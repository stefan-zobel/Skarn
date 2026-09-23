# The vMachine virtual machine

This document describes `vmcore`, the runtime that executes Skarn programs: how values are represented,
how bytecode is encoded and dispatched, how functions are called, and how memory is managed. It is written
for someone who wants to read or change the VM, or target it from another compiler. The Skarn compiler
itself is described in [Compiler.md](Compiler.md); the language is described in the
[Skarn Guide](../SkarnGuide.md).

The source is the reference. Where this document and the code disagree, the code is right, and
`vmcore/Opcodes.h` is the authoritative list of instructions.

## Overview

vMachine is a register-based bytecode interpreter in C++20. Its design choices:

- **NaN-boxed values.** Every runtime value fits in 64 bits: a `double`, or a tagged immediate or pointer
  stored in the unused NaN space.
- **A register-window calling convention.** There is no operand stack. Each function works on a window of
  registers, and a call slides the window so that the caller's outgoing arguments become the callee's first
  registers.
- **A `while{switch}` dispatch loop** that keeps the hot interpreter state (instruction pointer, register
  window, stack cursors) in local variables.
- **A precise, moving garbage collector.** Cheney's copying algorithm over two growable semispaces: bump
  allocation, no fragmentation, and collection cost proportional to the live data.
- **Index-addressed globals, heap strings and string interning.**

A Skarn program shares no memory between threads. It can run functions in parallel as tasks and actors
(see "Tasks and actors"). Each is a separate VM instance on its own thread, with its own heap, and values
cross between them only as copies: an argument, a result or a message. There is no `async` and no shared
mutable state, and each collector stops only its own heap. In general, several VM instances may run at the
same time on separate threads, each with its own heap, interner and streams, sharing only the read-only
image.
It runs on **Windows x64 with MSVC** and on **macOS arm64 with Clang**; both
build the whole project and pass every suite. A small platform layer (`vmcore/Platform.h`) is what the rest
of the runtime sees:

- **Memory** is reserved and committed with `VirtualAlloc` on Windows and with `mmap` + `mprotect`
  elsewhere. Every stack ends in a guard page — 4 KiB, or 16 KiB on Apple Silicon, where that is the page
  size.
- **Fatal faults** (a guard-page hit, an illegal instruction, integer division by zero) reach one handler
  wrapped around the dispatch loop: a structured-exception frame on Windows, and a signal handler on POSIX
  that records the faulting address and jumps to a landing point set with `sigsetjmp`. Both SIGSEGV and
  SIGBUS are installed there, because a `PROT_NONE` guard page is reported as SIGBUS on macOS and as SIGSEGV
  elsewhere. The landing point is per thread on both platforms: structured exceptions are per thread
  inherently, and the POSIX handler finds its landing point through a lock-free table keyed by thread id.
  It is a table rather than thread-local storage because on macOS a thread's first thread-local access
  allocates, which a signal handler must never do.
- **The instruction encoding** uses bit-fields, whose layout the C++ standard leaves to the implementation.
  The two supported toolchains lay them out the same way; a bytecode file carries a sentinel so that a
  reader which does not is rejected rather than misled (see "Bytecode container format").

The compiler and the driver contain no platform-specific code at all, and neither does the language
surface: the one place where a platform is named, `std::process`'s `sh()`, asks which one it is running on
(`rawOsId`, wrapped as `currentOs()`) and picks `cmd /c` or `/bin/sh -c` accordingly. What remains
Windows-only is the PowerShell documentation and example gates.

## Source files

The runtime is header-only except for one translation unit, `vmcore/vmcore.cpp`, which contains
`execute()`, the native functions and the root-forwarding hook. `vmcore` builds as a static library; the
compiler, the driver and the test programs include its headers and link it.

| file | responsibility |
|---|---|
| `Value.h` | the NaN-boxed `Value`, its tags, constructors, equality and hashing |
| `Instruction.h` | the 32-bit instruction word, its field layouts and builders |
| `Opcodes.h` | the instruction set: `OpCode`, `Layout`, `OpList[]`, bytecode version numbers |
| `Interpreter.h` | the dispatch loop `run_switch_loop` and its fault wrapper `run_switch` |
| `NumericOps.h` | promoting arithmetic helpers used by the loop |
| `Context.h`, `VM.h` | the per-execution interpreter state and the tables it reaches |
| `VM_Resources.h` | the register, return, frame-size and closure stacks |
| `Heap.h` | object headers and the copying collector |
| `opcodes/op_string.h`, `op_map.h`, `op_vec.h`, `op_bytes.h` | runtime helpers for strings, maps, vectors and byte buffers |
| `GlobalEnv.h`, `StringInterner.h`, `HashTable.h`, `HashingPolicy.h` | globals, interning and the host-side hash table |
| `StructType.h`, `FunctionTable.h`, `TypeUniverse.h` | struct descriptors, per-function metadata, type ids for trait dispatch |
| `NativeRegistry.h`, `Natives.h` | the native-function registry |
| `Fault.h` | `VmFault`, the structured runtime error |
| `BytecodeIO.h` | the `.skbc` bytecode file format |
| `ValueCodec.h` | copying a runtime value from one heap to another through a byte buffer |
| `ByteIO.h` | the little-endian byte primitives both formats above share |
| `Assembler.h`, `Disassembler.h` | building and printing bytecode |
| `Execute.h` | the public entry point `execute()` |

## Values

A `Value` is 8 bytes. Any bit pattern below the first tag is an ordinary `double`; the tags occupy the quiet
NaN space in the top 16 bits and leave 48 bits of payload:

| tag | meaning |
|---|---|
| `TAG_FUNCPTR` | a native function pointer (invisible to the collector) |
| `TAG_PTR` | a pointer to a heap object (traced by the collector) |
| `TAG_INT` | a 48-bit signed integer |
| `TAG_BOOL` | a boolean |
| `TAG_SPECIAL` | a sub-tagged immediate: `Nil`, `Undefined`, a function id (`Func`), the map tombstone, … |

A function without captures is a `Func` immediate carrying its 16-bit function-table id; a function with
captures is a heap closure. A default-constructed `Value` is `Undefined`.

**Numbers.** Integers are 48-bit two's complement and wrap on overflow. `-0.0` is normalized to `0.0` and
every NaN is canonicalized to one bit pattern when a double is boxed. The rules follow Java, adapted to 48
bits:

- Integer division truncates toward zero, and the remainder takes the sign of the dividend. `MIN / -1` wraps
  instead of trapping; dividing an integer by zero traps.
- Shift counts are masked with `& 63`. `>>` is arithmetic and `>>>` is logical on the 48-bit value.
- Generic arithmetic opcodes promote: two integers give an integer, anything involving a double gives a
  double. Double division by zero follows IEEE-754 and does not trap.
- The numeric comparisons are IEEE-ordered, so a NaN operand makes every relation false except `!=`. All six
  relations are real opcodes, because `a >= b` is not `!(a < b)` when NaN is involved.

**Equality and hashing.** `Value::operator==` promotes (`Int(1) == Double(1.0)` is true) while
`Value::hash()` mixes the raw bits. Bit-keyed containers therefore follow `SameValueZero`: all NaNs are one
key, `±0.0` is one key, and `Int(1)` and `Double(1.0)` are different keys. This is deliberate and matches
the way Java separates `==` from `equals`.

## Instructions

Every instruction is one 32-bit word with an 8-bit opcode. The remaining 24 bits use one of a few layouts:

| layout | fields |
|---|---|
| `r6` | destination and two operand registers of 6 bits each, plus a 6-bit `flags` field |
| `c2` | an 8-bit register and a signed 16-bit constant |
| `j` | a signed 24-bit jump offset |
| `call` | the callee's frame size (8 bits) and a signed 16-bit offset |
| `b`, `b1` | one or two registers and a signed branch offset |
| `q4` | four 5-bit registers and a 4-bit extra field |
| `wide` | a register and a 16-bit payload extending the preceding constant load |
| `prop` | two registers and a 12-bit slot index (struct field, function id or trait method id) |

A few opcodes read an `r6` word with a different meaning: `CALL_NATIVE` and `CALL_INDIRECT` use a field as an
argument count, and the fused trait calls use the 12-bit slot of the `prop` layout. `Opcodes.h` records the
layout of every opcode, and the disassembler is driven entirely by that table.

Branch and jump offsets are relative to the following instruction and counted in instructions, not bytes.
There is no flags register: comparisons are fused into branch opcodes.

Because `r6` addresses 64 registers, a function can use at most 64 registers at once. The compiler allocates
registers by peak concurrent use, which keeps real functions well below that limit.

**Field widths are checked.** The builders assert in Debug builds that every field fits. Where an emitter
could produce a value that does not fit, it throws instead of truncating, because a truncated register
number produces a well-formed but wrong instruction that no later stage can detect.

## The instruction set

There are 120 opcodes. The groups, with representative members:

| group | opcodes |
|---|---|
| control | `NOP`, `HALT`, `J`, `CALL`, `RET`, `TCO_CALL` |
| constants and moves | `LOAD_CONST`, `LOAD_CONST_WIDE`, `LOAD_CONST_POOL`, `LOAD_STR`, `LOAD_CONST_ARRAY`, `LOAD_FN`, `MOV`, `MOV_TAKE` |
| integer arithmetic and bits | `ADD_INT` … `USHR_INT`, `INCR`, `DECR` |
| integer compare and branch | `BEQ_INT` … `BGE_INT`, `SET_EQ`/`SET_LT`/`SET_LE`, `BT`, `BF`, `CMOV` |
| promoting arithmetic | `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `NEG` |
| promoting compare and branch | `SET_*_NUM`, `B*_NUM` |
| numeric conversion | `I2D`, `D2I` (saturating), `DROUND` (floor, ceil, trunc, round, round-half-even) |
| equality and ordering | `EQ`, `NE`, `LT`, `LE`, `EQ_DEEP` (structural equality) |
| booleans | `AND_BOOL`, `OR_BOOL`, `NOT_BOOL` |
| type inspection | `IS_INT` … `IS_UNDEF`, `IS_OBJECT`, `GET_TYPE_ID`, `GET_KIND` |
| globals | `LOAD_GLOBAL`, `STORE_GLOBAL` |
| structs | `NEW_STRUCT`, `GET_PROP`, `SET_PROP` |
| arrays and indexing | `ALLOC`, `ANEW`, `AFILL`, `ARRAY_GET`, `ARRAY_SET`, `LEN` |
| maps | `MAP_NEW`, `MAP_GET`, `MAP_GET_OR_TRAP`, `MAP_SET`, `MAP_HAS`, `MAP_DELETE`, `MAP_KEYS`, `MAP_VALUES`, `MAP_ITER_NEXT`, `MAP_KEY_AT`, `MAP_VAL_AT` |
| vectors and bytes | `VEC_NEW`, `VEC_NEW_CAP`, `VEC_PUSH`, `VEC_POP`, `BYTES_NEW_CAP`, `BYTES_FROM_STR`, `BYTES_TO_STR`, `BYTES_APPEND`, `BYTES_APPEND_RANGE` |
| functions and closures | `CALL_INDIRECT`, `TCO_CALL_INDIRECT`, `MAKE_CLOSURE`, `LOAD_CAPTURE` |
| trait dispatch | `PROTO_RESOLVE`, `RESOLVE_CALL`, `RESOLVE_TCO_CALL` |
| strings and output | `TO_STRING`, `PRINT`, `PRINTLN` |
| host and faults | `CALL_NATIVE`, `PANIC` |

A few facilities exist for hand-written bytecode and are not emitted by the Skarn compiler: truthiness
(`TO_BOOL`, `LNOT`), the eager boolean operators, `MAP_GET` returning `Nil` on a miss, and the unfused
`PROTO_RESOLVE`.

**Trait dispatch** needs no vtables. Every value already carries enough information to name its type: an
immediate's tag, a heap object's kind, or a struct's type id. `TypeUniverse.h` folds these into one dense
type index, and the dispatch opcodes look up `trait_table[method_id * width + type_index]` to find the
function id. A missing implementation traps.

**Structural equality** (`EQ_DEEP`) compares structs, arrays, vectors, maps and byte buffers by content with
an explicit worklist, so a long list or deep tree never overflows the native stack. Cyclic values are
compared too: once a comparison runs past a small number of steps, it remembers object pairs it has
already expanded and treats a pair met again as equal. Two values are therefore equal when they unfold
alike, a difference is found wherever it sits, and every comparison terminates. Small comparisons never
reach that point and pay nothing for it.

**`TO_STRING`** renders any value: numbers and booleans as text, structs as `Name { field: value }`, tuples
and lists in their surface syntax, arrays, vectors and maps as their contents, closures as `<fn>`. Nested
dumps are depth-limited as a cycle guard.

## Calling convention

The VM owns four stacks, all reserved at a large maximum and committed on demand, so their base addresses
never move:

- the **register stack** of `Value`s, where each activation owns a window;
- the **return stack** of 16-byte frames (saved instruction pointer and window);
- a parallel **frame-size stack**;
- a parallel **closure stack** holding each activation's caller closure.

The register stack starts at about 1 MiB and grows to 128 MiB; the return stack allows roughly a million
nested non-tail calls. Growth is checked at the three non-tail call opcodes. At the reserved limit the VM
raises a located "stack overflow" fault.

**Calls.** `CALL` slides the window by the **caller's** frame size and sets the current frame size to the
**callee's**, which the instruction carries. A caller therefore writes its outgoing arguments starting at
register `frame_size`, and those registers become the callee's `r0, r1, …`. The return value comes back in
the callee's `r0`, which is the caller's `r[frame_size]`. `RET` restores the saved window.

**Tail calls.** `TCO_CALL` reuses the current window: arguments are written to `r0, r1, …` and no return
frame is pushed, so tail recursion runs in constant stack. `TCO_CALL_INDIRECT` and `RESOLVE_TCO_CALL` do the
same for function values and trait methods.

**Function values.** `CALL_INDIRECT` calls whatever is in a register: a `Func` immediate or a closure object.
The callee's frame size and entry point come from the function table (`FunctionTable.h`). For a closure the
object becomes the current closure, and `LOAD_CAPTURE` reads its captured values. `MAKE_CLOSURE` builds a
closure from a contiguous range of registers.

**The frame size is a garbage-collection contract.** The collector scans exactly `frame_size` registers of
every activation. So:

- `frame_size` must cover every register that can hold a heap pointer at a safepoint. The assembler's
  `validate_frames` checks this for every function, and for top-level code when the compiler declares its
  size.
- Every register below `frame_size` must be live data or a non-pointer. A window reuses slots that a
  finished activation left behind. Three mechanisms clear such leftovers inside opcodes that run anyway:
  - `RET` clears the dying frame;
  - a tail call that shrinks the frame clears the abandoned part;
  - `MOV_TAKE` empties the return-value slot as the caller copies the result out.
- Debug builds verify every register root at every safepoint.

## Native functions

Native functions are host functions called from bytecode, such as file and network I/O, math, time, and
number parsing. They are identified by a stable **id**, never by an address, so bytecode can be written to
disk.

- `NativeRegistry.h` maps names to ids and records each native's arity and return category. Ids are append
  only.
- `build_native_table()` (declared in `Natives.h`, defined in `vmcore.cpp`) builds the table of function
  pointers that the driver passes to `execute()`.
- `CALL_NATIVE` loads the id from a register and calls `native_table[id]` with a pointer to its arguments.

A native returns a plain value, or a heap kind that the **compiler** wraps: for a `Result` native, a
returned string means `Err(message)` and anything else means `Ok(value)`; for an `Option` native, `Nil`
means `None`. The VM never needs to know the `Result` and `Option` types. `CALL_NATIVE` is a GC safepoint,
so a native copies its argument bytes out before it allocates.

That leaves two channels, which is a limit worth knowing when a native has a third thing to say. The
non-blocking socket natives are the case in point: "would block" is neither a value nor an error, so each
one carries it inside its success value — a negative descriptor, an empty array, a zero count — and the
standard library turns that into an ordinary enum before a program sees it. Picking a carrier the type
system can already describe keeps this out of the compiler; a heterogeneous array would need a special
case in code generation, as the process-spawning native does.

The socket natives work on small integer descriptors into a per-execution table, never on raw OS handles,
and close whatever is still open when the execution ends. `tcpConnect` gives up after 10 seconds: on POSIX
through a non-blocking connect and `select()`, on Windows through a blocking connect bounded by `TCP_MAXRT`,
because there the `select()` wait can add one timer tick (about 15 ms) even on loopback.

**Adding a native:**

1. Append a `NativeId` and extend `native_id_of`, `native_return_of` and `native_arity`.
2. Write the `NativeFunc` and add it to `build_native_table()`.
3. Declare its Skarn signature and its standard-library module in the checker (`add_native` in
   `static_compiler/Check.cpp`).
4. Add a test.

No opcode is needed.

## Heap and garbage collector

Every heap object has an 8-byte header followed by its payload. A `Value` points at the payload, and
`GcObject::from_slots` recovers the header. Object sizes are padded to 8 bytes, and the same padded size is
used by allocation, copying and scanning.

| kind | payload | scanned |
|---|---|---|
| `KIND_ARRAY` | `Value[n]` | yes |
| `KIND_OBJECT` | a struct's fields; the header holds its type id | yes |
| `KIND_CLOSURE` | captured values; the header holds its function id | yes |
| `KIND_STRING` | bytes and a trailing NUL | no |
| `KIND_MAP`, `KIND_VEC`, `KIND_BYTES` | a small header pointing at a separate backing store | yes |

**Allocation** bumps a pointer in the active semispace. When it is full, the allocator collects, retries,
grows the heap if necessary and retries again. Each semispace starts at 4 MiB and may grow to 1 GiB. Both
double when more than 75 % of the space is still live after a collection.

**Collection** is Cheney's algorithm. The semispaces swap; every root is rewritten in place to point at its
object's copy; then a scan pointer walks the copied objects and forwards the pointers inside them. To-space
serves as the work list, dead objects are never touched, and the reclaimed memory is exactly what was not
copied.

**Roots:**

- the registers of every activation, up to its frame size;
- explicitly registered slots: globals and the string-literal pool;
- the interner's table;
- the current closure and the closure stack.

The constant pool is deliberately pointer-free and therefore not a root.

**The rule for C++ code:** any allocation may move every object. A heap `Value` held in a C++ local variable
that is not a root becomes invalid across an allocation, so re-read it through a root afterwards. Debug
builds poison the old semispace to make such mistakes fail loudly.

The collector is stop-the-world and has no finalizers. A generational nursery was built and measured, and
lost: Cheney already handles short-lived garbage at almost no cost, and copying a large, slowly growing
data structure repeatedly made programs that build such structures slower.

## Maps, vectors and byte buffers

The three growable containers are each two heap objects: a small **header** and a separate **backing**.
Growth replaces the backing without changing the header, so every reference to the container stays valid.

- **Map** (`op_map.h`): the header holds the backing, the live count and the used-slot count. The backing is
  a `KIND_ARRAY` of interleaved keys and values, probed linearly, with tombstones for deleted entries.
  - String keys hash and compare by content, which a moving collector cannot change. Other immediates key by
    their bits (`SameValueZero`).
  - A non-string heap object is not a valid key and traps: its address changes when the collector moves it.
  - Iteration order is the backing order. The `MAP_ITER_NEXT` / `MAP_KEY_AT` / `MAP_VAL_AT` cursor uses an
    integer position, so iterating stays memory-safe across a collection and even when the map grows.
- **Vector** (`op_vec.h`): the header holds the backing and the count. The backing is a `KIND_ARRAY` whose
  first `count` slots are live; it doubles when full.
- **Byte buffer** (`op_bytes.h`): the same shape as a vector, but the backing is a raw `KIND_STRING`, one
  byte per element. A read yields an `Int` from 0 to 255; a write masks to a byte.

`ARRAY_GET`, `ARRAY_SET` and `LEN` accept arrays, vectors and byte buffers; `LEN` also accepts maps and
strings.

## Strings, globals and interning

Strings are immutable **byte strings**: `KIND_STRING` objects with no encoding awareness in the VM.
Comparison is byte-wise and length is the byte count; code points are a library layer in Skarn. String
literals are interned when execution starts and rooted in a pool that `LOAD_STR` reads. The interner is a
strong root and hashes by content.

Globals are a fixed array of `Value` slots addressed by an 8-bit index, each registered as a root.
`HashTable.h` is the host-side open-addressing table behind the interner; bytecode maps use their own
representation, described in the previous section.

## Runtime faults

A serious runtime error aborts the program with a message, a source position and a call trace. There is no
recovery inside the script. Such errors are division by zero, an index out of bounds, a missing map key, a
wrong receiver kind, a missing trait implementation and `panic`.

The dispatch loop calls an out-of-line `raise_located`, which builds the message from the line, column,
function and module tables and throws a `VmFault` (`Fault.h`). The driver renders it with a caret into the
right source file, including imported modules and the standard library:

```
error: division by zero at line 2 (in boom)
  called from line 5 (in <script>)
  2 |   n / 0
    |       ^
```

A frame replaced by a tail call does not appear in the trace.

The dispatch loop itself contains no exception handler. `run_switch` wraps it in one frame for the faults
that cannot be located: an access violation, an illegal opcode, heap exhaustion. On Windows that frame is a
structured-exception handler; on POSIX it is a `sigsetjmp` landing point that the SIGSEGV/SIGBUS handler
jumps to after recording the faulting address. Either way the access violation is classified by that
address: inside a VM stack region it is a stack overflow, anywhere else it indicates a dangling pointer.

Heap exhaustion is reported only where the operating system says so. Windows fails the commit and the VM
raises "Heap exhausted"; on POSIX, committing an already-reserved mapping costs nothing and therefore
succeeds, so genuine exhaustion surfaces later, when the memory is first touched. That is a platform
limitation, not something the fault ladder can recover.

## Bytecode files (SKBC)

A compiled program can be saved as a `.skbc` file and run later without recompiling
(`static_vmrun --emit-bytecode` / `--run-bytecode`). `BytecodeIO.h` defines the format:

- a header with magic, two version numbers and a layout sentinel;
- tagged chunks: code, constant pool, constant arrays, struct types, string literals, function table, trait
  table, metadata, and an optional debug chunk with line, column, function and module tables;
- a CRC-32 footer.

Integers in the container are little-endian. The instruction words are stored exactly as the compiler that
wrote them packed their bit-fields — the C++ standard does not fix that layout — and the header carries a
sentinel word built from a signed bit-field, so a reader that packs them differently rejects the file
instead of misreading it.
`BYTECODE_ISA_VERSION` changes whenever the meaning or encoding of an instruction changes, so older bytecode
is rejected. `CONTAINER_FORMAT_VERSION` versions the file structure. Unknown optional chunks are skipped;
unknown required chunks are rejected.

## Copying values between heaps

`ValueCodec.h` moves a runtime value from one heap to another. It is the channel between executions that
each own a heap: a task's argument and result and every actor message travel through it (see "Tasks and
actors"). `vcodec::encode` writes the value
into a byte buffer without allocating; `vcodec::decode` rebuilds it in the destination heap without looking
at the source. Going through a buffer, instead of copying heap to heap directly, means a collection in the
destination can never move a half-copied graph out from under the copier.

- Every heap object is written once and referred to by index, so shared parts stay shared and cyclic
  values are copied correctly.
- Each slot is stored as its value type plus payload and rebuilt through the normal constructors, never
  as raw bits, so a decoded value can never turn into a pointer.
- The rebuild keeps every object it has created registered as a collector root, so it stays correct even
  when the destination heap collects while the graph is being built.
- Maps are copied without rehashing: their keys hash by content (strings) or by their bits (numbers and
  booleans), and neither changes when the value moves to another heap.
- Closures and raw function pointers are refused. A function value that is only an index into the shared
  program travels like a number.
- Copied strings are not interned in the destination. Nothing depends on that: the VM compares strings,
  and looks up string map keys, by content everywhere.
- The decoder checks every object against the program: a struct's type and field count, and the size,
  backing and counts of every vector, byte buffer and map. A buffer that describes an object the VM could not
  have built is rejected, so a bad buffer can yield a wrong value but never a malformed object.

The codec cannot tell a native handle, such as a socket, from any other struct holding an integer. Keeping
handles out of a message is the type checker's job, not the codec's.

This format carries no magic number, version or checksum: it never outlives the process that wrote it.
For the same reason it writes struct type ids and function ids as plain numbers, so a buffer can only be
decoded by an execution running the same program.

## Tasks and actors

A task or an actor is an **isolate**: a function that runs on its own thread, with its own heap, interner
and stacks, sharing the program with its starter read-only. Values cross between isolates only as copies.
- A **task** runs a function once and hands back one value.
  - `rawSpawn(fn, arg)` copies the argument out of the starter's heap, starts the task, and returns a task
    id.
  - `rawJoin(id)` waits for it and reports whether it returned a value.
  - `rawTaskTake(id)` copies that value into the starter's heap, or returns the task's fault message.
- An **actor** runs a function that loops on a **mailbox** until it is told to stop.
  - `rawSpawnActor(fn, arg)` starts one and returns its address.
  - `rawSend(pid, msg)` copies a message into its mailbox, and answers false once the actor has ended.
  - `rawReceive(inbox, ms)` waits for the next mail, with an optional timeout. The mail is a message, a
    report that an actor this isolate started has faulted, or `Stop`. `rawMailMsg`, `rawMailFrom` and
    `rawMailReason` read it.
  - The main program gets a mailbox of its own through `rawMainInbox()`, once.
  - `rawNewInbox(capacity)` gives an actor or the main program a further inbox with an address of its own,
    typically for a reply; `rawCloseInbox(inbox)` closes one again, and later sends to it answer false. The
    end of an actor closes all of its inboxes.
  - `rawStopActor(pid)` tells an actor to end: `Stop` goes into every inbox it owns, so it stops wherever
    it waits. It is a message, so an actor that never receives is not ended by it, and there is no kill.
  - `rawStopRequested(inbox)` answers whether anyone has told this actor to end. It reads the flag that
    every queued `Stop` sets and consumes no mail, so an actor whose loop is its own work — one that never
    reaches a receive — can ask and end itself. Any one of its inboxes answers: a stop reaches them all.
  - `rawMonitor(pid, inbox)` asks to be told when that actor ends. Exactly one report follows, into an
    inbox of the caller's own, as the same mail a crash report uses: the address plus a reason — the
    fault message, `"normal"` when the actor returned, or `"gone"` when it had already ended, in which
    case the report comes at once and the call answers false. It watches that one actor, not the address,
    so an actor started into the same slot afterwards is not watched. The automatic report to the starter
    is unaffected and stays crash-only. A report also means the ADDRESS IS FREE: the actor's mailboxes are
    closed, and a slot vacated, before either report goes out, so an actor started into that slot on the
    strength of the report never meets an address its predecessor still holds.
  - `rawSelect(boxes, ms)` waits until one of SEVERAL inboxes has something and answers which — the index
    into the list, or `-1` when the timeout passed. It takes nothing out; the `rawReceive` that follows
    reads the inbox that index names and cannot wait, because only the owner takes mail out of an inbox.
    Ties go to the lowest index, so the order of the list is a priority order. Every inbox must belong to
    the caller, and watching nothing with no deadline is refused — no event could ever end that wait.
- `rawTaskInput()` and `rawSelfId()` are internal: an isolate reads its argument and its own id with them.

**An address that outlives its actor.** A restarted actor is a new actor, so anyone holding the old
address would have to learn the new one. A **slot** is a mailbox made before its actor: `rawNewSlot(capacity)`
returns its address, and `rawSpawnInto(slot, fn, arg)` starts an actor that receives on it instead of on a
mailbox of its own. When that actor ends, the slot keeps its address, so another actor can be started into
it and answers where its predecessor did.
- What the ended actor had not read is dropped: delivering the message that crashed it to its successor is
  the classic crash loop.
- What arrives while the slot has no actor waits in it for the next one, instead of being refused.
- The report of a crash names the slot, so a supervisor recognizes its child across restarts.
- `rawReleaseSlot(slot)` ends the address: later sends answer false, and an actor still running in it is
  told to stop — also when it had been told once already, because releasing drops whatever was queued.

**Waiting.** `rawSleep(ms)` makes this isolate wait, and only this one: every isolate has a thread of its
own. It is what a loop that must poll uses — once an actor has received `Stop`, every further receive
answers `Stop` at once, so an actor winding down can no longer wait on its inbox.

**Waiting on several inboxes.** A receive waits on one mailbox's condition variable, which is why
`rawSelect` cannot use it. Instead every isolate has a **wake pad**, and every mailbox wakes its owner's
pad in addition to its own condition variable — so one wait covers all of an actor's inboxes. A send pays
for this only while somebody is actually parked on a pad: with nobody waiting, waking is a single atomic
read and no lock. The two locks are never nested — a mailbox is released before its owner's pad is woken —
so the select scans the mailboxes holding no pad lock, and a counter bumped on every wake is what catches
mail that arrives during the scan.

**Back-pressure.** An inbox may be **bounded** — `rawSpawnActorBounded(fn, arg, capacity)` starts an actor
whose mailbox holds at most `capacity` messages, and `rawNewInbox` takes a capacity too (0 = unbounded).
A `rawSend` to a full inbox waits until the receiver has taken a message, so a fast sender is slowed to its
receiver's pace instead of filling memory; `rawTrySend` never waits and reports "full" instead. Crash
reports and `Stop` always get through. A waiting sender is released with false when the receiver ends or
when the world ends, so a full inbox cannot hold up the end of the program. Two actors waiting to send to
each other's full inbox would wait forever — and **that is detected and reported**, see below.

**Deadlock detection.** A blocked send and a blocked join are the only waits that name their target
exactly: a receive could be answered by anyone, so it says nothing. A cycle over those two is therefore a
proof rather than a guess, and it is one no message could break — `Stop` and crash reports ignore an
inbox's capacity and never free a sender waiting for room. A sender that has waited past a grace period
(250 ms, doubling to 2 s) looks for such a cycle; when one is confirmed, **every** participant ends with a
located fault naming the ring, so each of their starters hears about it and a supervisor's strategy
decides what follows. A send that never blocks pays nothing for this, and an unbounded inbox is untouched.
What it does not claim: the verdict is that the *participants* cannot resolve the cycle — releasing a slot,
closing an inbox, or the end of the program still can.

Everything started under one call of `execute()` forms a **world**. Addresses are unique within it, so an
actor's address can be sent inside a message. When that call returns, the world sends every actor `Stop`
and waits for every isolate to end, so no isolate outlives the program it runs. Nothing can be cancelled.

`execute()` publishes the tables it was given as a `ProgramImage`, and every isolate of a world runs the
image of the call that created the world. An isolate does not start at instruction 0. Its code is the
program followed by a short entry stub, which reads the argument, calls the function through the ordinary
calling convention, and halts. Appending the stub leaves every function's address unchanged.

- **Failures are values.** A task that faults does not stop its starter: `rawJoin` returns false and the
  message is available as a string. An actor that faults is reported to the isolate that started it,
  through its mailbox.
- **Output.** A task's output is collected and written out when the task is joined, in join order, so it
  does not depend on scheduling. An actor writes to the main program's output stream a line at a time, so
  lines from different actors never mix. Isolates read an empty standard input.
- **Memory.** An isolate starts small — 64 KiB of register stack and 1024 return frames, and an actor's heap
  at 64 KiB — and grows like any other execution. Hundreds of actors are therefore affordable.

**Handing a connection to another isolate.** A socket is a descriptor into the socket table of ONE
execution, so it cannot travel inside a message. It changes owner through the world instead:
- `rawHandOff(fd)` takes the socket out of the caller's table (without closing it), parks it in the world
  under a fresh ticket, and returns the ticket.
- `rawTake(ticket)` moves it into the caller's table and returns a new descriptor. A ticket can be taken
  once; a ticket nobody takes is closed when the world ends.

A descriptor is a table slot plus a generation, and the generation advances whenever a slot's socket is
closed or handed off. A descriptor kept after either is therefore refused — with the message "socket was
handed to another actor" after a hand-off — and never reaches a later connection that reuses the slot. The
ticket itself is a plain integer; that a program cannot forge one is a rule of the language (see "Actors" in
[Compiler.md](Compiler.md)). Only connections move; listeners stay with the isolate that opened them.

## Embedding the VM

A compiler's entire contract with the VM is the instruction set plus the `Assembler` that emits it. It never
touches dispatch, value boxing or the collector. `Assembler.h` builds bytecode with labels and fixups,
maintains the constant pool, string literals, struct types and function table, and validates frame sizes when
it assembles.

The finished program is handed to `execute()` (`Execute.h`) as one bundle. Everything except the bytecode
has a default, so small hand-built programs pass only what they use:

- the instruction words and the top-level frame size;
- the constant pool, constant arrays, struct types, string literals and function table;
- the trait-dispatch table and its dimensions;
- line, column, function-name and module tables for fault reports;
- the native table, the program arguments, and the output and input streams.

The caller owns the heap, the globals and the interner and passes them in. There is no global or
thread-local interpreter state, so executions are independent — including at the same time on different
threads. Each concurrent execution needs its own heap, globals, interner, program arguments and output and
input streams; the constant pool, struct types, function table, trait table and native table are read-only
and may be shared. The two stream parameters default to the process-wide standard output and input, which
two concurrent executions must not both take, or they will interleave their output and compete for the
same input.

## Dispatch

`run_switch_loop` in `Interpreter.h` is a single `while (true) { switch (opcode) }` loop. The instruction
pointer, register window, return-stack cursors and current frame size live in local variables, which the
compiler keeps in machine registers.

**Safepoints.** The collector reads roots through `Context`, but the live cursors are locals. So the loop
writes them back (`SYNC_TO_CTX`) immediately before every opcode that can allocate. Between allocations the
copies in `Context` are allowed to be stale.

**Why a switch.** Threaded dispatch (a tail call per handler) is not faster here: the dispatch branch is
predicted almost perfectly, and the real cost is the work around each instruction and interpreter state
spilled to memory. A single loop with its state held in registers is faster than threading, and it builds
with the default `/sdl` and whole-program optimization settings. The fault frame sits outside the loop in a
separate, non-inlined function: on Windows because a handler inside the loop would force that state back
into memory, and on POSIX because `sigsetjmp` returns twice, which would leave the caller's own variables
indeterminate if the two were inlined together.

## Invariants

These are design decisions guarded by `static_assert`s or tests; change them only deliberately:

1. `Context` is exactly 64 bytes, `Value` exactly 8, a return frame exactly 16.
2. Branch offsets are relative to the next instruction and counted in instructions.
3. There is no flags register; integer `>`/`<=` branches are written by swapping operands, the numeric family
   has all six relations.
4. Every allocating opcode synchronizes the cursors into `Context` first.
5. The collector moves objects; pointers are precise and always point at a payload start; collection happens
   only at allocation safepoints.
6. `TAG_PTR` (traced) and `TAG_FUNCPTR` (untraced) stay distinct.
7. `frame_size` bounds, and is bounded by, the live pointer registers of each frame.
8. The bytecode encoding rests on an implementation-defined bit-field layout. The supported toolchains
   agree on it; the layout sentinel in a `.skbc` header is what turns a disagreement into a rejected file
   rather than a silent misread.

Integer, boolean and generic numeric opcodes do not check operand types in Release builds; the compiler
guarantees them, and Debug builds assert them. Heap operations do check their receiver and raise a located
fault.

## Adding an opcode

1. Add it to the `OpCode` enum and to `OpList[]` with its layout in `Opcodes.h`. Ids are stable; append.
2. Add a `case` to `run_switch_loop` in `Interpreter.h`. If it can allocate, call `SYNC_TO_CTX()` first and
   re-read heap values after the allocation.
3. Add an `Assembler` emitter, and extend `max_reg_of` / `validate_frames` if it reads registers in an
   unusual way.
4. Extend the disassembler only if you introduce a new layout.
5. Bump `BYTECODE_ISA_VERSION`.
6. Add a `test_*` to `vm_tests` and register it in `main()`.

A new opcode is worth adding when it removes work from a hot path: several dispatches collapsed into one on
code that actually runs often.

## Tests

`vm_tests` is a console program whose `main()` is the test suite. Each test builds bytecode with the
`Assembler`, executes it and checks the result; the program prints a pass/fail summary and exits non-zero on
any failure. It covers every opcode family, the calling convention and frame-size contract, the collector,
containers, strings, closures, trait dispatch, natives and the bytecode format.

On Windows:

```bash
msbuild vMachine.sln /p:Configuration=Release /p:Platform=x64
x64\Release\vm_tests.exe
```

On macOS, through CMake:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

`ctest` runs `vm_tests`, the compiler suite and the language-server self-test. The hardware-fault probe is a
separate entry (`vm_tests --fault-probe`), because it deliberately dereferences a bad pointer to prove the
fault frame classifies and recovers it — its failure mode is a killed process rather than a failed
assertion.
