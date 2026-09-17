#pragma once

#include <cstdint>
#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>
#include <iostream>
#include "Platform.h"

#include "Fault.h"          // VmFault -- the structured serious-fault exception raise_located throws

#include "Opcodes.h"        // OpCode, Layout, NativeFunc (ISA description; no dispatch macros)
#include "Instruction.h"
#include "Context.h"
#include "VM.h"
#include "VM_Resources.h"   // growable stacks: grow_reg/grow_ret + committed-end accessors
#include "Heap.h"
#include "GlobalEnv.h"
#include "StructType.h"
#include "FunctionTable.h"
#include "NumericOps.h"
#include "opcodes/op_string.h"   // is_string, str_eq, str_cmp, string_concat, VM_EXC_HEAP_EXHAUSTED
#include "opcodes/op_map.h"      // map_new/map_get/map_set, map_key_valid, VM_EXC_INVALID_MAP_KEY
#include "opcodes/op_vec.h"      // vec_new/vec_push/vec_pop, VEC_SLOT_*
#include "opcodes/op_bytes.h"    // bytes_new_cap/bytes_push/bytes_pop/bytes_from_str/bytes_to_string, BYTES_SLOT_*
#include "TypeUniverse.h"        // BuiltinTid, BUILTIN_COUNT, TRAIT_METHOD_NONE (trait dispatch)

// C4714: "function marked as __forceinline not inlined". MSVC will not inline a
// SKARN_FORCEINLINE helper into a function that contains a `__try`, so every
// forceinline helper called from such a function trips C4714 in optimized builds.
//
// HISTORY, and why the code no longer looks like that earlier: the __try used
// to wrap the whole dispatch loop, and this note called the consequence "benign".
// It was not. Disassembling the loop head showed 15 x86 instructions of dispatch
// preamble, three of them storing the SAME instruction word to three different
// stack slots before the opcode was even known -- SEH forcing values that live
// across the __try to be memory-resident instead of register-allocated. Hardware
// counters ruled out the usual suspects (indirect-branch mispredicts ~0, IPC ~2.0),
// so the interpreter is WORK-bound and that preamble was a real part of the per-op
// cost: removing the __try measured 2.0-3.1x faster across seven profiles. The loop
// therefore lives in its own SEH-free run_switch_loop, and run_switch is now nothing
// but the __try frame. The disable is kept (a helper called from run_switch itself would still trip
// it) and is scoped with a matching pop at end-of-file so it does not leak into the
// natives compiled after this header is included by vmcore.cpp, which inline their
// forceinline helpers fine and must stay covered by 4714.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4714)
#endif

#ifdef VM_COUNT_OPS
// -----------------------------------------------------------------------------
// PROFILING BUILD ONLY -- a per-opcode execution histogram. Absent from the
// shipping VM: everything here is inside `#ifdef VM_COUNT_OPS`, so an ordinary
// build emits not one instruction for it.
//
// Enable for a throwaway build by putting the define in the compiler's
// environment (the MSVC driver prepends whatever is in %CL%), then FORCING a
// rebuild -- MSBuild cannot see that a define changed, so an incremental build
// would silently reuse the uninstrumented objects:
//
//     CL=/DVM_COUNT_OPS msbuild vMachine.sln /t:Rebuild /p:Configuration=Release /p:Platform=x64
//
// Counting costs one increment per dispatched instruction, so the COUNTS are
// exact but the WALL TIME of an instrumented run is not comparable to a
// shipping run -- take timings from a clean build and op counts from this one
// (they are deterministic, so the two runs agree on what was executed).
//
// Remember to rebuild WITHOUT the define afterwards: leaving an instrumented
// static_vmrun.exe in x64/Release is the stale-exe trap that has bitten before.
// -----------------------------------------------------------------------------
#include <cstdio>

namespace vm_prof {
    inline uint64_t op_counts[256]{};

    // Dump non-zero buckets to stderr, descending, with each opcode's share.
    // stderr, not the VM's `out` sink, so a profiled run's stdout still matches
    // the ordinary run byte for byte.
    inline void dump_op_counts() {
        uint64_t total = 0;
        for (uint64_t c : op_counts) total += c;
        if (total == 0) return;

        uint8_t idx[256];
        int     n = 0;
        for (int i = 0; i < 256; ++i) if (op_counts[i]) idx[n++] = static_cast<uint8_t>(i);
        for (int i = 1; i < n; ++i) {            // insertion sort, descending by count
            uint8_t k = idx[i];
            int     j = i - 1;
            while (j >= 0 && op_counts[idx[j]] < op_counts[k]) { idx[j + 1] = idx[j]; --j; }
            idx[j + 1] = k;
        }

        std::fprintf(stderr, "[VM_OPS] total=%llu  distinct=%d\n",
                     static_cast<unsigned long long>(total), n);
        for (int i = 0; i < n; ++i) {
            const uint8_t id  = idx[i];
            const char*   nm  = "?";
            for (const OpEntry& e : OpList) if (e.id == id) { nm = e.name; break; }
            std::fprintf(stderr, "[VM_OPS] %-22s %14llu  %6.2f%%\n", nm,
                         static_cast<unsigned long long>(op_counts[id]),
                         100.0 * static_cast<double>(op_counts[id]) / static_cast<double>(total));
        }
    }
}
#endif

// SEH code for a trait-dispatch miss (PROTO_RESOLVE found no implementation of the
// method for the receiver's runtime type). Raised from run_switch's PROTO_RESOLVE
// case and translated by its __except -- the same abort-category as the array-OOB /
// invalid-map-key traps. Distinct from VM_EXC_HEAP_EXHAUSTED (0xE0564D00) and
// VM_EXC_INVALID_MAP_KEY (0xE0564D01).
inline constexpr unsigned long VM_EXC_NO_TRAIT_IMPL = 0xE0564D02u; // 'VM'\2, customer bit set

// =============================================================================
// Interpreter.h -- the V3 while{switch} dispatch loop.
//
// A single function drives every opcode, keeping the hot interpreter state
// (ip / window / return-stack cursors / current frame size) in LOCALS, so the
// compiler can hold them in registers across the whole loop instead of
// round-tripping through the Context struct at every handler boundary. That
// register residency -- not the dispatch instruction -- is what a dispatch
// benchmark (since removed) showed to be the real win over the old threaded
// tail-call handlers (see "Dispatch" in docs/VirtualMachine.md).
//
// GC correctness rule -- Context is a safepoint snapshot: the collector reads
// its roots THROUGH ctx, but the live state lives in these locals. So every
// opcode that can allocate -- ALLOC, ANEW, NEW_STRUCT, MAKE_CLOSURE, TO_STRING,
// ADD (string-concat branch), CALL_NATIVE, MAP_NEW, MAP_SET (grow), MAP_KEYS,
// MAP_VALUES, VEC_NEW and VEC_PUSH (grow) -- SYNC_TO_CTX() before the allocating call. Between allocations the
// ctx copies are intentionally stale, which is safe because nothing reads them
// there. The register stack and return stack never move, so no reload of `window`
// is needed after a collection (only the Values *inside* the window are rewritten
// in place, through the roots the GC found via ctx).
//
// This header is included by the single runtime TU (vmcore.cpp) only; run_switch
// and its helpers are `static`. It is the SOLE dispatcher: the threaded tail-call
// path (the op_*.h handlers, run_bytecode and the g_use_switch_dispatch toggle) was
// removed together with the vm_bench benchmark that settled the choice.
// =============================================================================

// Safepoint allocation helpers -- kept OUT-OF-LINE so the SEH-owning function itself
// performs no C++ `throw`. The exception propagates out through run_switch's frame,
// which has no objects to unwind. Since the earlier split this is belt-and-braces
// rather than load-bearing: the loop these are called from (run_switch_loop) holds no
// __try at all. Note these throw for the SAME condition that op_string.h / op_vec.h /
// op_map.h / op_bytes.h raise as VM_EXC_HEAP_EXHAUSTED -- see that code's definition
// for why one condition still has two mechanisms.
SKARN_NOINLINE static Value switch_alloc_array(Context* ctx, int32_t nslots) {
    // 0 is a valid (empty) array -- only a negative count is a bug. A 0-slot
    // KIND_ARRAY is just the 8-byte header; len is 0 and every index traps.
    if (nslots < 0) {
        throw std::runtime_error("ALLOC requires a non-negative slot count");
    }
    GcObject* obj = ctx->vm->heap->alloc_slots_gc(GcObject::KIND_ARRAY,
        static_cast<uint32_t>(nslots), ctx);
    if (!obj) {
        throw std::runtime_error("VM heap exhausted after GC");
    }
    return Value::fromPtr(obj->slots());
}

// ANEW's out-of-line allocator: element count comes from a register (unlike ALLOC's
// immediate), so it is a runtime int. Mirrors switch_alloc_array (0 allowed, negative
// is a fault) and shares the same collect/grow ladder via alloc_slots_gc.
SKARN_NOINLINE static Value switch_anew(Context* ctx, int64_t count) {
    if (count < 0) {
        throw std::runtime_error("array() requires a non-negative element count");
    }
    GcObject* obj = ctx->vm->heap->alloc_slots_gc(GcObject::KIND_ARRAY,
        static_cast<uint32_t>(count), ctx);
    if (!obj) {
        throw std::runtime_error("VM heap exhausted after GC (ANEW)");
    }
    return Value::fromPtr(obj->slots());
}

SKARN_NOINLINE static Value switch_new_struct(Context* ctx, uint16_t type_id) {
    assert(type_id < ctx->vm->struct_type_count && "NEW_STRUCT type id out of range");
    const uint32_t n_fields = ctx->vm->struct_types[type_id].field_count;
    GcObject* obj = ctx->vm->heap->alloc_object_gc(type_id, n_fields, ctx);
    if (!obj) {
        throw std::runtime_error("VM heap exhausted after GC (NEW_STRUCT)");
    }
    return Value::fromPtr(obj->slots());
}

// switch_make_closure -- allocate a KIND_CLOSURE for fn_id and populate its capture
// slots from window[capture_base .. capture_base + ncaptures). Kept OUT-OF-LINE for
// the same reason as switch_new_struct (run_switch owns the SEH frame; the throw
// lives here). ncaptures / self_slot come from the function table. Because
// alloc_closure_gc may collect, the captures are copied AFTER the allocation, read
// through ctx->window_ptr so any heap-pointer capture is taken at its post-collection
// address (the register frame is a GC root, forwarded in place). If the function has a
// self-capture slot (direct self-recursive lambda, Milestone A6), it is back-patched
// with the closure itself -- a self-reference the copying collector handles as a cycle.
SKARN_NOINLINE static Value switch_make_closure(Context* ctx, uint16_t fn_id, uint8_t capture_base) {
    assert(fn_id < ctx->vm->fn_table_size && "MAKE_CLOSURE fn id out of range");
    const FnInfo&  fn = ctx->vm->fn_table[fn_id];
    const uint32_t n  = fn.ncaptures;
    GcObject* obj = ctx->vm->heap->alloc_closure_gc(fn_id, n, ctx);
    if (!obj) {
        throw std::runtime_error("VM heap exhausted after GC (MAKE_CLOSURE)");
    }
    Value*       dst = obj->slots();
    const Value* win = ctx->window_ptr;
    for (uint32_t i = 0; i < n; ++i)
        dst[i] = win[capture_base + i];
    if (fn.self_slot != FnInfo::NO_SELF) {
        assert(fn.self_slot < n && "MAKE_CLOSURE self_slot out of range");
        dst[fn.self_slot] = Value::fromPtr(obj->slots());
    }
    return Value::fromPtr(obj->slots());
}

// MAP_KEYS / MAP_VALUES -- collect a map's live keys (want_values=false) or values
// (true) into a fresh KIND_ARRAY of exactly `count` elements, in backing (hash) order.
// Out-of-line so run_switch performs no C++ throw (like switch_anew). GC choreography:
// exactly ONE allocation (the result array). It may collect -> the map moves, but it is
// rooted via *map_slot (a register slot; register memory never moves, its Values are
// rewritten in place), so we re-fetch header + backing AFTER the alloc. The fill loop
// then allocates nothing, so backing and result stay put. rd may alias ra: the case
// writes window[rd] only after this returns, and *map_slot is fully read here first.
SKARN_NOINLINE static Value switch_map_collect(Context* ctx, Value* map_slot, bool want_values) {
    GcObject*     hdr   = GcObject::from_slots(map_slot->asPtr());
    assert(hdr->kind == GcObject::KIND_MAP && "MAP_KEYS/VALUES on a non-map");
    const int64_t count = hdr->slots()[MAP_SLOT_COUNT].asSigned48();   // live entries
    GcObject* result = ctx->vm->heap->alloc_slots_gc(GcObject::KIND_ARRAY,
        static_cast<uint32_t>(count), ctx);
    if (!result) {
        throw std::runtime_error("VM heap exhausted after GC (map keys/values)");
    }
    hdr = GcObject::from_slots(map_slot->asPtr());                     // re-fetch (may have moved)
    GcObject*      backing = GcObject::from_slots(hdr->slots()[MAP_SLOT_BACKING].asPtr());
    const uint32_t cap     = backing->slot_count() / 2;
    const Value*   b       = backing->slots();
    Value*         out     = result->slots();
    const uint32_t off     = want_values ? 1u : 0u;
    uint32_t j = 0;
    for (uint32_t p = 0; p < cap; ++p) {           // tight fill: no allocation -> nothing moves
        const Value key = b[2 * p];
        if (key.isUndefined() || key.isTombstone()) continue;
        out[j++] = b[2 * p + off];                 // key (off 0) or value (off 1)
    }
    assert(j == static_cast<uint32_t>(count) && "map count / live-slot mismatch");
    return Value::fromPtr(result->slots());
}

// trait_dense_id -- fold a value's runtime type into the trait-dispatch dense key
// (TypeUniverse.h): a fixed built-in block [0, BUILTIN_COUNT) for immediates and
// heap non-structs, then struct type ids offset above it. The one place the VM
// maps a live Value to the index PROTO_RESOLVE uses. Returns a value >= any table
// width (0xFFFFFFFF) for types that can never be a trait receiver (Undefined /
// Tombstone / native FuncPtr) so the caller's `dense < width` guard forces a miss.
SKARN_FORCEINLINE static uint32_t trait_dense_id(Value v) {
    if (v.isPtr()) {
        GcObject* obj = GcObject::from_slots(v.asPtr());
        switch (obj->kind) {
            case GcObject::KIND_OBJECT:
                return static_cast<uint32_t>(BUILTIN_COUNT) + obj->object_type_id();
            case GcObject::KIND_STRING:  return TID_STRING;
            case GcObject::KIND_ARRAY:   return TID_ARRAY;
            case GcObject::KIND_MAP:     return TID_MAP;
            case GcObject::KIND_CLOSURE: return TID_CLOSURE;
            case GcObject::KIND_VEC:     return TID_VEC;
            case GcObject::KIND_BYTES:   return TID_BYTES;
            default:                     return 0xFFFFFFFFu; // no other heap kind is a receiver
        }
    }
    if (v.isInt())    return TID_INT;
    if (v.isDouble()) return TID_DOUBLE;
    if (v.isBool())   return TID_BOOL;
    if (v.isNil())    return TID_NIL;
    if (v.isFunc())   return TID_FUNC;
    if (v.isAtom())   return TID_ATOM;
    return 0xFFFFFFFFu; // Undefined / Tombstone / native FuncPtr -> always a miss
}

// indexable_view -- resolve an array-or-vector receiver to (element base, logical
// length) so ARRAY_GET / ARRAY_SET / LEN are dual-kind over a fixed KIND_ARRAY and a
// growable KIND_VEC. A KIND_ARRAY indexes its own slots; a KIND_VEC indexes its
// backing's slots, bounded by the header `count` (NOT the backing capacity), so the
// unused tail [count, cap) is out of bounds. Returns false for any other receiver
// (the caller then SYNC+raise_located's). The KIND_ARRAY branch is first so the fixed
// -array hot path stays a single predicted compare. `out_elems`/`out_len` are written
// only on a true return.
SKARN_FORCEINLINE static bool
indexable_view(Value coll, Value*& out_elems, uint64_t& out_len) noexcept {
    if (!coll.isPtr()) return false;
    GcObject* o = GcObject::from_slots(coll.asPtr());
    if (o->kind == GcObject::KIND_ARRAY) {
        out_elems = o->slots();
        out_len   = o->slot_count();
        return true;
    }
    if (o->kind == GcObject::KIND_VEC) {
        out_elems = GcObject::from_slots(o->slots()[VEC_SLOT_BACKING].asPtr())->slots();
        out_len   = static_cast<uint64_t>(o->slots()[VEC_SLOT_COUNT].asSigned48());
        return true;
    }
    return false;
}

// =============================================================================
// Serious-fault reporting (Phase 1). A "serious" VM fault (a wrong-typed heap-op
// receiver, division by zero, an out-of-bounds index, an invalid map key, a trait
// miss) is turned into a DEFINED, LOCATED, reported abort -- in Release too. It is
// raised via an out-of-line C++ throw (a C++ exception carries the message + line +
// stacktrace; an SEH exception cannot), kept out of run_switch itself exactly like
// switch_alloc_array so the SEH __try function never unwinds a C++ object. The
// caller SYNC_TO_CTX()s first (the receiver ops are not safepoints, so ctx->ip /
// ctx->ret_stack_ptr would otherwise be stale) and then calls raise_located; a
// single top-level handler (execute()'s caller / static_vmrun) prints ctx and terminates.
// There is NO in-script recovery in this phase (that is deferred Phase 2). All the
// diagnostics are cold-path: they add nothing to the error-free hot path.
// =============================================================================

// Map an absolute bytecode pointer to its 1-based source line / column, or 0 if
// unknown (no table supplied, or ip outside it).
static uint32_t fault_line_of(const VM* vm, const uint8_t* ip) {
    if (!vm->code_base || !vm->line_table || ip < vm->code_base) return 0;
    const size_t idx = static_cast<size_t>(ip - vm->code_base) / 4;
    return idx < vm->line_table_size ? vm->line_table[idx] : 0;
}
static uint32_t fault_col_of(const VM* vm, const uint8_t* ip) {
    if (!vm->code_base || !vm->column_table || ip < vm->code_base) return 0;
    const size_t idx = static_cast<size_t>(ip - vm->code_base) / 4;
    return idx < vm->column_table_size ? vm->column_table[idx] : 0;
}

// Best-effort enclosing function id for an instruction pointer: the declared function whose
// code_offset is the largest <= this instruction's index. Returns SIZE_MAX for top-level code
// (before the first function) or when no fn_table was supplied. fn_table / function_names /
// function_modules are all keyed by function id, so a hit index into one indexes the others.
static size_t fault_fn_index_of(const VM* vm, const uint8_t* ip) {
    if (!vm->code_base || ip < vm->code_base) return static_cast<size_t>(-1);
    const uint32_t idx = static_cast<uint32_t>((ip - vm->code_base) / 4);
    size_t   best     = static_cast<size_t>(-1);
    uint32_t best_off = 0;
    for (size_t i = 0; i < vm->fn_table_size; ++i) {
        const uint32_t off = vm->fn_table[i].code_offset;
        if (off <= idx && (best == static_cast<size_t>(-1) || off >= best_off)) {
            best = i; best_off = off;
        }
    }
    return best;
}

// Render a compiler-internal synthetic function name into a readable form. The names come from
// Codegen: `$impl$Trait$Type$method`, `$default$Trait$method`, `$inherent$Type$method`, `$lambdaN`.
// A user function (no leading '$') passes through unchanged. Cold-path only.
static std::string prettify_fn_name(const std::string& raw) {
    if (raw.empty() || raw[0] != '$') return raw;
    // The ENTRY program is an ordinary module to the compiler, so its declarations mangle to
    // `$entry::name` -- and a synthetic method name EMBEDS that: `$impl$$entry::Trait$$entry::Type$m`.
    // The prefix is internal, and its own '$' would mis-segment the structural split below (four
    // fields would parse as six), so strip every occurrence FIRST. A real module keeps its path:
    // `$impl$net::http::Show$net::http::Req$show`. The literal mirrors ENTRY_MODULE_PREFIX in
    // static_compiler/Naming.h -- vmcore must not depend on the compiler, exactly as the `$impl` /
    // `$default` / `$lambda` literals below already are.
    std::string s = raw;
    for (const std::string pre = "$entry::";;) {
        const size_t i = s.find(pre);
        if (i == std::string::npos) break;
        s.erase(i, pre.size());
    }
    if (s.empty() || s[0] != '$') return s;       // a plain entry fn: `$entry::boom` -> `boom`
    std::vector<std::string> seg;                 // segments after the leading '$'
    for (size_t i = 1; i <= s.size(); ) {
        size_t j = s.find('$', i);
        if (j == std::string::npos) j = s.size();
        seg.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    if (seg.size() == 4 && seg[0] == "impl")       // Trait / Type / method
        return seg[1] + "::" + seg[3] + " (impl for " + seg[2] + ")";
    if (seg.size() == 3 && seg[0] == "default")    // Trait / method
        return seg[1] + "::" + seg[2] + " (default)";
    // An inherent (traitless) method. No qualifying suffix: `Type::method` is exactly how the
    // source can now spell the call, so the trace name and the surface name coincide.
    if (seg.size() == 3 && seg[0] == "inherent")   // Type / method
        return seg[1] + "::" + seg[2];
    if (!seg.empty() && seg[0].rfind("lambda", 0) == 0)
        return "<lambda>";
    return s;                                      // unknown $-form: leave literal (prefix stripped)
}

// Build one structured fault frame (line/col + prettified enclosing function) for the
// instruction at `ip`. A top-level frame gets an empty function ("" == <script>).
static VmFault::Frame make_frame(const VM* vm, const uint8_t* ip) {
    VmFault::Frame fr;
    fr.line = fault_line_of(vm, ip);
    fr.col  = fault_col_of(vm, ip);
    const size_t fi = fault_fn_index_of(vm, ip);          // one scan feeds both name + module
    if (fi != static_cast<size_t>(-1)) {
        if (vm->function_names && fi < vm->function_names_size && !vm->function_names[fi].empty())
            fr.function = prettify_fn_name(vm->function_names[fi]);
        if (vm->function_modules && fi < vm->function_modules_size)
            fr.module = vm->function_modules[fi];          // "" = root/entry; drives the per-module caret
    }
    return fr;
}

// Format one frame as "line N (in <fn>)" (the flat-message form; keeps VmFault::what()
// byte-identical to the earlier throw so existing catch sites are unaffected).
static std::string frame_where(const VmFault::Frame& fr) {
    std::string s = fr.line ? ("line " + std::to_string(fr.line)) : std::string("line ?");
    s += fr.function.empty() ? std::string(" (in <script>)") : (" (in " + fr.function + ")");
    return s;
}

// Build a located, structured fault (+ a return-stack stacktrace) and throw it as a
// VmFault. [[noreturn]] and out-of-line, so run_switch stays throw-free; the C++
// exception propagates out through the SEH frame (whose filter returns CONTINUE_SEARCH
// for the C++ EH code). The caller must SYNC_TO_CTX() first so ctx->ip and
// ctx->ret_stack_ptr are current (the faulting ops are not safepoints).
SKARN_NOINLINE [[noreturn]]
static void raise_located(Context* ctx, const char* what) {
    const VM* vm = ctx->vm;
    // Cap the collected/printed stacktrace. A deep-recursion "stack overflow" can have
    // ~1M return frames; building and printing a Frame per entry would be tens of MB and
    // slow. We keep the innermost TRACE_CAP callers and only COUNT the rest (a shallow
    // fault -- every existing case -- is well under the cap, so its message is unchanged).
    constexpr size_t TRACE_CAP = 32;
    std::vector<VmFault::Frame> frames;
    frames.push_back(make_frame(vm, ctx->ip));    // [0] = the fault site
    // Stacktrace: walk the return stack innermost-caller first. old_ip is the return
    // target (the instruction AFTER the CALL); step back one instruction so the line
    // points at the call site. TCO'd calls reused their frame, so they do not appear.
    size_t total_callers = 0;
    for (ReturnFrame* f = ctx->ret_stack_ptr; f > ctx->ret_stack_base; ) {
        --f;
        ++total_callers;
        if (frames.size() <= TRACE_CAP) {         // collect [0] + up to TRACE_CAP callers
            const uint8_t* call_ip = f->old_ip;
            if (vm->code_base && call_ip >= vm->code_base + 4) call_ip -= 4;
            frames.push_back(make_frame(vm, call_ip));
        }
    }
    std::string msg = std::string(what) + " at " + frame_where(frames[0]);
    for (size_t i = 1; i < frames.size(); ++i)
        msg += "\n  called from " + frame_where(frames[i]);
    const size_t shown_callers = frames.size() - 1;
    if (total_callers > shown_callers)
        msg += "\n  ... (" + std::to_string(total_callers - shown_callers) + " more frames)";
    throw VmFault(std::move(msg), std::string(what), std::move(frames));
}

// Work budget for EQ_DEEP. A structural compare touches at most O(reachable nodes) pairs
// for any FINITE value; a semispace caps at 1 GiB (~64M minimal 16-byte objects), so
// exceeding this bound means the value graph is CYCLIC (buildable only via a `mut` field).
// The budget is on total WORK, not depth, so no finite structure is ever truncated to a
// wrong answer -- it only bounds the non-terminating cyclic case, which then raises a
// located "stack overflow" fault (a deliberate choice; Rust's derived PartialEq
// likewise loops on an Rc cycle). Contrast TO_STRING's depth cap, which truncates DISPLAY
// where a wrong-but-shorter dump is harmless.
inline constexpr int64_t EQ_DEEP_MAX_WORK = 200'000'000;

// Structural (deep / value) equality behind the EQ_DEEP opcode. Immediates + String defer
// to the general EQ semantics (operator== + string content); two heap composites compare
// by shape (see the EQ_DEEP opcode-table entry). Uses an EXPLICIT worklist rather than
// native C++ recursion, so a deep-but-finite value (a long cons list, a deep tree) compares
// correctly and can never overflow the native stack -- which the SEH filter cannot cleanly
// catch anyway (it handles only the VM's own guard-page AV, not EXCEPTION_STACK_OVERFLOW).
// SKARN_NOINLINE keeps run_switch's __try frame free of the std::vector's unwind state
// (mirrors raise_located). Read-only: does NO allocation, so the value graph cannot move
// mid-compare and the queued Values need no rooting. May THROW via raise_located (cycle
// budget / closure operand), so the caller must SYNC_TO_CTX() first.
[[nodiscard]] SKARN_NOINLINE
static bool value_deep_eq(Value a0, Value b0, Context* ctx) {
    std::vector<std::pair<Value, Value>> work;
    work.emplace_back(a0, b0);
    int64_t budget = EQ_DEEP_MAX_WORK;
    while (!work.empty()) {
        if (--budget < 0) raise_located(ctx, "stack overflow");   // cyclic value graph
        const std::pair<Value, Value> pr = work.back();
        work.pop_back();
        const Value a = pr.first;
        const Value b = pr.second;

        // Non-pointer (or pointer-vs-non-pointer): general EQ semantics. operator==
        // handles numeric promotion, -0.0==0.0, NaN!=NaN, and bit-identity for
        // bool/atom/nil/func; a ptr-vs-non-ptr mismatch compares unequal there.
        if (!a.isPtr() || !b.isPtr()) {
            if (!(a == b)) return false;
            continue;
        }
        if (a.asPtr() == b.asPtr()) continue;                     // same object -> equal subtree
        const GcObject* oa = GcObject::from_slots(a.asPtr());
        const GcObject* ob = GcObject::from_slots(b.asPtr());
        if (oa->kind != ob->kind) return false;                  // different heap kind
        switch (oa->kind) {
        case GcObject::KIND_STRING:
            if (!str_eq(a, b)) return false;                     // content
            break;
        case GcObject::KIND_OBJECT: {                            // struct / tuple / enum variant
            if (oa->object_type_id() != ob->object_type_id()) return false;  // e.g. None vs Some
            const uint32_t n = oa->slot_count();
            if (n != ob->slot_count()) return false;             // defensive (same type -> same arity)
            const Value* sa = oa->slots();
            const Value* sb = ob->slots();
            for (uint32_t i = 0; i < n; ++i) work.emplace_back(sa[i], sb[i]);
            break;
        }
        case GcObject::KIND_ARRAY: {
            const uint32_t n = oa->slot_count();
            if (n != ob->slot_count()) return false;
            const Value* sa = oa->slots();
            const Value* sb = ob->slots();
            for (uint32_t i = 0; i < n; ++i) work.emplace_back(sa[i], sb[i]);
            break;
        }
        case GcObject::KIND_VEC: {                               // live prefix [0, count)
            const int64_t na = oa->slots()[1].asSigned48();      // VEC_SLOT_COUNT
            const int64_t nb = ob->slots()[1].asSigned48();
            if (na != nb) return false;
            const Value* sa = GcObject::from_slots(oa->slots()[0].asPtr())->slots();  // backing (slot 0)
            const Value* sb = GcObject::from_slots(ob->slots()[0].asPtr())->slots();
            for (int64_t i = 0; i < na; ++i) work.emplace_back(sa[i], sb[i]);
            break;
        }
        case GcObject::KIND_BYTES: {                            // live prefix [0, count)
            const int64_t na = oa->slots()[1].asSigned48();      // BYTES_SLOT_COUNT
            const int64_t nb = ob->slots()[1].asSigned48();
            if (na != nb) return false;
            const char* pa = GcObject::from_slots(oa->slots()[0].asPtr())->bytes();   // backing (slot 0)
            const char* pb = GcObject::from_slots(ob->slots()[0].asPtr())->bytes();
            for (int64_t i = 0; i < na; ++i) if (pa[i] != pb[i]) return false;
            break;
        }
        case GcObject::KIND_MAP: {
            // ORDER-INDEPENDENT: equal iff same live count AND every (k, v) of a has k
            // present in b with a deeply-equal value. Keys are Hashable (immediate/String),
            // so map_has/map_get compare keys by the correct rule (bits / content); the
            // value pairs are queued for the deep compare. map_get/map_has are non-allocating.
            GcObject* ha = const_cast<GcObject*>(oa);
            GcObject* hb = const_cast<GcObject*>(ob);
            if (ha->slots()[MAP_SLOT_COUNT].asSigned48() != hb->slots()[MAP_SLOT_COUNT].asSigned48())
                return false;
            const GcObject* back = GcObject::from_slots(ha->slots()[MAP_SLOT_BACKING].asPtr());
            const uint32_t  ns   = back->slot_count();           // 2 * capacity
            const Value*    kv   = back->slots();
            for (uint32_t i = 0; i + 1 < ns; i += 2) {
                const Value k = kv[i];
                if (k.isUndefined() || k.isTombstone()) continue;
                if (!map_has(hb, k)) return false;               // key absent in b
                work.emplace_back(kv[i + 1], map_get(hb, k));    // values -> deep compare
            }
            break;
        }
        case GcObject::KIND_CLOSURE:
            // The checker forbids `==` on a function-carrying type (a closure is not Eq),
            // so this is unreachable from well-typed source; defensive located trap.
            raise_located(ctx, "cannot compare function values");
        default:
            return false;
        }
    }
    return true;
}

// The soft-check margin (Value slots) run_switch keeps committed above the register
// window base when growth is enabled. PROVABLE bound: the widest register-index field
// in Instruction.h is 8-bit (c2.rd / wide.rd / b1.ra), so a single activation can touch
// at most window[0..255] -- 256 slots above its base (this already subsumes its
// outgoing-arg window). 512 is 2x headroom. NOTE: this rests on the 8-bit index ceiling
// (the r8 layout is unused); revisit if a wider window index is ever added.
static constexpr int VM_REG_MARGIN = 512;

// Grow the register and/or return stacks so the callee frame about to be pushed at a
// non-tail CALL fits. Called from run_switch ONLY when growth is enabled
// (ctx->vm->resources set) AND the soft-check tripped. One doubling step per stack always
// adds >= the INITIAL commit (>= 131072 reg slots / 16384 ret frames), far more than the
// <= 256+margin a single frame needs, so a single grow suffices. On the reserved cap or a
// VirtualAlloc commit failure it raises the same located VmFault as any other serious
// fault. SKARN_NOINLINE keeps run_switch's __try free of the cold commit/throw path;
// the caller SYNC's the pre-advance cursors first so an overflow trace points at the CALL.
SKARN_NOINLINE
static void grow_stacks_or_overflow(Context* ctx, bool need_reg, bool need_ret) {
    VM_Resources* res = ctx->vm->resources;
    if (need_reg && !res->grow_reg()) raise_located(ctx, "stack overflow");
    if (need_ret && !res->grow_ret()) raise_located(ctx, "stack overflow");
}

// Translate an ACCESS_VIOLATION into a message that says what actually happened.
//
// This used to report EVERY access violation as "VM Stack Overflow (hardware guard page)",
// which is right for exactly one cause and actively misleading for the rest: a dangling or
// corrupt heap pointer faults inside memcpy at a heap address and was reported as a stack
// overflow, sending diagnosis in the wrong direction (that cost real time while tracking
// down the stale-frame-register defect).
//
// The fault address distinguishes them. A genuine stack overflow lands inside one of the
// four stack regions: past the committed end (reserved pages are NOACCESS -- what a
// hand-built Context with growth disabled hits) or in the trailing guard page. Anything
// else is a wild pointer. When the regions are unknown (no VM_Resources -- hand-built test
// Contexts) we cannot classify, and say so rather than guess.
//
// Out-of-line and [[noreturn]]: building the message needs a std::string, which must not
// live in run_switch's __try/__except function (C2712) -- the same discipline as
// raise_located and the alloc helpers.
SKARN_NOINLINE [[noreturn]]
static void raise_access_violation(const Context* ctx, uintptr_t fault_addr, bool is_write) {
    const VM_Resources* res = (ctx && ctx->vm) ? ctx->vm->resources : nullptr;
    const void* addr = reinterpret_cast<const void*>(fault_addr);

    if (res && res->address_in_stacks(addr))
        throw std::runtime_error(std::format(
            "VM Stack Overflow ({} at {}, inside the register/return stack region)",
            is_write ? "write" : "read", addr));

    throw std::runtime_error(std::format(
        "Memory access violation: {} at {}{}. This is NOT a stack overflow -- the address "
        "lies outside the VM's stack regions, so it is a wild or dangling pointer "
        "(heap corruption, or a GC root that was not forwarded). Re-run the Debug build: "
        "Heap::verify_roots asserts at the first safepoint that sees a bad root, and "
        "verify_heap checks the survivors after every collection.",
        is_write ? "write" : "read", addr,
        res ? "" : " (VM stack regions unknown -- no VM_Resources, so this could not be "
                   "classified as a stack overflow either)"));
}

// run_switch -- execute bytecode via a single register-resident switch loop.
// Reads its initial state from ctx (set up by execute()), leaves results in the
// register stack (which the caller inspects through VM_Resources), and returns on
// HALT. SEH faults are translated to std::runtime_error by run_switch's __except.
// The dispatch loop proper. Deliberately SEH-FREE and deliberately __declspec(noinline):
// the noinline is LOAD-BEARING, because with LTCG the compiler would otherwise inline this
// back into run_switch's __try scope and reinstate exactly the cost the split removes.
SKARN_NOINLINE static void run_switch_loop(Context* ctx) {
    const uint8_t* ip     = ctx->ip;
    // The bytecode base (entry = instruction 0). CALL_INDIRECT jumps to an ABSOLUTE
    // code position (code_base + 4 * code_offset) recovered from the function table,
    // unlike the relative offset a static CALL/J carries.
    const uint8_t* const code_base = ctx->ip;
    Value*         window = ctx->window_ptr;
    ReturnFrame*   rsp    = ctx->ret_stack_ptr;
    uint8_t*       fsp    = ctx->frame_size_ptr;
    uint8_t        cfs    = ctx->current_frame_size;
    // Parallel closure-stack cursor (Milestone A), lockstep with fsp/rsp: on a CALL
    // it stores the SAVED caller closure, restored on RET. It needs no ctx sync --
    // the collector derives the live depth from the return stack and scans the stack
    // MEMORY (which never moves) directly; csp is pure run_switch scratch. The
    // current activation's closure itself lives in ctx->vm->current_closure (coherent
    // for the collector; see VM.h). Non-null in every real run (execute() sets it).
    Value*         csp    = ctx->vm->closure_stack_base + (rsp - ctx->ret_stack_base);

    // Flush the register-resident hot state back into ctx so a collection triggered
    // by an allocating opcode sees the correct register-window roots. Required before
    // EVERY safepoint (the four allocating opcodes). See the GC rule above.
    // In Debug it additionally validates the register roots it just published
    // (Heap::verify_roots): every safepoint routes through here, so this covers
    // the whole safepoint set -- including any future allocating opcode -- without
    // maintaining a call-site list. Root-only, so it stays affordable. Compiles
    // out entirely in Release.
    #ifndef NDEBUG
        #define SYNC_VERIFY_ROOTS()                                     \
            do {                                                        \
                if (ctx->vm && ctx->vm->heap)                           \
                    ctx->vm->heap->verify_roots(ctx);                   \
            } while (0)
    #else
        #define SYNC_VERIFY_ROOTS() do { } while (0)
    #endif

    #define SYNC_TO_CTX()                       \
        do {                                    \
            ctx->ip                 = ip;       \
            ctx->window_ptr         = window;   \
            ctx->ret_stack_ptr      = rsp;      \
            ctx->frame_size_ptr     = fsp;      \
            ctx->current_frame_size = cfs;      \
            SYNC_VERIFY_ROOTS();                \
        } while (0)

    // Serious-fault receiver checks (Phase 1). A non-pointer / wrong-kind receiver on
    // a heap-typed op raises a defined, LOCATED abort (raise_located) instead of
    // dereferencing a wild pointer -- one predicted-not-taken branch on the (already
    // memory-bound) heap ops, nothing on the arithmetic hot path. SYNC first so the
    // fault's ip / stacktrace are current (these ops are not safepoints). REQUIRE_KIND
    // binds `objptr` to the validated object; CHECK_KIND only validates (for ops that
    // pass the register pointer on). Necessary for correctness: asPtr() masks a
    // non-pointer into a plausible address, so a hardware fault would not catch it.
    #define REQUIRE_KIND(objptr, val, want, msg)                                    \
        GcObject* objptr = nullptr;                                                  \
        if (!(val).isPtr() ||                                                        \
            (objptr = GcObject::from_slots((val).asPtr()))->kind != (want))          \
            [[unlikely]] { SYNC_TO_CTX(); raise_located(ctx, (msg)); }
    #define CHECK_KIND(val, want, msg)                                              \
        if (!(val).isPtr() ||                                                        \
            GcObject::from_slots((val).asPtr())->kind != (want)) [[unlikely]]         \
            { SYNC_TO_CTX(); raise_located(ctx, (msg)); }

    // Growable-stack soft-check, emitted at each of the three non-tail call ops BEFORE
    // the frame push. Gated on ctx->vm->resources (growth enabled): if the upcoming
    // callee frame would pass the committed register end (minus VM_REG_MARGIN) or the
    // committed return end, commit the next chunk (or raise a located "stack overflow" at
    // the cap). `cfs` is still the CALLER frame size here (the slide), so window + cfs is
    // the callee base. On the common (no-grow) path this is one predicted-not-taken branch
    // reading resources; when null (vm_tests' raw Contexts) it is a single null test. The
    // SYNC uses ip-4 so an overflow trace points at THIS call instruction (ip was already
    // advanced past it). A successful grow moves no base, so the locals stay valid.
    #define GROW_CHECK()                                                            \
        do {                                                                        \
            VM_Resources* _res = ctx->vm->resources;                                \
            if (_res) [[likely]] {                                                   \
                const bool _nr = (window + cfs + VM_REG_MARGIN) >= _res->reg_committed_end(); \
                const bool _nt = (rsp + 1) >= _res->ret_committed_end();            \
                if (_nr || _nt) [[unlikely]] {                                       \
                    ctx->ip = ip - 4; ctx->window_ptr = window;                      \
                    ctx->ret_stack_ptr = rsp; ctx->frame_size_ptr = fsp;            \
                    ctx->current_frame_size = cfs;                                   \
                    grow_stacks_or_overflow(ctx, _nr, _nt);                          \
                }                                                                    \
            }                                                                        \
        } while (0)

    // A bare scope where the SEH __try used to be. The __try now lives in run_switch
    // (below), which calls this function -- see the header note on why. Keeping a plain
    // block here leaves the loop body's indentation untouched, so the change stays a
    // readable diff instead of a 1300-line reindent.
    {
        for (;;) {
            const uint32_t    raw   = *reinterpret_cast<const uint32_t*>(ip);
            const Instruction instr = Instruction::from_raw(raw);

#ifdef VM_COUNT_OPS
            ++vm_prof::op_counts[raw & 0xFFu];
#endif
            switch (static_cast<OpCode>(raw & 0xFFu)) {

            case OpCode::NOP:
                ip += 4; continue;

            case OpCode::HALT:
                return;

            // ---- control flow ----
            case OpCode::J:
                ip += 4;
                ip += (static_cast<intptr_t>(instr.j.offset) << 2);
                continue;

            case OpCode::CALL: {
                ip += 4;
                // The slide is the CALLER's frame size (must be > 0); window_size is
                // the CALLEE's frame size / GC scan count. See op_call in op_control.h.
                const uint8_t caller_frame_size = cfs;
                assert(caller_frame_size > 0 && "CALL slide (caller frame size) must be > 0");
                assert(instr.call.window_size > 0 && "CALL callee frame size must be > 0");
                assert(rsp < ctx->ret_stack_limit && "Return stack overflow");
                GROW_CHECK();
                rsp->old_ip     = ip;
                rsp->old_window = window;
                *fsp            = caller_frame_size;
                *csp            = ctx->vm->current_closure; // save caller closure (lockstep)
                ++rsp; ++fsp; ++csp;
                window += caller_frame_size;
                cfs     = static_cast<uint8_t>(instr.call.window_size);
                // A static CALL targets a NAMED function (no captures), so the callee
                // activation runs with no current closure. See CALL_INDIRECT for the
                // closure-callee case (Milestone A4).
                ctx->vm->current_closure = Value{};
                ip     += (static_cast<intptr_t>(instr.call.offset) << 2);
                continue;
            }

            case OpCode::CALL_INDIRECT: {
                ip += 4;
                // Recover the callee from the function VALUE in register ra, branching on
                // its tag: a capture-free Func immediate carries the fn id directly and runs
                // with no current closure; a KIND_CLOSURE heap object carries the fn id in
                // its header AND becomes the callee activation's current closure (so its body
                // can LOAD_CAPTURE). A non-function callee is a (deferred) runtime error --
                // caught here as a Debug assert on the type-check-free fast path.
                const Value callee = window[instr.r6.ra];
                uint32_t    fn_id;
                Value       new_closure;                 // Undefined for the Func-immediate case
                if (callee.isFunc()) {
                    fn_id = callee.asFuncId();
                } else {
                    assert(callee.isPtr() && "CALL_INDIRECT on a non-function value");
                    GcObject* clo = GcObject::from_slots(callee.asPtr());
                    assert(clo->kind == GcObject::KIND_CLOSURE &&
                           "CALL_INDIRECT on a non-closure heap pointer");
                    fn_id       = clo->closure_fn_id();
                    new_closure = callee;                // install as the callee's current closure
                }
                assert(fn_id < ctx->vm->fn_table_size && "CALL_INDIRECT fn id out of range");
                const FnInfo& fn = ctx->vm->fn_table[fn_id];
                // Debug arity check: a dynamic call can pass the wrong argument count.
                // The real (throwing) error is deferred to the error model.
                assert(static_cast<uint8_t>(instr.r6.rb) == fn.arity &&
                       "CALL_INDIRECT arity mismatch");
                // Frame setup is exactly a static CALL, except the callee frame size and
                // entry point come from the table (runtime) instead of the instruction.
                const uint8_t caller_frame_size = cfs;
                assert(caller_frame_size > 0 && "CALL_INDIRECT slide (caller frame size) must be > 0");
                assert(fn.frame_size > 0 && "CALL_INDIRECT callee frame size must be > 0");
                assert(rsp < ctx->ret_stack_limit && "Return stack overflow");
                GROW_CHECK();
                rsp->old_ip     = ip;
                rsp->old_window = window;
                *fsp            = caller_frame_size;
                *csp            = ctx->vm->current_closure; // save caller closure (lockstep)
                ++rsp; ++fsp; ++csp;
                window += caller_frame_size;
                cfs     = fn.frame_size;
                ctx->vm->current_closure = new_closure;  // the callee closure, or Undefined
                ip      = code_base + (static_cast<intptr_t>(fn.code_offset) << 2);
                continue;
            }

            case OpCode::RESOLVE_CALL: {
                ip += 4;
                // Fused PROTO_RESOLVE + CALL_INDIRECT. The receiver is the OUTGOING arg 0 --
                // window[cfs] (the caller placed the args at [cfs, cfs + nargs) exactly as for a
                // CALL_INDIRECT, arg 0 = the receiver) -- so no receiver register is encoded.
                // Resolve method #slot for its runtime type (identical to PROTO_RESOLVE), then run
                // the CALL_INDIRECT frame setup with the resolved fn id. A resolved trait method is
                // always a plain top-level Func (never a closure), so current_closure = Undefined.
                const uint32_t method_id = instr.prop_slot();
                const Value    recv      = window[cfs];   // outgoing arg 0
                assert(ctx->vm->trait_table && "RESOLVE_CALL requires a trait table");
                assert(method_id < ctx->vm->trait_method_count &&
                       "RESOLVE_CALL method id out of range");
                const uint32_t width = ctx->vm->trait_table_width;
                const uint32_t dense = trait_dense_id(recv);
                uint16_t fn_id16 = TRAIT_METHOD_NONE;
                if (dense < width)
                    fn_id16 = ctx->vm->trait_table[static_cast<size_t>(method_id) * width + dense];
                if (fn_id16 == TRAIT_METHOD_NONE) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "no trait implementation for this type"); }
                const uint32_t fn_id = fn_id16;
                assert(fn_id < ctx->vm->fn_table_size && "RESOLVE_CALL fn id out of range");
                const FnInfo& fn = ctx->vm->fn_table[fn_id];
                assert(static_cast<uint8_t>(instr.r6.rd) == fn.arity &&
                       "RESOLVE_CALL arity mismatch");
                const uint8_t caller_frame_size = cfs;
                assert(caller_frame_size > 0 && "RESOLVE_CALL slide (caller frame size) must be > 0");
                assert(fn.frame_size > 0 && "RESOLVE_CALL callee frame size must be > 0");
                assert(rsp < ctx->ret_stack_limit && "Return stack overflow");
                GROW_CHECK();
                rsp->old_ip     = ip;
                rsp->old_window = window;
                *fsp            = caller_frame_size;
                *csp            = ctx->vm->current_closure; // save caller closure (lockstep)
                ++rsp; ++fsp; ++csp;
                window += caller_frame_size;
                cfs     = fn.frame_size;
                ctx->vm->current_closure = Value{};       // resolved trait method = plain Func
                ip      = code_base + (static_cast<intptr_t>(fn.code_offset) << 2);
                continue;
            }

            case OpCode::RESOLVE_TCO_CALL: {
                ip += 4;
                // Fused PROTO_RESOLVE + TCO_CALL_INDIRECT (the TAIL sibling of RESOLVE_CALL). Resolve
                // method #slot for the receiver, then reuse the current frame in place. ASYMMETRY with
                // RESOLVE_CALL: a tail call places its args in the REUSED frame [0, nargs) (no slide),
                // so the receiver = arg 0 is at window[0] (NOT the outgoing window[cfs]). Resolve is
                // otherwise identical to PROTO_RESOLVE/RESOLVE_CALL; frame setup is TCO_CALL_INDIRECT's
                // (no return/closure-stack push, no window slide -- only cfs + current_closure change).
                const uint32_t method_id = instr.prop_slot();
                const Value    recv      = window[0];      // reused-frame arg 0
                assert(ctx->vm->trait_table && "RESOLVE_TCO_CALL requires a trait table");
                assert(method_id < ctx->vm->trait_method_count &&
                       "RESOLVE_TCO_CALL method id out of range");
                const uint32_t width = ctx->vm->trait_table_width;
                const uint32_t dense = trait_dense_id(recv);
                uint16_t fn_id16 = TRAIT_METHOD_NONE;
                if (dense < width)
                    fn_id16 = ctx->vm->trait_table[static_cast<size_t>(method_id) * width + dense];
                if (fn_id16 == TRAIT_METHOD_NONE) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "no trait implementation for this type"); }
                const uint32_t fn_id = fn_id16;
                assert(fn_id < ctx->vm->fn_table_size && "RESOLVE_TCO_CALL fn id out of range");
                const FnInfo& fn = ctx->vm->fn_table[fn_id];
                assert(static_cast<uint8_t>(instr.r6.rd) == fn.arity &&
                       "RESOLVE_TCO_CALL arity mismatch");
                assert(fn.frame_size > 0 && "RESOLVE_TCO_CALL callee frame size must be > 0");
                // Reuse the current frame: no push, no slide (as TCO_CALL_INDIRECT). Only the GC scan
                // count and the current closure change; the eventual RET pops the original CALL's frame.
                // Clear what a SHRINKING reused frame abandons -- see TCO_CALL.
                for (uint32_t i = fn.frame_size; i < cfs; ++i) window[i] = Value();
                cfs = fn.frame_size;
                ctx->vm->current_closure = Value{};        // resolved trait method = plain Func
                ip  = code_base + (static_cast<intptr_t>(fn.code_offset) << 2);
                continue;
            }

            case OpCode::RET: {
                assert(rsp > ctx->ret_stack_base && "Return stack underflow");
                // Clear the dying frame -- the first of the three mechanisms that keep the
                // frame_size GC-roots contract true (see MOV_TAKE in the opcode table). Every
                // slot here is about to sit ABOVE the caller's frame_size, where no collection
                // will ever forward it again; leaving a heap pointer behind means a later frame
                // at this same window base with a bigger frame_size scans a dangling root.
                // r0 is skipped because it IS the return value -- the caller takes it with
                // MOV_TAKE, which empties the slot, so it does not stay dirty either.
                for (uint32_t i = 1; i < cfs; ++i) window[i] = Value();
                --rsp; --fsp; --csp;
                cfs    = *fsp;
                ctx->vm->current_closure = *csp; // restore caller closure (lockstep)
                ip     = rsp->old_ip;
                window = rsp->old_window;
                continue;
            }

            case OpCode::TCO_CALL: {
                ip += 4;
                const uint8_t new_cfs = static_cast<uint8_t>(instr.call.window_size);
                // A tail call abandons this frame in place. If the callee's frame is SMALLER,
                // the slots between the two sizes drop out of the scanned region with whatever
                // heap pointers they hold -- the same dead-frame leftover RET clears, except no
                // RET runs here. Clear them (usually a zero-length loop). The third of the three
                // mechanisms; see MOV_TAKE in the opcode table.
                for (uint32_t i = new_cfs; i < cfs; ++i) window[i] = Value();
                cfs = new_cfs;
                // Tail call reuses the current frame (no return/closure-stack push), and
                // a static TCO_CALL targets a NAMED function, so the reused activation
                // runs with no current closure. (A self-tail-call from a named fn already
                // had Undefined here -- a no-op; closure tail calls go through
                // TCO_CALL_INDIRECT below.)
                ctx->vm->current_closure = Value{};
                ip += (static_cast<intptr_t>(instr.call.offset) << 2);
                continue;
            }

            case OpCode::TCO_CALL_INDIRECT: {
                ip += 4;
                // Fusion of TCO_CALL (reuse the current frame in place -- no return/
                // closure-stack push, no window slide) and CALL_INDIRECT (recover the
                // callee from the function VALUE in register ra, branching on its tag).
                // The compiler guarantees ra sits ABOVE the argument window [0, nargs)
                // (a high temp) and that nargs <= the caller frame size, so the argument
                // MOVs into [0, nargs) neither clobber the callee register nor each other.
                const Value callee = window[instr.r6.ra];
                uint32_t    fn_id;
                Value       new_closure;                 // Undefined for the Func-immediate case
                if (callee.isFunc()) {
                    fn_id = callee.asFuncId();
                } else {
                    assert(callee.isPtr() && "TCO_CALL_INDIRECT on a non-function value");
                    GcObject* clo = GcObject::from_slots(callee.asPtr());
                    assert(clo->kind == GcObject::KIND_CLOSURE &&
                           "TCO_CALL_INDIRECT on a non-closure heap pointer");
                    fn_id       = clo->closure_fn_id();
                    new_closure = callee;                // install as the callee's current closure
                }
                assert(fn_id < ctx->vm->fn_table_size && "TCO_CALL_INDIRECT fn id out of range");
                const FnInfo& fn = ctx->vm->fn_table[fn_id];
                assert(static_cast<uint8_t>(instr.r6.rb) == fn.arity &&
                       "TCO_CALL_INDIRECT arity mismatch");
                assert(fn.frame_size > 0 && "TCO_CALL_INDIRECT callee frame size must be > 0");
                // Reuse the current frame: no push, no slide. Only the GC scan count and
                // the current closure change; the return/closure stacks are untouched, so
                // the eventual RET pops the frame the original CALL pushed.
                // Clear what a SHRINKING reused frame abandons -- see TCO_CALL.
                for (uint32_t i = fn.frame_size; i < cfs; ++i) window[i] = Value();
                cfs = fn.frame_size;
                ctx->vm->current_closure = new_closure;  // the callee closure, or Undefined
                ip  = code_base + (static_cast<intptr_t>(fn.code_offset) << 2);
                continue;
            }

            // ---- constant / register moves ----
            case OpCode::LOAD_CONST:
                window[instr.c2.rd] = Value::fromSigned48(static_cast<int64_t>(instr.c2.cnst));
                ip += 4; continue;

            case OpCode::LOAD_CONST_WIDE: {
                const unsigned rd    = instr.wide.rd;
                const uint64_t low16 = static_cast<uint64_t>(instr.wide.payload);
                const int64_t  cur   = window[rd].asSigned48();
                window[rd] = Value::fromSigned48((cur << 16) | static_cast<int64_t>(low16));
                ip += 4; continue;
            }

            case OpCode::MOV:
                window[instr.r6.rd] = window[instr.r6.ra];
                ip += 4; continue;

            // ---- typed int arithmetic (48-bit, wrapping) ----
            // (assertInt on the operands is Debug-only; it vanishes in Release.)
            case OpCode::ADD_INT:
                window[instr.r6.ra].assertInt(); window[instr.r6.rb].assertInt();
                window[instr.r6.rd] = Value::fromSigned48(
                    window[instr.r6.ra].asSigned48() + window[instr.r6.rb].asSigned48());
                ip += 4; continue;
            case OpCode::SUB_INT:
                window[instr.r6.ra].assertInt(); window[instr.r6.rb].assertInt();
                window[instr.r6.rd] = Value::fromSigned48(
                    window[instr.r6.ra].asSigned48() - window[instr.r6.rb].asSigned48());
                ip += 4; continue;
            case OpCode::MUL_INT:
                window[instr.r6.ra].assertInt(); window[instr.r6.rb].assertInt();
                window[instr.r6.rd] = Value::fromSigned48(
                    window[instr.r6.ra].asSigned48() * window[instr.r6.rb].asSigned48());
                ip += 4; continue;
            case OpCode::INCR:
                window[instr.c2.rd].assertInt();
                window[instr.c2.rd] = Value::fromSigned48(window[instr.c2.rd].asSigned48() + 1LL);
                ip += 4; continue;
            case OpCode::DECR:
                window[instr.c2.rd].assertInt();
                window[instr.c2.rd] = Value::fromSigned48(window[instr.c2.rd].asSigned48() - 1LL);
                ip += 4; continue;
            case OpCode::DIV_INT: {
                window[instr.r6.ra].assertInt(); window[instr.r6.rb].assertInt();
                const int64_t va = window[instr.r6.ra].asSigned48();
                const int64_t vb = window[instr.r6.rb].asSigned48();
                if (vb == 0) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "division by zero"); }
                window[instr.r6.rd] = Value::fromSigned48(va / vb);
                ip += 4; continue;
            }
            case OpCode::MOD_INT: {
                window[instr.r6.ra].assertInt(); window[instr.r6.rb].assertInt();
                const int64_t va = window[instr.r6.ra].asSigned48();
                const int64_t vb = window[instr.r6.rb].asSigned48();
                if (vb == 0) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "division by zero"); }
                window[instr.r6.rd] = Value::fromSigned48(va % vb);
                ip += 4; continue;
            }
            case OpCode::NEG_INT:
                window[instr.r6.ra].assertInt();
                window[instr.r6.rd] = Value::fromSigned48(-window[instr.r6.ra].asSigned48());
                ip += 4; continue;

            // ---- Int -> Double widening (the static front end's N1 boundary coercion) ----
            case OpCode::I2D:
                window[instr.r6.ra].assertInt();
                window[instr.r6.rd] = Value::fromDouble(
                    static_cast<double>(window[instr.r6.ra].asSigned48()));
                ip += 4; continue;

            // ---- Double -> Int saturating conversion (the `toInt` builtin) ----
            // Java semantics (committed): NaN -> 0; +inf / overflow -> MAX_48;
            // -inf / underflow -> MIN_48; else truncate toward zero. Total, never
            // traps. MAX_48 = 2^47-1 and MIN_48 = -2^47 are exactly representable as
            // double, so the strict >/< bounds are correct (a value in (MAX_48, 2^47)
            // truncates to MAX_48 anyway; the `else` cast is provably in range).
            case OpCode::D2I: {
                window[instr.r6.ra].assertNumeric();
                const double d = window[instr.r6.ra].numAsDouble();
                constexpr int64_t MAX_48 =  140737488355327LL;   //  2^47 - 1
                constexpr int64_t MIN_48 = -140737488355328LL;   // -2^47
                int64_t r;
                if (d != d)                                r = 0;        // NaN
                else if (d > static_cast<double>(MAX_48))  r = MAX_48;   // +inf / overflow
                else if (d < static_cast<double>(MIN_48))  r = MIN_48;   // -inf / underflow
                else                                       r = static_cast<int64_t>(d);  // trunc toward zero
                window[instr.r6.rd] = Value::fromSigned48(r);
                ip += 4; continue;
            }

            // ---- Double rounding (floor/ceil/trunc/round/roundHalfToEven builtins) ----
            // MODE in the flags field selects the rounding rule; all Double -> Double, total,
            // no alloc/trap. -0.0 is normalized to 0.0 by Value::fromDouble (committed rule).
            case OpCode::DROUND: {
                window[instr.r6.ra].assertNumeric();
                const double d = window[instr.r6.ra].numAsDouble();
                double r;
                switch (instr.r6.flags) {
                    case 0:  r = std::floor(d);     break;   // toward -inf
                    case 1:  r = std::ceil(d);      break;   // toward +inf
                    case 2:  r = std::trunc(d);     break;   // toward zero
                    case 3:  r = std::round(d);     break;   // ties away from zero
                    default: r = std::nearbyint(d); break;   // 4: ties to even (default FE_TONEAREST)
                }
                window[instr.r6.rd] = Value::fromDouble(r);
                ip += 4; continue;
            }

            // ---- bitwise / shifts (48-bit) ----
            case OpCode::AND_INT:
                window[instr.r6.ra].assertInt(); window[instr.r6.rb].assertInt();
                window[instr.r6.rd] = Value::fromSigned48(
                    window[instr.r6.ra].asSigned48() & window[instr.r6.rb].asSigned48());
                ip += 4; continue;
            case OpCode::OR_INT:
                window[instr.r6.ra].assertInt(); window[instr.r6.rb].assertInt();
                window[instr.r6.rd] = Value::fromSigned48(
                    window[instr.r6.ra].asSigned48() | window[instr.r6.rb].asSigned48());
                ip += 4; continue;
            case OpCode::XOR_INT:
                window[instr.r6.ra].assertInt(); window[instr.r6.rb].assertInt();
                window[instr.r6.rd] = Value::fromSigned48(
                    window[instr.r6.ra].asSigned48() ^ window[instr.r6.rb].asSigned48());
                ip += 4; continue;
            case OpCode::SHL_INT:
                window[instr.r6.ra].assertInt(); window[instr.r6.rb].assertInt();
                window[instr.r6.rd] = Value::fromSigned48(
                    window[instr.r6.ra].asSigned48() << (window[instr.r6.rb].asSigned48() & 63));
                ip += 4; continue;
            case OpCode::SHR_INT:
                window[instr.r6.ra].assertInt(); window[instr.r6.rb].assertInt();
                window[instr.r6.rd] = Value::fromSigned48(
                    window[instr.r6.ra].asSigned48() >> (window[instr.r6.rb].asSigned48() & 63));
                ip += 4; continue;
            case OpCode::USHR_INT: {
                window[instr.r6.ra].assertInt(); window[instr.r6.rb].assertInt();
                const uint64_t va = window[instr.r6.ra].asRaw48();
                const int64_t  vb = window[instr.r6.rb].asSigned48();
                window[instr.r6.rd] = Value::fromRaw48(va >> (vb & 63));
                ip += 4; continue;
            }

            // ---- generic (promoting) arithmetic ----
            case OpCode::ADD: {
                const Value a = window[instr.r6.ra];
                const Value b = window[instr.r6.rb];
                if (a.isInt() && b.isInt()) {
                    window[instr.r6.rd] = Value::fromSigned48(a.asSigned48() + b.asSigned48());
                } else if (is_string(a) || is_string(b)) [[unlikely]] {
                    // string+string, string+number or number+string: string_add
                    // coerces a numeric operand to text (format_number) and concats.
                    SYNC_TO_CTX();                       // string_add allocates -> safepoint
                    window[instr.r6.rd] = string_add(a, b, ctx);
                } else {
                    a.assertNumeric(); b.assertNumeric(); // rules out Ptr and all non-numerics
                    window[instr.r6.rd] = Value::fromDouble(a.numAsDouble() + b.numAsDouble());
                }
                ip += 4; continue;
            }
            case OpCode::SUB:
                window[instr.r6.rd] = num_sub(window[instr.r6.ra], window[instr.r6.rb]);
                ip += 4; continue;
            case OpCode::MUL:
                window[instr.r6.rd] = num_mul(window[instr.r6.ra], window[instr.r6.rb]);
                ip += 4; continue;
            case OpCode::DIV: {
                // Pre-check the int/int zero divisor here (not inside num_div) so the
                // abort is LOCATED via raise_located; a double operand takes num_div's
                // IEEE path and never traps. num_div keeps its own guard as a net.
                const Value da = window[instr.r6.ra], db = window[instr.r6.rb];
                if (da.isInt() && db.isInt() && db.asSigned48() == 0) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "division by zero"); }
                window[instr.r6.rd] = num_div(da, db);
                ip += 4; continue;
            }
            case OpCode::MOD: {
                const Value ma = window[instr.r6.ra], mb = window[instr.r6.rb];
                if (ma.isInt() && mb.isInt() && mb.asSigned48() == 0) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "division by zero"); }
                window[instr.r6.rd] = num_mod(ma, mb);
                ip += 4; continue;
            }
            case OpCode::NEG:
                window[instr.r6.rd] = num_neg(window[instr.r6.ra]);
                ip += 4; continue;

            // ---- typed int compares -> Bool ----
            case OpCode::SET_EQ:
                window[instr.r6.ra].assertInt(); window[instr.r6.rb].assertInt();
                window[instr.r6.rd] = Value::fromBool(
                    window[instr.r6.ra].asSigned48() == window[instr.r6.rb].asSigned48());
                ip += 4; continue;
            case OpCode::SET_LT:
                window[instr.r6.ra].assertInt(); window[instr.r6.rb].assertInt();
                window[instr.r6.rd] = Value::fromBool(
                    window[instr.r6.ra].asSigned48() < window[instr.r6.rb].asSigned48());
                ip += 4; continue;
            case OpCode::SET_LE:
                window[instr.r6.ra].assertInt(); window[instr.r6.rb].assertInt();
                window[instr.r6.rd] = Value::fromBool(
                    window[instr.r6.ra].asSigned48() <= window[instr.r6.rb].asSigned48());
                ip += 4; continue;

            // ---- bool logic (eager fast path) ----
            case OpCode::AND_BOOL:
                window[instr.r6.rd] = Value::fromBool(
                    window[instr.r6.ra].asBool() && window[instr.r6.rb].asBool());
                ip += 4; continue;
            case OpCode::OR_BOOL:
                window[instr.r6.rd] = Value::fromBool(
                    window[instr.r6.ra].asBool() || window[instr.r6.rb].asBool());
                ip += 4; continue;
            case OpCode::NOT_BOOL:
                window[instr.r6.rd] = Value::fromBool(!window[instr.r6.ra].asBool());
                ip += 4; continue;

            // ---- generic (promoting) numeric compares -> Bool (IEEE) ----
            case OpCode::SET_EQ_NUM:
                window[instr.r6.ra].assertNumeric(); window[instr.r6.rb].assertNumeric();
                window[instr.r6.rd] = Value::fromBool(
                    window[instr.r6.ra].numAsDouble() == window[instr.r6.rb].numAsDouble());
                ip += 4; continue;
            case OpCode::SET_NE_NUM:
                window[instr.r6.ra].assertNumeric(); window[instr.r6.rb].assertNumeric();
                window[instr.r6.rd] = Value::fromBool(
                    window[instr.r6.ra].numAsDouble() != window[instr.r6.rb].numAsDouble());
                ip += 4; continue;
            case OpCode::SET_LT_NUM:
                window[instr.r6.ra].assertNumeric(); window[instr.r6.rb].assertNumeric();
                window[instr.r6.rd] = Value::fromBool(
                    window[instr.r6.ra].numAsDouble() < window[instr.r6.rb].numAsDouble());
                ip += 4; continue;
            case OpCode::SET_LE_NUM:
                window[instr.r6.ra].assertNumeric(); window[instr.r6.rb].assertNumeric();
                window[instr.r6.rd] = Value::fromBool(
                    window[instr.r6.ra].numAsDouble() <= window[instr.r6.rb].numAsDouble());
                ip += 4; continue;
            case OpCode::SET_GT_NUM:
                window[instr.r6.ra].assertNumeric(); window[instr.r6.rb].assertNumeric();
                window[instr.r6.rd] = Value::fromBool(
                    window[instr.r6.ra].numAsDouble() > window[instr.r6.rb].numAsDouble());
                ip += 4; continue;
            case OpCode::SET_GE_NUM:
                window[instr.r6.ra].assertNumeric(); window[instr.r6.rb].assertNumeric();
                window[instr.r6.rd] = Value::fromBool(
                    window[instr.r6.ra].numAsDouble() >= window[instr.r6.rb].numAsDouble());
                ip += 4; continue;

            // ---- general Value equality (string CONTENT aware) ----
            case OpCode::EQ: {
                const Value a = window[instr.r6.ra];
                const Value b = window[instr.r6.rb];
                bool r = (a == b);
                if (!r && a.isPtr() && b.isPtr() && is_string(a) && is_string(b))
                    r = str_eq(a, b);
                window[instr.r6.rd] = Value::fromBool(r);
                ip += 4; continue;
            }
            case OpCode::NE: {
                const Value a = window[instr.r6.ra];
                const Value b = window[instr.r6.rb];
                bool eq = (a == b);
                if (!eq && a.isPtr() && b.isPtr() && is_string(a) && is_string(b))
                    eq = str_eq(a, b);
                window[instr.r6.rd] = Value::fromBool(!eq);
                ip += 4; continue;
            }

            // ---- structural (deep) equality: the surface `==` on a composite ----
            // value_deep_eq can THROW (raise_located) on a cyclic value or a closure
            // operand, so SYNC first (like the other faulting ops) -- ctx->ip and the
            // return-stack cursor must be current for the located fault. NOT a safepoint
            // (the compare allocates nothing), so no reload afterward.
            case OpCode::EQ_DEEP: {
                const Value a = window[instr.r6.ra];
                const Value b = window[instr.r6.rb];
                SYNC_TO_CTX();
                window[instr.r6.rd] = Value::fromBool(value_deep_eq(a, b, ctx));
                ip += 4; continue;
            }

            // ---- generic ordering (Int / string content / numeric IEEE) ----
            case OpCode::LT: {
                const Value a = window[instr.r6.ra];
                const Value b = window[instr.r6.rb];
                bool r;
                if (a.isInt() && b.isInt()) {
                    r = a.asSigned48() < b.asSigned48();
                } else if (a.isPtr() && b.isPtr() && is_string(a) && is_string(b)) [[unlikely]] {
                    r = str_cmp(a, b) < 0;
                } else {
                    assert(!a.isPtr() && !b.isPtr() &&
                        "LT on a mixed string/number operand -- ordering across those types is undefined");
                    r = a.numAsDouble() < b.numAsDouble();
                }
                window[instr.r6.rd] = Value::fromBool(r);
                ip += 4; continue;
            }
            case OpCode::LE: {
                const Value a = window[instr.r6.ra];
                const Value b = window[instr.r6.rb];
                bool r;
                if (a.isInt() && b.isInt()) {
                    r = a.asSigned48() <= b.asSigned48();
                } else if (a.isPtr() && b.isPtr() && is_string(a) && is_string(b)) [[unlikely]] {
                    r = str_cmp(a, b) <= 0;
                } else {
                    assert(!a.isPtr() && !b.isPtr() &&
                        "LE on a mixed string/number operand -- ordering across those types is undefined");
                    r = a.numAsDouble() <= b.numAsDouble();
                }
                window[instr.r6.rd] = Value::fromBool(r);
                ip += 4; continue;
            }

            // ---- type predicates -> Bool ----
            case OpCode::IS_INT:
                window[instr.r6.rd] = Value::fromBool(window[instr.r6.ra].isInt());
                ip += 4; continue;
            case OpCode::IS_DOUBLE:
                window[instr.r6.rd] = Value::fromBool(window[instr.r6.ra].isDouble());
                ip += 4; continue;
            case OpCode::IS_BOOL:
                window[instr.r6.rd] = Value::fromBool(window[instr.r6.ra].isBool());
                ip += 4; continue;
            case OpCode::IS_NIL:
                window[instr.r6.rd] = Value::fromBool(window[instr.r6.ra].isNil());
                ip += 4; continue;
            case OpCode::IS_UNDEF:
                window[instr.r6.rd] = Value::fromBool(window[instr.r6.ra].isUndefined());
                ip += 4; continue;
            case OpCode::TO_BOOL:
                window[instr.r6.rd] = Value::fromBool(window[instr.r6.ra].isTruthy());
                ip += 4; continue;
            case OpCode::LNOT:
                window[instr.r6.rd] = Value::fromBool(window[instr.r6.ra].isFalsy());
                ip += 4; continue;

            // Move that EMPTIES the source. Used for the return-value fetch after a call:
            // that slot sits above the caller's frame, so a copy left behind would never be
            // forwarded again and would go stale. See the opcode table.
            case OpCode::MOV_TAKE:
                window[instr.r6.rd] = window[instr.r6.ra];
                window[instr.r6.ra] = Value();
                ip += 4; continue;
            case OpCode::TO_STRING: {
                // Generic to-string coercion. Atoms/bools/nil resolve to pre-interned
                // pool strings (no alloc), but the Int/Double branch allocates a fresh
                // string -> SYNC before it so the collector sees current roots.
                const Value a = window[instr.r6.ra];
                SYNC_TO_CTX();
                window[instr.r6.rd] = to_string_value(a, ctx);
                ip += 4; continue;
            }

            // ---- output (PRINT / PRINTLN) ----
            // A pure sink: ra is a KIND_STRING (the compiler emits TO_STRING first),
            // written to vm->out. No allocation -> no GC safepoint / SYNC needed. rd is
            // set to Nil so `print(x)` / `println(x)` are expressions yielding nil. rd
            // may alias ra (the compiler reuses one temp): the string is read before rd
            // is overwritten, so the aliasing is safe.
            case OpCode::PRINT: {
                const Value s = window[instr.r6.ra];
                assert(s.isPtr() && "PRINT operand must be a heap string");
                const GcObject* o = GcObject::from_slots(s.asPtr());
                assert(o->kind == GcObject::KIND_STRING && "PRINT operand must be KIND_STRING");
                assert(ctx->vm->out && "PRINT requires an output sink (vm->out)");
                ctx->vm->out->write(o->bytes(), o->string_length());
                window[instr.r6.rd] = Value::fromNil();
                ip += 4; continue;
            }
            case OpCode::PRINTLN: {
                const Value s = window[instr.r6.ra];
                assert(s.isPtr() && "PRINTLN operand must be a heap string");
                const GcObject* o = GcObject::from_slots(s.asPtr());
                assert(o->kind == GcObject::KIND_STRING && "PRINTLN operand must be KIND_STRING");
                assert(ctx->vm->out && "PRINTLN requires an output sink (vm->out)");
                ctx->vm->out->write(o->bytes(), o->string_length());
                ctx->vm->out->put('\n');
                window[instr.r6.rd] = Value::fromNil();
                ip += 4; continue;
            }

            // ---- panic (surface `panic(msg)`) ----
            // Abort with a LOCATED fault carrying the user's message. ra is a KIND_STRING
            // (the compiler emits TO_STRING first). SYNC first so raise_located's ip /
            // stacktrace are current (PANIC is not a safepoint), then throw -- it never
            // returns, so there is no ip advance. A KIND_STRING payload is NUL-terminated,
            // so bytes() is a valid C string for raise_located's `what`.
            case OpCode::PANIC: {
                const Value s = window[instr.r6.ra];
                assert(s.isPtr() && "PANIC operand must be a heap string");
                const GcObject* o = GcObject::from_slots(s.asPtr());
                assert(o->kind == GcObject::KIND_STRING && "PANIC operand must be KIND_STRING");
                SYNC_TO_CTX();
                raise_located(ctx, o->bytes());
            }

            // ---- conditional move ----
            case OpCode::CMOV:
                window[instr.q4.rd] = window[instr.q4.rc].asBool()
                    ? window[instr.q4.ra]
                    : window[instr.q4.rb];
                ip += 4; continue;

            // ---- typed int compare-and-branch ----
            case OpCode::BEQ_INT: {
                window[instr.b.ra].assertInt(); window[instr.b.rb].assertInt();
                const int64_t va = window[instr.b.ra].asSigned48();
                const int64_t vb = window[instr.b.rb].asSigned48();
                ip += 4;
                if (va == vb) ip += (static_cast<intptr_t>(instr.b.offset) << 2);
                continue;
            }
            case OpCode::BNE_INT: {
                window[instr.b.ra].assertInt(); window[instr.b.rb].assertInt();
                const int64_t va = window[instr.b.ra].asSigned48();
                const int64_t vb = window[instr.b.rb].asSigned48();
                ip += 4;
                if (va != vb) ip += (static_cast<intptr_t>(instr.b.offset) << 2);
                continue;
            }
            case OpCode::BLT_INT: {
                window[instr.b.ra].assertInt(); window[instr.b.rb].assertInt();
                const int64_t va = window[instr.b.ra].asSigned48();
                const int64_t vb = window[instr.b.rb].asSigned48();
                ip += 4;
                if (va < vb) ip += (static_cast<intptr_t>(instr.b.offset) << 2);
                continue;
            }
            case OpCode::BGE_INT: {
                window[instr.b.ra].assertInt(); window[instr.b.rb].assertInt();
                const int64_t va = window[instr.b.ra].asSigned48();
                const int64_t vb = window[instr.b.rb].asSigned48();
                ip += 4;
                if (va >= vb) ip += (static_cast<intptr_t>(instr.b.offset) << 2);
                continue;
            }

            // ---- generic (promoting) numeric compare-and-branch (IEEE) ----
            case OpCode::BEQ_NUM: {
                window[instr.b.ra].assertNumeric(); window[instr.b.rb].assertNumeric();
                const double da = window[instr.b.ra].numAsDouble();
                const double db = window[instr.b.rb].numAsDouble();
                ip += 4;
                if (da == db) ip += (static_cast<intptr_t>(instr.b.offset) << 2);
                continue;
            }
            case OpCode::BNE_NUM: {
                window[instr.b.ra].assertNumeric(); window[instr.b.rb].assertNumeric();
                const double da = window[instr.b.ra].numAsDouble();
                const double db = window[instr.b.rb].numAsDouble();
                ip += 4;
                if (da != db) ip += (static_cast<intptr_t>(instr.b.offset) << 2);
                continue;
            }
            case OpCode::BLT_NUM: {
                window[instr.b.ra].assertNumeric(); window[instr.b.rb].assertNumeric();
                const double da = window[instr.b.ra].numAsDouble();
                const double db = window[instr.b.rb].numAsDouble();
                ip += 4;
                if (da < db) ip += (static_cast<intptr_t>(instr.b.offset) << 2);
                continue;
            }
            case OpCode::BLE_NUM: {
                window[instr.b.ra].assertNumeric(); window[instr.b.rb].assertNumeric();
                const double da = window[instr.b.ra].numAsDouble();
                const double db = window[instr.b.rb].numAsDouble();
                ip += 4;
                if (da <= db) ip += (static_cast<intptr_t>(instr.b.offset) << 2);
                continue;
            }
            case OpCode::BGT_NUM: {
                window[instr.b.ra].assertNumeric(); window[instr.b.rb].assertNumeric();
                const double da = window[instr.b.ra].numAsDouble();
                const double db = window[instr.b.rb].numAsDouble();
                ip += 4;
                if (da > db) ip += (static_cast<intptr_t>(instr.b.offset) << 2);
                continue;
            }
            case OpCode::BGE_NUM: {
                window[instr.b.ra].assertNumeric(); window[instr.b.rb].assertNumeric();
                const double da = window[instr.b.ra].numAsDouble();
                const double db = window[instr.b.rb].numAsDouble();
                ip += 4;
                if (da >= db) ip += (static_cast<intptr_t>(instr.b.offset) << 2);
                continue;
            }

            // ---- branch on bool ----
            case OpCode::BT: {
                const bool va = window[instr.b1.ra].asBool();
                ip += 4;
                if (va) ip += (static_cast<intptr_t>(instr.b1.offset) << 2);
                continue;
            }
            case OpCode::BF: {
                const bool va = window[instr.b1.ra].asBool();
                ip += 4;
                if (!va) ip += (static_cast<intptr_t>(instr.b1.offset) << 2);
                continue;
            }

            // ---- globals ----
            case OpCode::LOAD_GLOBAL: {
                assert(ctx->vm->globals && "LOAD_GLOBAL requires active GlobalEnv");
                const uint8_t index = static_cast<uint8_t>(instr.c2.cnst & 0xFF);
                window[instr.c2.rd] = ctx->vm->globals->get(index);
                ip += 4; continue;
            }
            case OpCode::STORE_GLOBAL: {
                assert(ctx->vm->globals && "STORE_GLOBAL requires active GlobalEnv");
                const uint8_t index = static_cast<uint8_t>(instr.c2.cnst & 0xFF);
                ctx->vm->globals->set(index, window[instr.c2.rd]);
                ip += 4; continue;
            }

            // ---- constant / string pools ----
            case OpCode::LOAD_CONST_POOL: {
                assert(ctx->vm->const_pool && "LOAD_CONST_POOL requires an active constant pool");
                const uint16_t index = static_cast<uint16_t>(instr.c2.cnst);
                assert(index < ctx->vm->const_pool_size && "LOAD_CONST_POOL index out of range");
                window[instr.c2.rd] = ctx->vm->const_pool[index];
                ip += 4; continue;
            }
            case OpCode::LOAD_STR: {
                assert(ctx->vm->string_pool && "LOAD_STR requires an active string pool");
                const uint16_t index = static_cast<uint16_t>(instr.c2.cnst);
                assert(index < ctx->vm->string_pool_size && "LOAD_STR index out of range");
                window[instr.c2.rd] = ctx->vm->string_pool[index];
                ip += 4; continue;
            }
            case OpCode::LOAD_CONST_ARRAY: {
                // Pure rooted-pool read (the const arrays are pre-built at execute() setup),
                // so -- unlike ALLOC/ANEW -- this does NOT allocate and needs no safepoint.
                assert(ctx->vm->const_array_pool && "LOAD_CONST_ARRAY requires an active const-array pool");
                const uint16_t index = static_cast<uint16_t>(instr.c2.cnst);
                assert(index < ctx->vm->const_array_pool_size && "LOAD_CONST_ARRAY index out of range");
                window[instr.c2.rd] = ctx->vm->const_array_pool[index];
                ip += 4; continue;
            }

            // ---- first-class functions ----
            case OpCode::LOAD_FN: {
                // Materialize a first-class function value: a GC-invisible Func
                // immediate carrying the function-table id. CALL_INDIRECT later
                // indexes ctx->vm->fn_table with this id to recover the callee's
                // entry point and frame size; here we only tag the id into a Value.
                const uint16_t fn_id = static_cast<uint16_t>(instr.c2.cnst);
                assert(fn_id < ctx->vm->fn_table_size && "LOAD_FN id out of range");
                window[instr.c2.rd] = Value::fromFunc(fn_id);
                ip += 4; continue;
            }

            case OpCode::MAKE_CLOSURE: {
                // Build a capturing closure. Prop layout: rd = destination, ra = the
                // capture BASE register, 12-bit slot = fn id (capacity 4096 -- fine for
                // now; LOAD_FN's C2 fn id is the wider 16-bit path). ncaptures come from
                // the function table. Allocates -> safepoint.
                assert(ctx->vm->heap && "MAKE_CLOSURE requires ctx->vm->heap");
                const uint16_t fn_id        = static_cast<uint16_t>(instr.prop_slot());
                const uint8_t  capture_base = static_cast<uint8_t>(instr.r6.ra);
                SYNC_TO_CTX();                           // allocates -> safepoint
                window[instr.r6.rd] = switch_make_closure(ctx, fn_id, capture_base);
                ip += 4; continue;
            }

            case OpCode::LOAD_CAPTURE: {
                // rd = current_closure.captures[index]. Emitted only inside a closure body,
                // where current_closure is the KIND_CLOSURE the active CALL_INDIRECT
                // installed. Pure read (no allocation) -- current_closure is a live root, so
                // it is stable here; no SYNC/safepoint needed.
                const Value clo_val = ctx->vm->current_closure;
                assert(clo_val.isPtr() && "LOAD_CAPTURE outside a closure activation");
                GcObject* clo = GcObject::from_slots(clo_val.asPtr());
                assert(clo->kind == GcObject::KIND_CLOSURE &&
                       "LOAD_CAPTURE: current closure is not a KIND_CLOSURE");
                const uint16_t index = static_cast<uint16_t>(instr.c2.cnst);
                assert(index < clo->slot_count() && "LOAD_CAPTURE index out of range");
                window[instr.c2.rd] = clo->slots()[index];
                ip += 4; continue;
            }

            // ---- fixed-shape structs ----
            case OpCode::NEW_STRUCT: {
                assert(ctx->vm->heap && "NEW_STRUCT requires ctx->vm->heap");
                SYNC_TO_CTX();                           // allocates -> safepoint
                window[instr.c2.rd] = switch_new_struct(ctx, static_cast<uint16_t>(instr.c2.cnst));
                ip += 4; continue;
            }
            case OpCode::GET_PROP: {
                const Value obj_val = window[instr.r6.ra];
                REQUIRE_KIND(obj, obj_val, GcObject::KIND_OBJECT,
                             "field access on a non-struct value");
                const uint32_t slot = instr.prop_slot();
                assert(slot < obj->slot_count() && "GET_PROP slot index out of range");
                window[instr.r6.rd] = obj->slots()[slot];
                ip += 4; continue;
            }
            case OpCode::SET_PROP: {
                const Value obj_val = window[instr.r6.ra];
                REQUIRE_KIND(obj, obj_val, GcObject::KIND_OBJECT,
                             "field assignment on a non-struct value");
                const uint32_t slot = instr.prop_slot();
                assert(slot < obj->slot_count() && "SET_PROP slot index out of range");
                obj->slots()[slot] = window[instr.r6.rd];
                ip += 4; continue;
            }
            case OpCode::IS_OBJECT: {
                const Value v = window[instr.r6.ra];
                const bool is_obj = v.isPtr() &&
                    GcObject::from_slots(v.asPtr())->kind == GcObject::KIND_OBJECT;
                window[instr.r6.rd] = Value::fromBool(is_obj);
                ip += 4; continue;
            }
            case OpCode::GET_TYPE_ID: {
                const Value v = window[instr.r6.ra];
                int64_t id = -1;
                if (v.isPtr()) {
                    GcObject* obj = GcObject::from_slots(v.asPtr());
                    if (obj->kind == GcObject::KIND_OBJECT)
                        id = static_cast<int64_t>(obj->object_type_id());
                }
                window[instr.r6.rd] = Value::fromSigned48(id);
                ip += 4; continue;
            }
            case OpCode::GET_KIND: {
                // rd = Int(heap kind of ra), or Int(-1) if ra is not a heap pointer.
                // Mirrors GET_TYPE_ID, but reads the GcObject header `kind` byte
                // (ARRAY/STRING/OBJECT/CLOSURE/MAP/VEC/BYTES) instead of the struct type id --
                // the primitive the compiler's `for … in …` lowering dispatches on to
                // tell an array from a map (both non-structs, so GET_TYPE_ID cannot
                // distinguish them). No allocation.
                const Value v = window[instr.r6.ra];
                int64_t k = -1;
                if (v.isPtr())
                    k = static_cast<int64_t>(GcObject::from_slots(v.asPtr())->kind);
                window[instr.r6.rd] = Value::fromSigned48(k);
                ip += 4; continue;
            }

            // ---- trait (protocol) dispatch ----
            case OpCode::PROTO_RESOLVE: {
                // rd = the concrete method's function VALUE for (method_id, typeof(ra)).
                // Prop layout: rd = destination, ra = receiver, 12-bit slot = GLOBAL
                // method id. Folds the receiver's runtime type into one dense key
                // (trait_dense_id), indexes the flat trait table, and writes a Func
                // immediate so the following CALL_INDIRECT/TCO_CALL_INDIRECT dispatches
                // it exactly like any first-class function. A miss (no impl of the
                // method for that type) traps. No allocation.
                const Value    recv      = window[instr.r6.ra];
                const uint32_t method_id = instr.prop_slot();
                assert(ctx->vm->trait_table && "PROTO_RESOLVE requires a trait table");
                assert(method_id < ctx->vm->trait_method_count &&
                       "PROTO_RESOLVE method id out of range");
                const uint32_t width = ctx->vm->trait_table_width;
                const uint32_t dense = trait_dense_id(recv);
                uint16_t fn_id = TRAIT_METHOD_NONE;
                if (dense < width)
                    fn_id = ctx->vm->trait_table[static_cast<size_t>(method_id) * width + dense];
                if (fn_id == TRAIT_METHOD_NONE) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "no trait implementation for this type"); }
                window[instr.r6.rd] = Value::fromFunc(fn_id);
                ip += 4; continue;
            }

            // ---- array allocation ----
            case OpCode::ALLOC: {
                assert(ctx->vm->heap && "ALLOC requires ctx->vm->heap");
                SYNC_TO_CTX();                           // allocates -> safepoint
                window[instr.c2.rd] = switch_alloc_array(ctx, instr.c2.cnst);
                ip += 4; continue;
            }
            case OpCode::ANEW: {
                assert(ctx->vm->heap && "ANEW requires ctx->vm->heap");
                const int64_t count = window[instr.r6.ra].asSigned48();
                SYNC_TO_CTX();                           // allocates -> safepoint
                window[instr.r6.rd] = switch_anew(ctx, count);
                ip += 4; continue;
            }

            // ---- array/vector/bytes element access (bounds-checked; OOB -> located trap) ----
            // Tri-kind: a fixed KIND_ARRAY or a growable KIND_VEC (Value slots, via
            // indexable_view), or a KIND_BYTES (a byte reads/writes as an Int 0..255).
            // The array/vec hot path stays a single indexable_view() compare; the bytes
            // branch sits after it (off the array hot path).
            case OpCode::ARRAY_GET: {
                const Value coll = window[instr.r6.ra];
                Value* elems = nullptr; uint64_t len = 0;
                if (indexable_view(coll, elems, len)) {
                    const int64_t i = window[instr.r6.rb].asSigned48();
                    if (i < 0 || static_cast<uint64_t>(i) >= len) [[unlikely]]
                        { SYNC_TO_CTX(); raise_located(ctx, "array index out of bounds"); }
                    window[instr.r6.rd] = elems[i];
                    ip += 4; continue;
                }
                if (coll.isPtr()) {
                    GcObject* o = GcObject::from_slots(coll.asPtr());
                    if (o->kind == GcObject::KIND_BYTES) {
                        const int64_t count = o->slots()[BYTES_SLOT_COUNT].asSigned48();
                        const int64_t i = window[instr.r6.rb].asSigned48();
                        if (i < 0 || i >= count) [[unlikely]]
                            { SYNC_TO_CTX(); raise_located(ctx, "array index out of bounds"); }
                        GcObject* backing = GcObject::from_slots(o->slots()[BYTES_SLOT_BACKING].asPtr());
                        window[instr.r6.rd] = Value::fromSigned48(
                            static_cast<unsigned char>(backing->bytes()[i]));
                        ip += 4; continue;
                    }
                }
                SYNC_TO_CTX(); raise_located(ctx, "array indexing on a non-array value");
            }
            case OpCode::ARRAY_SET: {
                const Value coll = window[instr.r6.ra];
                Value* elems = nullptr; uint64_t len = 0;
                if (indexable_view(coll, elems, len)) {
                    const int64_t i = window[instr.r6.rb].asSigned48();
                    if (i < 0 || static_cast<uint64_t>(i) >= len) [[unlikely]]
                        { SYNC_TO_CTX(); raise_located(ctx, "array index out of bounds"); }
                    elems[i] = window[instr.r6.rd];          // rd = value (as SET_PROP)
                    ip += 4; continue;
                }
                if (coll.isPtr()) {
                    GcObject* o = GcObject::from_slots(coll.asPtr());
                    if (o->kind == GcObject::KIND_BYTES) {
                        const int64_t count = o->slots()[BYTES_SLOT_COUNT].asSigned48();
                        const int64_t i = window[instr.r6.rb].asSigned48();
                        if (i < 0 || i >= count) [[unlikely]]
                            { SYNC_TO_CTX(); raise_located(ctx, "array index out of bounds"); }
                        GcObject* backing = GcObject::from_slots(o->slots()[BYTES_SLOT_BACKING].asPtr());
                        backing->bytes()[i] = static_cast<char>(
                            window[instr.r6.rd].asSigned48() & 0xFF);   // rd = value, masked
                        ip += 4; continue;
                    }
                }
                SYNC_TO_CTX(); raise_located(ctx, "array assignment on a non-array value");
            }
            case OpCode::LEN: {
                // Quint-kind: an array's length is its slot count; a map's / vector's /
                // byte-buffer's length is its live-entry count (the count slot), NOT the
                // backing's slot count; a STRING's length is its BYTE count (string_length()
                // -- byte strings, no UTF-8 code-point counting).
                const Value obj_val = window[instr.r6.ra];
                if (!obj_val.isPtr()) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "len() of a non-array/non-map/non-vector value"); }
                GcObject* obj = GcObject::from_slots(obj_val.asPtr());
                int64_t len = 0;
                switch (obj->kind) {
                    case GcObject::KIND_ARRAY:  len = static_cast<int64_t>(obj->slot_count()); break;
                    case GcObject::KIND_MAP:    len = obj->slots()[MAP_SLOT_COUNT].asSigned48(); break;
                    case GcObject::KIND_VEC:    len = obj->slots()[VEC_SLOT_COUNT].asSigned48(); break;
                    case GcObject::KIND_BYTES:  len = obj->slots()[BYTES_SLOT_COUNT].asSigned48(); break;
                    case GcObject::KIND_STRING: len = static_cast<int64_t>(obj->string_length()); break;  // BYTE length (byte strings, no UTF-8)
                    default: [[unlikely]]
                        SYNC_TO_CTX(); raise_located(ctx, "len() of a non-array/non-map/non-vector value");
                }
                window[instr.r6.rd] = Value::fromSigned48(len);
                ip += 4; continue;
            }
            case OpCode::AFILL: {
                const Value arr_val = window[instr.r6.rd];   // rd = array
                REQUIRE_KIND(arr, arr_val, GcObject::KIND_ARRAY,
                             "array fill on a non-array value");
                const Value v = window[instr.r6.ra];         // ra = fill value
                const uint32_t n = arr->slot_count();
                Value* s = arr->slots();
                for (uint32_t k = 0; k < n; ++k) s[k] = v;
                ip += 4; continue;
            }

            // ---- dynamic maps (KIND_MAP; content/bit hashed, see op_map.h) ----
            case OpCode::MAP_NEW: {
                assert(ctx->vm->heap && "MAP_NEW requires ctx->vm->heap");
                SYNC_TO_CTX();                           // allocates header + backing -> safepoint
                map_new(ctx, &window[instr.r6.rd]);
                ip += 4; continue;
            }
            case OpCode::MAP_GET: {
                const Value map_val = window[instr.r6.ra];
                REQUIRE_KIND(map_obj, map_val, GcObject::KIND_MAP,
                             "map lookup on a non-map value");
                const Value key = window[instr.r6.rb];
                if (!map_key_valid(key)) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "invalid map key type"); }
                window[instr.r6.rd] = map_get(map_obj, key);   // Nil on miss; no allocation
                ip += 4; continue;
            }
            case OpCode::MAP_GET_OR_TRAP: {
                // The TOTAL sibling of MAP_GET: TRAP on a missing key (mirrors ARRAY_GET's OOB
                // trap) rather than returning Nil, so `m[k]` reads a value or aborts. No alloc.
                const Value map_val = window[instr.r6.ra];
                REQUIRE_KIND(map_obj, map_val, GcObject::KIND_MAP,
                             "map lookup on a non-map value");
                const Value key = window[instr.r6.rb];
                if (!map_key_valid(key)) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "invalid map key type"); }
                if (!map_has(map_obj, key)) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "map key not found"); }
                window[instr.r6.rd] = map_get(map_obj, key);   // present; no allocation
                ip += 4; continue;
            }
            case OpCode::MAP_SET: {
                assert(ctx->vm->heap && "MAP_SET requires ctx->vm->heap");
                const Value map_val = window[instr.r6.ra];
                CHECK_KIND(map_val, GcObject::KIND_MAP, "map assignment on a non-map value");
                const Value key = window[instr.r6.rb];
                if (!map_key_valid(key)) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "invalid map key type"); }
                SYNC_TO_CTX();                           // may grow (allocate) -> safepoint
                // rd carries the value (as ARRAY_SET/SET_PROP); ra = map, rb = key.
                map_set(ctx, &window[instr.r6.ra], &window[instr.r6.rb], &window[instr.r6.rd]);
                ip += 4; continue;
            }
            case OpCode::MAP_HAS: {
                const Value map_val = window[instr.r6.ra];
                REQUIRE_KIND(map_obj, map_val, GcObject::KIND_MAP,
                             "has() on a non-map value");
                const Value key = window[instr.r6.rb];
                if (!map_key_valid(key)) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "invalid map key type"); }
                window[instr.r6.rd] = Value::fromBool(map_has(map_obj, key));  // no allocation
                ip += 4; continue;
            }
            case OpCode::MAP_DELETE: {
                const Value map_val = window[instr.r6.ra];
                REQUIRE_KIND(map_obj, map_val, GcObject::KIND_MAP,
                             "delete() on a non-map value");
                const Value key = window[instr.r6.rb];
                if (!map_key_valid(key)) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "invalid map key type"); }
                window[instr.r6.rd] = Value::fromBool(map_delete(map_obj, key));  // tombstone; no alloc
                ip += 4; continue;
            }
            case OpCode::MAP_KEYS: {
                assert(ctx->vm->heap && "MAP_KEYS requires ctx->vm->heap");
                const Value map_val = window[instr.r6.ra];
                CHECK_KIND(map_val, GcObject::KIND_MAP, "keys() on a non-map value");
                SYNC_TO_CTX();                           // allocates the result array -> safepoint
                window[instr.r6.rd] = switch_map_collect(ctx, &window[instr.r6.ra], /*want_values=*/false);
                ip += 4; continue;
            }
            case OpCode::MAP_VALUES: {
                assert(ctx->vm->heap && "MAP_VALUES requires ctx->vm->heap");
                const Value map_val = window[instr.r6.ra];
                CHECK_KIND(map_val, GcObject::KIND_MAP, "values() on a non-map value");
                SYNC_TO_CTX();                           // allocates the result array -> safepoint
                window[instr.r6.rd] = switch_map_collect(ctx, &window[instr.r6.ra], /*want_values=*/true);
                ip += 4; continue;
            }
            case OpCode::MAP_ITER_NEXT: {
                // No-copy live map iteration: rd = the next live pair-index >= the cursor in rb,
                // scanning the backing and skipping empty/tombstone slots; -1 once exhausted.
                // Backing/cap are re-derived from the LIVE header every call (safe across a moving
                // GC and a mid-iteration grow -- the int cursor survives, a raw pointer would not).
                // No allocation -> not a safepoint. Mirrors switch_map_collect's walk (one entry).
                const Value map_val = window[instr.r6.ra];
                REQUIRE_KIND(map_obj, map_val, GcObject::KIND_MAP, "map iteration on a non-map value");
                GcObject*      backing = GcObject::from_slots(map_obj->slots()[MAP_SLOT_BACKING].asPtr());
                const int64_t  cap     = static_cast<int64_t>(backing->slot_count() / 2);
                const Value*   b       = backing->slots();
                int64_t        p       = window[instr.r6.rb].asSigned48();
                if (p < 0) p = 0;
                int64_t result = -1;
                for (; p < cap; ++p) {
                    const Value key = b[2 * p];
                    if (key.isUndefined() || key.isTombstone()) continue;
                    result = p; break;
                }
                window[instr.r6.rd] = Value::fromSigned48(result);
                ip += 4; continue;
            }
            case OpCode::MAP_KEY_AT: {
                // rd = the key at backing pair-index rb (paired with MAP_ITER_NEXT). No allocation.
                const Value map_val = window[instr.r6.ra];
                REQUIRE_KIND(map_obj, map_val, GcObject::KIND_MAP, "map key access on a non-map value");
                GcObject*      backing = GcObject::from_slots(map_obj->slots()[MAP_SLOT_BACKING].asPtr());
                const int64_t  cap     = static_cast<int64_t>(backing->slot_count() / 2);
                const int64_t  p       = window[instr.r6.rb].asSigned48();
                if (p < 0 || p >= cap) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "map iteration index out of range"); }
                window[instr.r6.rd] = backing->slots()[2 * p];
                ip += 4; continue;
            }
            case OpCode::MAP_VAL_AT: {
                // rd = the value at backing pair-index rb (the value sibling of MAP_KEY_AT). No alloc.
                const Value map_val = window[instr.r6.ra];
                REQUIRE_KIND(map_obj, map_val, GcObject::KIND_MAP, "map value access on a non-map value");
                GcObject*      backing = GcObject::from_slots(map_obj->slots()[MAP_SLOT_BACKING].asPtr());
                const int64_t  cap     = static_cast<int64_t>(backing->slot_count() / 2);
                const int64_t  p       = window[instr.r6.rb].asSigned48();
                if (p < 0 || p >= cap) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "map iteration index out of range"); }
                window[instr.r6.rd] = backing->slots()[2 * p + 1];
                ip += 4; continue;
            }

            // ---- growable vectors (KIND_VEC; header + backing, see op_vec.h) ----
            case OpCode::VEC_NEW: {
                assert(ctx->vm->heap && "VEC_NEW requires ctx->vm->heap");
                SYNC_TO_CTX();                           // allocates header + backing -> safepoint
                vec_new(ctx, &window[instr.r6.rd]);
                ip += 4; continue;
            }
            // Tri-kind: push/pop dispatch on the RECEIVER's header kind at run time
            // (the compiler cannot know statically whether `push(x, v)` targets a vec or
            // a byte buffer), pushing/popping a Value for KIND_VEC and a byte (Int 0..255)
            // for KIND_BYTES.
            case OpCode::VEC_PUSH: {
                assert(ctx->vm->heap && "VEC_PUSH requires ctx->vm->heap");
                const Value recv = window[instr.r6.rd];  // rd = receiver (stays the receiver)
                if (recv.isPtr()) {
                    GcObject* o = GcObject::from_slots(recv.asPtr());
                    if (o->kind == GcObject::KIND_VEC) {
                        SYNC_TO_CTX();                   // may grow (allocate) -> safepoint
                        vec_push(ctx, &window[instr.r6.rd], &window[instr.r6.ra]);
                        ip += 4; continue;
                    }
                    if (o->kind == GcObject::KIND_BYTES) {
                        const int64_t b = window[instr.r6.ra].asSigned48();
                        SYNC_TO_CTX();                   // may grow (allocate) -> safepoint
                        bytes_push(ctx, &window[instr.r6.rd], b);
                        ip += 4; continue;
                    }
                }
                SYNC_TO_CTX(); raise_located(ctx, "push() on a non-vector/non-bytes value");
            }
            case OpCode::VEC_POP: {
                const Value recv = window[instr.r6.ra];
                if (recv.isPtr()) {
                    GcObject* o = GcObject::from_slots(recv.asPtr());
                    if (o->kind == GcObject::KIND_VEC) {
                        window[instr.r6.rd] = vec_pop(o);    // Nil if empty; no allocation
                        ip += 4; continue;
                    }
                    if (o->kind == GcObject::KIND_BYTES) {
                        window[instr.r6.rd] = bytes_pop(o);  // Int 0..255, or Nil if empty
                        ip += 4; continue;
                    }
                }
                SYNC_TO_CTX(); raise_located(ctx, "pop() on a non-vector/non-bytes value");
            }
            case OpCode::VEC_NEW_CAP: {
                assert(ctx->vm->heap && "VEC_NEW_CAP requires ctx->vm->heap");
                const int64_t cap = window[instr.r6.ra].asSigned48();   // capacity hint
                if (cap < 0) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "vector capacity must be non-negative"); }
                SYNC_TO_CTX();                           // allocates header + backing -> safepoint
                vec_new_cap(ctx, &window[instr.r6.rd], static_cast<uint32_t>(cap));
                ip += 4; continue;
            }

            // ---- growable byte buffers (KIND_BYTES; header + KIND_STRING backing, see op_bytes.h) ----
            case OpCode::BYTES_NEW_CAP: {
                assert(ctx->vm->heap && "BYTES_NEW_CAP requires ctx->vm->heap");
                const int64_t cap = window[instr.r6.ra].asSigned48();   // capacity in bytes
                if (cap < 0) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "byte buffer capacity must be non-negative"); }
                SYNC_TO_CTX();                           // allocates header + backing -> safepoint
                bytes_new_cap(ctx, &window[instr.r6.rd], static_cast<uint32_t>(cap));
                ip += 4; continue;
            }
            case OpCode::BYTES_FROM_STR: {
                assert(ctx->vm->heap && "BYTES_FROM_STR requires ctx->vm->heap");
                const Value src_val = window[instr.r6.ra];
                REQUIRE_KIND(src_obj, src_val, GcObject::KIND_STRING,
                             "toBytes() of a non-string value");
                SYNC_TO_CTX();                           // allocates header + backing -> safepoint
                bytes_from_string_obj(ctx, &window[instr.r6.rd], src_obj);  // std::string out-of-line
                ip += 4; continue;
            }
            case OpCode::BYTES_TO_STR: {
                assert(ctx->vm->heap && "BYTES_TO_STR requires ctx->vm->heap");
                const Value buf_val = window[instr.r6.ra];
                CHECK_KIND(buf_val, GcObject::KIND_BYTES, "fromBytes() of a non-bytes value");
                SYNC_TO_CTX();                           // allocates a string -> safepoint
                window[instr.r6.rd] = bytes_to_string(ctx, &window[instr.r6.ra]);
                ip += 4; continue;
            }
            // Bulk byte-blit: append a whole source (rd = dst KIND_BYTES; ra = src KIND_STRING or
            // KIND_BYTES) in one grow + memcpy, replacing a per-byte push loop. rd stays the buffer.
            case OpCode::BYTES_APPEND: {
                assert(ctx->vm->heap && "BYTES_APPEND requires ctx->vm->heap");
                const Value dst_val = window[instr.r6.rd];
                const Value src_val = window[instr.r6.ra];
                CHECK_KIND(dst_val, GcObject::KIND_BYTES, "appendBytes() into a non-bytes value");
                if (!src_val.isPtr()) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "appendBytes() of a non-string/non-bytes value"); }
                GcObject* src_obj = GcObject::from_slots(src_val.asPtr());
                if (src_obj->kind != GcObject::KIND_STRING &&
                    src_obj->kind != GcObject::KIND_BYTES) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "appendBytes() of a non-string/non-bytes value"); }
                const uint32_t n = bytes_src_len(src_obj);
                SYNC_TO_CTX();                           // may grow (allocate) -> safepoint
                bytes_append_span(ctx, &window[instr.r6.rd], &window[instr.r6.ra], 0, n);
                ip += 4; continue;
            }
            // Bulk byte-blit of a sub-range (q4: rd = dst, ra = src KIND_BYTES, rb = lo, rc = hi).
            case OpCode::BYTES_APPEND_RANGE: {
                assert(ctx->vm->heap && "BYTES_APPEND_RANGE requires ctx->vm->heap");
                const Value dst_val = window[instr.q4.rd];
                const Value src_val = window[instr.q4.ra];
                CHECK_KIND(dst_val, GcObject::KIND_BYTES, "appendBytes range into a non-bytes value");
                REQUIRE_KIND(src_obj, src_val, GcObject::KIND_BYTES,
                             "appendBytes range of a non-bytes value");
                const int64_t lo   = window[instr.q4.rb].asSigned48();
                const int64_t hi   = window[instr.q4.rc].asSigned48();
                const int64_t slen = static_cast<int64_t>(bytes_src_len(src_obj));
                if (lo < 0 || hi < lo || hi > slen) [[unlikely]]
                    { SYNC_TO_CTX(); raise_located(ctx, "appendBytes range out of bounds"); }
                SYNC_TO_CTX();                           // may grow (allocate) -> safepoint
                bytes_append_span(ctx, &window[instr.q4.rd], &window[instr.q4.ra],
                                  static_cast<uint32_t>(lo), static_cast<uint32_t>(hi - lo));
                ip += 4; continue;
            }

            // ---- native call ----
            case OpCode::CALL_NATIVE: {
                // r6-encoded (Layout::CallNative): rd = result, ra = the register holding the
                // NativeId, rb = first argument, flags = nargs (a COUNT, not a register).
                const Value callee = window[instr.r6.ra];
                const auto  nargs  = static_cast<uint8_t>(instr.r6.flags);
                // Single-mode (the legacy baked-pointer path was retired earlier): the
                // operand register holds a NativeId INDEX into VM::native_table, which the
                // driver / test supplies via execute()'s native_table parameter.
                // See "Native functions" in docs/VirtualMachine.md and NativeRegistry.h.
                assert(ctx->vm->native_table && "CALL_NATIVE requires a native_table");
                const int64_t id = callee.asSigned48();
                assert(id >= 0 && static_cast<size_t>(id) < ctx->vm->native_table_size
                       && "CALL_NATIVE native id out of range");
                const NativeFunc func = ctx->vm->native_table[static_cast<size_t>(id)];
                SYNC_TO_CTX();                           // native may allocate / collect
                window[instr.r6.rd] = func(&window[instr.r6.rb], nargs, ctx);
                // Reload the hot cursors: a native is contractually not supposed to move
                // ip/window, but reloading keeps run_switch in lockstep with ctx exactly
                // as the tail-call handlers (which re-read ctx every dispatch) would.
                window = ctx->window_ptr;
                rsp    = ctx->ret_stack_ptr;
                fsp    = ctx->frame_size_ptr;
                cfs    = ctx->current_frame_size;
                ip += 4; continue;
            }

            default:
                // ILLEGAL -- raise SEH so run_switch's __except translates it, mirroring
                // op_illegal. RaiseException does not return for this code.
                ctx->ip = ip;
                std::cerr << "Illegal Opcode at " << static_cast<const void*>(ip) << "\n";
                RaiseException(EXCEPTION_ILLEGAL_INSTRUCTION, 0, 0, nullptr);
            }
        }
    }
    #undef SYNC_TO_CTX
    #undef SYNC_VERIFY_ROOTS
    #undef REQUIRE_KIND
    #undef CHECK_KIND
    #undef GROW_CHECK
}

// The SEH frame, and nothing else. It exists as a SEPARATE function from the dispatch
// loop because a `__try` forces every value that lives across the region to be
// memory-resident instead of register-allocated, and it blocks inlining of the
// SKARN_FORCEINLINE helpers. Wrapping the loop directly makes the interpreter
// several times slower, and it silently undermines the register-residency that is the
// whole reason vmcore dispatches with a while{switch} in the first place. SEH is dynamic
// and stack-based, so a hardware exception raised inside run_switch_loop still unwinds to
// the __try here -- the fault model is unchanged. See "Dispatch" in docs/VirtualMachine.md.
//
// Only the truly-fatal, non-locatable SEH faults reach the filter: a guard-page stack
// overflow (ACCESS_VIOLATION), an illegal opcode, and heap exhaustion. The located,
// resumable-category faults (wrong receiver, division by zero, array OOB, invalid map
// key, no trait impl) are raised as C++ throws via raise_located and propagate straight
// through this frame (CONTINUE_SEARCH for the C++ EH code). The INT_DIVIDE_BY_ZERO entry
// stays as a defensive net for num_div/num_mod's own internal guard (unreachable given
// the switch pre-check) and any hardware #DE. The AV's fault address must be captured
// HERE: GetExceptionInformation() is only valid inside the filter expression.
// ExceptionInformation[0] is the access type (0 = read, 1 = write), [1] the faulting
// address -- what tells a stack overflow apart from a wild pointer (raise_access_violation).
#ifdef _WIN32
static void run_switch(Context* ctx) {
    DWORD     seh_code       = 0;
    uintptr_t seh_fault_addr = 0;   // ACCESS_VIOLATION only: ExceptionInformation[1]
    bool      seh_fault_write = false;
    __try {
        run_switch_loop(ctx);
    }
    __except (
        seh_code = GetExceptionCode(),
        seh_fault_addr  = (seh_code == EXCEPTION_ACCESS_VIOLATION)
            ? GetExceptionInformation()->ExceptionRecord->ExceptionInformation[1] : 0,
        seh_fault_write = (seh_code == EXCEPTION_ACCESS_VIOLATION)
            && GetExceptionInformation()->ExceptionRecord->ExceptionInformation[0] == 1,
        (seh_code == EXCEPTION_ACCESS_VIOLATION ||
         seh_code == EXCEPTION_ILLEGAL_INSTRUCTION ||
         seh_code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
         seh_code == VM_EXC_HEAP_EXHAUSTED)
        ? EXCEPTION_EXECUTE_HANDLER
        : EXCEPTION_CONTINUE_SEARCH)
    {
        if (seh_code == EXCEPTION_ACCESS_VIOLATION)
            raise_access_violation(ctx, seh_fault_addr, seh_fault_write);
        else if (seh_code == EXCEPTION_INT_DIVIDE_BY_ZERO)
            throw std::runtime_error("Division by zero");
        else if (seh_code == VM_EXC_HEAP_EXHAUSTED)
            throw std::runtime_error("Heap exhausted");
        else
            throw std::runtime_error("Illegal Opcode executed");
    }
}
#else
// On POSIX all faults are C++ exceptions (thrown by our RaiseException stub and
// by raise_located). The SEH __try/__except frame is unnecessary here; the loop
// just propagates exceptions normally to execute()'s caller.
static void run_switch(Context* ctx) {
    run_switch_loop(ctx);
}
#endif

#ifdef _MSC_VER
#pragma warning(pop)   // restore C4714 for the rest of the TU (see the disable above)
#endif
