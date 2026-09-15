#pragma once

#include <ostream>
#include <iostream>     // std::cerr default sink
#include <format>
#include "Value.h"
#include "Context.h"
#include "Heap.h"

// =============================================================================
// Debug.h -- development-time inspection helpers (register window + heap dump).
//
// These are diagnostic aids, deliberately OFF the hot path: they are ordinary
// functions you call by hand from a debugger or a temporary printf-style probe
// while investigating a failure (a mis-forwarded pointer, a wrong register). They
// are always compiled (not #ifdef'd) so a Release repro can call them too; they
// cost nothing unless invoked. Every Value is rendered through the existing
// std::formatter<Value>, so the output matches the rest of the VM's tracing.
//
// Convention: the subject comes first, the output stream last and defaults to
// std::cerr, so `dump_registers(ctx)` / `dump_heap(heap)` are the common forms.
// The heap walk lives in Heap::debug_dump (it owns the semispace-scan invariants,
// reusing padded_total_bytes() exactly like collect()); dump_heap just forwards.
// =============================================================================
namespace vm_dbg {

// Dump `count` registers of a raw window as "rN = <Value>" lines.
inline void dump_registers(const Value* window, unsigned count, std::ostream& os = std::cerr) {
    os << std::format("registers ({}):\n", count);
    for (unsigned i = 0; i < count; ++i)
        os << std::format("  r{} = {}\n", i, window[i]);
}

// Dump the live register frame of a Context (its current_frame_size slots -- the
// exact range the GC scans as roots for that frame).
inline void dump_registers(const Context& ctx, std::ostream& os = std::cerr) {
    dump_registers(ctx.window_ptr, ctx.current_frame_size, os);
}

// Dump every live object in the heap's active semispace.
inline void dump_heap(const Heap& h, std::ostream& os = std::cerr) {
    h.debug_dump(os);
}

} // namespace vm_dbg
