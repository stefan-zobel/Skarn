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
- whether `Head::m(x)` names an inherent method or a trait method (`IdentExpr::inherent`).

Several real defects came from two parts of the compiler answering one question independently and
disagreeing.

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
- **Inherent impls** give a type its own methods and associated functions. A method call `recv.m()` resolves
  field first, then inherent method, then trait method. `Type::m(x)` and `Trait::m(x)` reach a method
  explicitly.
  - An inherent impl on a built-in type (`String`, `Vec`, …) is forbidden by the orphan rule, so functions
    over built-ins stay free functions.
  - Built-in functions and natives can only be called, never used as values.
- **`dyn Trait`** values are the plain values themselves: no box, no vtable. A method may appear in a `dyn`
  type only if it takes `self` and does not mention `Self` in another parameter.
- **Sealed marker traits.** `Eq` and `Hashable` are derived by the compiler and cannot be implemented by
  users; see "Language semantics" below.

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

The advisory tier warns about an unused binding, an ignored `Result` or `Option`, and a duplicate literal key
in a map literal. An ignored `Result` or `Option` at the end of a `-> ()` body is an error rather than a warning. `--strict` makes every warning fatal.

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
independently of order.

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
