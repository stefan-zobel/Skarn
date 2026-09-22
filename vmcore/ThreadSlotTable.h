#pragma once

// =============================================================================
// ThreadSlotTable -- a fixed-capacity, lock-free map from a thread id to one pointer,
// whose LOOKUP is async-signal-safe.
//
// WHY THIS EXISTS. The POSIX fault path (`FaultSignals.h`) needs a signal handler to find
// the `Landing` frame armed by the thread it interrupted. A handler has no argument
// channel, so that frame has to be reachable through a rendezvous slot. A single global
// slot works for exactly one thread inside execute() at a time; a table keyed by thread id
// lifts that to N, which is what lets several VM instances run concurrently.
//
// WHY NOT thread_local -- the constraint that shapes everything here. `FaultSignals.h`
// documents it in full: Darwin has no local-exec TLS model, so every thread_local access
// goes through a TLV descriptor and a thread's FIRST access calls malloc. Doing that inside
// a SIGSEGV handler risks taking the very malloc lock the faulting thread is holding. So
// the lookup must reach the frame WITHOUT touching TLS, and that is precisely what an array
// of atomics scanned by thread id does.
//
// THE SPLIT THAT MAKES IT SAFE. `load` runs in signal context and does nothing but atomic
// loads and integer comparisons -- no call, no allocation, no lock, no TLS. `store` runs in
// NORMAL context only (from the arming RAII object), so it may CAS.
//
// PORTABLE ON PURPOSE. The key is a `uintptr_t`, not a `pthread_t`, and this header pulls in
// no POSIX declaration at all. That is deliberate: only the POSIX build uses it, but the
// Windows build can still COMPILE and TEST it, and the data structure is the part that can
// actually be got wrong. The POSIX caller casts `pthread_self()` -- the same cast-and-compare
// the fault handler already uses instead of `pthread_equal`, which is not on the POSIX
// async-signal-safe list.
//
// WRITE ORDER, AND WHY A TORN VIEW IS STILL CORRECT. A fault can in principle interrupt the
// owning thread mid-`store`, so every intermediate state a `load` could observe has to be a
// SAFE one. Both orders below are chosen so the only observable intermediate is
// `(tid matches, value == nullptr)`, which the caller reads as "this thread has no armed
// window" and handles exactly like an unarmed thread -- degraded, never wrong:
//   claim:   CAS tid 0 -> tid, THEN publish value
//   release: clear value to nullptr, THEN release tid to 0
// Releasing in the other order would let another thread claim the slot and publish its own
// value, which this thread's trailing clear would then destroy.
// =============================================================================

#include <atomic>
#include <cstdint>

// `Capacity` bounds how many threads may hold a window AT ONCE. Sized for isolates, which
// scale with CORE count -- not with connection count, which is what an event loop inside one
// instance is for. On overflow `store` returns false and the caller keeps its pre-existing
// no-classification fallback, so exceeding it degrades rather than corrupts.
template <typename T, unsigned Capacity = 64>
class ThreadSlotTable {
public:
    static constexpr uintptr_t FREE = 0;   // no thread owns this slot

    // ---- signal context ----------------------------------------------------
    // The armed pointer for `tid`, or nullptr if that thread has none. ASYNC-SIGNAL-SAFE:
    // atomic loads and integer comparisons only. Worst case is a scan of `Capacity` slots,
    // which is paid once per fault, not per instruction.
    T* load(uintptr_t tid) const noexcept {
        for (const Slot& s : slots_) {
            if (s.tid.load(std::memory_order_acquire) == tid)
                return s.value.load(std::memory_order_acquire);
        }
        return nullptr;
    }

    // ---- normal context only -----------------------------------------------
    // Publish `v` as `tid`'s armed pointer; `v == nullptr` releases the slot. Returns false
    // ONLY when a new slot was needed and the table is full. Callers never contend for the
    // same `tid` -- a thread stores only its own -- so the only contention is claiming a
    // free slot, which the CAS resolves.
    bool store(uintptr_t tid, T* v) noexcept {
        for (Slot& s : slots_) {                       // already ours? (re-arm / nesting)
            if (s.tid.load(std::memory_order_acquire) == tid) {
                s.value.store(v, std::memory_order_release);
                if (v == nullptr)                      // clear value BEFORE releasing tid
                    s.tid.store(FREE, std::memory_order_release);
                return true;
            }
        }
        if (v == nullptr)
            return true;                               // releasing a slot we do not hold: no-op

        for (Slot& s : slots_) {                       // claim a free one
            uintptr_t expected = FREE;
            if (s.tid.compare_exchange_strong(expected, tid,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
                s.value.store(v, std::memory_order_release);
                return true;
            }
        }
        return false;                                  // table full
    }

    static constexpr unsigned capacity() noexcept { return Capacity; }

private:
    struct Slot {
        std::atomic<uintptr_t> tid{ FREE };
        std::atomic<T*>        value{ nullptr };
    };

    static_assert(Capacity > 0, "a zero-capacity table can never arm anything");
    static_assert(std::atomic<uintptr_t>::is_always_lock_free,
                  "the thread-id load must be lock-free: it runs inside a signal handler");
    static_assert(std::atomic<T*>::is_always_lock_free,
                  "the value load must be lock-free: it runs inside a signal handler");

    Slot slots_[Capacity];
};
