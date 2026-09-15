#pragma once

// =============================================================================
// Codegen.h -- typed AST -> bytecode lowering for Skarn (namespace svc).
//
// The checker has already run and filled every Expr::ty, so lowering knows the
// concrete type at each node and picks the TYPED path: `*_INT` opcodes for Int
// operands, generic promoting ops for Double, and the I2D widening at an Int->Double
// boundary. Types are ERASED here -- nothing type-shaped reaches the VM.
//
// Deliverable 1 (P0 + P1) scope: a program is a list of top-level statements (the
// "main" region). Supported: Int/Double/Bool/String literals, arithmetic
// (typed int vs. promoting double) + bitwise/shifts, comparisons (typed int / *_NUM /
// generic), bool logic + short-circuit `&&`/`||`, `!`/unary `-`/`~`, `if`/`while`/
// blocks as expressions, `let`/`let mut` + assignment + scoping. Anything else --
// functions, calls, structs, enums, tuples, containers, `match`, closures, traits,
// `?`, `for`, `return` -- is a clean CodegenError (it lands in a later phase).
//
// The register model mirrors the former dynamic Codegen: per
// function/region within the r6 64-register window, [0, n_locals) hold params + let
// bindings (peak-concurrent, scope-reclaimed), [n_locals, frame_size) are expression
// temporaries (a LIFO stack). frame_size is computed by a measure pass before emit.
// =============================================================================

#include "Ast.h"
#include "Compiler.h"
#include "Assembler.h"
#include "NativeRegistry.h"   // native_id_of -- a native name resolves globally (capture analysis)

#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace svc {

// Thrown when lowering hits a construct not yet supported (a later-phase feature) or an
// internal register-budget violation. Carries the offending node's 1-based position.
// `display_name` (Naming.h) is applied in the CONSTRUCTORS -- the choke point every codegen message
// passes through -- so the entry program's internal `$entry::` prefix never reaches a reader, and a
// message added later cannot leak it. Mirrors what `Checker::error`/`warn` do for type errors.
class CodegenError : public std::runtime_error {
public:
    explicit CodegenError(const std::string& msg) : std::runtime_error(display_name(msg)) {}
    CodegenError(const std::string& msg, uint32_t line, uint32_t col)
        : std::runtime_error(display_name(msg) + " at line " + std::to_string(line) + ":" + std::to_string(col)),
          line_(line), col_(col) {}
    uint32_t line() const noexcept { return line_; }
    uint32_t col()  const noexcept { return col_; }
private:
    uint32_t line_ = 0;
    uint32_t col_  = 0;
};

class Codegen {
public:
    // Lower a whole (already type-checked) program to a Module.
    Module generate(const Program& prog);

    // Enable inlining of small non-recursive DIRECT calls. Off by default;
    // svc::compile / compile_modules set it from the process-wide svc::inline_calls() knob, which
    // the drivers drive (`static_vmrun --inline`). A pure codegen transform: the same program
    // compiled either way must produce the same VALUES, which is what the differential sweep checks.
    void set_inlining(bool on) { inlining_ = on; }
    // Print the measure-vs-emit inline site tally to stderr after codegen (development only).
    void set_inline_report(bool on) { inline_report_ = on; }
    // Tuning knobs, settable so ONE binary can sweep the space -- a rebuild per data point would
    // change code layout between points, which is exactly the confound a tuning sweep must avoid.
    void set_inline_max_nodes(int n) { inline_max_nodes_ = n; }
    void set_inline_max_depth(int d) { inline_max_depth_ = d; }

    // Scalar Replacement of Aggregates -- dissolve a non-escaping struct
    // binding into one register per FIELD, so the object is never built and its fields are never
    // read back through a heap pointer. The sequel to inlining and dependent on it: only after an
    // expansion are a struct's construction and its use in one body. Off by default.
    void set_sroa(bool on) { sroa_ = on; }
    // Print the dissolution tally (bindings the pre-pass permitted vs dissolutions emit took)
    // to stderr after codegen (development only).
    void set_sroa_report(bool on) { sroa_report_ = on; }
    // Tuning: dissolve only a struct with at most N fields. Register cost grows with the field
    // count, so this is the budget knob -- and **0 dissolves nothing**, which is the A/B's NULL
    // CONTROL: SROA on, bytecode byte-identical, true effect exactly zero. Settable so ONE binary
    // produces every data point (a rebuild per point would vary code layout between them).
    void set_sroa_max_fields(int n) { sroa_max_fields_ = n; }

private:
    // ----- measure pass (compute the frame size without emitting) -----
    struct Plan { int n_locals = 0; int peak_temp = 0; int frame_size = 0; };
    Plan plan_top_level(const std::vector<const Stmt*>& stmts);
    // Does a plan fit the register file -- frame <= 63 AND frame + the widest remaining outgoing
    // call window <= 64? See the definition for why the second half is not optional.
    bool plan_fits(const Plan& p) const;
    // Measure a region; if inlining made it not fit, revoke exactly the permissions this region
    // granted and measure once more with inlining off.
    Plan plan_with_inline_retry(const std::function<Plan()>& measure);
    Plan plan_function(const std::string& name, int nparams, const BlockExpr& body);
    // A single traversal computes BOTH frame quantities per node: `.temp` = peak concurrent
    // temporaries (position-independent, relative to temp_base_) and `.local_peak` = peak named
    // locals (a scope-stack peak threading `base`). Merging what used to be two parallel walkers
    // (need_* + peak_locals_*) means every AST shape is measured in ONE place -- no shadow-interpreter
    // drift between two switches. The emitter self-check (alloc_temp/alloc_local) backstops it.
    struct Need { int temp = 0; int local_peak = 0; };
    Need measure_stmts(const std::vector<const Stmt*>& stmts, int base);
    Need measure_stmt(const Stmt& s, int base);
    // The honoring wrapper -- the mirror of `compile_expr`: a node the checker marked
    // `widen_double` reserves the I2D temp. Every recursion goes through THIS, not the raw walk.
    Need measure_expr(const Expr& e, int base);
    // Memoized per measure run (see measure_cache_); the switch over AST shapes is the _uncached half.
    Need measure_expr_raw(const Expr& e, int base);
    Need measure_expr_raw_uncached(const Expr& e, int base);
    Need measure_block(const BlockExpr& b, int base);
    // Mirrors compile_coerced's allocation on the `.temp` field: a non-temp Int widened to Double
    // needs a fresh I2D temp; an IntLit folds to one double-const temp; a value already in a temp
    // is widened in place. Locals are unaffected. Use at every measure site feeding a want_double
    // coercion (let init / assign / binary operand / struct-literal field).
    Need measure_coerced(const Expr& e, int base, bool want_double);
    // Temp-only leaf helpers for calls (a call's argument LOCALS are measured by the Call/Pipe case
    // recursing into children; these size only the temp staging). They read measure_expr(a,0).temp.
    int  need_call(const std::vector<const Expr*>& args);
    int  need_indirect_call(const Expr& callee, const std::vector<const Expr*>& args);
    // NOT const: a dissolved `let` records itself in sroa_this_measure_ here, which is the only
    // point in the measure that sees every `let` of the region being planned.
    int  let_binding_count(const LetStmt& l);
    // True if the tail spine of `e` reaches a self-call (matching arity) -> the measure
    // reserves nparams temps for the TCO_CALL argument writeback.
    bool has_tail_self_call(const Expr& e, const std::string& name, int nparams) const;
    // Max argument count over NON-self tail calls on the spine (0 if none). The measure
    // reserves that + 1 (covers the indirect-tail callee temp; the direct-tail form needs
    // one fewer -- over-reserving a few temps is harmless).
    int  has_tail_nonself_call(const Expr& e, const std::string& self_name, int self_nparams) const;

    // ----- emit pass -----
    void begin_frame(const Plan& p);
    // Emit the top-level region: run each statement, leave the final expression's value
    // in r0, then HALT.
    void compile_top_level(const std::vector<const Stmt*>& stmts);
    // Emit one top-level function: begin its frame, bind params (by name) to r0.., then compile
    // the body in tail position (it emits its own RET / TCO). `ret_double` drives the Int->Double
    // return-boundary coercion (true iff the function's return type is Double). A synthetic trait
    // method fn passes its parameter names with `self` prepended; a lifted lambda passes its own.
    void compile_function(const std::string& name, const std::vector<std::string>& param_names,
                          const BlockExpr& body, bool ret_double);
    // THE ONE PLACE the checker's `Expr::widen_double` record is honored -- see the comment on the
    // definition. Everything that compiles a sub-expression goes through this or `compile_into`.
    int  compile_expr(const Expr& e);           // -> register holding the value
    int  compile_expr_raw(const Expr& e);       // the dispatch switch, WITHOUT the coercion
    void compile_into(const Expr& e, int dst);  // compute e, leave it in dst
    // Compute e into a register, widening Int->Double when `want_double` and e is Int
    // (an N1 coercion boundary): an int literal folds to a double constant, a runtime
    // Int emits I2D. Returns the register holding the (possibly widened) value. Compiles RAW, so a
    // site that derives `want_double` ITSELF never double-widens a node the checker also marked.
    int  compile_coerced(const Expr& e, bool want_double);
    int  compile_block(const BlockExpr& b);
    // Compile `e` in a VALUE-DISCARDED position -- a loop body, a non-final statement. Identical
    // side effects, but nothing is materialised for the result. The case that matters is a
    // statement-only block (`while c { i = i + 1 }`), which would otherwise synthesize the unit
    // Nil it is about to throw away; that Nil is a CONSTANT-POOL load, so in a loop body it costs
    // one dispatch PER ITERATION. See compile_block_stmts.
    void compile_discard(const Expr& e);
    // The shared body of compile_block. `want_value == false` is the discard mode above: no unit
    // Nil, no block-local rescue MOV, returns -1. NOTE the measure_* walk is deliberately NOT
    // taught about this mode -- it stays a sound UPPER bound on the frame (the self-check asserts
    // `emit_peak <= plan_peak`), so a discarded block merely leaves one plan slot unused.
    int  compile_block_stmts(const BlockExpr& b, bool want_value);
    // compile_if in discard mode: no result temp, no per-arm MOV, and no unit Nil for a missing
    // `else`. Same upper-bound note as above -- measure_if still plans the result slot.
    void compile_if_discard(const IfExpr& f);
    // compile_match in discard mode. Reaches `if let` and `while let` too, both of which the parser
    // desugars onto MatchExpr. Same upper-bound note -- measure_expr's Match case still plans the
    // result slot. The value path (compile_match) and the tail path (compile_tail_match) are
    // untouched, so a tail-position match -- most of std -- is unaffected.
    void compile_match_discard(const MatchExpr& m);
    void compile_stmt(const Stmt& s);
    // Register coalescing: when `dst >= 0` the FINAL op writes directly into `dst` (an assignment
    // target / let-init / return slot) instead of a fresh temp + MOV, saving the copy. Safe because
    // operands are computed into their own registers first and only the single result op targets
    // `dst`. `dst = -1` (the default) keeps the value-in-a-temp behaviour. Driven by compile_into.
    int  compile_arith(const BinaryExpr& b, int dst = -1);       // ADD/ADD_INT/… by result type
    int  compile_concat_operand(const Expr& e);    // a `+`-concat operand: TO_STRING a non-String
    int  compile_stringify(const Expr& e);         // toString/print lowering: `Name(inner)` if transparent, else TO_STRING
    int  compile_comparison(const BinaryExpr& b, int dst = -1);  // typed-int / *_NUM / generic
    int  compile_short_circuit(const BinaryExpr& b);
    int  compile_unary(const UnaryExpr& u, int dst = -1);
    int  compile_if(const IfExpr& f);
    int  compile_while(const WhileExpr& w);
    int  compile_loop(const LoopExpr& l);

    // ----- structs / tuples / enums (fixed-shape objects) -----
    // A registered struct type (a struct, or one enum variant): its VM type id, its fields
    // in declaration order (name -> slot is the index), and per-field "is Double?" for the
    // Int->Double field-init coercion. is_tuple marks a positional (tuple) shape.
    struct StructDef {
        uint16_t                 id = 0;
        std::vector<std::string> field_names;
        std::vector<char>        field_double;   // 1 if the field's declared type is Double
        bool                     is_tuple = false;
        bool                     is_transparent = false;   // erases to its single field's immediate; no NEW_STRUCT
        bool                     is_int_enum = false;      // an int-backed enum variant: IS the bare Int discriminant
        int64_t                  enum_disc = 0;            // its runtime discriminant (valid iff is_int_enum)
    };
    // Register every struct + enum variant as a fixed-shape type up front (before the
    // measure), so a constructor name classifies and a field slot resolves statically.
    void register_program_types(const Program& prog);
    void register_struct_shape(const std::string& name, const std::vector<Field>& fields,
                               bool is_tuple, bool is_transparent, DumpStyle style);
    const StructDef* find_struct(const std::string& name) const;
    bool cg_field_exists(const TyPtr& t, const std::string& name) const;
    // S2: inherent (traitless) methods, keyed by mangled target head -> method names. A `.method()`
    // whose head has an inherent method lowers to a DIRECT call of `$inherent$Head$method`.
    std::unordered_map<std::string, std::unordered_set<std::string>> inherent_methods_;
    bool is_inherent_method(const std::string& head, const std::string& name) const {
        auto it = inherent_methods_.find(head);
        return it != inherent_methods_.end() && it->second.count(name) > 0;
    }
    bool is_ctor_name(const std::string& name) const { return structs_.count(name) > 0; }
    static int field_slot(const StructDef& sd, const std::string& field);
    // The struct/variant name of an expression's static type ("" if not a Named struct).
    std::string struct_name_of(const TyPtr& t) const;
    // The registered StructDef if `t` is a transparent (erased) struct, else nullptr.
    const StructDef* transparent_struct_of(const TyPtr& t) const;
    // The (disc, name) variant list if `t` is an int-backed enum TYPE, else nullptr.
    const std::vector<std::pair<int64_t, std::string>>* int_enum_of(const TyPtr& t) const;
    // True iff a `==`/`!=` on `t` may use the fast EQ/NE opcode: an immediate or String at run time
    // (a primitive, or an erasure type -- transparent newtype / int-backed enum / Char). A COMPOSITE
    // (struct/tuple/enum/Vec/Array/Map/Bytes) or a generic type param -> the structural EQ_DEEP opcode.
    bool is_flat_eq_type(const TyPtr& t) const;
    // Lazily register (once per arity) the anonymous tuple type `$TupleN { _0.._N-1 }`.
    uint16_t ensure_tuple_type(int arity);
    int  compile_struct_lit(const StructLit& sl);     // NEW_STRUCT + SET_PROPs (record)
    int  compile_ctor_call(const std::string& name, const std::vector<const Expr*>& args); // tuple ctor
    int  compile_tuple(const TupleExpr& t);           // `(a, b)` -> $TupleN
    int  compile_field(const FieldExpr& fe);          // GET_PROP (record field or t.N)
    // `obj.f = v` -> SET_PROP; with `op != Assign` a read-modify-write (GET_PROP, op, SET_PROP) over
    // ONE evaluation of the receiver.
    void compile_field_assign(const FieldExpr& target, const Expr& value,
                              TokKind op = TokKind::Assign);

    // ----- containers: List / Map literals + `for` -----
    uint16_t ensure_cons_type();                      // the `$List { head, tail }` cons cell
    int  compile_list_lit(const ListLit& ll);         // `[...]` -> $List cons cells (empty = nil)
    int  compile_map_lit(const MapLit& ml);           // `#{...}` -> MAP_NEW + MAP_SETs
    // `for pat in iter { body }` -- the iterable kind (List / Map) is known STATICALLY from
    // `iter->ty`, so no GET_KIND dispatch: a List spine-walks, a Map iterates keys/values.
    int  compile_for(const ForExpr& f);
    // Number of leaf-ident bindings a for-pattern introduces (ident=1, `_`=0, tuple/struct/tuple-struct
    // = all nested leaf idents, recursively).
    int  for_pattern_var_count(const Pattern& p) const;
    // Bind the current element `r_elem` to the (irrefutable) for-pattern into the pre-allocated,
    // iteration-stable local `slots` -- ident leaves consume a slot, nested aggregates read through a
    // temp and recurse. Pushes each ident into scope.
    void bind_for_pattern(const Pattern& p, int r_elem, const std::vector<int>& slots);
    void bind_for_pattern_rec(const Pattern& p, int rs, const std::vector<int>& slots, size_t& si);

    // ----- match + patterns -----
    int  compile_match(const MatchExpr& m);            // compare-and-branch chain over arms
    void compile_tail_match(const MatchExpr& m);       // each arm body emitted in tail position
    // `e?` -- unwrap Ok/Some payload, else `return <scrutinee>` from the enclosing fn. The operand's
    // static type (Result[T,E] / Option[T], recognized by name) picks the success variant; the checker
    // has already enforced the enclosing return-type compatibility (a hard error, never reaches here).
    int  compile_try(const TryExpr& t);
    // Test `rs` (a protected scrutinee local) against `pat`; branch to `fail_lbl` on
    // mismatch, binding pattern vars into scope. `scrut_ty` is the scrutinee's static struct
    // name ("" if not a known struct), used to skip a redundant type discrimination.
    void compile_pattern_test(const Pattern& pat, int rs, const std::string& scrut_ty,
                              const std::string& fail_lbl);
    // Test the field/element at `slot` of `rs` (fetched via GET_PROP) against a sub-pattern.
    void compile_field_pattern(const Pattern& pat, int rs, int slot, const std::string& fail_lbl);
    // Bind an IRREFUTABLE destructuring `let` pattern (tuple / record struct / tuple-struct) whose
    // value is already in register `rs` -- reads slots into locals, no discrimination, no fail path.
    void bind_let_pattern(const Pattern& pat, int rs, bool is_mut);
    void compile_list_pattern(const ListPat& lp, int rs, const std::string& fail_lbl);
    void compile_map_pattern(const MapPat& mp, int rs, const std::string& fail_lbl);
    // Measure a pattern in one pass: `.binding_locals` = fresh locals its bindings need;
    // `.test_temps` = peak test-temporaries its discrimination/fetch needs.
    struct PatNeed { int test_temps = 0; int binding_locals = 0; };
    PatNeed measure_pattern(const Pattern& pat) const;

    // ----- calls -----
    // Dispatch a call `callee(args)`: a bare, unshadowed top-level fn name -> a static
    // direct CALL; anything else (a local/param holding a function value, an arbitrary
    // expression) -> an indirect CALL_INDIRECT. Constructors / builtins / trait methods
    // are later phases (a clean CodegenError).
    // `result_ty` is the call expression's static type (for the Array[Double]/Vec[Double] element
    // coercion of a container builtin); null when unavailable (harmless -- no coercion emitted).
    // `site` is the enclosing CALL / PIPE node -- the key the measure recorded an inline
    // permission under. Null (the synthetic for-loop dispatches) simply never inlines.
    int  compile_call_dispatch(const Expr* callee, const std::vector<const Expr*>& args,
                               const TyPtr& result_ty = {}, const Expr* site = nullptr);
    int  compile_call(const std::string& name, const std::vector<const Expr*>& args);
    int  compile_call_indirect(const Expr& callee, const std::vector<const Expr*>& args);
    // Container builtins (P7b): array(n, init) / vec() / push(v, x) / len(x) -> opcodes. `len` is
    // kind-dispatched by the VM; the others are Array/Vec-specific. Not TCO'd (value + RET in tail).
    static bool is_builtin_name(const std::string& name) { return is_builtin_fn_name(name); }
    // True if `callee` is an unqualified container-builtin call (not shadowed by a user fn).
    bool is_builtin_callee(const Expr* callee) const {
        if (!callee || callee->kind != ExprKind::Ident) return false;
        const auto& id = static_cast<const IdentExpr&>(*callee);
        return id.qualifier.empty() && local_reg(id.name) < 0 &&
               !user_fn_ref(id) && is_builtin_name(id.name);
    }
    int  compile_builtin_call(const std::string& name, const std::vector<const Expr*>& args,
                              const TyPtr& result_ty);
    // Native I/O calls (file / time / env / stdin) -> CALL_NATIVE via the VM registry
    // (NativeRegistry.h). A Plain native writes its result directly; a Result/Option native's raw
    // result is wrapped (Ok/Err by GET_KIND, Some/None by IS_NIL) exactly like the dynamic side.
    int  compile_native_call(const std::string& name, const std::vector<const Expr*>& args);
    void emit_wrap_result(int dst, int rres);   // rres is String -> Err(rres), else Ok(rres)
    void emit_wrap_option(int dst, int rres);   // rres is Nil    -> None,      else Some(rres)
    void emit_wrap_process(int dst, int rres);  // rres is String -> Err, else Ok(ProcessOutput{...})
    // True if `callee` is an unqualified native call (not a local / user fn). `native_call_need`
    // is its measure: dst + a GC-rooted temp arg window + fill scratch + the wrap temps.
    bool is_native_callee(const Expr* callee) const;
    int  native_call_need(const std::vector<const Expr*>& args, const Expr* callee);
    int  compile_index(const IndexExpr& ix);                 // `a[i]` -> ARRAY_GET (tri-kind)
    // `a[i] = v` -> ARRAY_SET / MAP_SET; with `op != Assign` a read-modify-write over ONE evaluation
    // of the container AND the index (the read is the total-or-trap form, MAP_GET_OR_TRAP for a map).
    void compile_index_assign(const IndexExpr& target, const Expr& value,
                              TokKind op = TokKind::Assign);
    // Desugar a pipe `x |> f` / `x |> f(a)` to (callee, [x, ...args]); returns the callee
    // Expr (an Ident) or nullptr if the rhs is not a name / call to one.
    const Expr* pipe_call_shape(const PipeExpr& p, std::vector<const Expr*>& args) const;

    // ----- traits (dispatch table + devirtualization) ---------------------------
    // The checker has ALREADY validated coherence/arity/ambiguity/completeness/supertraits,
    // so this link step only lowers: assign a global method id per (trait, method), stage
    // each impl/default body as a synthetic non-capturing fn, and later fill the dispatch
    // table `fn_id[method_id * width + dense_id]` (TypeUniverse.h) for the dynamic path.
    struct TraitDef {
        std::vector<std::string> methods;                        // decl order
        std::unordered_map<std::string, uint16_t> method_id;     // method -> global id
        std::unordered_map<std::string, int>      arity;         // method -> arg count (incl. self)
        std::unordered_map<std::string, uint16_t> default_fn_id; // method -> $default fn id (if any)
    };
    struct ImplDef {
        std::string trait_name;
        std::string head;                                        // target type-head name
        std::vector<uint32_t> dense_ids;                         // 1 for a struct/primitive; N for an enum
        std::unordered_map<std::string, uint16_t> own_fn_id;     // method -> impl fn id
    };
    // A synthetic method fn to declare + compile like a top-level fn (self is param 0).
    struct MethodFn {
        std::string              synth_name;
        std::vector<std::string> param_names;   // `self` prepended (a method's receiver is r0)
        const BlockExpr*         body = nullptr;
        const Type*              ret  = nullptr;
        bool                     is_default = false;
        std::string              trait, head, method;
        bool                     is_blanket = false;   // a blanket-impl body; head is its type param
        int                      blanket_idx = -1;     // index into blankets_ (for the fn-id back-fill)
        std::string              module;               // owning module prefix (fault caret); set at push
    };
    // A blanket impl `impl[T: bounds] Tr for T`: every concrete type satisfying `bound_traits`
    // gains `Tr` through the shared `method_fn_id` bodies (unless it has its own concrete impl).
    struct BlanketDef {
        std::string trait_name;
        std::vector<std::string> bound_traits;                   // the type param's bound trait names
        std::unordered_map<std::string, uint16_t> method_fn_id;  // method -> blanket body fn id
    };
    // A resolved (trait, method) reference for a dispatch site.
    struct MRef { uint16_t id = 0; int arity = 0; std::string trait, method; };

    void link_traits(const Program& prog);   // build the trait/impl model + stage synthetic fns
    void build_trait_table(Module& m);        // fill the dispatch grid (after emit, struct count final)
    MRef resolve_method(const std::string& qualifier, const std::string& method) const;
    // True if `name` is an unqualified trait-method name not shadowed by a fn / ctor. (During the
    // measure pass no locals are in scope, so a local shadow is ignored there -- over-reserving a
    // temp is harmless; the emit path re-checks local_reg for the actual routing.)
    bool is_trait_method_name(const std::string& name) const;
    bool is_trait_call(const Expr* callee) const;
    // Any qualified `Head::method(...)`, trait OR inherent -- the sizing walkers' predicate.
    static bool is_qualified_method_call(const Expr* callee);
    // The staged body name of a checker-resolved `Type::method` reference.
    static std::string inherent_fn_name(const IdentExpr& id);
    // The concrete impl-target head of a receiver's static type, or "" if it is not a concrete
    // monomorphic type (a generic Var, a tuple, a fn) -- those must dispatch dynamically.
    std::string concrete_head_of(const TyPtr& t) const;
    // The synthetic fn NAME implementing `method` of `trait` for concrete `head` (own impl wins over
    // a trait default), or "" if none resolves statically (-> dynamic RESOLVE_CALL dispatch).
    std::string devirt_target(const std::string& trait, const std::string& method,
                              const std::string& head) const;
    // Does concrete type `head` satisfy `trait` -- via a concrete impl, or (recursively, cycle-guarded)
    // via a blanket impl whose own bounds `head` meets? Drives blanket trait-table filling.
    bool head_satisfies_trait(const std::string& head, const std::string& trait,
                              std::unordered_set<std::string>& in_progress) const;
    // The dense id(s) of an impl target head: a struct -> one id (BUILTIN_COUNT + struct id); an
    // enum -> one per variant; a primitive / Map / Array -> its fixed TID_*; List -> BOTH the $List
    // cons-cell struct column AND TID_NIL. Not const: List force-registers the `$List` type. Throws
    // for a not-yet-lowered target (Vec/Bytes -- the VM does not dispatch KIND_VEC/KIND_BYTES -- or a
    // blanket impl's type param).
    std::vector<uint32_t> dense_ids_for_head(const std::string& head, uint32_t line, uint32_t col);
    // Does the `for`-loop iterable's static type iterate via the `Iterable` protocol (a generic var
    // bounded by Iterable, or a concrete type with an `impl Iterable`)? Drives the compile_for fallback.
    bool iter_protocol_applies(const TyPtr& it) const;
    // Which lazy iteration protocol the `for`-loop iterable's static type supports (checked BEFORE the
    // eager Iterable fallback): Iterator -> the value IS a pull cursor, drive next() in place;
    // IntoIterator -> the value PRODUCES a fresh cursor via intoIter(), then drive next().
    enum class IterProto { None, Iterator, IntoIterator };
    IterProto iterator_protocol(const TyPtr& it) const;
    int  compile_trait_call(const MRef& ref, const std::vector<const Expr*>& args,
                            const Expr* site = nullptr);   // devirt (inlinable) or dynamic
    // A 1-arg trait method dispatched dynamically on a receiver ALREADY in register `rrecv` (a loop
    // cursor local, not an AST node): the fused RESOLVE_CALL, result returned in a fresh temp.
    int  emit_next_call(int rrecv, const MRef& ref);
    void emit_tail_trait_call(const MRef& ref, const std::vector<const Expr*>& args);// fused RESOLVE_TCO_CALL

    // ----- closures (lambda lifting + by-value capture) -------------------------
    // Every lambda is LIFTED to a synthetic top-level fn (`$lambdaN`); it evaluates to a `Func`
    // immediate (`LOAD_FN`) when capture-free, else a `KIND_CLOSURE` (`MAKE_CLOSURE` over a
    // contiguous window of its captured values, read back inside the body with `LOAD_CAPTURE`).
    // Capture is BY VALUE. The static language has NO self-recursive `let f = <lambda>` (a `let`
    // binds after its init), so there is no `self_slot` back-patch and no mutual-recursion cells --
    // recursion goes through a top-level `fn` (already order-independent).
    struct LiftedLambda { std::string name; const LambdaExpr* lam; std::string module; };
    void collect_lambdas_program(const Program& prog);
    void collect_lambdas_expr(const Expr& e);
    void collect_lambdas_stmt(const Stmt& s);
    void collect_lambdas_block(const BlockExpr& b);
    void analyze_captures();
    // A lambda's free variables = names its body references that are NOT bound (params/let/match/
    // for), NOT a top-level fn / constructor / trait-method name. `bound` is threaded DOWN through
    // nested lambdas so an outer local used only in a nested lambda is captured at both levels.
    void free_vars_expr(const Expr& e, const std::unordered_set<std::string>& bound,
                        std::vector<std::string>& out, std::unordered_set<std::string>& seen) const;
    void free_vars_stmt(const Stmt& s, std::unordered_set<std::string>& bound,
                        std::vector<std::string>& out, std::unordered_set<std::string>& seen) const;
    void free_vars_block(const BlockExpr& b, std::unordered_set<std::string> bound,
                         std::vector<std::string>& out, std::unordered_set<std::string>& seen) const;
    void set_current_lambda_captures(const LambdaExpr* lam);   // capture name -> index, for a body
    void emit_capture_source(const std::string& name, int slot);  // fill a capture window slot
    int  compile_lambda(const LambdaExpr& lam);   // LOAD_FN (capture-free) or MAKE_CLOSURE
    bool is_capture_name(const std::string& name) const { return current_lambda_captures_.count(name) > 0; }
    // True if `name` resolves GLOBALLY (a top-level fn / constructor / trait method / container
    // builtin / native), NOT to a local -- so a lambda body that references it must NOT try to
    // capture it. (Builtins like `len` / `array` were missing here, so a lambda using one -- e.g.
    // `fn(x) { len(x) }` -- wrongly attempted to capture the builtin name and aborted codegen; the
    // same held for NATIVES -- `fn() { getEnv("X") }` -- fixed by the native_id_of clause, found by
    // the native differential sweep.)
    //
    // Deliberately keeps `is_function_name` rather than `user_fn_ref`: this is a UNION of every
    // global set, not a routing choice. A name that loses the user fn (shadowing is out of scope
    // here -- see user_fn_ref) is still a builtin or a native, so the answer stays true and the
    // capture analysis stays correct. Nothing to "fix up" later.
    bool is_global_name(const std::string& name) const {
        return is_function_name(name) || is_ctor_name(name) || is_trait_method_name(name) ||
               is_builtin_name(name) || is_const_name(name) || native_id_of(name) >= 0;
    }

    // ----- tail position (emits RET / TCO directly) -----
    void compile_tail(const Expr& e);
    void compile_tail_block(const BlockExpr& b);
    void compile_tail_if(const IfExpr& f);
    // The shared tail-call classifier (self / direct-named / indirect), else value + RET.
    void compile_tail_call_shape(const Expr* callee, bool callee_is_ident,
                                 const std::string& name, const std::string& qualifier,
                                 const std::vector<const Expr*>& args, const Expr& fallback);
    void emit_tail_self_call(const std::vector<const Expr*>& args);
    void emit_tail_direct_call(const std::string& name, const std::vector<const Expr*>& args);
    void emit_tail_indirect_call(const Expr& callee, const std::vector<const Expr*>& args);
    void emit_return_nil();
    // Compute `e` into r0 for a RET, applying the Int->Double return-boundary coercion when
    // the enclosing function returns Double and `e` is Int.
    void compile_ret_value(const Expr& e);
    bool is_function_name(const std::string& name) const { return fn_names_.count(name) > 0; }
    // Does this REFERENCE resolve to a top-level user fn? `is_function_name` alone cannot say:
    // a bare name may be an ENTRY-module fn the checker deliberately routed past, because it is
    // not in scope inside a real module -- there the ambient builtin / native / trait method wins
    // (`fn toInt` must not hijack the name inside `std::math`). The checker owns that decision and
    // records it in `IdentExpr::ambient`; every ROUTING site asks here rather than
    // `is_function_name`, so codegen cannot drift from the checker.
    bool user_fn_ref(const IdentExpr& id) const { return !id.ambient && is_function_name(id.name); }
    // A module const (S0): its reference inlines the literal initializer at the use site.
    bool is_const_name(const std::string& name) const { return const_defs_.count(name) > 0; }
    // Compile `cond` (statically Bool -- the checker forbids non-Bool conditions) and
    // branch to `target` when it is false. No TO_BOOL: a static win over the dynamic side.
    void branch_if_false(const Expr& cond, const std::string& target);
    // Branch to `target` when `cond` (statically Bool) is TRUE -- the fused complement of
    // branch_if_false, used by the rotated (test-at-bottom) loop back-edge. Tests the DIRECT
    // relation (no negation), so the fused int/`*_NUM` compare-branch is NaN-sound for Double too.
    void branch_if_true(const Expr& cond, const std::string& target);

    // ----- register allocator (LIFO temp stack above the locals) -----
    int  alloc_temp();
    // The single choke point for local-register allocation (params, let/match/for bindings).
    // Guards the frame budget at the emit site (a local landing in the temp region means the
    // measure_* walk under-measured `.local_peak`) and tracks the peak for the frame-end self-check.
    int  alloc_local();
    void free_if_temp(int reg);
    bool is_temp(int reg) const { return reg >= temp_base_; }
    int  local_reg(const std::string& name) const;   // -1 if not a local
    std::string fresh_label(const char* prefix);

    // ----- type-directed helpers (read the checker's Expr::ty) -----
    static bool is_int_ty(const TyPtr& t);
    static bool is_double_ty(const TyPtr& t);
    static bool is_num_ty(const TyPtr& t);       // Int or Double
    static bool is_string_ty(const TyPtr& t);    // String
    // Arithmetic/bitwise opcode for `op`; `dbl` picks the promoting double form over the
    // typed *_INT form (bitwise/shifts are always Int).
    static OpCode arith_opcode(TokKind op, bool dbl);

    Assembler as_;

    // Top-level function tables, populated up front (before the measure pass) so calls
    // resolve order-independently and a bare fn name classifies as a value / call target.
    std::unordered_set<std::string>       fn_names_;   // top-level fn names
    std::unordered_map<std::string, int>  fn_arity_;   // fn name -> param count
    std::unordered_map<std::string, const Expr*> const_defs_;   // const name -> literal initializer (S0; inlined)
    // A const ARRAY (`const NAME: Array[T] = [...]`) instead maps its name -> the const-array pool
    // index; a reference lowers to LOAD_CONST_ARRAY (a shared, pre-built, immutable instance) rather
    // than inlining. Populated alongside const_defs_ in generate().
    std::unordered_map<std::string, uint16_t> const_array_index_;
    std::unordered_map<std::string, Plan> fn_plans_;   // fn name -> measured frame plan

    // The function currently being emitted (for self-tail-call detection). Empty name /
    // -1 arity while emitting the top-level "main" region (which HALTs, never RETs, and
    // cannot be tail-called).
    // The region currently being EMITTED, for the frame-budget diagnostics only. Unlike
    // current_fn_name_ it is not cleared by an inline expansion, so an under-measure reported from
    // inside a spliced body still names the function whose frame actually overflowed.
    std::string emit_region_name_;
    std::string current_fn_name_;
    int         current_fn_nparams_ = -1;
    bool        current_fn_ret_double_ = false;   // the enclosing fn returns Double (RET coercion)

    std::unordered_map<std::string, StructDef> structs_;         // struct/variant name -> def
    // Int-backed enum TYPE name -> its variants (discriminant, name) in declaration order. Drives
    // the static-type-directed `toString` discriminant->name switch (compile_stringify).
    std::unordered_map<std::string, std::vector<std::pair<int64_t, std::string>>> int_enum_variants_;
    std::unordered_map<int, uint16_t>          tuple_type_ids_;  // arity -> $TupleN type id (lazy)
    int                                        cons_type_id_ = -1;  // `$List` cons-cell type id (lazy)

    // Trait dispatch model (built by link_traits, consumed by the call sites + build_trait_table).
    std::unordered_map<std::string, TraitDef>  traits_;          // trait name -> def
    std::vector<ImplDef>                        impls_;
    std::vector<BlanketDef>                     blankets_;        // blanket impls (one per trait)
    std::unordered_map<std::string, size_t>     impl_index_;     // "trait\0head" -> impls_ index
    std::unordered_map<std::string, std::vector<std::string>> method_owners_;   // method -> trait names
    std::unordered_map<std::string, std::vector<std::string>> enum_variants_;   // enum name -> variant names
    std::vector<MethodFn>                       method_fns_;     // synthetic impl/default fns to emit
    uint16_t                                    next_method_id_ = 0;

    // Closure state (built before the measure pass, consumed by the lambda call sites).
    std::vector<LiftedLambda>                   lifted_;
    std::unordered_map<const LambdaExpr*, std::string>              lambda_names_;
    std::unordered_map<const LambdaExpr*, std::vector<std::string>> lambda_captures_;
    int                                         lambda_seq_ = 0;
    // During the measure/emit of a lambda body: captured name -> its capture index.
    std::unordered_map<std::string, int>        current_lambda_captures_;
    bool                                        current_fn_is_lambda_ = false;
    // The module prefix of the item whose body collect_lambdas_* is currently descending, so a
    // lifted lambda inherits its enclosing item's module for the fault caret ("" = root/entry).
    std::string                                 collect_module_;

    // An in-scope local: name -> register. (No struct-type tracking yet -- structs are a
    // later phase.) `is_mut` is recorded for completeness; assignment is checked upstream.
    //
    // SROA: a DISSOLVED binding has one register per FIELD in `fields` (empty = an ordinary
    // local), and `reg` stays a VALID register -- fields[0] -- rather than a sentinel, because
    // seven of local_reg's callers use only its SIGN, as "is this name shadowed by a local?".
    // Answering -1 there would let a dissolved `x` fall through to a module const / fn of the
    // same name and silently compile the WRONG thing -- the shadowing class this project has
    // already been bitten by.
    //
    // The field registers are embedded rather than kept in a parallel structure keyed by scope
    // slot, which is what the plan called for. Same reasoning, stronger conclusion: correctness
    // rests on the scope_mark / scope_.resize discipline, and there are TWELVE restore points
    // (block, three match flavours, five for-loop bodies, tail-block, inline). One truncation
    // that a parallel structure could miss is one too many; embedded, `resize` cannot forget.
    struct Local {
        std::string      name;
        int              reg;
        bool             is_mut = false;
        std::vector<int> fields;            // non-empty <=> dissolved (SROA)
    };
    std::vector<Local> scope_;

    // Loop targets for break/continue. `result` is the register a `break value` moves its
    // value into (the enclosing `loop`'s result temp, allocated before the body so it stays
    // reserved across it); -1 for a `while`/`for`, which never carry a break value.
    struct LoopCtx { std::string brk; std::string cont; int result; };
    std::vector<LoopCtx> loop_stack_;

    int n_locals_   = 0;   // next local register to hand out
    int temp_base_  = 0;   // first temporary register (== plan.n_locals)
    int temp_top_   = 0;   // next free temporary
    int frame_size_ = 0;   // current region's frame size
    int label_seq_  = 0;   // fresh-label counter

    // Emitter self-check (frame-sizing): the peak locals/temps actually touched during emit,
    // cross-checked against the plan at each frame boundary. plan_* are stashed by begin_frame.
    int emit_peak_temp_   = 0;   // max (temp_top_ - temp_base_) observed this frame
    int emit_peak_locals_ = 0;   // max n_locals_ observed this frame
    int plan_peak_temp_   = 0;   // the plan's peak_temp for the current frame
    int plan_n_locals_    = 0;   // the plan's n_locals for the current frame

    // ----- inlining -----
    // The master switch (set_inlining). While false NOTHING below is consulted and codegen is
    // byte-identical to the pre-inliner compiler -- which is what makes the A/B a single binary.
    bool inlining_      = false;
    bool inline_report_ = false;

    // What a direct call site could expand in place: the callee's body plus everything
    // compile_function would bind for it. Built ONCE (build_inline_targets) BEFORE the measure
    // pass, from the two body-carrying sources -- top-level FnItems, and the synthetic method fns
    // in method_fns_, which already carry `const BlockExpr* body`. Keyed by the MANGLED name a
    // direct call resolves to, so a hit is exactly "the function `CALL <name>` would have run".
    // Lifted lambdas are deliberately ABSENT: a lambda body resolves its free names through
    // current_lambda_captures_ -> LOAD_CAPTURE, which reads the callee's own closure object; an
    // expansion inside another frame has no such closure to read.
    struct InlineTarget {
        const BlockExpr*         body = nullptr;
        std::vector<std::string> params;         // in order; "self" first for a method
        bool ret_double = false;                 // drives the Int->Double return-boundary coercion
        int  nodes      = 0;                     // body size -- the threshold input
        bool has_lambda = false;                 // a body lambda is lifted per-body; see above
        bool has_return = false;                 // an early `return`  -> needs the S3 exit label
        bool has_try    = false;                 // the `?` operator    -> needs the S3 exit label
        bool duplicate  = false;                 // name staged twice -> never expand (see below)
        // SROA (S2): the body's TAIL expression when it is a plain struct literal. Such an
        // expansion can write a dissolved destination's field registers directly, so the callee's
        // result object is never built -- which is the whole `Vec3::add`/`sub`/`scale` shape.
        const StructLit* tail_lit = nullptr;
        // SROA (S3): per parameter, "every use in this body is `p.field`". Such a parameter can be
        // bound to a dissolved argument's field registers instead of one register holding an
        // object -- which is what stops a dissolved binding from being materialised again the
        // moment it is passed on, and is where ~70 % of the SROA payoff lives (the two-kernel
        // decomposition puts deleted GET_PROPs at 30.3 % against allocation's 12.8 %).
        //
        // The plan called for a separate `mut`-parameter exclusion. It is REDUNDANT and therefore
        // not stored: a mut parameter that actually mutates does so through `p.f = v`, whose place
        // ROOT is `p` -- already an escape (sroa_scan_target), so field_only is false anyway. One
        // that never mutates is harmless. MethodFn does not carry mutability at all, so relying on
        // the scan rather than on a flag is also what makes methods reachable.
        std::vector<char> params_field_only;
    };
    // Body-size ceiling, in scanned AST nodes. Sized against the measured customer: the leaf
    // `Vec3` methods (`sub`/`dot`/`lengthSquared`) come out around 16-20 nodes.
    int inline_max_nodes_ = 20;
    // How deep an expansion may nest. 1 = a call inside an expanded body stays an ordinary call.
    // Raising it multiplies code growth, so it is a measured decision, not a default.
    int inline_max_depth_ = 2;
    std::unordered_map<std::string, InlineTarget> inline_targets_;
    void build_inline_targets(const std::vector<const FnItem*>& fns);
    // Scan a callee body ONCE for everything the eligibility decision needs. Deliberately one
    // traversal with an EXHAUSTIVE switch (no `default:`, C4062 escalated to an error) so a kind
    // added to Ast.h fails the BUILD here instead of slipping through: a `return` hiding inside an
    // unhandled node would be emitted as a bare RET *from the caller* -- silent wrong code, the
    // exact class this project's guards exist to make loud.
    void scan_inline_body(const Expr& e, InlineTarget& t) const;
    void scan_inline_body_stmt(const Stmt& s, InlineTarget& t) const;
    // The mangled callee of a DIRECT call, or "" when the site is not one. A PURE mirror of the
    // direct-call arms of compile_call_dispatch, for the measure pass (which cannot run the
    // emitters). It reads only the callee expression, the checker's write-backs and the tables
    // built before the measure -- never scope_ / fn_plans_ / the frame counters, whose values
    // depend on where the walk currently stands. Like is_trait_method_name above it is
    // deliberately PERMISSIVE where the two passes cannot agree (scope_ is empty during the
    // measure): emit re-checks and may decline, which merely leaves a reserved slot unused.
    // `out_args` receives the list the CALLEE sees, which is NOT always `args`: a `recv.method(a)`
    // site prepends the receiver, exactly as emit's `margs` does. Getting that wrong is not
    // cosmetic -- the measure would then decline precisely the sites emit takes, i.e. under-size
    // the frame for an expansion that happens. Yielded rather than recomputed at each caller, for
    // the same reason the target name is.
    std::string direct_call_target(const Expr* callee, const std::vector<const Expr*>& args,
                                   std::vector<const Expr*>& out_args) const;
    // Stamp the per-instruction line/column side table from a node -- the ONLY way codegen sets a
    // position, so the inline pin (pos_pinned_) cannot be bypassed by a site added later.
    void set_pos_at(const Expr& e) { if (!pos_pinned_ && e.line) as_.set_pos(e.line, e.col); }
    void set_pos_at(const Stmt& s) { if (!pos_pinned_ && s.line) as_.set_pos(s.line, s.col); }
    // The frame an expansion of `t` at `margs` needs, mirroring compile_inline step for step.
    Need measure_inline_site(const InlineTarget& t, const std::vector<const Expr*>& margs, int base);
    // Expand `t` in place: bind the arguments to fresh locals, compile the body in VALUE position
    // (never compile_tail -- a TCO_CALL would tail-call out of the CALLER), return the result reg.
    // `dst` non-null (SROA, S2): the body's tail struct literal is written into those registers
    // instead of into a result temp, so the callee's object is never built. Returns (*dst)[0] then,
    // which is a real register -- the emit hooks test `>= 0` to decide whether to fall back to an
    // ordinary call, and a sentinel there would make them emit the call a SECOND time.
    int  compile_inline(const InlineTarget& t, const std::vector<const Expr*>& margs,
                        const Expr* site, const std::vector<int>* dst = nullptr);
    // The expandable callee for `name` at `nargs` arguments, or nullptr. The ONE eligibility
    // decision; consulted by the measure (to record a site) and by emit (to take it).
    const InlineTarget* inline_target_for(const std::string& name, size_t nargs) const;
    // Emit hook, called at each DIRECT-call arm before the ordinary compile_call: expand the
    // callee in place and return the register holding its value, or -1 to fall back to the call.
    // Returning -1 is always safe -- the measure merely reserved a frame slot that goes unused.
    // `site` is the CALL / PIPE node, the key the measure recorded its PERMISSION under: emit
    // expands only what the measure sized the frame for, and never re-derives that answer.
    int try_inline_call(const Expr* site, const std::string& name,
                        const std::vector<const Expr*>& args);
    // Call/Pipe nodes the measure judged expandable. Written ONLY by the measure walk, read only
    // by emit. A node may be asked for several times per measure (need_call re-walks arguments at
    // base 0, and an inline site measures its arguments once more) -- a SET makes that idempotent,
    // which a counter is not.
    std::unordered_set<const Expr*> inline_sites_;

    // The measure MEMO. Those repeated walks each recursed through the whole subtree, so a nesting of
    // n calls cost 2^n measure steps (3^n with the inliner's extra argument walk): 387 million for
    // sixteen nested `inc(..)` before this existed. A node's Need depends on `base` only by TRANSLATION
    // -- `.temp` not at all, `.local_peak` as base + a constant -- so one entry per node answers every
    // base it is asked at; measure_expr_raw self-checks that assumption node by node. The key also holds
    // what legitimately varies WITHIN one run: the expansion depth, and whether this node is the Pipe
    // rhs being skipped. Everything else (inlining_, sroa_, the decision maps) changes only between the
    // runs of plan_with_inline_retry, which clears the memo before each.
    struct MeasureKey {
        const Expr* e; int depth; bool skip;
        bool operator==(const MeasureKey& o) const { return e == o.e && depth == o.depth && skip == o.skip; }
    };
    struct MeasureKeyHash {
        size_t operator()(const MeasureKey& k) const {
            return std::hash<const Expr*>()(k.e) ^ (static_cast<size_t>(k.depth) << 1) ^ (k.skip ? 1u : 0u);
        }
    };
    std::unordered_map<MeasureKey, Need, MeasureKeyHash> measure_cache_;   // value: { temp, local_peak - base }
    // The node whose invariance self-check is currently re-deriving it (it must bypass its own entry).
    const Expr* measure_bypass_ = nullptr;

    // measure_expr's Pipe case recurses into `p.rhs` for local_peak, and for `x |> f(a)` that rhs
    // IS a CallExpr node -- but emit never visits it as a call (compile_expr's Pipe goes straight
    // to compile_call_dispatch with the shape pipe_call_shape derived, keyed by the PIPE node).
    // Counting it would record a phantom site: harmless to correctness (emit simply never looks
    // it up) but it would reserve frame for an expansion that never happens, and it would make
    // the S1 drift report disagree for a reason that is not drift. Set while descending that rhs.
    const Expr* measure_skip_call_ = nullptr;

    // Drift report (set_inline_report): DISTINCT sites permitted by the measure vs expansions
    // taken by emit. Emit may take fewer -- a tail-position site is measured but never expanded
    // (TCO wins there) -- but never more, which is checked as a hard internal error.
    int inline_seen_emit_ = 0;

    // One entry per expansion currently being emitted. `return` and the `?` operator inside an
    // expanded body must NOT emit a RET -- that would return from the CALLER -- so they write the
    // value into this frame's result register and jump to its exit label instead. A stack rather
    // than a single slot because an expansion's ARGUMENTS are compiled before its own frame is
    // pushed, so a `return` there still belongs to whatever encloses it.
    struct InlineFrame { int result; std::string exit_label; };
    std::vector<InlineFrame> inline_stack_;
    // Where a `return` writes its value: r0 normally (the calling convention), the innermost
    // expansion's result register inside one. Consulted by compile_ret_value and compile_try, so
    // neither has to know whether it is inside an expansion.
    int  return_target() const { return inline_stack_.empty() ? 0 : inline_stack_.back().result; }
    // Emit the terminator a `return` needs here: RET normally, a jump to the expansion's exit
    // label inside one.
    void emit_return_jump();

    // Expansion nesting. Non-zero means "currently inside an expanded body", which BOTH passes
    // use to enforce depth 1. Emit needs it independently of the permission set: a call node
    // inside a callee body is permitted when that callee is measured as its OWN function, and
    // expanding it while the body is spliced elsewhere would use a frame nobody sized for it.
    int inline_depth_ = 0;
    // How often the frame-budget retry revoked a region's permissions. A non-zero count means
    // inlining is being limited by the 63-register file rather than by the size threshold, which
    // is what would make argument ALIASING (instead of copying) worth its complexity.
    int inline_retries_ = 0;
    // Sites recorded during the measure currently running, so the frame-budget retry can undo
    // exactly them (plan_* clears this before each attempt).
    std::vector<const Expr*> sites_this_measure_;
    // Largest argument count over the call sites of the region being measured that will NOT be
    // expanded. Those are the ones that still open an outgoing window at `frame_size`, and
    // compile_call throws when `frame_size + nargs > 64` -- an EMIT-time error, after the plan is
    // frozen. Inlining raises frame_size, so without this a program that compiled yesterday could
    // abort today. Feeds the retry predicate.
    int measure_max_call_args_ = 0;
    // While true, set_pos_at is a no-op so every instruction of an expansion keeps the CALL
    // SITE's position. Without it a trap inside an inlined callee would report the caller's
    // function and module (fault_fn_index_of attributes by ip range) with the callee's line
    // number -- a caret drawn at unrelated source.
    bool pos_pinned_ = false;

    // ----- scalar replacement of aggregates -----
    // The master switch (set_sroa). While false NOTHING below is consulted and codegen is
    // byte-identical to the pre-SROA compiler -- the same single-binary A/B property the inliner's
    // `inlining_` gives, and the reason a rebuild never sits between two measured points.
    bool sroa_        = false;
    bool sroa_report_ = false;
    // Dissolve only a struct with at most this many fields; 0 dissolves nothing (the null control).
    // A PLACEHOLDER default until the S4 measurement, exactly as the inliner shipped 48 nodes and
    // measured its way to 20 -- 4 covers the measured customer (demo/raytracer's 3-field Vec3)
    // without committing to anything wider.
    int  sroa_max_fields_ = 4;
    // Report counters, all still zero at S0 -- the transform lands in S1. Kept beside the switch so
    // the report line below has something to print from the first slice on.
    size_t sroa_permitted_ = 0;   // bindings the pre-pass judged dissolvable
    size_t sroa_seen_emit_ = 0;   // dissolutions emit actually took
    int    sroa_retries_   = 0;   // regions whose dissolutions the budget ladder revoked

    // The decision, keyed by the `let` node: name -> field count. Filled ONCE by a PRE-PASS before
    // any planning (decision D1), read by let_binding_count during the measure and by compile_stmt
    // during emit. It cannot be decided inside the measure the way the inliner's is:
    // let_binding_count is consulted to place the initializer's base BEFORE anything about that
    // `let` has been measured.
    std::unordered_map<const LetStmt*, int> sroa_lets_;
    // The `let`s this region's measure relied on, so the budget ladder can revoke exactly them --
    // the `sites_this_measure_` mirror. Erasing from sroa_lets_ (rather than leaving the master
    // switch off) is what keeps emit and the frozen plan in agreement.
    std::vector<const LetStmt*> sroa_this_measure_;

    // ONE exhaustive walker, TWO modes. Discovery (`name == nullptr`) finds every BlockExpr and
    // offers its `let`s to sroa_try_block; classify decides whether one binding's uses are all
    // field reads. Two walkers would be two places to forget the shorthand case -- `S { x }`
    // carries no Ident node at all, so an Ident-based scan misses that use completely.
    // `in_lambda` suppresses the `x.f` exemption: a lambda body is LIFTED into its own function,
    // where the binding arrives as a by-value CAPTURE -- emit_capture_source MOVs the whole value,
    // so even a field read there needs the object. Found by the dissolved-binding guard on the
    // first run of the probe, which is precisely what that guard is for.
    struct SroaCtx { const std::string* name = nullptr; bool escaped = false; bool in_lambda = false; };
    void sroa_walk_expr (const Expr& e, SroaCtx& c, const std::unordered_set<std::string>& bound);
    void sroa_walk_stmt (const Stmt& s, SroaCtx& c, std::unordered_set<std::string>& bound);
    void sroa_walk_block(const BlockExpr& b, SroaCtx& c, std::unordered_set<std::string> bound);
    // An assignment PLACE (`x`, `x.f`, `x[i].g`). Its leftmost Ident is written through, so it is
    // an escape even though `x.f` is the very shape that does NOT escape when read.
    void sroa_scan_target(const Expr& t, SroaCtx& c, const std::unordered_set<std::string>& bound);
    // Consider every `let` of ONE statement sequence. Two entry points because a block owns
    // `StmtPtr`s while the top level arrives as a flat list of borrowed pointers.
    void sroa_try_stmts(const std::vector<const Stmt*>& stmts);
    void sroa_try_block(const BlockExpr& b);
    void collect_sroa_lets(const Program& prog, const std::vector<const Stmt*>& top);
    // Emit: the fields of `sl` straight into `regs`, no NEW_STRUCT and no SET_PROP.
    void compile_into_dissolved(const StructLit& sl, const std::vector<int>& regs);
    // Produce `e`'s struct value into `regs`, one field per register, by whatever route works:
    // a literal, a block whose tail is one, an inline expansion whose body tail is one -- or, as
    // the universal fallback, materialise it once and read the fields out (which costs exactly
    // what the un-dissolved program paid, so an optimistic pre-pass can never make things worse).
    void compile_dissolved_or_explode(const Expr& e, const std::vector<int>& regs);
    void compile_block_dissolved(const BlockExpr& b, const std::vector<int>& regs);
    bool sroa_producer_ok(const InlineTarget& t, size_t nfields) const;
    // How many LOCALS parameter `i` will take. An upper bound: the measure reserves the dissolved
    // width whenever it COULD apply, emit takes it only when the argument really is a dissolved
    // binding. Over-reserving is the safe direction (the plan is an upper bound by design).
    int  sroa_param_slots(const InlineTarget& t, size_t i, const Expr& arg) const;
    // Classify a call site during the escape scan: an occurrence of the binding at a FIELD-ONLY
    // parameter position is not an escape, because the expansion will bind that parameter to the
    // very same field registers. Returns true if it consumed the whole call.
    bool sroa_call_field_only(const CallExpr& cl, SroaCtx& c,
                              const std::unordered_set<std::string>& bound);

    // The dissolved destination offered to the NEXT expansion of `sroa_dst_site_`. Rather than
    // re-resolving which callee a call site expands -- the "decide twice" mistake that caused the
    // builtin-shadowing defect -- the destination is handed to `try_inline_call`, the ONE choke
    // point all four emit hooks already funnel through. Every decline (a caller local shadowing
    // the fn name, the depth cap, not a target at all) then falls through to the ordinary call on
    // its own, and the explode fallback picks the result up. One-shot: cleared when consumed, so
    // a nested expansion cannot claim a destination meant for its parent.
    const std::vector<int>* sroa_dst_      = nullptr;
    const Expr*             sroa_dst_site_ = nullptr;
    bool                    sroa_dst_used_ = false;
    // The scope entry for `name`, or null. Unlike local_reg this exposes the dissolved fields.
    const Local* local_slot(const std::string& name) const;
    // local_reg for a site that consumes the binding as a VALUE. A dissolved binding has no such
    // register, and the escape scan is supposed to have made every one of these unreachable --
    // so this says so out loud instead of quietly handing back field 0.
    int local_value_reg(const std::string& name, uint32_t line = 0, uint32_t col = 0) const;
    // Rebuild the object a dissolved binding stands for, into a fresh temp.
    int materialize_dissolved(const Local& l, const TyPtr& ty, uint32_t line, uint32_t col);
};

} // namespace svc
