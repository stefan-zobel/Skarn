#pragma once

#include <cassert>
#include <cstdint>
#include "..\Value.h"
#include "..\Heap.h"
#include "..\Context.h"

// =============================================================================
// op_vec.h -- runtime helpers for the growable vector type (KIND_VEC).
//
// A vector is TWO heap objects, the same header+backing shape the map pioneered
// (see "Maps, vectors and byte buffers" in docs/VirtualMachine.md), minus the hashing / tombstones:
//   * the HEADER  (KIND_VEC): a 2-slot Value payload
//         slot 0 = backing (TAG_PTR -> the KIND_ARRAY below)
//         slot 1 = count    (Int: live elements, held at the front [0, count))
//     Scanned by the GC exactly like a KIND_ARRAY, so `backing` is forwarded for
//     free and the Int `count` slot forwards as a no-op.
//   * the BACKING (KIND_ARRAY): `capacity` slots, of which [0, count) are live;
//     the tail [count, capacity) is Undefined (the natural array init). Capacity
//     doubles on growth. There is no hashing and no tombstones -- push appends at
//     `count`, pop shrinks `count`, and growth is a straight memcpy of the live
//     prefix (unlike the map, which must rehash).
//
// Only `inline` helpers live here, no opcode handlers: run_switch's VEC_* cases
// call these. Heap exhaustion is RAISED as VM_EXC_HEAP_EXHAUSTED (SEH, not a C++
// throw) -- a mechanism whose original justification has since been retired; read
// the code's definition in op_string.h before changing anything about it.
// =============================================================================

#include "op_string.h"   // VM_EXC_HEAP_EXHAUSTED (shared heap-exhausted SEH code)

// Vector header (KIND_VEC) slot indices and sizing.
inline constexpr uint32_t VEC_SLOT_BACKING = 0;
inline constexpr uint32_t VEC_SLOT_COUNT   = 1;
inline constexpr uint32_t VEC_HDR_SLOTS    = 2;
inline constexpr uint32_t VEC_INITIAL_CAP  = 8;

// Allocate a new empty vector into *dst (a register slot => a GC root across the
// second allocation), with an initial backing capacity of `cap` slots (count 0).
// Order matters under the moving collector: allocate the HEADER first and root it
// via *dst (its backing slot stays Undefined -- scanned as a non-pointer), THEN
// allocate the backing; a collection triggered by the backing alloc keeps the
// header alive through *dst. Re-fetch the header after. `cap` may be 0 -- the first
// push then grows via vec_grow's `old_cap ? .. : VEC_INITIAL_CAP` floor, so there is
// no grow-from-zero trap.
inline void vec_new_cap(Context* ctx, Value* dst, uint32_t cap) noexcept {
    GcObject* hdr = ctx->vm->heap->alloc_slots_gc(GcObject::KIND_VEC, VEC_HDR_SLOTS, ctx);
    if (!hdr) RaiseException(VM_EXC_HEAP_EXHAUSTED, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    hdr->slots()[VEC_SLOT_COUNT] = Value::fromSigned48(0);
    *dst = Value::fromPtr(hdr->slots());          // root the header before the next alloc

    GcObject* backing = ctx->vm->heap->alloc_slots_gc(GcObject::KIND_ARRAY, cap, ctx);
    if (!backing) RaiseException(VM_EXC_HEAP_EXHAUSTED, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    hdr = GcObject::from_slots(dst->asPtr());      // re-fetch (the header may have moved)
    hdr->slots()[VEC_SLOT_BACKING] = Value::fromPtr(backing->slots());
}

// Allocate a new empty vector with the default initial capacity (the `vec()` builtin).
inline void vec_new(Context* ctx, Value* dst) noexcept {
    vec_new_cap(ctx, dst, VEC_INITIAL_CAP);
}

// Grow the backing to 2x capacity and copy the live prefix [0, count) across.
// Allocates -> may collect; `vec_slot` (a register) roots the header, re-fetched
// after. Simpler than map_grow: no rehash, just a linear copy. RAISES on exhaustion.
[[msvc::noinline]] inline void vec_grow(Context* ctx, Value* vec_slot) noexcept {
    GcObject*      hdr     = GcObject::from_slots(vec_slot->asPtr());
    GcObject*      old_b   = GcObject::from_slots(hdr->slots()[VEC_SLOT_BACKING].asPtr());
    const uint32_t old_cap = old_b->slot_count();
    const uint32_t new_cap = old_cap ? old_cap * 2 : VEC_INITIAL_CAP;

    GcObject* new_b = ctx->vm->heap->alloc_slots_gc(GcObject::KIND_ARRAY, new_cap, ctx);
    if (!new_b) RaiseException(VM_EXC_HEAP_EXHAUSTED, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    // Re-fetch after the possible collection (header and old backing both moved).
    hdr   = GcObject::from_slots(vec_slot->asPtr());
    old_b = GcObject::from_slots(hdr->slots()[VEC_SLOT_BACKING].asPtr());

    const uint32_t count = static_cast<uint32_t>(hdr->slots()[VEC_SLOT_COUNT].asSigned48());
    const Value*   os    = old_b->slots();
    Value*         ns    = new_b->slots();
    for (uint32_t i = 0; i < count; ++i) ns[i] = os[i];   // live prefix; tail stays Undefined
    hdr->slots()[VEC_SLOT_BACKING] = Value::fromPtr(new_b->slots());
}

// Append *val_slot to the vector. `vec_slot` / `val_slot` are register slots so the
// vector and the value both survive a collection triggered by a grow. Allocates only
// on the grow path.
inline void vec_push(Context* ctx, Value* vec_slot, Value* val_slot) noexcept {
    GcObject* hdr     = GcObject::from_slots(vec_slot->asPtr());
    GcObject* backing = GcObject::from_slots(hdr->slots()[VEC_SLOT_BACKING].asPtr());
    uint32_t  count   = static_cast<uint32_t>(hdr->slots()[VEC_SLOT_COUNT].asSigned48());
    uint32_t  cap     = backing->slot_count();

    if (count >= cap) {                           // full -> grow (allocates; re-fetch below)
        vec_grow(ctx, vec_slot);
        hdr     = GcObject::from_slots(vec_slot->asPtr());
        backing = GcObject::from_slots(hdr->slots()[VEC_SLOT_BACKING].asPtr());
        count   = static_cast<uint32_t>(hdr->slots()[VEC_SLOT_COUNT].asSigned48());
    }
    backing->slots()[count] = *val_slot;
    hdr->slots()[VEC_SLOT_COUNT] = Value::fromSigned48(static_cast<int64_t>(count) + 1);
}

// Remove and return the last element, or Nil if the vector is empty (a no-op then).
// Non-allocating. The vacated slot is reset to Nil so a popped heap value is no longer
// pinned as a GC root.
[[nodiscard]] [[msvc::forceinline]] inline Value vec_pop(GcObject* vec_obj) noexcept {
    const int64_t count = vec_obj->slots()[VEC_SLOT_COUNT].asSigned48();
    if (count <= 0) return Value::fromNil();      // empty -> Nil, count unchanged
    GcObject* backing = GcObject::from_slots(vec_obj->slots()[VEC_SLOT_BACKING].asPtr());
    const uint32_t last = static_cast<uint32_t>(count - 1);
    const Value    x    = backing->slots()[last];
    backing->slots()[last]              = Value::fromNil();          // unpin from GC
    vec_obj->slots()[VEC_SLOT_COUNT]    = Value::fromSigned48(count - 1);
    return x;
}
