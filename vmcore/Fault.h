#pragma once

#include <stdexcept>
#include <string>
#include <vector>
#include <cstdint>

// =============================================================================
// Fault.h -- VmFault: a serious VM runtime fault (Phase 1 serious-fault reporting),
// carried as a STRUCTURED exception so a driver can render source context (the
// offending line + a caret), not just print a flat string.
//
// It derives from std::runtime_error and its what() is the SAME located message the
// earlier flat throw produced ("<cause> at line N (in <fn>)" + "  called from ..."),
// so every existing `catch (const std::exception&)` site keeps working unchanged; a
// driver that wants source context downcasts to VmFault and reads `frames`.
//
// Thrown out-of-line by raise_located (Interpreter.h); caught at the single top-level
// handler in execute()'s caller / static_vmrun. There is NO in-script recovery in this phase.
// =============================================================================
struct VmFault : std::runtime_error {
    struct Frame {
        uint32_t    line = 0;      // 1-based source line, 0 = unknown
        uint32_t    col  = 0;      // 1-based column, 0 = unknown
        std::string function;      // enclosing function, already prettified; empty = <script>
        std::string module;        // owning module's mangle prefix ("" = root/entry), for a
                                   // per-module source caret; empty when no module table was supplied
    };

    std::string        cause;      // the "<what>" text without the location suffix
    std::vector<Frame> frames;     // [0] = the fault site, then its callers outward

    VmFault(std::string what_msg, std::string cause_, std::vector<Frame> frames_)
        : std::runtime_error(std::move(what_msg)),
          cause(std::move(cause_)),
          frames(std::move(frames_)) {}
};
