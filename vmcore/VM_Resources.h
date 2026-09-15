#pragma once

#include <stdexcept>
#include "Windows.h"
#include "Context.h"

// Owns all memory blocks the VM needs:
//   - Register stack:   Value slots
//   - Return stack:     ReturnFrame frames
//   - Frame-size stack: uint8_t   (parallel to the return stack)
//   - Closure stack:    Value      (parallel to the return stack) --
//       holds the SAVED caller closure per activation (Milestone A). Advances in
//       lockstep with the return/frame-size stacks; its live entries are scanned as
//       GC roots (see forward_vm_external_roots).
//
// GROWABLE: each region is VirtualAlloc-RESERVED at a large MAX up front
// (address space only, ~free) and COMMITTED incrementally, exactly like the GC heap's
// semispaces (Heap.h). The reserved BASE never moves, so window/rsp/old_window/csp and
// every GC root (register frames via the return stack; the closure stack) stay valid
// across growth, and committing more pages adds zero spurious roots (the collector
// walks only LIVE frames). All four regions use the same scheme so all bases are fixed;
// the frame-size + closure stacks therefore live in VirtualAlloc memory too (not new[]),
// since they must grow in lockstep with the return stack without moving.
//
// Growth is driven by run_switch's soft-check at the three non-tail call ops (via the
// non-owning VM::resources pointer). When that pointer is null (the hand-built Contexts
// in vm_tests), no growth happens: a write past the INITIAL commit hits reserved-but-
// uncommitted memory and raises the SAME EXCEPTION_ACCESS_VIOLATION as the old guard
// page -> "VM Stack Overflow". So INITIAL must equal the historical fixed sizes exactly.
class VM_Resources {
public:
    // INITIAL committed sizes -- IDENTICAL to the historical fixed capacities, so the
    // opt-out path (vm_tests' raw Contexts, which never enable growth) overflows at the
    // very same depth as before. The two limits scale TOGETHER: the return stack caps
    // activation depth, the register stack caps total live slots (~avg_frame_size x
    // depth). Small frames exhaust the return stack first; large frames the register
    // stack. (Tail recursion is already O(1) and never grows either.)
    static constexpr size_t REG_COUNT        = 131'072;   // 1 MiB of Value slots (initial)
    static constexpr size_t RET_FRAME_COUNT  = 16'384;    // 256 KiB of ReturnFrames (initial)

    // RESERVED maxima (address space only). ~64-128x the initial depth (~1M small
    // frames) -- far past any realistic non-tail recursion, incl. a self-hosting
    // parser/checker. frame-size + closure MUST reserve the same frame count as the
    // return stack (they are frame-indexed, lockstep). Total reserved VA ~= 153 MiB.
    static constexpr size_t REG_MAX_COUNT    = 16'777'216;   // 128 MiB of Value slots
    static constexpr size_t RET_MAX_FRAMES   = 1'048'576;    // 16 MiB of ReturnFrames

    static constexpr size_t REG_INIT_SIZE    = REG_COUNT      * sizeof(Value);
    static constexpr size_t RET_INIT_SIZE    = RET_FRAME_COUNT * sizeof(ReturnFrame);
    static constexpr size_t REG_MAX_SIZE     = REG_MAX_COUNT   * sizeof(Value);
    static constexpr size_t RET_MAX_SIZE     = RET_MAX_FRAMES  * sizeof(ReturnFrame);
    static constexpr size_t FSZ_MAX_SIZE     = RET_MAX_FRAMES  * sizeof(uint8_t);
    static constexpr size_t CLO_MAX_SIZE     = RET_MAX_FRAMES  * sizeof(Value);

    // A single trailing PAGE_NOACCESS page past the RESERVED end is defense-in-depth:
    // an overflow past the cap is caught proactively by run_switch's soft-check (a clean
    // located fault), but a stray write beyond the reserved range still faults here.
    static constexpr size_t GUARD_SIZE = 4096;   // x64/Windows page size
    static_assert(REG_INIT_SIZE % GUARD_SIZE == 0, "register stack initial must be page-aligned");
    static_assert(RET_INIT_SIZE % GUARD_SIZE == 0, "return stack initial must be page-aligned");
    static_assert(REG_MAX_SIZE  % GUARD_SIZE == 0, "register stack max must be page-aligned");
    static_assert(RET_MAX_SIZE  % GUARD_SIZE == 0, "return stack max must be page-aligned");

    VM_Resources() {
        reg_base_ = static_cast<Value*>      (reserve_with_guard(REG_MAX_SIZE, REG_INIT_SIZE));
        ret_base_ = static_cast<ReturnFrame*>(reserve_with_guard(RET_MAX_SIZE, RET_INIT_SIZE));
        frame_size_base_    = static_cast<uint8_t*>(reserve_with_guard(FSZ_MAX_SIZE, RET_FRAME_COUNT * sizeof(uint8_t)));
        closure_stack_base_ = static_cast<Value*>  (reserve_with_guard(CLO_MAX_SIZE, RET_FRAME_COUNT * sizeof(Value)));
        reg_committed_bytes_ = REG_INIT_SIZE;
        ret_committed_frames_ = RET_FRAME_COUNT;
    }

    ~VM_Resources() { release_all(); }

    // Move constructor -- transfers ownership, leaves source in a safe empty state
    VM_Resources(VM_Resources&& other) noexcept { steal_from(other); }

    VM_Resources& operator=(VM_Resources&& other) noexcept {
        if (this != &other) { release_all(); steal_from(other); }
        return *this;
    }

    // Copy is not allowed -- each instance owns unique VirtualAlloc'd memory
    VM_Resources(const VM_Resources&)            = delete;
    VM_Resources& operator=(const VM_Resources&) = delete;

    Value*       get_reg_base()         const { return reg_base_; }
    ReturnFrame* get_ret_base()         const { return ret_base_; }
    uint8_t*     get_frame_size_base()  const { return frame_size_base_; }
    Value*       get_closure_base()     const { return closure_stack_base_; }

    // Current committed ends (one-past-last usable slot/frame). run_switch's soft-check
    // compares the advancing window/rsp against these (minus a margin for the register
    // stack). Updated by grow_reg / grow_ret.
    Value*       reg_committed_end()    const { return reg_base_ + reg_committed_bytes_ / sizeof(Value); }
    ReturnFrame* ret_committed_end()    const { return ret_base_ + ret_committed_frames_; }

    // Reserved caps -- the true hard limit (a soft-check overflow aborts here).
    Value*       reg_reserved_end()     const { return reg_base_ + REG_MAX_COUNT; }
    ReturnFrame* ret_reserved_end()     const { return ret_base_ + RET_MAX_FRAMES; }

    // Does `addr` fall inside one of the four stack regions (its reserved range plus the
    // trailing guard page)? Used ONLY to classify an access violation: a fault in here is a
    // genuine stack overflow -- either past the committed end (reserved pages are NOACCESS,
    // which is what a hand-built Context without growth hits) or in the guard page past the
    // reserved end. A fault anywhere else is NOT a stack overflow, and saying so would
    // misdirect diagnosis (see the SEH filter in Interpreter.h). Address arithmetic only --
    // safe to call from an exception handler.
    [[nodiscard]] bool address_in_stacks(const void* addr) const noexcept {
        const auto a = reinterpret_cast<uintptr_t>(addr);
        auto in = [a](const void* base, size_t reserved_bytes) {
            const auto b = reinterpret_cast<uintptr_t>(base);
            return a >= b && a < b + reserved_bytes + GUARD_SIZE;
        };
        return in(reg_base_,           REG_MAX_SIZE)
            || in(ret_base_,           RET_MAX_SIZE)
            || in(frame_size_base_,    FSZ_MAX_SIZE)
            || in(closure_stack_base_, CLO_MAX_SIZE);
    }

    // Grow the register stack by (at least) one doubling step, clamped to REG_MAX_COUNT.
    // Returns the new committed end, or nullptr if already at the cap (or commit failed).
    Value* grow_reg() {
        if (reg_committed_bytes_ >= REG_MAX_SIZE) return nullptr;
        size_t next = reg_committed_bytes_ * 2;
        if (next > REG_MAX_SIZE) next = REG_MAX_SIZE;
        if (!commit_to(reg_base_, next)) return nullptr;
        reg_committed_bytes_ = next;
        return reg_committed_end();
    }

    // Grow the return stack by (at least) one doubling step, clamped to RET_MAX_FRAMES.
    // Commits ret + frame-size + closure by the SAME frame count (lockstep) BEFORE the
    // caller advances rsp/csp past the old commit -- else the GC scan of the closure
    // stack at the new depth would fault. Returns the new committed end, or nullptr at
    // the cap / on a commit failure (partial commits are harmless: unused, re-committed
    // idempotently next time).
    ReturnFrame* grow_ret() {
        if (ret_committed_frames_ >= RET_MAX_FRAMES) return nullptr;
        size_t next = ret_committed_frames_ * 2;
        if (next > RET_MAX_FRAMES) next = RET_MAX_FRAMES;
        if (!commit_to(ret_base_,           next * sizeof(ReturnFrame))) return nullptr;
        if (!commit_to(frame_size_base_,    next * sizeof(uint8_t)))     return nullptr;
        if (!commit_to(closure_stack_base_, next * sizeof(Value)))       return nullptr;
        ret_committed_frames_ = next;
        return ret_committed_end();
    }

private:
    Value*       reg_base_            = nullptr;
    ReturnFrame* ret_base_            = nullptr;
    uint8_t*     frame_size_base_     = nullptr;
    Value*       closure_stack_base_  = nullptr;
    size_t       reg_committed_bytes_  = 0;
    size_t       ret_committed_frames_ = 0;

    // Reserve `reserve_size` + one guard page (address space), commit `commit_size` at
    // the base. `commit_size` <= `reserve_size`, both page-aligned. The guard page sits
    // just past the reserved end.
    static void* reserve_with_guard(size_t reserve_size, size_t commit_size) {
        void* ptr = VirtualAlloc(nullptr, reserve_size + GUARD_SIZE, MEM_RESERVE, PAGE_NOACCESS);
        if (!ptr) throw std::runtime_error("VirtualAlloc(MEM_RESERVE) failed");
        if (commit_size &&
            !VirtualAlloc(ptr, commit_size, MEM_COMMIT, PAGE_READWRITE)) {
            VirtualFree(ptr, 0, MEM_RELEASE);
            throw std::runtime_error("VirtualAlloc(MEM_COMMIT) failed");
        }
        // The trailing guard page stays reserved-only (PAGE_NOACCESS) -- a stray write
        // there faults just like the reserved-but-uncommitted tail.
        return ptr;
    }

    // Commit the reserved region up to `total_bytes` from the base. MEM_COMMIT is
    // idempotent over already-committed pages, so committing the whole [base, total)
    // prefix each time is safe and simple. Returns false on failure.
    static bool commit_to(void* base, size_t total_bytes) {
        return VirtualAlloc(base, total_bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
    }

    void release_all() {
        if (reg_base_)           VirtualFree(reg_base_, 0, MEM_RELEASE);
        if (ret_base_)           VirtualFree(ret_base_, 0, MEM_RELEASE);
        if (frame_size_base_)    VirtualFree(frame_size_base_, 0, MEM_RELEASE);
        if (closure_stack_base_) VirtualFree(closure_stack_base_, 0, MEM_RELEASE);
    }

    void steal_from(VM_Resources& other) noexcept {
        reg_base_             = other.reg_base_;
        ret_base_             = other.ret_base_;
        frame_size_base_      = other.frame_size_base_;
        closure_stack_base_   = other.closure_stack_base_;
        reg_committed_bytes_  = other.reg_committed_bytes_;
        ret_committed_frames_ = other.ret_committed_frames_;
        other.reg_base_            = nullptr;
        other.ret_base_            = nullptr;
        other.frame_size_base_     = nullptr;
        other.closure_stack_base_  = nullptr;
        other.reg_committed_bytes_  = 0;
        other.ret_committed_frames_ = 0;
    }
};
