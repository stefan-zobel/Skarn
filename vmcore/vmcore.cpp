// =============================================================================
// vmcore.cpp -- the VM runtime translation unit.
//
// Drives bytecode through the register-resident while{switch} interpreter
// (run_switch, Interpreter.h) and exposes execute() (declared in Execute.h).
// Every other project links this as a static library and reaches the runtime
// solely through execute().
//
// The threaded tail-call handlers and their dispatch_table are gone entirely:
// they were retired to a separate benchmark project (vm_bench) when vmcore moved
// to while{switch}, and that project was removed earlier. run_switch owns the
// SEH __try/__except frame that translates guard-page / illegal-instruction /
// divide-by-zero faults into std::runtime_error. See "Dispatch" in docs/VirtualMachine.md.
// =============================================================================

#include <stdexcept>
#include <vector>
#include <string>
#include <iostream>   // std::cout -- the default PRINT/PRINTLN sink
#include <fstream>    // readFile / writeFile natives
#include <iterator>   // std::istreambuf_iterator (readFile)
#include <chrono>     // millisTime native (wall-clock ms since epoch)
#include <cmath>      // std::math natives (sqrt/pow/sin/... + isnan/isinf)
#include <cstdlib>    // std::getenv / std::free
#include <filesystem> // fileExists / deleteFile natives
#include <thread>     // reader threads that drain a child process's stdout/stderr (rawRun)
#ifdef _WIN32
// Winsock MUST precede <windows.h> to avoid winsock.h/winsock2.h redefinition errors.
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#  include <windows.h>
#else
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <netdb.h>
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <cerrno>
#  include <cstring>    // std::strerror
#  include <time.h>     // clock_gettime
#endif
#include "Platform.h"   // cross-platform macros + POSIX socket/SEH stubs

#include "Instruction.h"
#include "Context.h"
#include "VM_Resources.h"
#include "Heap.h"
#include "GlobalEnv.h"
#include "StringInterner.h"
#include "VM.h"
#include "Execute.h"
#include "NativeRegistry.h" // NativeId ids for build_native_table()
#include "Natives.h"        // build_native_table() declaration
#include "Interpreter.h"   // run_switch: the register-resident while{switch} dispatcher

// ---- Portable socket type aliases -------------------------------------------
#ifdef _WIN32
using socket_t = SOCKET;
static constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
static void sock_close(socket_t s) { closesocket(s); }
#else
using socket_t = int;
static constexpr socket_t INVALID_SOCK = -1;
static void sock_close(socket_t s) { ::close(s); }
#endif

// TCP socket registry for the std::net natives -- defined here (before execute()) so execute() can
// hold a stack-local instance whose destructor closes any socket still open at teardown. A socket is
// exposed to Skarn as a small Int DESCRIPTOR (an index into `socks`), never a raw OS SOCKET (a 64-bit
// kernel handle that need not fit a 48-bit Int). NOT a GC root (integer handles, no Values). The
// tcp* native implementations live further down, next to build_native_table().
struct NetRegistry {
    std::vector<socket_t> socks;
    ~NetRegistry() {
        for (socket_t s : socks)
            if (s != INVALID_SOCK) sock_close(s);
    }
    int add(socket_t s) {
        for (size_t i = 0; i < socks.size(); ++i)
            if (socks[i] == INVALID_SOCK) { socks[i] = s; return static_cast<int>(i); }
        socks.push_back(s);
        return static_cast<int>(socks.size()) - 1;
    }
    socket_t get(int fd) const {
        if (fd < 0 || static_cast<size_t>(fd) >= socks.size()) return INVALID_SOCK;
        return socks[fd];
    }
    void drop(int fd) {
        if (fd >= 0 && static_cast<size_t>(fd) < socks.size()) socks[fd] = INVALID_SOCK;
    }
};

// =============================================================================
// execute -- sets up VM_Resources + Context, runs bytecode, returns resources
// so the caller can inspect registers after HALT. Declared in Execute.h (which
// carries the default arguments); this definition must not repeat them.
// =============================================================================
// RAII holder for a pre-built GC-rooted Value pool (the string-literal pool, the
// TO_STRING display-string pool, AND the const-array pool). Registers each slot as a
// GC root when filled and removes them on scope exit, so a caller-owned Heap never
// keeps a dangling root pointer into this (stack-local) vector after execute()
// returns -- including on the exception path out of the interpreter. The vector is
// sized once and never reallocates, so the registered &slot addresses stay stable.
namespace {
struct RootedValuePool {
    Heap*              heap = nullptr;
    std::vector<Value> slots;
    ~RootedValuePool() {
        if (heap)
            for (Value& s : slots)
                heap->remove_root(&s);
    }
};
} // namespace

VM_Resources execute(const std::vector<uint32_t>& bytecode,
                     Heap* heap,
                     GlobalEnv* global_env,
                     StringInterner* string_interner,
                     uint8_t top_frame_size,
                     const std::vector<Value>* const_pool,
                     const std::vector<StructType>* struct_types,
                     const std::vector<std::string>* string_literals,
                     const std::vector<std::string>* atom_names,
                     const std::vector<FnInfo>* fn_table,
                     std::ostream* out,
                     const std::vector<uint16_t>* trait_table,
                     uint32_t trait_table_width,
                     uint32_t trait_method_count,
                     const std::vector<uint32_t>* line_table,
                     const std::vector<std::string>* function_names,
                     const std::vector<uint32_t>* column_table,
                     const std::vector<NativeFunc>* native_table,
                     const std::vector<std::string>* script_args,
                     std::istream* in,
                     const std::vector<std::string>* function_modules,
                     const std::vector<std::vector<Value>>* const_arrays) {
    std::vector<uint32_t> padded = bytecode;
    padded.resize(bytecode.size() + 3,
        Instruction::J(static_cast<uint8_t>(OpCode::HALT)).raw);

    // The "current VM instance" -- gathers the shared state the execution needs
    // behind one pointer reached through Context.vm. This replaces the three TLS
    // hooks and their save/restore guards: because vm rides in the per-execution
    // Context, nesting is automatically correct without any global state. The
    // local outlives the run_switch() call below, so ctx.vm stays valid throughout.
    VM vm;
    vm.heap            = heap;
    vm.globals         = global_env;
    vm.interner        = string_interner;
    vm.const_pool        = const_pool ? const_pool->data() : nullptr;
    vm.const_pool_size   = const_pool ? const_pool->size() : 0;
    vm.struct_types      = struct_types ? struct_types->data() : nullptr;
    vm.struct_type_count = struct_types ? struct_types->size() : 0;
    vm.fn_table          = fn_table ? fn_table->data() : nullptr;
    vm.fn_table_size     = fn_table ? fn_table->size() : 0;
    vm.trait_table         = trait_table ? trait_table->data() : nullptr;
    vm.trait_table_width   = trait_table_width;
    vm.trait_method_count  = trait_method_count;
    vm.native_table        = native_table ? native_table->data() : nullptr;
    vm.native_table_size   = native_table ? native_table->size() : 0;
    vm.script_args         = script_args;   // process context for the args() native
    // TCP socket registry (std::net natives). A stack-local whose destructor closes any socket
    // still open when execute() returns (incl. the VmFault path) -- the safety net behind tcpClose.
    NetRegistry net_registry;
    vm.net                 = &net_registry;

    // PRINT / PRINTLN sink: caller-supplied stream, or std::cout by default. A caller
    // (tests, a future REPL) can capture output by passing its own std::ostream.
    vm.out               = out ? out : &std::cout;
    // stdin sink for readLine / readAllStdin: caller-supplied stream, or std::cin by
    // default. A caller (tests, a REPL) can feed input by passing its own std::istream.
    vm.in                = in ? in : &std::cin;
    // Serious-fault diagnostics (Phase 1): the code origin (instruction 0) so a fault
    // ip maps to an instruction index, plus the per-instruction line table and the
    // per-fn-id name table. Cold-path only (raise_located); not GC roots.
    vm.code_base            = reinterpret_cast<const uint8_t*>(padded.data());
    vm.line_table           = line_table ? line_table->data() : nullptr;
    vm.line_table_size      = line_table ? line_table->size() : 0;
    vm.column_table         = column_table ? column_table->data() : nullptr;
    vm.column_table_size    = column_table ? column_table->size() : 0;
    vm.function_names       = function_names ? function_names->data() : nullptr;
    vm.function_names_size  = function_names ? function_names->size() : 0;
    vm.function_modules      = function_modules ? function_modules->data() : nullptr;
    vm.function_modules_size = function_modules ? function_modules->size() : 0;

    // The constant pool is NOT scanned as a GC root, so under the copying
    // collector it must never hold a heap pointer -- an unforwarded TAG_PTR
    // there would dangle after the first collection. Today the pool only holds
    // immediates (doubles / full 64-bit Value bit patterns). If pooled heap
    // pointers are ever wanted, register the pool as a forwarded root source
    // (see Heap::forward / forward_vm_external_roots).
#ifndef NDEBUG
    if (const_pool) {
        for (const Value& v : *const_pool)
            assert(!v.isPtr() && "constant pool must not hold heap pointers");
    }
#endif

    VM_Resources res;
    // The parallel closure stack lives in VM_Resources; publish its base into the VM
    // so run_switch can push/pop saved caller closures and the collector can scan the
    // live entries (current_closure starts Undefined -- the top-level activation
    // captures nothing). See VM.h and forward_vm_external_roots below.
    vm.closure_stack_base = res.get_closure_base();
    // Enable growable stacks: run_switch's soft-check at the three non-tail call ops grows
    // the register/return stacks on demand via this back-pointer (reserve-big/commit-
    // incrementally, fixed bases). Only execute() sets it; the hand-built Contexts in
    // vm_tests leave vm.resources null, so their stacks stay at the INITIAL commit and an
    // overflow faults on reserved-but-uncommitted memory (the old fixed-cap behavior).
    vm.resources = &res;

    Context ctx;
    ctx.ip                 = reinterpret_cast<const uint8_t*>(padded.data());
    ctx.window_ptr         = res.get_reg_base();
    ctx.vm                 = &vm;
    ctx.ret_stack_base     = res.get_ret_base();
    ctx.ret_stack_ptr      = res.get_ret_base();
    // The Debug overflow assert at the three call ops (rsp < ret_stack_limit) means the
    // true RESERVED cap in the growable path -- the soft-check grows (or raises a clean
    // located "stack overflow") before rsp ever reaches it, so the assert never false-fires
    // when the return stack legitimately grows past its initial commit. (The vm_tests
    // helpers keep + RET_FRAME_COUNT -- their real, growth-disabled fault point.)
    ctx.ret_stack_limit    = res.get_ret_base() + VM_Resources::RET_MAX_FRAMES;
    ctx.frame_size_ptr     = res.get_frame_size_base();
    // Top-level frame size: it plays the same dual role as any function's frame
    // size -- the GC scan count for the top-level register frame AND the slide the
    // first CALL uses (op_call slides by the CALLER's frame size). It must therefore
    // be the boundary between the top-level's own locals and its outgoing-argument
    // window: cover every top-level register that can hold a heap pointer across a
    // CALL / GC-triggering allocation, and equal the register index where outgoing
    // arguments start. Defaults to 0 (top-level performs no CALL and holds no heap
    // pointers); callers pass an explicit value otherwise. There is no auto-computed
    // helper -- see "Calling convention" in docs/VirtualMachine.md.
    ctx.current_frame_size = top_frame_size;

    // Pre-intern string literals into a GC-ROOTED pool before running. Strings are
    // heap objects (TAG_PTR), so they cannot ride the pointer-free constant pool;
    // instead each literal text is interned once here and its pool slot registered
    // as a GC root, so the moving collector rewrites the pointer in place. LOAD_STR
    // then just indexes ctx->vm->string_pool. This runs AFTER ctx is fully set up,
    // because interning can trigger a collection that walks ctx's register frames
    // (harmless here: the fresh top frame holds only Undefined slots).
    RootedValuePool str_pool;
    if (string_literals && !string_literals->empty()) {
        assert(heap && string_interner &&
               "string literals require both a heap and a string interner");
        str_pool.heap = heap;
        str_pool.slots.resize(string_literals->size());  // fixed size -> stable &slot addresses
        for (Value& s : str_pool.slots)
            heap->add_root(&s);                           // root BEFORE interning can collect
        for (size_t i = 0; i < string_literals->size(); ++i)
            str_pool.slots[i] = string_interner->intern((*string_literals)[i], *heap, &ctx);
        vm.string_pool      = str_pool.slots.data();
        vm.string_pool_size = str_pool.slots.size();
    }

    // Pre-intern TO_STRING's display strings into a second GC-ROOTED pool: the atom
    // names (each prefixed with ':') indexed by atom id, plus the three constants
    // true/false/nil. These are bounded, compile-time-known sets, so they are
    // materialized once here -- TO_STRING then does a rooted-pool lookup for atoms /
    // bool / nil (no allocation); only its numeric branch allocates. Same rooting
    // discipline as the string-literal pool above. Built whenever a caller passes an
    // atom_names table (the compiler always does, possibly empty, so the true/false/nil
    // constants are available to every compiled program); legacy callers that pass
    // nullptr get no pool and can only TO_STRING numbers/strings.
    RootedValuePool atom_pool;
    if (atom_names && heap && string_interner) {
        const size_t n = atom_names->size();
        atom_pool.heap = heap;
        atom_pool.slots.resize(n + 3);                // [0,n) atoms, then true/false/nil
        for (Value& s : atom_pool.slots)
            heap->add_root(&s);                       // root BEFORE interning can collect
        for (size_t i = 0; i < n; ++i)
            atom_pool.slots[i] = string_interner->intern(":" + (*atom_names)[i], *heap, &ctx);
        atom_pool.slots[n + 0] = string_interner->intern("true",  *heap, &ctx);
        atom_pool.slots[n + 1] = string_interner->intern("false", *heap, &ctx);
        atom_pool.slots[n + 2] = string_interner->intern("nil",   *heap, &ctx);
        vm.atom_pool      = atom_pool.slots.data();
        vm.atom_pool_size = n;
        vm.str_true       = &atom_pool.slots[n + 0];
        vm.str_false      = &atom_pool.slots[n + 1];
        vm.str_nil        = &atom_pool.slots[n + 2];
    }

    // Build the const-array literals into a GC-ROOTED pool, once, before running. Each
    // const is one KIND_ARRAY whose elements are scalar immediates (Int/Double/Bool), so --
    // unlike the string pool -- there is NO per-element allocation and thus no GC mid-fill;
    // the only safepoint is the per-array alloc_slots_gc, and every already-built array is
    // held by its (rooted) pool slot across it, so the collector rewrites it in place.
    // LOAD_CONST_ARRAY then just indexes ctx->vm->const_array_pool. Same rooting discipline
    // as the string-literal pool. The shared single instance is sound because the checker
    // keeps a const array non-escaping (read-only).
    RootedValuePool carr_pool;
    if (const_arrays && !const_arrays->empty()) {
        assert(heap && "const arrays require a heap");
        carr_pool.heap = heap;
        carr_pool.slots.resize(const_arrays->size());   // fixed size -> stable &slot addresses
        for (Value& s : carr_pool.slots)
            heap->add_root(&s);                          // root BEFORE any alloc can collect
        for (size_t i = 0; i < const_arrays->size(); ++i) {
            const std::vector<Value>& elems = (*const_arrays)[i];
            GcObject* arrObj = heap->alloc_slots_gc(
                GcObject::KIND_ARRAY, static_cast<uint32_t>(elems.size()), &ctx);
            // Scalar immediates only -> no allocation between here and the last store, so
            // arrObj cannot move before we publish it into the rooted slot below.
            Value* dst = arrObj->slots();
            for (size_t j = 0; j < elems.size(); ++j)
                dst[j] = elems[j];
            carr_pool.slots[i] = Value::fromPtr(arrObj->payload());
        }
        vm.const_array_pool      = carr_pool.slots.data();
        vm.const_array_pool_size = carr_pool.slots.size();
    }

    run_switch(&ctx);
#ifdef VM_COUNT_OPS
    vm_prof::dump_op_counts();   // profiling build only; writes to stderr (see Interpreter.h)
#endif
    return res;
}

// The decoupling seam that keeps Heap.h ignorant of VM's layout: the collector
// calls this to forward external strong roots -- the active interner AND the
// closure activation state (Milestone A). The closure stack is lockstep with the
// return stack, so its live depth is exactly the return-stack depth; scanning
// [0..ret_depth) plus current_closure forwards every live closure reference in
// place (each captured heap pointer is then reached transitively by the Cheney
// scan of the KIND_CLOSURE payload).
void forward_vm_external_roots(Context* ctx, Heap& heap) noexcept {
    VM* vm = ctx->vm;
    if (!vm)
        return;
    if (vm->interner)
        vm->interner->forward_roots(heap);

    // The innermost activation's closure (Undefined for non-closure activations --
    // forward() skips it as a non-pointer).
    heap.forward(&vm->current_closure);

    // The saved caller closures on the parallel closure stack.
    if (vm->closure_stack_base) {
        const size_t ret_depth =
            static_cast<size_t>(ctx->ret_stack_ptr - ctx->ret_stack_base);
        for (size_t i = 0; i < ret_depth; ++i)
            heap.forward(&vm->closure_stack_base[i]);
    }
}

// =============================================================================
// Built-in native functions (the native registry -- see Natives.h /
// NativeRegistry.h). They return ONLY heap-KIND values (Bytes / String / nil),
// never struct-type ids; the compiler wraps the result in Ok/Err. A native is
// called at a GC safepoint (CALL_NATIVE SYNC'd the cursors), so it may allocate
// -- but it must materialize any argument bytes into a HOST buffer BEFORE the
// allocation, since a collection can relocate the argument objects.
// =============================================================================

// Allocate a KIND_STRING error message and return it as a Value. Used for both
// the "String = error" convention and argument-validation failures.
static Value native_make_error(Context* ctx, const std::string& msg) noexcept {
    GcObject* o = ctx->vm->heap->alloc_string_gc(msg, ctx);
    return Value::fromPtr(o->payload());
}

// readFile(path) -> Bytes (success) | String (error message).
static Value native_read_file(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 1 || !is_string(args[0]))
        return native_make_error(ctx, "readFile: path must be a string");
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());   // host copy, before any alloc
    }
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return native_make_error(ctx, "could not open file: " + path);
    const std::string contents((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    // Build a KIND_BYTES from the raw bytes. Allocates (safepoint), but `contents`
    // is a host buffer, stable across a collection.
    Value result;
    bytes_from_str(ctx, &result, contents);
    return result;
}

// writeFile(path, buffer) -> nil (success) | String (error message).
// v1: `buffer` must be a KIND_BYTES (a String source is deliberately NOT accepted
// yet.
static Value native_write_file(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 2 || !is_string(args[0]))
        return native_make_error(ctx, "writeFile: path must be a string");
    if (!args[1].isPtr() ||
        GcObject::from_slots(args[1].asPtr())->kind != GcObject::KIND_BYTES)
        return native_make_error(ctx, "writeFile: second argument must be a byte buffer");
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());
    }
    // Materialize the buffer's live bytes [0, count) into a host string BEFORE any
    // alloc / file op (a later error alloc could otherwise relocate the backing).
    std::string data;
    {
        GcObject*      hdr     = GcObject::from_slots(args[1].asPtr());
        GcObject*      backing = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
        const uint32_t count   = static_cast<uint32_t>(hdr->slots()[BYTES_SLOT_COUNT].asSigned48());
        data.assign(backing->bytes(), count);
    }
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return native_make_error(ctx, "could not open file: " + path);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!f)
        return native_make_error(ctx, "could not write file: " + path);
    return Value::fromNil();
}

// A monotonic high-resolution clock reading in nanoseconds.
static int64_t qpc_now_ns() noexcept {
#ifdef _WIN32
    static const LARGE_INTEGER freq = [] {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f;
    }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (c.QuadPart / freq.QuadPart) * 1000000000LL
         + ((c.QuadPart % freq.QuadPart) * 1000000000LL) / freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
#endif
}

// nanoTime() -> Int. A MONOTONIC counter, offset from the first call (Java
// System.nanoTime semantics: only differences are meaningful). The offset fits 48
// bit for ~39 h, then wraps via fromSigned48 (documented limit) -- absolute nanos
// since epoch would NOT fit, which is exactly why this is offset-from-start. Plain
// return kind -> no Ok/Err wrap. Does not allocate.
static Value native_nano_time(Value*, uint8_t, Context*) {
    static const int64_t origin = qpc_now_ns();
    return Value::fromSigned48(qpc_now_ns() - origin);
}

// millisTime() -> Int. Wall-clock milliseconds since the Unix epoch (~1.7e12 today,
// comfortably within MAX_48 ~ 1.4e14 -> good to ~year 6400). Plain return kind. Does
// not allocate.
static Value native_millis_time(Value*, uint8_t, Context*) {
    using namespace std::chrono;
    return Value::fromSigned48(static_cast<int64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count()));
}

// args() -> Array[String] (Plain -- no Ok/Err wrap). Builds a fresh KIND_ARRAY of the
// driver-supplied command-line args (host std::strings in VM::script_args, stable across
// a collection). Each element string is a separate allocation (a safepoint that can move
// the array), so the array is ROOTED via heap->add_root while it is being filled -- the
// collector then rewrites `arr` in place and the already-stored strings survive as its
// (scanned) slots. Unfilled slots are memset-0 (a double, not a pointer) -> GC-safe.
static Value native_args(Value*, uint8_t, Context* ctx) {
    Heap* heap = ctx->vm->heap;
    const std::vector<std::string>* a = ctx->vm->script_args;
    const uint32_t n = a ? static_cast<uint32_t>(a->size()) : 0u;
    GcObject* arrObj = heap->alloc_slots_gc(GcObject::KIND_ARRAY, n, ctx);
    Value arr = Value::fromPtr(arrObj->payload());
    if (n == 0) return arr;                       // empty array, no element allocations
    heap->add_root(&arr);                         // track moves during the string allocs
    for (uint32_t i = 0; i < n; ++i) {
        GcObject* s = heap->alloc_string_gc((*a)[i], ctx);   // may collect -> arr rewritten
        // No allocation between here and the store, so `s` cannot move before we root it
        // in the (up-to-date, via the root) array slot.
        GcObject::from_slots(arr.asPtr())->slots()[i] = Value::fromPtr(s->payload());
    }
    heap->remove_root(&arr);
    return arr;
}

// getEnv(name) -> String (present) | nil (absent). The compiler wraps this Some/None
// (NativeReturn::Option). Option has no error channel, so a non-string argument reads as None.
// Single heap alloc on the present path.
static Value native_get_env(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 1 || !is_string(args[0]))
        return Value::fromNil();
    std::string name;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        name.assign(o->bytes(), o->string_length());
    }
    std::string val;
#ifdef _WIN32
    char*  buf = nullptr;
    size_t sz  = 0;
    if (_dupenv_s(&buf, &sz, name.c_str()) != 0 || !buf) {
        std::free(buf);
        return Value::fromNil();
    }
    val.assign(buf, sz > 0 ? sz - 1 : 0);  // sz includes trailing NUL
    std::free(buf);
#else
    const char* raw = std::getenv(name.c_str());
    if (!raw) return Value::fromNil();
    val = raw;
#endif
    GcObject* s = ctx->vm->heap->alloc_string_gc(val, ctx);
    return Value::fromPtr(s->payload());
}

// fileExists(path) -> Bool (Plain -- no wrap). True for ANY filesystem entry (file or
// directory, `test -e` style); a non-string arg or a lookup error reads as false.
// Allocates nothing.
static Value native_file_exists(Value* args, uint8_t nargs, Context*) {
    if (nargs < 1 || !is_string(args[0])) return Value::fromBool(false);
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());
    }
    std::error_code ec;
    return Value::fromBool(std::filesystem::exists(path, ec) && !ec);
}

// deleteFile(path) -> nil (success) | String (error message). A missing file is an Err
// (Rust remove_file semantics). std::filesystem::remove also deletes an empty directory;
// a non-empty directory / permission failure sets `ec` -> Err. Allocates only on the Err
// path (native_make_error); the path is materialized host-side before that alloc.
static Value native_delete_file(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 1 || !is_string(args[0]))
        return native_make_error(ctx, "deleteFile: path must be a string");
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());
    }
    std::error_code ec;
    const bool removed = std::filesystem::remove(path, ec);
    if (ec)       return native_make_error(ctx, "could not delete file: " + path +
                                                " (" + ec.message() + ")");
    if (!removed) return native_make_error(ctx, "no such file: " + path);
    return Value::fromNil();
}

// listDir(path) -> Array[String] (success) | String (error message). The compiler wraps
// this Ok/Err (NativeReturn::Result); the success result is a KIND_ARRAY, so the Ok/Err
// discriminator (String => Err) routes it to Ok(array). Elements are the LEAF names of the
// directory entries (filename(), ls-style; neither "." nor ".." are yielded by
// directory_iterator), in filesystem order (unspecified). Two phases: collect every name
// host-side FIRST (no VM alloc while iterating), then build the KIND_ARRAY exactly like
// native_args (root-then-fill, so a per-string collection can move the array in place).
static Value native_list_dir(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 1 || !is_string(args[0]))
        return native_make_error(ctx, "listDir: path must be a string");
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());
    }
    std::error_code ec;
    std::filesystem::directory_iterator it(path, ec), end;
    if (ec)
        return native_make_error(ctx, "could not list directory: " + path +
                                      " (" + ec.message() + ")");
    std::vector<std::string> names;                 // host-side, before any VM alloc
    for (; it != end; it.increment(ec)) {
        if (ec)
            return native_make_error(ctx, "could not list directory: " + path +
                                          " (" + ec.message() + ")");
        names.push_back(it->path().filename().string());
    }
    // Build the KIND_ARRAY -- same rooted-then-filled discipline as native_args.
    Heap*          heap = ctx->vm->heap;
    const uint32_t n    = static_cast<uint32_t>(names.size());
    GcObject* arrObj = heap->alloc_slots_gc(GcObject::KIND_ARRAY, n, ctx);
    Value arr = Value::fromPtr(arrObj->payload());
    if (n == 0) return arr;                         // empty directory -> empty array
    heap->add_root(&arr);                           // track moves during the string allocs
    for (uint32_t i = 0; i < n; ++i) {
        GcObject* s = heap->alloc_string_gc(names[i], ctx);   // may collect -> arr rewritten
        GcObject::from_slots(arr.asPtr())->slots()[i] = Value::fromPtr(s->payload());
    }
    heap->remove_root(&arr);
    return arr;
}

// mkdir(path) -> nil (success) | String (error message). The compiler wraps this Ok/Err
// (NativeReturn::Result). create_directories (mkdir -p): creates any missing parent
// directories too, and an ALREADY-EXISTING directory is idempotent success (create_directories
// returns false with a clear ec in that case). A real failure (a path component is a file,
// permission denied, ...) sets ec -> Err. Allocates only on the Err path (native_make_error).
static Value native_make_dir(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 1 || !is_string(args[0]))
        return native_make_error(ctx, "mkdir: path must be a string");
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());
    }
    std::error_code ec;
    std::filesystem::create_directories(path, ec);  // false + clear ec == already existed (Ok)
    if (ec)
        return native_make_error(ctx, "could not create directory: " + path +
                                      " (" + ec.message() + ")");
    return Value::fromNil();
}

// appendFile(path, buffer) -> nil (success) | String (error message). The compiler wraps
// this Ok/Err (NativeReturn::Result). Like writeFile but opens the file in APPEND mode
// (std::ios::app), creating it if absent -- so repeated calls accumulate. `buffer` must be
// a KIND_BYTES (a String source is served by the pure-prelude appendTextFile wrapper). The
// buffer's live bytes are materialized host-side BEFORE the file op (a later error alloc
// could otherwise relocate the backing).
static Value native_append_file(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 2 || !is_string(args[0]))
        return native_make_error(ctx, "appendFile: path must be a string");
    if (!args[1].isPtr() ||
        GcObject::from_slots(args[1].asPtr())->kind != GcObject::KIND_BYTES)
        return native_make_error(ctx, "appendFile: second argument must be a byte buffer");
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());
    }
    std::string data;
    {
        GcObject*      hdr     = GcObject::from_slots(args[1].asPtr());
        GcObject*      backing = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
        const uint32_t count   = static_cast<uint32_t>(hdr->slots()[BYTES_SLOT_COUNT].asSigned48());
        data.assign(backing->bytes(), count);
    }
    std::ofstream f(path, std::ios::binary | std::ios::app);
    if (!f)
        return native_make_error(ctx, "could not open file: " + path);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!f)
        return native_make_error(ctx, "could not write file: " + path);
    return Value::fromNil();
}

// isFile(path) -> Bool (Plain -- no wrap). True iff the path names a regular file (NOT a
// directory -- the finer complement of fileExists, which is `test -e`). A non-string arg or
// a lookup error reads as false. Allocates nothing.
static Value native_is_file(Value* args, uint8_t nargs, Context*) {
    if (nargs < 1 || !is_string(args[0])) return Value::fromBool(false);
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());
    }
    std::error_code ec;
    return Value::fromBool(std::filesystem::is_regular_file(path, ec) && !ec);
}

// isDir(path) -> Bool (Plain -- no wrap). True iff the path names a directory. A non-string
// arg or a lookup error reads as false. Allocates nothing.
static Value native_is_dir(Value* args, uint8_t nargs, Context*) {
    if (nargs < 1 || !is_string(args[0])) return Value::fromBool(false);
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());
    }
    std::error_code ec;
    return Value::fromBool(std::filesystem::is_directory(path, ec) && !ec);
}

// fileSize(path) -> Int (success) | String (error message). The compiler wraps this Ok/Err
// (NativeReturn::Result); the success payload is an IMMEDIATE Int (like parseInt), so the
// Ok/Err discriminator (String => Err) routes it to Ok(size). std::filesystem::file_size of
// a missing / non-regular path (a directory is implementation-defined) sets `ec` -> Err. A
// size beyond signed 48-bit (~140 TB) is also reported as an Err rather than silently
// truncated. Allocates only on the Err path.
static Value native_file_size(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 1 || !is_string(args[0]))
        return native_make_error(ctx, "fileSize: path must be a string");
    std::string path;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        path.assign(o->bytes(), o->string_length());
    }
    std::error_code ec;
    const std::uintmax_t sz = std::filesystem::file_size(path, ec);
    if (ec)
        return native_make_error(ctx, "could not stat file: " + path +
                                      " (" + ec.message() + ")");
    constexpr std::uintmax_t MAX_48 = 140737488355327ULL;   // 2^47 - 1
    if (sz > MAX_48)
        return native_make_error(ctx, "fileSize: file too large for Int: " + path);
    return Value::fromSigned48(static_cast<int64_t>(sz));    // Ok payload (immediate)
}

// Materialize two KIND_STRING path arguments into host buffers BEFORE any alloc (the two-path file
// ops below share this). Returns false + writes an error Value into *err if either arg is not a string.
static bool native_two_paths(Value* args, uint8_t nargs, Context* ctx, const char* who,
                             std::string& from, std::string& to, Value* err) {
    if (nargs < 2 || !is_string(args[0]) || !is_string(args[1])) {
        *err = native_make_error(ctx, std::string(who) + ": both arguments must be strings");
        return false;
    }
    GcObject* a = GcObject::from_slots(args[0].asPtr());
    from.assign(a->bytes(), a->string_length());
    GcObject* b = GcObject::from_slots(args[1].asPtr());
    to.assign(b->bytes(), b->string_length());
    return true;
}

// rename(from, to) -> nil (success) | String (error message). The compiler wraps this Ok/Err
// (NativeReturn::Result). std::filesystem::rename OVERWRITES an existing destination and is atomic on
// the same filesystem; a cross-filesystem move (or a missing source / permission failure) sets `ec`
// and is passed through as an Err. Allocates only on the Err path.
static Value native_rename(Value* args, uint8_t nargs, Context* ctx) {
    std::string from, to; Value err;
    if (!native_two_paths(args, nargs, ctx, "rename", from, to, &err)) return err;
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec)
        return native_make_error(ctx, "could not rename " + from + " -> " + to +
                                      " (" + ec.message() + ")");
    return Value::fromNil();
}

// copyFile(from, to) -> nil (success) | String (error message). The compiler wraps this Ok/Err
// (NativeReturn::Result). Uses the std::filesystem::copy_file DEFAULT (copy_options::none): an
// ALREADY-EXISTING destination is an Err (no silent overwrite), as is a missing / non-regular source.
// Allocates only on the Err path.
static Value native_copy_file(Value* args, uint8_t nargs, Context* ctx) {
    std::string from, to; Value err;
    if (!native_two_paths(args, nargs, ctx, "copyFile", from, to, &err)) return err;
    std::error_code ec;
    std::filesystem::copy_file(from, to, ec);   // default: fail if `to` exists
    if (ec)
        return native_make_error(ctx, "could not copy " + from + " -> " + to +
                                      " (" + ec.message() + ")");
    return Value::fromNil();
}

// std::math natives -- thin libm wrappers (Plain return, no allocation, no safepoint). The compiler
// types the arg(s) as Double (an Int arg widens via I2D at the call boundary), but numAsDouble()
// promotes either kind defensively. IEEE semantics: a domain error yields NaN / +-inf (no trap).
#define MATH1(NAME, FN) \
    static Value NAME(Value* a, uint8_t, Context*) { return Value::fromDouble(FN(a[0].numAsDouble())); }
#define MATH2(NAME, FN) \
    static Value NAME(Value* a, uint8_t, Context*) { return Value::fromDouble(FN(a[0].numAsDouble(), a[1].numAsDouble())); }
#define MATHP(NAME, FN) \
    static Value NAME(Value* a, uint8_t, Context*) { return Value::fromBool(FN(a[0].numAsDouble())); }
MATH1(native_sqrt,  std::sqrt)
MATH1(native_cbrt,  std::cbrt)
MATH2(native_pow,   std::pow)
MATH2(native_hypot, std::hypot)
MATH1(native_exp,   std::exp)
MATH1(native_ln,    std::log)      // natural log (surface name `ln`)
MATH1(native_log2,  std::log2)
MATH1(native_log10, std::log10)
MATH1(native_sin,   std::sin)
MATH1(native_cos,   std::cos)
MATH1(native_tan,   std::tan)
MATH1(native_asin,  std::asin)
MATH1(native_acos,  std::acos)
MATH1(native_atan,  std::atan)
MATH2(native_atan2, std::atan2)
MATH1(native_fabs,  std::fabs)
MATHP(native_isnan, std::isnan)
MATHP(native_isinf, std::isinf)
#undef MATH1
#undef MATH2
#undef MATHP

// readLine() -> String (a line) | nil (EOF). The compiler wraps this Some/None
// (NativeReturn::Option): nil => None at EOF, else Some(line). Reads one line from the
// VM's input sink (ctx->vm->in, std::cin by default, redirectable for tests). getline strips
// the trailing '\n'; a trailing '\r' (CRLF from a pipe/binary stream) is stripped defensively.
// Zero-arg (dummy window base). Allocates one string on the Some path (safepoint); None
// allocates nothing.
static Value native_read_line(Value*, uint8_t, Context* ctx) {
    std::string line;
    if (!std::getline(*ctx->vm->in, line))
        return Value::fromNil();                    // EOF / stream error -> None
    if (!line.empty() && line.back() == '\r')
        line.pop_back();                            // CRLF -> LF (defensive)
    GcObject* s = ctx->vm->heap->alloc_string_gc(line, ctx);
    return Value::fromPtr(s->payload());
}

// readAllStdin() -> String. Reads the WHOLE input sink (ctx->vm->in) to EOF into one string;
// an immediately-empty stream yields "" (a bulk read cannot "fail" -> Plain return kind, no
// wrap, no prelude requirement). Zero-arg (dummy window base). Allocates one string (safepoint).
static Value native_read_all_stdin(Value*, uint8_t, Context* ctx) {
    std::string all((std::istreambuf_iterator<char>(*ctx->vm->in)),
                    std::istreambuf_iterator<char>());
    GcObject* s = ctx->vm->heap->alloc_string_gc(all, ctx);
    return Value::fromPtr(s->payload());
}

// f64ToBytes(x) -> Bytes: the 8 raw IEEE-754 bytes of x, little-endian (Plain). Enables a pure-Skarn
// program -- notably the self-hosting codegen (North Star P5) -- to materialize a double literal's
// const-pool bit pattern, which the language's 48-bit Int cannot hold. The 8 bytes are read back as two
// u32 halves (each < 2^32, in-range for a 48-bit Int) -> the lo/hi Value halves the SKBC const pool
// serializes. `x` is already a normalized double (fromDouble canonicalizes -0.0/NaN), so the bytes match
// the C++ codegen's Value::fromDouble path exactly. Allocates one KIND_BYTES (safepoint).
static Value native_f64_to_bytes(Value* args, uint8_t nargs, Context* ctx) {
    double d = (nargs >= 1) ? args[0].numAsDouble() : 0.0;
    unsigned char buf[8];
    std::memcpy(buf, &d, 8);                            // host is little-endian x64 -> LE byte order
    Value result;
    bytes_from_str(ctx, &result, std::string_view(reinterpret_cast<const char*>(buf), 8));
    return result;
}

// parseInt(s) -> Int (success) | String (error message). Strict full-string base-10
// parse via std::from_chars (locale-free, non-allocating, no exceptions): the WHOLE
// string must be consumed (no leading/trailing space, no trailing junk, no leading
// '+'), and the value must fit signed 48-bit; otherwise the error String. An
// out-of-range integer is a parse FAILURE (Err), not saturated -- contrast toInt/D2I.
static Value native_parse_int(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 1 || !is_string(args[0]))
        return native_make_error(ctx, "parseInt: invalid integer");
    std::string s;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        s.assign(o->bytes(), o->string_length());     // host copy before any alloc
    }
    const char* first = s.data();
    const char* last  = first + s.size();
    int64_t v = 0;
    auto r = std::from_chars(first, last, v);          // base 10
    constexpr int64_t MAX_48 =  140737488355327LL;     //  2^47 - 1
    constexpr int64_t MIN_48 = -140737488355328LL;     // -2^47
    if (r.ec != std::errc{} || r.ptr != last || v < MIN_48 || v > MAX_48)
        return native_make_error(ctx, "parseInt: invalid integer");
    return Value::fromSigned48(v);                      // Ok payload (immediate)
}

// parseDouble(s) -> Double (success) | String (error message). Strict full-string
// parse in chars_format::general: an optional '-', decimal digits with an optional '.'
// and an optional exponent, or one of the inf / infinity / nan spellings (any case).
// Rejected: a leading '+' or whitespace, a hexadecimal 0x form, a ',' decimal
// separator, and any trailing junk.
//
// std::from_chars is exactly that grammar, and it is locale-independent. libc++ has no
// floating-point from_chars, so the #else falls back to strtod -- which is a LOOSER
// grammar (it takes '+', leading whitespace and hex floats) and is locale-SENSITIVE via
// LC_NUMERIC. The pre-scan below removes the difference: it rejects everything strtod
// would accept and from_chars would not, so both paths accept the same strings.
static Value native_parse_double(Value* args, uint8_t nargs, Context* ctx) {
    if (nargs < 1 || !is_string(args[0]))
        return native_make_error(ctx, "parseDouble: invalid number");
    std::string s;
    {
        GcObject* o = GcObject::from_slots(args[0].asPtr());
        s.assign(o->bytes(), o->string_length());
    }
    const char* first = s.data();
    const char* last  = first + s.size();
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    double v = 0.0;
    auto r = std::from_chars(first, last, v);           // chars_format::general
    if (r.ec != std::errc{} || r.ptr != last)
        return native_make_error(ctx, "parseDouble: invalid number");
    return Value::fromDouble(v);
#else
    const auto reject = [&] { return native_make_error(ctx, "parseDouble: invalid number"); };
    if (first == last) return reject();
    // strtod SKIPS leading whitespace and then reports a clean end, so the trailing-junk
    // check below cannot see it -- it has to be rejected up front.
    if (*first == ' ' || *first == '\t' || *first == '\n' || *first == '\r'
        || *first == '\f' || *first == '\v')
        return reject();
    const char* p = first;
    if (*p == '-') ++p;                                 // '+' is NOT part of the grammar
    if (p == last) return reject();
    if (*p == '+' || *p == ' ' || *p == '\t') return reject();
    if (*p == '0' && p + 1 < last && (p[1] == 'x' || p[1] == 'X'))
        return reject();                                // no hex floats
    if (s.find(',') != std::string::npos)
        return reject();                                // no locale decimal comma
    char*  endptr = nullptr;
    double v      = std::strtod(first, &endptr);
    if (endptr != last) return reject();                // trailing junk / nothing parsed
    return Value::fromDouble(v);
#endif
}

// Drain a readable pipe to EOF into `out`. Runs on its own std::thread so
// stdout and stderr are read concurrently (a child that fills one pipe while we are
// blocked on the other would otherwise deadlock). Never throws across the thread
// boundary; a broken pipe / closed handle just ends the loop.
#ifdef _WIN32
// Quote one argument for a Win32 command line (CommandLineToArgvW inverse rules).
// CreateProcess takes ONE command line string, so build_command_line reconstructs it.
static std::string quote_win32_arg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos)
        return arg;
    std::string out = "\"";
    for (size_t i = 0; ; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == '\\') { ++backslashes; ++i; }
        if (i == arg.size()) { out.append(backslashes * 2, '\\'); break; }
        if (arg[i] == '"') { out.append(backslashes * 2 + 1, '\\'); out.push_back('"'); }
        else { out.append(backslashes, '\\'); out.push_back(arg[i]); }
    }
    out.push_back('"');
    return out;
}
static std::string build_command_line(const std::vector<std::string>& argv) {
    std::string cmd;
    for (size_t i = 0; i < argv.size(); ++i) { if (i) cmd.push_back(' '); cmd += quote_win32_arg(argv[i]); }
    return cmd;
}
static void drain_pipe(HANDLE h, std::string* out) noexcept {
    char buf[4096]; DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
        out->append(buf, got);
}
#else
static void drain_pipe(int fd, std::string* out) noexcept {
    char buf[4096]; ssize_t got = 0;
    while ((got = ::read(fd, buf, sizeof(buf))) > 0)
        out->append(buf, static_cast<size_t>(got));
    ::close(fd);
}
#endif

// rawRun(argv, input) -> [stdoutBytes, stderrBytes, exitInt] (success) | String (spawn error).
// The low-level process primitive; the prelude's run/runWith reshape the Array[3] into a
// ProcessOutput struct. The compiler wraps this Ok/Err (NativeReturn::Result): the success
// result is a KIND_ARRAY, so the "String => Err" discriminator routes it to Ok(array).
//   argv  -- a non-empty KIND_ARRAY of KIND_STRING; argv[0] is the program, the rest args.
//   input -- a KIND_BYTES fed to the child's stdin (then EOF), or nil for empty stdin.
// Rust Command::output semantics: only a SPAWN failure (program not found, pipe error) is an
// Err; a process that runs and exits non-zero is a success whose exitCode is data. All pipe
// I/O + threading is host-side (no VM alloc); only the final Array[3] + its two KIND_BYTES are
// allocated, with the native_args root-then-fill discipline (a per-Bytes collection can move
// the array in place).
static Value native_run_process(Value* args, uint8_t nargs, Context* ctx) {
    // --- Validate + materialize everything host-side BEFORE any VM alloc ---------------
    // argv may be a KIND_ARRAY or a KIND_VEC (the prelude's run/runWith normalize any
    // iterable to a vec first, so a bare cons-list literal works at the surface too).
    std::vector<std::string> argv;
    {
        if (!args[0].isPtr())
            return native_make_error(ctx, "run: first argument must be an array of strings");
        GcObject*     hdr = GcObject::from_slots(args[0].asPtr());
        Value*        elems = nullptr;
        uint32_t      n     = 0;
        if (hdr->kind == GcObject::KIND_ARRAY) {
            elems = hdr->slots();
            n     = static_cast<uint32_t>(hdr->slot_count());
        } else if (hdr->kind == GcObject::KIND_VEC) {
            GcObject* backing = GcObject::from_slots(hdr->slots()[VEC_SLOT_BACKING].asPtr());
            n     = static_cast<uint32_t>(hdr->slots()[VEC_SLOT_COUNT].asSigned48());
            elems = backing->slots();
        } else {
            return native_make_error(ctx, "run: first argument must be an array of strings");
        }
        argv.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            if (!is_string(elems[i]))
                return native_make_error(ctx, "run: argv elements must be strings");
            GcObject* s = GcObject::from_slots(elems[i].asPtr());
            argv.emplace_back(s->bytes(), s->string_length());
        }
    }
    if (argv.empty())
        return native_make_error(ctx, "run: argv must not be empty");
    std::string stdin_data;                         // empty unless a KIND_BYTES input is given
    if (nargs >= 2 && args[1].isPtr() &&
        GcObject::from_slots(args[1].asPtr())->kind == GcObject::KIND_BYTES) {
        GcObject*      hdr     = GcObject::from_slots(args[1].asPtr());
        GcObject*      backing = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
        const uint32_t count   = static_cast<uint32_t>(hdr->slots()[BYTES_SLOT_COUNT].asSigned48());
        stdin_data.assign(backing->bytes(), count);
    }

    // --- Spawn child process with three pipes (stdin/stdout/stderr) ----------------
    std::string out_buf, err_buf;
    int64_t exit_code = 0;

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE in_rd = nullptr, in_wr = nullptr;
    HANDLE out_rd = nullptr, out_wr = nullptr;
    HANDLE err_rd = nullptr, err_wr = nullptr;
    auto close_all = [&]() {
        for (HANDLE* h : { &in_rd, &in_wr, &out_rd, &out_wr, &err_rd, &err_wr })
            if (*h) { CloseHandle(*h); *h = nullptr; }
    };
    if (!CreatePipe(&in_rd, &in_wr, &sa, 0) ||
        !CreatePipe(&out_rd, &out_wr, &sa, 0) ||
        !CreatePipe(&err_rd, &err_wr, &sa, 0)) {
        close_all();
        return native_make_error(ctx, "run: could not create pipes for " + argv[0]);
    }
    SetHandleInformation(in_wr,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_rd; si.hStdOutput = out_wr; si.hStdError = err_wr;
    PROCESS_INFORMATION pi{};
    std::string cmdline = build_command_line(argv);
    std::vector<char> cmd_mut(cmdline.begin(), cmdline.end()); cmd_mut.push_back('\0');
    if (!CreateProcessA(nullptr, cmd_mut.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        DWORD gle = GetLastError(); close_all();
        return native_make_error(ctx, "run: could not start process: " + argv[0] +
                                      " (error " + std::to_string(gle) + ")");
    }
    CloseHandle(in_rd); CloseHandle(out_wr); CloseHandle(err_wr);
    in_rd = out_wr = err_wr = nullptr;
    std::thread t_out(drain_pipe, out_rd, &out_buf);
    std::thread t_err(drain_pipe, err_rd, &err_buf);
    if (!stdin_data.empty()) {
        const char* p = stdin_data.data(); size_t left = stdin_data.size();
        while (left > 0) {
            DWORD wrote = 0; DWORD chunk = static_cast<DWORD>(left > (1u<<20) ? (1u<<20) : left);
            if (!WriteFile(in_wr, p, chunk, &wrote, nullptr) || wrote == 0) break;
            p += wrote; left -= wrote;
        }
    }
    CloseHandle(in_wr); in_wr = nullptr;
    t_out.join(); t_err.join();
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD wec = 0; GetExitCodeProcess(pi.hProcess, &wec);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    CloseHandle(out_rd); CloseHandle(err_rd);
    exit_code = static_cast<int64_t>(static_cast<int32_t>(wec));
#else
    int in_pipe[2]  = {-1, -1};   // [read-end, write-end]: child reads, parent writes
    int out_pipe[2] = {-1, -1};   // [read-end, write-end]: parent reads, child writes
    int err_pipe[2] = {-1, -1};
    // exec_err_pipe: O_CLOEXEC write-end is closed by exec on success; child writes errno on
    // exec failure so the parent can distinguish "exec failed" from "process exited non-zero".
    int exec_err_pipe[2] = {-1, -1};
    auto close_fds = [&]() {
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1],
                       err_pipe[0], err_pipe[1], exec_err_pipe[0], exec_err_pipe[1]})
            if (fd >= 0) ::close(fd);
    };
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0 ||
        pipe(exec_err_pipe) != 0) {
        close_fds();
        return native_make_error(ctx, "run: could not create pipes for " + argv[0]);
    }
    fcntl(exec_err_pipe[1], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) {
        close_fds();
        return native_make_error(ctx, "run: fork failed for " + argv[0] + ": " + std::strerror(errno));
    }
    if (pid == 0) {
        // Child: wire up stdio then exec
        ::close(exec_err_pipe[0]);
        dup2(in_pipe[0],  STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]})
            ::close(fd);
        std::vector<char*> exec_argv;
        exec_argv.reserve(argv.size() + 1);
        for (auto& s : argv) exec_argv.push_back(const_cast<char*>(s.c_str()));
        exec_argv.push_back(nullptr);
        execvp(exec_argv[0], exec_argv.data());
        // exec failed: send errno back to parent then exit
        int child_errno = errno;
        ::write(exec_err_pipe[1], &child_errno, sizeof(child_errno));
        _exit(127);
    }
    // Parent: close child-side ends so EOF propagates
    ::close(in_pipe[0]);  in_pipe[0]  = -1;
    ::close(out_pipe[1]); out_pipe[1] = -1;
    ::close(err_pipe[1]); err_pipe[1] = -1;
    // Read from exec_err_pipe: returns sizeof(int) bytes if exec failed (errno), 0 bytes
    // (EOF via O_CLOEXEC) if exec succeeded.
    ::close(exec_err_pipe[1]); exec_err_pipe[1] = -1;
    int child_exec_errno = 0;
    ssize_t exec_err_n = ::read(exec_err_pipe[0], &child_exec_errno, sizeof(child_exec_errno));
    ::close(exec_err_pipe[0]); exec_err_pipe[0] = -1;
    if (exec_err_n == static_cast<ssize_t>(sizeof(child_exec_errno))) {
        waitpid(pid, nullptr, 0);
        ::close(in_pipe[1]); in_pipe[1] = -1;
        // drain threads haven't started yet so drain fds manually
        ::close(out_pipe[0]); out_pipe[0] = -1;
        ::close(err_pipe[0]); err_pipe[0] = -1;
        return native_make_error(ctx, "run: could not start process: " + argv[0] +
                                      ": " + std::strerror(child_exec_errno));
    }
    std::thread t_out(drain_pipe, out_pipe[0], &out_buf);  // drain_pipe closes fd on return
    std::thread t_err(drain_pipe, err_pipe[0], &err_buf);
    out_pipe[0] = err_pipe[0] = -1;  // drain_pipe owns/closes these
    if (!stdin_data.empty()) {
        const char* p = stdin_data.data(); size_t left = stdin_data.size();
        while (left > 0) {
            ssize_t wrote = ::write(in_pipe[1], p, left);
            if (wrote <= 0) break;
            p += wrote; left -= static_cast<size_t>(wrote);
        }
    }
    ::close(in_pipe[1]); in_pipe[1] = -1;   // EOF to child's stdin
    t_out.join(); t_err.join();
    int wstatus = 0; waitpid(pid, &wstatus, 0);
    exit_code = static_cast<int64_t>(WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1);
#endif

    // --- Build the Array[3] result (root-then-fill; see native_args) --------------------
    Heap* heap = ctx->vm->heap;
    GcObject* arrObj = heap->alloc_slots_gc(GcObject::KIND_ARRAY, 3, ctx);
    Value arr = Value::fromPtr(arrObj->payload());
    heap->add_root(&arr);                           // the two Bytes allocs below can move it
    Value out_bytes;
    bytes_from_str(ctx, &out_bytes, out_buf);       // may collect -> arr rewritten in place
    GcObject::from_slots(arr.asPtr())->slots()[0] = out_bytes;
    Value err_bytes;
    bytes_from_str(ctx, &err_bytes, err_buf);       // may collect -> arr rewritten in place
    GcObject::from_slots(arr.asPtr())->slots()[1] = err_bytes;
    GcObject::from_slots(arr.asPtr())->slots()[2] = Value::fromSigned48(exit_code);
    heap->remove_root(&arr);
    return arr;
}

// rawGcStats() -> Array[Double] (Plain -- no wrap). A runtime GC introspection hook: the 8
// Heap::GcStats counters, in the field order of the Skarn `GcStats` struct that the prelude
// `gcStats()` wrapper unpacks. Each counter is a uint64; a DOUBLE (53-bit exact-integer range)
// carries it faithfully for any realistic run -- unlike a 48-bit Int, so this sidesteps the
// int-range caveat entirely. Non-deterministic (depends on heap history), so it is EXCLUDED from
// the differential (RefEval leaves it Unsupported, like nanoTime). Builds a fresh KIND_ARRAY of
// 8 immediate doubles; the doubles are immediates (no per-element heap object), so no root-while-
// filling is needed -- the single array alloc is the only safepoint, and stats() is read after it.
static Value native_raw_gc_stats(Value*, uint8_t, Context* ctx) {
    Heap* heap = ctx->vm->heap;
    GcObject* arrObj = heap->alloc_slots_gc(GcObject::KIND_ARRAY, 8, ctx);
    const Heap::GcStats& s = heap->stats();   // read AFTER the alloc (no move afterwards)
    Value* slot = arrObj->slots();
    slot[0] = Value::fromDouble(static_cast<double>(s.collections));
    slot[1] = Value::fromDouble(static_cast<double>(s.objects_alloced));
    slot[2] = Value::fromDouble(static_cast<double>(s.bytes_alloced));
    slot[3] = Value::fromDouble(static_cast<double>(s.from_used_sum));
    slot[4] = Value::fromDouble(static_cast<double>(s.survivors_sum));
    slot[5] = Value::fromDouble(static_cast<double>(s.gc_ns_total));
    slot[6] = Value::fromDouble(static_cast<double>(s.gc_ns_max));
    slot[7] = Value::fromDouble(static_cast<double>(s.grow_events));
    return Value::fromPtr(arrObj->payload());
}

// gcResetStats() -> nil (Plain). Clears the GcStats counters so a subsequent gcStats() measures
// GC over a code region (reset -> ...code... -> gcStats()). Non-allocating, non-deterministic.
static Value native_gc_reset_stats(Value*, uint8_t, Context* ctx) {
    ctx->vm->heap->reset_stats();
    return Value::fromNil();
}

// =============================================================================
// TCP networking natives (std::net). Blocking sockets over Winsock. A socket is
// exposed to Skarn as a small Int DESCRIPTOR -- an index into this per-execution
// registry (VM::net) -- never a raw OS SOCKET (a 64-bit kernel handle that need
// not fit a 48-bit Int). The registry closes any still-open socket at execute()
// teardown (RAII), the safety net behind explicit tcpClose. NOT a GC root (it
// holds integer handles, no Values). Each native returns a Bytes/Int/nil on
// success or a String error message (the compiler wraps Ok/Err, NRET_RESULT). The NetRegistry type
// itself is defined near the top of this file (execute() holds a stack-local instance).
// =============================================================================

// Lazy, once-only WSAStartup (single-threaded VM -> no synchronization). No WSACleanup:
// process exit reclaims the Winsock state, and a paired cleanup would race a still-open socket.
static bool ensure_wsa() {
#ifdef _WIN32
    static bool inited = false, ok = false;
    if (!inited) { WSADATA d; ok = (WSAStartup(MAKEWORD(2, 2), &d) == 0); inited = true; }
    return ok;
#else
    return true;  // POSIX sockets need no initialization
#endif
}

static std::string net_error_msg(const char* op) {
#ifdef _WIN32
    return std::string(op) + " failed (WSA error " + std::to_string(WSAGetLastError()) + ")";
#else
    return std::string(op) + " failed: " + std::strerror(errno);
#endif
}

#ifndef _WIN32
static int set_nonblocking(int s, bool nb) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(s, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
#endif
static const long NET_CONNECT_TIMEOUT_SEC = 10;   // connect() select() timeout
static const long NET_RECV_CHUNK_MAX      = 1 << 20; // cap a single tcpRecv at 1 MiB

// tcpConnect(host, port) -> Int descriptor (success) | String (error). Resolves host with
// getaddrinfo(AF_UNSPEC) so BOTH IPv4 and IPv6 addresses are tried; connect is non-blocking
// with a select() timeout so a dead host does not hang the VM.
static Value native_tcp_connect(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "tcpConnect: networking unavailable");
    if (nargs < 2 || !is_string(args[0]) || !args[1].isInt())
        return native_make_error(ctx, "tcpConnect: expected (host: String, port: Int)");
    std::string host;
    { GcObject* o = GcObject::from_slots(args[0].asPtr()); host.assign(o->bytes(), o->string_length()); }
    long port = static_cast<long>(args[1].asSigned48());
    if (port < 0 || port > 65535) return native_make_error(ctx, "tcpConnect: port out of range (0..65535)");
    if (!ensure_wsa()) return native_make_error(ctx, "tcpConnect: WSAStartup failed");

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
        return native_make_error(ctx, "tcpConnect: cannot resolve host: " + host);

    socket_t sock = INVALID_SOCK;
    for (addrinfo* ai = res; ai && sock == INVALID_SOCK; ai = ai->ai_next) {
        socket_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCK) continue;
#ifdef _WIN32
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
#else
        set_nonblocking(s, true);
#endif
        int rc = connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        bool ok = (rc == 0);
#ifdef _WIN32
        if (rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
#else
        if (rc == -1 && (errno == EINPROGRESS || errno == EWOULDBLOCK)) {
#endif
            fd_set wf; FD_ZERO(&wf); FD_SET(s, &wf);
            timeval tv{}; tv.tv_sec = NET_CONNECT_TIMEOUT_SEC; tv.tv_usec = 0;
#ifdef _WIN32
            if (select(0, nullptr, &wf, nullptr, &tv) > 0) {
#else
            if (select(s + 1, nullptr, &wf, nullptr, &tv) > 0) {
#endif
                int soErr = 0; socklen_t len = sizeof(soErr);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len);
                ok = (soErr == 0);
            }
        }
#ifdef _WIN32
        u_long bl = 0; ioctlsocket(s, FIONBIO, &bl);
#else
        set_nonblocking(s, false);
#endif
        if (ok) sock = s; else sock_close(s);
    }
    freeaddrinfo(res);
    if (sock == INVALID_SOCK) return native_make_error(ctx, "tcpConnect: could not connect to " + host);
    return Value::fromSigned48(ctx->vm->net->add(sock));
}

// tcpSend(sock, data) -> nil (success) | String (error). Sends the buffer's live bytes in full
// (loops until every byte is flushed or an error occurs).
static Value native_tcp_send(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "tcpSend: networking unavailable");
    if (nargs < 2 || !args[0].isInt())
        return native_make_error(ctx, "tcpSend: expected (sock: Int, data: Bytes)");
    if (!args[1].isPtr() || GcObject::from_slots(args[1].asPtr())->kind != GcObject::KIND_BYTES)
        return native_make_error(ctx, "tcpSend: data must be a byte buffer");
    socket_t s = ctx->vm->net->get(static_cast<int>(args[0].asSigned48()));
    if (s == INVALID_SOCK) return native_make_error(ctx, "tcpSend: invalid socket");
    std::string data;
    {
        GcObject*      hdr     = GcObject::from_slots(args[1].asPtr());
        GcObject*      backing = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
        const uint32_t count   = static_cast<uint32_t>(hdr->slots()[BYTES_SLOT_COUNT].asSigned48());
        data.assign(backing->bytes(), count);   // host copy (no alloc happens after this before the sends)
    }
    size_t sent = 0;
    while (sent < data.size()) {
        int n = ::send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n < 0) return native_make_error(ctx, "tcpSend: " + net_error_msg("send"));
        sent += static_cast<size_t>(n);
    }
    return Value::fromNil();
}

// tcpRecv(sock, maxBytes) -> Bytes (success; EMPTY = EOF) | String (error). One recv of up to
// maxBytes; a 0-length return is an orderly peer shutdown, surfaced as an empty Bytes.
static Value native_tcp_recv(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "tcpRecv: networking unavailable");
    if (nargs < 2 || !args[0].isInt() || !args[1].isInt())
        return native_make_error(ctx, "tcpRecv: expected (sock: Int, maxBytes: Int)");
    socket_t s = ctx->vm->net->get(static_cast<int>(args[0].asSigned48()));
    if (s == INVALID_SOCK) return native_make_error(ctx, "tcpRecv: invalid socket");
    long maxB = static_cast<long>(args[1].asSigned48());
    if (maxB < 0) return native_make_error(ctx, "tcpRecv: maxBytes must be non-negative");
    if (maxB > NET_RECV_CHUNK_MAX) maxB = NET_RECV_CHUNK_MAX;
    std::string buf;
    buf.resize(static_cast<size_t>(maxB));
    int n = (maxB == 0) ? 0 : ::recv(s, buf.data(), static_cast<int>(maxB), 0);
    if (n < 0) {
#ifdef _WIN32
        if (WSAGetLastError() == WSAETIMEDOUT) return native_make_error(ctx, "tcpRecv: timeout");
#else
        if (errno == EAGAIN || errno == ETIMEDOUT) return native_make_error(ctx, "tcpRecv: timeout");
#endif
        return native_make_error(ctx, "tcpRecv: " + net_error_msg("recv"));
    }
    const std::string got(buf.data(), static_cast<size_t>(n));  // host copy before the allocating build
    Value result;
    bytes_from_str(ctx, &result, got);
    return result;
}

// tcpClose(sock) -> nil (success) | String (error). Closes the socket and frees its registry slot.
// Used for a client connection AND for a server's listen socket (self-shutdown).
static Value native_tcp_close(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "tcpClose: networking unavailable");
    if (nargs < 1 || !args[0].isInt()) return native_make_error(ctx, "tcpClose: expected (sock: Int)");
    int fd = static_cast<int>(args[0].asSigned48());
    socket_t s = ctx->vm->net->get(fd);
    if (s == INVALID_SOCK) return native_make_error(ctx, "tcpClose: invalid socket");
    sock_close(s);
    ctx->vm->net->drop(fd);
    return Value::fromNil();
}

// tcpListen(port) -> Int listen-descriptor (success) | String (error). Creates a DUAL-STACK IPv6
// listener (IPV6_V6ONLY=0), so it accepts both IPv6 and IPv4-mapped clients (a "127.0.0.1" connect works).
static Value native_tcp_listen(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "tcpListen: networking unavailable");
    if (nargs < 1 || !args[0].isInt()) return native_make_error(ctx, "tcpListen: expected (port: Int)");
    long port = static_cast<long>(args[0].asSigned48());
    if (port < 0 || port > 65535) return native_make_error(ctx, "tcpListen: port out of range (0..65535)");
    if (!ensure_wsa()) return native_make_error(ctx, "tcpListen: WSAStartup failed");
    socket_t s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCK) return native_make_error(ctx, "tcpListen: " + net_error_msg("socket"));
#ifdef _WIN32
    DWORD v6only = 0; setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&v6only), sizeof(v6only));
    BOOL  reuse  = TRUE; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse), sizeof(reuse));
#else
    int v6only = 0; setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    int reuse  = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6; addr.sin6_addr = in6addr_any; addr.sin6_port = htons(static_cast<uint16_t>(port));
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::string m = "tcpListen: " + net_error_msg("bind"); sock_close(s); return native_make_error(ctx, m);
    }
    if (listen(s, SOMAXCONN) < 0) {
        std::string m = "tcpListen: " + net_error_msg("listen"); sock_close(s); return native_make_error(ctx, m);
    }
    return Value::fromSigned48(ctx->vm->net->add(s));
}

// tcpAccept(listenSock) -> Int descriptor of an accepted connection (success) | String (error).
// BLOCKS until a client connects (the single-threaded VM serves one connection at a time).
static Value native_tcp_accept(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "tcpAccept: networking unavailable");
    if (nargs < 1 || !args[0].isInt()) return native_make_error(ctx, "tcpAccept: expected (sock: Int)");
    socket_t s = ctx->vm->net->get(static_cast<int>(args[0].asSigned48()));
    if (s == INVALID_SOCK) return native_make_error(ctx, "tcpAccept: invalid socket");
    socket_t c = accept(s, nullptr, nullptr);
    if (c == INVALID_SOCK) return native_make_error(ctx, "tcpAccept: " + net_error_msg("accept"));
    return Value::fromSigned48(ctx->vm->net->add(c));
}

// tcpLocalPort(sock) -> Int port (success) | String (error). The port the socket is actually bound
// to, via getsockname. Its reason to exist is `tcpListen(0)`: asking the OS for a free port is the
// only way to run a server without guessing a fixed number, and on Windows the dynamic range is
// 1024-60000, so ANY hardcoded port can collide with an unrelated outbound connection (an observed,
// intermittent `bind` WSAEACCES). Reads the v6 sockaddr the dual-stack listener binds, but falls
// back to the v4 layout so it is also correct for a `tcpConnect` descriptor.
static Value native_tcp_local_port(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "tcpLocalPort: networking unavailable");
    if (nargs < 1 || !args[0].isInt()) return native_make_error(ctx, "tcpLocalPort: expected (sock: Int)");
    socket_t s = ctx->vm->net->get(static_cast<int>(args[0].asSigned48()));
    if (s == INVALID_SOCK) return native_make_error(ctx, "tcpLocalPort: invalid socket");
    sockaddr_storage ss{};
    socklen_t len = static_cast<socklen_t>(sizeof(ss));
    if (getsockname(s, reinterpret_cast<sockaddr*>(&ss), &len) < 0)
        return native_make_error(ctx, "tcpLocalPort: " + net_error_msg("getsockname"));
    uint16_t net_port = 0;
    if (ss.ss_family == AF_INET6)      net_port = reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port;
    else if (ss.ss_family == AF_INET)  net_port = reinterpret_cast<sockaddr_in*>(&ss)->sin_port;
    else return native_make_error(ctx, "tcpLocalPort: unsupported address family");
    return Value::fromSigned48(static_cast<int64_t>(ntohs(net_port)));
}

// tcpSetTimeout(sock, ms) -> nil (success) | String (error). Sets the recv/send timeout in
// milliseconds; ms <= 0 means block indefinitely (the default for a fresh socket).
static Value native_tcp_set_timeout(Value* args, uint8_t nargs, Context* ctx) {
    if (!ctx->vm->net) return native_make_error(ctx, "tcpSetTimeout: networking unavailable");
    if (nargs < 2 || !args[0].isInt() || !args[1].isInt())
        return native_make_error(ctx, "tcpSetTimeout: expected (sock: Int, ms: Int)");
    socket_t s = ctx->vm->net->get(static_cast<int>(args[0].asSigned48()));
    if (s == INVALID_SOCK) return native_make_error(ctx, "tcpSetTimeout: invalid socket");
    long ms = static_cast<long>(args[1].asSigned48());
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(ms < 0 ? 0 : ms);   // Windows SO_*TIMEO: DWORD of milliseconds
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&tv), sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&tv), sizeof(tv));
#else
    struct timeval tv{};
    if (ms > 0) { tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000; }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    return Value::fromNil();
}

// =============================================================================
// SHA-256 (FIPS 180-4). A native because 32-bit modular arithmetic is awkward in a 48-bit-Int
// language; pure + deterministic, so KAT-anchored (empty / "abc" / the fox vector). Returns the raw
// 32-byte digest as Bytes; the std::hash prelude adds the hex-string helpers on top.
// =============================================================================
static void sha256_digest(const uint8_t* msg, size_t len, uint8_t out[32]) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    uint32_t h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    // Padding: append 0x80, then zeros up to 56 mod 64, then the 64-bit big-endian bit length.
    std::vector<uint8_t> m(msg, msg + len);
    const uint64_t bitlen = static_cast<uint64_t>(len) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0x00);
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<uint8_t>(bitlen >> (i * 8)));
    auto rotr = [](uint32_t x, int n) -> uint32_t { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint32_t>(m[off + 4*i])     << 24) | (static_cast<uint32_t>(m[off + 4*i + 1]) << 16) |
                   (static_cast<uint32_t>(m[off + 4*i + 2]) <<  8) |  static_cast<uint32_t>(m[off + 4*i + 3]);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            const uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    for (int i = 0; i < 8; ++i) {
        out[4*i]   = static_cast<uint8_t>(h[i] >> 24);
        out[4*i+1] = static_cast<uint8_t>(h[i] >> 16);
        out[4*i+2] = static_cast<uint8_t>(h[i] >> 8);
        out[4*i+3] = static_cast<uint8_t>(h[i]);
    }
}

// sha256(data: Bytes) -> Bytes (the 32-byte digest; Plain, always succeeds). The checker guarantees a
// Bytes argument; a non-Bytes (only reachable via hand-assembled bytecode) hashes an empty input.
static Value native_sha256(Value* args, uint8_t nargs, Context* ctx) {
    std::string data;
    if (nargs >= 1 && args[0].isPtr() &&
        GcObject::from_slots(args[0].asPtr())->kind == GcObject::KIND_BYTES) {
        GcObject*      hdr     = GcObject::from_slots(args[0].asPtr());
        GcObject*      backing = GcObject::from_slots(hdr->slots()[BYTES_SLOT_BACKING].asPtr());
        const uint32_t count   = static_cast<uint32_t>(hdr->slots()[BYTES_SLOT_COUNT].asSigned48());
        data.assign(backing->bytes(), count);   // host copy before the allocating build below
    }
    uint8_t dg[32];
    sha256_digest(reinterpret_cast<const uint8_t*>(data.data()), data.size(), dg);
    Value result;
    bytes_from_str(ctx, &result, std::string(reinterpret_cast<const char*>(dg), 32));
    return result;
}

std::vector<NativeFunc> build_native_table() {
    std::vector<NativeFunc> t(NATIVE_COUNT, nullptr);
    t[NATIVE_READ_FILE]   = native_read_file;
    t[NATIVE_WRITE_FILE]  = native_write_file;
    t[NATIVE_NANO_TIME]   = native_nano_time;
    t[NATIVE_MILLIS_TIME] = native_millis_time;
    t[NATIVE_ARGS]        = native_args;
    t[NATIVE_GET_ENV]     = native_get_env;
    t[NATIVE_FILE_EXISTS] = native_file_exists;
    t[NATIVE_DELETE_FILE] = native_delete_file;
    t[NATIVE_LIST_DIR]    = native_list_dir;
    t[NATIVE_MAKE_DIR]    = native_make_dir;
    t[NATIVE_READ_LINE]   = native_read_line;
    t[NATIVE_READ_ALL_STDIN] = native_read_all_stdin;
    t[NATIVE_RUN_PROCESS] = native_run_process;
    t[NATIVE_PARSE_INT]   = native_parse_int;
    t[NATIVE_PARSE_DOUBLE] = native_parse_double;
    t[NATIVE_RAW_GC_STATS] = native_raw_gc_stats;
    t[NATIVE_GC_RESET_STATS] = native_gc_reset_stats;
    t[NATIVE_APPEND_FILE] = native_append_file;
    t[NATIVE_IS_FILE]     = native_is_file;
    t[NATIVE_IS_DIR]      = native_is_dir;
    t[NATIVE_FILE_SIZE]   = native_file_size;
    t[NATIVE_RENAME]      = native_rename;
    t[NATIVE_COPY_FILE]   = native_copy_file;
    t[NATIVE_SQRT]  = native_sqrt;   t[NATIVE_CBRT]  = native_cbrt;  t[NATIVE_POW]   = native_pow;
    t[NATIVE_HYPOT] = native_hypot;  t[NATIVE_EXP]   = native_exp;   t[NATIVE_LN]    = native_ln;
    t[NATIVE_LOG2]  = native_log2;   t[NATIVE_LOG10] = native_log10; t[NATIVE_SIN]   = native_sin;
    t[NATIVE_COS]   = native_cos;    t[NATIVE_TAN]   = native_tan;   t[NATIVE_ASIN]  = native_asin;
    t[NATIVE_ACOS]  = native_acos;   t[NATIVE_ATAN]  = native_atan;  t[NATIVE_ATAN2] = native_atan2;
    t[NATIVE_FABS]  = native_fabs;   t[NATIVE_ISNAN] = native_isnan; t[NATIVE_ISINF] = native_isinf;
    t[NATIVE_F64_TO_BYTES] = native_f64_to_bytes;
    t[NATIVE_TCP_CONNECT]  = native_tcp_connect;
    t[NATIVE_TCP_SEND]     = native_tcp_send;
    t[NATIVE_TCP_RECV]     = native_tcp_recv;
    t[NATIVE_TCP_CLOSE]    = native_tcp_close;
    t[NATIVE_TCP_LISTEN]   = native_tcp_listen;
    t[NATIVE_TCP_ACCEPT]   = native_tcp_accept;
    t[NATIVE_TCP_SET_TIMEOUT] = native_tcp_set_timeout;
    t[NATIVE_TCP_LOCAL_PORT] = native_tcp_local_port;
    t[NATIVE_SHA256]       = native_sha256;
    return t;
}
