#pragma once
// Platform.h -- cross-platform abstractions for vmcore.
// On Windows: pulls in real Windows/Winsock headers and MSVC attributes.
// On POSIX (macOS): stubs the small set of Windows APIs the VM uses
// internally so each call site keeps a single, readable form.

#ifdef _WIN32
// Guarded: a consumer that already defines either of these would otherwise get a
// macro-redefinition warning, which /W4 surfaces on every translation unit.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <stdexcept>
#  include <sys/mman.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  include <cerrno>
#  include <cstring>
#  include <cstdint>

// ---- SEH exception codes (used by RaiseException call sites) ----------------
inline constexpr unsigned long EXCEPTION_INT_DIVIDE_BY_ZERO  = 0xC0000094UL;
inline constexpr unsigned long EXCEPTION_ILLEGAL_INSTRUCTION  = 0xC000001DUL;
inline constexpr unsigned long EXCEPTION_ACCESS_VIOLATION     = 0xC0000005UL;
inline constexpr unsigned long EXCEPTION_NONCONTINUABLE       = 1UL;

// POSIX replacement for Windows SEH -- [[noreturn]] so callers are dead-ends.
[[noreturn]] inline void RaiseException(unsigned long code, unsigned long, unsigned long, const void*) {
    if (code == EXCEPTION_INT_DIVIDE_BY_ZERO)
        throw std::runtime_error("Division by zero");
    if (code == EXCEPTION_ILLEGAL_INSTRUCTION)
        throw std::runtime_error("Illegal opcode executed");
    throw std::runtime_error("Heap exhausted");
}

// ---- Socket compatibility aliases -------------------------------------------
using SOCKET = int;
static constexpr int INVALID_SOCKET  = -1;
static constexpr int SOCKET_ERROR    = -1;
static inline int closesocket(int s) { return ::close(s); }

#endif // _WIN32

// ---- Cross-platform compiler-hint macros ------------------------------------
// They live in their own leaf header so the hot leaf headers (Value.h, Instruction.h,
// HashingPolicy.h) can take the hints without taking <windows.h> with them. Included
// here as well, so everything that has Platform.h keeps having them.
#include "Inline.h"

// ---- Allocation-helper exception specification -------------------------------
// The allocation helpers in opcodes/op_{string,vec,map,bytes}.h signal heap exhaustion
// through RaiseException, and what that IS differs per platform:
//
//   Windows -- a genuine SEH raise. It unwinds to run_switch's __except WITHOUT ever
//              throwing a C++ exception, so the helpers really do not throw, and saying
//              so keeps unwind edges out of the enormous dispatch function. That is the
//              only reason the `noexcept` was ever there; it is not decoration.
//   POSIX   -- the stub at the top of this header THROWS std::runtime_error, so the same
//              helpers must NOT be noexcept or the throw would hit std::terminate.
//
// One specifier cannot be right for both, hence the macro. Apply it ONLY to helpers that
// can reach a RaiseException site: the non-allocating ones (vec_pop, map_get, str_eq, ...)
// keep their plain `noexcept` on every platform.
#ifdef _WIN32
#  define SKARN_ALLOC_NOEXCEPT noexcept
#else
#  define SKARN_ALLOC_NOEXCEPT
#endif
