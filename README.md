# The Skarn Language Guide

Skarn is a sound, statically typed language in the ML/Rust tradition — checked ahead of time and *erased* at
compile time (generics compile to one shared body, not a copy per type) — that happens to target a compact
bytecode VM (the vMachine interpreter). Its type system borrows from the functional world, but its **core is
imperative**: statements, mutable bindings, loops, and in-place updates are ordinary Skarn, not an escape
hatch ([§22](#22-programming-styles-imperative-functional-streaming) writes the same program three ways).
This guide is a complete, example-driven tour of the language for working programmers. It assumes you are
comfortable with a mainstream statically typed language and have seen a few functional-programming ideas
(closures, pattern matching, immutable-by-default values), but it does **not** assume you know any particular
functional language — every construct is explained on its own terms.

Every code block in this guide is a real program (or fragment) that compiles and runs. Where it helps, the
expected output is shown in a trailing comment (`// => ...`).

## Why Skarn?

Skarn is a statically-typed language with a **Rust-flavored surface** — enums with exhaustive `match`,
`Option`/`Result` with `?`, traits with bounds, immutability by default, everything an expression — but it runs
on a garbage-collected bytecode VM, so it **drops the parts of Rust that make Rust hard**.

> **In a hurry?** [Skarn in 30 minutes](SkarnIn30Minutes.md) is the working core — bindings, functions,
> `struct`, `enum`/`match`, `Option`/`Result`/`?`, the everyday collections, text handling, iterator pipelines,
> `use`, and file/argument I/O — enough to finish a real program. Come back here for traits, generics, `dyn`,
> and the full standard library.

The one-sentence pitch: *the safety and expressiveness of Rust's type system, without the borrow checker.*

Concretely:

- **No borrow checker, no lifetimes, no ownership/move semantics.** A GC manages memory, so you never write
  `&`, `&mut`, or `'a`, never fight the borrow checker, and never track who "owns" a value — you just pass
  values around.
- **No `null`.** Absence is `Option[T]`; the null-dereference bug class is gone by construction.
- **No exceptions.** Recoverable failure is a `Result[T, E]` value threaded with `?`; unrecoverable failure is
  `panic`, which aborts. No invisible control flow.
- **No classes or inheritance.** Data is plain `struct`s and `enum`s; behavior is `trait`s. Composition over
  hierarchy.
- **Sound, then erased.** The type checker is the single guarantee. At run time, generic *type parameters*
  carry no witness — no dictionaries, no per-type specialization — and there is no reflection, no boxing, and
  no vtables. Trait dispatch instead reads the coarse type tag every value already carries (the one the GC and
  the value representation need anyway), so it too costs nothing extra. See [§16](#16-traits)
  for how "erased" and "dispatch on the runtime type" fit together without contradiction.

### Compared to the languages it borrows from

| | Shares with Skarn | What Skarn does differently |
|---|---|---|
| **Rust** | enums + `match`, `Option`/`Result` + `?`, traits + bounds, immutability, no null | **GC instead of a borrow checker** — no lifetimes/ownership/`&mut`; generics are *erased* (one body), not monomorphized |
| **Gleam / OCaml / Elm (the ML family)** | sound static typing, sum types + exhaustive `match`, *erased* generics (one body), immutability by default | Rust-style traits + bounds + `dyn`; a C-family curly-brace surface with method syntax; inference for locals only (signatures are annotated); targets a compact bytecode VM |
| **Kotlin** | GC on a bytecode VM, *erased* generics, sealed types + exhaustive `when`, expression-oriented (`if`/`when`) | `Option` instead of nullable types (`T?`); `Result` + `?` instead of exceptions; no classes or subclassing (structs + traits only) |
| **Swift** | enums with associated values + exhaustive `switch`, `Optional` as a real sum type (≈ `Option`), protocols + constraints (≈ traits + bounds), value-type structs | runs on a GC bytecode VM (Swift is native + ARC); errors as `Result`/`?` values rather than `throws`; no classes or subclassing |
| **Go** | GC, an application / CLI focus, errors as values (not exceptions) | real sum types + generics-with-traits + exhaustive `match` + `?` — deliberately *not* Go's minimal surface / `if err != nil` |

### Deliberate scope (what Skarn is *not*)

Skarn keeps its type system **first-order and simple on purpose**: generics are rank-1 with trait bounds — no
higher-kinded types, no lifetimes, no higher-rank polymorphism. Strings are **byte strings** by default (code
points are an opt-in layer). It targets a compact interpreter, not native code, so it is an
**application/scripting** language, not a systems language: you trade Rust's zero-overhead-at-any-cost for a GC
that removes Rust's single hardest concept. Where Rust asks "who owns this, and for how long?", Skarn's answer
is "the GC does — write the obvious code."

Skarn is also **single-threaded**, by design for v1: there are no threads, no `async`, and no channels; the
garbage collector is stop-the-world; and `std::net` is blocking (one connection at a time). Concurrency is a
deliberate non-goal here — when you need parallelism, shell out to OS processes with `std::process`.

Let's be honest about the rest, though: what Skarn removes is Rust's **difficulty** (ownership, lifetimes, the borrow
checker), not Rust's **feature count**. The surface is Rust-family-sized — traits, generics, `dyn`, exhaustive
pattern matching, lazy iterators — so this is not a tiny language you learn in an afternoon. What it *is* is a
**layered** one. The core you need to be productive — `let`/`fn`/`struct`/`enum`/`match`/`Option`/`Result` and
the everyday collections — is small and quick to hold in your head (that is exactly
[Skarn in 30 minutes](SkarnIn30Minutes.md)). The heavier machinery — blanket and parametric `impl`s, `dyn`,
the erasure types, the full call-form and module surface — is mostly there for **reading** the standard library
and growing into, not for everyday **writing**. You can go a long way on the core and reach for the rest only
when a problem asks for it.

## How to read this guide

- Sections build on each other, but each is self-contained enough to skim.
- Code is shown in normal, multi-line style. In real files you may write several small statements on one line
  separated by spaces; the compiler mostly treats newlines and blank lines as formatting rather than required
  separators. Two precise rules govern where a newline *does* matter:
  1. **Continuation (look at the next token).** A newline ends the current statement — *unless* the next line
     begins with a **binary operator** or **`.`**, in which case the expression continues onto it. This is
     exactly what lets you break long expressions and **method chains** across lines: both
     `let total = a + b`⏎`    + c` and `sb.append("a")`⏎`  .append("b")`⏎`  .build()` work, because `+` and `.`
     continue the line. (A leading `(` or `[` does **not** continue, so `f`⏎`(x)` is two statements, not a call.)
  2. **Value cutoff (look at the previous token).** After `return` or `break`, a newline *always* ends the
     statement, so the value is never taken from the following line — `return`⏎`  x` is a bare `return` then a
     separate statement `x`, not `return x`.
  The one thing to watch falls out of rule 1: a statement you meant to open with a unary `-` continues the line
  above as a subtraction instead. `-` is the *only* token this happens to — see the note in [§5](#5-operators).
- Run any example by saving it to a `.skn` file and passing it to the runner:

```
static_vmrun.exe myprogram.skn
```

### Table of contents

1. [What Skarn is](#1-what-skarn-is)
2. [Comments](#2-comments)
3. [Primitive values and types](#3-primitive-values-and-types)
4. [Bindings: `let` and `let mut`](#4-bindings-let-and-let-mut)
5. [Operators](#5-operators)
6. [The numeric model](#6-the-numeric-model)
7. [Strings](#7-strings)
8. [Control flow is expressions](#8-control-flow-is-expressions)
9. [Functions](#9-functions)
10. [Closures and function values](#10-closures-and-function-values)
11. [Structs and tuples](#11-structs-and-tuples)
12. [Enums (sum types)](#12-enums-sum-types)
13. [Pattern matching](#13-pattern-matching)
14. [Collections](#14-collections)
15. [Generics](#15-generics)
16. [Traits](#16-traits)
17. [Trait objects: `dyn Trait`](#17-trait-objects-dyn-trait)
18. [Option, Result, and the `?` operator](#18-option-result-and-the--operator)
19. [Iterators](#19-iterators)
20. [Modules](#20-modules)
21. [Input, output, and the standard natives](#21-input-output-and-the-standard-natives)
22. [Programming styles: imperative, functional, streaming](#22-programming-styles-imperative-functional-streaming)
23. [The type system in one page](#23-the-type-system-in-one-page)
24. [A complete little program](#24-a-complete-little-program)
25. [The memory & cost model](#25-the-memory--cost-model)
26. [Quick reference: the standard library](#26-quick-reference-the-standard-library)

---

## 1. What Skarn is

Skarn is **statically typed** and **type-checked before it runs**. If a program passes the type checker, whole
classes of bugs cannot occur: there are no null-pointer errors (there is no `null`), no "undefined is not a
function", and no silent type coercions that produce a wrong answer.

Three properties are worth stating up front, because they shape everything else:

- **The types are checked and then erased.** Types exist only at compile time; generics carry no witness and
  cost nothing at run time. Every value does still carry a coarse runtime tag (is-it-an-Int, which struct,
  which enum variant) — that is what `match` and trait dispatch read — but *nothing the type system needed*
  survives compilation. (See [§16](#16-traits) for how dispatch uses that tag.)
- **There is no escape hatch.** There is no `any` type and no `unsafe`. The type checker is the only thing
  standing between you and a bad program, so it is strict: it never lets an ill-typed program through.
- **Values are immutable by default.** A binding does not change unless you explicitly ask for it with `mut`.

A first program:

```rust
println("Hello, world")
// => Hello, world
```

Top-level statements run in order, top to bottom. There is no mandatory `main` function; the file *is* the
program.

---

## 2. Comments

Two comment forms, exactly as you would expect:

```rust
// a line comment runs to the end of the line

/*
   a block comment
   can span multiple lines
*/

let x = 1 /* comments can also appear inline */ + 2
```

---

## 3. Primitive values and types

Skarn has a small set of primitive types. You will usually let the compiler infer them, but you can always
write them out.

| Type     | Meaning                                            | Example literal |
|----------|----------------------------------------------------|-----------------|
| `Int`    | 48-bit signed integer, wraps on overflow           | `42`            |
| `Double` | 64-bit floating point                              | `3.5` `6e-3`    |
| `Bool`   | boolean                                            | `true` `false`  |
| `String` | text (a sequence of bytes)                         | `"hello"`       |
| `()`     | the "unit" value — the single value of no interest | `()`            |

There is one more literal form worth knowing: a **character literal** written with single quotes, such as
`'A'`. A *bare* character literal is simply an `Int` holding the byte value of that one character (`'A'` is
`65`) — which pairs naturally with the fact that strings are byte sequences. Skarn *does* also have an opt-in
`Char` type for Unicode **code points** (a zero-cost wrapper over an `Int`), but a character literal becomes a
`Char` only in a `Char`-typed context; a bare `'A'` stays an `Int`. See
[Char and UTF-8 code points](#char-and-utf-8-code-points).

```rust
let anInt: Int = 42
let aDouble: Double = 3.5
let aBool: Bool = true
let aString: String = "hello"
let aChar: Int = 'A'            // a character literal is just an Int (its byte value)
let unit: () = ()

println("int="  + anInt)        // => int=42
println("dbl="  + aDouble)      // => dbl=3.5
println("bool=" + aBool)        // => bool=true
println("str="  + aString)      // => str=hello
println("char=" + aChar)        // => char=65
```

A `Double` literal may also be written in **scientific notation** with an `e`/`E` exponent — `6e-3`, `1.5e10`,
`2.5E+8`. An exponent makes the literal a `Double` even without a decimal point (`1e6` is `1000000.0`).

An `Int` literal may be written in **hexadecimal** (`0x` / `0X`), **octal** (`0o` / `0O`), or **binary**
(`0b` / `0B`): `0xFF` is 255, `0o17` is 15, `0b1010` is 10. Unlike a decimal literal (a signed magnitude that
must be below 2⁴⁷), a radix literal is a **bit pattern** of up to the full 48-bit width, sign-extended from
bit 47 — so `0x800000000000` is the smallest `Int` and **`0xFFFFFFFFFFFF` (all 48 bits set) is `-1`**, a
convenient full-width mask. A value beyond 48 bits (e.g. `0x1000000000000`) is a compile error.

```rust
let flags   = 0xFF           // 255
let allOnes = 0xFFFFFFFFFFFF // -1  (every bit set, in 48-bit two's complement)
let masked  = allOnes & 0x0F // 15
let perms   = 0o755          // 493
let bits    = 0b1011         // 11
```

Note that a `Double` always prints with a decimal point (`3.5`, and `4.0` rather than `4`), so you can always
tell the two numeric types apart in output.

---

## 4. Bindings: `let` and `let mut`

A `let` binding introduces a name. By default the name is **immutable** — you cannot assign to it again.

```rust
let x = 10          // type Int, inferred
let y: Int = 20     // type written explicitly
println("sum=" + (x + y))   // => sum=30
```

To allow reassignment, add `mut`:

```rust
let mut counter = 0
counter = counter + 5
counter = counter + 5
println("counter=" + counter)   // => counter=10
```

`mut` governs **all** mutation through a binding, under one rule (Rust's): the *root* binding must be `mut` to
reassign it (`x = ..`), to assign through a place path (`p.x = ..`, `a[i] = ..`), **or** to pass it to a
function or method that mutates it in place (`push(v, x)`, `sort(v)`, `sb.append(s)` — anything whose
parameter or `mut self` receiver is `mut`). A plain `let v = vec()` followed by `push(v, 1)` is therefore a compile error — write `let mut v`. (A
fresh temporary you never bind, like `push(vec(), 1)`, needs no `mut`.)

One distinction to keep straight: a **`let mut` binding can be reassigned whatever its type** — `let mut n = 0
n = n + 1` is fine for a scalar, an `enum`, or a `List`, because reassignment just rebinds the name. The
"aggregate only" restriction is about **`mut` *parameters*** (and `mut self`): declaring a *parameter* `mut`
means the callee mutates it **in place** so the *caller* sees the change, and only the mutable aggregates —
`struct` / tuple / `Array` / `Vec` / `Bytes` / `Map` and trait objects — have an in-place mutation surface. So
a scalar / `enum` / `List` *parameter* cannot be declared `mut` (there is nothing to propagate back), even
though a `let mut` local of the same type reassigns freely.

**Type inference is local.** Inside a function body the compiler infers the types of your `let` bindings from
their initializers, so you rarely annotate them. But function parameters and return types must always be
written out (see [§9](#9-functions)) — this keeps type inference simple and predictable, and makes every
function signature self-documenting.

**Shadowing** is allowed: a later `let` with the same name introduces a new binding that hides the old one.
This is handy for transforming a value step by step without inventing new names.

```rust
let value = "  42  "
let value = slice(value, 2, 4)   // a new binding; the old one is shadowed
println("value=" + value)        // => value=42
```

Bindings are **block scoped**: a name introduced inside a `{ ... }` block is not visible outside it.

### Module constants — `const`

For a value that never changes and lives at the top level of a module, use a `const`. Unlike a top-level
`let` (which is private module-init state), a `const` is a real, exportable declaration — mark it `pub` and
other modules can `use` it. Its value must be a single literal, and the type annotation is required:

```rust
const MAX_RETRIES: Int = 5
const GREETING: String = "hello"

fn attempts() -> Int { MAX_RETRIES * 2 }   // => 10
```

A `const` is **inlined** at each use site (it has no runtime storage), so referencing one is exactly as cheap
as writing the literal. `std::math`'s `PI` and `E` are consts of this kind.

#### Constant lookup tables — `const NAME: Array[T] = [...]`

A `const` whose type is `Array[T]` is a compile-time **lookup table**, built once at startup and shared — the
idiomatic way to write table-driven code (CRC/hash tables, S-boxes, char-class maps). Elements are scalar
literals (`Int`, `Double`, or `Bool`):

```rust
const SQUARES: Array[Int] = [0, 1, 4, 9, 16, 25]

fn square(i: Int) -> Int { SQUARES[i] }   // one array read, no per-call rebuild
```

Because the table is a single shared instance, it is deliberately **read-only and non-escaping**: you may only
read it directly — index it (`SQUARES[i]`), iterate it (`for x in SQUARES`), or take its `len(SQUARES)`. It
cannot be bound to a variable, passed to a function, returned, stored in a field, or mutated (`SQUARES[i] = v`
is an error). A constant table is also **module-private** — `pub` is not allowed on it; to share one, expose a
`pub fn` accessor that reads it. (The `std::hash` `crc32` is implemented exactly this way, over a private
256-entry table.)

---

## 5. Operators

### Arithmetic

`+ - * / %` work on numbers. If both operands are `Int` the result is `Int`; if either is `Double`, both are
treated as `Double` and the result is `Double`.

```rust
let a = 3 + 4 * 2       // => 11 (Int)
let b = 10.0 / 4.0      // => 2.5 (Double)
let c = 3 + 0.5         // => 3.5 (Int promoted to Double)
let d = -5              // unary minus: a negative literal
let e = -a              // and prefix negation of any expression   // => -11
```

A prefix `-` (and `!` for `Bool`, `~` for bitwise-not) works anywhere an expression is expected. The one
caveat is at the *start of a statement*, and it is just an instance of the newline **continuation** rule
(rule 1 under [How to read this guide](#how-to-read-this-guide)): because a line that begins with a binary
operator continues the previous one, `x`⏎`-y` parses as `x - y`. `-` is the *only* token this can bite — it is
the sole operator that is **both** a line-continuation **and** a valid expression opener (`!` and `~` open an
expression but are not binary operators; `*`/`&` are not prefix operators), so naming it specifically is exact,
not a special case. Inside expressions, arguments, and `let` initializers — where negation almost always
appears — there is nothing to watch for.

### Compound assignment

`+= -= *= /= %=` update a place in one step. `a op= b` means exactly `a = a op b`, so the type rules and the
traps are the same ones the plain operator has:

```rust
let mut n = 10
n += 5                  // => 15
n *= 2                  // => 30
n %= 7                  // => 2

struct Counter { hits: Int }
let mut c = Counter { hits: 0 }
c.hits += 1             // a field
let mut v = array(3, 0)
v[1] += 10              // an element
println("${n} ${c.hits} ${v[1]}")   // => 2 1 10
```

Three things are worth knowing, because they are choices rather than consequences:

- **The place is evaluated exactly once.** In `v[nextIndex()] += 1`, `nextIndex()` is called one time, not
  two — the compiler reads, computes, and writes back through a single evaluation of the index.
- **It is numeric only** (`Int` and `Double`). `s += "text"` is rejected on purpose: repeated string
  concatenation is quadratic, and Skarn has [`StringBuilder`](#building-strings-efficiently--stringbuilder)
  for building text. Plain `s = s + t` still works when you really do want the copy.
- **`m[k] += 1` needs `k` to already be there.** The read half is the ordinary `m[k]`, which aborts on a
  missing key ([§14](#14-collections)), so this is not the way to build up a counter map — use `getOrPut` or
  `upsert` for that. On an `Array`/`Vec` element there is no such caveat, since the slot always exists.

The same rules apply to `mut`: the binding you assign to (or reach through) must be `mut`, exactly as for `=`.

### Comparison and equality

`== !=` are **structural (deep value) equality**: two values are equal when they have the *same shape and
equal components*, all the way down — never by object identity. A `struct` equals another with equal fields,
a tuple equals a tuple with equal elements, an `enum` value equals one of the same variant with equal
payloads, and a `Vec` / `Array` / `Bytes` / `Map` equals another with equal elements (a `Map` compares
**regardless of insertion order**). `Int` / `Double` / `Bool` / `Char` / `transparent struct` newtypes /
integer-backed `enum`s compare by value; `String` by content. This agrees with **pattern matching**, which
is also structural — so `p == Point { x: 1, y: 2 }` is `true` exactly when `match p { Point { x: 1, y: 2 } => … }`
matches.

```rust
struct Point { x: Int, y: Int }
let e5 = (Point { x: 1, y: 2 } == Point { x: 1, y: 2 })  // => true
let e6 = (Some(1) == Some(1))                            // => true
let na: Option[Int] = None
let nb: Option[Int] = None
let e7 = (na == nb)                                      // => true  (two distinct values, so this is real
                                                         //           structural equality, not the self short-circuit;
                                                         //           a bare `None == None` has no element type, so annotate)
let e8 = (Some(1) == None)                               // => false (None's element type comes from Some(1))
let e9 = (toVec([1, 2, 3]) == toVec([1, 2, 3]))          // => true
```

A type supports `==` only when **all of its components do**. The things that have no structural equality are a
**function / closure** value, a **trait object** (`dyn T` — its concrete type is hidden), and an **unbounded
type parameter**; a type that stores one of these is not comparable, and `==` on it is a **compile error** (not
a silent wrong answer) — compare the other fields instead. In a generic function, compare a type parameter with
`==` only under a **`T: Eq`** bound, which requests exactly this: `fn allEq[T: Eq](xs: Vec[T], v: T) -> Bool
{ … a == v … }`. `Eq` is a built-in, automatically-derived marker — you never `impl` it and there is no `dyn Eq`.

Each of these is rejected at compile time (never a silent wrong answer):

```rust
struct Box { f: fn(Int) -> Int }
let a = Box { f: fn(x: Int) -> Int { x } }
a == a                            // error: type Box does not support '==' (it contains a function value)

fn same2[T](a: T, b: T) -> Bool { a == b }   // error: T does not support '==' — add a 'T: Eq' bound

struct P { x: Int }
impl Eq for P {}                  // error: cannot implement the built-in 'Eq' marker (it is derived)

fn f(x: dyn Eq) -> Int { 0 }      // error: 'Eq' is a compile-time marker, not usable as 'dyn Eq'
```

> **`NaN` and equality.** `Double` compares by IEEE rules, so **`NaN` is never equal to `NaN`** — and a
> composite that *contains* a `NaN` inherits that: `P { x: 0.0/0.0 } == P { x: 0.0/0.0 }` is `false` (two
> distinct values), exactly as `0.0/0.0 == 0.0/0.0` is `false`. A value is still equal to *itself*
> (`p == p` is `true` — same object), but not to a structurally-equal copy once a `NaN` is involved. Note this
> is a *different* rule from **map keys**, which canonicalize `NaN` (so a `NaN` key is findable); `==` stays
> IEEE, key-equality does not — a deliberate split. One consequence of the self-equality short-circuit: for a
> `NaN`-containing value, `contains(v, x)` answers differently depending on whether `v` holds `x` *itself* or a
> structural copy of it — the only case where that distinction is observable.

`< <= > >=` compare **two numbers, two strings, or two `Char`s** (there is no ordering on composites). The
`Ord` *trait* — what `sort` / `sorted` / `minOf` / `maxOf` require — is a separate thing from the operators: it
has **built-in** impls for `Int` / `Double` / `String` (see [§19](#19-iterators)), and your own `struct` / `enum`
can add it by writing `impl Ord` (only the erasure types, like `Char`, cannot). So a `Char` can be compared with
`<` but a `Vec[Char]` cannot be `sort`ed — use the comparator form, which needs no `Ord`:
`sortBy(chars, fn(a: Char, b: Char) -> Bool { a < b })`.

```rust
let e1 = (2 + 2 == 4)         // => true
let e2 = ("abc" == "abc")     // => true (String compares its text)
let e3 = (5 > 3 && 2 <= 2)    // => true
let e4 = ("apple" < "banana") // => true (lexicographic, byte order)
```

### Boolean logic

`&& || !` operate on `Bool` and short-circuit: the right side of `&&`/`||` is only evaluated if needed. The
result is always a `Bool`. Conditions (in `if`, `while`, and so on) must be `Bool` — there is no "truthiness"
that turns numbers or strings into booleans.

```rust
fn expensive() -> Bool {
    println("  (evaluated)")
    true
}

let short = false && expensive()   // expensive() is never called
println("short=" + short)          // => short=false
```

### Bitwise and shift

`& | ^ ~ << >> >>>` operate on `Int` only (`>>` is an arithmetic/sign-preserving shift, `>>>` is logical).
`~x` is bitwise-not — it flips every bit, equivalent to `x ^ -1`.

```rust
let bits = (6 & 3) | (1 << 4)   // (2) | (16) => 18
println("bits=" + bits)         // => bits=18

println("not="  + (~5))             // => not=-6   (~x flips every bit)
println("shr="  + ((-8) >> 1))      // => shr=-4   (arithmetic: sign preserved)
println("ushr=" + (8 >>> 1))        // => ushr=4   (logical: zero-filled)
```

### String `+`

`+` also concatenates strings. If one side is a `String` and the other is a number or `Bool`, the non-string
operand is converted to text automatically. (For anything else — a struct, a list, and so on — call
`toString(x)` explicitly; see [§7](#7-strings).)

```rust
let label = "x" + 1 + "y" + true    // => "x1ytrue"
println(label)
```

### The pipe operator `|>`

`x |> f` is just another way to write `f(x)`, and `x |> f(a, b)` means `f(x, a, b)`. It reads left to right,
which is convenient for chains of transformations.

The pipe is **syntactic**: `|>` is rewritten to an ordinary call at compile time, so its right-hand side must
be a *call target* — a function name or a function call with its remaining arguments — not an arbitrary
expression that happens to produce a function. `x |> f` and `x |> g(a)` are fine; `x |> (pickFn())` or
`x |> table[0]` are not. It also binds more loosely than the other operators, so `a + b |> f` means
`f(a + b)`; parenthesize if you mean something else.

```rust
fn twice(n: Int) -> Int { n * 2 }
fn plus(n: Int, k: Int) -> Int { n + k }

let piped = 5 |> twice |> plus(1)   // plus(twice(5), 1) => 11
println("piped=" + piped)
```

The pipe is one of a small handful of call forms — there is also method syntax `x.method()` for a type's own
operations. All the forms are laid out together in *Calling things — the forms at a glance* (§16); the short
version is that `|>` threads a value into any free function, while `.` reaches a type's methods.

---

## 6. The numeric model

Skarn has two number types with deliberately simple, predictable rules.

- **`Int` is 48-bit and wraps on overflow** (like two's-complement integer arithmetic). It does not throw on
  overflow.
- **Mixing `Int` and `Double` promotes the `Int` to `Double`.** An all-`Int` expression stays `Int`.
- **Integer `/` truncates toward zero, and `%` takes the sign of the dividend.** Dividing an integer by zero
  (with `/` or `%`) aborts the program with a located error. A `Double` division by `0.0` follows IEEE rules
  (it yields infinity or NaN and does not abort).

```rust
let idiv = 7 / 2      // => 3
let imod = 7 % 3      // => 1
let promoted = 3 + 0.5  // => 3.5
```

The promotion above happens **only where the two types meet**. Two `Int`s divided by each other stay integer
division, even when the result is assigned to a `Double` — the division has already happened by then. When you
want the other arithmetic, widen the operands first with **`toDouble`**:

```rust
let w = 470
let h = 264
println(w / h)                        // => 1                    (integer division)
println(toDouble(w) / toDouble(h))    // => 1.7803030303030303   (what you meant)
```

`toDouble` takes an `Int` and is exact — every 48-bit `Int` is representable as a `Double`. Passing it a value
that is already a `Double` is an error rather than a silent no-op, since it can only be a mistake.

To convert a `Double` to an `Int`, use `toInt`. It is **saturating and total**: it never aborts. A fractional
value truncates toward zero; a value too large becomes the maximum `Int`; too small becomes the minimum; and a
NaN becomes `0`.

```rust
println("toInt(3.9)=" + toInt(3.9))    // => toInt(3.9)=3
println("toInt(-2.7)=" + toInt(-2.7))  // => toInt(-2.7)=-2
```

To choose *which* integer, round first — `floor`, `ceil`, `trunc` (toward zero), `round` (ties **away** from
zero), and `roundHalfToEven` (ties **to even** — banker's rounding). These all take a `Double` and return a
`Double` (so they compose with further float math); narrow to `Int` with `toInt` when you want one.

```rust
println(floor(2.7))            // => 2.0
println(ceil(2.3))             // => 3.0
println(round(2.5))            // => 3.0   (ties away from zero)
println(roundHalfToEven(2.5))  // => 2.0   (ties to even)
println(roundHalfToEven(3.5))  // => 4.0
println(toInt(round(2.5)))     // => 3     (round, then narrow)
```

When you need the narrowing itself to be *checked* rather than saturating, `std::math` offers
`toIntChecked(x) -> Result[Int, String]`: it truncates toward zero like `toInt`, but returns `Err` when `x` is
NaN, infinite, or out of `Int` range (import it with `use std::math::*`).

```rust
use std::math::*
println(toIntChecked(3.9))                  // => Ok(3)
println(toIntChecked(1.0 / 0.0))            // => Err("toIntChecked: value out of Int range")
```

`std::math` also provides the usual numeric functions (all opt-in with `use std::math::*`). The `Double`
functions follow IEEE semantics — a domain error yields `NaN` or `±∞` rather than trapping, and `isNaN` /
`isInfinite` let you test for those:

- **Powers / roots:** `sqrt`, `cbrt`, `pow(x, y)`, `hypot(x, y)`
- **Exp / log:** `exp`, `ln` (natural log), `log2`, `log10`
- **Trig:** `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2(y, x)`
- **Sign / magnitude:** `abs` (`Double`), `absInt` (`Int`), `sign` (`Double`), `signInt` (`Int`)
- **Integer:** `gcd(a, b)`, `lcm(a, b)`
- **Scalar min/max/clamp** — generic over any `Ord` type (the built-in `Int` / `Double` / `String`, or your own
  via `impl Ord`), so they preserve the
  type: `minOf(a, b)`, `maxOf(a, b)`, `clamp(x, lo, hi)`. (The bare `min`/`max` are the *iterator* terminals of
  [§19](#19-iterators); scalar versions get the `-Of` suffix because Skarn has no overloading.)
- **Constants:** `PI` and `E` (see the `const` note in [§4](#4-bindings-let-and-let-mut)).

```rust
use std::math::*
println(sqrt(2.0))                           // => 1.4142135623730951
println(gcd(12, 18))                         // => 6
println(minOf(3, 7))                         // => 3   (Int in, Int out)
println(clamp(15, 0, 10))                    // => 10
println(PI)                                  // => 3.141592653589793
```

To parse text into a number, use `parseInt` or `parseDouble`. These can fail, so they return a `Result`
(covered in [§18](#18-option-result-and-the--operator)); here we handle it with a `match`:

```rust
fn parseOr(s: String, fallback: Int) -> Int {
    match parseInt(s) {
        Ok(n)  => n,
        Err(_) => fallback
    }
}

println("parse=" + parseOr("123", -1))   // => parse=123
println("bad="   + parseOr("xyz", -1))   // => bad=-1
```

---

## 7. Strings

A `String` is a sequence of bytes. If your source file is UTF-8, string literals keep their bytes exactly, so
text round-trips faithfully — but *by default* the language treats a string as bytes, not as Unicode code
points. In particular, `len` returns the number of **bytes**. When you do need code points, there is an opt-in
layer (`chars`/`charCount`/`nthChar` and the `Char` type) — see
[Char and UTF-8 code points](#char-and-utf-8-code-points).

String literals support the escapes `\n \t \r \\ \" \' \0` and `\$` for escaping a literal `${` (the string
interpolation symbol).

```rust
let greeting = "Hello" + ", " + "world"
println("greeting=" + greeting)          // => greeting=Hello, world
println("len="      + len(greeting))     // => len=12

let sub = slice(greeting, 0, 5)          // a byte-range substring [start, end)
println("sub=" + sub)                    // => sub=Hello

let oneChar = charStr(66)                // build a one-byte String from a byte value
println("oneChar=" + oneChar)            // => oneChar=B

println("less=" + ("apple" < "banana"))  // => less=true
```

Because a string is a sequence of bytes, `for c in s` iterates its **bytes** — each `c` is an `Int` in the
range `0..255`, exactly like a bare char literal (`'A'` is the byte `65`). This is the natural fit for ASCII
scanning. Byte iteration does **not** decode UTF-8 into code points — when you want **code points**, use the
opt-in `chars(s)` iterator (it yields `Char`s), covered in
[Char and UTF-8 code points](#char-and-utf-8-code-points).

```rust
let mut sum = 0
for c in "abc" {                         // c: Int -- the byte value of each character
    sum = sum + c
}
println("sum=" + sum)                    // => sum=294   (97 + 98 + 99)

let mut vowels = 0
for c in "banana" {
    if c == 'a' { vowels = vowels + 1 }  // compare the byte to a char literal
}
println("a-count=" + vowels)             // => a-count=3
```

Any value can be turned into text with `toString`. Primitives render as you would expect, and compound values
(structs, enums, tuples, and collections) render as a readable dump — this is handy for quick debugging output:

```rust
struct Pt { x: Int, y: Int }
enum Col { Red, Rgb(Int, Int, Int) }

println(toString(42))                    // => 42
println(toString(true))                  // => true
println(toString(3.5))                   // => 3.5
println(toString(Pt { x: 1, y: 2 }))     // => Pt { x: 1, y: 2 }
println(toString(Rgb(255, 0, 0)))        // => Rgb(255, 0, 0)

let v: Vec[Int] = vec()
push(v, 1)
push(v, 2)
println(toString(v))                     // => [1, 2]

let mut m: Map[String, Int] = #{}
m["k"] = 9
println(toString(m))                     // => #{"k" => 9}
```

Recall that when you concatenate with `+`, a *scalar* (number or `Bool`) is stringified automatically, but a
compound value is not — so `"pt=" + toString(p)` works while `"pt=" + p` does not.

### String interpolation

Any string literal may embed expressions with `${ … }`. Each hole is stringified (exactly as `toString`) and
spliced into the surrounding text:

```rust
let name = "Ada"
let n    = 42
println("hello ${name}, n=${n}")         // => hello Ada, n=42
println("sum = ${n + n}")                // => sum = 84
```

A hole may hold **any** type — including compound values, which is where interpolation is more convenient than
`+` (recall `"pt=" + p` is a type error, but the hole below is fine):

```rust
struct Pt { x: Int, y: Int }
let p = Pt { x: 1, y: 2 }
println("p = ${p}")                      // => p = Pt { x: 1, y: 2 }
println("some = ${Some(7)}")             // => some = Some(7)
```

Interpolations **nest** — a hole can contain another interpolated string:

```rust
let x = 5
println("outer ${ "inner ${x}" } done")  // => outer inner 5 done
```

Only the exact sequence `${` starts a hole. A lone `$` is a literal dollar sign (no escaping needed); to write
a literal `${`, escape the dollar with `\$`:

```rust
println("price: $5")                     // => price: $5
println("literal \${x}")                 // => literal ${x}
```

Interpolation is pure syntactic sugar: `"a=${x}, b=${y}"` is exactly `"a=" + toString(x) + ", b=" +
toString(y)`, so it behaves identically to writing that chain by hand.

#### Format specifiers

A hole may carry a **format specifier** after a colon — `${value:spec}` — to control width, alignment,
precision, and numeric base. The specifier grammar is a subset of Rust's:

```
[[fill]align] ['+'] ['#'] ['0'] [width] ['.'precision] [type]
```

- **align** — `<` left, `>` right, `^` center; a character *before* the align is the fill (default space).
- **`+`** — sign flag: force a leading `+` on a non-negative number.
- **`#`** — alternate form: add a base prefix (`0x`/`0X`/`0b`/`0o`) for the `x`/`X`/`b`/`o` types.
- **`0`** — zero-pad flag (for a number the zeros go after the sign and any base prefix).
- **width** — the minimum field width.
- **`.`precision** — the number of decimals (`f`) or mantissa digits (`e`/`E`) for a `Double`.
- **type** — `x`/`X` hex, `b` binary, `o` octal, `d` decimal, `f` fixed-point `Double`, `g` shortest
  round-trip `Double`, `e`/`E` scientific `Double`, `s` string.

```rust
let n = 255
println("hex ${n:x}, bin ${n:b}")            // => hex ff, bin 11111111
println("[${42:>6}] [${42:<6}] [${42:^6}]")  // => [    42] [42    ] [  42  ]
println("[${42:06}] [${-42:06}]")            // => [000042] [-00042]
println("${n:#x} ${n:#b} ${n:+} ${-7:+}")    // => 0xff 0b11111111 +255 -7
println("[${255:#08x}]")                     // => [0x0000ff]   (zeros after the prefix)
let pi = 3.14159
println("pi=${pi:.2f}")                      // => pi=3.14
println("g=${1000000.5:g}")                  // => g=1000000.5   (shortest round-trip)
println("${1234.5:e} ${1234.5:.2e}")         // => 1.234500e3 1.23e3   (scientific, default precision 6)
println("[${"hi":*^8}]")                     // => [***hi***]
```

Specifiers apply to **scalars only** (`Int`, `Double`, `String`, `Bool`) — the same rule as `+`. A specifier on
a compound value is a type error (`… does not implement Format`); use a plain `${value}` hole for those. An
inapplicable field is ignored (e.g. a precision on an `Int`), and the specifier grammar is checked at compile
time, so a malformed `${x:zq}` is a compile error. A negative number in a non-decimal base prints
sign-magnitude (`${-255:x}` => `-ff`, `${-255:#x}` => `-0xff`). Scientific `e`/`E` defaults to precision 6
(like C's `%e`) and prints a minimal signed exponent (`1.234500e3`, `${x:e}` of a small value like
`${0.05:e}` => `5.000000e-2`).

Under the hood `${value:spec}` desugars to `format(value, "spec")` — an ordinary function you can also call
directly — so, like the rest of interpolation, specifiers add nothing to the runtime.

> **Not yet supported:** significant-digit precision for `g` (`${x:.3g}` currently ignores the precision),
> a shortest-round-trip mantissa for `e`/`E` (it uses a fixed default precision instead), digit grouping,
> dynamic width/precision (`${x:>{w}}`), named arguments, string precision (truncation), and two's-complement
> negative bases. To assemble a large string efficiently in a loop, use the `StringBuilder` helper (below)
> rather than a long `+` chain.

### Raw strings

A **raw string** is written with an `r` prefix — `r"…"` — and takes its contents **byte for byte**: no
backslash escapes and no `${ … }` interpolation are processed. A `\` is just a backslash and a `${` is just
those two characters. This is the natural choice for regexes, Windows paths, and any text full of backslashes
or `$` that you would otherwise have to escape.

```rust
let path = r"C:\Users\name\file.txt"     // no \U / \n / \f escaping needed
let re   = r"\d+\.\d+"                   // a regex, verbatim
println(path)                            // => C:\Users\name\file.txt
println("len=${len(re)}")                // => len=8   (\d+\.\d+ is 8 bytes)
```

To include a `"` inside a raw string, use the **hash form**: open with `r#"` and close with `"#`. The string
then ends only at a `"` followed by the matching number of `#`, so a bare `"` is content. Add more `#`s
(`r##"…"##`) if the text itself contains `"#`.

```rust
let json = r#"{"key": "value with \" backslash-quote"}"#
println(json)                            // => {"key": "value with \" backslash-quote"}
```

A raw string may span multiple lines — a newline in the source is part of the string:

```rust
let banner = r"line one
line two"
println(banner)                          // prints both lines
```

A raw string is an ordinary `String` (the `r` is purely lexical), so it works everywhere a string literal
does — including as an operand of `+`, inside a `${ … }` hole of a *normal* string, and in patterns. Only the
lowercase `r` immediately before the quote (or `#`) starts a raw string; an identifier like `range` or a bare
`r` on its own is unaffected.

You can move between a `String` and its raw bytes with `toBytes` and `fromBytes`. This is how you build a
string from computed **bytes** (for **code points** there is the opt-in `Char` type and
`codePointToStr` / `appendCodePoint`, covered in [Char and UTF-8 code points](#char-and-utf-8-code-points)):

```rust
let bytes = toBytes("Hi")
println("byte0=" + bytes[0])         // => byte0=72   (the byte 'H')
println("round=" + fromBytes(bytes)) // => round=Hi
```

Putting these together, you can build a string byte by byte — collect bytes into a `Bytes` buffer, then
`fromBytes` it:

```rust
fn repeatChar(c: Int, times: Int) -> String {
    let buf = bytes()          // an empty byte buffer
    let mut i = 0
    while i < times {
        push(buf, c)           // append the byte
        i = i + 1
    }
    fromBytes(buf)
}

println("stars=" + repeatChar('*', 5))   // => stars=*****
```

### Building strings efficiently — `StringBuilder`

Concatenating with `+` in a loop allocates a fresh intermediate string at every step (`O(n²)` overall). When
you assemble a string piece by piece, reach for `StringBuilder` instead: it accumulates into a `Bytes` buffer
(one bulk copy per append) and hands you the finished `String` with `build`.

```rust
let mut sb = stringBuilder()  // or stringBuilderCap(64) to pre-size the buffer
sb.append("items: ")
let mut i = 0
while i < 3 {
    sb.append("[")
    sb.appendByte('0' + i)    // append a single byte
    sb.append("]")
    i = i + 1
}
println(sb.build())           // => items: [0][1][2]
```

`sb.append(...)` / `sb.appendByte(...)` mutate the builder in place (and also return it, so calls chain —
`sb.append("a").append("b")`), so the builder binding must be `mut`; `sb.len()` is the number of bytes
accumulated so far. `StringBuilder` lives in the always-available `std::string` ring.

### String helpers

`std::string` provides the everyday helpers for searching, trimming, and changing case. They all work on
**bytes**: indices and lengths are byte offsets, and case conversion maps only ASCII `A`–`Z` / `a`–`z` (any
byte `≥ 0x80`, including UTF-8 continuation bytes, is left untouched). For ASCII text this is exactly what you
expect; for multibyte UTF-8 it operates byte-wise.

```rust
let s = "  Hello, World  "

// search: byte index of the first / last match, or -1 if absent
println("idx="  + indexOf(s, "World"))        // => idx=9
println("last=" + lastIndexOf("a.b.c", "."))  // => last=3
println("miss=" + indexOf(s, "xyz"))          // => miss=-1

// predicates
println(startsWith("README.md", "READ"))      // => true
println(endsWith("README.md", ".md"))         // => true
println(hasSubstr(s, "lo, W"))                // => true

// trim ASCII whitespace ( \t \n \v \f \r and space )
println("[" + trim(s) + "]")                  // => [Hello, World]
println("[" + trimStart(s) + "]")             // => [Hello, World  ]
println("[" + trimEnd(s) + "]")               // => [  Hello, World]

// ASCII case
println(toUpper("Hi, x9!"))                   // => HI, X9!
println(toLower("Hi, X9!"))                   // => hi, x9!

// small companions
println(charAt("ABC", 1))                     // => 66   (the byte at index 1)
println(isEmpty(""))                          // => true
```

Three more build the result string from an input. `replace` substitutes **all** occurrences (an empty pattern
is a no-op); `padStart` / `padEnd` grow a string to a byte `width` with a single `padByte` (returning it
unchanged if it is already that wide); `repeatStr` repeats a string `n` times.

```rust
println(replace("a.b.c", ".", "-"))            // => a-b-c
println(replace("hello", "l", ""))             // => heo   (delete every "l")

println("[" + padStart("42", 5, '0') + "]")    // => [00042]
println("[" + padEnd("42", 5, ' ') + "]")      // => [42   ]

println(repeatStr("ab", 3))                    // => ababab
```

For classifying a single character — remember a character is just an `Int` byte — there is the
`isAscii…` family: `isAsciiControl`, `isAsciiDigit`, `isAsciiWhitespace`, `isAsciiAlpha`,
`isAsciiAlphanumeric`, `isAsciiUpper`, `isAsciiLower`, and `isAsciiPrintable`. Each takes an `Int` and
returns a `Bool`; a byte `≥ 128` answers `false` to all of them. `hasControl(s)` is a whole-string
companion — `true` if `s` contains any ASCII control byte.

```rust
println(isAsciiDigit('7'))         // => true
println(isAsciiAlpha('Q'))         // => true
println(isAsciiControl(9))         // => true    (tab)
println(isAsciiPrintable(127))     // => false   (DEL)
println(hasControl("clean"))       // => false
println(hasControl("bad\ttab"))    // => true
```

To take a string *apart* — into fields on a separator, or into lines — use `split` and `lines`, covered with
the iterators in [section 19](#19-iterators) since they produce lazy `String` iterators.

---

## 8. Control flow is expressions

Almost everything in Skarn is an expression that produces a value, including `if`, `match`, blocks, and
`loop`. This is why you rarely need a separate "temporary variable then assign" dance.

### `if` / `else`

`if` is an expression. When you use its value, it needs an `else`, and both branches must produce the same
type.

```rust
let n = 75
let grade =
    if n >= 90 { "A" }
    else if n >= 40 { "C" }
    else { "F" }
println("grade=" + grade)   // => grade=C
```

A block `{ ... }` is also an expression: its value is the value of its last expression.

```rust
let area = {
    let w = 3
    let h = 4
    w * h        // the block evaluates to 12
}
println("area=" + area)   // => area=12
```

### `while`

`while` runs a block while a `Bool` condition holds. It is used for its side effects and has no useful value.

```rust
let mut i = 0
let mut acc = 0
while i < 5 {
    acc = acc + i
    i = i + 1
}
println("acc=" + acc)   // => acc=10
```

### `loop` and `break`

`loop` is an infinite loop. You leave it with `break`. Uniquely, in a `loop` you can `break` **with a value**,
which becomes the value of the whole `loop` expression — handy for "search and return".

```rust
let mut n = 8
let firstPowerOfTwoAtLeastN =
    loop {
        if n & (n - 1) == 0 { break n }   // n is a power of two
        n = n + 1
    }
println("power=" + firstPowerOfTwoAtLeastN)   // => power=8
```

(`break` with a value is only allowed in `loop`. `while` and `for` loops fall through to a plain exit, so they
use a valueless `break`.)

### `for`

`for pattern in iterable` walks an iterable — a range, a collection, or any iterator (see
[§19](#19-iterators)). `break` and `continue` work inside it.

```rust
let mut sum = 0
for k in range(0, 5) {   // 0, 1, 2, 3, 4
    if k == 3 { continue }
    sum = sum + k
}
println("sum=" + sum)   // => sum=7   (0 + 1 + 2 + 4)
```

### Statement position

When an expression's value is not used — a statement that is not the last in a block, or the body of a
`while`/`for`, or the body of a function returning `()` — its value is simply discarded. So you can call a
function that returns something and ignore the result, and a statement-position `if` needs no `else`:

```rust
fn logIfBig(n: Int) -> () {
    if n > 100 {
        println("big: " + n)     // no `else` needed; the value is discarded
    }
}
logIfBig(500)   // => big: 500
```

---

## 9. Functions

A function is declared with `fn`. **Every parameter and the return type must be annotated.** The body is a
block whose final expression is the return value.

```rust
fn add(a: Int, b: Int) -> Int {
    a + b        // the last expression is returned
}
println("add=" + add(2, 3))   // => add=5
```

You can also return early with `return`:

```rust
fn describe(n: Int) -> String {
    if n < 0 {
        return "negative"
    }
    if n == 0 {
        return "zero"
    }
    "positive"
}
println(describe(-5))   // => negative
```

A function that does not produce a useful value returns `()` (unit):

```rust
fn greet(name: String) -> () {
    println("Hi, " + name)
}
greet("Ada")   // => Hi, Ada
```

### Recursion and tail calls

Recursion works as usual:

```rust
fn factorial(n: Int) -> Int {
    if n <= 1 { 1 } else { n * factorial(n - 1) }
}
println("5! = " + factorial(5))   // => 5! = 120
```

Skarn guarantees **proper tail calls**: when a function's last action is to call another function (including
itself), it reuses the current stack frame instead of growing the stack. This means a tail-recursive loop runs
in constant stack space — you can recurse millions of levels deep without overflowing:

```rust
fn countUp(n: Int, acc: Int) -> Int {
    if n == 0 { acc }
    else { countUp(n - 1, acc + 1) }     // tail call: no stack growth
}
println("deep=" + countUp(1000000, 0))   // => deep=1000000
```

---

## 10. Closures and function values

Functions are values. You can pass them to other functions, store them, and return them. A function type is
written `fn(ArgTypes) -> ReturnType`.

```rust
fn applyTwice(f: fn(Int) -> Int, x: Int) -> Int {
    f(f(x))
}

let inc = fn(x: Int) -> Int { x + 1 }   // an anonymous function (lambda)
println("twice=" + applyTwice(inc, 10)) // => twice=12
```

A lambda can **capture** variables from the enclosing scope. Captures are **by value** (the lambda gets a copy
of the captured value at the moment it is created). Here a function builds and returns another function that
remembers `by`:

```rust
fn adder(by: Int) -> fn(Int) -> Int {
    fn(x: Int) -> Int { x + by }   // captures `by`
}

let add10 = adder(10)
println("closure=" + add10(5))     // => closure=15
```

**What "by value" precisely means** — the lambda copies the *slot* of each captured variable at the moment it
is created, and nothing more. For an **immediate** (an `Int`, `Bool`, `Char`, …) the slot *is* the value, so the
lambda takes a **snapshot**: a later reassignment of the outer variable is invisible to it.

```rust
let mut counter = 0
let peek = fn() -> Int { counter }
counter = 10
println(peek())   // => 0   (the snapshot taken at creation, not the current 10)
```

For a **heap value** (a `Map`, `Vec`, a struct, …) the slot is a *pointer*, so the lambda shares the **same
object** — mutating it through either name is visible to both (this is exactly what makes the memoize trick in
[§14](#14-collections) work). But reassigning the outer variable to a *new* object still leaves the lambda holding the
old one. In short: a capture shares the pointed-to object, never the variable *binding*.

### Type inference for lambdas

A lambda's parameter and return types are **inferred from context** whenever the surrounding code already fixes
which function type is expected — most commonly a call argument (`map`, `filter`, `fold`, …) or a `let` with an
explicit function type. In those positions you can drop every annotation:

```rust
let evens = collect(filter(range(0, 10), fn(x) { x % 2 == 0 }))  // x: Int, -> Bool  (from filter)
println(toString(evens))                                         // => [0, 2, 4, 6, 8]

let f: fn(Int) -> Int = fn(x) { x + 1 }                          // x: Int, -> Int   (from the let type)
println("f=" + f(41))                                            // => f=42
```

The one place you must annotate is a lambda with **no** expected type — such as a bare `let` binding, where
there is nothing to infer from:

```rust
let inc = fn(x: Int) -> Int { x + 1 }   // annotations required -- nothing here fixes x's type
```

Omitting them there is a compile error (`cannot infer the type of lambda parameter 'x'`). This is the same
annotate-at-the-boundary discipline Skarn uses throughout: types are inferred locally, but never guessed across a
definition boundary.

Function values combine naturally with the pipe operator and with the iterator combinators in
[§19](#19-iterators).

---

## 11. Structs and tuples

### Structs

A `struct` groups named fields. You create one with `Name { field: value, ... }` and read a field with `.name`.

```rust
struct Point {
    x: Int,
    y: Int
}

fn manhattan(p: Point) -> Int {
    let ax = if p.x < 0 { -p.x } else { p.x }
    let ay = if p.y < 0 { -p.y } else { p.y }
    ax + ay
}

let p = Point { x: 3, y: 4 }
println("p.x="  + p.x)             // => p.x=3
println("dist=" + manhattan(p))    // => dist=7
```

A struct is a **reference value**, exactly like a collection: a binding or parameter holds a *reference* to it,
not the struct itself, so assigning it to another name or passing it to a function shares one object rather than
copying it. What "immutable by default" gives you is that a plain binding cannot be mutated — there is no
"mutate a field" statement unless the binding is `mut`. So structs *feel* value-like as long as you don't
mutate, but that is a property of the default, not of copying: two `mut` names for the same struct see each
other's changes. This matters enough that [§25](#25-the-memory--cost-model) treats it in full. The idiomatic
"change" is therefore not to mutate in place but to build a new struct:

```rust
let moved = Point { x: p.x + 1, y: p.y }
println("moved.x=" + moved.x)   // => moved.x=4
```

Spelling out every unchanged field gets tedious. **Record update** `Name { field: value, ..base }` copies
every field you *don't* list from `base` (the same struct type, evaluated once), so you write only what
changes. It must come last; `Point { ..base }` alone is a plain copy:

```rust
struct Config { host: String, port: Int, tls: Bool }

let base = Config { host: "localhost", port: 8080, tls: false }
let secure = Config { port: 443, tls: true, ..base }   // host copied from base
println("host=" + secure.host + " port=" + secure.port)   // => host=localhost port=443
```

When a field's value is already held by a binding of the *same name*, **field shorthand** lets you write the
name once: `Point { x, y }` means `Point { x: x, y: y }`. Shorthand and explicit fields mix freely, and it
combines with record update. The name must be a **local binding** — shorthand does not reach for a global or an
imported name:

```rust
fn scaled(x: Int, y: Int, k: Int) -> Point {
    let x = x * k
    let y = y * k
    Point { x, y }                     // == Point { x: x, y: y }
}

let q = scaled(3, 4, 2)
println("q.x=" + q.x + " q.y=" + q.y)  // => q.x=6 q.y=8
```

This is most worth having in constructor functions, where the parameters are deliberately named after the
fields they fill.

When the binding *is* `mut`, you can assign through a whole **place path** — any chain of field (`.x`) and
index (`[i]`) steps, e.g. `pts[0].x = 40` or `line.start.y = 9`. Only the **root** binding needs `mut`
(assigning through a temporary such as `mk().x` is rejected):

```rust
struct Pt { x: Int, y: Int }

let mut pts: Vec[Pt] = vec()
push(pts, Pt { x: 1, y: 2 })
pts[0].x = 40                       // index-then-field, through the mut root `pts`
println("pts[0].x=" + pts[0].x)     // => pts[0].x=40
```

### Tuple structs

A struct can have positional fields instead of named ones. You read them by destructuring (see
[§13](#13-pattern-matching)):

```rust
struct Rgb(Int, Int, Int)

let red = Rgb(255, 0, 0)
let Rgb(r, g, b) = red          // destructure to name the fields
println("r=" + r + " g=" + g + " b=" + b)   // => r=255 g=0 b=0
```

### Transparent newtypes

A `transparent struct` with a single `Int`, `Double`, or `Bool` field is a **zero-cost wrapper**: it is a
distinct type to the compiler, but at run time it *is* the wrapped value — no object is allocated, and
construction, the `.0` projection, and pattern matching all compile to nothing. Use it to give a bare number a
meaningful, misuse-proof type (`UserId` is not interchangeable with a plain `Int`):

```rust
transparent struct UserId(Int)

let a = UserId(41)
let b = UserId(a.0 + 1)                 // `.0` reads the wrapped value (unique to transparent structs)
println(b.0)                            // => 42
match b { UserId(n) => println(n) }     // => 42  -- patterns work too

let mut seen: Map[UserId, Bool] = #{}   // a transparent newtype is a valid map / set key
seen[b] = true
println(getOr(seen, UserId(42), false)) // => true
```

Notes and limits (this is an intentionally small first version):

- Exactly **one field**, of type `Int`, `Double`, or `Bool`; written in the tuple form `Name(T)`. (A regular
  tuple struct like `Rgb` above has no `.0` — you read it by destructuring; `.0` is unique to `transparent`.)
- `toString`, `print`, and `"${…}"` interpolation print the **name** at the top level: `toString(UserId(42))`
  is `"UserId(42)"` (and `Meters(3.0)`, `Flag(true)` likewise). A transparent value **nested inside a container**
  still shows the bare value (`toString([UserId(1), UserId(2)])` is `"[1, 2]"`) — a container dump has no type
  information to recover the name.
- You **cannot implement a trait** for a transparent struct (it shares its underlying type's runtime identity).

### Anonymous tuples

A tuple `(a, b, ...)` groups values without declaring a type. You read a tuple by position with `.0`, `.1`,
..., or by destructuring:

```rust
let pair = (7, "seven")
println("first="  + pair.0)     // => first=7
println("second=" + pair.1)     // => second=seven

let (num, word) = pair          // destructuring
println(num + " = " + word)     // => 7 = seven
```

---

## 12. Enums (sum types)

An `enum` is a type that is exactly one of several named **variants**. A variant may carry data (a payload) or
not. Enums are the natural way to model "one of these shapes" — states, results, trees.

```rust
enum Direction {
    North,
    South,
    East,
    West
}

fn dx(d: Direction) -> Int {
    match d {
        East => 1,
        West => -1,
        North => 0,
        South => 0
    }
}
println("dx(East)=" + dx(East))   // => dx(East)=1
```

Variants can carry values:

```rust
enum Shape {
    Circle(Double),
    Rect(Double, Double)
}

fn area(s: Shape) -> Double {
    match s {
        Circle(r)  => 3.14159 * r * r,
        Rect(w, h) => w * h
    }
}
println("circle=" + area(Circle(2.0)))    // => circle=12.56636
println("rect="   + area(Rect(3.0, 4.0))) // => rect=12.0
```

A variant's payload can also be **named fields** — a *record variant* `V { f: T }`. It is built and
matched with the same brace syntax as a record struct (fields by name, order-free), but its type is the
enum. Tuple variants, record variants, and nullary variants can be mixed freely in one enum:

```rust
enum Shape {
    Dot,
    Circle { r: Double },
    Rect { w: Double, h: Double }
}

fn area(s: Shape) -> Double {
    match s {
        Dot              => 0.0,
        Circle { r }     => 3.14159 * r * r,
        Rect { w, h }    => w * h
    }
}
println("circle=" + area(Circle { r: 2.0 }))       // => circle=12.56636
println("rect="   + area(Rect { h: 4.0, w: 3.0 })) // => rect=12.0  (fields are order-free)
```

Enums can be generic and recursive, which lets you build tree-shaped data:

```rust
enum Tree[T] {
    Leaf(T),
    Branch(Tree[T], Tree[T])
}

fn countLeaves[T](t: Tree[T]) -> Int {
    match t {
        Leaf(_)      => 1,
        Branch(l, r) => countLeaves(l) + countLeaves(r)
    }
}

let tree = Branch(Branch(Leaf(1), Leaf(2)), Leaf(3))
println("leaves=" + countLeaves(tree))   // => leaves=3
```

The built-in `Option` and `Result` types are themselves enums; see [§18](#18-option-result-and-the--operator).

### Integer-backed enums

An enum whose variants are **all nullary** (no payload) can opt into an **Int-backed** representation with a
`: Int` annotation. At runtime such a value **is** a plain integer — zero heap allocation per value — while
staying a distinct type you match on by name (like the mirror of a `transparent struct`, §11):

```rust
enum Direction : Int { North, East, South, West }        // discriminants 0, 1, 2, 3

let d: Direction = South
println(d)                                     // => South   (prints the NAME, not 2)
println(match d { North => "up", East => "right", South => "down", West => "left" })  // => down

let mut seen: Map[Direction, Int] = #{}        // usable as a map key (it is an Int underneath)
seen[South] = 1
println(getOr(seen, South, 0))                 // => 1
```

Discriminants are the declaration order (`0, 1, 2, …`) unless you **pin** one with `= value`; the counter then
resumes from there:

```rust
enum Status : Int { Ok = 200, Created = 201, NotFound = 404, Teapot = 418 }
```

Notes and limits:

- Every variant must be nullary — a payload variant (`V(Int)` / `V { x: Int }`) under `: Int` is an error (it
  can't fit in a single integer). The repr must be `Int`.
- `toString`, `print`, and `"${…}"` print the variant **name** at the top level (`South`). A value **nested in a
  container** prints as the bare integer (`println([North, South])` => `[0, 2]`) — the dump has no type
  information to recover the name.
- You **cannot implement a trait** for an Int-backed enum (it shares its integer representation at runtime).
- There is no built-in `Direction`↔`Int` conversion yet; you match on the variants by name.

### Namespacing variants under their enum

Variant constructors belong to their enum. You can always write the **qualified path** `Enum::Variant` — as
a value, a call, a pattern, or a record-variant literal:

```rust
enum Color { Red, Green, Blue }
enum Shape { Dot, Rect { w: Int, h: Int } }

let c = Color::Green                        // qualified value
let s = Shape::Rect { w: 3, h: 4 }          // qualified record-variant literal
match c { Color::Red => 0, Color::Green => 1, Color::Blue => 2 }
```

Within the module that **defines** an enum, its variants are also usable **bare** (`Green`, `Rect { … }`),
so most code needs no qualifier. The qualified form matters when two enums in the **same module** share a
variant name — they now coexist, and you disambiguate with the enum:

```rust
enum Token { Number, Ident }
enum Node  { Number, List }                 // `Number` also here -- no longer an error

let t = Token::Number
let n = Node::Number
```

A **bare** `Number` in that module is a compile error ("ambiguous variant 'Number'; qualify it") — write
`Token::Number` or `Node::Number`. (The prelude's `Option`/`Result` variants — `Some`/`None`/`Ok`/`Err` —
are always available bare, everywhere.)

To bring another module's variants in bare, import them from the enum:

```rust
use std::json::Json::*                       // all of Json's variants, bare
use std::json::Json::{ Null, Text }          // or just some
```

### Char and UTF-8 code points

A `String` is a sequence of **bytes** by default: `for c in s` yields byte values, `len(s)` is a byte count,
and a char literal `'A'` is an `Int` byte (0..255). When you want Unicode **code points**, use the opt-in
**`Char`** type — a zero-cost wrapper over the code point (it *is* the bare integer at runtime, no allocation)
that keeps code points distinct from plain integers:

```rust
let c: Char = 'A'                  // a byte char literal is a Char in Char context
let euro = Char(0x20AC)            // build a Char from any code point
println(c)                         // => A        (prints the GLYPH)
println(euro)                      // => €
println(codePoint(euro))           // => 8364     (unwrap to the Int code point)

println(c == 'A')                  // => true
println('a' <= Char('m') && Char('m') <= 'z')    // => true   (Chars compare with < <= > >=)
// note: a Char is comparable with `<`, but not `Ord` — `sort(Vec[Char])` / `minOf(Char, …)` are rejected
// (an erasure type cannot implement the method trait `Ord`; see §16 and §19).

let mut counts: Map[Char, Int] = #{}             // a Char is a valid map key
counts[c] = 1
println(getOr(counts, c, 0))       // => 1
```

Note that a *bare* char literal has no `Char` context, so it stays an `Int` byte — `println('A')` prints `65`,
and `b[i] == 'A'` compares bytes. The glyph appears only for `Char`-typed values.

The UTF-8 codec is a small pure-Skarn library (no import needed):

```rust
println(codePointToStr(0x1F600))   // => 😀      (encode a code point to its UTF-8 String)
let s = "héllo€"
println(len(s))                    // => 9       (bytes)
println(charCount(s))              // => 6       (code points)
match nthChar(s, 5) { Some(ch) => println(ch), None => {} }   // => €   (the 6th code point)
println(fromChars(chars(s)) == s)  // => true    (decode + re-encode round-trips)
```

`chars(s)` is a lazy iterator of the `Char`s in `s` (a malformed byte decodes to U+FFFD, the replacement
character, so it never fails); `charCount`/`nthChar` count/index code points (O(n) — a String is not directly
indexable). `for c in s` is unchanged and still walks **bytes**. As with transparent newtypes, you cannot
`impl` a trait for `Char`, and a `Char` **nested in a container** prints as its bare code-point integer
(`println([c, euro])` => `[65, 8364]`).

---

## 13. Pattern matching

`match` inspects a value against a series of **patterns** and runs the first arm that fits. It is an
expression, so it produces a value, and it is **exhaustive**: the compiler requires that your patterns cover
every possible case (a wildcard `_` covers "everything else").

### Basic matching

```rust
fn classify(n: Int) -> String {
    match n {
        0 => "zero",
        1 => "one",
        _ => "many"     // wildcard: everything else
    }
}
println(classify(0) + " " + classify(1) + " " + classify(50))
// => zero one many
```

### Binding and guards

A lowercase name in a pattern **binds** the matched value. An arm can add a `if` **guard** that must also hold:

```rust
fn sign(n: Int) -> String {
    match n {
        x if x > 0 => "pos",
        x if x < 0 => "neg",
        _          => "zero"
    }
}
println(sign(5) + " " + sign(-2) + " " + sign(0))   // => pos neg zero
```

### Matching literals, including characters

Because a character literal is an `Int`, you can match on characters directly:

```rust
fn isVowel(c: Int) -> Bool {
    match c {
        'a' => true,
        'e' => true,
        'i' => true,
        'o' => true,
        'u' => true,
        _   => false
    }
}
println("vowel a=" + isVowel('a'))   // => vowel a=true
println("vowel z=" + isVowel('z'))   // => vowel z=false
```

### Or-patterns: one arm, several shapes

When several patterns should run the **same** arm, join them with `|`. The arm matches if **any**
alternative matches, so the five-arm vowel table above collapses to one line:

```rust
fn isVowel(c: Int) -> Bool {
    match c {
        'a' | 'e' | 'i' | 'o' | 'u' => true,
        _ => false
    }
}
println("vowel e=" + isVowel('e'))   // => vowel e=true
```

Or-patterns count toward exhaustiveness, so a single arm can cover every case of an enum without a
wildcard, and a guard (`if …`) applies to the whole group:

```rust
enum Dir { North, South, East, West }

fn axis(d: Dir) -> String {
    match d {
        North | South => "vertical",
        East  | West  => "horizontal"
    }
}
println(axis(East))   // => horizontal
```

A leading `|` is allowed (handy when you list alternatives vertically). Two rules keep them sound: the
alternatives may **not bind variables** (use literals, `_`, or nullary constructors — for value-carrying
variants that need a binding, write separate arms), and an alternative that can never match (already
covered by an earlier one) is reported as an unreachable-pattern warning.

### Range patterns

A numeric range `lo..hi` matches any value **from `lo` to `hi`, inclusive of both ends**. The bounds must be
`Int` or `Double` literals (a character literal is an `Int`, so `'0'..'9'` matches the digit bytes). This
replaces a chain of `>=`/`<=` guards for byte and number classification:

```rust
fn classify(c: Int) -> String {
    match c {
        '0'..'9' => "digit",
        'a'..'z' | 'A'..'Z' => "letter",
        ' ' | '\t' | '\n' => "space",
        _ => "other"
    }
}
println(classify('7'))   // => digit
println(classify('Q'))   // => letter
```

The sibling form **`lo..<hi` excludes the upper bound** — it matches `lo <= v < hi`:

```rust
fn grade(n: Int) -> String {
    match n {
        0..<60  => "F",
        60..<70 => "D",
        70..<80 => "C",
        80..<90 => "B",
        _       => "A"
    }
}
println(grade(59) + grade(60) + grade(89) + grade(90))   // => FDBA
```

Note that `..` is the **inclusive** form — this is not Rust, where a bare `..` excludes the upper bound. On
`Int` bounds the two spellings are interchangeable (`0..<10` is exactly `0..9`), so pick whichever reads
better against the number you actually have in mind: `0..<60` says "below 60" more directly than `0..59`.
On `Double` bounds the choice is real, because there is no way to write the largest double below `1.0`:

```rust
fn unitInterval(x: Double) -> Bool { match x { 0.0..<1.0 => true, _ => false } }
println("${unitInterval(0.999)} ${unitInterval(1.0)}")   // => true false
```

A range binds nothing, and — because the numeric types are unbounded — a range never makes a match
exhaustive on its own: a `match` built only from ranges still needs a `_` arm. The two forms are distinct
patterns, so `1..9` and `1..<9` may appear in the same `match` without either being flagged unreachable.

### Struct, enum, and tuple patterns

Patterns mirror the shapes you can construct. You can match specific field values and bind the rest:

```rust
struct Point { x: Int, y: Int }

fn quadrant(p: Point) -> Int {
    match p {
        Point { x: 0, y: 0 } => 0,        // exactly the origin
        Point { x, y }       => if x > 0 && y > 0 { 1 } else { 9 }
    }
}
println("q="      + quadrant(Point { x: 2, y: 3 }))   // => q=1
println("origin=" + quadrant(Point { x: 0, y: 0 }))   // => origin=0
```

`Point { x, y }` above is the pattern form of the same **field shorthand** the struct literal has: it binds each
field to a name equal to the field. Two rules go with it. A struct pattern must name **every** field — there is
no `..` rest pattern, so adding a field to a struct makes the compiler point at each pattern that now needs
updating. And because shorthand *binds*, it may not appear in an or-pattern, which (in this version) forbids
bindings entirely:

```rust
// match e { A(P { x, y }) | B(P { x, y }) => x + y }
// error: an or-pattern alternative cannot bind a variable; use separate match arms
```

Patterns **nest**: you can match a variant inside another variant in one arm, which is how you take apart
layered data without a chain of nested `match`es.

```rust
enum Shape { Circle(Double), Rect(Double, Double) }

fn describe(s: Option[Shape]) -> String {
    match s {
        Some(Circle(r))  => "circle r=" + toString(r),
        Some(Rect(w, h)) => "rect " + toString(w) + "x" + toString(h),
        None             => "nothing"
    }
}
println(describe(Some(Rect(2.0, 3.0))))   // => rect 2.0x3.0
println(describe(None))                   // => nothing
```

### List patterns

A `List` (see [§14](#14-collections)) can be matched as empty `[]` or as "a head and the rest" `[h, ..t]`:

```rust
fn listLen[T](xs: List[T]) -> Int {
    match xs {
        []          => 0,
        [_, ..rest] => 1 + listLen(rest)
    }
}
println("len=" + listLen([10, 20, 30]))   // => len=3
```

### Map patterns

A `Map` can be matched with `#{ key => value_pattern, … }`. This is a **partial** match: each key you name
is a literal, its value is checked or bound by a sub-pattern, and every other entry is ignored. Use a plain
name to bind the value, or `_` to test only for the key's presence. (There is no whole-map pattern — a map
pattern never forces a match on its own, so an arm list built only of map patterns still needs a `_`.)

```rust
let mut inv: Map[String, Int] = #{}
inv["gold"] = 42
inv["wood"] = 7

let msg = match inv {
    #{ "gold" => n } => "you have " + n + " gold",   // binds n = 42
    _               => "no gold"
}
println(msg)   // => you have 42 gold
```

### Mutable bindings in a pattern

A binding in a `match` arm can be marked `mut`, exactly like a `let`, so you can reassign it inside the arm:

```rust
fn clampLow(o: Option[Int]) -> Int {
    match o {
        Some(mut n) => { if n < 0 { n = 0 }  n },   // n is reassignable here
        None        => 0
    }
}
println(clampLow(Some(-5)))   // => 0
println(clampLow(Some(7)))    // => 7
```

As with any `mut`, this makes the *binding* reassignable; it does not change how the matched value is stored.
(The `mut` marker applies to a `match` arm's bindings; a destructuring `let` binding stays immutable.)

### Destructuring `let`

When a pattern cannot fail (a tuple, a single-variant destructure), you can use it directly in a `let`:

```rust
let (a, b) = (1, 2)
println("a=" + a + " b=" + b)   // => a=1 b=2
```

### `if let`

`if let` runs a block only when a value matches a single pattern, binding along the way. It is the concise form
of a `match` with one interesting arm:

```rust
let maybe: Option[Int] = Some(42)

if let Some(v) = maybe {
    println("got " + v)       // => got 42
} else {
    println("nothing")
}
```

### `while let`

`while let` is the looping sibling: it repeats a block **as long as** a value keeps matching a pattern,
binding along the way. It is the concise form of a `loop` that `break`s when the pattern stops matching —
ideal for draining a container or consuming a cursor:

```rust
let v: Vec[Int] = vec()
push(v, 1) push(v, 2) push(v, 3)

let mut total = 0
while let Some(x) = pop(v) {   // stops when pop returns None
    total = total + x
}
println("total=" + total)      // => total=6
```

---

## 14. Collections

Skarn provides several collection types. They all work with `for`, `len`, and (where it makes sense) indexing.

### Fixed-size arrays: `Array[T]`

`array(n, init)` creates an array of `n` elements all equal to `init`. Index with `a[i]`. To assign to an
element, the array binding must be `mut`. An `init` value is required because Skarn types are non-nullable — a
fresh slot must already hold a real `T` (there is no zero-fill). For the empty case, `emptyArray()` builds an
empty `Array[T]` (its element type inferred from context, like `vec()`).

```rust
let mut arr = array(3, 0)
arr[0] = 10
arr[1] = 20
arr[2] = 30
println("arr[1]=" + arr[1])   // => arr[1]=20
println("len="    + len(arr)) // => len=3
```

Indexing is **bounds-checked**: `a[i]` aborts (a located panic) if `i` is out of range. When a miss should be
handled instead of fatal, use `get(a, i)`, which returns an `Option[T]` (`None` for an out-of-range index):

```rust
let mut arr = array(3, 0)
arr[0] = 10
match get(arr, 9) {
    Some(x) => println("got " + x),
    None    => println("arr[9]=None")   // => arr[9]=None
}
```

### Growable vectors: `Vec[T]`

`vec()` creates an empty, growable vector. `push(v, x)` appends and `pop(v)` removes the last element
(returning an `Option`, since the vector might be empty). A vector is a reference value, so `push`/`pop` mutate
the *shared* object in place — every name bound to it, and every function it was passed to, sees the change
(see [§25](#25-the-memory--cost-model)). Because they mutate, the binding you push/pop through must be `mut`
(Skarn's one mutation rule — see below).

```rust
let mut v: Vec[Int] = vec()
push(v, 1)
push(v, 2)
push(v, 3)
let last = pop(v)
println("len="  + len(v))            // => len=2
println("last=" + toString(last))    // => last=Some(3)
```

A vector also supports random access with `v[i]`, exactly like an array: reading is bounds-checked (aborts on
an out-of-range index), and `get(v, i)` is the safe `Option` form. **One consistent rule governs all in-place
mutation:** whether you push/pop, or assign to an element (`v[i] = x`), or assign to a field (`p.x = v`), the
*root binding* must be declared `mut`. A mutating function like `push` takes its target through a `mut`
parameter, so passing a plain (non-`mut`) binding to it is a compile error — exactly as Rust requires `let mut`
before `v.push(..)`. (A fresh temporary, e.g. `push(vec(), 1)`, is trivially yours to mutate and needs no
binding.)

```rust
let mut v: Vec[Int] = vec()
push(v, 10)
push(v, 20)
push(v, 30)
println("v[1]=" + v[1])              // => v[1]=20
v[1] = 99
println("v[1]=" + v[1])              // => v[1]=99
match get(v, 5) {
    Some(x) => println("got " + x),
    None    => println("v[5]=None")  // => v[5]=None
}
```

Iterating a `Vec` with `for x in v` is a **live** view of the same object, not a snapshot: overwriting an
element in place (`v[i] = …`) before the loop reaches it *is* seen. Structural changes mid-walk stay
memory-safe — never undefined behaviour — but the walk is bounded by the elements present when it began:
`push`ing does **not** extend the current walk (the new element is not visited), and `pop`ping the vector
**below the loop's current position** turns its next read into the same defined **`array index out of bounds`**
abort as any out-of-range `v[i]` — a clean, located fault, never silent corruption. To grow or shrink while
iterating, loop over a snapshot (`clone(v)`) or an index range instead. This trap is a **deliberate**
asymmetry with the map rule below (where mid-walk mutation is *never* a crash): a `Vec` read is a
bounds-checked index, so failing fast on a shrink beats silently walking a different set — whereas a hash map
has no positional promise to violate in the first place.

### Maps: `Map[K, V]`

`#{}` is an empty map. Assign with `m[k] = v` (the binding must be `mut`). Read with `m[k]` (which aborts if
the key is missing) or, safely, with `get(m, k)` which returns an `Option`. `has`, `delete`, `keys`, and
`values` round out the API.

```rust
let mut ages: Map[String, Int] = #{}
ages["ada"]  = 36
ages["alan"] = 41

println("ada="     + ages["ada"])         // => ada=36
println("has bob=" + has(ages, "bob"))    // => has bob=false

match get(ages, "alan") {
    Some(a) => println("alan=" + a),      // => alan=41
    None    => println("no alan")
}
```

You can iterate a map with `for (key, value) in m` (a lazy, live walk over the entries — no snapshot; the
order is unspecified), snapshot its `keys(m)` or `values(m)` into arrays, and remove an entry with
`delete(m, k)`:

```rust
let mut scores: Map[String, Int] = #{}
scores["a"] = 1
scores["b"] = 2
scores["c"] = 3

let mut total = 0
for (k, v) in scores {
    total = total + v
}
println("total="  + total)             // => total=6
println("nkeys="  + len(keys(scores))) // => nkeys=3

delete(scores, "b")
println("after="  + len(keys(scores))) // => after=2
```

Because that walk is **live, not a snapshot**, mutating the *same* map inside the loop is defined and
memory-safe — never a crash, corruption, or undefined behaviour — but **which entries the walk then visits is
unspecified**: an inserted key may or may not be seen, and deleting keys mid-walk is safe but the order of the
entries still to come is not promised. When you need to add or remove keys based on what you see, iterate a
**snapshot** instead — collect `keys(m)` first and loop over that.

Four helpers make the common lookup-with-fallback and read-modify-write patterns concise. `getOr(m, k, dflt)`
returns the value or a default; `getOrElse(m, k, f)` computes the default lazily via a thunk (only on a miss);
`getOrPut(m, k, dflt)` inserts the default if the key is absent and returns the stored value; and
`upsert(m, k, dflt, f)` inserts `dflt` on a miss or replaces the value with `f(current)` on a hit. The last two
mutate the map, so the binding must be `mut`.

```rust
let mut counts: Map[String, Int] = #{}
for w in ["a", "b", "a", "c", "a"] {
    upsert(counts, w, 1, fn(c) { c + 1 })   // insert 1, or bump the existing count
}
println("a=" + getOr(counts, "a", 0))       // => a=3
println("z=" + getOr(counts, "z", 0))       // => z=0
```

Because `getOrElse` runs its thunk only on a miss, and a closure can mutate a captured heap value (a `Map` or
`Vec` is a shared reference), the two compose into a lazy **memoize**: the fallback both computes the value and
stores it, so the work happens once per distinct input.

```rust
let mut cache: Map[Int, Int] = #{}
let mut computed: Vec[Int] = vec()
let memoSquare = fn(n: Int) -> Int {
    getOrElse(cache, n, fn() {
        push(computed, n)   // record the miss
        let v = n * n
        cache[n] = v        // populate the cache from inside the thunk
        v
    })
}

println("square 4 = " + memoSquare(4))          // => square 4 = 16   (miss — computed)
println("square 4 = " + memoSquare(4))          // => square 4 = 16   (hit  — thunk not run)
println("square 5 = " + memoSquare(5))          // => square 5 = 25   (miss — computed)
println("computed " + len(computed) + " time(s)") // => computed 2 time(s)
```

A map's **key type must be `Hashable`** — the primitives `Int`, `Double`, `Bool`, and `String`, plus the
erasure types that wrap one of them: `Char`, integer-backed `enum`s (`enum … : Int`), and `transparent struct`
newtypes (each of which *is* its underlying primitive at run time — see [§7](#7-strings) and [§12](#12-enums-sum-types)).
A composite or mutable type — a plain `struct`, a tuple, a `Vec`, or an ordinary payload-carrying `enum` — is
**not** `Hashable`, so using one as a key is a **compile error**, not a runtime surprise:

```rust
struct Point { x: Int, y: Int }
let m: Map[Point, Int] = #{}   // error: type Point cannot be a map key (does not satisfy 'Hashable')
```

If you write a generic function that builds a map, give its key parameter the bound: `fn index[K: Hashable](…)`.
(The same rule powers `std::set`, since a `Set[T]` is a `Map[T, Bool]` under the hood.)

Key identity follows *SameValueZero* (the same rule JavaScript's `Map` uses), which tidies up the two floating
-point edge cases so they are not footguns: `-0.0` and `0.0` are the **same** key, and — despite `NaN != NaN`
under `==` — every `NaN` is the **same** key, so a value stored under a `NaN` key is found again with any `NaN`.
`String` keys compare by their bytes (contents), so a key you built by concatenation matches an equal literal.

### Cons lists: `List[T]`

A list literal `[a, b, c]` builds an immutable singly-linked list. Lists shine with the `[h, ..t]` pattern
(see [§13](#13-pattern-matching)) and iterate with `for`.

```rust
let nums = [1, 2, 3, 4]
let mut total = 0
for x in nums {
    total = total + x
}
println("total=" + total)   // => total=10
```

### Byte buffers: `Bytes`

`Bytes` is a growable buffer of raw bytes — useful for binary data and for building strings from computed
bytes. Bytes read and write as `Int` values in the range `0..255`. Convert with `toBytes` / `fromBytes`.

```rust
let b = toBytes("Hi")
println("b[0]=" + b[0])              // => b[0]=72
println("back=" + fromBytes(b))      // => back=Hi
```

`bytes()` starts an empty buffer that you grow with `push`; `b[i]` reads a byte and `b[i] = n` writes one (a
write masks the value with `& 0xFF`; like any index assignment it needs a `mut` binding). Indexing is
bounds-checked (`b[i]` aborts on an out-of-range index; `get(b, i)` returns an `Option[Int]`). This is how you
build a string from computed bytes:

```rust
let mut b = bytes()
push(b, 72)                          // 'H'
push(b, 105)                         // 'i'
b[0] = 74                            // 'J'
println("str=" + fromBytes(b))       // => str=Ji
println("len=" + len(b))             // => len=2
```

### Cloning: `clone`

`clone(x)` (or `x |> clone`) makes an **independent shallow copy** of a container — `Vec`, `Map`, `Bytes`, and
`Array` are all supported. The copy has its own top-level storage, so mutating it never touches the original
(nested heap references are shared, not deep-copied).

```rust
let a: Vec[Int] = vec()
push(a, 1) push(a, 2)
let b = clone(a)
push(b, 3)
println("orig=" + len(a) + " copy=" + len(b))   // => orig=2 copy=3
```

`clone` is a trait, so it is **user-extensible** — implement it for your own types and `clone` dispatches to
your body:

```rust
struct Point { x: Int, y: Int }
impl Clone for Point { fn clone(self) -> Point { Point { ..self } } }
let p = Point { x: 3, y: 4 }
let q = p |> clone
println("q=(" + q.x + ", " + q.y + ")")          // => q=(3, 4)
```

---

## 15. Generics

Functions and types can be **generic** — parameterized over one or more type variables written in square
brackets, `[T]`. A generic definition is written once and works for every concrete type you use it with.

```rust
fn identity[T](x: T) -> T { x }

println("id int=" + identity(99))       // => id int=99
println("id str=" + identity("hello"))  // => id str=hello
```

Structs and enums can be generic too:

```rust
struct Box[T] { value: T }

fn unbox[T](b: Box[T]) -> T { b.value }

let boxed = Box { value: 7 }
println("unbox=" + unbox(boxed))   // => unbox=7
```

Generics can have multiple type parameters:

```rust
fn pairUp[A, B](a: A, b: B) -> (A, B) { (a, b) }

let (x, y) = pairUp(1, "one")
println(x + " = " + y)   // => 1 = one
```

A generic type and generic functions over it compose into reusable data structures. Here is a small generic
stack built on top of a `Vec`:

```rust
struct Stack[T] { items: Vec[T] }

fn newStack[T]() -> Stack[T] { Stack { items: vec() } }
fn spush[T](s: Stack[T], x: T) -> () { push(s.items, x) }
fn spop[T](s: Stack[T]) -> Option[T] { pop(s.items) }   // Option: the stack may be empty

let s: Stack[Int] = newStack()
spush(s, 10)
spush(s, 20)
println("popped=" + toString(spop(s)))    // => popped=Some(20)
println("size="   + len(s.items))         // => size=1
```

Two things to keep in mind. First, generics are **first-order**: a type parameter stands for an ordinary type
like `Int` or `String`, not for a type constructor. Second, because types are erased at run time, there is a
single compiled body per generic function that serves all its uses — you pay nothing at run time for
genericity.

To require that a type parameter supports certain operations, add a **bound** — but bounds are expressed with
traits, so we turn to those next.

---

## 16. Traits

A **trait** is a named set of operations that a type can implement. This is Skarn's mechanism for
polymorphism, and it deserves a careful look because it works differently from the interfaces you may be used
to.

### Calling things — the forms at a glance

By this point you have seen a value passed to a function in a few different ways. Before traits add one more,
here is the whole picture in one place — there are **five** call forms, and which ones apply depends on
*what* you are calling:

| Form | Example | What it can call |
|------|---------|------------------|
| **free function** | `area(shape)`, `len(xs)` | any free function or builtin |
| **pipe** | `shape \|> area`, `range(0, n) \|> map(f) \|> sum` | threads a value into *any* free function, left-to-right |
| **method** | `shape.area()`, `rng.nextInt(1, 7)` | a type's own methods — an **inherent** method or a **trait** method |
| **qualified trait** | `Shape::area(x)` | one specific trait's method, when you need to name it explicitly |
| **qualified type** | `Point::sum(p)`, `Point::new(1, 2)` | one specific type's **inherent** method, likewise — and the only form for an **associated function** (one without `self`) |

The rule of thumb is short:

- **free-function and pipe forms reach *free functions*.** `f(x)` and `x |> f` are the same call; the pipe just
  reads left-to-right and chains. Standalone operations like `map`/`filter`/`sum`/`len` are free functions, so
  you write `xs |> map(f)`, never `xs.map(f)`.
- **the `.` form reaches a *type's methods*.** Those come from an `impl` block — either a **trait** `impl`
  (a shared contract, this section) or an **inherent** `impl` (a type's own API, later in this section).
- **a trait method accepts all three of free / pipe / `.`** — `area(s)`, `s |> area`, and `s.area()` are the
  exact same call. An **inherent** method is not a free function, so `area(s)` does *not* reach one — it is
  written `s.area()` or, qualified, `Point::area(s)`. The two qualified forms are the escape hatches for
  naming one specific trait or one specific type when the plain form would be ambiguous or unavailable.

So the receiver-centric std types read as methods (`rng.nextInt(1, 7)`, `dt.toIso()`, `conn.send(bytes)?`,
`json.getField("id")`), while pipelines of standalone verbs stay pipes (`range(0, n) |> filter(even) |> collect`). The rest
of this section fills in the two `impl` kinds behind the method form; the details of resolution order and the
`.`-vs-`|>` split are collected under *Method-call syntax and inherent methods* below.

### Defining and implementing a trait

You declare a trait with the operations it requires, then provide an `impl` block for each type that supports
it.

```rust
trait Shape {
    fn area(self) -> Double
}

struct Circle { r: Double }
struct Square { s: Double }

impl Shape for Circle {
    fn area(self) -> Double { 3.14159 * self.r * self.r }
}
impl Shape for Square {
    fn area(self) -> Double { self.s * self.s }
}
```

### Calling a trait method

A trait method can be called two **equivalent** ways: as an ordinary function with the receiver as the first
argument — `area(shape)` — or with method syntax — `shape.area()`. Either way, **dispatch is on the receiver
(the first argument)**: at run time Skarn looks at its actual type and calls the matching implementation. Here
is one thing that sets Skarn apart from a typical object-oriented language: the free-function form is
first-class, not an afterthought — so it also composes with the pipe, `shape |> area`. (The full story on
`.`-syntax, and on *inherent* methods a type defines for itself, is under
[Method-call syntax and inherent methods](#method-call-syntax-and-inherent-methods).)

```rust
println("circle=" + area(Circle { r: 2.0 }))   // free-function form => circle=12.56636
let sq = Square { s: 3.0 }
println("square=" + sq.area())                 // method form (equivalent) => square=9.0
```

The receiver parameter is named `self`, and its type is the type being implemented. Inside the method you read
its fields as `self.field`.

### Default methods

A trait can provide a default implementation for a method in terms of the others. Implementers get it for free
and may override it.

```rust
trait Animal {
    fn sound(self) -> String
    fn speak(self) -> String { "It says " + sound(self) }   // default
}

struct Duck {}
impl Animal for Duck {
    fn sound(self) -> String { "quack" }
    // `speak` is inherited from the default
}

println(speak(Duck {}))   // => It says quack
```

### Supertraits

A trait can require another trait. `Describable: Named` means "anything that is `Describable` must also be
`Named`", so a `Describable` method may call the `Named` methods.

```rust
trait Named {
    fn label(self) -> String
}
trait Describable: Named {
    fn describe(self) -> String { "This is " + label(self) }
}

struct Widget { id: Int }
impl Named for Widget {
    fn label(self) -> String { "widget#" + self.id }
}
impl Describable for Widget {}

println(describe(Widget { id: 7 }))   // => This is widget#7
```

### Trait bounds on generics

A trait can constrain a generic type parameter with `T: TraitName`. Inside the function you may then use that
trait's methods on values of type `T`:

```rust
fn totalArea[T: Shape](a: T, b: T) -> Double {
    area(a) + area(b)
}

println("total=" + totalArea(Circle { r: 1.0 }, Circle { r: 2.0 }))
// => total=15.70795
```

The bound lets a generic function work uniformly over any implementer while calling the trait's methods on
values of the abstract type `T`:

```rust
trait Show {
    fn show(self) -> String
}
struct Widget { id: Int }
impl Show for Widget {
    fn show(self) -> String { "widget#" + self.id }
}

fn showAll[T: Show](xs: Vec[T]) -> String {
    let mut acc = ""
    for x in xs {
        acc = acc + show(x) + " "   // `show` is available because of the `T: Show` bound
    }
    acc
}

let ws: Vec[Widget] = vec()
push(ws, Widget { id: 1 })
push(ws, Widget { id: 2 })
println(showAll(ws))   // => widget#1 widget#2
```

Because `T` is a single type parameter, `showAll` requires a `Vec[Widget]` — every element the same type. When
you genuinely want a mixed collection, use a trait object; see [§17](#17-trait-objects-dyn-trait).

### Multiple bounds

A parameter can require several traits at once, joined with `+`. Inside the function every method from every
named trait is available on `T`:

```rust
fn atMost[T: Ord + Eq](a: T, b: T) -> Bool {
    lessThan(a, b) || a == b   // `lessThan` comes from Ord, `==` from Eq
}
// Why both bounds? A total order *could* derive `==` from `lessThan`, but `Eq` and `Ord` are
// independent traits here — a type can be `Eq` (auto-derived) without an `impl Ord` — so a function
// that uses a method from each genuinely needs both named.
println(atMost(3, 5))   // => true
println(atMost(5, 5))   // => true
println(atMost(7, 5))   // => false
```

Here `Ord` and `Eq` are built-in (`lessThan` and structural `==`); the same `+` syntax works with your own
traits — `[T: Show + Ord]`, and so on.

You can also **implement the built-in traits for your own types**, not just your own traits — and `Ord` is the
one you will reach for most often, because it unlocks `sorted` / `sort` / `min` / `max` on a type of your own.
Just write `impl Ord for YourType { fn lessThan(self, other: YourType) -> Bool { … } }`; there is a worked
example under [§19 sorting](#19-iterators). (`Clone` is the other user-implementable built-in; `Eq` and
`Hashable` are sealed markers you never implement — see [§23](#built-in-traits-at-a-glance).)

### Mutating the receiver: `mut self`

By default a method only reads its receiver. To let a method modify the receiver in place — and have the caller
see the change — declare the receiver as `mut self`. This is how stateful objects (counters, cursors) work.

```rust
trait Tick {
    fn tick(mut self) -> Int
}

struct Ctr { n: Int }
impl Tick for Ctr {
    fn tick(mut self) -> Int {
        self.n = self.n + 1
        self.n
    }
}

let ctr = Ctr { n: 0 }
println("ticks=" + (tick(ctr) + tick(ctr) + tick(ctr)))   // => ticks=6  (1 + 2 + 3)
```

### How this differs from an object-oriented interface

If your mental model of polymorphism comes from classes and interfaces, a few points are worth making explicit
— not as criticism of that model, just so the differences do not trip you up:

- **A type does not "belong to" a trait, and there is no class hierarchy.** A `struct` is a plain record of
  fields. It declares no interfaces. Instead, a trait implementation is a separate `impl` block that can live
  anywhere and be added *after the fact* — you can implement a trait you define for a type someone else
  defined. Polymorphism is a relationship you attach, not an inheritance you are born with.
- **There is no subclassing and no method overriding across a hierarchy.** Supertraits (`A: B`) express
  requirements between traits, not an "is-a" chain between concrete types. There is no `super`, no protected
  members, no diamond problem.
- **Dispatch is on the first argument, and methods are free functions.** `draw(shape)` selects an
  implementation by the runtime type of `shape`. There is no hidden `this`; the receiver is just the explicit
  first parameter, `self`.
- **The set of implementations is closed and checked.** The compiler knows every `impl` in the program. There
  is a coherence rule (informally, the "orphan rule"): to write `impl SomeTrait for SomeType`, either the trait
  or the type must be defined in the same module. This prevents two modules from giving the same type
  conflicting implementations of the same trait.

The practical upshot: traits let you describe *capabilities* ("this type can be measured", "this type can be
shown as text") and attach them to types independently, rather than baking a fixed interface list into a class
definition.

### How "erased" and "dispatch on the runtime type" fit together

These two phrases sound contradictory — the overview says types are *erased*, this section says dispatch picks
an implementation *by the runtime type of the receiver*. Both are true, because they talk about different
things:

- **What is erased** are the *generic type parameters*. A generic function like `fn largest[T: Ord](xs: …)` is
  compiled to **one** body, shared by every `T`. That body carries no hidden dictionary, no witness table, and
  no per-`T` copy; `T` simply does not exist at run time, and there is no reflection to ask "what is `T`?".
- **What dispatch uses** is not `T` but the *value's own type*, which every value carries intrinsically anyway
  (an integer, a string, and a `Point` are distinguishable at run time regardless of any generic). A trait call
  reads that coarse tag and looks the implementation up in a global, compile-time-built table. When the
  concrete type is already known at the call site, the compiler skips the lookup and calls the implementation
  directly.

So there is no boxing and no vtable pointer travelling alongside your values — dispatch is a table lookup keyed
on a tag the value already has. This is also **why `dyn Trait` is free** (a `dyn Show` *is* the plain value;
see [§17](#17-trait-objects-dyn-trait)).

The same mechanism explains one real limitation. The **erasure types** — `transparent struct`s, integer-backed
`enum`s, and `Char` — deliberately *share their runtime representation* with the primitive they wrap (a
`UserId(Int)` is an `Int` at run time, carrying `Int`'s tag). Dispatch keys on that tag, so it cannot tell a
`UserId` from an `Int` — which is exactly why **erasure types may not implement traits, and cannot be used
through a bound on a method-carrying trait** (`Show`, `Ord`, …) in generic code — a deliberate trade for their
zero-cost representation. If you need a wrapper type that participates in dispatched traits, use a normal
`struct` with a single field instead of a `transparent` one.

The exception is the **compile-time marker traits** `Eq` and `Hashable` (see [§5](#5-operators) and
[§14](#14-collections)): these carry no methods and dispatch *nothing* — the bound is discharged statically by the
checker — so an erasure type satisfies them like any other value. `UserId` *is* `Eq` and `Hashable`
(`fn index[K: Hashable](…)` and `fn allEq[T: Eq](…)` accept it), it just cannot implement a *method* trait.
`Ord` is a method trait (`lessThan`), so it is **not** among the exceptions — an erasure type is not `Ord`.

### Method-call syntax and inherent methods

Any method can be called with **dot syntax** on its receiver — `x.method(args)` — as an alternative to the
free-function / pipe forms. This works for trait methods:

```rust
let a = area(shape)      // free-function form
let a = shape |> area    // pipe form
let a = shape.area()     // method form -- all three call the same trait method
```

For a type's **own** operations that are not a shared contract, write a traitless **inherent `impl`** block —
it introduces the type parameters once and gives the type a first-class API:

```rust
struct Rect { w: Int, h: Int }

impl Rect {
    fn new(w: Int, h: Int) -> Self { Rect { w: w, h: h } }   // no self -> an associated function
    fn square(side: Int) -> Self { Rect::new(side, side) }
    fn area(self) -> Int { self.w * self.h }
    fn scaled(self, k: Int) -> Rect { Rect { w: self.w * k, h: self.h * k } }
}

let r = Rect::new(3, 4)
println(r.area())                 // => 12
println(r.scaled(2).area())       // => 48   (methods chain left-to-right)
println(Rect::area(r))            // => 12   the same call, named explicitly
println(r |> Rect::scaled(2) |> Rect::area)   // => 48   and so it fits a pipe
println(Rect::square(5).area())   // => 25
```

A member **without** `self` is an **associated function** — a constructor or factory that belongs to the type
rather than to a value of it. It is written `Rect::new(3, 4)`, never `r.new(…)`: there is no receiver, so there
is nothing for `.` to dispatch on. `Self` names the impl target, in the signature as well as in the body.

Associated functions are how a library keeps its constructors out of the global namespace. Skarn has no
overloading and no `import … as`, so a free `fn parse(…)` claims that name for every program that imports the
module; `Doc::parse(…)` does not. The standard library uses this where the pressure was real — `Json::parse`,
`Regex::compile`, `CliArgs::parse`, `Set::new`, `ByteReader::new`, `Rng::fromSeed` — and it is what lets
`std::json` and `std::cli` *both* call their parser `parse`. Where a free name reads better it stays free:
`seconds(30)` beats `Duration::seconds(30)`, so `std::time` keeps its constructors, as do `std::net` and
`stringBuilder`.

Generic types and `mut self` work as expected:

```rust
struct Stack[T] { items: Vec[T] }

impl[T] Stack[T] {
    fn push(mut self, x: T) -> () { push(self.items, x) }   // mut self -> caller-visible
    fn size(self) -> Int { len(self.items) }
}
```

Rules and boundaries:

- **Inherent methods are not free functions** — `r.area()`, not `area(r)` or `r |> area`. (Trait methods keep
  all three forms.) The qualified `Rect::area(r)` is the exception, described at the end of this list. An
  inherent method may only be added to a type defined in the same module.
- **Resolution order for `x.name(...)`** is: a struct **field** named `name` (so a function-valued field stays
  callable), then an **inherent** method, then a **trait** method. An inherent method therefore shadows a
  same-named trait method for `.`; the trait method is still reachable as `Trait::name(x)`. A **field** in
  turn shadows an inherent method — and since a field is not callable, that method is then reachable *only*
  as `Type::name(x)`. An **associated function** takes no part in this: it has no receiver, so it never
  shadows anything for `.` and is reached only as `Type::name(...)`.
- **An associated function on a generic type needs its type argument fixed** by an argument, by an annotation,
  or by the enclosing return type — there is no receiver to solve it from and no syntax to name it
  explicitly. `Bag::of(1)` and `let b: Bag[Int] = Bag::empty()` are fine; a bare `let b = Bag::empty()` is
  rejected as *cannot infer the type argument*.
- **`.` and `|>` are complements, not rivals.** `.method()` reaches a type's methods; the pipe threads a value
  into *any* free function or builtin, which is what the lazy-iterator pipelines use:
  `xs |> intoIter |> map(f) |> filter(p) |> sum`. There is no `xs.map(f)` — `map`/`filter`/… are free
  functions that take a `dyn Iterator` (lift a container with `intoIter` first).
- **This is exactly why the standard library reads in two styles, and the split is principled, not drift.** The
  built-in collections — `Vec`, `Map`, `String`, `Bytes` — are not defined in any user module, and the orphan
  rule forbids adding an inherent `impl` to a type you do not own, so their verbs *must* be free functions:
  `len(xs)`, `push(xs, v)`, `map`/`filter`/`sum`, all used with `|>`. The library types that Skarn *does* define
  — `Json`, `Rng`, `Set`, `DateTime`, a network `Conn` — own their behavior, so they carry it as `.` methods:
  `j.stringify()`, `rng.nextInt(1, 6)`, `s.insert(x)`, `conn.send(bytes)`. So "collections use free functions"
  and "`std::json`/`std::random`/… use methods" are the same rule seen from both sides — free functions for
  types the library cannot extend, methods for the ones it defines.
- **`Type::method(x)` names an inherent method explicitly**, mirroring `Trait::method(x)`. `p.sum()` and
  `Point::sum(p)` are the same call, and the qualified form also works in a pipe (`p |> Point::sum`). It
  matters most in the one case where `.` cannot reach the method at all: resolution is **field-first**, so a
  struct with a field named like one of its methods hides that method from `p.value()` — `Counter::value(c)`
  is then the only way in. If a trait *and* a type share a name and both declare the method, the qualified
  call is rejected as ambiguous rather than silently resolved. Like `Trait::method`, it must be **called** —
  `let f = Point::sum` is not (yet) a value. Turning *every* free function into a `x.f()` call (full UFCS)
  remains a deliberate non-goal — it would make `|>` redundant.

### Blanket and parametric impls

Two heavier forms of `impl` round out the trait system.

A **blanket impl** implements a trait for *every* type that already satisfies some bound — write the impl over
a type parameter instead of a concrete head. Here everything that is `Named` gets `Greet` for free:

```rust
trait Named { fn name(self) -> String }
struct Dog {}
impl Named for Dog { fn name(self) -> String { "Rex" } }

trait Greet { fn greet(self) -> String }
impl[T: Named] Greet for T {                 // blanket: for all T: Named
    fn greet(self) -> String { "Hi, " + self.name() }
}

println(Dog {}.greet())     // => Hi, Rex
```

A concrete `impl` always wins over a blanket one for the same trait; there is at most one blanket impl per
trait.

A **parametric trait** carries its own type parameter, and the implementing type supplies it — the parameter is
an *output* of the impl, solved from the head:

```rust
trait FirstOf[T] { fn firstOf(self) -> Option[T] }

impl[T] FirstOf[T] for Vec[T] {
    fn firstOf(self) -> Option[T] {
        if len(self) > 0 { Some(self[0]) } else { None }
    }
}

let mut xs: Vec[Int] = vec()
push(xs, 10)  push(xs, 20)
println(toString(xs.firstOf()))   // => Some(10)
```

This is how `Iterator[T]` / `IntoIterator[T]` (see [§19](#19-iterators)) are defined: the element type is
determined by the impl, one impl per (trait, implementing type). There is at most one impl of a given
parametric trait per type; overlapping or multi-parameter dispatch is not supported.

---

## 17. Trait objects: `dyn Trait`

A generic function with a bound, `fn f[T: Shape](x: T)`, is resolved once per call site: the caller picks one
concrete `T`. That is exactly what you want for uniform code, but it means a `Vec[T]` is **homogeneous** — every
element is the same type.

Sometimes you need the opposite: a collection where **each element chooses and hides its own type**, as long as
they all implement a common trait. That is what `dyn Trait` gives you. `dyn Shape` is a real type — "some type
that implements `Shape`" — and you can put different concrete types into the same `Vec[dyn Shape]`.

```rust
trait Show {
    fn show(self) -> String
}

struct Dog { name: String }
struct Cat { lives: Int }

impl Show for Dog { fn show(self) -> String { "Dog " + self.name } }
impl Show for Cat { fn show(self) -> String { "Cat with " + self.lives + " lives" } }

let zoo: Vec[dyn Show] = vec()
push(zoo, Dog { name: "Rex" })    // a Dog, seen as `dyn Show`
push(zoo, Cat { lives: 9 })       // a Cat, seen as `dyn Show`

for animal in zoo {
    println("  " + show(animal))  // dispatches to each element's real type
}
// =>   Dog Rex
// =>   Cat with 9 lives
```

A concrete value flows into a `dyn` slot automatically (a `Dog` *is* a `dyn Show`). You can store a `dyn` value
in a field, pass it as a parameter, or return it:

```rust
struct Labeled {
    tag: String,
    item: dyn Show
}

let boxed = Labeled { tag: "pet", item: Dog { name: "Fido" } }
println(boxed.tag + " = " + show(boxed.item))   // => pet = Dog Fido
```

### Object safety

Not every trait can be used as `dyn Trait`. The rule is simple and has one important consequence:

- Every method must take `self` (dispatch reads the receiver from the first argument, so there must be a
  receiver).
- `self`'s type may not appear in any *other* parameter. This is the load-bearing rule: two different `dyn Show`
  values may hide two different concrete types, so a method like `fn equals(self, other: Self) -> Bool` could be
  handed a wrong-typed second argument. Traits that compare two values of the same type therefore cannot be used
  as trait objects.
- No method may **return `Self`**, and no method may be **generic** (have its own type parameters) — both are v1
  restrictions, not fundamental. So `Clone` (`fn clone(self) -> Self`) is *not* object-safe: `dyn Clone` is
  rejected (what concrete type would `clone` on it return?).

### When to use which

- Reach for a **generic bound `T: Trait`** when the caller knows the concrete type and every value in a given
  use is the same type. It is the default, and it is the cheapest (calls can often be resolved statically).
- Reach for **`dyn Trait`** when you need to mix different concrete types behind a common capability — a
  heterogeneous list, a plugin registry, a field that could hold any implementer.

Compared with the OO contrast in the previous section: `dyn Trait` is the closest thing Skarn has to "a
variable of interface type holding any implementing object". The difference is that the value carries its own
type (there is no separate wrapper object), and the trait must be object-safe.

---

## 18. Option, Result, and the `?` operator

Skarn has no `null`. A value that might be absent has type `Option[T]`, which is an enum with two variants:
`Some(value)` or `None`. A computation that might fail has type `Result[T, E]`: `Ok(value)` or `Err(error)`.
Because these are ordinary enums, you handle them with `match` — but there is also convenient sugar.

### Option

```rust
fn half(n: Int) -> Option[Int] {
    if n % 2 == 0 { Some(n / 2) } else { None }
}

println(toString(half(10)))   // => Some(5)
println(toString(half(7)))    // => None

// a default when absent:
println("or=" + unwrapOr(half(7), -1))   // => or=-1
```

### Result

```rust
fn safeDiv(a: Int, b: Int) -> Result[Int, String] {
    if b == 0 { Err("divide by zero") }
    else      { Ok(a / b) }
}

match safeDiv(10, 2) {
    Ok(v)  => println("ok " + v),      // => ok 5
    Err(e) => println("err " + e)
}
```

### The `?` operator

Chaining fallible steps with `match` at every level is tedious. The postfix `?` operator does it for you: on a
`Some`/`Ok` it unwraps the value and continues; on a `None`/`Err` it immediately returns that from the
enclosing function. The enclosing function's return type must itself be an `Option`/`Result`.

```rust
fn quarter(n: Int) -> Option[Int] {
    let h = half(n)?    // if None, `quarter` returns None right here
    half(h)
}
println(toString(quarter(20)))   // => Some(5)
println(toString(quarter(6)))    // => None   (6 -> 3, and half(3) is None)

fn pipeline() -> Result[Int, String] {
    let x = safeDiv(100, 5)?     // x = 20
    let y = safeDiv(x, 2)?       // y = 10
    Ok(y)
}
match pipeline() {
    Ok(v)  => println("ok " + v),    // => ok 10
    Err(e) => println("err " + e)
}
```

### Combinators

Often you want to transform the value inside an `Option`/`Result` without writing a `match`. The combinators do
that: `optionMap`/`resultMap` transform the contained value; `optionAndThen`/`resultAndThen` chain another
fallible step; `mapErr` transforms an error; `unwrapOr` supplies a default; `okOr` turns an `Option` into a
`Result`; `ok` turns a `Result` into an `Option`.

```rust
let mapped = optionMap(Some(10), fn(x: Int) -> Int { x * 3 })
println("map=" + toString(mapped))    // => map=Some(30)

let chained = optionAndThen(Some(4), fn(x: Int) -> Option[Int] {
    if x > 0 { Some(x + 1) } else { None }
})
println("resultAndThen=" + toString(chained))   // => resultAndThen=Some(5)

let r: Result[Int, String] = Ok(21)
println("res=" + toString(resultMap(r, fn(x: Int) -> Int { x * 2 })))   // => res=Ok(42)
```

(`Some`/`Ok` are distinct combinator names for `Option` and `Result` where a single name would be ambiguous —
hence `optionMap` vs `resultMap`.)

`unwrap(x)` and `expect(x, msg)` extract the value but **abort** on `None`/`Err`, so use them only when absence
truly cannot happen.

The compiler helps you not to ignore these: discarding a `Result` or `Option` produces an advisory warning,
because the whole point of returning one is to make you handle it.

### `panic` and `assert`

When a situation is genuinely unrecoverable, `panic(msg)` aborts with a located error message. `assert(cond,
msg)` panics if `cond` is false. These are for programmer errors and broken invariants — not for expected
failures, which belong in a `Result`.

Because `panic` never returns, its type is **`Never`** — the type with no values, which the checker treats as
compatible with every type. That is what lets a `panic` sit in a `match` arm beside arms that produce a real
value (`match … { Some(v) => v, None => panic("missing") }`): the panicking arm contributes no type of its own.
A `loop` with no `break` has type `Never` for the same reason. You rarely write `Never` yourself; it is mostly
something the checker infers.

```rust
fn mustBePositive(n: Int) -> Int {
    assert(n > 0, "expected a positive number")
    n
}
println(mustBePositive(5))   // => 5
```

---

## 19. Iterators

An **iterator** produces a sequence of values one at a time, on demand. Iterators are **lazy**: building a
pipeline does no work; the work happens only as a consumer pulls values. This lets you express computations
over sequences — even infinite ones — cleanly and without building intermediate collections.

### Sources

You obtain an iterator from a source. The most common is `range(lo, hi)` (the integers `lo` up to but not
including `hi`). Others include `repeat(x)` (the same value forever), `iterate(seed, f)` (`seed`, `f(seed)`,
`f(f(seed))`, ...), `once(x)`, and `empty()`. Sources like `repeat` and `iterate` are infinite; because
iterators are lazy, you bound them with a stage like `take`:

```rust
let powers = iterate(1, fn(x: Int) -> Int { x * 2 }) |> take(5) |> collect
println("powers=" + toString(powers))   // => powers=[1, 2, 4, 8, 16]

let threes = repeat(3) |> take(4) |> collect
println("threes=" + toString(threes))   // => threes=[3, 3, 3, 3]
```

### Consuming with `for`

Any iterator can drive a `for` loop:

```rust
let mut sum = 0
for x in range(0, 5) {
    sum = sum + x
}
println("sum=" + sum)   // => sum=10
```

### Stages: transforming a pipeline

Stages take an iterator and return a new iterator, so you compose them. The element-wise stages are lazy and
build no intermediate vectors — the two exceptions are `chunks` and `windows`, which necessarily materialize a
`Vec` per pull (see their cost note in [§25](#25-the-memory--cost-model)).

- `map(it, f)` — apply `f` to each element
- `filter(it, p)` — keep elements where `p` is true
- `take(it, n)` / `drop(it, n)` — first `n` / all but the first `n`
- `takeWhile(it, p)` / `dropWhile(it, p)` — while a predicate holds
- `enumerate(it)` — pair each element with its index, `(Int, T)`
- `zip(a, b)` — pair up two iterators, stopping at the shorter
- `chain(a, b)` — one iterator after another
- `scan(it, init, f)` — running accumulator
- `flatMap(it, f)` — map each element to an iterator and flatten
- `stepBy(it, n)` — the first element, then every `n`-th thereafter
- `inspect(it, f)` — run `f` on each element as it passes (a debug tap), yielding it unchanged
- `dedup(it)` — collapse *consecutive* equal elements (the element type must be `Eq`)
- `chunks(it, n)` — non-overlapping blocks of up to `n`, each a `Vec[T]` (the last may be shorter)
- `windows(it, n)` — overlapping sliding views of exactly `n`, each a `Vec[T]` (fewer than `n` elements yields nothing)
- `peekable(it)` — a cursor whose `.peek()` method looks at the next element without consuming it

Each stage takes its iterator as the *first* argument, which is exactly the shape the pipe operator (`|>`, from
[§5](#5-operators)) was made for. Writing a pipeline with `|>` reads top to bottom in the order the data flows —
source, then each stage, then a terminal — instead of inside-out:

```rust
let s =
    range(0, 10)
    |> filter(fn(x) { x % 2 == 0 })
    |> map(fn(x) { x * x })
    |> sum
println("pipeline=" + s)   // => pipeline=120   (0 + 4 + 16 + 36 + 64)
```

The lambdas here carry no type annotations — `filter` and `map` fix the parameter and result types, so they are
inferred (see [§10](#10-closures-and-function-values)). That is the same computation as the nested
`sum(map(filter(range(0, 10), ...), ...))`, just easier to read. The rest of this section uses `|>` wherever a
value flows through a chain, and spells out the lambda types where it helps document what each combinator expects
— but you can drop them anywhere the combinator's signature already fixes the type.

### Terminals: producing a result

A terminal drives the iterator to completion and produces a value:

- `sum` / `product` — numeric fold
- `count` — number of elements
- `collect` — gather into a `Vec`
- `fold(it, init, f)` / `reduce(it, f)` — general accumulation
- `forEach(it, f)` — run `f` for its side effect
- `any` / `all` / `find` / `position` / `contains`
- `min` / `max` / `minBy` / `maxBy`
- `partition` (split by a predicate) / `unzip` (split pairs) / `join` (concatenate strings) / `toMap`

```rust
println("collect=" + toString(collect(range(0, 5))))   // => collect=[0, 1, 2, 3, 4]
println("count="   + count(range(0, 100)))             // => count=100
println("max="     + toString(max(range(3, 9))))       // => max=Some(8)

let digits = range(1, 4) |> map(fn(x: Int) -> String { toString(x) }) |> join("-")
println("join=" + digits)                              // => join=1-2-3
```

`fold` accumulates with an explicit seed; `reduce` uses the first element as the seed (so it returns an
`Option`, since an empty iterator has nothing to start from). `partition` splits into two vectors by a
predicate, and `toMap` gathers a sequence of `(key, value)` pairs into a `Map`:

```rust
println("fold="   + fold(range(1, 5), 100, fn(a: Int, x: Int) -> Int { a + x }))   // => fold=110
println("reduce=" + toString(reduce(range(1, 5), fn(a: Int, b: Int) -> Int { a * b })))  // => reduce=Some(24)

let (evens, odds) = partition(range(0, 10), fn(x: Int) -> Bool { x % 2 == 0 })
println("split=" + len(evens) + "/" + len(odds))   // => split=5/5

let squares = toMap(map(range(1, 4), fn(x: Int) -> (Int, Int) { (x, x * x) }))
println("sq[3]=" + squares[3])   // => sq[3]=9
```

`min` and `max` work on any `Ord` element type — the built-in `Int` / `Double` / `String`, or your own type
once you `impl Ord` for it — returning an `Option` because the sequence might be empty. When you want to order by
a custom rule (or order a `Char`, which is *not* `Ord`), `minBy`/`maxBy` take a comparison function:

```rust
let words = ["pear", "apple", "kiwi"]
println("min=" + unwrapOr(min(intoIter(words)), "?"))   // => min=apple

let biggest = maxBy(range(1, 5), fn(a: Int, b: Int) -> Bool { a < b })
println("maxBy=" + toString(biggest))   // => maxBy=Some(4)
```

To order a whole collection, `sorted(v)` returns a **new**, sorted `Vec` (leaving `v` untouched), `sort(v)`
sorts a `mut` vector **in place**, and `sortBy(v, less)` orders by an explicit comparator. All three are a
**stable** merge sort. There are **two ways to give a type an order**. The primary one is to make it `Ord`
(built in for `Int` / `Double` / `String`; add it to **your own `struct` / `enum`** with `impl Ord`, below) —
then `sorted` / `sort` / `min` / `max` just work on it. The other is `sortBy` with a comparator, for a **one-off
ordering** or for a type that cannot be `Ord` at all — a `Char` or the other erasure types, even though a `Char`
*can* be compared with `<` (see the `Ord` note in [§5](#5-operators)).

```rust
let names = sorted(toVec(["cherry", "apple", "banana"]))
println(toString(names))                          // => ["apple", "banana", "cherry"]

// give your OWN type an order once, with impl Ord, then `sorted` works on it:
struct Person { name: String, age: Int }
impl Ord for Person { fn lessThan(self, o: Person) -> Bool { self.age < o.age } }
let byAge = sorted(toVec([Person { name: "Ann", age: 30 }, Person { name: "Bo", age: 20 }]))
println(byAge[0].name)                            // => Bo

// sortBy is for a one-off ordering (no Ord needed) — here, by length:
let byLen = sortBy(toVec(["ccc", "a", "bb"]), fn(a: String, b: String) -> Bool { len(a) < len(b) })
println(toString(byLen))                          // => ["a", "bb", "ccc"]

let mut ns = toVec([3, 1, 2])
sort(ns)                                          // sorts ns in place
println(toString(ns))                             // => [1, 2, 3]
```

`enumerate` and `zip` yield tuples, which pair beautifully with a destructuring `for`:

```rust
for (i, x) in enumerate(zip(range(0, 3), range(10, 13))) {
    println("  #" + i + " -> " + x.0 + "/" + x.1)
}
// =>   #0 -> 0/10
// =>   #1 -> 1/11
// =>   #2 -> 2/12
```

This reads beautifully but is not free: each step allocates the tuple(s) *and* the iterator's `Option` — a
tuple-yielding combinator is the most allocation-heavy *element-wise* loop shape (only `chunks`/`windows`, which
build a whole `Vec` per pull, are heavier still). In a hot loop prefer a plain index, or the zero-allocation
`for (k, v) in m` (over a map) and direct `for x in xs` (over a container), which bypass both. See
[§25](#25-the-memory--cost-model) for the full cost model.

### Writing your own iterator

An iterator is just a type that implements the `Iterator` trait: a single method `next(mut self) ->
Option[T]` that returns the next element (`Some`) or signals the end (`None`), advancing its own state. Here is
an infinite Fibonacci iterator, bounded with `take`:

```rust
struct Fib { a: Int, b: Int }

impl Iterator[Int] for Fib {
    fn next(mut self) -> Option[Int] {
        let x = self.a
        self.a = self.b
        self.b = x + self.b
        Some(x)
    }
}

let mut fibs: Vec[Int] = vec()
for x in take(Fib { a: 0, b: 1 }, 8) {
    push(fibs, x)
}
println("fibs=" + toString(fibs))   // => fibs=[0, 1, 1, 2, 3, 5, 8, 13]
```

A user `Iterator` like `Fib` is **single-use** — driving it consumes it. If you want a value that can be
iterated *repeatedly*, implement `IntoIterator` instead: its `intoIter` method hands back a **fresh** cursor
each time, so every `for` over it starts from the beginning. This is exactly why `for x in someVec` works and
can be written twice.

```rust
struct Countdown { from: Int }
struct CountCur  { i: Int }

impl Iterator[Int] for CountCur {
    fn next(mut self) -> Option[Int] {
        if self.i <= 0 { None }
        else { let x = self.i  self.i = self.i - 1  Some(x) }
    }
}
impl IntoIterator[Int] for Countdown {
    fn intoIter(self) -> dyn Iterator[Int] { CountCur { i: self.from } }
}

let cd = Countdown { from: 3 }
let mut out: Vec[Int] = vec()
for x in cd { push(out, x) }
for x in cd { push(out, x) }        // a fresh cursor -> runs again from 3
println("countdown=" + toString(out))   // => countdown=[3, 2, 1, 3, 2, 1]
```

When you want an eager snapshot instead of a lazy pipeline, `toVec(x)` materializes any iterable into a fresh,
owned `Vec`:

```rust
println("toVec=" + toString(toVec([7, 8, 9])))   // => toVec=[7, 8, 9]
```

### `Iterable`: the eager bridge

`Iterator` and `IntoIterator` are the *lazy* protocols — they pull one element at a time. There is a third,
simpler protocol, **`Iterable[T]`**, for a type that can hand over **all** of its elements at once. Its single
method is `iter(self) -> Vec[T]`, which **by contract returns a fresh `Vec` the caller owns** (never an alias
to internal storage). Implementing it is what lets `toVec` (and `for`, as a fallback after
`Iterator`/`IntoIterator`) work on your type:

```rust
struct Bag { items: Vec[Int] }
impl Iterable[Int] for Bag {
    fn iter(self) -> Vec[Int] { clone(self.items) }   // hand back a COPY, not the internal Vec
}

let b = Bag { items: toVec([3, 1, 2]) }
println("snapshot=" + toString(toVec(b)))    // => snapshot=[3, 1, 2]   (goes through your `iter`)
for x in b { println("  x=" + x) }           // `for` also falls back to `iter`
```

`toVec` hands your `iter` result straight back, so `iter`'s freshness contract is what makes `toVec(x)` an
independent snapshot. Returning the internal `self.items` directly would hand callers an alias to the struct's
own storage (mutating the result would mutate the `Bag`), so `iter` must `clone` it, as above — every built-in
container already does. `Iterable` is the eager escape hatch: it materializes everything up front, so prefer
`Iterator` / `IntoIterator` when you want laziness. The built-in collections satisfy the lazy protocols
directly, which is why `for` and the combinators "just work" on a `Vec`, `Array`, `Map`, or `Bytes` without
any of this. If your own type is `Iterable`-only and you want a lazy pipeline over it, snapshot it first —
`intoIter(toVec(x)) |> map(…) |> …` — where the `toVec` copy is the one eager cost, made visible at the call
site. (Because `for x in b` over the `Iterable` fallback calls `iter` once per loop, it copies the elements
on **every** pass; a `Vec`/`Array`/… does not, since it iterates through the zero-copy `IntoIterator` cursor.)

### Streaming text: `split` and `lines`

Two string sources produce a `dyn Iterator[String]`, so all of the stages and terminals above apply to text.

`split(s, sep)` cuts a string into fields on a literal separator (which may be more than one byte). Every
separator produces a boundary, so `N` separators yield `N + 1` fields and empty fields are **kept** — that is
what makes `split` and `join` exact inverses:

```rust
println("fields=" + toString(collect(split("a,b,,c", ","))))   // => fields=["a", "b", "", "c"]
println("count="  + count(split("a,b,,c", ",")))               // => count=4

let s = "2026-07-18"
println("round=" + join(split(s, "-"), "-"))                   // => round=2026-07-18
```

An empty separator cannot split, so `split(s, "")` passes the input through unchanged: it yields the whole
string as a single field. (This keeps `join(split(s, sep), sep) == s` even when `sep` is empty.)

`lines(s)` splits text into lines using *terminator* semantics: a trailing newline does **not** create a
phantom empty final line, and a `\r` directly before each `\n` (Windows `CRLF`) is stripped. An interior blank
line is preserved, and `lines("")` yields no lines at all.

```rust
let doc = "alpha\r\nbeta\r\n\r\ndelta\r\n"
let mut n = 0
for line in lines(doc) {
    println("  line " + n + ": [" + line + "]")
    n = n + 1
}
println("lines=" + n)
// =>   line 0: [alpha]
// =>   line 1: [beta]
// =>   line 2: []
// =>   line 3: [delta]
// => lines=4
```

Because both return a lazy iterator, they compose with the rest of the toolkit. Here is a word count that
splits each line on spaces and drops the empty fields produced by runs of whitespace:

```rust
let text = "the quick brown\nfox jumps over\n\nthe lazy dog\n"
let mut words = 0
for ln in lines(text) {
    words = words + count(filter(split(ln, " "), fn(w: String) -> Bool { len(w) > 0 }))
}
println("words=" + words)   // => words=9
```

---

## 20. Modules

Each `.skn` file is a **module**. A module's top-level names (structs, enums, functions, traits) live in
that module's namespace; other files reach them by importing.

Given a module file `geo.skn`:

```rust
// geo.skn
pub struct Point { x: Int, y: Int }

pub fn origin() -> Point { Point { x: 0, y: 0 } }

pub fn manhattan(a: Point, b: Point) -> Int {
    let dx = a.x - b.x
    let dy = a.y - b.y
    let ax = if dx < 0 { -dx } else { dx }
    let ay = if dy < 0 { -dy } else { dy }
    ax + ay
}
```

another file uses it:

```rust
// main.skn
import geo             // load the module
use geo::manhattan     // bring `manhattan` into scope unqualified

let a = geo::Point { x: 3, y: 4 }   // reach a name with the `geo::` qualifier
let b = geo::origin()

println("distance = " + manhattan(a, b))   // => distance = 7
```

The forms you will use:

- `import a::b` — load a module (the path `a::b` maps to the file `a/b.skn` relative to the entry file).
- `use a::b::name` — bring one name into scope so you can use it unqualified.
- `use a::b::{x, Y}` — bring several names in at once.
- `use a::b::*` — bring in everything (a "glob"; a local definition or explicit `use` silently wins over it).
- `mod::name` — reach a name with a qualifier without a `use`.

Same-named types or functions in different modules coexist without conflict. The trait coherence rule from
[§16](#16-traits) applies across modules: an `impl Trait for Type` must live in the module that defines the
trait or the module that defines the type.

### Visibility: `pub`

Top-level items are **private to their module by default**. Mark an item `pub` to export it — only then can
another module reach it (by `use`, glob, or a `mod::name` qualifier):

```rust
// util.skn
pub fn add(a: Int, b: Int) -> Int { a + b }    // exported
fn helper() -> Int { 1 }                       // private to util
```

```rust
// main.skn
use util::add        // OK
println(toString(add(40, 2)))   // => 42
// use util::helper  // error: item 'helper' is private in module 'util'
```

`pub` goes on `fn` / `struct` / `enum` / `trait`. A struct's fields and an enum's variants inherit their
item's visibility (a `pub enum`'s constructors are public). Within a single module everything is visible, so
visibility only matters across module boundaries. A `use m::*` glob silently brings in only the `pub` names.

**The file you run is a module too, and it is the one nobody can import.** Its `import`s reach outward, but
nothing reaches back in — a module cannot see a name defined in the entry file, `pub` or not, because there is
no path that would name it. So the rule for a growing program is simply: whatever two files need to share,
put in a module.

### The standard library

The prelude is organized into `std` modules, split by how you reach them. The **ring** modules are
auto-imported into every module, so their names resolve **unqualified** with no `use`:

- `std::core` — `Option` / `Result` with their constructors and combinators, the `Unwrap` trait, the `Map`
  lookup helpers, and the sealed marker traits `Eq` and `Hashable`.
- `std::iter` — the `Iterator` / `IntoIterator` / `Iterable` traits and the whole lazy pipeline of
  [§19](#19-iterators) (sources, stages, terminals), the `Vec` sorts, `Ord`, and the opt-in code-point layer
  (`chars` / `charCount` / `nthChar`, [§12](#char-and-utf-8-code-points)).
- `std::string` — the byte-level `String` helpers (slicing, search, `trim`, case, padding, ASCII
  classification) and the `StringBuilder` accumulator.

Every other module is **opt-in**: its names do not resolve until you `use` it. That is a hard rule rather than
a convention, and it is also what **gates the natives** — a module that never writes `use std::io` cannot touch
the filesystem, and one without `use std::process` cannot start a program.

| Module | What it is for |
|---|---|
| `std::io` | files — read / write / append, copy, rename, delete, `mkdir`, `listDir`, stat — and standard input |
| `std::env` | command-line arguments, environment variables, and the clocks `nanoTime` / `millisTime` |
| `std::process` | run an external command and capture its output |
| `std::math` | roots, powers, logarithms, trigonometry, `gcd`/`lcm`, generic `minOf`/`maxOf`/`clamp`, `PI`/`E`, the fallible `toIntChecked`. (The rounding builtins `floor`/`ceil`/`trunc`/`round`/`roundHalfToEven`, `toInt` and `toDouble` are **always** available — no `use`.) |
| `std::bytes` | a little-endian binary reader/writer over `Bytes`, with varints and length-prefixed strings |
| `std::json` | a JSON parser and serializer over a `Json` value tree |
| `std::random` | a seedable PRNG (xoshiro128\*\*) — **deterministic** (same seed → same stream) and **not cryptographically secure**; it pulls no entropy of its own, so for an unpredictable seed pass one in (`use std::env`, then `Rng::fromSeed(nanoTime())`) |
| `std::set` | a hash `Set[T]` with the usual set algebra; `T` must be `Hashable`, and iteration is hash-order |
| `std::time` | strict-UTC dates and times at millisecond precision — `Instant`, `Duration`, `DateTime`, `Stopwatch`. An `Instant` spans roughly year −2490..6429; sub-millisecond precision and time zones are out of scope for v1 |
| `std::cli` | a spec-free command-line parser: `--name=value` options, `--name` / `-abc` flags, `--`, positionals |
| `std::hash` | CRC-32 and MurmurHash3 (fast, **not** secure) plus SHA-256 (cryptographic) and hex encoding |
| `std::net` | blocking TCP (IPv4 + IPv6) and a minimal HTTP/1.0 `httpGet` — one connection at a time, **plaintext only** (no TLS, so `http://` not `https://`) |
| `std::regex` | linear-time byte-level regular expressions (Thompson NFA / Pike VM) — no catastrophic backtracking, and therefore **no** backreferences or lookaround |

This table says only what each module is *for*. **Every function of every module, with its signature, is listed
in [§26](#26-quick-reference-the-standard-library).**

You may define a function of your own named like a built-in, a native, or a trait method — `toInt`, `sqrt`,
`next`. It wins wherever it is **visible**, which means your own module, and it does not reach any further: the
standard library keeps calling the real one, so `toIntChecked` still converts even if your file defines its own
`toInt`. A *library* function you have shadowed this way is still reachable by name — `std::sum(...)` gets the
real one even where a local `sum` shadows it.

One thing to expect when you read the library: it comes in **three call styles**, and the split follows the
orphan rule of [§16](#16-traits) rather than taste. A verb whose receiver is a **built-in** type — `Vec`,
`Map`, `String`, `Bytes` — has to be a free function, because no module owns those types and so none may
attach methods to them: `len(xs)`, `push(xs, v)`, `map` / `filter` / `sum`, all natural with `|>`. A type the
library **defines** — `Json`, `Rng`, `Set`, `Instant`, `TcpConn`, `Regex`, `ByteReader`, `CliArgs`,
`StringBuilder` — owns its behavior and carries it as `.` methods: `j.stringify()`, `rng.nextInt(1, 6)`,
`s.insert(x)`, `c.send(bytes)`. Its **constructors** are associated functions on the same type, so they read
`Type::name(…)`: `Json::parse(text)`, `Regex::compile(pat)`, `CliArgs::parse(argv)`, `Set::new()` /
`Set::fromVec(v)`, `ByteReader::new(b)`, `Rng::fromSeed(seed)`. A handful of constructors stay free where the
bare name reads better and claims nothing surprising — `now()`, `seconds(30)`, `connect(host, port)`,
`stringBuilder()` — as do functions with no natural receiver (`isLeapYear(y)`).

For example, `std::json` round-trips a document through its `Json` value tree:

```rust
use std::json::*

let doc = unwrap(Json::parse("{\"name\": \"Ada\", \"age\": 36}"))
let name = unwrap(unwrap(doc.getField("name")).asStr())
println(name)                  // => Ada
println(doc.stringify())       // => {"name":"Ada","age":36}
```

The `as*` accessors are the convenient path, but a `Json` is an ordinary enum — you can also `match` a value
directly against its cases, qualified as `Json::Case` (the cases are `Null`, `Boolean`, `Integer`, `Number`,
`Text`, `Arr`, `Obj`):

```rust
use std::json::*

fn describe(j: Json) -> String {
  match j {
    Json::Null       => "null",
    Json::Boolean(b) => if b { "yes" } else { "no" },
    Json::Integer(n) => "int " + toString(n),
    Json::Number(d)  => "num " + toString(d),
    Json::Text(s)    => "str " + s,
    Json::Arr(xs)    => "array of " + toString(len(xs)),
    Json::Obj(es)    => "object of " + toString(len(es)),
  }
}
println(describe(unwrap(Json::parse("42"))))          // => int 42
println(describe(unwrap(Json::parse("[1,2,3]"))))     // => array of 3
```

And `std::random`, seeded for reproducibility, rolls a die and shuffles a deck:

```rust
use std::random::*

let mut rng = Rng::fromSeed(42)
println(rng.nextInt(1, 7))     // a fair die: an Int in [1, 6]
println(rng.nextDouble())      // a Double in [0, 1)

let mut deck: Vec[Int] = vec()
let mut i = 1
while i <= 5 {
  push(deck, i)
  i = i + 1
}
rng.shuffle(deck)              // a uniformly-random permutation, in place
println(toString(deck))
```

`std::cli` turns a raw argument vector into flags, options, and positionals; `std::hash` checksums bytes:

```rust
use std::cli::*
use std::hash::*

let mut argv: Vec[String] = vec()
push(argv, "build")
push(argv, "--opt=O2")
push(argv, "-v")
push(argv, "main.skn")

let c = CliArgs::parse(argv)                    // real programs: CliArgs::parse(args())
println(c.getOptOr("opt", "O0"))           // => O2
println(toString(c.hasFlag("v")))          // => true
println(toString(c.numPositionals()))      // => 2   ("build", "main.skn")

println(toString(crc32Str("123456789")))   // => 3421780262
```

And `std::set` deduplicates and answers membership / set-algebra questions:

```rust
use std::set::*

let mut v: Vec[Int] = vec()
push(v, 1) push(v, 2) push(v, 2) push(v, 3)
let a = Set::fromVec(v)               // {1, 2, 3}
println(a.size())              // 3
println(a.isMember(2))         // true

let mut w: Vec[Int] = vec()
push(w, 2) push(w, 3) push(w, 4)
let b = Set::fromVec(w)
println(a.intersect(b).size()) // 2   ({2, 3})
```

And `std::time` decomposes an instant, does duration arithmetic, and round-trips ISO-8601:

```rust
use std::time::*

let t = instantFromMillis(1784644215123)    // 2026-07-21 14:30:15.123 UTC
let dt = t.toDateTime()
println(dt.toIso())                         // => 2026-07-21T14:30:15.123Z
println(dt.weekday())                       // => 2   (ISO Mon=1..Sun=7 -> Tuesday)

let later = t.addDuration(hours(2))
println(t.durationBetween(later).inMinutes())   // => 120

match parseIso("2000-02-29T12:00:00.000Z") {
  Ok(d)  => println(d.year),                // => 2000
  Err(m) => println(m)
}
```

And `std::net` opens a TCP connection. Here a server and a client talk over loopback in one program (the VM is
single-threaded, so this works because `connect` queues into the listen backlog and `accept` then picks it up):

```rust
use std::net::*

fn echo() -> Result[(), String] {
  let lst = listen(48080)?                    // dual-stack listener (IPv4 + IPv6)
  let mut cli = connect("127.0.0.1", 48080)?  // client connects
  let mut srv = lst.accept()?                 // server side of the connection

  cli.sendStr("ping\n")?
  let got = srv.recvLine()?                   // => Some("ping")
  println(match got { Some(s) => s, None => "<eof>" })

  cli.close()?  srv.close()?  lst.close()?
  Ok(())
}
match echo() { Ok(_) => println("done"), Err(e) => println(e) }
```

A one-line HTTP GET (plain `http://`, no TLS) fetches a page; a JSON body would feed `std::json::parse`:

```rust
use std::net::*
match httpGet("example.com", 80, "/") {
  Ok(r)  => println("status " + toString(r.status)),   // => status 200
  Err(e) => println(e)
}
```

And `std::regex` matches, captures, and rewrites text with a linear-time engine (compile once, reuse):

```rust
use std::regex::*
let re = match Regex::compile("(?<key>[a-z]+)=([0-9]+)") { Ok(r) => r, Err(e) => panic(e.message) }

match re.captures("  port=8080;") {           // capture groups (0 = whole match)
  Some(c) => {
    match c.groupNamed("key") { Some(m) => println(m.text), None => () }   // => port
    match c.group(2)          { Some(m) => println(m.text), None => () }   // => 8080
  },
  None => println("no match"),
}

println(toString(count(re.searchAll("a=1 b=22 c=333"))))   // lazy: => 3
println(re.replaceAllRe("a=1 b=22", "$1:$2"))              // templates: => a:1 b:22
```

Any prelude name can also be reached explicitly as `std::name` (useful when a local definition shadows it).

---

## 21. Input, output, and the standard natives

Beyond the language itself, a set of built-in functions ("natives") give you access to the outside world. They
are ordinary functions; the fallible ones return `Result` or `Option`.

Printing is always available. The rest live in **opt-in `std` modules** ([§20](#20-modules)) and need a `use`
before they resolve: `use std::io::*` for files and stdin, `use std::env::*` for arguments/environment/time,
and `use std::process::*` for spawning processes.

### Printing

`print(x)` and `println(x)` write text to standard output (`println` adds a newline). Both accept any value
and stringify it, and both accept multiple arguments (printed back-to-back with no separator).

```rust
print("no newline ")
println("with newline")
println("a=", 1, " b=", 2)   // => a=1 b=2
```

### Program arguments and environment

`args()` returns the command-line arguments as a `Vec[String]`. `getEnv(name)` looks up an environment
variable and returns an `Option[String]`.

```rust
use std::env::*

println("argc=" + len(args()))

match getEnv("PATH") {
    Some(_) => println("PATH is set"),
    None    => println("PATH is unset")
}
```

### Time

`nanoTime()` is a monotonic timer (only differences between readings are meaningful); `millisTime()` is
wall-clock milliseconds since the Unix epoch.

```rust
use std::env::*

let t0 = nanoTime()
let t1 = nanoTime()
println("elapsed >= 0: " + (t1 - t0 >= 0))   // => elapsed >= 0: true
```

### Files

`writeFile(path, bytes)` and `readFile(path)` handle whole files as `Bytes`; both return a `Result`.
`appendFile(path, bytes)` appends (creating the file if absent). Because a `String` is a byte string, the
text wrappers `readTextFile(path) -> Result[String, String]`, `writeTextFile(path, s)` and
`appendTextFile(path, s)` are just those byte natives with `toBytes`/`fromBytes` applied for you — use them
when you have text rather than raw bytes. `fileExists`, `isFile`, `isDir`, `fileSize`, `deleteFile`,
`listDir`, and `mkdir` cover the rest of the basics.

```rust
use std::io::*

let path = "greeting.txt"

match writeTextFile(path, "payload") {
    Ok(_)  => println("wrote it: " + fileExists(path)),   // => wrote it: true
    Err(e) => println("write failed: " + e)
}
match appendTextFile(path, "!") {                         // now holds "payload!"
    Ok(_)  => match fileSize(path) { Ok(n) => println("size: " + n), Err(_) => () },  // => size: 8
    Err(e) => println("append failed: " + e)
}
match readTextFile(path) {
    Ok(text) => println("read back: " + text),            // => read back: payload!
    Err(e)   => println("read failed: " + e)
}
println("isFile: " + isFile(path))                        // => isFile: true (a regular file, not a directory)
```

`fileExists` is `test -e` (any entry); `isFile` and `isDir` are the finer file-vs-directory queries.
`fileSize(path)` returns the byte length as a `Result[Int, String]`. `rename(from, to)` and
`copyFile(from, to)` move / copy a file (both `Result[(), String]`); `copyFile` fails if the destination
already exists.

### Standard input

`readLine()` reads one line as an `Option[String]` (`None` at end of input); `readAllStdin()` reads everything
to end of input as one `String`.

### Running processes

`run(argv)` runs a child process and returns a `Result` describing its output and exit code; `runText(argv)`
is the same with the output decoded to `String`; `sh(cmdline)` is a convenience wrapper.

```rust
use std::process::*

match runText(["cmd", "/c", "echo", "from-child"]) {
    Ok(out) => println("child said: " + slice(out.stdout, 0, 10)),   // => child said: from-child
    Err(e)  => println("spawn failed: " + e)
}
```

---

## 22. Programming styles: imperative, functional, streaming

Skarn is **multi-paradigm** and does not push you toward one way of writing code. You can write in a plain
imperative style, in a functional style, or freely mix the two — the type system is happy with all of it, and
no style is second-class. The only *gentle* leanings are that bindings are **immutable by default** (you opt
into mutation with `mut`) and that failure is carried in values (`Option`/`Result`) rather than thrown; neither
stops you from writing a tight mutable loop when that is the clearest thing.

Here is one small problem — the sum of the squares of the even numbers below `n` — written three ways.

**Imperative** — a mutable accumulator and a loop:

```rust
fn sumEvenSquaresImp(n: Int) -> Int {
    let mut total = 0
    let mut i = 0
    while i < n {
        if i % 2 == 0 { total = total + i * i }
        i = i + 1
    }
    total
}
```

**Functional** — no mutation, a tail-recursive helper (Skarn guarantees proper tail calls, so this runs in
constant stack space just like the loop):

```rust
fn sumEvenSquaresRec(i: Int, n: Int, acc: Int) -> Int {
    if i >= n { acc }
    else if i % 2 == 0 { sumEvenSquaresRec(i + 1, n, acc + i * i) }
    else { sumEvenSquaresRec(i + 1, n, acc) }
}
// call it as sumEvenSquaresRec(0, n, 0)
```

**Streaming** — a lazy pipeline that describes the transformation and lets `sum` pull the values through:

```rust
fn sumEvenSquaresStream(n: Int) -> Int {
    range(0, n)
        |> filter(fn(x: Int) -> Bool { x % 2 == 0 })
        |> map(fn(x: Int) -> Int { x * x })
        |> sum
}
```

All three return the same answer (`sumEvenSquaresImp(10)`, `sumEvenSquaresRec(0, 10, 0)`, and
`sumEvenSquaresStream(10)` are all `120`). Which one is "best" is a readability choice, not a correctness one —
pick the one that makes the intent clearest for the problem at hand.

### Is streaming a third paradigm?

You can reasonably argue that the lazy pipeline is *just* functional programming: it is built entirely from
higher-order functions composing immutable transformations, so by lineage it belongs to the functional family.
What makes it feel like its own idiom is **laziness**: a stream describes *what* to compute and produces
elements only on demand, which decouples producing values from consuming them and lets you work with
sequences that are conceptually **infinite** — something neither a loop nor plain recursion expresses as
directly:

```rust
// The first five squares, taken from an INFINITE stream of naturals.
let firstFiveSquares =
    iterate(0, fn(x: Int) -> Int { x + 1 })      // 0, 1, 2, 3, ... forever
        |> map(fn(x: Int) -> Int { x * x })
        |> take(5)
        |> collect
println(toString(firstFiveSquares))              // => [0, 1, 4, 9, 16]
```

`iterate` would run forever on its own; `take(5)` only ever asks it for five values, so the pipeline
terminates. Call it a distinct third style or a flavor of the functional one — either way it is a genuinely
different way to *think about* a computation, and it is a first-class citizen in Skarn.

Finally, none of these are walled off from each other. A largely functional program can drop into an
imperative loop for one hot inner routine; a streaming pipeline can end in a `for` loop that mutates a
`Vec`. Reach for whatever fits.

---

## 23. The type system in one page

A few properties that hold everywhere, gathered in one place:

- **Sound and erased.** If the checker accepts a program, its type assumptions hold at run time; and since
  types are erased, they impose no run-time cost. There is no `any` and no cast that can lie.
- **No null.** Absence is modeled with `Option`; a value of type `T` is always a real `T`.
- **Immutable by default.** A `let` binding cannot be reassigned. `let mut` allows reassignment. To modify a
  field or an element in place (`p.x = ...`, `a[i] = ...`, `m[k] = ...`), the binding you reach through must be
  `mut`, and to modify the receiver of a method, declare it `mut self`.

### Values and aliasing

Collections and structs are reference values: binding one to a new name, or passing it to a function, does not
copy it — both names refer to the same underlying object. So a mutation made through one name is visible
through the other. This is worth internalizing:

```rust
let original: Vec[Int] = vec()
push(original, 1)

let alias = original     // both names refer to the SAME vector
push(alias, 2)
push(alias, 3)

println("len through original = " + len(original))   // => len through original = 3
```

A method with `mut self`, or a function parameter marked `mut`, uses exactly this aliasing to make in-place
changes the caller can see (as the counter and iterator examples earlier showed).

### Why containers are invariant

A `Vec[Int]` is not a `Vec[dyn Show]`, even if `Int` implemented `Show`. If it were, you could hand your
`Vec[Int]` to code expecting a `Vec[dyn Show]`, which might push a `String` into it — corrupting your integer
vector. So container types are **invariant** in their element type: `Vec[A]` and `Vec[B]` are unrelated unless
`A` and `B` are the same. (Building a `Vec[dyn Show]` directly and pushing different implementers into it is
fine — each element is converted at the point you add it.)

### Built-in traits at a glance

Four traits are built into the standard library, and they split **2 + 2** — two are sealed, two are ordinary,
and the split is *reasoned*, not arbitrary. **Two are sealed compile-time markers** you never implement (the
checker decides membership and discharges the bound statically, no dispatch): **`Eq`** — sealed because equality
is **structural by construction**, so there is nothing to implement — and **`Hashable`** — sealed because a heap
object's **pointer bits are not stable under the moving GC**, so only the primitive-backed types can be keys.
**The other two are ordinary dispatched traits you *can* implement** for your own types: **`Ord`** (built-in
impls for `Int`/`Double`/`String`; add your own with `impl Ord` — the one exception is the erasure types) and
**`Clone`**.

| Trait | What it gates | Members | You `impl` it? | `dyn T`? |
|-------|---------------|---------|----------------|----------|
| `Eq` | `==` / `!=` (see [§5](#5-operators)) | derived structurally: any type whose components are all `Eq` (immediates + `String` + erasure types are the leaves; a function-carrying type is **not** `Eq`) | no (auto-derived) | no |
| `Hashable` | map keys / set elements (see [§14](#14-collections)) | `Int`, `Double`, `Bool`, `String`, and erasure types that wrap one of those (`Char`, integer-backed `enum`s, `transparent` newtypes over these) | no (fixed marker) | no |
| `Ord` | `sort` / `sorted` / `min` / `max` (ring, `std::iter`) and `minOf` / `maxOf` / `clamp` (opt-in `std::math` — needs `use std::math`); see [§19](#19-iterators) | `Int` / `Double` / `String` built in; **user types may `impl Ord`** (write `fn lessThan`). The one exception is **erasure types** (`Char` / `transparent` newtypes / integer-backed `enum`s) — a method trait can't dispatch on them; a `Char` compares with `<` but is not `Ord` | **yes** (like `Clone`; erasure types excepted) | no |
| `Clone` | `clone(x)` (see [§14](#14-collections)) | built-in for the containers; **user-extensible** — write `impl Clone for MyType` | **yes** | no (returns `Self`) |

The two markers (`Eq`, `Hashable`) cost nothing at run time; `==` on a leaf is a single instruction and on a
composite a structural walk. None of the four is usable as `dyn T`, but for two different reasons: `Eq` and
`Hashable` are **compile-time markers** — there is nothing to dispatch, so a `dyn` of them is meaningless and
rejected; `Ord` and `Clone` fail **object safety** (`Ord` takes `Self` as a second parameter, `Clone` returns
`Self` — see [§17](#17-trait-objects-dyn-trait)).

The same information, seen per *type* — the table below shows each type's **built-in** / automatic capability.
`==` and map-key reach the erasure types (marker traits, statically discharged); the built-in `<` reaches `Char`
(the compiler knows the static type at the site). The `Ord` column shows the **built-in** impls (`Int` /
`Double` / `String`); a `✓` is automatic, but — unlike the markers — `Ord` is an ordinary trait, so **your own
`struct` / `enum` can add it with `impl Ord`** (the built-in containers can't, by the orphan rule, and the
erasure types can't, being a method trait):

| Type | `==` (`Eq`) | map key (`Hashable`) | `<` `<=` `>` `>=` | `Ord` |
|------|:-----------:|:--------------------:|:-----------------:|:----------------------:|
| `Int` / `Double` | ✓ | ✓ | ✓ | ✓ |
| `String` | ✓ | ✓ | ✓ | ✓ |
| `Bool` | ✓ | ✓ | — | — |
| `Char` | ✓ | ✓ | ✓ | **—** (use `sortBy` with a comparator) |
| `transparent` newtype / int-backed `enum` | ✓ | ✓ | — | — |
| `struct` / payload `enum` | ✓ (if components are `Eq`) | — | — | — (✓ via `impl Ord`) |
| tuple / `Vec` / `Array` / `Map` / `Bytes` | ✓ (if components are `Eq`) | — | — | — |
| `()` (unit) | ✓ | — | — | — |
| a function / closure value | — | — | — | — |
| `dyn T` (a trait object) | — | — | — | — |

An empty cell means "not **automatically** a member." For `Eq` / `Hashable` / the `<` operators that is a real,
fixed limitation — you cannot add them. The **`Ord`** column is the exception: a user `struct` / `enum` can join
it with `impl Ord` (only the erasure types and the builtin containers genuinely cannot), so its `—` reads "not
automatic," *not* "impossible."

That makes `<` and `Ord` **orthogonal, with both off-diagonals occupied** — and both are worth knowing, because
each traps the reader in the opposite direction:
- a **`Char`** has `<` but is **not** `Ord` (an erasure type — no method dispatch); order a `Vec[Char]` with
  `sortBy` and a comparator.
- a **user `struct` with `impl Ord`** has `Ord` but **not** `<` (the operators stay hard-wired to
  `Int`/`Double`/`String`/`Char`), so you order it with `sorted` / `lessThan`, never `a < b`.

Lifting `Ord` (and `<`) to the **erasure types** is a possible future step (it needs monomorphizing an erased
body per erasure instantiation, a deferred idea).

### Advisory warnings

Beyond type errors (which stop compilation), the checker emits **advisory warnings** for two easy-to-miss
mistakes. They do not stop the program, but they are worth fixing:

- **An unused binding** — you introduced a `let` and never read it.
- **An ignored `Result`/`Option`** — you called something that returns one and threw the answer away, which
  usually means you forgot to handle a possible failure.

Both are silenced by making your intent explicit: prefix a name with `_` to say "deliberately unused", or
assign to the throwaway pattern `_`:

```rust
let _scratch = 1 + 1        // intentionally unused: no warning
let _ = pop(someVec)        // deliberately discard the returned Option
```

The runner can be asked to treat these warnings as hard errors, which is useful in a CI setting.

---

## 24. A complete little program

To close, here is a small program that ties many features together: an evaluator for arithmetic expressions
with named variables. It uses an `enum` to model the expression tree, `match` with recursion to walk it, a
`Map` for the variable environment, and `Result` with `?` to report an unbound variable cleanly.

```rust
// An arithmetic expression is one of these shapes.
enum Expr {
    Num(Int),
    Var(String),
    Add(Expr, Expr),
    Mul(Expr, Expr)
}

// Evaluate an expression against a variable environment.
// A missing variable turns into an `Err`, which `?` propagates.
fn eval(e: Expr, env: Map[String, Int]) -> Result[Int, String] {
    match e {
        Num(n) => Ok(n),

        Var(name) => match get(env, name) {
            Some(v) => Ok(v),
            None    => Err("unbound variable: " + name)
        },

        Add(a, b) => {
            let x = eval(a, env)?      // short-circuits on the first Err
            let y = eval(b, env)?
            Ok(x + y)
        },

        Mul(a, b) => {
            let x = eval(a, env)?
            let y = eval(b, env)?
            Ok(x * y)
        }
    }
}

// Build an environment: x = 10, y = 3
let mut env: Map[String, Int] = #{}
env["x"] = 10
env["y"] = 3

// Evaluate  (x * 2) + y
let program = Add(Mul(Var("x"), Num(2)), Var("y"))
match eval(program, env) {
    Ok(v)  => println("result = " + v),     // => result = 23
    Err(e) => println("error: " + e)
}

// Referencing an unknown variable surfaces as an error through `?`
let broken = Add(Var("z"), Num(1))
match eval(broken, env) {
    Ok(v)  => println("result = " + v),
    Err(e) => println("error: " + e)         // => error: unbound variable: z
}
```

---

## 25. The memory & cost model

Skarn has no manual memory management — no `free`, no reference counting, no borrow checker. A **garbage
collector** reclaims values once they are unreachable. To write efficient code (and to understand what `clone`
does), it helps to know which values live where.

### Two kinds of value

- **Immediates** live directly in a slot — 8 bytes, copied by value, never heap-allocated: `Int`, `Double`,
  `Bool`, `()`, `Char`, and the erased types (`transparent struct`s, integer-backed `enum`s). Copying one is
  free; two bindings to `42` are fully independent.
- **Heap objects** are everything with internal structure: `struct`s and `enum` variants with fields, `Array`,
  `Vec`, `Map`, `Bytes`, `String`, capturing closures, and **tuples** — a `(a, b)` is a heap object (note the
  asymmetry with the immediate `()` above: the empty tuple is a slot value, but *any* `(a, b, …)` is on the
  heap). A variable or field does **not** hold the object — it holds a **reference** to it. (A plain function
  or a non-capturing lambda is itself an immediate.)

The single most important fact about the cost model:

> Assigning, passing, or returning a heap value copies the **reference**, not the contents. It is always O(1),
> and there are **no hidden deep copies anywhere** in the language.

The one operation that *does* walk a heap value's contents is **structural `==` / `!=`** (see [§5](#5-operators)):
comparing two composites is O(the size of the value graph), not O(1). Comparing two large `Vec`s or `Map`s in a
loop is a real cost — compare a cheaper key or an early length check when it matters. (Equality is the only such
walk; it still allocates nothing. Comparing a value against *itself* is O(1) — a same-object short-circuit.)

### Sharing and aliasing

Because assignment shares the reference, two bindings can refer to the *same* object, and a mutation through one
is visible through the other:

```rust
let a: Vec[Int] = vec()
push(a, 1)  push(a, 2)
let b = a              // b and a are the SAME vector
push(b, 3)
println(toString(len(a)))   // => 3  -- a sees the push made through b
```

This is the one place Skarn's "immutable by default" promise can mislead, so it is worth being precise: `mut`
controls the **binding** — which *name* you are allowed to mutate through — not the object. It is not ownership,
and it does not prevent aliasing. In particular, an immutable binding is **not** a private snapshot: if any
`mut` name refers to the same object, that object can still change under it.

```rust
let mut xs: Vec[Int] = vec()
push(xs, 1)  push(xs, 2)  push(xs, 3)
let snapshot = xs       // NOT a snapshot -- just another name for the same vector
push(xs, 99)
println(toString(len(snapshot)))   // => 4  -- the "snapshot" grew too
```

If you came from a language with value-typed structs (C#, Swift, C++), note that Skarn structs behave like the
reference types of Java/C#/Python here, not like values — the difference from those languages is only that
mutation is opt-in via `mut`, not that assignment copies.

When you want an independent copy, ask for one explicitly with `clone`:

```rust
let c = clone(a)      // a fresh vector with the same elements
push(c, 4)
println(toString(len(a)))   // => 3  -- a is untouched
```

### `clone` is shallow — on purpose

`clone` copies **one level**: the outer container is new, but its elements are the *same* references. If the
elements are themselves heap objects, both copies share them:

```rust
let inner: Vec[Int] = vec()
push(inner, 1)
let outer: Vec[Vec[Int]] = vec()
push(outer, inner)          // outer[0] is a reference to `inner`

let dup = clone(outer)      // dup is new, but dup[0] is STILL `inner`
let shared = dup[0]
push(shared, 2)
println(toString(len(inner)))   // => 2  -- the shared inner vector changed
```

There is no built-in deep clone; if you need one, clone each level yourself. Shallow-by-default keeps `clone`
honest — it never silently walks and copies an arbitrarily large object graph.

### When do you allocate?

You allocate on the heap exactly when you **build or grow** something: constructing a `struct` / `enum` variant /
**tuple `(a, b)`**; creating an `array` / `vec` / `map` / `bytes` / `String`; pushing past a container's capacity;
building a capturing closure; or producing a new `String` (concatenation, `toString`, interpolation — strings are
immutable, so each makes a new one). Reading, indexing, pattern-matching, and passing values around allocate
nothing.

A **lazy-iterator** pipeline has its own per-element cost, easy to miss: each pulled element allocates one
`Option` (the `next()` result), and a **tuple-yielding** combinator — `enumerate`, `zip`, `partition`, `unzip` —
allocates a tuple on top of it. Heavier still are **`chunks`** and **`windows`**, which build a whole `Vec` per
pull (strictly more than a tuple). So `for (i, x) in enumerate(zip(a, b))` is the most allocation-heavy
*element-wise* loop shape (an `Option` and a tuple per step, per stage), and a `chunks`/`windows` pipeline is
heavier again. The **zero-allocation** loop forms, by contrast, are a
direct `for x in xs` over a container and `for (k, v) in m` over a map: both bypass the `Option`/tuple entirely via
a built-in fast path, binding the elements (and, for the map, the key and value) with no per-element heap traffic.
Reach for those in a hot loop; use the combinator pipelines for clarity where the allocation does not matter.

Everything else is free — the "you pay nothing" side of the design:

- **Immediates** (numbers, bools, `Char`, transparent newtypes, int-backed enums) never touch the heap.
- **Generics, traits, and `dyn Trait` are erased**: no generic type witnesses (dictionaries), no boxing, no
  vtables. A `dyn Show` value *is* the plain value — using a concrete type where a `dyn` is expected is a no-op.
  Trait dispatch is a table lookup on the coarse type tag a value already carries, not a pointer riding
  alongside it, so abstraction has no runtime price. (See [§16](#16-traits) for the full picture.)
- **Passing a large structure** is a pointer copy, not a `memcpy`.

### The collector

The GC is a **moving, compacting, stop-the-world collector**: live objects are relocated and packed together, so
the heap never fragments and allocation is a fast bump-pointer. Collection cost is proportional to the amount of
*live* data, not to how much garbage you produced — short-lived temporaries (a `Vec` built and dropped inside a
loop, the per-element `Option`s of a lazy iterator) are cheap to reclaim. You never trigger or tune it; it runs
automatically when the heap needs room.

---

## 26. Quick reference: the standard library

The functions below are always in scope (no import needed). They are ordinary functions — remember that a
trait method or a library function is called as `f(x)`, and `x |> f` is the same thing written left to right.

**Core and output**

| Function | Purpose |
|----------|---------|
| `print(a, ...)` / `println(a, ...)` | write values to standard output (`println` adds a newline) |
| `toString(x)` | render any value as text |
| `panic(msg)` | abort the program with a message |
| `assert(cond, msg)` | abort if `cond` is false |

**Numbers**

| Function | Purpose |
|----------|---------|
| `toInt(d)` | `Double` → `Int` (saturating, never aborts) |
| `toDouble(i)` | `Int` → `Double` (exact; a `Double` argument is an error) |
| `floor(d)` / `ceil(d)` / `trunc(d)` | round `Double` → `Double` toward −∞ / +∞ / zero |
| `round(d)` / `roundHalfToEven(d)` | round `Double` → `Double`, ties away from zero / ties to even |
| `parseInt(s)` | `String` → `Result[Int, String]` |
| `parseDouble(s)` | `String` → `Result[Double, String]` |
| `toIntChecked(d)` *(`std::math`)* | `Double` → `Result[Int, String]` (`Err` on NaN / ∞ / overflow) |

**Math** *(all `std::math` — `use std::math::*`)*

| Function | Purpose |
|----------|---------|
| `sqrt` / `cbrt` / `pow(x,y)` / `hypot(x,y)` | roots and powers |
| `exp` / `ln` / `log2` / `log10` | exponential and logarithms (`ln` = natural log) |
| `sin` / `cos` / `tan` / `asin` / `acos` / `atan` / `atan2(y,x)` | trigonometry |
| `abs` / `absInt` / `sign` / `signInt` | magnitude and sign (`Double` and `Int` forms) |
| `gcd(a,b)` / `lcm(a,b)` | integer gcd / lcm |
| `minOf(a,b)` / `maxOf(a,b)` / `clamp(x,lo,hi)` | scalar min/max/clamp, generic over any ordered type |
| `isNaN(x)` / `isInfinite(x)` | IEEE predicates |
| `PI` / `E` | constants |

**Binary I/O** *(all `std::bytes` — `use std::bytes::*`; little-endian)*

| Function | Purpose |
|----------|---------|
| `writeU8(b,v)` / `writeU16LE(b,v)` / `writeU32LE(b,v)` | append an unsigned 1/2/4-byte value (masking); returns `b` |
| `writeI32LE(b,v)` | append a signed 4-byte value (two's complement); returns `b` |
| `writeVarU(b,v)` | append `v ≥ 0` as unsigned LEB128 (1–7 bytes); returns `b` |
| `writeVarI(b,v)` | append a signed `Int` as a zigzag varint (compact for small magnitudes); returns `b` |
| `writeStr(b,s)` | append a length-prefixed `String` (varU byte-length + raw bytes); returns `b` |
| `writeBytes(b,src)` | append the raw bytes of `src`; returns `b` |
| `ByteReader::new(b)` | a `ByteReader` cursor over `b` (`pos` starts at 0) |
| `r.readU8()` / `r.readU16LE()` / `r.readU32LE()` / `r.readI32LE()` | read a 1/2/4-byte value → `Result[Int, String]` (`Err` on underrun) |
| `r.readVarU()` / `r.readVarI()` | read an unsigned LEB128 / a signed zigzag varint → `Result[Int, String]` |
| `r.readStr()` | read a length-prefixed `String` → `Result[String, String]` |
| `r.readBytes(n)` | read `n` raw bytes → `Result[Bytes, String]` |
| `r.remaining()` / `r.atEnd()` | bytes left to read / whether the cursor is at the end |

**JSON** *(all `std::json` — `use std::json::*`)*

| Function | Purpose |
|----------|---------|
| `Json::parse(s)` | parse JSON text → `Result[Json, String]` |
| `j.stringify()` / `j.stringifyPretty(indent)` | serialize a `Json` to compact / indented text |
| `j.stringifyChecked()` | serialize → `Result[String, String]` (`Err` on a non-finite number) |
| `j.getField(key)` / `j.at(i)` | look up an object field / array element → `Option[Json]` |
| `j.asInt()`/`j.asDouble()`/`j.asStr()`/`j.asBool()`/`j.asArr()`/`j.asObj()`/`j.asMap()`/`j.isNull()` | extract a `Json` case (each `Option`, except `isNull` → `Bool`; `asArr`/`asObj` hand back the **live** payload `Vec`, not a copy) |

**Randomness** *(all `std::random` — `use std::random::*`; deterministic, NOT cryptographic)*

| Function | Purpose |
|----------|---------|
| `Rng::fromSeed(seed)` / `Rng::fromState(a,b,c,d)` | a new `Rng` from an `Int` seed (reproducible) / raw state words |
| `r.nextU32()` | a raw 32-bit draw in `[0, 2³²)` |
| `r.nextIntBounded(bound)` / `r.nextInt(lo, hi)` | a uniform `Int` in `[0, bound)` / `[lo, hi)` (bias-free) |
| `r.nextInt48()` | a uniform `Int` over the full signed range (can be negative) |
| `r.nextBool()` | a random `Bool` |
| `r.nextDouble()` / `r.nextDoubleRange(lo, hi)` | a uniform `Double` in `[0, 1)` / `[lo, hi)` |
| `r.shuffle(v)` / `r.choice(v)` | shuffle a `Vec` in place / a random element → `Option[T]` |
| `r.nextGaussian()` / `r.nextGaussianMS(mu, sigma)` | a normal deviate (mean 0, sd 1) / scaled to `mu`, `sigma` |
| `r.choiceWeighted(items, weights)` | a weighted pick → `Option[T]` (`None` if empty / mismatched / total ≤ 0) |
| `r.sample(v, k)` | `k` distinct elements without replacement → `Vec[T]` (`k` clamped to `[0, len]`) |
| `r.jump()` / `r.splitRng()` | advance one stream / return a fresh non-overlapping stream |
| `r.fillU32(out, n)` / `r.fillDoubles(out, n)` | append `n` draws to a `Vec` (bulk, load/store-optimized) |

**Command-line arguments** *(all `std::cli` — `use std::cli::*`; a space-separated option value like `--out file` is **not** supported — write `--out=file`)*

| Function | Purpose |
|----------|---------|
| `CliArgs::parse(argv)` | parse anything iterable of `String` — an `Array` from `args()` or a `Vec` you built — → `CliArgs` |
| `c.hasFlag(name)` | was `--name` / short `-n` present? → `Bool` |
| `c.getOpt(name)` / `c.getOptOr(name, dflt)` | the value of `--name=value` → `Option[String]` / with a default |
| `c.numPositionals()` / `c.positionalAt(i)` | positional count → `Int` / the i-th positional → `Option[String]` |

**Hashing** *(all `std::hash` — `use std::hash::*`)*

| Function | Purpose |
|----------|---------|
| `crc32(bytes)` / `crc32Str(s)` | CRC-32 (IEEE/zlib) checksum of a `Bytes` / `String` → non-negative 32-bit `Int` (not secure) |
| `murmur3(bytes, seed)` / `murmur3Str(s, seed)` | MurmurHash3 x86_32 of a `Bytes` / `String` with a 32-bit `seed` → non-negative 32-bit `Int` (fast, non-cryptographic) |
| `sha256(bytes)` / `sha256Str(s)` | SHA-256 (FIPS 180-4) → the raw 32-byte digest as `Bytes` |
| `sha256Hex(bytes)` / `sha256HexStr(s)` | SHA-256 as a 64-char lowercase hex `String` |
| `toHex(bytes)` | lowercase hex encoding of any `Bytes` (2 chars/byte) |

**Sets** *(all `std::set` — `use std::set::*`; `T` must be `Hashable`; a `Set` is `IntoIterator`, so `for x in s` and the lazy combinators work — in hash order)*

| Function | Purpose |
|----------|---------|
| `Set::new()` / `Set::fromVec(v)` | an empty `Set[T]` — needs an annotation (`let s: Set[Int] = Set::new()`), since nothing else fixes `T` / a set of the distinct elements of a `Vec` |
| `s.insert(x)` / `s.remove(x)` | add / delete `x` → `Bool` (was-new / was-present); `s` must be `mut` |
| `s.isMember(x)` / `s.size()` | membership → `Bool` / cardinality → `Int` (both O(1)) |
| `a.union(b)` / `a.intersect(b)` / `a.difference(b)` | the combined / common / left-only set |
| `a.isSubset(b)` / `a.isDisjoint(b)` | `a ⊆ b` / `a ∩ b = ∅` → `Bool` |

**Dates and times** *(all `std::time` — `use std::time::*`; strict UTC, millisecond precision)*

| Function | Purpose |
|----------|---------|
| `now()` / `instantFromMillis(ms)` / `fromDateTime(dt)` | build an `Instant` (free constructors) |
| `t.toEpochMillis()` | the `Instant` as epoch milliseconds |
| `t.toDateTime()` | `Instant` → decomposed civil `DateTime` (UTC) |
| `dateTime(y,mo,d,h,mi,s,ms)` / `dateOnly(y,mo,d)` | a strictly-validated `DateTime` → `Option` (free; bad fields → `None`) |
| `dt.toIso()` / `parseIso(s)` | ISO-8601 `YYYY-MM-DDThh:mm:ss.sssZ` ↔ `DateTime` (`dt.toIso()` method, `parseIso` free → `Result`) |
| `dt.weekday()` | ISO weekday, Monday=1 .. Sunday=7 |
| `isLeapYear(y)` / `daysInMonth(y, mo)` | calendar predicates (year-based → free) |
| `millis`/`seconds`/`minutes`/`hours`/`days` `(n)` | build a `Duration` (free constructors) |
| `d.inMillis()`/`d.inSeconds()`/`d.inMinutes()`/`d.inHours()`/`d.inDays()` | a `Duration` as an `Int` (truncating) |
| `t.addDuration(d)` / `t.subDuration(d)` / `a.durationBetween(b)` | `Instant` arithmetic |
| `a.add(b)` / `a.sub(b)` / `d.scale(k)` / `d.negate()` | `Duration` algebra |
| `a.isBefore(b)` / `a.isAfter(b)` | order two `Instant`s → `Bool` |
| `startStopwatch()` / `sw.elapsedNanos()` / `sw.elapsedMillis()` | a monotonic stopwatch (`startStopwatch` free) |

**TCP networking** *(all `std::net` — `use std::net::*`; blocking, plaintext only; close sockets explicitly)*

| Function | Purpose |
|----------|---------|
| `connect(host, port)` | open a client connection → `Result[TcpConn, String]` (IPv4/IPv6, name or literal; free) |
| `c.send(bytes)` / `c.sendStr(s)` | send a whole buffer / string → `Result[(), String]` |
| `c.recv(n)` | receive up to `n` bytes → `Result[Bytes, String]` (an **empty `Bytes` means EOF**) |
| `c.recvLine()` / `c.recvAll()` | read one `\n`-line → `Result[Option[String], String]` / drain to EOF → `Result[Bytes, String]` (both need `mut c`) |
| `c.setTimeout(ms)` / `c.close()` | recv/send timeout (0 = block) / close the connection → `Result[(), String]` |
| `listen(port)` (free) / `l.accept()` / `l.close()` | server: bind+listen → `TcpListener`; block for a client → `TcpConn`; stop listening |
| `httpGet(host, port, path)` | a minimal HTTP/1.0 GET → `Result[HttpResponse, String]` (`.status: Int`, `.body: String`; free) |

**Regex** *(all `std::regex` — `use std::regex::*`; byte-level, linear-time Pike VM; no backrefs/lookaround. Note the names: matching anywhere is `search`, not `find`, and rewriting is `replaceRe`, not `replace` — those two belong to `std::iter` / `std::string`)*

| Function | Purpose |
|----------|---------|
| `Regex::compile(pattern)` / `Regex::compileWith(pattern, flags)` | build a `Regex` → `Result[Regex, RegexError]`; `flags` ⊆ `"ims"` (also inline `(?ims)`) |
| `re.isMatch(s)` | does the pattern match anywhere in `s` → `Bool` |
| `re.search(s)` / `re.searchFrom(s, from)` | leftmost match → `Option[Match]` (`.start`, `.end`, `.text`) |
| `re.captures(s)` / `re.capturesFrom(s, from)` | leftmost match with groups → `Option[Captures]` |
| `c.group(i)` / `c.groupNamed(name)` | the i-th / named capture → `Option[Match]` (group 0 = whole match) |
| `re.searchAll(s)` / `re.capturesAll(s)` | every non-overlapping match / its captures → a lazy `dyn Iterator` |
| `re.splitRe(s)` | the substrings between matches → a lazy `dyn Iterator[String]` |
| `re.replaceRe(s, repl)` / `re.replaceAllRe(s, repl)` | replace the first / all matches; template `$0`..`$N`, `${name}`, `$$` |

Supported syntax: `. * + ? | ( ) (?:…) (?<name>…) [...] ^ $`, non-greedy `*?`, counts `{n}` / `{n,}` /
`{n,m}`, the shorthands `\d \w \s` (and their negations), the word boundary `\b`, and the flags `i` (ASCII
case), `m` (multiline), `s` (dotall) — as `Regex::compileWith(pattern, "ims")` or inline `(?ims)`.

**Strings and bytes**

| Function | Purpose |
|----------|---------|
| `len(x)` | length (bytes of a `String`, element count of a collection) |
| `slice(s, start, end)` | substring over the byte range `[start, end)` |
| `charStr(byte)` | a one-byte `String` from an `Int` byte value |
| `charAt(s, i)` / `isEmpty(s)` | the byte at index `i` (traps if out of range) / whether `s` is empty |
| `indexOf(s, sub)` / `lastIndexOf(s, sub)` | first / last byte index of `sub`, or `-1` if absent |
| `startsWith(s, p)` / `endsWith(s, p)` / `hasSubstr(s, sub)` | prefix / suffix / containment test (`Bool`) |
| `trim(s)` / `trimStart(s)` / `trimEnd(s)` | strip ASCII whitespace from both ends / start / end |
| `toUpper(s)` / `toLower(s)` | ASCII case conversion (bytes `≥ 0x80` unchanged) |
| `replace(s, from, to)` | replace **all** occurrences of `from` (empty `from` is a no-op) |
| `padStart(s, width, padByte)` / `padEnd(s, width, padByte)` | pad to a byte `width` with a single `padByte` |
| `repeatStr(s, n)` | `s` repeated `n` times (`n ≤ 0` → `""`) |
| `isAsciiControl/Digit/Whitespace/Alpha/Alphanumeric/Upper/Lower/Printable(c)` | ASCII classify a byte `Int` → `Bool` (`≥ 128` → `false`) |
| `hasControl(s)` | `Bool` — does `s` contain any ASCII control byte |
| `toBytes(s)` / `fromBytes(b)` | convert between `String` and `Bytes` |
| `split(s, sep)` | lazily split into fields on a literal separator (empties kept; inverse of `join`) |
| `lines(s)` | lazily split into lines (terminator semantics; `CRLF` handled) |
| `stringBuilder()` / `stringBuilderCap(n)` | a `StringBuilder` accumulator over `Bytes` (empty / pre-sized to `n`) |
| `sb.append(s)` / `sb.appendByte(c)` | append a `String` / one byte to a `StringBuilder` (mutates in place; returns `sb`, so calls chain) |
| `sb.build()` / `sb.len()` | the accumulated `String` / its current byte length |
| `format(x, spec)` | format a scalar per a `${x:spec}` specifier (width/align/precision/base, sign `+`, `#` prefix, scientific `e`/`E`) → `String` |

**Unicode code points** *(`std::string` / `std::iter`; the opt-in code-point layer over the default byte model — `for c in s` still iterates bytes)*

| Function | Purpose |
|----------|---------|
| `chars(s)` | lazily iterate the UTF-8 code points of `s` as `Char`s (a malformed byte decodes to U+FFFD, so it never fails) |
| `charCount(s)` | number of code points (O(n); note `len(s)` is the byte count) |
| `nthChar(s, i)` | the i-th code point → `Option[Char]` (O(n) — a `String` is not directly indexable) |
| `codePoint(c)` | a `Char`'s `Int` code point |
| `codePointToStr(cp)` | encode an `Int` code point → its UTF-8 `String` (invalid/surrogate → U+FFFD) |
| `fromChars(it)` | build a `String` from an iterator of `Char` (`fromChars(chars(s)) == s` for valid UTF-8) |
| `sb.appendCodePoint(cp)` | append a code point's UTF-8 to a `StringBuilder` (mutates in place; returns `sb`) |

**Collections**

| Function | Purpose |
|----------|---------|
| `array(n, init)` / `emptyArray()` / `vec()` / `bytes()` | create an `Array` (filled / empty) / `Vec` / `Bytes` |
| `clone(c)` | an independent shallow copy of a `Vec` / `Map` / `Bytes` / `Array` (a `Clone` trait — implement it for your own types) |
| `push(c, x)` / `pop(c)` | append to / remove the last element of a `Vec` or `Bytes` (`pop` returns `Option`) |
| `c[i]` / `c[i] = x` | read / write an element of an `Array`, `Vec`, or `Bytes` by index (bounds-checked — aborts on out-of-range; assignment needs a `mut` binding) |
| `len(c)` | number of elements |
| `get(c, i)` | safe indexed read of an `Array` / `Vec` / `Bytes` — `Option` (`None` if out of range) |
| `m[k]` / `m[k] = v` | read (aborts if the key is missing) / insert-or-update a `Map` entry (assignment needs a `mut` binding) |
| `has(m, k)` / `get(m, k)` | map membership test / safe lookup (`Option`) |
| `delete(m, k)` | remove a map entry |
| `keys(m)` / `values(m)` | snapshot a map's keys / values |
| `getOr(m, k, dflt)` / `getOrElse(m, k, f)` | map lookup with an eager / lazy default (non-mutating) |
| `getOrPut(m, k, dflt)` / `upsert(m, k, dflt, f)` | insert-if-absent / read-modify-write a `Map` entry (needs a `mut` binding) |

**Option and Result**

| Function | Purpose |
|----------|---------|
| `Some(x)` / `None` / `Ok(x)` / `Err(e)` | the constructors |
| `isSome` / `isNone` / `isOk` / `isErr` | test which case |
| `unwrap(x)` / `expect(x, msg)` / `unwrapOr(x, d)` | the `Unwrap` trait — extract from an `Option` **or** a `Result`, aborting (`unwrap`/`expect`) or defaulting (`unwrapOr`) on `None`/`Err` |
| `unwrapErr(r)` | extract a `Result`'s `Err` payload (aborts on `Ok`) |
| `optionMap` / `resultMap` | transform the contained value |
| `optionAndThen` / `resultAndThen` | chain another fallible step |
| `mapErr` | transform an error |
| `okOr(o, e)` / `ok(r)` | convert between `Option` and `Result` |

**Iterators — sources**

| Function | Purpose |
|----------|---------|
| `range(lo, hi)` | integers `lo` up to (not including) `hi` |
| `repeat(x)` | the same value forever |
| `iterate(seed, f)` | `seed`, `f(seed)`, `f(f(seed))`, ... |
| `once(x)` / `empty()` | exactly one / no elements |

**Iterators — stages (iterator → iterator)**

| Function | Purpose |
|----------|---------|
| `map(it, f)` / `filter(it, p)` | transform / keep matching elements |
| `take(it, n)` / `drop(it, n)` | first `n` / all but the first `n` |
| `takeWhile(it, p)` / `dropWhile(it, p)` | while a predicate holds |
| `enumerate(it)` | pair each element with its index |
| `zip(a, b)` / `chain(a, b)` | pair up / concatenate two iterators |
| `scan(it, init, f)` | running accumulator |
| `flatMap(it, f)` | map to iterators and flatten |
| `stepBy(it, n)` | first element, then every `n`-th |
| `inspect(it, f)` | run `f` per element (a tap), passthrough |
| `dedup(it)` | drop *consecutive* duplicates (`T: Eq`) |
| `chunks(it, n)` / `windows(it, n)` | non-overlapping / overlapping `Vec[T]` groups of `n` |
| `peekable(it)` | look ahead one element via `.peek()` |

**Iterators — terminals (iterator → value)**

| Function | Purpose |
|----------|---------|
| `sum` / `product` / `count` | numeric fold / element count |
| `collect(it)` / `toVec(x)` | gather into a `Vec` |
| `fold(it, init, f)` / `reduce(it, f)` | general accumulation |
| `forEach(it, f)` | run `f` for its side effect |
| `any` / `all` / `find` / `position` / `contains` | search predicates |
| `min` / `max` / `minBy` / `maxBy` | extremes |
| `partition` / `unzip` / `join` / `toMap` | split by predicate / split pairs / join strings / build a map |

**Sorting a `Vec`** *(`std::iter`)*

| Function | Purpose |
|----------|---------|
| `sorted(v)` | new sorted `Vec` (`Ord`; non-mutating), stable |
| `sort(v)` | sort a `mut` `Vec` in place (`Ord`), stable |
| `sortBy(v, less)` | new `Vec` sorted by a `fn(T,T) -> Bool` comparator, stable |

**Input / output natives** (opt-in — `use std::env::*` / `std::io` / `std::process`; see [§20](#20-modules))

| Function | Purpose |
|----------|---------|
| `args()` / `getEnv(name)` | command-line arguments / an environment variable (`Option`) |
| `nanoTime()` / `millisTime()` | monotonic timer / wall-clock milliseconds |
| `readFile(path)` / `writeFile(path, bytes)` / `appendFile(path, bytes)` | whole-file read / write / append (`Result`) |
| `readTextFile` / `writeTextFile` / `appendTextFile` | the same as `String` (byte wrappers over the above) |
| `fileExists` / `isFile` / `isDir` / `fileSize` | entry / regular-file / directory query, byte length |
| `deleteFile` / `rename` / `copyFile` / `listDir` / `mkdir` | delete / move / copy / list / make directory |
| `readLine()` / `readAllStdin()` | read a line / all of standard input |
| `run(argv)` / `runText(argv)` / `sh(cmdline)` | spawn a process and capture its output |

---

### Where to go next

- The grammar in `docs/skarn_grammar.ebnf` is the precise reference for the syntax.
- The `demo/` directory has larger runnable programs — an iterator showcase (`iterator_for`), JSON both ways
  (`json_parser` from scratch and `json` over `std::json`), one demo per standard-library module (`math`,
  `random`, `set`, `time`, `bytes`, `map_helpers`), string/byte iteration (`string_iter`), trait objects
  (`dyn_traits`), string interpolation (`interp`), and file I/O (`file_io`).
- A few features exist in the language's design space but are intentionally not part of this guide because they
  are not built yet (for example loop labels and a handful of extra iterator combinators like `rev` and `cycle`).
