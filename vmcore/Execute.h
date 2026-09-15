#pragma once

// =============================================================================
// Execute.h -- the public entry point of the vmcore runtime library.
//
// This is the seam between the VM runtime and everything that drives it (the
// test suite, the compiler, the language driver). It declares execute(); the
// single definition lives in vmcore.cpp, whose run_switch dispatcher inlines all
// opcode handling -- there are no non-inline handler symbols, so consumers just
// link vmcore and include this header.
// =============================================================================

#include <vector>
#include <string>
#include <cstdint>
#include <iosfwd>   // std::ostream (forward decl only) for the optional output sink

#include "VM_Resources.h"
#include "Heap.h"
#include "GlobalEnv.h"
#include "StringInterner.h"
#include "Value.h"
#include "StructType.h"
#include "FunctionTable.h"
#include "Fault.h"   // VmFault -- the structured serious-fault exception a driver can render

// Native function signature (identical to the alias in Opcodes.h / VM.h; a typedef
// may be re-declared to the same type). Declared here so execute() can take a
// native_table without Execute.h pulling in Opcodes.h. See NativeRegistry.h.
struct Context;
using NativeFunc = Value(*)(Value* args, uint8_t nargs, Context* ctx);

// Sets up VM_Resources + Context, runs the bytecode, and returns the resources
// so the caller can inspect registers after HALT. Default arguments live here
// (the sole declaration); the definition in vmcore.cpp must not repeat them.
VM_Resources execute(const std::vector<uint32_t>&    bytecode,
                     Heap*                           heap            = nullptr,
                     GlobalEnv*                      global_env      = nullptr,
                     StringInterner*                 string_interner = nullptr,
                     uint8_t                         top_frame_size  = 0,
                     const std::vector<Value>*       const_pool      = nullptr,
                     const std::vector<StructType>*  struct_types    = nullptr,
                     const std::vector<std::string>* string_literals = nullptr,
                     const std::vector<std::string>* atom_names      = nullptr,
                     const std::vector<FnInfo>*      fn_table        = nullptr,
                     std::ostream*                   out             = nullptr,
                     // Trait dispatch table (PROTO_RESOLVE): flat fn_id grid
                     // fn_id[method_id * width + dense_id]; see TypeUniverse.h /
                     // VM::trait_table. Placed last so existing positional callers
                     // (which pass up to `out`) are unaffected.
                     const std::vector<uint16_t>*    trait_table       = nullptr,
                     uint32_t                        trait_table_width = 0,
                     uint32_t                        trait_method_count = 0,
                     // Serious-fault diagnostics (Phase 1), consulted only at fault
                     // time to build a located message + stacktrace. line_table is
                     // per-instruction source lines (index == instruction index);
                     // function_names is per-function-id names. Both optional and
                     // placed last, so existing positional callers are unaffected.
                     const std::vector<uint32_t>*    line_table        = nullptr,
                     const std::vector<std::string>* function_names    = nullptr,
                     const std::vector<uint32_t>*    column_table      = nullptr,
                     // Native-function registry (CALL_NATIVE): a flat table of
                     // NativeFunc pointers indexed by the NativeId baked into the
                     // bytecode. When supplied, CALL_NATIVE resolves its operand as an
                     // id; when null, as a legacy baked pointer. Placed last, so
                     // existing positional callers are unaffected. See NativeRegistry.h.
                     const std::vector<NativeFunc>*  native_table      = nullptr,
                     // Process context for the args() native: the script's command-line
                     // arguments (those after the script path). Non-owning, non-GC; set
                     // by the driver (static_vmrun). Placed last, existing callers unaffected.
                     const std::vector<std::string>* script_args       = nullptr,
                     // Input sink for the stdin natives (readLine / readAllStdin): a
                     // caller-supplied std::istream, or std::cin by default. Non-owning,
                     // non-GC. Placed last, so existing positional callers are unaffected.
                     // See VM::in.
                     std::istream*                   in                = nullptr,
                     // Per-function-id owning module prefix (parallel to function_names), consulted
                     // only at fault time so a driver renders the caret against the RIGHT module
                     // source. Optional and placed last, so existing positional callers are unaffected.
                     const std::vector<std::string>* function_modules  = nullptr,
                     // Const-array literals (LOAD_CONST_ARRAY): one inner vector per `const NAME:
                     // Array[T] = [...]`, holding its scalar (Int/Double/Bool) element immediates.
                     // execute() builds each into a GC-rooted KIND_ARRAY once at setup. Optional and
                     // placed last, so existing positional callers are unaffected. See VM::const_array_pool.
                     const std::vector<std::vector<Value>>* const_arrays = nullptr);
