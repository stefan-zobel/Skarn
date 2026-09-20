#pragma once
// =============================================================================
// Inline.h -- the compiler's inlining hints, and nothing else.
//
// WHY THIS IS ITS OWN HEADER RATHER THAN PART OF Platform.h. The hints are needed by the
// leaf headers -- Value.h, Instruction.h, HashingPolicy.h -- which are the hottest and
// most widely included code in the project: the NaN-boxing predicates and the bytecode
// decoders, reached by the compiler front end and every consumer of the value layer.
// Platform.h pulls in <windows.h> on Windows, so having those leaves include IT would
// drag the whole Win32 surface into every translation unit that merely wants a Value.
// This header includes nothing at all, so a leaf can take the hints without taking a
// platform with them. Platform.h includes it too, so anything that has Platform.h has
// these as well.
//
// WHY THE HINTS ARE MACROS AND NOT RAW ATTRIBUTES. [[msvc::forceinline]] is a scoped
// attribute that only MSVC honours: GCC and Clang parse it, ignore it, and warn
// ("scoped attribute directive ignored"). That is not a cosmetic difference on these
// particular functions -- an ignored hint on Value::isInt or on the instruction decoder
// is an inlining decision quietly handed back to the optimizer in the hottest path the
// VM has. Route every hint through these macros so each toolchain gets the spelling it
// actually acts on, and so a build that suppresses unknown-attribute warnings cannot
// hide the loss.
// =============================================================================

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
