#pragma once

#include <cassert>
#include <charconv>
#include <cstring>
#include <string>
#include <string_view>
#include "../Heap.h"
#include "../Value.h"
#include "../Context.h"
#include "../StructType.h"   // TO_STRING's struct dump reads name / field_names

// =============================================================================
// op_string.h -- runtime helpers for string values (KIND_STRING heap objects).
//
// This header holds only `inline` helpers, no opcode handlers of its own: the ADD,
// EQ/NE/LT/LE and TO_STRING cases of run_switch_loop (Interpreter.h) call them for the
// shared content-compare + concat logic. (Those cases used to be the separate handlers
// op_add_num / op_compare of the removed tail-call dispatcher, which is why this header
// lives under opcodes/ without defining any.) Strings are compared by CONTENT (not pointer
// identity), which is what makes `==` / `<` correct for concat results -- those are
// NOT interned, so pointer identity would be wrong.
// =============================================================================

// These three are SKARN_FORCEINLINE and use manual byte loops rather than a memcmp
// CALL. The reason that MADE that mandatory is retired: under the old threaded tail-call
// dispatcher the op_eq/op_ne/op_lt/op_le handlers had to stay CALL-FREE leaves so their
// DISPATCH remained a tail-jmp, or a deep TCO loop grew the native stack. Those handlers,
// that dispatcher are all gone; in a
// while{switch} a call inside a case costs throughput, not stack. Inlining helpers this
// small and this hot is still the right default -- but it is now a codegen preference,
// not a correctness requirement.

// True iff `v` is a heap string (KIND_STRING). Only derefs on the pointer path.
[[nodiscard]] SKARN_FORCEINLINE inline bool is_string(Value v) noexcept {
    return v.isPtr() && GcObject::from_slots(v.asPtr())->kind == GcObject::KIND_STRING;
}

// Three-way lexicographic compare of two heap strings by CONTENT: unsigned byte
// order, ties broken by length (a prefix is < the longer string). The internal
// "STR_CMP" -- a shared helper, deliberately not exposed as an opcode (no surface
// operator needs a three-way result yet). Both args must be KIND_STRING.
[[nodiscard]] SKARN_FORCEINLINE inline int str_cmp(Value a, Value b) noexcept {
    const GcObject* oa = GcObject::from_slots(a.asPtr());
    const GcObject* ob = GcObject::from_slots(b.asPtr());
    assert(oa->kind == GcObject::KIND_STRING && ob->kind == GcObject::KIND_STRING);
    const uint32_t la = oa->string_length();
    const uint32_t lb = ob->string_length();
    const uint32_t n  = la < lb ? la : lb;
    const auto* pa = reinterpret_cast<const unsigned char*>(oa->bytes());
    const auto* pb = reinterpret_cast<const unsigned char*>(ob->bytes());
    for (uint32_t i = 0; i < n; ++i)
        if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
    if (la != lb) return la < lb ? -1 : 1;
    return 0;
}

// Content equality of two heap strings (length short-circuit then byte compare).
[[nodiscard]] SKARN_FORCEINLINE inline bool str_eq(Value a, Value b) noexcept {
    const GcObject* oa = GcObject::from_slots(a.asPtr());
    const GcObject* ob = GcObject::from_slots(b.asPtr());
    assert(oa->kind == GcObject::KIND_STRING && ob->kind == GcObject::KIND_STRING);
    const uint32_t la = oa->string_length();
    if (la != ob->string_length()) return false;
    const auto* pa = reinterpret_cast<const unsigned char*>(oa->bytes());
    const auto* pb = reinterpret_cast<const unsigned char*>(ob->bytes());
    for (uint32_t i = 0; i < la; ++i)
        if (pa[i] != pb[i]) return false;
    return true;
}

// Custom SEH code for heap exhaustion. RAISED (not C++ thrown) by all 15 allocating
// helpers across op_string.h / op_vec.h / op_map.h / op_bytes.h; run_switch's __except
// translates it to a std::runtime_error, mirroring the div-by-zero SEH pattern.
//
// THE ORIGINAL REASON IS RETIRED -- the mechanism outlived it. The raise existed so
// string_concat could stay `noexcept`, which kept the old threaded tail-call
// dispatcher's ADD handler free of C++ EH state and therefore tail-call-optimized in
// Release; a throwing call there defeated the final DISPATCH tail-jmp and an ADD-heavy
// TCO loop then grew the native stack. That dispatcher, its op_add_num handler and
// run_bytecode have all been deleted. run_switch_loop is a while{switch} with no
// tail-jmp to defeat, and it ALREADY calls throwing helpers for this very condition
// (switch_alloc_array / switch_anew / switch_new_struct / switch_make_closure /
// switch_map_collect all throw std::runtime_error, as does StringInterner). So one
// condition having two mechanisms is now historical accident, not design.
//
// Converting these sites to plain throws is possible but was DELIBERATELY NOT done.
// It buys no throughput -- the __try frame has to stay for the
// access-violation classification either way -- and the path is not verifiable as
// things stand: Heap::MAX_SEMI is a static constexpr 1 GiB, not per-instance, so heap
// exhaustion cannot be provoked in a test and vm_tests covers none of these 15 sites.
// Making that cap per-instance is the prerequisite; the payoff worth having afterwards
// is routing the fault through raise_located, so an OOM gets a line and a stacktrace
// like every other serious fault.
inline constexpr unsigned long VM_EXC_HEAP_EXHAUSTED = 0xE0564D00u; // 'VM'\0, customer bit set

// Concatenate two heap strings into a fresh (NON-interned) KIND_STRING; returns the
// tagged pointer to the new string. GC-robust: both operand contents are copied into
// a host std::string FIRST, so the subsequent alloc_string_gc may relocate a/b (the
// host copy owns the bytes independently of the moving collector). Both args must be
// KIND_STRING. `noexcept` (see VM_EXC_HEAP_EXHAUSTED): on the near-impossible 1 GiB
// OOM it raises an SEH fault instead of throwing.
//
// SKARN_NOINLINE WAS load-bearing for a reason that no longer exists: inlining this
// (with its std::string local + cleanup) into the old tail-call dispatcher's op_add_num
// gave that handler unwind code, which defeated the DISPATCH tail-jmp and grew the native
// stack on every ADD. There is no tail-jmp under the while{switch}, so the attribute is
// now a plain code-quality choice -- keeping a std::string and its cleanup out of the one
// enormous dispatch function. That benefit is UNMEASURED; treat it as a sane default
// rather than a fact, and measure before removing it.
[[nodiscard]] SKARN_NOINLINE inline Value string_concat(Value a, Value b, Context* ctx) {
    const GcObject* oa = GcObject::from_slots(a.asPtr());
    const GcObject* ob = GcObject::from_slots(b.asPtr());
    assert(oa->kind == GcObject::KIND_STRING && ob->kind == GcObject::KIND_STRING);

    std::string joined;
    joined.reserve(static_cast<size_t>(oa->string_length()) + ob->string_length());
    joined.append(oa->bytes(), oa->string_length());
    joined.append(ob->bytes(), ob->string_length());

    // From here on a/b may be moved by a collection triggered inside the allocator;
    // `joined` is unaffected, so we never touch a/b again.
    GcObject* obj = ctx->vm->heap->alloc_string_gc(joined, ctx);
    if (!obj) {
        RaiseException(VM_EXC_HEAP_EXHAUSTED, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        return Value{}; // unreachable (noncontinuable) -- keeps the compiler happy
    }
    return Value::fromPtr(obj->bytes());
}

// Format a numeric Value (Int or Double) to its textual form, host-side. Ints go
// through to_chars as a signed 48-bit decimal; doubles use to_chars' shortest
// round-trip form, then get a trailing ".0" appended when the result reads as a bare
// integer (only an optional '-' and digits) so a Double always prints AS a Double
// (2.0 -> "2.0", not "2"). Values with a '.', exponent, or letters -- "0.5", "1e+16",
// and NaN/inf's "nan"/"inf"/"-inf" -- are left exactly as to_chars produced them.
// Precondition: v.isInt() || v.isDouble().
[[nodiscard]] inline std::string format_number(Value v) {
    char buf[40];
    if (v.isInt()) {
        const std::to_chars_result res = std::to_chars(buf, buf + sizeof(buf), v.asSigned48());
        return std::string(buf, res.ptr);
    }
    // Use snprintf with shortest-round-trip: try increasing precision until strtod
    // reproduces the same bits, matching the semantics of to_chars(chars_format::general).
    double d = v.numAsDouble();
    for (int prec = 1; prec <= 17; ++prec) {
        int n = std::snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (n > 0 && n < static_cast<int>(sizeof(buf))) {
            char* ep = nullptr;
            if (std::strtod(buf, &ep) == d && ep == buf + n) {
                std::string s(buf, static_cast<size_t>(n));
                bool looks_integer = true;
                for (char c : s) { if (!(c == '-' || (c >= '0' && c <= '9'))) { looks_integer = false; break; } }
                if (looks_integer) s += ".0";
                return s;
            }
        }
    }
    int n = std::snprintf(buf, sizeof(buf), "%.17g", d);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : "nan";
}

// Concatenate two operands where AT LEAST one is a heap string, coercing a numeric
// (Int/Double) operand to its textual form via format_number -- the `number + string`
// / `string + number` `+` coercion path for op ADD. Same GC discipline as
// string_concat: BOTH sides are materialized into a host std::string BEFORE
// alloc_string_gc, so a collection that relocates a/b during the allocation cannot
// dangle. A non-string, non-number operand (struct/bool/nil/atom) is a type error
// (Debug assert only -- the compiler is expected to emit well-typed ADDs, matching
// the other type-check-free fast paths). `noexcept` / SKARN_NOINLINE mirror
// string_concat (raises the heap-exhausted SEH code rather than throwing).
[[nodiscard]] SKARN_NOINLINE inline Value string_add(Value a, Value b, Context* ctx) {
    const auto to_host = [](Value v) -> std::string {
        if (is_string(v)) {
            const GcObject* o = GcObject::from_slots(v.asPtr());
            return std::string(o->bytes(), o->string_length());
        }
        assert((v.isInt() || v.isDouble()) &&
            "ADD string coercion: operand is neither string nor number");
        return format_number(v);
    };
    std::string joined = to_host(a);
    joined += to_host(b);

    // From here a/b may be moved by a collection inside the allocator; `joined` owns
    // the bytes independently, so we never touch a/b again.
    GcObject* obj = ctx->vm->heap->alloc_string_gc(joined, ctx);
    if (!obj) {
        RaiseException(VM_EXC_HEAP_EXHAUSTED, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        return Value{}; // unreachable (noncontinuable)
    }
    return Value::fromPtr(obj->bytes());
}

// Max struct nesting the default field dump renders before truncating with "{ ... }".
// It doubles as the CYCLE GUARD: structs are mutable (SET_PROP), so a self-referential
// graph (a.next = a) is buildable; this cap stops the otherwise-infinite host-side
// recursion, which has no SEH guard of its own and would overflow the native stack.
inline constexpr int TO_STRING_MAX_DEPTH = 4;

// Spine-length cap for the cons-list dump. A cons list renders its elements at ONE
// depth level (like array width), so the depth cap does NOT bound a long-but-flat
// list -- only nested elements consume depth. This cap therefore exists purely to
// stop a self-referential (cyclic) list, which is buildable by hand via SET_PROP on
// a `$List` tail; well-formed surface lists never approach it.
inline constexpr int TO_STRING_MAX_LIST = 100;

// Forward decls: append_field_value (a field's / element's text) and the per-kind body
// dumps are mutually recursive -- a field / element may itself be any heap object.
// dump_object picks the KIND_OBJECT renderer by the type's DumpStyle (struct / tuple /
// list); append_tuple_dump and append_list_dump are its tuple / cons-list variants.
inline void append_struct_dump(Value v, Context* ctx, std::string& out, int depth) noexcept;
inline void append_tuple_dump (Value v, Context* ctx, std::string& out, int depth) noexcept;
inline void append_named_tuple_dump(Value v, Context* ctx, std::string& out, int depth) noexcept;
inline void append_list_dump  (Value v, Context* ctx, std::string& out, int depth) noexcept;
inline void append_array_dump (Value v, Context* ctx, std::string& out, int depth) noexcept;
inline void append_vec_dump   (Value v, Context* ctx, std::string& out, int depth) noexcept;
inline void append_bytes_dump (Value v, Context* ctx, std::string& out, int depth) noexcept;
inline void append_map_dump   (Value v, Context* ctx, std::string& out, int depth) noexcept;
inline void dump_object       (Value v, Context* ctx, std::string& out, int depth) noexcept;

// Append the display text of a struct FIELD / array ELEMENT value into `out`.
// Primitives inline; nested structs and arrays recurse (depth-limited); strings are
// QUOTED (Rust-Debug style -- note this differs from a TOP-LEVEL toString(string),
// which is identity/unquoted); closures render as a placeholder. Purely host-side,
// NO allocation.
inline void append_field_value(Value v, Context* ctx, std::string& out, int depth) noexcept {
    if (v.isInt() || v.isDouble()) { out += format_number(v);                 return; }
    if (v.isBool())                { out += v.asBool() ? "true" : "false";    return; }
    if (v.isNil())                 { out += "nil";                            return; }
    if (v.isUndefined())           { out += "undefined";                      return; } // omitted field
    if (v.isAtom()) {
        const uint32_t id = v.asAtomId();
        if (ctx->vm->atom_pool && id < ctx->vm->atom_pool_size) {
            const GcObject* o = GcObject::from_slots(ctx->vm->atom_pool[id].asPtr());
            out.append(o->bytes(), o->string_length());             // pre-interned ":name"
        } else {
            out += ":?";                                            // no atom pool wired
        }
        return;
    }
    if (v.isPtr()) {
        const GcObject* o = GcObject::from_slots(v.asPtr());
        switch (o->kind) {
        case GcObject::KIND_STRING:                                 // Rust-Debug: quote it
            out += '"';
            out.append(o->bytes(), o->string_length());
            out += '"';
            return;
        case GcObject::KIND_OBJECT:  dump_object       (v, ctx, out, depth); return;
        case GcObject::KIND_ARRAY:   append_array_dump (v, ctx, out, depth); return;
        case GcObject::KIND_VEC:     append_vec_dump   (v, ctx, out, depth); return;
        case GcObject::KIND_BYTES:   append_bytes_dump (v, ctx, out, depth); return;
        case GcObject::KIND_MAP:     append_map_dump   (v, ctx, out, depth); return;
        case GcObject::KIND_CLOSURE: out += "<fn>";                          return;
        default:                     out += "<?>";                           return;
        }
    }
    out += "<?>";
}

// Render a KIND_OBJECT by its type's DumpStyle: a compiler tuple `$TupleN` prints as
// `(e0, e1)`, a cons-list `$List` cell as `[e0, e1, ...]`, everything else as the
// default `Name { ... }` field dump. The style is host-side metadata in the struct-type
// registry (StructType::dump_style), so this reads the same channel as name/field_names
// and needs no object-layout change. An out-of-range type id degrades to the field dump.
inline void dump_object(Value v, Context* ctx, std::string& out, int depth) noexcept {
    const uint16_t tid = GcObject::from_slots(v.asPtr())->object_type_id();
    const DumpStyle st = (tid < ctx->vm->struct_type_count)
                       ? ctx->vm->struct_types[tid].dump_style : DumpStyle::Struct;
    switch (st) {
    case DumpStyle::Tuple:      append_tuple_dump(v, ctx, out, depth); return;
    case DumpStyle::List:       append_list_dump (v, ctx, out, depth); return;
    case DumpStyle::NamedTuple: append_named_tuple_dump(v, ctx, out, depth); return;
    default:                    append_struct_dump(v, ctx, out, depth); return;
    }
}

// Append a tuple-struct dump: `Name(e0, e1)` (positional). A NULLARY tuple-struct
// (0 fields, e.g. `None`) prints as just `Name` (no parens). At TO_STRING_MAX_DEPTH the
// body elides to `Name(...)` (a tuple-struct field could hold a self-reference).
// Elements render via append_field_value (strings QUOTED, nested recurse). NO allocation.
// tid is valid here (dump_object guarded tid < struct_type_count before dispatching).
inline void append_named_tuple_dump(Value v, Context* ctx, std::string& out, int depth) noexcept {
    const GcObject* o = GcObject::from_slots(v.asPtr());
    out += ctx->vm->struct_types[o->object_type_id()].name;
    const uint32_t n = o->slot_count();
    if (n == 0) return;                                                      // nullary -> just the name
    if (depth >= TO_STRING_MAX_DEPTH) { out += "(...)"; return; }            // cycle guard
    out += "(";
    const Value* elems = o->slots();
    for (uint32_t i = 0; i < n; ++i) {
        if (i) out += ", ";
        append_field_value(elems[i], ctx, out, depth + 1);
    }
    out += ")";
}

// Append a tuple dump: `(e0, e1)` (positional, no field names). Empty `$Tuple0` -> `()`;
// a 1-tuple -> `(e0,)` (trailing comma, Rust-style, to disambiguate from a parenthesized
// group). At TO_STRING_MAX_DEPTH the body elides to `(...)` (the same fixed-arity cycle
// guard as a struct: a tuple field could hold a self-reference). Elements render via
// append_field_value (strings QUOTED, nested recurse). NO allocation.
inline void append_tuple_dump(Value v, Context* ctx, std::string& out, int depth) noexcept {
    const GcObject* o = GcObject::from_slots(v.asPtr());
    const uint32_t  n = o->slot_count();
    if (n == 0)                       { out += "()";    return; }            // empty tuple
    if (depth >= TO_STRING_MAX_DEPTH) { out += "(...)"; return; }            // cycle guard
    out += "(";
    const Value* elems = o->slots();
    for (uint32_t i = 0; i < n; ++i) {
        if (i) out += ", ";
        append_field_value(elems[i], ctx, out, depth + 1);
    }
    if (n == 1) out += ",";                                                  // (e0,)
    out += ")";
}

// Append a cons-list dump: `[e0, e1, e2]`. `v` is a `$List { head, tail }` cell (the
// EMPTY list is nil, not a cell -- so toString([]) prints "nil" via the nil branch, a
// documented consequence of the cons encoding). The tail spine is walked ITERATIVELY at
// ONE depth level (like array width), so a long flat list renders in full; only NESTED
// elements consume depth. The per-cell DumpStyle::List re-check keeps a malformed graph
// (a tail pointing at a non-list struct) from being mis-walked, and TO_STRING_MAX_LIST
// bounds a hand-built cyclic tail. Heads render via append_field_value. NO allocation.
inline void append_list_dump(Value v, Context* ctx, std::string& out, int depth) noexcept {
    if (depth >= TO_STRING_MAX_DEPTH) { out += "[ ... ]"; return; }          // cycle guard
    out += "[";
    Value cur = v;
    int   count = 0;
    bool  first = true;
    while (cur.isPtr()) {
        const GcObject* o = GcObject::from_slots(cur.asPtr());
        if (o->kind != GcObject::KIND_OBJECT || o->slot_count() < 2) break;  // not a $List cell
        const uint16_t tid = o->object_type_id();
        if (tid >= ctx->vm->struct_type_count ||
            ctx->vm->struct_types[tid].dump_style != DumpStyle::List) break; // not a list
        if (count >= TO_STRING_MAX_LIST) { out += ", ..."; break; }          // cyclic guard
        if (!first) out += ", ";
        first = false;
        const Value* s = o->slots();
        append_field_value(s[0], ctx, out, depth + 1);                       // head
        cur = s[1];                                                          // tail
        ++count;
    }
    out += "]";
}

// Append the default field dump of a KIND_OBJECT struct: `Name { f0: v0, f1: v1 }`
// (Rust-near). A zero-field struct renders as just `Name`. At TO_STRING_MAX_DEPTH the
// body is elided as `Name { ... }` (the cycle / deep-nesting guard). Type name and
// field names come from the host-side struct-type registry (ctx->vm->struct_types).
// NO allocation -- the whole dump is built into the caller's buffer.
inline void append_struct_dump(Value v, Context* ctx, std::string& out, int depth) noexcept {
    const GcObject* o   = GcObject::from_slots(v.asPtr());
    const uint16_t  tid = o->object_type_id();
    assert(tid < ctx->vm->struct_type_count && "TO_STRING: struct type id out of range");
    const StructType& st = ctx->vm->struct_types[tid];
    out += st.name;
    const uint32_t n = o->slot_count();
    if (n == 0) return;                                             // `Name`, no braces
    if (depth >= TO_STRING_MAX_DEPTH) { out += " { ... }"; return; }
    out += " { ";
    const Value* fields = o->slots();
    for (uint32_t i = 0; i < n; ++i) {
        if (i) out += ", ";
        if (i < st.field_names.size()) out += st.field_names[i];
        else                           out += '?';                 // registry/arity mismatch
        out += ": ";
        append_field_value(fields[i], ctx, out, depth + 1);
    }
    out += " }";
}

// Append the default dump of a KIND_ARRAY: `[e0, e1, e2]` (Rust-near). An empty array
// renders as `[]`; at TO_STRING_MAX_DEPTH the body is elided as `[ ... ]`. This is the
// SAME cycle / deep-nesting guard as structs: arrays are mutable (ARRAY_SET), so a
// self-referential array (a[0] = a) is buildable and would otherwise recurse forever
// host-side (no SEH guard). Elements render via append_field_value (strings QUOTED,
// nested structs/arrays recurse, atoms `:name`, closures `<fn>`, undefined slots
// `undefined`). NO allocation -- the whole dump is built into the caller's buffer.
inline void append_array_dump(Value v, Context* ctx, std::string& out, int depth) noexcept {
    const GcObject* o = GcObject::from_slots(v.asPtr());
    const uint32_t  n = o->slot_count();
    if (n == 0)                       { out += "[]";      return; }          // empty
    if (depth >= TO_STRING_MAX_DEPTH) { out += "[ ... ]"; return; }          // cycle guard
    out += "[";
    const Value* elems = o->slots();
    for (uint32_t i = 0; i < n; ++i) {
        if (i) out += ", ";
        append_field_value(elems[i], ctx, out, depth + 1);
    }
    out += "]";
}

// Append the default dump of a KIND_VEC: `[e0, e1, e2]` -- the SAME surface form as a
// KIND_ARRAY (both are sequences; distinguished by construction, not by print). Only the
// live prefix [0, count) is dumped, read through the header {backing = slot 0, count =
// slot 1} (op_vec.h layout, referenced by index to avoid a circular include). Empty
// vector -> `[]`; at TO_STRING_MAX_DEPTH -> `[ ... ]` (vectors are mutable, so a
// self-reference v[0]=v is buildable -- the same load-bearing cycle guard as arrays).
inline void append_vec_dump(Value v, Context* ctx, std::string& out, int depth) noexcept {
    const GcObject* hdr = GcObject::from_slots(v.asPtr());
    const int64_t   n   = hdr->slots()[1].asSigned48();          // count (VEC_SLOT_COUNT)
    if (n <= 0)                       { out += "[]";      return; }          // empty
    if (depth >= TO_STRING_MAX_DEPTH) { out += "[ ... ]"; return; }          // cycle guard
    out += "[";
    const Value* elems = GcObject::from_slots(hdr->slots()[0].asPtr())->slots(); // backing (slot 0)
    for (int64_t i = 0; i < n; ++i) {
        if (i) out += ", ";
        append_field_value(elems[i], ctx, out, depth + 1);
    }
    out += "]";
}

// Append the default dump of a KIND_BYTES: `#b[DE AD BE EF]` -- uppercase, space-separated
// hex, mirroring the (future) `#b[..]` literal sigil. An empty buffer renders as `#b[]`.
// Only the live prefix [0, count) is shown, read through the header {backing = slot 0,
// count = slot 1} (op_bytes.h layout, referenced by index to avoid a circular include).
// Bytes hold no references, so there is no depth/cycle guard -- `depth` is unused.
inline void append_bytes_dump(Value v, Context* ctx, std::string& out, int /*depth*/) noexcept {
    (void)ctx;
    const GcObject* hdr   = GcObject::from_slots(v.asPtr());
    const int64_t   n     = hdr->slots()[1].asSigned48();               // count (BYTES_SLOT_COUNT)
    const char*     bytes = GcObject::from_slots(hdr->slots()[0].asPtr())->bytes(); // backing (slot 0)
    static const char HEX[] = "0123456789ABCDEF";
    out += "#b[";
    for (int64_t i = 0; i < n; ++i) {
        if (i) out += ' ';
        const unsigned char b = static_cast<unsigned char>(bytes[i]);
        out += HEX[b >> 4];
        out += HEX[b & 0xF];
    }
    out += "]";
}

// Append the default dump of a KIND_MAP: `#{k => v, k2 => v2}` (mirrors the `#{...}`
// literal syntax). An empty map renders as `#{}`; at TO_STRING_MAX_DEPTH the body is
// elided as `#{ ... }` -- the SAME cycle / deep-nesting guard as structs/arrays (a map
// is mutable via MAP_SET, so `m.[k] = m` is buildable and would otherwise recurse
// forever host-side). Keys and values render via append_field_value (strings QUOTED,
// nested structs/arrays/maps recurse, atoms `:name`). Iteration is in BACKING (hash)
// order -- insertion order is NOT preserved (documented as unspecified).
//
// Layout coupling: the KIND_MAP header's slot 0 is the KIND_ARRAY backing of interleaved
// [k0,v0,k1,v1,...] (see op_map.h MAP_SLOT_BACKING -- not referenced here because op_map.h
// includes THIS header, not vice versa). Empty slots are Undefined, deleted are
// Tombstone; both are skipped. NO allocation.
inline void append_map_dump(Value v, Context* ctx, std::string& out, int depth) noexcept {
    if (depth >= TO_STRING_MAX_DEPTH) { out += "#{ ... }"; return; }          // cycle guard
    const GcObject* hdr = GcObject::from_slots(v.asPtr());
    const GcObject* b   = GcObject::from_slots(hdr->slots()[0].asPtr());      // backing (slot 0)
    const uint32_t  n   = b->slot_count();                                    // 2 * capacity
    const Value*    kv  = b->slots();
    out += "#{";
    bool first = true;
    for (uint32_t i = 0; i + 1 < n; i += 2) {
        const Value k = kv[i];
        if (k.isUndefined() || k.isTombstone()) continue;                     // empty / deleted
        if (!first) out += ", ";
        first = false;
        append_field_value(k, ctx, out, depth + 1);
        out += " => ";
        append_field_value(kv[i + 1], ctx, out, depth + 1);
    }
    out += "}";
}

// The generic to-string coercion behind the TO_STRING opcode. Returns a KIND_STRING
// heap Value for any supported operand:
//   - string  -> the operand itself (identity; no allocation)
//   - atom     -> ctx->vm->atom_pool[id]: the atom's name WITH a leading ':' (e.g.
//                 ":ok"), PRE-INTERNED once at execute() setup. No allocation here --
//                 atom names are a bounded, compile-time-known set, so they are
//                 materialized up front and this is a rooted-pool lookup like LOAD_STR.
//   - bool/nil -> ctx->vm->str_true / str_false / str_nil: the three constant names,
//                 likewise pre-interned at setup (bounded set). No allocation.
//   - Int/Double -> format_number + alloc_string_gc: numbers are unbounded, so this is
//                 an allocating branch (a fresh, NON-interned string, like the `+`
//                 coercion). This is why TO_STRING is SYNC'd to a GC safepoint.
//   - struct   -> the default field dump (append_struct_dump), built host-side into one
//                 buffer, then a single alloc_string_gc.
//   - array    -> the default element dump (append_array_dump), `[e0, e1, ...]`, same
//                 single-buffer / single-alloc discipline. Closures still render as a
//                 <fn> placeholder (their own slice can improve on this later).
// append_* never allocate, so the struct graph cannot be moved mid-render; the single
// alloc happens after the buffer is complete. undefined / other -> Debug-assert.
// `noexcept` / SKARN_NOINLINE mirror string_add (raises on heap exhaustion).
[[nodiscard]] SKARN_NOINLINE inline Value to_string_value(Value v, Context* ctx) {
    if (is_string(v)) return v;                                      // identity, no alloc
    if (v.isAtom()) {
        const uint32_t id = v.asAtomId();
        assert(id < ctx->vm->atom_pool_size && "TO_STRING: atom id out of range");
        return ctx->vm->atom_pool[id];                              // pre-interned ":name"
    }
    if (v.isBool()) return v.asBool() ? *ctx->vm->str_true : *ctx->vm->str_false;
    if (v.isNil())  return *ctx->vm->str_nil;

    if (v.isPtr()) {
        // Heap object (KIND_STRING already returned above): a KIND_OBJECT dumps per its
        // DumpStyle (struct field dump / tuple / cons-list), array/map get their element
        // dumps, closure -> <fn> placeholder. Rendered host-side into one buffer, then
        // ONE alloc. No allocation happens during the render, so the object graph
        // stays put -- safe under the moving collector.
        std::string s;
        const GcObject* o = GcObject::from_slots(v.asPtr());
        if (o->kind == GcObject::KIND_OBJECT)       dump_object       (v, ctx, s, 0);
        else if (o->kind == GcObject::KIND_ARRAY)   append_array_dump (v, ctx, s, 0);
        else if (o->kind == GcObject::KIND_VEC)     append_vec_dump   (v, ctx, s, 0);
        else if (o->kind == GcObject::KIND_BYTES)   append_bytes_dump (v, ctx, s, 0);
        else if (o->kind == GcObject::KIND_MAP)     append_map_dump   (v, ctx, s, 0);
        else if (o->kind == GcObject::KIND_CLOSURE) s = "<fn>";
        else                                        s = "<?>";
        GcObject* obj = ctx->vm->heap->alloc_string_gc(s, ctx);
        if (!obj) {
            RaiseException(VM_EXC_HEAP_EXHAUSTED, EXCEPTION_NONCONTINUABLE, 0, nullptr);
            return Value{}; // unreachable (noncontinuable)
        }
        return Value::fromPtr(obj->bytes());
    }

    assert((v.isInt() || v.isDouble()) && "TO_STRING: unsupported operand type");
    const std::string s = format_number(v);                         // the one allocating path
    GcObject* obj = ctx->vm->heap->alloc_string_gc(s, ctx);
    if (!obj) {
        RaiseException(VM_EXC_HEAP_EXHAUSTED, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        return Value{}; // unreachable (noncontinuable)
    }
    return Value::fromPtr(obj->bytes());
}
