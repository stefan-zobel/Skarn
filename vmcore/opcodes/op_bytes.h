#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include "../Value.h"
#include "../Heap.h"
#include "../Context.h"

// =============================================================================
// op_bytes.h -- runtime helpers for the growable byte buffer type (KIND_BYTES).
//
// The binary sibling of KIND_VEC (op_vec.h): the SAME header+backing shape, but
// the backing holds raw bytes (1 byte/element) instead of Value slots.
//   * the HEADER  (KIND_BYTES): a 2-slot Value payload
//         slot 0 = backing (TAG_PTR -> the KIND_STRING below)
//         slot 1 = count    (Int: live bytes, held at the front [0, count))
//     Scanned by the GC exactly like a KIND_ARRAY/KIND_VEC header -- `backing`
//     is forwarded, the Int `count` slot forwards as a no-op.
//   * the BACKING (KIND_STRING): a raw byte store reused purely as capacity.
//     Allocated with `capacity + 1` bytes so string_length() == capacity and the
//     trailing byte is a NUL (a well-formed KIND_STRING the collector moves as
//     opaque bytes -- "KIND_STRING: raw bytes, no outgoing references"). [0, count)
//     are live; [count, capacity) is scratch. Capacity doubles on growth; growth
//     is a straight memcpy of the live prefix (no rehash, unlike the map).
//
// A byte is stored / read as an Int in [0, 255]: writes mask & 0xFF, reads
// zero-extend. There is NO scalar Byte type -- Int is the element type, exactly
// as in Python/Lua/JS. Only `inline` helpers live here; run_switch's BYTES_* cases
// and the tri-kind ARRAY_GET/ARRAY_SET/LEN/VEC_PUSH/VEC_POP branches call these.
// Heap exhaustion is RAISED as VM_EXC_HEAP_EXHAUSTED, exactly like op_vec.h -- a
// mechanism whose original justification has since been retired; read the code's
// definition in op_string.h before changing anything about it.
// =============================================================================

#include "op_string.h"   // VM_EXC_HEAP_EXHAUSTED (shared heap-exhausted SEH code)

// Byte-buffer header (KIND_BYTES) slot indices and sizing.
inline constexpr uint32_t BYTES_SLOT_BACKING = 0;
inline constexpr uint32_t BYTES_SLOT_COUNT   = 1;
inline constexpr uint32_t BYTES_HDR_SLOTS    = 2;
inline constexpr uint32_t BYTES_INITIAL_CAP  = 8;

// Usable byte capacity of a KIND_BYTES backing (a KIND_STRING sized capacity+1).
[[nodiscard]] inline uint32_t bytes_backing_cap(GcObject* backing) noexcept {
    return backing->string_length();   // size - 1 (the trailing NUL is scratch)
}

// Allocate a new empty byte buffer into *dst (a register slot => a GC root across
// the second allocation), with an initial backing capacity of `cap` bytes (count
// 0). Same order discipline as vec_new_cap: allocate + root the HEADER first (its
// backing slot stays Undefined -- scanned as a non-pointer), THEN allocate the
// backing; re-fetch the header after. `cap` may be 0 -- the first push then grows
// via bytes_grow's `old_cap ? .. : BYTES_INITIAL_CAP` floor.
inline void bytes_new_cap(Context* ctx, Value* dst, uint32_t cap) SKARN_ALLOC_NOEXCEPT {
    GcObject* hdr = ctx->vm->heap->alloc_slots_gc(GcObject::KIND_BYTES, BYTES_HDR_SLOTS, ctx);
    if (!hdr) RaiseException(VM_EXC_HEAP_EXHAUSTED, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    hdr->slots()[BYTES_SLOT_COUNT] = Value::fromSigned48(0);
    *dst = Value::fromPtr(hdr->slots());          // root the header before the next alloc

    GcObject* backing = ctx->vm->heap->alloc_bytes_gc(GcObject::KIND_STRING, cap + 1, ctx);
    if (!backing) RaiseException(VM_EXC_HEAP_EXHAUSTED, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    hdr = GcObject::from_slots(dst->asPtr());      // re-fetch (the header may have moved)
    hdr->slots()[BYTES_SLOT_BACKING] = Value::fromPtr(backing->payload());
}

// Grow the backing to 2x capacity and copy the live prefix [0, count) across.
// Allocates -> may collect; `buf_slot` (a register) roots the header, re-fetched
// after. The straight-memcpy analogue of vec_grow. RAISES on exhaustion.
SKARN_NOINLINE inline void bytes_grow(Context* ctx, Value* buf_slot) SKARN_ALLOC_NOEXCEPT {
    GcObject*      hdr     = GcObject::from_slots(buf_slot->asPtr());
    GcObject*      old_b   = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
    const uint32_t old_cap = bytes_backing_cap(old_b);
    const uint32_t new_cap = old_cap ? old_cap * 2 : BYTES_INITIAL_CAP;

    GcObject* new_b = ctx->vm->heap->alloc_bytes_gc(GcObject::KIND_STRING, new_cap + 1, ctx);
    if (!new_b) RaiseException(VM_EXC_HEAP_EXHAUSTED, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    // Re-fetch after the possible collection (header and old backing both moved).
    hdr   = GcObject::from_slots(buf_slot->asPtr());
    old_b = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());

    const uint32_t count = static_cast<uint32_t>(hdr->slots()[BYTES_SLOT_COUNT].asSigned48());
    if (count) std::memcpy(new_b->bytes(), old_b->bytes(), count);   // live prefix
    hdr->slots()[BYTES_SLOT_BACKING] = Value::fromPtr(new_b->payload());
}

// Append `byte_val` (masked to 0..255) to the buffer. `buf_slot` is a register slot
// so the buffer survives a collection triggered by a grow. Allocates only on the
// grow path.
inline void bytes_push(Context* ctx, Value* buf_slot, int64_t byte_val) noexcept {
    GcObject* hdr     = GcObject::from_slots(buf_slot->asPtr());
    GcObject* backing = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
    uint32_t  count   = static_cast<uint32_t>(hdr->slots()[BYTES_SLOT_COUNT].asSigned48());
    uint32_t  cap     = bytes_backing_cap(backing);

    if (count >= cap) {                           // full -> grow (allocates; re-fetch below)
        bytes_grow(ctx, buf_slot);
        hdr     = GcObject::from_slots(buf_slot->asPtr());
        backing = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
        count   = static_cast<uint32_t>(hdr->slots()[BYTES_SLOT_COUNT].asSigned48());
    }
    backing->bytes()[count] = static_cast<char>(byte_val & 0xFF);
    hdr->slots()[BYTES_SLOT_COUNT] = Value::fromSigned48(static_cast<int64_t>(count) + 1);
}

// Remove and return the last byte as an Int (0..255), or Nil if the buffer is empty
// (a no-op then). Non-allocating. No GC unpin needed -- a byte is not a pointer.
[[nodiscard]] SKARN_FORCEINLINE inline Value bytes_pop(GcObject* buf_obj) noexcept {
    const int64_t count = buf_obj->slots()[BYTES_SLOT_COUNT].asSigned48();
    if (count <= 0) return Value::fromNil();
    GcObject* backing = GcObject::from_slots(buf_obj->slots()[BYTES_SLOT_BACKING].asPtr());
    const int64_t b = static_cast<unsigned char>(backing->bytes()[count - 1]);
    buf_obj->slots()[BYTES_SLOT_COUNT] = Value::fromSigned48(count - 1);
    return Value::fromSigned48(b);
}

// Build a byte buffer holding a copy of `src` (the `toBytes(str)` builtin). `src`
// MUST be host-stable (materialized into a std::string by the caller): the two
// allocations here can collect and would move a source that lived on the GC heap.
inline void bytes_from_str(Context* ctx, Value* dst, std::string_view src) SKARN_ALLOC_NOEXCEPT {
    const uint32_t n = static_cast<uint32_t>(src.size());
    bytes_new_cap(ctx, dst, n);                    // header + backing, *dst rooted throughout
    GcObject* hdr     = GcObject::from_slots(dst->asPtr());
    GcObject* backing = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
    if (n) std::memcpy(backing->bytes(), src.data(), n);
    hdr->slots()[BYTES_SLOT_COUNT] = Value::fromSigned48(static_cast<int64_t>(n));
}

// Out-of-line wrapper for BYTES_FROM_STR: materialize the source KIND_STRING's bytes
// into a host std::string (stable across the allocs), then build the buffer. Kept
// SKARN_NOINLINE so the std::string (which needs unwinding) stays out of the dispatch
// function -- the same discipline as to_string_value. NOTE: this no longer avoids C2712.
// The caller is run_switch_LOOP, which holds no __try (the SEH frame lives in the
// separate run_switch), so nothing forbids an unwindable local there.
// C2712 does still bind raise_access_violation, which run_switch's __except calls.
SKARN_NOINLINE inline void bytes_from_string_obj(Context* ctx, Value* dst,
                                                     const GcObject* src_obj) SKARN_ALLOC_NOEXCEPT {
    std::string host(src_obj->bytes(), src_obj->string_length());
    bytes_from_str(ctx, dst, host);
}

// Ensure the buffer's backing has capacity >= `needed` bytes, growing it (one alloc +
// copy of the live prefix) if short; a no-op when already large enough. Sized to a
// TARGET rather than bytes_grow's fixed doubling (still doubles when that suffices, so
// repeated appends stay amortized O(1)). `buf_slot` (a register) roots the header across
// the possible collection; re-fetched after. RAISES on exhaustion.
SKARN_NOINLINE inline void bytes_ensure_cap(Context* ctx, Value* buf_slot, uint32_t needed) SKARN_ALLOC_NOEXCEPT {
    GcObject* hdr     = GcObject::from_slots(buf_slot->asPtr());
    GcObject* backing = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
    const uint32_t cap = bytes_backing_cap(backing);
    if (cap >= needed) return;
    uint32_t new_cap = cap ? cap * 2 : BYTES_INITIAL_CAP;
    if (new_cap < needed) new_cap = needed;

    GcObject* new_b = ctx->vm->heap->alloc_bytes_gc(GcObject::KIND_STRING, new_cap + 1, ctx);
    if (!new_b) RaiseException(VM_EXC_HEAP_EXHAUSTED, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    // Re-fetch after the possible collection (header and backing both moved).
    hdr     = GcObject::from_slots(buf_slot->asPtr());
    backing = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
    const uint32_t count = static_cast<uint32_t>(hdr->slots()[BYTES_SLOT_COUNT].asSigned48());
    if (count) std::memcpy(new_b->bytes(), backing->bytes(), count);   // live prefix
    hdr->slots()[BYTES_SLOT_BACKING] = Value::fromPtr(new_b->payload());
}

// Append `n` bytes at `src_off` of the source's backing onto the dst byte buffer -- one
// grow + memcpy (the BYTES_APPEND_RANGE core; whole-buffer BYTES_APPEND routes here after
// deriving n/off). Both slots are register roots, so the capacity grow (which may collect)
// is GC-safe: the source is re-fetched via `src_slot` AFTER the grow. `src` may be a
// KIND_STRING (off/len over its byte payload) or a KIND_BYTES (over its backing). Handles
// src == dst: the destination region [count, count+n) is disjoint from the source
// [src_off, src_off+n) which lies within [0, count). Caller validates the source range.
SKARN_NOINLINE inline void bytes_append_span(Context* ctx, Value* dst_slot, Value* src_slot,
                                                 uint32_t src_off, uint32_t n) SKARN_ALLOC_NOEXCEPT {
    if (n == 0) return;
    GcObject*      dhdr  = GcObject::from_slots(dst_slot->asPtr());
    const uint32_t count = static_cast<uint32_t>(dhdr->slots()[BYTES_SLOT_COUNT].asSigned48());
    bytes_ensure_cap(ctx, dst_slot, count + n);                 // may collect; roots via dst_slot
    dhdr = GcObject::from_slots(dst_slot->asPtr());             // re-fetch dst after the grow
    GcObject* dback = GcObject::from_slots(dhdr->slots()[BYTES_SLOT_BACKING].asPtr());
    GcObject* src   = GcObject::from_slots(src_slot->asPtr());  // re-fetch src (may have moved)
    const char* sptr = (src->kind == GcObject::KIND_STRING)
        ? src->bytes()
        : GcObject::from_slots(src->slots()[BYTES_SLOT_BACKING].asPtr())->bytes();
    std::memcpy(dback->bytes() + count, sptr + src_off, n);
    dhdr->slots()[BYTES_SLOT_COUNT] = Value::fromSigned48(static_cast<int64_t>(count) + n);
}

// Live byte length of a source that is a KIND_STRING (its payload) or a KIND_BYTES (its
// count slot) -- the length BYTES_APPEND appends and the bound BYTES_APPEND_RANGE checks.
[[nodiscard]] SKARN_FORCEINLINE inline uint32_t bytes_src_len(const GcObject* src) noexcept {
    return src->kind == GcObject::KIND_STRING
        ? src->string_length()
        : static_cast<uint32_t>(src->slots()[BYTES_SLOT_COUNT].asSigned48());
}

// Decode a buffer's live bytes [0, count) into a fresh KIND_STRING (the `fromBytes(b)`
// builtin). Bytes are materialized host-side FIRST, so the following alloc_string_gc
// collection cannot move the source out from under the copy. SKARN_NOINLINE so the
// std::string stays out of the dispatch function -- no longer a C2712 requirement, see
// bytes_from_string_obj above.
[[nodiscard]] SKARN_NOINLINE inline Value bytes_to_string(Context* ctx, Value* buf_slot) SKARN_ALLOC_NOEXCEPT {
    GcObject*      hdr     = GcObject::from_slots(buf_slot->asPtr());
    GcObject*      backing = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
    const uint32_t count   = static_cast<uint32_t>(hdr->slots()[BYTES_SLOT_COUNT].asSigned48());
    std::string    host(backing->bytes(), count);  // materialize before the alloc can move things
    GcObject* s = ctx->vm->heap->alloc_string_gc(host, ctx);
    if (!s) RaiseException(VM_EXC_HEAP_EXHAUSTED, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return Value::fromPtr(s->payload());
}

// See the matching assert in op_vec.h for why this is checked rather than assumed.
#ifdef _WIN32
static_assert(noexcept(bytes_to_string(nullptr, nullptr)),
              "bytes allocation helpers must stay noexcept on Windows (SEH, not a throw)");
#endif
