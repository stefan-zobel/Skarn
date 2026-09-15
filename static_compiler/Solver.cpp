// =============================================================================
// Solver.cpp -- the TypeContext implementation (union-find over flexible vars).
//
// `apply` follows bound flexible variables and rebuilds compound types, so it
// returns a type fully resolved under the current solution. `unify` is standard
// first-order unification with an occurs-check; rigid variables and mismatched
// heads fail. `subsumes`/`join` add the single `Int <: Double` subtyping edge.
// =============================================================================

#include "Solver.h"

namespace svc {

// ----- variable supply ------------------------------------------------------

TyPtr TypeContext::fresh_var(std::string name, std::vector<Bound> bounds) {
    auto t = std::make_shared<Ty>();
    t->kind = TyKind::Var;
    t->var_id = next_id_++;
    t->rigid = false;
    t->name = std::move(name);
    t->bounds = std::move(bounds);
    if (subst_.size() < next_id_) subst_.resize(next_id_);
    return t;
}

TyPtr TypeContext::rigid_var(std::string name, std::vector<Bound> bounds) {
    auto t = std::make_shared<Ty>();
    t->kind = TyKind::Var;
    t->var_id = next_id_++;
    t->rigid = true;
    t->name = std::move(name);
    t->bounds = std::move(bounds);
    if (subst_.size() < next_id_) subst_.resize(next_id_);
    return t;
}

void TypeContext::set_var_bounds(const TyPtr& v, std::vector<Bound> bounds) {
    // `v` was just handed out by rigid_var (created non-const via make_shared, exposed as
    // TyPtr = shared_ptr<const Ty>). Attaching bounds before any consumer observes them is
    // safe; see the header note. This preserves single object identity in make_generics.
    const_cast<Ty*>(v.get())->bounds = std::move(bounds);
}

// ----- apply (resolve through the substitution) -----------------------------

TyPtr TypeContext::apply(const TyPtr& t) const {
    if (!t) return t;
    switch (t->kind) {
        case TyKind::Var:
            if (!t->rigid && bound(t->var_id)) {
                // Path compression (union-find): resolve once, store the resolved form back, so a
                // var recurring in a compound is not re-expanded at every occurrence (which is
                // exponential -- see the subst_ note in Solver.h). Sound: the stored form is equal
                // to subst_[id] under the current solution, and still holds any not-yet-bound vars
                // (they resolve on a later apply), so a subsequent unify sees the same type.
                TyPtr r = apply(subst_[t->var_id]);
                subst_[t->var_id] = r;
                return r;
            }
            return t;
        case TyKind::Named: {
            std::vector<TyPtr> args;
            args.reserve(t->args.size());
            for (const auto& a : t->args) args.push_back(apply(a));
            return make_named(t->name, std::move(args));
        }
        case TyKind::Fn: {
            std::vector<TyPtr> ps;
            ps.reserve(t->args.size());
            for (const auto& a : t->args) ps.push_back(apply(a));
            return make_fn(std::move(ps), apply(t->ret));
        }
        case TyKind::Tuple: {
            std::vector<TyPtr> es;
            es.reserve(t->args.size());
            for (const auto& a : t->args) es.push_back(apply(a));
            return make_tuple(std::move(es));
        }
        case TyKind::Dyn: {
            // A parametric trait object (`dyn Iterable[T]`) carries type args that must be
            // resolved through the substitution like any other. Falling into `default` here
            // would treat it as an atomic leaf -- silently, since `default` swallows it.
            std::vector<TyPtr> as;
            as.reserve(t->args.size());
            for (const auto& a : t->args) as.push_back(apply(a));
            return make_dyn(t->name, std::move(as));
        }
        default:
            return t;   // primitives / Unit / Never / Error / Unresolved
    }
}

// ----- occurs-check ---------------------------------------------------------

bool TypeContext::occurs(uint32_t id, const TyPtr& t) const {
    TyPtr a = apply(t);
    if (!a) return false;
    switch (a->kind) {
        case TyKind::Var:
            return !a->rigid && a->var_id == id;
        case TyKind::Named:
        case TyKind::Tuple:
        case TyKind::Dyn:      // `dyn Iterable[T]` -- T must be occurs-checked like any arg
            for (const auto& x : a->args) if (occurs(id, x)) return true;
            return false;
        case TyKind::Fn:
            for (const auto& x : a->args) if (occurs(id, x)) return true;
            return occurs(id, a->ret);
        default:
            return false;
    }
}

// ----- unify ----------------------------------------------------------------

bool TypeContext::unify(const TyPtr& a0, const TyPtr& b0) {
    TyPtr a = apply(a0), b = apply(b0);
    if (!a || !b) return false;

    // Same variable (either flavour).
    if (a->kind == TyKind::Var && b->kind == TyKind::Var && a->var_id == b->var_id)
        return true;

    // Bind a flexible variable to the other side.
    if (a->kind == TyKind::Var && !a->rigid) {
        if (occurs(a->var_id, b)) return false;
        subst_[a->var_id] = b;
        return true;
    }
    if (b->kind == TyKind::Var && !b->rigid) {
        if (occurs(b->var_id, a)) return false;
        subst_[b->var_id] = a;
        return true;
    }

    // Error is consistent with everything (poison already reported upstream).
    if (a->kind == TyKind::Error || b->kind == TyKind::Error) return true;

    // A rigid variable only matches itself (handled above) -- anything else fails.
    if (a->kind == TyKind::Var || b->kind == TyKind::Var) return false;

    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case TyKind::Unit:
        case TyKind::Int:
        case TyKind::Double:
        case TyKind::Bool:
        case TyKind::String:
        case TyKind::Never:
        case TyKind::Unresolved:
            return true;
        case TyKind::Named:
        case TyKind::Dyn:      // same trait name + pairwise-unifiable trait args
            if (a->name != b->name || a->args.size() != b->args.size()) return false;
            for (size_t i = 0; i < a->args.size(); ++i)
                if (!unify(a->args[i], b->args[i])) return false;
            return true;
        case TyKind::Fn:
            if (a->args.size() != b->args.size()) return false;
            for (size_t i = 0; i < a->args.size(); ++i)
                if (!unify(a->args[i], b->args[i])) return false;
            return unify(a->ret, b->ret);
        case TyKind::Tuple:
            if (a->args.size() != b->args.size()) return false;
            for (size_t i = 0; i < a->args.size(); ++i)
                if (!unify(a->args[i], b->args[i])) return false;
            return true;
        case TyKind::Var:   // unreachable (handled above)
            return false;
    }
    return false;
}

// ----- subsumes (sub acceptable where sup expected) -------------------------

bool TypeContext::subsumes(const TyPtr& sub0, const TyPtr& sup0) {
    TyPtr sub = apply(sub0), sup = apply(sup0);
    if (!sub || !sup) return false;
    if (sub->kind == TyKind::Never) return true;                 // bottom
    if (sub->kind == TyKind::Error || sup->kind == TyKind::Error) return true;
    if (sub->kind == TyKind::Int && sup->kind == TyKind::Double) return true;  // widening
    // A trait object accepts any implementor -- the second (and last) leaf edge. Two `dyn`s
    // relate by equality: v1 has no upcast `dyn Sub -> dyn Super` (the value would be
    // unchanged, so it is free to add later. Deliberately NOT
    // reached through a constructor: `unify`'s Named/Fn/Tuple recursion never calls back
    // here, which is what keeps `Vec[Int] <: Vec[dyn Show]` false.
    if (sup->kind == TyKind::Dyn && sub->kind != TyKind::Dyn)
        return impl_oracle_ && impl_oracle_(sub, sup->name, sup->args);
    return unify(sub, sup);                                      // else equality
}

// ----- join (least upper bound for branch/arm merge) ------------------------

TyPtr TypeContext::join(const TyPtr& a0, const TyPtr& b0) {
    TyPtr a = apply(a0), b = apply(b0);
    if (!a) return b;
    if (!b) return a;
    if (a->kind == TyKind::Never) return b;
    if (b->kind == TyKind::Never) return a;
    if (a->kind == TyKind::Error) return b;
    if (b->kind == TyKind::Error) return a;
    if (equals(a, b)) return a;
    // NOTE: join does NOT widen {Int,Double} -> Double. Numeric widening is a
    // check-BOUNDARY coercion only (ratified N1/N3): a mixed numeric merge in
    // inference position -- a container literal `[1, 2.0]`, an inferred branch --
    // is a type error, so join is exact and the caller reports the incompatibility.
    if (unify(a, b)) return apply(a);
    return ty_error();
}

// ----- substitution / instantiation -----------------------------------------

TyPtr TypeContext::substitute(const TyPtr& t, const std::unordered_map<uint32_t, TyPtr>& m) const {
    if (!t) return t;
    switch (t->kind) {
        case TyKind::Var: {
            auto it = m.find(t->var_id);
            return it != m.end() ? it->second : t;
        }
        case TyKind::Named: {
            std::vector<TyPtr> args;
            args.reserve(t->args.size());
            for (const auto& a : t->args) args.push_back(substitute(a, m));
            return make_named(t->name, std::move(args));
        }
        case TyKind::Fn: {
            std::vector<TyPtr> ps;
            ps.reserve(t->args.size());
            for (const auto& a : t->args) ps.push_back(substitute(a, m));
            return make_fn(std::move(ps), substitute(t->ret, m));
        }
        case TyKind::Tuple: {
            std::vector<TyPtr> es;
            es.reserve(t->args.size());
            for (const auto& a : t->args) es.push_back(substitute(a, m));
            return make_tuple(std::move(es));
        }
        case TyKind::Dyn: {
            // `dyn Iterable[T]` inside a generic signature: T must be instantiated at the
            // call site like any other arg (the `default` arm below would silently skip it).
            std::vector<TyPtr> as;
            as.reserve(t->args.size());
            for (const auto& a : t->args) as.push_back(substitute(a, m));
            return make_dyn(t->name, std::move(as));
        }
        default:
            return t;
    }
}

TyPtr TypeContext::instantiate(const TyPtr& body, const std::vector<uint32_t>& rigid_ids) {
    std::unordered_map<uint32_t, TyPtr> m;
    m.reserve(rigid_ids.size());
    for (uint32_t id : rigid_ids) m[id] = fresh_var();
    return substitute(body, m);
}

} // namespace svc
