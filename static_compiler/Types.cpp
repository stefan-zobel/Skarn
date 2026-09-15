// =============================================================================
// Types.cpp -- constructors, structural equality, and rendering for `svc::Ty`.
//
// The primitives are shared singletons (immutable, so aliasing is safe); compound
// types are freshly allocated. `equals` is a plain structural comparison (variables
// by id) that ignores any substitution -- the TypeContext (Solver.cpp) applies the
// solution before calling it where "equal up to the current solution" is wanted.
// =============================================================================

#include "Types.h"
#include "Naming.h"

namespace svc {

// ----- primitive singletons -------------------------------------------------

static TyPtr prim(TyKind k) {
    auto t = std::make_shared<Ty>();
    t->kind = k;
    return t;
}

TyPtr ty_unit()   { static const TyPtr t = prim(TyKind::Unit);   return t; }
TyPtr ty_int()    { static const TyPtr t = prim(TyKind::Int);    return t; }
TyPtr ty_double() { static const TyPtr t = prim(TyKind::Double); return t; }
TyPtr ty_bool()   { static const TyPtr t = prim(TyKind::Bool);   return t; }
TyPtr ty_string() { static const TyPtr t = prim(TyKind::String); return t; }
TyPtr ty_never()  { static const TyPtr t = prim(TyKind::Never);  return t; }
TyPtr ty_error()  { static const TyPtr t = prim(TyKind::Error);  return t; }

// ----- compound constructors ------------------------------------------------

TyPtr make_named(std::string name, std::vector<TyPtr> args) {
    auto t = std::make_shared<Ty>();
    t->kind = TyKind::Named;
    t->name = std::move(name);
    t->args = std::move(args);
    return t;
}

TyPtr make_fn(std::vector<TyPtr> params, TyPtr ret) {
    auto t = std::make_shared<Ty>();
    t->kind = TyKind::Fn;
    t->args = std::move(params);
    t->ret = std::move(ret);
    return t;
}

TyPtr make_tuple(std::vector<TyPtr> elems) {
    auto t = std::make_shared<Ty>();
    t->kind = TyKind::Tuple;
    t->args = std::move(elems);
    return t;
}

TyPtr make_dyn(std::string trait, std::vector<TyPtr> args) {
    auto t = std::make_shared<Ty>();
    t->kind = TyKind::Dyn;
    t->name = std::move(trait);
    t->args = std::move(args);
    return t;
}

// ----- variable occurrence --------------------------------------------------

bool mentions_var(const TyPtr& t, uint32_t var_id) {
    if (!t) return false;
    switch (t->kind) {
        case TyKind::Var:
            return t->var_id == var_id;
        case TyKind::Named:
        case TyKind::Tuple:
        case TyKind::Dyn:
            for (const auto& a : t->args)
                if (mentions_var(a, var_id)) return true;
            return false;
        case TyKind::Fn:
            for (const auto& a : t->args)
                if (mentions_var(a, var_id)) return true;
            return mentions_var(t->ret, var_id);
        default:
            return false;   // primitives / Unit / Never / Error / Unresolved carry no vars
    }
}

// ----- structural equality --------------------------------------------------

bool equals(const Ty& a, const Ty& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case TyKind::Unresolved:
        case TyKind::Unit:
        case TyKind::Int:
        case TyKind::Double:
        case TyKind::Bool:
        case TyKind::String:
        case TyKind::Never:
        case TyKind::Error:
            return true;
        case TyKind::Var:
            return a.var_id == b.var_id;
        case TyKind::Named:
        case TyKind::Dyn:                  // same trait + same trait args
            if (a.name != b.name || a.args.size() != b.args.size()) return false;
            for (size_t i = 0; i < a.args.size(); ++i)
                if (!equals(a.args[i], b.args[i])) return false;
            return true;
        case TyKind::Fn:
            if (a.args.size() != b.args.size()) return false;
            for (size_t i = 0; i < a.args.size(); ++i)
                if (!equals(a.args[i], b.args[i])) return false;
            return equals(a.ret, b.ret);
        case TyKind::Tuple:
            if (a.args.size() != b.args.size()) return false;
            for (size_t i = 0; i < a.args.size(); ++i)
                if (!equals(a.args[i], b.args[i])) return false;
            return true;
    }
    return false;
}

bool equals(const TyPtr& a, const TyPtr& b) {
    if (a == b) return true;         // same pointer (incl. both null)
    if (!a || !b) return false;
    return equals(*a, *b);
}

// ----- diagnostics ----------------------------------------------------------

std::string describe(const Ty& t) {
    switch (t.kind) {
        case TyKind::Unresolved: return "?";
        case TyKind::Unit:       return "()";
        case TyKind::Int:        return "Int";
        case TyKind::Double:     return "Double";
        case TyKind::Bool:       return "Bool";
        case TyKind::String:     return "String";
        case TyKind::Never:      return "!";
        case TyKind::Error:      return "<error>";
        case TyKind::Var:
            // A rigid var carries the user's own parameter name (`T`) -- keep it.
            // A flexible (fresh) var is an internal solver unknown: render an anonymous
            // placeholder, NEVER the raw var_id. The id is a global counter (`next_id_++`),
            // so leaking it makes messages meaningless AND unstable across compiles.
            // (Two distinct fresh vars both render `_`; acceptable -- the number was
            // meaningless anyway, and per-message a/b/c naming belongs to a real
            // structured-diagnostics layer, not here.)
            return t.name.empty() ? "_" : t.name;
        case TyKind::Named: {
            // Codegen-internal fiction names (`$List`/`$TupleN`/`$impl...`) never reach
            // here: the checker uses plain make_named("List", ...) and a distinct
            // TyKind::Tuple, so `t.name` is always a real surface type name. A MODULE path is
            // kept (`util::Point` is what the reader wrote); only the entry program's internal
            // prefix is stripped, so a message says `Money`, not `$entry::Money`.
            std::string s = display_name(t.name);
            if (!t.args.empty()) {
                s += '[';
                for (size_t i = 0; i < t.args.size(); ++i) {
                    if (i) s += ", ";
                    s += describe(t.args[i]);
                }
                s += ']';
            }
            return s;
        }
        case TyKind::Fn: {
            std::string s = "fn(";
            for (size_t i = 0; i < t.args.size(); ++i) {
                if (i) s += ", ";
                s += describe(t.args[i]);
            }
            s += ") -> ";
            s += describe(t.ret);
            return s;
        }
        case TyKind::Tuple: {
            std::string s = "(";
            for (size_t i = 0; i < t.args.size(); ++i) {
                if (i) s += ", ";
                s += describe(t.args[i]);
            }
            s += ')';
            return s;
        }
        case TyKind::Dyn: {
            std::string s = "dyn " + display_name(t.name);
            if (!t.args.empty()) {
                s += '[';
                for (size_t i = 0; i < t.args.size(); ++i) {
                    if (i) s += ", ";
                    s += describe(t.args[i]);
                }
                s += ']';
            }
            return s;
        }
    }
    return "?";
}

std::string describe(const TyPtr& t) {
    return t ? describe(*t) : "?";
}

// ----- TypeRenderer (per-message stable variable naming) --------------------

std::string TypeRenderer::name_for(const Ty& v) {
    auto it = names_.find(v.var_id);
    if (it != names_.end()) return it->second; // same var_id -> same token, both sides

    std::string token;
    if (v.name.empty()) {
        // Anonymous flexible var: a, b, ..., z, a1, b1, ... -- stable within the message.
        const uint32_t i = next_++;
        token.assign(1, static_cast<char>('a' + (i % 26)));
        if (i >= 26) token += std::to_string(i / 26);
    } else {
        // Rigid / named flexible var: keep the user's name, but disambiguate a genuine
        // collision -- two DISTINCT vars (different var_id) that share a name. The first
        // renders `T`, the next `T#2`, then `T#3`, ... (the `#` cannot occur in a real
        // type name, so the artifact is unmistakable).
        const uint32_t n = used_names_[v.name]++;
        token = n == 0 ? v.name : v.name + "#" + std::to_string(n + 1);
    }
    names_.emplace(v.var_id, token);
    return token;
}

std::string TypeRenderer::operator()(const Ty& t) {
    switch (t.kind) {
        case TyKind::Unresolved: return "?";
        case TyKind::Unit:       return "()";
        case TyKind::Int:        return "Int";
        case TyKind::Double:     return "Double";
        case TyKind::Bool:       return "Bool";
        case TyKind::String:     return "String";
        case TyKind::Never:      return "!";
        case TyKind::Error:      return "<error>";
        case TyKind::Var:
            // A truly anonymous flexible var gets a stable per-message letter instead of
            // `_`; a rigid / named var keeps its user name, disambiguated on a collision.
            // Both are cached by var_id, so the same var reads the same token everywhere.
            return name_for(t);
        case TyKind::Named: {
            std::string s = display_name(t.name);   // hide the entry program's internal prefix
            if (!t.args.empty()) {
                s += '[';
                for (size_t i = 0; i < t.args.size(); ++i) {
                    if (i) s += ", ";
                    s += (*this)(t.args[i]);
                }
                s += ']';
            }
            return s;
        }
        case TyKind::Fn: {
            std::string s = "fn(";
            for (size_t i = 0; i < t.args.size(); ++i) {
                if (i) s += ", ";
                s += (*this)(t.args[i]);
            }
            s += ") -> ";
            s += (*this)(t.ret);
            return s;
        }
        case TyKind::Tuple: {
            std::string s = "(";
            for (size_t i = 0; i < t.args.size(); ++i) {
                if (i) s += ", ";
                s += (*this)(t.args[i]);
            }
            s += ')';
            return s;
        }
        case TyKind::Dyn: {
            std::string s = "dyn " + display_name(t.name);
            if (!t.args.empty()) {
                s += '[';
                for (size_t i = 0; i < t.args.size(); ++i) {
                    if (i) s += ", ";
                    s += (*this)(t.args[i]);
                }
                s += ']';
            }
            return s;
        }
    }
    return "?";
}

std::string TypeRenderer::operator()(const TyPtr& t) {
    return t ? (*this)(*t) : "?";
}

} // namespace svc
