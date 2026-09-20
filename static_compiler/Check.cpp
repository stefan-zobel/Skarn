// =============================================================================
// Check.cpp -- the Skarn typechecker.
//
// Two stages over the item list (`Checker::run`):
//
// SIGNATURES: module name mangling (`mangle_declarations`), then
//   register_names     -- record every type / constructor / trait / function name
//                         (duplicate detection), so mutually-recursive signatures
//                         can refer to each other.
//   resolve_signatures -- resolve every field / param / return `Type` to a
//                         semantic `Ty` in the item's generic (+ `Self`) scope,
//                         catching unknown-type / generic-arity errors.
// Then check_traits (supertrait existence + acyclicity), check_impls (coherence:
// one impl per (trait,type), method presence/arity/self-ness, supertrait closure)
// and check_orphan.
//
// BODIES: check_bodies -- bidirectional, local checking of every fn / method / trait
// default / const body and the top-level statements -- then drain_pending_generics.
// The signature tables are members of `Checker`, shared by both stages.
// =============================================================================

#include "Check.h"
#include "Solver.h"
#include "NativeRegistry.h"   // native_id_of -- keep native names in sync with the VM registry

#include <algorithm>
#include <cassert>
#include <charconv>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace svc {

namespace {

// One resolved generic parameter: its written name + bounds + the rigid var id it
// binds to inside the enclosing signature (used by later stages to instantiate).
struct GenericInfo {
    std::string name;
    std::vector<Bound> bounds;   // resolved trait bounds (parametric: `Iterable[T]`)
    uint32_t id = 0;
    TyPtr var;              // the rigid var this parameter binds to (for body scopes)
};

struct StructInfo {
    std::string name;
    std::vector<GenericInfo> generics;
    bool is_tuple = false;
    bool is_transparent = false;      // erases to its single Int/Double/Bool field's immediate at runtime
    std::vector<std::string> field_names;
    std::vector<TyPtr> field_types;   // may reference this struct's generic rigid vars
    uint32_t line = 0, col = 0;
};

struct VariantInfo {
    std::string enum_name;
    std::string name;
    uint32_t index = 0;
    bool is_tuple = false;
    int64_t disc = 0;                 // int-backed enums: the runtime discriminant (auto or pinned)
    std::vector<std::string> field_names;
    std::vector<TyPtr> field_types;
};

struct EnumInfo {
    std::string name;
    std::vector<GenericInfo> generics;
    std::vector<std::string> variant_names;   // in declaration order
    bool is_int_backed = false;               // `enum N : Int` -> erases to a bare Int discriminant
    uint32_t line = 0, col = 0;
};

// Where a signature's parameters were WRITTEN, so a call-site argument mismatch can point back at
// the declaring parameter (a secondary "note" caret). `params` is a non-owning pointer INTO the AST:
// every unit's items are moved into the one `Program` that outlives `check()` (Compiler.cpp), and a
// DiagLabel copies text + positions, so nothing dangles once the errors escape. Null for a builtin or
// native, which has no source to point at -- those calls keep the historical single caret.
// The vector aligns 1:1 with `FnSig::params` / `MethodSig::params` (`Method::params` excludes `self`).
struct ParamOrigin {
    const std::vector<Param>* params = nullptr;
    std::string module;        // the module that DECLARED it, for per-module caret routing
};

struct FnSig {
    std::vector<GenericInfo> generics;
    std::vector<TyPtr> params;
    std::vector<bool> params_mut;   // per-param `mut` (enforced at the call site); empty = none
    TyPtr ret;
    ParamOrigin origin;             // declaration site of the params (diagnostics only)
    uint32_t line = 0, col = 0;
};

struct MethodSig {
    std::string name;
    bool has_self = false;
    bool self_mut = false;     // `mut self` receiver (enforced at the call site)
    size_t param_count = 0;    // non-self params
    bool has_default = false;  // trait: a default body is present
    std::vector<GenericInfo> generics;
    std::vector<TyPtr> params;
    std::vector<bool> params_mut;   // per-non-self-param `mut`; empty = none
    TyPtr ret;
    ParamOrigin origin;             // declaration site of the params (diagnostics only)
    uint32_t line = 0, col = 0;
};

struct TraitInfo {
    std::string name;
    std::vector<GenericInfo> generics;   // the trait's own type params (`trait Iterable[T]`)
    std::vector<std::string> supertraits;
    std::vector<MethodSig> methods;
    uint32_t self_id = 0;      // the rigid `Self` var these method sigs use
    uint32_t line = 0, col = 0;
};

struct ImplInfo {
    const ImplDecl* ast = nullptr;
    std::string module;        // the impl's home module prefix (orphan rule)
    std::string trait_name;
    std::vector<TyPtr> trait_args;   // the trait's type arguments (`impl[X] Iterable[X] for …`)
    std::vector<GenericInfo> generics;   // the impl's own type params (for matching: instantiate + unify target)
    std::string head;          // target type-head name (coherence key component)
    TyPtr target;
    // A BLANKET impl `impl[T: bounds] Tr for T` -- the target IS one of the impl's own generic
    // params, so `head` is just that param's name and does NOT key a concrete type. Such impls
    // live in `blanket_impls_`, not `impl_keys_`; `blanket_bounds` is that param's bound list.
    bool is_blanket = false;
    std::vector<Bound> blanket_bounds;
    uint32_t line = 0, col = 0;
};

// The single blanket impl registered for a trait (`impl[T: bounds] Tr for T`): a concrete type
// satisfies `Tr` through it iff the type meets every one of `bounds` (see satisfies_via_blanket).
struct BlanketInfo {
    std::vector<Bound> bounds;
    uint32_t line = 0, col = 0;
};

// A constructor in a type's value space (for exhaustiveness): its identity + the
// types of its fields (the sub-columns produced when specializing on it).
struct CtorSig {
    std::string id;
    std::vector<TyPtr> fields;
};

// A pattern normalized for Maranget's usefulness algorithm: either a wildcard
// (covers everything -- also how a binding pattern is modelled) or a constructor
// with sub-patterns. Literals become nullary constructors over an "infinite" type.
struct UPat {
    bool wild = true;
    std::string con;
    std::vector<UPat> args;
};

bool is_primitive_type(const std::string& n) {
    return n == "Int" || n == "Double" || n == "Bool" || n == "String";
}

// The bitwise/shift operator family -- Int-only on BOTH sides, unlike the arithmetic family
// (which admits Double). The plain binary form states this as a case list in `infer_binary`;
// this predicate is what `check_compound_assign` tests, so `a & b` and `a &= b` cannot drift
// apart on which types they accept. Keep the two in sync.
bool is_bitwise_op(TokKind op) {
    switch (op) {
        case TokKind::BitAnd: case TokKind::BitOr: case TokKind::BitXor:
        case TokKind::Shl:    case TokKind::Shr:   case TokKind::UShr:
            return true;
        default:
            return false;
    }
}

// The VM's Int is 48-bit signed: [-2^47, 2^47 - 1]. A literal exceeding this wraps at
// runtime, so the checker must reject it. The negative bound is one larger in
// magnitude, so a negative literal `-N` is validated against 2^47 (see infer_unary).
static constexpr int64_t INT48_MAX     = 140737488355327LL;   //  2^47 - 1
static constexpr int64_t INT48_NEG_MAG = 140737488355328LL;   //  2^47  (magnitude of MIN_48)

// Built-in generic container / sequence types (not user-declared). Returns the
// number of type arguments, or -1 if `n` is not a built-in type.
int builtin_type_arity(const std::string& n) {
    if (n == "Bytes") return 0;
    if (n == "List" || n == "Array" || n == "Vec") return 1;
    if (n == "Map") return 2;
    return -1;
}

TyPtr primitive_ty(const std::string& n) {
    if (n == "Int")    return ty_int();
    if (n == "Double") return ty_double();
    if (n == "Bool")   return ty_bool();
    if (n == "String") return ty_string();
    return ty_error();
}

// -----------------------------------------------------------------------------

// A variable binding in a lexical scope: its type + whether it was declared `mut`, plus
// the unused-binding bookkeeping (advisory): `track` = subject to the unused warning (a
// let / match / for binding, NOT a parameter), `used` flips on the first genuine read,
// `carrier` = its type is Result/Option (selects the must-use message), `line/col` = the
// binding site (where the warning is anchored).
struct ScopeVar {
    TyPtr ty;
    bool is_mut = false;
    bool track = false;
    bool used = false;
    bool carrier = false;
    uint32_t line = 0, col = 0;
    // Where a `let`-bound lambda READ this binding, i.e. snapshotted it by value (0 = never). Kept
    // for the first capture only. NOTE: these must stay LAST -- `define` builds a ScopeVar by
    // POSITIONAL aggregate init, so a field inserted above would silently shift `carrier`/`line`
    // into the wrong slots with no diagnostic anywhere.
    uint32_t cap_line = 0, cap_col = 0;
};

class Checker {
public:
    Checker(Program& p, const CheckOptions& options) : prog_(p), options_(options) {}

    CheckResult run() {
        // Teach the solver the `Concrete <: dyn Trait` edge: it is AST-free and cannot know the
        // impls. Installed up front but only ever CALLED from `subsumes` during body checking, by
        // which time the trait/impl registries are fully populated.
        tc_.set_impl_oracle([this](const TyPtr& ty, const std::string& trait,
                                   const std::vector<TyPtr>& args) {
            return dyn_coercible(ty, trait, args);
        });
        mangle_declarations();
        register_names();
        register_builtins();
        check_explicit_uses();   // after register_builtins: it needs native_module_
        resolve_signatures();
        for (auto& kv : traits_)
            for (const auto& ms : kv.second.methods)
                method_owners_[ms.name].push_back(kv.first);
        check_traits();
        check_impls();
        check_orphan();
        check_bodies();
        drain_pending_generics();
        if (options_.resolve_types) resolve_expr_types();
        finalize_diagnostics();
        CheckResult result{ std::move(errors_), std::move(warnings_), {} };
        if (options_.list_ambient) result.ambient = ambient_fns();
        return result;
    }

    // What the builtins that infer_call special-cases accept, written by hand: no FnSig can state
    // them (polymorphic over container kinds, variadic, or a FnSig row that goes unused, as push /
    // pop). One line per accepted shape; keep in step with the check_* function named in infer_call.
    static constexpr std::pair<std::string_view, std::string_view> SPECIAL_BUILTIN_SIGNATURES[] = {
        { "len",             "fn len(String | List[T] | Array[T] | Vec[T] | Bytes | Map[K, V]) -> Int" },
        { "print",           "fn print(T, ...)" },
        { "println",         "fn println(T, ...)" },
        { "panic",           "fn panic(String) -> Never" },
        { "has",             "fn has(Map[K, V], K) -> Bool" },
        { "delete",          "fn delete(Map[K, V], K) -> Bool" },
        { "get",             "fn get(Map[K, V], K) -> Option[V]\nfn get(Array[T], Int) -> Option[T]\n"
                             "fn get(Vec[T], Int) -> Option[T]\nfn get(Bytes, Int) -> Option[Int]" },
        { "keys",            "fn keys(Map[K, V]) -> Array[K]" },
        { "values",          "fn values(Map[K, V]) -> Array[V]" },
        { "push",            "fn push(mut Vec[T], T) -> Vec[T]\nfn push(mut Bytes, Int) -> Bytes" },
        { "pop",             "fn pop(mut Vec[T]) -> Option[T]\nfn pop(mut Bytes) -> Option[Int]" },
        { "toBytes",         "fn toBytes(String) -> Bytes" },
        { "fromBytes",       "fn fromBytes(Bytes) -> String" },
        { "bytes",           "fn bytes() -> Bytes\nfn bytes(Int) -> Bytes" },
        { "appendBytes",     "fn appendBytes(mut Bytes, String) -> Bytes\nfn appendBytes(mut Bytes, Bytes) -> Bytes" },
        { "toInt",           "fn toInt(Double) -> Int" },
        { "toDouble",        "fn toDouble(Int) -> Double" },
        { "floor",           "fn floor(Double) -> Double" },
        { "ceil",            "fn ceil(Double) -> Double" },
        { "trunc",           "fn trunc(Double) -> Double" },
        { "round",           "fn round(Double) -> Double" },
        { "roundHalfToEven", "fn roundHalfToEven(Double) -> Double" },
        { "ordinal",         "fn ordinal(E) -> Int" },   // E: an int-backed enum
    };

    // CheckOptions::list_ambient: the builtins and natives a user may call, sorted by name. The
    // cursor primitives behind the prelude's MapCursor and `_appendBytesRange` are internal.
    std::vector<AmbientFn> ambient_fns() const {
        std::vector<AmbientFn> out;
        auto internal = [](std::string_view n) {
            return n == "mapIterNext" || n == "mapKeyAt" || n == "mapValAt" || n.starts_with('_');
        };
        auto special = [](std::string_view n) -> std::string {
            for (const auto& [name, sig] : SPECIAL_BUILTIN_SIGNATURES)
                if (name == n) return std::string(sig);
            return {};
        };
        for (const auto& [name, sig] : builtin_fns_) {
            if (internal(name)) continue;
            std::string s = special(name);
            if (s.empty()) {
                s = "fn " + name + "(";
                for (size_t i = 0; i < sig.params.size(); ++i) s += (i ? ", " : "") + describe(sig.params[i]);
                s += ")";
                if (sig.ret) s += " -> " + describe(sig.ret);
                s = display_name(s);
            }
            const auto m = native_module_.find(name);
            out.push_back(AmbientFn{ name, m != native_module_.end() ? m->second : std::string(), s });
        }
        for (std::string_view n : BUILTIN_FN_NAMES)
            if (!internal(n) && !builtin_fns_.count(std::string(n)))
                out.push_back(AmbientFn{ std::string(n), {}, special(n) });
        std::sort(out.begin(), out.end(), [](const AmbientFn& a, const AmbientFn& b) { return a.name < b.name; });
        return out;
    }

    // Collect-all polish: drop exact-duplicate diagnostics (a body-checking pass may
    // re-resolve a signature and re-surface the same one), then order by source position
    // so the report reads top-to-bottom. Applied to errors AND warnings alike -- the
    // warning tier (must-use / unused) is emitted at scope exit in unordered-map order, so
    // the sort is what makes its output deterministic.
    static void finalize_list(std::vector<TypeError>& list) {
        std::unordered_set<std::string> seen;
        std::vector<TypeError> out;
        out.reserve(list.size());
        for (auto& e : list) {
            std::string key = std::to_string(e.line) + ":" + std::to_string(e.col) + ":" + e.message;
            if (seen.insert(key).second) out.push_back(std::move(e));
        }
        std::stable_sort(out.begin(), out.end(), [](const TypeError& a, const TypeError& b) {
            if (a.line != b.line) return a.line < b.line;
            return a.col < b.col;
        });
        list = std::move(out);
    }
    void finalize_diagnostics() { finalize_list(errors_); finalize_list(warnings_); }

    // CheckOptions::resolve_types: inference is over, so the solution is final -- write it into every
    // slot of the user items (the prelude is never shown to a user, so it is skipped).
    void resolve_expr_types() {
        for (size_t i = prog_.prelude_item_count; i < prog_.items.size(); ++i)
            for_each_expr(*prog_.items[i], [this](Expr& e) { if (e.ty) e.ty = apply(e.ty); });
    }

private:
    Program& prog_;
    const CheckOptions options_;
    TypeContext tc_;
    std::vector<TypeError> errors_;
    std::vector<TypeError> warnings_;   // advisory tier (must-use / unused); never affects ok()

    std::unordered_map<std::string, StructInfo> structs_;
    std::unordered_map<std::string, EnumInfo>   enums_;
    std::unordered_map<std::string, VariantInfo> variants_;   // variant name -> info
    std::unordered_map<std::string, FnSig>      fns_;
    std::unordered_map<std::string, TyPtr>      consts_;        // module const (mangled name -> resolved type)
    // The const's INITIALIZER, mangled name -> the `ConstItem::value` node (never owned, never
    // written through). Needed by range-pattern bounds, which must know a bound's VALUE at check
    // time to build an exact Maranget con id. Mirrors Codegen's `const_defs_` and RefEval's, and is
    // populated under the SAME duplicate guard as `consts_` -- `register_names` keeps the FIRST
    // duplicate while Codegen keeps the LAST, so a separate guard could make the con id disagree
    // with the value actually emitted in a program that has other errors.
    std::unordered_map<std::string, const Expr*> const_values_;
    std::unordered_map<std::string, FnSig>      builtin_fns_;   // container-constructing builtins (P7b)
    // Native-gating: the opt-in module each gated native belongs to. A native
    // NOT in this map is AMBIENT (always available -- the ring natives parseInt/parseDouble/GC
    // introspection). A gated native (readFile in std::io, rawRun in std::process, ...) is callable
    // only from its own module, or where `use m::*` or `use m::name` brings it in (native_available).
    std::unordered_map<std::string, std::string> native_module_;   // bare native name -> owning opt-in module
    std::unordered_map<std::string, TraitInfo>  traits_;
    std::vector<ImplInfo>                       impls_;
    std::unordered_set<std::string>             impl_keys_;   // "trait\x1fhead" of every CONCRETE impl
    std::unordered_map<std::string, size_t>     impl_index_;  // "trait\x1fhead" -> impls_ index (parametric-trait matching)
    std::unordered_map<std::string, BlanketInfo> blanket_impls_;   // trait -> its (single) blanket impl
    std::unordered_set<std::string>             blanket_in_progress_;   // cycle guard in satisfies_via_blanket
    int bound_depth_ = 0;                        // recursion guard for parametric-bound discharge
    std::unordered_map<std::string, std::vector<std::string>> method_owners_;   // method name -> declaring traits
    std::unordered_map<std::string, std::optional<std::string>> object_safety_; // trait -> why it is NOT dyn-able
    std::unordered_map<std::string, bool> eq_memo_;   // structural-Eq cache, keyed by describe(ty) (see is_eq)

    // Inherent (traitless) impls: `impl[T: bounds] Head[T] { fn m(self, …) … }`. One record per
    // target head; the method sigs are resolved against the impl's generics (shared ids with `target`).
    struct InherentInfo {
        std::vector<GenericInfo> generics;                        // impl[T: bounds]
        TyPtr target;                                             // Head[T]
        std::unordered_map<std::string, MethodSig> methods;       // method name -> resolved sig
        uint32_t line = 0, col = 0;
    };
    std::unordered_map<std::string, InherentInfo> inherent_;      // mangled target head -> info

    // Namespaces for duplicate detection (populated in pass 1a).
    std::unordered_set<std::string> ctor_names_;   // struct names + variant names

    // Body-checking state (set per function / top-level).
    std::vector<std::unordered_map<std::string, ScopeVar>> scopes_;
    std::unordered_map<std::string, TyPtr> cur_generic_env_;   // in-scope type params
    TyPtr cur_return_;
    // True while checking a module's TOP-LEVEL statements (not a fn / method / lambda body): a `return`
    // there has no function to return from; lowered, it would emit a RET from the top-level frame,
    // which the VM cannot survive.
    bool at_top_level_ = false;
    // One frame per enclosing loop (innermost last). `is_loop` marks a `loop` -- the only form
    // that may `break` with a VALUE (a `while`/`for` has a fall-through exit yielding unit, so a
    // value there could not be reconciled). For a `loop` in INFER position `break_join` accumulates
    // the join of every break value (starting at `Never`, so a break-less loop stays `Never`); in
    // CHECK position `expected` is pushed into each break value instead. The stack is cleared at
    // every fn/method entry and saved/restored across a lambda, so a `break` can never escape a
    // function into an enclosing loop.
    struct LoopFrame { bool is_loop; bool has_expected; TyPtr expected; TyPtr break_join;
                       const Expr* first_break = nullptr;      // first value-contributing break (for the diagnostic)
                       bool break_reported = false; };         // `join_element`'s latch -- must stay LAST (aggregate init)
    std::vector<LoopFrame> loop_frames_;
    // The LAMBDA WRITE BARRIER -- the third scope barrier, alongside `cur_return_` and `loop_frames_`.
    // `scopes_` is one vector shared by a function and every lambda nested in it (it is never cleared),
    // and `lookup` walks straight through, so the checker cannot otherwise tell a lambda's OWN local
    // from an enclosing one. `lambda_floor_` is the index of the lambda's own parameter scope: a name
    // resolving BELOW it is a by-value CAPTURE. 0 means "not inside a lambda", so nothing is captured
    // and the whole mechanism costs one predicted branch on the ordinary path.
    size_t lambda_floor_ = 0;
    // The SNAPSHOT WARNING's arming state -- the outward-facing half of the same rule. A capture is
    // taken at closure-CREATION time, so rebinding the outer name afterwards leaves the lambda
    // holding a stale value with nothing said. Only a lambda that OUTLIVES its statement can be
    // caught out that way, and the one shape the checker can recognise cheaply and certainly is a
    // lambda bound by a `let`: `let_bound_lambda_` is the initializer node `check_let` is currently
    // checking, and `lambda_is_let_bound_` is true while inside such a lambda's body (INHERITED by a
    // nested lambda -- codegen threads an inner lambda's captures out through the enclosing closure,
    // so the trap is just as real one level down). An argument-position lambda
    // (`fold(xs, 0, fn(a, b) { a + b + acc })`) is consumed within its statement, so a later
    // `acc = ...` is not a trap and must stay silent.
    const LambdaExpr* let_bound_lambda_ = nullptr;
    bool lambda_is_let_bound_ = false;

    // ----- module scoping ---------------------------------------------------------
    // `cur_module_` is the mangle prefix of the item currently being resolved / checked
    // (`$entry` for the entry program, "util" / "std::math" for a module, "" / `$prelude` for a
    // bare scope). A module's declared struct/enum/variant/fn/const/trait names are registered
    // under `prefix::name`, and references are resolved against the module's import environment
    // (own decls + explicit `use` names + `use m::*` globs + a bare/ambient fallback). A bare
    // scope mangles to identity (see `mangle_name`). Impls carry no name of their own.
    std::string cur_module_;
    std::unordered_map<std::string, std::unordered_set<std::string>> module_declared_;   // prefix -> its bare decl names
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> use_map_;   // prefix -> (bare -> mangled): explicit `use` (STRONG)
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> use_glob_;  // prefix -> (bare -> mangled): `use m::*` (WEAK)
    std::unordered_map<std::string, std::unordered_set<std::string>> imported_prefixes_;  // prefix -> module prefixes it imports
    // prefix -> the modules it glob-imports (`use m::*`). Kept apart from imported_prefixes_, which any
    // `import`/`use` of a module fills: a gated native is reachable through a glob or its own name only.
    std::unordered_map<std::string, std::unordered_set<std::string>> glob_modules_;
    // module prefix -> the short variant names that collide within it (they are enum-qualified instead of
    // entering module_declared_), so check_explicit_uses can tell them apart from a name that is absent.
    std::unordered_map<std::string, std::unordered_set<std::string>> colliding_variants_;
    // Visibility: the MANGLED names of every `pub` (exported) item -- a pub struct/enum/fn/trait,
    // plus every VARIANT of a pub enum (variants inherit). A cross-module reference to a name NOT in
    // this set is rejected (default private). Empty-prefix / same-module names are always visible, so
    // this is consulted only when the referrer's module differs from the referent's (visible_across).
    std::unordered_set<std::string> pub_items_;
    // Home module of each trait / user type (for the orphan rule), keyed by the MANGLED name
    // (`mod::Trait`, `mod::Type`). A builtin type is in NEITHER map (foreign).
    std::unordered_map<std::string, std::string> trait_module_;   // mangled trait name -> its module
    std::unordered_map<std::string, std::string> type_module_;    // mangled type name -> its module

    // `prefix::name`, or bare `name` for an empty prefix AND the prelude (its own scoping
    // prefix mangles to bare, so prelude names stay ambient -- see PRELUDE_MODULE_PREFIX in Naming.h).
    static std::string mangle(const std::string& prefix, const std::string& name) {
        return mangle_name(prefix, name);   // single source of truth in Ast.h
    }

    // Resolve a bare struct/enum/variant/fn reference in the CURRENT module to its canonical
    // (mangled) name: an own declaration -> `cur_module::name`; a `use`-imported name -> its
    // source module's mangled name; a `use m::*` glob name -> likewise; otherwise the bare name
    // (a prelude / ambient name). Identity in a bare scope.
    std::string mangle_ref(const std::string& name) const {
        // Priority (Rust's weak-glob rule): own declaration > explicit `use` (both STRONG) >
        // `use m::*` glob (WEAK) > a bare ambient/prelude/root name.
        if (auto dit = module_declared_.find(cur_module_);
            dit != module_declared_.end() && dit->second.count(name))
            return mangle(cur_module_, name);
        if (auto uit = use_map_.find(cur_module_); uit != use_map_.end())
            if (auto nit = uit->second.find(name); nit != uit->second.end())
                return nit->second;
        if (auto git = use_glob_.find(cur_module_); git != use_glob_.end())
            if (auto nit = git->second.find(name); nit != git->second.end())
                return nit->second;
        return name;
    }

    // ----- shadowing an ambient name is MODULE-SCOPED -------------------------------------
    //
    // Builtins, natives and trait-method names belong to no module, so they are never mangled. A
    // BARE user declaration shares their namespace, and a whole-program `fns_` lookup would let one
    // such definition disable the ambient name EVERYWHERE, including inside the sealed standard
    // library (`fn toInt(x: Double) -> Int { 999 }` making `std::math::toIntChecked(3.9)` return
    // `Ok(999)`). The entry program mangles to `$entry::name` (ENTRY_MODULE_PREFIX, Naming.h), so the
    // only BARE declarations are the monolithic dev prelude's (`--prelude <path>`, see bare_scope).
    //
    // The rule for that scope: a user fn shadows an ambient name only where that fn is
    // actually VISIBLE. A module-mangled key was resolved through an own declaration / `use`, so it is
    // in scope by construction; a BARE key is in scope only from a bare-mangling scope.
    bool is_ambient_name(const std::string& n) const {
        return is_builtin_fn_name(n) || native_id_of(n) >= 0 || method_owners_.count(n) > 0;
    }
    bool user_fn_in_scope(const std::string& key) const {
        if (key.find("::") != std::string::npos) return true;   // module-mangled => resolved in scope
        if (bare_scope(cur_module_)) return true;               // we ARE the entry / dev-prelude scope
        return !is_ambient_name(key);
    }
    // `fns_` lookup with the rule applied. When a match is skipped, the reference is marked
    // `ambient` so codegen and the RefEval oracle take the ambient route too, instead of asking
    // the same question a second time and answering it differently.
    const FnSig* user_fn(const std::string& key, IdentExpr* id = nullptr) const {
        auto it = fns_.find(key);
        if (it == fns_.end()) return nullptr;
        if (user_fn_in_scope(key)) return &it->second;
        if (id) id->ambient = true;
        return nullptr;
    }

    // Diagnostic aid: a bare name that is unknown in the CURRENT module is very often one that some
    // OTHER module exports and this one never imported -- the single most common newcomer mistake,
    // because the whole opt-in standard library is reached that way. Returns a fragment to append to
    // the unknown-name error, or "" when no module offers the name.
    //
    // This works uniformly for VALUES, TYPES and TRAITS because all three register their bare name in
    // `module_declared_` (see the item pass) -- so one helper serves every unknown-name site. A gated
    // NATIVE is not covered here and needs none: it reports its own module via `native_module_`.
    //
    // Only `pub` candidates are offered: suggesting a `use` for a private item would send the reader
    // after a name that stays unreachable. Candidates are SORTED -- `module_declared_` is unordered,
    // and a run-order-dependent diagnostic would make any fixture that locks this message flaky.
    std::string missing_use_hint(const std::string& name) const {
        // The entry program is not importable -- nothing can name it -- so a name IT declares can
        // never be brought into scope from a module. Say that, rather than listing modules (or,
        // absurdly, offering `import $entry`). This is the one diagnostic that explains the entry's
        // prefix to a reader who never sees it, so it is worth answering before the general search.
        if (cur_module_ != ENTRY_MODULE_PREFIX)
            if (auto eit = module_declared_.find(ENTRY_MODULE_PREFIX);
                eit != module_declared_.end() && eit->second.count(name))
                return "; it is defined in the entry program, which cannot be imported -- move it "
                       "into a module to share it";

        std::vector<std::string> mods;
        for (const auto& [prefix, names] : module_declared_) {
            if (prefix.empty() || prefix == cur_module_) continue;   // ambient / our own module
            if (prefix == PRELUDE_MODULE_PREFIX) continue;           // the ambient prelude is always in scope
            if (prefix == ENTRY_MODULE_PREFIX) continue;             // handled above (never importable)
            if (!names.count(name)) continue;
            if (!pub_items_.count(mangle(prefix, name))) continue;   // private: a `use` would not help
            mods.push_back(prefix);
        }
        if (mods.empty()) {
            // No `use` would help -- but if a module this one IMPORTS declares the name privately, say
            // so: "unknown" for an item the reader can see in that file sends them looking for a typo.
            // Only imported modules are named, so an unrelated module's internals stay out of view.
            for (const auto& [prefix, names] : module_declared_) {
                if (prefix.empty() || prefix == cur_module_ || prefix == PRELUDE_MODULE_PREFIX ||
                    prefix == ENTRY_MODULE_PREFIX || !names.count(name) || !imported_by_current(prefix))
                    continue;
                return "; '" + name + "' is declared in '" + prefix + "' but is not `pub`";
            }
            return "";
        }
        std::sort(mods.begin(), mods.end());

        if (mods.size() > 1) {                                       // ambiguous: list, do not guess
            std::string s = "; it is declared in ";
            const size_t shown = mods.size() > 3 ? 3 : mods.size();
            for (size_t i = 0; i < shown; ++i) { if (i) s += ", "; s += "'" + mods[i] + "'"; }
            if (mods.size() > shown) s += ", ...";
            return s + " -- add the matching `use`";
        }

        const std::string& m = mods.front();
        // A std module is reached with a glob (`use std::io::*`), which is how the guide teaches it.
        // A user module additionally needs its `import` unless this module already has one.
        if (m.rfind("std::", 0) == 0)
            return "; it is in '" + m + "' -- add `use " + m + "::*`";
        if (imported_by_current(m))
            return "; it is in '" + m + "' -- add `use " + m + "::" + name + "`";
        return "; it is in '" + m + "' -- add `import " + m + "` and `use " + m + "::" + name + "`";
    }

    // The virtual `std::Name` qualifier: `std` is not a real module but an alias reaching the
    // ambient std entities REGARDLESS of local shadowing (that is its purpose). Resolve `name`
    // against the ring modules + the ambient `$prelude` and return its canonical mangled name;
    // a name in none of them (e.g. a native/builtin) stays bare. NOTE: this deliberately does NOT
    // consult a local decl -- `std::map` reaches the ring `map` even if a local `map` shadows it.
    std::string resolve_std(const std::string& name) const {
        for (const char* R : { STD_CORE, STD_ITER, STD_STRING, PRELUDE_MODULE_PREFIX }) {
            auto dit = module_declared_.find(R);
            if (dit != module_declared_.end() && dit->second.count(name))
                return mangle(R, name);
        }
        return name;   // an ambient builtin/native (not a declared std item) -- stays bare
    }

    // A qualifier `q` in `q::name` is a MODULE path iff it starts lowercase (module names are
    // lident); an uppercase qualifier is a trait (`Trait::method`). The case split makes the two
    // `::` roles unambiguous -- no collision, no warning needed.
    static bool is_module_qualifier(const std::string& q) {
        return !q.empty() && (q[0] == '_' || (q[0] >= 'a' && q[0] <= 'z'));
    }
    // Is module `mod` imported (via `import`/`use`) by the module currently being checked?
    bool imported_by_current(const std::string& mod) const {
        auto it = imported_prefixes_.find(cur_module_);
        return it != imported_prefixes_.end() && it->second.count(mod);
    }

    // Visibility: is the foreign (already-mangled) name `key` reachable from the current
    // module? An ambient/prelude name (empty prefix or `$prelude`, both bare) and a same-module name
    // are always visible; any other module's name must be `pub`. Consulted only at CROSS-module
    // resolution (a `mod::name` qualifier or an imported name) -- a bare same-module ref never hits it.
    bool visible_across(const std::string& key) const {
        const std::string owner = module_prefix_of(key);
        // Ambient / prelude ($prelude) / the entry program: an entry name is only ever referenced
        // from the entry itself (nothing can import it), so `pub` must not be demanded there.
        if (owner.empty() || owner == PRELUDE_MODULE_PREFIX || owner == ENTRY_MODULE_PREFIX) return true;
        if (owner == cur_module_) return true;                             // same module
        return pub_items_.count(key) > 0;                                  // else must be exported
    }

    // Resolve a possibly module-qualified type/ctor/struct reference to its canonical (mangled)
    // name. An EMPTY qualifier takes the ordinary `mangle_ref` path (own-decl > use > glob >
    // ambient). A
    // MODULE qualifier (`mod::Name`, always a lident head from the parser) resolves to the
    // `mod::name` key (bare for the virtual `std`), with an import check. Clears `qualifier`
    // (it is consumed here; codegen reads only the written-back `name`). Mirrors the qualified
    // CALL resolution in `call_module_member`.
    std::string resolve_qualified_ref(std::string& qualifier, const std::string& name,
                                      uint32_t line, uint32_t col) {
        if (qualifier.empty()) return mangle_ref(name);
        if (!is_module_qualifier(qualifier)) {           // `Enum::Variant`: uppercase enum head
            std::string vk = resolve_qualified_variant(qualifier, name, line, col);
            qualifier.clear();
            return vk.empty() ? name : vk;               // on error the helper already reported; the
        }                                                //   bare fallback keeps one clean downstream error
        const std::string mod = qualifier;
        const bool is_std = (mod == "std");
        if (!is_std && !imported_by_current(mod))
            error(line, col, "module '" + mod + "' is not imported");
        qualifier.clear();
        const std::string key = is_std ? resolve_std(name) : (mod + "::" + name);
        if (!is_std && !visible_across(key))                          // default private
            error(line, col, "item '" + name + "' is private in module '" + mod + "'");
        return key;
    }

    // Resolve a qualified enum-variant reference `Enum::Variant` to the variant's canonical
    // (mangled) key, verifying the variant actually belongs to the named enum. The `enum_qual` head
    // is an uppercase type name resolved in the current scope (own module / `use`-imported / ring),
    // so a variant of an imported enum reads as `Enum::Variant` once the enum type is in scope
    // (the parser carries a single qualifier segment -- `mod::Enum::Variant` is NOT a value path;
    // cross-module variants come bare via `use mod::Enum::*`). On any failure (head is
    // not an enum / private / has no such variant) it reports a precise diagnostic and returns "".
    std::string resolve_qualified_variant(const std::string& enum_qual, const std::string& variant,
                                          uint32_t line, uint32_t col) {
        const std::string ekey = mangle_ref(enum_qual);
        auto eit = enums_.find(ekey);
        if (eit == enums_.end()) {
            // A head that is a real type of another kind is "not an enum"; a head that names NOTHING in
            // scope is usually an enum whose `use` is missing (`Json::Integer(3)`), so say that instead.
            if (structs_.count(ekey) || traits_.count(ekey))
                error(line, col, "'" + enum_qual + "' is not an enum");
            else
                error(line, col, "unknown type '" + enum_qual + "'" + missing_use_hint(enum_qual));
            return {};
        }
        if (!visible_across(ekey)) {
            error(line, col, "enum '" + enum_qual + "' is private");
            return {};
        }
        // Find the variant by its SHORT name among this enum's variants -- robust to either key
        // shape (a plain `mod::V` or a collision-disambiguated `mod::Enum::V`).
        for (const std::string& vk : eit->second.variant_names)
            if (short_name(vk) == variant) return vk;
        error(line, col, "enum '" + short_name(ekey) + "' has no variant '" + variant + "'");
        return {};
    }

    // A BARE uppercase name that failed to resolve MAY be a collision-disambiguated variant
    // (short name shared by >1 enum in the current module -> only enum-qualified keys exist). If so,
    // report an ambiguity with a `Enum::V` hint and return true; otherwise return false (let the
    // caller emit its ordinary "unknown" error). Detected purely from `variants_`, so no extra state.
    bool report_if_ambiguous_variant(const std::string& shortName, uint32_t line, uint32_t col) {
        std::vector<std::string> cands;
        for (const auto& [vk, vi] : variants_)
            if (short_name(vk) == shortName && module_prefix_of(vi.enum_name) == cur_module_)
                cands.push_back(short_name(vi.enum_name) + "::" + shortName);
        if (cands.size() < 2) return false;
        std::sort(cands.begin(), cands.end());
        std::string hint;
        for (size_t i = 0; i < cands.size(); ++i) { if (i) hint += " or "; hint += cands[i]; }
        error(line, col, "ambiguous variant '" + shortName + "'; qualify it (" + hint + ")");
        return true;
    }

    // A SPECULATIVE region: diagnostics raised inside are dropped. `subsumes` is a predicate --
    // callers decide from its result whether to report -- so the `Concrete <: dyn Trait` oracle
    // must not leave errors behind when it merely fails to prove the coercion. Scoped (RAII) and
    // counted, so it composes with nesting. It suppresses ONLY reporting: unification still binds,
    // exactly as everywhere else in `subsumes`.
    int quiet_depth_ = 0;
    struct QuietGuard {
        int& d;
        explicit QuietGuard(int& depth) : d(depth) { ++d; }
        ~QuietGuard() { --d; }
    };

    // ----- the nesting-depth limit -------------------------------------------------------
    // Checking is recursive descent over the AST, so expression nesting costs STACK: one level of a
    // pipe chain costs ~21 KB in a Debug build (`infer_pipe` alone 11 KB), and without a limit a deep
    // enough expression simply overflows -- the process dies with no diagnostic at all, at a depth
    // that differs between Debug and Release, i.e. the two configurations would accept different
    // programs. The limit makes that a located, deterministic error instead.
    //
    // 200 is chosen against the 8 MB stack the front-end executables reserve (see their vcxproj /
    // CMakeLists): 200 x 21 KB = 4.2 MB, about half. It cannot reject anything that would otherwise
    // compile: codegen already refuses a call/pipe nest beyond ~64 registers, and no real expression
    // nests 200 deep. The AST depth is what matters, NOT the source nesting -- a left-nested chain
    // (`1 + 1 + 1 ...`, `x |> f |> g`) is read by a LOOP in the parser but is n levels deep here.
    static constexpr int MAX_EXPR_DEPTH = 200;
    int expr_depth_ = 0;
    const Expr* depth_node_ = nullptr;   // the node the innermost level belongs to
    bool too_deep_reported_ = false;   // latch: report once per run, not at every level on the way out
    // Counts AST LEVELS, not calls: `check_expr` hands most nodes on to `infer`, so the two funnels
    // would otherwise count one node twice and halve the real limit. Entering the node that already
    // owns the innermost level is free.
    struct DepthGuard {
        Checker& c;
        const Expr* prev;
        bool counted;
        DepthGuard(Checker& ch, const Expr& e) : c(ch), prev(ch.depth_node_), counted(ch.depth_node_ != &e) {
            if (counted) { ++c.expr_depth_; c.depth_node_ = &e; }
        }
        ~DepthGuard() { if (counted) { --c.expr_depth_; c.depth_node_ = prev; } }
    };
    // True (and reports, once) when this node is past the limit. The caller poisons its subtree with
    // `ty_error()`, which every consumer already handles.
    bool too_deep(const Expr& e) {
        if (expr_depth_ <= MAX_EXPR_DEPTH) return false;
        if (!too_deep_reported_ && quiet_depth_ == 0) {
            too_deep_reported_ = true;
            error(e.line, e.col, "expression nested too deeply (more than " +
                  std::to_string(MAX_EXPR_DEPTH) + " levels); split it into several statements");
        }
        return true;
    }

    // A generic argument a call site could not yet decide, re-examined once every body has been
    // checked (`drain_pending_generics`). A type argument is often fixed only AFTER the call that
    // introduced it -- by a later argument (`fold(xs, Set::new(), fn(acc: Set[Int], x: Int) ...)`) or a
    // later statement (`let mut s = Set::new()` then `s.insert(3)`). Discharging the bound at the call
    // would test the still-flexible var against its OWN bounds, which passes vacuously, so the type
    // fixed later (a non-Hashable `P` reaching `Map[P, Int]` through an unannotated binding) would never
    // be checked; rejecting the call would refuse programs whose type IS fixed. Deferring avoids both.
    // `bound.trait` empty = a bare "must be solved" record (an associated function's unbounded
    // generic). Still unsolved at the drain = "cannot infer".
    struct PendingGeneric {
        TyPtr var;
        Bound bound;
        std::string generic;   // the type parameter's name, for the diagnostic
        std::string what;      // the callee / constructed type, for the diagnostic
        uint32_t line = 0, col = 0;
        std::string module;
    };
    std::vector<PendingGeneric> pending_generics_;

    // A bound on a generic impl's type parameter that `impl_bounds_hold` could not decide because the parameter
    // matched a still-unsolved var (`check(t)` with `t: Timed[?]`). Collected only while a user-facing site has
    // set `impl_obligations_`; that site turns each one into a `PendingGeneric`.
    struct ImplObligation {
        TyPtr var;
        Bound bound;
        std::string generic;   // the impl's type parameter, for the diagnostic
        std::string impl;      // the impl it belongs to (`Named for Timed`), for the diagnostic
    };
    std::vector<ImplObligation>* impl_obligations_ = nullptr;

    // `display_name` (Naming.h) is applied HERE -- the one choke point every checker diagnostic
    // passes through -- rather than at each of the ~40 sites that interpolate a possibly-mangled
    // name. The per-site version is whack-a-mole: the next message added would leak the entry
    // program's internal `$entry::` prefix again. A real module path is kept.
    void error(uint32_t line, uint32_t col, std::string msg) {
        if (quiet_depth_ > 0) return;
        errors_.push_back(TypeError{ display_name(std::move(msg)), line, col, cur_module_ });
    }

    // Richer sink for structured diagnostics: the same error plus secondary "note" carets
    // (dual expected/found reporting). An empty `labels` is identical to the 3-arg form.
    void error(uint32_t line, uint32_t col, std::string msg, std::vector<DiagLabel> labels) {
        if (quiet_depth_ > 0) return;
        for (DiagLabel& l : labels) l.text = display_name(std::move(l.text));   // notes too
        errors_.push_back(TypeError{ display_name(std::move(msg)), line, col, cur_module_,
                                     std::move(labels) });
    }

    // A secondary caret at an AST type node (an annotation / declared return type), in the
    // current module -- the "expected because of this ..." note that accompanies a mismatch.
    DiagLabel label_at(const Type& t, std::string text) const {
        return DiagLabel{ std::move(text), t.line, t.col, cur_module_ };
    }

    // Advisory diagnostic (must-use / unused). Never affects `ok()`; a future
    // `static_vmrun --strict` escalates the collected list to fatal.
    void warn(uint32_t line, uint32_t col, std::string msg) {
        warnings_.push_back(TypeError{ display_name(std::move(msg)), line, col, cur_module_ });
    }

    // The twin of the 4-arg `error` above: an advisory diagnostic with secondary "note" carets.
    // A warning whose whole point is "this happened HERE because of something THERE" -- the lambda
    // snapshot warning -- is unreadable without the second caret. `static_vmrun` already renders
    // `w.labels` exactly as it renders an error's, so this needs no driver change.
    void warn(uint32_t line, uint32_t col, std::string msg, std::vector<DiagLabel> labels) {
        for (DiagLabel& l : labels) l.text = display_name(std::move(l.text));   // notes too
        warnings_.push_back(TypeError{ display_name(std::move(msg)), line, col, cur_module_,
                                       std::move(labels) });
    }

    // A Result/Option CARRIER: a discarded one is almost always a forgotten failure (Track 1 must-use), and in
    // `-> ()` tail position it is a hard error. Keyed by NAME, exactly as `infer_try` recognizes Result/Option
    // (no static prelude needed).
    bool is_result_carrier(const TyPtr& t) {
        if (!t) return false;
        TyPtr a = apply(t);
        // A carrier is recognized by SHORT name -- the ring's std::core::Result OR a user's own `Result`
        // (a program may define its own without the prelude).
        return a && a->kind == TyKind::Named &&
               (short_name(a->name) == "Result" || short_name(a->name) == "Option");
    }

    // A type the program marked with `impl MustUse for T {}` (std::core's open marker). Only the WARNING follows
    // from it. Absent without the prelude; an unsolved var is not judged, and a wrapper whose argument is still
    // open does not warn (nothing collects an obligation here: a warning is advice, not a soundness check).
    bool is_marked_must_use(const TyPtr& t) {
        if (!t || traits_.find(std_MustUse()) == traits_.end()) return false;
        TyPtr a = apply(t);
        if (a->kind == TyKind::Var && !a->rigid) return false;
        if (a->kind == TyKind::Error || a->kind == TyKind::Never) return false;
        return satisfies_bound(a, std_MustUse());
    }

    // A discarded value of this type draws the must-use warning.
    bool is_must_use_type(const TyPtr& t) { return is_result_carrier(t) || is_marked_must_use(t); }

    // must-use: warn when a must-use expression's value is thrown away (a statement-position expression / a
    // loop body). `e.ty` is already filled by `infer`.
    void check_discarded(const Expr& e) {
        if (is_result_carrier(e.ty))
            warn(e.line, e.col, "unused " + apply(e.ty)->name +
                 ": handle it (match / unwrap / `?`), or discard it explicitly with `let _ = ...`");
        else if (is_marked_must_use(e.ty))
            warn(e.line, e.col, "unused " + describe(apply(e.ty)) +
                 ": its type is marked MustUse -- use the value, or discard it explicitly with `let _ = ...`");
    }

    bool is_type_name(const std::string& n) const {
        return is_primitive_type(n) || builtin_type_arity(n) >= 0 ||
               structs_.count(n) || enums_.count(n);
    }

    // ----- pass 0: module name mangling -------------------------------------
    // Rewrite every module's declared struct/enum/variant/fn/const/trait NAME to `prefix::name` in
    // place on the AST -- so register_names, resolve_signatures, the body passes AND codegen all see
    // the canonical (globally-unique) name. Records the per-module BARE declared-name sets and
    // builds the import environment (explicit `use` targets + imported module prefixes) that
    // mangle_ref resolves references against. A bare scope (empty / `$prelude`) mangles to
    // identity. Impls declare no name; they carry their module via ImplInfo.
    void mangle_declarations() {
        auto join = [](const std::vector<std::string>& p) {
            std::string s; for (size_t i = 0; i < p.size(); ++i) { if (i) s += "::"; s += p[i]; } return s;
        };
        // Count each variant SHORT name per module across all enums. A short name
        // used by >1 enum in one module COLLIDES -> those variants get an enum-qualified key
        // (`mod::Enum::V`) instead of the plain `mod::V`, so they coexist (Rust model). A
        // non-colliding variant keeps its plain key.
        std::unordered_map<std::string, std::unordered_map<std::string, int>> vcount;
        for (auto& item : prog_.items)
            if (item->kind == ItemKind::Enum)
                for (auto& v : static_cast<EnumItem&>(*item).variants)
                    vcount[item->module_prefix][v.name]++;
        auto variant_collides = [&](const std::string& P, const std::string& n) {
            auto vit = vcount.find(P);
            return vit != vcount.end() && vit->second.count(n) && vit->second.at(n) > 1;
        };
        auto is_upper = [](const std::string& s) { return !s.empty() && s[0] >= 'A' && s[0] <= 'Z'; };
        std::unordered_map<std::string, std::vector<std::string>> enum_variant_keys;   // enumKey -> variant keys
        for (auto& item : prog_.items) {
            const std::string& P = item->module_prefix;
            switch (item->kind) {
                case ItemKind::Struct: {
                    auto& s = static_cast<StructItem&>(*item);
                    module_declared_[P].insert(s.name);
                    s.name = mangle(P, s.name);
                    type_module_[s.name] = P;                 // home module (orphan rule)
                    if (item->is_pub) pub_items_.insert(s.name);
                    break;
                }
                case ItemKind::Enum: {
                    auto& e = static_cast<EnumItem&>(*item);
                    module_declared_[P].insert(e.name);
                    e.name = mangle(P, e.name);
                    type_module_[e.name] = P;                 // home module (orphan rule)
                    if (item->is_pub) pub_items_.insert(e.name);
                    for (auto& v : e.variants) {
                        if (variant_collides(P, v.name)) {              // colliding short -> enum-qualified key
                            colliding_variants_[P].insert(v.name);
                            v.name = e.name + "::" + v.name;           // e.name is already mangled here
                            // NOT added to module_declared_: the bare short is ambiguous (must qualify)
                        } else {
                            module_declared_[P].insert(v.name);
                            v.name = mangle(P, v.name);
                        }
                        if (item->is_pub) pub_items_.insert(v.name);   // variants inherit the enum's visibility
                        enum_variant_keys[e.name].push_back(v.name);   // for `use mod::Enum::*`
                    }
                    break;
                }
                case ItemKind::Fn: {
                    auto& f = static_cast<FnItem&>(*item);
                    module_declared_[P].insert(f.name);
                    f.name = mangle(P, f.name);
                    if (item->is_pub) pub_items_.insert(f.name);
                    break;
                }
                case ItemKind::Const: {
                    auto& c = static_cast<ConstItem&>(*item);
                    module_declared_[P].insert(c.name);
                    c.name = mangle(P, c.name);
                    if (item->is_pub) pub_items_.insert(c.name);
                    break;
                }
                case ItemKind::Import:
                    imported_prefixes_[P].insert(join(static_cast<ImportItem&>(*item).path));
                    break;
                case ItemKind::Use: {
                    auto& u = static_cast<UseItem&>(*item);
                    if (!u.path.empty() && is_upper(u.path.back())) {   // `use mod::Enum::(*|V|{...})`
                        std::vector<std::string> modsegs(u.path.begin(), u.path.end() - 1);
                        imported_prefixes_[P].insert(join(modsegs));    // the MODULE (enum-use expanded in pass 3)
                        break;
                    }
                    const std::string src = join(u.path);
                    imported_prefixes_[P].insert(src);
                    if (!u.glob)                                     // explicit names (STRONG); glob below
                        for (const auto& n : u.names) {
                            if (use_map_[P].count(n))                // two explicit imports of one name
                                error(item->line, item->col, "conflicting import of '" + n + "'");
                            use_map_[P][n] = mangle(src, n);
                        }
                    break;
                }
                case ItemKind::Trait: {
                    auto& t = static_cast<TraitDecl&>(*item);
                    module_declared_[P].insert(t.name);     // participates in the module namespace
                    t.name = mangle(P, t.name);             // mangle the decl (coexistence across modules)
                    trait_module_[t.name] = P;              // keyed by the MANGLED trait name (orphan rule)
                    if (item->is_pub) pub_items_.insert(t.name);
                    break;
                }
                case ItemKind::Impl:
                case ItemKind::Stmt:
                    break;   // impls carry their module via ImplInfo; stmts declare no top-level names
            }
        }

        // Second pass: expand `use m::*` globs (WEAK) + the strong-vs-strong collision
        // check -- now that every module's declared-name set and every explicit `use` are known.
        for (auto& item : prog_.items) {
            if (item->kind != ItemKind::Use) continue;
            auto& u = static_cast<UseItem&>(*item);
            const std::string& P = item->module_prefix;
            if (!u.path.empty() && is_upper(u.path.back())) continue;   // enum-use: expanded below
            if (u.glob) {                                            // `use m::*`: all of m's PUB names, weakly
                const std::string src = join(u.path);
                glob_modules_[P].insert(src);                        // + m's gated natives (native_available)
                if (auto dit = module_declared_.find(src); dit != module_declared_.end())
                    for (const auto& n : dit->second) {
                        const std::string key = mangle(src, n);
                        if (src != P && !visible_across(key)) continue;   // a glob SKIPS non-pub names (Rust)
                        use_glob_[P][n] = key;                            // overridden by own-decl / explicit use
                    }
            } else {                                                 // explicit `use` may not shadow a local decl
                for (const auto& n : u.names)
                    if (auto dit = module_declared_.find(P); dit != module_declared_.end() && dit->second.count(n))
                        error(item->line, item->col,
                              "import of '" + n + "' conflicts with a local definition");
                // Whether `n` exists in the source module and is visible is checked in check_explicit_uses,
                // which runs after register_builtins: a native is not a module declaration.
            }
        }

        // Expand enum-variant uses `use mod::Enum::(* | V | {X, Y})` -- bring another
        // module's enum variants into scope by their SHORT name (glob = WEAK like a module glob;
        // explicit = STRONG). The enum's variant keys were gathered above (enum_variant_keys), so
        // this resolves the short name to the canonical (possibly collision-disambiguated) key.
        for (auto& item : prog_.items) {
            if (item->kind != ItemKind::Use) continue;
            auto& u = static_cast<UseItem&>(*item);
            if (u.path.empty() || !is_upper(u.path.back())) continue;   // only enum-uses here
            const std::string& P = item->module_prefix;
            std::vector<std::string> modsegs(u.path.begin(), u.path.end() - 1);
            const std::string enumMod = join(modsegs);
            const std::string enumKey = mangle(enumMod, u.path.back());
            auto eit = enum_variant_keys.find(enumKey);
            if (eit == enum_variant_keys.end()) {
                error(item->line, item->col, "'" + u.path.back() + "' is not an enum in module '" + enumMod + "'");
                continue;
            }
            if (enumMod != P && !pub_items_.count(enumKey)) {          // a private enum can't be use'd cross-module
                error(item->line, item->col, "enum '" + u.path.back() + "' is private in module '" + enumMod + "'");
                continue;
            }
            if (u.glob) {                                             // `use mod::Enum::*` -- all variants, WEAK
                for (const auto& vk : eit->second) use_glob_[P][short_name(vk)] = vk;
            } else {                                                  // `use mod::Enum::{X, Y}` -- STRONG
                for (const auto& n : u.names) {
                    std::string found;
                    for (const auto& vk : eit->second) if (short_name(vk) == n) { found = vk; break; }
                    if (found.empty()) {
                        error(item->line, item->col, "enum '" + u.path.back() + "' has no variant '" + n + "'");
                        continue;
                    }
                    if (use_map_[P].count(n))
                        error(item->line, item->col, "conflicting import of '" + n + "'");
                    use_map_[P][n] = found;
                }
            }
        }

        // Third pass: the implicit auto-import RING. Every module behaves as if it wrote
        // `use std::core::*; use std::iter::*; use std::string::*`, so the ring modules' names
        // resolve BARE everywhere (WEAK, like any glob -- a local def or an explicit `use` still
        // wins via mangle_ref's own-decl > use_map_ > use_glob_ priority). This is what keeps the
        // stdlib split ergonomically identical to a flat prelude. A ring module does not glob-import
        // itself (own decls win regardless). module_declared_[R] holds BARE names (inserted before
        // the decl was mangled), so use_glob_[P][bare] = R::bare -- the canonical mangled name.
        {
            const char* const kRing[] = { STD_CORE, STD_ITER, STD_STRING };
            std::unordered_set<std::string> modules;                 // every distinct module prefix present
            for (const auto& item : prog_.items) modules.insert(item->module_prefix);
            for (const auto& P : modules)
                for (const char* R : kRing) {
                    if (P == R) continue;                            // no self-glob
                    auto dit = module_declared_.find(R);
                    if (dit == module_declared_.end()) continue;     // ring module absent (no prelude)
                    for (const auto& n : dit->second) {
                        const std::string key = mangle(R, n);
                        if (!pub_items_.count(key)) continue;        // only EXPORTED ring names go bare
                        use_glob_[P][n] = key;
                    }
                }
        }
    }

    // ----- pass 1a: shapes (names + generic arities) ------------------------
    // INVARIANT: after this pass EVERY user struct / enum / trait has its NAME and
    // its generic ARITY registered (the generics vector is pre-sized with placeholder
    // GenericInfos; resolve_* in pass 1b overwrites them with fully-resolved generics).
    // This is what makes pass 1b (contents) order-independent: content resolution reads
    // from OTHER items only their arity (`resolve_type` -> `.generics.size()`), never
    // their rigid vars or contents, so a forward reference sees the right arity. Do NOT
    // move arity registration into pass 1b -- that reintroduces the forward-ref bug class.

    void register_names() {
        for (const auto& item : prog_.items) {
            cur_module_ = item->module_prefix;   // tag any duplicate-decl error with its module
            switch (item->kind) {
                case ItemKind::Struct: {
                    const auto& s = static_cast<const StructItem&>(*item);
                    if (is_primitive_type(s.name))
                        error(s.line, s.col, "cannot redefine builtin type '" + s.name + "'");
                    else if (structs_.count(s.name) || enums_.count(s.name))
                        error(s.line, s.col, "duplicate type '" + s.name + "'");
                    else {
                        StructInfo si; si.name = s.name; si.is_tuple = s.is_tuple;
                        si.is_transparent = s.is_transparent;
                        si.line = s.line; si.col = s.col;
                        // Pre-size the generic arity NOW (syntactic -- no resolution needed) so a
                        // FORWARD reference from an earlier-declared type checks against the right
                        // arity. resolve_struct overwrites these with the fully-resolved generics.
                        for (const auto& gp : s.generics)
                            si.generics.push_back(GenericInfo{ gp.name, {}, 0, nullptr });
                        structs_.emplace(s.name, std::move(si));
                    }
                    register_ctor(s.name, s.line, s.col);
                    break;
                }
                case ItemKind::Enum: {
                    const auto& e = static_cast<const EnumItem&>(*item);
                    if (is_primitive_type(e.name))
                        error(e.line, e.col, "cannot redefine builtin type '" + e.name + "'");
                    else if (structs_.count(e.name) || enums_.count(e.name))
                        error(e.line, e.col, "duplicate type '" + e.name + "'");
                    else {
                        EnumInfo ei; ei.name = e.name; ei.line = e.line; ei.col = e.col;
                        ei.is_int_backed = e.is_int_backed;
                        // Pre-size the generic arity NOW (see the struct branch); resolve_enum
                        // overwrites these with the fully-resolved generics.
                        for (const auto& gp : e.generics)
                            ei.generics.push_back(GenericInfo{ gp.name, {}, 0, nullptr });
                        enums_.emplace(e.name, std::move(ei));
                    }
                    for (const auto& v : e.variants) {
                        register_ctor(v.name, v.line, v.col);
                        if (!variants_.count(v.name)) {
                            VariantInfo vi; vi.enum_name = e.name; vi.name = v.name; vi.is_tuple = v.is_tuple;
                            variants_.emplace(v.name, std::move(vi));
                        }
                    }
                    break;
                }
                case ItemKind::Trait: {
                    const auto& t = static_cast<const TraitDecl&>(*item);
                    if (traits_.count(t.name))
                        error(t.line, t.col, "duplicate trait '" + t.name + "'");
                    else {
                        TraitInfo ti; ti.name = t.name; ti.line = t.line; ti.col = t.col;
                        // Pre-size the generic arity NOW (see the struct/enum branches); resolve_trait
                        // overwrites these with the fully-resolved generics.
                        for (const auto& gp : t.generics)
                            ti.generics.push_back(GenericInfo{ gp.name, {}, 0, nullptr });
                        traits_.emplace(t.name, std::move(ti));
                    }
                    break;
                }
                case ItemKind::Fn: {
                    const auto& f = static_cast<const FnItem&>(*item);
                    if (fns_.count(f.name))
                        error(f.line, f.col, "duplicate function '" + f.name + "'");
                    else
                        fns_.emplace(f.name, FnSig{});   // filled in 1b
                    break;
                }
                case ItemKind::Const: {
                    const auto& c = static_cast<const ConstItem&>(*item);
                    if (consts_.count(c.name) || fns_.count(c.name))
                        error(c.line, c.col, "duplicate declaration '" + c.name + "'");
                    else {
                        consts_.emplace(c.name, nullptr);   // type resolved in 1b (resolve_const)
                        const_values_.emplace(c.name, c.value.get());   // same guard -- see the decl
                    }
                    break;
                }
                case ItemKind::Impl:
                case ItemKind::Stmt:
                case ItemKind::Import:   // module items: resolved by the loader and mangle_declarations
                case ItemKind::Use:
                    break;   // impls handled in check_impls; top-level stmts in check_bodies
            }
        }
    }

    // Container-constructing builtins (P7b). Each is a generic FnSig with one fresh rigid type
    // param `T` used as a substitution template (call_direct_fn re-instantiates it per call).
    // `len` is not here -- it is polymorphic over container kinds, special-cased in infer_call
    // (check_len).
    //
    // A user-defined fn of the same name still WINS, but only where it is in scope: infer_call
    // consults `user_fn` BEFORE `builtin_fns_`, so the shadowing is decided per referring module
    // (see user_fn_in_scope). The registration itself must therefore be UNCONDITIONAL -- skipping
    // it when some user fn exists is what left `std::math` with no `sqrt` at all.
    void register_builtins() {
        auto add = [&](const std::string& name, auto make_params, auto make_ret) {
            TyPtr T = tc_.rigid_var("T");
            GenericInfo g{ "T", {}, T->var_id, T };
            FnSig sig;
            sig.generics = { g };
            sig.params   = make_params(T);
            sig.ret      = make_ret(T);
            builtin_fns_.emplace(name, std::move(sig));
        };
        // array(n: Int, init: T) -> Array[T]   -- always-initialized fixed array (sound: no Undefined)
        add("array", [](TyPtr T) { return std::vector<TyPtr>{ ty_int(), T }; },
                     [](TyPtr T) { return make_named("Array", { T }); });
        // vec() -> Vec[T]                       -- empty growable vector (T inferred from use)
        add("vec",   [](TyPtr)  { return std::vector<TyPtr>{}; },
                     [](TyPtr T) { return make_named("Vec", { T }); });
        // emptyArray() -> Array[T]              -- empty fixed array (T inferred from use, like vec()).
        // Sound with no init value: 0 slots, so no element is ever read. Lowers to a zero-count ANEW.
        add("emptyArray", [](TyPtr)  { return std::vector<TyPtr>{}; },
                          [](TyPtr T) { return make_named("Array", { T }); });
        // push(v: Vec[T], x: T) -> Vec[T]       -- append, returns the vector (chainable)
        add("push",  [](TyPtr T) { return std::vector<TyPtr>{ make_named("Vec", { T }), T }; },
                     [](TyPtr T) { return make_named("Vec", { T }); });
        // pop(v: Vec[T]) -> Option[T]           -- remove+return the last element (None if empty)
        add("pop",   [](TyPtr T) { return std::vector<TyPtr>{ make_named("Vec", { T }) }; },
                     [](TyPtr T) { return make_named(std_Option(), { T }); });
        // toString(x: T) -> String              -- universal stringify (any type -> its text form)
        add("toString", [](TyPtr T) { return std::vector<TyPtr>{ T }; },
                        [](TyPtr)   { return ty_string(); });
        // print(a, ...) -- variadic; special-cased in check_print (see check_println).
        register_natives();
    }

    // Native I/O functions (file / time / env / stdin / process). Each is a MONOMORPHIC FnSig (no
    // generics) lowered to CALL_NATIVE by codegen; the VM registry (NativeRegistry.h) knows only
    // id/arity/return-category, so the full static TYPES live here (this checker is the sole guard).
    // A Result/Option return is must-use + `?`-compatible automatically (by the type NAME). A user
    // definition of the same name still wins WHERE IT IS IN SCOPE (see register_builtins -- the
    // registration is unconditional, infer_call decides). `rawRun` returns a heterogeneous [Bytes,Bytes,Int]
    // array the invariant Array[T] cannot type -- codegen (emit_wrap_process) reshapes it into the
    // prelude-declared ProcessOutput struct, so the checker types it as Result[ProcessOutput, String].
    void register_natives() {
        // `module` is the opt-in module gating the native ("" = ambient/ring = always available).
        auto add_native = [&](const std::string& name, std::vector<TyPtr> params, TyPtr ret,
                              const std::string& module = "") {
            // The name MUST be a real VM native id (else CALL_NATIVE would resolve nothing at run
            // time). This catches drift from NativeRegistry.h in the Debug test suite.
            assert(native_id_of(name) >= 0 && "static native name missing from NativeRegistry.h");
            if (!module.empty()) native_module_[name] = module;   // gate it
            FnSig sig;
            sig.params = std::move(params);
            sig.ret    = std::move(ret);
            builtin_fns_.emplace(name, std::move(sig));
        };
        const TyPtr S = ty_string();
        const TyPtr B = make_named("Bytes", {});
        // File / stdin I/O -- std::io (opt-in).
        add_native("readFile",     { S },    make_named(std_Result(), { B, S }),               STD_IO);
        add_native("writeFile",    { S, B }, make_named(std_Result(), { ty_unit(), S }),       STD_IO);
        add_native("deleteFile",   { S },    make_named(std_Result(), { ty_unit(), S }),       STD_IO);
        add_native("mkdir",        { S },    make_named(std_Result(), { ty_unit(), S }),       STD_IO);
        add_native("listDir",      { S },    make_named(std_Result(), { make_named("Array", { S }), S }), STD_IO);
        add_native("readLine",     {},       make_named(std_Option(), { S }),                  STD_IO);
        add_native("fileExists",   { S },    ty_bool(),                                        STD_IO);
        add_native("appendFile",   { S, B }, make_named(std_Result(), { ty_unit(), S }),       STD_IO);
        add_native("isFile",       { S },    ty_bool(),                                        STD_IO);
        add_native("isDir",        { S },    ty_bool(),                                        STD_IO);
        add_native("fileSize",     { S },    make_named(std_Result(), { ty_int(), S }),        STD_IO);
        add_native("rename",       { S, S }, make_named(std_Result(), { ty_unit(), S }),       STD_IO);
        add_native("copyFile",     { S, S }, make_named(std_Result(), { ty_unit(), S }),       STD_IO);
        add_native("readAllStdin", {},       S,                                                STD_IO);
        // Environment / time -- std::env (opt-in).
        add_native("getEnv",       { S },    make_named(std_Option(), { S }),                  STD_ENV);
        add_native("nanoTime",     {},       ty_int(),                                         STD_ENV);
        add_native("millisTime",   {},       ty_int(),                                         STD_ENV);
        add_native("args",         {},       make_named("Array", { S }),                       STD_ENV);
        // Ambient / ring natives (always available -- no `use` needed).
        add_native("parseInt",     { S },    make_named(std_Result(), { ty_int(),    S }));
        add_native("parseDouble",  { S },    make_named(std_Result(), { ty_double(), S }));
        // GC introspection. rawGcStats returns the 8 GcStats counters as an Array[Double] (Double's
        // 53-bit exact-integer range carries a uint64 counter for any realistic run); the prelude
        // gcStats() wrapper unpacks it into a GcStats struct. gcResetStats clears the counters. Ambient.
        add_native("rawGcStats",   {},       make_named("Array", { ty_double() }));
        add_native("gcResetStats", {},       ty_unit());
        // f64ToBytes(x) -> the 8 raw IEEE-754 bytes of x, little-endian (ambient). The one native the
        // pure-Skarn self-hosting codegen needs: it materializes a double literal's const-pool bit
        // pattern, which the 48-bit Int cannot hold (read back as two u32 halves). See NativeRegistry.h.
        add_native("f64ToBytes",   { ty_double() }, make_named("Bytes", {}));
        // Math -- std::math (opt-in). libm wrappers (Plain Double; IEEE domain errors -> NaN/inf) +
        // two predicates. An Int arg widens to Double via I2D at the call boundary.
        const TyPtr D = ty_double();
        add_native("sqrt",  { D },    D, STD_MATH);   add_native("cbrt",  { D },    D, STD_MATH);
        add_native("pow",   { D, D }, D, STD_MATH);   add_native("hypot", { D, D }, D, STD_MATH);
        add_native("exp",   { D },    D, STD_MATH);   add_native("ln",    { D },    D, STD_MATH);
        add_native("log2",  { D },    D, STD_MATH);   add_native("log10", { D },    D, STD_MATH);
        add_native("sin",   { D },    D, STD_MATH);   add_native("cos",   { D },    D, STD_MATH);
        add_native("tan",   { D },    D, STD_MATH);   add_native("asin",  { D },    D, STD_MATH);
        add_native("acos",  { D },    D, STD_MATH);   add_native("atan",  { D },    D, STD_MATH);
        add_native("atan2", { D, D }, D, STD_MATH);   add_native("abs",   { D },    D, STD_MATH);
        add_native("isNaN", { D }, ty_bool(), STD_MATH);
        add_native("isInfinite", { D }, ty_bool(), STD_MATH);
        // Process spawn -- std::process (opt-in; the prelude run/sh/... wrappers live there too, so
        // they may call rawRun as same-module code). argv is a Vec[String] (the wrappers convert a
        // List literal); the success payload is the prelude ProcessOutput struct (built by codegen
        // from the native's raw [stdout, stderr, exitCode] array).
        add_native("rawRun",       { make_named("Vec", { S }), B },
                                   make_named(std_Result(), { make_named(std_ProcessOutput(), {}), S }), STD_PROCESS);
        // The running platform as a raw id (0 Windows, 1 macOS, 2 other). Gated to std::process like
        // rawRun, so the prelude's currentOs() wrapper reaches it as same-module code; that wrapper is
        // what sh() branches on to pick the platform's shell.
        add_native("rawOsId",      {},       ty_int(),                                        STD_PROCESS);
        // TCP networking -- std::net (opt-in; the prelude connect/send/recv/... wrappers live there too,
        // so they may call the tcp* natives as same-module code). A socket is an Int descriptor; every
        // native returns a Result (success = Int/Bytes/unit, failure = a String error message).
        add_native("tcpConnect",   { S, ty_int() }, make_named(std_Result(), { ty_int(),   S }), STD_NET);
        add_native("tcpSend",      { ty_int(), B }, make_named(std_Result(), { ty_unit(),  S }), STD_NET);
        add_native("tcpRecv",      { ty_int(), ty_int() }, make_named(std_Result(), { B,    S }), STD_NET);
        add_native("tcpClose",     { ty_int() },    make_named(std_Result(), { ty_unit(),  S }), STD_NET);
        add_native("tcpListen",    { ty_int() },    make_named(std_Result(), { ty_int(),   S }), STD_NET);
        add_native("tcpAccept",    { ty_int() },    make_named(std_Result(), { ty_int(),   S }), STD_NET);
        add_native("tcpSetTimeout",{ ty_int(), ty_int() }, make_named(std_Result(), { ty_unit(), S }), STD_NET);
        add_native("tcpLocalPort", { ty_int() },    make_named(std_Result(), { ty_int(),   S }), STD_NET);
        // Hashing -- std::hash (opt-in; the pure-Skarn crc32 + hex helpers live in hash.skn, same module).
        // sha256 is a native (32-bit modular arithmetic is awkward in a 48-bit-Int language) returning the
        // raw 32-byte digest as Bytes; the prelude sha256Hex/sha256HexStr render it.
        add_native("sha256",       { B },    B,                                                 STD_HASH);
    }

    // Is a gated native `name` callable from the module currently being checked? A native NOT in
    // native_module_ is ambient (true). A gated native is reached like any other item, BY NAME: from its
    // OWN module (the prelude wrappers using their sibling native), through a glob `use m::*`, or through
    // an explicit `use m::name`. Importing the module in any other way -- `import m`, or `use m::other`
    // -- does not reach it. (Before it did: any use/import unlocked every native of m.)
    bool native_available(const std::string& name) const {
        auto it = native_module_.find(name);
        if (it == native_module_.end()) return true;                 // ambient/ring native
        const std::string& owner = it->second;
        if (cur_module_ == owner) return true;
        if (auto g = glob_modules_.find(cur_module_); g != glob_modules_.end() && g->second.count(owner))
            return true;
        if (auto u = use_map_.find(cur_module_); u != use_map_.end())
            if (auto n = u->second.find(name); n != u->second.end() && n->second == mangle(owner, name))
                return true;
        return false;
    }

    // Validate every explicit `use m::name` against what `m` really offers. Runs after register_builtins
    // because a NATIVE is not a module declaration -- only native_module_ knows that `sqrt` belongs to
    // std::math -- which is why the check once reported every native of a module with Skarn items of its
    // own as "private", and passed any name at all for a module without them (std::env, a std path that
    // does not exist). A glob skips non-pub names silently (Rust); an explicit name is a hard error.
    void check_explicit_uses() {
        auto join = [](const std::vector<std::string>& p) {
            std::string s; for (size_t i = 0; i < p.size(); ++i) { if (i) s += "::"; s += p[i]; } return s;
        };
        auto owns_native = [&](const std::string& mod) {
            for (const auto& kv : native_module_) if (kv.second == mod) return true;
            return false;
        };
        const std::string saved = cur_module_;
        for (auto& item : prog_.items) {
            if (item->kind != ItemKind::Use) continue;
            const auto& u = static_cast<const UseItem&>(*item);
            if (u.glob || u.path.empty()) continue;
            if (const char c = u.path.back()[0]; c >= 'A' && c <= 'Z') continue;   // `use m::Enum::{V}`: the enum-variant use pass
            const std::string& P = item->module_prefix;
            const std::string src = join(u.path);
            if (src == P) continue;
            cur_module_ = P;
            const auto dit = module_declared_.find(src);
            const auto own = module_declared_.find(P);
            for (const auto& n : u.names) {
                if (own != module_declared_.end() && own->second.count(n)) continue;   // reported as a conflict
                if (auto nit = native_module_.find(n); nit != native_module_.end() && nit->second == src)
                    continue;                                                         // m's own native
                if (dit != module_declared_.end() && dit->second.count(n)) {
                    if (!visible_across(mangle(src, n)))
                        error(item->line, item->col, "item '" + n + "' is private in module '" + src + "'");
                    continue;
                }
                if (auto cit = colliding_variants_.find(src); cit != colliding_variants_.end() && cit->second.count(n)) {
                    // A colliding variant must be reached through its enum (`use m::Enum::V`); this keeps the
                    // diagnosis it always had.
                    error(item->line, item->col, "item '" + n + "' is private in module '" + src + "'");
                    continue;
                }
                // A user module that cannot be resolved never gets here (the loader rejects it); a `std::`
                // path is virtual, so an unknown one is reported by the checker.
                const bool known_module = dit != module_declared_.end() || owns_native(src) ||
                                          src.rfind("std::", 0) != 0;
                error(item->line, item->col, known_module
                    ? "module '" + src + "' has no item '" + n + "'"
                    : "unknown module '" + src + "'");
            }
        }
        cur_module_ = saved;
    }

    // print(a, ...) -- variadic; each argument is rendered and written back-to-back with NO separator
    // (sugar for sequential single-arg prints). Requires at least one argument. Special-cased (not a
    // fixed FnSig) because of the variadic arity.
    TyPtr check_print(const std::vector<Expr*>& args, Expr& node) {
        if (args.empty())
            error(node.line, node.col, "print expects at least one argument");
        for (Expr* a : args) infer(*a);   // any type is printable
        return ty_unit();
    }

    // println(a, ...) -- like print but appends a single newline after the last argument; VARIADIC,
    // zero or more arguments (println() is a bare newline). Special-cased because of the variadic arity.
    TyPtr check_println(const std::vector<Expr*>& args) {
        for (Expr* a : args) infer(*a);   // any type is printable; any arg count is valid
        return ty_unit();
    }

    // panic(msg: String) -> Never -- abort with a message. Returns the bottom type Never, so it
    // composes in a match arm / anywhere a value is expected (it never actually produces one).
    TyPtr check_panic(const std::vector<Expr*>& args, Expr& node) {
        if (args.size() != 1) {
            error(node.line, node.col, "panic expects exactly one argument (the message)");
            for (Expr* a : args) infer(*a);
            return ty_never();
        }
        check_expr(*args[0], ty_string());
        return ty_never();
    }

    // ----- Map builtins (P7d) -----------------------------------------------
    // Infer arg0 and, if it is a Map[K,V], bind k/v and return true; else report + return false.
    bool map_key_value(Expr& arg0, TyPtr& k, TyPtr& v, Expr& node, const char* who) {
        TyPtr t = apply(infer(arg0));
        if (t->kind == TyKind::Named && t->name == "Map" && t->args.size() == 2) {
            k = t->args[0]; v = t->args[1];
            return true;
        }
        if (t->kind != TyKind::Error)
            error(node.line, node.col, std::string(who) + " expects a Map, found " + describe(t));
        return false;
    }

    // has(m: Map[K,V], k: K) -> Bool  /  delete(m: Map[K,V], k: K) -> Bool
    TyPtr check_map_kv_bool(const std::vector<Expr*>& args, Expr& node, const char* who) {
        if (args.size() != 2) {
            error(node.line, node.col, std::string(who) + " expects a map and a key");
            for (Expr* a : args) infer(*a);
            return ty_bool();
        }
        TyPtr k, v;
        if (map_key_value(*args[0], k, v, node, who)) check_expr(*args[1], k);
        else infer(*args[1]);
        return ty_bool();
    }

    // get(coll, i) -> Option[Elem]  -- the miss-safe TOTAL read (codegen wraps in Some/None):
    //   Map[K,V] + K   -> Option[V]   (present ? Some(value) : None)
    //   Array/Vec[T]+Int -> Option[T]   (0 <= i < len ? Some(a[i]) : None)
    //   Bytes + Int    -> Option[Int]
    // The safe counterpart to the trapping index operators `m[k]` / `a[i]`. Element-type logic
    // mirrors `infer_index` exactly, so `get` and `[]` stay consistent (List is not indexable).
    TyPtr check_get(const std::vector<Expr*>& args, Expr& node) {
        if (args.size() != 2) {
            error(node.line, node.col, "get expects a collection and an index/key");
            for (Expr* a : args) infer(*a);
            return make_named(std_Option(), { ty_error() });
        }
        TyPtr o = apply(infer(*args[0]));
        if (o->kind == TyKind::Named) {
            if (o->name == "Map" && o->args.size() == 2) {
                check_expr(*args[1], o->args[0]);
                return make_named(std_Option(), { o->args[1] });
            }
            if ((o->name == "Array" || o->name == "Vec") && o->args.size() == 1) {
                check_expr(*args[1], ty_int());
                return make_named(std_Option(), { o->args[0] });
            }
            if (o->name == "Bytes") {
                check_expr(*args[1], ty_int());
                return make_named(std_Option(), { ty_int() });
            }
        }
        if (o->kind != TyKind::Error)
            error(node.line, node.col, "get expects a Map, Array, Vec, or Bytes, found " + describe(o));
        infer(*args[1]);
        return make_named(std_Option(), { ty_error() });
    }

    // Internal live-map cursor primitives (behind the prelude MapCursor), over MAP_ITER_NEXT /
    // MAP_KEY_AT / MAP_VAL_AT. mode: 0 = mapIterNext -> Int, 1 = mapKeyAt -> K, 2 = mapValAt -> V.
    //   mapIterNext(m: Map[K,V], i: Int) -> Int  (next live pair-index >= i, or -1)
    //   mapKeyAt(m: Map[K,V], i: Int) -> K  /  mapValAt(m: Map[K,V], i: Int) -> V
    TyPtr check_map_iter(const std::vector<Expr*>& args, Expr& node, const char* who, int mode) {
        if (args.size() != 2) {
            error(node.line, node.col, std::string(who) + " expects a map and an index");
            for (Expr* a : args) infer(*a);
            return mode == 0 ? ty_int() : ty_error();
        }
        TyPtr k, v;
        if (map_key_value(*args[0], k, v, node, who)) {
            check_expr(*args[1], ty_int());
            if (mode == 1) return k;
            if (mode == 2) return v;
            return ty_int();
        }
        infer(*args[1]);
        return mode == 0 ? ty_int() : ty_error();
    }

    // keys(m: Map[K,V]) -> Array[K]  /  values(m: Map[K,V]) -> Array[V]
    TyPtr check_map_collect(const std::vector<Expr*>& args, Expr& node, const char* who, bool want_key) {
        if (args.size() != 1) {
            error(node.line, node.col, std::string(who) + " expects a map");
            for (Expr* a : args) infer(*a);
            return make_named("Array", { ty_error() });
        }
        TyPtr k, v;
        if (map_key_value(*args[0], k, v, node, who)) return make_named("Array", { want_key ? k : v });
        return make_named("Array", { ty_error() });
    }

    // A monomorphic one-argument builtin: `who(x: from) -> to` (Bytes conversions).
    TyPtr check_mono1(const std::vector<Expr*>& args, Expr& node, const char* who, TyPtr from, TyPtr to) {
        if (args.size() != 1) {
            error(node.line, node.col, std::string(who) + " expects exactly one argument");
            for (Expr* a : args) infer(*a);
            return to;
        }
        check_expr(*args[0], from);
        return to;
    }

    // bytes() / bytes(n: Int) -> Bytes -- an empty (or capacity-hinted) growable byte buffer.
    TyPtr check_bytes(const std::vector<Expr*>& args, Expr& node) {
        if (args.size() > 1) error(node.line, node.col, "bytes expects zero or one (Int capacity) argument");
        if (args.size() == 1) check_expr(*args[0], ty_int());
        return make_named("Bytes", {});
    }

    // The `len` builtin: accepts any container / string, returns Int. Special-cased (not a rigid
    // FnSig) because it is polymorphic over the container kinds the checker cannot express as one
    // generic signature. Codegen lowers it to the kind-dispatching LEN opcode.
    TyPtr check_len(const std::vector<Expr*>& args, Expr& node) {
        if (args.size() != 1) {
            error(node.line, node.col, "len expects exactly one argument");
            for (Expr* a : args) infer(*a);
            return ty_int();
        }
        // len(K) is an allowed read site for a const array: authorize this exact argument.
        const Expr* prev = const_array_ok_node_;
        const_array_ok_node_ = args[0];
        TyPtr t = apply(infer(*args[0]));
        const_array_ok_node_ = prev;
        // Matches the VM's LEN opcode: containers (KIND_ARRAY/Map/Vec/Bytes) + String. A String's
        // length is its BYTE count (byte strings, no UTF-8 code-point counting) -- identical to
        // len(toBytes(s)); a code-point count would need a separate UTF-8-decoding builtin.
        bool ok = t->kind == TyKind::String ||
                  (t->kind == TyKind::Named &&
                   (t->name == "List" || t->name == "Array" || t->name == "Vec" ||
                    t->name == "Bytes" || t->name == "Map"));
        if (!ok && t->kind != TyKind::Error)
            error(node.line, node.col, "len expects a String or container (List/Array/Vec/Bytes/Map), found " + describe(t));
        return ty_int();
    }

    // The `ordinal` builtin: an int-backed enum -> its Int DISCRIMINANT (so with `enum S : Int
    // { Ok = 200 }`, `ordinal(Ok)` is 200, not the declaration position 0 -- Java's `ordinal()` means
    // the position, but Java has no pinning and Skarn does). Special-cased for the same reason as
    // `len` above: it accepts an open FAMILY of types -- every `enum E : Int` -- which no rigid FnSig
    // can name. It is the projection the OTHER two erasure customers already have: a transparent
    // struct (and hence Char) has `.0`, and a nullary variant has no field for `.0` to reach.
    //
    // Deliberately NOT `check_mono1`, and the second reason is the one that bites: besides taking a
    // single fixed argument type, it reaches the argument through `check_expr(a, from)` -- the
    // COERCING path, the one that records `Expr::widen_double`. A fixed `from` would therefore both
    // accept a bare Int and plant a widening mark on a node codegen must not widen. `infer` first,
    // then inspect, exactly as `check_len` does.
    TyPtr check_ordinal(const std::vector<Expr*>& args, Expr& node) {
        if (args.size() != 1) {
            error(node.line, node.col, "ordinal expects exactly one argument");
            for (Expr* a : args) infer(*a);
            return ty_int();
        }
        TyPtr t = apply(infer(*args[0]));
        // For a TyKind::Named the `name` IS the mangled `enums_` key (as in `is_eq` /
        // `constructor_space`); no `mangle_ref` -- that is only for a SYNTACTIC name.
        bool ok = false;
        if (t->kind == TyKind::Named) {
            auto eit = enums_.find(t->name);
            ok = eit != enums_.end() && eit->second.is_int_backed;
        }
        if (!ok && t->kind != TyKind::Error)
            error(node.line, node.col, "ordinal expects an int-backed enum (one declared `enum ... : Int`), found " +
                  describe(t));
        // `ty_int()` even on the error path, like `check_len`: the result type is known regardless, so
        // returning `ty_error()` would let one bad `ordinal` cascade into its whole surrounding expression.
        return ty_int();
    }

    // push(Bytes, Int) -> Bytes  |  push(Vec[T], T) -> Vec[T]. Polymorphic over the two growable
    // buffers (the VM's VEC_PUSH is tri-kind -- bytes_push masks the value & 0xFF -- so a Bytes
    // receiver is sound under erasure). One generic FnSig cannot express both, so it is special-cased.
    TyPtr check_push(const std::vector<Expr*>& args, Expr& node) {
        if (args.size() != 2) {
            error(node.line, node.col, "push expects exactly two arguments");
            for (Expr* a : args) infer(*a);
            return ty_error();
        }
        require_mut_arg(args[0], "push (it mutates its receiver)");   // receiver must be `mut`
        TyPtr t0 = apply(infer(*args[0]));
        if (t0->kind == TyKind::Named && t0->name == "Bytes") {
            check_expr(*args[1], ty_int());                 // a byte value (0..255, masked at run time)
            return make_named("Bytes", {});
        }
        TyPtr elem = tc_.fresh_var();                       // generic Vec path (unchanged semantics)
        if (!tc_.unify(t0, make_named("Vec", { elem })) && t0->kind != TyKind::Error)
            error(node.line, node.col, "push expects a Vec or Bytes, found " + describe(t0));
        check_expr(*args[1], apply(elem));
        return apply(make_named("Vec", { elem }));
    }

    // pop(Bytes) -> Option[Int]  |  pop(Vec[T]) -> Option[T]. The Bytes sibling of check_push.
    TyPtr check_pop(const std::vector<Expr*>& args, Expr& node) {
        if (args.size() != 1) {
            error(node.line, node.col, "pop expects exactly one argument");
            for (Expr* a : args) infer(*a);
            return make_named(std_Option(), { ty_error() });
        }
        require_mut_arg(args[0], "pop (it mutates its receiver)");   // receiver must be `mut`
        TyPtr t0 = apply(infer(*args[0]));
        if (t0->kind == TyKind::Named && t0->name == "Bytes")
            return make_named(std_Option(), { ty_int() });
        TyPtr elem = tc_.fresh_var();
        if (!tc_.unify(t0, make_named("Vec", { elem })) && t0->kind != TyKind::Error)
            error(node.line, node.col, "pop expects a Vec or Bytes, found " + describe(t0));
        return make_named(std_Option(), { apply(elem) });
    }

    // appendBytes(Bytes, String) -> Bytes  |  appendBytes(Bytes, Bytes) -> Bytes. The bulk byte-blit
    // (VM BYTES_APPEND): arg0 is the dst buffer, arg1 the WHOLE source (a String OR a Bytes). Result is
    // the dst Bytes (chainable). One name over the two source types => special-cased like push.
    TyPtr check_append_bytes(const std::vector<Expr*>& args, Expr& node) {
        if (args.size() != 2) {
            error(node.line, node.col, "appendBytes expects exactly two arguments");
            for (Expr* a : args) infer(*a);
            return make_named("Bytes", {});
        }
        require_mut_arg(args[0], "appendBytes (it mutates its destination)");   // dst must be `mut`
        check_expr(*args[0], make_named("Bytes", {}));    // dst must be a Bytes buffer
        TyPtr t1 = apply(infer(*args[1]));
        const bool src_ok = t1->kind == TyKind::String ||
                            (t1->kind == TyKind::Named && t1->name == "Bytes");
        if (!src_ok && t1->kind != TyKind::Error)
            error(args[1]->line, args[1]->col,
                  "appendBytes source must be a String or Bytes, found " + describe(t1));
        return make_named("Bytes", {});
    }

    // _appendBytesRange(Bytes, Bytes, Int, Int) -> Bytes. Internal (the prelude slice/_appendRange/
    // readBytes); the bulk sub-range blit (VM BYTES_APPEND_RANGE). arg0 dst, arg1 src, arg2 lo, arg3 hi.
    TyPtr check_append_bytes_range(const std::vector<Expr*>& args, Expr& node) {
        if (args.size() != 4) {
            error(node.line, node.col, "_appendBytesRange expects exactly four arguments");
            for (Expr* a : args) infer(*a);
            return make_named("Bytes", {});
        }
        require_mut_arg(args[0], "_appendBytesRange (it mutates its destination)");   // dst must be `mut`
        check_expr(*args[0], make_named("Bytes", {}));
        check_expr(*args[1], make_named("Bytes", {}));
        check_expr(*args[2], ty_int());
        check_expr(*args[3], ty_int());
        return make_named("Bytes", {});
    }

    void register_ctor(const std::string& name, uint32_t line, uint32_t col) {
        if (!ctor_names_.insert(name).second)
            error(line, col, "duplicate constructor '" + name + "'");
    }

    // ----- generic scope ----------------------------------------------------

    // Resolve a parameter's syntactic bound refs into semantic `Bound`s: keep the
    // trait name, resolve each type argument in the given scope. Called from
    // make_generics's sub-pass 2, where `env` already holds ALL sibling type params, so a
    // bound may reference any sibling regardless of order (`[I: Iterable[T], T]`). A name
    // that is neither a sibling nor a known type still errors as an unknown type.
    std::vector<Bound> resolve_bounds(const std::vector<BoundRef>& brs,
                                      const std::unordered_map<std::string, TyPtr>& env) {
        std::vector<Bound> out;
        out.reserve(brs.size());
        for (const auto& br : brs) {
            Bound b;
            b.trait = mangle_ref(br.trait);   // resolve the bound trait through module/ring scope
                                              // (e.g. `I: Iterable` in std::iter -> std::iter::Iterable)
            // ...and write it back onto the AST, mirroring the impl-target writeback below: CODEGEN
            // reads `BoundRef::trait` straight from the AST (Codegen.cpp, BlanketDef::bound_traits)
            // and matches it against the MANGLED trait names in its impl model. Resolving only into
            // the semantic `Bound` left the two disagreeing, so a blanket impl whose bound trait is
            // declared in a MODULE never fired -- `impl[T: Show] Dbg for T` compiled and then trapped
            // at run time with "no trait implementation for this type". Invisible while the entry
            // program mangled bare (there the two names coincide); universal once it did not.
            // Idempotent: mangle_ref of an already-mangled name is identity, and make_generics runs
            // over the same AST node more than once.
            const_cast<BoundRef&>(br).trait = b.trait;
            for (const auto& a : br.args) b.args.push_back(resolve_type(*a, env));
            out.push_back(std::move(b));
        }
        return out;
    }

    // Build the rigid vars for a generic-param list and the name->Ty env used to
    // resolve the signature's types. Reports duplicate type parameters. TWO sub-passes so
    // a bound may reference ANY sibling type param regardless of declaration order
    // (`[I: Iterable[T], T]` -- `I`'s bound names the later sibling `T`).
    std::vector<GenericInfo> make_generics(const std::vector<GenericParam>& gps,
                                           std::unordered_map<std::string, TyPtr>& env,
                                           uint32_t line, uint32_t col, bool report = true) {
        // Sub-pass 1: create every rigid var with EMPTY bounds and add it to `env`, so all
        // sibling names are visible before any bound is resolved.
        std::vector<GenericInfo> out;
        std::vector<const GenericParam*> accepted;   // parallel to `out`, for sub-pass 2
        for (const auto& gp : gps) {
            if (env.count(gp.name)) {
                if (report) error(line, col, "duplicate type parameter '" + gp.name + "'");
                continue;
            }
            TyPtr v = tc_.rigid_var(gp.name);
            env.emplace(gp.name, v);
            out.push_back(GenericInfo{ gp.name, {}, v->var_id, v });
            accepted.push_back(&gp);
        }
        // Sub-pass 2: resolve each accepted param's bounds against the FULL env, then attach
        // them to that param's rigid var (the SAME object) and its GenericInfo.
        for (size_t i = 0; i < out.size(); ++i) {
            std::vector<Bound> bs = resolve_bounds(accepted[i]->bounds, env);
            tc_.set_var_bounds(out[i].var, bs);
            out[i].bounds = std::move(bs);
        }
        return out;
    }

    // ----- resolve_type -----------------------------------------------------

    TyPtr resolve_type(const Type& t, const std::unordered_map<std::string, TyPtr>& env) {
        switch (t.kind) {
            case TypeKind::Fn: {
                const auto& ft = static_cast<const FnType&>(t);
                std::vector<TyPtr> ps;
                for (const auto& p : ft.params) ps.push_back(resolve_type(*p, env));
                return make_fn(std::move(ps), ft.ret ? resolve_type(*ft.ret, env) : ty_error());
            }
            case TypeKind::Tuple: {
                const auto& tt = static_cast<const TupleType&>(t);
                if (tt.elems.empty()) return ty_unit();
                std::vector<TyPtr> es;
                for (const auto& e : tt.elems) es.push_back(resolve_type(*e, env));
                return make_tuple(std::move(es));
            }
            case TypeKind::Dyn: {
                // `dyn Trait[...]` -- a trait object (existential). The trait must exist and be
                // OBJECT-SAFE, and a parametric trait's params must be written out in full: there
                // is no impl here to solve them from (the whole point is that the concrete type is
                // hidden), so `dyn Iterable` alone would mean "some unknown element type" -- useless
                // and unsound. Contrast `T: Iterable[E]`, where the impl match solves E.
                const auto& dt = static_cast<const DynType&>(t);
                std::vector<TyPtr> args;
                for (const auto& a : dt.args) args.push_back(resolve_type(*a, env));

                std::string key;
                if (!dt.qualifier.empty()) {
                    const bool is_std = (dt.qualifier == "std");
                    if (!is_std && !imported_by_current(dt.qualifier))
                        error(t.line, t.col, "module '" + dt.qualifier + "' is not imported");
                    key = is_std ? resolve_std(dt.trait) : (dt.qualifier + "::" + dt.trait);
                    if (!is_std && !visible_across(key))              // default private
                        error(t.line, t.col, "trait '" + dt.trait + "' is private in module '" + dt.qualifier + "'");
                } else {
                    key = mangle_ref(dt.trait);
                }
                auto it = traits_.find(key);
                if (it == traits_.end()) {
                    error(t.line, t.col, "unknown trait '" + dt.trait + "' after 'dyn' (a trait object "
                                         "names a TRAIT, not a type)" + missing_use_hint(dt.trait));
                    return ty_error();
                }
                const size_t want = it->second.generics.size();
                if (args.size() != want) {
                    error(t.line, t.col, "trait object 'dyn " + dt.trait + "' needs " + std::to_string(want) +
                          " type argument(s), got " + std::to_string(args.size()) +
                          (want > 0 && args.empty()
                               ? "; a trait object must name them (there is no impl here to infer them from)"
                               : ""));
                    return ty_error();
                }
                if (auto why = object_safety_error(key)) {
                    error(t.line, t.col, "trait '" + dt.trait + "' is not object-safe, so 'dyn " +
                          dt.trait + "' is not a valid type: " + *why);
                    return ty_error();
                }
                return make_dyn(key, std::move(args));
            }
            case TypeKind::Named: {
                const auto& nt = static_cast<const NamedType&>(t);
                std::vector<TyPtr> args;
                for (const auto& a : nt.args) args.push_back(resolve_type(*a, env));

                // A module-qualified type `mod::Name` is always a user struct/enum: resolve it
                // against the named module directly. A primitive/generic-param/builtin never
                // carries a qualifier, so bypass those fast-paths below -- this also stops
                // `mod::Int` from matching the primitive `Int`. (No AST writeback: a type resolves
                // via the returned `Ty`, whose name codegen reads, not through the AST node.)
                if (!nt.qualifier.empty()) {
                    const bool is_std = (nt.qualifier == "std");
                    if (!is_std && !imported_by_current(nt.qualifier))
                        error(t.line, t.col, "module '" + nt.qualifier + "' is not imported");
                    const std::string key = is_std ? resolve_std(nt.name) : (nt.qualifier + "::" + nt.name);
                    if (!is_std && (structs_.count(key) || enums_.count(key)) && !visible_across(key))
                        error(t.line, t.col, "type '" + nt.name + "' is private in module '" + nt.qualifier + "'");
                    if (auto it = structs_.find(key); it != structs_.end()) {
                        check_arity(t, nt.name, it->second.generics.size(), args.size());
                        return make_named(key, std::move(args));
                    }
                    if (auto it = enums_.find(key); it != enums_.end()) {
                        check_arity(t, nt.name, it->second.generics.size(), args.size());
                        return make_named(key, std::move(args));
                    }
                    error(t.line, t.col, "unknown type '" + nt.qualifier + "::" + nt.name + "'");
                    return ty_error();
                }

                if (nt.name == "Self") {
                    auto it = env.find("Self");
                    if (it == env.end()) {
                        error(t.line, t.col, "'Self' is only valid inside a trait or impl");
                        return ty_error();
                    }
                    if (!args.empty())
                        error(t.line, t.col, "'Self' takes no type arguments");
                    return it->second;
                }
                if (auto it = env.find(nt.name); it != env.end()) {   // a generic param
                    if (!args.empty())
                        error(t.line, t.col, "type parameter '" + nt.name + "' takes no type arguments");
                    return it->second;
                }
                if (is_primitive_type(nt.name)) {
                    if (!args.empty())
                        error(t.line, t.col, "'" + nt.name + "' takes no type arguments");
                    return primitive_ty(nt.name);
                }
                if (int ar = builtin_type_arity(nt.name); ar >= 0) {
                    check_arity(t, nt.name, static_cast<size_t>(ar), args.size());
                    return make_named(nt.name, std::move(args));
                }
                const std::string key = mangle_ref(nt.name);   // module-qualify a user type ref
                if (auto it = structs_.find(key); it != structs_.end()) {
                    check_arity(t, nt.name, it->second.generics.size(), args.size());
                    return make_named(key, std::move(args));
                }
                if (auto it = enums_.find(key); it != enums_.end()) {
                    check_arity(t, nt.name, it->second.generics.size(), args.size());
                    return make_named(key, std::move(args));
                }
                error(t.line, t.col, "unknown type '" + nt.name + "'" + missing_use_hint(nt.name));
                return ty_error();
            }
        }
        return ty_error();
    }

    void check_arity(const Type& t, const std::string& name, size_t expect, size_t got) {
        if (expect != got)
            error(t.line, t.col, "type '" + name + "' expects " + std::to_string(expect) +
                                 " type argument(s), got " + std::to_string(got));
    }

    // ----- pass 1b: contents (field/variant/param/return/method-sig types) --
    // Order-independent by construction: every type/trait arity is already registered
    // (pass 1a invariant), and no resolve_* reads another item's rigid vars or contents
    // -- only arities via resolve_type. So the declaration-order loop below is safe even
    // for mutually-recursive / forward-referencing generic types.

    void resolve_signatures() {
        for (const auto& item : prog_.items) {
            cur_module_ = item->module_prefix;   // resolve this item's signature types in its module
            switch (item->kind) {
                case ItemKind::Struct: resolve_struct(static_cast<const StructItem&>(*item)); break;
                case ItemKind::Enum:   resolve_enum(static_cast<const EnumItem&>(*item));     break;
                case ItemKind::Fn:     resolve_fn(static_cast<const FnItem&>(*item));         break;
                case ItemKind::Trait:  resolve_trait(static_cast<const TraitDecl&>(*item));   break;
                case ItemKind::Impl:   resolve_impl(static_cast<const ImplDecl&>(*item));     break;
                case ItemKind::Const:  resolve_const(static_cast<const ConstItem&>(*item));   break;
                case ItemKind::Stmt:   break;
                case ItemKind::Import:                                                        // module items:
                case ItemKind::Use:    break;                                                 // resolved in mangle_declarations
            }
        }
    }

    void resolve_struct(const StructItem& s) {
        auto it = structs_.find(s.name);
        if (it == structs_.end()) return;   // a duplicate; skip
        StructInfo& si = it->second;
        std::unordered_map<std::string, TyPtr> env;
        si.generics = make_generics(s.generics, env, s.line, s.col);
        for (const auto& f : s.fields) {
            si.field_names.push_back(f.name);
            si.field_types.push_back(f.type ? resolve_type(*f.type, env) : ty_error());
        }
        if (si.is_transparent) {
            // A transparent (erased) struct must be a single-field TUPLE struct over a primitive
            // immediate. Its value IS that field's immediate at runtime, so >1 field, a heap/named
            // field, a brace (record) form, or generics cannot erase.
            bool ok = si.is_tuple && s.generics.empty() && si.field_types.size() == 1;
            if (ok) {
                TyKind fk = si.field_types[0]->kind;
                ok = (fk == TyKind::Int || fk == TyKind::Double || fk == TyKind::Bool);
            }
            if (!ok) {
                error(s.line, s.col, "a transparent struct must be a single-field tuple struct "
                      "`Name(T)` over an Int, Double, or Bool field, with no type parameters");
            } else {
                // The erased value is an immediate whose underlying primitive is Hashable, so the
                // newtype is a valid map/Set key. Synthesize the marker-trait impl key (a user
                // `impl Hashable for UserId` is forbidden) so BOTH Map construction and a
                // `T: Hashable` bound (Set[UserId]) discharge through the normal path. This does
                // NOT make the newtype inherit any OTHER trait of its underlying type. Skipped
                // when no `Hashable` trait is in scope (--no-prelude), like require_hashable_key.
                const std::string hashable = mangle_ref("Hashable");
                if (traits_.find(hashable) != traits_.end())
                    impl_keys_.insert(hashable + "\x1f" + si.name);
            }
        }
    }

    void resolve_enum(const EnumItem& e) {
        auto it = enums_.find(e.name);
        if (it == enums_.end()) return;
        EnumInfo& ei = it->second;
        std::unordered_map<std::string, TyPtr> env;
        ei.generics = make_generics(e.generics, env, e.line, e.col);
        uint32_t idx = 0;
        // Int-backed discriminant assignment (declaration-order counter, overridable by `= v`;
        // the counter resumes at v+1). Applied identically in Codegen::register_program_types.
        constexpr int64_t MIN_48 = -(int64_t(1) << 47), MAX_48 = (int64_t(1) << 47) - 1;
        int64_t next_disc = 0;
        std::unordered_map<int64_t, std::string> seen_disc;
        for (const auto& v : e.variants) {
            ei.variant_names.push_back(v.name);
            auto vit = variants_.find(v.name);
            VariantInfo* vip = (vit != variants_.end() && vit->second.enum_name == e.name) ? &vit->second : nullptr;
            if (vip) {
                vip->index = idx;
                for (const auto& f : v.fields) {
                    vip->field_names.push_back(f.name);
                    vip->field_types.push_back(f.type ? resolve_type(*f.type, env) : ty_error());
                }
            }
            if (e.is_int_backed) {
                if (!v.fields.empty())   // a tuple/record payload cannot erase to a single Int
                    error(v.line, v.col, "an int-backed enum variant must be nullary; '" + v.name +
                          "' has a payload (a `: Int` enum erases to a bare Int discriminant)");
                int64_t d = v.has_disc ? v.disc : next_disc;
                if (d < MIN_48 || d > MAX_48)
                    error(v.line, v.col, "enum discriminant for '" + v.name + "' is out of the 48-bit Int range");
                auto dup = seen_disc.find(d);
                if (dup != seen_disc.end())
                    error(v.line, v.col, "duplicate enum discriminant " + std::to_string(d) +
                          " on '" + v.name + "' (already used by '" + dup->second + "')");
                else seen_disc.emplace(d, v.name);
                if (vip) vip->disc = d;
                next_disc = d + 1;
            } else if (v.has_disc) {
                error(v.line, v.col, "an explicit discriminant `= " + std::to_string(v.disc) +
                      "` is only allowed on an int-backed enum (`enum " + e.name + " : Int`)");
            }
            ++idx;
        }
        if (e.is_int_backed) {
            if (e.repr != "Int")
                error(e.line, e.col, "an int-backed enum repr must be `Int` (found `" + e.repr + "`)");
            if (!e.generics.empty())
                error(e.line, e.col, "an int-backed enum cannot have type parameters");
            // The erased value is a bare Int -> a valid map/Set key. Synthesize the Hashable
            // marker-impl key for the ENUM type, exactly like resolve_struct does for a transparent
            // struct (a user `impl Hashable for Color` stays forbidden). Skipped under --no-prelude.
            const std::string hashable = mangle_ref("Hashable");
            if (traits_.find(hashable) != traits_.end())
                impl_keys_.insert(hashable + "\x1f" + e.name);
        }
    }

    // Resolve a module const's declared type (consts are non-generic -> empty env). The value
    // literal is type-checked against this in check_const_body.
    void resolve_const(const ConstItem& c) {
        auto it = consts_.find(c.name);
        if (it == consts_.end()) return;   // a duplicate; skip
        std::unordered_map<std::string, TyPtr> env;
        it->second = c.type ? resolve_type(*c.type, env) : ty_error();
    }

    // True iff t is a fixed-size Array[E].
    bool is_array_ty(const TyPtr& t0) {
        TyPtr t = apply(t0);
        return t->kind == TyKind::Named && t->name == "Array" && t->args.size() == 1;
    }

    // A permitted const-array element literal: a numeric/bool literal, or a negated numeric literal.
    static bool is_const_array_element(const Expr& e) {
        if (e.kind == ExprKind::IntLit || e.kind == ExprKind::DoubleLit || e.kind == ExprKind::BoolLit)
            return true;
        if (e.kind == ExprKind::Unary) {
            const auto& u = static_cast<const UnaryExpr&>(e);
            return u.op == TokKind::Minus && u.operand &&
                   (u.operand->kind == ExprKind::IntLit || u.operand->kind == ExprKind::DoubleLit);
        }
        return false;
    }

    // Type-check a const's initializer. A SCALAR const inlines a literal at each use (exact kind
    // match). An ARRAY const `const NAME: Array[T] = [lit, ...]` is a pre-built, shared, immutable
    // table (Route 2): scalar literal elements only, `pub` forbidden (module-private -- see the
    // read-only guard below), and it is a non-escaping read-only value.
    void check_const_body(ConstItem& c) {
        auto it = consts_.find(c.name);
        if (it == consts_.end() || !it->second) return;
        TyPtr declared = apply(it->second);

        if (is_array_ty(declared)) {
            // A const array is a module-private implementation table: `pub` is forbidden (share via
            // an accessor fn instead). This also keeps the read-only guard's surface intra-module.
            if (c.is_pub)
                error(c.line, c.col,
                      "a const array cannot be 'pub': it is a module-private implementation table -- "
                      "expose an accessor function (`pub fn`) that reads it instead");
            check_const_array_body(c, declared->args[0]);
            return;
        }

        cur_generic_env_.clear();
        lambda_floor_ = 0; lambda_is_let_bound_ = false;   // see check_fn_body
        push_scope();
        TyPtr got = apply(infer(*c.value));
        pop_scope();
        // Require an EXACT kind match (no Int<:Double coercion): the value is inlined verbatim at each
        // use site, so an Int literal in a `Double` const would place Int bits in a Double slot. The
        // user writes `3.0` for a Double const. (char literals are Ints -> an `Int` const accepts them.)
        if (declared->kind != TyKind::Error && got->kind != TyKind::Error && got->kind != declared->kind)
            error(c.value->line, c.value->col,
                  "const '" + c.name + "' has a " + describe(got) + " initializer but is declared " +
                  describe(declared));
    }

    // Validate a `const NAME: Array[T] = [...]` body: T must be a scalar (Int/Double/Bool -- v1 has
    // no String-element tables), the initializer must be an array literal, and every element must be
    // a scalar literal of kind T (checked with the ordinary literal path, so the 48-bit range check
    // and Int<:Double element coercion both apply). Sets the ListLit's type to Array[T].
    void check_const_array_body(ConstItem& c, TyPtr elemTy0) {
        TyPtr elemTy = apply(elemTy0);
        const bool scalar = elemTy->kind == TyKind::Int || elemTy->kind == TyKind::Double ||
                            elemTy->kind == TyKind::Bool;
        if (!scalar && elemTy->kind != TyKind::Error) {
            error(c.line, c.col, "a const array element type must be Int, Double, or Bool (found " +
                  describe(elemTy) + ")");
            return;
        }
        if (c.value->kind != ExprKind::ListLit) {
            error(c.value->line, c.value->col,
                  "a const array initializer must be an array literal [ ... ]");
            return;
        }
        auto& lst = static_cast<ListLit&>(*c.value);
        if (lst.elems.empty()) {
            error(c.value->line, c.value->col, "a const array must have at least one element");
            return;
        }
        cur_generic_env_.clear();
        push_scope();
        for (auto& el : lst.elems) {
            if (!is_const_array_element(*el))
                error(el->line, el->col, "a const array element must be a scalar literal");
            else
                check_expr(*el, elemTy);   // range/coercion check + sets el->ty
        }
        pop_scope();
        lst.ty = make_named("Array", { apply(elemTy) });
    }

    // The read-only guard for const arrays (Route 2). A const array is ONE shared heap instance, so
    // it is sound only if it can never be mutated -- and Skarn tracks mutability on bindings, not
    // values, so the only sound v1 rule is NON-ESCAPING: a const-array reference may appear only at a
    // direct read site (`K[i]`, `for x in K`, `len(K)`) and nowhere else (no binding, no passing, no
    // returning, no storing, no capture, no mut receiver). infer_ident emits this error by DEFAULT for
    // an Array-typed const; an allowed parent authorizes its ONE direct child via const_array_ok_node_
    // (an exact-pointer token, so a nested `foo(K)` is NOT authorized -- fail-closed).
    const Expr* const_array_ok_node_ = nullptr;

    TyPtr const_ref_type(IdentExpr& e, const TyPtr& t) {
        TyPtr ct = apply(t);
        if (is_array_ty(ct) && &e != const_array_ok_node_)
            error(e.line, e.col,
                  "a const array is read-only and cannot escape its use site: index it (K[i]), "
                  "iterate it (for x in K), or take its len(K) directly -- it cannot be bound, "
                  "passed, returned, stored, or captured");
        return ct;
    }

    void resolve_fn(const FnItem& f) {
        auto it = fns_.find(f.name);
        if (it == fns_.end()) return;
        FnSig& sig = it->second;
        sig.line = f.line; sig.col = f.col;
        // Declaration site, for the call-site note caret. The module comes from the ITEM rather than
        // `cur_module_`, so the note routes to the right file regardless of which pass resolves it.
        sig.origin = { &f.params, f.module_prefix };
        std::unordered_map<std::string, TyPtr> env;
        sig.generics = make_generics(f.generics, env, f.line, f.col);
        for (const auto& p : f.params) {
            sig.params.push_back(p.type ? resolve_type(*p.type, env) : ty_error());
            sig.params_mut.push_back(p.is_mut);
        }
        sig.ret = f.ret ? resolve_type(*f.ret, env) : ty_error();
    }

    void resolve_trait(const TraitDecl& t) {
        auto it = traits_.find(t.name);
        if (it == traits_.end()) return;
        TraitInfo& ti = it->second;
        // Module-qualify supertrait references (+ writeback for codegen / the tree-shaker).
        auto& td = const_cast<TraitDecl&>(t);
        for (auto& s : td.supertraits) s = mangle_ref(s);
        ti.supertraits = td.supertraits;
        TyPtr self = tc_.rigid_var("Self");
        ti.self_id = self->var_id;
        std::unordered_map<std::string, TyPtr> env;
        env.emplace("Self", self);
        ti.generics = make_generics(t.generics, env, t.line, t.col);   // trait type params (`Iterable[T]`)
        for (const auto& m : t.methods)
            ti.methods.push_back(resolve_method(m, env, t.module_prefix));
    }

    void resolve_impl(const ImplDecl& im) {
        std::unordered_map<std::string, TyPtr> env;
        std::vector<GenericInfo> gens = make_generics(im.generics, env, im.line, im.col);
        ImplInfo info;
        info.ast = &im;
        info.module = im.module_prefix;   // for the orphan rule
        const_cast<ImplDecl&>(im).trait_name = mangle_ref(im.trait_name);   // module-qualify (+ writeback)
        info.trait_name = im.trait_name;
        info.line = im.line; info.col = im.col;
        for (const auto& a : im.trait_args)              // `impl[X] Iterable[X] for …`
            info.trait_args.push_back(resolve_type(*a, env));
        if (im.target && im.target->kind == TypeKind::Named) {
            // Module-qualify the target head: the coherence key (impl_keys_), the trait-satisfaction
            // lookup (against the mangled receiver type), AND codegen's trait table (find_struct on the
            // head) all key by the MANGLED type name. resolve_type is const, so writeback here on the AST
            // NamedType so codegen reads the mangled head too. A generic-param target (blanket `for T`) is
            // unaffected (mangle_ref of a non-declared name is identity). cur_module_ is the impl's module.
            auto& nt = const_cast<NamedType&>(static_cast<const NamedType&>(*im.target));
            nt.name = mangle_ref(nt.name);
            info.head = nt.name;
            info.target = resolve_type(*im.target, env);
            // Blanket impl: the target head is one of the impl's own generic params (`for T`),
            // not a concrete type. Record it + that param's bounds for the discharge fallback.
            for (const auto& g : gens)
                if (g.name == nt.name) { info.is_blanket = true; info.blanket_bounds = g.bounds; break; }
        } else {
            error(im.line, im.col, "impl target must be a named type");
            info.target = ty_error();
        }
        info.generics = std::move(gens);   // kept for parametric-trait impl matching (trait_args_for)
        impls_.push_back(std::move(info));
    }

    // `base_env` carries `Self` and the trait's own type params, in scope for the
    // method's parameter/return types (`fn iter(self) -> Vec[T]`).
    // `module` is the ENCLOSING trait/impl's module prefix: `Method` is not an `Item`, so it cannot
    // carry one, and it is needed to route the parameter note caret to the declaring file.
    MethodSig resolve_method(const Method& m, const std::unordered_map<std::string, TyPtr>& base_env,
                             const std::string& module) {
        MethodSig ms;
        ms.name = m.name;
        ms.has_self = m.has_self;
        ms.self_mut = m.self_mut;
        ms.param_count = m.params.size();
        ms.has_default = (m.body != nullptr);
        ms.line = 0; ms.col = 0;
        // `Method::params` is the non-self params, so it aligns 1:1 with `ms.params` below.
        ms.origin = { &m.params, module };         // declaration site, for the call-site note caret
        std::unordered_map<std::string, TyPtr> env = base_env;
        ms.generics = make_generics(m.generics, env, 0, 0);
        for (const auto& p : m.params) {
            ms.params.push_back(p.type ? resolve_type(*p.type, env) : ty_error());
            ms.params_mut.push_back(p.is_mut);
        }
        ms.ret = m.ret ? resolve_type(*m.ret, env) : ty_error();
        return ms;
    }

    // ----- trait checks -----------------------------------------------------

    void check_traits() {
        // Unknown supertraits.
        for (auto& kv : traits_) {
            TraitInfo& ti = kv.second;
            for (const auto& s : ti.supertraits)
                if (!traits_.count(s))
                    error(ti.line, ti.col, "unknown trait '" + s + "'");
        }
        // Cycle detection over the (known) supertrait edges.
        std::unordered_map<std::string, int> color;   // 0=white 1=gray 2=black
        std::unordered_set<std::string> reported;
        std::vector<std::string> stack;
        std::function<void(const std::string&)> dfs = [&](const std::string& n) {
            color[n] = 1;
            auto it = traits_.find(n);
            if (it != traits_.end()) {
                for (const auto& s : it->second.supertraits) {
                    if (!traits_.count(s)) continue;
                    if (color[s] == 1) {
                        if (reported.insert(s).second)
                            error(traits_[s].line, traits_[s].col,
                                  "cyclic trait dependency involving '" + s + "'");
                    } else if (color[s] == 0) {
                        dfs(s);
                    }
                }
            }
            color[n] = 2;
        };
        for (auto& kv : traits_)
            if (color[kv.first] == 0) dfs(kv.first);
    }

    // Transitive supertrait closure (excluding the trait itself), cycle-safe.
    std::unordered_set<std::string> supertrait_closure(const std::string& trait) {
        std::unordered_set<std::string> out;
        std::vector<std::string> work;
        auto it = traits_.find(trait);
        if (it != traits_.end())
            for (const auto& s : it->second.supertraits) work.push_back(s);
        while (!work.empty()) {
            std::string s = work.back(); work.pop_back();
            if (!traits_.count(s) || !out.insert(s).second) continue;
            for (const auto& s2 : traits_[s].supertraits) work.push_back(s2);
        }
        return out;
    }

    // ----- object safety (may a trait become a `dyn Trait`?) ----------------
    //
    // Returns the REASON a trait is not object-safe, or nullopt if it is. Cached: the
    // answer is a property of the trait, and `resolve_type` may ask repeatedly.
    //
    // Our rules are deliberately DIFFERENT from Rust's, because Rust's two reasons do not
    // apply here: (a) Rust's `Self` is unsized in a dyn context -- ours is always an 8-byte
    // NaN-boxed value; (b) Rust monomorphizes, so a generic method needs a vtable slot per
    // instantiation -- we ERASE, so one body serves every instantiation. So only OS1/OS2 are
    // forced; OS3/OS4 are v1 CONSERVATISM we intend to lift.
    std::optional<std::string> object_safety_error(const std::string& trait) {
        if (auto c = object_safety_.find(trait); c != object_safety_.end()) return c->second;
        object_safety_.emplace(trait, std::nullopt);       // provisional: breaks a supertrait cycle
        std::optional<std::string> why = compute_object_safety(trait);
        object_safety_[trait] = why;
        return why;
    }

    std::optional<std::string> compute_object_safety(const std::string& trait) {
        auto it = traits_.find(trait);
        if (it == traits_.end()) return std::nullopt;       // unknown trait: already reported
        const TraitInfo& ti = it->second;

        // `Eq` / `Hashable` are compile-time MARKER traits, not dispatchable traits: they carry no
        // methods (so they are vacuously object-safe by the OS1-OS3 method rules), but a `dyn` of them
        // is meaningless -- there is nothing to dispatch, and the concrete type they'd need is hidden.
        // Reject both uniformly (distinct from the method-based object-safety failures of Ord/Clone).
        if (is_eq_trait(trait))       return "'Eq' is a compile-time marker, not usable as a trait object";
        if (trait == std_Hashable())  return "'Hashable' is a compile-time marker, not usable as a trait object";
        if (trait == std_MustUse())   return "'MustUse' is a compile-time marker, not usable as a trait object";

        // OS5: a supertrait's methods are reachable through the object, so they must be
        // dispatchable too. (Checked first: it names the real culprit.)
        for (const auto& s : supertrait_closure(trait))
            if (auto why = object_safety_error(s))
                return "its supertrait '" + s + "' is not object-safe (" + *why + ")";

        for (const auto& ms : ti.methods) {
            // OS1 (mechanical): dispatch reads the receiver's runtime type from argument 0.
            // With no `self` there is nothing to dispatch on.
            if (!ms.has_self)
                return "method '" + ms.name + "' takes no 'self', so there is nothing to "
                       "dispatch on at run time";

            // OS2 (SOUNDNESS -- the one rule that must never be relaxed): two `dyn T` values
            // may hide DIFFERENT types. Dispatch picks the impl from the receiver, so a second
            // `Self`-typed argument would reach a body expecting the receiver's type.
            for (size_t i = 0; i < ms.params.size(); ++i)
                if (mentions_var(ms.params[i], ti.self_id))
                    return "method '" + ms.name + "' takes 'Self' as a parameter, and two trait "
                           "objects may hide different types";

            // OS3 (v1 conservatism -- sound to lift, see the v2 backlog): a `Self` return is
            // fine here (the value IS the hidden type, which does implement the trait, and every
            // value is 8 bytes) -- unlike Rust, where `Self` would be unsized.
            if (mentions_var(ms.ret, ti.self_id))
                return "method '" + ms.name + "' returns 'Self' (a v1 restriction)";

            // OS4 (v1 conservatism -- sound to lift, see the v2 backlog): erasure gives a generic
            // method ONE function id regardless of its type arguments, so it is dispatchable --
            // unlike Rust, which would need a vtable slot per instantiation.
            if (!ms.generics.empty())
                return "method '" + ms.name + "' is generic (a v1 restriction)";
        }
        return std::nullopt;
    }

    // ----- impl coherence ---------------------------------------------------

    // The ORPHAN RULE: an `impl Trait for Type` is allowed only if the TRAIT or the
    // target TYPE is local to the impl's own module -- so no module may add an impl of a foreign
    // trait for a foreign type (which is where two modules could define conflicting impls).
    // Builtins (Int/Vec/...) are foreign (in neither home map); a blanket impl's target is a type
    // param, so it must satisfy trait-locality.
    void check_orphan() {
        for (const auto& im : impls_) {
            cur_module_ = im.module;   // tag the orphan error with the impl's module
            if (!traits_.count(im.trait_name)) continue;   // unknown trait: already errored
            const std::string& M = im.module;
            if (auto it = trait_module_.find(im.trait_name); it != trait_module_.end() && it->second == M)
                continue;   // the trait is local to this module
            bool type_local = false;
            if (!im.is_blanket && im.target) {
                TyPtr t = apply(im.target);
                if (t->kind == TyKind::Named)
                    if (auto it = type_module_.find(t->name); it != type_module_.end())
                        type_local = (it->second == M);
            }
            if (type_local) continue;                      // the target type is local to this module
            error(im.line, im.col,
                  "orphan impl: neither the trait '" + im.trait_name + "' nor the type '" + im.head +
                  "' is local to this module (implement a foreign trait for a foreign type only in "
                  "the trait's or the type's own module)");
        }
    }

    void check_impls() {
        std::unordered_map<std::string, size_t> seen;   // "trait\x1fhead" -> impls_ index

        for (size_t i = 0; i < impls_.size(); ++i) {
            ImplInfo& im = impls_[i];
            cur_module_ = im.module;   // tag impl-coherence errors with the impl's module
            if (im.ast && im.ast->is_inherent) {   // traitless inherent impl -> its own registration
                register_inherent(const_cast<ImplDecl&>(*im.ast));
                continue;
            }
            auto tit = traits_.find(im.trait_name);
            if (tit == traits_.end()) {
                error(im.line, im.col, "unknown trait '" + im.trait_name + "'"
                                      + missing_use_hint(im.trait_name));
                continue;
            }
            // `Eq` is a SEALED built-in marker: `==` is structural (the EQ_DEEP opcode), and a
            // type's Eq-ness is derived automatically (is_eq), so a user `impl Eq for X` would be
            // a no-op that misleads. A manual `impl Eq` override is a deliberately-deferred feature.
            if (is_eq_trait(im.trait_name)) {
                error(im.line, im.col, "cannot implement the built-in 'Eq' marker: '==' is "
                      "structural and derived automatically (a manual 'impl Eq' override is a "
                      "later feature)");
                continue;
            }
            // `Hashable` is a SEALED built-in marker: the permitted map-key/set-element set is fixed
            // (Int, Double, Bool, String -- plus the erasure types via synthesized impl-keys). A user
            // `impl Hashable for X` would make X satisfy the key bound at COMPILE time yet TRAP at run
            // time ("invalid map key type" -- a non-string heap key's pointer bits aren't GC-stable).
            // Only the four built-in impls in std::core are legitimate, so allow that module and reject
            // every other written impl. (`--no-prelude` user `trait Hashable` mangles to a different
            // name != std::core::Hashable, so the seal correctly no-fires, like is_eq_trait.)
            if (im.trait_name == std_Hashable() && im.module != STD_CORE) {
                error(im.line, im.col, "cannot implement the built-in 'Hashable' marker: it is a "
                      "fixed compiler marker for the permitted map-key types (Int, Double, Bool, "
                      "String)");
                continue;
            }
            // A parametric trait must receive exactly its declared number of type args.
            if (im.trait_args.size() != tit->second.generics.size())
                error(im.line, im.col, "impl of trait '" + im.trait_name + "' provides " +
                      std::to_string(im.trait_args.size()) + " type argument(s) but the trait declares " +
                      std::to_string(tit->second.generics.size()));
            if (im.is_blanket) {
                // Blanket impls of a PARAMETRIC trait are not supported: the trait args
                // would have to be solved per satisfying type, which the blanket path does not do.
                if (!tit->second.generics.empty())
                    error(im.line, im.col, "a blanket impl of the parametric trait '" +
                          im.trait_name + "' is not supported");
                else
                    register_blanket(im, tit->second);
                continue;
            }
            // A transparent struct erases to its underlying immediate at runtime (SAME NaN-box tag
            // as the primitive), so it carries no distinct identity through dynamic dispatch / dyn /
            // an erased bound -- a user impl would collide with the primitive's row. Forbid it;
            // transparent types dispatch statically only.
            // The one exception is `MustUse`: it has no methods, is never a trait object and never selects a
            // blanket impl (register_blanket), so nothing ever dispatches through it -- it is read statically.
            const bool erasure_ok = im.trait_name == std_MustUse();
            if (auto sit = structs_.find(im.head); !erasure_ok && sit != structs_.end() && sit->second.is_transparent)
                error(im.line, im.col, "cannot implement trait '" + im.trait_name +
                      "' for the transparent struct '" + im.head +
                      "' (transparent structs erase at runtime and dispatch statically only)");
            // Same reason for an int-backed enum: it erases to a bare Int (shares TID_INT), so it
            // carries no distinct identity through dynamic dispatch / dyn / an erased bound.
            if (auto eit = enums_.find(im.head); !erasure_ok && eit != enums_.end() && eit->second.is_int_backed)
                error(im.line, im.col, "cannot implement trait '" + im.trait_name +
                      "' for the int-backed enum '" + im.head +
                      "' (int-backed enums erase at runtime and dispatch statically only)");
            const std::string key = im.trait_name + "\x1f" + im.head;
            impl_keys_.insert(key);
            impl_index_.emplace(key, i);   // for parametric-trait impl matching (trait_args_for)
            if (!seen.emplace(key, i).second) {
                error(im.line, im.col,
                      "duplicate impl of trait '" + im.trait_name + "' for type '" + im.head + "'");
            }
            check_impl_methods(im, tit->second);
        }

        // Supertrait closure: a CONCRETE `impl Tr for H` requires `impl S for H` for each
        // supertrait S -- checked only now that every impl key is registered. Blankets have no
        // concrete head, so they are skipped here (their supertrait coverage is checked in
        // register_blanket, over the blanket's bounds instead of a concrete head).
        for (const auto& im : impls_) {
            if (im.is_blanket || !traits_.count(im.trait_name)) continue;
            for (const auto& s : supertrait_closure(im.trait_name)) {
                if (!seen.count(s + "\x1f" + im.head))
                    error(im.line, im.col, "type '" + im.head + "' implements '" + im.trait_name +
                                           "' but not its supertrait '" + s + "'");
                // Both impls exist, but a generic one must not promise more than the supertrait impl delivers:
                // the target's type parameters carry this impl's bounds, so the supertrait impl's bounds have to
                // follow from them (`impl[T] Loud for W[T]` over `impl[T: Named] Named for W[T]` fails for W[Int]).
                else if (!im.generics.empty() && !satisfies_bound(im.target, s))
                    error(im.line, im.col, "impl of '" + im.trait_name + "' for '" + im.head +
                                           "' does not guarantee its supertrait '" + s +
                                           "': add the bounds of that impl's type parameters to this impl");
            }
        }
    }

    // Register a blanket impl `impl[T: bounds] Tr for T`: at most one per trait; still verify it
    // provides/inherits every trait method; and (soundness) require its bounds to cover Tr's
    // supertrait closure, so a type gaining Tr through it also satisfies every supertrait.
    void register_blanket(const ImplInfo& im, const TraitInfo& ti) {
        // An erasure type may implement `MustUse`, so a blanket selected by it would reach such a type -- and a
        // call through the blanket dispatches on the erased runtime tag, the primitive's column.
        for (const auto& b : im.blanket_bounds)
            if (b.trait == std_MustUse()) {
                error(im.line, im.col, "'MustUse' marks values for a warning; it cannot select a blanket impl");
                return;
            }
        auto ins = blanket_impls_.emplace(im.trait_name, BlanketInfo{ im.blanket_bounds, im.line, im.col });
        if (!ins.second) {
            error(im.line, im.col, "duplicate blanket impl of trait '" + im.trait_name + "'");
            return;
        }
        check_impl_methods(im, ti);
        TyPtr probe = tc_.rigid_var("T", im.blanket_bounds);
        for (const auto& s : supertrait_closure(im.trait_name))
            if (!satisfies_bound(probe, s))
                error(im.line, im.col, "blanket impl of trait '" + im.trait_name +
                      "' requires its supertrait '" + s + "' among the bounds");
    }

    void check_impl_methods(const ImplInfo& im, const TraitInfo& ti) {
        std::unordered_map<std::string, const MethodSig*> trait_methods;
        for (const auto& ms : ti.methods) trait_methods.emplace(ms.name, &ms);

        std::unordered_set<std::string> provided;
        for (const auto& m : im.ast->methods) {
            provided.insert(m.name);
            auto it = trait_methods.find(m.name);
            if (it == trait_methods.end()) {
                error(im.line, im.col,
                      "method '" + m.name + "' is not a member of trait '" + im.trait_name + "'");
                continue;
            }
            const MethodSig& ms = *it->second;
            if (m.has_self != ms.has_self) {
                error(im.line, im.col, m.has_self
                      ? "method '" + m.name + "' must not take 'self' to match trait '" + im.trait_name + "'"
                      : "method '" + m.name + "' must take 'self' to match trait '" + im.trait_name + "'");
            }
            if (m.params.size() != ms.param_count) {
                error(im.line, im.col,
                      "method '" + m.name + "' has the wrong number of parameters for trait '" +
                      im.trait_name + "'");
            }
        }
        // Missing methods (no impl body and no trait default).
        for (const auto& ms : ti.methods) {
            if (!provided.count(ms.name) && !ms.has_default)
                error(im.line, im.col,
                      "impl of '" + im.trait_name + "' for '" + im.head +
                      "' is missing method '" + ms.name + "'");
        }
    }

    // ===== body checking ========

    // Thin wrappers over the solver.
    TyPtr apply(const TyPtr& t)                          { return tc_.apply(t); }
    bool  subsumes(const TyPtr& a, const TyPtr& b)       { return tc_.subsumes(a, b); }
    static bool numeric(const TyPtr& t) { return t->kind == TyKind::Int || t->kind == TyKind::Double; }

    void check_bodies() {
        for (const auto& item : prog_.items) {
            cur_module_ = item->module_prefix;   // resolve this body's references in its module
            if (item->kind == ItemKind::Fn)    check_fn_body(static_cast<FnItem&>(*item));
            if (item->kind == ItemKind::Impl)  check_impl_bodies(static_cast<ImplDecl&>(*item));
            if (item->kind == ItemKind::Trait) check_trait_defaults(static_cast<TraitDecl&>(*item));
            if (item->kind == ItemKind::Const) check_const_body(static_cast<ConstItem&>(*item));
        }

        // Top-level statements: each MODULE gets its OWN top-level scope (Python-style private init) --
        // a `let` at a module's top level is NOT visible to other modules or to the entry program. Items
        // are grouped by module (topological order), so a change of `module_prefix` marks a boundary; a
        // single-module program is still one scope. (Top-level lets are not module *declarations* -- only
        // fn/struct/enum/trait are exported -- so a module's top-level binding is genuinely private.)
        cur_generic_env_.clear();
        cur_return_ = ty_unit();
        at_top_level_ = true;
        loop_frames_.clear();
        lambda_floor_ = 0; lambda_is_let_bound_ = false;   // see check_fn_body
        bool have_scope = false;
        std::string tl_module;
        for (const auto& item : prog_.items)
            if (item->kind == ItemKind::Stmt) {
                if (!have_scope || item->module_prefix != tl_module) {
                    if (have_scope) pop_scope();
                    push_scope();
                    have_scope = true;
                    tl_module = item->module_prefix;
                }
                cur_module_ = item->module_prefix;   // each module's top-level stmts resolve in it
                check_discard_stmt(*static_cast<StmtItem&>(*item).stmt, /*tail=*/false);   // top-level stmt discarded
            }
        if (have_scope) pop_scope();
    }

    // A `mut` param / `mut self` is meaningful only for a HEAP AGGREGATE with an
    // in-place mutation surface (`x.f = v` / `x[i] = v`): a record struct, tuple,
    // tuple-struct, or Array/Vec/Bytes/Map. A scalar / enum / List / fn cannot be
    // mutated in place, so `mut` on it is a checker error (no mutable primitive params).
    // An ABSTRACT receiver (a type var -- a generic param, or a trait-default `self`)
    // is NOT rejected: it cannot be proven non-aggregate, and field access on it is
    // separately rejected already, so `mut` there is harmless.
    bool is_mutable_aggregate(const TyPtr& t0) {
        TyPtr t = apply(t0);
        switch (t->kind) {
            case TyKind::Tuple:
            case TyKind::Var:
            case TyKind::Error:
            case TyKind::Never:
            case TyKind::Dyn:      // a trait object is always a heap cursor struct -> mutable in place
                                   // (its methods take `mut self`; e.g. an Iterator advanced via `next`)
                return true;
            case TyKind::Named: {
                const std::string& n = t->name;
                if (n == "Array" || n == "Vec" || n == "Bytes" || n == "Map") return true;
                return structs_.count(n) != 0;   // record struct or tuple-struct (NOT an enum)
            }
            default:
                return false;   // Int / Double / Bool / String / Unit / Fn / List
        }
    }

    // Enforce the aggregate-only rule for a `mut` param / `mut self` at its binding site.
    void reject_bad_mut(bool is_mut, const TyPtr& ty, const std::string& what,
                        uint32_t line, uint32_t col) {
        if (is_mut && !is_mutable_aggregate(ty))
            error(line, col, "'" + what + "' cannot be declared 'mut': only aggregates "
                  "(structs, tuples, Array/Vec/Bytes/Map) support in-place mutation");
    }

    void check_fn_body(FnItem& f) {
        auto it = fns_.find(f.name);
        if (it == fns_.end()) return;
        FnSig& sig = it->second;
        cur_generic_env_.clear();
        for (const auto& g : sig.generics) cur_generic_env_.emplace(g.name, g.var);
        cur_return_ = sig.ret;
        at_top_level_ = false;
        loop_frames_.clear();
        lambda_floor_ = 0; lambda_is_let_bound_ = false;   // never inside a lambda (belt-and-braces)
        push_scope();
        for (size_t i = 0; i < f.params.size() && i < sig.params.size(); ++i) {
            reject_bad_mut(f.params[i].is_mut, sig.params[i], f.params[i].name,
                           f.params[i].line, f.params[i].col);
            define(f.params[i].name, sig.params[i], f.params[i].is_mut);
        }
        if (f.body) {
            if (f.ret) {
                DiagLabel origin = label_at(*f.ret, "expected because of the declared return type");
                check_body_against_ret(*f.body, sig.ret, &origin);
            } else {
                check_body_against_ret(*f.body, sig.ret);
            }
        }
        pop_scope();
    }

    // Check a function-like BODY against its declared return type. A `-> ()` body's value is
    // DISCARDED (it may end in a non-unit expression, which is dropped -- so no `push`-returns-Bytes
    // wart); a dropped MUST-USE value in this tail position is still a hard error (check_discard
    // tail=true). Otherwise strict. This is the ONLY place a unit-return body is made lax -- a
    // `let x: () = <non-unit>` (via check_expr/check_block) stays an error (the NARROW guarantee).
    // Shared by check_fn_body / check_method_body / check_lambda.
    void check_body_against_ret(Expr& body, const TyPtr& ret, const DiagLabel* origin = nullptr) {
        if (apply(ret)->kind == TyKind::Unit) check_discard(body, /*tail=*/true);
        else check_expr(body, ret, origin);
    }

    // Check every method body of an impl, with `self : <target>` and the impl's
    // generics (+ each method's own generics) in scope.
    // Register a traitless inherent impl `impl[G] Head[..] { … }` -- resolve method sigs against
    // the impl's generics (shared ids with the target), enforce the orphan rule (Head local), forbid
    // erased targets, and detect a duplicate method / a second inherent block for the same head.
    void register_inherent(ImplDecl& im) {
        std::unordered_map<std::string, TyPtr> env;
        std::vector<GenericInfo> gens = make_generics(im.generics, env, im.line, im.col, /*report=*/false);
        TyPtr target = im.target ? resolve_type(*im.target, env) : ty_error();
        if (!target || target->kind != TyKind::Named) {
            error(im.line, im.col, "inherent impl target must be a named type");
            return;
        }
        const std::string head = target->name;
        auto tm = type_module_.find(head);
        if (tm == type_module_.end() || tm->second != cur_module_) {
            error(im.line, im.col, "inherent impl of '" + short_name(head) +
                  "' must be in the type's own module (cannot add methods to a foreign/builtin type)");
            return;
        }
        if (auto sit = structs_.find(head); sit != structs_.end() && sit->second.is_transparent) {
            error(im.line, im.col, "cannot add inherent methods to the transparent struct '" + short_name(head) + "'");
            return;
        }
        if (auto eit = enums_.find(head); eit != enums_.end() && eit->second.is_int_backed) {
            error(im.line, im.col, "cannot add inherent methods to the int-backed enum '" + short_name(head) + "'");
            return;
        }
        if (inherent_.count(head)) {
            error(im.line, im.col, "duplicate inherent impl for '" + short_name(head) +
                  "' (merge the methods into one `impl` block)");
            return;
        }
        InherentInfo ii;
        ii.generics = std::move(gens);
        ii.target = target;
        ii.line = im.line; ii.col = im.col;
        // `Self` names the impl target in a SIGNATURE too, not just in a body (`check_method_body`
        // already binds it there). Without this an inherent `fn new(...) -> Self` would be rejected with
        // "'Self' is only valid inside a trait or impl" -- inside an impl. For a generic head `Self`
        // is `Box[T]` over the impl's own generic ids, which every call site substitutes anyway.
        env["Self"] = target;
        for (auto& mth : im.methods) {
            // A method WITHOUT `self` is an ASSOCIATED FUNCTION (`Point::new(1, 2)`) -- legal, and
            // reachable only through the qualified form. `has_self` is the single fact that decides
            // reachability: a method answers to `.` and `::`, an associated fn to `::` alone. Both
            // the `.` resolution below and codegen's dot branch filter on it, so the two cannot drift.
            if (ii.methods.count(mth.name))
                error(im.line, im.col, "duplicate inherent method '" + mth.name + "' for '" + short_name(head) + "'");
            else
                ii.methods.emplace(mth.name, resolve_method(mth, env, im.module_prefix));
        }
        inherent_[head] = std::move(ii);
    }

    // Check the bodies of an inherent impl's methods (Self := target, `self` bound). No trait
    // conformance -- there is no trait to promise a signature.
    void check_inherent_bodies(ImplDecl& im) {
        std::unordered_map<std::string, TyPtr> env;
        make_generics(im.generics, env, im.line, im.col, /*report=*/false);
        TyPtr self_ty = im.target ? resolve_type(*im.target, env) : ty_error();
        for (auto& mth : im.methods)
            if (mth.body) check_method_body(mth, self_ty, env);
    }

    // Resolve an inherent-method call `recv.name(args)` -- instantiate the impl generics as fresh
    // vars, solve them by unifying the impl target with the receiver, then check the non-self args.
    TyPtr call_inherent_method(const std::string& head, const std::string& name,
                               const TyPtr& recvTy, Expr* recvExpr,
                               const std::vector<Expr*>& args, Expr& node) {
        const InherentInfo& ii = inherent_.at(head);
        const MethodSig& ms = ii.methods.at(name);
        std::unordered_map<uint32_t, TyPtr> m;
        for (const auto& g : ii.generics) m[g.id] = tc_.fresh_var(g.name, g.bounds);
        for (const auto& g : ms.generics) m[g.id] = tc_.fresh_var(g.name, g.bounds);
        TyPtr target = tc_.substitute(ii.target, m);
        tc_.unify(target, recvTy);                    // solve the impl generics from the receiver
        std::vector<TyPtr> params;
        for (const auto& p : ms.params) params.push_back(tc_.substitute(p, m));
        TyPtr ret = tc_.substitute(ms.ret, m);
        if (args.size() != ms.params.size()) {
            error(node.line, node.col, "method '" + name + "' expects " + std::to_string(ms.params.size()) +
                  " argument(s), got " + std::to_string(args.size()));
            for (Expr* a : args) infer(*a);
            return apply(ret);
        }
        if (ms.self_mut) require_mut_arg(recvExpr, "a `mut self` method");
        check_args_two_pass(args, params, &ms.origin);
        for (size_t i = 0; i < args.size() && i < ms.params_mut.size(); ++i)
            if (ms.params_mut[i]) require_mut_arg(args[i], "a `mut` parameter");
        const std::string what = name;
        discharge_bounds(ii.generics, m, node.line, node.col, &what);
        discharge_bounds(ms.generics, m, node.line, node.col, &what);
        return apply(ret);
    }

    void check_impl_bodies(ImplDecl& im) {
        if (im.is_inherent) { check_inherent_bodies(im); return; }
        if (!im.target || im.target->kind != TypeKind::Named || !traits_.count(im.trait_name)) return;
        std::unordered_map<std::string, TyPtr> env;
        make_generics(im.generics, env, im.line, im.col, /*report=*/false);
        TyPtr self_ty = resolve_type(*im.target, env);
        std::vector<TyPtr> trait_args;                       // `impl[X] Iterable[X] for …`
        for (const auto& a : im.trait_args) trait_args.push_back(resolve_type(*a, env));
        const TraitInfo& ti = traits_[im.trait_name];
        for (auto& mth : im.methods) {
            const MethodSig* ms = nullptr;
            for (const auto& x : ti.methods) if (x.name == mth.name) { ms = &x; break; }
            if (ms) check_method_conformance(im, mth, *ms, ti.self_id, self_ty, ti.generics, trait_args, env);
            if (mth.body) check_method_body(mth, self_ty, env);
        }
    }

    // Verify an impl method's param/return types MATCH the trait's (after Self :=
    // target). Otherwise the impl could declare a different type than the trait
    // promises -- a caller resolves via the trait signature, so a mismatch would let
    // a wrongly-typed value through. Methods with their OWN
    // generics are checked too: the trait's and the impl's positional type params
    // are aligned to a shared set of rigid skolems, then the signatures are deep-
    // compared -- and the impl may only ASSUME bounds the trait guarantees to callers.
    void check_method_conformance(const ImplDecl& im, const Method& mth, const MethodSig& ms,
                                  uint32_t self_id, TyPtr self_ty,
                                  const std::vector<GenericInfo>& trait_generics,
                                  const std::vector<TyPtr>& trait_args,
                                  const std::unordered_map<std::string, TyPtr>& env) {
        if (mth.params.size() != ms.params.size()) return;   // arity already reported while resolving signatures
        // An impl may DROP a `mut` the trait declares, but never ADD one. Every caller -- concrete, generic
        // or `dyn` -- enforces the call-site `mut` rule from the TRAIT signature (`require_mut_arg`), so an
        // impl-only `mut self` / `mut` parameter would mutate a caller's non-`mut` binding with no diagnostic.
        // Checked before the generics comparison below, whose mismatch branch returns early.
        if (mth.has_self && mth.self_mut && !ms.self_mut)
            error(mth.self_line ? mth.self_line : im.line, mth.self_line ? mth.self_col : im.col,
                  "method '" + mth.name + "' takes `mut self` but trait '" + im.trait_name +
                  "' declares `self` -- a call through the trait does not require a `mut` receiver; "
                  "add `mut` to the trait method or remove it here");
        for (size_t i = 0; i < mth.params.size(); ++i) {
            const Param& p = mth.params[i];
            if (p.is_mut && !(i < ms.params_mut.size() && ms.params_mut[i]))
                error(p.line ? p.line : im.line, p.line ? p.col : im.col,
                      "method '" + mth.name + "' parameter '" + p.name + "' is `mut` but trait '" +
                      im.trait_name + "' declares it without `mut` -- a call through the trait does not "
                      "require a `mut` argument; add `mut` to the trait method or remove it here");
        }
        std::unordered_map<uint32_t, TyPtr> sub; sub[self_id] = self_ty;
        // Map the trait's own type params to the impl's trait args (`impl Iterable[Int] for …`),
        // so the trait method's `-> Vec[T]` compares against the impl's `-> Vec[Int]`.
        for (size_t i = 0; i < trait_generics.size() && i < trait_args.size(); ++i)
            sub[trait_generics[i].id] = trait_args[i];
        std::unordered_map<std::string, TyPtr> e2 = env; e2["Self"] = self_ty;

        if (mth.generics.size() != ms.generics.size()) {
            error(im.line, im.col, "method '" + mth.name + "' has " +
                  std::to_string(mth.generics.size()) + " type parameter(s) but trait '" +
                  im.trait_name + "' declares " + std::to_string(ms.generics.size()));
            return;
        }
        // Align the method's own type parameters: map BOTH the trait's (by rigid id)
        // and the impl's (by written name) i-th generic to one shared skolem, so a
        // signature comparison treats them as the same abstract type.
        for (size_t i = 0; i < ms.generics.size(); ++i) {
            TyPtr sk = tc_.rigid_var(ms.generics[i].name, {});
            sub[ms.generics[i].id]        = sk;
            e2[mth.generics[i].name]      = sk;
        }
        // Soundness: a caller discharges only the TRAIT method's bounds, so the impl method may not ASSUME a
        // bound the trait does not guarantee -- an extra impl bound would let the body use an operation the caller
        // never proved. (Dropping a trait bound is safe; only ADDING or CHANGING one is unsound.)
        // Compared only once every shared skolem is bound, so a bound argument naming a sibling (`I: Src[T]`)
        // resolves on both sides. The impl's bounds are RESOLVED here, not read off the AST: the AST still holds
        // the name as written (`Plain`) until the body check resolves it, while the trait's are mangled
        // (`$entry::Plain`), so a raw comparison rejected every bounded generic method of a compiled program.
        for (size_t i = 0; i < ms.generics.size(); ++i) {
            for (const Bound& ib : resolve_bounds(mth.generics[i].bounds, e2)) {
                bool guaranteed = false;
                const Bound* same_trait = nullptr;
                Bound same_trait_sub;
                for (const auto& tb : ms.generics[i].bounds) {
                    if (tb.trait == ib.trait) {
                        Bound tsub;
                        tsub.trait = tb.trait;
                        for (const auto& a : tb.args) tsub.args.push_back(apply(tc_.substitute(a, sub)));
                        bool args_equal = tsub.args.size() == ib.args.size();
                        for (size_t k = 0; args_equal && k < ib.args.size(); ++k)
                            args_equal = equals(apply(ib.args[k]), tsub.args[k]);
                        if (args_equal) { guaranteed = true; break; }
                        same_trait = &tb;
                        same_trait_sub = std::move(tsub);
                    } else if (ib.args.empty() && supertrait_closure(tb.trait).count(ib.trait)) {
                        // A supertrait reached through a bound: only a NON-parametric one, since a var's
                        // parametric supertrait arguments are not solved (see `trait_args_for`).
                        guaranteed = true;
                        break;
                    }
                }
                if (guaranteed) continue;
                if (same_trait)
                    error(im.line, im.col, "method '" + mth.name + "' type parameter '" + mth.generics[i].name +
                          "' declares bound '" + describe_bound(ib) + "' but the trait '" + im.trait_name +
                          "' requires '" + describe_bound(same_trait_sub) + "'");
                else
                    error(im.line, im.col, "method '" + mth.name + "' type parameter '" +
                          mth.generics[i].name + "' declares bound '" + ib.trait +
                          "' that the trait '" + im.trait_name + "' does not require");
            }
        }

        for (size_t i = 0; i < mth.params.size(); ++i) {
            if (!mth.params[i].type) continue;
            TyPtr want = apply(tc_.substitute(ms.params[i], sub));
            TyPtr got  = apply(resolve_type(*mth.params[i].type, e2));
            if (!equals(want, got) && want->kind != TyKind::Error && got->kind != TyKind::Error) {
                TypeRenderer r;
                error(im.line, im.col, "method '" + mth.name + "' parameter '" + mth.params[i].name +
                      "' has type " + r(got) + " but trait '" + im.trait_name +
                      "' requires " + r(want));
            }
        }
        TyPtr wantR = apply(tc_.substitute(ms.ret, sub));
        TyPtr gotR  = apply(mth.ret ? resolve_type(*mth.ret, e2) : ty_error());
        if (!equals(wantR, gotR) && wantR->kind != TyKind::Error && gotR->kind != TyKind::Error) {
            TypeRenderer r;
            error(im.line, im.col, "method '" + mth.name + "' returns " + r(gotR) +
                  " but trait '" + im.trait_name + "' requires " + r(wantR));
        }
    }

    // Check each trait DEFAULT method body, with `self : Self` where Self is a rigid
    // variable bounded by the trait itself (so the body may call the trait's own /
    // supertrait methods on self).
    void check_trait_defaults(TraitDecl& td) {
        for (auto& mth : td.methods) {
            if (!mth.body) continue;
            std::unordered_map<std::string, TyPtr> env;
            TyPtr self_ty = tc_.rigid_var("Self", { Bound{ td.name, {} } });
            env.emplace("Self", self_ty);
            check_method_body(mth, self_ty, env);
        }
    }

    void check_method_body(Method& mth, TyPtr self_ty, const std::unordered_map<std::string, TyPtr>& outer_env) {
        std::unordered_map<std::string, TyPtr> env = outer_env;
        env["Self"] = self_ty;
        make_generics(mth.generics, env, 0, 0, /*report=*/false);
        cur_generic_env_ = env;
        cur_return_ = mth.ret ? resolve_type(*mth.ret, env) : ty_error();
        at_top_level_ = false;
        loop_frames_.clear();
        lambda_floor_ = 0; lambda_is_let_bound_ = false;   // see check_fn_body
        push_scope();
        if (mth.has_self) {
            reject_bad_mut(mth.self_mut, apply(self_ty), "self", mth.self_line, mth.self_col);
            define("self", apply(self_ty), mth.self_mut);
        }
        for (const auto& p : mth.params) {
            TyPtr pt = p.type ? resolve_type(*p.type, env) : ty_error();
            reject_bad_mut(p.is_mut, pt, p.name, p.line, p.col);
            define(p.name, pt, p.is_mut);
        }
        check_body_against_ret(*mth.body, cur_return_);
        pop_scope();
    }

    // ----- scopes -----------------------------------------------------------
    void push_scope() { scopes_.emplace_back(); }
    // Popping a scope flushes the unused-binding warnings for it: any TRACKED binding never
    // read, whose name is not `_`-prefixed, warns once (the must-use message for a
    // Result/Option carrier, else the generic unused-variable one). Emitted here, at scope
    // exit; `finalize_diagnostics` sorts them into source order.
    void pop_scope() {
        for (auto& kv : scopes_.back()) {
            const ScopeVar& v = kv.second;
            if (!v.track || v.used) continue;
            if (!kv.first.empty() && kv.first[0] == '_') continue;   // `_name` opts out
            if (v.carrier && is_result_carrier(v.ty))
                warn(v.line, v.col, "unused " + apply(v.ty)->name + " '" + kv.first +
                     "' -- use it (match / unwrap / `?`), or discard it with `let _ = ...`");
            else if (v.carrier)
                warn(v.line, v.col, "unused " + describe(apply(v.ty)) + " '" + kv.first +
                     "' -- its type is marked MustUse; use it, or discard it with `let _ = ...`");
            else
                warn(v.line, v.col, "unused variable '" + kv.first + "' (prefix with '_' to silence)");
        }
        scopes_.pop_back();
    }
    void define(const std::string& n, TyPtr t, bool is_mut,
                bool track = false, uint32_t line = 0, uint32_t col = 0) {
        bool carrier = track && is_must_use_type(t);
        scopes_.back()[n] = ScopeVar{ std::move(t), is_mut, track, false, carrier, line, col };
    }
    ScopeVar* lookup(const std::string& n) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->find(n);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }
    // `lookup` plus the INDEX of the scope the name resolved in (0 = outermost). Split out rather than
    // folded into `lookup` because only the lambda write barrier needs the index, and the ~10 other
    // callers are better off with the narrower contract.
    ScopeVar* lookup_at(const std::string& n, size_t& depth_out) {
        for (size_t i = scopes_.size(); i-- > 0; ) {
            auto f = scopes_[i].find(n);
            if (f != scopes_[i].end()) { depth_out = i; return &f->second; }
        }
        return nullptr;
    }
    // True iff `n` resolves OUTSIDE the lambda currently being checked -- i.e. the lambda reaches it
    // through a by-value capture rather than owning it. False whenever no lambda is being checked, so
    // the scope walk is skipped entirely on the ordinary path.
    bool is_captured_name(const std::string& n) {
        if (lambda_floor_ == 0) return false;
        size_t d = 0;
        return lookup_at(n, d) != nullptr && d < lambda_floor_;
    }
    // Record that a `let`-bound lambda READ `n` from outside itself, i.e. snapshotted it by value.
    // Only a `mut` binding is marked: an immutable one can never be rebound, so it can never go
    // stale. Called beside each site that sets `used`, so a read is a read wherever it is spelled.
    // Returns immediately outside a let-bound lambda body, which is also what keeps the ordinary
    // checker path free of a second scope walk.
    void note_capture(const std::string& n, uint32_t line, uint32_t col) {
        if (!lambda_is_let_bound_ || lambda_floor_ == 0) return;
        size_t d = 0;
        ScopeVar* v = lookup_at(n, d);
        if (!v || d >= lambda_floor_) return;       // the lambda's own local -- not a capture
        if (!v->is_mut || v->cap_line != 0) return; // immutable, or already marked (keep the FIRST)
        v->cap_line = line;
        v->cap_col  = col;
    }

    std::vector<TyPtr> fresh_args(size_t n) {
        std::vector<TyPtr> v;
        for (size_t i = 0; i < n; ++i) v.push_back(tc_.fresh_var());
        return v;
    }

    TyPtr subst_generics(const TyPtr& t, const std::vector<GenericInfo>& gens,
                         const std::vector<TyPtr>& args) {
        if (gens.empty()) return t;
        std::unordered_map<uint32_t, TyPtr> m;
        for (size_t i = 0; i < gens.size() && i < args.size(); ++i) m[gens[i].id] = args[i];
        return tc_.substitute(t, m);
    }

    // ----- statements -------------------------------------------------------
    TyPtr check_statement(Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let:    check_let(static_cast<LetStmt&>(s));       return ty_unit();
            case StmtKind::Assign: check_assign(static_cast<AssignStmt&>(s)); return ty_unit();
            // Returns the expression's TYPE -- infer_block uses this for the block's tail value, so it
            // must NOT be forced to unit here. Discard positions call check_discard_stmt instead.
            case StmtKind::Expr:   return infer(*static_cast<ExprStmt&>(s).expr);
        }
        return ty_unit();
    }

    // Check `e` in a VALUE-DISCARDED position (a non-final statement, a `-> ()` body tail, a
    // while/for body, or a nested branch/tail of one). Accepts ANY type (the value is dropped),
    // and -- unlike the infer path -- does NOT join if/match branches (each arm is discarded
    // independently). Kept SEPARATE from check_expr so strict positions (let-annotation, arguments,
    // fields, a value-USED unit context) stay strict: `let x: () = 5` is still an error.
    //
    // `tail` = this value sits in RETURN/tail position (a `-> ()` body tail, or a branch/arm of such).
    // A discarded MUST-USE value (Result/Option) there is almost always an oversight -> HARD ERROR
    // (as before this relaxation). Outside tail position (a non-final statement / loop body) a
    // discarded must-use value is the usual advisory must-use WARNING.
    void check_discard(Expr& e, bool tail) {
        switch (e.kind) {
        case ExprKind::Block: {
            auto& b = static_cast<BlockExpr&>(e);
            push_scope();
            for (size_t i = 0; i < b.stmts.size(); ++i)
                check_discard_stmt(*b.stmts[i], /*tail=*/tail && i + 1 == b.stmts.size());
            pop_scope();
            e.ty = ty_unit();
            return;
        }
        case ExprKind::If: {
            auto& i = static_cast<IfExpr&>(e);
            check_expr(*i.cond, ty_bool());
            check_discard(*i.then_blk, tail);
            if (i.else_blk) check_discard(*i.else_blk, tail);
            e.ty = ty_unit();
            return;
        }
        case ExprKind::Match: {
            auto& m = static_cast<MatchExpr&>(e);
            TyPtr scrut = apply(infer(*m.scrut));
            size_t pattern_errors = 0;
            for (auto& arm : m.arms) {
                push_scope();
                const size_t before = errors_.size();
                check_pattern(*arm.pat, scrut);
                pattern_errors += errors_.size() - before;
                if (arm.guard) check_expr(*arm.guard, ty_bool());
                check_discard(*arm.body, tail);
                pop_scope();
            }
            check_match_coverage_if_typed(m, scrut, pattern_errors);
            e.ty = ty_unit();
            return;
        }
        default: {
            TyPtr t = apply(infer(e));           // sets e.ty
            if (tail && is_result_carrier(t))    // a dropped Result/Option in return position -> hard error
                error(e.line, e.col, "type mismatch: expected (), found " + describe(t));
            else
                check_discarded(e);              // statement discard -> advisory must-use warning
            return;
        }
        }
    }
    // A statement in discard context: an ExprStmt's value is dropped; Let/Assign stay normal.
    TyPtr check_discard_stmt(Stmt& s, bool tail) {
        if (s.kind == StmtKind::Expr) { check_discard(*static_cast<ExprStmt&>(s).expr, tail); return ty_unit(); }
        return check_statement(s);
    }

    void check_let(LetStmt& l) {
        // Arm the snapshot warning for a lambda that is THIS `let`'s initializer -- a lambda bound to
        // a name outlives the statement, so a later rebinding of anything it captured is a real trap.
        // Keyed on the AST NODE, not on the binding: the initializer is checked BEFORE `bind_pattern`
        // defines the name (which is also why `let f = fn() { f() }` is already an unknown-variable
        // error), and node identity routes correctly through BOTH the annotated path (check_expr ->
        // check_lambda) and the un-annotated one (infer -> infer_lambda). Deliberately narrow: an
        // initializer that merely CONTAINS a lambda (`let (f, g) = (fn(){..}, ..)`, `let f = if c {
        // fn(){..} } else {..}`) does not arm it -- conservative in the silent direction.
        const LambdaExpr* savedLB = let_bound_lambda_;
        let_bound_lambda_ = (l.init && l.init->kind == ExprKind::Lambda)
                                ? static_cast<const LambdaExpr*>(l.init.get()) : nullptr;
        TyPtr bound;
        if (l.type) {
            TyPtr expected = resolve_type(*l.type, cur_generic_env_);
            if (l.init) {
                DiagLabel origin = label_at(*l.type, "expected because of this type annotation");
                check_expr(*l.init, expected, &origin);
            }
            bound = expected;
        } else {
            bound = l.init ? infer(*l.init) : ty_error();
        }
        let_bound_lambda_ = savedLB;
        bind_pattern(*l.pat, apply(bound), l.is_mut);
    }

    // Binds an IRREFUTABLE `let` pattern: ident / `_` / tuple / record struct `P{..}` / tuple-struct
    // `Pair(..)` (all one-shape), nested. Refutable patterns (enum variant / list / map / literal)
    // require `match`. All bindings inherit the `let`'s `mut`.
    //
    // `ctx` names the construct in every diagnostic ("let" or "for"): both callers share this binder,
    // so a fixed word would advise a user with a rejected `for` pattern about `'let'`.
    //
    // The switch is EXHAUSTIVE (no `default:`) on purpose -- a new PatKind must be classified as
    // irrefutable or not, deliberately, rather than inheriting the refutable message by accident.
    void bind_pattern(Pattern& p, TyPtr ty, bool is_mut, const char* ctx = "let") {
        // Every refutable shape lands here: it needs a `match`, not a binder.
        auto refutable = [&](const Pattern& at) {
            error(at.line, at.col, std::string("refutable pattern is not allowed in '") + ctx +
                  "' (use 'match')");
        };
        switch (p.kind) {
            case PatKind::Wildcard:
                return;
            case PatKind::Ident:
                define(static_cast<IdentPat&>(p).name, ty, is_mut, /*track=*/true, p.line, p.col);
                return;
            case PatKind::Tuple: {
                auto& tp = static_cast<TuplePat&>(p);
                TyPtr a = apply(ty);
                if (a->kind == TyKind::Tuple && a->args.size() == tp.elems.size()) {
                    for (size_t i = 0; i < tp.elems.size(); ++i)
                        bind_pattern(*tp.elems[i], a->args[i], is_mut, ctx);
                } else {
                    if (a->kind != TyKind::Error)
                        error(p.line, p.col, "tuple pattern does not match type " + describe(a));
                    for (auto& e : tp.elems) bind_pattern(*e, ty_error(), is_mut, ctx);
                }
                return;
            }
            case PatKind::Struct: {
                // A record struct is irrefutable (one shape); a record enum variant is refutable.
                auto& sp = static_cast<StructPat&>(p);
                sp.name = resolve_qualified_ref(sp.qualifier, sp.name, sp.line, sp.col);   // module-qualify (+ writeback)
                auto sit = structs_.find(sp.name);
                if (sit == structs_.end()) {
                    if (variants_.count(sp.name)) refutable(p);
                    else error(p.line, p.col, "unknown struct '" + sp.name + "' in pattern");
                    for (auto& fp : sp.fields) if (fp.pat) bind_pattern(*fp.pat, ty_error(), is_mut, ctx);
                    return;
                }
                const StructInfo& si = sit->second;
                std::vector<TyPtr> targs = named_args_for(ty, sp.name, si.generics.size(), p);
                std::unordered_map<uint32_t, TyPtr> m;
                for (size_t i = 0; i < si.generics.size() && i < targs.size(); ++i) m[si.generics[i].id] = targs[i];
                std::unordered_map<std::string, size_t> field_index;
                for (size_t i = 0; i < si.field_names.size(); ++i) field_index[si.field_names[i]] = i;
                for (auto& fp : sp.fields) {
                    auto fit = field_index.find(fp.name);
                    if (fit == field_index.end()) {
                        error(p.line, p.col, "struct '" + sp.name + "' has no field '" + fp.name + "'");
                        if (fp.pat) bind_pattern(*fp.pat, ty_error(), is_mut, ctx);
                        continue;
                    }
                    TyPtr ft = tc_.substitute(si.field_types[fit->second], m);
                    if (fp.pat) bind_pattern(*fp.pat, ft, is_mut, ctx);
                    else        define(fp.name, apply(ft), is_mut, /*track=*/true, p.line, p.col);  // shorthand `{ x }`
                }
                return;
            }
            case PatKind::Ctor: {
                // A tuple-STRUCT (single constructor) is irrefutable; an enum variant is refutable.
                auto& cp = static_cast<CtorPat&>(p);
                const size_t before = errors_.size();
                cp.name = resolve_qualified_ref(cp.qualifier, cp.name, cp.line, cp.col);   // module-qualify (+ writeback)
                auto sit = structs_.find(cp.name);
                if (sit == structs_.end() || !sit->second.is_tuple) {
                    if (variants_.count(cp.name)) refutable(p);
                    else if (errors_.size() == before)   // else the qualified head was already reported
                        error(p.line, p.col, "unknown constructor '" + cp.name + "' in pattern");
                    for (auto& sub : cp.elems) bind_pattern(*sub, ty_error(), is_mut, ctx);
                    return;
                }
                const StructInfo& si = sit->second;
                std::vector<TyPtr> targs = named_args_for(ty, cp.name, si.generics.size(), p);
                std::unordered_map<uint32_t, TyPtr> m;
                for (size_t i = 0; i < si.generics.size() && i < targs.size(); ++i) m[si.generics[i].id] = targs[i];
                if (cp.elems.size() != si.field_types.size())
                    error(p.line, p.col, "constructor '" + cp.name + "' expects " +
                          std::to_string(si.field_types.size()) + " field(s), got " + std::to_string(cp.elems.size()));
                for (size_t i = 0; i < cp.elems.size() && i < si.field_types.size(); ++i)
                    bind_pattern(*cp.elems[i], tc_.substitute(si.field_types[i], m), is_mut, ctx);
                return;
            }
            case PatKind::Bind:
                // Rejected on its OWN terms, not by asking whether `sub` is irrefutable: in a binder
                // position `let n @ sub = e` binds exactly what `let n = e` binds, so the `@` is pure
                // noise even when it would be accepted. Saying so beats the generic refutable message.
                error(p.line, p.col, std::string("an '@' binding is not allowed in '") + ctx +
                      "' -- it always matches here, so write just the name");
                return;
            // Genuinely refutable in a binder position -> require `match`. Listed one by one rather
            // than caught by a `default:`, so a new PatKind is classified deliberately (see above).
            case PatKind::Literal:
            case PatKind::Range:
            case PatKind::List:
            case PatKind::Map:
            case PatKind::Or:
                refutable(p);
                return;
        }
    }

    // Walk a field/index lvalue chain down to its ROOT binding identifier (a lowercase,
    // unqualified name). Returns nullptr if the target is not rooted in a binding
    // (e.g. `mk().x` -- an rvalue temporary).
    static IdentExpr* assignment_root(Expr* e) {
        for (;;) {
            switch (e->kind) {
                case ExprKind::Field: e = static_cast<FieldExpr&>(*e).obj.get(); break;
                case ExprKind::Index: e = static_cast<IndexExpr&>(*e).obj.get(); break;
                case ExprKind::Ident: {
                    auto& id = static_cast<IdentExpr&>(*e);
                    return (id.qualifier.empty() && !id.upper) ? &id : nullptr;
                }
                default: return nullptr;
            }
        }
    }

    // An argument bound to a `mut` parameter must, IF it is a named
    // lvalue (an ident / field / index path rooted in a local binding), have a `mut` root binding --
    // exactly the rule for `p.x = …` / `a[i] = …`. A temporary / rvalue arg is fine (a fresh
    // value is trivially the caller's to mutate). Unifies push/field-assign/index-assign under one
    // mutation rule; runtime-neutral (checker-only). `what` names the callee for the diagnostic.
    void require_mut_arg(Expr* arg, const std::string& what) {
        if (!arg) return;
        IdentExpr* root = assignment_root(arg);
        if (!root) return;                       // a temporary / rvalue (or qualified/Upper) -- OK
        ScopeVar* v = lookup(root->name);
        if (!v) return;                          // unknown variable -- reported elsewhere
        if (!v->is_mut)
            error(arg->line, arg->col,
                  "cannot pass immutable binding '" + root->name + "' to " + what +
                  " (declare it 'mut')");
    }

    // A COMPOUND assignment `target op= value` is typed exactly as if it were `target = target op value`
    // checked against the target's type -- stated once here so the two spellings cannot drift. The
    // accepted set therefore follows the PLAIN operator's, which splits the family in two:
    //
    //   arithmetic (`+= -= *= /= %=`)  -- numeric, Int and Double:
    //     Int    op= Int              ok
    //     Double op= Int | Double     ok (Int <: Double at the boundary)
    //     Int    op= Double           rejected -- a Double does not fit an Int slot
    //   bitwise / shift (`&= |= ^= <<= >>= >>>=`) -- INTEGER-ONLY, both sides:
    //     Int    op= Int              ok
    //     anything involving a Double rejected -- the VM's bitwise/shift handlers do no coercion,
    //                                 so `d &= 1` has no meaning to fall back on
    //
    // Restricting the arithmetic forms to Int/Double is deliberate. `s += t` would be the quadratic
    // string-building idiom the language has StringBuilder for, so the rejection has to SAY that -- a
    // bare "type error" reads like an oversight, given that `s = s + t` still works. That advice is
    // wrong for the bitwise forms (`s |= t` has no string meaning at all), so it is arithmetic-only.
    // `Char` and transparent newtypes fall out on their own (they already reject `c + 1`), so they
    // need no special case.
    void check_compound_assign(AssignStmt& a, const TyPtr& targetTy) {
        const TyPtr t = apply(targetTy);
        const TyPtr v = apply(infer(*a.value));
        if (t->kind == TyKind::Error || v->kind == TyKind::Error) return;   // already reported
        const bool bits = is_bitwise_op(a.op);
        auto ok = [&](const TyPtr& x) {
            return x->kind == TyKind::Int || (!bits && x->kind == TyKind::Double);
        };
        const std::string op = to_string(a.op);
        const std::string want = bits ? "integer-only (Int)" : "numeric-only (Int / Double)";
        if (!ok(t)) {
            std::string msg = "'" + op + "=' is " + want + ", but the target is " + describe(t);
            if (!bits && t->kind == TyKind::String)
                msg += "; for strings use a StringBuilder, or write `s = s " + op +
                       " t` if you really want the copy";
            error(a.line, a.col, std::move(msg));
            return;
        }
        if (!ok(v)) {
            error(a.line, a.col, "'" + op + "=' needs " + (bits ? "an Int operand" : "an Int or Double operand") +
                                 ", found " + describe(v));
            return;
        }
        // Only the narrowing direction remains: a Double operand cannot flow into an Int target.
        // (Unreachable for the bitwise forms -- `ok` already rejected every Double there.)
        if (t->kind == TyKind::Int && v->kind == TyKind::Double)
            error(a.line, a.col, "type mismatch: expected Int, found Double");
    }

    void check_assign(AssignStmt& a) {
        if (a.target->kind == ExprKind::Ident) {
            auto& id = static_cast<IdentExpr&>(*a.target);
            if (id.qualifier.empty() && !id.upper) {
                ScopeVar* v = lookup(id.name);
                if (!v) {
                    error(a.target->line, a.target->col,
                          "unknown variable '" + id.name + "'" + missing_use_hint(id.name));
                    infer(*a.value);
                    return;
                }
                // THE LAMBDA WRITE BARRIER. A capture is a by-value copy of the BINDING, so storing
                // into the name inside a lambda could never be seen outside it -- there is no code to
                // generate, and codegen would fail with a message blaming a name that is in scope and
                // `mut`. Reported BEFORE the mutability test on purpose: telling
                // someone to add `mut` to a captured binding is advice that cannot work.
                //
                // The discriminator is the TARGET FORM, not its root. `o.f = v` / `a[i] = v` through a
                // captured name stay legal below: the copy is a POINTER, so the mutation lands in the
                // one shared object and is observable outside (SkarnGuide.md section 10 teaches this,
                // and demo/map_helpers.skn is built on it).
                const bool captured = is_captured_name(id.name);
                if (captured)
                    error(a.target->line, a.target->col,
                          "cannot assign to '" + id.name + "' inside a lambda -- it is captured by "
                          "value, so the write would be invisible outside; mutate through a captured "
                          "object (`o.f = ...` / `a[i] = ...`) or return the new value");
                else if (!v->is_mut)
                    error(a.target->line, a.target->col,
                          "cannot assign to immutable binding '" + id.name + "'");
                // The OUTWARD half of the same rule, one tier down. A `let`-bound lambda snapshotted
                // this binding by value at closure-creation time, so rebinding the NAME here leaves
                // it holding the old value -- silently, and the guide's own section 10 example turns
                // on exactly that. Advisory, not an error: the program is well defined and this is
                // occasionally what someone meant. Reported above the compound dispatch below so
                // `n += 1` is covered by the same guard as `n = 1`. Rebinding only: mutating through
                // the name (`o.f = v` / `a[i] = v` / `push(v, x)`) reaches the one shared object and
                // the lambda DOES see it, so those paths never get here.
                if (!captured && v->cap_line != 0)
                    warn(a.target->line, a.target->col,
                         "'" + id.name + "' is reassigned after a lambda captured it -- the lambda "
                         "kept a by-value snapshot and will not see this value",
                         { DiagLabel{ "captured here", v->cap_line, v->cap_col, cur_module_ } });
                // Fall through either way: the right-hand side still gets checked, so one bad target
                // does not swallow every diagnostic inside its own value expression.
                a.target->ty = v->ty;
                if (a.op != TokKind::Assign) { check_compound_assign(a, v->ty); return; }
                check_expr(*a.value, v->ty);
                return;
            }
        }
        // A field / index lvalue requires the ROOT binding to be `mut` -- else a
        // non-`mut` value could be mutated under the programmer via `p.x = …` / `a[i] = …`.
        if (a.target->kind == ExprKind::Field || a.target->kind == ExprKind::Index) {
            if (IdentExpr* root = assignment_root(a.target.get())) {
                if (ScopeVar* v = lookup(root->name); v && !v->is_mut)
                    error(a.target->line, a.target->col,
                          "cannot mutate through immutable binding '" + root->name +
                          "' (declare it 'mut')");
                // (v == nullptr -> unknown variable; `infer` below reports it.)
            } else {
                error(a.target->line, a.target->col, "cannot assign to a temporary");
            }
        }
        TyPtr t = infer(*a.target);   // a field / index lvalue
        if (a.op != TokKind::Assign) { check_compound_assign(a, t); return; }
        check_expr(*a.value, t);
    }

    // ----- expressions ------------------------------------------------------
    TyPtr infer(Expr& e) {
        DepthGuard depth(*this, e);
        if (too_deep(e)) { e.ty = ty_error(); return e.ty; }
        TyPtr t = infer_impl(e);
        // Store the APPLIED type on the node: the solver's union-find is torn down when
        // check() returns, so codegen (erasure) must read a resolved type off Expr::ty --
        // e.g. a field-access receiver's concrete struct name. Returning `t` keeps the
        // checker's own callers (which apply as needed) unchanged.
        e.ty = apply(t);
        return t;
    }

    // NOTE: `expected` is taken BY VALUE, not `const TyPtr&`. A caller may pass a reference
    // into the scope vector (e.g. check_assign's `v->ty`), and checking `e` can `define()` new
    // bindings (a lambda param, a block/match binding) that REALLOCATE that vector -- which
    // would dangle a by-reference `expected` and fault in a later apply()/subsumes(). Copying
    // the shared_ptr at the boundary keeps it alive and stable regardless of scope growth.
    // `origin`, when non-null, is a secondary "note" caret describing WHERE the expected type
    // came from (an annotation / a declared return type). It is consumed only at the generic
    // "type mismatch" emit below and forwarded to the TAIL-position helpers (block/if/match) so
    // the note tracks the value that actually produced the type. It is passed EXPLICITLY (never
    // ambient), so a descent into a call argument / operator operand passes null and gets no
    // secondary caret -- the annotation is not the reason THOSE positions have their type.
    // Record on the NODE that this expression is an `Int` accepted where a `Double` was expected, so
    // codegen can widen without re-deriving the question. Called at BOTH of check_expr's non-delegating
    // exits. The delegating ones (Lambda/Block/If/Match/Loop/ListLit/MapLit) push `expected` inward and
    // return early, so their leaves get marked by their own nested check_expr calls -- which is exactly
    // where the widening has to happen for them anyway.
    void mark_widen(Expr& e, const TyPtr& actual, const TyPtr& expected) {
        const TyPtr a = apply(actual), x = apply(expected);
        e.widen_double = a && x && a->kind == TyKind::Int && x->kind == TyKind::Double;
    }

    // The complement of `mark_widen`, for the DELEGATING constructs (Block/If/Match/Loop). Such a
    // construct produces no value of its own -- its leaves do, each of them checked against the same
    // `expected`, so a leaf that took the `Int <: Double` edge carries `widen_double` and widens
    // ITSELF. The construct must then report the type its leaves actually produce, or every site that
    // derives a coercion from the PARENT type (the return boundary, a `let` annotation, a struct
    // field) widens a SECOND time -- and `I2D` on double bits reads garbage through `asSigned48()`,
    // which is a Debug assert and in Release a silent `0.0`. One value, one widening, at the leaf.
    void settle_delegated(Expr& e, const TyPtr& expected) {
        const TyPtr a = apply(e.ty), x = apply(expected);
        if (a && x && a->kind == TyKind::Int && x->kind == TyKind::Double) e.ty = x;
    }

    void check_expr(Expr& e, TyPtr expected, const DiagLabel* origin = nullptr) {
        // The second funnel of the recursion, next to `infer` -- a delegating construct
        // (Block/If/Match/Loop) recurses through here without passing `infer` at all.
        DepthGuard depth(*this, e);
        if (too_deep(e)) { e.ty = ty_error(); return; }
        // Bidirectional cases: push the expected type inward so lambda params,
        // block/if/match tails, and empty container literals get their types from
        // context (e.g. `fn f() -> List[Int] { [] }`).
        if (e.kind == ExprKind::Lambda) { check_lambda(static_cast<LambdaExpr&>(e), apply(expected)); return; }
        // These four DELEGATE: they push `expected` into their leaves and the leaves carry the
        // `Int <: Double` record, so `settle_delegated` reports the type the leaves produce.
        if (e.kind == ExprKind::Block)  { check_block(static_cast<BlockExpr&>(e), expected, origin); settle_delegated(e, expected); return; }
        if (e.kind == ExprKind::If)     { check_if(static_cast<IfExpr&>(e), expected, origin);    settle_delegated(e, expected); return; }
        if (e.kind == ExprKind::Match)  { check_match(static_cast<MatchExpr&>(e), expected, origin); settle_delegated(e, expected); return; }
        if (e.kind == ExprKind::Loop)   { check_loop(static_cast<LoopExpr&>(e), expected); settle_delegated(e, expected); return; }
        if (e.kind == ExprKind::ListLit && check_list_expected(static_cast<ListLit&>(e), expected)) return;
        if (e.kind == ExprKind::MapLit  && check_map_expected(static_cast<MapLit&>(e), expected))  return;
        // A byte int-literal (0..255, e.g. the char literal 'A') in a Char-expected context becomes a
        // Char (the zero-cost code-point type). LITERAL-ONLY: an arbitrary Int expression does NOT coerce
        // -- write `Char(x)` -- which keeps the type distinction meaningful. Char erases to the code-point
        // Int, so codegen emits the literal unchanged (no coercion opcode). A code point > 255 needs the
        // explicit `Char(cp)` (char literals are single-byte).
        if (e.kind == ExprKind::IntLit && is_char_ty(expected)) {
            int64_t v = static_cast<IntLit&>(e).value;
            if (v >= 0 && v <= 255) { e.ty = apply(expected); return; }
        }
        // A BOUNDED generic fn used as a value, checked against a concrete `fn(...)->R`:
        // monomorphize it here and discharge the bound against the (now concrete) expected
        // type -- the only sound way to give it away as a value (see check_bounded_fn_value).
        // Outside a concrete-fn expected type the rejection in infer_ident still stands.
        if (e.kind == ExprKind::Ident) {
            auto& id = static_cast<IdentExpr&>(e);
            if (id.qualifier.empty() && !id.upper && !lookup(id.name)) {
                const std::string key = mangle_ref(id.name);
                if (const FnSig* sig = user_fn(key, &id)) {
                    bool bounded = false;
                    for (const auto& g : sig->generics) if (!g.bounds.empty()) { bounded = true; break; }
                    TyPtr exp = apply(expected);
                    if (bounded && exp->kind == TyKind::Fn) { id.name = key; check_bounded_fn_value(id, *sig, exp, e); return; }
                }
            }
        }
        // A call in check position: thread the expected type into the call so a return-type-determined
        // bounded generic (`let s: Set[P] = Set::new()`) gets its type argument pinned before bound discharge
        // (see call_direct_fn). Mirror infer()'s Expr::ty bookkeeping, then do the ordinary subsumes.
        if (e.kind == ExprKind::Call) {
            auto& c = static_cast<CallExpr&>(e);
            std::vector<Expr*> cargs;
            cargs.reserve(c.args.size());
            for (auto& a : c.args) cargs.push_back(a.get());
            TyPtr actual = infer_call(*c.callee, cargs, c, apply(expected));
            c.ty = apply(actual);
            mark_widen(e, actual, expected);
            if (!subsumes(actual, expected)) {
                TypeRenderer r;
                std::vector<DiagLabel> labels;
                if (origin) labels.push_back(*origin);
                error(e.line, e.col, "type mismatch: expected " + r(apply(expected)) +
                                     ", found " + r(apply(actual)) + iterator_lift_hint(actual, expected), std::move(labels));
            }
            return;
        }
        TyPtr actual = infer(e);
        mark_widen(e, actual, expected);
        if (!subsumes(actual, expected)) {
            TypeRenderer r;
            std::vector<DiagLabel> labels;
            if (origin) labels.push_back(*origin);
            error(e.line, e.col, "type mismatch: expected " + r(apply(expected)) +
                                 ", found " + r(apply(actual)) + iterator_lift_hint(actual, expected), std::move(labels));
        }
    }

    // A bounded generic fn (`fn describe[T: Display](x: T) -> String`) used as a first-class
    // value where a CONCRETE `fn(...)->R` is expected: instantiate it (bound-carrying fresh
    // vars, like call_direct_fn), unify against the expected fn type, then discharge each bound
    // against the SOLVED type argument. Sound only because we require each bounded type param to
    // resolve to a concrete or rigid (bound-carrying) type -- an unsolved inference var is
    // rejected, so a bound is never trivially passed. Sets `e.ty` to the
    // now-monomorphic fn type.
    void check_bounded_fn_value(IdentExpr& id, const FnSig& sig, const TyPtr& expectedFn, Expr& e) {
        std::unordered_map<uint32_t, TyPtr> m;
        for (const auto& g : sig.generics) m[g.id] = tc_.fresh_var(g.name, g.bounds);
        std::vector<TyPtr> params;
        for (const auto& p : sig.params) params.push_back(tc_.substitute(p, m));
        TyPtr inst = make_fn(std::move(params), tc_.substitute(sig.ret, m));
        if (!tc_.subsumes(inst, expectedFn)) {
            TypeRenderer r;
            error(e.line, e.col, "type mismatch: expected " + r(apply(expectedFn)) +
                                 ", found " + r(apply(inst)));
            e.ty = apply(expectedFn);
            return;
        }
        for (const auto& g : sig.generics) {
            if (g.bounds.empty()) continue;
            TyPtr solved = apply(m[g.id]);
            if (solved->kind == TyKind::Var && !solved->rigid) {
                error(e.line, e.col, "cannot use generic function '" + id.name +
                      "' as a value here: its type parameter '" + g.name +
                      "' is not determined by the expected type; call it directly, or use it "
                      "where a concrete function type is expected");
                e.ty = apply(expectedFn);
                return;
            }
            for (const auto& b : g.bounds) {
                Bound bi;
                bi.trait = b.trait;
                for (const auto& a : b.args) bi.args.push_back(tc_.substitute(a, m));
                if (!discharge_parametric_bound(solved, bi, e.line, e.col))
                    error(e.line, e.col, "type " + describe(solved) + " does not satisfy the bound '" + describe_bound(bi) + "'");
            }
        }
        e.ty = apply(inst);
    }

    TyPtr infer_impl(Expr& e) {
        switch (e.kind) {
            case ExprKind::IntLit:
                // A bare (positive) integer literal must fit MAX_48. The negated form
                // `-N` reaches MIN_48 and is validated in infer_unary (which then skips
                // inferring the operand, so this branch never double-flags it).
                if (static_cast<IntLit&>(e).value > INT48_MAX)
                    error(e.line, e.col, "integer literal out of range for a 48-bit Int");
                return ty_int();
            case ExprKind::DoubleLit: return ty_double();
            case ExprKind::StrLit:    return ty_string();
            case ExprKind::BoolLit:   return ty_bool();
            case ExprKind::Ident:     return infer_ident(static_cast<IdentExpr&>(e));
            case ExprKind::Unary:     return infer_unary(static_cast<UnaryExpr&>(e));
            case ExprKind::Binary:    return infer_binary(static_cast<BinaryExpr&>(e));
            case ExprKind::Pipe:      return infer_pipe(static_cast<PipeExpr&>(e));
            case ExprKind::Call:      return infer_call_expr(static_cast<CallExpr&>(e));
            case ExprKind::Field:     return infer_field(static_cast<FieldExpr&>(e));
            case ExprKind::Index:     return infer_index(static_cast<IndexExpr&>(e));
            case ExprKind::Tuple:     return infer_tuple(static_cast<TupleExpr&>(e));
            case ExprKind::ListLit:   return infer_list(static_cast<ListLit&>(e));
            case ExprKind::MapLit:    return infer_map(static_cast<MapLit&>(e));
            case ExprKind::StructLit: return infer_struct_lit(static_cast<StructLit&>(e));
            case ExprKind::Block:     return infer_block(static_cast<BlockExpr&>(e));
            case ExprKind::If:        return infer_if(static_cast<IfExpr&>(e));
            case ExprKind::While:     return infer_while(static_cast<WhileExpr&>(e));
            case ExprKind::Return:    return infer_return(static_cast<ReturnExpr&>(e));
            case ExprKind::Match:     return infer_match(static_cast<MatchExpr&>(e));
            case ExprKind::Break:     return infer_break(static_cast<BreakExpr&>(e));
            case ExprKind::Continue:
                if (loop_frames_.empty()) error(e.line, e.col, "'continue' outside of a loop");
                return ty_never();
            case ExprKind::Lambda:    return infer_lambda(static_cast<LambdaExpr&>(e));
            case ExprKind::Try:       return infer_try(static_cast<TryExpr&>(e));
            case ExprKind::For:       return infer_for(static_cast<ForExpr&>(e));
            case ExprKind::Loop:      return infer_loop(static_cast<LoopExpr&>(e));
        }
        return ty_error();
    }

    TyPtr infer_ident(IdentExpr& e) {
        if (!e.qualifier.empty()) {
            if (is_module_qualifier(e.qualifier)) {          // `mod::Red` value / `mod::helper` fn value
                const std::string mod = e.qualifier;
                const bool is_std = (mod == "std");          // virtual std module = ambient/prelude (bare)
                if (!is_std && !imported_by_current(mod)) {
                    error(e.line, e.col, "module '" + mod + "' is not imported");
                    return ty_error();
                }
                const std::string key = is_std ? resolve_std(e.name) : (mod + "::" + e.name);
                if (!is_std && !visible_across(key)) {       // default private
                    error(e.line, e.col, "item '" + e.name + "' is private in module '" + mod + "'");
                    return ty_error();
                }
                e.qualifier.clear(); e.name = key;           // writeback (unqualified; mangled unless std)
                if (auto cit = consts_.find(key); cit != consts_.end() && cit->second)
                    return const_ref_type(e, cit->second);   // a module const (`math::PI`)
                if (e.upper) {                               // a nullary ctor value like `pal::Red`
                    TyPtr ct = infer_ctor_value(e);
                    if (ct->kind != TyKind::Error) return ct;
                    // Fall through to this branch's own wording rather than leaving the failure
                    // silent (see report_unknown_uppercase).
                    error(e.line, e.col, "module '" + mod + "' has no value '" + short_name(key) + "'");
                    return ty_error();
                }
                if (auto it = fns_.find(key); it != fns_.end()) {
                    for (const auto& g : it->second.generics)
                        if (!g.bounds.empty()) {
                            error(e.line, e.col, "generic function '" + key + "' with a trait bound "
                                  "cannot be used as a first-class value; wrap it in a lambda");
                            return ty_error();
                        }
                    return instantiate_fn(it->second);
                }
                error(e.line, e.col, "module '" + mod + "' has no value '" + e.name + "'");
                return ty_error();
            }
            if (e.upper) {                               // `Enum::Variant` used as a VALUE
                std::string vkey = resolve_qualified_variant(e.qualifier, e.name, e.line, e.col);
                if (vkey.empty()) return ty_error();
                e.qualifier.clear(); e.name = vkey;
                return infer_ctor_value(e);
            }
            error(e.line, e.col, "'" + e.qualifier + "::" + e.name + "' can only be used as a call target");
            return ty_error();
        }
        if (e.upper) {
            // An uppercase name is a constructor OR an uppercase const (`PI`); check const first.
            const std::string ckey = mangle_ref(e.name);
            if (auto cit = consts_.find(ckey); cit != consts_.end() && cit->second) {
                e.name = ckey; return const_ref_type(e, cit->second);
            }
            if (report_if_ambiguous_variant(e.name, e.line, e.col)) return ty_error();   // collision
            TyPtr ct = infer_ctor_value(e);
            if (ct->kind != TyKind::Error) return ct;
            report_unknown_uppercase(e, ckey);
            return ty_error();
        }
        if (ScopeVar* v = lookup(e.name)) {   // a local shadows a const
            v->used = true;
            note_capture(e.name, e.line, e.col);
            return v->ty;
        }
        const std::string key = mangle_ref(e.name);   // a local shadows; else module-qualify
        if (const FnSig* usig = user_fn(key, &e)) {
            e.name = key;   // writeback: codegen resolves the fn value by this (mangled) name
            // A generic fn with trait bounds cannot become a first-class `Fn` value
            // -- an `Fn` type has nowhere to record `T: Display`, so the bound would be
            // silently dropped and an unsatisfying type could slip through the eventual
            // indirect call. Reject (sound); direct calls still discharge via
            // `call_direct_fn`. (A richer future option threads bounds through the `Fn`
            // type)
            for (const auto& g : usig->generics)
                if (!g.bounds.empty()) {
                    error(e.line, e.col, "generic function '" + e.name + "' with a trait bound "
                          "cannot be used as a first-class value; wrap it in a lambda");
                    return ty_error();
                }
            return instantiate_fn(*usig);
        }
        if (auto cit = consts_.find(key); cit != consts_.end() && cit->second) {
            e.name = key; return const_ref_type(e, cit->second);   // a lowercase module const (after local + fn)
        }
        if (method_owners_.count(e.name)) {
            error(e.line, e.col, "trait method '" + e.name + "' must be called, not used as a value");
            return ty_error();
        }
        // A builtin or native is call-only: it lowers to an opcode / CALL_NATIVE, with no function object
        // behind it. Saying so beats "unknown variable 'toString'" for `map(toString)` -- wrong on its face
        // for a name the same program calls two lines later -- while `map(trim)`, a prelude fn, just works.
        if (is_builtin_fn_name(e.name) || (native_id_of(e.name) >= 0 && native_available(e.name))) {
            error(e.line, e.col, "built-in function '" + e.name + "' can only be called, not used as a value "
                  "-- wrap it in a lambda, e.g. `fn(x: T) -> U { " + e.name + "(x) }`");
            return ty_error();
        }
        error(e.line, e.col, "unknown variable '" + e.name + "'" + missing_use_hint(e.name));
        return ty_error();
    }

    TyPtr instantiate_fn(const FnSig& sig) {
        std::vector<uint32_t> ids;
        for (const auto& g : sig.generics) ids.push_back(g.id);
        return tc_.instantiate(make_fn(sig.params, sig.ret), ids);
    }

    // A constructor used as a value: nullary -> the built type; with fields -> the
    // constructor FUNCTION `fn(field-types) -> Type` (so `Some(x)` type-checks as a
    // call). Generics are instantiated to fresh vars, solved by the call's args.
    // Diagnose an uppercase name in VALUE position that resolved to no constructor. Kept out of
    // `infer_ctor_value` (a pure resolver with three callers, each wanting its own wording).
    //
    // It must REPORT: returning `ty_error()` silently would leave `errors_` empty, pass the program,
    // and let codegen raise an unlocated `CodegenError: undefined variable 'X'` instead -- which in a
    // multi-module program carries no module id, so the caret would land on the WRONG FILE.
    void report_unknown_uppercase(const IdentExpr& e, const std::string& key) {
        // A record struct is a real type -- it just needs its fields, so say that instead of
        // "unknown".
        if (auto sit = structs_.find(key); sit != structs_.end() && !sit->second.is_tuple) {
            error(e.line, e.col, "struct '" + e.name + "' needs field initializers -- write `" +
                  e.name + " { ... }`");
            return;
        }
        if (enums_.count(key)) {
            error(e.line, e.col, "'" + e.name +
                  "' is an enum type, not a value; use one of its variants");
            return;
        }
        // An uppercase value is a constant (`PI`) as often as a constructor, so name both.
        error(e.line, e.col, "unknown constant or constructor '" + e.name + "'" + missing_use_hint(e.name));
    }

    TyPtr infer_ctor_value(IdentExpr& e) {
        const std::string key = mangle_ref(e.name);   // module-qualify the constructor ref
        if (auto it = variants_.find(key); it != variants_.end()) {
            e.name = key;   // writeback for codegen (the enum/variant is registered mangled)
            const VariantInfo& vi = it->second;
            auto eit = enums_.find(vi.enum_name);
            size_t n = (eit != enums_.end()) ? eit->second.generics.size() : 0;
            std::vector<TyPtr> targs = fresh_args(n);
            std::unordered_map<uint32_t, TyPtr> m;
            if (eit != enums_.end())
                for (size_t i = 0; i < eit->second.generics.size(); ++i) m[eit->second.generics[i].id] = targs[i];
            TyPtr made = make_named(vi.enum_name, targs);
            if (vi.field_types.empty()) return made;
            std::vector<TyPtr> ptys;
            for (const auto& ft : vi.field_types) ptys.push_back(tc_.substitute(ft, m));
            return make_fn(std::move(ptys), made);
        }
        if (auto it = structs_.find(key); it != structs_.end() && it->second.is_tuple) {
            e.name = key;   // writeback for codegen
            const StructInfo& si = it->second;
            std::vector<TyPtr> targs = fresh_args(si.generics.size());
            std::unordered_map<uint32_t, TyPtr> m;
            for (size_t i = 0; i < si.generics.size(); ++i) m[si.generics[i].id] = targs[i];
            TyPtr made = make_named(key, targs);
            if (si.field_types.empty()) return made;
            std::vector<TyPtr> ptys;
            for (const auto& ft : si.field_types) ptys.push_back(tc_.substitute(ft, m));
            return make_fn(std::move(ptys), made);
        }
        return ty_error();
    }

    TyPtr infer_unary(UnaryExpr& e) {
        // `-N` over an integer literal is a single negative literal whose valid
        // magnitude reaches MIN_48 = -2^47 (one past +MAX_48). Validate it as a unit
        // and do NOT infer the operand on its own (that would flag +2^47 spuriously).
        if (e.op == TokKind::Minus && e.operand->kind == ExprKind::IntLit) {
            if (static_cast<IntLit&>(*e.operand).value > INT48_NEG_MAG)
                error(e.line, e.col, "integer literal out of range for a 48-bit Int");
            e.operand->ty = ty_int();
            return ty_int();
        }
        TyPtr t = apply(infer(*e.operand));
        switch (e.op) {
            case TokKind::Minus:
                if (numeric(t)) return t;
                if (t->kind == TyKind::Error) return ty_error();
                error(e.line, e.col, "unary '-' requires a numeric operand, found " + describe(t) + generic_hint(t));
                return ty_error();
            case TokKind::Tilde:
                if (t->kind == TyKind::Int || t->kind == TyKind::Error) return ty_int();
                error(e.line, e.col, "'~' requires an Int operand, found " + describe(t) + generic_hint(t));
                return ty_int();
            case TokKind::Not:
                if (t->kind == TyKind::Bool || t->kind == TyKind::Error) return ty_bool();
                error(e.line, e.col, "'!' requires a Bool operand, found " + describe(t));
                return ty_bool();
            default:
                return ty_error();
        }
    }

    TyPtr infer_binary(BinaryExpr& e) {
        if (e.op == TokKind::AndAnd || e.op == TokKind::OrOr) {
            check_expr(*e.lhs, ty_bool());
            check_expr(*e.rhs, ty_bool());
            return ty_bool();
        }
        TyPtr l = apply(infer(*e.lhs));
        TyPtr r = apply(infer(*e.rhs));
        // Char literal coercion in operator position: a byte int-literal opposite a Char operand becomes
        // a Char (so `c == 'A'` / `'a' <= c` work). Literal-only, symmetric; the erased runtime value is
        // the code-point Int, so `==`/`<` lower to the generic EQ/LT on Ints (no codegen change).
        auto coerce_char_operand = [&](Expr& operand, TyPtr& operand_ty, const TyPtr& other_ty) {
            if (is_char_ty(other_ty) && operand.kind == ExprKind::IntLit) {
                int64_t v = static_cast<IntLit&>(operand).value;
                if (v >= 0 && v <= 255) { operand_ty = apply(other_ty); operand.ty = operand_ty; }
            }
        };
        coerce_char_operand(*e.lhs, l, r);
        coerce_char_operand(*e.rhs, r, l);
        // An operand that is still an UNSOLVED flexible variable after an error was already reported is
        // almost always that error's shadow -- a lambda whose argument failed to check never gets its
        // parameter type (`xs |> map(fn(x) { x * 2 })` with `xs` a Vec would add "found T and Int").
        // Only then is it treated like Error; with no earlier error the operator still reports it.
        const auto unsolved = [&](const TyPtr& t) { return t->kind == TyKind::Var && !t->rigid; };
        const bool anyErr = (l->kind == TyKind::Error || r->kind == TyKind::Error) ||
                            (!errors_.empty() && (unsolved(l) || unsolved(r)));
        TypeRenderer tr;   // per-message variable naming; at most one operator error fires below
        switch (e.op) {
            case TokKind::Plus:
                // Java-style `+`: a String on EITHER side concatenates, coercing the other operand to
                // text -- but SCALARS ONLY (String / number / Bool). A heap operand (struct/array/map/
                // tuple/enum/closure) still needs an explicit toString(x). Sound: the coercion is decided
                // from concrete operand types here, and codegen stringifies the non-String operand via the
                // total TO_STRING opcode.
                if (l->kind == TyKind::String || r->kind == TyKind::String) {
                    if (anyErr) return ty_error();
                    auto concat_ok = [&](const TyPtr& t) {
                        return t->kind == TyKind::String || numeric(t) || t->kind == TyKind::Bool;
                    };
                    if (concat_ok(l) && concat_ok(r)) return ty_string();
                    error(e.line, e.col, "'+' concatenates a String only with a String, number, or Bool "
                                         "(found " + tr(l) + " and " + tr(r) + ")");
                    return ty_error();
                }
                [[fallthrough]];
            case TokKind::Minus: case TokKind::Star: case TokKind::Slash: case TokKind::Percent:
                if (anyErr) return ty_error();
                if (numeric(l) && numeric(r))
                    return (l->kind == TyKind::Double || r->kind == TyKind::Double) ? ty_double() : ty_int();
                error(e.line, e.col, "arithmetic operator requires numeric operands (found " +
                                     tr(l) + " and " + tr(r) + ")" + generic_hint(l, r));
                return ty_error();
            // Int-only, both sides. `is_bitwise_op` mirrors this case list for the compound
            // `op=` forms -- change both together.
            case TokKind::BitAnd: case TokKind::BitOr: case TokKind::BitXor:
            case TokKind::Shl: case TokKind::Shr: case TokKind::UShr:
                if (anyErr) return ty_error();
                if (l->kind == TyKind::Int && r->kind == TyKind::Int) return ty_int();
                error(e.line, e.col, "bitwise/shift operator requires Int operands (found " +
                                     tr(l) + " and " + tr(r) + ")" + generic_hint(l, r));
                return ty_error();
            case TokKind::Lt: case TokKind::Le: case TokKind::Gt: case TokKind::Ge:
                if (anyErr) return ty_bool();
                if ((numeric(l) && numeric(r)) ||
                    (l->kind == TyKind::String && r->kind == TyKind::String) ||
                    (is_char_ty(l) && is_char_ty(r))) return ty_bool();   // Char: erased code-point Int order
                error(e.line, e.col, "comparison requires two numbers or two strings (found " +
                                     tr(l) + " and " + tr(r) + ")" + generic_hint(l, r));
                return ty_bool();
            case TokKind::EqEq: case TokKind::NotEq: {
                if (anyErr) return ty_bool();
                TyPtr j = apply(tc_.join(l, r));
                if (j->kind == TyKind::Error) {
                    error(e.line, e.col, "cannot compare " + tr(l) + " with " + tr(r));
                } else if (!is_eq(j)) {
                    // Structural `==`: a value is comparable iff all its components are (see is_eq).
                    // A function/closure component, a trait object, or an unbounded type parameter
                    // has no structural equality -- reject rather than silently compare references.
                    error(e.line, e.col, "type " + tr(j) + " does not support '==' / '!=' "
                          "(it contains a function value, a trait object, or an unbounded type "
                          "parameter -- add a 'T: Eq' bound, compare fields directly, or use `match`)");
                }
                return ty_bool();
            }
            default:
                return ty_error();
        }
    }

    TyPtr infer_pipe(PipeExpr& e) {
        // x |> f  /  x |> f(a, ...)   ==   f(x)  /  f(x, a, ...)
        std::vector<Expr*> args;
        args.push_back(e.lhs.get());
        if (e.rhs->kind == ExprKind::Call) {
            auto& c = static_cast<CallExpr&>(*e.rhs);
            for (auto& a : c.args) args.push_back(a.get());
            return infer_call(*c.callee, args, e);
        }
        return infer_call(*e.rhs, args, e);
    }

    TyPtr infer_call_expr(CallExpr& c) {
        std::vector<Expr*> args;
        for (auto& a : c.args) args.push_back(a.get());
        return infer_call(*c.callee, args, c);
    }

    // `expected` is the type the call result is checked against (check-mode only; null in infer
    // mode). It is threaded to `call_direct_fn` so a return-type-determined bounded generic
    // (`set[T: Hashable]()`) gets its type argument pinned before bound discharge.
    TyPtr infer_call(Expr& callee, const std::vector<Expr*>& args, Expr& node,
                     TyPtr expected = nullptr) {
        if (callee.kind == ExprKind::Ident) {
            auto& id = static_cast<IdentExpr&>(callee);
            if (!id.qualifier.empty()) {
                if (is_module_qualifier(id.qualifier)) {   // `mod::name(...)` -- module-qualified call
                    callee.ty = ty_error();
                    return call_module_member(id, args, node);
                }
                callee.ty = ty_error();
                if (id.upper) {                            // `Enum::Variant(...)` -- qualified ctor call
                    std::string vkey = resolve_qualified_variant(id.qualifier, id.name, node.line, node.col);
                    if (vkey.empty()) { for (Expr* a : args) infer(*a); return ty_error(); }
                    id.qualifier.clear(); id.name = vkey;
                    return call_ctor(id, args, node);
                }
                // An uppercase qualifier + lowercase name is `Trait::method(x)` OR `Type::method(x)`
                // (a qualified INHERENT call). Both resolve the head with the same `mangle_ref`, and
                // a trait and a type MAY share that key (`trait Foo` + `struct Foo` is legal), so the
                // choice is made HERE, once, and written back for codegen + the oracle to obey.
                id.qualifier = mangle_ref(id.qualifier);   // module-qualify the head (+ writeback)
                const bool as_trait    = trait_has_method(id.qualifier, id.name);
                const bool as_inherent = inherent_has_method(id.qualifier, id.name);
                if (as_trait && as_inherent) {
                    // Genuinely ambiguous: silently preferring either one is the class of bug that
                    // costs the most to find. Name both and let the author disambiguate -- except
                    // that an ASSOCIATED function has no `.` spelling to escape to, so there the
                    // only remedy is a rename.
                    const std::string fix = inherent_member_has_self(id.qualifier, id.name)
                        ? "use `x." + id.name + "(...)` for the inherent one"
                        : "'" + id.name + "' is an associated function of the type, which has no `.` "
                          "form -- rename one of the two";
                    error(node.line, node.col, "'" + short_name(id.qualifier) + "::" + id.name +
                          "' is ambiguous: trait '" + short_name(id.qualifier) + "' and type '" +
                          short_name(id.qualifier) + "' both have a method '" + id.name + "'; " + fix);
                    for (Expr* a : args) infer(*a);
                    return ty_error();
                }
                if (as_inherent)
                    return inherent_member_has_self(id.qualifier, id.name)
                        ? call_qualified_inherent(id, id.qualifier, args, node)
                        : call_associated_fn(id, id.qualifier, args, node, expected);
                if (!traits_.count(id.qualifier)) {
                    // Not a trait at all. Naming the head's actual kind beats a blanket
                    // "unknown trait 'Counter'", which sends the reader looking for a missing trait.
                    const std::string bare = short_name(id.qualifier);
                    if (structs_.count(id.qualifier) || enums_.count(id.qualifier))
                        error(node.line, node.col, "type '" + bare + "' has no method '" + id.name +
                              (inherent_.count(id.qualifier) ? "'"
                                                             : "' (it has no inherent `impl` block)"));
                    else
                        error(node.line, node.col, "unknown trait or type '" + bare + "'" +
                              missing_use_hint(bare));
                    for (Expr* a : args) infer(*a);
                    return ty_error();
                }
                return call_trait_method(id.qualifier, id.name, args, node);
            }
            if (id.upper) {                            // `V(...)` / `S(...)` constructor call
                const std::string key = mangle_ref(id.name);   // module-qualify the ctor ref
                bool is_variant = variants_.count(key) > 0;
                bool is_tuple_struct = false;
                if (auto sit = structs_.find(key); sit != structs_.end())
                    is_tuple_struct = sit->second.is_tuple;
                if (is_variant || is_tuple_struct) {
                    id.name = key;   // writeback for codegen; call_ctor looks up the mangled name
                    callee.ty = ty_error();
                    return call_ctor(id, args, node);
                }
                if (report_if_ambiguous_variant(id.name, node.line, node.col)) {   // collision
                    for (Expr* a : args) infer(*a);
                    callee.ty = ty_error();
                    return ty_error();
                }
            }
            if (id.qualifier.empty() && !id.upper && !lookup(id.name)) {
                // A direct call to a top-level fn (not shadowed) -> instantiate + bounds. A user
                // fn is module-qualified; a builtin (checked below on the bare name) stays ambient.
                const std::string key = mangle_ref(id.name);
                if (const FnSig* sig = user_fn(key, &id)) {
                    id.name = key;   // writeback for codegen
                    callee.ty = ty_error();
                    return call_direct_fn(*sig, short_name(key), args, node, expected);
                }
                // push / pop are polymorphic over Vec AND Bytes (the VM's VEC_PUSH/VEC_POP are
                // tri-kind), which one generic FnSig cannot express -- special-case before the
                // generic builtin_fns_ dispatch below (the builtin_fns_ push/pop rows go unused).
                if (id.name == "push") { callee.ty = ty_error(); return check_push(args, node); }
                if (id.name == "pop")  { callee.ty = ty_error(); return check_pop(args, node); }
                // Bulk byte-blit builtins (VM BYTES_APPEND / BYTES_APPEND_RANGE) -- one name over
                // String|Bytes sources, special-cased like push.
                if (id.name == "appendBytes")       { callee.ty = ty_error(); return check_append_bytes(args, node); }
                if (id.name == "_appendBytesRange") { callee.ty = ty_error(); return check_append_bytes_range(args, node); }
                // A container-constructing builtin (array / vec), an ambient native, or a GATED
                // opt-in native. A gated native (readFile/rawRun/getEnv/...) requires its module to
                // be in scope -- reject with a `use` hint otherwise.
                if (auto bit = builtin_fns_.find(id.name); bit != builtin_fns_.end()) {
                    if (!native_available(id.name)) {
                        error(node.line, node.col, "native '" + id.name + "' requires `use " +
                              native_module_.at(id.name) + "::*` or `use " + native_module_.at(id.name) +
                              "::" + id.name + "`");
                        for (Expr* a : args) infer(*a);
                        return ty_error();
                    }
                    callee.ty = ty_error();
                    return call_direct_fn(bit->second, id.name, args, node, expected);
                }
                if (id.name == "len") { callee.ty = ty_error(); return check_len(args, node); }
                if (id.name == "print")   { callee.ty = ty_error(); return check_print(args, node); }
                if (id.name == "println") { callee.ty = ty_error(); return check_println(args); }
                if (id.name == "panic")   { callee.ty = ty_error(); return check_panic(args, node); }
                if (id.name == "has")     { callee.ty = ty_error(); return check_map_kv_bool(args, node, "has"); }
                if (id.name == "delete")  { callee.ty = ty_error(); return check_map_kv_bool(args, node, "delete"); }
                if (id.name == "get")     { callee.ty = ty_error(); return check_get(args, node); }
                if (id.name == "keys")    { callee.ty = ty_error(); return check_map_collect(args, node, "keys", /*key=*/true); }
                if (id.name == "values")  { callee.ty = ty_error(); return check_map_collect(args, node, "values", /*key=*/false); }
                if (id.name == "mapIterNext") { callee.ty = ty_error(); return check_map_iter(args, node, "mapIterNext", 0); }
                if (id.name == "mapKeyAt")    { callee.ty = ty_error(); return check_map_iter(args, node, "mapKeyAt", 1); }
                if (id.name == "mapValAt")    { callee.ty = ty_error(); return check_map_iter(args, node, "mapValAt", 2); }
                if (id.name == "toInt")     { callee.ty = ty_error(); return check_mono1(args, node, "toInt", ty_double(), ty_int()); }
                // The mirror of toInt. Widening an Int to a Double DOES happen implicitly at a check
                // boundary, but only there -- so an expression like `x / (n - 1)` on two Ints silently
                // stays integer division, and there was no named way to ask for the widening. Strict on
                // its argument (a Double arg is "expected Int, found Double"), because toDouble(3.5) is
                // always a mistake rather than a no-op worth accepting silently.
                if (id.name == "toDouble")  { callee.ty = ty_error(); return check_mono1(args, node, "toDouble", ty_int(), ty_double()); }
                // Double rounding builtins (DROUND): Double -> Double. An Int arg coerces via Int <: Double.
                if (id.name == "floor" || id.name == "ceil" || id.name == "trunc" ||
                    id.name == "round" || id.name == "roundHalfToEven") {
                    callee.ty = ty_error(); return check_mono1(args, node, id.name.c_str(), ty_double(), ty_double());
                }
                // Erasure projection, not a numeric conversion: an int-backed enum -> its discriminant.
                if (id.name == "ordinal")   { callee.ty = ty_error(); return check_ordinal(args, node); }
                if (id.name == "toBytes")   { callee.ty = ty_error(); return check_mono1(args, node, "toBytes", ty_string(), make_named("Bytes", {})); }
                if (id.name == "fromBytes") { callee.ty = ty_error(); return check_mono1(args, node, "fromBytes", make_named("Bytes", {}), ty_string()); }
                if (id.name == "bytes")     { callee.ty = ty_error(); return check_bytes(args, node); }
                // An unqualified trait-method call -> dispatch on the receiver (arg 0), which also
                // decides WHICH trait's method when several declare the name. The receiver is inferred
                // once, here, and handed on (the `pre_self` rule of call_trait_method).
                if (auto oit = method_owners_.find(id.name); oit != method_owners_.end()) {
                    callee.ty = ty_error();
                    // Only a choice among several `self` methods needs the receiver first; otherwise
                    // call_trait_method infers (or checks) arg 0 itself, exactly as it always has.
                    const bool pre = oit->second.size() > 1 && !args.empty() && all_take_self(id.name);
                    TyPtr recvTy = pre ? apply(infer(*args[0])) : nullptr;
                    const std::string trait = pick_trait(id.name, recvTy, node.line, node.col);
                    if (trait.empty()) {
                        for (size_t i = pre ? 1 : 0; i < args.size(); ++i) infer(*args[i]);
                        return ty_error();
                    }
                    id.resolved_trait = trait;   // the routing decision; codegen + the oracle obey it
                    return call_trait_method(trait, id.name, args, node, recvTy);
                }
            }
        }
        // Method-call syntax `recv.method(args)`: a call whose callee is a (non-tuple) field
        // access. FIELD-FIRST (a fn-valued field keeps working); otherwise resolve as a method.
        // The receiver is inferred exactly ONCE here and its type handed on -- see call_trait_method.
        TyPtr ct;
        if (callee.kind == ExprKind::Field && !static_cast<FieldExpr&>(callee).tuple_index) {
            auto& fld = static_cast<FieldExpr&>(callee);
            TyPtr recvTy = apply(infer(*fld.obj));
            if (!field_exists(recvTy, fld.name))
                return method_call(recvTy, fld, args, node);
            // A real struct field -> the general value-call path below, typed from the receiver type
            // already in hand (infer(callee) would infer the receiver a second time).
            ct = apply(field_type_of(recvTy, fld));
            callee.ty = ct;
        } else {
            ct = apply(infer(callee));
        }
        if (ct->kind == TyKind::Error) { for (Expr* a : args) infer(*a); return ty_error(); }
        if (ct->kind == TyKind::Fn) {
            if (args.size() != ct->args.size()) {
                error(node.line, node.col, "wrong number of arguments: expected " +
                      std::to_string(ct->args.size()) + ", got " + std::to_string(args.size()));
                for (Expr* a : args) infer(*a);
                return apply(ct->ret);
            }
            check_args_two_pass(args, ct->args);
            return apply(ct->ret);
        }
        error(node.line, node.col, "cannot call a value of type " + describe(ct));
        for (Expr* a : args) infer(*a);
        return ty_error();
    }

    // Does the named type `t` have a struct field `name`? (Method-call syntax is field-first.)
    bool field_exists(const TyPtr& t, const std::string& name) const {
        if (!t || t->kind != TyKind::Named) return false;
        auto it = structs_.find(t->name);
        if (it == structs_.end()) return false;
        for (const auto& f : it->second.field_names) if (f == name) return true;
        return false;
    }

    // Resolve method-call syntax `recv.method(args)` to a trait method. Builds [recv]+args and
    // reuses `call_trait_method`, which infers the receiver (arg 0), checks the trait bound, and
    // solves the trait's type params from the receiver's impl.
    TyPtr method_call(const TyPtr& recvTy, FieldExpr& fld, const std::vector<Expr*>& args, Expr& node) {
        // An inherent method on the receiver's head takes precedence over a trait method (Rust
        // shadowing; the trait method stays reachable via `Trait::method(x)`). Only a method with
        // `self` is reachable this way -- an ASSOCIATED function has no receiver to dispatch on, so
        // it must NOT shadow a same-named trait method here (codegen's dot branch applies the same
        // filter, so the two agree by construction rather than by coincidence).
        if (recvTy && recvTy->kind == TyKind::Named) {
            auto hit = inherent_.find(recvTy->name);
            if (hit != inherent_.end()) {
                auto mit = hit->second.methods.find(fld.name);
                if (mit != hit->second.methods.end() && mit->second.has_self)
                    return call_inherent_method(recvTy->name, fld.name, recvTy, fld.obj.get(), args, node);
                // An associated function reached through `.`: name it, rather than letting the
                // generic "has no field or method" below claim it does not exist.
                if (mit != hit->second.methods.end() && !method_owners_.count(fld.name)) {
                    error(node.line, node.col, "'" + fld.name + "' is an associated function of '" +
                          short_name(recvTy->name) + "' (it takes no `self`); call it as " +
                          short_name(recvTy->name) + "::" + fld.name + "(...)");
                    for (Expr* a : args) infer(*a);
                    return ty_error();
                }
            }
        }
        auto oit = method_owners_.find(fld.name);
        if (oit == method_owners_.end() || oit->second.empty()) {
            error(node.line, node.col, "type " + describe(recvTy) + " has no field or method '" + fld.name + "'");
            for (Expr* a : args) infer(*a);
            return ty_error();
        }
        const std::string trait = pick_trait(fld.name, recvTy, node.line, node.col);
        if (trait.empty()) {
            for (Expr* a : args) infer(*a);
            return ty_error();
        }
        std::vector<Expr*> margs;
        margs.push_back(fld.obj.get());
        for (Expr* a : args) margs.push_back(a);
        fld.resolved_trait = trait;   // the routing decision; codegen + the oracle obey it
        return call_trait_method(trait, fld.name, margs, node, recvTy);
    }

    // The trait an unqualified trait-method call (`recv.m(..)`, `m(recv, ..)`, `recv |> m`) dispatches to,
    // or "" after reporting why there is none. Every trait of the whole program that declares `m` is a
    // candidate (method_owners_), so the name alone must not decide: a private trait in one module would
    // make a correct call in another ambiguous. The RECEIVER decides -- the traits its type may implement
    // (an impl for its head, a `dyn` trait and its supertraits, a type parameter's bounds, a blanket impl);
    // a tie is broken by visibility (a trait private to another module cannot be meant). The pick is
    // written back (IdentExpr / FieldExpr::resolved_trait) and obeyed by codegen and the RefEval oracle.
    //
    // A strict relaxation of the old rule "ambiguous as soon as two traits declare `m`": with one
    // declaring trait the pick is that trait, as before. A receiver whose type is not known yet is not
    // waited for (its trait fixes the parameter types the arguments are checked against), so without a
    // receiver the visible traits decide -- over-rejecting at worst, never picking a wrong trait.
    std::string pick_trait(const std::string& method, const TyPtr& recvIn, uint32_t line, uint32_t col) {
        const std::vector<std::string>& owners = method_owners_.at(method);
        if (owners.size() == 1) return owners[0];
        const TyPtr rs = recvIn ? apply(recvIn) : nullptr;
        if (rs && (rs->kind == TyKind::Error || rs->kind == TyKind::Never)) return owners[0];   // already reported
        auto visible = [&](const std::vector<std::string>& from) {
            std::vector<std::string> out;
            for (const auto& t : from) if (trait_visible_here(t)) out.push_back(t);
            return out;
        };
        std::vector<std::string> may;
        if (rs) for (const auto& t : owners) if (may_implement(rs, t)) may.push_back(t);
        if (may.size() == 1) return may[0];
        if (may.size() > 1) {
            const std::vector<std::string> vis = visible(may);
            if (vis.size() == 1) return vis[0];
            const std::vector<std::string>& named = vis.empty() ? may : vis;
            error(line, col, "ambiguous method '" + method + "': type " + describe(rs) + " implements " +
                  trait_list(named, " and ") + "; qualify it as " + qualified_forms(named, method));
            return {};
        }
        const std::vector<std::string> vis = visible(owners);
        if (vis.size() == 1) return vis[0];   // call_trait_method reports "does not implement", as before
        const std::vector<std::string>& named = vis.empty() ? owners : vis;
        if (!rs)
            error(line, col, "ambiguous method '" + method + "' (declared by " + trait_list(named, " and ") +
                  "); qualify it as " + qualified_forms(named, method));
        else if (rs->kind == TyKind::Var && !rs->rigid && rs->bounds.empty())
            error(line, col, "ambiguous method '" + method + "' (declared by " + trait_list(named, " and ") +
                  ") on a receiver whose type is not known here; qualify it as " +
                  qualified_forms(named, method));
        else
            error(line, col, "type " + describe(rs) + " has no method '" + method + "' (declared by " +
                  trait_list(named, " and ") + ", which it does not implement)");
        return {};
    }

    // Does every trait declaring `method` declare it with `self` (so the first argument is a receiver)?
    bool all_take_self(const std::string& method) const {
        for (const auto& t : method_owners_.at(method))
            for (const auto& ms : traits_.at(t).methods)
                if (ms.name == method && !ms.has_self) return false;
        return true;
    }

    // Could `rs` implement `trait`? For a named head, an impl of the trait for that head counts whatever
    // its own bounds say (call_trait_method checks those, and records what it cannot decide yet); else
    // satisfies_bound: a `dyn` trait and its supertraits, a type parameter's bounds, a blanket impl. A
    // pure probe: nothing is deferred from here.
    bool may_implement(const TyPtr& rs, const std::string& trait) {
        const std::string head = type_head(rs);
        if (!head.empty()) {
            const std::string key = trait + "\x1f" + head;
            if (impl_index_.count(key) || impl_keys_.count(key)) return true;
        }
        ObligationScope pure(impl_obligations_, nullptr);
        return satisfies_bound(rs, trait);
    }

    // May code in the current module mean the trait `t`? Its own module's, a bare / prelude one, or a
    // `pub` one -- a private trait of another module cannot be named there. (Not visible_across, which
    // counts the entry program's items as visible from everywhere.)
    bool trait_visible_here(const std::string& t) const {
        const std::string owner = module_prefix_of(t);
        return owner.empty() || owner == PRELUDE_MODULE_PREFIX || owner == cur_module_ || pub_items_.count(t) > 0;
    }

    // How to NAME the candidate traits in a diagnostic: the short name, or the mangled one where two
    // candidates share it (`util::Named` vs. `Named`), which `error` renders without the entry prefix.
    // Sorted -- the owner list follows an unordered map.
    static std::vector<std::string> trait_names(const std::vector<std::string>& traits) {
        std::vector<std::string> names;
        for (const auto& t : traits) {
            const std::string s = short_name(t);
            bool shared = false;
            for (const auto& o : traits) if (o != t && short_name(o) == s) { shared = true; break; }
            names.push_back(shared ? t : s);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    // "'A' and 'B'" / "'A', 'B' and 'C'".
    static std::string trait_list(const std::vector<std::string>& traits, const char* last_sep) {
        const std::vector<std::string> names = trait_names(traits);
        std::string s;
        for (size_t i = 0; i < names.size(); ++i)
            s += (i == 0 ? "" : i + 1 == names.size() ? last_sep : ", ") + ("'" + names[i] + "'");
        return s;
    }
    // "A::m(..) or B::m(..)" for the same list.
    static std::string qualified_forms(const std::vector<std::string>& traits, const std::string& method) {
        const std::vector<std::string> names = trait_names(traits);
        std::string s;
        for (size_t i = 0; i < names.size(); ++i)
            s += (i == 0 ? "" : i + 1 == names.size() ? " or " : ", ") + (names[i] + "::" + method + "(..)");
        return s;
    }

    // Instantiate a generic fn's scheme with fresh (bound-carrying) vars, check the
    // args, then discharge each generic's trait bounds against its solved type.
    TyPtr call_direct_fn(const FnSig& sig, const std::string& what, const std::vector<Expr*>& args, Expr& node,
                         TyPtr expected = nullptr) {
        std::unordered_map<uint32_t, TyPtr> m;
        for (const auto& g : sig.generics)
            m[g.id] = tc_.fresh_var(g.name, g.bounds);
        std::vector<TyPtr> params;
        for (const auto& p : sig.params) params.push_back(tc_.substitute(p, m));
        TyPtr ret = tc_.substitute(sig.ret, m);

        if (args.size() != params.size()) {
            error(node.line, node.col, "wrong number of arguments: expected " +
                  std::to_string(params.size()) + ", got " + std::to_string(args.size()));
            for (Expr* a : args) infer(*a);
            return apply(ret);
        }
        check_args_two_pass(args, params, &sig.origin);
        for (size_t i = 0; i < args.size() && i < sig.params_mut.size(); ++i)
            if (sig.params_mut[i]) require_mut_arg(args[i], "a `mut` parameter");
        // Pin a return-type-determined type parameter BEFORE discharging bounds. A nullary (or
        // otherwise return-only) bounded generic -- e.g. `mkSet[T: Hashable]()` assigned to a
        // `Set[P]` -- fixes T solely through the expected type; the pin solves it here, so the bound
        // is discharged at once instead of being deferred to `drain_pending_generics`. Best-effort: a
        // genuine return-type mismatch is left for the caller's `subsumes` to report (we ignore the
        // bool here). Only the check-mode Call path passes `expected`; infer-mode callers pass null.
        if (expected) tc_.subsumes(apply(ret), apply(expected));
        discharge_bounds(sig.generics, m, node.line, node.col, &what);
        return apply(ret);
    }

    // A module-qualified call `mod::name(args)`: `name` is a fn or a (tuple/variant) constructor
    // in the imported module `mod`. Resolves to the mangled `mod::name`, clears the qualifier +
    // writes the mangled name back, then reuses the ordinary ctor / direct-fn call path (so codegen
    // lowers it as a plain direct call). Only single-segment modules are reachable this way (the
    // parser accepts one `::`); a multi-segment module (`net::http`) is used via `use`.
    TyPtr call_module_member(IdentExpr& id, const std::vector<Expr*>& args, Expr& node) {
        const std::string mod = id.qualifier;
        // `std` is a VIRTUAL module = the ambient/prelude namespace (bare-named). `std::map`
        // resolves to the bare `map`, so a module that shadows a prelude name with a local one
        // can still reach the prelude version. It is always available (no import required) and its
        // members are NOT mangled (prelude items stay bare).
        const bool is_std = (mod == "std");
        if (!is_std && !imported_by_current(mod)) {
            error(node.line, node.col, "module '" + mod + "' is not imported");
            for (Expr* a : args) infer(*a);
            return ty_error();
        }
        const std::string key = is_std ? resolve_std(id.name) : (mod + "::" + id.name);
        if (!is_std && !visible_across(key)) {                        // default private
            error(node.line, node.col, "item '" + id.name + "' is private in module '" + mod + "'");
            for (Expr* a : args) infer(*a);
            return ty_error();
        }
        const bool is_variant = variants_.count(key) > 0;
        const bool is_tuple_struct = structs_.count(key) && structs_.at(key).is_tuple;
        if (is_variant || is_tuple_struct) {
            id.qualifier.clear(); id.name = key; id.upper = true;    // writeback for codegen
            return call_ctor(id, args, node);
        }
        if (auto it = fns_.find(key); it != fns_.end()) {
            id.qualifier.clear(); id.name = key; id.upper = false;   // writeback for codegen
            return call_direct_fn(it->second, short_name(key), args, node);
        }
        error(node.line, node.col, "module '" + mod + "' has no function or constructor '" + id.name + "'");
        for (Expr* a : args) infer(*a);
        return ty_error();
    }

    // A constructor CALL `V(args)` / `S(args)` (an enum variant or a tuple struct).
    // Mirrors `call_direct_fn`: instantiate the OWNING type's generics as fresh
    // bound-carrying vars, check the field arguments, then discharge the type's
    // generic bounds -- so an `enum E[T: Display]{ V(T) }` / `struct S[T: Display](T)`
    // bound is enforced at every construction, not just at a direct fn call. Returns
    // the built named type with its args solved.
    TyPtr call_ctor(IdentExpr& id, const std::vector<Expr*>& args, Expr& node) {
        const std::vector<GenericInfo>* gens = nullptr;
        const std::vector<TyPtr>* field_types = nullptr;
        std::string made_name;
        if (auto vit = variants_.find(id.name); vit != variants_.end()) {
            const VariantInfo& vi = vit->second;
            made_name   = vi.enum_name;
            field_types = &vi.field_types;
            if (auto eit = enums_.find(vi.enum_name); eit != enums_.end())
                gens = &eit->second.generics;
        } else {
            const StructInfo& si = structs_.at(id.name);   // is_tuple guaranteed by caller
            made_name   = id.name;
            field_types = &si.field_types;
            gens        = &si.generics;
        }
        static const std::vector<GenericInfo> kNoGens;
        const std::vector<GenericInfo>& G = gens ? *gens : kNoGens;

        std::unordered_map<uint32_t, TyPtr> m;
        std::vector<TyPtr> targs;
        for (const auto& g : G) { TyPtr fv = tc_.fresh_var(g.name, g.bounds); m[g.id] = fv; targs.push_back(fv); }

        std::vector<TyPtr> params;
        for (const auto& ft : *field_types) params.push_back(tc_.substitute(ft, m));

        if (args.size() != params.size()) {
            error(node.line, node.col, "wrong number of arguments: expected " +
                  std::to_string(params.size()) + ", got " + std::to_string(args.size()));
            for (Expr* a : args) infer(*a);
        } else {
            check_args_two_pass(args, params);
            const std::string what = short_name(made_name);
            discharge_bounds(G, m, node.line, node.col, &what);
        }
        std::vector<TyPtr> applied;
        for (const auto& t : targs) applied.push_back(apply(t));
        return make_named(made_name, std::move(applied));
    }

    // Check non-lambda arguments first so type variables get solved before a
    // dependent lambda body is checked (e.g. `map(xs, fn(x) { x + 1 })`).
    //
    // `origin` (when the callee has a source declaration) turns an argument mismatch into a DUAL
    // report: the primary caret on the offending argument, a secondary note on the parameter that
    // demanded the type. It is passed EXPLICITLY, never inherited from an enclosing context -- an
    // argument error must cite its parameter, not a surrounding `let` annotation. `check_expr`
    // already threads the note through block/if/match, so it survives a compound argument.
    void check_args_two_pass(const std::vector<Expr*>& args, const std::vector<TyPtr>& params,
                             const ParamOrigin* origin = nullptr) {
        // Builds the note for argument `i`, or nothing when the callee has no source parameters
        // (a builtin / native) or the counts do not line up (a poisoned signature).
        auto note = [&](size_t i) -> std::optional<DiagLabel> {
            if (!origin || !origin->params || i >= origin->params->size()) return std::nullopt;
            const Param& p = (*origin->params)[i];
            if (!p.type) return std::nullopt;
            return DiagLabel{ "expected because of parameter '" + p.name + "'",
                              p.type->line, p.type->col, origin->module };
        };
        auto check_one = [&](size_t i) {
            std::optional<DiagLabel> lbl = note(i);
            check_expr(*args[i], params[i], lbl ? &*lbl : nullptr);
        };
        for (size_t i = 0; i < args.size(); ++i)
            if (args[i]->kind != ExprKind::Lambda) check_one(i);
        for (size_t i = 0; i < args.size(); ++i)
            if (args[i]->kind == ExprKind::Lambda) check_one(i);
    }

    // Does `ty` satisfy trait bound `b`? A concrete type: an impl exists. A type
    // variable (rigid skolem OR still-unsolved flexible var): `b` is in the var's
    // declared bound closure. Poison/`Never`: vacuously true (already reported /
    // bottom). Note the deliberate soundness stance: an unsolved flexible var is NOT
    // leniently accepted -- it is discharged against its OWN bounds, so a truly
    // unconstrained/unbounded type cannot be PROVEN to satisfy `b` and is rejected.
    // This is safe for completeness because every bounded generic is instantiated as
    // a fresh var CARRYING its bounds (see `call_direct_fn` etc.), so an
    // unsolved-but-constrained var still discharges here; only a genuinely unbounded
    // unknown fails -- exactly the unsound case (the checker is the only guard once
    // types erase). A var's own bounds say nothing about the type it is solved to LATER,
    // which is why user-facing call sites never reach this with an unsolved argument:
    // `discharge_bounds` defers those to `drain_pending_generics`.
    bool satisfies_bound(TyPtr ty, const std::string& b) {
        ty = apply(ty);
        if (ty->kind == TyKind::Error || ty->kind == TyKind::Never) return true;
        // `Eq` is a sealed STRUCTURAL marker: it has no impls to look up -- membership is decided
        // by is_eq (all components Eq). This makes a written `T: Eq` bound discharge structurally
        // and lets any `satisfies_bound(ty, Eq)` query (e.g. a container's element bound) work.
        if (is_eq_trait(b)) return is_eq(ty);
        // A trait object satisfies its OWN trait and that trait's supertraits -- the exact
        // mirror of the Var branch below (a `dyn A` is "some T: A", so it knows precisely what
        // a `T: A` knows). It may also satisfy a blanket keyed on those, and soundly so: the
        // hidden type does satisfy the blanket's requirements, so the linker has filled that
        // type's column in the dispatch table.
        if (ty->kind == TyKind::Dyn) {
            if (ty->name == b) return true;
            if (supertrait_closure(ty->name).count(b)) return true;
            return satisfies_via_blanket(ty, b);
        }
        if (ty->kind == TyKind::Var) {
            for (const auto& bd : ty->bounds) {
                if (bd.trait == b) return true;
                if (supertrait_closure(bd.trait).count(b)) return true;
            }
            return satisfies_via_blanket(ty, b);   // a var may satisfy `b` via a blanket keyed on its other bounds
        }
        std::string head = type_head(ty);
        if (!head.empty()) {
            const std::string key = b + "\x1f" + head;
            // A concrete impl for the head DECIDES alone -- its own bounds included. No blanket fallback when
            // those bounds fail: the dispatch-table column for the head belongs to that impl at run time.
            if (auto iit = impl_index_.find(key); iit != impl_index_.end())
                return impl_bounds_hold(impls_[iit->second], ty);
            if (impl_keys_.count(key) > 0) return true;   // a synthesized marker key (no impl declaration)
        }
        return satisfies_via_blanket(ty, b);       // else a blanket `impl[T: reqs] b for T` whose reqs `ty` meets
    }

    // A blanket `impl[T: reqs] b for T` makes `ty` satisfy `b` iff `ty` meets every one of `reqs`.
    // The in-progress set breaks a bounds cycle across blankets (fail-to-prove is sound: over-reject).
    bool satisfies_via_blanket(TyPtr ty, const std::string& b) {
        auto it = blanket_impls_.find(b);
        if (it == blanket_impls_.end()) return false;
        if (!blanket_in_progress_.insert(b).second) return false;
        bool ok = true;
        for (const auto& req : it->second.bounds)
            if (!satisfies_bound(ty, req.trait)) { ok = false; break; }
        blanket_in_progress_.erase(b);
        return ok;
    }

    // Does the generic impl `im` apply to `ty` -- its target matches AND every bound on its own type parameters
    // holds for what they matched? `impl[T: Named] Named for Timed[T]` covers `Timed[Str]` but not `Timed[Int]`.
    // Pure: matching binds a local map, never the substitution, so a failed probe leaves nothing behind.
    //
    // A matched type parameter that is still an unsolved flexible var cannot be decided yet. At a user-facing
    // site `impl_obligations_` is set, and the bound is recorded there for `drain_pending_generics` to check
    // against the type fixed later; anywhere else (speculative `dyn` probes, internal discharges) the var is
    // judged by its own bounds, which rejects rather than accepts what it cannot prove.
    bool impl_bounds_hold(const ImplInfo& im, const TyPtr& ty) {
        if (im.generics.empty()) return true;
        if (bound_depth_ > 64) return false;                           // recursion safety net (sound over-reject)
        std::unordered_map<uint32_t, TyPtr> m;
        if (!match_impl_pattern(im, im.target, ty, m)) return false;
        struct Pending { const GenericInfo* g; const Bound* b; };
        std::vector<Pending> checks;
        // Pass 1: a parametric bound's arguments may bind further impl parameters (`[I: Iterable[E], E]`), so
        // solve those before any parameter's own bounds are judged.
        for (const auto& g : im.generics)
            for (const auto& b : g.bounds) {
                auto it = m.find(g.id);
                if (it == m.end()) continue;   // not in the target: fixed through the trait's arguments instead
                TyPtr sub = apply(it->second);
                if (sub->kind == TyKind::Var && !sub->rigid) { checks.push_back({ &g, &b }); continue; }
                if (!b.args.empty()) {
                    std::vector<TyPtr> args;
                    ++bound_depth_;
                    const bool found = pure_trait_args(b.trait, sub, args);
                    --bound_depth_;
                    if (!found || args.size() != b.args.size()) return false;
                    for (size_t i = 0; i < args.size(); ++i)
                        if (!match_impl_pattern(im, b.args[i], args[i], m)) return false;
                }
                checks.push_back({ &g, &b });
            }
        // Pass 2: every bound against what its parameter matched.
        for (const auto& c : checks) {
            TyPtr sub = apply(m[c.g->id]);
            if (sub->kind == TyKind::Var && !sub->rigid && impl_obligations_) {
                std::unordered_map<uint32_t, TyPtr> full = m;
                for (const auto& g : im.generics)
                    if (!full.count(g.id)) full[g.id] = tc_.fresh_var(g.name);
                Bound bi;
                bi.trait = c.b->trait;
                for (const auto& a : c.b->args) bi.args.push_back(tc_.substitute(a, full));
                impl_obligations_->push_back(ImplObligation{ sub, std::move(bi), c.g->name,
                                                             "impl " + im.trait_name + " for " + im.head });
                continue;
            }
            ++bound_depth_;
            const bool ok = satisfies_bound(sub, c.b->trait);
            --bound_depth_;
            if (!ok) return false;
        }
        return true;
    }

    // Structural match of `pat` -- an impl target or bound argument whose leaves may be `im`'s own type
    // parameters -- against `ty`, binding those parameters in `m`. A parameter met twice must match the same
    // shape both times. An unsolved flexible var in `ty` matches anything: it is decided where it gets fixed.
    bool match_impl_pattern(const ImplInfo& im, const TyPtr& pat, const TyPtr& tyIn,
                            std::unordered_map<uint32_t, TyPtr>& m) {
        TyPtr ty = apply(tyIn);
        if (pat->kind == TyKind::Var && pat->rigid) {
            for (const auto& g : im.generics)
                if (g.id == pat->var_id) {
                    auto it = m.find(g.id);
                    if (it == m.end()) { m.emplace(g.id, ty); return true; }
                    return shapes_agree(it->second, ty);
                }
        }
        if (ty->kind == TyKind::Var && !ty->rigid) return true;
        if (ty->kind == TyKind::Error || ty->kind == TyKind::Never) return true;
        if (pat->kind != ty->kind) return false;
        if (pat->kind == TyKind::Var) return pat->var_id == ty->var_id;
        if ((pat->kind == TyKind::Named || pat->kind == TyKind::Dyn) && pat->name != ty->name) return false;
        if (pat->args.size() != ty->args.size()) return false;
        for (size_t i = 0; i < pat->args.size(); ++i)
            if (!match_impl_pattern(im, pat->args[i], ty->args[i], m)) return false;
        if (pat->kind == TyKind::Fn && pat->ret && ty->ret) return match_impl_pattern(im, pat->ret, ty->ret, m);
        return true;
    }

    // Two types of the same shape, where an unsolved flexible var (or poison) agrees with anything.
    bool shapes_agree(const TyPtr& aIn, const TyPtr& bIn) {
        TyPtr a = apply(aIn), b = apply(bIn);
        if ((a->kind == TyKind::Var && !a->rigid) || (b->kind == TyKind::Var && !b->rigid)) return true;
        if (a->kind == TyKind::Error || b->kind == TyKind::Error) return true;
        if (a->kind != b->kind) return false;
        if (a->kind == TyKind::Var) return a->var_id == b->var_id;
        if ((a->kind == TyKind::Named || a->kind == TyKind::Dyn) && a->name != b->name) return false;
        if (a->args.size() != b->args.size()) return false;
        for (size_t i = 0; i < a->args.size(); ++i)
            if (!shapes_agree(a->args[i], b->args[i])) return false;
        if (a->kind == TyKind::Fn && a->ret && b->ret) return shapes_agree(a->ret, b->ret);
        return true;
    }

    // The trait arguments `sub` implements `trait` with, WITHOUT unifying anything: from a var's or trait
    // object's own bound, else from the matching impl. An impl parameter the target does not fix comes back as a
    // fresh var, which `match_impl_pattern` accepts.
    bool pure_trait_args(const std::string& trait, const TyPtr& subIn, std::vector<TyPtr>& out) {
        TyPtr sub = apply(subIn);
        if (sub->kind == TyKind::Var) {
            for (const auto& bd : sub->bounds)
                if (bd.trait == trait) { out = bd.args; return true; }
            return false;
        }
        if (sub->kind == TyKind::Dyn) {
            if (sub->name != trait) return false;
            out = sub->args;
            return true;
        }
        const std::string head = type_head(sub);
        if (head.empty()) return false;
        auto iit = impl_index_.find(trait + "\x1f" + head);
        if (iit == impl_index_.end()) return false;
        const ImplInfo& im = impls_[iit->second];
        std::unordered_map<uint32_t, TyPtr> m;
        if (!match_impl_pattern(im, im.target, sub, m)) return false;
        for (const auto& g : im.generics)
            if (!m.count(g.id)) m[g.id] = tc_.fresh_var(g.name);
        out.clear();
        for (const auto& a : im.trait_args) out.push_back(tc_.substitute(a, m));
        return true;
    }

    // Enforce `K: Hashable` at a Map CONSTRUCTION site (the only place a Map value is minted -- a
    // `#{...}` literal or a `Map[K,V]`-typed `#{}`). A concrete non-hashable key, or an abstract key
    // parameter lacking the `Hashable` bound, is rejected here -- so `Map[BadKey, V]` (and `Set[BadKey]`,
    // which builds a `Map[T, Bool]`) is a compile error instead of the runtime "Invalid map key type"
    // trap. Reads/writes on an already-built map need no check: you cannot obtain such a map without
    // constructing it, and the constructor is gated. `satisfies_bound` handles concrete types, a rigid
    // param carrying the bound, dyn, and blanket impls; an Error/Never key is let through (already reported).
    // The well-known zero-cost `Char` type: a transparent newtype over Int (the code-point) defined in
    // the std::string prelude. Recognized by a transparent struct named "Char" -- so it enables the
    // byte-literal-in-Char-context coercion, Char comparison, and glyph stringify, but is inert under
    // --no-prelude / a prelude that omits it. `Char` erases to the bare code-point Int at runtime.
    bool is_char_ty(const TyPtr& t) {
        if (!t) return false;
        TyPtr a = apply(t);
        if (a->kind != TyKind::Named || short_name(a->name) != "Char") return false;
        auto it = structs_.find(a->name);
        return it != structs_.end() && it->second.is_transparent;
    }

    // Is `t` the sealed built-in `Eq` marker trait? Matched by its EXACT canonical name
    // (`std::core::Eq`), NOT its short name -- so a user's own trait named `Eq` in another module,
    // or the example `trait Eq` some tests define under --no-prelude, is a distinct ordinary trait
    // and is unaffected by the seal / structural-== handling. Absent (--no-prelude) -> never matches.
    bool is_eq_trait(const std::string& t) const { return t == std_Eq(); }

    // STRUCTURAL equality-ability: may `==` / `!=` be applied to a value of type `t`? A type is
    // Eq iff every one of its components is Eq. Leaves = Int / Double / Bool / String / ()
    // (and the erasure types -- a transparent newtype over a primitive, an int-backed enum, Char --
    // fall out of the recursion for free: they bottom out at their Int/Double/Bool field or have no
    // payload). NOT Eq: a function/closure type (`Fn` -- not comparable; the EQ_DEEP opcode traps on
    // one, but the checker forbids it here so a well-typed program never reaches that trap), a `dyn`
    // (its concrete type is hidden -- conservative), and an unbounded type parameter.
    //
    // Cycle-safe: memoized on the nominal (struct/enum) head+args string, seeded provisionally to
    // `true` before recursing, so a self-recursive data type (List / Tree) assumes Eq on its own
    // back-edge -- the correct greatest fixpoint, and a concrete non-Eq field still forces false.
    // KNOWN LIMIT (documented, astronomically rare): a MUTUALLY-recursive pair of data types where
    // the non-Eq (function) field sits on the node reached via the back-edge can be mis-cached as
    // Eq; if it ever were, EQ_DEEP's closure operand still traps cleanly at run time -- never a
    // silent wrong answer. A precise fix (SCC / fixpoint iteration) is deferred.
    bool is_eq(TyPtr t) {
        t = apply(t);
        switch (t->kind) {
        case TyKind::Error: case TyKind::Never: return true;   // already reported / vacuous
        case TyKind::Unit:
        case TyKind::Int: case TyKind::Double: case TyKind::Bool:
        case TyKind::String: return true;                      // immediates + String (leaves)
        case TyKind::Fn:   return false;                       // functions / closures are not comparable
        case TyKind::Dyn:  return false;                       // hidden concrete type -> conservative
        case TyKind::Tuple:
            for (const auto& e : t->args) if (!is_eq(e)) return false;
            return true;
        case TyKind::Var:                                      // a type parameter: Eq iff it carries the bound
            for (const auto& bd : t->bounds) {
                if (is_eq_trait(bd.trait)) return true;
                for (const auto& s : supertrait_closure(bd.trait)) if (is_eq_trait(s)) return true;
            }
            return false;
        case TyKind::Named: return is_eq_named(t);
        default: return false;
        }
    }

    bool is_eq_named(const TyPtr& t) {
        // Built-in containers: Eq iff their element type(s) are Eq (Bytes is always Eq).
        if (t->name == "Bytes") return true;
        if ((t->name == "Array" || t->name == "Vec" || t->name == "List") && t->args.size() == 1)
            return is_eq(t->args[0]);
        if (t->name == "Map" && t->args.size() == 2)
            return is_eq(t->args[0]) && is_eq(t->args[1]);
        // A nominal struct / enum: recurse into its (arg-substituted) field / payload types.
        const std::string key = describe(t);
        if (auto it = eq_memo_.find(key); it != eq_memo_.end()) return it->second;
        eq_memo_[key] = true;                                  // provisional (break the recursion cycle)
        bool result = true;
        if (auto sit = structs_.find(t->name); sit != structs_.end()) {
            const StructInfo& si = sit->second;
            std::unordered_map<uint32_t, TyPtr> m;
            for (size_t i = 0; i < si.generics.size() && i < t->args.size(); ++i) m[si.generics[i].id] = t->args[i];
            for (const auto& ft : si.field_types)
                if (!is_eq(tc_.substitute(ft, m))) { result = false; break; }
        } else if (auto eit = enums_.find(t->name); eit != enums_.end()) {
            const EnumInfo& ei = eit->second;
            std::unordered_map<uint32_t, TyPtr> m;
            for (size_t i = 0; i < ei.generics.size() && i < t->args.size(); ++i) m[ei.generics[i].id] = t->args[i];
            for (const auto& vn : ei.variant_names) {
                auto vit = variants_.find(vn);
                if (vit == variants_.end() || vit->second.enum_name != t->name) continue;
                for (const auto& ft : vit->second.field_types)
                    if (!is_eq(tc_.substitute(ft, m))) { result = false; break; }
                if (!result) break;
            }
        } else {
            result = false;                                   // unknown opaque Named type -> conservative
        }
        eq_memo_[key] = result;
        return result;
    }

    void require_hashable_key(const TyPtr& key, uint32_t line, uint32_t col) {
        TyPtr k = apply(key);
        if (k->kind == TyKind::Error || k->kind == TyKind::Never) return;
        // The trait name is MANGLED (`std::core::Hashable`), so resolve "Hashable" through the current
        // module's scope exactly as a written `K: Hashable` bound is (mangle_ref, line ~1039) -- the impl
        // keys and rigid-var bounds all use that canonical form. If no `Hashable` trait is in scope (e.g.
        // `--no-prelude`, or a monolithic custom prelude that omits it), gating is OFF: Map falls back to
        // its historical permissive behavior (any key compiles, an unpermitted one traps at runtime).
        const std::string hashable = mangle_ref("Hashable");
        if (traits_.find(hashable) == traits_.end()) return;
        if (satisfies_bound(k, hashable)) return;
        error(line, col, "type " + describe(k) + " cannot be a map key: it does not satisfy the bound "
              "'Hashable' (only Int, Double, Bool, String -- or a type parameter bounded "
              "'K: Hashable' -- may be a map key)");
    }

    // If an operand is a generic type parameter, the real problem in an
    // arithmetic/bitwise/comparison error isn't a mismatch -- Skarn has no
    // numeric/Int trait, so a rigid `T` can never be used there. Append a one-line
    // explanation to disambiguate the otherwise-confusing "found T and T".
    static bool is_type_param(const TyPtr& t) { return t->kind == TyKind::Var && t->rigid; }
    std::string generic_hint(const TyPtr& l, const TyPtr& r) {
        if (is_type_param(l) || is_type_param(r))
            return "; a generic type parameter has no numeric capability -- "
                   "use a concrete Int or Double";
        return "";
    }
    std::string generic_hint(const TyPtr& t) { return generic_hint(t, t); }

    // A container where a `dyn Iterator` is expected: the lazy combinators (`map`, `filter`, ...) take an
    // Iterator, and a Vec / Array / Map / List / Bytes / String must be lifted first. The guide teaches
    // the lift, but the bare mismatch message did not mention it -- the one step every newcomer misses.
    std::string iterator_lift_hint(const TyPtr& actual, const TyPtr& expected) {
        const TyPtr a = apply(actual), x = apply(expected);
        if (!a || !x || x->kind != TyKind::Dyn || x->name != std_Iterator()) return "";
        const bool container = a->kind == TyKind::String ||
            (a->kind == TyKind::Named && (a->name == "Vec" || a->name == "Array" || a->name == "Map" ||
                                          a->name == "Bytes" || short_name(a->name) == "List"));
        if (!container) return "";
        return " -- a " + (a->kind == TyKind::String ? std::string("String") : short_name(a->name)) +
               " is not an Iterator; lift it with `intoIter` first (`xs |> intoIter |> map(...)`)";
    }

    std::string describe_bound(const Bound& b) {
        std::string s = b.trait;
        if (!b.args.empty()) {
            s += '[';
            for (size_t i = 0; i < b.args.size(); ++i) { if (i) s += ", "; s += describe(apply(b.args[i])); }
            s += ']';
        }
        return s;
    }

    // Find trait `trait`'s type ARGUMENTS as implemented for `selfTy`, writing them to
    // `out`. This is the output-inference heart of parametric traits:
    //   * `selfTy` a type variable carrying the bound -> read its args directly.
    //   * `selfTy` concrete -> match the single impl for (trait, head): instantiate the
    //     impl's own generics to fresh vars, unify its target with `selfTy` (solving them),
    //     discharge the impl's own generic bounds, and read the (now-solved) trait args.
    // The one-impl-per-(trait,head) coherence rule makes the trait args a FUNCTION of the
    // Self type (Rust's associated-type / functional-dependency discipline), so this is
    // deterministic. Returns false when nothing provides `trait` for `selfTy`.
    bool trait_args_for(const std::string& trait, TyPtr selfTy,
                        std::vector<TyPtr>& out, uint32_t line, uint32_t col) {
        selfTy = apply(selfTy);
        if (selfTy->kind == TyKind::Var) {
            for (const auto& bd : selfTy->bounds)
                if (bd.trait == trait) { out = bd.args; return true; }
            return false;   // a parametric supertrait via a var bound is not supported
        }
        // A trait object carries its trait args written out (there is no impl to solve them
        // from) -- read them straight off, mirroring the Var branch above.
        if (selfTy->kind == TyKind::Dyn) {
            if (selfTy->name == trait) { out = selfTy->args; return true; }
            return false;
        }
        const std::string head = type_head(selfTy);
        if (head.empty()) return false;
        auto iit = impl_index_.find(trait + "\x1f" + head);
        if (iit == impl_index_.end()) return false;
        const ImplInfo& im = impls_[iit->second];
        std::unordered_map<uint32_t, TyPtr> m2;
        for (const auto& g : im.generics) m2[g.id] = tc_.fresh_var(g.name, g.bounds);
        if (!tc_.unify(tc_.substitute(im.target, m2), selfTy)) return false;   // solve the impl generics
        discharge_bounds(im.generics, m2, line, col);                          // the impl's own bounds
        out.clear();
        for (const auto& a : im.trait_args) out.push_back(tc_.substitute(a, m2));
        return true;
    }

    // The IMPL ORACLE the solver asks to decide `sub <: dyn Trait[args]` (installed on `tc_` in
    // run()). Reuses the hardened `satisfies_bound` for "does an impl exist", then -- for a
    // parametric trait -- solves the trait's args for `sub` from its impl and requires them to
    // MATCH what the trait object names: an `impl Iterable[Int] for Nums` coerces to
    // `dyn Iterable[Int]` but not to `dyn Iterable[String]`.
    //
    // Speculative by contract (see `set_impl_oracle`): the QuietGuard drops any diagnostic raised
    // underneath, since a failure here just means "not coercible", which the caller reports in its
    // own words.
    bool dyn_coercible(const TyPtr& sub, const std::string& trait, const std::vector<TyPtr>& args) {
        QuietGuard q(quiet_depth_);
        ObligationScope noSink(impl_obligations_, nullptr);   // a probe: nothing it cannot prove is deferred
        if (!satisfies_bound(sub, trait)) return false;
        if (args.empty()) return true;
        std::vector<TyPtr> solved;
        if (!trait_args_for(trait, sub, solved, 0, 0)) return false;
        if (solved.size() != args.size()) return false;
        for (size_t i = 0; i < args.size(); ++i)
            if (!tc_.unify(args[i], solved[i])) return false;
        return true;
    }

    // Discharge one (possibly parametric) bound against a solved self type. A non-parametric
    // bound delegates to `satisfies_bound`. A parametric bound `Iterable[T]` finds the trait's
    // args for `selfTy` and UNIFIES them with the bound's args, thereby SOLVING the output
    // type parameter (`T`). Soundness gate: after unification every output arg must be
    // determined (not an unsolved flexible var) -- an undetermined erased parameter is rejected.
    bool discharge_parametric_bound(TyPtr selfTy, const Bound& b, uint32_t line, uint32_t col) {
        if (b.args.empty()) return satisfies_bound(selfTy, b.trait);   // non-parametric: unchanged
        if (bound_depth_ > 64) return false;                           // recursion safety net (sound over-reject)
        ++bound_depth_;
        std::vector<TyPtr> solved;
        bool ok = trait_args_for(b.trait, selfTy, solved, line, col);
        --bound_depth_;
        if (!ok || solved.size() != b.args.size()) return false;
        for (size_t i = 0; i < b.args.size(); ++i)
            if (!tc_.unify(b.args[i], solved[i])) return false;
        for (const auto& a : b.args) {
            TyPtr r = apply(a);
            if (r->kind == TyKind::Var && !r->rigid) {
                error(line, col, "cannot determine the type parameter(s) of bound '" +
                      describe_bound(b) + "' for " + describe(apply(selfTy)));
                return true;   // reported; suppress the generic "does not satisfy" message
            }
        }
        return true;
    }

    // Discharge each bounded generic's trait bounds against its solved type. `m` maps
    // each generic's rigid id to the fresh (bound-carrying) var it was instantiated to;
    // call AFTER the arguments have been checked (so the vars are solved). A parametric
    // bound's args are substituted through `m` so its OUTPUT vars are the fn's fresh vars,
    // solved here by impl-matching. The one reusable bound-checking site shared by every
    // call/constructor shape.
    // `what` non-null = a USER-FACING call or construction site (named by `what` in a diagnostic): a
    // bound whose type argument is still an unsolved flexible var is DEFERRED to
    // `drain_pending_generics` instead of being discharged vacuously against the var's own bounds.
    // Null = an internal discharge (the impl-bound step of `trait_args_for`, reached speculatively
    // from the `dyn` oracle too), which keeps the immediate behaviour.
    void discharge_bounds(const std::vector<GenericInfo>& gens,
                          const std::unordered_map<uint32_t, TyPtr>& m,
                          uint32_t line, uint32_t col, const std::string* what = nullptr) {
        for (const auto& g : gens) {
            if (g.bounds.empty()) continue;
            auto it = m.find(g.id);
            if (it == m.end()) continue;
            TyPtr solved = apply(it->second);
            for (const auto& b : g.bounds) {
                Bound bi;
                bi.trait = b.trait;
                for (const auto& a : b.args) bi.args.push_back(tc_.substitute(a, m));
                if (what && solved->kind == TyKind::Var && !solved->rigid) {
                    defer_generic(it->second, std::move(bi), g.name, *what, line, col);
                    continue;
                }
                const bool ok = with_impl_obligations(what, line, col,
                    [&] { return discharge_parametric_bound(solved, bi, line, col); });
                if (!ok)
                    error(line, col, "type " + describe(solved) +
                          " does not satisfy the bound '" + describe_bound(bi) + "'");
            }
        }
    }

    // Installs an impl-obligation sink for one scope and restores the previous one on exit.
    struct ObligationScope {
        std::vector<ImplObligation>*& slot;
        std::vector<ImplObligation>* saved;
        ObligationScope(std::vector<ImplObligation>*& s, std::vector<ImplObligation>* now) : slot(s), saved(s) { s = now; }
        ~ObligationScope() { slot = saved; }
    };

    // Runs `probe` -- a bound check at the site named by `what` -- collecting the impl bounds it could not decide
    // yet, and defers each to `drain_pending_generics` if the probe succeeded. No `what` (an internal discharge)
    // or a speculative region: nothing is collected, so an undecidable bound fails instead of being deferred.
    template <class Probe>
    bool with_impl_obligations(const std::string* what, uint32_t line, uint32_t col, Probe&& probe) {
        std::vector<ImplObligation> obs;
        bool ok;
        {
            ObligationScope scope(impl_obligations_, (what && quiet_depth_ == 0) ? &obs : nullptr);
            ok = probe();
        }
        if (ok)
            for (auto& o : obs) defer_generic(o.var, std::move(o.bound), o.generic, o.impl, line, col);
        return ok;
    }

    // Record a generic argument to re-examine after all bodies are checked. Not inside a speculative
    // region: its diagnostics are dropped there, so a record would outlive the probe that made it.
    void defer_generic(const TyPtr& var, Bound bound, const std::string& generic, const std::string& what,
                       uint32_t line, uint32_t col) {
        if (quiet_depth_ > 0) return;
        pending_generics_.push_back(PendingGeneric{ var, std::move(bound), generic, what, line, col, cur_module_ });
    }

    // Every type parameter of an associated-function call must end up solved -- by an argument, the
    // expected type, or any later use of the result. A bounded one is already recorded by
    // `discharge_bounds`; this adds the bare "must be solved" record for an unbounded one (with no
    // receiver and no turbofish, an unsolved one would leak an unnamed var into the result type).
    void require_solved_generics(const std::vector<GenericInfo>& gens,
                                 const std::unordered_map<uint32_t, TyPtr>& m,
                                 const std::string& what, uint32_t line, uint32_t col) {
        for (const auto& g : gens) {
            if (!g.bounds.empty()) continue;
            auto it = m.find(g.id);
            if (it == m.end()) continue;
            TyPtr solved = apply(it->second);
            if (solved->kind == TyKind::Var && !solved->rigid)
                defer_generic(it->second, Bound{}, g.name, what, line, col);
        }
    }

    // Re-examine the deferred generic arguments once every body is checked, i.e. once every use that
    // could fix them has been seen. Solved: discharge the bound for real, reported at the ORIGINAL call
    // site. Still unsolved: nothing in the program fixes the type, so it cannot be inferred -- unless
    // other errors were reported, in which case an unsolved var is most likely their fallout (the same
    // stance as the comparison check's `unsolved` guard).
    // One root cause is reported ONCE, at its earliest site: the var a call introduced flows on into
    // every later method call on the value (`let s = mkSet()` then `s.size()` re-instantiates the
    // impl's `T` and unifies it with the same var), and each of those sites recorded it too.
    void drain_pending_generics() {
        const bool had_errors = !errors_.empty();
        const std::string saved_module = cur_module_;
        std::vector<PendingGeneric> pending = std::move(pending_generics_);
        pending_generics_.clear();
        std::stable_sort(pending.begin(), pending.end(), [](const PendingGeneric& a, const PendingGeneric& b) {
            if (a.module != b.module) return a.module < b.module;
            return a.line != b.line ? a.line < b.line : a.col < b.col;
        });
        std::unordered_set<uint32_t> unsolved_reported;   // by the var's representative
        std::unordered_set<std::string> unsatisfied_reported;   // by (module, type, bound)
        for (auto& p : pending) {
            cur_module_ = p.module;
            TyPtr solved = apply(p.var);
            if (solved->kind == TyKind::Var && !solved->rigid) {
                if (!had_errors && unsolved_reported.insert(solved->var_id).second)
                    error(p.line, p.col, "cannot infer the type argument '" + p.generic + "' of '" +
                          p.what + "'; add a type annotation");
                continue;
            }
            if (p.bound.trait.empty()) continue;
            if (!discharge_parametric_bound(solved, p.bound, p.line, p.col)) {
                const std::string msg = "type " + describe(solved) +
                                        " does not satisfy the bound '" + describe_bound(p.bound) + "'";
                if (unsatisfied_reported.insert(p.module + '\x1f' + msg).second)
                    error(p.line, p.col, msg);
            }
        }
        cur_module_ = saved_module;
    }

    static std::string type_head(const TyPtr& t) {
        switch (t->kind) {
            case TyKind::Int:    return "Int";
            case TyKind::Double: return "Double";
            case TyKind::Bool:   return "Bool";
            case TyKind::String: return "String";
            case TyKind::Named:  return t->name;
            // Tuple / Fn / Unit / Var cannot have impls -- and neither can Dyn in v1
            // (`impl Foo for dyn Bar` is a v2 item; a trait object's impls are the hidden
            // type's, reached through the trait, not registered against the object itself).
            default:             return "";
        }
    }

    // Does the (mangled) trait `key` declare a method `m`? Its own methods only -- a supertrait's
    // method is reached through the supertrait's own name, exactly as an unqualified call does.
    bool trait_has_method(const std::string& key, const std::string& m) const {
        auto it = traits_.find(key);
        if (it == traits_.end()) return false;
        for (const auto& ms : it->second.methods) if (ms.name == m) return true;
        return false;
    }
    // Does the (mangled) TYPE `key` have an inherent method `m`?
    bool inherent_has_method(const std::string& key, const std::string& m) const {
        auto it = inherent_.find(key);
        return it != inherent_.end() && it->second.methods.count(m) > 0;
    }
    // Is that inherent member a METHOD (takes `self`) rather than an ASSOCIATED function? Callers
    // must have established `inherent_has_method` first; a missing entry answers false, which routes
    // to the associated path and reports there.
    bool inherent_member_has_self(const std::string& key, const std::string& m) const {
        auto it = inherent_.find(key);
        if (it == inherent_.end()) return false;
        auto mit = it->second.methods.find(m);
        return mit != it->second.methods.end() && mit->second.has_self;
    }

    // `Head::method(recv, ...)` where an UPPERCASE `Head` names a TYPE with an inherent method --
    // the qualified twin of `Trait::method(x)`, and the only way to name an inherent method when
    // the receiver is not syntactically first or when a same-named FIELD shadows it for `.`
    // (field-first resolution, see infer_call's Field branch, would otherwise make it unreachable).
    //
    // The receiver is arg 0, exactly as in the trait form; everything after it is a normal argument.
    // The actual work is `call_inherent_method` -- the SAME function the `.` path uses -- so generic
    // solving, `mut self` / `mut` params and bound discharge cannot drift between the two spellings.
    TyPtr call_qualified_inherent(IdentExpr& id, const std::string& head,
                                  const std::vector<Expr*>& args, Expr& node) {
        if (args.empty()) {
            error(node.line, node.col, "'" + short_name(head) + "::" + id.name +
                  "' needs a receiver argument: write " + short_name(head) + "::" + id.name + "(x)");
            return ty_error();
        }
        TyPtr recvTy = apply(infer(*args[0]));
        if (!recvTy || recvTy->kind != TyKind::Named || recvTy->name != head) {
            if (recvTy && recvTy->kind != TyKind::Error)
                error(node.line, node.col, "'" + short_name(head) + "::" + id.name +
                      "' expects a " + short_name(head) + " receiver, found " + describe(recvTy));
            for (size_t i = 1; i < args.size(); ++i) infer(*args[i]);
            return ty_error();
        }
        id.inherent = true;   // the routing decision; codegen + the RefEval oracle obey it
        std::vector<Expr*> rest(args.begin() + 1, args.end());
        return call_inherent_method(head, id.name, recvTy, args[0], rest, node);
    }

    // `Head::name(args)` where the inherent member takes NO `self` -- an ASSOCIATED FUNCTION
    // (`Point::new(1, 2)`, `Set::empty()`). The receiver-less sibling of `call_inherent_method`,
    // deliberately a separate function: that one OPENS by unifying the impl target with the
    // receiver, which is the entire generic-solving step and has no counterpart here. Instead the
    // impl's type params are solved from the arguments and, failing that, from the EXPECTED type.
    TyPtr call_associated_fn(IdentExpr& id, const std::string& head,
                             const std::vector<Expr*>& args, Expr& node, const TyPtr& expected) {
        const InherentInfo& ii = inherent_.at(head);
        const MethodSig& ms = ii.methods.at(id.name);
        std::unordered_map<uint32_t, TyPtr> m;
        for (const auto& g : ii.generics) m[g.id] = tc_.fresh_var(g.name, g.bounds);
        for (const auto& g : ms.generics) m[g.id] = tc_.fresh_var(g.name, g.bounds);
        std::vector<TyPtr> params;
        for (const auto& p : ms.params) params.push_back(tc_.substitute(p, m));
        TyPtr ret = tc_.substitute(ms.ret, m);
        if (args.size() != ms.params.size()) {
            error(node.line, node.col, "'" + short_name(head) + "::" + id.name + "' expects " +
                  std::to_string(ms.params.size()) + " argument(s), got " + std::to_string(args.size()));
            for (Expr* a : args) infer(*a);
            return apply(ret);
        }
        id.inherent = true;   // the routing decision; codegen + the RefEval oracle obey it
        check_args_two_pass(args, params, &ms.origin);
        for (size_t i = 0; i < args.size() && i < ms.params_mut.size(); ++i)
            if (ms.params_mut[i]) require_mut_arg(args[i], "a `mut` parameter");
        // Pin a return-type-determined type parameter BEFORE discharging bounds -- the same rule (and
        // the same one-liner) as `call_direct_fn`, and it matters MORE here: with no receiver, a head
        // param like `Set[T]`'s T has no other source at the call itself.
        if (expected) tc_.subsumes(apply(ret), apply(expected));
        // A type param nothing has fixed YET is not an error here: a later argument of an enclosing
        // call or a later use of the result may still fix it (`fold(xs, Set::new(), fn(acc: Set[Int], ..)`).
        // Its bound is deferred, and so is the requirement that it be solved at all (see
        // `drain_pending_generics`), which keeps a bounded head param from satisfying its own bound
        // vacuously. Scoped to this path: a METHOD's return-only generic is not required to be solved.
        const std::string what = short_name(head) + "::" + id.name;
        require_solved_generics(ii.generics, m, what, node.line, node.col);
        require_solved_generics(ms.generics, m, what, node.line, node.col);
        discharge_bounds(ii.generics, m, node.line, node.col, &what);
        discharge_bounds(ms.generics, m, node.line, node.col, &what);
        return apply(ret);
    }

    // A trait-method call `m(self, ...)` / `Trait::m(...)` / `x |> m`: instantiate the
    // method signature (Self + method generics fresh), dispatch on the receiver (arg 0),
    // discharge the trait bound, and check the remaining arguments.
    // `pre_self`: the receiver's type when the caller has ALREADY inferred `args[0]` (method syntax
    // `recv.m()`). It must then not be inferred again: inferring a `Type::fn(..)` call writes the resolved
    // head back (`Json` -> `std::json::Json`), and a second pass reads that lowercase mangled head as a
    // MODULE path ("module 'std::json::Json' is not imported"). Re-inference also doubled the work per
    // link of a method chain -- 2^n for `x.m().m()...`.
    TyPtr call_trait_method(const std::string& traitName, const std::string& methodName,
                            const std::vector<Expr*>& args, Expr& node, TyPtr pre_self = nullptr) {
        // Error recovery still types every argument, but never an already-inferred receiver.
        auto infer_args_on_error = [&] { for (size_t i = pre_self ? 1 : 0; i < args.size(); ++i) infer(*args[i]); };
        auto tit = traits_.find(traitName);
        if (tit == traits_.end()) {
            error(node.line, node.col, "unknown trait '" + traitName + "'" + missing_use_hint(traitName));
            infer_args_on_error();
            return ty_error();
        }
        const TraitInfo& ti = tit->second;
        const MethodSig* ms = nullptr;
        for (const auto& mm : ti.methods) if (mm.name == methodName) { ms = &mm; break; }
        if (!ms) {
            error(node.line, node.col, "trait '" + traitName + "' has no method '" + methodName + "'");
            infer_args_on_error();
            return ty_error();
        }
        std::unordered_map<uint32_t, TyPtr> m;
        TyPtr fresh_self = tc_.fresh_var();
        m[ti.self_id] = fresh_self;
        for (const auto& g : ti.generics) m[g.id] = tc_.fresh_var(g.name, g.bounds);   // trait type params (`Iterable[T]`)
        for (const auto& g : ms->generics) m[g.id] = tc_.fresh_var(g.name, g.bounds);  // carry bounds
        std::vector<TyPtr> params;
        for (const auto& p : ms->params) params.push_back(tc_.substitute(p, m));
        TyPtr ret = tc_.substitute(ms->ret, m);

        size_t want = (ms->has_self ? 1u : 0u) + params.size();
        if (args.size() != want) {
            error(node.line, node.col, "method '" + methodName + "' expects " + std::to_string(want) +
                  " argument(s), got " + std::to_string(args.size()));
            infer_args_on_error();
            return apply(ret);
        }
        size_t off = 0;
        if (ms->has_self) {
            TyPtr selfTy = pre_self ? apply(pre_self) : apply(infer(*args[0]));
            tc_.unify(fresh_self, selfTy);
            TyPtr rs = apply(fresh_self);
            if (!with_impl_obligations(&methodName, node.line, node.col,
                                       [&] { return satisfies_bound(rs, traitName); }))
                error(node.line, node.col, "type " + describe(rs) +
                      " does not implement trait '" + traitName + "'");
            // Solve the trait's own type params (`Iterable[T]`) from the receiver's impl, so
            // the method's param/return types (`-> Vec[T]`) resolve to the concrete element type.
            if (!ti.generics.empty()) {
                std::vector<TyPtr> targs;
                if (trait_args_for(traitName, rs, targs, node.line, node.col) &&
                    targs.size() == ti.generics.size())
                    for (size_t i = 0; i < ti.generics.size(); ++i)
                        tc_.unify(m[ti.generics[i].id], targs[i]);
            }
            if (ms->self_mut) require_mut_arg(args[0], "a `mut self` method");
            off = 1;
        }
        std::vector<Expr*> rest(args.begin() + off, args.end());
        // `rest` drops the receiver when the method takes `self`, and `ms->origin.params` is likewise
        // self-free (`Method::params`), so the two stay index-aligned.
        check_args_two_pass(rest, params, &ms->origin);
        for (size_t i = 0; i < rest.size() && i < ms->params_mut.size(); ++i)
            if (ms->params_mut[i]) require_mut_arg(rest[i], "a `mut` parameter");
        discharge_bounds(ms->generics, m, node.line, node.col, &methodName);   // the method's OWN generic bounds
        return apply(ret);
    }

    TyPtr infer_field(FieldExpr& e) {
        TyPtr o = apply(infer(*e.obj));
        if (o->kind == TyKind::Error) return ty_error();
        if (e.tuple_index) {
            if (o->kind == TyKind::Tuple) {
                if (e.index < o->args.size()) return o->args[e.index];
                error(e.line, e.col, "tuple index " + std::to_string(e.index) +
                      " out of range for " + describe(o));
                return ty_error();
            }
            // A transparent struct erases to its single field; `.0` reads that field's immediate.
            // (A regular named tuple struct has no `.0` projection -- it is pattern-only.)
            if (o->kind == TyKind::Named) {
                if (auto it = structs_.find(o->name); it != structs_.end() && it->second.is_transparent) {
                    if (e.index == 0 && !it->second.field_types.empty())
                        return it->second.field_types[0];   // no generics on a transparent struct
                    error(e.line, e.col, "tuple index " + std::to_string(e.index) +
                          " out of range for " + describe(o));
                    return ty_error();
                }
            }
            error(e.line, e.col, "tuple index on non-tuple type " + describe(o));
            return ty_error();
        }
        return field_type_of(o, e);
    }

    // The type of the named field `e.name` on an already-inferred receiver type `o` (never re-infers
    // `e.obj`, so the method-call path can reuse it with the receiver type it already holds).
    TyPtr field_type_of(const TyPtr& o, const FieldExpr& e) {
        if (o->kind == TyKind::Named) {
            if (auto it = structs_.find(o->name); it != structs_.end()) {
                const StructInfo& si = it->second;
                for (size_t i = 0; i < si.field_names.size(); ++i)
                    if (si.field_names[i] == e.name)
                        return subst_generics(si.field_types[i], si.generics, o->args);
                error(e.line, e.col, "no field '" + e.name + "' on struct '" + o->name + "'");
                return ty_error();
            }
        }
        error(e.line, e.col, "type " + describe(o) + " has no field '" + e.name + "'");
        return ty_error();
    }

    TyPtr infer_index(IndexExpr& e) {
        // The index base is an allowed read site for a const array (`K[i]`): authorize this exact
        // child so infer_ident does not reject it as an escape (a nested `foo(K)[i]` is NOT matched).
        const Expr* prev = const_array_ok_node_;
        const_array_ok_node_ = e.obj.get();
        TyPtr o = apply(infer(*e.obj));
        const_array_ok_node_ = prev;
        if (o->kind == TyKind::Error) { infer(*e.index); return ty_error(); }
        if (o->kind == TyKind::Named) {
            // Map: the index is the KEY (type K); `m[k]` reads the value (type V) and TRAPS
            // on a missing key (MAP_GET_OR_TRAP). The safe, total path is `get(m, k) -> Option[V]`.
            if (o->name == "Map" && o->args.size() == 2) {
                check_expr(*e.index, o->args[0]);
                return o->args[1];
            }
            if ((o->name == "Array" || o->name == "Vec") && o->args.size() == 1) {
                check_expr(*e.index, ty_int());
                return o->args[0];
            }
            if (o->name == "Bytes") {
                check_expr(*e.index, ty_int());
                return ty_int();
            }
        }
        check_expr(*e.index, ty_int());   // best-effort: still check the index expression
        const std::string hint = (o->kind == TyKind::String)
            ? " -- a String is not indexable; use `charAt(s, i)` for a byte, or `chars(s)` for code points"
            : "";
        error(e.line, e.col, "cannot index a value of type " + describe(o) + hint);
        return ty_error();
    }

    TyPtr infer_tuple(TupleExpr& e) {
        if (e.elems.empty()) return ty_unit();
        std::vector<TyPtr> es;
        for (auto& x : e.elems) es.push_back(infer(*x));
        return make_tuple(std::move(es));
    }

    // Fold one more element type into a running `join` for a MULTI-element construct (list literal,
    // map literal, match arms, `break` values). `acc` keeps the type the FIRST element established --
    // which is what the note caret points at -- and advances only while the elements agree.
    // `first_site` may be null (then no note caret). Returns whether the element was folded in.
    //
    // The `reported` latch is the reason this is one helper instead of four copies. A failed join
    // deliberately does NOT advance `acc`, so every LATER element of the offending type collides with
    // it again; without the latch the NUMBER of diagnostics would depend on element ORDER
    // (`[2.0, 1, 3]` reporting the identical message twice where `[1, 2.0, 3]` reports it once).
    //
    // Reporting is also what keeps the fold SOUND, which is why `infer_map` must come through here
    // too. `join` yields `Error` on a mismatch and `Error` subsumes everything (`Solver.cpp:181`), so
    // an unreported Error type silences every later check on that value: with a bare `join`,
    // `#{1 => 7, 2 => 8.0}` would infer `Map[Int, Error]` and accept `let s: String = m[1]`, putting
    // an Int into a `String` binding on the erased runtime. A REPORTED mismatch means nothing is lowered.
    bool join_element(TyPtr& acc, const TyPtr& t, bool& reported, const std::string& what,
                      const std::string& first_note, const Expr* first_site, const Expr& here) {
        TyPtr j = tc_.join(acc, t);
        if (apply(j)->kind == TyKind::Error &&
            apply(acc)->kind != TyKind::Error && apply(t)->kind != TyKind::Error) {
            if (reported) return false;                  // already said it for this construct
            reported = true;
            TypeRenderer r;
            std::vector<DiagLabel> labels;
            if (first_site)
                labels.push_back(DiagLabel{ first_note + r(apply(acc)),
                                            first_site->line, first_site->col, cur_module_ });
            error(here.line, here.col, what + " have incompatible types (" +
                  r(apply(acc)) + " and " + r(apply(t)) + ")", std::move(labels));
            return false;
        }
        acc = j;
        return true;
    }

    // An id for a map-literal key that is equal exactly when two keys are THE SAME KEY at run time.
    // `nullopt` for anything that is not a literal (or a negated numeric literal) -- such a key is a
    // runtime matter and gets no diagnostic.
    //
    // DO NOT reuse `literal_con` here, tempting as it looks. That one deliberately keeps `i1` and `d1`
    // DISTINCT, which is right for pattern exhaustiveness (a column has one type) and wrong here: a map
    // key canonicalizes, so after the `Int`->`Double` widening `1` and `1.0` are one key. `double_con`
    // IS reused, and its `-0.0 -> 0.0` fold is the VM's own key rule (SameValueZero).
    //
    // `Expr::widen_double` is what makes the `1` / `1.0` pair detectable at all: the checker records
    // there that it used the `Int <: Double` edge on this very key, so an `Int` literal must be
    // rendered as the DOUBLE it will actually become. Reading the flag off the KEY NODE is deliberate --
    // for `-1` the mark sits on the `UnaryExpr`, not the `IntLit` beneath it.
    //
    // The unary arm is deliberately narrower than `literal_con`'s, which prefixes "-" for ANY unary op:
    // that is harmless for a con id but would make `~1` and `-1` compare equal here, i.e. invent a
    // duplicate that does not exist. Only negation, only on a numeric literal. And it negates the
    // VALUE rather than prefixing the string, so `-0` collides with `0` and `-0.0` with `0.0` -- which
    // is what the run time does (SameValueZero), and a string prefix would have missed.
    static std::optional<std::string> map_key_id(const Expr& k) {
        const bool as_double = k.widen_double;
        switch (k.kind) {
            case ExprKind::BoolLit:
                return std::string(static_cast<const BoolLit&>(k).value ? "b1" : "b0");
            case ExprKind::IntLit: {
                const int64_t v = static_cast<const IntLit&>(k).value;
                return as_double ? double_con(static_cast<double>(v)) : "i" + std::to_string(v);
            }
            case ExprKind::DoubleLit:
                return double_con(static_cast<const DoubleLit&>(k).value);
            case ExprKind::StrLit:
                return "s:" + static_cast<const StrLit&>(k).value;
            case ExprKind::Unary: {
                const auto& u = static_cast<const UnaryExpr&>(k);
                if (u.op != TokKind::Minus || !u.operand) return std::nullopt;
                if (u.operand->kind == ExprKind::IntLit) {
                    const int64_t v = -static_cast<const IntLit&>(*u.operand).value;
                    return as_double ? double_con(static_cast<double>(v)) : "i" + std::to_string(v);
                }
                if (u.operand->kind == ExprKind::DoubleLit)
                    return double_con(-static_cast<const DoubleLit&>(*u.operand).value);
                return std::nullopt;
            }
            default: return std::nullopt;
        }
    }

    // How a key literal reads in the source, near enough for a diagnostic. Used ONLY to explain a
    // duplicate whose two halves are spelled differently (`1` and `1.0`) -- without that, the message
    // reads as a compiler bug to anyone who can see the two keys are not the same text.
    static std::string key_spelling(const Expr& k) {
        switch (k.kind) {
            case ExprKind::BoolLit: return static_cast<const BoolLit&>(k).value ? "true" : "false";
            case ExprKind::IntLit:  return std::to_string(static_cast<const IntLit&>(k).value);
            case ExprKind::StrLit:  return "\"" + static_cast<const StrLit&>(k).value + "\"";
            case ExprKind::DoubleLit: {
                char buf[40];
                auto [end, ec] = std::to_chars(buf, buf + sizeof buf,
                                               static_cast<const DoubleLit&>(k).value);
                if (ec != std::errc{}) return "?";
                std::string s(buf, end);
                // `to_chars` prints 1.0 as "1"; the `.0` is the entire point of the message here.
                if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
                    s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
                    s += ".0";
                return s;
            }
            case ExprKind::Unary: {
                const auto& u = static_cast<const UnaryExpr&>(k);
                return u.operand ? "-" + key_spelling(*u.operand) : "?";
            }
            default: return "?";
        }
    }

    // Warn where a map literal names one key twice. The RUNTIME behaviour is unchanged and stays
    // last-wins -- the same as Python and JS -- so this is advisory only (`--strict` escalates); making
    // it an error would break anyone relying on that deliberately.
    //
    // Called from BOTH Map-construction choke points (`infer_map` + `check_map_expected`), the sibling
    // of `require_hashable_key`. It must run AFTER those functions have checked their entries: in the
    // check-position one it is `check_expr` on the key that records the `Int`->`Double` widening, and a
    // key id computed before that would silently miss every widened pair.
    void warn_duplicate_map_keys(const MapLit& e) {
        std::unordered_map<std::string, const Expr*> seen;
        for (const auto& entry : e.entries) {
            if (!entry.first) continue;
            const Expr& k = *entry.first;
            const std::optional<std::string> id = map_key_id(k);
            if (!id) continue;
            const auto it = seen.find(*id);
            if (it == seen.end()) { seen.emplace(*id, &k); continue; }   // keep the FIRST occurrence
            const std::string here = key_spelling(k), before = key_spelling(*it->second);
            std::string msg = "duplicate key in map literal -- this entry replaces the earlier one";
            if (here != before)   // `1` and `1.0`: say WHY they are one key, or this reads as a bug
                msg = "duplicate key in map literal: " + here + " and " + before +
                      " are the same key once the Int is widened to Double -- this entry replaces"
                      " the earlier one";
            std::vector<DiagLabel> labels;
            labels.push_back(DiagLabel{ "the earlier entry with this key",
                                        it->second->line, it->second->col, cur_module_ });
            warn(k.line, k.col, std::move(msg), std::move(labels));
        }
    }

    TyPtr infer_list(ListLit& e) {
        if (e.elems.empty()) {
            error(e.line, e.col, "cannot infer the element type of an empty list; add a type annotation");
            return make_named("List", { ty_error() });
        }
        TyPtr elem = infer(*e.elems[0]);
        bool reported = false;
        for (size_t i = 1; i < e.elems.size(); ++i) {
            TyPtr t = infer(*e.elems[i]);
            join_element(elem, t, reported, "list elements", "this element has type ",
                         e.elems[0].get(), *e.elems[i]);
        }
        return make_named("List", { apply(elem) });
    }

    TyPtr infer_map(MapLit& e) {
        if (e.entries.empty()) {
            error(e.line, e.col, "cannot infer the type of an empty map; add a type annotation");
            return make_named("Map", { ty_error(), ty_error() });
        }
        TyPtr k = infer(*e.entries[0].first);
        TyPtr v = infer(*e.entries[0].second);
        bool kReported = false, vReported = false;
        for (size_t i = 1; i < e.entries.size(); ++i) {
            join_element(k, infer(*e.entries[i].first), kReported, "map keys", "this key has type ",
                         e.entries[0].first.get(), *e.entries[i].first);
            join_element(v, infer(*e.entries[i].second), vReported, "map values", "this value has type ",
                         e.entries[0].second.get(), *e.entries[i].second);
        }
        // `k` is the FIRST key's type even on a mismatch, never `Error` -- so the key gate below
        // asks about a real type instead of being silently satisfied by `Error`.
        require_hashable_key(k, e.line, e.col);
        warn_duplicate_map_keys(e);
        return make_named("Map", { apply(k), apply(v) });
    }

    // ----- lambdas + check-position container literals ------------

    // A lambda with no expected type: every parameter must be annotated.
    TyPtr infer_lambda(LambdaExpr& l) {
        const size_t savedFloor = lambda_floor_;
        const bool   savedLetB  = lambda_is_let_bound_;
        lambda_floor_ = scopes_.size();             // the scope push_scope() is about to create
        lambda_is_let_bound_ |= (let_bound_lambda_ == &l);   // |= : a nested lambda INHERITS it
        push_scope();
        TyPtr savedRet = cur_return_;
        const bool savedTop = at_top_level_;
        at_top_level_ = false;                      // a lambda body is a function body
        auto savedLoops = std::move(loop_frames_); loop_frames_.clear();   // a lambda is a loop boundary
        std::vector<TyPtr> ptys;
        for (const auto& p : l.params) {
            TyPtr pt = p.type ? resolve_type(*p.type, cur_generic_env_) : ty_error();
            if (!p.type)
                error(l.line, l.col, "cannot infer the type of lambda parameter '" + p.name +
                      "'; add a type annotation or use it where a function type is expected");
            ptys.push_back(pt);
            define(p.name, pt, false);
        }
        TyPtr rt;
        if (l.ret) { rt = resolve_type(*l.ret, cur_generic_env_); cur_return_ = rt; check_body_against_ret(*l.body, rt); }
        else       { cur_return_ = ty_error(); rt = infer(*l.body); }
        cur_return_ = savedRet; at_top_level_ = savedTop; loop_frames_ = std::move(savedLoops);
        lambda_floor_ = savedFloor; lambda_is_let_bound_ = savedLetB;
        pop_scope();
        TyPtr t = make_fn(std::move(ptys), rt);
        l.ty = t;
        return t;
    }

    // A lambda checked against an expected `fn(...) -> R`: params take their types
    // from the expectation (or the annotation, checked consistent), the body is
    // checked against R.
    void check_lambda(LambdaExpr& l, TyPtr expected) {
        if (expected->kind != TyKind::Fn || expected->args.size() != l.params.size()) {
            TyPtr t = infer_lambda(l);
            if (!subsumes(t, expected)) {
                TypeRenderer r;
                error(l.line, l.col, "type mismatch: expected " + r(expected) +
                      ", found " + r(apply(t)));
            }
            return;
        }
        // The floor is installed HERE, not at the top of the function: the delegating branch above
        // RETURNS without reaching the restore, so a top-of-function install would leak the floor into
        // the caller -- after which every ordinary local of the enclosing function resolves "below the
        // floor" and `check_assign` rejects it, naming lambdas in lambda-free code. `infer_lambda`
        // installs and restores its own floor, so the delegation is correct by construction.
        const size_t savedFloor = lambda_floor_;
        const bool   savedLetB  = lambda_is_let_bound_;
        lambda_floor_ = scopes_.size();             // the scope push_scope() is about to create
        lambda_is_let_bound_ |= (let_bound_lambda_ == &l);   // |= : a nested lambda INHERITS it
        push_scope();
        TyPtr savedRet = cur_return_;
        const bool savedTop = at_top_level_;
        at_top_level_ = false;                      // a lambda body is a function body
        auto savedLoops = std::move(loop_frames_); loop_frames_.clear();   // a lambda is a loop boundary
        std::vector<TyPtr> ptys;
        for (size_t i = 0; i < l.params.size(); ++i) {
            TyPtr pt;
            if (l.params[i].type) {
                pt = resolve_type(*l.params[i].type, cur_generic_env_);
                if (!subsumes(expected->args[i], pt)) {
                    TypeRenderer r;
                    error(l.line, l.col, "lambda parameter '" + l.params[i].name +
                          "' is annotated " + r(apply(pt)) + " but " +
                          r(apply(expected->args[i])) + " is expected");
                }
            } else pt = expected->args[i];
            ptys.push_back(pt);
            define(l.params[i].name, apply(pt), false);
        }
        TyPtr rt = l.ret ? resolve_type(*l.ret, cur_generic_env_) : expected->ret;
        // A lambda's return is COVARIANT: its actual return `rt` must be `<: expected->ret`. With an
        // explicit `-> R` annotation, unify `rt` against the expected return slot -- WITHOUT this the
        // annotated return was never reconciled with the context, which was (a) INCOMPLETE (a fresh
        // output var stayed unsolved: `map(.., fn(x:Int)->Int{..})` left `U` free in `map[T,U]`'s
        // `dyn Iterator[U]` result) and (b) UNSOUND (a concrete mismatch slipped through -- a
        // `fn(Int)->Int` accepted where `fn(Int)->String` was required). No annotation => `rt` already
        // IS `expected->ret`, so this is a no-op. The parameter side is already checked above.
        if (l.ret && expected->ret && !subsumes(rt, expected->ret)) {
            TypeRenderer r;
            error(l.line, l.col, "lambda returns " + r(apply(rt)) + " but " +
                  r(apply(expected->ret)) + " is expected");
        }
        cur_return_ = rt;
        check_body_against_ret(*l.body, rt);
        cur_return_ = savedRet; at_top_level_ = savedTop; loop_frames_ = std::move(savedLoops);
        lambda_floor_ = savedFloor; lambda_is_let_bound_ = savedLetB;
        pop_scope();
        l.ty = make_fn(std::move(ptys), apply(rt));
    }

    // Empty / element-directed list & map literals when an expected type is known.
    bool check_list_expected(ListLit& e, const TyPtr& expected) {
        TyPtr exp = apply(expected);
        if (exp->kind == TyKind::Named && exp->name == "List" && exp->args.size() == 1) {
            for (auto& x : e.elems) check_expr(*x, exp->args[0]);
            e.ty = make_named("List", { apply(exp->args[0]) });
            return true;
        }
        return false;
    }

    bool check_map_expected(MapLit& e, const TyPtr& expected) {
        TyPtr exp = apply(expected);
        if (exp->kind == TyKind::Named && exp->name == "Map" && exp->args.size() == 2) {
            for (auto& entry : e.entries) {
                check_expr(*entry.first, exp->args[0]);
                check_expr(*entry.second, exp->args[1]);
            }
            require_hashable_key(exp->args[0], e.line, e.col);
            // AFTER the loop above, and that is load-bearing: `check_expr` on each key is what records
            // the `Int`->`Double` widening, so scanning first finds no widened duplicate at all. Proved
            // by moving this line up once -- it turns `dupkey_widened*` red and nothing else.
            warn_duplicate_map_keys(e);
            e.ty = make_named("Map", { apply(exp->args[0]), apply(exp->args[1]) });
            return true;
        }
        return false;
    }

    // Check-mode block / if / match: push the expected type into the tail
    // expression(s), giving precise per-branch errors and letting the expected
    // type reach an empty literal or a lambda in tail position.
    void check_block(BlockExpr& e, const TyPtr& expected, const DiagLabel* origin = nullptr) {
        push_scope();
        if (e.stmts.empty()) {
            if (!subsumes(ty_unit(), expected))
                error(e.line, e.col, "type mismatch: expected " + describe(apply(expected)) + ", found ()");
            e.ty = apply(expected);
            pop_scope();
            return;
        }
        for (size_t i = 0; i + 1 < e.stmts.size(); ++i)
            check_discard_stmt(*e.stmts[i], /*tail=*/false);   // a non-final statement's value is dropped
        Stmt& last = *e.stmts.back();
        if (last.kind == StmtKind::Expr) {
            // The tail IS the block's value -> check it STRICTLY against the expected type. (Discard
            // is driven by the CALLER being a discard position -- check_fn_body for a `-> ()` body,
            // check_discard for a statement/loop -- NOT by `expected` happening to be unit, so a
            // genuine `let x: () = <non-unit>` stays an error. That is the NARROW guarantee.)
            Expr& le = *static_cast<ExprStmt&>(last).expr;
            check_expr(le, expected, origin);
            e.ty = le.ty ? le.ty : apply(expected);
        } else {
            check_statement(last);
            if (!subsumes(ty_unit(), expected))
                error(e.line, e.col, "type mismatch: expected " + describe(apply(expected)) + ", found ()");
            e.ty = ty_unit();
        }
        pop_scope();
    }

    void check_if(IfExpr& e, const TyPtr& expected, const DiagLabel* origin = nullptr) {
        check_expr(*e.cond, ty_bool());
        check_expr(*e.then_blk, expected, origin);
        if (e.else_blk) check_expr(*e.else_blk, expected, origin);
        else if (!subsumes(ty_unit(), expected))
            error(e.line, e.col, "'if' without 'else' has type () but " +
                  describe(apply(expected)) + " is expected");
        e.ty = apply(expected);
    }

    void check_match(MatchExpr& e, const TyPtr& expected, const DiagLabel* origin = nullptr) {
        TyPtr scrut = apply(infer(*e.scrut));
        size_t pattern_errors = 0;
        for (auto& arm : e.arms) {
            push_scope();
            const size_t before = errors_.size();
            check_pattern(*arm.pat, scrut);
            pattern_errors += errors_.size() - before;
            if (arm.guard) check_expr(*arm.guard, ty_bool());
            check_expr(*arm.body, expected, origin);
            pop_scope();
        }
        check_match_coverage_if_typed(e, scrut, pattern_errors);
        e.ty = apply(expected);
    }

    // A record-style enum-variant literal `V { f: v }` (brace syntax). Mirrors infer_struct_lit's
    // field-checking but resolves the variant in variants_ and yields the ENUM type. Tuple/nullary
    // variants (built as `V(..)` / `V`) and `..base` on a variant are rejected here.
    TyPtr infer_record_variant_lit(StructLit& e, const VariantInfo& vi) {
        if (vi.is_tuple) {
            error(e.line, e.col, "variant '" + e.name + "' is not a record variant"
                  + (vi.field_types.empty() ? " (use '" + e.name + "')" : " (use '" + e.name + "(...)')"));
            for (auto& fi : e.fields) if (fi.value) infer(*fi.value);
            return ty_error();
        }
        if (e.base) {
            error(e.line, e.col, "record update `..base` is not supported on enum variant '" + e.name + "'");
            infer(*e.base);
        }
        auto eit = enums_.find(vi.enum_name);
        std::unordered_map<uint32_t, TyPtr> m;
        std::vector<TyPtr> targs;
        if (eit != enums_.end())
            for (const auto& g : eit->second.generics) { TyPtr fv = tc_.fresh_var(g.name, g.bounds); m[g.id] = fv; targs.push_back(fv); }

        std::unordered_map<std::string, size_t> field_index;
        for (size_t i = 0; i < vi.field_names.size(); ++i) field_index[vi.field_names[i]] = i;

        std::unordered_set<std::string> seen;
        for (auto& fi : e.fields) {
            auto fit = field_index.find(fi.name);
            if (fit == field_index.end()) {
                error(e.line, e.col, "variant '" + e.name + "' has no field '" + fi.name + "'");
                if (fi.value) infer(*fi.value);
                continue;
            }
            if (!seen.insert(fi.name).second)
                error(e.line, e.col, "duplicate field '" + fi.name + "' in variant literal");
            TyPtr expected = tc_.substitute(vi.field_types[fit->second], m);
            if (fi.value) {
                check_expr(*fi.value, expected);
            } else {   // shorthand `{ x }` == `{ x: x }`
                ScopeVar* v = lookup(fi.name);
                if (!v) {
                    error(e.line, e.col, "unknown variable '" + fi.name + "'");
                } else {
                    v->used = true;
                    // A shorthand field is a READ of the local, so it captures like any other.
                    // `FieldInit` carries no position, so the note carets at the literal.
                    note_capture(fi.name, e.line, e.col);
                    if (!subsumes(v->ty, expected)) {
                        TypeRenderer r;
                        error(e.line, e.col, "field '" + fi.name + "': expected " +
                              r(apply(expected)) + ", found " + r(apply(v->ty)));
                    }
                }
            }
        }
        if (!e.base)
            for (const auto& fn : vi.field_names)
                if (!seen.count(fn))
                    error(e.line, e.col, "missing field '" + fn + "' in literal for variant '" + e.name + "'");

        if (eit != enums_.end()) {
            const std::string what = short_name(vi.enum_name);
            discharge_bounds(eit->second.generics, m, e.line, e.col, &what);
        }
        std::vector<TyPtr> applied;
        for (auto& a : targs) applied.push_back(apply(a));
        return make_named(vi.enum_name, std::move(applied));
    }

    TyPtr infer_struct_lit(StructLit& e) {
        e.name = resolve_qualified_ref(e.qualifier, e.name, e.line, e.col);   // module-qualify (+ writeback)
        // A record-variant literal `V { f: v }` resolves in variants_, not structs_.
        if (!structs_.count(e.name))
            if (auto vit = variants_.find(e.name); vit != variants_.end())
                return infer_record_variant_lit(e, vit->second);
        auto it = structs_.find(e.name);
        if (it == structs_.end()) {
            error(e.line, e.col, enums_.count(e.name)
                  ? "'" + e.name + "' is an enum, not a struct"
                  : "unknown struct '" + e.name + "'");
            for (auto& fi : e.fields) if (fi.value) infer(*fi.value);
            return ty_error();
        }
        const StructInfo& si = it->second;
        std::unordered_map<uint32_t, TyPtr> m;
        std::vector<TyPtr> targs;
        for (const auto& g : si.generics) { TyPtr fv = tc_.fresh_var(g.name, g.bounds); m[g.id] = fv; targs.push_back(fv); }

        std::unordered_map<std::string, size_t> field_index;
        for (size_t i = 0; i < si.field_names.size(); ++i) field_index[si.field_names[i]] = i;

        std::unordered_set<std::string> seen;
        for (auto& fi : e.fields) {
            auto fit = field_index.find(fi.name);
            if (fit == field_index.end()) {
                error(e.line, e.col, "struct '" + e.name + "' has no field '" + fi.name + "'");
                if (fi.value) infer(*fi.value);
                continue;
            }
            if (!seen.insert(fi.name).second)
                error(e.line, e.col, "duplicate field '" + fi.name + "' in struct literal");
            TyPtr expected = tc_.substitute(si.field_types[fit->second], m);
            if (fi.value) {
                check_expr(*fi.value, expected);
            } else {   // shorthand `{ x }` == `{ x: x }`  -- reads `x`, so it counts as a use
                ScopeVar* v = lookup(fi.name);
                if (!v) {
                    error(e.line, e.col, "unknown variable '" + fi.name + "'");
                } else {
                    v->used = true;
                    // A shorthand field is a READ of the local, so it captures like any other.
                    // `FieldInit` carries no position, so the note carets at the literal.
                    note_capture(fi.name, e.line, e.col);
                    if (!subsumes(v->ty, expected)) {
                        TypeRenderer r;
                        error(e.line, e.col, "field '" + fi.name + "': expected " +
                              r(apply(expected)) + ", found " + r(apply(v->ty)));
                    }
                }
            }
        }
        if (e.base) {
            // Record update `Name { …, ..base }`: the base supplies every unlisted field, so there is no
            // missing-field error. It must be the SAME struct type -- checking it against the literal's type
            // (whose args are the fresh vars `targs`) both enforces that and SOLVES any generic arg the
            // explicit fields did not pin (e.g. `Box { ..b }` gets its `T` from `b`).
            check_expr(*e.base, make_named(e.name, targs));
        } else {
            for (const auto& fn : si.field_names)
                if (!seen.count(fn))
                    error(e.line, e.col, "missing field '" + fn + "' in literal for struct '" + e.name + "'");
        }

        const std::string what = short_name(e.name);
        discharge_bounds(si.generics, m, e.line, e.col, &what);   // enforce `struct S[T: Display]` at the literal
        std::vector<TyPtr> applied;
        for (auto& a : targs) applied.push_back(apply(a));
        return make_named(e.name, std::move(applied));
    }

    TyPtr infer_block(BlockExpr& e) {
        push_scope();
        TyPtr val = ty_unit();
        for (size_t i = 0; i < e.stmts.size(); ++i) {
            if (i + 1 == e.stmts.size())               // the tail: its value IS the block's value
                val = check_statement(*e.stmts[i]);
            else                                       // a non-final statement's value is discarded
                check_discard_stmt(*e.stmts[i], /*tail=*/false);   // (no branch-join; must-use warning)
        }
        pop_scope();
        return val;
    }

    TyPtr infer_if(IfExpr& e) {
        check_expr(*e.cond, ty_bool());
        TyPtr thenT = infer(*e.then_blk);
        if (e.else_blk) {
            TyPtr elseT = infer(*e.else_blk);
            // Deliberately NOT routed through `join_element`: an `if` joins exactly TWO branches, so
            // there is no later element to collide with a stale accumulator and nothing to latch.
            TyPtr j = tc_.join(thenT, elseT);
            if (apply(j)->kind == TyKind::Error &&
                apply(thenT)->kind != TyKind::Error && apply(elseT)->kind != TyKind::Error) {
                TypeRenderer r;
                DiagLabel thenLbl{ "the 'then' branch has type " + r(apply(thenT)),
                                   e.then_blk->line, e.then_blk->col, cur_module_ };
                error(e.line, e.col, "'if' branches have incompatible types (" +
                      r(apply(thenT)) + " and " + r(apply(elseT)) + ")", { std::move(thenLbl) });
            }
            return apply(j);
        }
        return ty_unit();   // no else -> statement-like, yields unit
    }

    TyPtr infer_while(WhileExpr& e) {
        check_expr(*e.cond, ty_bool());
        loop_frames_.push_back({ /*is_loop=*/false, false, nullptr, nullptr });
        check_discard(*e.body, /*tail=*/false);   // a loop body's value is dropped (must-use warning; no join)
        loop_frames_.pop_back();
        return ty_unit();
    }

    // `break [value]` -- diverges (type `Never`), but a VALUE contributes to the nearest
    // enclosing loop's result: it is the sole way a `loop` produces one. A value is rejected
    // in a `while`/`for` (their condition-false / iterator-empty exit yields unit, which could
    // not be reconciled with a break value -- Rust's rule, and what keeps `loop` sound).
    TyPtr infer_break(BreakExpr& e) {
        if (loop_frames_.empty()) {
            error(e.line, e.col, "'break' outside of a loop");
            if (e.value) infer(*e.value);
            return ty_never();
        }
        // The frame is addressed by INDEX, never by a reference held across the value's check: a value
        // that contains a loop (`break loop { break 2 }`) pushes onto `loop_frames_`, and a reallocation
        // would leave a reference dangling -- the outer join would go to freed memory, the outer loop would
        // stay `Never`, and `Never` subsumes every type (`let y: Int = <a String loop>` would be accepted).
        const size_t fi = loop_frames_.size() - 1;
        const LoopFrame& fr = loop_frames_[fi];
        if (e.value) {
            if (!fr.is_loop) {
                error(e.line, e.col, "a 'break' with a value is only allowed inside a 'loop' "
                                     "(a 'while'/'for' loop always exits with ())");
                infer(*e.value);
            } else if (fr.has_expected) {
                const TyPtr expected = fr.expected;
                check_expr(*e.value, expected);
            } else {
                const TyPtr vt = apply(infer(*e.value));
                join_break(loop_frames_[fi], vt, *e.value);
            }
        } else if (fr.is_loop) {                              // a valueless break contributes ()
            if (fr.has_expected) {
                if (!subsumes(ty_unit(), fr.expected)) {
                    TypeRenderer r;
                    error(e.line, e.col, "this 'break' must carry a value: the enclosing 'loop' "
                                         "is expected to yield " + r(apply(fr.expected)));
                }
            } else {
                join_break(loop_frames_[fi], ty_unit(), e);
            }
        }
        return ty_never();
    }

    // Merge one break's value type into the enclosing loop's accumulated result. `join` is exact and
    // reports nothing itself (it yields Error on an incompatible pair), so the incompatibility is
    // reported by `join_element` -- shared with the list/map/match folds -- anchored at the offending
    // break with a note at the first one that fixed the type. The latch lives in the LoopFrame
    // because that is this construct's "one report per construct" scope.
    void join_break(LoopFrame& fr, TyPtr vt, const Expr& site) {
        if (join_element(fr.break_join, vt, fr.break_reported, "'break' values",
                         "an earlier 'break' has type ", fr.first_break, site) && !fr.first_break)
            fr.first_break = &site;   // only a break that actually FIXED the type anchors the note
    }

    // `loop { body }` in INFER position: the type is the JOIN of every `break` value
    // (`Never` when no `break` is reachable -- an infinite loop diverges, like panic/return).
    TyPtr infer_loop(LoopExpr& e) {
        loop_frames_.push_back({ /*is_loop=*/true, /*has_expected=*/false, nullptr, ty_never() });
        if (e.body) check_discard(*e.body, /*tail=*/false);   // the body's own value is dropped
        TyPtr t = apply(loop_frames_.back().break_join);
        loop_frames_.pop_back();
        e.ty = t;
        return t;
    }

    // `loop { body }` in CHECK position: push `expected` into every `break` value instead of
    // joining. A loop with NO break is fine at any expected type (it diverges: Never <: T).
    void check_loop(LoopExpr& e, TyPtr expected) {
        loop_frames_.push_back({ /*is_loop=*/true, /*has_expected=*/true, apply(expected), ty_never() });
        if (e.body) check_discard(*e.body, /*tail=*/false);
        loop_frames_.pop_back();
        e.ty = apply(expected);
    }

    // `for pat in iter { body }` -- external iteration, yields (). The loop variable pattern is
    // irrefutable (ident / `_` / tuple / record struct / tuple-struct, nested) and binds the element type.
    TyPtr infer_for(ForExpr& e) {
        // The iterable is an allowed read site for a const array (`for x in K`): authorize this child.
        const Expr* prev = const_array_ok_node_;
        const_array_ok_node_ = e.iter.get();
        TyPtr it = apply(infer(*e.iter));
        const_array_ok_node_ = prev;
        TyPtr elem = for_elem_type(it, e);
        push_scope();
        bind_pattern(*e.pat, elem, false, "for");
        loop_frames_.push_back({ /*is_loop=*/false, false, nullptr, nullptr });
        check_discard(*e.body, /*tail=*/false);   // a loop body's value is dropped (must-use warning; no join)
        loop_frames_.pop_back();
        pop_scope();
        return ty_unit();
    }

    // Element type produced by iterating `it`: List/Array/Vec[E] -> E; Bytes -> Int;
    // Map[K,V] -> the (K, V) pair. Anything else is not iterable.
    TyPtr for_elem_type(TyPtr it, ForExpr& e) {
        it = apply(it);
        if (it->kind == TyKind::Error) return ty_error();
        if (it->kind == TyKind::Named) {
            if ((it->name == "List" || it->name == "Array" || it->name == "Vec") && it->args.size() == 1)
                return it->args[0];
            if (it->name == "Bytes") return ty_int();
            if (it->name == "Map" && it->args.size() == 2)
                return make_tuple({ it->args[0], it->args[1] });
        }
        // Lazy iteration protocol. Try `Iterator[E]` first (the value IS a pull cursor -- drive it in
        // place), then `IntoIterator[E]` (the value PRODUCES a fresh cursor via `intoIter`). Either way
        // the loop yields the element type E. A `dyn Iterator[E]` / a generic `I: Iterator[E]` receiver
        // both resolve here (trait_args_for reads the args off the Dyn / the bound var).
        if (traits_.count(std_Iterator())) {
            std::vector<TyPtr> targs;
            if (trait_args_for(std_Iterator(), it, targs, e.iter->line, e.iter->col) && targs.size() == 1)
                return targs[0];
        }
        if (traits_.count(std_IntoIterator())) {
            std::vector<TyPtr> targs;
            if (trait_args_for(std_IntoIterator(), it, targs, e.iter->line, e.iter->col) && targs.size() == 1)
                return targs[0];
        }
        // Iterable protocol (eager, legacy): a generic `I: Iterable[T]` receiver or a user type with an
        // `impl Iterable[E]` iterates, yielding its element type E (`for x in iter(it)`).
        if (traits_.count(std_Iterable())) {
            std::vector<TyPtr> targs;
            if (trait_args_for(std_Iterable(), it, targs, e.iter->line, e.iter->col) && targs.size() == 1)
                return targs[0];
        }
        error(e.iter->line, e.iter->col, "cannot iterate over a value of type " + describe(it));
        return ty_error();
    }

    TyPtr infer_return(ReturnExpr& e) {
        if (at_top_level_) {
            error(e.line, e.col, "'return' outside of a function");
            if (e.value) infer(*e.value);
            return ty_never();
        }
        TyPtr ret = cur_return_ ? cur_return_ : ty_unit();
        if (e.value) check_expr(*e.value, ret);
        else if (!subsumes(ty_unit(), ret))
            error(e.line, e.col, "return without a value, but the function returns " + describe(apply(ret)));
        return ty_never();
    }

    // `e?` -- unwrap Ok/Some, else early-return Err/None. The operand must be a
    // Result[T,E] / Option[T]; the enclosing function must return a compatible
    // Result[_,E] / Option[_]; the result is the T payload.
    TyPtr infer_try(TryExpr& e) {
        TyPtr o = apply(infer(*e.operand));
        TyPtr ret = apply(cur_return_ ? cur_return_ : ty_unit());
        if (o->kind == TyKind::Error) return ty_error();
        if (o->kind == TyKind::Named && short_name(o->name) == "Result" && o->args.size() == 2) {
            if (ret->kind == TyKind::Named && short_name(ret->name) == "Result" && ret->args.size() == 2) {
                if (!subsumes(o->args[1], ret->args[1])) {
                    TypeRenderer r;
                    error(e.line, e.col, "the '?' error type " + r(apply(o->args[1])) +
                          " is not compatible with the function's error type " + r(apply(ret->args[1])));
                }
            } else {
                error(e.line, e.col, "'?' on a Result requires the enclosing function to return a Result");
            }
            return o->args[0];
        }
        if (o->kind == TyKind::Named && short_name(o->name) == "Option" && o->args.size() == 1) {
            if (!(ret->kind == TyKind::Named && short_name(ret->name) == "Option"))
                error(e.line, e.col, "'?' on an Option requires the enclosing function to return an Option");
            return o->args[0];
        }
        error(e.line, e.col, "'?' can only be applied to a Result or Option, found " + describe(o));
        return ty_error();
    }

    // ===== match + patterns =======================================

    TyPtr infer_match(MatchExpr& e) {
        TyPtr scrut = apply(infer(*e.scrut));
        TyPtr result = ty_never();
        bool first = true, reported = false;
        size_t pattern_errors = 0;
        for (auto& arm : e.arms) {
            push_scope();
            const size_t before = errors_.size();
            check_pattern(*arm.pat, scrut);
            pattern_errors += errors_.size() - before;
            if (arm.guard) check_expr(*arm.guard, ty_bool());
            TyPtr bt = infer(*arm.body);
            pop_scope();
            if (first) { result = bt; first = false; }
            else
                join_element(result, bt, reported, "match arms", "an earlier arm has type ",
                             e.arms[0].body.get(), *arm.body);
        }
        check_match_coverage_if_typed(e, scrut, pattern_errors);
        return apply(result);
    }

    // Ensure `scrut` is the named type `name[arity]`; return its (applied) type args,
    // unifying fresh args in when the scrutinee is still an unbound var.
    std::vector<TyPtr> named_args_for(TyPtr scrut, const std::string& name, size_t arity, const Pattern& p) {
        scrut = apply(scrut);
        if (scrut->kind == TyKind::Named && scrut->name == name && scrut->args.size() == arity)
            return scrut->args;
        std::vector<TyPtr> fresh = fresh_args(arity);
        if (!tc_.unify(scrut, make_named(name, fresh)) && scrut->kind != TyKind::Error)
            error(p.line, p.col, "pattern of type '" + name + "' does not match scrutinee of type " + describe(scrut));
        std::vector<TyPtr> out;
        for (auto& a : fresh) out.push_back(apply(a));
        return out;
    }

    // What ONE or-pattern alternative bound: read back out of the arm scope after checking it.
    // `line`/`col` are the binding SITE (from ScopeVar), which is what identifies a binding -- see
    // check_or_pattern for why a TyPtr cannot.
    struct AltBinding {
        std::string name;
        TyPtr       ty;
        bool        is_mut = false;
        uint32_t    line = 0, col = 0;
    };

    // Check one alternative and report the bindings IT introduced, leaving the scope as it found it.
    //
    // There is no facility to check a pattern without defining its names, and none can be bolted on:
    // `check_pattern` is NOT idempotent on the AST (check_ctor_pattern / check_struct_pattern call
    // resolve_qualified_ref, which CLEARS `qualifier` in place, so a second pass re-mangles an
    // already-mangled name; the Literal case writes `lit->ty`). So the alternative is checked exactly
    // once, for real, and the scope map is snapshotted around it.
    //
    // Snapshot-and-diff, NOT snapshot-clear-restore: check_map_pattern evaluates the map KEY
    // expression, which may reference locals, so an emptied scope would break `#{ k => v }`.
    std::vector<AltBinding> check_alt_bindings(Pattern& alt, TyPtr scrut) {
        const auto before = scopes_.back();
        check_pattern(alt, scrut);

        std::vector<AltBinding> out;
        for (const auto& kv : scopes_.back()) {
            const auto it = before.find(kv.first);
            // "Bound by THIS alternative" = absent before, or bound at a different SITE. Keying on
            // the site rather than on the type is what makes `Pair(x, Ok(x) | Err(x))` work: `x` is
            // already in the arm scope, and `define` mints a fresh TyPtr on every call, so pointer
            // identity would report a spurious rebind for every such name.
            if (it == before.end() || it->second.line != kv.second.line || it->second.col != kv.second.col)
                out.push_back({ kv.first, kv.second.ty, kv.second.is_mut, kv.second.line, kv.second.col });
        }
        // `scopes_` is an unordered_map, so sort for a deterministic diagnostic order.
        std::sort(out.begin(), out.end(),
                  [](const AltBinding& a, const AltBinding& b) { return a.name < b.name; });

        scopes_.back() = before;
        return out;
    }

    static const AltBinding* find_binding(const std::vector<AltBinding>& v, const std::string& n) {
        for (const auto& b : v) if (b.name == n) return &b;
        return nullptr;
    }

    // `A | B | C` -- every alternative must bind the SAME names, at the same types, with the same
    // mutability (Rust's rule). Checked here rather than by a syntactic "does it bind anything"
    // predicate, because the interesting failures are about AGREEMENT, not about binding at all.
    void check_or_pattern(OrPat& op, TyPtr scrut) {
        if (op.alts.empty()) return;

        std::vector<std::vector<AltBinding>> per_alt;
        per_alt.reserve(op.alts.size());
        for (auto& alt : op.alts) per_alt.push_back(check_alt_bindings(*alt, scrut));

        const std::vector<AltBinding>& ref = per_alt[0];
        for (size_t i = 1; i < per_alt.size(); ++i) {
            for (const auto& b : ref)
                if (!find_binding(per_alt[i], b.name))
                    error(op.alts[i]->line, op.alts[i]->col,
                          "this or-pattern alternative does not bind '" + b.name + "'",
                          { DiagLabel{ "'" + b.name + "' is bound here", b.line, b.col, cur_module_ } });

            for (const auto& b : per_alt[i])
                if (!find_binding(ref, b.name))
                    error(b.line, b.col,
                          "'" + b.name + "' is not bound by every or-pattern alternative",
                          { DiagLabel{ "this alternative does not bind it",
                                       op.alts[0]->line, op.alts[0]->col, cur_module_ } });

            for (const auto& b : ref) {
                const AltBinding* o = find_binding(per_alt[i], b.name);
                if (!o) continue;
                // unify FIRST, then compare exactly. The alternatives share solver state, so on a
                // still-unresolved scrutinee `Ok(x)` yields `x : ?a` and `Err(x)` yields `x : ?b` --
                // two distinct free variables that exact equality rejects, failing a program the body
                // would have resolved. unify merges exactly that case and nothing else: it will not
                // merge Int with Double either, so the no-subtyping rule stands.
                tc_.unify(b.ty, o->ty);
                const TyPtr a = apply(b.ty), c = apply(o->ty);
                if (!equals(a, c) && a->kind != TyKind::Error && c->kind != TyKind::Error) {
                    TypeRenderer r;
                    error(o->line, o->col,
                          "'" + b.name + "' has type " + r(c) + " here but " + r(a) +
                          " in another or-pattern alternative",
                          { DiagLabel{ "bound as " + r(a) + " here", b.line, b.col, cur_module_ } });
                }
                if (b.is_mut != o->is_mut)
                    error(o->line, o->col,
                          std::string("'") + b.name + "' is " + (o->is_mut ? "'mut'" : "immutable") +
                          " here but " + (b.is_mut ? "'mut'" : "immutable") +
                          " in another or-pattern alternative",
                          { DiagLabel{ std::string("bound ") + (b.is_mut ? "'mut'" : "immutable") +
                                       " here", b.line, b.col, cur_module_ } });
            }
        }

        // Install the UNION of the names, each at the first NON-ERROR type recorded for it. Not
        // alternative 0's set: `Ok(_) | Err(x) => x + 1` would then report the disagreement AND a
        // follow-on "unknown identifier 'x'" plus whatever the body does with an error type. The
        // house style is the opposite -- check_ctor_pattern keeps checking surplus elements against
        // ty_error() for exactly this reason.
        std::vector<AltBinding> merged;
        for (const auto& av : per_alt)
            for (const auto& b : av) {
                AltBinding* m = nullptr;
                for (auto& e : merged) if (e.name == b.name) { m = &e; break; }
                if (!m) { merged.push_back(b); continue; }
                if (apply(m->ty)->kind == TyKind::Error && apply(b.ty)->kind != TyKind::Error)
                    m->ty = b.ty;
            }
        for (const auto& b : merged)
            define(b.name, apply(b.ty), b.is_mut, /*track=*/true, b.line, b.col);
    }

    // The literal EXPRESSION a range-pattern bound denotes, following one level of `const`
    // indirection; nullptr for anything else. `const` returns `const` on purpose: the initializer
    // node is SHARED by every use site (Codegen recompiles the same node per reference) and its
    // `ty` slot is filled at that const's own item position, which the linear body walk may reach
    // AFTER the function reading it -- so it may be inspected, never written through.
    // Non-recursive in practice: the parser restricts a const initializer to a literal, so the
    // Ident branch always lands on one.
    const Expr* bound_literal(const Expr& e) const {
        switch (e.kind) {
            case ExprKind::IntLit: case ExprKind::DoubleLit:
            case ExprKind::BoolLit: case ExprKind::StrLit:
                return &e;
            case ExprKind::Unary: {                       // a signed number literal: `-1` / `-1.5`
                const auto& u = static_cast<const UnaryExpr&>(e);
                return (u.op == TokKind::Minus && u.operand && bound_literal(*u.operand)) ? &e : nullptr;
            }
            case ExprKind::Ident: {                       // a scalar `const` (name already mangled)
                auto it = const_values_.find(static_cast<const IdentExpr&>(e).name);
                return (it == const_values_.end() || !it->second) ? nullptr : bound_literal(*it->second);
            }
            default: return nullptr;
        }
    }

    // Gate a range-pattern bound BEFORE inferring it. The parser admits only a literal leaf or a
    // NAME here, so this decides what a name may be. Three things the order buys:
    //
    // (a) A LOCAL wins, and is then rejected. Lowercase const names are legal, so `let lo = 5`
    //     genuinely shadows `const lo`; resolving straight to the const would test against a value
    //     the reader cannot see -- a wrong result with no diagnostic. Expression scoping already
    //     says local > fn > const, and a pattern must not disagree with it.
    // (b) resolve_qualified_ref CLEARS the qualifier, so the name must be written back in the same
    //     breath. Measured, not assumed: dropping the writeback does NOT break codegen -- the
    //     `infer` below runs `infer_ident`, which resolves and writes back on its own, and that is
    //     what Codegen and the RefEval oracle read (both key their const tables on the mangled
    //     name). What it DOES break is (i) `bound_literal` here and in `lower_pat`, which look the
    //     name up directly, and (ii) a QUALIFIED bound, whose cleared qualifier would leave
    //     `infer_ident` resolving a bare `LO` instead of `util::LO`. So the gate resolves once and
    //     leaves ONE canonical name behind rather than letting two passes re-derive it.
    // (c) Gating BEFORE infer keeps it to one diagnostic per mistake -- an Array-typed const would
    //     otherwise also trip the const-array escape error in `const_ref_type`.
    bool check_range_bound(Expr* e) {
        if (!e) return false;
        if (e->kind != ExprKind::Ident) return true;      // a literal leaf; the parser admits no other
        auto& id = static_cast<IdentExpr&>(*e);
        const std::string written = id.name;              // what the reader typed -- for the messages
        if (id.qualifier.empty() && !id.upper)
            if (ScopeVar* v = lookup(id.name)) {
                v->used = true;                           // a genuine reference: no "unused" pile-on
                error(id.line, id.col, "a range-pattern bound must be a literal or a constant; '" +
                      written + "' is a local binding");
                return false;
            }
        const std::string key = resolve_qualified_ref(id.qualifier, id.name, id.line, id.col);
        id.name = key;                                    // (b) -- one canonical resolved name
        if (!const_values_.count(key)) {
            const bool known = fns_.count(key) || variants_.count(key) || structs_.count(key);
            error(id.line, id.col, known
                  ? ("a range-pattern bound must name a constant; '" + written + "' is not a constant")
                  : ("unknown constant '" + written + "' in a range-pattern bound" +
                     missing_use_hint(written)));
            return false;
        }
        if (!bound_literal(*e)) {                         // an Array const: no scalar value to test
            error(id.line, id.col, "constant '" + written + "' is not a numeric constant "
                  "(a range-pattern bound must be Int or Double)");
            return false;
        }
        return true;
    }

    void check_pattern(Pattern& p, TyPtr scrut) {
        switch (p.kind) {
            case PatKind::Wildcard:
                return;
            case PatKind::Ident: {
                auto& ip = static_cast<IdentPat&>(p);
                define(ip.name, apply(scrut), ip.is_mut, /*track=*/true, p.line, p.col);
                return;
            }
            case PatKind::Bind: {
                // `n @ sub`: `n` takes the type of THIS position (not of `sub`), and `sub` is checked
                // against that same type. Bind first, so a diagnostic inside `sub` is reported after
                // the name is already in scope -- the order a reader expects from left to right.
                auto& bp = static_cast<BindPat&>(p);
                define(bp.name, apply(scrut), bp.is_mut, /*track=*/true, p.line, p.col);
                if (bp.sub) check_pattern(*bp.sub, scrut);
                return;
            }
            case PatKind::Literal: {
                auto& lp = static_cast<LiteralPat&>(p);
                TyPtr lt = lp.lit ? infer(*lp.lit) : ty_error();
                TyPtr s = apply(scrut);
                // A byte int-literal pattern (e.g. 'A') matches a Char scrutinee: Char erases to the
                // code-point Int, so codegen's ordinary literal test (BNE_INT) works on the bare Int.
                // Retype the literal to Char (literals never complete a signature -- still needs `_`).
                if (is_char_ty(s) && lp.lit && lp.lit->kind == ExprKind::IntLit) {
                    int64_t v = static_cast<IntLit&>(*lp.lit).value;
                    if (v >= 0 && v <= 255) { lp.lit->ty = s; return; }
                }
                if (!subsumes(lt, s) && s->kind != TyKind::Error && apply(lt)->kind != TyKind::Error)
                    error(p.line, p.col, "literal pattern of type " + describe(apply(lt)) +
                          " does not match " + describe(s));
                return;
            }
            case PatKind::Range: {
                // `lo..hi` / `lo..<hi` -- each bound is a numeric literal or a scalar `const` NAME
                // (see check_range_bound). Both must be the same numeric kind (Int or Double, char
                // literals being Int) and must match the scrutinee. Binds nothing. Exclusivity is a
                // matching detail only, so it does not affect this check.
                auto& rp = static_cast<RangePat&>(p);
                // Gate both bounds first -- see check_range_bound for why order matters. Both are
                // gated even when the first fails, so a program with two bad bounds reports two.
                const bool lo_ok = check_range_bound(rp.lo.get());
                const bool hi_ok = check_range_bound(rp.hi.get());
                if (!lo_ok || !hi_ok) return;
                TyPtr lo = rp.lo ? apply(infer(*rp.lo)) : ty_error();
                TyPtr hi = rp.hi ? apply(infer(*rp.hi)) : ty_error();
                TyPtr s  = apply(scrut);
                // A byte int-literal RANGE matches a Char scrutinee -- the exact mirror of the
                // literal-pattern rule above, and the reason `'a'..'z'` is writable at all. Char
                // erases to the code-point Int, so codegen's BLT_NUM/BGT_NUM already compare bare
                // Ints and the oracle's `as_double` agrees; no VM or codegen change is involved.
                // Keyed on the VALUE range, not on syntax: the lexer turns `'a'` into an ordinary
                // Int token (Lexer.cpp), so `'a'..'z'` and `97..122` are indistinguishable here --
                // and, being the same set, they correctly share one Maranget con id.
                //
                // LITERAL bounds only. The rule retypes the node, and a NAMED bound's value node is
                // a const initializer SHARED by every use site, so writing its `ty` would leak into
                // unrelated uses -- and would read a `ty` slot the linear body walk may not have
                // filled yet. `const A: Char = 'a'` as a bound is therefore still a type error.
                if (is_char_ty(s)) {
                    auto byte_lit = [](Expr* e) -> IntLit* {
                        if (!e || e->kind != ExprKind::IntLit) return nullptr;
                        auto* n = static_cast<IntLit*>(e);
                        return (n->value >= 0 && n->value <= 255) ? n : nullptr;
                    };
                    IntLit* l = byte_lit(rp.lo.get());
                    IntLit* h = byte_lit(rp.hi.get());
                    if (l && h) { l->ty = s; h->ty = s; return; }
                }
                auto numeric = [](const TyPtr& t) { return t->kind == TyKind::Int || t->kind == TyKind::Double; };
                if (lo->kind != TyKind::Error && !numeric(lo))
                    error(rp.line, rp.col, "range pattern bounds must be Int or Double, found " + describe(lo));
                else if (hi->kind != TyKind::Error && !numeric(hi))
                    error(rp.line, rp.col, "range pattern bounds must be Int or Double, found " + describe(hi));
                else {
                    if (!subsumes(lo, s) && s->kind != TyKind::Error && lo->kind != TyKind::Error)
                        error(rp.line, rp.col, "range pattern of type " + describe(lo) + " does not match " + describe(s));
                    if (!subsumes(hi, s) && s->kind != TyKind::Error && hi->kind != TyKind::Error)
                        error(rp.line, rp.col, "range pattern of type " + describe(hi) + " does not match " + describe(s));
                }
                return;
            }
            case PatKind::Tuple: {
                auto& tp = static_cast<TuplePat&>(p);
                TyPtr s = apply(scrut);
                if (tp.elems.empty() && s->kind == TyKind::Unit) return;
                if (s->kind == TyKind::Tuple && s->args.size() == tp.elems.size()) {
                    for (size_t i = 0; i < tp.elems.size(); ++i) check_pattern(*tp.elems[i], s->args[i]);
                    return;
                }
                std::vector<TyPtr> fresh = fresh_args(tp.elems.size());
                if (!tc_.unify(s, make_tuple(fresh)) && s->kind != TyKind::Error)
                    error(p.line, p.col, "tuple pattern does not match " + describe(s));
                for (size_t i = 0; i < tp.elems.size(); ++i) check_pattern(*tp.elems[i], apply(fresh[i]));
                return;
            }
            case PatKind::Ctor:   check_ctor_pattern(static_cast<CtorPat&>(p), scrut);   return;
            case PatKind::Struct: check_struct_pattern(static_cast<StructPat&>(p), scrut); return;
            case PatKind::List:   check_list_pattern(static_cast<ListPat&>(p), scrut);   return;
            case PatKind::Map:    check_map_pattern(static_cast<MapPat&>(p), scrut);     return;
            case PatKind::Or:
                // Every alternative is checked against the SAME scrutinee; their bindings must agree
                // in name, type and mutability. Exhaustiveness is separate -- check_match_coverage
                // expands the alternatives into their own Maranget rows, and binders are invisible
                // there anyway.
                check_or_pattern(static_cast<OrPat&>(p), scrut);
                return;
        }
    }

    void check_ctor_pattern(CtorPat& c, TyPtr scrut) {
        const size_t errors_before = errors_.size();
        c.name = resolve_qualified_ref(c.qualifier, c.name, c.line, c.col);   // module-qualify (+ writeback)
        if (auto vit = variants_.find(c.name); vit != variants_.end()) {
            const VariantInfo& vi = vit->second;
            auto eit = enums_.find(vi.enum_name);
            size_t garity = (eit != enums_.end()) ? eit->second.generics.size() : 0;
            std::vector<TyPtr> targs = named_args_for(scrut, vi.enum_name, garity, c);
            std::unordered_map<uint32_t, TyPtr> m;
            if (eit != enums_.end())
                for (size_t i = 0; i < eit->second.generics.size() && i < targs.size(); ++i)
                    m[eit->second.generics[i].id] = targs[i];
            if (c.elems.size() != vi.field_types.size())
                error(c.line, c.col, "constructor '" + c.name + "' expects " +
                      std::to_string(vi.field_types.size()) + " field(s), got " + std::to_string(c.elems.size()));
            for (size_t i = 0; i < c.elems.size() && i < vi.field_types.size(); ++i)
                check_pattern(*c.elems[i], tc_.substitute(vi.field_types[i], m));
            for (size_t i = vi.field_types.size(); i < c.elems.size(); ++i)
                check_pattern(*c.elems[i], ty_error());
            return;
        }
        if (auto sit = structs_.find(c.name); sit != structs_.end() && sit->second.is_tuple) {
            const StructInfo& si = sit->second;
            std::vector<TyPtr> targs = named_args_for(scrut, c.name, si.generics.size(), c);
            std::unordered_map<uint32_t, TyPtr> m;
            for (size_t i = 0; i < si.generics.size() && i < targs.size(); ++i) m[si.generics[i].id] = targs[i];
            if (c.elems.size() != si.field_types.size())
                error(c.line, c.col, "constructor '" + c.name + "' expects " +
                      std::to_string(si.field_types.size()) + " field(s), got " + std::to_string(c.elems.size()));
            for (size_t i = 0; i < c.elems.size() && i < si.field_types.size(); ++i)
                check_pattern(*c.elems[i], tc_.substitute(si.field_types[i], m));
            return;
        }
        // The qualified head (`Enum::V`) may already have been reported by resolve_qualified_ref; a second
        // "unknown constructor" for the same spot would only repeat it.
        if (errors_.size() == errors_before && !report_if_ambiguous_variant(short_name(c.name), c.line, c.col))
            error(c.line, c.col, "unknown constructor '" + c.name + "' in pattern");
        for (auto& sub : c.elems) check_pattern(*sub, ty_error());
    }

    void check_struct_pattern(StructPat& s, TyPtr scrut) {
        s.name = resolve_qualified_ref(s.qualifier, s.name, s.line, s.col);   // module-qualify (+ writeback)
        // A record-variant pattern `V { f: p }` resolves in variants_ (mirrors check_ctor_pattern's variant branch).
        if (auto vit = variants_.find(s.name); vit != variants_.end()) {
            const VariantInfo& vi = vit->second;
            if (vi.is_tuple) {
                error(s.line, s.col, "variant '" + s.name + "' is not a record variant");
                for (auto& fp : s.fields) if (fp.pat) check_pattern(*fp.pat, ty_error());
                return;
            }
            auto eit = enums_.find(vi.enum_name);
            size_t garity = (eit != enums_.end()) ? eit->second.generics.size() : 0;
            std::vector<TyPtr> targs = named_args_for(scrut, vi.enum_name, garity, s);
            std::unordered_map<uint32_t, TyPtr> m;
            if (eit != enums_.end())
                for (size_t i = 0; i < eit->second.generics.size() && i < targs.size(); ++i)
                    m[eit->second.generics[i].id] = targs[i];
            std::unordered_map<std::string, size_t> field_index;
            for (size_t i = 0; i < vi.field_names.size(); ++i) field_index[vi.field_names[i]] = i;
            for (auto& fp : s.fields) {
                auto fit = field_index.find(fp.name);
                if (fit == field_index.end()) {
                    error(s.line, s.col, "variant '" + s.name + "' has no field '" + fp.name + "'");
                    if (fp.pat) check_pattern(*fp.pat, ty_error());
                    continue;
                }
                TyPtr ft = tc_.substitute(vi.field_types[fit->second], m);
                if (fp.pat) check_pattern(*fp.pat, ft);
                else define(fp.name, apply(ft), false, /*track=*/true, s.line, s.col);   // shorthand `{ x }` binds x
            }
            return;
        }
        auto sit = structs_.find(s.name);
        if (sit == structs_.end()) {
            if (!report_if_ambiguous_variant(short_name(s.name), s.line, s.col))   // collision hint
                error(s.line, s.col, "unknown struct '" + s.name + "' in pattern");
            for (auto& fp : s.fields) if (fp.pat) check_pattern(*fp.pat, ty_error());
            return;
        }
        const StructInfo& si = sit->second;
        std::vector<TyPtr> targs = named_args_for(scrut, s.name, si.generics.size(), s);
        std::unordered_map<uint32_t, TyPtr> m;
        for (size_t i = 0; i < si.generics.size() && i < targs.size(); ++i) m[si.generics[i].id] = targs[i];
        std::unordered_map<std::string, size_t> field_index;
        for (size_t i = 0; i < si.field_names.size(); ++i) field_index[si.field_names[i]] = i;
        for (auto& fp : s.fields) {
            auto fit = field_index.find(fp.name);
            if (fit == field_index.end()) {
                error(s.line, s.col, "struct '" + s.name + "' has no field '" + fp.name + "'");
                if (fp.pat) check_pattern(*fp.pat, ty_error());
                continue;
            }
            TyPtr ft = tc_.substitute(si.field_types[fit->second], m);
            if (fp.pat) check_pattern(*fp.pat, ft);
            else define(fp.name, apply(ft), false, /*track=*/true, s.line, s.col);   // shorthand `{ x }` binds x
        }
    }

    // Element type of a sequence type (List/Array/Vec[T]), or Error if not one.
    TyPtr seq_elem(TyPtr t) {
        t = apply(t);
        if (t->kind == TyKind::Named && t->args.size() == 1 &&
            (t->name == "List" || t->name == "Array" || t->name == "Vec"))
            return t->args[0];
        return ty_error();
    }

    void check_list_pattern(ListPat& l, TyPtr scrut) {
        TyPtr s = apply(scrut);
        TyPtr elem;
        std::string cname = "List";
        if (s->kind == TyKind::Named && s->args.size() == 1 && s->name == "List") {
            elem = s->args[0];
        } else if (s->kind == TyKind::Named && s->args.size() == 1 && (s->name == "Array" || s->name == "Vec")) {
            // A list pattern is a CONS pattern -- codegen tests for a cons cell, which a Vec / Array never
            // is, so accepting one here meant an arm that silently never matched (`match toVec([1, 5])
            // { [1, d] => d, _ => 0 }` was 0). Reject it, but still bind the elements at their element
            // type so the arm body does not cascade into unknown-variable errors.
            TypeRenderer r;
            error(l.line, l.col, "a list pattern matches a List, not a " + r(s) + " -- index the " + s->name +
                  " instead (`len(v)`, `v[i]`, `get(v, i)`)");
            elem = s->args[0]; cname = s->name;
        } else {
            elem = tc_.fresh_var();
            if (!tc_.unify(s, make_named("List", { elem })) && s->kind != TyKind::Error)
                error(l.line, l.col, "list pattern does not match " + describe(s));
        }
        for (auto& sub : l.elems) check_pattern(*sub, apply(elem));
        if (l.rest) {
            // The `..rest` tail must be a plain name or `_`: codegen binds the remaining spine
            // directly and has no path for anything else. That restriction was only ever enforced
            // DOWNSTREAM (a CodegenError after the checker had accepted the program), which is the
            // wrong layer -- `[a, ..mod::Ctor]` already parsed and reached it, and `@` would add
            // `[a, ..r @ p]`, which additionally under-measures the frame.
            if (l.rest->kind != PatKind::Ident && l.rest->kind != PatKind::Wildcard)
                error(l.rest->line, l.rest->col,
                      "a list '..' tail must be a name or '_' (it binds the remaining elements)");
            else
                check_pattern(*l.rest, make_named(cname, { apply(elem) }));
        }
    }

    void check_map_pattern(MapPat& mp, TyPtr scrut) {
        TyPtr s = apply(scrut);
        TyPtr K, V;
        if (s->kind == TyKind::Named && s->name == "Map" && s->args.size() == 2) {
            K = s->args[0]; V = s->args[1];
        } else {
            K = tc_.fresh_var(); V = tc_.fresh_var();
            if (!tc_.unify(s, make_named("Map", { K, V })) && s->kind != TyKind::Error)
                error(mp.line, mp.col, "map pattern does not match " + describe(s));
        }
        for (auto& entry : mp.entries) {
            check_expr(*entry.first, apply(K));
            check_pattern(*entry.second, apply(V));
        }
    }

    // ----- exhaustiveness + usefulness (Maranget) ---------------------------

    // The finite constructor space of a type, or nullopt for an "infinite"/opaque
    // type (Int/Double/String/Map/Array/Vec/type-var), where a wildcard is
    // required for exhaustiveness.
    std::optional<std::vector<CtorSig>> constructor_space(TyPtr ty) {
        ty = apply(ty);
        switch (ty->kind) {
            case TyKind::Bool:
                return std::vector<CtorSig>{ { "true", {} }, { "false", {} } };
            case TyKind::Unit:
                return std::vector<CtorSig>{ { "T:0", {} } };
            case TyKind::Tuple:
                return std::vector<CtorSig>{ { "T:" + std::to_string(ty->args.size()), ty->args } };
            case TyKind::Named: {
                if (auto eit = enums_.find(ty->name); eit != enums_.end()) {
                    const EnumInfo& ei = eit->second;
                    std::unordered_map<uint32_t, TyPtr> m;
                    for (size_t i = 0; i < ei.generics.size() && i < ty->args.size(); ++i)
                        m[ei.generics[i].id] = ty->args[i];
                    std::vector<CtorSig> out;
                    for (const auto& vn : ei.variant_names) {
                        CtorSig c; c.id = "V:" + vn;
                        auto v = variants_.find(vn);
                        if (v != variants_.end())
                            for (const auto& ft : v->second.field_types) c.fields.push_back(tc_.substitute(ft, m));
                        out.push_back(std::move(c));
                    }
                    return out;
                }
                if (auto sit = structs_.find(ty->name); sit != structs_.end()) {
                    const StructInfo& si = sit->second;
                    std::unordered_map<uint32_t, TyPtr> m;
                    for (size_t i = 0; i < si.generics.size() && i < ty->args.size(); ++i)
                        m[si.generics[i].id] = ty->args[i];
                    CtorSig c; c.id = "S:" + ty->name;
                    for (const auto& ft : si.field_types) c.fields.push_back(tc_.substitute(ft, m));
                    return std::vector<CtorSig>{ std::move(c) };
                }
                if (ty->name == "List" && ty->args.size() == 1) {
                    return std::vector<CtorSig>{
                        { "nil", {} },
                        { "cons", { ty->args[0], make_named("List", { ty->args[0] }) } }
                    };
                }
                return std::nullopt;   // Array / Vec / Map / Bytes -> opaque
            }
            default:
                return std::nullopt;   // Int / Double / String / Var / Fn / ...
        }
    }

    // ---- nested or-pattern row expansion -------------------------------------------------------
    //
    // Maranget works on ROWS with no alternation, so an arm containing nested alternatives has to
    // become several rows: `Some(A | B)` is exactly `Some(A)` and `Some(B)`. lower_pat marks such a
    // position with OR_MARKER and expand_ors distributes it upward through the enclosing
    // constructors -- the cartesian product over every marker in the row.
    //
    // The product is bounded. Above OR_EXPANSION_CAP rows the whole arm collapses to ONE opaque
    // leaf, which is the same treatment Map and Range already get: a con no constructor_space
    // contains, so it never completes a signature and never matches another row's specialization.
    // That is sound in BOTH directions -- it can only make an arm look more useful (no false
    // "unreachable match arm") and the match look less complete (at worst a spurious
    // "non-exhaustive match", which the user fixes with a `_`). Never a missed exhaustiveness hole.
    static constexpr const char* OR_MARKER = "$or";      // an alternation site, erased by expand_ors
    static constexpr const char* OR_CAPPED = "$or!";     // the over-cap fallback leaf
    static constexpr size_t OR_EXPANSION_CAP = 64;

    // Expand every OR_MARKER in `u` into concrete rows, appending to `out`. Sets `capped` and stops
    // early once more than `OR_EXPANSION_CAP` rows would be produced.
    static void expand_ors(const UPat& u, std::vector<UPat>& out, bool& capped) {
        if (capped) return;
        if (!u.wild && u.con == OR_MARKER) {             // an alternation site: each alt is its own row
            for (const UPat& a : u.args) {
                expand_ors(a, out, capped);
                if (capped || out.size() > OR_EXPANSION_CAP) { capped = true; return; }
            }
            return;
        }
        if (u.wild || u.args.empty()) { out.push_back(u); return; }

        std::vector<std::vector<UPat>> per;              // the expansions of each argument
        for (const UPat& a : u.args) {
            std::vector<UPat> rows;
            expand_ors(a, rows, capped);
            if (capped) return;
            per.push_back(std::move(rows));
        }
        UPat base = u; base.args.clear();
        std::vector<UPat> acc{ base };
        for (const auto& choices : per) {                // cartesian product, argument by argument
            std::vector<UPat> next;
            for (const UPat& prefix : acc)
                for (const UPat& ch : choices) {
                    UPat row = prefix; row.args.push_back(ch);
                    next.push_back(std::move(row));
                    if (next.size() > OR_EXPANSION_CAP) { capped = true; return; }
                }
            acc = std::move(next);
        }
        for (auto& r : acc) out.push_back(std::move(r));
    }

    // The rows one match ALTERNATIVE contributes: normally one, several if it nests alternation.
    static std::vector<UPat> rows_for(const UPat& lowered) {
        std::vector<UPat> rows;
        bool capped = false;
        expand_ors(lowered, rows, capped);
        if (capped || rows.empty()) return { UPat{ false, OR_CAPPED, {} } };
        return rows;
    }

    // A con id for a Double must be INJECTIVE -- distinct doubles must not collapse -- or two
    // genuinely different arms share a con and the second is reported "unreachable match arm".
    // `std::to_string` is `%f`, six fractional digits, so it collapsed `1.0000001` and `1.0000002`
    // (and anything past 1e15 in the other direction). `to_chars`'s shortest round-trip form is
    // injective by definition. The id is internal -- it appears in no diagnostic -- so only
    // injectivity matters, never stability across builds.
    //
    // The `-0.0` fold mirrors `Value::fromDouble` and is DEFENSIVE only: the lexer never builds a
    // negative-zero DoubleLit (the sign is a separate UnaryExpr), so a written `-0.0` still gets the
    // con `-d0` and does not collide with `0.0`'s `d0`. That misses a redundancy report rather than
    // inventing one, which is the same safe direction overlapping ranges already take.
    static std::string double_con(double v) {
        if (v == 0.0) v = 0.0;                       // folds -0.0 into 0.0 (the VM's own rule)
        char buf[40];
        auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v);
        return ec == std::errc{} ? "d" + std::string(buf, end) : "d?";
    }

    static std::string literal_con(const Expr& lit) {
        switch (lit.kind) {
            case ExprKind::BoolLit: return static_cast<const BoolLit&>(lit).value ? "true" : "false";
            case ExprKind::IntLit:  return "i" + std::to_string(static_cast<const IntLit&>(lit).value);
            case ExprKind::DoubleLit: return double_con(static_cast<const DoubleLit&>(lit).value);
            case ExprKind::StrLit:  return "s:" + static_cast<const StrLit&>(lit).value;
            case ExprKind::Unary: {   // negative numeric literal: `-1` / `-1.5`
                const auto& u = static_cast<const UnaryExpr&>(lit);
                return u.operand ? ("-" + literal_con(*u.operand)) : "lit?";
            }
            default: return "lit?";
        }
    }

    // Lower an AST pattern to the Maranget UPat over column type `colTy`.
    UPat lower_pat(Pattern& p, TyPtr colTy) {
        colTy = apply(colTy);
        switch (p.kind) {
            case PatKind::Wildcard:
            case PatKind::Ident:
                return UPat{};   // wildcard
            case PatKind::Literal: {
                auto& lp = static_cast<LiteralPat&>(p);
                UPat u; u.wild = false; u.con = lp.lit ? literal_con(*lp.lit) : "lit?";
                return u;
            }
            case PatKind::Range: {
                // A range is a NON-WILD leaf with a con id unique to its bounds. Int/Double have no finite
                // constructor_space, so it never completes exhaustiveness (a `_` stays required -- v1 scope);
                // an identical duplicate range is still caught as redundant (same con id), and a range after
                // a wildcard is unreachable as usual. Overlapping-but-distinct ranges are conservatively NOT
                // flagged (safe: no false "unreachable"), matching how Map/opaque leaves behave.
                // The separator MUST carry the exclusivity: `1..9` and `1..<9` are different sets, so
                // sharing one con id would make the second look like a duplicate of the first and be
                // reported as an unreachable arm.
                //
                // A bound may NAME a scalar `const`, so the id is built from the literal the bound
                // DENOTES (`bound_literal`), not from the bound node: `LO..HI` and `1..9` are the
                // same set and must share a con, or the second would never be caught as a duplicate.
                // A bound that resolves to nothing has already been reported, and gets a SITE-unique
                // id -- sharing one "?" across every bad range would stack a bogus "unreachable
                // match arm" on top of the real error.
                auto& rp = static_cast<RangePat&>(p);
                const Expr* lo = rp.lo ? bound_literal(*rp.lo) : nullptr;
                const Expr* hi = rp.hi ? bound_literal(*rp.hi) : nullptr;
                const std::string site = "?" + std::to_string(p.line) + ":" + std::to_string(p.col);
                UPat u; u.wild = false;
                u.con = "range:" + (lo ? literal_con(*lo) : site) + (rp.exclusive ? "..<" : "..") +
                        (hi ? literal_con(*hi) : site);
                return u;
            }
            case PatKind::Tuple: {
                auto& tp = static_cast<TuplePat&>(p);
                UPat u; u.wild = false; u.con = "T:" + std::to_string(tp.elems.size());
                for (size_t i = 0; i < tp.elems.size(); ++i)
                    u.args.push_back(lower_pat(*tp.elems[i],
                        (colTy->kind == TyKind::Tuple && i < colTy->args.size()) ? colTy->args[i] : ty_error()));
                return u;
            }
            case PatKind::Ctor: {
                auto& c = static_cast<CtorPat&>(p);
                if (auto vit = variants_.find(c.name); vit != variants_.end())
                    return lower_ctor("V:" + c.name, c.elems, ctor_fields(colTy, "V:" + c.name));
                if (auto sit = structs_.find(c.name); sit != structs_.end() && sit->second.is_tuple)
                    return lower_ctor("S:" + c.name, c.elems, ctor_fields(colTy, "S:" + c.name));
                return UPat{};   // unknown ctor (already errored) -> treat as wildcard
            }
            case PatKind::Struct: {
                auto& s = static_cast<StructPat&>(p);
                UPat u; u.wild = false;
                // A record-VARIANT pattern must lower to the enum's "V:" ctor id (matching
                // constructor_space), not "S:", or exhaustiveness over the enum breaks.
                const std::vector<std::string>* fnames = nullptr;
                if (auto vit = variants_.find(s.name); vit != variants_.end() && !vit->second.is_tuple) {
                    u.con = "V:" + s.name;
                    fnames = &vit->second.field_names;
                } else {
                    u.con = "S:" + s.name;
                    if (auto sit = structs_.find(s.name); sit != structs_.end())
                        fnames = &sit->second.field_names;
                }
                std::vector<TyPtr> fields = ctor_fields(colTy, u.con);
                if (fnames) {
                    std::unordered_map<std::string, Pattern*> by_name;
                    for (auto& fp : s.fields) by_name[fp.name] = fp.pat.get();
                    for (size_t i = 0; i < fnames->size(); ++i) {
                        Pattern* sub = by_name.count((*fnames)[i]) ? by_name[(*fnames)[i]] : nullptr;
                        TyPtr ft = i < fields.size() ? fields[i] : ty_error();
                        u.args.push_back(sub ? lower_pat(*sub, ft) : UPat{});
                    }
                }
                return u;
            }
            case PatKind::List: {
                auto& lp = static_cast<ListPat&>(p);
                TyPtr elemT = seq_elem(colTy);
                TyPtr listTy = make_named("List", { elemT });
                UPat tail = lp.rest ? lower_pat(*lp.rest, listTy) : UPat{ false, "nil", {} };
                for (size_t i = lp.elems.size(); i-- > 0;) {
                    UPat cons; cons.wild = false; cons.con = "cons";
                    cons.args.push_back(lower_pat(*lp.elems[i], elemT));
                    cons.args.push_back(std::move(tail));
                    tail = std::move(cons);
                }
                return tail;
            }
            case PatKind::Map:
                return UPat{ false, "map", {} };   // opaque; never completes a signature
            case PatKind::Bind:
                // The binding is invisible to exhaustiveness -- only `sub` constrains the value, the
                // same way an Ident lowers to a wildcard above.
                return static_cast<BindPat&>(p).sub ? lower_pat(*static_cast<BindPat&>(p).sub, colTy)
                                                    : UPat{};
            case PatKind::Or: {
                // A NESTED or-pattern (`Some(A | B)`, `(true | false, n)`). It cannot be one row, so
                // it lowers to a MARKER that expand_ors turns into one row per combination. Lowering
                // just the first alternative would silently under-approximate the row set.
                auto& op = static_cast<OrPat&>(p);
                if (op.alts.empty()) return UPat{};
                UPat u; u.wild = false; u.con = OR_MARKER;
                for (auto& a : op.alts) u.args.push_back(lower_pat(*a, colTy));
                return u;
            }
        }
        return UPat{};
    }

    UPat lower_ctor(std::string id, std::vector<PatPtr>& elems, const std::vector<TyPtr>& fields) {
        UPat u; u.wild = false; u.con = std::move(id);
        for (size_t i = 0; i < fields.size(); ++i)
            u.args.push_back(i < elems.size() ? lower_pat(*elems[i], fields[i]) : UPat{});
        return u;
    }

    std::vector<TyPtr> ctor_fields(TyPtr colTy, const std::string& id) {
        if (auto sp = constructor_space(colTy))
            for (const auto& c : *sp) if (c.id == id) return c.fields;
        return {};
    }

    std::optional<std::vector<UPat>> specialize_row(const std::vector<UPat>& row, const std::string& id, size_t arity) {
        if (row.empty()) return std::nullopt;
        std::vector<UPat> out;
        if (row[0].wild) out.assign(arity, UPat{});
        else if (row[0].con == id) { out = row[0].args; out.resize(arity); }
        else return std::nullopt;
        out.insert(out.end(), row.begin() + 1, row.end());
        return out;
    }

    // Maranget's U(P, q): is query `q` useful w.r.t. the pattern matrix `P`?
    bool useful(const std::vector<std::vector<UPat>>& P, const std::vector<UPat>& q,
                const std::vector<TyPtr>& col) {
        if (q.empty()) return P.empty();
        auto rest_col = [&] { return std::vector<TyPtr>(col.begin() + 1, col.end()); };
        auto rest_q   = [&] { return std::vector<UPat>(q.begin() + 1, q.end()); };

        if (!q[0].wild) {
            std::vector<TyPtr> fields = ctor_fields(col[0], q[0].con);
            std::vector<std::vector<UPat>> S;
            for (const auto& row : P)
                if (auto sr = specialize_row(row, q[0].con, fields.size())) S.push_back(std::move(*sr));
            std::vector<TyPtr> ncol = fields;
            ncol.insert(ncol.end(), col.begin() + 1, col.end());
            std::vector<UPat> nq = q[0].args; nq.resize(fields.size());
            nq.insert(nq.end(), q.begin() + 1, q.end());
            return useful(S, nq, ncol);
        }

        auto sp = constructor_space(col[0]);
        std::unordered_set<std::string> sigma;
        for (const auto& row : P) if (!row.empty() && !row[0].wild) sigma.insert(row[0].con);
        bool complete = sp && !sp->empty();
        if (complete) for (const auto& c : *sp) if (!sigma.count(c.id)) { complete = false; break; }

        if (complete) {
            for (const auto& c : *sp) {
                std::vector<std::vector<UPat>> S;
                for (const auto& row : P)
                    if (auto sr = specialize_row(row, c.id, c.fields.size())) S.push_back(std::move(*sr));
                std::vector<TyPtr> ncol = c.fields;
                ncol.insert(ncol.end(), col.begin() + 1, col.end());
                std::vector<UPat> nq(c.fields.size());
                nq.insert(nq.end(), q.begin() + 1, q.end());
                if (useful(S, nq, ncol)) return true;
            }
            return false;
        }
        std::vector<std::vector<UPat>> D;
        for (const auto& row : P)
            if (!row.empty() && row[0].wild) D.push_back(std::vector<UPat>(row.begin() + 1, row.end()));
        std::vector<TyPtr> ncol = rest_col();
        std::vector<UPat> nq = rest_q();
        return useful(D, nq, ncol);
    }

    // No coverage verdict over a scrutinee or an arm pattern that already failed to type: exhaustiveness of
    // an unknown type is meaningless, and its "non-exhaustive match" / "unreachable match arm" sorted ABOVE
    // the real error (it carets the `match` keyword, left of the scrutinee), so a newcomer read the
    // follow-on first. The real error has always been reported already, so nothing is lost. All three
    // match paths (infer, check against an expected type, statement position) come through here.
    void check_match_coverage_if_typed(MatchExpr& e, const TyPtr& scrut, size_t pattern_errors) {
        if (apply(scrut)->kind == TyKind::Error || pattern_errors != 0) return;
        check_match_coverage(e, apply(scrut));
    }

    void check_match_coverage(MatchExpr& e, TyPtr scrutTy) {
        std::vector<std::vector<UPat>> seen;   // prior UNGUARDED arm rows (each alternative is its own row)
        std::vector<TyPtr> col{ scrutTy };
        for (auto& arm : e.arms) {
            // An or-pattern arm `A | B | ...` contributes one Maranget row per alternative; a plain
            // arm is a single row. Reachability of the ARM = any alternative useful; a single dead
            // alternative in a live multi-alt arm is a warning, not an error.
            // Look THROUGH any `@` wrapper first: `n @ (A | B)` must get the same row-per-alternative
            // treatment `A | B` gets, since the binding constrains nothing. A chain (`a @ b @ p`) is
            // legal syntax, so peel until a non-Bind node.
            Pattern* head = arm.pat.get();
            while (head->kind == PatKind::Bind && static_cast<BindPat*>(head)->sub)
                head = static_cast<BindPat*>(head)->sub.get();

            const bool is_or = head->kind == PatKind::Or;
            std::vector<Pattern*> alts;
            if (is_or) for (auto& a : static_cast<OrPat&>(*head).alts) alts.push_back(a.get());
            else       alts.push_back(arm.pat.get());

            // One SOURCE alternative can contribute several rows (nested alternation), so the
            // row -> alternative mapping is not the identity and has to be recorded: the
            // per-alternative warning below asks whether ALL of that alternative's rows are dead.
            std::vector<UPat> rows;
            std::vector<size_t> row_alt;
            for (size_t ai = 0; ai < alts.size(); ++ai)
                for (UPat& r : rows_for(lower_pat(*alts[ai], scrutTy))) {
                    rows.push_back(std::move(r));
                    row_alt.push_back(ai);
                }

            if (!arm.guard) {
                std::vector<std::vector<UPat>> local = seen;   // seen + earlier alts of THIS arm
                std::vector<bool> uf(rows.size());
                bool any = false;
                for (size_t i = 0; i < rows.size(); ++i) {
                    uf[i] = useful(local, { rows[i] }, col);
                    any = any || uf[i];
                    local.push_back({ rows[i] });
                }
                if (!any)
                    error(arm.pat->line, arm.pat->col,
                          e.from_if_let ? "this `if let` pattern is irrefutable (it always matches) -- use a plain `let`"
                                        : "unreachable match arm");
                else if (is_or) {
                    std::vector<bool> alt_live(alts.size(), false);
                    for (size_t i = 0; i < rows.size(); ++i) if (uf[i]) alt_live[row_alt[i]] = true;
                    for (size_t ai = 0; ai < alts.size(); ++ai)
                        if (!alt_live[ai]) warn(alts[ai]->line, alts[ai]->col, "unreachable pattern alternative");
                }
                for (auto& r : rows) seen.push_back({ std::move(r) });
            }
        }
        if (useful(seen, { UPat{} }, col))
            error(e.line, e.col, "non-exhaustive match");
    }
};

} // namespace

CheckResult check(Program& program, const CheckOptions& options) {
    return Checker(program, options).run();
}

} // namespace svc
