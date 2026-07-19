# An Introduction to Skarn

Skarn is a small, statically typed scripting language that runs on the vMachine bytecode interpreter.
This guide is a complete, example-driven tour of the language for working programmers. It assumes you are
comfortable with a mainstream statically typed language and have seen a few functional-programming ideas
(closures, pattern matching, immutable-by-default values), but it does **not** assume you know any particular
functional language — every construct is explained on its own terms.

Every code block in this guide is a real program (or fragment) that compiles and runs. Where it helps, the
expected output is shown in a trailing comment (`// => ...`).

## How to read this guide

- Sections build on each other, but each is self-contained enough to skim.
- Code is shown in normal, multi-line style. In real files you may write several small statements on one line
  separated by spaces; the compiler uses newlines and blank lines only as hints, not as required separators.
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
25. [Quick reference: the standard library](#25-quick-reference-the-standard-library)

---

## 1. What Skarn is

Skarn is **statically typed** and **type-checked before it runs**. If a program passes the type checker, whole
classes of bugs cannot occur: there are no null-pointer errors (there is no `null`), no "undefined is not a
function", and no silent type coercions that produce a wrong answer.

Three properties are worth stating up front, because they shape everything else:

- **The types are checked and then erased.** Types exist only at compile time. At run time there are just
  values; no type information is carried around for the sake of the type system. This keeps the language fast
  and means generics cost nothing at run time.
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
`'A'`. Skarn does **not** have a separate character type — a character literal is simply an `Int` holding the
byte value of that one character (`'A'` is `65`). This pairs naturally with the fact that strings are byte
sequences.

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

---

## 5. Operators

### Arithmetic

`+ - * / %` work on numbers. If both operands are `Int` the result is `Int`; if either is `Double`, both are
treated as `Double` and the result is `Double`.

```rust
let a = 3 + 4 * 2       // => 11 (Int)
let b = 10.0 / 4.0      // => 2.5 (Double)
let c = 3 + 0.5         // => 3.5 (Int promoted to Double)
```

### Comparison and equality

`== !=` compare any two values of the same type by value (structural equality). `< <= > >=` compare **numbers
or strings**.

```rust
let e1 = (2 + 2 == 4)         // => true
let e2 = ("abc" == "abc")     // => true (compares the text, not identity)
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

`& | ^ << >> >>>` operate on `Int` only (`>>` is an arithmetic/sign-preserving shift, `>>>` is logical). There
is no bitwise-not operator; use `x ^ (0 - 1)` to flip all bits.

```rust
let bits = (6 & 3) | (1 << 4)   // (2) | (16) => 18
println("bits=" + bits)         // => bits=18

println("shr="  + ((0 - 8) >> 1))   // => shr=-4   (arithmetic: sign preserved)
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

```rust
fn twice(n: Int) -> Int { n * 2 }
fn plus(n: Int, k: Int) -> Int { n + k }

let piped = 5 |> twice |> plus(1)   // plus(twice(5), 1) => 11
println("piped=" + piped)
```

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

To convert a `Double` to an `Int`, use `toInt`. It is **saturating and total**: it never aborts. A fractional
value truncates toward zero; a value too large becomes the maximum `Int`; too small becomes the minimum; and a
NaN becomes `0`.

```rust
println("toInt(3.9)=" + toInt(3.9))    // => toInt(3.9)=3
println("toInt(-2.7)=" + toInt(0.0 - 2.7))  // => toInt(-2.7)=-2
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
- **Scalar min/max/clamp** — generic over any ordered type (`Int`, `Double`, `String`), so they preserve the
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

println("parse=" + parseOr("123", 0 - 1))   // => parse=123
println("bad="   + parseOr("xyz", 0 - 1))   // => bad=-1
```

---

## 7. Strings

A `String` is a sequence of bytes. If your source file is UTF-8, string literals keep their bytes exactly, so
text round-trips faithfully — but the language treats a string as bytes, not as Unicode code points. In
particular, `len` returns the number of **bytes**.

String literals support the escapes `\n \t \r \\ \" \' \0`.

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

You can move between a `String` and its raw bytes with `toBytes` and `fromBytes`. This is how you build a
string from computed bytes (there is no character type to append):

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
println("[" + trim(s) + "]")                   // => [Hello, World]
println("[" + trimStart(s) + "]")              // => [Hello, World  ]
println("[" + trimEnd(s) + "]")                // => [  Hello, World]

// ASCII case
println(toUpper("Hi, x9!"))                    // => HI, X9!
println(toLower("Hi, X9!"))                    // => hi, x9!

// small companions
println(charAt("ABC", 1))                      // => 66   (the byte at index 1)
println(isEmpty(""))                           // => true
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
println(describe(0 - 5))   // => negative
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
    else { countUp(n - 1, acc + 1) }   // tail call: no stack growth
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
println("closure=" + add10(5))   // => closure=15
```

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
    let ax = if p.x < 0 { 0 - p.x } else { p.x }
    let ay = if p.y < 0 { 0 - p.y } else { p.y }
    ax + ay
}

let p = Point { x: 3, y: 4 }
println("p.x="  + p.x)             // => p.x=3
println("dist=" + manhattan(p))    // => dist=7
```

Structs are values; there is no separate "mutate a field" statement unless the binding is `mut` (see
[§23](#23-the-type-system-in-one-page)). The idiomatic "change" is to build a new struct:

```rust
let moved = Point { x: p.x + 1, y: p.y }
println("moved.x=" + moved.x)   // => moved.x=4
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
        West => 0 - 1,
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
println(sign(5) + " " + sign(0 - 2) + " " + sign(0))   // => pos neg zero
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

---

## 14. Collections

Skarn provides several collection types. They all work with `for`, `len`, and (where it makes sense) indexing.

### Fixed-size arrays: `Array[T]`

`array(n, init)` creates an array of `n` elements all equal to `init`. Index with `a[i]`. To assign to an
element, the array binding must be `mut`.

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
(returning an `Option`, since the vector might be empty).

```rust
let v: Vec[Int] = vec()
push(v, 1)
push(v, 2)
push(v, 3)
let last = pop(v)
println("len="  + len(v))            // => len=2
println("last=" + toString(last))    // => last=Some(3)
```

A vector also supports random access with `v[i]`, exactly like an array: reading is bounds-checked (aborts on
an out-of-range index), and `get(v, i)` is the safe `Option` form. Assigning to an element (`v[i] = x`) needs a
`mut` binding — note that `push`/`pop` mutate in place *without* `mut` (they go through a mutable parameter),
but an index **assignment** is a mutation of the binding itself, so it requires `mut`:

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
println("popped=" + toString(spop(s)))   // => popped=Some(20)
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

Here is the first thing that differs from a typical object-oriented language: **a trait method is called like
an ordinary function, and dispatch is on the first argument.** You write `area(shape)`, not `shape.area()`.
At run time, Skarn looks at the actual type of the first argument and calls the matching implementation.

```rust
println("circle=" + area(Circle { r: 2.0 }))   // => circle=12.56636
println("square=" + area(Square { s: 3.0 }))   // => square=9.0
```

(The pipe operator makes this read like a method chain when you want it to: `Circle { r: 2.0 } |> area`.)

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
push(zoo, Dog { name: "Rex" })   // a Dog, seen as `dyn Show`
push(zoo, Cat { lives: 9 })      // a Cat, seen as `dyn Show`

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
println("or=" + unwrapOr(half(7), 0 - 1))   // => or=-1
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
    let x = safeDiv(100, 5)?   // x = 20
    let y = safeDiv(x, 2)?     // y = 10
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

Stages take an iterator and return a new iterator, so you compose them. Because everything is lazy, no
intermediate vectors are created.

- `map(it, f)` — apply `f` to each element
- `filter(it, p)` — keep elements where `p` is true
- `take(it, n)` / `drop(it, n)` — first `n` / all but the first `n`
- `takeWhile(it, p)` / `dropWhile(it, p)` — while a predicate holds
- `enumerate(it)` — pair each element with its index, `(Int, T)`
- `zip(a, b)` — pair up two iterators, stopping at the shorter
- `chain(a, b)` — one iterator after another
- `scan(it, init, f)` — running accumulator
- `flatMap(it, f)` — map each element to an iterator and flatten

Each stage takes its iterator as the *first* argument, which is exactly the shape the pipe operator (`|>`, from
[§5](#5-operators)) was made for. Writing a pipeline with `|>` reads top to bottom in the order the data flows —
source, then each stage, then a terminal — instead of inside-out:

```rust
let s =
    range(0, 10)
    |> filter(fn(x: Int) -> Bool { x % 2 == 0 })
    |> map(fn(x: Int) -> Int { x * x })
    |> sum
println("pipeline=" + s)   // => pipeline=120   (0 + 4 + 16 + 36 + 64)
```

That is the same computation as the nested `sum(map(filter(range(0, 10), ...), ...))`, just easier to read. The
rest of this section uses `|>` wherever a value flows through a chain.

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

`min` and `max` work on any type that can be ordered (numbers and strings out of the box), returning an
`Option` because the sequence might be empty. When you want to order by a custom rule, `minBy`/`maxBy` take a
comparison function:

```rust
let words = ["pear", "apple", "kiwi"]
println("min=" + unwrapOr(min(intoIter(words)), "?"))   // => min=apple

let biggest = maxBy(range(1, 5), fn(a: Int, b: Int) -> Bool { a < b })
println("maxBy=" + toString(biggest))   // => maxBy=Some(4)
```

To order a whole collection, `sorted(v)` returns a **new**, sorted `Vec` (leaving `v` untouched), `sort(v)`
sorts a `mut` vector **in place**, and `sortBy(v, less)` orders by a custom comparator. All three are a
**stable** merge sort; `sorted`/`sort` use `Ord` (numbers and strings), so they work on any ordered element:

```rust
let names = sorted(toVec(["cherry", "apple", "banana"]))
println(toString(names))                          // => ["apple", "banana", "cherry"]

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

When you want an eager snapshot instead of a lazy pipeline, `toVec(x)` copies any iterable into a `Vec`:

```rust
println("toVec=" + toString(toVec([7, 8, 9])))   // => toVec=[7, 8, 9]
```

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
    let ax = if dx < 0 { 0 - dx } else { dx }
    let ay = if dy < 0 { 0 - dy } else { dy }
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
pub fn add(a: Int, b: Int) -> Int { a + b }   // exported
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

### The standard library

The prelude is organized into `std` modules. The **ring** modules are auto-imported everywhere, so their
names are available **unqualified** with no `use`:

- `std::core` — `Option`, `Result`, `Some`/`None`/`Ok`/`Err`, and their combinators.
- `std::iter` — the `Iterator`/`IntoIterator`/`Iterable` traits, `Ord`, all the stream sources, stages,
  and terminals from [§19](#19-iterators), and the `Vec` sorts `sort`/`sorted`/`sortBy`.
- `std::string` — string helpers: `slice`/`charStr`/`charAt`/`isEmpty`, search (`indexOf`/`lastIndexOf`/
  `startsWith`/`endsWith`/`hasSubstr`), `trim`/`trimStart`/`trimEnd`, `toUpper`/`toLower`,
  `replace`/`padStart`/`padEnd`/`repeatStr`, and the `isAscii…` classification family + `hasControl`.

The **opt-in** modules must be brought in with a `use` before their functions resolve:

- `std::io` — `readFile`, `writeFile`, `appendFile`, `readTextFile`, `writeTextFile`, `appendTextFile`, `deleteFile`, `rename`, `copyFile`, `mkdir`, `listDir`, `fileExists`, `isFile`, `isDir`, `fileSize`, `readLine`, `readAllStdin`.
- `std::env` — `getEnv`, `args`, `nanoTime`, `millisTime`.
- `std::process` — `run`, `runWith`, `runText`, `sh`.
- `std::math` — numeric functions: `sqrt`/`cbrt`/`pow`/`hypot`, `exp`/`ln`/`log2`/`log10`, the trig family
  (`sin`/`cos`/`tan`/`asin`/`acos`/`atan`/`atan2`), `abs`/`absInt`/`sign`/`signInt`, `gcd`/`lcm`, the generic
  `minOf`/`maxOf`/`clamp`, `isNaN`/`isInfinite`, the constants `PI`/`E`, and the fallible `toIntChecked`. (The
  rounding builtins `floor`/`ceil`/`trunc`/`round`/`roundHalfToEven` and `toInt` are always-available — no `use`.)

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
match appendTextFile(path, "!") {                          // now holds "payload!"
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

## 25. Quick reference: the standard library

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

**Collections**

| Function | Purpose |
|----------|---------|
| `array(n, init)` / `vec()` / `bytes()` | create an `Array` / `Vec` / `Bytes` |
| `push(c, x)` / `pop(c)` | append to / remove the last element of a `Vec` or `Bytes` (`pop` returns `Option`) |
| `c[i]` / `c[i] = x` | read / write an element of an `Array`, `Vec`, or `Bytes` by index (bounds-checked — aborts on out-of-range; assignment needs a `mut` binding) |
| `len(c)` | number of elements |
| `get(c, i)` | safe indexed read of an `Array` / `Vec` / `Bytes` — `Option` (`None` if out of range) |
| `m[k]` / `m[k] = v` | read (aborts if the key is missing) / insert-or-update a `Map` entry (assignment needs a `mut` binding) |
| `has(m, k)` / `get(m, k)` | map membership test / safe lookup (`Option`) |
| `delete(m, k)` | remove a map entry |
| `keys(m)` / `values(m)` | snapshot a map's keys / values |

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

- The grammar in `docs/alternative_typed_grammar.ebnf` is the precise reference for the syntax.
- A few features exist in the language's design space but are intentionally not part of this guide because they
  are not built yet (for example loop labels, `while let`, and a handful of extra iterator combinators like
  `rev` and `cycle`).
