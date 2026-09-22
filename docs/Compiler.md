# The Skarn compiler

This document describes `static_compiler`, the front end that turns Skarn source into bytecode for the
virtual machine: how a program is checked, how names and modules are resolved, how the code generator lowers
it, and how the whole pipeline is verified. It is written for someone who wants to read or change the
compiler. The language itself is described in the [Skarn Guide](../SkarnGuide.md), and the machine the
compiler targets in [VirtualMachine.md](VirtualMachine.md).

## Overview

Skarn is statically type-checked with a **sound** type system whose types are **erased** at run time, the
model of OCaml, Gleam and Rust's generics. Erasure is the central design constraint: the VM performs no type
checks of its own, so the checker is the only guard. Anything it lets through runs as untyped bytecode.
Most of the verification described at the end exists because of that.

The pipeline:

```
source ─ lexer ─ parser ─ checker ─ tree-shaker ─ code generator ─ Assembler ─ bytecode
                                    (module loader in front, standard library prepended)
```

`svc::compile(source, prelude)` compiles a single file; `svc::load_modules` and `svc::compile_modules` do the
same for a program of several files. Both return an `svc::Module`: the bytecode plus every table `execute()`
needs. A program the checker rejects is never lowered.

## Source files

Everything lives in namespace `svc`.

| file | responsibility |
|---|---|
| `Token.h`, `Lexer` | tokens; literals (radix, digit separators, raw strings), string interpolation, automatic statement separation |
| `Parser`, `Ast` | recursive-descent and Pratt parser; the typed AST, where every expression carries a type slot |
| `Types` | the semantic type representation |
| `Solver` | `TypeContext`: type variables, union-find, unification, subtyping, instantiation |
| `Check` | the checker: declarations, expressions, patterns, exhaustiveness, warnings, diagnostics |
| `Codegen` | register planning and lowering to bytecode, trait tables, inliner, scalar replacement |
| `Compiler` | the public API: prelude handling, tree-shaking, compiler options |
| `Loader` | the file-system-free module loader: import graph and cycle detection |
| `Naming.h` | name mangling helpers |
| `std/*.skn`, `std/modules.manifest` | the standard library, in Skarn |

`docs/skarn_grammar.ebnf` describes the syntax; where it and the parser disagree, the parser is right.

## The driver: `static_vmrun`

`static_vmrun` reads the entry file, loads its imports from disk, compiles, and runs the program with the
native functions attached:

- `import net::http` loads `net/http.skn` relative to the entry file. A missing module or an import cycle is
  reported before checking.
- Arguments after the script name reach the program through `args()`.
- `--strict` makes warnings fatal.
- `--dump-ast` type-checks and prints the typed AST without running. It is exactly the program the code
  generator would see, which makes it the quickest way to tell a checker problem from a code-generation
  problem.
- `--emit-bytecode <file>` writes a `.skbc` image; `--run-bytecode <file>` runs one without compiling.
  `--strip-debug` omits the source-position tables.
- `--no-inline` and `--sroa` switch the two optional code-generation transforms.

Exit code 0 means success. 1 means a compile error or a runtime fault, reported with a caret into the right
source file. 2 means a driver error.

## The language server: `skarn_lsp`

`skarn_lsp` gives an editor live diagnostics, an outline, hover, go to definition, find references, rename,
completion and signature help; it is described in [LanguageServer.md](LanguageServer.md). It runs the front
half of the compiler only, through entry points the compiler provides for tools and never uses itself:

- `svc::load_modules_for_tools` lexes and parses in an **error-tolerant** mode: every syntax error is
  collected, and parsing resumes at the next statement, match arm, member, field, variant or item, leaving
  the failed construct out whole, so the tree has no error nodes and the checker runs on it unchanged.
- `svc::check_modules_for_tools` returns the checked program with every error and warning instead of
  throwing, writes the final inferred type into every expression, and lists the builtins and natives a
  program may call, each with the module whose `use` reaches it and its signature (hand-written for a
  builtin the checker types per call, one line per accepted shape).
- The parser records the position of every name token (declared names, the member after `.`, the tail of a
  qualified path, the names in a `use` list); the compiler itself does not read them.
- For the formatter, every token carries its source bytes, `Lexer::tokenize_with_comments` also returns the
  comments, and `Parser::set_layout` reports where each statement, match arm and member starts and what each
  `{` opens.

## Modules and the standard library

One `.skn` file is one module. Modules are a **name-resolution layer**: every top-level name is mangled to
`module::name`, and the back end works on one flat program exactly as before. A single-file program is simply
a program with one module.

- `import a::b` loads a module; `use a::b::name`, `use a::b::{x, Y}` and `use a::b::*` bring names into
  scope; `mod::name` is a qualified reference.
- Items are **private by default**. `pub` exports a function, type, enum, trait or constant; struct fields and
  enum variants follow their item, and methods follow their type or trait.
- A glob import is **weak**: a local definition or an explicit `use` wins over it, while two explicit imports
  of the same name are an error.
- **Orphan rule:** `impl Trait for Type` requires the trait or the type to be defined in the same module.
- **Uppercase and lowercase decide** what a `::` path means: a lowercase head is a module, an uppercase head a
  type, trait or enum (`Enum::Variant`, `Type::method`, `Trait::method`).
- The file being run is a module too, and it is the one nothing can import.

The import rule is the same for every kind of item:

| form | a `pub` item | a native gated by a module | a private item |
|---|---|---|---|
| nothing, or `import m` alone | not in scope (a user module allows `m::name`) | no | no |
| `use m::*` | yes | yes | skipped |
| `use m::name` / `use m::{name, …}` | yes | yes | error: private |
| `use m::other` | not `name` | no | no |
| `m::name` without `import m` | error: not imported | — | — |

**The standard library** is written in Skarn, in `static_compiler/std/`, one file per module, in the order
given by `std/modules.manifest`. A pre-build step (`gen_prelude_embed.ps1`) embeds the files into the
compiler, so a program always uses the built-in copy; the library is never read from disk at run time.
`std::core`, `std::iter` and `std::string` are imported into every module implicitly; the other modules need
a `use`. Natives are gated by module in the same way (`sqrt` needs `std::math`, `readFile` needs `std::io`);
`std::env` exists only to gate natives and has no source file.

Every library function passes through the checker like user code, and the tree-shaker removes whatever a
program does not reach.

**Adding to the library:** edit the right `std/*.skn` file and rebuild. A new module also needs a line in the
manifest. A function that needs the host needs a native: see "Native functions" in
[VirtualMachine.md](VirtualMachine.md).

## The type checker

### Soundness and erasure

The checker has no `any` or unknown type to fall back on. When it cannot establish a type, it rejects the
program. An internal error type suppresses follow-up errors, and every place that can produce one must
report a diagnostic.

Its central pattern is **decide once, record, obey**. Where the code generator needs an answer that depends on
types or scopes, the checker writes the answer into the AST and code generation reads it instead of working
it out again:

- the `Int` → `Double` widening at a node (`Expr::widen_double`);
- whether a name refers to a user function or a built-in (`IdentExpr::ambient`);
- whether `Head::m(x)` names an inherent method or a trait method (`IdentExpr::inherent`);
- which trait an unqualified trait-method call `x.m()` / `m(x)` dispatches to (`IdentExpr::resolved_trait`,
  `FieldExpr::resolved_trait`).

Several real defects came from two parts of the compiler answering one question independently and
disagreeing.

**Expressions may nest 200 levels deep.** Parsing and checking are recursive descent, so nesting costs
stack; beyond the limit the program is rejected with *expression nested too deeply* instead of the compiler
running out of stack. No realistic program comes close — a chain of calls or pipes is already limited to
about 64 by the register window — and the limit is a count, so every build rejects exactly the same
programs.

### Inference

The checker runs in two passes. The first collects every signature; the second checks bodies
bidirectionally: an expected type flows down into an expression, an inferred type flows up. Inference is
local:

- Function signatures are annotated, and locals are inferred.
- A lambda takes its parameter types from the expected function type when there is one.
- A generic call solves its type parameters from the arguments and, if needed, from the expected result type.

There are exactly two subtyping edges: `Int <: Double` (applied where a value meets an expected type) and
`Concrete <: dyn Trait`. Neither propagates through a type constructor, so containers are invariant. Joining
branch types (in `if`, `match`, or the `break` values of a `loop`) is exact.

### Generics, traits and `dyn`

- **Generics** are first-order and rank-1 with trait bounds. One body serves every instantiation.
  - A type argument a call cannot yet decide is settled after the whole body has been checked, so it may be
    fixed by a later argument of an enclosing call or by a later statement (`fold(xs, Set::new(), fn(acc:
    Set[Int], x: Int) …)`). Its bounds are checked against that later type and reported at the call. A type
    argument nothing fixes is an error for an associated function (`Type::f()`) and for a bounded type
    parameter; an unbounded one of a free function may stay open.
- **Traits** may be generic (`trait Iterable[T]`). A trait's type parameter can be solved as an output from the
  matching impl, which gives associated-type-like inference. Blanket impls (`impl[T: Show] Tr for T`) are
  allowed; a concrete impl wins over a blanket one.
  - An impl for a generic type may carry bounds of its own (`impl[T: Show] Show for Box[T]`). It applies only
    to the instantiations that meet them, at every place a type is asked to satisfy a trait: a bound, a method
    call, a `dyn` coercion, a blanket built on top. When the wrapped type is still open at that place, the
    bound is settled later, like a deferred type argument. An impl of a subtrait must require at least what
    the supertrait's impl for the same type requires.
  - Several traits may declare a method with the same name. An unqualified call `x.m()`, `m(x)` or `x |> m`
    is resolved by its receiver: the trait its type may implement (an impl for its head, a `dyn` trait and
    its supertraits, a type parameter's bounds, a blanket impl). A tie goes to the trait visible in the
    calling module (its own, the standard library's, or a `pub` one); a receiver that implements two of them
    is an error naming both. With a single declaring trait the rule is the plain lookup it always was.
  - A trait method may have its own bounded type parameters. An impl must match the trait's signature; it may
    drop a bound or narrow it to a supertrait the trait's bound implies, but not add one or change a bound's
    type arguments, since callers prove only the trait's bounds. A trait with a generic method cannot be used
    as `dyn`.
- **Inherent impls** give a type its own methods and associated functions. A method call `recv.m()` resolves
  field first, then inherent method, then trait method. `Type::m(x)` and `Trait::m(x)` reach a method
  explicitly.
  - An inherent impl on a built-in type (`String`, `Vec`, …) is forbidden by the orphan rule, so functions
    over built-ins stay free functions.
  - Built-in functions and natives can only be called, never used as values.
- **`dyn Trait`** values are the plain values themselves: no box, no vtable. A method may appear in a `dyn`
  type only if it takes `self` and does not mention `Self` in another parameter.
- **Sealed marker traits.** `Eq`, `Hashable` and `Sendable` are derived by the compiler and cannot be
  implemented by users; see "Language semantics" below and, for `Sendable`, "Fork-join tasks". The fourth
  marker, `MustUse`, is open; see "Warnings and diagnostics".

Higher-kinded types, higher-rank polymorphism and `where` clauses beyond a parameter's own bounds are not
part of the language.

### Patterns and exhaustiveness

`match` is checked for exhaustiveness and for unreachable arms with Maranget's usefulness algorithm.
Or-patterns expand to rows, capped at 64. Above the cap an arm counts as an opaque row, which can only cause a
spurious "non-exhaustive" error, never a missed case.

`if let` and `while let` are parsed into `match`. Range bounds are literals or named constants.
List patterns apply to `List` only.

### Mutability and closures

- A binding is immutable unless declared `let mut`. Assigning a field or element (`p.x = …`, `a[i] = …`)
  requires the root binding to be `mut`.
- So does passing a named binding to a `mut` parameter, including `mut self`. `mut` is an attribute of the
  parameter, not part of the type. An impl may not add `mut` to a parameter its trait declares without it.
- Lambdas capture **by value**. Assigning to a captured name inside a lambda is an error, because the write
  could never be seen outside. Reassigning a variable after a named lambda captured it is a warning, because
  the lambda keeps the old value.

### Warnings and diagnostics

The advisory tier warns about an unused binding, an ignored `Result` or `Option`, an ignored value of a type
marked `MustUse`, and a duplicate literal key in a map literal. An ignored `Result` or `Option` at the end of a
`-> ()` body is an error rather than a warning. `--strict` makes every warning fatal.

`MustUse` is an open marker trait in `std::core`: `impl MustUse for Outcome {}` makes a dropped `Outcome` warn,
and `impl[T: MustUse] MustUse for Timed[T] {}` passes the mark through a wrapper. Erasure types may implement
it, because nothing dispatches through it; for the same reason it cannot be used as `dyn` or as the bound that
selects a blanket impl.

Diagnostics carry a caret into the right file and module. Several carry more:

- a second caret naming the parameter that demanded a type;
- a hint naming the module to `use` when an unknown name is exported somewhere else;
- specific explanations for common mistakes: a missing `intoIter`, indexing a string, a built-in used as a
  value, `pub` on a method.

## Language semantics

Each rule is described for users in the guide; this is the compiler's side of it.

### Numbers

`Int` is 48-bit and wraps; `Double` is IEEE-754. There is no implicit conversion except the `Int` →
`Double` widening where an `Int` value meets an expected `Double`. `toInt` saturates (`NaN` → 0, overflow to
the extreme values). `floor`, `ceil`, `trunc`, `round` and `roundHalfToEven` map to one rounding opcode. Integer
division by zero traps; bitwise operators are `Int`-only.

### Equality

`==` is structural. On a scalar or a string it lowers to the VM's general `EQ`. On a struct, tuple, enum,
`Vec`, `Array`, `Map` or `Bytes` it lowers to `EQ_DEEP`, which compares by content. A map compares
independently of order. A value that refers back to itself through a `mut` field compares like any other:
two values are equal when they unfold alike.

A type is comparable only if all its components are. A function anywhere inside makes `==` a compile error,
and a generic `==` needs a `T: Eq` bound. Doubles keep IEEE semantics inside composites, so a struct holding
`NaN` is not equal to a copy of itself.

### Map keys

`Map[K, V]` and `Set[T]` require `K: Hashable`. `Hashable` is sealed: it holds for `Int`, `Double`, `Bool` and
`String`, plus the erased newtypes, `Char` and integer-backed enums. A struct key is a compile error, because
the VM cannot hash a heap object whose address the collector changes.

### Strings and conversion

`+` concatenates a `String` with a `String`, `Int`, `Double` or `Bool`; any other operand needs `toString`.
The checker decides this statically, so at run time the VM only sees string concatenation.

String interpolation and format specifiers are rewritten by the parser into calls to `toString` and `format`.
`toString` renders newtypes, `Char`s and integer-backed enums by name or glyph before falling back to the
VM's generic dump.

### No truthiness

Every condition must be a `Bool`: `if`, `while`, a match guard, the operands of `&&`, `||` and `!`. `&&` and
`||` lower to short-circuit jumps. The VM's truthiness opcodes are never emitted.

### Errors

`Option` and `Result` are ordinary enums from `std::core`.

- `?` is its own AST node with its own lowering: one type-id test and a field read. On failure the
  original value is returned unchanged.
- The error type must match the function's exactly; there is no automatic conversion. A `dyn Trait` error
  type is the one relaxation.
- `panic` aborts with a located fault. There is no in-language exception handling.

### Fork-join tasks

`std::task` runs functions in parallel: `spawn(f, x)` returns a `Task[R]`, and `t.join()` waits for it and
returns `Result[R, String]`. `Err` carries the message of the fault that ended the task. `spawn` is an
ordinary generic function in `std/task.skn`; the VM side is described under "Tasks and actors" in
[VirtualMachine.md](VirtualMachine.md). Because a task works on its own heap, its argument and result are
copied. The checker therefore adds these rules at every call of `spawn`:

- **`f` must name a top-level function** that is not generic and takes one parameter. A lambda, a closure
  or a function-typed variable is rejected: a closure is a heap object that cannot be copied, and a
  `fn(A) -> R` type does not say whether a value is one. A generic function is rejected because the types
  checked here must be the ones the task actually runs with.
- **The parameter type and the result type must be sendable.** Sendable means plain data: numbers, `Bool`,
  `String`, `Bytes`, and tuples, collections, structs and enums built only from sendable parts. It excludes
  function values, trait objects, type parameters without the bound `Sendable`, and handles — a socket
  (`TcpConn`, `TcpListener`, `NbConn`, `NbListener`), a `Task` or an actor's `Inbox`. A handle is a struct
  over an integer that is meaningful only in the heap that created it. The check walks recursive types,
  treating a type met again as sendable so far, and caches nothing. A disallowed component behind a mutual
  recursion is therefore still found.
- **`spawn` can only be called**, never used as a value, since a value would be called where none of this is
  checked.
- **A `Task` is built only by `spawn`.** A struct literal or record update of `Task` outside `std::task` is
  an error. Otherwise a `Task[String]` could be made from the id of a task that returns an `Int`.

Two additions let generic code work with tasks and actors:
- **`Sendable` is a sealed marker trait** in `std::core` that names exactly the sendable types. Like `Eq`, it
  has no impls: the compiler decides it from a type's parts, and a written `impl Sendable` is an error. A
  type parameter is sendable when it carries the bound, so `fn replyInbox[R: Sendable]() -> Inbox[R]`
  compiles and the checks happen where the helper is called, with the concrete type. A failed bound names the
  part that is not sendable. `dyn Sendable` does not exist, and `Sendable` cannot select a blanket impl,
  since erased newtypes are sendable and would dispatch as their underlying primitive.
- **`taskFn(f)` makes a checked function value**, a `TaskFn[A, R]`, under the rules of `spawn`; `t.spawn(x)`
  then starts a task. A generic helper cannot call `spawn` on a function it received as a parameter, since
  the parameter might hold a closure. It can take a `TaskFn` instead. A `TaskFn` literal outside `std::task`
  is an error, so its function is always a named one, and a `TaskFn` is itself sendable.

The four natives underneath are generic, and they take the `Task` itself, so a task's result type comes from
its handle. `rawTaskTake` and `rawTaskError` are one native under two types, and `rawJoin`'s `Bool` says which
one applies. The usual native convention, "a returned string means failure", cannot work here because a task
may return a string. The reference interpreter used for differential testing models a task sequentially: it
copies the argument at `spawn` and runs the function at `join`. That is observably the same, because output
appears at `join` in both.

### Actors

`std::actor` runs long-lived functions that talk by messages, after Erlang's model, with typed mailboxes and
one operating-system thread per actor. The runtime is the one tasks use, described under "Tasks and actors"
in [VirtualMachine.md](VirtualMachine.md).
- `spawnActor(f, init)` returns a `Pid[M]`.
- `send(pid, m)` copies a message into that actor's mailbox.
- The actor loops on `inbox.receive()`, which returns `Mail[M]`: a message, a report that an actor it
  started has faulted, or `Stop` when the program ends.

Like tasks, all of it is ordinary Skarn in `std/actor.skn` over generic natives. The actor function runs
behind a small trampoline, which builds the actor's `Inbox` from its own id and calls the function with its
start value.

A `Pid[M]` promises that the actor it names receives `M`. The checker keeps that promise where a `Pid` is
**made**, so `send` needs no check of its own:
- **`spawnActor(f, init)`:** `f` must name a non-generic top-level function
  `fn(Inbox[M], I) -> ()`, for the same reasons as a task's function. The message type `M` and the start
  value `I` must be sendable. Every message is copied, so a message may carry a `Pid` (it is plain data)
  but not an `Inbox`, the receiving end of one actor's mailbox.
- **`mainInbox()`** gives the main program an inbox. Its message type comes only from an annotation
  (`let inbox: Inbox[T] = mainInbox()`); it must be known at the call and sendable.
- **`newInbox()` / `newBoundedInbox(n)`** give an actor or the main program a further inbox, with the same
  rule as `mainInbox`: an annotated, sendable message type.
- **`spawnActorBounded(f, init, n)`** starts an actor whose inbox holds at most `n` messages, with the rules
  of `spawnActor`.
- **`ask(pid, make, timeoutMs) -> Result[R, AskError]`** makes a reply inbox, sends `make(replyAddress)`,
  waits for the reply and closes the inbox. Its reply type `R` is fixed by the `Pid[R]` the request carries
  (or by an annotation) and must be known at the call and sendable, because `R` is the message type of the
  inbox it makes.
- **`actorFn(f)`** makes an `ActorFn[M, I]` under the rules of `spawnActor`. `a.spawn(init)` and
  `a.spawnBounded(init, n)` then start actors from code that received the function as a value — a worker
  pool, or a supervisor that is sent a child's function and start value and starts it. An `ActorFn` is
  sendable, because only `actorFn` makes one.
- **Inside a generic function**, the message type of a new inbox and the reply type of `ask` may be a type
  parameter bounded `Sendable`.
- **`newSlot()` / `newBoundedSlot(n)`** get the same rule as the inbox makers: a `Slot[M]` is an address
  that outlives the actors started into it (`s.spawn(actorFn(f), init)`), so `M` must be known at the call
  and sendable. A `Slot` is plain data, so it can be sent to the supervisor that keeps the address alive.
- **These functions can only be called**, never used as values, so no call escapes the rules above.
- **`Pid`, `Inbox`, `Slot` and `ActorFn` literals are an error outside `std::actor`.**

A request that wants an answer does not need a selective receive: the answer goes to an inbox of its own,
with its own type. A bounded inbox provides back-pressure — `send` waits while it is full, `trySend` reports
`Full` instead — and exit reports and `Stop` always get through. None of this needs a checker rule beyond
the ones above; the waiting is the runtime's.

A connection cannot be a message: `TcpConn` stays unsendable, since its descriptor means something only in
the actor that opened it. `std::net` moves it in two steps instead. `c.handOff()` detaches the connection
and returns a `SocketHandOff`, a struct of plain data — a ticket and the bytes `recvLine` had already read
ahead — which can be sent like any message. The receiving actor calls `h.take()` once to get a `TcpConn`
back. A `SocketHandOff` literal or record update outside `std::net` is an error, so a program cannot forge a
ticket for someone else's connection. After the hand-off the sender's `TcpConn` still type-checks; it is a
rule of the runtime, not of the type system, that every operation on it now returns an `Err` saying the
socket was handed to another actor.

`std::supervisor` keeps actors running, written in Skarn over `std::actor`, with no rule and no native of
its own.
- **What it does:** `supervise(inbox, children, limit)` starts every child and starts again each one that
  crashes (`one_for_one`), and it returns at `Stop`. More than `maxRestarts` restarts within `withinMs`
  milliseconds, and it panics; its own starter is then told, so supervisors nest into trees.
- **Why a function, not an actor:** a crash is reported to the actor that started the child. The loop
  therefore runs inside the supervising actor, which builds its children there, as `child(actorFn(f),
  init)` values in a `Vec[dyn Supervised]`. The children may have different message types; a trait object
  cannot be sent, and the list never is.
- **A child at a fixed address:** `childIn(slot, actorFn(f), init)` puts the child in a `Slot`, so its
  address survives every restart and a message sent while it is being started again waits in the slot
  instead of being dropped. Past the limit the supervisor releases its children's addresses before it
  gives up, so none is left to swallow mail nobody will read.
- **Limits:**
  - Only a crash is reported, so only a crash restarts a child.
  - A child made with `child` has a new address after a restart; one made with `childIn` keeps the slot's.
  - A supervisor that gives up leaves its children running.

The reference interpreter used for differential testing does not model actors. A receive depends on which
actor ran first, and a sequential model would present one schedule as the answer. Actor programs are
therefore tested through the VM, with programs whose output is the same under every schedule.

## Code generation

### Register planning

Each function's frame is **measured, then emitted**. A measuring walk computes, for every construct, how many
local registers and temporaries it needs; the emitter then allocates within that plan. Register allocation
follows peak concurrent use: block and arm registers are reclaimed at the end of their scope. A frame may
use at most 63 registers.

Two rules keep measure and emit in agreement:

- **Every construct is taught to both walks.** If emit needs more than the measure planned, the allocator
  throws at the emit site. An undersized frame would otherwise overwrite a live register or hide a pointer
  from the collector.
- **Measurements are memoized and independent of the starting register.** This keeps compile time linear in
  nesting depth. A self-check rejects a construct whose measurement depends on where it starts.

Pattern bindings are always copied into a local of their own, never aliased to a temporary that the parent
pattern frees again.

### Representation

- **Structs, tuples and enum variants** are VM struct objects with a type id; `match` tests the id.
  - Tuples are generated `$TupleN` types.
  - `List` is a cons cell `$List`, and the empty list is `Nil`.
  - A nullary variant is a zero-field object.
- **Erased types** are distinct to the checker and bare immediates at run time. Their constructors, fields and
  patterns lower to nothing, and they cannot carry impls, because trait dispatch cannot tell them from their
  underlying type. They are:
  - transparent newtypes (`transparent struct UserId(Int)`);
  - integer-backed enums (`enum Color : Int`);
  - `Char`.
- **Constants** are inlined at each use. A constant array is built once when execution starts and read from a
  rooted pool; the checker keeps it read-only and non-escaping.
- **Functions:** a function without captures is a `Func` value; a lambda with captures is a closure object
  holding copies of its captured values.

### Calls

- A direct call becomes `CALL`; a call in tail position becomes `TCO_CALL`, so tail recursion runs in
  constant stack.
- A call through a function value becomes `CALL_INDIRECT` / `TCO_CALL_INDIRECT`.
- A trait method call on a statically known receiver becomes a direct call. On a generic or `dyn` receiver it
  becomes one fused `RESOLVE_CALL` / `RESOLVE_TCO_CALL` that looks up the implementation in the trait table.
  The code generator builds that table after emitting, when all type ids are known.
- Inherent methods are ordinary functions.
- The return value is fetched with `MOV_TAKE`, which clears the slot. This is part of the frame-size contract
  described in "Calling convention" in [VirtualMachine.md](VirtualMachine.md).

`for` dispatches on the static type of what it iterates:

- `List`, `Array`, `Vec`, `Bytes` and `Map` get direct loops; a map is walked in place without copying.
- Anything else is driven through `Iterator` / `IntoIterator`, whose combinators are library code.

### Optimizations

- **Tree-shaking.** Starting from the entry program, the compiler keeps only the functions, types, traits and
  constants that are reachable. When a trait is kept, all its impls are kept. A trait method reached only through
  method syntax is recorded separately, so it survives.
- **Peepholes:** fused compare-and-branch, `INCR`/`DECR` for `x += 1`, loop rotation, register coalescing, and
  no materialized value where a statement discards it.
- **Inlining** (on by default; `--no-inline`) expands small non-recursive direct calls. The measuring walk
  decides each site once and emit only looks the decision up. The measurement reserves room for either
  outcome, because declining an expansion can need more registers than expanding. If an expansion would
  overflow the register limit, the function is planned again without inlining.
- **Scalar replacement** (`--sroa`, off by default) stores a non-escaping struct binding as one register per
  field, so the object is never built.

## Verification

The dangerous failure is a wrong value with no error. It is guarded in layers, all in `static_compiler_tests`:

1. **Emitter self-check.** Register allocation throws when emit leaves the measured plan.
2. **Differential oracle.** `RefEval` is an independent tree-walking interpreter over the typed AST. The test
   harness runs a program both as bytecode and through the oracle and compares value, output and fault.
   - A construct the oracle does not model is a test failure, not a skip.
   - A **coverage assertion** requires every expression, statement and pattern kind, every operator, every
     built-in function and every deterministic native to have been exercised by an agreeing run.
   - Adding an AST kind breaks the build until the coverage tables are updated.
3. **Torture corpus.** Hand-written adversarial programs that combine features.
4. **Program generator.** `Generator` builds random well-typed programs, runs them differentially and shrinks
   any failure. It covers the scalar core, composites, patterns, method syntax, inherent impls, control flow,
   `?`, closures, lazy pipelines and long call chains. A green sweep only speaks for the forms the generator can
   produce, so every new feature should get a generator production.

Running them:

```bash
x64\Release\static_compiler_tests.exe
x64\Release\static_compiler_tests.exe 20000 8
x64\Release\static_compiler_tests.exe --repro 1234
x64\Release\static_compiler_tests.exe --gen 1234
```

- With no arguments, the suite runs every test, a 1000-seed sweep and the coverage assertion.
- A number runs a larger sweep, optionally split across processes.
- `--repro <seed>` reruns one generated program; `--gen <seed>` prints it without running it.
- Use Release builds for sweeps.

**The documentation is tested as well.** `tests/guide_claims/run_guide_claims.ps1` runs:

- about two hundred claim programs, each pinning one statement of the guides, including the diagnostics
  for common mistakes;
- every code block of both guides, checked against the output annotated in the text;
- every program under `examples/`, compared with its expected output.

`tests/check_doc_anchors.ps1` checks that section names cited from source comments still exist in this
document and [VirtualMachine.md](VirtualMachine.md).

## Adding a language feature

1. **Syntax:** lexer and parser; update `docs/skarn_grammar.ebnf`.
2. **Checker:** typing rules, patterns and exhaustiveness if they are involved, and a diagnostic for the likely
   mistake. Record in the AST any decision the code generator needs.
3. **Code generation:** the measuring walk and the emitter, together.
4. **Oracle:** teach `RefEval` the construct, and update the coverage tables.
5. **Generator:** add a production, so sweeps reach the new form.
6. **Tests:** checker tests, differential `check_same` tests, and torture programs for its combinations.
7. **Documentation:** the guide section, a guide claim for any rule stated in prose, and the syntax
   highlighters (`docs/skarn.tmLanguage.json`, `tools/vscode-skarn`, `tools/skarn.npp-udl.xml`).
8. **Gates:** Debug and Release builds, both test suites, the documentation gate, and the demos.
