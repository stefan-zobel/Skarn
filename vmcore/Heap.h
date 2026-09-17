#pragma once

#include <cstdint>
#include <cstddef>
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <chrono>       // steady_clock -- measurement-only GC timing (--bench)
#include <ostream>      // debug_dump(std::ostream&)  (<format> comes via Value.h)
#include "Platform.h"
#include "Value.h"
#include "Context.h"
#include "VM.h"

// =============================================================================
// GcObject -- header preceding every heap-allocated object.
//
// Memory layout (contiguous):
//   [ GcObject (8 Byte) | payload... ]
//
// Payload interpretation depends on 'kind':
//   KIND_ARRAY  -> Value slot[0], slot[1], ...
//   KIND_STRING -> char byte[0], byte[1], ... , '\0'
//
// A Value holding a pointer to a GcObject always points to the payload start,
// i.e. to (GcObject* + 1). The header is reached via (payload_ptr - 1).
//
// Alignment: every object's *footprint* (header + payload) is rounded up to 8
// (padded_total_bytes()) for the bump/copy/scan stride, so each object's header
// starts 8-aligned as promised by static_assert(alignof(GcObject) == 8). `size`
// itself stays EXACT -- only the stride is padded. Arrays are 8 + 8n (already
// aligned); only odd-length KIND_STRING payloads ever get trailing pad bytes,
// which are never interpreted (the scan lands exactly on the next header).
//
// Forwarding (copying GC): while a collection is in progress the collector
// evacuates each live object from from-space to to-space and overwrites the
// *from-space* header to record where the object went:
//   - color is set to the FORWARDED sentinel, and
//   - size is repurposed to hold the to-space *byte offset* of the new header
//     (relative to the active to-space base).
// color (byte 4) and size (bytes 0..3) do not overlap, so both survive. This
// caps a single semispace at 4 GiB (offset must fit in uint32_t) -- far below
// the reserved MAX_SEMI, so it is a non-constraint in practice.
// =============================================================================
struct alignas(8) GcObject {
    uint32_t size;  // payload bytes (NOT slot count); reused as fwd offset when FORWARDED
    uint8_t  color; // tri-color state: WHITE=0, GRAY=1, BLACK=2; FORWARDED=3 during GC.
                    // Only FORWARDED is load-bearing under Cheney -- WHITE/GRAY/BLACK are
                    // vestigial. That spare capacity is deliberately kept as the reserved
                    // home for a future UNIVERSAL (all-kinds) per-object header flag, e.g.
                    // a generational-GC age / remembered-set bit, without growing the
                    // 8-byte header.
    uint8_t  kind;  // object kind
    uint16_t _pad;  // explicit padding -- struct is exactly 8 Byte. KIND-SPECIFIC scratch:
                    // reserved so KIND_STRING can cache a 16-bit content-hash fold here for
                    // PropertyKeyPolicy. Consumed ONLY by the
                    // DEFERRED dynamic map/dict type -- near-term fixed-shape structs use
                    // slot indices, not hashed keys, so they never touch this. arrays / a
                    // future KIND_OBJECT are free to overload it for their own per-kind use.

    // Array view ---------------------------------------------------------------
    [[nodiscard]] Value* slots() noexcept {
        return reinterpret_cast<Value*>(this + 1);
    }
    [[nodiscard]] const Value* slots() const noexcept {
        return reinterpret_cast<const Value*>(this + 1);
    }

    [[nodiscard]] uint32_t slot_count() const noexcept {
        assert((kind == KIND_ARRAY || kind == KIND_OBJECT || kind == KIND_CLOSURE ||
                kind == KIND_MAP   || kind == KIND_VEC    || kind == KIND_BYTES) &&
               "slot_count() only valid for KIND_ARRAY / KIND_OBJECT / KIND_CLOSURE / KIND_MAP / KIND_VEC / KIND_BYTES");
        assert((size % sizeof(Value)) == 0 && "slot payload must be Value-aligned");
        return static_cast<uint32_t>(size / sizeof(Value));
    }

    // Struct type id -- KIND_OBJECT overloads the kind-specific `_pad` bytes to
    // hold the 16-bit type id (index into the VM struct-type registry). Accessor-
    // mediated so the field can be narrowed later (e.g. to carve out per-object
    // flag bits) with a localized change.
    [[nodiscard]] uint16_t object_type_id() const noexcept {
        assert(kind == KIND_OBJECT && "object_type_id() only valid for KIND_OBJECT");
        return _pad;
    }
    void set_object_type_id(uint16_t id) noexcept {
        assert(kind == KIND_OBJECT && "set_object_type_id() only valid for KIND_OBJECT");
        _pad = id;
    }

    // Closure function id -- KIND_CLOSURE overloads the kind-specific `_pad` bytes
    // to hold the 16-bit function-table id (index into VM.fn_table). The payload
    // is Value[ncaptures] (the by-value captures), scanned by the GC exactly like a
    // KIND_ARRAY. Mirrors object_type_id(); accessor-mediated so the field can be
    // narrowed later (e.g. to carve out per-object flag bits) with a localized change.
    [[nodiscard]] uint16_t closure_fn_id() const noexcept {
        assert(kind == KIND_CLOSURE && "closure_fn_id() only valid for KIND_CLOSURE");
        return _pad;
    }
    void set_closure_fn_id(uint16_t id) noexcept {
        assert(kind == KIND_CLOSURE && "set_closure_fn_id() only valid for KIND_CLOSURE");
        _pad = id;
    }

    // Byte / string view -------------------------------------------------------
    [[nodiscard]] char* bytes() noexcept {
        return reinterpret_cast<char*>(this + 1);
    }
    [[nodiscard]] const char* bytes() const noexcept {
        return reinterpret_cast<const char*>(this + 1);
    }

    [[nodiscard]] uint32_t string_length() const noexcept {
        assert(kind == KIND_STRING && "string_length() only valid for KIND_STRING");
        assert(size > 0 && "KIND_STRING payload must include trailing NUL");
        return size - 1; // payload includes trailing '\0'
    }

    // Common -------------------------------------------------------------------
    // Payload start, kind-agnostic. A Value pointer always holds this address
    // (slots() and bytes() are just typed views of the same location).
    [[nodiscard]] void* payload() noexcept {
        return this + 1;
    }

    [[nodiscard]] size_t total_bytes() const noexcept {
        return sizeof(GcObject) + size;
    }

    // Round a byte count up to the 8-byte GcObject alignment.
    [[nodiscard]] static constexpr size_t align_up_8(size_t n) noexcept {
        return (n + 7) & ~size_t(7);
    }

    // Object footprint (header + payload) rounded up to 8 so the *next* object's
    // header stays 8-aligned. `size` is kept EXACT (string_length()/slot_count()
    // and the FORWARDED offset math read it unchanged); only the bump/copy/scan
    // stride uses this padded value. Arrays are 8 + 8n -- already aligned, so
    // padding only ever affects odd-length strings.
    [[nodiscard]] size_t padded_total_bytes() const noexcept {
        return align_up_8(total_bytes());
    }

    [[nodiscard]] static GcObject* from_slots(void* ptr) noexcept {
        return static_cast<GcObject*>(ptr) - 1;
    }

    static constexpr uint8_t WHITE     = 0;
    static constexpr uint8_t GRAY      = 1;
    static constexpr uint8_t BLACK     = 2;
    static constexpr uint8_t FORWARDED = 3; // from-space marker during a copying collection

    static constexpr uint8_t KIND_ARRAY   = 0;
    static constexpr uint8_t KIND_STRING  = 1;
    static constexpr uint8_t KIND_OBJECT  = 2; // fixed-shape struct: payload is Value[nfields], _pad = type id
    static constexpr uint8_t KIND_CLOSURE = 3; // closure: payload is Value[ncaptures], _pad = function id
    static constexpr uint8_t KIND_MAP     = 4; // dynamic map header: payload is Value[3] = {backing, count, used}
    static constexpr uint8_t KIND_VEC     = 5; // growable vector header: payload is Value[2] = {backing, count}
    static constexpr uint8_t KIND_BYTES   = 6; // growable byte buffer header: payload is Value[2] = {backing (KIND_STRING), count}
};

static_assert(sizeof(GcObject)  == 8);
static_assert(alignof(GcObject) == 8);

// =============================================================================
// Heap -- Cheney copying collector over two VirtualAlloc'd semispaces.
//
// Allocation:
//   Linear bump pointer inside the active semispace. alloc_*() returns nullptr
//   when the active semispace is exhausted -- the caller triggers a GC and
//   retries once -- the full allocate-or-collect-or-grow ladder is alloc_*_gc()
//   below; StringInterner::intern drives the same ladder by hand.
//
// Collection (collect):
//   Stop-the-world Cheney scan. flip the semispaces, evacuate every root
//   IN PLACE (each Value* slot is rewritten to the survivor's new address),
//   then scan the freshly copied objects in to-space -- to-space itself is the
//   implicit worklist, so no auxiliary mark stack is needed. Dead objects are
//   never touched; only survivors are copied.
//
//   Roots: current + all saved register frames (via Context), explicitly
//   registered global roots (Value* slots), and external strong roots
//   (forward_vm_external_roots -- currently the active StringInterner via ctx->vm).
//   KIND_ARRAY payloads are scanned as Value[]; KIND_STRING payloads are raw
//   bytes and carry no outgoing references.
//
// Memory:
//   Two semispaces, each VirtualAlloc-RESERVEd at MAX_SEMI and committed
//   incrementally. Both are always committed to the same size (semi_capacity_)
//   so to-space can always hold from-space's survivors -- no growth is needed
//   mid-collection. Growth is occupancy-driven: after a collection, if live
//   data exceeds GROW_THRESHOLD of the committed size, both semispaces double
//   (capped at MAX_SEMI). The reserved bases never move, so forwarding offsets
//   stay valid across growth.
//
// Limitations (intentional):
//   Single-threaded, stop-the-world. No finalizers. No generational nursery or
//   write barrier -- and that absence is a RESULT, not a to-do. One was built
//   (bump nursery, promote-on-first-survival, precise remembered set, then card
//   marking), measured and REVERTED: Cheney copies only survivors, so it already
//   handles young churn at ~0 % GC time, while the nursery REGRESSED the
//   incremental build of a RETAINED structure by ~2x GC time, because a copying
//   old generation re-copies retained data. Fixing that needs a NON-MOVING old
//   generation, which gives up the zero-fragmentation bump-allocation pillar.
//   Do not rebuild without new evidence.
// =============================================================================
class Heap {
public:
    static constexpr size_t INITIAL_SEMI   = 4 * 1024 * 1024;          // 4 MiB usable to start (8 MiB committed total)
    static constexpr size_t MAX_SEMI       = size_t(1) * 1024 * 1024 * 1024; // 1 GiB reserved per semispace
    static constexpr float  GROW_THRESHOLD = 0.75f;

    // -------------------------------------------------------------------
    // GcStats -- measurement-only instrumentation (the `--bench` GC benchmark).
    // Purely ADDITIVE and behavior-preserving: the collector's decisions are
    // unchanged; these counters only record what already happened. Read via
    // stats(), cleared via reset_stats(). The only hot-path cost is two counter
    // increments per successful bump allocation (negligible vs. the payload
    // memset); the timing/survivor accounting is per-collection (cold).
    // -------------------------------------------------------------------
    struct GcStats {
        uint64_t collections     = 0; // completed collect() cycles
        uint64_t objects_alloced = 0; // successful bump allocations
        uint64_t bytes_alloced   = 0; // their total (padded) footprint bytes
        uint64_t from_used_sum   = 0; // sum over collections of bytes live at entry
        uint64_t survivors_sum   = 0; // sum over collections of bytes surviving
        uint64_t gc_ns_total     = 0; // total wall-time spent inside collect()
        uint64_t gc_ns_max       = 0; // longest single collect() (max pause)
        uint64_t grow_events     = 0; // semispace growths
    };
    [[nodiscard]] const GcStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = GcStats{}; }

    explicit Heap(size_t initial_semi = INITIAL_SEMI)
        : semi_capacity_(0)
        , active_(0)
    {
        assert(initial_semi > sizeof(GcObject) && "Heap semispace too small");
        assert(initial_semi <= MAX_SEMI && "initial semispace exceeds reserve");
        space_[0] = reserve_semi();
        space_[1] = reserve_semi();
        if (!commit_both(initial_semi)) {
            release_semi(space_[0]);
            release_semi(space_[1]);
            throw std::runtime_error("Heap: initial VirtualAlloc commit failed");
        }
        bump_ = space_[active_];
    }

    ~Heap() {
        release_semi(space_[0]);
        release_semi(space_[1]);
    }

    // Owns raw OS memory -- non-copyable, non-movable (never copied/moved today).
    Heap(const Heap&)            = delete;
    Heap& operator=(const Heap&) = delete;

    // -------------------------------------------------------------------
    // Global roots -- Value* slots that are always scanned as GC roots.
    //
    // The caller owns the Value -- Heap only stores the pointer.
    // The Value must remain at a stable address.
    // -------------------------------------------------------------------
    void add_root(Value* slot) {
        assert(slot && "add_root: null slot");
        global_roots_.push_back(slot);
    }

    void remove_root(Value* slot) noexcept {
        for (auto it = global_roots_.begin(); it != global_roots_.end(); ++it) {
            if (*it == slot) {
                global_roots_.erase(it);
                return;
            }
        }
    }

    [[nodiscard]] size_t root_count() const noexcept {
        return global_roots_.size();
    }

    // -------------------------------------------------------------------
    // alloc_slots -- allocate a KIND_ARRAY-like payload with n_slots Value slots.
    // The payload is initialized to Value() (Undefined).
    // Returns nullptr if the active semispace is full.
    // -------------------------------------------------------------------
    [[nodiscard]] GcObject* alloc_slots(uint8_t kind, uint32_t n_slots) noexcept {
        const size_t payload_bytes = static_cast<size_t>(n_slots) * sizeof(Value);
        const size_t total         = total_for_slots(n_slots);
        if (used() + total > semi_capacity_)
            return nullptr;

        auto* obj  = reinterpret_cast<GcObject*>(bump_);
        obj->size  = static_cast<uint32_t>(payload_bytes);
        obj->color = GcObject::WHITE;
        obj->kind  = kind;
        obj->_pad  = 0;

        Value* s = obj->slots();
        for (uint32_t i = 0; i < n_slots; ++i)
            s[i] = Value{};

        stats_.objects_alloced++;
        stats_.bytes_alloced += total;
        bump_ += total;
        return obj;
    }

    // -------------------------------------------------------------------
    // alloc_bytes -- allocate a raw byte payload.
    // The payload is zero-initialized.
    // Returns nullptr if the active semispace is full.
    // -------------------------------------------------------------------
    [[nodiscard]] GcObject* alloc_bytes(uint8_t kind, uint32_t n_bytes) noexcept {
        const size_t total = total_for_bytes(n_bytes);
        if (used() + total > semi_capacity_)
            return nullptr;

        auto* obj  = reinterpret_cast<GcObject*>(bump_);
        obj->size  = n_bytes;
        obj->color = GcObject::WHITE;
        obj->kind  = kind;
        obj->_pad  = 0;

        std::memset(obj->bytes(), 0, n_bytes);

        stats_.objects_alloced++;
        stats_.bytes_alloced += total;
        bump_ += total;
        return obj;
    }

    // -------------------------------------------------------------------
    // Transitional compatibility wrapper:
    //   old code expects alloc(kind, n_slots) for Value-slot objects.
    // Keep it for now so the rest of the VM compiles unchanged.
    // -------------------------------------------------------------------
    [[nodiscard]] GcObject* alloc(uint8_t kind, uint32_t n_slots) noexcept {
        return alloc_slots(kind, n_slots);
    }

    // -------------------------------------------------------------------
    // alloc_string -- allocate a NUL-terminated string payload.
    //
    // Layout:
    //   payload[0..len-1] = string bytes
    //   payload[len]      = '\0'
    //
    // size stores the full payload bytes INCLUDING the trailing NUL.
    // Returns nullptr if the active semispace is full.
    // -------------------------------------------------------------------
    [[nodiscard]] GcObject* alloc_string(std::string_view s) noexcept {
        // +1 for trailing '\0'
        GcObject* obj = alloc_bytes(GcObject::KIND_STRING,
            static_cast<uint32_t>(s.size() + 1));
        if (!obj)
            return nullptr;

        if (!s.empty())
            std::memcpy(obj->bytes(), s.data(), s.size());
        obj->bytes()[s.size()] = '\0';
        return obj;
    }

    // -------------------------------------------------------------------
    // alloc_slots_gc -- the full allocation policy for a KIND_ARRAY-like object:
    // try to allocate; on failure collect once and retry; if it still does not
    // fit (the live set alone leaves no room, or the object is simply larger
    // than a semispace), grow the heap and retry a last time. Encapsulates
    // allocate-or-collect-or-grow so call sites neither duplicate the retry
    // ladder nor re-derive the object's byte size. Returns nullptr only if even
    // growth to MAX_SEMI cannot satisfy the request.
    // -------------------------------------------------------------------
    [[nodiscard]] GcObject* alloc_slots_gc(uint8_t kind, uint32_t n_slots, Context* ctx) noexcept {
        if (GcObject* o = alloc_slots(kind, n_slots)) return o;
        collect(ctx);
        if (GcObject* o = alloc_slots(kind, n_slots)) return o;
        if (grow_to_fit(total_for_slots(n_slots)))
            return alloc_slots(kind, n_slots);
        return nullptr;
    }

    // -------------------------------------------------------------------
    // alloc_string_gc -- the full allocation policy for a KIND_STRING: try to
    // allocate; on failure collect once and retry; if it still does not fit, grow
    // the heap and retry a last time. The string counterpart of alloc_slots_gc,
    // used by the concat path (op_string.h). Returns nullptr only if even growth to
    // MAX_SEMI cannot satisfy the request. (The interner drives this ladder by hand
    // instead, because it must re-check the intern table between collect and retry.)
    // -------------------------------------------------------------------
    [[nodiscard]] GcObject* alloc_string_gc(std::string_view s, Context* ctx) noexcept {
        if (GcObject* o = alloc_string(s)) return o;
        collect(ctx);
        if (GcObject* o = alloc_string(s)) return o;
        if (reserve_string(s))
            return alloc_string(s);
        return nullptr;
    }

    // -------------------------------------------------------------------
    // alloc_bytes_gc -- the full allocation policy for a raw byte payload
    // (allocate-or-collect-or-grow), the byte-payload counterpart of
    // alloc_slots_gc. Used for the KIND_STRING backing of a growable byte
    // buffer (KIND_BYTES, op_bytes.h). Returns nullptr only if even growth to
    // MAX_SEMI cannot satisfy the request.
    // -------------------------------------------------------------------
    [[nodiscard]] GcObject* alloc_bytes_gc(uint8_t kind, uint32_t n_bytes, Context* ctx) noexcept {
        if (GcObject* o = alloc_bytes(kind, n_bytes)) return o;
        collect(ctx);
        if (GcObject* o = alloc_bytes(kind, n_bytes)) return o;
        if (grow_to_fit(total_for_bytes(n_bytes)))
            return alloc_bytes(kind, n_bytes);
        return nullptr;
    }

    // -------------------------------------------------------------------
    // alloc_object / alloc_object_gc -- allocate a KIND_OBJECT (fixed-shape
    // struct) with n_fields Value slots, stamping the 16-bit type id into the
    // header. Payload is Value[n_fields] (Undefined-initialized), scanned by the
    // GC exactly like a KIND_ARRAY. The _gc variant runs the full
    // allocate-or-collect-or-grow ladder. Returns nullptr only if even growth to
    // MAX_SEMI cannot satisfy the request.
    // -------------------------------------------------------------------
    [[nodiscard]] GcObject* alloc_object(uint16_t type_id, uint32_t n_fields) noexcept {
        GcObject* obj = alloc_slots(GcObject::KIND_OBJECT, n_fields);
        if (obj) obj->set_object_type_id(type_id);
        return obj;
    }

    [[nodiscard]] GcObject* alloc_object_gc(uint16_t type_id, uint32_t n_fields, Context* ctx) noexcept {
        GcObject* obj = alloc_slots_gc(GcObject::KIND_OBJECT, n_fields, ctx);
        if (obj) obj->set_object_type_id(type_id);
        return obj;
    }

    // -------------------------------------------------------------------
    // alloc_closure / alloc_closure_gc -- allocate a KIND_CLOSURE with
    // n_captures Value slots, stamping the 16-bit function-table id into the
    // header. Payload is Value[n_captures] (the by-value captures, Undefined-
    // initialized), scanned by the GC exactly like a KIND_ARRAY. The _gc variant
    // runs the full allocate-or-collect-or-grow ladder. Returns nullptr only if
    // even growth to MAX_SEMI cannot satisfy the request. Capture-free functions
    // are Func immediates, not closures -- a KIND_CLOSURE always carries >= 1
    // capture in practice, but n_captures == 0 is allocated correctly too.
    // -------------------------------------------------------------------
    [[nodiscard]] GcObject* alloc_closure(uint16_t fn_id, uint32_t n_captures) noexcept {
        GcObject* obj = alloc_slots(GcObject::KIND_CLOSURE, n_captures);
        if (obj) obj->set_closure_fn_id(fn_id);
        return obj;
    }

    [[nodiscard]] GcObject* alloc_closure_gc(uint16_t fn_id, uint32_t n_captures, Context* ctx) noexcept {
        GcObject* obj = alloc_slots_gc(GcObject::KIND_CLOSURE, n_captures, ctx);
        if (obj) obj->set_closure_fn_id(fn_id);
        return obj;
    }

    // -------------------------------------------------------------------
    // reserve_string -- grow the heap (last resort) so a string of `s` will fit
    // on top of the current live set. StringInterner uses this instead of
    // alloc_slots_gc because its retry path must re-check the intern table
    // between the collection and the retry, so it drives the ladder itself.
    // -------------------------------------------------------------------
    [[nodiscard]] bool reserve_string(std::string_view s) noexcept {
        return grow_to_fit(total_for_bytes(static_cast<uint32_t>(s.size() + 1)));
    }

    // -------------------------------------------------------------------
    // grow_to_fit -- commit enough capacity so an object of `total` bytes fits
    // on top of the current live set. Last-resort allocation growth, used by
    // the caller only after a collection failed to free enough room. Returns
    // false if even MAX_SEMI cannot hold it.
    //
    // It can fail for two DIFFERENT reasons, and they do not behave alike across
    // platforms:
    //
    //   1. "the request exceeds our own reservation" -- `needed > new_cap` below, a
    //      plain comparison against MAX_SEMI with no operating system involved. Identical
    //      everywhere, and the branch a program realistically reaches.
    //   2. "the system is out of memory" -- commit_both() fails. On Windows
    //      VirtualAlloc(MEM_COMMIT) genuinely fails when there is nothing to commit, so
    //      this returns false and the caller degrades cleanly. On POSIX it effectively
    //      cannot: mprotect() on an already-reserved mapping charges nothing, so it
    //      succeeds and real exhaustion surfaces later as SIGBUS or an OOM kill on first
    //      touch instead of as a "Heap exhausted" fault.
    //
    // Reason 2 has no clean fix -- touching pages to force the charge does not help,
    // because the failure arrives as a signal rather than an error code. Treat it as a
    // known platform limitation, not as something the ladder can paper over.
    // -------------------------------------------------------------------
    [[nodiscard]] bool grow_to_fit(size_t total) noexcept {
        size_t needed = used() + total;
        if (needed <= semi_capacity_)
            return true;
        size_t new_cap = semi_capacity_;
        while (new_cap < needed && new_cap < MAX_SEMI)
            new_cap *= 2;
        if (new_cap > MAX_SEMI)
            new_cap = MAX_SEMI;
        if (needed > new_cap)
            return false;
        stats_.grow_events++; // measurement-only
        return commit_both(new_cap);
    }

    // -------------------------------------------------------------------
    // collect -- full stop-the-world Cheney copying cycle.
    // Returns the number of bytes reclaimed (from-space used minus survivors).
    // -------------------------------------------------------------------
    size_t collect(Context* ctx) noexcept {
        const auto   t0        = std::chrono::steady_clock::now(); // measurement-only
        const size_t from_used = used();

        // Flip: the other semispace becomes to-space, empty.
        active_ ^= 1;
        bump_ = space_[active_];

        // (a) Evacuate every root in place.
        forward_frame(ctx->window_ptr, ctx->current_frame_size);

        ReturnFrame* frame      = ctx->ret_stack_ptr;
        uint8_t*     size_entry = ctx->frame_size_ptr;
        while (frame != ctx->ret_stack_base) {
            --frame;
            --size_entry;
            forward_frame(frame->old_window, *size_entry);
        }

        for (Value* slot : global_roots_)
            forward(slot);

        forward_vm_external_roots(ctx, *this);

        // (b) Cheney scan: to-space is the implicit worklist. Newly copied
        //     objects appended by forward() extend [space_[active_], bump_),
        //     so this loop naturally drains the transitive closure.
        std::byte* scan = space_[active_];
        while (scan < bump_) {
            auto* obj = reinterpret_cast<GcObject*>(scan);
            if (obj->kind == GcObject::KIND_ARRAY  ||
                obj->kind == GcObject::KIND_OBJECT ||
                obj->kind == GcObject::KIND_CLOSURE ||
                obj->kind == GcObject::KIND_MAP    ||
                obj->kind == GcObject::KIND_VEC    ||
                obj->kind == GcObject::KIND_BYTES) {
                // KIND_OBJECT (fixed-shape struct), KIND_CLOSURE (by-value captures),
                // KIND_MAP (header slots {backing, count, used}), KIND_VEC (header slots
                // {backing, count}) and KIND_BYTES (header slots {backing, count}) all
                // have an all-Value payload, so they are scanned identically to a
                // KIND_ARRAY -- forward every slot. For KIND_MAP / KIND_VEC / KIND_BYTES
                // that forwards the `backing` pointer (slot 0); the Int slot(s) forward
                // as no-ops, and the backing itself -- an ordinary KIND_ARRAY, or a
                // KIND_STRING for KIND_BYTES (moved as opaque bytes) -- is handled when
                // the walk reaches it. (The type id / function id lives in the header
                // `_pad`, which rides in forward()'s memcpy and is never a Value slot,
                // so it is untouched here.)
                Value*         s = obj->slots();
                const uint32_t n = obj->slot_count();
                for (uint32_t i = 0; i < n; ++i)
                    forward(&s[i]);
            }
            // KIND_STRING: raw bytes, no outgoing references.
            scan += obj->padded_total_bytes();
        }

#ifndef NDEBUG
        // Poison the abandoned from-space. A heap Value captured in a non-root
        // location across this collection is now a stale from-space pointer;
        // poisoning makes dereferencing it read obvious garbage (or fault)
        // immediately instead of returning plausible stale data. Debug-only.
        std::memset(space_[active_ ^ 1], 0xDD, from_used);
        // Prove the survivors form a well-formed graph (structure + interior
        // pointers). Reads only to-space, so ordering vs. the poison above is
        // irrelevant. Debug-only; aborts via assert on any inconsistency.
        verify_heap();
#endif

        const size_t survivors = used();
        maybe_grow(survivors);

        // Measurement-only accounting (behavior-preserving). See GcStats.
        const auto     t1 = std::chrono::steady_clock::now();
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        stats_.collections++;
        stats_.from_used_sum += from_used;
        stats_.survivors_sum += survivors;
        stats_.gc_ns_total   += ns;
        if (ns > stats_.gc_ns_max) stats_.gc_ns_max = ns;

        return from_used - survivors;
    }

    [[nodiscard]] size_t used()     const noexcept {
        return static_cast<size_t>(bump_ - space_[active_]);
    }
    [[nodiscard]] size_t capacity() const noexcept { return semi_capacity_; }
    [[nodiscard]] size_t free()     const noexcept { return semi_capacity_ - used(); }

#ifndef NDEBUG
    // -------------------------------------------------------------------
    // verify_roots -- Debug-only check that every REGISTER ROOT the collector
    // would scan actually denotes a live object. The complement of verify_heap:
    // that one walks the heap and can therefore only ever see a bad pointer that
    // is reachable as an interior slot of a surviving object -- a bad ROOT is
    // structurally invisible to it, because the collector has already read the
    // root by the time anything is in to-space.
    //
    // Scans exactly what collect() scans (mirror of the root phase there): the
    // current frame [0, current_frame_size) plus every saved frame via the return
    // stack and its parallel frame-size array. For each isPtr() slot the target
    // must be a well-formed header in the ACTIVE semispace: in range, 8-aligned,
    // valid kind, and Value-aligned payload for the slot-carrying kinds. Slots
    // never written read as all-zero bits, which is a `double` and not a pointer,
    // so untouched frame registers are skipped for free.
    //
    // Called from run_switch's SYNC_TO_CTX() so it fires at EVERY safepoint --
    // including any future allocating opcode, without maintaining a call-site
    // list. Deliberately ROOT-ONLY (O(live frame slots)), not verify_heap-strength
    // (O(live heap)): a whole-heap walk at every allocation is too slow even for
    // Debug (the per-collection verify_heap already shows up in Debug GC time).
    // Allocation-free and noexcept, so it cannot perturb the collector.
    // -------------------------------------------------------------------
    void verify_roots(const Context* ctx) const noexcept {
        if (!ctx) return;
        verify_frame(ctx->window_ptr, ctx->current_frame_size);

        const ReturnFrame* frame      = ctx->ret_stack_ptr;
        const uint8_t*     size_entry = ctx->frame_size_ptr;
        while (frame != ctx->ret_stack_base) {
            --frame;
            --size_entry;
            verify_frame(frame->old_window, *size_entry);
        }
    }
#endif // NDEBUG

    // -------------------------------------------------------------------
    // forward -- evacuate the object referenced by *slot (if any) to to-space
    // and rewrite *slot to the survivor's new payload address. Idempotent per
    // object via the FORWARDED marker, so shared/cyclic references converge.
    //
    // Public because external strong-root providers (e.g. StringInterner)
    // must forward the pointer slots they own through the same path.
    // -------------------------------------------------------------------
    [[msvc::forceinline]] void forward(Value* slot) noexcept {
        if (!slot->isPtr())
            return; // immediates, Bool/Nil, native-fn TAG_INT: not heap pointers

        GcObject* obj = GcObject::from_slots(slot->asPtr());

        if (obj->color == GcObject::FORWARDED) {
            // Already copied -- size holds the to-space offset of the new header.
            auto* moved = reinterpret_cast<GcObject*>(space_[active_] + obj->size);
            *slot = Value::fromPtr(moved->payload());
            return;
        }

        // Copy the exact object bytes, but advance the cursor by the padded
        // footprint so the next copied header stays 8-aligned (see
        // padded_total_bytes()). dst is therefore always 8-aligned, so the
        // FORWARDED offset recorded below is too.
        const size_t copy_bytes = obj->total_bytes();
        const size_t footprint  = obj->padded_total_bytes();
        auto* dst = reinterpret_cast<GcObject*>(bump_);
        std::memcpy(dst, obj, copy_bytes);
        bump_ += footprint;

        // Leave a forwarding record in the abandoned from-space header.
        obj->color = GcObject::FORWARDED;
        obj->size  = static_cast<uint32_t>(
            reinterpret_cast<std::byte*>(dst) - space_[active_]);

        *slot = Value::fromPtr(dst->payload());
    }

    // -------------------------------------------------------------------
    // debug_dump -- walk the active semispace and print one line per live
    // object: index, byte offset, kind, exact payload size, struct type id
    // (KIND_OBJECT), and a short payload preview (string bytes, or the first
    // few Value slots). A development aid ONLY -- never on the hot path. It
    // reuses padded_total_bytes() as the stride, exactly like collect()'s
    // Cheney scan, so the walk stays consistent with the collector. The
    // register-window counterpart is vm_dbg::dump_registers (vmcore/Debug.h).
    // -------------------------------------------------------------------
    void debug_dump(std::ostream& os) const {
        os << std::format("heap: used={} B, capacity={} B, global_roots={}\n",
            used(), semi_capacity_, global_roots_.size());
        const std::byte* scan = space_[active_];
        size_t idx = 0;
        while (scan < bump_) {
            const auto* obj = reinterpret_cast<const GcObject*>(scan);
            const size_t off = static_cast<size_t>(scan - space_[active_]);
            switch (obj->kind) {
            case GcObject::KIND_STRING: {
                const uint32_t len = obj->string_length();
                const std::string_view sv(obj->bytes(), len);
                constexpr uint32_t CAP = 40;
                os << std::format("  [{}] @{} STRING size={} \"{}{}\"\n",
                    idx, off, obj->size, sv.substr(0, CAP), len > CAP ? "..." : "");
                break;
            }
            case GcObject::KIND_ARRAY:
            case GcObject::KIND_OBJECT:
            case GcObject::KIND_CLOSURE:
            case GcObject::KIND_MAP:
            case GcObject::KIND_VEC:
            case GcObject::KIND_BYTES: {
                const uint32_t n   = obj->slot_count();
                const char*    tag = (obj->kind == GcObject::KIND_OBJECT)  ? "OBJECT"
                                   : (obj->kind == GcObject::KIND_CLOSURE) ? "CLOSURE"
                                   : (obj->kind == GcObject::KIND_MAP)     ? "MAP"
                                   : (obj->kind == GcObject::KIND_VEC)     ? "VEC"
                                   : (obj->kind == GcObject::KIND_BYTES)   ? "BYTES"
                                   :                                         "ARRAY";
                os << std::format("  [{}] @{} {} size={}", idx, off, tag, obj->size);
                if (obj->kind == GcObject::KIND_OBJECT)
                    os << std::format(" type_id={}", obj->object_type_id());
                else if (obj->kind == GcObject::KIND_CLOSURE)
                    os << std::format(" fn_id={}", obj->closure_fn_id());
                os << " slots=[";
                constexpr uint32_t CAP = 8;
                const uint32_t shown = n < CAP ? n : CAP;
                const Value*   s     = obj->slots();
                for (uint32_t i = 0; i < shown; ++i)
                    os << (i ? ", " : "") << std::format("{}", s[i]);
                if (n > shown) os << ", ...";
                os << "]\n";
                break;
            }
            default:
                os << std::format("  [{}] @{} kind={} size={} (unknown)\n",
                    idx, off, static_cast<unsigned>(obj->kind), obj->size);
                break;
            }
            scan += obj->padded_total_bytes();
            ++idx;
        }
    }

private:
    std::byte* space_[2];        // reserved semispace bases (stable for the Heap's lifetime)
    size_t     semi_capacity_;   // committed bytes per semispace (both kept equal)
    int        active_;          // index of the semispace we allocate from
    std::byte* bump_;            // allocation cursor inside space_[active_]

    std::vector<Value*> global_roots_;

    GcStats stats_{}; // measurement-only accounting; see stats() / reset_stats()

#ifndef NDEBUG
    // -------------------------------------------------------------------
    // verify_frame -- validate one register frame's worth of GC roots. The
    // per-frame worker behind verify_roots (see its comment for the rationale).
    // A slot is either not a pointer (immediates, and the all-zero bits of a
    // never-written committed page, which read as `double` 0.0) or must denote a
    // well-formed live header in the active semispace.
    // -------------------------------------------------------------------
    void verify_frame(const Value* win, uint8_t n) const noexcept {
        const std::byte* const lo  = space_[active_];
        const std::byte* const end = bump_;
        for (uint8_t i = 0; i < n; ++i) {
            if (!win[i].isPtr())
                continue;
            const auto* p = reinterpret_cast<const std::byte*>(win[i].asPtr());
            // Note `p <= end`, not `p < end`: a zero-length payload (an empty array, a
            // nullary struct) has its header at end-8 and therefore its payload pointer
            // exactly AT the bump cursor. That object is live and legal.
            assert(p >= lo + sizeof(GcObject) && p <= end &&
                   "verify_roots: register root outside the active semispace "
                   "(a stale pointer from a dead frame?)");
            assert((reinterpret_cast<uintptr_t>(p) % 8) == 0 &&
                   "verify_roots: register root not 8-aligned");
            const auto* obj = reinterpret_cast<const GcObject*>(p - sizeof(GcObject));
            assert(obj->kind <= GcObject::KIND_BYTES &&
                   "verify_roots: register root has an invalid kind");
            assert((obj->kind == GcObject::KIND_STRING || (obj->size % sizeof(Value)) == 0) &&
                   "verify_roots: register root's slot payload not Value-aligned");
            assert(p - sizeof(GcObject) + obj->padded_total_bytes() <= end &&
                   "verify_roots: register root's object runs past the bump cursor");
        }
    }

    // -------------------------------------------------------------------
    // verify_heap -- Debug-only post-collection consistency walk over the
    // active semispace. Complements the from-space poisoning: poisoning makes
    // a STALE pointer read garbage; this proves the SURVIVORS form a well-
    // formed graph. Two linear passes using padded_total_bytes() as the stride
    // -- the exact same walk as the Cheney scan and debug_dump(), so it can
    // never disagree with the collector. All failures are assert()s (abort in
    // Debug, compiled out in Release). Allocation-free and noexcept, so it
    // cannot perturb the collector.
    //
    //   Pass 1 (structure): valid kind (never FORWARDED in to-space), size
    //   sanity, in-bounds; the walk must land EXACTLY on bump_ -- a corrupt
    //   size lands it inside a padding gap or past the end (the sharpest check).
    //   Pass 2 (interior pointers): every isPtr() slot points at a valid live
    //   payload start (in [base+8, bump_), 8-aligned). Catches a missed root /
    //   un-forwarded pointer, which would point into from-space or off-heap.
    //
    // Pointer validation is range + 8-alignment: allocation-free and strong in
    // practice (the VM has no interior pointers, and a correct collect yields
    // only payload starts). The exact-membership upgrade (record every payload
    // offset in pass 1, binary-search in pass 2) is the strengthening path if a
    // wrong-but-in-range pointer ever needs catching.
    // -------------------------------------------------------------------
    void verify_heap() const noexcept {
        const std::byte* const lo  = space_[active_];
        const std::byte* const end = bump_;
        const std::byte* const cap = space_[active_] + semi_capacity_;

        // Pass 1 -- structure + exact-fit walk.
        const std::byte* scan = lo;
        while (scan < end) {
            const auto* obj = reinterpret_cast<const GcObject*>(scan);
            assert(obj->kind <= GcObject::KIND_BYTES &&
                   "verify_heap: invalid/FORWARDED kind in to-space");
            if (obj->kind == GcObject::KIND_STRING) {
                assert(obj->size >= 1 &&
                       "verify_heap: KIND_STRING payload must include trailing NUL");
            } else {
                assert((obj->size % sizeof(Value)) == 0 &&
                       "verify_heap: slot payload not Value-aligned");
            }
            const size_t stride = obj->padded_total_bytes();
            assert(scan + stride <= cap &&
                   "verify_heap: object runs past committed capacity");
            scan += stride;
        }
        assert(scan == end &&
               "verify_heap: object walk did not land exactly on bump_ (size/stride corruption)");

        // Pass 2 -- interior pointers land on a valid live payload start.
        const std::byte* const payload_lo = lo + sizeof(GcObject);
        scan = lo;
        while (scan < end) {
            const auto* obj = reinterpret_cast<const GcObject*>(scan);
            if (obj->kind != GcObject::KIND_STRING) {
                const Value*   s = obj->slots();
                const uint32_t n = obj->slot_count();
                for (uint32_t i = 0; i < n; ++i) {
                    if (!s[i].isPtr())
                        continue;
                    const auto* p = reinterpret_cast<const std::byte*>(s[i].asPtr());
                    assert(p >= payload_lo && p < end &&
                           "verify_heap: interior pointer outside the active semispace");
                    assert((reinterpret_cast<uintptr_t>(p) % 8) == 0 &&
                           "verify_heap: interior pointer not 8-aligned");
                }
            }
            scan += obj->padded_total_bytes();
        }
    }
#endif // NDEBUG

    // Forward every heap pointer in a register frame's live slot range.
    void forward_frame(Value* win, uint8_t n) noexcept {
        for (uint8_t i = 0; i < n; ++i)
            forward(&win[i]);
    }

    // Total bytes (header + payload) for an object -- the single source of the
    // object-size arithmetic shared by alloc_* and the grow/reserve helpers.
    // total_for_slots is 8 + 8*n -- already a multiple of 8, so no padding is
    // needed (kept explicit for symmetry with total_for_bytes).
    [[nodiscard]] static constexpr size_t total_for_slots(uint32_t n_slots) noexcept {
        return GcObject::align_up_8(sizeof(GcObject) + static_cast<size_t>(n_slots) * sizeof(Value));
    }
    // Strings have odd-length payloads, so the footprint is padded up to 8 to
    // keep the following object's header 8-aligned. `size` stays exact.
    [[nodiscard]] static constexpr size_t total_for_bytes(uint32_t n_bytes) noexcept {
        return GcObject::align_up_8(sizeof(GcObject) + static_cast<size_t>(n_bytes));
    }

    // ---- Memory management (VirtualAlloc on Windows, mmap on POSIX) ---------

    static std::byte* reserve_semi() {
#ifdef _WIN32
        void* p = VirtualAlloc(nullptr, MAX_SEMI, MEM_RESERVE, PAGE_READWRITE);
        if (!p) throw std::runtime_error("Heap: VirtualAlloc reserve failed");
#else
        void* p = mmap(nullptr, MAX_SEMI, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) throw std::runtime_error("Heap: mmap reserve failed");
#endif
        return static_cast<std::byte*>(p);
    }

    static void release_semi(std::byte* base) noexcept {
        if (!base) return;
#ifdef _WIN32
        VirtualFree(base, 0, MEM_RELEASE);
#else
        munmap(base, MAX_SEMI);
#endif
    }

    // Commit [0, new_cap) in BOTH semispaces (idempotent on already-committed
    // pages) and publish the new capacity. Returns false on failure, leaving
    // semi_capacity_ unchanged.
    [[nodiscard]] bool commit_both(size_t new_cap) noexcept {
        assert(new_cap <= MAX_SEMI);
        for (int i = 0; i < 2; ++i) {
#ifdef _WIN32
            if (!VirtualAlloc(space_[i], new_cap, MEM_COMMIT, PAGE_READWRITE))
                return false;
#else
            if (mprotect(space_[i], new_cap, PROT_READ | PROT_WRITE) != 0)
                return false;
#endif
        }
        semi_capacity_ = new_cap;
        return true;
    }

    // Occupancy-driven growth after a collection.
    void maybe_grow(size_t live) noexcept {
        if (live <= static_cast<size_t>(GROW_THRESHOLD * semi_capacity_))
            return;
        size_t new_cap = semi_capacity_;
        while (live > static_cast<size_t>(GROW_THRESHOLD * new_cap) && new_cap < MAX_SEMI)
            new_cap *= 2;
        if (new_cap > MAX_SEMI)
            new_cap = MAX_SEMI;
        if (new_cap != semi_capacity_) {
            stats_.grow_events++; // measurement-only
            (void)commit_both(new_cap); // best-effort; a later alloc still checks room
        }
    }
};
