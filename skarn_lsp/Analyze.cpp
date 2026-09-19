// =============================================================================
// Analyze.cpp -- see Analyze.h for the contract.
//
// Every way the front end can reject a program is mapped here, in one place:
//   syntax errors every lex/parse error of every module, recovered by the tool-mode loader
//                 (module key + position each)
//   LoadError     a missing module or an import cycle
//   LexError /    only from the prelude path, which the loader does not wrap -- the sealed
//   ParseError    std never produces one, but the mapping must be total
//   type errors   every one, each with its module and optional secondary notes
//   warnings      the advisory tier -- reported even while errors exist
// A module key names the file: "" or "$entry" is the entry program, "a::b" the file the
// resolver served for it. An error inside the sealed std (a "std::" key or "$prelude")
// has no editable file, so it is reported on the entry's first line with its origin
// spelled out -- it would mean a broken build, and it must not vanish silently.
// =============================================================================

#include "Analyze.h"

#include "Compiler.h"   // svc::check_modules_for_tools / svc::builtin_prelude
#include "Lexer.h"      // svc::LexError
#include "Loader.h"     // svc::load_modules / svc::LoadError / svc::ModuleResolver
#include "Naming.h"     // svc::ENTRY_MODULE_PREFIX
#include "Parser.h"     // svc::ParseError

#include <cctype>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>

namespace lsp {

namespace {

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

void strip_bom(std::string& s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF)
        s.erase(0, 3);
}

std::optional<std::string> read_disk(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    strip_bom(s);
    return s;
}

std::string dir_of(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

// "parse error in module 'util': expected X" -> "parse error: expected X". The module is
// already named by the file the diagnostic sits in.
std::string strip_module_prefix(const std::string& msg) {
    for (const char* kind : { "parse error in module '", "lex error in module '" }) {
        const std::string k = kind;
        if (!starts_with(msg, k)) continue;
        const std::size_t end = msg.find("': ", k.size());
        if (end == std::string::npos) break;
        const std::string head = k.substr(0, k.find(" in module"));   // "parse error" / "lex error"
        return head + ": " + msg.substr(end + 3);
    }
    return msg;
}

// The text between the first pair of single quotes after `marker`, or empty.
std::string quoted_after(const std::string& msg, const std::string& marker) {
    const std::size_t m = msg.find(marker);
    if (m == std::string::npos) return {};
    const std::size_t open = msg.find('\'', m + marker.size());
    if (open == std::string::npos) return {};
    const std::size_t close = msg.find('\'', open + 1);
    if (close == std::string::npos) return {};
    return msg.substr(open + 1, close - open - 1);
}

// Whether a checker diagnostic lies inside an item that lost part of itself to a syntax error:
// from the item's start up to the next item of the same module.
class BrokenItems {
public:
    explicit BrokenItems(const svc::Program& prog) {
        for (std::size_t i = prog.prelude_item_count; i < prog.items.size(); ++i) {
            const svc::Item& it = *prog.items[i];
            starts_[it.module_prefix].push_back(Start{ it.line, it.col, it.has_syntax_error });
        }
    }

    bool contains(const svc::TypeError& te) const {
        const auto it = starts_.find(te.module.empty() ? std::string(svc::ENTRY_MODULE_PREFIX) : te.module);
        if (it == starts_.end()) return false;
        bool broken = false;
        for (const Start& s : it->second) {   // source order: the last start at or before the error
            if (s.line > te.line || (s.line == te.line && s.col > te.col)) break;
            broken = s.broken;
        }
        return broken;
    }

private:
    struct Start { uint32_t line, col; bool broken; };
    std::map<std::string, std::vector<Start>> starts_;
};

class Analysis {
public:
    Analysis(const std::string& entry_path, const std::string& entry_text, const Overlay& overlay)
        : entry_path_(normalize_path(entry_path)), overlay_(overlay) {
        entry_dir_ = dir_of(entry_path_);
        path_text_[entry_path_] = entry_text;
        strip_bom(path_text_[entry_path_]);
    }

    AnalysisResult run() {
        try {
            const svc::ModuleResolver resolver =
                [this](const std::vector<std::string>& segs) { return resolve(segs); };
            svc::ToolLoad load = svc::load_modules_for_tools(path_text_[entry_path_].c_str(), resolver);
            for (const svc::SyntaxError& se : load.syntax_errors) {
                std::string path = path_of(se.module);
                if (path.empty()) path = entry_path_;
                add(path, se.line, se.col, Severity::Error, se.message);
                result_.syntax_error_paths.insert(path);
            }
            svc::ToolCheck tc = svc::check_modules_for_tools(std::move(load.modules), &svc::builtin_prelude());
            const BrokenItems broken(tc.program);
            for (const svc::TypeError& te : tc.errors)
                if (!broken.contains(te)) add_type_error(te, Severity::Error);
            // Warnings are advice about finished code: in a file with a syntax error, dropped text
            // turns used bindings into unused ones, so the file gets none until it parses.
            for (const svc::TypeError& w : tc.warnings)
                if (!broken.contains(w) && !result_.syntax_error_paths.count(path_of(w.module)))
                    add_type_error(w, Severity::Warning);
            result_.program = std::make_shared<const svc::Program>(std::move(tc.program));
            for (svc::AmbientFn& f : tc.ambient)
                result_.ambient.push_back(AmbientName{ std::move(f.name), std::move(f.module), std::move(f.signature) });
        } catch (const svc::LoadError& e) {
            add_load_error(e);
        } catch (const svc::LexError& e) {
            add(entry_path_, 1, 1, Severity::Error, std::string("lex error in the std: ") + e.what());
        } catch (const svc::ParseError& e) {
            add(entry_path_, 1, 1, Severity::Error, std::string("parse error in the std: ") + e.what());
        } catch (const std::exception& e) {
            add(entry_path_, 1, 1, Severity::Error, std::string("internal error: ") + e.what());
        } catch (...) {
            add(entry_path_, 1, 1, Severity::Error, "internal error: unknown exception");
        }
        result_.module_of[entry_path_] = svc::ENTRY_MODULE_PREFIX;
        result_.path_of[svc::ENTRY_MODULE_PREFIX] = entry_path_;
        for (const auto& [key, path] : key_path_) {
            result_.module_of[path] = key;
            result_.path_of[key]    = path;
        }
        result_.texts = path_text_;
        return std::move(result_);
    }

private:
    std::optional<std::string> resolve(const std::vector<std::string>& segs) {
        std::string rel, key;
        for (std::size_t i = 0; i < segs.size(); ++i) {
            if (i) { rel += '/'; key += "::"; }
            rel += segs[i];
            key += segs[i];
        }
        const std::string path = normalize_path(entry_dir_ + rel + ".skn");
        std::optional<std::string> text;
        if (const auto it = overlay_.find(path); it != overlay_.end()) text = it->second;
        else text = read_disk(path);
        if (!text) return std::nullopt;
        strip_bom(*text);
        key_path_[key]    = path;
        path_text_[path]  = *text;
        result_.modules.insert(path);
        return text;
    }

    // The file a module key belongs to; empty for the sealed std / prelude.
    std::string path_of(const std::string& key) const {
        if (key.empty() || key == svc::ENTRY_MODULE_PREFIX) return entry_path_;
        if (const auto it = key_path_.find(key); it != key_path_.end()) return it->second;
        return {};
    }

    Range range_at(const std::string& path, uint32_t line, uint32_t col) const {
        static const std::string none;
        const auto it = path_text_.find(path);
        return ident_range(it != path_text_.end() ? it->second : none, line, col);
    }

    void add(const std::string& path, uint32_t line, uint32_t col, Severity sev, std::string msg,
             std::vector<RelatedInfo> related = {}) {
        Diagnostic d;
        d.range    = range_at(path, line, col);
        d.severity = sev;
        d.message  = std::move(msg);
        d.related  = std::move(related);
        result_.by_path[path].push_back(std::move(d));
    }

    void add_type_error(const svc::TypeError& te, Severity sev) {
        std::vector<RelatedInfo> related;
        for (const svc::DiagLabel& lb : te.labels) {
            const std::string lp = path_of(lb.module);
            if (lp.empty()) continue;   // a note inside the embedded std has no file to open
            related.push_back(RelatedInfo{ lp, range_at(lp, lb.line, lb.col), lb.text });
        }
        const std::string path = path_of(te.module);
        if (path.empty()) {
            std::ostringstream msg;
            msg << "in " << te.module << " " << te.line << ":" << te.col << ": " << te.message;
            add(entry_path_, 1, 1, sev, msg.str(), std::move(related));
            return;
        }
        add(path, te.line, te.col, sev, te.message, std::move(related));
    }

    void add_load_error(const svc::LoadError& e) {
        const std::string msg = e.what();
        if (e.line != 0) {   // a lex/parse error inside a module, positioned by the loader
            std::string path = path_of(e.module);
            if (path.empty()) path = entry_path_;
            add(path, e.line, e.col, Severity::Error, strip_module_prefix(msg));
            return;
        }
        // A missing module or an import cycle carries no position: point at the import line
        // in the importing file, found by searching for the module path in its source.
        std::string target = quoted_after(msg, "cannot resolve module");
        std::string importer = quoted_after(msg, "imported by");
        if (target.empty()) target = quoted_after(msg, "import cycle involving module");
        std::string path = path_of(importer);
        if (path.empty()) path = entry_path_;
        uint32_t line = 1, col = 1;
        if (!target.empty()) locate_import(path, target, line, col);
        add(path, line, col, Severity::Error, msg);
    }

    void locate_import(const std::string& path, const std::string& target, uint32_t& line, uint32_t& col) const {
        const auto it = path_text_.find(path);
        if (it == path_text_.end()) return;
        std::istringstream in(it->second);
        std::string text;
        for (uint32_t n = 1; std::getline(in, text); ++n) {
            std::size_t first = text.find_first_not_of(" \t");
            if (first == std::string::npos) continue;
            const std::string body = text.substr(first);
            if (!starts_with(body, "import ") && !starts_with(body, "use ")) continue;
            const std::size_t at = text.find(target, first);
            if (at == std::string::npos) continue;
            line = n;
            col  = static_cast<uint32_t>(at) + 1;
            return;
        }
    }

    std::string entry_path_;
    std::string entry_dir_;
    const Overlay& overlay_;
    std::map<std::string, std::string> key_path_;    // module key -> normalized path
    std::map<std::string, std::string> path_text_;   // normalized path -> source text
    AnalysisResult result_;
};

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percent_decode(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex_value(s[i + 1]), lo = hex_value(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

std::string percent_encode(const std::string& s) {
    static const char* HEX = "0123456789ABCDEF";
    std::string out;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/') {
            out += ch;
        } else {
            out += '%';
            out += HEX[c >> 4];
            out += HEX[c & 0xF];
        }
    }
    return out;
}

bool has_drive_letter(const std::string& p) {
    return p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':';
}

} // namespace

std::string line_text(const std::string& src, uint32_t line) {
    std::size_t start = 0;
    for (uint32_t cur = 1; cur < line; ++cur) {
        const std::size_t nl = src.find('\n', start);
        if (nl == std::string::npos) return {};
        start = nl + 1;
    }
    if (start > src.size()) return {};
    const std::size_t nl = src.find('\n', start);
    std::string text = src.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    if (!text.empty() && text.back() == '\r') text.pop_back();
    return text;
}

Range ident_range(const std::string& src, uint32_t line, uint32_t col) {
    if (line == 0) { line = 1; col = 1; }
    if (col == 0) col = 1;
    const std::string text = line_text(src, line);
    const int len = static_cast<int>(text.size());
    int c0 = static_cast<int>(col) - 1;
    if (c0 > len) c0 = len;
    int c1 = c0;
    if (c0 < len && is_ident_char(text[static_cast<std::size_t>(c0)])) {
        while (c1 < len && is_ident_char(text[static_cast<std::size_t>(c1)])) ++c1;
    } else if (c0 < len) {
        c1 = c0 + 1;
    }
    const int l0 = static_cast<int>(line) - 1;
    return Range{ Position{ l0, c0 }, Position{ l0, c1 } };
}

bool operator==(const Diagnostic& a, const Diagnostic& b) {
    auto same_range = [](const Range& x, const Range& y) {
        return x.start.line == y.start.line && x.start.character == y.start.character &&
               x.end.line == y.end.line && x.end.character == y.end.character;
    };
    if (!same_range(a.range, b.range) || a.severity != b.severity || a.message != b.message ||
        a.related.size() != b.related.size())
        return false;
    for (std::size_t i = 0; i < a.related.size(); ++i)
        if (a.related[i].path != b.related[i].path || a.related[i].message != b.related[i].message ||
            !same_range(a.related[i].range, b.related[i].range))
            return false;
    return true;
}

AnalysisResult analyze(const std::string& entry_path, const std::string& entry_text, const Overlay& overlay) {
    return Analysis(entry_path, entry_text, overlay).run();
}

std::string normalize_path(std::string p) {
    for (char& c : p)
        if (c == '\\') c = '/';
    if (has_drive_letter(p)) p[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(p[0])));
    return p;
}

std::string uri_to_path(const std::string& uri) {
    const std::string scheme = "file://";
    if (uri.size() < scheme.size()) return {};
    for (std::size_t i = 0; i < scheme.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(uri[i])) != scheme[i]) return {};
    std::string rest = uri.substr(scheme.size());
    const std::size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? std::string() : rest.substr(slash);
    path = percent_decode(path);
    if (!authority.empty() && authority != "localhost")
        return normalize_path("//" + percent_decode(authority) + path);   // UNC share
    if (path.size() >= 3 && path[0] == '/' && has_drive_letter(path.substr(1))) path.erase(0, 1);
    return normalize_path(path);
}

std::string path_to_uri(const std::string& path) {
    const std::string p = normalize_path(path);
    if (has_drive_letter(p)) return "file:///" + std::string(1, p[0]) + "%3A" + percent_encode(p.substr(2));
    if (p.size() >= 2 && p[0] == '/' && p[1] == '/') {                    // UNC share
        const std::size_t slash = p.find('/', 2);
        const std::string host = slash == std::string::npos ? p.substr(2) : p.substr(2, slash - 2);
        const std::string rest = slash == std::string::npos ? std::string() : p.substr(slash);
        return "file://" + host + percent_encode(rest);
    }
    return "file://" + percent_encode(p);
}

} // namespace lsp
