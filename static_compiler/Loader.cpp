// =============================================================================
// Loader.cpp -- multi-module loading (Slice 1b). See Loader.h for the contract.
//
// A depth-first walk of the import DAG. Each module is parsed exactly once (cached by
// its "::"-joined path key). A three-state mark per key drives both cycle detection and
// the topological order: a key on the current DFS stack ("visiting") re-encountered is a
// cycle; a key appended to the output after its dependencies ("done") gives post-order =
// topological order (the entry/root module, visited first, is emitted last).
// =============================================================================

#include "Loader.h"

#include "Lexer.h"
#include "Parser.h"

#include <unordered_set>

namespace svc {

std::string join_module_path(const std::vector<std::string>& path) {
    std::string s;
    for (size_t i = 0; i < path.size(); ++i) { if (i) s += "::"; s += path[i]; }
    return s;
}

namespace {

Program parse_module_source(const std::string& src) {
    Lexer lex(src);
    Parser parser(lex.tokenize());
    return parser.parse_program();     // propagates LexError / ParseError
}

// The module paths this program depends on -- one per `import`/`use` declaration (a
// `use a::b::{..}` depends on module `a::b`, exactly like `import a::b`).
std::vector<std::vector<std::string>> module_deps(const Program& prog) {
    std::vector<std::vector<std::string>> deps;
    auto add = [&](const std::vector<std::string>& path) {
        // `std::*` modules (std::core / std::iter / std::string / std::io / std::process / std::env
        // / std::bytes) are always EMBEDDED (prepended by the prelude), never loaded from the FS.
        // Skip them here so `use std::io::*` does not trigger a "cannot resolve module 'std::io'".
        // The checker still sees the `use` item (imported_prefixes_) -- that is what gates the native.
        if (!path.empty() && path[0] == "std") return;
        deps.push_back(path);
    };
    for (const auto& it : prog.items) {
        if (it->kind == ItemKind::Import)
            add(static_cast<const ImportItem&>(*it).path);
        else if (it->kind == ItemKind::Use) {
            // `use mod::Enum::(*|V|{..})` (an uppercase last segment = an enum-variant use, S3)
            // depends on module `mod`, NOT `mod::Enum` -- strip the trailing enum segment.
            std::vector<std::string> p = static_cast<const UseItem&>(*it).path;
            if (!p.empty() && !p.back().empty() && p.back()[0] >= 'A' && p.back()[0] <= 'Z')
                p.pop_back();
            add(p);
        }
    }
    return deps;
}

// Recursive DFS state. `out` accumulates modules in post-order (= topological).
struct Loader {
    const ModuleResolver&           resolve;
    std::vector<LoadedModule>       out;
    std::unordered_set<std::string> done;      // keys fully emitted into `out`
    std::unordered_set<std::string> visiting;  // keys on the current DFS stack (cycle guard)

    explicit Loader(const ModuleResolver& r) : resolve(r) {}

    // Parse `src` for module `path` (already resolved by the caller / entry), then recurse
    // into its dependencies before emitting it. `path` empty = the root module.
    void visit(const std::vector<std::string>& path, const std::string& src) {
        const std::string key = join_module_path(path);
        // Wrap a lex/parse error so the offending module + position travel with it (a driver renders
        // the caret against that module's source). The entry module has the empty key.
        const std::string where = key.empty() ? std::string("<entry>") : key;
        Program prog;
        try {
            prog = parse_module_source(src);
        } catch (const ParseError& e) {
            throw LoadError("parse error in module '" + where + "': " + e.what(), key, e.line(), e.col());
        } catch (const LexError& e) {
            throw LoadError("lex error in module '" + where + "': " + e.what(), key, e.line(), e.col());
        }
        visiting.insert(key);
        for (const auto& dep : module_deps(prog)) {
            const std::string dkey = join_module_path(dep);
            if (done.count(dkey)) continue;                 // already loaded (shared dependency)
            if (visiting.count(dkey))
                throw LoadError("import cycle involving module '" + dkey + "'");
            std::optional<std::string> dsrc = resolve(dep);
            if (!dsrc)
                throw LoadError("cannot resolve module '" + dkey + "'" +
                                (key.empty() ? "" : " imported by '" + key + "'"));
            visit(dep, *dsrc);
        }
        visiting.erase(key);
        done.insert(key);
        LoadedModule m;
        m.path = path;
        m.name = key;
        m.program = std::move(prog);
        out.push_back(std::move(m));
    }
};

} // namespace

ModuleSet load_modules(const char* entry_source, const ModuleResolver& resolve) {
    Loader ld(resolve);
    ld.visit({}, entry_source ? entry_source : "");   // root: empty path
    ModuleSet set;
    set.modules = std::move(ld.out);
    return set;
}

} // namespace svc
