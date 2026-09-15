#pragma once

// =============================================================================
// Types.h -- the SEMANTIC type representation `svc::Ty` for Skarn.
//
// This is the canonical, resolved type the TYPECHECKER produces -- deliberately
// distinct from the syntactic `Type` AST in Ast.h (which is only what the
// programmer *wrote*). The two differ: syntactic `NamedType{"T"}` might resolve
// to a concrete type, a type variable, or an alias; the checker's job is to map
// syntax -> `Ty`.
//
// `Ty` is IMMUTABLE and shared (`TyPtr = shared_ptr<const Ty>`), so it can sit in
// the AST's per-expression slot (`Expr::ty`) and be cached freely. Type variables
// carry a unique id but NO mutable binding -- unification's union-find lives in the
// TypeContext (Solver.h), not on the node. Types are ERASED at runtime (the Gleam
// model): nothing here reaches the VM.
//
// Namespace `svc` (Skarn compiler).
// =============================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace svc {

enum class TyKind : uint8_t {
    Unresolved,   // not yet checked (the parser-time default)
    Unit,         // `()`
    Int,
    Double,
    Bool,
    String,
    Named,        // a declared type / constructor, possibly with args: `List[T]`
    Fn,           // `fn(A, B) -> R`
    Tuple,        // `(A, B, ...)`
    Var,          // a type variable `T` (rigid skolem or flexible inference var)
    Dyn,          // `dyn Show` / `dyn Iterable[Int]` -- a TRAIT OBJECT: the EXISTENTIAL
                  // "some type implementing this trait, which I do not name". The dual of
                  // a generic param (`T: Show` is universal -- the CALLER picks one T for
                  // the whole call; `dyn Show` is existential -- each VALUE picks and hides
                  // its own), which is why only `dyn` gives a heterogeneous `Vec[dyn Show]`.
                  // Encoded in the shared layout: `name` = the (mangled) trait, `args` =
                  // the trait's type arguments. At RUNTIME it is erased to nothing at all:
                  // every value already carries its own type (NaN-box tag / GcObject kind +
                  // type id), so a `dyn` value IS the plain value -- no box, no vtable
                  // pointer -- and dispatch is the existing dynamic path (RESOLVE_CALL).
    Never,        // divergence: the type of return/break/continue/panic; a subtype
                  // of every type (a diverging branch never constrains a join)
    Error,        // poison from an ALREADY-REPORTED error; consistent with
                  // everything so one mistake does not cascade. NOT an escape
                  // hatch -- it is never produced as a legal inferred type.
};

struct Ty;
using TyPtr = std::shared_ptr<const Ty>;   // null OR kind == Unresolved => not yet checked

// A trait bound on a type variable: a trait name plus its (possibly empty) type
// arguments. Empty `args` == a non-parametric bound (`Display`); non-empty ==
// a parametric bound (`Iterable[T]`), whose args may be OUTPUT type parameters
// solved by matching the impl during bound discharge (functional-dependency rule).
struct Bound {
    std::string trait;
    std::vector<TyPtr> args;   // parametric-trait type arguments (empty for a bare trait name)
};

struct Ty {
    TyKind kind = TyKind::Unresolved;
    std::string name;                 // Named: type/ctor name; Var: display name; Dyn: trait name
    std::vector<TyPtr> args;          // Named/Dyn: type args | Fn: parameter types | Tuple: element types
    TyPtr ret;                        // Fn: result type
    uint32_t var_id = 0;              // Var: unique id (0 = not a var)
    bool rigid = false;               // Var: rigid (a generic param being checked) vs flexible (bindable)
    std::vector<Bound> bounds;        // Var: declared trait bounds (for bound discharge)
};

// ----- primitive singletons (shared, immutable) -----------------------------
TyPtr ty_unit();
TyPtr ty_int();
TyPtr ty_double();
TyPtr ty_bool();
TyPtr ty_string();
TyPtr ty_never();
TyPtr ty_error();

// ----- compound constructors ------------------------------------------------
TyPtr make_named(std::string name, std::vector<TyPtr> args = {});
TyPtr make_fn(std::vector<TyPtr> params, TyPtr ret);
TyPtr make_tuple(std::vector<TyPtr> elems);
// `dyn <trait>[args]` -- a trait object. `trait` must already be mangled (the checker
// resolves the name); `args` are the parametric trait's type arguments, which must be
// fully written at the use site (there is no impl to solve them from).
TyPtr make_dyn(std::string trait, std::vector<TyPtr> args = {});

// Does `t` mention the type variable `var_id` anywhere (recursively)? A plain structural
// walk that does NOT consult any substitution -- apply first if you need that. Used by the
// object-safety check to find `Self` in a trait method's parameter / return types.
bool mentions_var(const TyPtr& t, uint32_t var_id);

// ----- structural equality --------------------------------------------------
// Same shape, comparing type variables by id. Does NOT consult any substitution
// (apply first if you want equality up to the current solution). Used by the
// solver's `join` and by the tests.
bool equals(const Ty& a, const Ty& b);
bool equals(const TyPtr& a, const TyPtr& b);

// ----- diagnostics ----------------------------------------------------------
// Human-readable rendering of a semantic type (error messages). A null / Unresolved
// type renders "?"; Never renders "!"; Error renders "<error>".
std::string describe(const Ty& t);
std::string describe(const TyPtr& t);

// A MESSAGE-SCOPED type renderer: like `describe`, but each DISTINCT anonymous
// flexible inference variable gets a stable per-message letter (a, b, ..., z, a1,
// b1, ...) instead of the ambiguous `_` that `describe` prints for all of them. So a
// variable that appears twice in one diagnostic reads the same letter both times
// (`fn(a) -> a`, not `fn(_) -> _`). Named flexible vars keep their user name.
//
// Rigid vars (a generic parameter `T`) also keep their user name -- but two DISTINCT
// rigid params that happen to share a name (both `T`, from different scopes) are
// disambiguated: the first renders `T`, the next distinct one `T#2`, then `T#3`, ...
// (the `#` makes it un-confusable with a real type name). Keyed by `var_id`, so the
// SAME rigid var on both sides of a message renders identically (never a false `T#2`).
//
// It is AST-free (walks an ALREADY-applied `Ty`; apply the solution first). Create
// ONE instance per diagnostic and reuse it across BOTH sides of a two-type message
// so a shared variable is named consistently on each side. `describe` itself is left
// unchanged (the naming needs the per-message map this class owns).
class TypeRenderer {
public:
    std::string operator()(const Ty& t);
    std::string operator()(const TyPtr& t);   // null -> "?"
private:
    std::unordered_map<uint32_t, std::string> names_;   // var_id -> assigned token
    std::unordered_map<std::string, uint32_t> used_names_; // base name -> distinct-var count
    uint32_t next_ = 0;
    std::string name_for(const Ty& v);                  // assign/lookup a token by v.var_id
};

} // namespace svc
