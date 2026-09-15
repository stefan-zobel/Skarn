#pragma once

#include <cstdint>

// =============================================================================
// FunctionTable.h -- FnInfo: one entry of the per-function side table (the
// "function table"), indexed by a 16-bit function id.
//
// This is the runtime source of truth an INDIRECT call needs. A static CALL bakes
// the callee's frame_size (= GC scan count = the instruction's window_size) into
// the call site at assemble time; an indirect call (CALL_INDIRECT), whose target
// is a runtime Value, cannot -- so it recovers frame_size AND the entry point from
// this table via the callee's function id. The id lives in a Func immediate
// (Value::fromFunc) for capture-free functions, or in a KIND_CLOSURE header for
// capturing closures.
//
// See "Calling convention" in docs/VirtualMachine.md.
//
//   - code_offset : entry point as an INSTRUCTION index (not bytes). The runtime
//                   computes ip = code_base + 4 * code_offset. Matches how the
//                   assembler measures branch/jump/call offsets (instruction units).
//   - frame_size  : the callee frame size == GC scan count. This is the value a
//                   static CALL carries in its window_size field.
//   - arity       : parameter count. Backs the Debug-only arity check at
//                   CALL_INDIRECT (a dynamic call can pass the wrong argument
//                   count; the real error is deferred to the error model).
//   - ncaptures   : number of capture slots (0 for capture-free functions; used by
//                   MAKE_CLOSURE / KIND_CLOSURE in Milestone A).
//   - self_slot   : the capture slot back-patched with the closure itself, for
//                   direct self-recursive lambdas (Milestone A6), or NO_SELF.
// =============================================================================
struct FnInfo {
    uint32_t code_offset;             // entry point as an instruction index
    uint8_t  frame_size;              // callee frame size == GC scan count
    uint8_t  arity;                   // parameter count (Debug arity check)
    uint8_t  ncaptures;               // capture slot count (0 = capture-free)
    uint8_t  self_slot;               // self-capture slot, or NO_SELF

    static constexpr uint8_t NO_SELF = 0xFF;
};
