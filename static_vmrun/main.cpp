// =============================================================================
// skarnvm -- the Skarn driver: read a script file, type-check + compile it, run it.
// (Built from the project static_vmrun; the program is skarnvm on every platform.)
//
//   skarnvm [--no-prelude | --prelude <path>] [--strict] [--no-inline] <script-file>
//
// Reads the whole file, strips a UTF-8 BOM, compiles it with the static prelude (so
// Option/Result + the typed combinators + the container builtins are available) and
// executes the resulting Module -- print/println write to stdout via execute()'s sink.
//
// The driver for the STATIC front end (`svc::compile`, which type-checks then ERASES
// -- a rejected program produces a CheckFailure and no bytecode). It links
// `static_compiler` + `vmcore` and includes only the public headers (Compiler.h /
// Execute.h) -- the same seam every consumer uses.
//
// Prelude resolution:
//   --no-prelude          compile with NO prelude.
//   --prelude <path>      read a monolithic prelude from <path>; a missing file is a driver
//                         error. An explicit whole-replacement -- one bare "$prelude" unit.
//   (neither flag)        use the SEALED built-in std, embedded into the compiler
//                         (svc::builtin_prelude()) -- the module-split ring + opt-in modules.
//                         std is NOT read from disk at run time (no user override/extension).
// The prelude is passed to the compiler as SOURCE TEXT, never as a path -- the compiler
// stays filesystem-free. It is parsed as separate module units and their items prepended.
//
// Exit codes (test-suite convention: errors -> stderr, non-zero exit):
//   0  success
//   1  script error   (type/compile error OR runtime trap)
//   2  driver error   (bad usage / file not readable)
// =============================================================================

#include <algorithm> // sort -- the --dump-names listing
#include <cstdlib>   // atoi -- the inliner tuning flags
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <exception>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "Compiler.h"   // svc::compile / svc::builtin_prelude / svc::CheckFailure / svc::Module / module_to_image
#include "BytecodeIO.h"  // bcio::ModuleImage / serialize / deserialize / BytecodeError
#include "Loader.h"      // svc::load_modules / ModuleSet / LoadError -- multi-file loading
#include "Lexer.h"       // svc::LexError    -- caught to render a source caret
#include "Parser.h"      // svc::ParseError
#include "Codegen.h"     // svc::CodegenError -- located ones carry a caret too
#include "Execute.h"     // execute() + Heap / StringInterner / VmFault
#include "Natives.h"     // build_native_table() -- the file/time/env/stdin native registry

namespace {

// The program's standard output, pushed to the OS line by line. The C runtime buffers stdout in blocks
// when it is a pipe or a file (MSVC treats _IOLBF as full buffering, so setvbuf cannot ask for lines),
// and a process that is killed -- the usual end of a server -- would lose everything still in that
// block. So a completed line goes out at once when nothing was flushed during the last GAP, which is
// every line a server, a progress report or a log writes. Inside a burst the lines are gathered and a
// flusher thread pushes them at most GAP after the first, because a flush per line made a program that
// prints a million lines 4 to 11 times slower; this way it pays 10-17 %. A line not yet ended waits for its newline or for std::io's flushOutput(). The VM's
// actors hand their output to this stream a whole line at a time, so their lines go out the same way.
class LineFlushBuf : public std::streambuf {
public:
    explicit LineFlushBuf(std::streambuf* target) : target_(target), flusher_([this] { run(); }) {}
    ~LineFlushBuf() override {
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_one();
        flusher_.join();
        std::lock_guard<std::mutex> lk(m_);
        target_->pubsync();
    }
protected:
    int_type overflow(int_type c) override {
        if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
        std::lock_guard<std::mutex> lk(m_);
        if (traits_type::eq_int_type(target_->sputc(traits_type::to_char_type(c)), traits_type::eof()))
            return traits_type::eof();
        if (traits_type::to_char_type(c) == '\n') line_ended();
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::lock_guard<std::mutex> lk(m_);
        const std::streamsize done = target_->sputn(s, n);
        if (done > 0 && std::char_traits<char>::find(s, static_cast<size_t>(done), '\n') != nullptr)
            line_ended();
        return done;
    }
    int sync() override {
        std::lock_guard<std::mutex> lk(m_);
        dirty_ = false;
        last_  = Clock::now();
        return target_->pubsync();
    }
private:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds GAP{ 10 };
    // m_ held. A line after a quiet GAP goes out at once; inside a burst the flusher pushes it at most
    // GAP later, so bulk output pays one flush per GAP instead of one per line.
    void line_ended() {
        const auto now = Clock::now();
        if (now - last_ >= GAP) { target_->pubsync(); last_ = now; dirty_ = false; }
        else if (!dirty_)       { dirty_ = true; cv_.notify_one(); }
    }
    void run() {
        std::unique_lock<std::mutex> lk(m_);
        for (;;) {
            cv_.wait(lk, [this] { return stop_ || dirty_; });
            if (stop_) return;
            cv_.wait_until(lk, last_ + GAP, [this] { return stop_; });
            if (dirty_) { target_->pubsync(); last_ = Clock::now(); dirty_ = false; }
        }
    }
    std::streambuf*         target_;
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    dirty_ = false;
    bool                    stop_  = false;
    Clock::time_point       last_{};
    std::thread             flusher_;   // last: starts after everything it reads is built
};

// Reads the entire file at `path` into `out`. Returns false if it cannot be opened.
// Binary mode so no newline translation happens (the lexer treats '\r' as whitespace).
bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Strips a leading UTF-8 BOM (EF BB BF) in place, if present.
void strip_bom(std::string& s) {
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
}

// Directory of `path` WITH a trailing separator (empty = the current directory). Used as the
// module search root: an `import net::http` resolves to `<dir>/net/http.skn`.
std::string dir_of(const std::string& path) {
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// --dump-names: every public name of the embedded std, one line each, sorted, tab-separated:
//   <module> <kind> <name> [<signature>]
// The kinds: `fn` (a `pub fn`, or an associated fn of a public type as `Type::name`), `method` (a
// method of a public type as `Type.name`, or a method a public trait declares as `Trait.name`),
// `type`, `trait`, `const`, `variant` (`Enum::Variant`), and `ambient` -- a builtin or native, whose
// module is the one whose `use` makes it reachable ("-" = always reachable) and which carries its
// signature (a builtin typed per call has one per accepted shape, joined by "; "). Methods of trait
// impls are not listed: the trait's own declaration names them. This is the checker's own view (the
// one editor completion uses), so it covers the natives, which no .skn file declares.
int dump_std_names() {
    const svc::ModuleResolver no_imports =
        [](const std::vector<std::string>&) -> std::optional<std::string> { return std::nullopt; };
    svc::ToolCheck tc = svc::check_modules_for_tools(svc::load_modules("", no_imports), &svc::builtin_prelude());
    if (!tc.errors.empty()) {
        std::cerr << "error: the embedded std does not type-check: " << tc.errors.front().message << "\n";
        return 1;
    }
    std::vector<std::string> lines;
    auto emit = [&](const std::string& module, const char* kind, const std::string& name,
                    const std::string& signature = std::string()) {
        lines.push_back(module + "\t" + kind + "\t" + name + (signature.empty() ? "" : "\t" + signature));
    };
    auto in_std = [](const svc::Item& it) { return it.is_pub && it.module_prefix.rfind("std::", 0) == 0; };

    std::set<std::pair<std::string, std::string>> pub_types;   // (module, type name): whose impls count
    for (const svc::ItemPtr& ip : tc.program.items) {
        if (!in_std(*ip)) continue;
        const std::string& mod = ip->module_prefix;
        switch (ip->kind) {
        case svc::ItemKind::Fn:
            emit(mod, "fn", svc::short_name(static_cast<const svc::FnItem&>(*ip).name));
            break;
        case svc::ItemKind::Const:
            emit(mod, "const", svc::short_name(static_cast<const svc::ConstItem&>(*ip).name));
            break;
        case svc::ItemKind::Struct: {
            const std::string name = svc::short_name(static_cast<const svc::StructItem&>(*ip).name);
            emit(mod, "type", name);
            pub_types.insert({ mod, name });
            break;
        }
        case svc::ItemKind::Enum: {
            const auto& e = static_cast<const svc::EnumItem&>(*ip);
            const std::string name = svc::short_name(e.name);
            emit(mod, "type", name);
            pub_types.insert({ mod, name });
            for (const svc::EnumVariant& v : e.variants) emit(mod, "variant", name + "::" + svc::short_name(v.name));
            break;
        }
        case svc::ItemKind::Trait: {
            const auto& t = static_cast<const svc::TraitDecl&>(*ip);
            const std::string name = svc::short_name(t.name);
            emit(mod, "trait", name);
            for (const svc::Method& m : t.methods) emit(mod, "method", name + "." + m.name);
            break;
        }
        default:
            break;
        }
    }
    // Inherent impls carry no `pub` of their own: their methods are public with their type.
    for (const svc::ItemPtr& ip : tc.program.items) {
        if (ip->kind != svc::ItemKind::Impl) continue;
        const auto& impl = static_cast<const svc::ImplDecl&>(*ip);
        if (!impl.is_inherent || !impl.target || impl.target->kind != svc::TypeKind::Named) continue;
        const std::string type = svc::short_name(static_cast<const svc::NamedType&>(*impl.target).name);
        if (!pub_types.count({ impl.module_prefix, type })) continue;
        for (const svc::Method& m : impl.methods)
            emit(impl.module_prefix, m.has_self ? "method" : "fn", type + (m.has_self ? "." : "::") + m.name);
    }
    for (const svc::AmbientFn& f : tc.ambient) {
        std::string signature = f.signature;
        for (size_t at = signature.find('\n'); at != std::string::npos; at = signature.find('\n', at))
            signature.replace(at, 1, "; ");
        emit(f.module.empty() ? "-" : f.module, "ambient", f.name, signature);
    }
    std::sort(lines.begin(), lines.end());
    for (const std::string& l : lines) std::cout << l << "\n";
    return 0;
}

void print_usage() {
    std::cerr << "usage: skarnvm [--no-prelude | --prelude <path>] [--strict] [--no-inline]\n"
                 "               [--emit-bytecode <file>] [--strip-debug] <script-file> [args...]\n"
                 "       skarnvm --run-bytecode <file> [args...]\n"
                 "       skarnvm --dump-ast [--no-prelude | --prelude <path>] <script-file>\n"
                 "       skarnvm --dump-names\n"
                 "\n"
                 "  --dump-ast   type-check only, print the type-annotated AST of the program\n"
                 "               (prelude items omitted) to stdout and exit; nothing is run\n"
                 "  --dump-names print every public name of the built-in standard library, one\n"
                 "               per line: module, kind, name and, for a builtin or native, its\n"
                 "               signature, separated by tabs; nothing else may be given\n"
                 "  --no-inline  do NOT inline small non-recursive direct calls. Inlining is a\n"
                 "               codegen-only transform (same values, fewer dispatches) and is ON\n"
                 "               by default; it costs code size and flattens a fault trace through\n"
                 "               an expanded callee. A compile option, so it does not combine with\n"
                 "               --run-bytecode\n"
                 "  --sroa       dissolve a non-escaping struct binding into one register per\n"
                 "               FIELD, so the object is never built. Codegen-only (same values),\n"
                 "               OFF by default, and dependent on inlining. --sroa-max-fields N\n"
                 "               caps the field count it will dissolve; 0 dissolves nothing\n";
}

// Print the offending source line and a caret under the fault column. Only renders when
// `line` is within `source` (guards against a prelude-origin position). Shared by the
// runtime-fault (VmFault), compile-error and type-error paths -- all carry a 1-based (line, col).
void render_source_context(std::ostream& err, const std::string& source, uint32_t line, uint32_t col) {
    if (line == 0) return;
    size_t   start = 0;
    uint32_t cur   = 1;
    while (cur < line) {
        size_t nl = source.find('\n', start);
        if (nl == std::string::npos) return;    // line is past EOF -> not our source, skip
        start = nl + 1;
        ++cur;
    }
    if (start > source.size()) return;
    size_t nl = source.find('\n', start);
    std::string text = source.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    if (!text.empty() && text.back() == '\r') text.pop_back();    // strip CRLF's '\r'

    const std::string num = std::to_string(line);
    err << "  " << num << " | " << text << "\n";
    if (col > 0) {
        std::string indent;                      // keep tabs so the caret lines up
        for (uint32_t k = 0; k + 1 < col && k < text.size(); ++k)
            indent += (text[k] == '\t') ? '\t' : ' ';
        err << "  " << std::string(num.size(), ' ') << " | " << indent << "^\n";
    }
}

// Writes `bytes` to `path` in binary mode. Returns false if it cannot be opened/written.
bool write_file_binary(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    if (!bytes.empty())
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// Runs a ready-to-execute bytecode image on a fresh heap, wiring the native registry, the
// program's args(), and stdin -- exactly as the compile path does. `src_for_caret` renders a
// VmFault caret (empty when running a serialized image with no source on hand). Shared by the
// normal run and the --run-bytecode path. Returns 0 on success, 1 on a runtime trap, and the
// program's own code when it called std::process's exit (ProgramExit: no message, no caret -- the
// world's end has already run while the exception left execute()).
// `module_sources` maps a module prefix ("" = entry, "std::core" / "net::http" = a std/imported
// module) to its source text, so a VmFault caret renders against the RIGHT file (the frame carries
// its owning module in `frame.module`). `fallback_src` is used when the frame's module is not in the
// map (e.g. a --run-bytecode image with no sources on hand). An empty map + empty fallback => no caret.
int run_image(const bcio::ModuleImage& img,
              const std::map<std::string, std::string>& module_sources,
              const std::string& fallback_src,
              const std::vector<std::string>& script_args) {
    try {
        Heap heap;
        StringInterner interner;
        const std::vector<NativeFunc> natives = build_native_table();
        // Skarn has no atoms, but execute() gates the true/false/nil TO_STRING pool on a
        // non-null atom_names table (vmcore is unchanged). Pass an EMPTY one so that pool builds.
        const std::vector<std::string> no_atoms;
        LineFlushBuf line_buf(std::cout.rdbuf());
        std::ostream out(&line_buf);
        execute(img.bytecode, &heap, nullptr, &interner, img.top_frame_size,
                &img.constants, &img.struct_types, &img.string_literals,
                &no_atoms, &img.function_table, &out,
                &img.trait_table, img.trait_table_width, img.trait_method_count,
                &img.line_table, &img.function_names, &img.column_table,
                &natives,        // the file/time/env/stdin native registry
                &script_args,    // positional args after the script -> args()
                &std::cin,       // stdin sink for readLine / readAllStdin
                &img.function_modules,   // per-function module prefix -> per-module fault caret
                &img.const_arrays);      // const-array literals -> the LOAD_CONST_ARRAY pool
        std::cout.flush();
    } catch (const ProgramExit& e) {
        std::cout.flush();
        return e.code;
    } catch (const std::exception& e) {
        std::cout.flush();
        std::cerr << "error: " << e.what() << "\n";
        if (const auto* vf = dynamic_cast<const VmFault*>(&e))
            if (!vf->frames.empty()) {
                const auto it = module_sources.find(vf->frames[0].module);
                const std::string& src = it != module_sources.end() ? it->second : fallback_src;
                render_source_context(std::cerr, src, vf->frames[0].line, vf->frames[0].col);
            }
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // ---- parse arguments -----------------------------------------------------
    std::string path;
    std::string prelude_path;
    bool no_prelude  = false;   // --no-prelude
    bool has_prelude = false;   // --prelude <path> given
    bool strict      = false;   // --strict: treat advisory warnings as fatal
    std::string emit_path;      // --emit-bytecode <file>: compile, serialize, write, exit
    bool has_emit    = false;
    std::string run_bc_path;    // --run-bytecode <file>: deserialize + run, skip compilation
    bool has_run_bc  = false;
    bool strip_debug = false;   // --strip-debug: omit the DBGL chunk when emitting
    bool dump_ast    = false;   // --dump-ast: check only, print the typed AST, exit
    bool dump_names  = false;   // --dump-names: list the public names of the embedded std, exit
    // Seeded from the COMPILER's default rather than repeating it here: two copies of one default
    // is exactly the kind of thing that silently drifts (this one did -- the driver's unconditional
    // override kept inlining off after the library default was flipped on).
    bool inline_on   = svc::inline_calls();   // --inline / --no-inline
    bool inline_report = false; // --inline-report: print the measure-vs-emit site tally to stderr
    int  inline_max_nodes = 20; // --inline-max-nodes N: callee body-size ceiling (S4 tuning)
    int  inline_depth     = 2;  // --inline-depth N: how deep an expansion may nest (S4 tuning)
    bool inline_given = false;  // whether any of them was passed (for the --run-bytecode check)
    // Scalar replacement of aggregates -- seeded from the compiler's default for the same reason
    // as inlining above (one source of truth for the default, not two that can drift apart).
    bool sroa_on     = svc::sroa();          // --sroa / --no-sroa
    bool sroa_report = false;                // --sroa-report: dissolution tally to stderr
    int  sroa_max_fields = svc::sroa_max_fields();   // --sroa-max-fields N (0 = the null control)
    bool sroa_given  = false;   // for the --run-bytecode check
    std::vector<std::string> script_args;   // positional args AFTER the script path -> args()
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (!path.empty()) {
            // Everything after the script path is the SCRIPT's argument, verbatim -- including
            // `-`/`--`-prefixed ones (so `args()` sees a real command line: flags, options, `--`).
            // Driver options must precede the script path (the usual convention).
            script_args.push_back(arg);
            continue;
        }
        if (arg == "--no-prelude") {
            no_prelude = true;
        } else if (arg == "--strict") {
            strict = true;
        } else if (arg == "--prelude") {
            if (i + 1 >= argc) {
                std::cerr << "error: --prelude requires a path argument\n";
                print_usage();
                return 2;
            }
            prelude_path = argv[++i];
            has_prelude  = true;
        } else if (arg == "--emit-bytecode") {
            if (i + 1 >= argc) {
                std::cerr << "error: --emit-bytecode requires a path argument\n";
                print_usage();
                return 2;
            }
            emit_path = argv[++i];
            has_emit  = true;
        } else if (arg == "--run-bytecode") {
            if (i + 1 >= argc) {
                std::cerr << "error: --run-bytecode requires a path argument\n";
                print_usage();
                return 2;
            }
            run_bc_path = argv[++i];
            has_run_bc  = true;
        } else if (arg == "--strip-debug") {
            strip_debug = true;
        } else if (arg == "--dump-ast") {
            dump_ast = true;
        } else if (arg == "--dump-names") {
            dump_names = true;
        } else if (arg == "--inline") {
            inline_on = true;  inline_given = true;
        } else if (arg == "--no-inline") {
            inline_on = false; inline_given = true;
        } else if (arg == "--inline-report") {
            inline_report = true; inline_given = true;
        } else if (arg == "--inline-max-nodes" || arg == "--inline-depth") {
            if (i + 1 >= argc) {
                std::cerr << "error: " << arg << " requires a number\n";
                print_usage();
                return 2;
            }
            const int v = std::atoi(argv[++i]);
            if (v < 1) {
                std::cerr << "error: " << arg << " must be >= 1\n";
                return 2;
            }
            if (arg == "--inline-max-nodes") inline_max_nodes = v; else inline_depth = v;
            inline_given = true;
        } else if (arg == "--sroa") {
            sroa_on = true;  sroa_given = true;
        } else if (arg == "--no-sroa") {
            sroa_on = false; sroa_given = true;
        } else if (arg == "--sroa-report") {
            sroa_report = true; sroa_given = true;
        } else if (arg == "--sroa-max-fields") {
            // Unlike the inline knobs this accepts 0 -- that IS the null control (SROA on, nothing
            // small enough to dissolve, byte-identical bytecode), so the bound is >= 0, not >= 1.
            if (i + 1 >= argc) {
                std::cerr << "error: " << arg << " requires a number\n";
                print_usage();
                return 2;
            }
            const int v = std::atoi(argv[++i]);
            if (v < 0) {
                std::cerr << "error: " << arg << " must be >= 0\n";
                return 2;
            }
            sroa_max_fields = v;
            sroa_given = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option '" << arg << "'\n";
            print_usage();
            return 2;
        } else if (path.empty()) {
            path = arg;
        } else {
            std::cerr << "error: unexpected extra argument '" << arg << "'\n";
            print_usage();
            return 2;
        }
    }
    // ---- --dump-names: a listing of the embedded std, not of a program ----------
    // Alone or not at all: any other option or a script path would be silently ignored.
    if (dump_names) {
        if (argc != 2) {
            std::cerr << "error: --dump-names takes no other option and no script\n";
            print_usage();
            return 2;
        }
        return dump_std_names();
    }

    // ---- --run-bytecode: skip compilation, deserialize + run a serialized image ----
    if (has_run_bc) {
        if (has_emit || no_prelude || has_prelude || strict || dump_ast || inline_given || sroa_given) {
            std::cerr << "error: --run-bytecode cannot be combined with compile options\n";
            print_usage();
            return 2;
        }
        // There is no <script-file> with --run-bytecode (the image comes from the flag);
        // a bare positional is the program's first arg().
        if (!path.empty()) { script_args.insert(script_args.begin(), path); path.clear(); }
        std::string blob;
        if (!read_file(run_bc_path, blob)) {
            std::cerr << "error: cannot open '" << run_bc_path << "'\n";
            return 2;
        }
        try {
            bcio::ModuleImage img = bcio::deserialize(
                reinterpret_cast<const uint8_t*>(blob.data()), blob.size());
            const std::map<std::string, std::string> no_srcs;   // no sources with a serialized image
            return run_image(img, no_srcs, /*fallback*/ std::string(), script_args);
        } catch (const bcio::BytecodeError& e) {
            std::cerr << "error: " << e.what() << "\n";
            return 1;
        }
    }

    if (path.empty()) {
        print_usage();
        return 2;
    }
    if (no_prelude && has_prelude) {
        std::cerr << "error: --no-prelude and --prelude are mutually exclusive\n";
        print_usage();
        return 2;
    }
    // Both are terminal actions that produce a different artifact -- combining them would
    // silently honour only one, so say so instead.
    if (dump_ast && has_emit) {
        std::cerr << "error: --dump-ast and --emit-bytecode are mutually exclusive\n";
        print_usage();
        return 2;
    }

    // A codegen-only switch: it must be set before any svc::compile* call, and it changes the
    // emitted bytecode, never the accepted program.
    svc::set_inline_calls(inline_on);
    svc::set_inline_report(inline_report);
    svc::set_inline_max_nodes(inline_max_nodes);
    svc::set_inline_max_depth(inline_depth);
    svc::set_sroa(sroa_on);
    svc::set_sroa_report(sroa_report);
    svc::set_sroa_max_fields(sroa_max_fields);

    // ---- read the source -----------------------------------------------------
    std::string source;
    if (!read_file(path, source)) {
        std::cerr << "error: cannot open '" << path << "'\n";
        return 2;
    }
    strip_bom(source);

    // ---- resolve the prelude -------------------------------------------------
    // The prelude is a LIST of { prefix, source } std modules (the stdlib split). Default: the
    // EMBEDDED set (already partitioned into std::core / std::iter / std::string / $prelude), so
    // the ring is always present. --prelude <path> overrides with one monolithic "$prelude" unit
    // (a user-supplied single-module prelude; no ring split). --no-prelude uses no prelude.
    std::vector<svc::PreludeModule> prelude_mods;
    if (!no_prelude) {
        if (has_prelude) {
            std::string prelude_text;
            if (!read_file(prelude_path, prelude_text)) {
                std::cerr << "error: cannot open prelude '" << prelude_path << "'\n";
                return 2;
            }
            strip_bom(prelude_text);
            prelude_mods.push_back({ "$prelude", std::move(prelude_text) });
        } else {
            prelude_mods = svc::builtin_prelude();   // the embedded, module-split set
        }
    }

    // ---- compile (lex -> parse -> CHECK -> lower) ----------------------------
    auto report_compile_error = [&](const char* what, uint32_t line, uint32_t col) {
        std::cerr << "error: " << what << "\n";
        render_source_context(std::cerr, source, line, col);
    };
    // Multi-file loading: the entry source + every module it transitively `import`/`use`s, resolved
    // as `<entry-dir>/seg0/seg1/....skn`. A single-file script (no imports) yields a root-only
    // module set, so compile_modules is byte-identical to compile() there.
    //
    // Per-module source map, so a COMPILE error's caret renders against the RIGHT file: the entry
    // ("$entry"), the prelude ("$prelude"), and every module the resolver reads. render_for() picks
    // the source for a module key (falling back to the entry source). (A RUNTIME fault / a
    // CodegenError carries no module id, so its caret still renders against the entry source -- a
    // documented gap.) The empty key is kept as a fallback for an image whose entry frames predate
    // the entry prefix.
    std::map<std::string, std::string> module_sources;
    module_sources[svc::ENTRY_MODULE_PREFIX] = source;
    module_sources[""] = source;
    for (const auto& pm : prelude_mods) module_sources[pm.prefix] = pm.source;
    auto render_for = [&](const std::string& mod, uint32_t line, uint32_t col) {
        auto it = module_sources.find(mod);
        render_source_context(std::cerr, it != module_sources.end() ? it->second : source, line, col);
    };

    const std::string script_dir = dir_of(path);
    svc::ModuleResolver resolver = [&](const std::vector<std::string>& segs) -> std::optional<std::string> {
        std::string rel, key;
        for (size_t i = 0; i < segs.size(); ++i) { if (i) { rel += "/"; key += "::"; } rel += segs[i]; key += segs[i]; }
        std::string full = script_dir + rel + ".skn";
        std::string src;
        if (!read_file(full, src)) return std::nullopt;
        strip_bom(src);
        module_sources[key] = src;   // record for caret rendering (module path key -> source)
        return src;
    };
    // Advisory warnings are reported identically whether we compile or only check, so the
    // reporting lives in one lambda both paths call (--strict escalates in both).
    auto report_warnings = [&](const std::vector<svc::TypeError>& ws) {
        for (const auto& w : ws) {
            std::cerr << "warning: " << w.message << "\n";
            render_for(w.module, w.line, w.col);   // caret against the warning's own module source
            for (const auto& lb : w.labels) {
                std::cerr << "  note: " << lb.text << "\n";
                render_for(lb.module, lb.line, lb.col);
            }
        }
        if (strict && !ws.empty()) {
            std::cerr << "error: aborting due to " << ws.size() << " warning(s) (--strict)\n";
            return false;
        }
        return true;
    };

    svc::Module module;
    try {
        svc::ModuleSet modules = svc::load_modules(source.c_str(), resolver);
        // --dump-ast: print what the CHECKER concluded, then stop -- nothing is lowered or run.
        // It goes through parse_check_modules, i.e. compile_modules' own front half, so the dump
        // is byte-for-byte the program the bytecode path sees rather than a second parse that
        // could disagree with it. That is the whole value: when the differential sweep shrinks a
        // disagreement to a minimal program, this answers "checker or codegen?" without adding
        // prints and rebuilding. Prelude items are omitted (see dump_typed). stdout carries the
        // dump alone, so it pipes cleanly; warnings and errors stay on stderr.
        if (dump_ast) {
            std::vector<svc::TypeError> warns;
            const svc::Program prog =
                svc::parse_check_modules(std::move(modules), &prelude_mods, &warns);
            std::cout << svc::dump_typed(prog) << "\n";
            return report_warnings(warns) ? 0 : 1;
        }
        module = svc::compile_modules(std::move(modules), &prelude_mods);
    } catch (const svc::LoadError& e) {
        std::cerr << "error: " << e.what() << "\n";      // missing module / cycle / a module's parse error
        if (e.line) render_for(e.module, e.line, e.col); // a wrapped lex/parse error -> caret in its file
        return 1;
    } catch (const svc::CheckFailure& e) {
        // Every reported type error, each with a caret against ITS module's source (types are erased,
        // so the checker is the only guard -- a rejected program is never lowered).
        for (const auto& te : e.errors()) {
            std::cerr << "error: type error: " << te.message << "\n";
            render_for(te.module, te.line, te.col);
            for (const auto& lb : te.labels) {   // secondary "note" carets (dual expected/found)
                std::cerr << "  note: " << lb.text << "\n";
                render_for(lb.module, lb.line, lb.col);
            }
        }
        if (e.errors().empty()) std::cerr << "error: " << e.what() << "\n";
        return 1;
    } catch (const svc::LexError& e) {                   // a prelude lex/parse error (not via the loader)
        report_compile_error(e.what(), e.line(), e.col());
        return 1;
    } catch (const svc::ParseError& e) {
        report_compile_error(e.what(), e.line(), e.col());
        return 1;
    } catch (const svc::CodegenError& e) {
        report_compile_error(e.what(), e.line(), e.col());
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    // ---- report advisory warnings --------------------------------------------
    // The checker's must-use / unused-binding warnings (never affect a successful compile).
    // By default the program still runs; --strict makes a non-empty list fatal.
    if (!report_warnings(module.warnings)) return 1;

    // ---- emit or run ---------------------------------------------------------
    // The execution-ready image (the serializable subset of the compiled Module).
    const bcio::ModuleImage img = svc::module_to_image(module);

    // --emit-bytecode: serialize the image and write it out, then stop. --strip-debug
    // omits the line/column/function-name tables (a smaller image; VM-fault carets degrade).
    if (has_emit) {
        const std::vector<uint8_t> bytes = bcio::serialize(img, /*include_debug=*/!strip_debug);
        if (!write_file_binary(emit_path, bytes)) {
            std::cerr << "error: cannot write '" << emit_path << "'\n";
            return 2;
        }
        return 0;
    }

    // Normal run. Heap must outlive the interner; the static front end emits no globals.
    // print/println write to std::cout; the line/column/function-name/module tables feed VM-fault
    // reporting (a panic / index trap -> a source caret against the RIGHT module's source).
    return run_image(img, module_sources, /*fallback*/ source, script_args);
}
