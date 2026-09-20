// =============================================================================
// Complete.cpp -- see Complete.h.
// =============================================================================

#include "Complete.h"

#include "Query.h"     // probe / call_at
#include "Symbols.h"   // render_item / render_method / render_type / drop_module_paths

#include "Ast.h"       // svc::Program and the item types
#include "Lexer.h"     // svc::Lexer -- where the cursor is
#include "Naming.h"    // svc::short_name / svc::ENTRY_MODULE_PREFIX
#include "Types.h"     // svc::Ty

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace lsp {

namespace {

// Spelled so that no program uses it; it must lex as ONE identifier after any prefix. Where
// nothing is typed yet and a name (not a member) is completed, the probe is capitalized: a type
// position parses only an uppercase name, and an expression takes one just as well.
constexpr const char* PROBE       = "zzSkarnCompletionProbe";
constexpr const char* UPPER_PROBE = "ZzSkarnCompletionProbe";

constexpr const char* RING_MODULES[] = { "std::core", "std::iter", "std::string" };

constexpr const char* KEYWORDS[] = {
    "let", "mut", "fn", "if", "else", "match", "while", "for", "in", "loop", "break", "continue",
    "return", "true", "false", "struct", "enum", "trait", "impl", "dyn", "self", "import", "use",
    "pub", "const", "transparent",
};

constexpr const char* BUILTIN_TYPES[] = {
    "Int", "Double", "Bool", "String", "Vec", "Map", "Array", "Bytes", "List",
};

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The byte offset of `pos` in `text`, clamped to the end of its line.
std::size_t offset_of(const std::string& text, Position pos) {
    std::size_t off = 0;
    for (int l = 0; l < pos.line; ++l) {
        const std::size_t nl = text.find('\n', off);
        if (nl == std::string::npos) return text.size();
        off = nl + 1;
    }
    std::size_t end = text.find('\n', off);
    if (end == std::string::npos) end = text.size();
    if (end > off && text[end - 1] == '\r') --end;
    return std::min(off + static_cast<std::size_t>(std::max(pos.character, 0)), end);
}

enum class Context { None, Member, Name, Qualified };

// After `::`: the path before it, and whether it is a `use` item's path (which may have several
// segments; an expression or type path has exactly one, the checker's rule).
struct QualifiedPath {
    std::vector<std::string> segments;
    bool in_use = false;
};

bool is_name_token(svc::TokKind k) { return k == svc::TokKind::LIdent || k == svc::TokKind::UIdent; }

// The path `a::b` whose last `::` is `toks[colons]`, walking back over `name ::` pairs; `start`
// receives the index of its first segment.
std::vector<std::string> path_before(const std::vector<svc::Token>& toks, std::size_t colons, std::size_t& start) {
    std::vector<std::string> segs;
    std::size_t j = colons;
    while (j >= 1 && toks[j].kind == svc::TokKind::ColonColon && is_name_token(toks[j - 1].kind)) {
        segs.insert(segs.begin(), toks[j - 1].text);
        start = j - 1;
        if (j < 2) break;
        j -= 2;
    }
    return segs;
}

// Whether the tokens before index `start` begin a `use` item: `use` (after an optional `pub`).
bool starts_use(const std::vector<svc::Token>& toks, std::size_t start) {
    return start >= 1 && toks[start - 1].kind == svc::TokKind::KwUse;
}

// What finishes the cursor's line for the second attempt, from `toks` (which end just before
// the probe): a closer for every bracket, brace and `${` hole the line opened and left open,
// innermost first, with ` {}` inserted where an `if` / `while` / `match` / `for` head on the line
// still lacks its block. A line that starts a `let` without its `= value` gets ` = 0`, and one
// that starts a `fn` header without its body gets ` {}` (after ` -> ()` when it has no return
// type yet, which is mandatory) -- `let z: T` and `fn f(a: T` parse only then. A block closed here early leaves its real `}` further down stray, which the
// error-tolerant parser absorbs; only the probe's own node matters.
std::string line_completion(const std::vector<svc::Token>& toks, uint32_t line) {
    // What the cursor's line opened and left open; an interpolated string's `${` hole is closed
    // by `}"`. Opened on an earlier line counts only to pair the closers.
    std::vector<std::string> open;
    std::size_t earlier = 0;
    std::size_t head_at = SIZE_MAX;   // open.size() when the pending head's keyword appeared
    bool impl = false;
    svc::TokKind first = svc::TokKind::Eof;   // the line's first token (after `pub`)
    bool assign = false, brace = false, arrow = false;
    auto opens = [&](const svc::Token& t, std::string closer) {
        if (t.line == line) open.push_back(std::move(closer)); else ++earlier;
    };
    auto closes = [&] {
        if (!open.empty()) open.pop_back(); else if (earlier > 0) --earlier;
    };
    for (const svc::Token& t : toks) {
        const svc::TokKind k = t.kind;
        if (t.line == line) {
            if (first == svc::TokKind::Eof && k != svc::TokKind::KwPub && k != svc::TokKind::StmtEnd) first = k;
            if (k == svc::TokKind::Assign) assign = true;
            if (k == svc::TokKind::LBrace) brace = true;
            if (k == svc::TokKind::Arrow) arrow = true;
            if (k == svc::TokKind::KwImpl) impl = true;
            if (k == svc::TokKind::KwIf || k == svc::TokKind::KwWhile || k == svc::TokKind::KwMatch ||
                (k == svc::TokKind::KwFor && !impl))
                head_at = open.size();
            if (k == svc::TokKind::LBrace && open.size() <= head_at) head_at = SIZE_MAX;   // the head's block
        }
        switch (k) {
        case svc::TokKind::LParen:         opens(t, ")"); break;
        case svc::TokKind::LBracket:       opens(t, "]"); break;
        case svc::TokKind::LBrace:         opens(t, "}"); break;
        case svc::TokKind::InterpStrBegin: opens(t, "}\""); break;
        case svc::TokKind::RParen: case svc::TokKind::RBracket: case svc::TokKind::RBrace:
        case svc::TokKind::InterpStrEnd:
            closes(); break;
        default: break;
        }
        if (head_at != SIZE_MAX && head_at > open.size()) head_at = SIZE_MAX;   // the head's own bracket closed around it
    }
    std::string s;
    for (std::size_t i = open.size(); i-- > 0;) {
        if (i + 1 == head_at) s += " {}";   // everything opened after the head's keyword is closed
        s += open[i];
    }
    if (head_at == 0) s += " {}";
    if (first == svc::TokKind::KwLet && !assign) s += " = 0";
    if (first == svc::TokKind::KwFn && !brace) s += arrow ? " {}" : " -> () {}";
    return s;
}

// What kind of name the cursor completes, from the tokens of the text before it plus the probe
// (whose identifier must start at `line`/`col`); `finish` receives line_completion(), `qp` the
// path of a Qualified context.
Context cursor_context(const std::string& head, uint32_t line, uint32_t col, std::string& finish, QualifiedPath& qp) {
    std::vector<svc::LexError> errs;
    svc::Lexer lex(head);
    std::vector<svc::Token> toks = lex.tokenize_tolerant(errs);
    std::size_t i = toks.size();
    while (i > 0 && (toks[i - 1].kind == svc::TokKind::Eof || toks[i - 1].kind == svc::TokKind::StmtEnd)) --i;
    if (i == 0) return Context::None;
    const svc::Token& t = toks[i - 1];
    const std::string probe = PROBE;
    const bool ident = t.kind == svc::TokKind::LIdent || t.kind == svc::TokKind::UIdent;
    if (!ident || t.line != line || t.col != col || t.text.size() < probe.size() ||
        t.text.compare(t.text.size() - probe.size(), probe.size(), probe) != 0)
        return Context::None;   // inside a string, a comment or a char literal
    toks.resize(i - 1);
    finish = line_completion(toks, line);
    if (i < 2) return Context::Name;
    // `use a::b::{x, <probe>`: the names of module `a::b`.
    if (toks[i - 2].kind == svc::TokKind::Comma || toks[i - 2].kind == svc::TokKind::LBrace) {
        std::size_t j = i - 2;
        while (j > 0 && (toks[j].kind == svc::TokKind::Comma || is_name_token(toks[j].kind))) --j;
        if (toks[j].kind == svc::TokKind::LBrace && j >= 1 && toks[j - 1].kind == svc::TokKind::ColonColon) {
            std::size_t start = j - 1;
            qp.segments = path_before(toks, j - 1, start);
            if (!qp.segments.empty() && starts_use(toks, start)) {
                qp.in_use = true;
                return Context::Qualified;
            }
            qp.segments.clear();
        }
    }
    switch (toks[i - 2].kind) {
    case svc::TokKind::Dot:
        if (i >= 3 && (toks[i - 3].kind == svc::TokKind::Int || toks[i - 3].kind == svc::TokKind::Double))
            return Context::None;   // `1.` is the start of a number
        return Context::Member;
    case svc::TokKind::ColonColon: {
        std::size_t start = i - 2;
        qp.segments = path_before(toks, i - 2, start);
        if (qp.segments.empty()) return Context::None;
        if (start >= 1 && toks[start - 1].kind == svc::TokKind::KwImport) return Context::None;   // a file path
        qp.in_use = starts_use(toks, start);
        if (!qp.in_use && qp.segments.size() != 1) return Context::None;   // `head::name` takes one segment
        return Context::Qualified;
    }
    case svc::TokKind::KwLet: case svc::TokKind::KwMut: case svc::TokKind::KwFn:
    case svc::TokKind::KwStruct: case svc::TokKind::KwEnum: case svc::TokKind::KwTrait:
    case svc::TokKind::KwConst: case svc::TokKind::KwImport: case svc::TokKind::KwUse:
        return Context::None;
    case svc::TokKind::KwFor:   // `impl Tr for X` names a type; `for x` declares the loop binding
        for (std::size_t j = i - 2; j-- > 0 && toks[j].line == line;)
            if (toks[j].kind == svc::TokKind::KwImpl) return Context::Name;
        return Context::None;
    default:
        return Context::Name;
    }
}

// The call the cursor is in, from the tokens of the text before it plus the probe (whose
// identifier must END at the cursor, `line`/`col`): the innermost open `(` that follows a name,
// a `)` or a `]`, looking outward through groups and brackets but never out of a block or a
// `${` hole -- nothing is shown inside a lambda body. The parameter list of a declaration or a
// `fn` type is no call.
struct CallContext {
    bool        found = false;
    uint32_t    line = 0, col = 0;       // the call's `(`
    int         commas = 0;              // at the call's depth: the argument the cursor is in
    bool        needs_value = false;     // what precedes the cursor ends no expression: `(`, `,`, `+`
    std::string finish;                  // line_completion()
};

CallContext call_context(const std::string& head, uint32_t line, uint32_t col) {
    CallContext cc;
    std::vector<svc::LexError> errs;
    svc::Lexer lex(head);
    std::vector<svc::Token> toks = lex.tokenize_tolerant(errs);
    std::size_t n = toks.size();
    while (n > 0 && (toks[n - 1].kind == svc::TokKind::Eof || toks[n - 1].kind == svc::TokKind::StmtEnd)) --n;
    if (n == 0) return cc;
    const svc::Token& t = toks[n - 1];
    const std::string probe = PROBE;
    const bool ident = t.kind == svc::TokKind::LIdent || t.kind == svc::TokKind::UIdent;
    if (!ident || t.line != line || t.text.size() < probe.size() ||
        t.col + t.text.size() != col + probe.size() ||
        t.text.compare(t.text.size() - probe.size(), probe.size(), probe) != 0)
        return cc;   // inside a string, a comment or a char literal
    const bool bare = t.text.size() == probe.size();
    toks.resize(n - 1);
    cc.finish = line_completion(toks, line);
    if (bare && !toks.empty()) {
        switch (toks.back().kind) {
        case svc::TokKind::LIdent: case svc::TokKind::UIdent: case svc::TokKind::Int: case svc::TokKind::Double:
        case svc::TokKind::Str: case svc::TokKind::KwTrue: case svc::TokKind::KwFalse:
        case svc::TokKind::KwSelf: case svc::TokKind::RParen: case svc::TokKind::RBracket: case svc::TokKind::RBrace:
        case svc::TokKind::InterpStrEnd: case svc::TokKind::Question:
            break;
        default:
            cc.needs_value = true;
        }
    }

    struct Open { svc::TokKind kind; std::size_t at; int commas; };
    std::vector<Open> open;
    for (std::size_t i = 0; i < toks.size(); ++i) {
        switch (toks[i].kind) {
        case svc::TokKind::LParen: case svc::TokKind::LBracket: case svc::TokKind::LBrace:
        case svc::TokKind::InterpStrBegin:
            open.push_back(Open{ toks[i].kind, i, 0 }); break;
        case svc::TokKind::RParen: case svc::TokKind::RBracket: case svc::TokKind::RBrace:
        case svc::TokKind::InterpStrEnd:
            if (!open.empty()) open.pop_back();
            break;
        case svc::TokKind::Comma:
            if (!open.empty()) ++open.back().commas;
            break;
        default: break;
        }
    }
    auto is_name = [](svc::TokKind k) { return k == svc::TokKind::LIdent || k == svc::TokKind::UIdent; };
    // `fn f(`, `fn f[T](`, `struct P(`: the name before `at` is declared there.
    auto declared = [&](std::size_t at) {
        return at >= 1 && is_name(toks[at].kind) && (toks[at - 1].kind == svc::TokKind::KwFn ||
                                                      toks[at - 1].kind == svc::TokKind::KwStruct);
    };
    for (auto o = open.rbegin(); o != open.rend(); ++o) {
        if (o->kind == svc::TokKind::LBrace || o->kind == svc::TokKind::InterpStrBegin) return cc;
        if (o->kind == svc::TokKind::LBracket || o->at == 0) continue;
        const svc::TokKind prev = toks[o->at - 1].kind;
        if (prev == svc::TokKind::KwFn) return cc;   // a lambda's parameters or a `fn` type
        bool call = is_name(prev) || prev == svc::TokKind::RParen;
        if (is_name(prev) && declared(o->at - 1)) return cc;
        if (prev == svc::TokKind::RBracket) {   // `f[T](` declares when `f` does; `xs[0](` calls
            std::size_t j = o->at - 1;
            for (int depth = 0; j-- > 0;) {
                if (toks[j].kind == svc::TokKind::RBracket) ++depth;
                else if (toks[j].kind == svc::TokKind::LBracket && depth-- == 0) break;
            }
            if (j != SIZE_MAX && j > 0 && declared(j - 1)) return cc;
            call = true;
        }
        if (!call) continue;   // a group, a tuple, a condition in parentheses
        cc.found = true;
        cc.line = toks[o->at].line;
        cc.col = toks[o->at].col;
        cc.commas = o->commas;
        return cc;
    }
    return cc;
}

// The file at `path` with `insert` at byte `off`, re-analyzed through the analysis's entry, so
// an imported module is checked in its program.
AnalysisResult reanalyze(const AnalysisResult& analysis, const std::string& path, std::size_t off,
                         const std::string& insert) {
    const std::string& entry = analysis.path_of.at(svc::ENTRY_MODULE_PREFIX);
    Overlay texts = analysis.texts;
    const std::string& text = analysis.texts.at(path);
    texts[path] = text.substr(0, off) + insert + text.substr(off);
    return analyze(entry, texts[entry], texts);
}

// ---- the candidates -----------------------------------------------------------------

class Collector {
public:
    Collector(const svc::Program& prog, std::string module, const std::vector<AmbientName>& ambient)
        : prog_(prog), module_(std::move(module)), ambient_(ambient) {
        for (const svc::ItemPtr& ip : prog.items) {
            const svc::Item& it = *ip;
            if (it.kind == svc::ItemKind::Struct) {
                const auto& st = static_cast<const svc::StructItem&>(it);
                structs_[st.name] = &st;
            } else if (it.kind == svc::ItemKind::Trait) {
                const auto& tr = static_cast<const svc::TraitDecl&>(it);
                traits_[tr.name] = &tr;
            } else if (it.kind == svc::ItemKind::Impl) {
                const auto& im = static_cast<const svc::ImplDecl&>(it);
                const std::string head = impl_head(im);
                if (head.empty()) continue;
                if (!im.is_inherent && bound_param(im, head)) blankets_.push_back(&im);
                else impls_.emplace(head, &im);
            } else if (it.kind == svc::ItemKind::Enum) {
                const auto& en = static_cast<const svc::EnumItem&>(it);
                enums_[en.name] = &en;
                for (const svc::EnumVariant& v : en.variants) variants_[v.name] = { &en, &v };
            } else if (it.kind == svc::ItemKind::Fn) {
                const auto& fn = static_cast<const svc::FnItem&>(it);
                fns_[fn.name] = &fn;
            }
        }
    }

    std::vector<CompletionItem> take() { return std::move(items_); }

    // The members `recv.` reaches on a receiver of type `t`.
    void members(const svc::TyPtr& t) {
        const Reach r = reach(t);
        if (!r.head.empty()) {
            // Field first, as the checker resolves `recv.name`.
            if (const auto s = structs_.find(r.head); s != structs_.end() && !s->second->is_tuple)
                for (const svc::Field& f : s->second->fields)
                    add(f.name, CompletionKind::Field, f.name + ": " + (f.type ? render_type(*f.type) : std::string("?")), '0');
            const auto [lo, hi] = impls_.equal_range(r.head);
            for (auto it = lo; it != hi; ++it)
                if (it->second->is_inherent)
                    for (const svc::Method& m : it->second->methods)   // an associated fn is not reached by `.`
                        if (m.has_self) add(m.name, CompletionKind::Method, render_method(m), '1');
        }
        for (const std::string& name : r.traits)
            if (const svc::TraitDecl* tr = trait(name))
                for (const svc::Method& m : tr->methods)
                    if (m.has_self) add(m.name, CompletionKind::Method, render_method(m), '2');
    }

    // ---- signature help -------------------------------------------------------------

    // The signatures the callee of `c` may have: one, one per shape for a builtin, and for a trait
    // method one per candidate trait while the call has not checked (the pick the checker records is
    // there only once it has).
    std::vector<SignatureInfo> signatures(const svc::CallExpr& c) const {
        std::vector<SignatureInfo> out;
        const svc::Expr* callee = c.callee.get();
        if (!callee) return out;
        if (callee->kind == svc::ExprKind::Field) {   // `recv.m(..)`: `self` is the receiver
            const auto& f = static_cast<const svc::FieldExpr&>(*callee);
            if (trait_method_signature(f.resolved_trait, f.name, /*self_is_arg=*/false, out)) return out;
            for (const svc::Method* m : methods_named(f.obj ? f.obj->ty : svc::TyPtr(), f.name))
                out.push_back(method_signature(*m, /*self_is_arg=*/false));
            if (out.empty()) fn_type_signature(f.name, callee->ty, out);   // a field of `fn` type
            return out;
        }
        if (callee->kind != svc::ExprKind::Ident) { fn_type_signature({}, callee->ty, out); return out; }
        const auto& id = static_cast<const svc::IdentExpr&>(*callee);
        if (!id.qualifier.empty()) {   // `Type::m(..)` / `Trait::m(..)`; the checker mangled the head
            // Not `id.inherent`: the checker records it only for a call that checks, and a call
            // being typed rarely has its arguments yet. A type's method first, as the checker.
            const auto [lo, hi] = impls_.equal_range(id.qualifier);
            for (auto it = lo; it != hi; ++it)
                if (it->second->is_inherent)
                    for (const svc::Method& m : it->second->methods)
                        if (m.name == id.name) out.push_back(method_signature(m, /*self_is_arg=*/true));
            if (out.empty())
                if (const svc::TraitDecl* tr = trait(id.qualifier))
                    for (const svc::Method& m : tr->methods)
                        if (m.name == id.name) out.push_back(method_signature(m, /*self_is_arg=*/true));
            return out;
        }
        // A resolved fn, constructor or variant carries its mangled name.
        if (const auto f = fns_.find(id.name); f != fns_.end()) {
            out.push_back(fn_signature(*f->second));
            return out;
        }
        if (const auto v = variants_.find(id.name); v != variants_.end()) {
            if (v->second.second->is_tuple)
                out.push_back(fields_signature(svc::short_name(v->second.first->name) + "::" + svc::short_name(id.name),
                                               v->second.second->fields));
            return out;
        }
        if (const auto s = structs_.find(id.name); s != structs_.end()) {
            if (s->second->is_tuple) out.push_back(fields_signature(svc::short_name(id.name), s->second->fields));
            return out;
        }
        if (fn_type_signature(id.name, callee->ty, out)) return out;   // a local of `fn` type
        const std::string bare = svc::short_name(id.name);
        for (const AmbientName& a : ambient_)
            if (a.name == bare) {
                ambient_signatures(a.signature, out);
                return out;
            }
        // `area(s)`: dispatched on the first argument. The trait the checker picked when the call
        // checked; else the ones the first argument's type reaches; else, with no argument typed yet,
        // every trait declaring the name.
        if (trait_method_signature(id.resolved_trait, bare, /*self_is_arg=*/true, out)) return out;
        if (!c.args.empty() && c.args[0] && c.args[0]->ty) {
            for (const svc::Method* m : methods_named(c.args[0]->ty, bare))
                out.push_back(method_signature(*m, /*self_is_arg=*/true));
            if (!out.empty()) return out;
        }
        for (const auto& [name, tr] : traits_)
            for (const svc::Method& m : tr->methods)
                if (m.has_self && m.name == bare) out.push_back(method_signature(m, /*self_is_arg=*/true));
        return out;
    }

    // The one signature of `method` in the trait the checker dispatched the call to, if it recorded
    // one and the trait is a user declaration (a std trait's methods are rendered from the tree too,
    // so this holds for them as well). False = nothing recorded, the caller falls back.
    bool trait_method_signature(const std::string& picked, const std::string& method, bool self_is_arg,
                                std::vector<SignatureInfo>& out) const {
        if (picked.empty()) return false;
        const svc::TraitDecl* tr = trait(picked);
        if (!tr) return false;
        for (const svc::Method& m : tr->methods)
            if (m.name == method) { out.push_back(method_signature(m, self_is_arg)); return true; }
        return false;
    }

private:
    // What `recv.` reaches on a receiver of type `t`: the head type, whose fields and inherent
    // methods count, and the traits, supertraits included.
    struct Reach {
        std::string head;
        std::vector<std::string> traits;
    };
    Reach reach(const svc::TyPtr& t) const {
        Reach r;
        if (!t) return r;
        switch (t->kind) {
        case svc::TyKind::Named:  r.head = t->name; break;
        case svc::TyKind::Int:    r.head = "Int"; break;
        case svc::TyKind::Double: r.head = "Double"; break;
        case svc::TyKind::Bool:   r.head = "Bool"; break;
        case svc::TyKind::String: r.head = "String"; break;
        case svc::TyKind::Dyn:    r.traits.push_back(t->name); break;
        case svc::TyKind::Var:    for (const svc::Bound& b : t->bounds) r.traits.push_back(b.trait); break;
        default: return r;
        }
        if (!r.head.empty()) {
            const auto [lo, hi] = impls_.equal_range(r.head);
            for (auto it = lo; it != hi; ++it)
                if (!it->second->is_inherent) r.traits.push_back(it->second->trait_name);
            for (const svc::ImplDecl* im : blankets_) {
                const svc::GenericParam* g = bound_param(*im, impl_head(*im));
                bool holds = true;
                for (const svc::BoundRef& b : g->bounds) holds = holds && implements(r.head, b.trait);
                if (holds) r.traits.push_back(im->trait_name);
            }
        }
        for (std::size_t i = 0; i < r.traits.size(); ++i)   // grows: the supertraits are appended
            if (const svc::TraitDecl* tr = trait(r.traits[i]))
                for (const std::string& s : tr->supertraits)
                    if (std::find(r.traits.begin(), r.traits.end(), s) == r.traits.end()) r.traits.push_back(s);
        return r;
    }

    // The methods `recv.name(..)` reaches: the inherent one, which hides a trait method of the
    // same name, else those of the traits.
    std::vector<const svc::Method*> methods_named(const svc::TyPtr& t, const std::string& name) const {
        std::vector<const svc::Method*> out;
        const Reach r = reach(t);
        if (!r.head.empty()) {
            const auto [lo, hi] = impls_.equal_range(r.head);
            for (auto it = lo; it != hi; ++it)
                if (it->second->is_inherent)
                    for (const svc::Method& m : it->second->methods)
                        if (m.has_self && m.name == name) out.push_back(&m);
        }
        if (!out.empty()) return out;
        for (const std::string& tn : r.traits)
            if (const svc::TraitDecl* tr = trait(tn))
                for (const svc::Method& m : tr->methods)
                    if (m.has_self && m.name == name) out.push_back(&m);
        return out;
    }

    // A label assembled piece by piece; a parameter's piece is recorded with its offsets.
    struct Label {
        SignatureInfo sig;
        void text(const std::string& s) { sig.label += s; }
        void param(const std::string& s) {
            const auto start = static_cast<uint32_t>(sig.label.size());
            sig.label += s;
            sig.params.emplace_back(start, static_cast<uint32_t>(sig.label.size()));
        }
    };

    static std::string param_text(const svc::Param& p) {
        return (p.is_mut ? "mut " : "") + p.name + (p.type ? ": " + render_type(*p.type) : std::string());
    }

    static SignatureInfo method_signature(const svc::Method& m, bool self_is_arg) {
        Label l;
        l.text("fn " + m.name + render_generics(m.generics) + "(");
        bool first = true;
        if (m.has_self) {
            const std::string self = m.self_mut ? "mut self" : "self";
            if (self_is_arg) l.param(self); else l.text(self);
            first = false;
        }
        for (const svc::Param& p : m.params) {
            if (!first) l.text(", ");
            first = false;
            l.param(param_text(p));
        }
        l.text(")");
        if (m.ret) l.text(" -> " + render_type(*m.ret));
        return l.sig;
    }

    static SignatureInfo fn_signature(const svc::FnItem& f) {
        Label l;
        l.text("fn " + svc::short_name(f.name) + render_generics(f.generics) + "(");
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            if (i) l.text(", ");
            l.param(param_text(f.params[i]));
        }
        l.text(")");
        if (f.ret) l.text(" -> " + render_type(*f.ret));
        return l.sig;
    }

    // A tuple variant or tuple struct built like a call: `Shape::Circle(Double)`.
    static SignatureInfo fields_signature(const std::string& name, const std::vector<svc::Field>& fields) {
        Label l;
        l.text(name + "(");
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (i) l.text(", ");
            l.param(fields[i].type ? render_type(*fields[i].type) : std::string("?"));
        }
        l.text(")");
        return l.sig;
    }

    // A callee of `fn` type -- a local, a parameter, a closure, a field: `fn name(A, B) -> R`.
    static bool fn_type_signature(const std::string& name, const svc::TyPtr& t, std::vector<SignatureInfo>& out) {
        if (!t || t->kind != svc::TyKind::Fn) return false;
        auto text = [](const svc::TyPtr& ty) {
            return ty ? drop_module_paths(svc::display_name(svc::describe(ty))) : std::string("?");
        };
        Label l;
        l.text(name.empty() ? "fn(" : "fn " + name + "(");
        for (std::size_t i = 0; i < t->args.size(); ++i) {
            if (i) l.text(", ");
            l.param(text(t->args[i]));
        }
        l.text(")");
        if (t->ret && t->ret->kind != svc::TyKind::Unit) l.text(" -> " + text(t->ret));
        out.push_back(std::move(l.sig));
        return true;
    }

    // A builtin's or native's signature text, one line per shape: the parameters are what lies
    // between the parentheses, split at the commas outside brackets. A trailing `...` marks a
    // variadic builtin.
    static void ambient_signatures(const std::string& text, std::vector<SignatureInfo>& out) {
        std::size_t from = 0;
        while (from < text.size()) {
            std::size_t nl = text.find('\n', from);
            if (nl == std::string::npos) nl = text.size();
            const std::string line = drop_module_paths(text.substr(from, nl - from));
            from = nl + 1;
            SignatureInfo sig;
            sig.label = line;
            const std::size_t open = line.find('(');
            if (open == std::string::npos) { out.push_back(std::move(sig)); continue; }
            int depth = 0;
            std::size_t start = open + 1;
            for (std::size_t i = open + 1; i < line.size(); ++i) {
                const char ch = line[i];
                if (ch == '(' || ch == '[') { ++depth; continue; }
                if ((ch == ')' || ch == ']') && depth > 0) { --depth; continue; }
                if ((ch == ',' && depth == 0) || ch == ')') {
                    std::size_t a = start, b = i;
                    while (a < b && line[a] == ' ') ++a;
                    while (b > a && line[b - 1] == ' ') --b;
                    if (b > a) sig.params.emplace_back(static_cast<uint32_t>(a), static_cast<uint32_t>(b));
                    if (ch == ')') break;
                    start = i + 1;
                }
            }
            if (!sig.params.empty()) {
                const auto& [a, b] = sig.params.back();
                sig.variadic = line.compare(a, b - a, "...") == 0;
            }
            out.push_back(std::move(sig));
        }
    }

public:

    enum class Want { Values, Types, Both };   // an expression, a type position, or unknown

    // ---- after `::` -------------------------------------------------------------------

    // `Head::` with an uppercase head: an enum's variants and the fns of its inherent impls, a
    // struct's inherent fns (a method too: `Type::m(x)` takes the receiver first), a trait's
    // methods. A type's trait methods are not reachable this way.
    void type_members(const std::string& head) {
        const svc::Item* it = resolve_type(head);
        if (!it) return;
        std::string key;
        if (it->kind == svc::ItemKind::Trait) {
            for (const svc::Method& m : static_cast<const svc::TraitDecl&>(*it).methods)
                add(m.name, CompletionKind::Method, render_method(m), '1');
            return;
        }
        if (it->kind == svc::ItemKind::Enum) {
            const auto& en = static_cast<const svc::EnumItem&>(*it);
            add_variants(en, '0');
            key = en.name;
        } else {
            key = static_cast<const svc::StructItem&>(*it).name;
        }
        const auto [lo, hi] = impls_.equal_range(key);
        for (auto i = lo; i != hi; ++i)
            if (i->second->is_inherent)
                for (const svc::Method& m : i->second->methods)
                    add(m.name, m.has_self ? CompletionKind::Method : CompletionKind::Function, render_method(m), '1');
    }

    // `mod::` with a lowercase head: the `pub` items of a module this file imports, or of the
    // ring for the virtual `std` (natives are not reached as `std::name`).
    void module_members(const std::string& head, Want want) {
        const bool types_only = want == Want::Types;
        if (head == "std") {
            for (const svc::ItemPtr& ip : prog_.items)
                if (ip->is_pub && is_ring(ip->module_prefix)) add_item(*ip, '1', types_only, /*variants=*/false);
            return;
        }
        if (!imports(head)) return;
        for (const svc::ItemPtr& ip : prog_.items)
            if (ip->is_pub && ip->module_prefix == head) add_item(*ip, '1', types_only, /*variants=*/true);
    }

    // `use a::b::` / `use a::b::{x, `: the modules below `a::b`, the `pub` items and gated
    // natives of module `a::b`, or the variants of enum `b` in module `a`.
    void use_path(const std::vector<std::string>& segs) {
        std::string path;
        for (std::size_t i = 0; i < segs.size(); ++i) path += (i ? "::" : "") + segs[i];
        std::set<std::string> modules;
        for (const svc::ItemPtr& ip : prog_.items) modules.insert(ip->module_prefix);
        for (const AmbientName& f : ambient_) if (!f.module.empty()) modules.insert(f.module);
        const std::string below = path + "::";
        for (const std::string& m : modules)
            if (m.rfind(below, 0) == 0) {
                const std::string rest = m.substr(below.size());
                add(rest.substr(0, rest.find("::")), CompletionKind::Module, {}, '0');
            }
        if (modules.count(path)) {
            for (const svc::ItemPtr& ip : prog_.items)
                if (ip->is_pub && ip->module_prefix == path) add_item(*ip, '1', /*types_only=*/false, /*variants=*/false);
            for (const AmbientName& f : ambient_)
                if (f.module == path) add(f.name, CompletionKind::Function, drop_module_paths(f.signature), '1');
            return;
        }
        if (segs.size() < 2) return;
        const std::string mod = path.substr(0, path.size() - segs.back().size() - 2);
        for (const svc::ItemPtr& ip : prog_.items)
            if (ip->kind == svc::ItemKind::Enum && ip->module_prefix == mod && item_name(*ip) == segs.back())
                add_variants(static_cast<const svc::EnumItem&>(*ip), '0');
    }

    // The names in scope: `locals` (or the type parameters) first, then this file's declarations,
    // then what the std, the `use` items and the builtins bring in, then the keywords.
    void names(const std::vector<ProbeLocal>& locals, const std::vector<std::string>& type_params, Want want) {
        const bool types_only = want == Want::Types;
        if (!types_only)
            for (const ProbeLocal& l : locals)
                add(l.name, CompletionKind::Variable, l.type.empty() ? std::string() : l.name + ": " + l.type, '0');
        if (want != Want::Values)
            for (auto tp = type_params.rbegin(); tp != type_params.rend(); ++tp)
                add(*tp, CompletionKind::TypeParameter, {}, '0');
        for (const svc::ItemPtr& ip : prog_.items)
            if (ip->module_prefix == module_) add_item(*ip, '1', types_only, /*variants=*/true);
        for (const svc::ItemPtr& ip : prog_.items)
            if (ip->is_pub && is_ring(ip->module_prefix)) {
                // Of the ring's enums only Option and Result have bare variants.
                const bool core_enum = ip->kind == svc::ItemKind::Enum &&
                    (static_cast<const svc::EnumItem&>(*ip).name == svc::std_Option() ||
                     static_cast<const svc::EnumItem&>(*ip).name == svc::std_Result());
                add_item(*ip, '2', types_only, core_enum);
            }
        for (const svc::ItemPtr& ip : prog_.items)
            if (ip->module_prefix == module_ && ip->kind == svc::ItemKind::Use)
                add_use(static_cast<const svc::UseItem&>(*ip), types_only);
        // A trait method is callable bare, `area(s)` (the checker dispatches on the first argument),
        // for every trait of the program.
        if (!types_only)
            for (const auto& [name, tr] : traits_) {
                const bool std_trait = tr->module_prefix.rfind("std::", 0) == 0;
                for (const svc::Method& m : tr->methods)
                    if (m.has_self && !(std_trait && !m.name.empty() && m.name[0] == '_'))
                        add(m.name, CompletionKind::Method, render_method(m), tr->module_prefix == module_ ? '1' : '2');
            }
        if (want != Want::Values)
            for (const char* t : BUILTIN_TYPES) add(t, CompletionKind::Struct, {}, '2');
        if (types_only) return;
        // A module this file imports is named by its last segment (`import net::http` -> `http::f`).
        for (const svc::ItemPtr& ip : prog_.items)
            if (ip->module_prefix == module_ && ip->kind == svc::ItemKind::Import) {
                const auto& im = static_cast<const svc::ImportItem&>(*ip);
                if (!im.path.empty()) add(im.path.back(), CompletionKind::Module, {}, '1');
            }
        for (const AmbientName& f : ambient_)
            if (f.module.empty()) add(f.name, CompletionKind::Function, drop_module_paths(f.signature), '2');
        for (const char* k : KEYWORDS) add(k, CompletionKind::Keyword, {}, '3');
    }

private:
    void add(const std::string& label, CompletionKind kind, std::string detail, char group) {
        if (label.empty() || !seen_.insert(label).second) return;
        items_.push_back(CompletionItem{ label, kind, std::move(detail), std::string(1, group) + label });
    }

    void add_item(const svc::Item& it, char group, bool types_only, bool variants) {
        const bool std_item = it.module_prefix.rfind("std::", 0) == 0;
        auto name_of = [&](const std::string& mangled) {
            const std::string n = svc::short_name(mangled);
            return std_item && !n.empty() && n[0] == '_' ? std::string() : n;   // std internals
        };
        switch (it.kind) {
        case svc::ItemKind::Fn:
            if (!types_only) add(name_of(static_cast<const svc::FnItem&>(it).name), CompletionKind::Function, render_item(it), group);
            break;
        case svc::ItemKind::Const:
            if (!types_only) add(name_of(static_cast<const svc::ConstItem&>(it).name), CompletionKind::Constant, render_item(it), group);
            break;
        case svc::ItemKind::Struct:
            add(name_of(static_cast<const svc::StructItem&>(it).name), CompletionKind::Struct, render_item(it), group);
            break;
        case svc::ItemKind::Enum: {
            const auto& en = static_cast<const svc::EnumItem&>(it);
            add(name_of(en.name), CompletionKind::Enum, render_item(it), group);
            if (variants && !types_only) add_variants(en, group);
            break;
        }
        case svc::ItemKind::Trait:
            add(name_of(static_cast<const svc::TraitDecl&>(it).name), CompletionKind::Interface, render_item(it), group);
            break;
        case svc::ItemKind::Impl: case svc::ItemKind::Stmt: case svc::ItemKind::Import: case svc::ItemKind::Use:
            break;
        }
    }

    // An uppercase type name as this module sees it, as the checker's `mangle_ref` resolves it: this
    // module's declaration, then what its `use` items bring in, then the ring's `pub` items.
    const svc::Item* resolve_type(const std::string& name) const {
        auto find = [&](const std::string& module, bool need_pub) -> const svc::Item* {
            for (const svc::ItemPtr& ip : prog_.items)
                if (ip->module_prefix == module && (!need_pub || ip->is_pub) && item_name(*ip) == name &&
                    (ip->kind == svc::ItemKind::Struct || ip->kind == svc::ItemKind::Enum || ip->kind == svc::ItemKind::Trait))
                    return ip.get();
            return nullptr;
        };
        if (const svc::Item* it = find(module_, false)) return it;
        for (bool glob : { false, true })
            for (const svc::ItemPtr& ip : prog_.items) {
                if (ip->module_prefix != module_ || ip->kind != svc::ItemKind::Use) continue;
                const auto& u = static_cast<const svc::UseItem&>(*ip);
                if (u.glob != glob) continue;
                if (!glob && std::find(u.names.begin(), u.names.end(), name) == u.names.end()) continue;
                std::string path;
                for (std::size_t i = 0; i < u.path.size(); ++i) path += (i ? "::" : "") + u.path[i];
                if (const svc::Item* it = find(path, true)) return it;
            }
        for (const char* r : RING_MODULES)
            if (const svc::Item* it = find(r, true)) return it;
        return nullptr;
    }

    // Whether this module imports module `m` (`import m`, or a `use` of it), as `mod::name` requires.
    bool imports(const std::string& m) const {
        for (const svc::ItemPtr& ip : prog_.items) {
            if (ip->module_prefix != module_) continue;
            const std::vector<std::string>* segs = nullptr;
            if (ip->kind == svc::ItemKind::Import) segs = &static_cast<const svc::ImportItem&>(*ip).path;
            else if (ip->kind == svc::ItemKind::Use) segs = &static_cast<const svc::UseItem&>(*ip).path;
            if (!segs) continue;
            std::string path;
            for (std::size_t i = 0; i < segs->size(); ++i) path += (i ? "::" : "") + (*segs)[i];
            if (path == m) return true;
        }
        return false;
    }

    void add_variants(const svc::EnumItem& en, char group, const std::string& only = {}) {
        for (const svc::EnumVariant& v : en.variants) {
            const std::string n = svc::short_name(v.name);
            if (only.empty() || n == only)
                add(n, CompletionKind::EnumMember, svc::short_name(en.name) + "::" + n, group);
        }
    }

    // `use m::name`, `use m::{a, b}`, `use m::*`, `use m::Enum::*`, `use m::Enum::{V}`.
    void add_use(const svc::UseItem& u, bool types_only) {
        std::string path;
        for (std::size_t i = 0; i < u.path.size(); ++i) path += (i ? "::" : "") + u.path[i];
        const auto en = enums_.find(path);
        if (u.glob) {
            if (en != enums_.end()) { if (!types_only) add_variants(*en->second, '2'); return; }
            for (const svc::ItemPtr& ip : prog_.items)
                if (ip->module_prefix == path && ip->is_pub) add_item(*ip, '2', types_only, /*variants=*/true);
            if (!types_only)
                for (const AmbientName& f : ambient_)
                    if (f.module == path) add(f.name, CompletionKind::Function, drop_module_paths(f.signature), '2');
            return;
        }
        for (const std::string& n : u.names) {
            if (en != enums_.end()) { if (!types_only) add_variants(*en->second, '2', n); continue; }
            for (const svc::ItemPtr& ip : prog_.items)
                if (ip->module_prefix == path && item_name(*ip) == n) add_item(*ip, '2', types_only, /*variants=*/false);
            if (!types_only)
                for (const AmbientName& f : ambient_)
                    if (f.module == path && f.name == n) add(f.name, CompletionKind::Function, drop_module_paths(f.signature), '2');
        }
    }

    static std::string item_name(const svc::Item& it) {
        switch (it.kind) {
        case svc::ItemKind::Fn:     return svc::short_name(static_cast<const svc::FnItem&>(it).name);
        case svc::ItemKind::Const:  return svc::short_name(static_cast<const svc::ConstItem&>(it).name);
        case svc::ItemKind::Struct: return svc::short_name(static_cast<const svc::StructItem&>(it).name);
        case svc::ItemKind::Enum:   return svc::short_name(static_cast<const svc::EnumItem&>(it).name);
        case svc::ItemKind::Trait:  return svc::short_name(static_cast<const svc::TraitDecl&>(it).name);
        default:                    return {};
        }
    }

    static bool is_ring(const std::string& module) {
        for (const char* r : RING_MODULES) if (module == r) return true;
        return false;
    }

    // The head type an impl is for (`Vec` for `impl Clone for Vec[T]`), or empty.
    static std::string impl_head(const svc::ImplDecl& im) {
        if (!im.target || im.target->kind != svc::TypeKind::Named) return {};
        return static_cast<const svc::NamedType&>(*im.target).name;
    }

    // A blanket impl `impl[T: B] Tr for T`: the generic parameter the head names, else null.
    static const svc::GenericParam* bound_param(const svc::ImplDecl& im, const std::string& head) {
        for (const svc::GenericParam& g : im.generics) if (g.name == head) return &g;
        return nullptr;
    }

    static bool same_trait(const std::string& a, const std::string& b) {
        return a == b || svc::short_name(a) == svc::short_name(b);
    }

    const svc::TraitDecl* trait(const std::string& name) const {
        if (const auto it = traits_.find(name); it != traits_.end()) return it->second;
        for (const auto& [n, tr] : traits_) if (same_trait(n, name)) return tr;
        return nullptr;
    }

    bool implements(const std::string& head, const std::string& tr) const {
        const auto [lo, hi] = impls_.equal_range(head);
        for (auto it = lo; it != hi; ++it)
            if (!it->second->is_inherent && same_trait(it->second->trait_name, tr)) return true;
        return false;
    }

    const svc::Program& prog_;
    std::string module_;
    const std::vector<AmbientName>& ambient_;
    std::map<std::string, const svc::StructItem*> structs_;
    std::map<std::string, const svc::TraitDecl*> traits_;
    std::map<std::string, const svc::EnumItem*> enums_;
    std::map<std::string, const svc::FnItem*> fns_;
    std::map<std::string, std::pair<const svc::EnumItem*, const svc::EnumVariant*>> variants_;
    std::multimap<std::string, const svc::ImplDecl*> impls_;   // head -> impl (inherent or trait)
    std::vector<const svc::ImplDecl*> blankets_;              // trait impls for a bare type parameter
    std::set<std::string> seen_;
    std::vector<CompletionItem> items_;
};

} // namespace

std::vector<CompletionItem> complete(const AnalysisResult& analysis, const std::string& path, Position pos) {
    const auto tit = analysis.texts.find(path);
    const auto eit = analysis.path_of.find(svc::ENTRY_MODULE_PREFIX);
    if (tit == analysis.texts.end() || eit == analysis.path_of.end()) return {};
    const std::string& text = tit->second;
    const std::size_t off = offset_of(text, pos);
    std::size_t word = off;
    while (word > 0 && is_ident_char(text[word - 1])) --word;
    std::size_t line_start = 0;
    if (word > 0)
        if (const std::size_t nl = text.rfind('\n', word - 1); nl != std::string::npos) line_start = nl + 1;
    const uint32_t line = static_cast<uint32_t>(pos.line) + 1;
    const uint32_t col  = static_cast<uint32_t>(word - line_start) + 1;

    std::string finish;
    QualifiedPath qp;
    const Context ctx = cursor_context(text.substr(0, off) + PROBE, line, col, finish, qp);
    if (ctx == Context::None) return {};
    // After `::` the path decides; the current tree's items are intact even while the line does
    // not parse. Only a module head asks the probe whether the position is a type.
    if (ctx == Context::Qualified) {
        if (!analysis.program || !analysis.module_of.count(path)) return {};
        const std::string& head = qp.segments.front();
        if (qp.in_use || std::isupper(static_cast<unsigned char>(head[0]))) {
            Collector c(*analysis.program, analysis.module_of.at(path), analysis.ambient);
            if (qp.in_use) c.use_path(qp.segments); else c.type_members(head);
            return c.take();
        }
        const std::string word_probe = word == off ? UPPER_PROBE : PROBE;
        AnalysisResult r = reanalyze(analysis, path, off, word_probe);
        Probe p = probe(r, path, line, col);
        if (!p.exact && !finish.empty()) {
            AnalysisResult r2 = reanalyze(analysis, path, off, word_probe + finish);
            Probe p2 = probe(r2, path, line, col);
            if (p2.exact) p = std::move(p2);
        }
        Collector c(*analysis.program, analysis.module_of.at(path), analysis.ambient);
        c.module_members(head, p.exact && p.kind == Probe::Kind::Type ? Collector::Want::Types : Collector::Want::Both);
        return c.take();
    }
    const std::string probe_word = ctx == Context::Name && word == off ? UPPER_PROBE : PROBE;

    // Re-analyze with the probe at the cursor.
    auto attempt = [&](const std::string& insert, AnalysisResult& out) {
        out = reanalyze(analysis, path, off, insert);
        return probe(out, path, line, col);
    };
    AnalysisResult r;
    Probe p = attempt(probe_word, r);
    if (!p.exact && !finish.empty()) {   // finish the line and try again
        AnalysisResult r2;
        Probe p2 = attempt(probe_word + finish, r2);
        if (p2.exact) { r = std::move(r2); p = std::move(p2); }
    }
    if (!r.program || !r.module_of.count(path)) return {};

    Collector c(*r.program, r.module_of.at(path), r.ambient);
    if (ctx == Context::Member) {
        if (p.exact && p.kind == Probe::Kind::Member) c.members(p.receiver);
    } else if (p.kind == Probe::Kind::Type) {
        c.names({}, p.type_params, Collector::Want::Types);
    } else if (p.kind == Probe::Kind::Value) {
        c.names(p.locals, p.type_params, p.exact ? Collector::Want::Values : Collector::Want::Both);
    }
    return c.take();
}

std::optional<SignatureHelp> signature_help(const AnalysisResult& analysis, const std::string& path, Position pos) {
    const auto tit = analysis.texts.find(path);
    if (tit == analysis.texts.end() || !analysis.path_of.count(svc::ENTRY_MODULE_PREFIX)) return std::nullopt;
    const std::string& text = tit->second;
    const std::size_t off = offset_of(text, pos);
    std::size_t line_start = 0;
    if (off > 0)
        if (const std::size_t nl = text.rfind('\n', off - 1); nl != std::string::npos) line_start = nl + 1;
    const uint32_t line = static_cast<uint32_t>(pos.line) + 1;
    const CallContext cc = call_context(text.substr(0, off) + PROBE, line, static_cast<uint32_t>(off - line_start) + 1);
    if (!cc.found) return std::nullopt;

    // The call in the current tree; while the line does not parse, in a re-analysis with a
    // value at the cursor where one is missing, and then once more with the line finished.
    const AnalysisResult* a = &analysis;
    CallSite site = call_at(analysis, path, cc.line, cc.col);
    AnalysisResult r;
    const std::string value = cc.needs_value ? PROBE : "";
    for (const std::string& insert : { value, value + cc.finish }) {
        if (site.call) break;
        if (insert.empty() || (insert == value && !cc.needs_value)) continue;
        r = reanalyze(analysis, path, off, insert);
        site = call_at(r, path, cc.line, cc.col);
        a = &r;
    }
    if (!site.call || !a->program || !a->module_of.count(path)) return std::nullopt;

    const Collector c(*a->program, a->module_of.at(path), a->ambient);
    SignatureHelp help;
    help.signatures = c.signatures(*site.call);
    if (help.signatures.empty()) return std::nullopt;
    help.active_parameter = cc.commas + (site.piped ? 1 : 0);
    for (std::size_t i = 0; i < help.signatures.size(); ++i) {
        const SignatureInfo& s = help.signatures[i];
        if (s.variadic || static_cast<int>(s.params.size()) > help.active_parameter) {
            help.active_signature = static_cast<int>(i);
            break;
        }
    }
    const SignatureInfo& active = help.signatures[help.active_signature];
    if (active.variadic && help.active_parameter >= static_cast<int>(active.params.size()))
        help.active_parameter = static_cast<int>(active.params.size()) - 1;
    return help;
}

} // namespace lsp
