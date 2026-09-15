// =============================================================================
// Codegen.cpp -- typed AST -> bytecode lowering for Skarn (svc).
//
// Deliverable 1 (P0 + P1): the "main" region only -- top-level statements,
// literals, arithmetic/bitwise, comparisons, bool logic + short-circuit, unary
// ops, `if`/`while`/blocks-as-expressions, `let`/`mut`/assign + scoping. Lowering
// reads the checker's Expr::ty to pick the typed path (`*_INT` vs. promoting
// double, the I2D Int->Double widening). See Codegen.h for the register model.
// =============================================================================

#include "Codegen.h"
#include "TypeUniverse.h"   // BUILTIN_COUNT + TID_* dense-id layout (shared with the VM)
#include "NativeRegistry.h" // native_id_of / native_return_of -- route + wrap native calls

#include <cassert>
#include <algorithm>  // sort -- the inline drift report
#include <iostream>   // the inline drift report (set_inline_report)
#include <string>
#include <vector>

namespace svc {

namespace {
inline int imax(int a, int b) { return a > b ? a : b; }

// GET_KIND of a KIND_STRING heap object -- the discriminator a native Result uses (a native never
// returns a String on success, so kind==STRING marks the Err payload). Mirrors the dynamic side.
constexpr int64_t VM_KIND_STRING = 1;

// Syntactic `: Double` annotation? (used to spot a `let x: Double = <Int>` boundary).
bool annotation_is_double(const Type* t) {
    if (!t || t->kind != TypeKind::Named) return false;
    return static_cast<const NamedType*>(t)->name == "Double";
}

const char* expr_kind_name(ExprKind k) {
    switch (k) {
    case ExprKind::Pipe:      return "pipe `|>`";
    case ExprKind::Call:      return "function call";
    case ExprKind::Field:     return "field access";
    case ExprKind::Index:     return "index `a[i]`";
    case ExprKind::Try:       return "try `?`";
    case ExprKind::Match:     return "match";
    case ExprKind::For:       return "for-loop";
    case ExprKind::Loop:      return "loop";
    case ExprKind::Break:     return "break";
    case ExprKind::Continue:  return "continue";
    case ExprKind::Return:    return "return";
    case ExprKind::Lambda:    return "lambda";
    case ExprKind::StructLit: return "struct literal";
    case ExprKind::Tuple:     return "tuple";
    case ExprKind::ListLit:   return "list literal";
    case ExprKind::MapLit:    return "map literal";
    default:                  return "expression";
    }
}

// Extra concurrent scratch temps a builtin needs BEYOND result(1) + held args (need_call), for the
// Option-wrapping builtins that hold scratch while building Some/None. `get` takes the MAX over its two
// lowerings: the Map path holds a MAP_HAS Bool + MAP_GET value (+2); the sequence path holds a LEN + a
// zero const + the ARRAY_GET value (+3) -- so +3 covers both (the map path slightly over-reserves).
int builtin_extra_slots(const Expr* callee) {
    if (callee && callee->kind == ExprKind::Ident) {
        const std::string& n = static_cast<const IdentExpr&>(*callee).name;
        if (n == "get") return 3;   // seq path: LEN + zero const + ARRAY_GET value (>= the map path's +2)
        // pop holds THREE scratch temps concurrently -- LEN Int + VEC_POP value + zero const (rlen/rv/rz)
        // -- ABOVE the result AND the argument register, symmetric with `get`'s +3. A bare-local argument
        // hides the count (the arg is not a temp, so need_call's held slot is slack and +2 sufficed); a
        // field/call argument materializes a real temp that the scratch stacks on top of, so +2 under-
        // measured the frame by one. (Regression: pop_field_option.)
        if (n == "pop") return 3;
    }
    return 0;
}

// Extract the element Values of a const-array literal for the pool. The checker guarantees `lst`
// is `Array[T]` with T scalar (Int/Double/Bool) and every element a scalar literal (possibly a
// negated numeric literal), so this is a straight fold. A numeric literal in a Double array is
// widened to a double (the Int<:Double element coercion the checker already accepted).
std::vector<Value> const_array_values(const ListLit& lst) {
    const TyPtr elem = (lst.ty && lst.ty->kind == TyKind::Named && lst.ty->args.size() == 1)
                           ? lst.ty->args[0] : nullptr;
    const bool dbl = elem && elem->kind == TyKind::Double;
    const bool bln = elem && elem->kind == TyKind::Bool;
    std::vector<Value> out;
    out.reserve(lst.elems.size());
    for (const auto& elp : lst.elems) {
        const Expr* e = elp.get();
        bool neg = false;
        if (e->kind == ExprKind::Unary) {
            const auto& u = static_cast<const UnaryExpr&>(*e);
            if (u.op == TokKind::Minus && u.operand) { e = u.operand.get(); neg = true; }
        }
        if (bln) {
            out.push_back(Value::fromBool(static_cast<const BoolLit&>(*e).value));
        } else if (dbl) {
            double d = (e->kind == ExprKind::DoubleLit)
                           ? static_cast<const DoubleLit&>(*e).value
                           : static_cast<double>(static_cast<const IntLit&>(*e).value);
            out.push_back(Value::fromDouble(neg ? -d : d));
        } else {
            int64_t v = static_cast<const IntLit&>(*e).value;
            out.push_back(Value::fromSigned48(neg ? -v : v));
        }
    }
    return out;
}
} // namespace

// ----- type-directed helpers ------------------------------------------------

bool Codegen::is_int_ty(const TyPtr& t)    { return t && t->kind == TyKind::Int; }
bool Codegen::is_double_ty(const TyPtr& t) { return t && t->kind == TyKind::Double; }
bool Codegen::is_num_ty(const TyPtr& t) {
    return t && (t->kind == TyKind::Int || t->kind == TyKind::Double);
}
bool Codegen::is_string_ty(const TyPtr& t) { return t && t->kind == TyKind::String; }

OpCode Codegen::arith_opcode(TokKind op, bool dbl) {
    switch (op) {
    case TokKind::Plus:    return dbl ? OpCode::ADD : OpCode::ADD_INT;
    case TokKind::Minus:   return dbl ? OpCode::SUB : OpCode::SUB_INT;
    case TokKind::Star:    return dbl ? OpCode::MUL : OpCode::MUL_INT;
    case TokKind::Slash:   return dbl ? OpCode::DIV : OpCode::DIV_INT;
    case TokKind::Percent: return dbl ? OpCode::MOD : OpCode::MOD_INT;
    case TokKind::BitAnd:  return OpCode::AND_INT;
    case TokKind::BitOr:   return OpCode::OR_INT;
    case TokKind::BitXor:  return OpCode::XOR_INT;
    case TokKind::Shl:     return OpCode::SHL_INT;
    case TokKind::Shr:     return OpCode::SHR_INT;
    case TokKind::UShr:    return OpCode::USHR_INT;
    default:
        throw CodegenError("unsupported binary operator");
    }
}

static bool is_comparison(TokKind op) {
    switch (op) {
    case TokKind::EqEq: case TokKind::NotEq:
    case TokKind::Lt:   case TokKind::Le:
    case TokKind::Gt:   case TokKind::Ge: return true;
    default: return false;
    }
}

// ----- register allocator ---------------------------------------------------

int Codegen::alloc_temp() {
    // Emitter self-check: a temp handed out at or above frame_size_ means the measure_* walk
    // under-measured this frame's `.temp` (the temp would land in GC-invisible registers). Fire
    // here, at the emit site, rather than downstream in validate_frames.
    if (temp_top_ >= frame_size_)
        throw CodegenError("internal: temporary register budget exceeded in '" + emit_region_name_ +
                           "' -- frame under-measured (the measure_* walk is missing an AST shape)");
    if (temp_top_ >= 64)   // belt-and-suspenders (unreachable while frame_size_ <= 63)
        throw CodegenError("register budget exceeded (too many live temporaries)");
    int r = temp_top_++;
    emit_peak_temp_ = imax(emit_peak_temp_, temp_top_ - temp_base_);
    return r;
}

int Codegen::alloc_local() {
    // Emitter self-check: a local handed out at or above temp_base_ means the measure_* walk
    // under-measured this frame's `.local_peak` -- the local would collide with the temp stack (a
    // class validate_frames cannot see, since both regions sit below frame_size_).
    int r = n_locals_++;
    if (r >= temp_base_)
        throw CodegenError("internal: local register budget exceeded in '" + emit_region_name_ +
                           "' -- frame under-measured (the measure_* walk is missing an AST shape)");
    emit_peak_locals_ = imax(emit_peak_locals_, n_locals_);
    return r;
}

void Codegen::free_if_temp(int reg) {
    // LIFO discipline: only the current top temp is freed here; our lowering always
    // frees in reverse allocation order, so a non-top free is a no-op (freed later).
    if (reg >= temp_base_ && reg == temp_top_ - 1) --temp_top_;
}

int Codegen::local_reg(const std::string& name) const {
    for (auto it = scope_.rbegin(); it != scope_.rend(); ++it)
        if (it->name == name) return it->reg;
    return -1;
}

// Rebuild a dissolved binding's object into a fresh temp: exactly the NEW_STRUCT + SET_PROP the
// un-dissolved program would have emitted at the `let`, moved to the point that needs it. Costs
// one temp, which the ordinary Ident measure already reserves.
int Codegen::materialize_dissolved(const Local& l, const TyPtr& ty, uint32_t line, uint32_t col) {
    const std::string sn = struct_name_of(ty);
    const StructDef* sd = sn.empty() ? nullptr : find_struct(sn);
    if (!sd || sd->field_names.size() != l.fields.size())
        throw CodegenError("internal: cannot rebuild dissolved binding '" + l.name +
                           "' -- its type is not the struct it was dissolved as", line, col);
    const int rd = alloc_temp();
    as_.NEW_STRUCT(static_cast<uint8_t>(rd), sd->id);
    for (size_t i = 0; i < l.fields.size(); ++i)
        as_.SET_PROP(static_cast<uint8_t>(rd), static_cast<uint16_t>(i),
                     static_cast<uint8_t>(l.fields[i]));
    return rd;
}

const Codegen::Local* Codegen::local_slot(const std::string& name) const {
    for (auto it = scope_.rbegin(); it != scope_.rend(); ++it)
        if (it->name == name) return &*it;
    return nullptr;
}

// local_reg for a site that consumes the binding as a VALUE. A dissolved binding has no register
// holding the object, and the escape scan is supposed to have made every one of these unreachable;
// this says so out loud instead of quietly handing back field 0 and producing a wrong value with
// no diagnostic -- the failure mode this project treats as the dangerous one.
int Codegen::local_value_reg(const std::string& name, uint32_t line, uint32_t col) const {
    const Local* l = local_slot(name);
    if (!l) return -1;
    if (!l->fields.empty())
        throw CodegenError("internal: '" + name + "' is a dissolved (SROA) binding and has no "
                           "single register -- the escape scan should have rejected this use",
                           line, col);
    return l->reg;
}

std::string Codegen::fresh_label(const char* prefix) {
    return std::string(prefix) + std::to_string(label_seq_++);
}

// ----- measure pass ---------------------------------------------------------

int Codegen::let_binding_count(const LetStmt& l) {
    if (!l.pat) return 0;
    if (l.pat->kind == PatKind::Ident) {
        // A DISSOLVED binding occupies one local per FIELD instead of one. This single return is
        // the whole structural hook SROA needs in the measure: measure_stmt places the
        // initializer's base above it and measure_stmts advances the block watermark by it, so
        // both stay right for free. The temp side needs nothing -- the ordinary struct-literal
        // model already dominates the dissolved one (it additionally holds the NEW_STRUCT result),
        // and the measure is a deliberate upper bound.
        if (sroa_) {
            auto it = sroa_lets_.find(&l);
            if (it != sroa_lets_.end()) { sroa_this_measure_.push_back(&l); return it->second; }
        }
        return 1;
    }
    if (l.pat->kind == PatKind::Wildcard) return 0;   // `let _` binds nothing
    return measure_pattern(*l.pat).binding_locals;    // destructuring: leaf-ident bindings
}

Codegen::Need Codegen::measure_coerced(const Expr& e, int base, bool want_double) {
    // Mirrors compile_coerced on the `.temp` field: a non-temp Int (temp 0) widened to Double needs
    // a fresh I2D temp; an IntLit (temp 1) folds to a double-const temp; a value already in a temp is
    // widened in place -- both already counted. Locals are unaffected by the coercion.
    // RAW, for the same reason compile_coerced compiles RAW: this site performs the widening itself,
    // so measuring through the honoring wrapper would count the same I2D twice.
    Need n = measure_expr_raw(e, base);
    if (want_double && is_int_ty(e.ty) && n.temp == 0) n.temp = 1;
    return n;
}

// The MEASURE half of the `Expr::widen_double` mechanism -- the exact mirror of `compile_expr`'s
// wrapper below, and the reason a widened node never trips the emitter self-check: the coercion may
// need one fresh temp (an Int sitting in a LOCAL cannot be widened in place), so a node the checker
// marked reserves at least one.
Codegen::Need Codegen::measure_expr(const Expr& e, int base) {
    Need n = measure_expr_raw(e, base);
    if (e.widen_double && n.temp == 0) n.temp = 1;
    return n;
}

// The memoized entry to the structural walk below (see measure_cache_ in Codegen.h). Every shape
// builds `.local_peak` from `base` plus constants and `.temp` without `base`, so an answer recorded at
// one base is exact at any other. That assumption is checked here, per node, the first time the node
// is measured: its own formula is re-derived one register higher -- its children now answer from the
// memo -- and must give the same temps and a local peak shifted by exactly one. By induction over the
// tree every cached answer is then the one the uncached walk would have produced. The check costs one
// extra step per node, and a NEW shape that threads `base` any other way fails here, not as a frame
// under-measure found later.
Codegen::Need Codegen::measure_expr_raw(const Expr& e, int base) {
    const MeasureKey key{ &e, inline_depth_, &e == measure_skip_call_ };
    if (&e != measure_bypass_)
        if (auto it = measure_cache_.find(key); it != measure_cache_.end())
            return { it->second.temp, base + it->second.local_peak };
    const Need n = measure_expr_raw_uncached(e, base);
    const Expr* saved_bypass = measure_bypass_;
    measure_bypass_ = &e;
    const Need shifted = measure_expr_raw_uncached(e, base + 1);
    measure_bypass_ = saved_bypass;
    if (shifted.temp != n.temp || shifted.local_peak != n.local_peak + 1)
        throw CodegenError("internal: the frame measure of this expression depends on its base register "
                           "other than by translation (expression kind " +
                           std::to_string(static_cast<int>(e.kind)) + ") -- it cannot be memoized",
                           e.line, e.col);
    measure_cache_[key] = { n.temp, n.local_peak - base };
    return n;
}

// One structural walk yielding BOTH quantities: `.temp` (peak concurrent temporaries, computed
// exactly as the former need_expr did -- position-independent) and `.local_peak` (peak named locals,
// computed exactly as the former peak_locals_expr did -- threading `base`). Where the two former
// walkers recursed differently on the same node, both shapes are preserved here. Reached only through
// the memo above; recursion goes back through measure_expr / measure_coerced, never here directly.
Codegen::Need Codegen::measure_expr_raw_uncached(const Expr& e, int base) {
    switch (e.kind) {
    case ExprKind::IntLit: case ExprKind::DoubleLit: case ExprKind::BoolLit:
    case ExprKind::StrLit:
        return { 1, base };
    case ExprKind::Ident: {
        // A local needs no temp; a bare fn name -> LOAD_FN, a bare nullary ctor -> NEW_STRUCT,
        // a captured free variable (inside the lambda being measured) -> LOAD_CAPTURE (1 temp).
        const auto& id = static_cast<const IdentExpr&>(e);
        const auto& nm = id.name;
        // A const-ident inlines a single literal (1 temp), like a fn value / nullary ctor / capture.
        const bool held = user_fn_ref(id) || is_ctor_name(nm) || is_const_name(nm) ||
                          current_lambda_captures_.count(nm);
        return { held ? 1 : 0, base };
    }
    case ExprKind::Unary: {
        const auto& u = static_cast<const UnaryExpr&>(e);
        Need o = measure_expr(*u.operand, base);
        const int t = (u.op == TokKind::Tilde) ? imax(o.temp, o.temp > 0 ? 2 : 1)   // ~x = x XOR -1
                                               : imax(1, o.temp);                    // NEG / NOT_BOOL reuse the operand
        return { t, o.local_peak };
    }
    case ExprKind::Binary: {
        const auto& b = static_cast<const BinaryExpr&>(e);
        // A Double-result arithmetic widens each operand via compile_coerced (an Int operand may
        // need an extra I2D temp); the Bool short-circuits never widen (dbl=false). A String `+`
        // stringifies a non-String operand via TO_STRING (a temp when the operand has none of its
        // own -- mirrors measure_coerced's I2D +1).
        const bool str = b.op == TokKind::Plus && is_string_ty(b.ty);
        const bool dbl = !str && is_double_ty(b.ty);
        Need a = measure_coerced(*b.lhs, base, dbl);
        Need c = measure_coerced(*b.rhs, base, dbl);
        if (str) {
            if (!is_string_ty(b.lhs->ty) && a.temp == 0) a.temp = 1;
            if (!is_string_ty(b.rhs->ty) && c.temp == 0) c.temp = 1;
        }
        int t;
        if (b.op == TokKind::AndAnd || b.op == TokKind::OrOr)
            t = imax(imax(a.temp, 1), 1 + c.temp);              // Bool result held across the RHS
        else
            t = imax(a.temp, imax((a.temp > 0 ? 1 : 0) + c.temp, 1));
        return { t, imax(a.local_peak, c.local_peak) };
    }
    case ExprKind::If: {
        const auto& f = static_cast<const IfExpr&>(e);
        Need c = measure_expr(*f.cond, base);
        Need th = measure_expr(*f.then_blk, base);
        int t = imax(c.temp, th.temp), lp = imax(c.local_peak, th.local_peak);
        if (f.else_blk) { Need el = measure_expr(*f.else_blk, base); t = imax(t, el.temp); lp = imax(lp, el.local_peak); }
        return { 1 + t, lp };                                   // + the held result temp
    }
    case ExprKind::While: {
        const auto& w = static_cast<const WhileExpr&>(e);
        Need c = measure_expr(*w.cond, base);
        int t = c.temp, lp = c.local_peak;
        if (w.body) { Need bd = measure_expr(*w.body, base); t = imax(t, bd.temp); lp = imax(lp, bd.local_peak); }
        return { imax(t, 1), lp };                              // + the trailing nil temp
    }
    case ExprKind::Loop: {
        // compile_loop allocates the result temp FIRST (a `break value` MOVs into it), so it is
        // held across the whole body -- exactly like If's held result temp.
        const auto& l = static_cast<const LoopExpr&>(e);
        int t = 0, lp = base;
        if (l.body) { Need bd = measure_expr(*l.body, base); t = bd.temp; lp = bd.local_peak; }
        return { 1 + t, lp };
    }
    case ExprKind::Block:
        return measure_block(static_cast<const BlockExpr&>(e), base);
    case ExprKind::Call: {
        const auto& c = static_cast<const CallExpr&>(e);
        std::vector<const Expr*> args;
        for (const auto& a : c.args) args.push_back(a.get());
        // Inlining: the ONE place a call site is judged expandable. Recorded
        // here, in the measure, because the frame this walk computes must already contain the
        // expansion -- emit looks the decision up and never re-derives it.
        // The frame must hold the expansion OR the ordinary call, because a PERMISSION is not a
        // promise: emit declines a tail-position site (TCO wins there), and an ordinary call is
        // not always the cheaper of the two -- it holds every argument in a temp simultaneously,
        // while an expansion moves each into a local as it goes. So both models are measured and
        // the MAX is reserved. Depth 1 (D2): a call inside an already-expanded body is never
        // expanded, and emit enforces that independently -- this node may also carry a permission
        // earned while the enclosing callee was measured as its OWN function, at depth 0.
        Need inl{ 0, base };
        if (inlining_ && inline_depth_ < inline_max_depth_ && &e != measure_skip_call_) {
            // SEQUENCING, not style: direct_call_target writes its out-parameter, which
            // inline_target_for then reads. As two arguments of ONE call the order is unspecified
            // and MSVC evaluates the second first -- every site then looked like arity 0, so the
            // inliner silently did nothing at all. The named temporaries sequence the two.
            std::vector<const Expr*> margs;
            const std::string target = direct_call_target(c.callee.get(), args, margs);
            if (const InlineTarget* t = inline_target_for(target, margs.size())) {
                inline_sites_.insert(&e);
                sites_this_measure_.push_back(&e);
                inl = measure_inline_site(*t, margs, base);
            }
        }
        // Counted unconditionally, for the same reason: a permitted site emit declines still
        // opens an outgoing window at frame_size (plan_fits).
        measure_max_call_args_ = imax(measure_max_call_args_, static_cast<int>(args.size()));
        int lp = base;   // a call's argument LOCALS: imax over callee + args (peak_locals_expr's Call)
        if (c.callee) lp = imax(lp, measure_expr(*c.callee, base).local_peak);
        for (const auto& a : c.args) lp = imax(lp, measure_expr(*a, base).local_peak);
        int t;
        if (!c.callee) t = 1;
        // A dynamic trait dispatch holds the receiver + rest args (need_call); the +1 once held the
        // PROTO_RESOLVE Func, which the fused RESOLVE_CALL no longer needs, so for a trait call it is now
        // pure over-reservation. A container builtin holds its result temp + args similarly. Both a safe
        // upper bound (a devirtualized / direct call needs one fewer).
        else if (is_qualified_method_call(c.callee.get()) || is_trait_call(c.callee.get()) ||
                 is_builtin_callee(c.callee.get()))
            t = 1 + need_call(args) + builtin_extra_slots(c.callee.get());
        else if (is_native_callee(c.callee.get()))
            t = native_call_need(args, c.callee.get());
        // Otherwise a safe upper bound: measure every call as an INDIRECT call (a direct CALL needs
        // no callee temp, so it needs one fewer -- over-reserving is harmless).
        else
            t = need_indirect_call(*c.callee, args);
        // An argument the checker marked for Int->Double widening needs no correction here: the
        // need_* family above reads `measure_expr`, the honoring wrapper, which already reserves the
        // I2D temp for it.
        return { imax(t, inl.temp), imax(lp, inl.local_peak) };
    }
    case ExprKind::Index: {
        // The object is held (1) while the index is computed; ARRAY_GET reuses a temp.
        const auto& ix = static_cast<const IndexExpr&>(e);
        Need o = measure_expr(*ix.obj, base);
        Need i = measure_expr(*ix.index, base);
        return { imax(o.temp, 1 + i.temp), imax(o.local_peak, i.local_peak) };
    }
    case ExprKind::Pipe: {
        const auto& p = static_cast<const PipeExpr&>(e);
        std::vector<const Expr*> args;
        const Expr* callee = pipe_call_shape(p, args);
        Need inl{ 0, base };                                   // both models -- see the Call case
        if (inlining_ && inline_depth_ < inline_max_depth_) {
            std::vector<const Expr*> margs;                    // sequenced -- see the Call case
            const std::string target = direct_call_target(callee, args, margs);
            if (const InlineTarget* t = inline_target_for(target, margs.size())) {
                inline_sites_.insert(&e);
                sites_this_measure_.push_back(&e);
                inl = measure_inline_site(*t, margs, base);
            }
        }
        measure_max_call_args_ = imax(measure_max_call_args_, static_cast<int>(args.size()));
        Need l = measure_expr(*p.lhs, base);   // peak_locals_expr(Pipe) always recurses lhs + rhs
        // The rhs of `x |> f(a)` is a CallExpr that emit never visits as a call -- suppress the
        // phantom inline site while descending it (see measure_skip_call_).
        const Expr* saved_skip = measure_skip_call_;
        measure_skip_call_ = p.rhs.get();
        Need r = measure_expr(*p.rhs, base);
        measure_skip_call_ = saved_skip;
        const int lp = imax(l.local_peak, r.local_peak);
        int t;
        if (callee && (is_qualified_method_call(callee) || is_trait_call(callee) ||
                       is_builtin_callee(callee)))
            t = 1 + need_call(args) + builtin_extra_slots(callee);
        else if (callee && is_native_callee(callee))
            t = native_call_need(args, callee);
        else if (callee)
            t = need_indirect_call(*callee, args);
        else
            t = imax(l.temp, r.temp);
        return { imax(t, inl.temp), imax(lp, inl.local_peak) };
    }
    case ExprKind::Return: {
        const auto& r = static_cast<const ReturnExpr&>(e);
        if (!r.value) return { 1, base };
        Need v = measure_expr(*r.value, base);
        return { v.temp, v.local_peak };
    }
    case ExprKind::StructLit: {
        const auto& sl = static_cast<const StructLit&>(e);
        const StructDef* sd = find_struct(sl.name);
        const int held = sl.base ? 2 : 1;   // NEW_STRUCT result, plus the base temp held while copying
        int t = held, lp = base;
        for (const auto& fi : sl.fields) {
            bool fd = false;   // a Double field widens an Int initializer (compile_coerced -> +1 I2D)
            if (sd) { const int slot = field_slot(*sd, fi.name); if (slot >= 0) fd = sd->field_double[slot] != 0; }
            if (fi.value) { Need v = measure_coerced(*fi.value, base, fd); t = imax(t, held + v.temp); lp = imax(lp, v.local_peak); }
            else          t = imax(t, held);
        }
        if (sl.base) {                                     // record update: base compile + copied slots
            Need vb = measure_expr(*sl.base, base);        // base compiled with the result temp held (+1)
            t = imax(t, 1 + vb.temp); lp = imax(lp, vb.local_peak);
            t = imax(t, 3);                                // a copied slot holds rd + rb + one GET_PROP temp
        }
        return { t, lp };
    }
    case ExprKind::Tuple: {
        const auto& tp = static_cast<const TupleExpr&>(e);
        int t = 1, lp = base;
        for (const auto& x : tp.elems) { Need v = measure_expr(*x, base); t = imax(t, 1 + v.temp); lp = imax(lp, v.local_peak); }
        return { t, lp };
    }
    case ExprKind::Field: {
        Need o = measure_expr(*static_cast<const FieldExpr&>(e).obj, base);
        return { imax(o.temp, 1), o.local_peak };   // GET_PROP reuses the operand temp
    }
    case ExprKind::ListLit: {
        const auto& ll = static_cast<const ListLit&>(e);
        int t = 1, lp = base;                        // the acc temp (list built so far)
        for (const auto& x : ll.elems) { Need v = measure_expr(*x, base); t = imax(t, 2 + v.temp); lp = imax(lp, v.local_peak); }   // acc + cell + head
        return { t, lp };
    }
    case ExprKind::MapLit: {
        const auto& ml = static_cast<const MapLit&>(e);
        int t = 1, lp = base;                        // the map temp
        for (const auto& kv : ml.entries) {
            Need k = measure_expr(*kv.first, base);
            Need v = measure_expr(*kv.second, base);
            t = imax(t, 1 + imax(k.temp, (k.temp > 0 ? 1 : 0) + v.temp));
            lp = imax(lp, imax(k.local_peak, v.local_peak));
        }
        return { t, lp };
    }
    case ExprKind::For: {
        const auto& f = static_cast<const ForExpr&>(e);
        const TyPtr& ity = f.iter->ty;
        const std::string kind = (ity && ity->kind == TyKind::Named) ? ity->name : std::string();
        const int nvars = for_pattern_var_count(*f.pat);
        // Map: mapl/cur/found/zero + vars (no-copy live cursor); Array/Vec/Bytes & Iterable protocol:
        // arr/len/idx + vars; lazy Iterator/IntoIterator: cursor + opt + vars (`opt` is a LOCAL so the
        // rotated bottom-guard's Option survives the check->top edge); List: cursor + vars.
        const bool arrlike_m = (kind == "Array" || kind == "Vec" || kind == "Bytes");
        const bool lazy_iter = !arrlike_m && kind != "Map" && kind != "List" &&
                               iterator_protocol(ity) != IterProto::None;
        const int loop_locals =
            (kind == "Map")            ? 4 + nvars :
            arrlike_m                  ? 3 + nvars :
            lazy_iter                  ? 2 + nvars :
            iter_protocol_applies(ity) ? 3 + nvars :
                                         1 + nvars;
        // The iterable is compiled INTO a loop local: compile_for allocates the loop-var slots + the
        // destination local (cur / mapl / arr) FIRST, then `compile_into(iter, dst)`, so the
        // iterable's own transient locals sit ABOVE those. Measure it at that offset, not at `base`,
        // or a `for` whose CONTAINER expression needs a local of its own (a match/if element) is
        // under-counted by nvars+1. Under the Iterable protocol the iterable is instead the argument
        // of the `iter()` trait call, emitted after arr/len/idx (3) are allocated -> nvars+3.
        const bool arrlike = (kind == "Array" || kind == "Vec" || kind == "Bytes");
        const int pre_iter = (!arrlike && kind != "List" && kind != "Map" && iter_protocol_applies(ity))
                                 ? nvars + 3 : nvars + 1;
        Need iter = measure_expr(*f.iter, base + pre_iter);
        const int inner = base + loop_locals;
        int lp = imax(iter.local_peak, inner);
        int body_temp = 1;   // no body -> the need side contributes 1
        if (f.body) { Need bd = measure_expr(*f.body, inner); body_temp = bd.temp; lp = imax(lp, bd.local_peak); }
        // Loop bookkeeping temps (list test / map pair build / index bump) + iter + body.
        int t = imax(imax(iter.temp, body_temp), 3);
        if (iter_protocol_applies(ity) || iterator_protocol(ity) == IterProto::IntoIterator)
            t = imax(t, iter.temp + 4);   // the `iter(it)`/`intoIter(it)` trait call: recv + window + result, +1 kept from the former PROTO_RESOLVE Func
        // The lazy-iterator branch's per-iteration temps (Option result + None-test consts) fit the
        // base bound of 3; the `next()` dispatch reuses the outgoing window (validate_frames covers it).
        // A nested destructuring pattern holds the element across the bind + nested field reads: 1
        // (element) + the pattern's test-temp bound (a safe upper bound -- the binder emits no tests).
        if (f.pat && f.pat->kind != PatKind::Ident && f.pat->kind != PatKind::Wildcard)
            t = imax(t, 1 + measure_pattern(*f.pat).test_temps);
        return { t, lp };
    }
    case ExprKind::Match: {
        const auto& m = static_cast<const MatchExpr&>(e);
        const int sbase = base + 1;                  // the scrutinee local
        // compile_match allocates the scrutinee local FIRST, then compiles the scrutinee INTO
        // it -- so a scrutinee with its own locals (e.g. a nested `match`) sits ABOVE `sbase`.
        // Measure it at `sbase`, not `base` (mirrors LetStmt, whose binding slot is likewise
        // allocated before its initializer is compiled). Measuring at `base` under-counts a
        // match-scrutinee-is-a-match by one local per nesting level.
        Need scrut = measure_expr(*m.scrut, sbase);
        int lp = imax(scrut.local_peak, sbase);
        int arm_temp = 0;
        for (const auto& arm : m.arms) {
            PatNeed pn = measure_pattern(*arm.pat);
            int a = pn.test_temps;
            const int abase = sbase + pn.binding_locals;
            lp = imax(lp, abase);
            if (arm.guard) { Need g = measure_expr(*arm.guard, abase); a = imax(a, g.temp); lp = imax(lp, g.local_peak); }
            if (arm.body)  { Need bd = measure_expr(*arm.body, abase); a = imax(a, bd.temp); lp = imax(lp, bd.local_peak); }
            arm_temp = imax(arm_temp, a);
        }
        return { imax(scrut.temp, 1 + arm_temp), lp };   // the result temp is held across the arms
    }
    case ExprKind::Try: {
        // The scrutinee (1) is held while the type-id + compare temps (2) are live; the payload
        // reuses the scrutinee's slot. Cover the operand's own need too.
        const auto& t = static_cast<const TryExpr&>(e);
        Need o = measure_expr(*t.operand, base);
        return { imax(o.temp, 3), o.local_peak };
    }
    case ExprKind::Lambda: {
        // Capture-free -> LOAD_FN (1 temp). Capturing -> a contiguous window of `ncaptures` temps
        // that MAKE_CLOSURE reads (the closure result reuses the base, so the peak == ncaptures). A
        // lifted lambda has its OWN frame, so it contributes no enclosing locals (local_peak = base).
        const auto& lam = static_cast<const LambdaExpr&>(e);
        auto it = lambda_captures_.find(&lam);
        const int ncap = (it != lambda_captures_.end()) ? static_cast<int>(it->second.size()) : 0;
        return { ncap > 0 ? ncap : 1, base };
    }
    case ExprKind::Break: {
        // A jump; the dummy value is never read. `break value` first computes the value (its own
        // temps) and MOVs it into the enclosing loop's already-allocated result register.
        const auto& br = static_cast<const BreakExpr&>(e);
        if (!br.value) return { 1, base };
        Need v = measure_expr(*br.value, base);
        return { imax(1, v.temp), v.local_peak };
    }
    case ExprKind::Continue:
        return { 1, base };   // a jump; the dummy value is never read
    default:
        return { 1, base };   // unsupported here; emit throws before the frame size is used
    }
}

Codegen::Need Codegen::measure_block(const BlockExpr& b, int base) {
    std::vector<const Stmt*> ss;
    ss.reserve(b.stmts.size());
    for (const auto& s : b.stmts) ss.push_back(s.get());
    return measure_stmts(ss, base);
}

int Codegen::need_call(const std::vector<const Expr*>& args) {
    // Safe upper bound covering BOTH call paths. Non-tail (compile_call) holds only the arguments
    // that materialize into a temp. The TAIL path (emit_tail_direct/indirect_call) is the worst
    // case: it forces EVERY argument into a held temp -- even a bare local/param -- so the writeback
    // into the reused frame [0, nargs) cannot clobber a param that is still a source. So each arg
    // occupies >= 1 held slot while the remaining args are built; a later aggregate-literal arg
    // (tuple/struct needing its own temps) stacks ABOVE those. (Previously `held` was bumped only
    // for na>0 args, under-measuring a tail call like `f(a, a, (S{f:5}, "x"))` -- found by the
    // property generator, seed 48474.)
    int held = 0, peak = 1;
    for (const Expr* a : args) {
        const int na = measure_expr(*a, 0).temp;   // temp is position-independent -> base 0
        peak = imax(peak, held + imax(na, 1));
        ++held;                                    // every arg is held (tail-call staging worst case)
    }
    return imax(peak, imax(held, 1));
}

int Codegen::need_indirect_call(const Expr& callee, const std::vector<const Expr*>& args) {
    const int cneed = measure_expr(callee, 0).temp;   // temp is position-independent -> base 0
    const int ch    = cneed > 0 ? 1 : 0;   // the held callee register (a bare local needs none)
    return imax(cneed, ch + need_call(args));
}

Codegen::Need Codegen::measure_stmt(const Stmt& s, int base) {
    switch (s.kind) {
    case StmtKind::Let: {
        const auto& l = static_cast<const LetStmt&>(s);
        if (!l.init) return { 0, base };
        // The binding slot is allocated BEFORE the initializer is compiled (emit does
        // `reg = alloc_local()` then compiles the init), so nested locals in the init sit ABOVE
        // the binding slot -- measure the init at base + the binding count. A Double annotation
        // widens an Int init via compile_coerced (a non-temp Int -> +1 I2D temp).
        Need n = measure_coerced(*l.init, base + let_binding_count(l), annotation_is_double(l.type.get()));
        // A destructuring pattern (tuple / struct / tuple-struct) holds the scrutinee across the
        // binding: 1 (scrut) + the pattern's test-temp bound for nested field reads. This is a SAFE
        // upper bound -- the irrefutable binder uses fewer temps (it emits no discrimination).
        if (l.pat && l.pat->kind != PatKind::Ident && l.pat->kind != PatKind::Wildcard)
            n.temp = imax(n.temp, 1 + measure_pattern(*l.pat).test_temps);
        return n;
    }
    case StmtKind::Assign: {
        const auto& a = static_cast<const AssignStmt&>(s);
        // The target lvalue's ty is the slot type (var / element / field); an Int rvalue widens
        // into a Double slot via compile_coerced.
        const bool tdbl = a.target && is_double_ty(a.target->ty);
        Need v = measure_coerced(*a.value, base, tdbl);
        int t = v.temp, lp = v.local_peak;
        // A COMPOUND assignment holds one extra temp: the old value read out of the place, live
        // across the evaluation of the right-hand side. Under-counting it would let the rvalue's
        // temps overlap that register -- which the emitter self-check would catch, but only after
        // the plan and the emit had already disagreed.
        const int rmw = (a.op != TokKind::Assign) ? 1 : 0;
        if (a.target && a.target->kind == ExprKind::Index) {   // a[i] = v: obj + idx + value held
            const auto& ix = static_cast<const IndexExpr&>(*a.target);
            Need o = measure_expr(*ix.obj, base);
            Need i = measure_expr(*ix.index, base);
            t  = imax(imax(o.temp, 1 + i.temp), 2 + rmw + v.temp);
            lp = imax(lp, imax(o.local_peak, i.local_peak));
        } else if (a.target && a.target->kind == ExprKind::Field) {   // obj.f = v: obj + value held
            Need o = measure_expr(*static_cast<const FieldExpr&>(*a.target).obj, base);
            t  = imax(o.temp, 1 + rmw + v.temp);
            lp = imax(lp, o.local_peak);
        }
        return { t, lp };
    }
    case StmtKind::Expr:
        return measure_expr(*static_cast<const ExprStmt&>(s).expr, base);
    }
    return { 0, base };
}

Codegen::Need Codegen::measure_stmts(const std::vector<const Stmt*>& stmts, int base) {
    int t = 1;                    // at least the r0 result temp
    int running = base, lp = base;
    for (const Stmt* s : stmts) {
        Need sn = measure_stmt(*s, running);
        t  = imax(t, sn.temp);
        lp = imax(lp, sn.local_peak);
        if (s->kind == StmtKind::Let) {
            running += let_binding_count(static_cast<const LetStmt&>(*s));
            lp = imax(lp, running);   // the binding slot is live through the rest of the block
        }
    }
    return { t, lp };
}

// Does a measured plan fit the register file? TWO conditions, and the second is easy to forget:
// besides the 63-register frame itself, every call the region still makes opens an outgoing window
// at `frame_size`, and compile_call throws when `frame_size + nargs > 64`. That throw happens at
// EMIT time, long after the plan is frozen, with no way back -- and inlining is what raises
// `frame_size`. Without this a program that compiled before the inliner could abort with it.
bool Codegen::plan_fits(const Plan& p) const {
    return p.frame_size <= 63 && p.frame_size + imax(measure_max_call_args_, 1) <= 64;
}

// The register-budget LADDER: measure with both optimisations, and on a frame that does not fit,
// give one of them up and measure again -- SROA first, then inlining, per region, deterministic.
//
// SROA goes first because it is the smaller win (~4.7 % predicted, against the inliner's measured
// +8.7/+10.3 %): backing off the larger one first would silently trade it away for the smaller.
//
// The undo ERASES this region's entries from the decision maps rather than leaving a master switch
// off, and that distinction is load-bearing: emit runs later with both switches restored, so a
// decision still in the map would be taken by emit against a frame that was planned without it.
//
// Re-measure the whole region on each rung, never patch the previous plan: dissolving trades temps
// for locals, so undoing it can RAISE peak_temp while lowering n_locals -- the plan is not monotone
// in either direction.
Codegen::Plan Codegen::plan_with_inline_retry(const std::function<Plan()>& measure) {
    // The memo is cleared per run: a rung changes inlining_ / sroa_ / the decision maps, which the
    // memo's key deliberately does not carry.
    auto reset = [&] { sites_this_measure_.clear(); sroa_this_measure_.clear();
                       measure_max_call_args_ = 0; measure_cache_.clear(); };
    reset();
    Plan p = measure();
    if (plan_fits(p)) return p;

    if (sroa_ && !sroa_this_measure_.empty()) {          // rung 2: give up SROA, keep inlining
        ++sroa_retries_;
        for (const LetStmt* l : sroa_this_measure_) sroa_lets_.erase(l);
        reset();
        const bool saved = sroa_;
        sroa_ = false;                                   // also stops this pass re-recording
        p = measure();
        sroa_ = saved;
        if (plan_fits(p)) return p;
    }
    if (!inlining_) return p;                            // nothing left to give up
    ++inline_retries_;                                   // rung 3: give up inlining too
    for (const Expr* s : sites_this_measure_) inline_sites_.erase(s);
    reset();
    const bool saved_i = inlining_, saved_s = sroa_;
    inlining_ = false;
    sroa_     = false;
    p = measure();
    inlining_ = saved_i;
    sroa_     = saved_s;
    return p;
}

Codegen::Plan Codegen::plan_top_level(const std::vector<const Stmt*>& stmts) {
    return plan_with_inline_retry([&] {
        Need n = measure_stmts(stmts, 0);
        Plan p;
        p.n_locals   = n.local_peak;
        p.peak_temp  = n.temp;
        p.frame_size = imax(1, p.n_locals + p.peak_temp);
        return p;
    });
}

const Expr* Codegen::pipe_call_shape(const PipeExpr& p, std::vector<const Expr*>& args) const {
    args.clear();
    args.push_back(p.lhs.get());
    if (p.rhs->kind == ExprKind::Ident) return p.rhs.get();      // `x |> f`
    if (p.rhs->kind == ExprKind::Call) {                          // `x |> f(a, ...)`
        const auto& rc = static_cast<const CallExpr&>(*p.rhs);
        if (rc.callee && rc.callee->kind == ExprKind::Ident) {
            for (const auto& a : rc.args) args.push_back(a.get());
            return rc.callee.get();
        }
    }
    return nullptr;
}

bool Codegen::has_tail_self_call(const Expr& e, const std::string& name, int nparams) const {
    switch (e.kind) {
    case ExprKind::Block: {
        const auto& b = static_cast<const BlockExpr&>(e);
        if (b.stmts.empty() || b.stmts.back()->kind != StmtKind::Expr) return false;
        const auto& le = static_cast<const ExprStmt&>(*b.stmts.back()).expr;
        return le && has_tail_self_call(*le, name, nparams);
    }
    case ExprKind::If: {
        const auto& f = static_cast<const IfExpr&>(e);
        return (f.then_blk && has_tail_self_call(*f.then_blk, name, nparams)) ||
               (f.else_blk && has_tail_self_call(*f.else_blk, name, nparams));
    }
    case ExprKind::Match: {
        const auto& m = static_cast<const MatchExpr&>(e);
        for (const auto& arm : m.arms)
            if (arm.body && has_tail_self_call(*arm.body, name, nparams)) return true;
        return false;
    }
    case ExprKind::Call: {
        const auto& c = static_cast<const CallExpr&>(e);
        return c.callee && c.callee->kind == ExprKind::Ident &&
               static_cast<const IdentExpr&>(*c.callee).qualifier.empty() &&
               static_cast<const IdentExpr&>(*c.callee).name == name &&
               static_cast<int>(c.args.size()) == nparams;
    }
    case ExprKind::Pipe: {
        std::vector<const Expr*> args;
        const Expr* callee = pipe_call_shape(static_cast<const PipeExpr&>(e), args);
        if (!callee || callee->kind != ExprKind::Ident) return false;
        const auto& id = static_cast<const IdentExpr&>(*callee);
        return id.qualifier.empty() && id.name == name &&
               static_cast<int>(args.size()) == nparams;
    }
    case ExprKind::Return: {
        const auto& r = static_cast<const ReturnExpr&>(e);
        return r.value && has_tail_self_call(*r.value, name, nparams);
    }
    default:
        return false;
    }
}

// Max nargs over NON-self tail calls (a self-call reserves separately). Covers both the
// direct-tail (needs nargs temps) and indirect-tail (needs nargs + 1) staging: plan_function
// reserves this value + 1, a safe upper bound for either path.
int Codegen::has_tail_nonself_call(const Expr& e, const std::string& self_name,
                                   int self_nparams) const {
    switch (e.kind) {
    case ExprKind::Block: {
        const auto& b = static_cast<const BlockExpr&>(e);
        if (b.stmts.empty() || b.stmts.back()->kind != StmtKind::Expr) return 0;
        const auto& le = static_cast<const ExprStmt&>(*b.stmts.back()).expr;
        return le ? has_tail_nonself_call(*le, self_name, self_nparams) : 0;
    }
    case ExprKind::If: {
        const auto& f = static_cast<const IfExpr&>(e);
        const int t  = f.then_blk ? has_tail_nonself_call(*f.then_blk, self_name, self_nparams) : 0;
        const int el = f.else_blk ? has_tail_nonself_call(*f.else_blk, self_name, self_nparams) : 0;
        return imax(t, el);
    }
    case ExprKind::Match: {
        const auto& m = static_cast<const MatchExpr&>(e);
        int mx = 0;
        for (const auto& arm : m.arms)
            if (arm.body) mx = imax(mx, has_tail_nonself_call(*arm.body, self_name, self_nparams));
        return mx;
    }
    case ExprKind::Call: {
        const auto& c = static_cast<const CallExpr&>(e);
        const bool is_self = c.callee && c.callee->kind == ExprKind::Ident &&
                             static_cast<const IdentExpr&>(*c.callee).qualifier.empty() &&
                             static_cast<const IdentExpr&>(*c.callee).name == self_name &&
                             static_cast<int>(c.args.size()) == self_nparams;
        return is_self ? 0 : static_cast<int>(c.args.size());
    }
    case ExprKind::Pipe: {
        std::vector<const Expr*> args;
        const Expr* callee = pipe_call_shape(static_cast<const PipeExpr&>(e), args);
        if (!callee || callee->kind != ExprKind::Ident) return 0;
        const auto& id = static_cast<const IdentExpr&>(*callee);
        const bool is_self = id.qualifier.empty() && id.name == self_name &&
                             static_cast<int>(args.size()) == self_nparams;
        return is_self ? 0 : static_cast<int>(args.size());
    }
    case ExprKind::Return: {
        const auto& r = static_cast<const ReturnExpr&>(e);
        return r.value ? has_tail_nonself_call(*r.value, self_name, self_nparams) : 0;
    }
    default:
        return 0;
    }
}

Codegen::Plan Codegen::plan_function(const std::string& name, int nparams,
                                     const BlockExpr& body) {
    return plan_with_inline_retry([&] {
    Plan p;
    Need n = measure_block(body, nparams);            // params occupy r0..r(n-1)
    p.n_locals  = n.local_peak;
    p.peak_temp = n.temp;
    // A self-tail-call stages all nparams args into temps before the writeback into r0..
    if (has_tail_self_call(body, name, nparams))
        p.peak_temp = imax(p.peak_temp, nparams);
    // A non-self tail call stages nargs args (+1 callee for the indirect form) into temps.
    const int nonself = has_tail_nonself_call(body, name, nparams);
    if (nonself > 0)
        p.peak_temp = imax(p.peak_temp, nonself + 1);
    p.frame_size = imax(1, p.n_locals + p.peak_temp);
    return p;
    });
}

// ----- emit pass ------------------------------------------------------------

void Codegen::begin_frame(const Plan& p) {
    if (p.frame_size > 63)
        throw CodegenError("frame size exceeds the 63-register r6 limit");
    scope_.clear();
    n_locals_   = 0;
    temp_base_  = p.n_locals;
    temp_top_   = p.n_locals;
    frame_size_ = p.frame_size;
    // Emitter self-check bookkeeping for this frame.
    emit_peak_temp_   = 0;
    emit_peak_locals_ = 0;
    plan_peak_temp_   = p.peak_temp;
    plan_n_locals_    = p.n_locals;
}

int Codegen::compile_coerced(const Expr& e, bool want_double) {
    // Fold an int literal directly to a double constant (no I2D needed).
    if (want_double && e.kind == ExprKind::IntLit) {
        int t = alloc_temp();
        as_.load_double(static_cast<uint8_t>(t),
                        static_cast<double>(static_cast<const IntLit&>(e).value));
        return t;
    }
    // RAW on purpose. This function performs the widening itself, so going through the honoring
    // `compile_expr` wrapper would widen the SAME node twice -- and `I2D` asserts its source is an
    // Int (`Value.h:120`), so the second one reads double bits through `asSigned48()`: a Debug assert,
    // and in Release a silent 0.0. That was the observed failure when the wrapper first went in.
    int r = compile_expr_raw(e);
    if (want_double && is_int_ty(e.ty)) {           // a runtime Int flowing into a Double slot
        if (is_temp(r)) { as_.I2D(static_cast<uint8_t>(r), static_cast<uint8_t>(r)); return r; }
        int t = alloc_temp();
        as_.I2D(static_cast<uint8_t>(t), static_cast<uint8_t>(r));
        return t;
    }
    return r;
}

// THE ONE PLACE the checker's `Expr::widen_double` record is honored, and the reason it is central.
// `Int -> Double` coercion used to be re-derived per emit site from whatever type information that
// site happened to have, and nine of the twenty-six sites had none: codegen sees no callee parameter
// type at an indirect/trait/native call, and no element type inside an `if`/`match` branch, so
// `fn dv(a: Double, b: Double)` called as `dv(1, 2)` divided two raw Ints and returned 0. The checker
// DOES know -- it used the `Int <: Double` edge at that very node -- so it marks and this obeys, the
// same idiom as `IdentExpr::ambient` / `IdentExpr::inherent`.
// Sites that derive `want_double` themselves (a struct field via `field_double`, a `let` via its
// annotation, a return via `current_fn_ret_double_`) call `compile_coerced` DIRECTLY and therefore
// reach `compile_expr_raw`, so exactly one coercion is applied on any node.
int Codegen::compile_expr(const Expr& e) {
    if (e.widen_double) return compile_coerced(e, true);
    return compile_expr_raw(e);
}

int Codegen::compile_expr_raw(const Expr& e) {
    set_pos_at(e);
    switch (e.kind) {
    case ExprKind::IntLit: {
        int t = alloc_temp();
        as_.load_const(static_cast<uint8_t>(t), static_cast<const IntLit&>(e).value);
        return t;
    }
    case ExprKind::DoubleLit: {
        int t = alloc_temp();
        as_.load_double(static_cast<uint8_t>(t), static_cast<const DoubleLit&>(e).value);
        return t;
    }
    case ExprKind::BoolLit: {
        int t = alloc_temp();
        as_.load_constant(static_cast<uint8_t>(t), Value::fromBool(static_cast<const BoolLit&>(e).value));
        return t;
    }
    case ExprKind::StrLit: {
        int t = alloc_temp();
        as_.load_str(static_cast<uint8_t>(t), static_cast<const StrLit&>(e).value);
        return t;
    }
    case ExprKind::Ident: {
        const auto& id = static_cast<const IdentExpr&>(e);
        if (!id.qualifier.empty())
            throw CodegenError("qualified path '" + id.qualifier + "::" + id.name +
                               "' must be called, not used as a value", e.line, e.col);
        // SROA: a dissolved binding read as a WHOLE value. Under the pre-pass's rules there is
        // exactly one way to get here -- a FIELD-ONLY parameter position whose expansion emit
        // declined. The decline paths are not fully enumerable from the pre-pass (tail position is
        // one: `p.m()` as a body's tail takes the TCO path, which the guard caught), so rather
        // than trying to predict them, rebuild the object here.
        //
        // Sound because the binding is IMMUTABLE, so a fresh object is an equally valid
        // representation -- and because a field-only parameter cannot store its argument anywhere,
        // two materialisations of one binding can never meet in an EQ_DEEP. That is what keeps
        // decision D5 closed without the transitive reachability rule nobody can write.
        if (const Local* L = local_slot(id.name); L && !L->fields.empty())
            return materialize_dissolved(*L, e.ty, e.line, e.col);
        int r = local_value_reg(id.name, e.line, e.col);   // still guards the other value sites
        if (r >= 0) return r;
        // A module const: an ARRAY const loads the shared pooled instance (LOAD_CONST_ARRAY); a
        // scalar const inlines its literal initializer at the use site (S0).
        if (auto ci = const_defs_.find(id.name); ci != const_defs_.end()) {
            if (auto ai = const_array_index_.find(id.name); ai != const_array_index_.end()) {
                int t = alloc_temp();
                as_.load_const_array(static_cast<uint8_t>(t), ai->second);
                return t;
            }
            return compile_expr(*ci->second);
        }
        // A bare top-level fn name used as a value -> a first-class Func immediate.
        if (user_fn_ref(id)) {
            int t = alloc_temp();
            as_.LOAD_FN(static_cast<uint8_t>(t), as_.func_id(id.name));
            return t;
        }
        // A bare nullary constructor (`None`, a 0-field variant/tuple-struct) -> construct it.
        if (const StructDef* sd = find_struct(id.name)) {
            if (sd->is_int_enum) {                       // erased: the value IS the Int discriminant
                int t = alloc_temp();
                as_.load_const(static_cast<uint8_t>(t), sd->enum_disc);
                return t;
            }
            if (sd->field_names.empty()) {
                int t = alloc_temp();
                as_.NEW_STRUCT(static_cast<uint8_t>(t), sd->id);
                return t;
            }
            throw CodegenError("constructor '" + id.name +
                               "' needs arguments (or field initializers)", e.line, e.col);
        }
        // Inside a lifted lambda: a captured free variable -> read its by-value capture.
        auto cit = current_lambda_captures_.find(id.name);
        if (cit != current_lambda_captures_.end()) {
            int t = alloc_temp();
            as_.LOAD_CAPTURE(static_cast<uint8_t>(t), static_cast<uint16_t>(cit->second));
            return t;
        }
        if (current_fn_is_lambda_)
            throw CodegenError("'" + id.name + "' is not available inside this lambda -- only its "
                               "parameters, captured variables, and top-level functions are in scope",
                               e.line, e.col);
        throw CodegenError("undefined variable '" + id.name + "'", e.line, e.col);
    }
    case ExprKind::Unary:
        return compile_unary(static_cast<const UnaryExpr&>(e));
    case ExprKind::Binary: {
        const auto& b = static_cast<const BinaryExpr&>(e);
        if (b.op == TokKind::AndAnd || b.op == TokKind::OrOr)
            return compile_short_circuit(b);
        if (is_comparison(b.op))
            return compile_comparison(b);
        return compile_arith(b);
    }
    case ExprKind::If:
        return compile_if(static_cast<const IfExpr&>(e));
    case ExprKind::While:
        return compile_while(static_cast<const WhileExpr&>(e));
    case ExprKind::Loop:
        return compile_loop(static_cast<const LoopExpr&>(e));
    case ExprKind::Block:
        return compile_block(static_cast<const BlockExpr&>(e));
    case ExprKind::Call: {
        const auto& c = static_cast<const CallExpr&>(e);
        std::vector<const Expr*> args;
        for (const auto& a : c.args) args.push_back(a.get());
        return compile_call_dispatch(c.callee.get(), args, c.ty, &e);
    }
    case ExprKind::Pipe: {
        const auto& p = static_cast<const PipeExpr&>(e);
        std::vector<const Expr*> args;
        const Expr* callee = pipe_call_shape(p, args);
        if (!callee)
            throw CodegenError("static codegen: this pipe target is not supported yet "
                               "(only a named function / call to one)", e.line, e.col);
        return compile_call_dispatch(callee, args, p.ty, &e);
    }
    case ExprKind::Index:
        return compile_index(static_cast<const IndexExpr&>(e));
    case ExprKind::Return: {
        // An EARLY return (in a non-tail statement): compute the value into r0 (with the
        // return-boundary coercion) and RET. The result is a dummy (control diverges).
        const auto& r = static_cast<const ReturnExpr&>(e);
        if (r.value) compile_ret_value(*r.value);
        else         as_.load_constant(static_cast<uint8_t>(return_target()), Value::fromNil());
        emit_return_jump();
        return alloc_temp();
    }
    case ExprKind::StructLit:
        return compile_struct_lit(static_cast<const StructLit&>(e));
    case ExprKind::Tuple:
        return compile_tuple(static_cast<const TupleExpr&>(e));
    case ExprKind::Field:
        return compile_field(static_cast<const FieldExpr&>(e));
    case ExprKind::ListLit:
        return compile_list_lit(static_cast<const ListLit&>(e));
    case ExprKind::MapLit:
        return compile_map_lit(static_cast<const MapLit&>(e));
    case ExprKind::For:
        return compile_for(static_cast<const ForExpr&>(e));
    case ExprKind::Match:
        return compile_match(static_cast<const MatchExpr&>(e));
    case ExprKind::Try:
        return compile_try(static_cast<const TryExpr&>(e));
    case ExprKind::Lambda:
        return compile_lambda(static_cast<const LambdaExpr&>(e));
    case ExprKind::Break: {
        if (loop_stack_.empty()) throw CodegenError("'break' outside a loop", e.line, e.col);
        const auto& br = static_cast<const BreakExpr&>(e);
        if (br.value) {
            // `break value` -- the checker guarantees the nearest loop is a `loop` (result >= 0).
            int rv = compile_expr(*br.value);
            const int rr = loop_stack_.back().result;
            if (rr >= 0 && rv != rr) as_.R6(OpCode::MOV, static_cast<uint8_t>(rr), static_cast<uint8_t>(rv), 0);
            free_if_temp(rv);
        }
        as_.J(OpCode::J, loop_stack_.back().brk);
        return alloc_temp();                          // dummy (control diverges)
    }
    case ExprKind::Continue: {
        if (loop_stack_.empty()) throw CodegenError("'continue' outside a loop", e.line, e.col);
        as_.J(OpCode::J, loop_stack_.back().cont);
        return alloc_temp();
    }
    default:
        throw CodegenError(std::string("static codegen: ") + expr_kind_name(e.kind) +
                           " is not supported yet (it lands in a later phase)", e.line, e.col);
    }
}

void Codegen::compile_into(const Expr& e, int dst) {
    // Destination-driven fast path (register coalescing): a terminal arithmetic / comparison /
    // unary producer writes its result DIRECTLY into `dst` (no fresh temp + MOV). Short-circuit
    // `&&`/`||` keeps its own result register (control flow), so it takes the generic path below.
    // A node the checker marked for widening takes the generic path too: the fast paths emit the
    // producer straight into `dst` without ever reaching `compile_expr`, so they would silently skip
    // the coercion. The route that actually reaches this (verified by reverting the guard) is the
    // INLINE EXPANSION's argument copy -- `id(a + b)` at a `Double` parameter yields a raw `3`
    // without it, and only with inlining ON, because a real call compiles arguments via
    // `compile_expr`. Locked by `widen_inline_arg_binary` / `widen_inline_arg_unary`.
    if (!e.widen_double) {
        if (e.kind == ExprKind::Unary) {
            set_pos_at(e);
            compile_unary(static_cast<const UnaryExpr&>(e), dst);
            return;
        }
        if (e.kind == ExprKind::Binary) {
            const auto& b = static_cast<const BinaryExpr&>(e);
            if (b.op != TokKind::AndAnd && b.op != TokKind::OrOr) {
                set_pos_at(e);
                if (is_comparison(b.op)) compile_comparison(b, dst);
                else                     compile_arith(b, dst);
                return;
            }
        }
    }
    int r = compile_expr(e);
    if (r != dst) as_.R6(OpCode::MOV, static_cast<uint8_t>(dst), static_cast<uint8_t>(r), 0);
    free_if_temp(r);
}

// A `+`-concatenation operand: if it isn't already a String, stringify it via the total TO_STRING
// opcode (id 76) so the following ADD sees two Strings. Mirrors the `toString(x)` builtin lowering.
// The checker guarantees a non-String concat operand is a scalar (Int/Double/Bool).
int Codegen::compile_concat_operand(const Expr& e) {
    int r = compile_expr(e);
    if (is_string_ty(e.ty)) return r;
    int rd = is_temp(r) ? r : alloc_temp();
    as_.R6(OpCode::TO_STRING, static_cast<uint8_t>(rd), static_cast<uint8_t>(r), 0);
    return rd;
}

// The `toString(x)` / `print(x)` / `println(x)` lowering (and, via the parser's `${e}` desugar, string
// interpolation). If `e`'s STATIC type is a transparent (erased) struct, the runtime value is a bare
// immediate with no type id, so recover the name STATICALLY here and emit the tuple-struct display form
// `Name(<inner>)`. TOP-LEVEL only -- a transparent value NESTED in a container/struct still dumps as the
// bare immediate (the VM's TO_STRING has no type info; the permanent partial). Else a plain TO_STRING.
int Codegen::compile_stringify(const Expr& e) {
    // A Char is a transparent newtype over the code-point Int (std::string), so it must be intercepted
    // BEFORE the generic transparent-struct branch: render the GLYPH (not `Char(65)`) by calling the
    // prelude UTF-8 encoder `codePointToStr(cp)` -- in the SAME module as Char, and its erased Char arg
    // flows into the Int param unchanged. TOP-LEVEL only (a Char nested in a container dumps as the bare
    // code-point Int -- lossy, like #1/#2). The tree-shaker keeps codePointToStr for any Char stringify.
    if (transparent_struct_of(e.ty) && short_name(struct_name_of(e.ty)) == "Char") {
        const std::string cps = mangle_name(module_prefix_of(struct_name_of(e.ty)), "codePointToStr");
        return compile_call(cps, { &e });
    }
    if (transparent_struct_of(e.ty)) {
        const std::string disp = short_name(struct_name_of(e.ty));
        // Emit `"Name(" + toString(inner) + ")"` with a STRICTLY LIFO temp discipline: the result
        // accumulator `rd` stays at the BOTTOM (reusing the inner's slot when it is a temp), and at
        // most ONE scratch `rt` ever sits above it, always freed as the current top. (An earlier
        // version freed a buried temp -- a no-op that LEAKED a slot and poisoned later statements'
        // frames.) Peak = max(inner.temp, 2), which the caller's `1 + need_call` measure covers.
        int rin = compile_expr(e);                                     // the erased inner immediate
        int rd  = is_temp(rin) ? rin : alloc_temp();                   // accumulator (bottom slot)
        as_.R6(OpCode::TO_STRING, static_cast<uint8_t>(rd), static_cast<uint8_t>(rin), 0);   // e.g. "42"
        int rt = alloc_temp();                                         // one scratch above rd
        as_.load_str(static_cast<uint8_t>(rt), disp + "(");            // "Name("
        as_.R6(OpCode::ADD, static_cast<uint8_t>(rt), static_cast<uint8_t>(rt),
               static_cast<uint8_t>(rd));                              // "Name(42"
        as_.load_str(static_cast<uint8_t>(rd), ")");                   // reuse rd for ")"
        as_.R6(OpCode::ADD, static_cast<uint8_t>(rd), static_cast<uint8_t>(rt),
               static_cast<uint8_t>(rd));                              // "Name(42)"
        free_if_temp(rt);                                              // rt is the top -> freed LIFO
        return rd;
    }
    if (const auto* variants = int_enum_of(e.ty)) {
        // An int-backed enum value is a bare Int discriminant with no runtime type id, so recover the
        // variant NAME here via a static-type-directed discriminant->name switch. Temp discipline (2
        // temps, leak-free): `rd` (bottom) doubles as the compare CONSTANT during the search (it holds
        // no result until a match, which then jumps out) AND the result string; `rin` (the discriminant)
        // sits above it and is freed LIFO at the end. Peak = 1 + inner.temp, within the `1 + need_call`
        // toString measure. NESTED-in-a-container stays lossy (the VM dumps the bare Int) -- like #1.
        int rd  = alloc_temp();                                        // result accumulator (bottom slot)
        int rin = compile_expr(e);                                     // the discriminant Int (above rd)
        const std::string end_lbl = fresh_label("estr_");
        for (const auto& dv : *variants) {
            const std::string next_lbl = fresh_label("enext_");
            as_.load_const(static_cast<uint8_t>(rd), dv.first);        // rd := candidate discriminant
            as_.B(OpCode::BNE_INT, static_cast<uint8_t>(rin), static_cast<uint8_t>(rd), next_lbl);
            as_.load_str(static_cast<uint8_t>(rd), dv.second);         // match -> rd := the variant name
            as_.J(OpCode::J, end_lbl);
            as_.label(next_lbl);
        }
        as_.load_str(static_cast<uint8_t>(rd), "?");                   // unreachable (value is a valid variant)
        as_.label(end_lbl);
        free_if_temp(rin);                                             // rin is the top -> freed LIFO
        return rd;
    }
    int rx = compile_expr(e);
    int rd = is_temp(rx) ? rx : alloc_temp();
    as_.R6(OpCode::TO_STRING, static_cast<uint8_t>(rd), static_cast<uint8_t>(rx), 0);
    return rd;
}

int Codegen::compile_arith(const BinaryExpr& b, int dst_hint) {
    // INCR/DECR peephole: `x = x + 1` / `x = x - 1` (a self-assign onto the very register `x`
    // lives in) lowers to a single in-place INCR/DECR, skipping the const-1 load + ADD/SUB. This
    // fires ONLY when the destination is `x`'s own local (dst_hint), the type is Int, and the
    // literal is exactly 1 -- so it targets loop counters (`i = i + 1`) on the critical dependency
    // chain. Reading a local / an Int literal is pure, so dropping the explicit operand eval is safe.
    if (dst_hint >= 0 && is_int_ty(b.ty) && (b.op == TokKind::Plus || b.op == TokKind::Minus)) {
        auto is_one = [](const Expr& e) {
            return e.kind == ExprKind::IntLit && static_cast<const IntLit&>(e).value == 1;
        };
        const IdentExpr* var = nullptr;   // the `x` operand, if the pattern matches
        bool incr = true;
        if (b.op == TokKind::Plus) {                                   // x + 1  or  1 + x
            if (b.lhs->kind == ExprKind::Ident && is_one(*b.rhs))
                var = static_cast<const IdentExpr*>(b.lhs.get());
            else if (b.rhs->kind == ExprKind::Ident && is_one(*b.lhs))
                var = static_cast<const IdentExpr*>(b.rhs.get());
        } else if (b.lhs->kind == ExprKind::Ident && is_one(*b.rhs)) { // x - 1 (NOT 1 - x)
            var = static_cast<const IdentExpr*>(b.lhs.get());
            incr = false;
        }
        if (var && var->qualifier.empty() && local_reg(var->name) == dst_hint) {
            as_.C2(incr ? OpCode::INCR : OpCode::DECR, static_cast<uint8_t>(dst_hint));
            return dst_hint;
        }
    }
    // `+` with a String result is CONCATENATION -> the generic promoting ADD (id 41), which
    // runtime-dispatches to string_add. Java-style: a non-String scalar operand is first
    // stringified (compile_concat_operand). String+String is the common case (no TO_STRING).
    const bool str = b.op == TokKind::Plus && is_string_ty(b.ty);
    const bool dbl = !str && is_double_ty(b.ty);   // a Double result => the promoting double path
    int ra = str ? compile_concat_operand(*b.lhs) : (dbl ? compile_coerced(*b.lhs, true) : compile_expr(*b.lhs));
    int rb = str ? compile_concat_operand(*b.rhs) : (dbl ? compile_coerced(*b.rhs, true) : compile_expr(*b.rhs));
    const OpCode oc = str ? OpCode::ADD : arith_opcode(b.op, dbl);
    if (dst_hint >= 0) {   // coalesce: write the result straight into the caller's destination
        as_.R6(oc, static_cast<uint8_t>(dst_hint), static_cast<uint8_t>(ra), static_cast<uint8_t>(rb));
        free_if_temp(rb);
        free_if_temp(ra);
        return dst_hint;
    }
    int dst;
    if (is_temp(ra)) {
        dst = ra;
        as_.R6(oc, static_cast<uint8_t>(dst), static_cast<uint8_t>(ra), static_cast<uint8_t>(rb));
        free_if_temp(rb);
    } else if (is_temp(rb)) {
        dst = rb;
        as_.R6(oc, static_cast<uint8_t>(dst), static_cast<uint8_t>(ra), static_cast<uint8_t>(rb));
    } else {
        dst = alloc_temp();
        as_.R6(oc, static_cast<uint8_t>(dst), static_cast<uint8_t>(ra), static_cast<uint8_t>(rb));
    }
    return dst;
}

int Codegen::compile_comparison(const BinaryExpr& b, int dst_hint) {
    const bool li = is_int_ty(b.lhs->ty) && is_int_ty(b.rhs->ty);
    const bool nn = is_num_ty(b.lhs->ty) && is_num_ty(b.rhs->ty);
    int ra = compile_expr(*b.lhs);
    int rb = compile_expr(*b.rhs);

    OpCode oc = OpCode::EQ;
    bool swap = false, negate = false;
    if (li) {                                   // typed int fast path
        switch (b.op) {
        case TokKind::EqEq:  oc = OpCode::SET_EQ; break;
        case TokKind::NotEq: oc = OpCode::SET_EQ; negate = true; break;   // SET_EQ then NOT_BOOL
        case TokKind::Lt:    oc = OpCode::SET_LT; break;
        case TokKind::Le:    oc = OpCode::SET_LE; break;
        case TokKind::Gt:    oc = OpCode::SET_LT; swap = true; break;      // a > b == b < a
        case TokKind::Ge:    oc = OpCode::SET_LE; swap = true; break;
        default: break;
        }
    } else if (nn) {                            // >=1 Double: the six real *_NUM ops
        switch (b.op) {
        case TokKind::EqEq:  oc = OpCode::SET_EQ_NUM; break;
        case TokKind::NotEq: oc = OpCode::SET_NE_NUM; break;
        case TokKind::Lt:    oc = OpCode::SET_LT_NUM; break;
        case TokKind::Le:    oc = OpCode::SET_LE_NUM; break;
        case TokKind::Gt:    oc = OpCode::SET_GT_NUM; break;
        case TokKind::Ge:    oc = OpCode::SET_GE_NUM; break;
        default: break;
        }
    } else {                                    // Bool / String / composite: EQ(_DEEP)/NE, content LT/LE
        // `==`/`!=` on a COMPOSITE (struct/tuple/enum/Vec/Array/Map/Bytes) or a generic type param
        // lowers to the STRUCTURAL EQ_DEEP opcode (deep value equality); a flat immediate/String
        // keeps the cheaper EQ/NE. `!=` deep = EQ_DEEP then NOT_BOOL (the `negate` path below). The
        // operand type is taken from a non-diverging side (a Never/Error branch carries no shape).
        TyPtr et = (b.lhs->ty && b.lhs->ty->kind != TyKind::Never && b.lhs->ty->kind != TyKind::Error)
                 ? b.lhs->ty : b.rhs->ty;
        const bool deep = !is_flat_eq_type(et);
        switch (b.op) {
        case TokKind::EqEq:  oc = deep ? OpCode::EQ_DEEP : OpCode::EQ; break;
        case TokKind::NotEq: oc = deep ? OpCode::EQ_DEEP : OpCode::NE; negate = deep; break;
        case TokKind::Lt:    oc = OpCode::LT; break;
        case TokKind::Le:    oc = OpCode::LE; break;
        case TokKind::Gt:    oc = OpCode::LT; swap = true; break;
        case TokKind::Ge:    oc = OpCode::LE; swap = true; break;
        default: break;
        }
    }

    const uint8_t s1 = static_cast<uint8_t>(swap ? rb : ra);
    const uint8_t s2 = static_cast<uint8_t>(swap ? ra : rb);
    if (dst_hint >= 0) {   // coalesce: result straight into the caller's destination
        as_.R6(oc, static_cast<uint8_t>(dst_hint), s1, s2);
        if (negate)
            as_.R6(OpCode::NOT_BOOL, static_cast<uint8_t>(dst_hint), static_cast<uint8_t>(dst_hint), 0);
        free_if_temp(rb);
        free_if_temp(ra);
        return dst_hint;
    }
    int dst;
    if (is_temp(ra)) {
        dst = ra;
        as_.R6(oc, static_cast<uint8_t>(dst), s1, s2);
        free_if_temp(rb);
    } else if (is_temp(rb)) {
        dst = rb;
        as_.R6(oc, static_cast<uint8_t>(dst), s1, s2);
    } else {
        dst = alloc_temp();
        as_.R6(oc, static_cast<uint8_t>(dst), s1, s2);
    }
    if (negate)
        as_.R6(OpCode::NOT_BOOL, static_cast<uint8_t>(dst), static_cast<uint8_t>(dst), 0);
    return dst;
}

int Codegen::compile_short_circuit(const BinaryExpr& b) {
    const bool is_and = (b.op == TokKind::AndAnd);
    const std::string end_lbl = fresh_label(is_and ? "andend_" : "orend_");

    // The LHS Bool lands in `r`, which also holds the final result: it must be a mutable
    // temp surviving the branch + RHS. Operands are statically Bool (no TO_BOOL).
    int rl = compile_expr(*b.lhs);
    int r;
    if (is_temp(rl)) {
        r = rl;
    } else {                                    // a Bool local: copy into a fresh temp
        r = alloc_temp();
        as_.R6(OpCode::MOV, static_cast<uint8_t>(r), static_cast<uint8_t>(rl), 0);
    }

    // `&&` skips the RHS when the LHS is false; `||` when true. On the taken branch `r`
    // already holds the correct result.
    as_.B1(is_and ? OpCode::BF : OpCode::BT, static_cast<uint8_t>(r), end_lbl);

    int rr = compile_expr(*b.rhs);
    if (rr != r) as_.R6(OpCode::MOV, static_cast<uint8_t>(r), static_cast<uint8_t>(rr), 0);
    free_if_temp(rr);

    as_.label(end_lbl);
    return r;
}

int Codegen::compile_unary(const UnaryExpr& u, int dst_hint) {
    if (u.op == TokKind::Minus) {
        int re = compile_expr(*u.operand);
        const OpCode oc = is_double_ty(u.ty) ? OpCode::NEG : OpCode::NEG_INT;
        if (dst_hint >= 0) {
            as_.R6(oc, static_cast<uint8_t>(dst_hint), static_cast<uint8_t>(re), 0);
            free_if_temp(re);
            return dst_hint;
        }
        int dst = is_temp(re) ? re : alloc_temp();
        as_.R6(oc, static_cast<uint8_t>(dst), static_cast<uint8_t>(re), 0);
        return dst;
    }
    if (u.op == TokKind::Not) {                 // operand is statically Bool
        int re = compile_expr(*u.operand);
        if (dst_hint >= 0) {
            as_.R6(OpCode::NOT_BOOL, static_cast<uint8_t>(dst_hint), static_cast<uint8_t>(re), 0);
            free_if_temp(re);
            return dst_hint;
        }
        int dst = is_temp(re) ? re : alloc_temp();
        as_.R6(OpCode::NOT_BOOL, static_cast<uint8_t>(dst), static_cast<uint8_t>(re), 0);
        return dst;
    }
    if (u.op == TokKind::Tilde) {               // ~x == x XOR -1 (Int)
        int re = compile_expr(*u.operand);
        int ones = alloc_temp();
        as_.load_const(static_cast<uint8_t>(ones), -1);
        if (dst_hint >= 0) {
            as_.R6(OpCode::XOR_INT, static_cast<uint8_t>(dst_hint), static_cast<uint8_t>(re), static_cast<uint8_t>(ones));
            free_if_temp(ones);
            free_if_temp(re);
            return dst_hint;
        }
        if (is_temp(re)) {
            as_.R6(OpCode::XOR_INT, static_cast<uint8_t>(re), static_cast<uint8_t>(re), static_cast<uint8_t>(ones));
            free_if_temp(ones);
            return re;
        }
        as_.R6(OpCode::XOR_INT, static_cast<uint8_t>(ones), static_cast<uint8_t>(re), static_cast<uint8_t>(ones));
        return ones;
    }
    throw CodegenError("unsupported unary operator", u.line, u.col);
}

void Codegen::branch_if_false(const Expr& cond, const std::string& target) {
    // Fused compare-branch peephole: an `if`/`while`/`loop` condition of the form `a REL b` with
    // BOTH operands statically Int lowers to a SINGLE int compare-branch that jumps to `target`
    // when the relation is FALSE (the NEGATED relation) -- replacing `SET_* ; BF` (drops the Bool
    // temp and a dependent op on the loop hot path). INT-ONLY: negating a relation is unsound for
    // Double, since NaN breaks `a<b == !(a>=b)` (an ordered *_NUM branch is false for NaN, but
    // branch-if-false must TAKE the branch there); the Double/other path keeps `SET_* ; BF`.
    if (cond.kind == ExprKind::Binary) {
        const auto& b = static_cast<const BinaryExpr&>(cond);
        if (is_comparison(b.op) && is_int_ty(b.lhs->ty) && is_int_ty(b.rhs->ty)) {
            set_pos_at(cond);
            int ra = compile_expr(*b.lhs);
            int rb = compile_expr(*b.rhs);
            OpCode oc; bool swap = false;
            switch (b.op) {
            case TokKind::EqEq: oc = OpCode::BNE_INT; break;                // !(a==b) -> a!=b
            case TokKind::NotEq: oc = OpCode::BEQ_INT; break;               // !(a!=b) -> a==b
            case TokKind::Lt:   oc = OpCode::BGE_INT; break;                // !(a<b)  -> a>=b
            case TokKind::Ge:   oc = OpCode::BLT_INT; break;                // !(a>=b) -> a<b
            case TokKind::Le:   oc = OpCode::BLT_INT; swap = true; break;   // !(a<=b) -> a>b -> b<a
            case TokKind::Gt:   oc = OpCode::BGE_INT; swap = true; break;   // !(a>b)  -> a<=b -> b>=a
            default:            oc = OpCode::BNE_INT; break;                // unreachable (comparison)
            }
            const uint8_t s1 = static_cast<uint8_t>(swap ? rb : ra);
            const uint8_t s2 = static_cast<uint8_t>(swap ? ra : rb);
            as_.B(oc, s1, s2, target);
            free_if_temp(rb);
            free_if_temp(ra);
            return;
        }
    }
    // The checker forbids non-Bool conditions, so `cond` is statically Bool -- no TO_BOOL.
    int rc = compile_expr(cond);
    as_.B1(OpCode::BF, static_cast<uint8_t>(rc), target);
    free_if_temp(rc);
}

void Codegen::branch_if_true(const Expr& cond, const std::string& target) {
    // Fused compare-branch, jump-when-TRUE (the rotated-loop back-edge). Because it tests the
    // DIRECT relation (no negation), it is NaN-sound for Double too -- so unlike branch_if_false
    // it covers BOTH the typed-int fast path AND the `*_NUM` path (all six relations exist as real
    // branch opcodes, IEEE-ordered: a NaN operand makes an ordered branch not-taken, which for a
    // loop back-edge correctly EXITS the loop -- exactly what `while a<b` with NaN must do).
    if (cond.kind == ExprKind::Binary) {
        const auto& b = static_cast<const BinaryExpr&>(cond);
        const bool li = is_int_ty(b.lhs->ty) && is_int_ty(b.rhs->ty);
        const bool nn = !li && is_num_ty(b.lhs->ty) && is_num_ty(b.rhs->ty);
        if (is_comparison(b.op) && (li || nn)) {
            set_pos_at(cond);
            int ra = compile_expr(*b.lhs);
            int rb = compile_expr(*b.rhs);
            OpCode oc; bool swap = false;
            if (li) {                                   // typed int: BLE/BGT missing -> swap
                switch (b.op) {
                case TokKind::EqEq: oc = OpCode::BEQ_INT; break;               // a==b
                case TokKind::NotEq: oc = OpCode::BNE_INT; break;              // a!=b
                case TokKind::Lt:   oc = OpCode::BLT_INT; break;               // a<b
                case TokKind::Ge:   oc = OpCode::BGE_INT; break;               // a>=b
                case TokKind::Le:   oc = OpCode::BGE_INT; swap = true; break;  // a<=b -> b>=a
                case TokKind::Gt:   oc = OpCode::BLT_INT; swap = true; break;  // a>b  -> b<a
                default:            oc = OpCode::BEQ_INT; break;               // unreachable
                }
            } else {                                    // >=1 Double: the six real *_NUM branches
                switch (b.op) {
                case TokKind::EqEq: oc = OpCode::BEQ_NUM; break;
                case TokKind::NotEq: oc = OpCode::BNE_NUM; break;
                case TokKind::Lt:   oc = OpCode::BLT_NUM; break;
                case TokKind::Le:   oc = OpCode::BLE_NUM; break;
                case TokKind::Gt:   oc = OpCode::BGT_NUM; break;
                case TokKind::Ge:   oc = OpCode::BGE_NUM; break;
                default:            oc = OpCode::BEQ_NUM; break;               // unreachable
                }
            }
            const uint8_t s1 = static_cast<uint8_t>(swap ? rb : ra);
            const uint8_t s2 = static_cast<uint8_t>(swap ? ra : rb);
            as_.B(oc, s1, s2, target);
            free_if_temp(rb);
            free_if_temp(ra);
            return;
        }
    }
    int rc = compile_expr(cond);
    as_.B1(OpCode::BT, static_cast<uint8_t>(rc), target);
    free_if_temp(rc);
}

int Codegen::compile_if(const IfExpr& f) {
    const std::string else_lbl = fresh_label("else_");
    const std::string end_lbl  = fresh_label("endif_");

    int result = alloc_temp();                  // holds the if-expression value
    branch_if_false(*f.cond, else_lbl);

    int vt = compile_expr(*f.then_blk);
    if (vt != result) as_.R6(OpCode::MOV, static_cast<uint8_t>(result), static_cast<uint8_t>(vt), 0);
    free_if_temp(vt);
    as_.J(OpCode::J, end_lbl);

    as_.label(else_lbl);
    if (f.else_blk) {
        int ve = compile_expr(*f.else_blk);
        if (ve != result) as_.R6(OpCode::MOV, static_cast<uint8_t>(result), static_cast<uint8_t>(ve), 0);
        free_if_temp(ve);
    } else {
        as_.load_constant(static_cast<uint8_t>(result), Value::fromNil());   // an if without else yields unit
    }

    as_.label(end_lbl);
    return result;
}

int Codegen::compile_while(const WhileExpr& w) {
    // Test-at-bottom (loop rotation): the condition lives at `check`, reached once via the cold
    // entry `J check` (so the zero-trip case is correct) and via fall-through after the body on
    // every later pass. This removes the unconditional back-edge `J` a top-tested loop needs
    // (one critical-path dispatch per iteration) -- the body ends in a SINGLE fused compare-branch
    // (`branch_if_true`, jump-when-TRUE -> the direct relation -> NaN-sound for Double too).
    const std::string top_lbl   = fresh_label("while_");
    const std::string check_lbl = fresh_label("whilec_");
    const std::string end_lbl   = fresh_label("wend_");

    // `continue` -> `check` (re-test); `break` -> `end`. A while never carries a break value.
    loop_stack_.push_back(LoopCtx{ end_lbl, check_lbl, -1 });

    as_.J(OpCode::J, check_lbl);
    as_.label(top_lbl);
    if (w.body) compile_discard(*w.body);       // the body's value is dropped -- build nothing
    as_.label(check_lbl);
    branch_if_true(*w.cond, top_lbl);

    as_.label(end_lbl);
    loop_stack_.pop_back();
    int result = alloc_temp();                  // the loop expression yields unit
    as_.load_constant(static_cast<uint8_t>(result), Value::fromNil());
    return result;
}

// `loop { body }` -- the condition-less loop. Unlike `while`, it can PRODUCE a value: the result
// register is allocated up front (so it stays reserved across the body, exactly as compile_if
// holds its result temp) and every `break value` MOVs into it. A loop with no break, or only
// valueless breaks, leaves the pre-initialized Nil -- for a break-less (`Never`) loop the register
// is never read at all.
int Codegen::compile_loop(const LoopExpr& l) {
    const std::string top_lbl = fresh_label("loop_");
    const std::string end_lbl = fresh_label("lend_");

    int result = alloc_temp();
    as_.load_constant(static_cast<uint8_t>(result), Value::fromNil());
    loop_stack_.push_back(LoopCtx{ end_lbl, top_lbl, result });

    as_.label(top_lbl);
    if (l.body) compile_discard(*l.body);        // the body's own value is discarded
    as_.J(OpCode::J, top_lbl);

    as_.label(end_lbl);
    loop_stack_.pop_back();
    return result;
}

int Codegen::compile_block(const BlockExpr& b) { return compile_block_stmts(b, /*want_value=*/true); }

// Compile an expression whose value is thrown away. The side effects are identical to
// compile_expr; only the *result* is not built. For a block that ends in a statement rather than
// an expression this removes a `load_constant(Nil)` -- a CONSTANT-POOL load, so one extra dispatch
// on every pass through a loop body. It was a third of the ops (and 30 % of the x86 instructions)
// in the tightest loop the language can express, `while i < n { i = i + 1 }`.
void Codegen::compile_discard(const Expr& e) {
    if (e.kind == ExprKind::Block) {
        compile_block_stmts(static_cast<const BlockExpr&>(e), /*want_value=*/false);
        return;
    }
    if (e.kind == ExprKind::If) {
        compile_if_discard(static_cast<const IfExpr&>(e));
        return;
    }
    if (e.kind == ExprKind::Match) {
        compile_match_discard(static_cast<const MatchExpr&>(e));
        return;
    }
    int r = compile_expr(e);
    free_if_temp(r);
}

// The value-discarded form of compile_if -- the same branch skeleton minus everything that exists
// only to PRODUCE a value: the result temp, the per-arm MOV into it, and (for an `if` with no
// `else`) the unit `Nil`, which is a constant-pool load. Both arms recurse through
// compile_discard, so a statement-only arm block drops its own Nil as well.
//
// Without an `else` there is nothing to skip past, so the `J end` collapses too and the
// false-branch simply targets the end label: `if c { s }` as a statement emits the condition
// branch and the body, and nothing else.
void Codegen::compile_if_discard(const IfExpr& f) {
    const std::string end_lbl = fresh_label("endif_");

    if (!f.else_blk) {
        branch_if_false(*f.cond, end_lbl);
        compile_discard(*f.then_blk);
        as_.label(end_lbl);
        return;
    }

    const std::string else_lbl = fresh_label("else_");
    branch_if_false(*f.cond, else_lbl);
    compile_discard(*f.then_blk);
    as_.J(OpCode::J, end_lbl);
    as_.label(else_lbl);
    compile_discard(*f.else_blk);
    as_.label(end_lbl);
}

int Codegen::compile_block_stmts(const BlockExpr& b, bool want_value) {
    const size_t scope_mark  = scope_.size();
    const int    locals_mark = n_locals_;
    int value = -1;

    for (size_t i = 0; i < b.stmts.size(); ++i) {
        const Stmt& s = *b.stmts[i];
        const bool last = (i + 1 == b.stmts.size());
        if (last && s.kind == StmtKind::Expr) {
            const Expr& tail = *static_cast<const ExprStmt&>(s).expr;
            // In discard mode the tail expression is discarded too -- and recursing through
            // compile_discard means a nested statement-only block drops its Nil as well.
            if (want_value) value = compile_expr(tail);
            else            compile_discard(tail);
        } else {
            compile_stmt(s);
        }
    }

    scope_.resize(scope_mark);
    // If the value lives in a local declared inside this block, copy it into a temp before
    // the block's locals are reclaimed.
    if (value >= locals_mark && value < temp_base_) {
        int t = alloc_temp();
        as_.R6(OpCode::MOV, static_cast<uint8_t>(t), static_cast<uint8_t>(value), 0);
        value = t;
    }
    n_locals_ = locals_mark;

    if (!want_value) return -1;                 // nothing materialised: no Nil, no rescue MOV
    if (value < 0) {
        value = alloc_temp();
        as_.load_constant(static_cast<uint8_t>(value), Value::fromNil());
    }
    return value;
}

void Codegen::compile_stmt(const Stmt& s) {
    set_pos_at(s);
    switch (s.kind) {
    case StmtKind::Let: {
        const auto& l = static_cast<const LetStmt&>(s);
        if (l.pat && l.pat->kind == PatKind::Wildcard) {   // `let _ = e`: evaluate + discard
            if (l.init) { int r = compile_expr(*l.init); free_if_temp(r); }
            return;
        }
        // Irrefutable destructuring: `let (a, b) = e`, `let P{x, y} = e`, `let Pair(a, b) = e`.
        // Build the aggregate, then read its slots into locals (no alloc between -> no GC hazard).
        if (l.pat && (l.pat->kind == PatKind::Tuple ||
                      l.pat->kind == PatKind::Struct ||
                      l.pat->kind == PatKind::Ctor)) {
            int scrut = l.init ? compile_expr(*l.init) : alloc_temp();
            if (!l.init) as_.load_constant(static_cast<uint8_t>(scrut), Value::fromNil());
            bind_let_pattern(*l.pat, scrut, l.is_mut);
            free_if_temp(scrut);
            return;
        }
        if (!l.pat || l.pat->kind != PatKind::Ident)
            throw CodegenError("only simple `let name = ...` bindings are supported yet "
                               "(destructuring lands in a later phase)",
                               l.line, l.col);
        const auto& name = static_cast<const IdentPat&>(*l.pat).name;
        // SROA: a dissolved binding takes one local per FIELD, and the initializer is written
        // straight into them -- no NEW_STRUCT, no SET_PROP, no object. The lookup, not a re-
        // derivation: the pre-pass decided this and let_binding_count sized the frame for it, so
        // deciding again here is the one thing that could put emit and the plan out of step.
        if (sroa_) {
            auto it = sroa_lets_.find(&l);
            if (it != sroa_lets_.end()) {
                std::vector<int> regs;
                regs.reserve(static_cast<size_t>(it->second));
                for (int i = 0; i < it->second; ++i) regs.push_back(alloc_local());
                compile_dissolved_or_explode(*l.init, regs);
                Local loc{ name, regs[0], false, std::move(regs) };
                scope_.push_back(std::move(loc));
                ++sroa_seen_emit_;
                return;
            }
        }
        int reg = alloc_local();
        const bool want_double = annotation_is_double(l.type.get());
        if (l.init) {
            if (want_double && is_int_ty(l.init->ty)) {    // `let x: Double = <Int>` coercion
                int r = compile_coerced(*l.init, true);
                if (r != reg) as_.R6(OpCode::MOV, static_cast<uint8_t>(reg), static_cast<uint8_t>(r), 0);
                free_if_temp(r);
            } else {
                compile_into(*l.init, reg);
            }
        } else {
            as_.load_constant(static_cast<uint8_t>(reg), Value::fromNil());
        }
        scope_.push_back(Local{ name, reg, l.is_mut });
        return;
    }
    case StmtKind::Assign: {
        const auto& a = static_cast<const AssignStmt&>(s);
        if (a.target && a.target->kind == ExprKind::Field) {          // obj.field = / op= value
            compile_field_assign(static_cast<const FieldExpr&>(*a.target), *a.value, a.op);
            return;
        }
        if (a.target && a.target->kind == ExprKind::Index) {         // a[i] = / op= value
            compile_index_assign(static_cast<const IndexExpr&>(*a.target), *a.value, a.op);
            return;
        }
        if (!a.target || a.target->kind != ExprKind::Ident)
            throw CodegenError("only assignment to a variable is supported yet", a.line, a.col);
        const auto& name = static_cast<const IdentExpr&>(*a.target).name;
        int reg = local_value_reg(name, a.line, a.col);   // guards a dissolved binding (SROA)
        if (reg < 0) {
            // A lifted lambda runs in a FRESH frame, so `scope_` holds only its own locals and an
            // enclosing name misses here. The READ path resolves such a name through the closure
            // (LOAD_CAPTURE, compile_expr's Ident case); the WRITE path deliberately has no such
            // branch, because a capture is BY VALUE -- storing into the capture slot would be
            // invisible to the enclosing scope, which is not what `n = 1` means. So say WHICH of the
            // three situations this is instead of blaming the name: "undefined" is true only in the
            // last arm, and is actively misleading for a binding that is in scope and `mut`.
            //
            // The first arm is a BACKSTOP: the checker's lambda write barrier (`is_captured_name`,
            // Check.cpp) rejects a captured target before lowering, and `svc::compile` refuses to
            // lower a rejected program -- so it is unreachable from source and says so.
            if (is_capture_name(name))
                throw CodegenError("internal: '" + name + "' is a by-value capture of this lambda and "
                                   "cannot be assigned -- the checker should have rejected this",
                                   a.line, a.col);
            if (current_fn_is_lambda_)
                throw CodegenError("'" + name + "' is not available inside this lambda -- only its "
                                   "parameters, captured variables, and top-level functions are in scope",
                                   a.line, a.col);
            throw CodegenError("assignment to undefined variable '" + name + "'", a.line, a.col);
        }
        // A compound `x op= v` reads and writes the same register, so it is one arithmetic op in
        // place -- no separate read. `x += 1` on an Int takes the INCR/DECR shortcut for the same
        // reason `x = x + 1` does (compile_arith's peephole): it is the loop-counter critical path.
        if (a.op != TokKind::Assign) {
            const bool dbl = is_double_ty(a.target->ty);
            if (!dbl && (a.op == TokKind::Plus || a.op == TokKind::Minus) &&
                a.value->kind == ExprKind::IntLit &&
                static_cast<const IntLit&>(*a.value).value == 1) {
                as_.C2(a.op == TokKind::Plus ? OpCode::INCR : OpCode::DECR, static_cast<uint8_t>(reg));
                return;
            }
            int rv = dbl ? compile_coerced(*a.value, true) : compile_expr(*a.value);
            as_.R6(arith_opcode(a.op, dbl), static_cast<uint8_t>(reg),
                   static_cast<uint8_t>(reg), static_cast<uint8_t>(rv));
            free_if_temp(rv);
            return;
        }
        // The target IdentExpr's ty is the variable's declared type: widen an Int rvalue
        // into a Double variable at the boundary.
        if (is_double_ty(a.target->ty) && is_int_ty(a.value->ty)) {
            int r = compile_coerced(*a.value, true);
            if (r != reg) as_.R6(OpCode::MOV, static_cast<uint8_t>(reg), static_cast<uint8_t>(r), 0);
            free_if_temp(r);
        } else {
            compile_into(*a.value, reg);
        }
        return;
    }
    case StmtKind::Expr: {
        compile_discard(*static_cast<const ExprStmt&>(s).expr);
        return;
    }
    }
}

// ----- structs / tuples / enums ----------------------------------------------

const Codegen::StructDef* Codegen::find_struct(const std::string& name) const {
    auto it = structs_.find(name);
    return it == structs_.end() ? nullptr : &it->second;
}

int Codegen::field_slot(const StructDef& sd, const std::string& field) {
    for (size_t i = 0; i < sd.field_names.size(); ++i)
        if (sd.field_names[i] == field) return static_cast<int>(i);
    return -1;
}

std::string Codegen::struct_name_of(const TyPtr& t) const {
    return (t && t->kind == TyKind::Named && structs_.count(t->name)) ? t->name : std::string();
}

const Codegen::StructDef* Codegen::transparent_struct_of(const TyPtr& t) const {
    if (!t || t->kind != TyKind::Named) return nullptr;
    auto it = structs_.find(t->name);
    return (it != structs_.end() && it->second.is_transparent) ? &it->second : nullptr;
}

void Codegen::register_struct_shape(const std::string& name, const std::vector<Field>& fields,
                                    bool is_tuple, bool is_transparent, DumpStyle style) {
    StructDef sd;
    sd.is_tuple = is_tuple;
    sd.is_transparent = is_transparent;
    std::vector<std::string> fnames;
    for (const Field& f : fields) {
        fnames.push_back(f.name);
        sd.field_names.push_back(f.name);
        sd.field_double.push_back(annotation_is_double(f.type.get()) ? 1 : 0);
    }
    sd.id = as_.define_struct(name, fnames, style);
    structs_.emplace(name, std::move(sd));
}

const std::vector<std::pair<int64_t, std::string>>* Codegen::int_enum_of(const TyPtr& t) const {
    if (!t || t->kind != TyKind::Named) return nullptr;
    auto it = int_enum_variants_.find(t->name);
    return (it != int_enum_variants_.end()) ? &it->second : nullptr;
}

bool Codegen::is_flat_eq_type(const TyPtr& t) const {
    if (!t) return false;
    switch (t->kind) {
    case TyKind::Int: case TyKind::Double: case TyKind::Bool:
    case TyKind::String: case TyKind::Unit: return true;
    case TyKind::Named:
        if (transparent_struct_of(t)) return true;   // erases to its inner Int/Double/Bool (incl. Char)
        if (int_enum_of(t)) return true;             // erases to a bare Int discriminant
        return false;                                // struct / enum / Vec / Array / Map / Bytes / List
    default: return false;                           // Tuple / Var (generic) / Dyn / Fn -> structural
    }
}

void Codegen::register_program_types(const Program& prog) {
    for (const auto& item : prog.items) {
        if (item->kind == ItemKind::Struct) {
            const auto& si = static_cast<const StructItem&>(*item);
            register_struct_shape(si.name, si.fields, si.is_tuple, si.is_transparent,
                                  si.is_tuple ? DumpStyle::NamedTuple : DumpStyle::Struct);
        } else if (item->kind == ItemKind::Enum) {
            const auto& en = static_cast<const EnumItem&>(*item);
            if (en.is_int_backed) {
                // Erased: each variant IS a bare Int discriminant -- register a fieldless StructDef
                // WITHOUT a VM type id (no NEW_STRUCT / GET_TYPE_ID ever touches it). Discriminants
                // follow the SAME rule as the checker (resolve_enum): a counter from 0, pinned by
                // `= v` which resumes the counter at v+1.
                int64_t next_disc = 0;
                auto& table = int_enum_variants_[en.name];
                for (const EnumVariant& v : en.variants) {
                    const int64_t d = v.has_disc ? v.disc : next_disc;
                    next_disc = d + 1;
                    StructDef sd;
                    sd.is_int_enum = true;
                    sd.enum_disc = d;
                    structs_.emplace(v.name, std::move(sd));
                    table.emplace_back(d, short_name(v.name));
                }
            } else {
                for (const EnumVariant& v : en.variants)
                    register_struct_shape(v.name, v.fields, v.is_tuple, false,
                                          v.is_tuple ? DumpStyle::NamedTuple : DumpStyle::Struct);
            }
        }
    }
}

uint16_t Codegen::ensure_tuple_type(int arity) {
    auto it = tuple_type_ids_.find(arity);
    if (it != tuple_type_ids_.end()) return it->second;
    std::vector<std::string> fields;
    for (int i = 0; i < arity; ++i) fields.push_back("_" + std::to_string(i));
    const uint16_t id = as_.define_struct("$Tuple" + std::to_string(arity), fields, DumpStyle::Tuple);
    tuple_type_ids_.emplace(arity, id);
    return id;
}

int Codegen::compile_struct_lit(const StructLit& sl) {
    const StructDef* sd = find_struct(sl.name);
    if (!sd) throw CodegenError("unknown struct type '" + sl.name + "'", sl.line, sl.col);
    int rd = alloc_temp();
    as_.NEW_STRUCT(static_cast<uint8_t>(rd), sd->id);
    if (sl.base) {
        // Record update `Name { …explicit…, ..base }`: fill EVERY declared slot -- an explicit field if
        // present, else a copy of the base's slot. `base` is evaluated ONCE into `rb`.
        int rb = compile_expr(*sl.base);
        const int nslots = static_cast<int>(sd->field_names.size());
        for (int slot = 0; slot < nslots; ++slot) {
            const FieldInit* prov = nullptr;
            for (const FieldInit& fi : sl.fields) if (field_slot(*sd, fi.name) == slot) { prov = &fi; break; }
            int rv;
            if (prov && prov->value) {
                rv = (sd->field_double[slot] && is_int_ty(prov->value->ty)) ? compile_coerced(*prov->value, true)
                                                                            : compile_expr(*prov->value);
            } else if (prov) {                            // shorthand `{ x }` -> the local `x`
                rv = local_value_reg(prov->name, sl.line, sl.col);
                if (rv < 0) throw CodegenError("shorthand field '" + prov->name +
                                               "' has no matching local", sl.line, sl.col);
            } else {                                      // unlisted -> copy from the base
                rv = alloc_temp();
                as_.GET_PROP(static_cast<uint8_t>(rv), static_cast<uint8_t>(rb), static_cast<uint16_t>(slot));
            }
            as_.SET_PROP(static_cast<uint8_t>(rd), static_cast<uint16_t>(slot), static_cast<uint8_t>(rv));
            free_if_temp(rv);
        }
        free_if_temp(rb);
        return rd;
    }
    for (const FieldInit& fi : sl.fields) {
        const int slot = field_slot(*sd, fi.name);
        if (slot < 0) throw CodegenError("struct '" + sl.name + "' has no field '" + fi.name + "'",
                                         sl.line, sl.col);
        int rv;
        if (fi.value) {
            rv = (sd->field_double[slot] && is_int_ty(fi.value->ty)) ? compile_coerced(*fi.value, true)
                                                                     : compile_expr(*fi.value);
        } else {                                     // shorthand `Name { x }` -> the local `x`
            rv = local_value_reg(fi.name, sl.line, sl.col);
            if (rv < 0) throw CodegenError("shorthand field '" + fi.name +
                                           "' has no matching local", sl.line, sl.col);
        }
        as_.SET_PROP(static_cast<uint8_t>(rd), static_cast<uint16_t>(slot), static_cast<uint8_t>(rv));
        free_if_temp(rv);
    }
    return rd;
}

int Codegen::compile_ctor_call(const std::string& name, const std::vector<const Expr*>& args) {
    const StructDef* sd = find_struct(name);
    if (!sd) throw CodegenError("unknown constructor '" + name + "'");
    if (sd->is_int_enum) {                               // erased nullary variant -> the Int discriminant
        int t = alloc_temp();                            // (defensive: nullary variants normally take the
        as_.load_const(static_cast<uint8_t>(t), sd->enum_disc);   // bare-Ident path, never a call)
        return t;
    }
    if (sd->is_transparent && args.size() == 1) {
        // Erased newtype: the value IS the single field's immediate -- no NEW_STRUCT, no SET_PROP.
        // Honor the Int->Double coercion when the field is Double (e.g. `Meters(5)`).
        const bool dbl = !sd->field_double.empty() && sd->field_double[0];
        return (dbl && is_int_ty(args[0]->ty)) ? compile_coerced(*args[0], true)
                                               : compile_expr(*args[0]);
    }
    int rd = alloc_temp();
    as_.NEW_STRUCT(static_cast<uint8_t>(rd), sd->id);
    for (size_t i = 0; i < args.size(); ++i) {
        const bool dbl = i < sd->field_double.size() && sd->field_double[i];
        int rv = (dbl && is_int_ty(args[i]->ty)) ? compile_coerced(*args[i], true)
                                                 : compile_expr(*args[i]);
        as_.SET_PROP(static_cast<uint8_t>(rd), static_cast<uint16_t>(i), static_cast<uint8_t>(rv));
        free_if_temp(rv);
    }
    return rd;
}

int Codegen::compile_tuple(const TupleExpr& t) {
    const int n = static_cast<int>(t.elems.size());
    if (n == 0) {                                    // `()` is Unit -> nil placeholder
        int rd = alloc_temp();
        as_.load_constant(static_cast<uint8_t>(rd), Value::fromNil());
        return rd;
    }
    const uint16_t id = ensure_tuple_type(n);
    int rd = alloc_temp();
    as_.NEW_STRUCT(static_cast<uint8_t>(rd), id);
    for (int i = 0; i < n; ++i) {
        int rv = compile_expr(*t.elems[i]);
        as_.SET_PROP(static_cast<uint8_t>(rd), static_cast<uint16_t>(i), static_cast<uint8_t>(rv));
        free_if_temp(rv);
    }
    return rd;
}

int Codegen::compile_field(const FieldExpr& fe) {
    if (fe.tuple_index) {                             // `t.N` -- slot N of the tuple
        // A transparent struct erases: `.0` is the value itself (no GET_PROP). (A regular named
        // tuple struct has no `.0` projection -- the checker rejects it as pattern-only.)
        const std::string tsn = struct_name_of(fe.obj->ty);
        if (const StructDef* tsd = tsn.empty() ? nullptr : find_struct(tsn); tsd && tsd->is_transparent)
            return compile_expr(*fe.obj);
        int robj = compile_expr(*fe.obj);
        int dst = is_temp(robj) ? robj : alloc_temp();
        as_.GET_PROP(static_cast<uint8_t>(dst), static_cast<uint8_t>(robj),
                     static_cast<uint16_t>(fe.index));
        return dst;
    }
    const std::string sn = struct_name_of(fe.obj->ty);
    const StructDef* sd = sn.empty() ? nullptr : find_struct(sn);
    if (!sd)
        throw CodegenError("cannot resolve field '" + fe.name +
                           "': the receiver's struct type is not statically known",
                           fe.line, fe.col);
    const int slot = field_slot(*sd, fe.name);
    if (slot < 0) throw CodegenError("struct '" + sn + "' has no field '" + fe.name + "'",
                                     fe.line, fe.col);
    // SROA: the receiver is a DISSOLVED binding, so the field already lives in a register of its
    // own -- no object to point at and no GET_PROP to emit. Returning the local's register
    // directly is exactly what compile_expr's Ident arm already does for an ordinary local, so
    // the aliasing rules are unchanged.
    //
    // Do NOT justify this by "a GET_PROP costs 8.77 ns". That price is INVALID: the ladder rung
    // that produced it is now the four-distinct-target dispatch cliff and reads 32.6 ns, and
    // `GET_PROP` is currently UNPRICED on this interpreter. Measured: SROA deletes 16 % of all
    // GET_PROPs on a demo/raytracer for ZERO wall-clock change.
    if (fe.obj->kind == ExprKind::Ident) {
        const auto& id = static_cast<const IdentExpr&>(*fe.obj);
        if (id.qualifier.empty())
            if (const Local* L = local_slot(id.name); L && !L->fields.empty())
                return L->fields[slot];
    }
    int robj = compile_expr(*fe.obj);
    int dst = is_temp(robj) ? robj : alloc_temp();
    as_.GET_PROP(static_cast<uint8_t>(dst), static_cast<uint8_t>(robj), static_cast<uint16_t>(slot));
    return dst;
}

// compile_block_stmts, but the TAIL is produced field-by-field into `regs`. No rescue MOV: the
// destinations belong to an OUTER frame region (the caller's `let` locals), so the reclaim below
// cannot reissue them -- unlike a value living in a register this block itself allocated.
void Codegen::compile_block_dissolved(const BlockExpr& b, const std::vector<int>& regs) {
    const size_t scope_mark  = scope_.size();
    const int    locals_mark = n_locals_;
    for (size_t i = 0; i < b.stmts.size(); ++i) {
        const Stmt& s = *b.stmts[i];
        if (i + 1 == b.stmts.size() && s.kind == StmtKind::Expr)
            compile_dissolved_or_explode(*static_cast<const ExprStmt&>(s).expr, regs);
        else
            compile_stmt(s);
    }
    scope_.resize(scope_mark);
    n_locals_ = locals_mark;
}

// Produce `e`'s struct value into `regs`, one field per register.
void Codegen::compile_dissolved_or_explode(const Expr& e, const std::vector<int>& regs) {
    set_pos_at(e);
    if (e.kind == ExprKind::StructLit) {
        const auto& sl = static_cast<const StructLit&>(e);
        const StructDef* sd = find_struct(sl.name);
        if (!sl.base && sd && sl.fields.size() == regs.size() &&
            sd->field_names.size() == regs.size()) {
            compile_into_dissolved(sl, regs);
            return;
        }
    }
    if (e.kind == ExprKind::Block) { compile_block_dissolved(static_cast<const BlockExpr&>(e), regs); return; }

    // Offer the destination to whatever expansion this expression turns out to be, then compile it
    // NORMALLY. If an expansion claims it (try_inline_call), the registers are already written and
    // there is nothing left to do. If nothing claims it -- a plain call, a shadowed callee, the
    // depth cap -- we get an ordinary object back and read the fields out of it ONCE.
    //
    // That fallback is what makes an optimistic pre-pass safe: EXPLODING costs exactly what the
    // un-dissolved program paid, so a permission emit cannot honour is never worse than no
    // permission at all.
    const std::vector<int>* saved_dst  = sroa_dst_;
    const Expr*             saved_site = sroa_dst_site_;
    const bool              saved_used = sroa_dst_used_;
    sroa_dst_ = &regs; sroa_dst_site_ = &e; sroa_dst_used_ = false;
    const int t = compile_expr(e);
    const bool claimed = sroa_dst_used_;
    sroa_dst_ = saved_dst; sroa_dst_site_ = saved_site; sroa_dst_used_ = saved_used;
    if (claimed) return;

    for (size_t i = 0; i < regs.size(); ++i)
        as_.GET_PROP(static_cast<uint8_t>(regs[i]), static_cast<uint8_t>(t), static_cast<uint16_t>(i));
    free_if_temp(t);
}

// The fields of `sl` straight into `regs` -- compile_struct_lit's plain path minus the NEW_STRUCT
// and every SET_PROP. SOURCE ORDER, deliberately: the record-update loop iterates by SLOT, and
// copying that shape here would silently reorder the initializers' side effects (and, on the
// measured workload, change floating-point results).
void Codegen::compile_into_dissolved(const StructLit& sl, const std::vector<int>& regs) {
    const StructDef* sd = find_struct(sl.name);
    if (!sd) throw CodegenError("unknown struct type '" + sl.name + "'", sl.line, sl.col);
    for (const FieldInit& fi : sl.fields) {
        const int slot = field_slot(*sd, fi.name);
        if (slot < 0 || slot >= static_cast<int>(regs.size()))
            throw CodegenError("struct '" + sl.name + "' has no field '" + fi.name + "'",
                               sl.line, sl.col);
        const int dst = regs[slot];
        if (!fi.value) {                                   // shorthand `S { x }` -> the local `x`
            const int rv = local_value_reg(fi.name, sl.line, sl.col);
            if (rv < 0) throw CodegenError("shorthand field '" + fi.name + "' has no matching local",
                                           sl.line, sl.col);
            if (rv != dst) as_.R6(OpCode::MOV, static_cast<uint8_t>(dst), static_cast<uint8_t>(rv), 0);
            continue;
        }
        if (sd->field_double[slot] && is_int_ty(fi.value->ty)) {
            const int r = compile_coerced(*fi.value, true);
            if (r != dst) as_.R6(OpCode::MOV, static_cast<uint8_t>(dst), static_cast<uint8_t>(r), 0);
            free_if_temp(r);
        } else {
            compile_into(*fi.value, dst);
        }
    }
}

void Codegen::compile_field_assign(const FieldExpr& target, const Expr& value, TokKind op) {
    int slot;
    bool dbl = false;
    if (target.tuple_index) {
        slot = static_cast<int>(target.index);
    } else {
        const std::string sn = struct_name_of(target.obj->ty);
        const StructDef* sd = sn.empty() ? nullptr : find_struct(sn);
        if (!sd)
            throw CodegenError("cannot resolve field '" + target.name +
                               "': the receiver's struct type is not statically known",
                               target.line, target.col);
        slot = field_slot(*sd, target.name);
        if (slot < 0) throw CodegenError("struct '" + sn + "' has no field '" + target.name + "'",
                                         target.line, target.col);
        dbl = sd->field_double[slot] != 0;
    }
    int robj = compile_expr(*target.obj);
    // A compound `obj.f op= v` is a read-modify-write on ONE evaluation of the receiver: `robj` is
    // computed above and reused for both GET_PROP and SET_PROP, so a side-effecting receiver
    // expression runs exactly once.
    if (op != TokKind::Assign) {
        int rold = alloc_temp();
        as_.GET_PROP(static_cast<uint8_t>(rold), static_cast<uint8_t>(robj), static_cast<uint16_t>(slot));
        int rv = dbl ? compile_coerced(value, true) : compile_expr(value);
        as_.R6(arith_opcode(op, dbl), static_cast<uint8_t>(rold),
               static_cast<uint8_t>(rold), static_cast<uint8_t>(rv));
        as_.SET_PROP(static_cast<uint8_t>(robj), static_cast<uint16_t>(slot), static_cast<uint8_t>(rold));
        free_if_temp(rv);
        free_if_temp(rold);
        free_if_temp(robj);
        return;
    }
    int rv = (dbl && is_int_ty(value.ty)) ? compile_coerced(value, true) : compile_expr(value);
    as_.SET_PROP(static_cast<uint8_t>(robj), static_cast<uint16_t>(slot), static_cast<uint8_t>(rv));
    free_if_temp(rv);
    free_if_temp(robj);
}

// ----- match + patterns ------------------------------------------------------

// One pass over a pattern yielding BOTH quantities: `.binding_locals` (fresh locals its bindings
// need -- an Ident sub-pattern binds 1, a nested pattern recurses) and `.test_temps` (peak
// test-temporaries; a safe upper bound -- every sub-pattern is treated as a nested container holding
// a fetched temp above the test's own base, so over-reserving is harmless).
//
// The names a pattern binds, in a DETERMINISTIC order (first occurrence wins). Registers are handed
// out in this order for an or-pattern's shared bindings, so an `unordered_set` would make register
// assignment vary between runs. `add_pattern_names` below delegates to this rather than repeating
// the walk -- two copies of it would drift, and this one carries the 4062 rot-guard.
#pragma warning(push)
#pragma warning(error: 4062)
void collect_pattern_names(const Pattern& p, std::vector<std::string>& out) {
    auto add = [&out](const std::string& n) {
        for (const auto& e : out) if (e == n) return;
        out.push_back(n);
    };
    switch (p.kind) {
    case PatKind::Ident: add(static_cast<const IdentPat&>(p).name); break;
    case PatKind::Bind: {   // `n @ sub` binds `n` AND whatever `sub` binds
        const auto& bp = static_cast<const BindPat&>(p);
        add(bp.name);
        if (bp.sub) collect_pattern_names(*bp.sub, out);
        break;
    }
    case PatKind::Ctor:
        for (const auto& e : static_cast<const CtorPat&>(p).elems) collect_pattern_names(*e, out);
        break;
    case PatKind::Tuple:
        for (const auto& e : static_cast<const TuplePat&>(p).elems) collect_pattern_names(*e, out);
        break;
    case PatKind::List: {
        const auto& lp = static_cast<const ListPat&>(p);
        for (const auto& e : lp.elems) collect_pattern_names(*e, out);
        if (lp.rest) collect_pattern_names(*lp.rest, out);
        break;
    }
    case PatKind::Struct:
        for (const auto& fp : static_cast<const StructPat&>(p).fields)
            if (fp.pat) collect_pattern_names(*fp.pat, out);
            else        add(fp.name);              // shorthand `P { x }` binds `x`
        break;
    case PatKind::Map:
        for (const auto& kv : static_cast<const MapPat&>(p).entries)
            if (kv.second) collect_pattern_names(*kv.second, out);
        break;
    case PatKind::Or:
        // Every alternative binds the same names (the checker enforces it), so one pass over all of
        // them with first-occurrence-wins yields exactly that set.
        for (const auto& a : static_cast<const OrPat&>(p).alts) collect_pattern_names(*a, out);
        break;
    case PatKind::Wildcard:
    case PatKind::Literal:
    case PatKind::Range:
        break;            // bind nothing
    }
}
#pragma warning(pop)

// EXHAUSTIVE (no `default:`) under warning-as-error 4062: this is one of the two pattern walkers
// where a new Ast.h kind used to fail SILENTLY -- a wrapper node would have reported {0,0}, the
// frame would be under-measured, and the emitter self-check catches that only when the under-count
// happens to cross `temp_base_`. A build break is the only reliable guard here.
#pragma warning(push)
#pragma warning(error: 4062)
Codegen::PatNeed Codegen::measure_pattern(const Pattern& pat) const {
    switch (pat.kind) {
    // Both are a flat `{1, 0}` and neither walks its operand -- sound ONLY because the parser
    // admits nothing but a literal leaf or a `const` NAME there (Parser::parse_pattern_literal /
    // parse_range_bound), and both lower to a single register. That invariant is load-bearing, not
    // incidental: while `parse_prefix` was used here, `-f(3)` and `1..(g(1,2,3,4))` parsed, and
    // their argument temps were never budgeted -- a frame under-measure, i.e. either a CodegenError
    // or a silently wrong register read. Widen what a bound may be and this must become a real walk.
    case PatKind::Literal: return { 1, 0 };
    case PatKind::Range:   return { 1, 0 };   // one bound register live at a time (tested sequentially)
    case PatKind::Ctor: {
        const auto& cp = static_cast<const CtorPat&>(pat);
        int tt = 2, bl = 0;                                       // type-id discrimination
        for (const auto& e : cp.elems) {
            PatNeed s = measure_pattern(*e);
            tt  = imax(tt, 1 + s.test_temps);
            bl += (e->kind == PatKind::Ident) ? 1 : s.binding_locals;
        }
        return { tt, bl };
    }
    case PatKind::Struct: {
        const auto& sp = static_cast<const StructPat&>(pat);
        int tt = 2, bl = 0;
        for (const auto& fp : sp.fields) {
            if (!fp.pat) { ++bl; continue; }                      // shorthand binds the field
            PatNeed s = measure_pattern(*fp.pat);
            tt  = imax(tt, 1 + s.test_temps);
            bl += (fp.pat->kind == PatKind::Ident) ? 1 : s.binding_locals;
        }
        return { tt, bl };
    }
    case PatKind::Tuple: {
        const auto& tp = static_cast<const TuplePat&>(pat);
        int tt = 2, bl = 0;
        for (const auto& e : tp.elems) {
            PatNeed s = measure_pattern(*e);
            tt  = imax(tt, 1 + s.test_temps);
            bl += (e->kind == PatKind::Ident) ? 1 : s.binding_locals;
        }
        return { tt, bl };
    }
    case PatKind::List: {
        const auto& lp = static_cast<const ListPat&>(pat);
        int tt = 3, bl = 0;                                       // spine cursor + cell discrimination
        for (const auto& e : lp.elems) {
            PatNeed s = measure_pattern(*e);
            tt  = imax(tt, 2 + s.test_temps);
            bl += (e->kind == PatKind::Ident) ? 1 : s.binding_locals;
        }
        if (lp.rest && lp.rest->kind == PatKind::Ident) ++bl;
        return { tt, bl };
    }
    case PatKind::Map: {
        const auto& mp = static_cast<const MapPat&>(pat);
        int tt = 3, bl = 0;
        for (const auto& kv : mp.entries) {
            PatNeed s = measure_pattern(*kv.second);
            tt  = imax(tt, 2 + s.test_temps);
            bl += (kv.second->kind == PatKind::Ident) ? 1 : s.binding_locals;
        }
        return { tt, bl };
    }
    case PatKind::Or: {
        // Alternatives are tested SEQUENTIALLY, each freeing its temps before the next, so the peak
        // is the MAX (not the sum). v1a forbids bindings in alternatives, so binding_locals is 0.
        const auto& op = static_cast<const OrPat&>(pat);
        int tt = 0, bl = 0;
        for (const auto& a : op.alts) {
            PatNeed s = measure_pattern(*a);
            tt = imax(tt, s.test_temps);
            bl = imax(bl, s.binding_locals);   // MAX: the alternatives' own locals are reclaimed
        }                                      // between them (n_locals_ is reset per alternative)
        // Plus one SHARED register per distinct bound name -- every alternative copies into these,
        // and they are what the guard and the arm body read. The count CANNOT be taken from an
        // alternative's `binding_locals`: the Ident case binds a name while allocating ZERO
        // registers (it aliases `rs`), so that number counts registers, not names.
        std::vector<std::string> names;
        collect_pattern_names(pat, names);
        return { tt, static_cast<int>(names.size()) + bl };
    }
    case PatKind::Bind: {
        // `n @ sub`: one local for `n` (always -- compile_pattern_test copies rather than aliases,
        // see there) plus whatever `sub` needs. Position-independent precisely BECAUSE the emit
        // never aliases: there is no "top level is free, nested costs one" case split to mirror here.
        const auto& bp = static_cast<const BindPat&>(pat);
        PatNeed s = bp.sub ? measure_pattern(*bp.sub) : PatNeed{ 0, 0 };
        return { s.test_temps, 1 + s.binding_locals };
    }
    case PatKind::Wildcard:                       // binds nothing, tests nothing
    case PatKind::Ident:                          // whole-value alias -- no register of its own
        return { 0, 0 };
    }
    return { 0, 0 };   // unreachable: the switch is exhaustive, enforced by 4062-as-error
}
#pragma warning(pop)

void Codegen::compile_pattern_test(const Pattern& pat, int rs, const std::string& scrut_ty,
                                   const std::string& fail_lbl) {
    switch (pat.kind) {
    case PatKind::Wildcard:
        return;
    case PatKind::Ident:                                          // bind the whole scrutinee (aliases rs)
        scope_.push_back(Local{ static_cast<const IdentPat&>(pat).name, rs, false });
        return;
    case PatKind::Bind: {
        // `n @ sub` -- ALWAYS copy into a fresh local, never alias `rs`, and run `sub` against the
        // COPY. Two hazards, both closed by that one rule rather than by reasoning about callers:
        //   * `rs` may be a temp the caller frees the instant this test returns
        //     (compile_field_pattern's default, compile_list_pattern, compile_map_pattern all fetch
        //     into one), so an alias would go stale as soon as the arm body allocates -- the very
        //     bug the transparent-newtype branch above had;
        //   * `sub` may itself be an Ident (`n @ m` is legal), which WOULD alias `rs` one level down.
        // Recursing against the local closes both. Cost is one MOV, and it is what lets
        // measure_pattern report a position-independent `1 + sub`.
        const auto& bp = static_cast<const BindPat&>(pat);
        const int reg = alloc_local();
        as_.R6(OpCode::MOV, static_cast<uint8_t>(reg), static_cast<uint8_t>(rs), 0);
        scope_.push_back(Local{ bp.name, reg, bp.is_mut });
        if (bp.sub) compile_pattern_test(*bp.sub, reg, scrut_ty, fail_lbl);
        return;
    }
    case PatKind::Literal: {
        int rl = compile_expr(*static_cast<const LiteralPat&>(pat).lit);
        int rc = is_temp(rl) ? rl : alloc_temp();
        as_.R6(OpCode::EQ, static_cast<uint8_t>(rc), static_cast<uint8_t>(rs), static_cast<uint8_t>(rl));
        as_.B1(OpCode::BF, static_cast<uint8_t>(rc), fail_lbl);
        free_if_temp(rc);
        return;
    }
    case PatKind::Range: {                    // `lo..hi` inclusive / `lo..<hi` exclusive upper bound
        const auto& rp = static_cast<const RangePat&>(pat);
        int rlo = compile_expr(*rp.lo);                          // fail if rs < lo
        as_.B(OpCode::BLT_NUM, static_cast<uint8_t>(rs), static_cast<uint8_t>(rlo), fail_lbl);
        free_if_temp(rlo);
        int rhi = compile_expr(*rp.hi);          // fail if rs > hi (inclusive) / rs >= hi (exclusive)
        as_.B(rp.exclusive ? OpCode::BGE_NUM : OpCode::BGT_NUM,
              static_cast<uint8_t>(rs), static_cast<uint8_t>(rhi), fail_lbl);
        free_if_temp(rhi);
        return;
    }
    case PatKind::Ctor: {                                         // enum variant / tuple struct
        const auto& cp = static_cast<const CtorPat&>(pat);
        const StructDef* sd = find_struct(cp.name);
        if (!sd) throw CodegenError("unknown constructor '" + cp.name + "' in pattern", pat.line, pat.col);
        if (sd->is_transparent && cp.elems.size() == 1) {        // erased: the ctor wrapper vanishes
            const Pattern& sub = *cp.elems[0];
            if (sub.kind == PatKind::Ident) {
                // Copy into a FRESH LOCAL rather than aliasing `rs`. Every nesting parent
                // (compile_field_pattern's default, compile_list_pattern, compile_map_pattern)
                // fetches into a TEMP and frees it right after this test, so the plain Ident case
                // below -- which binds by register alias -- would leave the name pointing at a
                // register the arm body immediately reallocates: a silent wrong VALUE, invisible to
                // the emitter self-check because the measure already reserves this local
                // (measure_pattern's Ctor case counts an Ident element as 1). bind_let_pattern's
                // transparent branch has always done it this way; this mirrors it.
                int reg = alloc_local();
                as_.R6(OpCode::MOV, static_cast<uint8_t>(reg), static_cast<uint8_t>(rs), 0);
                scope_.push_back(Local{ static_cast<const IdentPat&>(sub).name, reg, false });
                return;
            }
            compile_pattern_test(sub, rs, std::string(), fail_lbl);   // match the inner pattern on rs
            return;
        }
        if (sd->is_int_enum) {                                   // erased: rs IS the Int discriminant
            int rc = alloc_temp(); as_.load_const(static_cast<uint8_t>(rc), sd->enum_disc);
            as_.B(OpCode::BNE_INT, static_cast<uint8_t>(rs), static_cast<uint8_t>(rc), fail_lbl);
            free_if_temp(rc);
            return;                                              // nullary -> no sub-bindings
        }
        if (scrut_ty != cp.name) {                               // discriminate the variant at run time
            int rt = alloc_temp(); as_.GET_TYPE_ID(static_cast<uint8_t>(rt), static_cast<uint8_t>(rs));
            int rc = alloc_temp(); as_.load_const(static_cast<uint8_t>(rc), static_cast<int64_t>(sd->id));
            as_.B(OpCode::BNE_INT, static_cast<uint8_t>(rt), static_cast<uint8_t>(rc), fail_lbl);
            free_if_temp(rc); free_if_temp(rt);
        }
        for (size_t i = 0; i < cp.elems.size(); ++i)
            compile_field_pattern(*cp.elems[i], rs, static_cast<int>(i), fail_lbl);
        return;
    }
    case PatKind::Tuple: {                                        // anonymous tuple -- no discrimination
        const auto& tp = static_cast<const TuplePat&>(pat);
        for (size_t i = 0; i < tp.elems.size(); ++i)
            compile_field_pattern(*tp.elems[i], rs, static_cast<int>(i), fail_lbl);
        return;
    }
    case PatKind::Struct: {                                       // record struct / record variant
        const auto& sp = static_cast<const StructPat&>(pat);
        const StructDef* sd = find_struct(sp.name);
        if (!sd) throw CodegenError("unknown struct type '" + sp.name + "' in pattern", pat.line, pat.col);
        if (scrut_ty != sp.name) {                               // discriminate (a record enum variant)
            int rt = alloc_temp(); as_.GET_TYPE_ID(static_cast<uint8_t>(rt), static_cast<uint8_t>(rs));
            int rc = alloc_temp(); as_.load_const(static_cast<uint8_t>(rc), static_cast<int64_t>(sd->id));
            as_.B(OpCode::BNE_INT, static_cast<uint8_t>(rt), static_cast<uint8_t>(rc), fail_lbl);
            free_if_temp(rc); free_if_temp(rt);
        }
        for (const FieldPat& fp : sp.fields) {
            const int slot = field_slot(*sd, fp.name);
            if (slot < 0) throw CodegenError("struct '" + sp.name + "' has no field '" + fp.name + "'",
                                             pat.line, pat.col);
            if (!fp.pat) {                                        // shorthand -> bind local `name`
                int reg = alloc_local();
                as_.GET_PROP(static_cast<uint8_t>(reg), static_cast<uint8_t>(rs), static_cast<uint16_t>(slot));
                scope_.push_back(Local{ fp.name, reg, false });
            } else {
                compile_field_pattern(*fp.pat, rs, slot, fail_lbl);
            }
        }
        return;
    }
    case PatKind::List:
        compile_list_pattern(static_cast<const ListPat&>(pat), rs, fail_lbl);
        return;
    case PatKind::Map:
        compile_map_pattern(static_cast<const MapPat&>(pat), rs, fail_lbl);
        return;
    case PatKind::Or: {                                          // `A | B | C`, bindings allowed
        const auto& op = static_cast<const OrPat&>(pat);
        const std::string matched = fresh_label("por_ok_");

        // Every alternative jumps to ONE label, so they must agree on where each bound name lives.
        // They are given SHARED ("canonical") registers, allocated once here; each alternative binds
        // into its own registers exactly as it does everywhere else and copies into the canonical
        // ones right before it jumps. No binding call site is touched -- pushing the target down
        // into all five of them is what strategy A would need, and the SROA post-mortem's lesson is
        // to make the fallback total rather than to enumerate the sites that must cooperate.
        std::vector<std::string> names;
        collect_pattern_names(pat, names);
        std::vector<int> canon;
        canon.reserve(names.size());
        for (size_t k = 0; k < names.size(); ++k) canon.push_back(alloc_local());
        const int canon_end = n_locals_;             // reclaim point BETWEEN alternatives
        std::vector<bool> canon_mut(names.size(), false);

        const size_t scope_mark = scope_.size();

        // Copy one alternative's bindings into the canonical registers. Searches `scope_` in REVERSE
        // and stops at `scope_mark`: `local_reg` would happily return an ENCLOSING local of the same
        // name and copy the wrong value with no diagnostic, and reverse rather than forward because
        // a pattern may bind one name twice (`define` overwrites silently) and the LAST binding is
        // the one the arm body sees.
        auto normalize = [&](const Pattern& alt) {
            for (size_t k = 0; k < names.size(); ++k) {
                const Local* src = nullptr;
                for (size_t j = scope_.size(); j-- > scope_mark; )
                    if (scope_[j].name == names[k]) { src = &scope_[j]; break; }
                if (!src)
                    throw CodegenError("internal: an or-pattern alternative does not bind '" +
                                       names[k] + "' -- the checker should have rejected this",
                                       alt.line, alt.col);
                if (src->is_mut) canon_mut[k] = true;
                as_.R6(OpCode::MOV, static_cast<uint8_t>(canon[k]), static_cast<uint8_t>(src->reg), 0);
            }
        };

        // The emit ORDER is load-bearing and every wrong version still compiles: the MOVs must run
        // before the scope unwind (their sources are found through `scope_`), before any label (or
        // they land on the failure path), and before the `n_locals_` reset (or the reset hands their
        // source registers out again). The last alternative differs only in that its failure goes to
        // `fail_lbl` and it falls through into `matched` instead of jumping.
        for (size_t i = 0; i < op.alts.size(); ++i) {
            const bool last = (i + 1 == op.alts.size());
            const std::string next = last ? fail_lbl : fresh_label("por_alt_");
            compile_pattern_test(*op.alts[i], rs, scrut_ty, next);
            normalize(*op.alts[i]);
            n_locals_ = canon_end;      // an alternative's own locals are dead once copied out;
                                        // reclaiming makes the frame cost `canonical + MAX`, not sum
            if (!last) { as_.J(OpCode::J, matched); as_.label(next); }
            scope_.resize(scope_mark);  // a failed alternative's names must not leak into the next,
        }                               // and the last one's must not leak into the arm

        as_.label(matched);
        // Only the canonical names reach the guard and the arm body. Constructed FRESH rather than
        // copied from an alternative's Local: a copy would carry an SROA `fields` vector and make
        // `local_value_reg`'s dissolved-binding throw reachable for no reason.
        for (size_t k = 0; k < names.size(); ++k)
            scope_.push_back(Local{ names[k], canon[k], canon_mut[k] });
        return;
    }
    }
}

void Codegen::compile_field_pattern(const Pattern& pat, int rs, int slot,
                                    const std::string& fail_lbl) {
    switch (pat.kind) {
    case PatKind::Wildcard:
        return;
    case PatKind::Ident: {
        int reg = alloc_local();
        as_.GET_PROP(static_cast<uint8_t>(reg), static_cast<uint8_t>(rs), static_cast<uint16_t>(slot));
        scope_.push_back(Local{ static_cast<const IdentPat&>(pat).name, reg, false });
        return;
    }
    case PatKind::Literal: {
        int rf = alloc_temp();
        as_.GET_PROP(static_cast<uint8_t>(rf), static_cast<uint8_t>(rs), static_cast<uint16_t>(slot));
        int rl = compile_expr(*static_cast<const LiteralPat&>(pat).lit);
        as_.R6(OpCode::EQ, static_cast<uint8_t>(rf), static_cast<uint8_t>(rf), static_cast<uint8_t>(rl));
        as_.B1(OpCode::BF, static_cast<uint8_t>(rf), fail_lbl);
        free_if_temp(rl);
        free_if_temp(rf);
        return;
    }
    default: {                                                   // nested Ctor / Tuple / Struct / List / Map
        int rf = alloc_temp();
        as_.GET_PROP(static_cast<uint8_t>(rf), static_cast<uint8_t>(rs), static_cast<uint16_t>(slot));
        compile_pattern_test(pat, rf, std::string(), fail_lbl);
        free_if_temp(rf);
        return;
    }
    }
}

// Bind an IRREFUTABLE let pattern whose value is already in register `rs` (checker-proven to have
// this exact shape). Reads each field via GET_PROP -- an ident leaf straight into a fresh local, a
// nested aggregate through a temp -- with NO type discrimination and NO fail path (contrast the
// `match` binders). Every binding inherits the `let`'s `mut`. GET_PROP never allocates, so no GC
// safepoint splits the read sequence and `rs` stays frame-rooted throughout.
//
// Exhaustive under 4062-as-error: falling out of this switch binds NOTHING and returns quietly.
#pragma warning(push)
#pragma warning(error: 4062)
void Codegen::bind_let_pattern(const Pattern& pat, int rs, bool is_mut) {
    auto bind_field = [&](const Pattern& sub, int slot) {
        if (sub.kind == PatKind::Wildcard) return;                       // discard
        if (sub.kind == PatKind::Ident) {                                // GET_PROP -> fresh local
            int reg = alloc_local();
            as_.GET_PROP(static_cast<uint8_t>(reg), static_cast<uint8_t>(rs), static_cast<uint16_t>(slot));
            scope_.push_back(Local{ static_cast<const IdentPat&>(sub).name, reg, is_mut });
            return;
        }
        int rf = alloc_temp();                                           // nested: read + recurse
        as_.GET_PROP(static_cast<uint8_t>(rf), static_cast<uint8_t>(rs), static_cast<uint16_t>(slot));
        bind_let_pattern(sub, rf, is_mut);
        free_if_temp(rf);
    };
    switch (pat.kind) {
    case PatKind::Tuple: {
        const auto& tp = static_cast<const TuplePat&>(pat);
        for (size_t i = 0; i < tp.elems.size(); ++i) bind_field(*tp.elems[i], static_cast<int>(i));
        return;
    }
    case PatKind::Ctor: {                                                // tuple-struct (single ctor)
        const auto& cp = static_cast<const CtorPat&>(pat);
        if (const StructDef* tsd = find_struct(cp.name); tsd && tsd->is_transparent && cp.elems.size() == 1) {
            // Erased newtype in an irrefutable let: the value IS the field. Copy it into a fresh
            // local (no GET_PROP) so the binding is stable independent of the RHS register.
            const Pattern& sub = *cp.elems[0];
            if (sub.kind == PatKind::Ident) {
                int reg = alloc_local();
                as_.R6(OpCode::MOV, static_cast<uint8_t>(reg), static_cast<uint8_t>(rs), 0);
                scope_.push_back(Local{ static_cast<const IdentPat&>(sub).name, reg, is_mut });
            }   // Wildcard: nothing to bind
            return;
        }
        for (size_t i = 0; i < cp.elems.size(); ++i) bind_field(*cp.elems[i], static_cast<int>(i));
        return;
    }
    case PatKind::Struct: {
        const auto& sp = static_cast<const StructPat&>(pat);
        const StructDef* sd = find_struct(sp.name);
        if (!sd) throw CodegenError("internal: unknown struct '" + sp.name + "' in let pattern",
                                    pat.line, pat.col);
        for (const FieldPat& fp : sp.fields) {
            const int slot = field_slot(*sd, fp.name);
            if (slot < 0) throw CodegenError("internal: struct '" + sp.name + "' has no field '" +
                                             fp.name + "'", pat.line, pat.col);
            if (!fp.pat) {                                               // shorthand `{x}` binds `x`
                int reg = alloc_local();
                as_.GET_PROP(static_cast<uint8_t>(reg), static_cast<uint8_t>(rs), static_cast<uint16_t>(slot));
                scope_.push_back(Local{ fp.name, reg, is_mut });
            } else {
                bind_field(*fp.pat, slot);
            }
        }
        return;
    }
    // Enumerated rather than caught by a `default:`, so a new Ast.h kind must be classified here
    // deliberately. Wildcard/Ident never arrive (compile_stmt and `bind_field` handle them before
    // recursing); the rest are refutable and the checker rejects them upstream.
    case PatKind::Wildcard:
    case PatKind::Ident:
    case PatKind::Literal:
    case PatKind::Range:
    case PatKind::List:
    case PatKind::Map:
    case PatKind::Or:
    case PatKind::Bind:          // rejected in `let` by the checker (an '@' there is pure noise)
        throw CodegenError("internal: refutable pattern reached the irrefutable let binder",
                           pat.line, pat.col);
    }
}
#pragma warning(pop)

void Codegen::compile_list_pattern(const ListPat& lp, int rs, const std::string& fail_lbl) {
    const uint16_t cid = ensure_cons_type();
    if (lp.elems.empty() && !lp.rest) {                          // `[]` -> the scrutinee must be nil
        int t = alloc_temp();
        as_.R6(OpCode::IS_NIL, static_cast<uint8_t>(t), static_cast<uint8_t>(rs), 0);
        as_.B1(OpCode::BF, static_cast<uint8_t>(t), fail_lbl);
        free_if_temp(t);
        return;
    }
    int cur = alloc_temp();                                      // spine cursor (held throughout)
    as_.R6(OpCode::MOV, static_cast<uint8_t>(cur), static_cast<uint8_t>(rs), 0);
    for (const PatPtr& el : lp.elems) {
        int rt = alloc_temp(); as_.GET_TYPE_ID(static_cast<uint8_t>(rt), static_cast<uint8_t>(cur));
        int rc = alloc_temp(); as_.load_const(static_cast<uint8_t>(rc), static_cast<int64_t>(cid));
        as_.B(OpCode::BNE_INT, static_cast<uint8_t>(rt), static_cast<uint8_t>(rc), fail_lbl);  // nil/too-short
        free_if_temp(rc); free_if_temp(rt);
        compile_field_pattern(*el, cur, 0, fail_lbl);            // head = cur.slot0
        as_.GET_PROP(static_cast<uint8_t>(cur), static_cast<uint8_t>(cur), 1);   // advance to tail
    }
    if (lp.rest) {
        if (lp.rest->kind == PatKind::Ident) {                  // bind `..rest` to the remaining tail
            int reg = alloc_local();
            as_.R6(OpCode::MOV, static_cast<uint8_t>(reg), static_cast<uint8_t>(cur), 0);
            scope_.push_back(Local{ static_cast<const IdentPat&>(*lp.rest).name, reg, false });
        } else if (lp.rest->kind != PatKind::Wildcard) {
            throw CodegenError("a list `..rest` pattern must be an identifier or `_`", lp.line, lp.col);
        }
    } else {                                                    // no rest -> the tail must be nil
        int t = alloc_temp();
        as_.R6(OpCode::IS_NIL, static_cast<uint8_t>(t), static_cast<uint8_t>(cur), 0);
        as_.B1(OpCode::BF, static_cast<uint8_t>(t), fail_lbl);
        free_if_temp(t);
    }
    free_if_temp(cur);
}

void Codegen::compile_map_pattern(const MapPat& mp, int rs, const std::string& fail_lbl) {
    // Partial match: the scrutinee must be a map that HAS each literal key, whose value
    // matches the sub-pattern; other keys are ignored. A receiver guard is unnecessary --
    // the checker types the scrutinee as a Map -- so no GET_KIND check.
    for (const auto& kv : mp.entries) {
        int rkey = compile_expr(*kv.first);                     // a literal key (checker-restricted)
        int rh = alloc_temp();
        as_.MAP_HAS(static_cast<uint8_t>(rh), static_cast<uint8_t>(rs), static_cast<uint8_t>(rkey));
        as_.B1(OpCode::BF, static_cast<uint8_t>(rh), fail_lbl); // key absent -> next arm
        free_if_temp(rh);
        const Pattern& vp = *kv.second;
        if (vp.kind == PatKind::Wildcard) {
            // presence-only
        } else if (vp.kind == PatKind::Ident) {
            int reg = alloc_local();
            as_.MAP_GET(static_cast<uint8_t>(reg), static_cast<uint8_t>(rs), static_cast<uint8_t>(rkey));
            scope_.push_back(Local{ static_cast<const IdentPat&>(vp).name, reg, false });
        } else {
            int rv = alloc_temp();
            as_.MAP_GET(static_cast<uint8_t>(rv), static_cast<uint8_t>(rs), static_cast<uint8_t>(rkey));
            compile_pattern_test(vp, rv, std::string(), fail_lbl);
            free_if_temp(rv);
        }
        free_if_temp(rkey);
    }
}

int Codegen::compile_match(const MatchExpr& m) {
    const std::string scrut_ty = struct_name_of(m.scrut->ty);
    const int locals_mark = n_locals_;
    int rs = alloc_local();
    compile_into(*m.scrut, rs);

    int result = alloc_temp();                                  // holds the match value across arms
    const int arm_locals_mark = n_locals_;
    const std::string end_lbl = fresh_label("mend_");

    for (const MatchArm& arm : m.arms) {
        const std::string next_lbl = fresh_label("marm_");
        const size_t scope_mark = scope_.size();
        compile_pattern_test(*arm.pat, rs, scrut_ty, next_lbl);
        if (arm.guard) branch_if_false(*arm.guard, next_lbl);
        int vb = arm.body ? compile_expr(*arm.body) : result;
        if (vb != result) as_.R6(OpCode::MOV, static_cast<uint8_t>(result), static_cast<uint8_t>(vb), 0);
        free_if_temp(vb);
        as_.J(OpCode::J, end_lbl);
        as_.label(next_lbl);
        scope_.resize(scope_mark);
        n_locals_ = arm_locals_mark;
    }
    as_.load_constant(static_cast<uint8_t>(result), Value::fromNil());   // unreachable (checker: exhaustive)
    as_.label(end_lbl);
    n_locals_ = locals_mark;
    return result;
}

// The value-discarded form of compile_match -- the same arm skeleton minus everything that exists
// only to PRODUCE a value: the result temp, the per-arm MOV into it, and the trailing unreachable
// Nil. Arm bodies recurse through compile_discard, which is what removes a statement-only arm
// block's OWN unit Nil -- a constant-pool load, and the expensive half of the pair.
//
// This reaches much further than a hand-written statement `match`: the parser desugars `if let` to a
// 2-arm match (a missing `else` becomes `()`, itself a Nil load, so BOTH paths paid) and `while let`
// to `loop { match e { PAT => BODY, _ => break } }`, where the match is the loop body's only
// statement and so paid PER ITERATION.
//
// The scrutinee local and all the locals/scope bookkeeping are kept verbatim -- the pattern tests
// read the scrutinee, and the arms' binding locals are unwound exactly as in the value path.
void Codegen::compile_match_discard(const MatchExpr& m) {
    const std::string scrut_ty = struct_name_of(m.scrut->ty);
    const int locals_mark = n_locals_;
    int rs = alloc_local();
    compile_into(*m.scrut, rs);

    const int arm_locals_mark = n_locals_;
    const std::string end_lbl = fresh_label("mend_");

    for (const MatchArm& arm : m.arms) {
        const std::string next_lbl = fresh_label("marm_");
        const size_t scope_mark = scope_.size();
        compile_pattern_test(*arm.pat, rs, scrut_ty, next_lbl);
        if (arm.guard) branch_if_false(*arm.guard, next_lbl);
        if (arm.body) compile_discard(*arm.body);   // a bodyless arm yields unit -- emit nothing
        as_.J(OpCode::J, end_lbl);
        as_.label(next_lbl);
        scope_.resize(scope_mark);
        n_locals_ = arm_locals_mark;
    }
    // No trailing Nil: the fall-through is unreachable (the checker proves exhaustiveness), and in
    // this position its register was never read anyway.
    as_.label(end_lbl);
    n_locals_ = locals_mark;
}

void Codegen::compile_tail_match(const MatchExpr& m) {
    const std::string scrut_ty = struct_name_of(m.scrut->ty);
    const int locals_mark = n_locals_;
    int rs = alloc_local();
    compile_into(*m.scrut, rs);
    const int arm_locals_mark = n_locals_;

    for (const MatchArm& arm : m.arms) {
        const std::string next_lbl = fresh_label("tmarm_");
        const size_t scope_mark = scope_.size();
        compile_pattern_test(*arm.pat, rs, scrut_ty, next_lbl);
        if (arm.guard) branch_if_false(*arm.guard, next_lbl);
        if (arm.body) compile_tail(*arm.body);                  // emits RET / TCO itself
        else          emit_return_nil();
        as_.label(next_lbl);
        scope_.resize(scope_mark);
        n_locals_ = arm_locals_mark;
    }
    emit_return_nil();                                          // unreachable (exhaustive)
    n_locals_ = locals_mark;
}

int Codegen::compile_try(const TryExpr& t) {
    // `e?` erases to: unwrap the Ok/Some payload, else `return <scrutinee>` (the Err/None value)
    // from the enclosing function. The operand's static type picks the success variant (Result ->
    // Ok, Option -> Some). Result has exactly Ok|Err, Option exactly Some|None, so "not the success
    // variant" IS the failure variant -- one GET_TYPE_ID compare decides it.
    const TyPtr& ot = t.operand->ty;
    // Recognize the carrier by SHORT name (the ring's std::core::Result OR a user's own `Result`),
    // and find the success variant in the SAME module as the enum (a variant shares its enum's
    // prefix) -- so `?` works on both the std Result/Option and a user-defined one.
    const bool is_result = ot && ot->kind == TyKind::Named && short_name(ot->name) == "Result";
    const std::string ok_name = ot ? mangle_name(module_prefix_of(ot->name), is_result ? "Ok" : "Some")
                                   : (is_result ? std_Ok() : std_Some());
    const StructDef* ok_sd = find_struct(ok_name);
    if (!ok_sd)
        throw CodegenError("internal: the '" + ok_name +
                           "' variant is not declared for '?'", t.line, t.col);

    int rs = compile_expr(*t.operand);                 // the scrutinee
    if (!is_temp(rs)) { int s = alloc_temp(); as_.R6(OpCode::MOV, static_cast<uint8_t>(s),
                                                     static_cast<uint8_t>(rs), 0); rs = s; }
    const std::string fail_lbl = fresh_label("qfail_");
    const std::string end_lbl  = fresh_label("qend_");
    int rt = alloc_temp(); as_.GET_TYPE_ID(static_cast<uint8_t>(rt), static_cast<uint8_t>(rs));
    int rc = alloc_temp(); as_.load_const(static_cast<uint8_t>(rc), static_cast<int64_t>(ok_sd->id));
    as_.B(OpCode::BNE_INT, static_cast<uint8_t>(rt), static_cast<uint8_t>(rc), fail_lbl);
    free_if_temp(rc);
    free_if_temp(rt);
    // Success: unwrap the payload (slot 0) into rs (reused -- rs is no longer needed here).
    as_.GET_PROP(static_cast<uint8_t>(rs), static_cast<uint8_t>(rs), 0);
    as_.J(OpCode::J, end_lbl);
    // Failure: return the Err/None scrutinee (still intact -- the branch skipped the GET_PROP).
    as_.label(fail_lbl);
    as_.R6(OpCode::MOV, static_cast<uint8_t>(return_target()), static_cast<uint8_t>(rs), 0);
    emit_return_jump();
    as_.label(end_lbl);
    return rs;
}

// ----- containers: List / Map literals + `for` -------------------------------

uint16_t Codegen::ensure_cons_type() {
    if (cons_type_id_ < 0)
        cons_type_id_ = static_cast<int>(as_.define_struct("$List", { "head", "tail" }, DumpStyle::List));
    return static_cast<uint16_t>(cons_type_id_);
}

int Codegen::compile_list_lit(const ListLit& ll) {
    const uint16_t cid = ensure_cons_type();
    int acc = alloc_temp();                                    // the list built so far (a GC root)
    as_.load_constant(static_cast<uint8_t>(acc), Value::fromNil());   // empty tail = nil
    for (size_t i = ll.elems.size(); i-- > 0;) {              // right-to-left: prepend each head
        int cell = alloc_temp();
        as_.NEW_STRUCT(static_cast<uint8_t>(cell), cid);
        int hv = compile_expr(*ll.elems[i]);
        as_.SET_PROP(static_cast<uint8_t>(cell), 0, static_cast<uint8_t>(hv));
        free_if_temp(hv);
        as_.SET_PROP(static_cast<uint8_t>(cell), 1, static_cast<uint8_t>(acc));   // tail = list so far
        as_.R6(OpCode::MOV, static_cast<uint8_t>(acc), static_cast<uint8_t>(cell), 0);
        free_if_temp(cell);
    }
    return acc;
}

int Codegen::compile_map_lit(const MapLit& ml) {
    int rd = alloc_temp();
    as_.MAP_NEW(static_cast<uint8_t>(rd));
    for (const auto& kv : ml.entries) {
        int rk = compile_expr(*kv.first);
        int rv = compile_expr(*kv.second);
        as_.MAP_SET(static_cast<uint8_t>(rd), static_cast<uint8_t>(rk), static_cast<uint8_t>(rv));
        free_if_temp(rv);                                     // LIFO: value above key
        free_if_temp(rk);
    }
    return rd;
}

int Codegen::for_pattern_var_count(const Pattern& p) const {
    if (p.kind == PatKind::Ident)    return 1;
    if (p.kind == PatKind::Wildcard) return 0;
    return measure_pattern(p).binding_locals;   // tuple / struct / tuple-struct: all leaf idents
}

void Codegen::bind_for_pattern(const Pattern& p, int r_elem, const std::vector<int>& slots) {
    size_t si = 0;
    bind_for_pattern_rec(p, r_elem, slots, si);
}

// Bind an IRREFUTABLE for-pattern whose value is in register `rs` into the pre-allocated,
// iteration-stable `slots` (checker-proven one-shape, like the destructuring-let binder). An ident
// LEAF consumes the next slot; a nested aggregate reads through a temp and recurses. No discrimination.
//
// Exhaustive under 4062-as-error, for the same reason as the `let` binder above.
#pragma warning(push)
#pragma warning(error: 4062)
void Codegen::bind_for_pattern_rec(const Pattern& p, int rs, const std::vector<int>& slots, size_t& si) {
    auto bind_field = [&](const Pattern& sub, int slot) {
        if (sub.kind == PatKind::Wildcard) return;                       // discard
        if (sub.kind == PatKind::Ident) {                                // GET_PROP -> stable slot
            const int reg = slots[si++];
            as_.GET_PROP(static_cast<uint8_t>(reg), static_cast<uint8_t>(rs), static_cast<uint16_t>(slot));
            scope_.push_back(Local{ static_cast<const IdentPat&>(sub).name, reg, false });
            return;
        }
        int rf = alloc_temp();                                           // nested: read + recurse
        as_.GET_PROP(static_cast<uint8_t>(rf), static_cast<uint8_t>(rs), static_cast<uint16_t>(slot));
        bind_for_pattern_rec(sub, rf, slots, si);
        free_if_temp(rf);
    };
    switch (p.kind) {
    case PatKind::Wildcard:
        return;
    case PatKind::Ident: {                                               // the whole element -> a stable slot
        const int reg = slots[si++];
        as_.R6(OpCode::MOV, static_cast<uint8_t>(reg), static_cast<uint8_t>(rs), 0);
        scope_.push_back(Local{ static_cast<const IdentPat&>(p).name, reg, false });
        return;
    }
    case PatKind::Tuple: {
        const auto& tp = static_cast<const TuplePat&>(p);
        for (size_t i = 0; i < tp.elems.size(); ++i) bind_field(*tp.elems[i], static_cast<int>(i));
        return;
    }
    case PatKind::Ctor: {                                                // tuple-struct (single ctor)
        const auto& cp = static_cast<const CtorPat&>(p);
        for (size_t i = 0; i < cp.elems.size(); ++i) bind_field(*cp.elems[i], static_cast<int>(i));
        return;
    }
    case PatKind::Struct: {
        const auto& sp = static_cast<const StructPat&>(p);
        const StructDef* sd = find_struct(sp.name);
        if (!sd) throw CodegenError("internal: unknown struct '" + sp.name + "' in for pattern",
                                    p.line, p.col);
        for (const FieldPat& fp : sp.fields) {
            const int slot = field_slot(*sd, fp.name);
            if (slot < 0) throw CodegenError("internal: struct '" + sp.name + "' has no field '" +
                                             fp.name + "'", p.line, p.col);
            if (!fp.pat) {                                               // shorthand `{x}` binds `x`
                const int reg = slots[si++];
                as_.GET_PROP(static_cast<uint8_t>(reg), static_cast<uint8_t>(rs), static_cast<uint16_t>(slot));
                scope_.push_back(Local{ fp.name, reg, false });
            } else {
                bind_field(*fp.pat, slot);
            }
        }
        return;
    }
    // Enumerated, not a `default:` -- see the note on the `let` binder above.
    case PatKind::Literal:
    case PatKind::Range:
    case PatKind::List:
    case PatKind::Map:
    case PatKind::Or:
    case PatKind::Bind:          // rejected in `for` by the checker, like in `let`
        throw CodegenError("internal: refutable pattern reached the irrefutable for binder", p.line, p.col);
    }
}
#pragma warning(pop)

int Codegen::compile_for(const ForExpr& f) {
    const TyPtr& it = f.iter->ty;
    const std::string kind = (it && it->kind == TyKind::Named) ? it->name : std::string();
    const int locals_mark = n_locals_;

    const int nvars = for_pattern_var_count(*f.pat);
    std::vector<int> slots;                                   // pre-allocated loop-var local slots
    for (int i = 0; i < nvars; ++i) slots.push_back(alloc_local());

    const std::string top   = fresh_label("for_");
    const std::string cont  = fresh_label("forc_");
    const std::string check = fresh_label("forck_");   // rotated (test-at-bottom) guard, emitted once
    const std::string end   = fresh_label("fore_");

    if (kind == "List") {
        int cur = alloc_local();                             // the cons cursor (a GC root local)
        compile_into(*f.iter, cur);
        const uint16_t cid = ensure_cons_type();
        loop_stack_.push_back(LoopCtx{ end, cont, -1 });   // a for never carries a break value
        as_.J(OpCode::J, check);                            // rotate: enter at the bottom guard
        as_.label(top);
        int head = alloc_temp();                            // `cur` is a cons here (guaranteed by check)
        as_.GET_PROP(static_cast<uint8_t>(head), static_cast<uint8_t>(cur), 0);
        const size_t scope_mark = scope_.size();
        bind_for_pattern(*f.pat, head, slots);
        free_if_temp(head);
        compile_discard(*f.body);                // the body's value is dropped -- build nothing
        scope_.resize(scope_mark);
        as_.label(cont);
        as_.GET_PROP(static_cast<uint8_t>(cur), static_cast<uint8_t>(cur), 1);   // cur = cur.tail
        as_.label(check);
        int kt = alloc_temp(); as_.GET_TYPE_ID(static_cast<uint8_t>(kt), static_cast<uint8_t>(cur));
        int kc = alloc_temp(); as_.load_const(static_cast<uint8_t>(kc), static_cast<int64_t>(cid));
        as_.B(OpCode::BEQ_INT, static_cast<uint8_t>(kt), static_cast<uint8_t>(kc), top);  // still a cons -> iterate
        free_if_temp(kc); free_if_temp(kt);
        as_.label(end);
        loop_stack_.pop_back();
    } else if (kind == "Map") {
        // No-copy live map walk: a cursor into the map's hash backing, advanced by MAP_ITER_NEXT
        // (skips empty/tombstone slots, yields -1 when exhausted); the key/value at each live slot
        // are read alloc-free by MAP_KEY_AT / MAP_VAL_AT. Replaces the old two-array snapshot.
        // `found` and `cur` are LOCALS so the cursor advance lives at the `cont` (continue) label.
        int mapl  = alloc_local(); compile_into(*f.iter, mapl);       // GC root
        int cur   = alloc_local(); as_.load_const(static_cast<uint8_t>(cur), 0);  // scan-start index
        int found = alloc_local();                                    // the live pair-index this iter
        int zero  = alloc_local(); as_.load_const(static_cast<uint8_t>(zero), 0); // for the j<0 test
        // A plain `(k, v)` tuple pattern with SIMPLE (ident/wildcard) elements binds key/value
        // directly (zero per-element alloc -- the churn win, and the overwhelmingly common form).
        // Any other shape (`for x in m`, or a nested destructure) materializes a $Tuple2 as before;
        // restricting the direct path to simple elements keeps its peak at 2 concurrent temps, so
        // the shared frame measure (base bound 3) covers it with no special-casing.
        auto simple_elem = [](const Pattern& p) {
            return p.kind == PatKind::Ident || p.kind == PatKind::Wildcard;
        };
        const bool direct = (f.pat->kind == PatKind::Tuple &&
                             static_cast<const TuplePat&>(*f.pat).elems.size() == 2 &&
                             simple_elem(*static_cast<const TuplePat&>(*f.pat).elems[0]) &&
                             simple_elem(*static_cast<const TuplePat&>(*f.pat).elems[1]));
        const uint16_t tid = direct ? 0 : ensure_tuple_type(2);
        loop_stack_.push_back(LoopCtx{ end, cont, -1 });   // a for never carries a break value
        as_.J(OpCode::J, check);                            // rotate: enter at the bottom guard
        as_.label(top);                                     // `found` is a live pair-index here (set by check)
        const size_t scope_mark = scope_.size();
        if (direct) {
            const auto& tp = static_cast<const TuplePat&>(*f.pat);
            int rk = alloc_temp(); as_.MAP_KEY_AT(static_cast<uint8_t>(rk), static_cast<uint8_t>(mapl), static_cast<uint8_t>(found));
            int rv = alloc_temp(); as_.MAP_VAL_AT(static_cast<uint8_t>(rv), static_cast<uint8_t>(mapl), static_cast<uint8_t>(found));
            size_t si = 0;
            bind_for_pattern_rec(*tp.elems[0], rk, slots, si);        // key   -> pattern element 0
            bind_for_pattern_rec(*tp.elems[1], rv, slots, si);        // value -> pattern element 1
            free_if_temp(rv); free_if_temp(rk);                       // LIFO
        } else {
            int pair = alloc_temp();
            as_.NEW_STRUCT(static_cast<uint8_t>(pair), tid);
            int rk = alloc_temp(); as_.MAP_KEY_AT(static_cast<uint8_t>(rk), static_cast<uint8_t>(mapl), static_cast<uint8_t>(found));
            as_.SET_PROP(static_cast<uint8_t>(pair), 0, static_cast<uint8_t>(rk));
            free_if_temp(rk);
            int rv = alloc_temp(); as_.MAP_VAL_AT(static_cast<uint8_t>(rv), static_cast<uint8_t>(mapl), static_cast<uint8_t>(found));
            as_.SET_PROP(static_cast<uint8_t>(pair), 1, static_cast<uint8_t>(rv));
            free_if_temp(rv);
            bind_for_pattern(*f.pat, pair, slots);
            free_if_temp(pair);
        }
        compile_discard(*f.body);                // the body's value is dropped -- build nothing
        scope_.resize(scope_mark);
        as_.label(cont);
        int one = alloc_temp(); as_.load_const(static_cast<uint8_t>(one), 1);
        as_.R6(OpCode::ADD_INT, static_cast<uint8_t>(cur), static_cast<uint8_t>(found), static_cast<uint8_t>(one));  // cur = found + 1
        free_if_temp(one);
        as_.label(check);
        as_.MAP_ITER_NEXT(static_cast<uint8_t>(found), static_cast<uint8_t>(mapl), static_cast<uint8_t>(cur));
        as_.B(OpCode::BGE_INT, static_cast<uint8_t>(found), static_cast<uint8_t>(zero), top);  // found >= 0 -> iterate
        as_.label(end);
        loop_stack_.pop_back();
    } else if (kind == "Array" || kind == "Vec" || kind == "Bytes") {
        // Snapshot-free index walk: the iterable in a local, an index counter, ARRAY_GET each
        // element (tri-kind -- an Array/Vec yields the element, a Bytes yields an Int 0..255).
        int arr = alloc_local(); compile_into(*f.iter, arr);     // GC root
        int len = alloc_local(); as_.LEN(static_cast<uint8_t>(len), static_cast<uint8_t>(arr));
        int idx = alloc_local(); as_.load_const(static_cast<uint8_t>(idx), 0);
        loop_stack_.push_back(LoopCtx{ end, cont, -1 });   // a for never carries a break value
        as_.J(OpCode::J, check);                            // rotate: enter at the bottom guard
        as_.label(top);                                     // idx < len here (guaranteed by check)
        int elem = alloc_temp();
        as_.ARRAY_GET(static_cast<uint8_t>(elem), static_cast<uint8_t>(arr), static_cast<uint8_t>(idx));
        const size_t scope_mark = scope_.size();
        bind_for_pattern(*f.pat, elem, slots);
        free_if_temp(elem);
        compile_discard(*f.body);                // the body's value is dropped -- build nothing
        scope_.resize(scope_mark);
        as_.label(cont);
        int one = alloc_temp(); as_.load_const(static_cast<uint8_t>(one), 1);
        as_.R6(OpCode::ADD_INT, static_cast<uint8_t>(idx), static_cast<uint8_t>(idx), static_cast<uint8_t>(one));
        free_if_temp(one);
        as_.label(check);
        as_.B(OpCode::BLT_INT, static_cast<uint8_t>(idx), static_cast<uint8_t>(len), top);   // idx < len -> iterate
        as_.label(end);
        loop_stack_.pop_back();
    } else if (iterator_protocol(it) != IterProto::None) {
        // Lazy protocol: drive next() to exhaustion. The cursor is a GC-root local; each iteration
        // dispatches `next` on it (a mut-self call that advances it in place) -> Option[T], matched
        // Some(elem) => bind+body / None => exit. No snapshot -- fully live, single-pass.
        const StructDef* none_sd = find_struct(std_None());
        if (!none_sd)
            throw CodegenError("static codegen: `for` over an iterator requires the prelude Option type "
                               "(compile with the static prelude)", f.line, f.col);
        const MRef next_ref = resolve_method(std_Iterator(), "next");
        int cur = alloc_local();                              // the cursor (GC root)
        if (iterator_protocol(it) == IterProto::IntoIterator) {
            int r = compile_trait_call(resolve_method(std_IntoIterator(), "intoIter"), { f.iter.get() });
            as_.R6(OpCode::MOV, static_cast<uint8_t>(cur), static_cast<uint8_t>(r), 0);
            free_if_temp(r);
        } else {
            compile_into(*f.iter, cur);                       // the value IS the cursor
        }
        // Rotated: `opt` is a LOCAL (not a LIFO temp) so the Option produced by the bottom `check`
        // survives the check->top control edge (top reads its Some payload before check re-runs).
        int opt = alloc_local();
        loop_stack_.push_back(LoopCtx{ end, cont, -1 });   // a for never carries a break value
        as_.J(OpCode::J, check);                            // rotate: enter at the bottom guard
        as_.label(top);                                     // `opt` is Some here (set by check)
        int elem = alloc_temp();
        as_.GET_PROP(static_cast<uint8_t>(elem), static_cast<uint8_t>(opt), 0);           // Some payload
        const size_t scope_mark = scope_.size();
        bind_for_pattern(*f.pat, elem, slots);
        free_if_temp(elem);
        compile_discard(*f.body);                // the body's value is dropped -- build nothing
        scope_.resize(scope_mark);
        as_.label(cont);
        as_.label(check);
        int rnext = emit_next_call(cur, next_ref);            // opt = next(cur)  (Option[T])
        as_.R6(OpCode::MOV, static_cast<uint8_t>(opt), static_cast<uint8_t>(rnext), 0);
        free_if_temp(rnext);
        int kt = alloc_temp(); as_.GET_TYPE_ID(static_cast<uint8_t>(kt), static_cast<uint8_t>(opt));
        int kc = alloc_temp(); as_.load_const(static_cast<uint8_t>(kc), static_cast<int64_t>(none_sd->id));
        as_.B(OpCode::BNE_INT, static_cast<uint8_t>(kt), static_cast<uint8_t>(kc), top);  // not None -> iterate
        free_if_temp(kc); free_if_temp(kt);
        as_.label(end);
        loop_stack_.pop_back();
    } else if (iter_protocol_applies(it)) {
        // Iterable protocol: `for x in it` == `for x in iter(it)`. Materialize the iterable to a Vec
        // (devirtualized for a concrete receiver, the fused RESOLVE_CALL for a generic `I: Iterable[T]`), then
        // index-walk it exactly like an Array. Mirrors the dynamic side's Enumerable fallback.
        int arr = alloc_local();                             // holds the materialized Vec (GC root)
        int len = alloc_local();
        int idx = alloc_local();
        int r = compile_trait_call(resolve_method(std_Iterable(), "iter"), { f.iter.get() });
        as_.R6(OpCode::MOV, static_cast<uint8_t>(arr), static_cast<uint8_t>(r), 0);
        free_if_temp(r);
        as_.LEN(static_cast<uint8_t>(len), static_cast<uint8_t>(arr));
        as_.load_const(static_cast<uint8_t>(idx), 0);
        loop_stack_.push_back(LoopCtx{ end, cont, -1 });   // a for never carries a break value
        as_.J(OpCode::J, check);                            // rotate: enter at the bottom guard
        as_.label(top);                                     // idx < len here (guaranteed by check)
        int elem = alloc_temp();
        as_.ARRAY_GET(static_cast<uint8_t>(elem), static_cast<uint8_t>(arr), static_cast<uint8_t>(idx));
        const size_t scope_mark = scope_.size();
        bind_for_pattern(*f.pat, elem, slots);
        free_if_temp(elem);
        compile_discard(*f.body);                // the body's value is dropped -- build nothing
        scope_.resize(scope_mark);
        as_.label(cont);
        int one = alloc_temp(); as_.load_const(static_cast<uint8_t>(one), 1);
        as_.R6(OpCode::ADD_INT, static_cast<uint8_t>(idx), static_cast<uint8_t>(idx), static_cast<uint8_t>(one));
        free_if_temp(one);
        as_.label(check);
        as_.B(OpCode::BLT_INT, static_cast<uint8_t>(idx), static_cast<uint8_t>(len), top);   // idx < len -> iterate
        as_.label(end);
        loop_stack_.pop_back();
    } else {
        throw CodegenError("static codegen: `for` over " +
                           (kind.empty() ? std::string("this type") : kind) +
                           " is not supported", f.line, f.col);
    }

    n_locals_ = locals_mark;                                 // reclaim the loop's locals
    int result = alloc_temp();
    as_.load_constant(static_cast<uint8_t>(result), Value::fromNil());   // `for` yields unit
    return result;
}

// ----- calls -----------------------------------------------------------------

int Codegen::compile_call_dispatch(const Expr* callee, const std::vector<const Expr*>& args,
                                   const TyPtr& result_ty, const Expr* site) {
    if (!callee) throw CodegenError("call has no callee");
    if (callee->kind == ExprKind::Ident) {
        const auto& id = static_cast<const IdentExpr&>(*callee);
        if (!id.qualifier.empty()) {
            if (id.inherent) {                       // `Type::method(x)` -> a plain direct call
                const std::string tgt = inherent_fn_name(id);
                const int inl = try_inline_call(site, tgt, args);
                return inl >= 0 ? inl : compile_call(tgt, args);
            }
            return compile_trait_call(resolve_method(id.qualifier, id.name), args, site);  // `Trait::method(..)`
        }
        if (local_reg(id.name) >= 0)                 // a local/param holding a function value
            return compile_call_indirect(*callee, args);
        if (is_capture_name(id.name))                // a captured fn-value inside a lambda body
            return compile_call_indirect(*callee, args);
        if (user_fn_ref(id)) {                       // a direct call to a top-level function (user wins)
            const int inl = try_inline_call(site, id.name, args);
            return inl >= 0 ? inl : compile_call(id.name, args);
        }
        if (is_ctor_name(id.name))                   // a tuple-struct / enum-variant constructor
            return compile_ctor_call(id.name, args);
        if (is_trait_method_name(id.name))           // an unqualified trait-method call
            return compile_trait_call(resolve_method("", id.name), args, site);
        if (is_builtin_name(id.name))                // a container builtin (array / vec / push / len)
            return compile_builtin_call(id.name, args, result_ty);
        if (native_id_of(id.name) >= 0)              // a native I/O call -> CALL_NATIVE (user won above)
            return compile_native_call(id.name, args);
        throw CodegenError("static codegen: a call to '" + id.name + "' is not supported",
                           callee->line, callee->col);
    }
    // Method-call syntax `recv.method(args)` (S1): a call whose callee is a (non-tuple) field
    // access that is NOT a real struct field -> a trait-method call with the receiver as arg 0.
    // (Field-first, mirroring the checker.) `compile_trait_call` devirtualizes on the receiver's
    // concrete head or emits the fused RESOLVE_CALL for a dyn/generic receiver.
    if (callee->kind == ExprKind::Field) {
        const auto& fld = static_cast<const FieldExpr&>(*callee);
        if (!fld.tuple_index && !cg_field_exists(fld.obj->ty, fld.name)) {
            const std::string head = (fld.obj->ty && fld.obj->ty->kind == TyKind::Named)
                                     ? fld.obj->ty->name : std::string();
            std::vector<const Expr*> margs;
            margs.push_back(fld.obj.get());
            for (const Expr* a : args) margs.push_back(a);
            if (is_inherent_method(head, fld.name)) {        // inherent -> a plain direct call (static)
                const std::string tgt = "$inherent$" + head + "$" + fld.name;
                const int inl = try_inline_call(site, tgt, margs);
                return inl >= 0 ? inl : compile_call(tgt, margs);
            }
            if (is_trait_method_name(fld.name))              // trait -> devirt / fused RESOLVE_CALL
                return compile_trait_call(resolve_method("", fld.name), margs, site);
        }
    }
    return compile_call_indirect(*callee, args);     // an arbitrary function-valued expression
}

// Does the named type `t` have a struct field `name`? (Method-call syntax is field-first.)
bool Codegen::cg_field_exists(const TyPtr& t, const std::string& name) const {
    if (!t || t->kind != TyKind::Named) return false;
    auto it = structs_.find(t->name);
    return it != structs_.end() && field_slot(it->second, name) >= 0;
}

int Codegen::compile_call(const std::string& name, const std::vector<const Expr*>& args) {
    if (frame_size_ + static_cast<int>(args.size()) > 64)
        throw CodegenError("call argument window exceeds the 64-register r6 limit");
    // Evaluate every argument first (holding each), so a later argument that is itself a
    // call cannot clobber an earlier one in the shared outgoing window.
    std::vector<int> arg_regs;
    arg_regs.reserve(args.size());
    for (const Expr* a : args) arg_regs.push_back(compile_expr(*a));
    for (size_t i = 0; i < arg_regs.size(); ++i)     // -> outgoing window [frame_size, ...)
        as_.R6(OpCode::MOV, static_cast<uint8_t>(frame_size_ + static_cast<int>(i)),
               static_cast<uint8_t>(arg_regs[i]), 0);
    for (size_t i = arg_regs.size(); i-- > 0;) free_if_temp(arg_regs[i]);
    as_.CALL(name);
    int dst = alloc_temp();                          // the return value comes back at r[frame_size]
    as_.MOV_TAKE(static_cast<uint8_t>(dst), static_cast<uint8_t>(frame_size_));
    return dst;
}

int Codegen::compile_call_indirect(const Expr& callee, const std::vector<const Expr*>& args) {
    if (frame_size_ + static_cast<int>(args.size()) > 64)
        throw CodegenError("call argument window exceeds the 64-register r6 limit");
    // The callee value lives below the outgoing window, so the argument MOVs do not clobber
    // it and CALL_INDIRECT reads it after the arguments are in place.
    int rcallee = compile_expr(callee);
    std::vector<int> arg_regs;
    arg_regs.reserve(args.size());
    for (const Expr* a : args) arg_regs.push_back(compile_expr(*a));
    for (size_t i = 0; i < arg_regs.size(); ++i)
        as_.R6(OpCode::MOV, static_cast<uint8_t>(frame_size_ + static_cast<int>(i)),
               static_cast<uint8_t>(arg_regs[i]), 0);
    as_.CALL_INDIRECT(static_cast<uint8_t>(rcallee), static_cast<uint8_t>(args.size()));
    for (size_t i = arg_regs.size(); i-- > 0;) free_if_temp(arg_regs[i]);
    free_if_temp(rcallee);
    int dst = alloc_temp();
    as_.MOV_TAKE(static_cast<uint8_t>(dst), static_cast<uint8_t>(frame_size_));
    return dst;
}

// ----- container builtins + index (P7b) -------------------------------------

int Codegen::compile_builtin_call(const std::string& name, const std::vector<const Expr*>& args,
                                  const TyPtr& result_ty) {
    if (name == "array") {                               // array(n, init) -> ANEW + AFILL
        // No element coercion: `Array` is invariant, so the element type is exactly the init's type
        // (`array(n, 0)` is Array[Int]; an Array[Double] needs `array(n, 0.0)`).
        int rd = alloc_temp();                            // the result (held lowest, GC-scanned)
        int rn = compile_expr(*args[0]);
        as_.ANEW(static_cast<uint8_t>(rd), static_cast<uint8_t>(rn));
        free_if_temp(rn);
        int rfill = compile_expr(*args[1]);
        as_.AFILL(static_cast<uint8_t>(rd), static_cast<uint8_t>(rfill));
        free_if_temp(rfill);
        return rd;
    }
    if (name == "vec") {                                 // vec() -> VEC_NEW
        int rd = alloc_temp();
        as_.VEC_NEW(static_cast<uint8_t>(rd));
        return rd;
    }
    if (name == "emptyArray") {                          // emptyArray() -> ANEW 0 (empty fixed array)
        int rd = alloc_temp();                            // the result (held lowest, GC-scanned)
        int rn = alloc_temp();
        as_.load_const(static_cast<uint8_t>(rn), 0);      // count = 0
        as_.ANEW(static_cast<uint8_t>(rd), static_cast<uint8_t>(rn));
        free_if_temp(rn);
        return rd;
    }
    if (name == "push") {                                // push(v, x) -> VEC_PUSH; returns the vec
        int rv = compile_expr(*args[0]);
        const bool elem_dbl = result_ty && result_ty->kind == TyKind::Named &&
                              result_ty->args.size() == 1 && is_double_ty(result_ty->args[0]);
        int rx = (elem_dbl && is_int_ty(args[1]->ty)) ? compile_coerced(*args[1], true)
                                                      : compile_expr(*args[1]);
        as_.VEC_PUSH(static_cast<uint8_t>(rv), static_cast<uint8_t>(rx));
        free_if_temp(rx);
        return rv;
    }
    if (name == "len") {                                 // len(x) -> LEN (kind-dispatched)
        int rx = compile_expr(*args[0]);
        int rd = is_temp(rx) ? rx : alloc_temp();
        as_.LEN(static_cast<uint8_t>(rd), static_cast<uint8_t>(rx));
        return rd;
    }
    if (name == "toString") {                            // toString(x) -> TO_STRING (generic coercion),
        return compile_stringify(*args[0]);              // or `Name(inner)` if x is a transparent struct
    }
    if (name == "print" || name == "println") {          // print(a, ...)/println(a, ...) -> TO_STRING + PRINT/PRINTLN
        // Variadic, NO separator: each arg is stringified and written back-to-back (sugar for sequential
        // single-arg prints). println appends a single newline AFTER the last arg (newline once). Args are
        // lowered sequentially, so only one arg-string temp is live at a time (well under the reserved
        // 1 + need_call(args) builtin window -- no measure change needed).
        const bool ln = name == "println";
        if (args.empty()) {                               // `println()` -> a bare newline (empty string)
            int rs = alloc_temp();                        // holds the KIND_STRING sink arg, then the Nil result
            as_.load_str(static_cast<uint8_t>(rs), "");
            as_.PRINTLN(static_cast<uint8_t>(rs), static_cast<uint8_t>(rs));   // rd aliases rstr; rd = Nil
            return rs;                                     // Nil == the unit value
        }
        int result = -1;
        for (size_t i = 0; i < args.size(); ++i) {
            int rs = compile_stringify(*args[i]);         // the KIND_STRING the sink writes (named if transparent)
            const bool last = (i + 1 == args.size());
            if (ln && last) as_.PRINTLN(static_cast<uint8_t>(rs), static_cast<uint8_t>(rs));   // newline once
            else            as_.PRINT(static_cast<uint8_t>(rs), static_cast<uint8_t>(rs));
            if (last) result = rs;                        // each rs holds Nil after; keep the last as the result
            else      free_if_temp(rs);
        }
        return result;                                    // Nil == the unit value
    }
    if (name == "panic") {                               // panic(msg) -> PANIC (diverges; never returns)
        int rmsg = compile_expr(*args[0]);
        as_.PANIC(static_cast<uint8_t>(rmsg));
        free_if_temp(rmsg);
        int rd = alloc_temp();                            // an unreachable placeholder value (panic aborts first)
        as_.load_constant(static_cast<uint8_t>(rd), Value::fromNil());
        return rd;
    }
    if (name == "has" || name == "delete") {             // has(m,k)/delete(m,k) -> MAP_HAS/MAP_DELETE -> Bool
        int rm = compile_expr(*args[0]);
        int rk = compile_expr(*args[1]);
        int rd = is_temp(rm) ? rm : (is_temp(rk) ? rk : alloc_temp());
        if (name == "has") as_.MAP_HAS(static_cast<uint8_t>(rd), static_cast<uint8_t>(rm), static_cast<uint8_t>(rk));
        else               as_.MAP_DELETE(static_cast<uint8_t>(rd), static_cast<uint8_t>(rm), static_cast<uint8_t>(rk));
        if (rd != rk) free_if_temp(rk);
        if (rd != rm) free_if_temp(rm);
        return rd;
    }
    if (name == "mapIterNext" || name == "mapKeyAt" || name == "mapValAt") {
        // Internal live-map cursor primitives (2 args: map + Int index) -> MAP_ITER_NEXT/KEY_AT/VAL_AT.
        // Same register shape as has/delete (reuse an arg temp for the result), so no extra scratch.
        int rm = compile_expr(*args[0]);
        int ri = compile_expr(*args[1]);
        int rd = is_temp(rm) ? rm : (is_temp(ri) ? ri : alloc_temp());
        if      (name == "mapIterNext") as_.MAP_ITER_NEXT(static_cast<uint8_t>(rd), static_cast<uint8_t>(rm), static_cast<uint8_t>(ri));
        else if (name == "mapKeyAt")    as_.MAP_KEY_AT(static_cast<uint8_t>(rd), static_cast<uint8_t>(rm), static_cast<uint8_t>(ri));
        else                            as_.MAP_VAL_AT(static_cast<uint8_t>(rd), static_cast<uint8_t>(rm), static_cast<uint8_t>(ri));
        if (rd != ri) free_if_temp(ri);
        if (rd != rm) free_if_temp(rm);
        return rd;
    }
    if (name == "keys" || name == "values") {            // keys(m)/values(m) -> MAP_KEYS/MAP_VALUES -> Array (allocates)
        int rm = compile_expr(*args[0]);
        int rd = is_temp(rm) ? rm : alloc_temp();        // rd may alias the map (read before written)
        if (name == "keys") as_.MAP_KEYS(static_cast<uint8_t>(rd), static_cast<uint8_t>(rm));
        else                as_.MAP_VALUES(static_cast<uint8_t>(rd), static_cast<uint8_t>(rm));
        return rd;
    }
    if (name == "get") {                                 // get(coll, i) -> Option[Elem]: the miss-safe total read
        const StructDef* some_sd = find_struct(std_Some());
        const StructDef* none_sd = find_struct(std_None());
        if (!some_sd || !none_sd)
            throw CodegenError("static codegen: 'get' requires the prelude Option type "
                               "(compile with the static prelude)", args[0]->line, args[0]->col);
        // rd (the Option result) is allocated LOWEST so it is the sole survivor; all scratch sits
        // above it and is freed in LIFO order after the join. Both arms allocate exactly one struct
        // (Some / None) -> temp_top matches on both paths.
        const Expr& coll = *args[0];
        const bool is_map = coll.ty && coll.ty->kind == svc::TyKind::Named && coll.ty->name == "Map";
        if (is_map) {                                    // Map: Some(MAP_GET) if present (MAP_HAS), else None.
            // MAP_GET is computed UNCONDITIONALLY (Nil on a miss, harmless) so neither branch allocates scratch.
            int rd = alloc_temp();
            int rm = compile_expr(*args[0]);
            int rk = compile_expr(*args[1]);
            int rh = alloc_temp(); as_.MAP_HAS(static_cast<uint8_t>(rh), static_cast<uint8_t>(rm), static_cast<uint8_t>(rk));
            int rv = alloc_temp(); as_.MAP_GET(static_cast<uint8_t>(rv), static_cast<uint8_t>(rm), static_cast<uint8_t>(rk));
            const std::string miss = fresh_label("get_miss_");
            const std::string end  = fresh_label("get_end_");
            as_.B1(OpCode::BF, static_cast<uint8_t>(rh), miss);
            as_.NEW_STRUCT(static_cast<uint8_t>(rd), some_sd->id);        // present -> Some(value)
            as_.SET_PROP(static_cast<uint8_t>(rd), 0, static_cast<uint8_t>(rv));
            as_.J(OpCode::J, end);
            as_.label(miss);
            as_.NEW_STRUCT(static_cast<uint8_t>(rd), none_sd->id);        // absent -> None
            as_.label(end);
            free_if_temp(rv);
            free_if_temp(rh);
            free_if_temp(rk);
            free_if_temp(rm);
            return rd;
        }
        // Sequence (Array / Vec / Bytes): 0 <= i < len ? Some(a[i]) : None. ARRAY_GET TRAPS on OOB, so
        // it must sit INSIDE the in-bounds arm (unlike MAP_GET's nil-on-miss) -- the value read is the
        // only per-arm scratch, freed before the jump so temp_top matches at the join.
        int rd = alloc_temp();
        int rc = compile_expr(*args[0]);
        int ri = compile_expr(*args[1]);
        int rlen = alloc_temp(); as_.LEN(static_cast<uint8_t>(rlen), static_cast<uint8_t>(rc));
        int rz   = alloc_temp(); as_.load_const(static_cast<uint8_t>(rz), static_cast<int64_t>(0));
        const std::string miss = fresh_label("get_oob_");
        const std::string end  = fresh_label("get_end_");
        as_.B(OpCode::BLT_INT, static_cast<uint8_t>(ri), static_cast<uint8_t>(rz),   miss);   // i < 0   -> None
        as_.B(OpCode::BGE_INT, static_cast<uint8_t>(ri), static_cast<uint8_t>(rlen), miss);   // i >= len -> None
        int rv = alloc_temp(); as_.ARRAY_GET(static_cast<uint8_t>(rv), static_cast<uint8_t>(rc), static_cast<uint8_t>(ri));  // in bounds (tri-kind)
        as_.NEW_STRUCT(static_cast<uint8_t>(rd), some_sd->id);
        as_.SET_PROP(static_cast<uint8_t>(rd), 0, static_cast<uint8_t>(rv));
        free_if_temp(rv);
        as_.J(OpCode::J, end);
        as_.label(miss);
        as_.NEW_STRUCT(static_cast<uint8_t>(rd), none_sd->id);
        as_.label(end);
        free_if_temp(rz);
        free_if_temp(rlen);
        free_if_temp(ri);
        free_if_temp(rc);
        return rd;
    }
    if (name == "pop") {                                 // pop(v) -> Option[T]: Some(last) if len>0, else None
        const StructDef* some_sd = find_struct(std_Some());
        const StructDef* none_sd = find_struct(std_None());
        if (!some_sd || !none_sd)
            throw CodegenError("static codegen: 'pop' requires the prelude Option type "
                               "(compile with the static prelude)", args[0]->line, args[0]->col);
        // As with `get`: rd lowest (survivor), scratch above, POP done UNCONDITIONALLY (Nil no-op on
        // empty) so neither branch allocates. The was-empty decision reads the length taken BEFORE pop.
        int rd = alloc_temp();
        int rvec = compile_expr(*args[0]);
        int rlen = alloc_temp(); as_.LEN(static_cast<uint8_t>(rlen), static_cast<uint8_t>(rvec));
        int rv   = alloc_temp(); as_.VEC_POP(static_cast<uint8_t>(rv), static_cast<uint8_t>(rvec));
        int rz   = alloc_temp(); as_.load_const(static_cast<uint8_t>(rz), static_cast<int64_t>(0));
        const std::string empty = fresh_label("pop_empty_");
        const std::string end   = fresh_label("pop_end_");
        as_.B(OpCode::BEQ_INT, static_cast<uint8_t>(rlen), static_cast<uint8_t>(rz), empty);
        as_.NEW_STRUCT(static_cast<uint8_t>(rd), some_sd->id);
        as_.SET_PROP(static_cast<uint8_t>(rd), 0, static_cast<uint8_t>(rv));
        as_.J(OpCode::J, end);
        as_.label(empty);
        as_.NEW_STRUCT(static_cast<uint8_t>(rd), none_sd->id);
        as_.label(end);
        free_if_temp(rz);
        free_if_temp(rv);
        free_if_temp(rlen);
        free_if_temp(rvec);
        return rd;
    }
    // The three numeric-conversion builtins below take a `Double` parameter, so the checker marks an
    // `Int` argument for widening -- but each of them derives the conversion from the argument's own
    // type and emits it ITSELF. They therefore compile the argument RAW, for exactly the reason
    // `compile_coerced` does: honoring the mark here as well would widen the same value twice, and
    // `I2D` on double bits reads garbage through `asSigned48()` (`toInt(7)` returned 0).
    if (name == "toInt") {                               // toInt(x: Double) -> Int  (D2I, saturating)
        int rs = compile_expr_raw(*args[0]);
        if (is_int_ty(args[0]->ty)) return rs;           // an Int arg is identity (no round-trip)
        int rd = is_temp(rs) ? rs : alloc_temp();
        as_.D2I(static_cast<uint8_t>(rd), static_cast<uint8_t>(rs));
        return rd;
    }
    if (name == "toDouble") {                            // toDouble(x: Int) -> Double  (I2D, exact)
        int rs = compile_expr_raw(*args[0]);
        // The operand can legitimately arrive as a Double even though the checker demands an Int
        // argument; widening it again would be wrong. Every 48-bit Int is exactly representable, so
        // I2D never loses.
        if (!is_int_ty(args[0]->ty)) return rs;
        int rd = is_temp(rs) ? rs : alloc_temp();
        as_.I2D(static_cast<uint8_t>(rd), static_cast<uint8_t>(rs));
        return rd;
    }
    if (name == "floor" || name == "ceil" || name == "trunc" ||          // Double -> Double (DROUND)
        name == "round" || name == "roundHalfToEven") {
        int rs = compile_expr_raw(*args[0]);
        int rd = is_temp(rs) ? rs : alloc_temp();
        if (is_int_ty(args[0]->ty)) {                    // rounding an integer is identity: just widen
            as_.I2D(static_cast<uint8_t>(rd), static_cast<uint8_t>(rs));
        } else {
            const uint8_t mode = name == "floor" ? 0 : name == "ceil"  ? 1 :
                                 name == "trunc" ? 2 : name == "round" ? 3 : 4;  // 4 = roundHalfToEven
            as_.DROUND(static_cast<uint8_t>(rd), static_cast<uint8_t>(rs), mode);
        }
        return rd;
    }
    // ordinal(e) -> the Int discriminant. Backed by NO opcode: an int-backed enum ERASES to a bare
    // Int, so the value in that register already IS the answer. Note this one uses the ordinary
    // `compile_expr`, unlike the three arms above -- they compile RAW because they derive a widening
    // themselves and must not honor the checker's mark a second time. `ordinal` derives nothing, and
    // its argument is an enum, a type `widen_double` can never be set on.
    if (name == "ordinal") return compile_expr(*args[0]);
    if (name == "toBytes") {                             // toBytes(str) -> BYTES_FROM_STR
        int rs = compile_expr(*args[0]);
        int rd = is_temp(rs) ? rs : alloc_temp();
        as_.BYTES_FROM_STR(static_cast<uint8_t>(rd), static_cast<uint8_t>(rs));
        return rd;
    }
    if (name == "fromBytes") {                           // fromBytes(buf) -> BYTES_TO_STR
        int rb = compile_expr(*args[0]);
        int rd = is_temp(rb) ? rb : alloc_temp();
        as_.BYTES_TO_STR(static_cast<uint8_t>(rd), static_cast<uint8_t>(rb));
        return rd;
    }
    if (name == "bytes") {                               // bytes()/bytes(n) -> BYTES_NEW_CAP (cap 0 or n)
        int rd = alloc_temp();
        int rcap;
        if (args.empty()) { rcap = alloc_temp(); as_.load_const(static_cast<uint8_t>(rcap), static_cast<int64_t>(0)); }
        else              { rcap = compile_expr(*args[0]); }
        as_.BYTES_NEW_CAP(static_cast<uint8_t>(rd), static_cast<uint8_t>(rcap));
        free_if_temp(rcap);
        return rd;
    }
    if (name == "appendBytes") {                         // appendBytes(dst, src) -> BYTES_APPEND; returns dst
        int rd = compile_expr(*args[0]);                 // dst buffer (stays the buffer, returned)
        int rs = compile_expr(*args[1]);                 // src: String or Bytes
        as_.BYTES_APPEND(static_cast<uint8_t>(rd), static_cast<uint8_t>(rs));
        free_if_temp(rs);
        return rd;
    }
    if (name == "_appendBytesRange") {                   // _appendBytesRange(dst, src, lo, hi) -> BYTES_APPEND_RANGE
        int rd  = compile_expr(*args[0]);                // dst (returned)
        int rs  = compile_expr(*args[1]);                // src Bytes
        int rlo = compile_expr(*args[2]);
        int rhi = compile_expr(*args[3]);
        // BYTES_APPEND_RANGE is q4: 4 registers in 5-bit fields (< 32). Guard LOUDLY rather than let
        // the Q4 builder silently truncate in Release (a miscompile). Unreachable from the prelude
        // sites (small fns) and realistic code; the same discipline as the >64-register ceiling.
        if (rd >= 32 || rs >= 32 || rlo >= 32 || rhi >= 32)
            throw CodegenError("_appendBytesRange operand register >= 32 (q4 encoding limit)");
        as_.BYTES_APPEND_RANGE(static_cast<uint8_t>(rd), static_cast<uint8_t>(rs),
                               static_cast<uint8_t>(rlo), static_cast<uint8_t>(rhi));
        free_if_temp(rhi); free_if_temp(rlo); free_if_temp(rs);
        return rd;
    }
    throw CodegenError("internal: unknown builtin '" + name + "'");
}

// ----- native I/O calls (CALL_NATIVE via the VM registry) --------------------

int Codegen::compile_native_call(const std::string& name, const std::vector<const Expr*>& args) {
    const int id = native_id_of(name);
    const NativeReturn nret = native_return_of(id);
    const int nargs = static_cast<int>(args.size());

    // dst allocated FIRST so it is the lowest live temp (the sole survivor of this expression, the
    // get/pop discipline). The argument window is a CONTIGUOUS block of TEMPS within [0, frame_size)
    // -- hence GC roots -- NOT the outgoing window [frame_size_, ...): CALL_NATIVE does not slide the
    // window and IS a GC safepoint, so outgoing-window args would be invisible to the collector.
    int dst = alloc_temp();
    const int base = temp_top_;                       // first window slot (== dst + 1)
    std::vector<int> win;
    win.reserve(static_cast<size_t>(nargs));
    for (int i = 0; i < nargs; ++i) win.push_back(alloc_temp());   // reserve the window contiguously
    for (int i = 0; i < nargs; ++i) {                 // then fill each slot (scratch lands above it)
        int r = compile_expr(*args[static_cast<size_t>(i)]);
        if (r != win[static_cast<size_t>(i)])
            as_.R6(OpCode::MOV, static_cast<uint8_t>(win[static_cast<size_t>(i)]),
                   static_cast<uint8_t>(r), 0);
        free_if_temp(r);
    }
    int id_reg = alloc_temp();
    const uint8_t first_arg = static_cast<uint8_t>(nargs > 0 ? base : dst);  // 0-arg base is a dummy

    if (nret == NRET_PLAIN) {                          // no wrap: the native writes straight into dst
        as_.call_native_id(static_cast<uint8_t>(dst), static_cast<uint8_t>(id_reg),
                           first_arg, static_cast<uint8_t>(nargs), static_cast<uint16_t>(id));
        free_if_temp(id_reg);
    } else {                                           // Result/Option: wrap the raw native result
        int rres = alloc_temp();
        as_.call_native_id(static_cast<uint8_t>(rres), static_cast<uint8_t>(id_reg),
                           first_arg, static_cast<uint8_t>(nargs), static_cast<uint16_t>(id));
        if (name == "rawRun")         emit_wrap_process(dst, rres);   // reshape the raw array -> struct
        else if (nret == NRET_RESULT) emit_wrap_result(dst, rres);
        else                          emit_wrap_option(dst, rres);
        free_if_temp(rres);
        free_if_temp(id_reg);
    }
    for (int i = nargs; i-- > 0;) free_if_temp(win[static_cast<size_t>(i)]);   // LIFO
    return dst;
}

// Wrap a raw native result into Result: kind==KIND_STRING marks the Err payload (a native never
// returns a String on success), else Ok. `dst` sits below `rres`, and `rres` (a heap value) stays
// a live in-frame root across the NEW_STRUCT allocations.
void Codegen::emit_wrap_result(int dst, int rres) {
    const StructDef* ok  = find_struct(std_Ok());
    const StructDef* err = find_struct(std_Err());
    if (!ok || !err)
        throw CodegenError("static codegen: a Result-returning native requires the static prelude "
                           "(compile with the prelude)");
    const std::string l_err  = fresh_label("iowrap_err_");
    const std::string l_done = fresh_label("iowrap_done_");
    int k = alloc_temp();
    int c = alloc_temp();
    as_.GET_KIND(static_cast<uint8_t>(k), static_cast<uint8_t>(rres));
    as_.load_const(static_cast<uint8_t>(c), VM_KIND_STRING);
    as_.B(OpCode::BEQ_INT, static_cast<uint8_t>(k), static_cast<uint8_t>(c), l_err);
    as_.NEW_STRUCT(static_cast<uint8_t>(dst), ok->id);            // Ok(rres)
    as_.SET_PROP(static_cast<uint8_t>(dst), 0, static_cast<uint8_t>(rres));
    as_.J(OpCode::J, l_done);
    as_.label(l_err);
    as_.NEW_STRUCT(static_cast<uint8_t>(dst), err->id);          // Err(rres)
    as_.SET_PROP(static_cast<uint8_t>(dst), 0, static_cast<uint8_t>(rres));
    as_.label(l_done);
    free_if_temp(c);
    free_if_temp(k);
}

// Wrap a raw native result into Option: a Nil result is None, else Some(result).
void Codegen::emit_wrap_option(int dst, int rres) {
    const StructDef* some = find_struct(std_Some());
    const StructDef* none = find_struct(std_None());
    if (!some || !none)
        throw CodegenError("static codegen: an Option-returning native requires the static prelude "
                           "(compile with the prelude)");
    const std::string l_none = fresh_label("optwrap_none_");
    const std::string l_done = fresh_label("optwrap_done_");
    int k = alloc_temp();
    as_.R6(OpCode::IS_NIL, static_cast<uint8_t>(k), static_cast<uint8_t>(rres), 0);
    as_.B1(OpCode::BT, static_cast<uint8_t>(k), l_none);
    as_.NEW_STRUCT(static_cast<uint8_t>(dst), some->id);         // Some(rres)
    as_.SET_PROP(static_cast<uint8_t>(dst), 0, static_cast<uint8_t>(rres));
    as_.J(OpCode::J, l_done);
    as_.label(l_none);
    as_.NEW_STRUCT(static_cast<uint8_t>(dst), none->id);         // None
    as_.label(l_done);
    free_if_temp(k);
}

// Wrap the rawRun native's raw result into Result[ProcessOutput, String]. On a spawn error the
// native returns a KIND_STRING (Err); on success a KIND_ARRAY[3] = [stdoutBytes, stderrBytes,
// exitInt], which is reshaped into a ProcessOutput struct (the invariant Array[T] cannot type the
// heterogeneous array, so codegen -- not the type system -- does the array->struct conversion).
// `rres` (the array) and `po` (the struct) are in-frame temps = GC roots across the two NEW_STRUCT
// safepoints; the field reads/sets between them are not safepoints, so the element temp is transient.
void Codegen::emit_wrap_process(int dst, int rres) {
    const StructDef* ok  = find_struct(std_Ok());
    const StructDef* err = find_struct(std_Err());
    const StructDef* po  = find_struct(std_ProcessOutput());
    if (!ok || !err || !po)
        throw CodegenError("static codegen: process spawn requires the static prelude "
                           "(Ok/Err/ProcessOutput -- compile with the prelude)");
    const std::string l_err  = fresh_label("procwrap_err_");
    const std::string l_done = fresh_label("procwrap_done_");
    int k = alloc_temp();
    int c = alloc_temp();
    as_.GET_KIND(static_cast<uint8_t>(k), static_cast<uint8_t>(rres));
    as_.load_const(static_cast<uint8_t>(c), VM_KIND_STRING);
    as_.B(OpCode::BEQ_INT, static_cast<uint8_t>(k), static_cast<uint8_t>(c), l_err);
    // Success: build ProcessOutput { stdout, stderr, exitCode } from the array's three slots.
    int pstruct = alloc_temp();
    as_.NEW_STRUCT(static_cast<uint8_t>(pstruct), po->id);        // fields default-init (GC-safe)
    int idx  = alloc_temp();
    int elem = alloc_temp();
    for (int i = 0; i < 3; ++i) {
        as_.load_const(static_cast<uint8_t>(idx), static_cast<int64_t>(i));
        as_.ARRAY_GET(static_cast<uint8_t>(elem), static_cast<uint8_t>(rres), static_cast<uint8_t>(idx));
        as_.SET_PROP(static_cast<uint8_t>(pstruct), static_cast<uint16_t>(i),
                     static_cast<uint8_t>(elem));
    }
    free_if_temp(elem);
    free_if_temp(idx);
    as_.NEW_STRUCT(static_cast<uint8_t>(dst), ok->id);           // Ok(ProcessOutput)
    as_.SET_PROP(static_cast<uint8_t>(dst), 0, static_cast<uint8_t>(pstruct));
    free_if_temp(pstruct);
    as_.J(OpCode::J, l_done);
    as_.label(l_err);
    as_.NEW_STRUCT(static_cast<uint8_t>(dst), err->id);         // Err(message string)
    as_.SET_PROP(static_cast<uint8_t>(dst), 0, static_cast<uint8_t>(rres));
    as_.label(l_done);
    free_if_temp(c);
    free_if_temp(k);
}

bool Codegen::is_native_callee(const Expr* callee) const {
    if (!callee || callee->kind != ExprKind::Ident) return false;
    const auto& id = static_cast<const IdentExpr&>(*callee);
    return id.qualifier.empty() && local_reg(id.name) < 0 &&
           !user_fn_ref(id) && native_id_of(id.name) >= 0;
}

int Codegen::native_call_need(const std::vector<const Expr*>& args, const Expr* callee) {
    const int nargs = static_cast<int>(args.size());
    const auto& id  = static_cast<const IdentExpr&>(*callee);
    const NativeReturn nret = native_return_of(native_id_of(id.name));
    // rawRun's array->struct reshape needs more scratch than the generic Result wrap: id_reg + rres
    // + the ProcessOutput struct + index/element temps + the k/c discriminator = 7.
    const int wrap_extra = (id.name == "rawRun") ? 7 : (nret == NRET_PLAIN ? 1 : 4);   // id_reg (+ rres + 2 wrap temps for R/O)
    // dst(1) + contiguous window(nargs) + fill scratch(need_call) + wrap_extra. A deliberately loose
    // but always-safe upper bound; nargs <= 2 so the slack is a couple of registers.
    return 1 + nargs + need_call(args) + wrap_extra;
}

// The receiver's static type is a Map (vs an Array/Vec/Bytes sequence). Maps index by KEY and
// use MAP_GET_OR_TRAP / MAP_SET; sequences index by Int and use ARRAY_GET / ARRAY_SET (tri-kind).
static bool index_is_map(const Expr& obj) {
    return obj.ty && obj.ty->kind == TyKind::Named && obj.ty->name == "Map";
}

int Codegen::compile_index(const IndexExpr& ix) {
    int robj = compile_expr(*ix.obj);
    int ridx = compile_expr(*ix.index);
    int dst;
    if (is_temp(robj))      dst = robj;                  // reuse the object temp (idx sits above it)
    else if (is_temp(ridx)) dst = ridx;
    else                    dst = alloc_temp();
    if (index_is_map(*ix.obj))                           // m[k] -- total read, TRAPS on a missing key
        as_.MAP_GET_OR_TRAP(static_cast<uint8_t>(dst), static_cast<uint8_t>(robj), static_cast<uint8_t>(ridx));
    else                                                 // a[i]/v[i]/b[i] -- bounds-checked, traps on OOB
        as_.ARRAY_GET(static_cast<uint8_t>(dst), static_cast<uint8_t>(robj), static_cast<uint8_t>(ridx));
    if (dst != ridx) free_if_temp(ridx);
    if (dst != robj) free_if_temp(robj);
    return dst;
}

void Codegen::compile_index_assign(const IndexExpr& target, const Expr& value, TokKind op) {
    int robj = compile_expr(*target.obj);
    int ridx = compile_expr(*target.index);
    // A compound `a[i] op= v` reuses the ONE `robj`/`ridx` pair for the read and the write, so both
    // the container and the index expression are evaluated exactly once (`v[next()] += 1` calls
    // `next()` a single time). The read is the same total-or-trap access as the surface `a[i]`:
    // MAP_GET_OR_TRAP for a map, so `m[k] += 1` on an ABSENT key aborts exactly as `m[k]` would --
    // compound assignment does not replace getOrPut/upsert for building a fresh entry.
    if (op != TokKind::Assign) {
        const bool dbl = is_double_ty(target.ty);
        const bool map = index_is_map(*target.obj);
        int rold = alloc_temp();
        as_.R6(map ? OpCode::MAP_GET_OR_TRAP : OpCode::ARRAY_GET, static_cast<uint8_t>(rold),
               static_cast<uint8_t>(robj), static_cast<uint8_t>(ridx));
        int rvc = dbl ? compile_coerced(value, true) : compile_expr(value);
        as_.R6(arith_opcode(op, dbl), static_cast<uint8_t>(rold),
               static_cast<uint8_t>(rold), static_cast<uint8_t>(rvc));
        if (map) as_.MAP_SET  (static_cast<uint8_t>(robj), static_cast<uint8_t>(ridx), static_cast<uint8_t>(rold));
        else     as_.ARRAY_SET(static_cast<uint8_t>(robj), static_cast<uint8_t>(ridx), static_cast<uint8_t>(rold));
        free_if_temp(rvc);
        free_if_temp(rold);
        free_if_temp(ridx);
        free_if_temp(robj);
        return;
    }
    // Widen an Int rvalue into a Double-element container at the element boundary.
    int rv = (is_double_ty(target.ty) && is_int_ty(value.ty)) ? compile_coerced(value, true)
                                                              : compile_expr(value);
    if (index_is_map(*target.obj))                       // m[k] = v -- insert/update (MAP_SET)
        as_.MAP_SET(static_cast<uint8_t>(robj), static_cast<uint8_t>(ridx), static_cast<uint8_t>(rv));
    else
        as_.ARRAY_SET(static_cast<uint8_t>(robj), static_cast<uint8_t>(ridx), static_cast<uint8_t>(rv));
    free_if_temp(rv);
    free_if_temp(ridx);
    free_if_temp(robj);
}

// ----- traits (dispatch table + devirtualization) ---------------------------

namespace {
// The head token of an impl target that maps to a fixed built-in dense id, or -1.
int builtin_dense_id(const std::string& n) {
    if (n == "Int")    return TID_INT;
    if (n == "Double") return TID_DOUBLE;
    if (n == "Bool")   return TID_BOOL;
    if (n == "String") return TID_STRING;
    if (n == "Map")    return TID_MAP;
    return -1;
}
} // namespace

bool Codegen::iter_protocol_applies(const TyPtr& it) const {
    const std::string iterable = std_Iterable();
    if (traits_.find(iterable) == traits_.end() || !it) return false;
    if (it->kind == TyKind::Var) {                      // a generic `I: Iterable[T]` receiver
        for (const auto& b : it->bounds) if (b.trait == iterable) return true;
        return false;
    }
    const std::string head = concrete_head_of(it);      // a concrete type with an `impl Iterable`
    if (head.empty()) return false;
    return impl_index_.count(iterable + std::string(1, '\0') + head) > 0;
}

Codegen::IterProto Codegen::iterator_protocol(const TyPtr& it) const {
    auto supports = [&](const std::string& trait) -> bool {
        if (traits_.find(trait) == traits_.end() || !it) return false;
        if (it->kind == TyKind::Dyn) return it->name == trait;      // `dyn Iterator[..]` (mangle = identity)
        if (it->kind == TyKind::Var) {                              // a generic `I: Iterator[..]` receiver
            for (const auto& b : it->bounds) if (b.trait == trait) return true;
            return false;
        }
        const std::string head = concrete_head_of(it);             // a concrete cursor with an impl
        if (head.empty()) return false;
        return impl_index_.count(trait + std::string(1, '\0') + head) > 0;
    };
    if (supports(std_Iterator()))     return IterProto::Iterator;
    if (supports(std_IntoIterator())) return IterProto::IntoIterator;
    return IterProto::None;
}

// A 1-arg trait dispatch on a receiver already in register `rrecv` (a loop cursor local). Mirrors the
// dynamic-dispatch tail of compile_trait_call via the FUSED RESOLVE_CALL: MOV the receiver into the
// outgoing window (arg 0), then one op resolves + calls (it reads the receiver back at window[frame_size]).
int Codegen::emit_next_call(int rrecv, const MRef& ref) {
    if (frame_size_ + 1 > 64)
        throw CodegenError("call argument window exceeds the 64-register r6 limit");
    as_.R6(OpCode::MOV, static_cast<uint8_t>(frame_size_), static_cast<uint8_t>(rrecv), 0);   // arg 0
    as_.RESOLVE_CALL(1, ref.id);
    int dst = alloc_temp();
    as_.MOV_TAKE(static_cast<uint8_t>(dst), static_cast<uint8_t>(frame_size_));      // result
    return dst;
}

std::vector<uint32_t> Codegen::dense_ids_for_head(const std::string& head,
                                                  uint32_t line, uint32_t col) {
    // An enum: the runtime receiver is one of its VARIANT structs, so fill every variant's
    // dense column (the impl body is shared across variants -- one row value, N columns).
    if (auto eit = enum_variants_.find(head); eit != enum_variants_.end()) {
        std::vector<uint32_t> ids;
        for (const std::string& v : eit->second) {
            const StructDef* sd = find_struct(v);
            if (sd) ids.push_back(static_cast<uint32_t>(BUILTIN_COUNT) + sd->id);
        }
        return ids;
    }
    if (const StructDef* sd = find_struct(head))      // a record / tuple struct
        return { static_cast<uint32_t>(BUILTIN_COUNT) + sd->id };
    const int b = builtin_dense_id(head);             // a primitive / Map
    if (b >= 0) return { static_cast<uint32_t>(b) };
    if (head == "Array") return { static_cast<uint32_t>(TID_ARRAY) };
    if (head == "Vec")   return { static_cast<uint32_t>(TID_VEC) };
    if (head == "Bytes") return { static_cast<uint32_t>(TID_BYTES) };
    if (head == "List") {
        // A List is either a `$List` cons cell (a struct) or nil (the empty list), so an impl for
        // List fills BOTH dense columns. Force-register `$List` so its column exists in the table.
        const uint16_t cid = ensure_cons_type();
        return { static_cast<uint32_t>(BUILTIN_COUNT) + cid, static_cast<uint32_t>(TID_NIL) };
    }
    // A blanket impl's bare type param (`impl[T: B] Tr for T`) still has no single runtime head, so
    // its lowering stays deferred. A clean, located CodegenError.
    throw CodegenError("static codegen: an impl for type '" + head +
                       "' is not lowered yet (structs, enums, primitives, Map, Array, Vec, Bytes and List are)",
                       line, col);
}

void Codegen::link_traits(const Program& prog) {
    // Record enum -> variant names first (used to expand an enum impl over its variant dense ids).
    for (const auto& item : prog.items)
        if (item->kind == ItemKind::Enum) {
            const auto& en = static_cast<const EnumItem&>(*item);
            std::vector<std::string> vs;
            for (const EnumVariant& v : en.variants) vs.push_back(v.name);
            enum_variants_[en.name] = std::move(vs);
        }

    // Pre-scan: which traits have at least one impl (concrete OR blanket -- both are
    // ItemKind::Impl carrying a trait_name). A trait with NO impl can never be dispatched
    // (the checker rejects a dispatch to an unimplemented trait), so its methods are never
    // referenced by a dynamic dispatch (RESOLVE_CALL / RESOLVE_TCO_CALL). We therefore withhold global method ids from such
    // traits, so they contribute no all-NONE rows to the dispatch table and the surviving
    // (impl'd) traits stay densely numbered. Consistent by construction: emission and
    // build_trait_table both read this same method_id map, so a denser map needs no
    // bytecode patching. This must run BEFORE pass (2) populates impls_. (Safe over-
    // approximation: a blanket-only trait whose bound no type satisfies still counts as
    // "has an impl" and keeps its -- rare -- all-NONE row.)
    std::unordered_set<std::string> impld_traits;
    for (const auto& item : prog.items)
        if (item->kind == ItemKind::Impl)
            impld_traits.insert(static_cast<const ImplDecl&>(*item).trait_name);

    // (1) Trait declarations: assign a global method id per method (impl'd traits only);
    // stage each default body.
    for (const auto& item : prog.items) {
        if (item->kind != ItemKind::Trait) continue;
        const auto& td = static_cast<const TraitDecl&>(*item);
        const bool live = impld_traits.count(td.name) > 0;
        TraitDef t;
        for (const Method& m : td.methods) {
            t.methods.push_back(m.name);
            if (live)                                     // withhold the id/row from a zero-impl trait
                t.method_id[m.name] = next_method_id_++;
            t.arity[m.name] = (m.has_self ? 1 : 0) + static_cast<int>(m.params.size());
            method_owners_[m.name].push_back(td.name);
            if (m.body) {                                 // a default method -> one shared fn
                std::vector<std::string> pn;
                if (m.has_self) pn.push_back("self");
                for (const Param& p : m.params) pn.push_back(p.name);
                method_fns_.push_back(MethodFn{
                    "$default$" + td.name + "$" + m.name, std::move(pn),
                    static_cast<const BlockExpr*>(m.body.get()), m.ret.get(),
                    /*is_default=*/true, td.name, /*head=*/"", m.name });
                method_fns_.back().module = item->module_prefix;   // trait's module (default body source)
            }
        }
        traits_.emplace(td.name, std::move(t));
    }

    // (2) Impl blocks: resolve the target head to its dense id(s), stage each method body.
    for (const auto& item : prog.items) {
        if (item->kind != ItemKind::Impl) continue;
        const auto& id = static_cast<const ImplDecl&>(*item);
        std::string head;
        if (id.target && id.target->kind == TypeKind::Named)
            head = static_cast<const NamedType&>(*id.target).name;
        if (id.is_inherent) {   // S2: stage inherent methods as $inherent$Head$method (direct-call, no trait table)
            for (const Method& m : id.methods) {
                std::vector<std::string> pn;
                if (m.has_self) pn.push_back("self");
                for (const Param& p : m.params) pn.push_back(p.name);
                method_fns_.push_back(MethodFn{
                    "$inherent$" + head + "$" + m.name, std::move(pn),
                    static_cast<const BlockExpr*>(m.body.get()), m.ret.get(),
                    /*is_default=*/false, /*trait=*/"", head, m.name });
                method_fns_.back().module = item->module_prefix;
                // Only a method with `self` is reachable through `.`; an ASSOCIATED function (no
                // `self`) is `Head::name(...)`-only and is reached via `id.inherent` instead. The
                // distinction is load-bearing, not tidiness: if a type had an associated `make()`
                // AND implemented a trait method `make(self)`, the checker resolves `x.make()` to the
                // TRAIT method, and an unfiltered `inherent_methods_` would make the dot branch below
                // emit `$inherent$Head$make` with the receiver as arg 0 -- a different function,
                // silently. The body is staged either way (above); only dot-reachability is filtered.
                if (m.has_self) inherent_methods_[head].insert(m.name);
            }
            continue;
        }
        // A blanket impl `impl[T: bounds] Tr for T` (head == one of the impl's own generic params):
        // stage each body like a generic fn (`self: T` dispatches dynamically through its bounds) and
        // record the bounds so build_trait_table can fill every bound-satisfying type's Tr columns.
        const GenericParam* blanket_gp = nullptr;
        for (const auto& gp : id.generics)
            if (gp.name == head) { blanket_gp = &gp; break; }
        if (blanket_gp) {
            const int bidx = static_cast<int>(blankets_.size());
            BlanketDef b;
            b.trait_name = id.trait_name;
            for (const BoundRef& br : blanket_gp->bounds) b.bound_traits.push_back(br.trait);
            for (const Method& m : id.methods) {
                std::vector<std::string> pn;
                if (m.has_self) pn.push_back("self");
                for (const Param& p : m.params) pn.push_back(p.name);
                method_fns_.push_back(MethodFn{
                    "$blanket$" + id.trait_name + "$" + m.name, std::move(pn),
                    static_cast<const BlockExpr*>(m.body.get()), m.ret.get(),
                    /*is_default=*/false, id.trait_name, head, m.name,
                    /*is_blanket=*/true, bidx });
                method_fns_.back().module = item->module_prefix;   // impl's module (body source)
            }
            blankets_.push_back(std::move(b));
            continue;                                     // NOT a concrete impl -> skip impls_/impl_index_
        }
        ImplDef d;
        d.trait_name = id.trait_name;
        d.head       = head;
        d.dense_ids  = dense_ids_for_head(head, id.line, id.col);
        for (const Method& m : id.methods) {
            d.own_fn_id[m.name] = 0;                      // id filled after declare_fn
            std::vector<std::string> pn;
            if (m.has_self) pn.push_back("self");
            for (const Param& p : m.params) pn.push_back(p.name);
            method_fns_.push_back(MethodFn{
                "$impl$" + id.trait_name + "$" + head + "$" + m.name, std::move(pn),
                static_cast<const BlockExpr*>(m.body.get()), m.ret.get(),
                /*is_default=*/false, id.trait_name, head, m.name });
            method_fns_.back().module = item->module_prefix;   // impl's module (body source)
        }
        impl_index_[id.trait_name + '\0' + head] = impls_.size();
        impls_.push_back(std::move(d));
    }
}

void Codegen::build_trait_table(Module& m) {
    const uint32_t W = static_cast<uint32_t>(BUILTIN_COUNT) +
                       static_cast<uint32_t>(as_.struct_types().size());
    const uint32_t M = next_method_id_;                   // one row per (trait, method)
    m.trait_table_width  = W;
    m.trait_method_count = M;
    if (M == 0) return;                                   // no traits -> empty table
    m.trait_table.assign(static_cast<size_t>(M) * W, TRAIT_METHOD_NONE);
    for (const ImplDef& im : impls_) {
        const TraitDef& t = traits_.at(im.trait_name);
        for (const std::string& method : t.methods) {
            uint16_t fn_id;
            auto own = im.own_fn_id.find(method);
            if (own != im.own_fn_id.end()) fn_id = own->second;       // impl-own method
            else                           fn_id = t.default_fn_id.at(method);  // shared default
            const uint32_t mid = t.method_id.at(method);
            for (uint32_t dense : im.dense_ids)
                m.trait_table[static_cast<size_t>(mid) * W + dense] = fn_id;
        }
    }

    // Blanket impls: fill each bound-satisfying type's EMPTY columns (a concrete impl, filled above,
    // wins). Candidate types = every concrete impl target head (a bound-satisfying type has a concrete
    // impl of the bound trait, so it appears there). head_satisfies_trait recurses through blanket
    // chains, so one pass over (blanket, head) suffices -- no outer fixpoint.
    if (!blankets_.empty()) {
        std::vector<std::string> heads;
        std::unordered_set<std::string> seen;
        for (const ImplDef& im : impls_)
            if (seen.insert(im.head).second) heads.push_back(im.head);
        for (const BlanketDef& b : blankets_) {
            const TraitDef& tb = traits_.at(b.trait_name);
            for (const std::string& H : heads) {
                if (impl_index_.count(b.trait_name + '\0' + H)) continue;   // concrete impl wins
                bool sat = true;
                for (const std::string& bt : b.bound_traits) {
                    std::unordered_set<std::string> ip;
                    if (!head_satisfies_trait(H, bt, ip)) { sat = false; break; }
                }
                if (!sat) continue;
                const std::vector<uint32_t> dens = dense_ids_for_head(H, 0, 0);
                for (const std::string& method : tb.methods) {
                    uint16_t fn_id;
                    auto own = b.method_fn_id.find(method);
                    if (own != b.method_fn_id.end()) fn_id = own->second;
                    else                             fn_id = tb.default_fn_id.at(method);
                    const uint32_t mid = tb.method_id.at(method);
                    for (uint32_t dense : dens)
                        if (m.trait_table[static_cast<size_t>(mid) * W + dense] == TRAIT_METHOD_NONE)
                            m.trait_table[static_cast<size_t>(mid) * W + dense] = fn_id;
                }
            }
        }
    }
}

bool Codegen::head_satisfies_trait(const std::string& head, const std::string& trait,
                                   std::unordered_set<std::string>& in_progress) const {
    if (impl_index_.count(trait + '\0' + head)) return true;   // a concrete impl of `trait` for `head`
    const std::string key = trait + '\0' + head;
    if (!in_progress.insert(key).second) return false;         // break a blanket cycle
    bool ok = false;
    for (const BlanketDef& b : blankets_) {                    // via a blanket of `trait`
        if (b.trait_name != trait) continue;
        bool all = true;
        for (const std::string& bt : b.bound_traits)
            if (!head_satisfies_trait(head, bt, in_progress)) { all = false; break; }
        if (all) { ok = true; break; }
    }
    in_progress.erase(key);
    return ok;
}

Codegen::MRef Codegen::resolve_method(const std::string& qualifier,
                                      const std::string& method) const {
    // The checker already rejected an ambiguous unqualified call and an unknown trait/method,
    // so this is a straight lookup (defensive throws only for an internal inconsistency).
    std::string trait = qualifier;
    if (trait.empty()) {
        auto oit = method_owners_.find(method);
        if (oit == method_owners_.end() || oit->second.empty())
            throw CodegenError("internal: '" + method + "' is not a trait method");
        trait = oit->second.front();
    }
    auto tit = traits_.find(trait);
    if (tit == traits_.end())
        throw CodegenError("internal: unknown trait '" + trait + "'");
    const TraitDef& t = tit->second;
    auto mid = t.method_id.find(method);
    if (mid == t.method_id.end())
        throw CodegenError("internal: trait '" + trait + "' has no method '" + method + "'");
    return MRef{ mid->second, t.arity.at(method), trait, method };
}

bool Codegen::is_trait_method_name(const std::string& name) const {
    return method_owners_.count(name) > 0;
}

bool Codegen::is_trait_call(const Expr* callee) const {
    if (!callee || callee->kind != ExprKind::Ident) return false;
    const auto& id = static_cast<const IdentExpr&>(*callee);
    // An uppercase qualifier is `Trait::method(...)` UNLESS the checker resolved it to a type's
    // inherent method (`Type::method(...)`), which lowers to a plain direct call instead.
    if (!id.qualifier.empty()) return !id.inherent;
    return local_reg(id.name) < 0 && !user_fn_ref(id) &&
           !is_ctor_name(id.name) && is_trait_method_name(id.name);
}

// Any qualified `Head::method(...)` call, trait or inherent. Used ONLY by the sizing walkers, so a
// qualified call keeps reserving the same (conservative) trait-call shape it always did -- a direct
// call needs one temp fewer, and over-reserving is harmless, whereas a plan/emit mismatch is not.
bool Codegen::is_qualified_method_call(const Expr* callee) {
    return callee && callee->kind == ExprKind::Ident &&
           !static_cast<const IdentExpr&>(*callee).qualifier.empty();
}

// The staged name of an inherent method body (see the `$inherent$` staging in register_traits).
std::string Codegen::inherent_fn_name(const IdentExpr& id) {
    return "$inherent$" + id.qualifier + "$" + id.name;
}

std::string Codegen::concrete_head_of(const TyPtr& t) const {
    if (!t) return {};
    switch (t->kind) {
    case TyKind::Int:    return "Int";
    case TyKind::Double: return "Double";
    case TyKind::Bool:   return "Bool";
    case TyKind::String: return "String";
    case TyKind::Named:  return t->name;                 // struct / enum / built-in container head
    // A trait object hides its concrete type BY CONSTRUCTION, so there is nothing to
    // devirtualize against: it takes the dynamic path (the fused RESOLVE_CALL / RESOLVE_TCO_CALL), which
    // is exactly the erased `T: Trait` path. Spelled out rather than left to `default` --
    // this is the whole of what `dyn` costs the backend.
    case TyKind::Dyn:    return {};
    default:             return {};                      // Var (generic), Tuple, Fn, ... -> dynamic
    }
}

std::string Codegen::devirt_target(const std::string& trait, const std::string& method,
                                   const std::string& head) const {
    auto iit = impl_index_.find(trait + '\0' + head);
    if (iit != impl_index_.end() && impls_[iit->second].own_fn_id.count(method))
        return "$impl$" + trait + "$" + head + "$" + method;      // concrete impl wins
    auto tit = traits_.find(trait);
    if (tit != traits_.end() && tit->second.default_fn_id.count(method))
        return "$default$" + trait + "$" + method;                // shared trait default
    return {};                                                    // -> dynamic dispatch
}

// A trait method call `m(recv, ...)`. If the receiver's static type is a concrete monomorphic
// type, DEVIRTUALIZE to a direct CALL of the impl/default fn (the static-types win); otherwise
// dispatch dynamically through the fused RESOLVE_CALL (resolve the receiver's impl and call it in one op).
int Codegen::compile_trait_call(const MRef& ref, const std::vector<const Expr*>& args,
                                const Expr* site) {
    if (static_cast<int>(args.size()) != ref.arity)
        throw CodegenError("trait method '" + ref.method + "' called with the wrong number of arguments");
    const std::string head = args.empty() ? std::string() : concrete_head_of(args[0]->ty);
    if (!head.empty()) {
        const std::string target = devirt_target(ref.trait, ref.method, head);
        if (!target.empty()) {                                    // devirtualized direct CALL
            const int inl = try_inline_call(site, target, args);
            return inl >= 0 ? inl : compile_call(target, args);
        }
    }
    // Dynamic dispatch via the FUSED RESOLVE_CALL (one op: resolve method #id for the receiver =
    // outgoing arg 0, then call). No Func temp and no separate PROTO_RESOLVE -- the args are placed
    // in the outgoing window (arg 0 = the receiver at window[frame_size]) and RESOLVE_CALL reads the
    // receiver there. Every held register sits below the outgoing window, so the argument MOVs can't
    // clobber anything the op needs.
    if (frame_size_ + static_cast<int>(args.size()) > 64)
        throw CodegenError("call argument window exceeds the 64-register r6 limit");
    std::vector<int> arg_regs;
    arg_regs.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i) arg_regs.push_back(compile_expr(*args[i]));
    for (size_t i = 0; i < arg_regs.size(); ++i)
        as_.R6(OpCode::MOV, static_cast<uint8_t>(frame_size_ + static_cast<int>(i)),
               static_cast<uint8_t>(arg_regs[i]), 0);
    as_.RESOLVE_CALL(static_cast<uint8_t>(args.size()), ref.id);
    for (size_t i = arg_regs.size(); i-- > 0;) free_if_temp(arg_regs[i]);
    int dst = alloc_temp();
    as_.MOV_TAKE(static_cast<uint8_t>(dst), static_cast<uint8_t>(frame_size_));
    return dst;
}

// Tail-position dynamic trait call, via the FUSED RESOLVE_TCO_CALL (one op: resolve method #id for the
// receiver, then reuse the frame in place). Precondition (enforced by the caller): nargs <= temp_base_, so
// the argument destinations [0, nargs) lie below the temp region and the rest-arg temps survive the writeback.
// The receiver = arg 0 lands at window[0], where RESOLVE_TCO_CALL reads it -- so, unlike emit_tail_trait_call's
// old form, there is NO PROTO_RESOLVE and NO Func temp (strictly one fewer temp; the Call measure clause's +1
// over-reserves, as for the non-tail RESOLVE_CALL). Mirrors compile_trait_call's fused dynamic path.
void Codegen::emit_tail_trait_call(const MRef& ref, const std::vector<const Expr*>& args) {
    const int nargs = static_cast<int>(args.size());
    int rrecv = compile_expr(*args[0]);                  // receiver (arg 0)
    std::vector<int> arg_regs;
    arg_regs.reserve(args.size());
    arg_regs.push_back(rrecv);
    for (size_t i = 1; i < args.size(); ++i) {           // rest args into temps (writeback-safe)
        int r = compile_expr(*args[i]);
        if (!is_temp(r)) {
            int t = alloc_temp();
            as_.R6(OpCode::MOV, static_cast<uint8_t>(t), static_cast<uint8_t>(r), 0);
            r = t;
        }
        arg_regs.push_back(r);
    }
    for (int i = 0; i < nargs; ++i)                       // -> reused frame [0, nargs); arg 0 = receiver at r0
        as_.R6(OpCode::MOV, static_cast<uint8_t>(i), static_cast<uint8_t>(arg_regs[i]), 0);
    as_.RESOLVE_TCO_CALL(static_cast<uint8_t>(nargs), ref.id);   // reads the receiver at window[0]
    for (int i = nargs; i-- > 1;) free_if_temp(arg_regs[i]);
    free_if_temp(rrecv);
}

// ----- closures (lambda lifting + by-value capture) -------------------------

namespace {
// Collect every ident-pattern binding name a pattern introduces (recursively), so the
// free-variable walk can add them to `bound` for a match arm / `for` body.
// The shadowing scan behind BOTH `free_vars_expr` (lambda captures) and the SROA escape analysis. A
// missed name fails in two different ways: a `@`-style binder inside a lambda body would look FREE
// and `emit_capture_source` would throw `internal: cannot capture`, while a missed shadow merely
// makes SROA decline.
//
// DELEGATES to `collect_pattern_names` (declared above `measure_pattern`) rather than repeating the
// walk -- callers here want set membership, callers there want a stable order, and two independent
// copies of the same traversal is precisely how a new Ast.h kind gets handled in one and forgotten
// in the other. The 4062 rot-guard lives on the surviving walk.
void add_pattern_names(const Pattern& p, std::unordered_set<std::string>& out) {
    std::vector<std::string> names;
    collect_pattern_names(p, names);
    out.insert(names.begin(), names.end());
}
} // namespace

void Codegen::collect_lambdas_expr(const Expr& e) {
    switch (e.kind) {
    case ExprKind::Unary:   collect_lambdas_expr(*static_cast<const UnaryExpr&>(e).operand); break;
    case ExprKind::Binary: { const auto& b = static_cast<const BinaryExpr&>(e);
                             collect_lambdas_expr(*b.lhs); collect_lambdas_expr(*b.rhs); break; }
    case ExprKind::Pipe:   { const auto& p = static_cast<const PipeExpr&>(e);
                             collect_lambdas_expr(*p.lhs); collect_lambdas_expr(*p.rhs); break; }
    case ExprKind::Call:   { const auto& c = static_cast<const CallExpr&>(e);
                             if (c.callee) collect_lambdas_expr(*c.callee);
                             for (const auto& a : c.args) collect_lambdas_expr(*a); break; }
    case ExprKind::Field:  collect_lambdas_expr(*static_cast<const FieldExpr&>(e).obj); break;
    case ExprKind::Index:  { const auto& ix = static_cast<const IndexExpr&>(e);
                             collect_lambdas_expr(*ix.obj); if (ix.index) collect_lambdas_expr(*ix.index); break; }
    case ExprKind::Try:    collect_lambdas_expr(*static_cast<const TryExpr&>(e).operand); break;
    case ExprKind::If:     { const auto& f = static_cast<const IfExpr&>(e);
                             collect_lambdas_expr(*f.cond);
                             if (f.then_blk) collect_lambdas_expr(*f.then_blk);
                             if (f.else_blk) collect_lambdas_expr(*f.else_blk); break; }
    case ExprKind::While:  { const auto& w = static_cast<const WhileExpr&>(e);
                             collect_lambdas_expr(*w.cond); if (w.body) collect_lambdas_expr(*w.body); break; }
    case ExprKind::Loop:   { const auto& l = static_cast<const LoopExpr&>(e);
                             if (l.body) collect_lambdas_expr(*l.body); break; }
    case ExprKind::Break:  { const auto& br = static_cast<const BreakExpr&>(e);
                             if (br.value) collect_lambdas_expr(*br.value); break; }   // `break (fn() -> …)`
    case ExprKind::For:    { const auto& fe = static_cast<const ForExpr&>(e);
                             collect_lambdas_expr(*fe.iter); if (fe.body) collect_lambdas_expr(*fe.body); break; }
    case ExprKind::Match:  { const auto& m = static_cast<const MatchExpr&>(e);
                             collect_lambdas_expr(*m.scrut);
                             for (const auto& arm : m.arms) {
                                 if (arm.guard) collect_lambdas_expr(*arm.guard);
                                 if (arm.body)  collect_lambdas_expr(*arm.body);
                             } break; }
    case ExprKind::Return: { const auto& r = static_cast<const ReturnExpr&>(e);
                             if (r.value) collect_lambdas_expr(*r.value); break; }
    case ExprKind::Block:  collect_lambdas_block(static_cast<const BlockExpr&>(e)); break;
    case ExprKind::StructLit: { const auto& sl = static_cast<const StructLit&>(e);
                             for (const auto& fi : sl.fields) if (fi.value) collect_lambdas_expr(*fi.value);
                             if (sl.base) collect_lambdas_expr(*sl.base); break; }
    case ExprKind::Tuple:  { const auto& t = static_cast<const TupleExpr&>(e);
                             for (const auto& x : t.elems) collect_lambdas_expr(*x); break; }
    case ExprKind::ListLit:{ const auto& l = static_cast<const ListLit&>(e);
                             for (const auto& x : l.elems) collect_lambdas_expr(*x); break; }
    case ExprKind::MapLit: { const auto& ml = static_cast<const MapLit&>(e);
                             for (const auto& kv : ml.entries) {
                                 collect_lambdas_expr(*kv.first); collect_lambdas_expr(*kv.second);
                             } break; }
    case ExprKind::Lambda: { const auto& lam = static_cast<const LambdaExpr&>(e);
                             const std::string name = "$lambda" + std::to_string(lambda_seq_++);
                             lambda_names_[&lam] = name;
                             lifted_.push_back(LiftedLambda{ name, &lam, collect_module_ });
                             if (lam.body) collect_lambdas_expr(*lam.body); break; }   // nested
    default: break;   // leaves: literals, Ident, Continue
    }
}

void Codegen::collect_lambdas_stmt(const Stmt& s) {
    switch (s.kind) {
    case StmtKind::Let:    { const auto& l = static_cast<const LetStmt&>(s);
                             if (l.init) collect_lambdas_expr(*l.init); break; }
    case StmtKind::Assign: { const auto& a = static_cast<const AssignStmt&>(s);
                             if (a.target) collect_lambdas_expr(*a.target);
                             if (a.value)  collect_lambdas_expr(*a.value); break; }
    case StmtKind::Expr:   collect_lambdas_expr(*static_cast<const ExprStmt&>(s).expr); break;
    }
}

void Codegen::collect_lambdas_block(const BlockExpr& b) {
    for (const auto& s : b.stmts) collect_lambdas_stmt(*s);
}

void Codegen::collect_lambdas_program(const Program& prog) {
    for (const auto& item : prog.items)
        if (item->kind == ItemKind::Fn) {
            const auto& fn = static_cast<const FnItem&>(*item);
            collect_module_ = item->module_prefix;        // lambdas in this fn body inherit its module
            if (fn.body) collect_lambdas_expr(*fn.body);
        }
    for (const MethodFn& mf : method_fns_) {              // impl / default bodies (staged by link_traits)
        collect_module_ = mf.module;                      // lambdas in a method body inherit the impl module
        if (mf.body) collect_lambdas_block(*mf.body);
    }
    // Top-level statements: usually the entry module (""), but an imported module may contribute
    // top-level stmts too, so descend per StmtItem for its own module (same order generate() collects).
    for (const auto& item : prog.items)
        if (item->kind == ItemKind::Stmt) {
            collect_module_ = item->module_prefix;
            collect_lambdas_stmt(*static_cast<const StmtItem&>(*item).stmt);
        }
}

void Codegen::free_vars_expr(const Expr& e, const std::unordered_set<std::string>& bound,
                             std::vector<std::string>& out, std::unordered_set<std::string>& seen) const {
    switch (e.kind) {
    case ExprKind::Ident: {
        const auto& id = static_cast<const IdentExpr&>(e);
        if (!id.qualifier.empty()) return;                // a qualified path is a trait call, never a capture
        if (bound.count(id.name) || is_global_name(id.name)) return;
        if (seen.insert(id.name).second) out.push_back(id.name);
        return;
    }
    case ExprKind::Unary:  free_vars_expr(*static_cast<const UnaryExpr&>(e).operand, bound, out, seen); return;
    case ExprKind::Binary: { const auto& b = static_cast<const BinaryExpr&>(e);
                             free_vars_expr(*b.lhs, bound, out, seen);
                             free_vars_expr(*b.rhs, bound, out, seen); return; }
    case ExprKind::Pipe:   { const auto& p = static_cast<const PipeExpr&>(e);
                             free_vars_expr(*p.lhs, bound, out, seen);
                             free_vars_expr(*p.rhs, bound, out, seen); return; }
    case ExprKind::Call:   { const auto& c = static_cast<const CallExpr&>(e);
                             if (c.callee) free_vars_expr(*c.callee, bound, out, seen);
                             for (const auto& a : c.args) free_vars_expr(*a, bound, out, seen); return; }
    case ExprKind::Field:  free_vars_expr(*static_cast<const FieldExpr&>(e).obj, bound, out, seen); return;
    case ExprKind::Index:  { const auto& ix = static_cast<const IndexExpr&>(e);
                             free_vars_expr(*ix.obj, bound, out, seen);
                             if (ix.index) free_vars_expr(*ix.index, bound, out, seen); return; }
    case ExprKind::Try:    free_vars_expr(*static_cast<const TryExpr&>(e).operand, bound, out, seen); return;
    case ExprKind::If:     { const auto& f = static_cast<const IfExpr&>(e);
                             free_vars_expr(*f.cond, bound, out, seen);
                             if (f.then_blk) free_vars_expr(*f.then_blk, bound, out, seen);
                             if (f.else_blk) free_vars_expr(*f.else_blk, bound, out, seen); return; }
    case ExprKind::While:  { const auto& w = static_cast<const WhileExpr&>(e);
                             free_vars_expr(*w.cond, bound, out, seen);
                             if (w.body) free_vars_expr(*w.body, bound, out, seen); return; }
    case ExprKind::Loop:   { const auto& l = static_cast<const LoopExpr&>(e);
                             if (l.body) free_vars_expr(*l.body, bound, out, seen); return; }
    case ExprKind::Break:  { const auto& br = static_cast<const BreakExpr&>(e);
                             if (br.value) free_vars_expr(*br.value, bound, out, seen); return; }
    case ExprKind::For:    { const auto& fe = static_cast<const ForExpr&>(e);
                             free_vars_expr(*fe.iter, bound, out, seen);
                             std::unordered_set<std::string> b2 = bound;
                             if (fe.pat) add_pattern_names(*fe.pat, b2);
                             if (fe.body) free_vars_expr(*fe.body, b2, out, seen); return; }
    case ExprKind::Match:  { const auto& m = static_cast<const MatchExpr&>(e);
                             free_vars_expr(*m.scrut, bound, out, seen);
                             for (const auto& arm : m.arms) {
                                 std::unordered_set<std::string> b2 = bound;
                                 if (arm.pat) add_pattern_names(*arm.pat, b2);
                                 if (arm.guard) free_vars_expr(*arm.guard, b2, out, seen);
                                 if (arm.body)  free_vars_expr(*arm.body, b2, out, seen);
                             } return; }
    case ExprKind::Return: { const auto& r = static_cast<const ReturnExpr&>(e);
                             if (r.value) free_vars_expr(*r.value, bound, out, seen); return; }
    case ExprKind::Block:  free_vars_block(static_cast<const BlockExpr&>(e), bound, out, seen); return;
    case ExprKind::StructLit: { const auto& sl = static_cast<const StructLit&>(e);
                             for (const auto& fi : sl.fields) {
                                 if (fi.value) free_vars_expr(*fi.value, bound, out, seen);
                                 else if (!bound.count(fi.name) && !is_global_name(fi.name)   // shorthand `{ x }`
                                          && seen.insert(fi.name).second) out.push_back(fi.name);
                             }
                             if (sl.base) free_vars_expr(*sl.base, bound, out, seen);   // record update base
                             return; }
    case ExprKind::Tuple:  { const auto& t = static_cast<const TupleExpr&>(e);
                             for (const auto& x : t.elems) free_vars_expr(*x, bound, out, seen); return; }
    case ExprKind::ListLit:{ const auto& l = static_cast<const ListLit&>(e);
                             for (const auto& x : l.elems) free_vars_expr(*x, bound, out, seen); return; }
    case ExprKind::MapLit: { const auto& ml = static_cast<const MapLit&>(e);
                             for (const auto& kv : ml.entries) {
                                 free_vars_expr(*kv.first, bound, out, seen);
                                 free_vars_expr(*kv.second, bound, out, seen);
                             } return; }
    case ExprKind::Lambda: { const auto& lam = static_cast<const LambdaExpr&>(e);
                             std::unordered_set<std::string> b2 = bound;   // thread bound DOWN
                             for (const Param& p : lam.params) b2.insert(p.name);
                             if (lam.body) free_vars_expr(*lam.body, b2, out, seen); return; }
    default: return;   // literals, Break, Continue
    }
}

void Codegen::free_vars_stmt(const Stmt& s, std::unordered_set<std::string>& bound,
                             std::vector<std::string>& out, std::unordered_set<std::string>& seen) const {
    switch (s.kind) {
    case StmtKind::Let: {
        const auto& l = static_cast<const LetStmt&>(s);
        if (l.init) free_vars_expr(*l.init, bound, out, seen);   // init sees the OUTER scope
        if (l.pat)  add_pattern_names(*l.pat, bound);            // the binding is visible after
        return;
    }
    case StmtKind::Assign: {
        const auto& a = static_cast<const AssignStmt&>(s);
        if (a.target) free_vars_expr(*a.target, bound, out, seen);
        if (a.value)  free_vars_expr(*a.value, bound, out, seen);
        return;
    }
    case StmtKind::Expr:
        free_vars_expr(*static_cast<const ExprStmt&>(s).expr, bound, out, seen);
        return;
    }
}

void Codegen::free_vars_block(const BlockExpr& b, std::unordered_set<std::string> bound,
                              std::vector<std::string>& out, std::unordered_set<std::string>& seen) const {
    for (const auto& s : b.stmts) free_vars_stmt(*s, bound, out, seen);
}

// =====================================================================================
// Scalar Replacement of Aggregates
//
// ONE exhaustive walker, two modes (see SroaCtx in the header). The switch has no `default:`
// under warning-as-error 4062, so a new Ast.h node fails the BUILD rather than silently reading
// as "no use of the binding here" -- which would be a missed escape, i.e. wrong code.
// =====================================================================================
#pragma warning(push)
#pragma warning(error: 4062)

void Codegen::sroa_walk_expr(const Expr& e, SroaCtx& c, const std::unordered_set<std::string>& bound) {
    if (c.name && c.escaped) return;                       // already decided; stop early
    switch (e.kind) {
    case ExprKind::IntLit: case ExprKind::DoubleLit: case ExprKind::StrLit:
    case ExprKind::BoolLit: case ExprKind::Continue:
        return;
    case ExprKind::Ident: {
        if (!c.name) return;
        const auto& id = static_cast<const IdentExpr&>(e);
        // Reaching the bare name at all IS the escape: the one non-escaping use (`x.f`) is
        // recognised in the Field arm below and never descends to here.
        if (id.qualifier.empty() && id.name == *c.name && !bound.count(id.name)) c.escaped = true;
        return;
    }
    case ExprKind::Field: {
        const auto& fe = static_cast<const FieldExpr&>(e);
        // `x.f` (a real FIELD) is the ONE use a dissolved binding supports. `x.m(...)` is the
        // SAME AST node -- cg_field_exists is the only thing separating them, which is why this
        // analysis is syntax PLUS the checker's write-backs, not syntax alone.
        if (c.name && !c.in_lambda && !fe.tuple_index && fe.obj && fe.obj->kind == ExprKind::Ident) {
            const auto& id = static_cast<const IdentExpr&>(*fe.obj);
            if (id.qualifier.empty() && id.name == *c.name && !bound.count(id.name) &&
                cg_field_exists(fe.obj->ty, fe.name))
                return;                                    // do NOT descend into obj
        }
        if (fe.obj) sroa_walk_expr(*fe.obj, c, bound);
        return;
    }
    case ExprKind::Unary:  sroa_walk_expr(*static_cast<const UnaryExpr&>(e).operand, c, bound); return;
    case ExprKind::Binary: { const auto& b = static_cast<const BinaryExpr&>(e);
                             sroa_walk_expr(*b.lhs, c, bound); sroa_walk_expr(*b.rhs, c, bound); return; }
    case ExprKind::Pipe:   { const auto& p = static_cast<const PipeExpr&>(e);
                             sroa_walk_expr(*p.lhs, c, bound); sroa_walk_expr(*p.rhs, c, bound); return; }
    case ExprKind::Call:   { const auto& cl = static_cast<const CallExpr&>(e);
                             // S3: passing the binding to a FIELD-ONLY parameter is not an escape.
                             if (c.name && !c.in_lambda && sroa_call_field_only(cl, c, bound)) return;
                             if (cl.callee) sroa_walk_expr(*cl.callee, c, bound);
                             for (const auto& a : cl.args) sroa_walk_expr(*a, c, bound); return; }
    case ExprKind::Index:  { const auto& ix = static_cast<const IndexExpr&>(e);
                             sroa_walk_expr(*ix.obj, c, bound); sroa_walk_expr(*ix.index, c, bound); return; }
    case ExprKind::Try:    sroa_walk_expr(*static_cast<const TryExpr&>(e).operand, c, bound); return;
    case ExprKind::If:     { const auto& f = static_cast<const IfExpr&>(e);
                             sroa_walk_expr(*f.cond, c, bound); sroa_walk_expr(*f.then_blk, c, bound);
                             if (f.else_blk) sroa_walk_expr(*f.else_blk, c, bound); return; }
    case ExprKind::Match:  { const auto& m = static_cast<const MatchExpr&>(e);
                             sroa_walk_expr(*m.scrut, c, bound);
                             for (const MatchArm& a : m.arms) {
                                 std::unordered_set<std::string> b2 = bound;
                                 if (a.pat) add_pattern_names(*a.pat, b2);   // an arm binder SHADOWS
                                 if (a.guard) sroa_walk_expr(*a.guard, c, b2);
                                 if (a.body)  sroa_walk_expr(*a.body, c, b2);
                             }
                             return; }
    case ExprKind::While:  { const auto& w = static_cast<const WhileExpr&>(e);
                             sroa_walk_expr(*w.cond, c, bound); sroa_walk_expr(*w.body, c, bound); return; }
    case ExprKind::For:    { const auto& f = static_cast<const ForExpr&>(e);
                             sroa_walk_expr(*f.iter, c, bound);
                             std::unordered_set<std::string> b2 = bound;
                             if (f.pat) add_pattern_names(*f.pat, b2);
                             if (f.body) sroa_walk_expr(*f.body, c, b2); return; }
    case ExprKind::Loop:   sroa_walk_expr(*static_cast<const LoopExpr&>(e).body, c, bound); return;
    case ExprKind::Break:  { const auto& b = static_cast<const BreakExpr&>(e);
                             if (b.value) sroa_walk_expr(*b.value, c, bound); return; }
    case ExprKind::Return: { const auto& r = static_cast<const ReturnExpr&>(e);
                             if (r.value) sroa_walk_expr(*r.value, c, bound); return; }
    case ExprKind::Block:  sroa_walk_block(static_cast<const BlockExpr&>(e), c, bound); return;
    case ExprKind::Lambda: { const auto& lam = static_cast<const LambdaExpr&>(e);
                             // DESCEND, but with the `x.f` exemption switched OFF: the body is
                             // lifted into its own function, where the binding is a by-value
                             // CAPTURE (emit_capture_source MOVs the whole register), so even a
                             // field read there needs an object. Any free use is an escape.
                             // scan_inline_body deliberately does not descend at all; that is safe
                             // only because a has_lambda target is rejected outright.
                             std::unordered_set<std::string> b2 = bound;
                             for (const Param& p : lam.params) b2.insert(p.name);
                             const bool saved = c.in_lambda;
                             c.in_lambda = true;
                             if (lam.body) sroa_walk_expr(*lam.body, c, b2);
                             c.in_lambda = saved; return; }
    case ExprKind::StructLit: { const auto& sl = static_cast<const StructLit&>(e);
                             for (const FieldInit& fi : sl.fields) {
                                 if (fi.value) { sroa_walk_expr(*fi.value, c, bound); continue; }
                                 // SHORTHAND `S { x }`: no Ident node exists -- compile_struct_lit
                                 // resolves the LOCAL by the FIELD's name. An Ident-based scan
                                 // misses this use entirely, which would dissolve a binding that
                                 // is then read as a whole value.
                                 if (c.name && fi.name == *c.name && !bound.count(fi.name)) c.escaped = true;
                             }
                             if (sl.base) sroa_walk_expr(*sl.base, c, bound); return; }
    case ExprKind::Tuple:  { for (const auto& x : static_cast<const TupleExpr&>(e).elems)
                                 sroa_walk_expr(*x, c, bound); return; }
    case ExprKind::ListLit:{ for (const auto& x : static_cast<const ListLit&>(e).elems)
                                 sroa_walk_expr(*x, c, bound); return; }
    case ExprKind::MapLit: { for (const auto& kv : static_cast<const MapLit&>(e).entries) {
                                 sroa_walk_expr(*kv.first, c, bound);
                                 sroa_walk_expr(*kv.second, c, bound); }
                             return; }
    }
}

void Codegen::sroa_walk_stmt(const Stmt& s, SroaCtx& c, std::unordered_set<std::string>& bound) {
    if (c.name && c.escaped) return;
    switch (s.kind) {
    case StmtKind::Let: {
        const auto& l = static_cast<const LetStmt&>(s);
        if (l.init) sroa_walk_expr(*l.init, c, bound);      // the init sees the OUTER scope
        if (l.pat)  add_pattern_names(*l.pat, bound);       // ... the binding SHADOWS after it
        return;
    }
    case StmtKind::Assign: {
        const auto& a = static_cast<const AssignStmt&>(s);
        if (a.target) sroa_scan_target(*a.target, c, bound);
        if (a.value)  sroa_walk_expr(*a.value, c, bound);
        return;
    }
    case StmtKind::Expr:
        sroa_walk_expr(*static_cast<const ExprStmt&>(s).expr, c, bound);
        return;
    }
}

#pragma warning(pop)

void Codegen::sroa_walk_block(const BlockExpr& b, SroaCtx& c, std::unordered_set<std::string> bound) {
    // Discovery visits EVERY block, including this one, so nested `let`s are considered too.
    if (!c.name) sroa_try_block(b);
    for (const auto& s : b.stmts) sroa_walk_stmt(*s, c, bound);
}

// An occurrence of the binding at a FIELD-ONLY parameter position does not need an object: the
// expansion binds that parameter to the same field registers. This is the whole of S3's reach --
// without it `let oc = ...` dies on `oc.dot(r.dir)`, whose receiver is a plain whole-value use.
//
// TWO conditions make emit unable to decline, which matters because a decline would leave the
// argument with no object to pass and nothing to fall back to at that point:
//   - a FIELD callee only (`recv.m(a)`). An Ident callee is declined by compile_call_dispatch when
//     a caller local shadows the name, and the pre-pass cannot see scope_ to predict that.
//   - inline depth >= 2. An expanded body runs at depth <= 1, so a nested call's `depth >= max`
//     test can only fire when max is 1.
// Budget revocation is handled by the ladder's ORDER: it gives up SROA before inlining, so a
// region can never keep a dissolution whose expansion it has revoked.
bool Codegen::sroa_call_field_only(const CallExpr& cl, SroaCtx& c,
                                   const std::unordered_set<std::string>& bound) {
    if (!inlining_ || !sroa_ || inline_max_depth_ < 2) return false;
    if (!cl.callee || cl.callee->kind != ExprKind::Field) return false;
    std::vector<const Expr*> args, margs;
    for (const auto& a : cl.args) args.push_back(a.get());
    const std::string tgt = direct_call_target(cl.callee.get(), args, margs);
    const InlineTarget* t = inline_target_for(tgt, margs.size());
    if (!t || t->params_field_only.size() != margs.size()) return false;
    bool ours_here = false;
    for (size_t i = 0; i < margs.size(); ++i) {
        const bool ours = margs[i]->kind == ExprKind::Ident &&
                          static_cast<const IdentExpr&>(*margs[i]).qualifier.empty() &&
                          static_cast<const IdentExpr&>(*margs[i]).name == *c.name &&
                          !bound.count(*c.name);
        if (ours) {
            if (!t->params_field_only[i]) return false;   // passed on as a whole value after all
            ours_here = true;
            continue;
        }
        sroa_walk_expr(*margs[i], c, bound);
    }
    return ours_here;   // absent here -> let the ordinary walk handle the call
}

int Codegen::sroa_param_slots(const InlineTarget& t, size_t i, const Expr& arg) const {
    if (!sroa_ || i >= t.params_field_only.size() || !t.params_field_only[i]) return 1;
    const std::string sn = struct_name_of(arg.ty);
    const StructDef* sd = sn.empty() ? nullptr : find_struct(sn);
    if (!sd || sd->is_transparent || sd->is_int_enum) return 1;
    const int nf = static_cast<int>(sd->field_names.size());
    return (nf >= 1 && nf <= sroa_max_fields_) ? nf : 1;
}

void Codegen::sroa_scan_target(const Expr& t, SroaCtx& c, const std::unordered_set<std::string>& bound) {
    if (!c.name) { sroa_walk_expr(t, c, bound); return; }   // discovery: just traverse
    // Walk down to the place's ROOT. `x.f = v` and `x[i].g = v` both WRITE through `x`, so the
    // Field arm's "x.f does not escape" exemption must NOT apply here: S1 dissolves only an
    // immutable binding, and compile_field_assign knows nothing about dissolution -- it would
    // write a materialised temp while every later read came from the untouched field register.
    const Expr* cur = &t;
    for (;;) {
        if (cur->kind == ExprKind::Field) { cur = static_cast<const FieldExpr&>(*cur).obj.get(); continue; }
        if (cur->kind == ExprKind::Index) {
            const auto& ix = static_cast<const IndexExpr&>(*cur);
            sroa_walk_expr(*ix.index, c, bound);            // the INDEX is an ordinary read
            cur = ix.obj.get(); continue;
        }
        break;
    }
    if (!cur) return;
    if (cur->kind == ExprKind::Ident) {
        const auto& id = static_cast<const IdentExpr&>(*cur);
        if (id.qualifier.empty() && id.name == *c.name && !bound.count(id.name)) c.escaped = true;
        return;
    }
    sroa_walk_expr(*cur, c, bound);                          // an exotic place: scan normally
}

void Codegen::sroa_try_block(const BlockExpr& b) {
    std::vector<const Stmt*> v;
    v.reserve(b.stmts.size());
    for (const auto& s : b.stmts) v.push_back(s.get());
    sroa_try_stmts(v);
}

// Consider every `let` of ONE statement sequence. A binding is scoped to the statements AFTER it,
// so that is exactly the region scanned -- and a later `let` of the same name shadows, which the
// walker's `bound` handles for free.
void Codegen::sroa_try_stmts(const std::vector<const Stmt*>& stmts) {
    for (size_t i = 0; i < stmts.size(); ++i) {
        if (stmts[i]->kind != StmtKind::Let) continue;
        const auto& l = static_cast<const LetStmt&>(*stmts[i]);
        // v1 eligibility. Every clause is a deliberate restriction, not an oversight:
        //   - a simple `let name = ...` (a destructuring pattern is a different structural hook,
        //     measure_pattern(...).binding_locals, and does not appear in the measured workload);
        //   - NOT `mut`, so the value cannot change and there is nothing for a write to miss;
        //   - a named struct in structs_, not transparent and not int-backed (both ERASE to a bare
        //     immediate already -- there is no object to dissolve);
        //   - a literal `StructLit` initialiser with no `..base` (the record-update path evaluates
        //     by SLOT rather than source order and needs a GET_PROP per unlisted field anyway);
        //   - at most sroa_max_fields_ fields -- and 0 dissolves nothing, the A/B's null control.
        if (!l.pat || l.pat->kind != PatKind::Ident || l.is_mut || !l.init) continue;
        const std::string sn = struct_name_of(l.init->ty);
        const StructDef* sd = sn.empty() ? nullptr : find_struct(sn);
        if (!sd || sd->is_transparent || sd->is_int_enum) continue;
        const int nf = static_cast<int>(sd->field_names.size());
        if (nf < 1 || nf > sroa_max_fields_) continue;
        // The initializer must be able to PRODUCE the value field by field; otherwise dissolving
        // only moves the cost around (the explode fallback would materialise the object anyway).
        // Two shapes today: a plain literal (S1), and a call whose callee body ENDS in one (S2).
        bool init_ok = false;
        if (l.init->kind == ExprKind::StructLit) {
            const auto& sl = static_cast<const StructLit&>(*l.init);
            // Completeness: without `..base` the checker requires every field, and a dissolved
            // literal has no NEW_STRUCT to default-initialise a slot nobody wrote. A short literal
            // would leave a register holding whatever the previous frame left there -- and it sits
            // BELOW frame_size, so the collector would scan it as a root. Check rather than trust.
            init_ok = !sl.base && static_cast<int>(sl.fields.size()) == nf;
        } else if (l.init->kind == ExprKind::Call && inlining_) {
            const auto& cl = static_cast<const CallExpr&>(*l.init);
            std::vector<const Expr*> args, margs;
            for (const auto& a : cl.args) args.push_back(a.get());
            // direct_call_target writes margs, inline_target_for reads it -- SEQUENCED through
            // named temporaries, never as two arguments of one call (see measure_expr's Call arm:
            // as arguments the order is unspecified and MSVC got it backwards, which silently
            // disabled the whole inliner).
            const std::string tgt = direct_call_target(cl.callee.get(), args, margs);
            const InlineTarget* t = inline_target_for(tgt, margs.size());
            init_ok = t && sroa_producer_ok(*t, static_cast<size_t>(nf));
        }
        if (!init_ok) continue;
        const auto& name = static_cast<const IdentPat&>(*l.pat).name;
        SroaCtx c; c.name = &name;
        std::unordered_set<std::string> bound;
        for (size_t j = i + 1; j < stmts.size() && !c.escaped; ++j)
            sroa_walk_stmt(*stmts[j], c, bound);
        // emplace, not assign: the count must not double if a body is ever reached twice by the
        // discovery walk. (It is not today -- collect_lambdas_program's traversal, which this
        // mirrors, visits each body once -- but the counter is a diagnostic, and a diagnostic that
        // can drift is worse than none.)
        if (!c.escaped && sroa_lets_.emplace(&l, nf).second) ++sroa_permitted_;
    }
}

// The pre-pass. Mirrors collect_lambdas_program's traversal exactly -- fn bodies, then the method
// bodies link_traits staged, then the top-level statements -- so the two agree about what "every
// body in the program" means. A lambda body needs no separate entry: the walker DESCENDS into one,
// so a `let` inside it is discovered here and measured later, when that lifted fn is planned.
void Codegen::collect_sroa_lets(const Program& prog, const std::vector<const Stmt*>& top) {
    SroaCtx disc;                                        // name == nullptr => discovery mode
    const std::unordered_set<std::string> none;
    for (const auto& item : prog.items)
        if (item->kind == ItemKind::Fn) {
            const auto& fn = static_cast<const FnItem&>(*item);
            if (fn.body) sroa_walk_expr(*fn.body, disc, none);
        }
    for (const MethodFn& mf : method_fns_)
        if (mf.body) sroa_walk_block(*mf.body, disc, none);
    // The top level is a flat statement list, not a BlockExpr, so it gets both halves by hand:
    // its own `let`s are candidates, and its statements are descended for nested blocks.
    sroa_try_stmts(top);
    { std::unordered_set<std::string> b2;
      for (const Stmt* s : top) sroa_walk_stmt(*s, disc, b2); }
}

void Codegen::analyze_captures() {
    for (const LiftedLambda& ll : lifted_) {
        std::unordered_set<std::string> bound;
        for (const Param& p : ll.lam->params) bound.insert(p.name);
        std::vector<std::string> caps;
        std::unordered_set<std::string> seen;
        if (ll.lam->body) free_vars_expr(*ll.lam->body, bound, caps, seen);
        lambda_captures_[ll.lam] = std::move(caps);
    }
}

void Codegen::set_current_lambda_captures(const LambdaExpr* lam) {
    current_lambda_captures_.clear();
    auto it = lambda_captures_.find(lam);
    if (it == lambda_captures_.end()) return;
    for (size_t i = 0; i < it->second.size(); ++i)
        current_lambda_captures_[it->second[i]] = static_cast<int>(i);
}

void Codegen::emit_capture_source(const std::string& name, int slot) {
    const int r = local_value_reg(name);   // guards a dissolved binding (SROA)
    if (r >= 0) {                                        // an enclosing local -> copy by value
        as_.R6(OpCode::MOV, static_cast<uint8_t>(slot), static_cast<uint8_t>(r), 0);
        return;
    }
    auto cit = current_lambda_captures_.find(name);      // a capture of the ENCLOSING lambda
    if (cit != current_lambda_captures_.end()) {         // -> re-read it from that closure (nested)
        as_.LOAD_CAPTURE(static_cast<uint8_t>(slot), static_cast<uint16_t>(cit->second));
        return;
    }
    throw CodegenError("internal: cannot capture '" + name +
                       "' -- not an enclosing local or a capture of the enclosing lambda");
}

int Codegen::compile_lambda(const LambdaExpr& lam) {
    auto nit = lambda_names_.find(&lam);
    if (nit == lambda_names_.end()) throw CodegenError("internal: lambda was not lifted", lam.line, lam.col);
    const uint16_t fid = as_.func_id(nit->second);
    const std::vector<std::string>& caps = lambda_captures_[&lam];
    if (caps.empty()) {                                  // capture-free -> a Func immediate
        int t = alloc_temp();
        as_.LOAD_FN(static_cast<uint8_t>(t), fid);
        return t;
    }
    // Build a contiguous capture window of temps, fill each with the free variable's current
    // value, then MAKE_CLOSURE with the destination reusing the base slot.
    int base = -1;
    for (size_t i = 0; i < caps.size(); ++i) {
        int slot = alloc_temp();
        if (i == 0) base = slot;
        emit_capture_source(caps[i], slot);
    }
    as_.MAKE_CLOSURE(static_cast<uint8_t>(base), fid, static_cast<uint8_t>(base));
    for (size_t i = caps.size(); i-- > 1;) free_if_temp(base + static_cast<int>(i));   // LIFO
    return base;
}

// ----- tail position ---------------------------------------------------------

void Codegen::emit_return_nil() {
    as_.load_constant(static_cast<uint8_t>(return_target()), Value::fromNil());
    emit_return_jump();
}

// RET, or -- inside an expansion -- a jump to that expansion's exit label. Every `return`-shaped
// emission routes through here, so a site added later cannot accidentally return from the caller.
void Codegen::emit_return_jump() {
    if (inline_stack_.empty()) as_.J(OpCode::RET);
    else                       as_.J(OpCode::J, inline_stack_.back().exit_label);
}

void Codegen::compile_ret_value(const Expr& e) {
    // Return-boundary coercion: a Double-returning fn with an Int tail value widens via I2D.
    // The destination is r0 for a real function and the expansion's result register inside one --
    // writing r0 there would clobber the CALLER's first parameter.
    const int dst = return_target();
    if (current_fn_ret_double_ && is_int_ty(e.ty)) {
        int r = compile_coerced(e, true);
        if (r != dst) as_.R6(OpCode::MOV, static_cast<uint8_t>(dst), static_cast<uint8_t>(r), 0);
        free_if_temp(r);
    } else {
        compile_into(e, dst);
    }
}

void Codegen::compile_tail(const Expr& e) {
    // An expansion compiles its body in VALUE position precisely so this family -- which emits RET
    // and TCO_CALL, both of which leave the CALLER -- is unreachable from it.
    assert(inline_stack_.empty() && "compile_tail reached from inside an inline expansion");
    // If the enclosing fn returns Double and this tail value is Int, a widening is required
    // at the RET boundary -- so it cannot TCO (the callee would return an un-widened Int).
    // Route to the coercing value + RET path.
    if (current_fn_ret_double_ && is_int_ty(e.ty)) {
        compile_ret_value(e);
        as_.J(OpCode::RET);
        return;
    }
    switch (e.kind) {
    case ExprKind::Block:
        compile_tail_block(static_cast<const BlockExpr&>(e));
        return;
    case ExprKind::If:
        compile_tail_if(static_cast<const IfExpr&>(e));
        return;
    case ExprKind::Match:
        compile_tail_match(static_cast<const MatchExpr&>(e));
        return;
    case ExprKind::Return: {
        const auto& r = static_cast<const ReturnExpr&>(e);   // `return e` == `e` in tail position
        if (r.value) compile_tail(*r.value);
        else         emit_return_nil();
        return;
    }
    case ExprKind::Call: {
        const auto& c = static_cast<const CallExpr&>(e);
        std::vector<const Expr*> args;
        for (const auto& a : c.args) args.push_back(a.get());
        std::string name, qualifier;
        const bool callee_is_ident = c.callee && c.callee->kind == ExprKind::Ident;
        if (callee_is_ident) {
            const auto& id = static_cast<const IdentExpr&>(*c.callee);
            name = id.name; qualifier = id.qualifier;
        }
        compile_tail_call_shape(c.callee.get(), callee_is_ident, name, qualifier, args, e);
        return;
    }
    case ExprKind::Pipe: {
        const auto& p = static_cast<const PipeExpr&>(e);
        std::vector<const Expr*> args;
        const Expr* callee = pipe_call_shape(p, args);
        if (callee && callee->kind == ExprKind::Ident) {
            const auto& id = static_cast<const IdentExpr&>(*callee);
            compile_tail_call_shape(callee, /*callee_is_ident=*/true, id.name, id.qualifier, args, e);
            return;
        }
        break;   // an unusual pipe target -> value + RET
    }
    default:
        break;
    }
    compile_ret_value(e);          // value + RET (compile_into evaluates then moves to r0)
    as_.J(OpCode::RET);
}

void Codegen::compile_tail_block(const BlockExpr& b) {
    const size_t scope_mark  = scope_.size();
    const int    locals_mark = n_locals_;

    if (b.stmts.empty()) {
        emit_return_nil();
    } else {
        const size_t n = b.stmts.size();
        for (size_t i = 0; i + 1 < n; ++i) compile_stmt(*b.stmts[i]);   // leading statements
        const Stmt& last = *b.stmts[n - 1];
        if (last.kind == StmtKind::Expr)
            compile_tail(*static_cast<const ExprStmt&>(last).expr);      // emits RET / TCO
        else {
            compile_stmt(last);
            emit_return_nil();                                           // a trailing let yields unit
        }
    }
    scope_.resize(scope_mark);
    n_locals_ = locals_mark;       // let a sibling tail branch reuse these registers
}

void Codegen::compile_tail_if(const IfExpr& f) {
    const std::string else_lbl = fresh_label("telse_");
    branch_if_false(*f.cond, else_lbl);
    compile_tail(*f.then_blk);                 // each arm RETs / loops on its own
    as_.label(else_lbl);
    if (f.else_blk) compile_tail(*f.else_blk);
    else            emit_return_nil();
}

void Codegen::compile_tail_call_shape(const Expr* callee, bool callee_is_ident,
                                      const std::string& name, const std::string& qualifier,
                                      const std::vector<const Expr*>& args, const Expr& fallback) {
    const int nargs = static_cast<int>(args.size());
    // The checker's routing decision for this callee (see Codegen::user_fn_ref): a bare name it
    // resolved past a shadowing entry-module fn must take the ambient route here too.
    const bool calls_user_fn = callee_is_ident && callee && callee->kind == ExprKind::Ident &&
                               user_fn_ref(static_cast<const IdentExpr&>(*callee));

    // A constructor call (`Ok(x)`, `Some(v)`), a container builtin (`array`/`vec`/`push`/`len`), or a
    // native I/O call (`rawRun`/`readFile`/...) in tail position is a VALUE, not a TCO-able function
    // call -- compute it and RET. (A native must be caught here: it is neither a ctor nor a builtin,
    // so without this it would fall through to the indirect-tail path and try to load the native name
    // as a bare function value -- "undefined variable".)
    if (callee_is_ident && qualifier.empty() && local_reg(name) < 0 &&
        (is_ctor_name(name) ||
         (!calls_user_fn && (is_builtin_name(name) || native_id_of(name) >= 0)))) {
        compile_ret_value(fallback);
        as_.J(OpCode::RET);
        return;
    }

    // A qualified INHERENT call `Type::method(x)` in tail position -> a direct TCO_CALL of the
    // staged body, exactly like the `.` form (which reaches the same fn through the Field branch
    // below). This branch is NOT optional: is_trait_call already excludes it, so without it the
    // call would fall through to the qualifier-must-be-empty paths and end as value + RET.
    if (callee_is_ident && callee && callee->kind == ExprKind::Ident &&
        static_cast<const IdentExpr&>(*callee).inherent) {
        const std::string target = inherent_fn_name(static_cast<const IdentExpr&>(*callee));
        auto it = fn_arity_.find(target);
        if (it != fn_arity_.end() && nargs == it->second) { emit_tail_direct_call(target, args); return; }
        compile_ret_value(fallback);
        as_.J(OpCode::RET);
        return;
    }

    // A trait method call in tail position: devirtualize to a direct TCO_CALL of the impl/default
    // fn when the receiver is concrete (the static-types win -- O(1) self-through-trait recursion),
    // else dispatch dynamically via TCO_CALL_INDIRECT when the args fit below the temp region.
    if (callee_is_ident && is_trait_call(callee)) {
        const MRef ref = resolve_method(qualifier, name);
        const std::string head = args.empty() ? std::string() : concrete_head_of(args[0]->ty);
        const std::string target = head.empty() ? std::string()
                                                 : devirt_target(ref.trait, ref.method, head);
        if (!target.empty()) { emit_tail_direct_call(target, args); return; }
        if (nargs <= temp_base_) { emit_tail_trait_call(ref, args); return; }
        compile_ret_value(fallback);
        as_.J(OpCode::RET);
        return;
    }

    // Method-call syntax `recv.method(args)` in tail position (S1): a Field callee that is a method
    // (not a real field) -> the receiver is arg 0; devirtualize to a direct TCO or a dynamic
    // TCO_CALL_INDIRECT, mirroring the trait-tail branch above.
    if (callee && callee->kind == ExprKind::Field) {
        const auto& fld = static_cast<const FieldExpr&>(*callee);
        if (!fld.tuple_index && !cg_field_exists(fld.obj->ty, fld.name)) {
            std::vector<const Expr*> margs;
            margs.push_back(fld.obj.get());
            for (const Expr* a : args) margs.push_back(a);
            const std::string tyhead = (fld.obj->ty && fld.obj->ty->kind == TyKind::Named)
                                       ? fld.obj->ty->name : std::string();
            if (is_inherent_method(tyhead, fld.name)) {          // inherent -> direct TCO
                emit_tail_direct_call("$inherent$" + tyhead + "$" + fld.name, margs);
                return;
            }
            if (is_trait_method_name(fld.name)) {
                const MRef ref = resolve_method("", fld.name);
                const std::string head = concrete_head_of(fld.obj->ty);
                const std::string target = head.empty() ? std::string()
                                                         : devirt_target(ref.trait, ref.method, head);
                if (!target.empty()) { emit_tail_direct_call(target, margs); return; }
                if (static_cast<int>(margs.size()) <= temp_base_) { emit_tail_trait_call(ref, margs); return; }
                compile_ret_value(fallback);
                as_.J(OpCode::RET);
                return;
            }
        }
    }

    // Self-tail-call: an unqualified Ident naming the current function with matching arity.
    if (callee_is_ident && qualifier.empty() && current_fn_nparams_ >= 0 &&
        name == current_fn_name_ && nargs == current_fn_nparams_) {
        emit_tail_self_call(args);
        return;
    }
    // A direct non-self tail call to another named top-level fn reuses the frame via
    // TCO_CALL <label> (mutual recursion in O(1) stack). Requires only nargs == the callee's
    // arity (the args land in the callee's [0, arity) window); the ascending writeback into
    // [0, nargs) is clobber-free (sources are the contiguous temp block above).
    if (callee_is_ident && qualifier.empty() && local_reg(name) < 0 && calls_user_fn) {
        auto it = fn_arity_.find(name);
        if (it != fn_arity_.end() && nargs == it->second) {
            emit_tail_direct_call(name, args);
            return;
        }
        compile_ret_value(fallback);
        as_.J(OpCode::RET);
        return;
    }
    // A tail call to a function VALUE (a local/param, an arbitrary expression) reuses the
    // frame via TCO_CALL_INDIRECT, but only when the arguments' destination [0, nargs) lies
    // wholly below the temp region (nargs <= temp_base_) -- then staging the callee/args in
    // temps cannot clobber them. Otherwise value + RET (still correct, just not O(1) stack).
    const bool static_direct = callee_is_ident && qualifier.empty() &&
                               local_reg(name) < 0 && calls_user_fn;
    if (callee && !static_direct && qualifier.empty() && nargs <= temp_base_) {
        emit_tail_indirect_call(*callee, args);
        return;
    }
    compile_ret_value(fallback);
    as_.J(OpCode::RET);
}

void Codegen::emit_tail_self_call(const std::vector<const Expr*>& args) {
    std::vector<int> arg_regs;
    arg_regs.reserve(args.size());
    for (const Expr* a : args) {
        int r = compile_expr(*a);
        if (!is_temp(r)) {                            // a param/local: copy into a temp so the
            int t = alloc_temp();                     // writeback into r0.. can't clobber a source
            as_.R6(OpCode::MOV, static_cast<uint8_t>(t), static_cast<uint8_t>(r), 0);
            r = t;
        }
        arg_regs.push_back(r);
    }
    for (size_t i = 0; i < arg_regs.size(); ++i)      // temps sit above the params -> conflict-free
        as_.R6(OpCode::MOV, static_cast<uint8_t>(i), static_cast<uint8_t>(arg_regs[i]), 0);
    for (size_t i = arg_regs.size(); i-- > 0;) free_if_temp(arg_regs[i]);
    as_.TCO_CALL(current_fn_name_);
}

void Codegen::emit_tail_direct_call(const std::string& name, const std::vector<const Expr*>& args) {
    std::vector<int> arg_regs;
    arg_regs.reserve(args.size());
    for (const Expr* a : args) {
        int r = compile_expr(*a);
        if (!is_temp(r)) {
            int t = alloc_temp();
            as_.R6(OpCode::MOV, static_cast<uint8_t>(t), static_cast<uint8_t>(r), 0);
            r = t;
        }
        arg_regs.push_back(r);
    }
    for (size_t i = 0; i < arg_regs.size(); ++i)
        as_.R6(OpCode::MOV, static_cast<uint8_t>(i), static_cast<uint8_t>(arg_regs[i]), 0);
    for (size_t i = arg_regs.size(); i-- > 0;) free_if_temp(arg_regs[i]);
    as_.TCO_CALL(name);
}

void Codegen::emit_tail_indirect_call(const Expr& callee, const std::vector<const Expr*>& args) {
    const int nargs = static_cast<int>(args.size());
    // Evaluate the callee FIRST into a temp (so it lives ABOVE the argument window [0, nargs)
    // and cannot be overwritten by the argument moves below).
    int rcallee = compile_expr(callee);
    if (!is_temp(rcallee)) {
        int t = alloc_temp();
        as_.R6(OpCode::MOV, static_cast<uint8_t>(t), static_cast<uint8_t>(rcallee), 0);
        rcallee = t;
    }
    std::vector<int> arg_regs;
    arg_regs.reserve(args.size());
    for (const Expr* a : args) {
        int r = compile_expr(*a);
        if (!is_temp(r)) {
            int t = alloc_temp();
            as_.R6(OpCode::MOV, static_cast<uint8_t>(t), static_cast<uint8_t>(r), 0);
            r = t;
        }
        arg_regs.push_back(r);
    }
    for (int i = 0; i < nargs; ++i)                   // -> reused frame [0, nargs); sources are temps above
        as_.R6(OpCode::MOV, static_cast<uint8_t>(i), static_cast<uint8_t>(arg_regs[i]), 0);
    as_.TCO_CALL_INDIRECT(static_cast<uint8_t>(rcallee), static_cast<uint8_t>(nargs));
    for (int i = nargs; i-- > 0;) free_if_temp(arg_regs[i]);
    free_if_temp(rcallee);
}

void Codegen::compile_function(const std::string& name, const std::vector<std::string>& param_names,
                               const BlockExpr& body, bool ret_double) {
    begin_frame(fn_plans_.at(name));
    emit_region_name_ = name;
    as_.label(name);
    current_fn_name_       = name;
    current_fn_nparams_    = static_cast<int>(param_names.size());
    current_fn_ret_double_ = ret_double;

    for (const std::string& pn : param_names) {       // params occupy r0..r(n-1)
        int reg = alloc_local();
        scope_.push_back(Local{ pn, reg, false });
    }
    compile_tail(body);                               // emits the RET / TCO_CALL itself

    // Emitter self-check: the emitted peak never exceeds the plan (an under-count would have
    // already thrown at the alloc site). `<=`, not `==`: the measure_* walk is a deliberate
    // upper bound (over-reservation is harmless), so equality would false-fire.
    assert(emit_peak_temp_   <= plan_peak_temp_ && "measure_* temp over-run vs plan");
    assert(emit_peak_locals_ <= plan_n_locals_ && "measure_* local over-run vs plan");

    current_fn_name_.clear();
    current_fn_nparams_    = -1;
    current_fn_ret_double_ = false;
}

void Codegen::compile_top_level(const std::vector<const Stmt*>& stmts) {
    emit_region_name_ = "<script>";
    int value = -1;
    for (size_t i = 0; i < stmts.size(); ++i) {
        const Stmt& s = *stmts[i];
        const bool last = (i + 1 == stmts.size());
        if (last && s.kind == StmtKind::Expr)
            value = compile_expr(*static_cast<const ExprStmt&>(s).expr);
        else
            compile_stmt(s);
    }
    if (value < 0)
        as_.load_constant(0, Value::fromNil());          // empty program / trailing statement
    else if (value != 0)
        as_.R6(OpCode::MOV, 0, static_cast<uint8_t>(value), 0);
    as_.J(OpCode::HALT);
    // Emitter self-check for the top-level "main" frame (see compile_function).
    assert(emit_peak_temp_   <= plan_peak_temp_ && "measure_* temp over-run vs plan (top-level)");
    assert(emit_peak_locals_ <= plan_n_locals_ && "measure_* local over-run vs plan (top-level)");
}

// ===== inlining: the callee table + the eligibility decision ==========
//
// Everything here is CONSULTED by two passes -- the measure (which records a site) and emit
// (which takes it) -- and is therefore built strictly BEFORE the measure and never mutated
// afterwards. Reading anything that changes as the walk proceeds (fn_plans_, scope_, the frame
// counters) would make the two passes disagree about the frame, which is the frame_size
// GC-roots contract broken.

// One traversal collecting every fact the eligibility decision needs. The switch is EXHAUSTIVE
// on purpose (see the header): a `return` inside a node this scan does not know about would be
// emitted as a bare RET *from the caller*.
#pragma warning(push)
#pragma warning(error: 4062)   // 'enumerator not handled in switch' -- the rot-guard, /W4 is not /WX
void Codegen::scan_inline_body(const Expr& e, InlineTarget& t) const {
    ++t.nodes;
    switch (e.kind) {
    case ExprKind::IntLit: case ExprKind::DoubleLit: case ExprKind::StrLit:
    case ExprKind::BoolLit: case ExprKind::Ident:  case ExprKind::Continue:
        return;                                        // leaves
    case ExprKind::Unary:  scan_inline_body(*static_cast<const UnaryExpr&>(e).operand, t); return;
    case ExprKind::Binary: { const auto& b = static_cast<const BinaryExpr&>(e);
                             scan_inline_body(*b.lhs, t); scan_inline_body(*b.rhs, t); return; }
    case ExprKind::Pipe:   { const auto& p = static_cast<const PipeExpr&>(e);
                             scan_inline_body(*p.lhs, t); scan_inline_body(*p.rhs, t); return; }
    case ExprKind::Call:   { const auto& c = static_cast<const CallExpr&>(e);
                             if (c.callee) scan_inline_body(*c.callee, t);
                             for (const auto& a : c.args) scan_inline_body(*a, t); return; }
    case ExprKind::Field:  scan_inline_body(*static_cast<const FieldExpr&>(e).obj, t); return;
    case ExprKind::Index:  { const auto& ix = static_cast<const IndexExpr&>(e);
                             scan_inline_body(*ix.obj, t); scan_inline_body(*ix.index, t); return; }
    case ExprKind::Try:    t.has_try = true;           // `?` emits a RET from the enclosing fn
                           scan_inline_body(*static_cast<const TryExpr&>(e).operand, t); return;
    case ExprKind::If:     { const auto& f = static_cast<const IfExpr&>(e);
                             scan_inline_body(*f.cond, t); scan_inline_body(*f.then_blk, t);
                             if (f.else_blk) scan_inline_body(*f.else_blk, t); return; }
    case ExprKind::Match:  { const auto& m = static_cast<const MatchExpr&>(e);
                             scan_inline_body(*m.scrut, t);
                             // Patterns are NOT scanned: the only expressions they can hold are
                             // literals (LiteralPat) and literal range bounds, neither of which
                             // can carry a return / `?` / lambda. Their node cost is ignored too.
                             for (const MatchArm& a : m.arms) {
                                 if (a.guard) scan_inline_body(*a.guard, t);
                                 if (a.body)  scan_inline_body(*a.body, t);
                             }
                             return; }
    case ExprKind::While:  { const auto& w = static_cast<const WhileExpr&>(e);
                             scan_inline_body(*w.cond, t); scan_inline_body(*w.body, t); return; }
    case ExprKind::For:    { const auto& f = static_cast<const ForExpr&>(e);
                             scan_inline_body(*f.iter, t); scan_inline_body(*f.body, t); return; }
    case ExprKind::Loop:   scan_inline_body(*static_cast<const LoopExpr&>(e).body, t); return;
    case ExprKind::Break:  { const auto& b = static_cast<const BreakExpr&>(e);
                             if (b.value) scan_inline_body(*b.value, t); return; }
    case ExprKind::Return: t.has_return = true;
                           { const auto& r = static_cast<const ReturnExpr&>(e);
                             if (r.value) scan_inline_body(*r.value, t); } return;
    case ExprKind::Block:  { const auto& b = static_cast<const BlockExpr&>(e);
                             for (const auto& s : b.stmts) scan_inline_body_stmt(*s, t); return; }
    case ExprKind::Lambda: t.has_lambda = true; return;   // not descended -- the body is lifted
    case ExprKind::StructLit: { const auto& sl = static_cast<const StructLit&>(e);
                             // A SHORTHAND field (`S { x }`) carries a null value -- it resolves the
                             // local named for the field, so there is no sub-expression to descend
                             // into. Count it as the one node the `x` of `S { x: x }` would cost:
                             // the two spellings are exactly as expensive, so letting them differ
                             // would make the size threshold spelling-dependent.
                             for (const FieldInit& f : sl.fields) {
                                 if (f.value) scan_inline_body(*f.value, t);
                                 else         ++t.nodes;
                             }
                             if (sl.base) scan_inline_body(*sl.base, t); return; }
    case ExprKind::Tuple:  { for (const auto& x : static_cast<const TupleExpr&>(e).elems)
                                 scan_inline_body(*x, t); return; }
    case ExprKind::ListLit:{ for (const auto& x : static_cast<const ListLit&>(e).elems)
                                 scan_inline_body(*x, t); return; }
    case ExprKind::MapLit: { for (const auto& kv : static_cast<const MapLit&>(e).entries) {
                                 scan_inline_body(*kv.first, t); scan_inline_body(*kv.second, t); }
                             return; }
    }
}

void Codegen::scan_inline_body_stmt(const Stmt& s, InlineTarget& t) const {
    ++t.nodes;
    switch (s.kind) {
    case StmtKind::Let:    { const auto& l = static_cast<const LetStmt&>(s);
                             if (l.init) scan_inline_body(*l.init, t); return; }
    case StmtKind::Assign: { const auto& a = static_cast<const AssignStmt&>(s);
                             if (a.target) scan_inline_body(*a.target, t);
                             if (a.value)  scan_inline_body(*a.value, t); return; }
    case StmtKind::Expr:   scan_inline_body(*static_cast<const ExprStmt&>(s).expr, t); return;
    }
}
#pragma warning(pop)

void Codegen::build_inline_targets(const std::vector<const FnItem*>& fns) {
    auto add = [&](const std::string& name, std::vector<std::string> params,
                   const BlockExpr* body, bool ret_double) {
        auto it = inline_targets_.find(name);
        if (it != inline_targets_.end()) {
            // Two bodies staged under ONE name. $default$/$impl$/$inherent$ are unique by
            // construction, but $blanket$Trait$method omits the HEAD, so two blanket impls of the
            // same trait collide here. Expanding either would be a coin flip, so expand neither.
            it->second.duplicate = true;
            return;
        }
        InlineTarget t;
        t.body       = body;
        t.params     = std::move(params);
        t.ret_double = ret_double;
        if (body) {
            scan_inline_body(*body, t);
            // The body's TAIL expression when it is a plain struct literal (SROA S2): such an
            // expansion can write a dissolved destination's registers directly instead of
            // building the callee's result object. `Vec3::add`/`sub`/`scale` are exactly this.
            if (!body->stmts.empty() && body->stmts.back()->kind == StmtKind::Expr) {
                const Expr& tail = *static_cast<const ExprStmt&>(*body->stmts.back()).expr;
                if (tail.kind == ExprKind::StructLit)
                    t.tail_lit = &static_cast<const StructLit&>(tail);
            }
            // Per parameter: is every use in this body a field read (SROA S3)? The SAME escape
            // scan the caller side uses, run over the callee's own body -- so the two can never
            // disagree about what "only read as `p.f`" means.
            t.params_field_only.assign(t.params.size(), 0);
            for (size_t i = 0; i < t.params.size(); ++i) {
                SroaCtx c; c.name = &t.params[i];
                std::unordered_set<std::string> bound;
                sroa_walk_block(*body, c, bound);
                t.params_field_only[i] = c.escaped ? 0 : 1;
            }
        }
        inline_targets_.emplace(name, std::move(t));
    };
    for (const FnItem* fn : fns) {
        std::vector<std::string> pn;
        for (const Param& p : fn->params) pn.push_back(p.name);
        add(fn->name, std::move(pn), static_cast<const BlockExpr*>(fn->body.get()),
            annotation_is_double(fn->ret.get()));
    }
    for (const MethodFn& mf : method_fns_)
        add(mf.synth_name, mf.param_names, mf.body, annotation_is_double(mf.ret));
}

const Codegen::InlineTarget* Codegen::inline_target_for(const std::string& name, size_t nargs) const {
    if (!inlining_ || name.empty()) return nullptr;
    auto it = inline_targets_.find(name);
    if (it == inline_targets_.end()) return nullptr;
    const InlineTarget& t = it->second;
    if (t.duplicate || !t.body)          return nullptr;
    if (t.params.size() != nargs)        return nullptr;   // not the site we think it is
    if (t.has_lambda)                    return nullptr;   // v1: a lifted body needs its own frame
    if (t.nodes > inline_max_nodes_)     return nullptr;
    return &t;
}

// The frame an expansion needs. Mirrors compile_inline step for step -- if the two ever disagree
// the emitter self-check (alloc_temp / alloc_local) throws at the emit site, which is the loud
// failure this pairing exists to guarantee.
Codegen::Need Codegen::measure_inline_site(const InlineTarget& t,
                                           const std::vector<const Expr*>& margs, int base) {
    int temp = 1;          // the result temp, allocated FIRST and live across the whole expansion
    int lp   = base;
    // One LOCAL per parameter, each allocated BEFORE its argument is compiled -- the `let` rule.
    // Hence argument i is measured at base + i + 1: its OWN slot is already taken too, not just
    // the i before it. (Measuring at base + i under-counts by one per argument, which the
    // emitter self-check catches -- loudly, but only once a 3-argument callee is expanded.)
    // SROA (S3): a FIELD-ONLY parameter fed a dissolved argument takes one local per FIELD, not
    // one. The measure reserves that width whenever it COULD apply -- it cannot see scope_, so it
    // cannot know whether the argument really is dissolved -- which is the safe direction.
    int slots = 0;
    for (size_t i = 0; i < margs.size(); ++i) {
        const int psl = sroa_param_slots(t, i, *margs[i]);
        // An `Int` argument at a `Double` parameter is widened by the expansion exactly as a real
        // call widens it; `measure_expr` is the honoring wrapper, so it accounts for the I2D temp.
        const Need a = measure_expr(*margs[i], base + slots + psl);
        temp = imax(temp, 1 + a.temp);
        lp   = imax(lp, a.local_peak);
        slots += psl;
    }
    const int pbase = base + slots;
    lp = imax(lp, pbase);
    ++inline_depth_;       // depth-limited: see inline_max_depth_
    const Need b = measure_coerced(*t.body, pbase, t.ret_double);
    --inline_depth_;
    return { imax(temp, 1 + b.temp), imax(lp, b.local_peak) };
}

int Codegen::try_inline_call(const Expr* site, const std::string& name,
                             const std::vector<const Expr*>& args) {
    // Two independent conditions, both required. The measure's PERMISSION is what sized this
    // frame, so it is never re-derived -- and the DEPTH check is not redundant with it: a call
    // node inside a callee body carries a permission earned while that callee was measured as
    // its own function, and honouring it here would expand into a frame sized for one level.
    if (!inlining_ || !site || inline_depth_ >= inline_max_depth_) return -1;
    if (!inline_sites_.count(site)) return -1;
    const InlineTarget* t = inline_target_for(name, args.size());
    if (!t) return -1;                        // measured permissively; emit declines (safe)
    // SROA (S2): a `let` being dissolved offered its field registers to THIS site. Claiming them
    // here -- after every decline above has already been applied -- is what lets the dissolved
    // path reuse the entire resolution instead of re-deriving it. One-shot.
    if (sroa_dst_ && sroa_dst_site_ == site && sroa_producer_ok(*t, sroa_dst_->size())) {
        const std::vector<int>* dst = sroa_dst_;
        sroa_dst_ = nullptr; sroa_dst_site_ = nullptr; sroa_dst_used_ = true;
        return compile_inline(*t, args, site, dst);
    }
    return compile_inline(*t, args, site);
}

// Is this callee a dissolvable PRODUCER -- can its expansion write `nfields` registers instead of
// returning an object? It must end in a plain struct literal of the right shape and have exactly
// ONE exit: compile_inline allocates a single result register and emit_return_jump writes it, so
// an early `return` or a failing `?` has nowhere to put N values.
bool Codegen::sroa_producer_ok(const InlineTarget& t, size_t nfields) const {
    if (!t.tail_lit || t.has_return || t.has_try || t.has_lambda || t.duplicate) return false;
    if (t.tail_lit->base || t.tail_lit->fields.size() != nfields) return false;
    const StructDef* sd = find_struct(t.tail_lit->name);
    return sd && sd->field_names.size() == nfields;
}

int Codegen::compile_inline(const InlineTarget& t, const std::vector<const Expr*>& margs,
                            const Expr* site, const std::vector<int>* dst) {
    // (1) The result temp FIRST: the body writes into it and it must outlive the callee's locals,
    //     which compile_block_stmts reclaims before returning. A DISSOLVED expansion needs none --
    //     its destinations are the caller's own locals, allocated before this call and therefore
    //     already outliving everything below.
    const int result      = dst ? (*dst)[0] : alloc_temp();
    const int locals_mark = n_locals_;

    // (2) Each argument into a fresh LOCAL, compiled in the CALLER's still-live scope. A COPY,
    //     never an alias: assignment to a plain Ident target writes local_reg(name) IN PLACE, so
    //     an alias would let the callee body write the caller's variable. `compile_into` honors
    //     `Expr::widen_double`, so an `Int` argument at a `Double` parameter is widened here exactly
    //     as a real call widens it -- the expansion has to be indistinguishable from the call it
    //     replaces, and earlier NEITHER side coerced, which is how `fn dv(a: Double,
    //     b: Double)` called as `dv(1, 2)` came to divide two raw Ints.
    std::vector<int>              pregs;
    std::vector<std::vector<int>> pfields(margs.size());   // per param: dissolved regs, else empty
    pregs.reserve(margs.size());
    for (size_t i = 0; i < margs.size(); ++i) {
        const Expr* a = margs[i];
        // SROA (S3): the argument IS a dissolved binding and the parameter is only ever read as
        // `p.f`, so give the parameter one register per field. One MOV per field replaces one MOV
        // plus a GET_PROP at every use. (How much that is WORTH is unsettled -- see the note in
        // compile_field; the measured wall effect on demo/raytracer is zero.)
        // Still a COPY, never an alias: that rule is what makes a mis-analysis produce a stale
        // read rather than a corrupted caller.
        const Local* src = nullptr;
        if (sroa_ && i < t.params_field_only.size() && t.params_field_only[i] &&
            a->kind == ExprKind::Ident && static_cast<const IdentExpr&>(*a).qualifier.empty())
            src = local_slot(static_cast<const IdentExpr&>(*a).name);
        if (src && !src->fields.empty()) {
            for (const int fr : src->fields) {
                const int r = alloc_local();
                as_.R6(OpCode::MOV, static_cast<uint8_t>(r), static_cast<uint8_t>(fr), 0);
                pfields[i].push_back(r);
            }
            pregs.push_back(pfields[i][0]);
        } else {
            const int r = alloc_local();
            compile_into(*a, r);
            pregs.push_back(r);
        }
    }

    // (3) Neutralise everything ambient the body could reach. scope_ is SAVED AND CLEARED, not
    //     pushed onto: local_reg is a flat reverse scan that wins ahead of every global lookup, so
    //     a caller local named like a fn the callee body calls would hijack it. loop_stack_ must
    //     go for the same reason (a `break` in the body would bind the caller's loop), and the
    //     lambda-capture map because a body ident matching an enclosing capture name would lower
    //     to LOAD_CAPTURE against the wrong closure.
    std::vector<Local>                  saved_scope;  saved_scope.swap(scope_);
    std::vector<LoopCtx>                saved_loops;  saved_loops.swap(loop_stack_);
    std::unordered_map<std::string,int> saved_caps;   saved_caps.swap(current_lambda_captures_);
    const bool        saved_is_lambda = current_fn_is_lambda_;
    const std::string saved_fn_name   = current_fn_name_;
    const int         saved_nparams   = current_fn_nparams_;
    const bool        saved_ret_dbl   = current_fn_ret_double_;
    const bool        saved_pin       = pos_pinned_;
    current_fn_is_lambda_  = false;
    current_fn_name_.clear();          // nothing here is tail-callable; see the value-position note
    current_fn_nparams_    = -1;
    current_fn_ret_double_ = t.ret_double;
    if (site && site->line) as_.set_pos(site->line, site->col);   // D4: pin, then gate
    pos_pinned_ = true;
    ++inline_depth_;
    // The exit label an early `return` / a failing `?` inside the body jumps to. Pushed AFTER the
    // arguments were compiled: a `return` in an argument expression belongs to whatever encloses
    // this call, not to the callee.
    const std::string exit_lbl = fresh_label("inlx_");
    inline_stack_.push_back(InlineFrame{ result, exit_lbl });
    for (size_t i = 0; i < t.params.size(); ++i)
        scope_.push_back(Local{ t.params[i], pregs[i], false, pfields[i] });

    // (4) The body in VALUE position. compile_tail is deliberately NOT used: it would emit a RET
    //     or a TCO_CALL and leave the CALLER. `ret_double` reproduces the return-boundary coercion
    //     compile_function would have applied.
    if (dst) {
        // DISSOLVED: the body's statements run normally and its tail struct literal is written
        // field-by-field into the caller's registers. Those are the caller's locals, below this
        // expansion's own, so the reclaim at the end cannot touch them and no rescue MOV is needed
        // -- which is exactly why the ordinary path's MOV-before-reclaim has no analogue here.
        compile_block_dissolved(*t.body, *dst);
    } else {
        const int rv = compile_coerced(*t.body, t.ret_double);
        // The MOV happens BEFORE the locals are reclaimed: a body like `{ x }` hands back a
        // parameter's local register, which the reclaim below would otherwise hand to the next
        // allocation.
        if (rv != result) as_.R6(OpCode::MOV, static_cast<uint8_t>(result), static_cast<uint8_t>(rv), 0);
        free_if_temp(rv);
    }
    // Both paths converge here with the value already in `result`: the fall-through above, and
    // every early return that jumped. The label goes AFTER the fall-through MOV for that reason.
    inline_stack_.pop_back();
    as_.label(exit_lbl);

    --inline_depth_;
    pos_pinned_            = saved_pin;
    current_fn_ret_double_ = saved_ret_dbl;
    current_fn_nparams_    = saved_nparams;
    current_fn_name_       = saved_fn_name;
    current_fn_is_lambda_  = saved_is_lambda;
    current_lambda_captures_.swap(saved_caps);
    loop_stack_.swap(saved_loops);
    scope_.swap(saved_scope);
    n_locals_ = locals_mark;
    ++inline_seen_emit_;
    return result;
}

std::string Codegen::direct_call_target(const Expr* callee, const std::vector<const Expr*>& args,
                                        std::vector<const Expr*>& out_args) const {
    out_args = args;
    if (!callee) return {};
    if (callee->kind == ExprKind::Ident) {
        const auto& id = static_cast<const IdentExpr&>(*callee);
        if (!id.qualifier.empty()) {
            if (id.inherent) return inherent_fn_name(id);                  // `Type::method(x)`
            const MRef r = resolve_method(id.qualifier, id.name);           // `Trait::method(x)`
            return args.empty() ? std::string()
                                : devirt_target(r.trait, r.method, concrete_head_of(args[0]->ty));
        }
        // local_reg / is_capture_name are NOT consulted: scope_ is empty during the measure, so
        // they would answer differently in the two passes. Emit checks them and declines.
        if (user_fn_ref(id)) return id.name;                                // top-level fn
        if (is_trait_method_name(id.name) && !is_ctor_name(id.name) && !args.empty()) {
            const MRef r = resolve_method("", id.name);                     // devirtualized trait call
            return devirt_target(r.trait, r.method, concrete_head_of(args[0]->ty));
        }
        return {};
    }
    if (callee->kind == ExprKind::Field) {                                  // `recv.method(a)`
        const auto& fld = static_cast<const FieldExpr&>(*callee);
        if (fld.tuple_index || cg_field_exists(fld.obj->ty, fld.name)) return {};
        out_args.insert(out_args.begin(), fld.obj.get());   // emit prepends the receiver as arg 0
        const std::string head = (fld.obj->ty && fld.obj->ty->kind == TyKind::Named)
                                 ? fld.obj->ty->name : std::string();
        if (is_inherent_method(head, fld.name)) return "$inherent$" + head + "$" + fld.name;
        if (is_trait_method_name(fld.name)) {
            const MRef r = resolve_method("", fld.name);
            return devirt_target(r.trait, r.method, concrete_head_of(fld.obj->ty));
        }
    }
    return {};
}

Module Codegen::generate(const Program& prog) {
    // Register every struct + enum variant as a fixed-shape type first, so a constructor
    // name classifies and a field slot resolves during the measure pass.
    register_program_types(prog);
    // Collect traits/impls: assign method ids, stage the synthetic $impl/$default method fns,
    // record enum->variant dense ids + method owners (all consumed by the measure + call sites).
    link_traits(prog);

    std::vector<const Stmt*>   stmts;
    std::vector<const FnItem*> fns;
    for (const auto& item : prog.items) {
        switch (item->kind) {
        case ItemKind::Stmt:
            stmts.push_back(static_cast<const StmtItem&>(*item).stmt.get());
            break;
        case ItemKind::Fn:
            fns.push_back(static_cast<const FnItem*>(item.get()));
            break;
        case ItemKind::Const: {
            // A scalar const inlines its literal at each reference (is_const_name -> compile_expr).
            // An ARRAY const (`const NAME: Array[T] = [...]`) instead becomes one pre-built entry in
            // the const-array pool -> LOAD_CONST_ARRAY at each use (a shared, immutable instance).
            const auto& cc = static_cast<const ConstItem&>(*item);
            const_defs_[cc.name] = cc.value.get();
            if (cc.value->kind == ExprKind::ListLit)
                const_array_index_[cc.name] =
                    as_.add_const_array(const_array_values(static_cast<const ListLit&>(*cc.value)));
            break;
        }
        case ItemKind::Struct:
        case ItemKind::Enum:
        case ItemKind::Trait:
        case ItemKind::Impl:
            break;   // registered by register_program_types / link_traits
        case ItemKind::Import:
        case ItemKind::Use:
            break;   // module items emit no code (resolved by the loader/resolver, Slice 2)
        }
    }

    // Register fn names + arity up front, so calls resolve order-independently and a bare
    // fn name classifies as a first-class value / a direct-call target during the measure.
    for (const FnItem* fn : fns) {
        fn_names_.insert(fn->name);
        fn_arity_[fn->name] = static_cast<int>(fn->params.size());
    }
    // The expandable-callee table, built from the SAME two sources and BEFORE the measure --
    // the measure records inline sites, so it must already be able to look one up. link_traits
    // (above) has filled method_fns_, so both body sources are complete here.
    if (inlining_) build_inline_targets(fns);
    // Lift every lambda to a synthetic top-level fn (`$lambdaN`) + analyze its by-value captures.
    // Runs AFTER the fn/ctor/trait-method names are known (so a global reference in a lambda body
    // is not mistaken for a capture) and BEFORE the measure (so frame sizes account for the capture
    // window + the body's LOAD_CAPTURE temps).
    collect_lambdas_program(prog);
    analyze_captures();
    // The SROA pre-pass, for the same reason and at the same point as build_inline_targets: the
    // measure must be able to look the decision up, and it cannot be taken DURING the measure --
    // let_binding_count places the initializer's base before anything about that `let` has been
    // measured (decision D1). Needs structs_ (registered first thing in generate) and
    // method_fns_ (filled by link_traits above).
    if (sroa_) collect_sroa_lets(prog, stmts);
    // Measure each function's frame, then declare it (declare_fn assigns the fn-table id for
    // LOAD_FN / CALL_INDIRECT AND registers the frame size for the direct-CALL patch path).
    // The synthetic trait method fns (impl bodies + trait defaults) are ordinary fns with `self`
    // as param 0 -- measured, declared and compiled exactly like a user fn.
    auto measure_fn = [&](const std::string& nm, int nparams, const BlockExpr& body) {
        Plan pf = plan_function(nm, nparams, body);
        if (pf.frame_size > 63)
            throw CodegenError("function '" + nm + "' frame size exceeds the 63-register r6 limit");
        fn_plans_[nm] = pf;
    };
    for (const FnItem* fn : fns)
        measure_fn(fn->name, static_cast<int>(fn->params.size()),
                   static_cast<const BlockExpr&>(*fn->body));
    for (const MethodFn& mf : method_fns_)
        measure_fn(mf.synth_name, static_cast<int>(mf.param_names.size()), *mf.body);
    for (const LiftedLambda& ll : lifted_) {              // current_lambda_captures_ so a captured
        set_current_lambda_captures(ll.lam);              // body ident sizes as a LOAD_CAPTURE temp
        measure_fn(ll.name, static_cast<int>(ll.lam->params.size()),
                   static_cast<const BlockExpr&>(*ll.lam->body));
        current_lambda_captures_.clear();
    }

    for (const FnItem* fn : fns)
        as_.declare_fn(fn->name, static_cast<uint8_t>(fn_plans_.at(fn->name).frame_size),
                       static_cast<uint8_t>(fn->params.size()),
                       0, FnInfo::NO_SELF, fn->module_prefix);
    for (const MethodFn& mf : method_fns_)
        as_.declare_fn(mf.synth_name, static_cast<uint8_t>(fn_plans_.at(mf.synth_name).frame_size),
                       static_cast<uint8_t>(mf.param_names.size()),
                       0, FnInfo::NO_SELF, mf.module);
    for (const LiftedLambda& ll : lifted_)                // ncaptures recorded for MAKE_CLOSURE / the
        as_.declare_fn(ll.name, static_cast<uint8_t>(fn_plans_.at(ll.name).frame_size),  // indirect call
                       static_cast<uint8_t>(ll.lam->params.size()),
                       static_cast<uint8_t>(lambda_captures_[ll.lam].size()),
                       FnInfo::NO_SELF, ll.module);
    // Back-fill each method's resolved fn id into the trait/impl model (declare_fn is now done).
    for (const MethodFn& mf : method_fns_) {
        const uint16_t id = as_.func_id(mf.synth_name);
        if (mf.is_blanket)      blankets_[mf.blanket_idx].method_fn_id[mf.method] = id;
        else if (mf.is_default) traits_.at(mf.trait).default_fn_id[mf.method] = id;
        else if (mf.trait.empty()) continue;   // S2 inherent method: called directly by name, no trait row
        else                    impls_[impl_index_.at(mf.trait + '\0' + mf.head)].own_fn_id[mf.method] = id;
    }

    // Emit the top-level "main" region first (entry at index 0, ends in HALT), then each fn, then
    // the synthetic method fns (all after main so index 0 stays the entry point).
    const Plan top = plan_top_level(stmts);
    as_.set_top_frame_size(static_cast<uint8_t>(top.frame_size));  // validate the top-level frame too
    begin_frame(top);
    compile_top_level(stmts);
    for (const FnItem* fn : fns) {
        std::vector<std::string> pn;
        for (const Param& p : fn->params) pn.push_back(p.name);
        compile_function(fn->name, pn, static_cast<const BlockExpr&>(*fn->body),
                         annotation_is_double(fn->ret.get()));
    }
    for (const MethodFn& mf : method_fns_)
        compile_function(mf.synth_name, mf.param_names, *mf.body, annotation_is_double(mf.ret));
    // The lifted lambdas last (main stays entry 0). current_fn_is_lambda_ + current_lambda_captures_
    // let the body resolve captured idents to LOAD_CAPTURE. ret-Double is read off the checker's
    // inferred lambda type (an unannotated lambda has no syntactic return type).
    for (const LiftedLambda& ll : lifted_) {
        current_fn_is_lambda_ = true;
        set_current_lambda_captures(ll.lam);
        std::vector<std::string> pn;
        for (const Param& p : ll.lam->params) pn.push_back(p.name);
        const bool ret_dbl = ll.lam->ty && ll.lam->ty->ret && is_double_ty(ll.lam->ty->ret);
        compile_function(ll.name, pn, static_cast<const BlockExpr&>(*ll.lam->body), ret_dbl);
        current_lambda_captures_.clear();
        current_fn_is_lambda_ = false;
    }

    // The inline drift report (SKARN_INLINE_REPORT=1). Emit may take FEWER sites than the measure
    // recorded -- a local shadowing a fn name, or a tail-position site, is invisible to the measure
    // by design -- but it must never take MORE, since it only ever expands what the frame was sized
    // for. So `emit > measure` is a hard internal error, not a report line.
    if (inlining_) {
        // No count comparison here, deliberately. `inline_seen_emit_` counts expansion EVENTS while
        // inline_sites_ holds DISTINCT nodes, and above depth 1 one body node is legitimately
        // expanded once per enclosing expansion -- so events exceed nodes for a benign reason. The
        // invariant that matters is structural and already enforced where it belongs:
        // try_inline_call expands only a site present in inline_sites_, so emit can never take one
        // the measure did not size. (At depth 1 the counts happened to relate, which made a bogus
        // check look like a working one.)
        if (inline_report_) {
            // Per-target detail: why a callee is or is not expandable. Sorted so two runs are
            // diffable (the map is unordered).
            std::vector<const std::pair<const std::string, InlineTarget>*> rows;
            for (const auto& kv : inline_targets_) rows.push_back(&kv);
            std::sort(rows.begin(), rows.end(), [](auto* x, auto* y) { return x->first < y->first; });
            for (const auto* kv : rows) {
                const InlineTarget& t = kv->second;
                std::cerr << "[inline]   " << kv->first << "  nparams=" << t.params.size()
                          << " nodes=" << t.nodes
                          << (t.body ? "" : " NO-BODY") << (t.duplicate ? " DUP" : "")
                          << (t.has_lambda ? " LAMBDA" : "") << (t.has_return ? " RETURN" : "")
                          << (t.has_try ? " TRY" : "")
                          << (t.nodes > inline_max_nodes_ ? " TOO-BIG" : "") << "\n";
            }
        }
    }

    Module m;
    m.bytecode        = as_.assemble();
    // The summary goes here, AFTER assemble(), so `code-words` is the real emitted size. Those are
    // the two quantities the S4 tuning decision rests on: how much CODE the expansions cost, and
    // whether the 63-register file (rather than the size threshold) is what limits them.
    if (inlining_ && inline_report_)
        std::cerr << "[inline] max-nodes=" << inline_max_nodes_
                  << " max-depth="       << inline_max_depth_
                  << " targets="         << inline_targets_.size()
                  << " permitted-sites=" << inline_sites_.size()
                  << " expansions="      << inline_seen_emit_
                  << " code-words="      << m.bytecode.size()
                  << " budget-retries="  << inline_retries_ << "\n";
    // The SROA counterpart. `dissolutions` can EXCEED `permitted-bindings` and that is correct,
    // not drift: a `let` inside an inlined callee body is emitted once per expansion, while the
    // permission is counted once per `let` node. (The decision is still site-independent -- the
    // escape scan runs over the callee's own block, so every site gets the same answer.)
    if (sroa_ && sroa_report_)
        std::cerr << "[sroa] max-fields="   << sroa_max_fields_
                  << " permitted-bindings=" << sroa_permitted_
                  << " dissolutions="       << sroa_seen_emit_
                  << " code-words="         << m.bytecode.size()
                  << " budget-retries="     << sroa_retries_ << "\n";
    m.constants       = as_.constant_pool();
    m.const_arrays    = as_.const_arrays();
    m.struct_types    = as_.struct_types();
    m.string_literals = as_.string_literals();
    m.function_table  = as_.function_table();
    m.line_table      = as_.line_table();
    m.column_table    = as_.column_table();
    m.function_names  = as_.function_names();
    m.function_modules = as_.function_modules();
    m.top_frame_size  = static_cast<uint8_t>(top.frame_size);
    build_trait_table(m);   // fill the dispatch grid now that the struct count is final
    return m;
}

} // namespace svc
