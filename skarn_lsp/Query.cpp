// =============================================================================
// Query.cpp -- see Query.h.
//
// One Index per file and query: it collects every declaration of the user program (all
// modules) under its mangled name, then walks one file, recording an "occurrence" for
// each name token -- what it refers to (a local binding, a declaration key, or a field)
// and what hover should say. References and rename build an Index for every file of the
// program. Programs are small and queries rare, so nothing is cached between queries.
// =============================================================================

#include "Query.h"

#include "Symbols.h"   // render_type / render_item / render_method / drop_module_paths

#include "Ast.h"       // svc::Program and the node types
#include "Lexer.h"     // svc::Lexer -- a new name must lex as exactly one identifier
#include "Naming.h"    // svc::short_name / svc::display_name
#include "Types.h"     // svc::describe

#include <algorithm>
#include <cctype>
#include <deque>
#include <exception>
#include <memory>
#include <set>
#include <tuple>
#include <vector>

namespace lsp {

namespace {

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool is_upper_start(const std::string& s) {
    return !s.empty() && std::isupper(static_cast<unsigned char>(s[0]));
}

// The inferred type as a reader would write it -- `Vec[dyn Shape]`, not
// `Vec[dyn geometry::shapes::Shape]`: every module path (lowercase `seg::` segments) is
// dropped, as in a declaration. Empty when the checker has no type.
std::string type_text(const svc::TyPtr& t) {
    if (!t || t->kind == svc::TyKind::Error || t->kind == svc::TyKind::Unresolved) return {};
    const std::string s = svc::display_name(svc::describe(t));
    if (s.find("<error>") != std::string::npos) return {};
    return drop_module_paths(s);
}

enum class DeclKind { Fn, Const, Struct, Enum, Variant, Trait, Method };

struct Decl {
    std::string path;
    uint32_t    line = 0, col = 0;   // the declared name
    std::string hover;
    DeclKind    kind = DeclKind::Fn;
};

struct Binding {
    std::string name;
    uint32_t    line = 0, col = 0;   // the name in the binding pattern / parameter list
    std::string type;                // empty until known
    bool        shorthand = false;   // introduced by a struct-pattern shorthand `P { x }`
};

enum Priority { LITERAL = 0, NAME = 1 };

struct Occurrence {
    uint32_t    line = 0, col = 0;   // the name token
    int         priority = NAME;
    Binding*    local = nullptr;     // a local's binding site or use
    std::string global;              // a declaration key (mangled name)
    const svc::Expr* expr = nullptr; // hover falls back to its inferred type
    std::string label;               // printed before that type; a field's name
    std::string sep = ": ";          // between label and type
    std::string text;                // fixed hover text (at a declaration)
    std::string field_of;            // a field access: the key of the receiver's struct
    bool        is_decl  = false;    // the declaration (or binding) site
    bool        is_write = false;    // an assignment target
    std::string shorthand;           // a struct shorthand `P { x }`: the field name (a rename writes `x: new`)
};

// A name nothing could be attributed to: a bare call of a trait method, a method call on
// a receiver of unknown type, an annotation type name the lookup could not decide.
struct Unresolved {
    enum class Kind { Bare, Member, Type } kind = Kind::Bare;   // `m(x)` / `recv.m()` / an annotation
    std::string name;
    uint32_t    line = 0, col = 0;
};

// What an occurrence refers to -- the identity references and rename compare.
struct Target {
    enum class Kind { None, Local, Global, Field } kind = Kind::None;
    const Binding* local = nullptr;
    std::string key;     // Global: the declaration key; Field: the struct key
    std::string name;    // the name as written (a Field's field name)
};

class Index {
public:
    // With a probe position (1-based, 0 = none) the walk also records what sits there; see probe().
    // With a call position it records the call whose `(` is there; see call_at().
    Index(const AnalysisResult& a, const std::string& path, uint32_t probe_line = 0, uint32_t probe_col = 0,
          uint32_t call_line = 0, uint32_t call_col = 0)
        : a_(a), path_(path), probe_{ probe_line, probe_col }, call_pos_{ call_line, call_col } {
        if (!a.program) return;
        const auto mit = a.module_of.find(path);
        const auto tit = a.texts.find(path);
        if (mit == a.module_of.end() || tit == a.texts.end()) return;
        module_ = mit->second;
        text_   = &tit->second;
        if (probing()) pair_braces();
        collect_decls();
        walk_file();
    }

    Probe probe_result() const {
        Probe p;
        const std::vector<const Binding*>* locals = nullptr;
        if (hit_ != Probe::Kind::None) {
            p.kind = hit_; p.exact = true; p.receiver = receiver_; locals = &exact_;
            p.qualified = hit_qualified_;
            p.type_params = exact_type_params_;
        } else if (has_fallback_) {
            p.kind = Probe::Kind::Value; locals = &fallback_;
            p.type_params = fallback_type_params_;
        }
        if (locals)
            for (const Binding* b : *locals) p.locals.push_back(ProbeLocal{ b->name, b->type });
        return p;
    }

    CallSite call_site() const { return CallSite{ call_, piped_ }; }

    const std::string& path() const { return path_; }
    const std::string* text() const { return text_; }
    const std::vector<Occurrence>& occurrences() const { return occ_; }
    const std::vector<Unresolved>& unresolved() const { return unresolved_; }

    const Decl* decl(const std::string& key) const {
        if (key.empty()) return nullptr;
        const auto it = decls_.find(key);
        return it == decls_.end() ? nullptr : &it->second;
    }

    std::optional<std::string> hover(Position pos) const {
        const Occurrence* o = find(pos);
        if (!o) return std::nullopt;
        std::string s;
        if (!o->text.empty()) {
            s = o->text;
        } else if (const Decl* d = decl(o->global)) {
            s = d->hover;
        } else if (o->expr && !type_text(o->expr->ty).empty()) {
            const std::string t = type_text(o->expr->ty);
            s = o->label.empty() ? t : o->label + o->sep + t;
        } else if (o->local && !o->local->type.empty()) {
            s = o->local->name + ": " + o->local->type;
        }
        if (s.empty()) return std::nullopt;
        return "```skarn\n" + s + "\n```";
    }

    std::optional<Location> definition(Position pos) const {
        const Occurrence* o = find(pos);
        if (!o) return std::nullopt;
        if (o->local) return Location{ path_, ident_range(*text_, o->local->line, o->local->col) };
        if (const Decl* d = decl(o->global)) return decl_location(*d);
        if (!o->field_of.empty()) return field_location(o->field_of, o->label);
        return std::nullopt;
    }

    std::optional<Location> decl_location(const Decl& d) const {
        const auto tit = a_.texts.find(d.path);
        if (tit == a_.texts.end()) return std::nullopt;
        return Location{ d.path, ident_range(tit->second, d.line, d.col) };
    }

    // Fields carry no position: the first `name:` after the struct's name.
    std::optional<Location> field_location(const std::string& struct_key, const std::string& field) const {
        const Decl* d = decl(struct_key);
        if (!d) return std::nullopt;
        const auto tit = a_.texts.find(d->path);
        if (tit == a_.texts.end()) return std::nullopt;
        for (uint32_t line = d->line; line < d->line + 64; ++line) {
            const std::string t = line_text(tit->second, line);
            for (std::size_t i = line == d->line ? d->col : 0; (i = t.find(field, i)) != std::string::npos; ++i) {
                std::size_t j = i + field.size();
                if ((i > 0 && is_ident_char(t[i - 1])) || (j < t.size() && is_ident_char(t[j]))) continue;
                while (j < t.size() && (t[j] == ' ' || t[j] == '\t')) ++j;
                if (j < t.size() && t[j] == ':')
                    return Location{ d->path, ident_range(tit->second, line, static_cast<uint32_t>(i) + 1) };
            }
            if (t.find('}') != std::string::npos) break;
        }
        return std::nullopt;
    }

    // The occurrence of the identifier or number token at `pos`.
    const Occurrence* find(Position pos) const {
        if (!text_) return nullptr;
        const uint32_t line = static_cast<uint32_t>(pos.line) + 1;
        const std::string t = line_text(*text_, line);
        std::size_t c = static_cast<std::size_t>(pos.character < 0 ? 0 : pos.character);
        if (c >= t.size() || !is_ident_char(t[c])) {
            if (c == 0 || c > t.size() || !is_ident_char(t[c - 1])) return nullptr;
            --c;   // the cursor sits just after an identifier
        }
        std::size_t start = c;
        while (start > 0 && is_ident_char(t[start - 1])) --start;
        const Occurrence* best = nullptr;
        for (const Occurrence& o : occ_)
            if (o.line == line && o.col == start + 1 && (!best || o.priority >= best->priority)) best = &o;
        return best;
    }

    Target target_of(const Occurrence& o) const {
        Target t;
        if (o.local) {
            t.kind = Target::Kind::Local; t.local = o.local; t.name = o.local->name;
        } else if (decl(o.global)) {
            t.kind = Target::Kind::Global; t.key = o.global; t.name = svc::short_name(o.global);
        } else if (!o.field_of.empty() && decl(o.field_of)) {
            t.kind = Target::Kind::Field; t.key = o.field_of; t.name = o.label;
        }
        return t;
    }

    std::vector<const Occurrence*> matching(const Target& t) const {
        std::vector<const Occurrence*> out;
        for (const Occurrence& o : occ_) {
            const bool hit =
                (t.kind == Target::Kind::Local  && o.local == t.local) ||
                (t.kind == Target::Kind::Global && o.global == t.key) ||
                (t.kind == Target::Kind::Field  && o.global.empty() && o.field_of == t.key && o.label == t.name);
            if (hit) out.push_back(&o);
        }
        return out;
    }

private:
    // ---- declarations --------------------------------------------------------------

    void add_decl(const std::string& key, const std::string& module, uint32_t line, uint32_t col,
                  std::string hover, DeclKind kind) {
        const auto pit = a_.path_of.find(module);
        if (pit == a_.path_of.end() || line == 0) return;
        decls_[key] = Decl{ pit->second, line, col, std::move(hover), kind };
    }

    void collect_decls() {
        const svc::Program& prog = *a_.program;
        for (std::size_t i = prog.prelude_item_count; i < prog.items.size(); ++i) {
            const svc::Item& it = *prog.items[i];
            const std::string& mod = it.module_prefix;
            switch (it.kind) {
            case svc::ItemKind::Fn:
                add_decl(static_cast<const svc::FnItem&>(it).name, mod, it.name_line, it.name_col, render_item(it), DeclKind::Fn);
                break;
            case svc::ItemKind::Const:
                add_decl(static_cast<const svc::ConstItem&>(it).name, mod, it.name_line, it.name_col, render_item(it), DeclKind::Const);
                break;
            case svc::ItemKind::Struct: {
                const auto& st = static_cast<const svc::StructItem&>(it);
                add_decl(st.name, mod, it.name_line, it.name_col, render_item(it), DeclKind::Struct);
                type_names_.emplace(svc::short_name(st.name), st.name);
                break;
            }
            case svc::ItemKind::Enum: {
                const auto& en = static_cast<const svc::EnumItem&>(it);
                add_decl(en.name, mod, it.name_line, it.name_col, render_item(it), DeclKind::Enum);
                type_names_.emplace(svc::short_name(en.name), en.name);
                for (const svc::EnumVariant& v : en.variants) {
                    add_decl(v.name, mod, v.line, v.col, variant_text(en, v), DeclKind::Variant);
                    variant_enum_[v.name] = en.name;
                }
                break;
            }
            case svc::ItemKind::Trait: {
                const auto& tr = static_cast<const svc::TraitDecl&>(it);
                add_decl(tr.name, mod, it.name_line, it.name_col, render_item(it), DeclKind::Trait);
                type_names_.emplace(svc::short_name(tr.name), tr.name);
                for (const svc::Method& m : tr.methods)
                    add_decl(tr.name + "::" + m.name, mod, m.line, m.col, render_method(m), DeclKind::Method);
                break;
            }
            case svc::ItemKind::Impl: {
                const auto& im = static_cast<const svc::ImplDecl&>(it);
                if (!im.target || im.target->kind != svc::TypeKind::Named) break;
                const std::string& head = static_cast<const svc::NamedType&>(*im.target).name;
                for (const svc::Method& m : im.methods) {
                    if (im.is_inherent) add_decl(head + "::" + m.name, mod, m.line, m.col, render_method(m), DeclKind::Method);
                    else trait_impl_[head + "::" + m.name] = im.trait_name + "::" + m.name;
                }
                break;
            }
            case svc::ItemKind::Stmt: case svc::ItemKind::Import: case svc::ItemKind::Use:
                break;
            }
        }
    }

    static std::string variant_text(const svc::EnumItem& en, const svc::EnumVariant& v) {
        std::string s = svc::short_name(en.name) + "::" + svc::short_name(v.name);
        if (v.is_tuple) {
            if (!v.fields.empty()) {
                s += '(';
                for (std::size_t i = 0; i < v.fields.size(); ++i) {
                    if (i) s += ", ";
                    s += render_type(*v.fields[i].type);
                }
                s += ')';
            }
        } else {
            s += " { ";
            for (std::size_t i = 0; i < v.fields.size(); ++i) {
                if (i) s += ", ";
                s += v.fields[i].name + ": " + render_type(*v.fields[i].type);
            }
            s += " }";
        }
        if (v.has_disc) s += " = " + std::to_string(v.disc);
        return s;
    }

    // `recv.name(..)`: the method the receiver's static type reaches -- an inherent method
    // of a named type, else the trait method its impl provides; for a `dyn Tr` or a bounded
    // type parameter, the trait's own method. Empty when none is known.
    std::string method_key(const svc::TyPtr& recv, const std::string& name) const {
        if (!recv) return {};
        std::vector<std::string> heads;
        if (recv->kind == svc::TyKind::Named || recv->kind == svc::TyKind::Dyn) heads.push_back(recv->name);
        if (recv->kind == svc::TyKind::Var)
            for (const svc::Bound& b : recv->bounds) heads.push_back(b.trait);
        for (const std::string& h : heads) {
            const std::string k = h + "::" + name;
            if (decl(k)) return k;
            if (const auto it = trait_impl_.find(k); it != trait_impl_.end() && decl(it->second)) return it->second;
        }
        return {};
    }

    // A type name as written in an annotation (never written back by the checker).
    std::string type_key(const std::string& name, const std::string& qualifier) const {
        if (name.find("::") != std::string::npos) return name;          // an impl target: mangled in place
        if (!qualifier.empty()) return qualifier + "::" + name;
        if (decls_.count(module_ + "::" + name)) return module_ + "::" + name;
        const auto [lo, hi] = type_names_.equal_range(name);
        if (lo != hi && std::next(lo) == hi) return lo->second;         // the only declaration of that name
        return {};
    }

    // The enum or type a qualified path's uppercase HEAD names, or empty (a module head, or
    // not a user declaration).
    std::string head_key(uint32_t line, uint32_t col, const std::string& written_qualifier,
                         const std::string& variant_key) const {
        if (!line) return {};
        const std::string t = line_text(*text_, line);
        if (col == 0 || col > t.size() || !std::isupper(static_cast<unsigned char>(t[col - 1]))) return {};
        if (!written_qualifier.empty() && decl(written_qualifier)) return written_qualifier;   // Type::m / Trait::m
        if (const auto it = variant_enum_.find(variant_key); it != variant_enum_.end()) return it->second;
        return {};
    }

    // ---- the walk ------------------------------------------------------------------

    void walk_file() {
        const svc::Program& prog = *a_.program;
        scopes_.emplace_back();   // the top-level statements of the file, in order
        const svc::Item* prev = nullptr;
        bool taken = false;
        for (std::size_t i = prog.prelude_item_count; i < prog.items.size(); ++i) {
            const svc::Item& it = *prog.items[i];
            if (it.module_prefix != module_) continue;
            if (probing() && !taken && after_probe(it.line, it.col)) { top_fallback(prev); taken = true; }
            walk_item(it);
            prev = &it;
        }
        if (probing() && !taken) top_fallback(prev);
    }

    // ---- the completion probe ------------------------------------------------------

    using Pos = std::pair<uint32_t, uint32_t>;   // 1-based line, column

    bool probing() const { return probe_.first != 0; }
    bool at_probe(uint32_t line, uint32_t col) const { return probing() && line == probe_.first && col == probe_.second; }
    bool after_probe(uint32_t line, uint32_t col) const { return Pos{ line, col } > probe_; }

    // Every binding visible now, innermost first, one per name.
    std::vector<const Binding*> visible() const {
        std::vector<const Binding*> out;
        std::set<std::string> names;
        for (auto s = scopes_.rbegin(); s != scopes_.rend(); ++s)
            for (auto b = s->rbegin(); b != s->rend(); ++b)
                if (names.insert((*b)->name).second) out.push_back(*b);
        return out;
    }

    void hit(Probe::Kind kind, svc::TyPtr receiver = {}, bool qualified = false) {
        if (hit_ != Probe::Kind::None) return;
        hit_ = kind;
        hit_qualified_ = qualified;
        receiver_ = std::move(receiver);
        if (kind == Probe::Kind::Value) exact_ = visible();
        exact_type_params_ = type_params_;
    }

    // The fallback: the bindings of the innermost `{ }` block around the probe, taken before the
    // first statement that starts after it. `open` is that block's `{`, and a later `{` is a
    // deeper block (blocks around one position nest).
    void fallback_at(Pos open) {
        if (has_fallback_ && open <= fallback_open_) return;
        fallback_ = visible();
        fallback_type_params_ = type_params_;
        fallback_open_ = open;
        has_fallback_ = true;
    }

    // Between two items: the file's top-level `let`s when the probe follows a top-level statement
    // (or nothing), none when it lies in a declaration whose body gave no block.
    void top_fallback(const svc::Item* prev) {
        if (has_fallback_) return;
        if (!prev || prev->kind == svc::ItemKind::Stmt) fallback_ = visible();
        fallback_type_params_.clear();
        fallback_open_ = Pos{ 0, 0 };
        has_fallback_ = true;
    }

    bool block_around_probe(const svc::Expr& blk) const {
        const auto it = brace_close_.find(Pos{ blk.line, blk.col });
        return it != brace_close_.end() && Pos{ blk.line, blk.col } < probe_ && probe_ <= it->second;
    }

    // `{` -> its `}`, from the file's tokens (the tree has no end positions).
    void pair_braces() {
        std::vector<svc::LexError> errs;
        svc::Lexer lex(*text_);
        std::vector<Pos> open;
        for (const svc::Token& t : lex.tokenize_tolerant(errs)) {
            if (t.kind == svc::TokKind::LBrace) open.push_back(Pos{ t.line, t.col });
            else if (t.kind == svc::TokKind::RBrace && !open.empty()) {
                brace_close_[open.back()] = Pos{ t.line, t.col };
                open.pop_back();
            }
        }
        for (const Pos& p : open) brace_close_[p] = Pos{ UINT32_MAX, UINT32_MAX };   // never closed
    }

    // A declaration's body sees its own parameters, never the file's top-level `let`s.
    template <class F> void in_fresh_scope(F&& body) {
        std::vector<std::vector<Binding*>> saved = std::move(scopes_);
        const std::size_t type_params = type_params_.size();
        scopes_.clear();
        scopes_.emplace_back();
        body();
        scopes_ = std::move(saved);
        type_params_.resize(type_params);
    }

    // A reference to (or, with `text`, the declaration of) the declaration `key`.
    void name_occurrence(uint32_t line, uint32_t col, std::string key, std::string text) {
        if (line == 0) return;
        Occurrence o;
        o.line = line; o.col = col;
        o.global = std::move(key);
        o.is_decl = !text.empty();
        o.text = std::move(text);
        occ_.push_back(std::move(o));
    }

    void walk_generics(const std::vector<svc::GenericParam>& gs) {
        for (const svc::GenericParam& g : gs) type_params_.push_back(g.name);
        for (const svc::GenericParam& g : gs)
            for (const svc::BoundRef& b : g.bounds) {
                if (decl(b.trait)) name_occurrence(b.line, b.col, b.trait, {});
                for (const auto& a : b.args) walk_type(*a);
            }
    }

    void walk_params(const std::vector<svc::Param>& ps) {
        for (const svc::Param& p : ps) {
            if (p.type) walk_type(*p.type);
            define(p.name, p.name_line, p.name_col, p.type ? render_type(*p.type) : std::string());
        }
    }

    void walk_method(const svc::Method& m, const std::string& key) {
        name_occurrence(m.line, m.col, key, render_method(m));
        in_fresh_scope([&] {
            walk_generics(m.generics);
            if (m.has_self) define("self", m.self_line, m.self_col, {});
            walk_params(m.params);
            if (m.ret) walk_type(*m.ret);
            if (m.body) walk_expr(*m.body);
        });
    }

    // An item's type parameters (and `Self` in a trait or impl) are in scope inside it only.
    void walk_item(const svc::Item& it) {
        const std::size_t type_params = type_params_.size();
        if (it.kind == svc::ItemKind::Trait || it.kind == svc::ItemKind::Impl) type_params_.push_back("Self");
        walk_item_parts(it);
        type_params_.resize(type_params);
    }

    void walk_item_parts(const svc::Item& it) {
        switch (it.kind) {
        case svc::ItemKind::Fn: {
            const auto& f = static_cast<const svc::FnItem&>(it);
            name_occurrence(it.name_line, it.name_col, f.name, render_item(it));
            in_fresh_scope([&] {
                walk_generics(f.generics);
                walk_params(f.params);
                if (f.ret) walk_type(*f.ret);
                if (f.body) walk_expr(*f.body);
            });
            break;
        }
        case svc::ItemKind::Const: {
            const auto& c = static_cast<const svc::ConstItem&>(it);
            name_occurrence(it.name_line, it.name_col, c.name, render_item(it));
            if (c.type) walk_type(*c.type);
            break;
        }
        case svc::ItemKind::Struct: {
            const auto& st = static_cast<const svc::StructItem&>(it);
            name_occurrence(it.name_line, it.name_col, st.name, render_item(it));
            walk_generics(st.generics);
            for (const svc::Field& f : st.fields) {
                if (f.type) walk_type(*f.type);
                if (st.is_tuple) continue;
                // A field has no recorded position; its declaration is found the way
                // go-to-definition finds it, so the two always agree.
                if (const auto loc = field_location(st.name, f.name)) {
                    Occurrence o;
                    o.line = static_cast<uint32_t>(loc->range.start.line) + 1;
                    o.col  = static_cast<uint32_t>(loc->range.start.character) + 1;
                    o.field_of = st.name; o.label = f.name; o.is_decl = true;
                    o.text = f.name + ": " + (f.type ? render_type(*f.type) : std::string("?"));
                    occ_.push_back(std::move(o));
                }
            }
            break;
        }
        case svc::ItemKind::Enum: {
            const auto& en = static_cast<const svc::EnumItem&>(it);
            name_occurrence(it.name_line, it.name_col, en.name, render_item(it));
            walk_generics(en.generics);
            for (const svc::EnumVariant& v : en.variants) {
                name_occurrence(v.line, v.col, v.name, variant_text(en, v));
                for (const svc::Field& f : v.fields) if (f.type) walk_type(*f.type);
            }
            break;
        }
        case svc::ItemKind::Trait: {
            const auto& tr = static_cast<const svc::TraitDecl&>(it);
            name_occurrence(it.name_line, it.name_col, tr.name, render_item(it));
            walk_generics(tr.generics);
            for (std::size_t i = 0; i < tr.supertraits.size() && i < tr.supertrait_pos.size(); ++i)
                if (decl(tr.supertraits[i]))
                    name_occurrence(tr.supertrait_pos[i].first, tr.supertrait_pos[i].second, tr.supertraits[i], {});
            for (const svc::Method& m : tr.methods) walk_method(m, tr.name + "::" + m.name);
            break;
        }
        case svc::ItemKind::Impl: {
            const auto& im = static_cast<const svc::ImplDecl&>(it);
            walk_generics(im.generics);
            if (!im.is_inherent && decl(im.trait_name)) name_occurrence(im.trait_line, im.trait_col, im.trait_name, {});
            for (const auto& a : im.trait_args) walk_type(*a);
            if (im.target) walk_type(*im.target);
            std::string head = im.trait_name;   // a trait impl's method leads to the trait's declaration
            if (im.is_inherent && im.target && im.target->kind == svc::TypeKind::Named)
                head = static_cast<const svc::NamedType&>(*im.target).name;
            for (const svc::Method& m : im.methods) walk_method(m, head + "::" + m.name);
            break;
        }
        case svc::ItemKind::Use: {
            const auto& u = static_cast<const svc::UseItem&>(it);
            std::string path;
            for (std::size_t i = 0; i < u.path.size(); ++i) path += (i ? "::" : "") + u.path[i];
            const std::string parent = u.path.size() > 1 ? path.substr(0, path.rfind("::")) : std::string();
            for (std::size_t i = 0; i < u.names.size() && i < u.name_pos.size(); ++i) {
                std::string key = path + "::" + u.names[i];
                if (!decl(key) && !parent.empty()) key = parent + "::" + u.names[i];   // `use m::Enum::{V}`
                if (decl(key)) name_occurrence(u.name_pos[i].first, u.name_pos[i].second, key, {});
            }
            break;
        }
        case svc::ItemKind::Stmt: {
            const auto& si = static_cast<const svc::StmtItem&>(it);
            if (si.stmt) walk_stmt(*si.stmt);
            break;
        }
        case svc::ItemKind::Import:
            break;
        }
    }

    void walk_type(const svc::Type& t) {
        switch (t.kind) {
        case svc::TypeKind::Named: {
            const auto& nt = static_cast<const svc::NamedType&>(t);
            if (at_probe(nt.name_line ? nt.name_line : t.line, nt.name_line ? nt.name_col : t.col))
                hit(Probe::Kind::Type, {}, !nt.qualifier.empty());
            const std::string key = type_key(nt.name, nt.qualifier);
            if (decl(key)) name_occurrence(nt.name_line, nt.name_col, key, {});
            else if (nt.name.find("::") == std::string::npos && nt.name_line)
                unresolved_.push_back(Unresolved{ Unresolved::Kind::Type, nt.name, nt.name_line, nt.name_col });
            for (const auto& a : nt.args) walk_type(*a);
            break;
        }
        case svc::TypeKind::Fn: {
            const auto& ft = static_cast<const svc::FnType&>(t);
            for (const auto& p : ft.params) walk_type(*p);
            if (ft.ret) walk_type(*ft.ret);
            break;
        }
        case svc::TypeKind::Tuple:
            for (const auto& e : static_cast<const svc::TupleType&>(t).elems) walk_type(*e);
            break;
        case svc::TypeKind::Dyn: {
            const auto& dt = static_cast<const svc::DynType&>(t);
            if (at_probe(dt.name_line, dt.name_col)) hit(Probe::Kind::Type, {}, !dt.qualifier.empty());
            const std::string key = type_key(dt.trait, dt.qualifier);
            if (decl(key)) name_occurrence(dt.name_line, dt.name_col, key, {});
            else if (dt.name_line) unresolved_.push_back(Unresolved{ Unresolved::Kind::Type, dt.trait, dt.name_line, dt.name_col });
            for (const auto& a : dt.args) walk_type(*a);
            break;
        }
        }
    }

    void walk_stmt(const svc::Stmt& s) {
        switch (s.kind) {
        case svc::StmtKind::Let: {
            const auto& l = static_cast<const svc::LetStmt&>(s);
            if (l.init) walk_expr(*l.init);
            if (l.type) walk_type(*l.type);
            // The binding becomes visible AFTER its initializer: `let x = x + 1` reads the outer x.
            std::string ty = l.type ? render_type(*l.type) : (l.init ? type_text(l.init->ty) : std::string());
            if (l.pat) bind_pattern(*l.pat, ty);
            break;
        }
        case svc::StmtKind::Assign: {
            const auto& as = static_cast<const svc::AssignStmt&>(s);
            if (as.target) {
                const std::size_t before = occ_.size();
                walk_expr(*as.target);
                // The assigned name: the target itself, or the root of `a.b = ..` / `a[i] = ..`.
                if (as.target->kind == svc::ExprKind::Ident && occ_.size() > before) occ_[before].is_write = true;
            }
            if (as.value) walk_expr(*as.value);
            break;
        }
        case svc::StmtKind::Expr: {
            const auto& es = static_cast<const svc::ExprStmt&>(s);
            if (es.expr) walk_expr(*es.expr);
            break;
        }
        }
    }

    // A qualified pattern's uppercase head (`Shape` in `Shape::Square(w)`) refers to the enum.
    void pattern_head(uint32_t qual_line, uint32_t qual_col, const std::string& variant_key) {
        const std::string k = head_key(qual_line, qual_col, {}, variant_key);
        if (!k.empty()) name_occurrence(qual_line, qual_col, k, {});
    }

    // Binds every name `p` introduces into the innermost scope. `whole_type` is the type of
    // the whole pattern when it is known (a `let` initializer / annotation); it only reaches a
    // name that stands for the whole value.
    void bind_pattern(const svc::Pattern& p, const std::string& whole_type = {}) {
        switch (p.kind) {
        case svc::PatKind::Wildcard: break;
        case svc::PatKind::Ident: {
            const auto& ip = static_cast<const svc::IdentPat&>(p);
            define(ip.name, p.line, p.col, whole_type);
            break;
        }
        case svc::PatKind::Bind: {
            const auto& bp = static_cast<const svc::BindPat&>(p);
            define(bp.name, p.line, p.col, whole_type);
            if (bp.sub) bind_pattern(*bp.sub);
            break;
        }
        case svc::PatKind::Literal: {
            const auto& lp = static_cast<const svc::LiteralPat&>(p);
            if (lp.lit) walk_expr(*lp.lit);
            break;
        }
        case svc::PatKind::Range: {
            const auto& rp = static_cast<const svc::RangePat&>(p);
            if (rp.lo) walk_expr(*rp.lo);
            if (rp.hi) walk_expr(*rp.hi);
            break;
        }
        case svc::PatKind::Ctor: {
            const auto& cp = static_cast<const svc::CtorPat&>(p);
            pattern_head(cp.qual_line, cp.qual_col, cp.name);
            if (decl(cp.name)) name_occurrence(p.line, p.col, cp.name, {});
            for (const auto& e : cp.elems) bind_pattern(*e);
            break;
        }
        case svc::PatKind::Tuple:
            for (const auto& e : static_cast<const svc::TuplePat&>(p).elems) bind_pattern(*e);
            break;
        case svc::PatKind::List: {
            const auto& lp = static_cast<const svc::ListPat&>(p);
            for (const auto& e : lp.elems) bind_pattern(*e);
            if (lp.rest) bind_pattern(*lp.rest);
            break;
        }
        case svc::PatKind::Struct: {
            const auto& sp = static_cast<const svc::StructPat&>(p);
            pattern_head(sp.qual_line, sp.qual_col, sp.name);
            if (decl(sp.name)) name_occurrence(p.line, p.col, sp.name, {});
            for (const svc::FieldPat& f : sp.fields) {
                if (f.pat) { bind_pattern(*f.pat); continue; }
                // Shorthand `P { x }` binds x; the parser keeps no position for it.
                const auto [line, col] = find_shorthand(p.line, p.col, f.name);
                define(f.name, line, col, {}, /*shorthand=*/true);
            }
            break;
        }
        case svc::PatKind::Map:
            for (const auto& kv : static_cast<const svc::MapPat&>(p).entries) {
                if (kv.first) walk_expr(*kv.first);
                if (kv.second) bind_pattern(*kv.second);
            }
            break;
        case svc::PatKind::Or: {
            // Every alternative binds the same names; the first one is the declaration.
            const auto& op = static_cast<const svc::OrPat&>(p);
            for (std::size_t i = 0; i < op.alts.size(); ++i) {
                const bool was = or_alt_;
                or_alt_ = or_alt_ || i > 0;
                bind_pattern(*op.alts[i]);
                or_alt_ = was;
            }
            break;
        }
        }
    }

    void walk_expr(const svc::Expr& e) {
        switch (e.kind) {
        case svc::ExprKind::IntLit: case svc::ExprKind::DoubleLit: case svc::ExprKind::BoolLit: {
            Occurrence o;
            o.line = e.line; o.col = e.col; o.priority = LITERAL; o.expr = &e;
            occ_.push_back(std::move(o));
            break;
        }
        case svc::ExprKind::StrLit: case svc::ExprKind::Continue:
            break;
        case svc::ExprKind::Ident: {
            const auto& id = static_cast<const svc::IdentExpr&>(e);
            const bool qualified = id.name_line != e.line || id.name_col != e.col;
            if (qualified) {   // the head of `Type::m` / `Trait::m` / `Enum::V` refers to that type
                const std::string k = head_key(e.line, e.col, id.qualifier, id.name);
                if (!k.empty()) name_occurrence(e.line, e.col, k, {});
            }
            Occurrence o;
            o.line = id.name_line ? id.name_line : e.line;
            o.col  = id.name_line ? id.name_col  : e.col;
            o.expr = &e; o.label = svc::short_name(id.name);
            if (at_probe(o.line, o.col)) hit(Probe::Kind::Value, {}, qualified);
            if (!id.qualifier.empty()) {
                o.global = id.qualifier + "::" + id.name;             // Trait::m / Type::m
            } else if (id.name.find("::") != std::string::npos) {
                o.global = id.name;                                   // mangled by the checker
            } else if (Binding* b = lookup(id.name)) {
                o.local = b;
                if (b->type.empty()) b->type = type_text(e.ty);       // a pattern binding learns its type from a use
            } else {
                unresolved_.push_back(Unresolved{ Unresolved::Kind::Bare, id.name, o.line, o.col });
            }
            occ_.push_back(std::move(o));
            break;
        }
        case svc::ExprKind::Field: {
            const auto& f = static_cast<const svc::FieldExpr&>(e);
            if (f.obj) walk_expr(*f.obj);
            if (!f.tuple_index && at_probe(f.name_line, f.name_col)) hit(Probe::Kind::Member, f.obj ? f.obj->ty : svc::TyPtr());
            Occurrence o;
            o.line = f.name_line; o.col = f.name_col; o.expr = &e; o.label = f.name;
            if (f.obj && f.obj->ty && f.obj->ty->kind == svc::TyKind::Named && !f.tuple_index)
                o.field_of = f.obj->ty->name;
            if (o.line) occ_.push_back(std::move(o));
            break;
        }
        case svc::ExprKind::Unary:  { const auto& u = static_cast<const svc::UnaryExpr&>(e); if (u.operand) walk_expr(*u.operand); break; }
        case svc::ExprKind::Binary: { const auto& b = static_cast<const svc::BinaryExpr&>(e); if (b.lhs) walk_expr(*b.lhs); if (b.rhs) walk_expr(*b.rhs); break; }
        case svc::ExprKind::Pipe:   {
            const auto& p = static_cast<const svc::PipeExpr&>(e);
            if (p.lhs) walk_expr(*p.lhs);
            if (p.rhs) walk_expr(*p.rhs);
            if (p.rhs && call_ == p.rhs.get()) piped_ = true;   // `x |> f(a)`: x is f's first argument
            break;
        }
        case svc::ExprKind::Call:   {
            const auto& c = static_cast<const svc::CallExpr&>(e);
            if (call_pos_.first != 0 && !call_ && Pos{ c.line, c.col } == call_pos_) call_ = &c;
            if (c.callee) walk_expr(*c.callee);
            // `recv.m(..)`: the member is a method, resolved through the receiver's type.
            if (c.callee && c.callee->kind == svc::ExprKind::Field && !occ_.empty() &&
                occ_.back().expr == c.callee.get()) {
                const auto& f = static_cast<const svc::FieldExpr&>(*c.callee);
                Occurrence& o = occ_.back();
                o.global   = f.obj ? method_key(f.obj->ty, f.name) : std::string();
                o.field_of.clear();
                // Unattributable only when the receiver's type is unknown; a named or `dyn`
                // receiver that reaches no user method is a std type's method.
                const svc::TyPtr& rt = f.obj ? f.obj->ty : svc::TyPtr();
                const bool known = rt && (rt->kind == svc::TyKind::Named || rt->kind == svc::TyKind::Dyn ||
                                          rt->kind == svc::TyKind::Int || rt->kind == svc::TyKind::Double ||
                                          rt->kind == svc::TyKind::Bool || rt->kind == svc::TyKind::String);
                if (o.global.empty() && !known) unresolved_.push_back(Unresolved{ Unresolved::Kind::Member, f.name, o.line, o.col });
            }
            // A builtin (`len`, `push`) or constructor callee has no type of its own; show
            // what the call produces instead: `len(..) -> Int`.
            if (c.callee && c.callee->kind == svc::ExprKind::Ident && type_text(c.callee->ty).empty() &&
                !occ_.empty() && occ_.back().expr == c.callee.get()) {
                occ_.back().expr  = &e;
                occ_.back().label += "(..)";
                occ_.back().sep   = " -> ";
            }
            for (const auto& a : c.args) walk_expr(*a);
            break;
        }
        case svc::ExprKind::Index:  { const auto& ix = static_cast<const svc::IndexExpr&>(e); if (ix.obj) walk_expr(*ix.obj); if (ix.index) walk_expr(*ix.index); break; }
        case svc::ExprKind::Try:    { const auto& t = static_cast<const svc::TryExpr&>(e); if (t.operand) walk_expr(*t.operand); break; }
        case svc::ExprKind::If:     {
            const auto& i = static_cast<const svc::IfExpr&>(e);
            if (i.cond) walk_expr(*i.cond);
            if (i.then_blk) walk_expr(*i.then_blk);
            if (i.else_blk) walk_expr(*i.else_blk);
            break;
        }
        case svc::ExprKind::Match:  {
            const auto& m = static_cast<const svc::MatchExpr&>(e);
            if (m.scrut) walk_expr(*m.scrut);
            for (const svc::MatchArm& arm : m.arms) {
                scopes_.emplace_back();
                if (arm.pat) bind_pattern(*arm.pat);
                if (arm.guard) walk_expr(*arm.guard);
                if (arm.body) walk_expr(*arm.body);
                scopes_.pop_back();
            }
            break;
        }
        case svc::ExprKind::While:  { const auto& w = static_cast<const svc::WhileExpr&>(e); if (w.cond) walk_expr(*w.cond); if (w.body) walk_expr(*w.body); break; }
        case svc::ExprKind::Loop:   { const auto& l = static_cast<const svc::LoopExpr&>(e); if (l.body) walk_expr(*l.body); break; }
        case svc::ExprKind::For:    {
            const auto& f = static_cast<const svc::ForExpr&>(e);
            if (f.iter) walk_expr(*f.iter);
            scopes_.emplace_back();
            if (f.pat) bind_pattern(*f.pat);
            if (f.body) walk_expr(*f.body);
            scopes_.pop_back();
            break;
        }
        case svc::ExprKind::Break:  { const auto& b = static_cast<const svc::BreakExpr&>(e); if (b.value) walk_expr(*b.value); break; }
        case svc::ExprKind::Return: { const auto& r = static_cast<const svc::ReturnExpr&>(e); if (r.value) walk_expr(*r.value); break; }
        case svc::ExprKind::Block:  {
            scopes_.emplace_back();
            const bool around = probing() && block_around_probe(e);
            bool taken = false;
            for (const auto& s : static_cast<const svc::BlockExpr&>(e).stmts) {
                if (around && !taken && after_probe(s->line, s->col)) { fallback_at(Pos{ e.line, e.col }); taken = true; }
                walk_stmt(*s);
            }
            if (around && !taken) fallback_at(Pos{ e.line, e.col });
            scopes_.pop_back();
            break;
        }
        case svc::ExprKind::Lambda: {
            const auto& l = static_cast<const svc::LambdaExpr&>(e);
            // A lambda sees the enclosing locals (it captures them), so no fresh scope.
            scopes_.emplace_back();
            const bool fn_ty = e.ty && e.ty->kind == svc::TyKind::Fn && e.ty->args.size() == l.params.size();
            for (std::size_t i = 0; i < l.params.size(); ++i) {
                const svc::Param& p = l.params[i];
                if (p.type) walk_type(*p.type);
                std::string ty = p.type ? render_type(*p.type) : (fn_ty ? type_text(e.ty->args[i]) : std::string());
                define(p.name, p.name_line, p.name_col, ty);
            }
            if (l.ret) walk_type(*l.ret);
            if (l.body) walk_expr(*l.body);
            scopes_.pop_back();
            break;
        }
        case svc::ExprKind::StructLit: {
            const auto& sl = static_cast<const svc::StructLit&>(e);
            if (sl.name_line != e.line || sl.name_col != e.col) {   // `Enum::Rec { .. }`: the head is the enum
                const std::string k = head_key(e.line, e.col, {}, sl.name);
                if (!k.empty()) name_occurrence(e.line, e.col, k, {});
            }
            if (decl(sl.name)) name_occurrence(sl.name_line ? sl.name_line : e.line, sl.name_line ? sl.name_col : e.col, sl.name, {});
            for (const svc::FieldInit& fi : sl.fields) {
                if (fi.value) { walk_expr(*fi.value); continue; }
                // Shorthand `P { x }` reads the local x; the parser keeps no position for it.
                Binding* b = lookup(fi.name);
                const auto [line, col] = find_shorthand(sl.name_line ? sl.name_line : e.line, sl.name_line ? sl.name_col : e.col, fi.name);
                if (!b || !line) continue;
                Occurrence o;
                o.line = line; o.col = col; o.local = b; o.shorthand = fi.name;
                occ_.push_back(std::move(o));
            }
            if (sl.base) walk_expr(*sl.base);
            break;
        }
        case svc::ExprKind::Tuple:   for (const auto& x : static_cast<const svc::TupleExpr&>(e).elems) walk_expr(*x); break;
        case svc::ExprKind::ListLit: for (const auto& x : static_cast<const svc::ListLit&>(e).elems) walk_expr(*x); break;
        case svc::ExprKind::MapLit:
            for (const auto& kv : static_cast<const svc::MapLit&>(e).entries) {
                if (kv.first) walk_expr(*kv.first);
                if (kv.second) walk_expr(*kv.second);
            }
            break;
        }
    }

    // ---- scopes --------------------------------------------------------------------

    void define(const std::string& name, uint32_t line, uint32_t col, std::string type, bool shorthand = false) {
        skip_mut(line, col);
        if (or_alt_) {   // a later or-pattern alternative re-binds what the first one declared
            for (Binding* b : scopes_.back())
                if (b->name == name) {
                    Occurrence o;
                    o.line = line; o.col = col; o.local = b; o.is_decl = true;
                    if (line) occ_.push_back(std::move(o));
                    return;
                }
        }
        store_.push_back(Binding{ name, line, col, std::move(type), shorthand });
        Binding* b = &store_.back();
        scopes_.back().push_back(b);
        Occurrence o;
        o.line = line; o.col = col; o.local = b; o.is_decl = true;
        if (shorthand) o.shorthand = name;
        if (line) occ_.push_back(std::move(o));
    }

    Binding* lookup(const std::string& name) {
        for (auto s = scopes_.rbegin(); s != scopes_.rend(); ++s)
            for (auto b = s->rbegin(); b != s->rend(); ++b)
                if ((*b)->name == name) return *b;
        return nullptr;
    }

    // A `mut x` pattern is anchored at `mut`; move the position onto the name.
    void skip_mut(uint32_t line, uint32_t& col) const {
        if (!line || !col) return;
        const std::string t = line_text(*text_, line);
        std::size_t i = col - 1;
        if (t.compare(i, 3, "mut") != 0 || (i + 3 < t.size() && is_ident_char(t[i + 3]))) return;
        i += 3;
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
        col = static_cast<uint32_t>(i) + 1;
    }

    // A struct shorthand field `name` in the braces after (line, col): a whole word preceded by
    // `{` or `,` and followed by `,` or `}` (a line break may stand for either). {0, 0} if absent.
    std::pair<uint32_t, uint32_t> find_shorthand(uint32_t line, uint32_t col, const std::string& name) const {
        if (!line) return { 0, 0 };
        for (uint32_t l = line; l < line + 64; ++l) {
            const std::string t = line_text(*text_, l);
            for (std::size_t i = l == line ? col : 0; (i = t.find(name, i)) != std::string::npos; ++i) {
                const std::size_t j = i + name.size();
                if ((i > 0 && is_ident_char(t[i - 1])) || (j < t.size() && is_ident_char(t[j]))) continue;
                std::size_t a = i;
                while (a > 0 && (t[a - 1] == ' ' || t[a - 1] == '\t')) --a;
                std::size_t b = j;
                while (b < t.size() && (t[b] == ' ' || t[b] == '\t')) ++b;
                const bool before = a == 0 || t[a - 1] == '{' || t[a - 1] == ',';
                const bool after  = b == t.size() || t[b] == ',' || t[b] == '}' || t.compare(b, 2, "//") == 0;
                if (before && after) return { l, static_cast<uint32_t>(i) + 1 };
            }
            if (l > line && t.find('}') != std::string::npos) break;
        }
        return { 0, 0 };
    }

    // The 1-based column of `word` as a whole word on `line` at or after `from`; 0 if absent.
    uint32_t find_word(uint32_t line, uint32_t from, const std::string& word) const {
        if (!line) return 0;
        const std::string t = line_text(*text_, line);
        for (std::size_t i = from ? from - 1 : 0; (i = t.find(word, i)) != std::string::npos; ++i) {
            const bool left  = i == 0 || !is_ident_char(t[i - 1]);
            const bool right = i + word.size() >= t.size() || !is_ident_char(t[i + word.size()]);
            if (left && right) return static_cast<uint32_t>(i) + 1;
        }
        return 0;
    }

    const AnalysisResult& a_;
    std::string path_;
    std::string module_;
    const std::string* text_ = nullptr;

    std::map<std::string, Decl> decls_;                  // mangled name -> declaration
    std::multimap<std::string, std::string> type_names_; // short type name -> mangled name
    std::map<std::string, std::string> trait_impl_;      // "Type::m" -> "Trait::m" (a trait impl's method)
    std::map<std::string, std::string> variant_enum_;    // variant key -> its enum's key
    std::deque<Binding> store_;                          // stable addresses for the occurrences
    std::vector<std::vector<Binding*>> scopes_;
    std::vector<std::string> type_params_;               // the generic parameters in scope
    bool or_alt_ = false;
    std::vector<Occurrence> occ_;
    std::vector<Unresolved> unresolved_;

    Pos probe_;                                         // {0, 0}: no probe
    std::map<Pos, Pos> brace_close_;                    // `{` -> `}` (only while probing)
    Probe::Kind hit_ = Probe::Kind::None;
    bool hit_qualified_ = false;
    svc::TyPtr receiver_;
    std::vector<const Binding*> exact_;
    std::vector<std::string> exact_type_params_;
    bool has_fallback_ = false;
    Pos fallback_open_{ 0, 0 };
    std::vector<const Binding*> fallback_;
    std::vector<std::string> fallback_type_params_;

    Pos call_pos_;                                      // {0, 0}: no call wanted
    const svc::CallExpr* call_ = nullptr;
    bool piped_ = false;
};

// ---- references ------------------------------------------------------------------------

struct Ref {
    std::string path;
    uint32_t    line = 0, col = 0;
    bool        is_decl = false;
    std::string shorthand;   // see Occurrence::shorthand
    bool operator<(const Ref& o) const { return std::tie(path, line, col) < std::tie(o.path, o.line, o.col); }
};

const AnalysisResult* first_covering(const std::vector<const AnalysisResult*>& analyses, const std::string& path) {
    for (const AnalysisResult* a : analyses)
        if (a && a->program && a->module_of.count(path)) return a;
    return nullptr;
}

// The target at `pos` and every reference to it (declaration included, marked). `index`
// is the queried file's Index; `target.local` points into it, so it is kept alive here.
struct Found {
    std::shared_ptr<const Index> index;
    Target target;
    std::vector<Ref> refs;
    std::vector<std::pair<std::string, Unresolved>> unresolved_same_name;   // (file, name) that COULD be further references
};

Found find_references(const std::vector<const AnalysisResult*>& analyses, const std::string& path, Position pos) {
    Found f;
    const AnalysisResult* a0 = first_covering(analyses, path);
    if (!a0) return f;
    const auto qi = std::make_shared<const Index>(*a0, path);
    f.index = qi;
    const Occurrence* at = qi->find(pos);
    if (!at) return f;
    f.target = qi->target_of(*at);
    if (f.target.kind == Target::Kind::None) return f;

    std::set<Ref> seen;
    auto add = [&](const std::string& p, const Occurrence& o) {
        Ref r{ p, o.line, o.col, o.is_decl, o.shorthand };
        if (seen.insert(r).second) f.refs.push_back(r);
    };
    if (f.target.kind == Target::Kind::Local) {
        for (const Occurrence* o : qi->matching(f.target)) add(path, *o);
    } else {
        for (const AnalysisResult* a : analyses) {
            if (!a || !a->program || !a->module_of.count(path)) continue;
            for (const auto& [file, module] : a->module_of) {
                const Index ix(*a, file);
                for (const Occurrence* o : ix.matching(f.target)) add(file, *o);
                for (const Unresolved& u : ix.unresolved())
                    if (u.name == f.target.name) {
                        f.unresolved_same_name.emplace_back(file, u);
                    }
            }
        }
        if (f.target.kind == Target::Kind::Field)
            if (const auto loc = qi->field_location(f.target.key, f.target.name)) {
                Ref r{ loc->path, static_cast<uint32_t>(loc->range.start.line) + 1,
                       static_cast<uint32_t>(loc->range.start.character) + 1, true, {} };
                if (seen.insert(r).second) f.refs.push_back(r);
            }
    }
    std::sort(f.refs.begin(), f.refs.end());
    return f;
}

Location ref_location(const std::vector<const AnalysisResult*>& analyses, const Ref& r) {
    for (const AnalysisResult* a : analyses)
        if (a) if (const auto it = a->texts.find(r.path); it != a->texts.end())
            return Location{ r.path, ident_range(it->second, r.line, r.col) };
    return Location{ r.path, ident_range({}, r.line, r.col) };
}

// ---- rename ---------------------------------------------------------------------------------

std::string where(const std::string& path, uint32_t line) {
    return path.substr(path.find_last_of('/') + 1) + ":" + std::to_string(line);
}

// Why the name at `pos` cannot be renamed; empty when it can.
std::string rename_refusal(const Occurrence* o, const Target& t) {
    if (!o || o->priority == LITERAL) return "There is nothing to rename here.";
    if (o->local && o->local->name == "self") return "`self` cannot be renamed.";
    if (t.kind == Target::Kind::Field) return "Renaming a field is not supported yet.";
    if (t.kind == Target::Kind::None) {
        if (!o->global.empty()) return "`" + svc::short_name(o->global) + "` is declared in the standard library.";
        if (!o->field_of.empty() || (o->expr && o->expr->kind == svc::ExprKind::Field))
            return "This name cannot be attributed to a declaration.";
        return "`" + o->label + "` is a builtin or declared in the standard library.";
    }
    return {};
}

// A new name must be ONE identifier token of the right case.
std::string name_problem(const std::string& old_name, const std::string& new_name, bool want_upper, bool either_case) {
    if (new_name == old_name) return "The new name is the same as the old one.";
    try {
        svc::Lexer lex(new_name);
        const auto toks = lex.tokenize();
        const bool one_ident = toks.size() == 2 && (toks[0].kind == svc::TokKind::LIdent || toks[0].kind == svc::TokKind::UIdent) &&
                               toks[0].text == new_name;
        if (!one_ident) return "`" + new_name + "` is not a valid name (or it is a keyword).";
    } catch (const std::exception&) {
        return "`" + new_name + "` is not a valid name.";
    }
    if (!either_case && is_upper_start(new_name) != want_upper)
        return want_upper ? "This name must start with an uppercase letter." : "This name must start with a lowercase letter.";
    return {};
}

// The text an edit writes for `r`: the new name, or `field: new` for a struct shorthand.
std::string edit_text(const Ref& r, const std::string& new_name) {
    return r.shorthand.empty() ? new_name : r.shorthand + ": " + new_name;
}

// Where the renamed name of `r` lands after the edits of its file: earlier edits on the same
// line shift it by their change in length, and in an expanded shorthand the name is the value.
uint32_t shifted_col(const std::vector<Ref>& refs, const Ref& r, const std::string& old_name, const std::string& new_name) {
    int shift = 0;
    for (const Ref& e : refs)
        if (e.path == r.path && e.line == r.line && e.col < r.col)
            shift += static_cast<int>(edit_text(e, new_name).size()) - static_cast<int>(old_name.size());
    if (!r.shorthand.empty()) shift += static_cast<int>(r.shorthand.size()) + 2;
    return static_cast<uint32_t>(static_cast<int>(r.col) + shift);
}

} // namespace

std::optional<std::string> hover(const AnalysisResult& analysis, const std::string& path, Position pos) {
    return Index(analysis, path).hover(pos);
}

std::optional<Location> definition(const AnalysisResult& analysis, const std::string& path, Position pos) {
    return Index(analysis, path).definition(pos);
}

Probe probe(const AnalysisResult& analysis, const std::string& path, uint32_t line, uint32_t col) {
    if (line == 0) return {};
    return Index(analysis, path, line, col).probe_result();
}

CallSite call_at(const AnalysisResult& analysis, const std::string& path, uint32_t line, uint32_t col) {
    if (line == 0) return {};
    return Index(analysis, path, 0, 0, line, col).call_site();
}

std::vector<Location> references(const std::vector<const AnalysisResult*>& analyses, const std::string& path,
                                 Position pos, bool include_declaration) {
    std::vector<Location> out;
    const Found f = find_references(analyses, path, pos);
    for (const Ref& r : f.refs)
        if (include_declaration || !r.is_decl) out.push_back(ref_location(analyses, r));
    return out;
}

std::vector<Highlight> highlights(const AnalysisResult& analysis, const std::string& path, Position pos) {
    std::vector<Highlight> out;
    const Index ix(analysis, path);
    const Occurrence* o = ix.find(pos);
    if (!o) return out;
    const Target t = ix.target_of(*o);
    if (t.kind == Target::Kind::None) return out;
    for (const Occurrence* m : ix.matching(t))
        out.push_back(Highlight{ ident_range(*ix.text(), m->line, m->col), m->is_decl || m->is_write });
    return out;
}

PrepareRename prepare_rename(const std::vector<const AnalysisResult*>& analyses, const std::string& path, Position pos) {
    PrepareRename p;
    const AnalysisResult* a0 = first_covering(analyses, path);
    if (!a0) { p.error = "The file is not part of a checked program."; return p; }
    // Text dropped around a syntax error is invisible to the index: an occurrence there would
    // silently keep the old name.
    for (const AnalysisResult* a : analyses)
        if (a && a->module_of.count(path) && !a->syntax_error_paths.empty()) {
            const std::string& bad = *a->syntax_error_paths.begin();
            p.error = "Fix the syntax errors in " + bad.substr(bad.find_last_of('/') + 1) + " first.";
            return p;
        }
    const Index qi(*a0, path);
    const Occurrence* o = qi.find(pos);
    const Target t = o ? qi.target_of(*o) : Target{};
    p.error = rename_refusal(o, t);
    if (!p.error.empty()) return p;
    p.range = ident_range(*qi.text(), o->line, o->col);
    p.placeholder = t.name;
    return p;
}

RenameResult rename(const std::vector<const AnalysisResult*>& analyses, const std::string& path, Position pos,
                    const std::string& new_name) {
    RenameResult res;
    const PrepareRename prep = prepare_rename(analyses, path, pos);
    if (!prep.error.empty()) { res.error = prep.error; return res; }

    const Found f = find_references(analyses, path, pos);
    const std::string& old_name = f.target.name;

    // The case the declaration's kind requires.
    bool want_upper = false, either = false;
    if (f.target.kind == Target::Kind::Global) {
        const AnalysisResult* a0 = first_covering(analyses, path);
        const Index qi(*a0, path);
        const Decl* d = qi.decl(f.target.key);
        switch (d ? d->kind : DeclKind::Fn) {
        case DeclKind::Struct: case DeclKind::Enum: case DeclKind::Variant: case DeclKind::Trait: want_upper = true; break;
        case DeclKind::Const: either = true; break;
        case DeclKind::Fn: case DeclKind::Method: break;
        }
    }
    if (std::string problem = name_problem(old_name, new_name, want_upper, either); !problem.empty()) {
        res.error = problem;
        return res;
    }
    // A name nothing could be attributed to might be one more reference the rename would miss:
    // a bare call `m(x)` only for a trait method (nothing else is callable without a receiver
    // and unmangled), `recv.m()` on an unknown receiver for any method, an undecided annotation
    // name for a type.
    if (f.target.kind == Target::Kind::Global) {
        const AnalysisResult* a0 = first_covering(analyses, path);
        const Index qi(*a0, path);
        const Decl* d = qi.decl(f.target.key);
        const std::string head = f.target.key.substr(0, f.target.key.rfind("::"));
        const Decl* owner = qi.decl(head);
        const bool trait_method = d && d->kind == DeclKind::Method && owner && owner->kind == DeclKind::Trait;
        const bool method = d && d->kind == DeclKind::Method;
        const bool type = d && (d->kind == DeclKind::Struct || d->kind == DeclKind::Enum || d->kind == DeclKind::Trait);
        for (const auto& [upath, u] : f.unresolved_same_name) {
            const bool blocks = (u.kind == Unresolved::Kind::Bare && trait_method) ||
                                (u.kind == Unresolved::Kind::Member && method) ||
                                (u.kind == Unresolved::Kind::Type && type);
            if (!blocks) continue;
            res.error = "`" + u.name + "` at " + where(upath, u.line) +
                        " cannot be attributed to a declaration, so the rename could miss it.";
            return res;
        }
    }

    // The edits, each checked against the text it replaces.
    std::map<std::string, std::string> edited;   // path -> text after the edits
    for (const Ref& r : f.refs) {
        const Location loc = ref_location(analyses, r);
        std::string text;
        for (const AnalysisResult* a : analyses)
            if (a) if (const auto it = a->texts.find(r.path); it != a->texts.end()) { text = it->second; break; }
        const std::string line = line_text(text, r.line);
        if (line.compare(r.col - 1, old_name.size(), old_name) != 0) {
            res.error = "Internal error: unexpected text at " + where(r.path, r.line) + "; nothing was renamed.";
            return res;
        }
        res.changes[r.path].push_back(TextEdit{ Range{ loc.range.start, Position{ loc.range.start.line,
                                                   loc.range.start.character + static_cast<int>(old_name.size()) } },
                                                edit_text(r, new_name) });
        if (!edited.count(r.path)) edited[r.path] = text;
    }
    // Apply per file, last edit first, so earlier offsets stay valid.
    for (auto& [p, text] : edited) {
        std::vector<TextEdit> es = res.changes[p];
        std::sort(es.begin(), es.end(), [](const TextEdit& x, const TextEdit& y) {
            return std::tie(x.range.start.line, x.range.start.character) > std::tie(y.range.start.line, y.range.start.character);
        });
        for (const TextEdit& e : es) {
            std::size_t off = 0;
            for (int l = 0; l < e.range.start.line; ++l) off = text.find('\n', off) + 1;
            off += static_cast<std::size_t>(e.range.start.character);
            text.replace(off, old_name.size(), e.new_text);
        }
    }

    // Verify: re-check every affected program with the edits applied.
    const Ref* decl_ref = nullptr;
    for (const Ref& r : f.refs) if (r.is_decl) { decl_ref = &r; break; }
    if (!decl_ref) { res.error = "Internal error: the declaration was not found; nothing was renamed."; return res; }
    std::set<Ref> expected;
    for (const Ref& r : f.refs) expected.insert(Ref{ r.path, r.line, shifted_col(f.refs, r, old_name, new_name), false, {} });
    const Ref new_decl{ decl_ref->path, decl_ref->line, shifted_col(f.refs, *decl_ref, old_name, new_name), true, {} };

    for (const AnalysisResult* a : analyses) {
        if (!a || !a->program || !a->module_of.count(path)) continue;
        const auto eit = a->path_of.find(svc::ENTRY_MODULE_PREFIX);
        if (eit == a->path_of.end()) continue;
        Overlay texts = a->texts;
        for (const auto& [p, t] : edited) if (texts.count(p)) texts[p] = t;
        const AnalysisResult after = analyze(eit->second, texts[eit->second], texts);

        // (a) no new error
        std::multiset<std::string> before_errs, after_errs;
        for (const auto& [p, ds] : a->by_path)
            for (const Diagnostic& d : ds) if (d.severity == Severity::Error) before_errs.insert(p + "\n" + d.message);
        for (const auto& [p, ds] : after.by_path)
            for (const Diagnostic& d : ds)
                if (d.severity == Severity::Error && !before_errs.count(p + "\n" + d.message)) {
                    res.error = "Renaming to `" + new_name + "` would cause an error at " +
                                where(p, static_cast<uint32_t>(d.range.start.line) + 1) + ": " + d.message;
                    res.changes.clear();
                    return res;
                }
        // (b) exactly the edited names now refer to the renamed declaration
        if (!after.module_of.count(new_decl.path)) continue;
        const std::vector<const AnalysisResult*> one{ &after };
        const Found g = find_references(one, new_decl.path,
                                        Position{ static_cast<int>(new_decl.line) - 1, static_cast<int>(new_decl.col) - 1 });
        std::set<Ref> got;
        for (const Ref& r : g.refs) got.insert(Ref{ r.path, r.line, r.col, false, {} });
        for (const Ref& r : got)
            if (!expected.count(r)) {
                res.error = "Renaming to `" + new_name + "` would capture the name at " + where(r.path, r.line) +
                            ", which refers to something else today.";
                res.changes.clear();
                return res;
            }
        for (const Ref& r : expected)
            if (after.module_of.count(r.path) && !got.count(r)) {
                res.error = "After renaming to `" + new_name + "`, the name at " + where(r.path, r.line) +
                            " would refer to something else.";
                res.changes.clear();
                return res;
            }
    }
    return res;
}

} // namespace lsp
