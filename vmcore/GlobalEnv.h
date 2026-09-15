#pragma once

#include <cstdint>
#include <cassert>
#include <memory>
#include <stdexcept>
#include "Value.h"
#include "Heap.h"

// =============================================================================
// GlobalEnv -- fixed-size array of global variable slots.
//
// Each slot is a Value at a stable address -- never moved, never reallocated.
// All slots are registered as GC roots with the Heap at construction time,
// so any heap pointer stored in a global variable is always reachable.
//
// Addressing:
//   Variables are addressed by a uint8_t index assigned at compile time.
//   LOAD_GLOBAL / STORE_GLOBAL opcodes use this index directly.
//   No name lookup at runtime.
//
// Lifetime:
//   The Heap must outlive the GlobalEnv -- the destructor calls
//   heap_.remove_root() for every slot, so the Heap must still exist
//   at that point. Declare GlobalEnv after Heap at every call site.
// =============================================================================
class GlobalEnv {
public:
    static constexpr uint16_t DEFAULT_MAX_GLOBALS = 256;

    explicit GlobalEnv(Heap& heap, uint16_t max_globals = DEFAULT_MAX_GLOBALS)
        : heap_(heap)
        , capacity_(max_globals)
        , slots_(std::make_unique<Value[]>(max_globals))
        , count_(0)
    {
        assert(max_globals > 0 && "GlobalEnv: capacity must be > 0");
        for (uint16_t i = 0; i < capacity_; ++i)
            heap_.add_root(&slots_[i]);
    }

    ~GlobalEnv() {
        for (uint16_t i = 0; i < capacity_; ++i)
            heap_.remove_root(&slots_[i]);
    }

    GlobalEnv(const GlobalEnv&)            = delete;
    GlobalEnv& operator=(const GlobalEnv&) = delete;
    GlobalEnv(GlobalEnv&&)                 = delete;
    GlobalEnv& operator=(GlobalEnv&&)      = delete;

    // Reserve the next free slot -- called by the compiler.
    [[nodiscard]] uint8_t define() {
        if (count_ >= capacity_)
            throw std::runtime_error("GlobalEnv: too many global variables");
        return static_cast<uint8_t>(count_++);
    }

    [[nodiscard]] Value  get(uint8_t index) const noexcept {
        assert(index < count_ && "GlobalEnv: get out of range");
        return slots_[index];
    }

    void set(uint8_t index, Value v) noexcept {
        assert(index < count_ && "GlobalEnv: set out of range");
        slots_[index] = v;
    }

    [[nodiscard]] Value* slot_ptr(uint8_t index) noexcept {
        assert(index < count_ && "GlobalEnv: slot_ptr out of range");
        return &slots_[index];
    }

    [[nodiscard]] uint16_t count()    const noexcept { return count_;    }
    [[nodiscard]] uint16_t capacity() const noexcept { return capacity_; }

private:
    Heap&                    heap_;
    uint16_t                 capacity_;
    std::unique_ptr<Value[]> slots_;
    uint16_t                 count_;
};
