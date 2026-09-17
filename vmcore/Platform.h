#pragma once
// Platform.h -- cross-platform abstractions for vmcore.
// On Windows: pulls in real Windows/Winsock headers and MSVC attributes.
// On POSIX (macOS, Linux): stubs the small set of Windows APIs the VM uses
// internally so each call site keeps a single, readable form.

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
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
#ifdef _MSC_VER
#  define SKARN_FORCEINLINE [[msvc::forceinline]]
#  define SKARN_NOINLINE    [[msvc::noinline]]
#elif defined(__GNUC__) || defined(__clang__)
#  define SKARN_FORCEINLINE [[gnu::always_inline]]
#  define SKARN_NOINLINE    [[gnu::noinline]]
#else
#  define SKARN_FORCEINLINE
#  define SKARN_NOINLINE
#endif
