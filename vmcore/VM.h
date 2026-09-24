#pragma once

#include <cstddef>
#include <cstdint>   // std::uint32_t for the trait-table dimensions
#include <string>    // std::string for the fault-time function-name table (function_names)
#include <iosfwd>    // std::ostream / std::istream (forward decls only) for the I/O sinks
#include "Value.h"   // current_closure is a by-value Value member (needs the complete type)

class Heap;
class GlobalEnv;
class StringInterner;
class VM_Resources;
struct Context;
struct StructType;
struct FnInfo;
struct NetRegistry;  // opaque per-execution TCP socket registry (defined in vmcore.cpp)
struct IsolateLocal; // this execution's place among the tasks and actors of its world (vmcore.cpp)
struct ProgramImage; // the read-only program an execute() runs (Execute.h)

// Native function signature (identical to the alias in Opcodes.h; a typedef may be
// re-declared to the same type, so both headers can appear in one TU). Declared here
// so VM can hold a native_table without pulling in Opcodes.h. Context is incomplete
// (a pointer parameter), Value is complete via Value.h above.
using NativeFunc = Value(*)(Value* args, uint8_t nargs, Context* ctx);

// =============================================================================
// VM -- the "current VM instance": the long-lived shared state an execution
// needs, reached through a single Context.vm pointer instead of thread-local
// globals. Holds NON-OWNING pointers -- lifetime and destruction order stay
// with the caller exactly as before (the Heap must outlive the GlobalEnv,
// whose destructor calls Heap::remove_root). Any field may be null when the
// program does not use that facility (e.g. a pure-int program has no heap /
// globals / interner / pool).
//
// This replaces the g_current_global_env / g_current_string_interner /
// g_current_const_pool(+size) TLS hooks and the heap field formerly held
// directly in Context. Reaching heap as ctx->vm->heap costs one extra
// dereference, but heap access is off the per-instruction hot path (only
// ALLOC, string allocation and native collect touch it), and folding heap in
// keeps the single cheaply-freeable Context slot (ret_stack_limit) available
// for a future hot facility.
// =============================================================================
struct VM {
    Heap*             heap              = nullptr;
    GlobalEnv*        globals           = nullptr;
    StringInterner*   interner          = nullptr;
    const Value*      const_pool        = nullptr;
    std::size_t       const_pool_size   = 0;
    const StructType* struct_types      = nullptr; // flat registry, indexed by 16-bit type id
    std::size_t       struct_type_count = 0;       // companion size for a Debug bounds assert
    const FnInfo*     fn_table          = nullptr; // per-function side table, indexed by 16-bit fn id;
    std::size_t       fn_table_size     = 0;       // read by LOAD_FN / CALL_INDIRECT (Debug bounds assert)
    // Trait dispatch table (PROTO_RESOLVE): a flat fn_id[method_id * width + dense_id]
    // grid, read-only at run time. Cell TRAIT_METHOD_NONE (0xFFFF) = the receiver's
    // type does not implement the method -> trap. NOT a GC root (function ids, no heap
    // pointers) and immutable, so it rides here like fn_table, off the hot path. width
    // = BUILTIN_COUNT + struct_type_count (see TypeUniverse.h); all null/0 when the
    // program declares no traits.
    const uint16_t*   trait_table         = nullptr;
    std::uint32_t     trait_table_width   = 0;      // dense-id dimension (row stride)
    std::uint32_t     trait_method_count  = 0;      // number of method rows (bounds check)
    // Native-function registry (CALL_NATIVE). A flat table of NativeFunc pointers,
    // indexed by the NativeId the compiler bakes into the bytecode (LOAD_CONST id +
    // CALL_NATIVE). NON-owning, immutable at run time, NOT a GC root (function
    // pointers, no heap). When PRESENT, CALL_NATIVE treats its operand register as an
    // id and indexes this table; when NULL, it treats the operand as a raw baked
    // pointer -- the legacy in-process path used only by vm_tests' GC helpers
    // (never serialized, so inconsequential). The two never mix within one
    // execute(). See "Native functions" in docs/VirtualMachine.md and NativeRegistry.h.
    const NativeFunc* native_table        = nullptr;
    std::size_t       native_table_size   = 0;
    const Value*      string_pool       = nullptr; // interned string literals (LOAD_STR); GC-ROOTED,
    std::size_t       string_pool_size  = 0;       // unlike const_pool -- its slots are forwarded
    // Pre-built const-array literals (LOAD_CONST_ARRAY): one KIND_ARRAY per `const NAME:
    // Array[T] = [...]`, materialized once at execute() setup and GC-ROOTED like string_pool
    // (a heap ptr can't ride the pointer-free const_pool). Null when the program has none.
    const Value*      const_array_pool      = nullptr;
    std::size_t       const_array_pool_size = 0;
    // TO_STRING's pre-interned display strings (bounded, compile-time-known sets),
    // materialized once at execute() setup and GC-ROOTED like string_pool. atom_pool
    // is indexed by atom id and holds ":name"; str_true/str_false/str_nil are the
    // three constant names. All null when the program uses no TO_STRING.
    const Value*      atom_pool         = nullptr; // atom id -> interned ":name"
    std::size_t       atom_pool_size    = 0;
    const Value*      str_true          = nullptr; // "true"
    const Value*      str_false         = nullptr; // "false"
    const Value*      str_nil           = nullptr; // "nil"
    // Closure activation state (Milestone A). The parallel closure stack (owned by
    // VM_Resources) holds the SAVED caller closure per activation, lockstep with the
    // return stack -- its live depth is the return-stack depth, so the collector
    // scans closure_stack_base[0..ret_depth) as roots. current_closure is the
    // innermost activation's closure (a KIND_CLOSURE heap object) or Undefined for a
    // non-closure (top-level / named-fn / capture-free) activation. It lives here
    // rather than register-resident in run_switch so the moving collector always sees
    // a coherent value and rewrites it in place -- no sync/reload dance; closure
    // machinery is off the per-instruction hot path (only CALL/RET/LOAD_CAPTURE
    // touch it), exactly the trade the "Reviewer notes  note makes for heap.
    Value*            closure_stack_base = nullptr;
    Value             current_closure;             // default-constructs to Undefined
    // Growable-stack driver. Non-owning back-pointer to the VM_Resources
    // that owns the register/return/frame-size/closure stacks. NOT a GC root. When set
    // (execute() sets it), run_switch's soft-check at the three non-tail call ops grows
    // the stacks on demand via resources->grow_reg()/grow_ret(), reading the current
    // committed ends through resources->reg_committed_end()/ret_committed_end(). When
    // null (the hand-built Contexts in vm_tests), growth is disabled: the stacks stay at
    // their INITIAL commit and an overflow faults on reserved-but-uncommitted memory,
    // exactly as the old fixed guard page did. Off the per-instruction hot path (only the
    // three call ops read it).
    VM_Resources*     resources          = nullptr;
    // Output sink for PRINT / PRINTLN. Non-owning; NOT a GC root (a raw ostream, not
    // a Value). execute() wires it to &std::cout by default, but a caller (tests, a
    // future REPL/embedding) can point it at any std::ostream to capture output.
    std::ostream*     out               = nullptr;
    // Input sink for the stdin natives (readLine / readAllStdin). Non-owning; NOT a GC
    // root (a raw istream, not a Value). execute() wires it to &std::cin by default, but
    // a caller (tests, a future REPL/embedding) can point it at any std::istream to feed
    // input (e.g. an istringstream), which is exactly what makes the stdin natives testable.
    std::istream*     in                = nullptr;
    // Process context: the script's command-line arguments (those AFTER the script path),
    // for the args() native. Non-owning; NOT a GC root (host std::strings, copied into
    // fresh heap strings on demand). Set by the driver (skarnvm); null => no args (empty).
    const std::vector<std::string>* script_args = nullptr;
    // TCP socket registry for the std::net natives (tcpConnect / tcpListen / ...). An opaque,
    // per-execution table of open OS SOCKET handles (integers, NOT Values) -> NOT a GC root and
    // off the hot path. A socket is exposed to Skarn as a small Int DESCRIPTOR (an index here),
    // never a raw handle. execute() points this at a stack-local NetRegistry whose destructor
    // closes any socket still open at return (the safety net; scripts should tcpClose explicitly).
    // execute() always wires this; null only in the hand-built Contexts of vm_tests (which use no
    // net native), where a net native would return an "invalid socket" error rather than crash.
    NetRegistry* net = nullptr;
    // Tasks and actors. `image` is the program this execute() runs. `isolate` is this execution's
    // place in its WORLD -- the tasks and actors started, directly or not, under one root
    // execute(), which owns the world and waits for all of them before it returns (see World in
    // vmcore.cpp). `task_input` is the COPIED argument when this execute() IS a task or an actor
    // (null otherwise); only rawTaskInput reads it. None are GC roots: ids and bytes, no Values.
    const ProgramImage*  image           = nullptr;
    IsolateLocal*        isolate         = nullptr;
    const std::uint8_t*  task_input      = nullptr;
    std::size_t          task_input_size = 0;
    // Where the task's entry stub begins (an instruction index; 0 = not a task). The stub is not
    // program code, so a fault trace leaves out the frames that return into it.
    std::uint32_t        task_entry_pc   = 0;
    // Serious-fault diagnostics (Phase 1), consulted ONLY at fault time (cold path) by
    // raise_located to build a located message + stacktrace. None are GC roots or touch
    // the hot path. code_base is the bytecode origin (instruction 0), so any ip maps to
    // an instruction index (ip - code_base)/4; line_table[index] is its source line
    // (0 = unknown). function_names[fn id] names the enclosing function for a trace
    // frame (empty when no diagnostics were supplied). All null/0 => "line ?" fallback.
    const uint8_t*        code_base           = nullptr;
    const std::uint32_t*  line_table          = nullptr;
    std::size_t           line_table_size     = 0;
    const std::uint32_t*  column_table        = nullptr; // parallel to line_table, for the caret column
    std::size_t           column_table_size   = 0;
    const std::string*    function_names      = nullptr; // index == function id
    std::size_t           function_names_size = 0;
    const std::string*    function_modules      = nullptr; // parallel to function_names: owning module prefix
    std::size_t           function_modules_size = 0;
};

// Forward all currently active external strong roots (for now: only interned
// strings, via ctx->vm->interner) in place: each owned Value* pointer slot is
// rewritten to its survivor address during a copying collection. Defined in
// vMachine.cpp, where VM and StringInterner are complete -- this keeps Heap.h
// ignorant of VM's layout (the decoupling seam Heap::collect calls through).
void forward_vm_external_roots(Context* ctx, Heap& heap) noexcept;
