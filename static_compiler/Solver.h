#pragma once

// =============================================================================
// Solver.h -- the `svc::TypeContext`: the pure type-inference engine.
//
// It owns the type-variable supply and the union-find substitution that binds
// FLEXIBLE inference variables. It is deliberately AST-free -- it speaks only
// `Ty` (Types.h) -- so it can be unit-tested in isolation and reused by every
// part of the checker (Check.cpp).
//
// Two flavours of type variable:
//   * flexible (`fresh_var`)  -- an unknown to be solved; unify may bind it.
//   * rigid    (`rigid_var`)  -- a generic parameter being checked (a skolem);
//                                it unifies only with itself. This is what makes
//                                bidirectional-local checking sound: inside
//                                `fn id[T](x: T) -> T`, `T` is rigid, so the body
//                                cannot secretly assume `T = Int`.
//
// The language has exactly TWO subtyping edges, both allowed by `subsumes` at a check
// boundary ONLY; everywhere else relations are equality (`unify`), and `join` is exact
// (it widens NOTHING -- see its comment below):
//   * `Int <: Double`          -- numeric widening (owner decision).
//   * `Concrete <: dyn Trait`  -- a trait object accepts any implementor. Decided by the
//                                 IMPL ORACLE below, since only the checker knows the impls.
// Both are LEAF edges: `unify`'s Named/Fn/Tuple recursion does not consult `subsumes`, so
// neither propagates through a constructor. That is deliberate -- it is what keeps
// containers invariant (`Vec[Int]` is NOT a `Vec[Double]` nor a `Vec[dyn Show]`, which
// would be unsound through a mutable alias). Element-wise coercion happens in the checker's
// structural `check` rules, where it is per-element and can be recorded.
// =============================================================================

#include "Types.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace svc {

// "Does `ty` implement `trait` (with these trait args)?" -- the one question `subsumes` must
// ask to decide `Concrete <: dyn Trait`, and the one thing the impl registry knows but this
// AST-free engine does not. The checker installs it (see Check.cpp's `dyn_coercible`); an
// unset oracle answers no, so a bare TypeContext stays self-contained and testable.
//
// It MUST be a pure predicate: `subsumes` is called speculatively (callers decide whether to
// report from its result), so the oracle may not emit diagnostics.
using ImplOracle = std::function<bool(const TyPtr& ty, const std::string& trait,
                                      const std::vector<TyPtr>& args)>;

class TypeContext {
public:
    void set_impl_oracle(ImplOracle o) { impl_oracle_ = std::move(o); }

    // Fresh FLEXIBLE inference variable (bindable by unify).
    TyPtr fresh_var(std::string name = "", std::vector<Bound> bounds = {});
    // Fresh RIGID variable (a skolem generic parameter; unifies only with itself).
    TyPtr rigid_var(std::string name, std::vector<Bound> bounds = {});
    // Attach bounds to a FRESHLY-created rigid var, in place. Valid ONLY before any
    // consumer reads the var's bounds -- used by make_generics's two-sub-pass (create
    // all sibling vars first, then resolve+attach bounds, so a bound may reference a
    // later sibling type parameter). Keeps single object identity (no var_id juggling).
    void set_var_bounds(const TyPtr& v, std::vector<Bound> bounds);

    // Resolve `t` through the substitution (follow bound flexible vars), rebuilding
    // compound types so the result is fully applied under the current solution.
    TyPtr apply(const TyPtr& t) const;

    // Impose equality a = b. Returns false on conflict (the caller reports).
    bool unify(const TyPtr& a, const TyPtr& b);

    // Is `sub` acceptable where `sup` is expected? Equality, plus the two leaf edges
    // (`Int <: Double`; `Concrete <: dyn Trait` via the impl oracle), plus Never/Error
    // wildcards, plus flexible-var binding. Compound widening is NOT done here (the
    // checker's structural `check` rules do it element-wise, where a coercion can be
    // recorded) -- see the file header.
    bool subsumes(const TyPtr& sub, const TyPtr& sup);

    // Least-upper-bound for branch/arm merge. Never -> the other; Error -> the
    // other (no cascade); equal -> that; else unify or, on genuine incompatibility,
    // `ty_error()` (which the caller reports). It does NOT widen {Int,Double} --
    // numeric widening is a check-boundary coercion (subsumes), not an inference
    // merge (ratified N1/N3: a mixed numeric container literal is a type error).
    TyPtr join(const TyPtr& a, const TyPtr& b);

    // Structurally replace rigid variables (keyed by id) -- used to instantiate a
    // generic signature or to substitute `Self` / concrete type arguments.
    TyPtr substitute(const TyPtr& t, const std::unordered_map<uint32_t, TyPtr>& m) const;

    // Instantiate a scheme: map each named rigid id to a fresh flexible var, then
    // substitute. Returns the instantiated body (used at generic call sites).
    TyPtr instantiate(const TyPtr& body, const std::vector<uint32_t>& rigid_ids);

private:
    uint32_t next_id_ = 1;
    // subst_[id] = binding for flexible var `id` (or null). `mutable` so `apply` (a logically
    // const query) can PATH-COMPRESS: when it follows a bound var it stores the resolved form
    // back, so a var is fully expanded at most once instead of re-expanded at every occurrence.
    // Without this a solution where one var recurs inside a compound (e.g. a `(K, V)` tuple
    // chained through impl resolution) makes `apply` rebuild an EXPONENTIAL tree of `Ty` nodes
    // -- a checker hang, not a real cycle (the occurs-check keeps subst_ acyclic). Semantically
    // neutral: the stored form is equal to the original, resolved under the same solution.
    mutable std::vector<TyPtr> subst_;
    ImplOracle impl_oracle_;     // unset => `Concrete <: dyn Trait` never holds

    bool bound(uint32_t id) const { return id < subst_.size() && subst_[id] != nullptr; }
    bool occurs(uint32_t id, const TyPtr& t) const;
};

} // namespace svc
