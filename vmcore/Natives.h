#pragma once

// =============================================================================
// Natives.h -- the built-in native functions the language driver links into the
// VM's native registry (VM::native_table). The compiler lowers readFile /
// writeFile to a CALL_NATIVE carrying a NativeId (NativeRegistry.h); at run time
// the VM indexes native_table[id] to reach the C++ implementation here.
//
// The natives deal ONLY in heap-KIND values (Bytes / String / nil) -- never in
// struct-type ids: readFile returns a Bytes on success or a String (error
// message) on failure; writeFile returns nil on success or a String on failure.
// The COMPILER wraps that result in Ok/Err in bytecode (it knows the tids), so
// the VM stays ignorant of the prelude Result type.
// See "Native functions" in docs/VirtualMachine.md.
// =============================================================================

#include <vector>
#include "Value.h"

struct Context;
// Identical to the alias in Opcodes.h / VM.h / Execute.h (a typedef may be
// re-declared to the same type).
using NativeFunc = Value(*)(Value* args, uint8_t nargs, Context* ctx);

// Builds the default native registry, indexed by NativeId (NativeRegistry.h).
// The driver (skarnvm) passes the result to execute(). Defined in vmcore.cpp,
// where the heap / bytes helpers are in scope.
std::vector<NativeFunc> build_native_table();
