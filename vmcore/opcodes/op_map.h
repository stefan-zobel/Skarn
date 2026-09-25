#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include "../Value.h"
#include "../Heap.h"
#include "../Context.h"

// =============================================================================
// op_map.h -- runtime helpers for the dynamic map type (KIND_MAP).
//
// A map is TWO heap objects (see "Maps, vectors and byte buffers" in docs/VirtualMachine.md):
//   * the HEADER  (KIND_MAP): a 3-slot Value payload
//         slot 0 = backing (TAG_PTR -> the KIND_ARRAY below)
//         slot 1 = count    (Int: live entries)
//         slot 2 = used     (Int: live + tombstones, drives the load factor)
//     Scanned by the GC exactly like a KIND_ARRAY, so `backing` is forwarded for
//     free and the two Int slots forward as no-ops.
//   * the BACKING (KIND_ARRAY): 2*capacity interleaved key/value slots
//         [k0, v0, k1, v1, ...], capacity a power of two.
//     Open addressing, linear probing. Empty slot = Undefined (the natural array
//     init), deleted = Tombstone. NOTE the empty sentinel is Undefined, NOT Nil
//     (unlike the host HashTable) -- that is exactly what lets Nil be a valid key.
//
// Key identity (the documented rule): STRING keys hash and compare by CONTENT
// (stable under the moving collector -- a pointer move does not change the hash,
// so the table needs no rehash across a collection, just like the interner). Every
// other permitted key (Int / Double / Bool / Atom / Nil) hashes and compares by
// its immediate BITS => SameValueZero: NaN keys collapse, -0.0/+0.0 collapse (both
// normalized at the Value level), and Int(1) != Double(1.0) (distinct bits, the
// deliberate Java-HashMap semantics). This is intentionally NOT the promoting
// Value::operator==.
//
// Only `inline` helpers live here, no opcode handlers: run_switch's MAP_* cases
// call these. Heap exhaustion is RAISED as VM_EXC_HEAP_EXHAUSTED (SEH, not a C++
// throw) -- a mechanism whose original justification has since been retired; read
// the code's definition in op_string.h before changing anything about it.
// =============================================================================

#include "op_string.h"   // VM_EXC_HEAP_EXHAUSTED (shared heap-exhausted SEH code)

// Custom SEH code for an invalid map key type -- RAISED by the MAP_GET/MAP_SET
// cases and translated to a C++ std::runtime_error("Invalid map key type") by
// run_switch's __except. Distinct from VM_EXC_HEAP_EXHAUSTED (0xE0564D00).
inline constexpr unsigned long VM_EXC_INVALID_MAP_KEY = 0xE0564D01u; // 'VM'\1, customer bit set

// Map header (KIND_MAP) slot indices and sizing.
inline constexpr uint32_t MAP_SLOT_BACKING = 0;
inline constexpr uint32_t MAP_SLOT_COUNT   = 1;
inline constexpr uint32_t MAP_SLOT_USED    = 2;
inline constexpr uint32_t MAP_HDR_SLOTS    = 3;
inline constexpr uint32_t MAP_INITIAL_CAP  = 8;   // power of two

// A key is valid iff it is an Int / Double / Bool / Atom / Nil immediate, or a
// heap KIND_STRING. Everything else -- Undefined, Tombstone, Func / FuncPtr, and
// non-string heap objects (Array / Struct / Closure / Map) -- is rejected.
[[nodiscard]] SKARN_FORCEINLINE inline bool map_key_valid(Value k) noexcept {
    if (k.isInt() || k.isDouble() || k.isBool() || k.isAtom() || k.isNil())
        return true;
    if (k.isPtr())
        return GcObject::from_slots(k.asPtr())->kind == GcObject::KIND_STRING;
    return false;
}

// FNV-1a 32-bit over raw bytes (matches StringPoolPolicy / the interner hash).
[[nodiscard]] SKARN_FORCEINLINE inline uint32_t map_fnv1a(const char* d, uint32_t n) noexcept {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i) { h ^= static_cast<uint8_t>(d[i]); h *= 16777619u; }
    return h;
}

// Key hash: string keys hash by CONTENT; every other permitted key by its bits.
[[nodiscard]] SKARN_FORCEINLINE inline uint32_t map_key_hash(Value k) noexcept {
    if (k.isPtr()) {
        const GcObject* s = GcObject::from_slots(k.asPtr());
        return map_fnv1a(s->bytes(), s->string_length());
    }
    return k.hash();
}

// Key equality: two string keys compare by CONTENT (so a non-interned concat
// result equals an equal literal); otherwise bit-identity (SameValueZero).
[[nodiscard]] SKARN_FORCEINLINE inline bool map_key_eq(Value a, Value b) noexcept {
    if (a.isPtr() && b.isPtr()) {
        const GcObject* oa = GcObject::from_slots(a.asPtr());
        const GcObject* ob = GcObject::from_slots(b.asPtr());
        const uint32_t la = oa->string_length();
        if (la != ob->string_length()) return false;
        return std::memcmp(oa->bytes(), ob->bytes(), la) == 0;
    }
    if (a.isPtr() || b.isPtr()) return false;    // one string, one immediate
    return std::memcmp(&a, &b, sizeof(Value)) == 0;
}

// Probe the backing (2*cap interleaved key/value slots) for `key`. Returns the
// pair index in [0, cap): the slot holding an equal key if present, otherwise the
// insertion slot (first tombstone if any, else the terminating empty slot).
[[nodiscard]] SKARN_FORCEINLINE inline uint32_t
map_probe(const Value* s, uint32_t cap, Value key, uint32_t h) noexcept {
    const uint32_t mask = cap - 1;
    uint32_t idx        = h & mask;
    uint32_t first_tomb = cap;                   // sentinel "none"
    for (;;) {
        const Value k = s[2 * idx];
        if (k.isUndefined())
            return (first_tomb != cap) ? first_tomb : idx;
        if (k.isTombstone()) {
            if (first_tomb == cap) first_tomb = idx;
        } else if (map_key_eq(k, key)) {
            return idx;
        }
        idx = (idx + 1) & mask;
    }
}

// rd = map[key] or Nil on miss. Non-allocating. Caller has validated the key.
[[nodiscard]] SKARN_FORCEINLINE inline Value map_get(GcObject* map_obj, Value key) noexcept {
    GcObject*      backing = GcObject::from_slots(map_obj->slots()[MAP_SLOT_BACKING].asPtr());
    const uint32_t cap     = backing->slot_count() / 2;
    const Value*   s       = backing->slots();
    const uint32_t i       = map_probe(s, cap, key, map_key_hash(key));
    const Value    k       = s[2 * i];
    if (!k.isUndefined() && !k.isTombstone() && map_key_eq(k, key))
        return s[2 * i + 1];
    return Value::fromNil();
}

// True iff `key` is present (the probe lands on a live, equal key). Non-allocating,
// caller has validated the key. This is map_get's probe returning presence instead of
// the value -- it distinguishes an absent key from one mapped to Nil (which map_get
// cannot: a miss and `m[k] = nil` both read Nil).
[[nodiscard]] SKARN_FORCEINLINE inline bool map_has(GcObject* map_obj, Value key) noexcept {
    GcObject*      backing = GcObject::from_slots(map_obj->slots()[MAP_SLOT_BACKING].asPtr());
    const uint32_t cap     = backing->slot_count() / 2;
    const Value*   s       = backing->slots();
    const uint32_t i       = map_probe(s, cap, key, map_key_hash(key));
    const Value    k       = s[2 * i];
    return !k.isUndefined() && !k.isTombstone() && map_key_eq(k, key);
}

// Delete `key` if present: tombstone its slot and decrement the live count. Returns
// whether a live key was removed. Non-allocating; caller has validated the key.
//   * The key slot becomes a Tombstone (NOT Undefined) so probe chains through this
//     slot stay intact -- a later key that collided here is still reachable.
//   * The value slot is reset to Nil so a deleted heap value is no longer pinned as a
//     GC root (the tombstone key is an immediate, so it pins nothing).
//   * `count` (live) drops by one; `used` (live + tombstones, the load-factor driver)
//     is left unchanged -- the tombstone still occupies a probe slot until a future
//     map_grow rehashes and drops it. Deleting never shrinks the backing by itself; the
//     next insert that finds the table full does (map_rehash_cap).
[[nodiscard]] SKARN_FORCEINLINE inline bool
map_delete(GcObject* map_obj, Value key) noexcept {
    GcObject*      backing = GcObject::from_slots(map_obj->slots()[MAP_SLOT_BACKING].asPtr());
    const uint32_t cap     = backing->slot_count() / 2;
    Value*         s       = backing->slots();
    const uint32_t i       = map_probe(s, cap, key, map_key_hash(key));
    const Value    k       = s[2 * i];
    if (k.isUndefined() || k.isTombstone() || !map_key_eq(k, key))
        return false;                                  // absent -> no-op
    s[2 * i]     = Value::tombstone();                 // key slot -> tombstone (keep the chain)
    s[2 * i + 1] = Value::fromNil();                   // drop the value (unpin from GC)
    map_obj->slots()[MAP_SLOT_COUNT] = Value::fromSigned48(
        map_obj->slots()[MAP_SLOT_COUNT].asSigned48() - 1);   // count-- ; used unchanged
    return true;
}

// Allocate a new empty map into *dst (a register slot => a GC root across the
// second allocation). Order matters under the moving collector: allocate the
// HEADER first and root it via *dst (its backing slot stays Undefined -- scanned
// as a non-pointer), THEN allocate the backing; a collection triggered by the
// backing alloc keeps the header alive through *dst. Re-fetch the header after.
inline void map_new(Context* ctx, Value* dst) SKARN_ALLOC_NOEXCEPT {
    GcObject* hdr = ctx->vm->heap->alloc_slots_gc(GcObject::KIND_MAP, MAP_HDR_SLOTS, ctx);
    if (!hdr) RaiseException(VM_EXC_HEAP_EXHAUSTED, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    hdr->slots()[MAP_SLOT_COUNT] = Value::fromSigned48(0);
    hdr->slots()[MAP_SLOT_USED]  = Value::fromSigned48(0);
    *dst = Value::fromPtr(hdr->slots());         // root the header before the next alloc

    GcObject* backing = ctx->vm->heap->alloc_slots_gc(GcObject::KIND_ARRAY,
        2 * MAP_INITIAL_CAP, ctx);
    if (!backing) RaiseException(VM_EXC_HEAP_EXHAUSTED, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    hdr = GcObject::from_slots(dst->asPtr());     // re-fetch (the header may have moved)
    hdr->slots()[MAP_SLOT_BACKING] = Value::fromPtr(backing->slots());
}

// The capacity map_grow rehashes into, chosen from what is LIVE:
//   * mostly live (count >= used / 2): twice the old capacity, as always -- a map that never
//     deletes has count == used and takes this branch every time, so its capacities (and with
//     them its iteration order) are exactly what they were before this rule existed;
//   * mostly tombstones: the smallest power of two >= MAP_INITIAL_CAP that leaves the live
//     entries at most 35 % full, never more than the old capacity -- the same size or smaller.
// Always doubling was the defect: a map whose keys keep changing (a new key in, an old one out)
// held one live entry in a backing that grew with every key it had ever held and never shrank,
// and a walk over it scans the whole backing.
// Either way at most 35 % of the new table is used after the rehash, and the next one comes at
// 70 %, so the scan of the old backing is paid for by the >= 35 % of inserts in between.
[[nodiscard]] inline uint32_t map_rehash_cap(uint32_t old_cap, int64_t count, int64_t used) noexcept {
    if (count * 2 >= used) return old_cap * 2;
    uint32_t cap = MAP_INITIAL_CAP;
    while (cap < old_cap && static_cast<int64_t>(cap) * 7 / 20 < count + 1) cap *= 2;
    return cap;
}

// Rehash the live entries into a new backing of map_rehash_cap's size (tombstones dropped).
// Allocates -> may collect; `map_slot` (a register) roots the header, re-fetched
// after. RAISES on heap exhaustion.
SKARN_NOINLINE inline void map_grow(Context* ctx, Value* map_slot) SKARN_ALLOC_NOEXCEPT {
    GcObject*      hdr     = GcObject::from_slots(map_slot->asPtr());
    GcObject*      old_b   = GcObject::from_slots(hdr->slots()[MAP_SLOT_BACKING].asPtr());
    const uint32_t old_cap = old_b->slot_count() / 2;
    const uint32_t new_cap = map_rehash_cap(old_cap, hdr->slots()[MAP_SLOT_COUNT].asSigned48(),
                                            hdr->slots()[MAP_SLOT_USED].asSigned48());

    GcObject* new_b = ctx->vm->heap->alloc_slots_gc(GcObject::KIND_ARRAY, 2 * new_cap, ctx);
    if (!new_b) RaiseException(VM_EXC_HEAP_EXHAUSTED, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    // Re-fetch after the possible collection (header and old backing both moved).
    hdr   = GcObject::from_slots(map_slot->asPtr());
    old_b = GcObject::from_slots(hdr->slots()[MAP_SLOT_BACKING].asPtr());

    const Value* os       = old_b->slots();
    Value*       ns       = new_b->slots();
    const uint32_t new_mask = new_cap - 1;
    for (uint32_t j = 0; j < old_cap; ++j) {
        const Value k = os[2 * j];
        if (k.isUndefined() || k.isTombstone()) continue;
        uint32_t idx = map_key_hash(k) & new_mask;
        while (!ns[2 * idx].isUndefined()) idx = (idx + 1) & new_mask; // no tombstones yet
        ns[2 * idx]     = k;
        ns[2 * idx + 1] = os[2 * j + 1];
    }
    hdr->slots()[MAP_SLOT_BACKING] = Value::fromPtr(new_b->slots());
    hdr->slots()[MAP_SLOT_USED]    = hdr->slots()[MAP_SLOT_COUNT];      // tombstones dropped
}

// map[key] = *val_slot. `map_slot` / `key_slot` / `val_slot` are register slots so
// the map, key and value all survive a collection triggered by a grow. Caller has
// validated the key. Allocates only on the grow path.
inline void map_set(Context* ctx, Value* map_slot, Value* key_slot, Value* val_slot) noexcept {
    GcObject* hdr     = GcObject::from_slots(map_slot->asPtr());
    GcObject* backing = GcObject::from_slots(hdr->slots()[MAP_SLOT_BACKING].asPtr());
    uint32_t  cap     = backing->slot_count() / 2;
    Value*    s       = backing->slots();

    Value    key = *key_slot;
    uint32_t h   = map_key_hash(key);
    uint32_t i   = map_probe(s, cap, key, h);
    Value    existing = s[2 * i];
    if (!existing.isUndefined() && !existing.isTombstone()) {
        s[2 * i + 1] = *val_slot;                // key present -> in-place update
        return;
    }

    // New insertion. Rehash if the table (live + tombstones) would be too full --
    // into a larger table, or into one of the same size or smaller when most of it
    // is tombstones (map_rehash_cap). The early-return for an update above is what
    // keeps repeated updates of the same key from triggering unbounded growth near
    // the load-factor boundary.
    const uint32_t used = static_cast<uint32_t>(hdr->slots()[MAP_SLOT_USED].asSigned48());
    if (used + 1u > (cap * 7u) / 10u) {          // load factor 0.70
        map_grow(ctx, map_slot);                 // allocates; re-fetch everything below
        hdr      = GcObject::from_slots(map_slot->asPtr());
        backing  = GcObject::from_slots(hdr->slots()[MAP_SLOT_BACKING].asPtr());
        cap      = backing->slot_count() / 2;
        s        = backing->slots();
        key      = *key_slot;                    // re-read (a heap-string key may have moved)
        h        = map_key_hash(key);
        i        = map_probe(s, cap, key, h);    // fresh table -> a terminating empty slot
        existing = s[2 * i];
    }

    const bool was_empty = existing.isUndefined();
    s[2 * i]     = key;
    s[2 * i + 1] = *val_slot;
    hdr->slots()[MAP_SLOT_COUNT] = Value::fromSigned48(
        hdr->slots()[MAP_SLOT_COUNT].asSigned48() + 1);
    if (was_empty)                               // tombstone reuse leaves `used` unchanged
        hdr->slots()[MAP_SLOT_USED] = Value::fromSigned48(
            hdr->slots()[MAP_SLOT_USED].asSigned48() + 1);
}

// See the matching assert in op_vec.h for why this is checked rather than assumed.
#ifdef _WIN32
static_assert(noexcept(map_new(nullptr, nullptr)),
              "map allocation helpers must stay noexcept on Windows (SEH, not a throw)");
#endif
