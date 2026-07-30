# Skarn in 30 minutes

This is the **working core** of Skarn — enough to write and finish a real program, not just to read one. It is
deliberately not the whole language: traits and `impl` blocks, generics, `dyn`, the erasure types, and the full
standard library are covered in the complete [Skarn Guide](SkarnGuide.md). Start here,
reach for that when you hit something this page does not mention.

If you know Java, C#, Kotlin, or a bit of Rust, almost everything below will look familiar. Run any snippet by
saving it to a `.skn` file:

```
static_vmrun.exe hello.skn
```

Every example on this page is executed by the project's test harness, so what you read here is what the
compiler actually does.

---

## 1. Hello, bindings, and comments

```rust
println("Hello, Skarn!")

let x = 41           // a line comment
let mut n = 0        // opt into mutation with `mut`
n = x + 1
println("n = ${n}")  // string interpolation: n = 42

/* block comments
   span lines */
```

`let` binds a value; `let mut` lets you reassign it later. There is **no `null`** — a possibly-absent value is
an `Option` (§7). Top-level statements run top to bottom; there is no mandatory `main`.

## 2. The primitive types

`Int` (a 48-bit signed integer), `Double`, `Bool`, and `String`. Numbers do not implicitly mix — an `Int` used
where a `Double` is expected is widened for you, but you write the `.0` on a double literal:

```rust
let count: Int = 10
let ratio: Double = 3.0 / 4.0     // => 0.75
let ok: Bool = count > 5
let name: String = "Ada"
println("${name}: ${count}, ${ratio}, ${ok}")
```

Strings are text you print and concatenate with `+`; `"...${expr}..."` interpolates any value.

## 3. Functions

```rust
fn add(a: Int, b: Int) -> Int { a + b }

fn greet(who: String) -> () {     // `-> ()` means "returns nothing useful"
  println("hi, ${who}")
}

println(add(2, 3))   // => 5
greet("Ada")
```

The **last expression is the return value** — no `return` keyword needed for it (though `return` exists for
early exits). Parameter and return types are always written out; types *inside* a body are inferred.

## 4. Control flow (it is all expressions)

```rust
let sign = if n > 0 { "pos" } else if n < 0 { "neg" } else { "zero" }

let mut i = 0
while i < 3 {
  println("i = ${i}")
  i = i + 1
}

for c in "abc" {      // NOTE: iterating a String yields BYTES (Int 0..255), not characters
  println(c)          // => 97, 98, 99
}
```

Because `if` is an expression, you assign its result directly — there is no ternary `?:`. For text you almost
always want §9 rather than that byte loop.

## 5. Structs — group related data

```rust
struct Point { x: Int, y: Int }

let p = Point { x: 3, y: 4 }
println("${p.x}, ${p.y}")          // => 3, 4

// Structs are immutable unless the binding is `mut`; update by building a new one:
let moved = Point { x: p.x + 1, ..p }   // ..p copies the rest (here: y)
println(moved.y)                         // => 4

// When a local already has the field's name, write it once:
let x = 10
let y = 20
println(Point { x, y }.x)                // => 10   (same as Point { x: x, y: y })
```

## 6. Enums and `match` — the heart of the language

An `enum` is a value that is *one of several shapes*, each optionally carrying data. `match` takes them apart,
and the compiler checks you handled **every** case.

```rust
enum Shape {
  Circle(Double),
  Rect(Double, Double),
}

fn area(s: Shape) -> Double {
  match s {
    Circle(r)    => 3.14159 * r * r,
    Rect(w, h)   => w * h,
  }
}

println(area(Circle(2.0)))     // => 12.56636
println(area(Rect(3.0, 4.0)))  // => 12.0
```

This replaces both "class hierarchies" and "enums + a switch you might forget to update": leave a case out and
the program does not compile.

## 7. No null, no exceptions: `Option`, `Result`, and `?`

Absence is `Option[T]` (`Some(v)` or `None`). A fallible result is `Result[T, E]` (`Ok(v)` or `Err(e)`). Both
are ordinary enums you `match`:

```rust
fn half(n: Int) -> Option[Int] {
  if n % 2 == 0 { Some(n / 2) } else { None }
}

match half(10) {
  Some(v) => println("got ${v}"),   // => got 5
  None    => println("odd"),
}
```

When a function itself returns `Result`/`Option`, the **`?` operator** unwraps the success value or returns the
failure immediately — no nested matching:

```rust
fn parseAndHalf(s: String) -> Result[Int, String] {
  let n = parseInt(s)?      // if Err, return it now; else n is the Int
  Ok(n / 2)
}
```

**`?` needs a function to return from**, so it does not work at the top level of a file — writing
`let n = parseInt("10")?` there is a compile error (*"`?` on a Result requires the enclosing function to return
a Result"*). The usual shape is a `run` function that does the work and a top-level `match` that reports the
outcome; §13 shows it.

## 8. Collections you will use daily

```rust
// A growable vector.
let mut xs: Vec[Int] = vec()
push(xs, 10)  push(xs, 20)  push(xs, 30)
println(len(xs))     // => 3
println(xs[1])       // => 20
for v in xs { println(v) }

// A map (hash keys: Int / Double / Bool / String).
let mut ages: Map[String, Int] = #{}
ages["Ada"] = 36
ages["Bo"]  = 29
match get(ages, "Ada") { Some(a) => println(a), None => println("?") }   // => 36
```

`m[k]` reads a key that must exist (a missing one aborts the program); `get(m, k)` returns an `Option`, and
`getOr(m, k, dflt)` supplies a default. Fixed-size arrays (`array(n)`, `a[i]`) and byte buffers (`Bytes`) exist
too; see the full guide.

One thing to internalize early: collections and structs are **reference values**. Binding one to a new name or
passing it to a function shares the *same* object, not a copy — so a `push` (or any `mut` mutation) is seen
through every name that refers to it. `mut` controls which *name* may mutate, not who else can see the change;
an immutable binding is not a private snapshot. Ask for `clone(x)` when you want an independent copy. The full
guide's memory model section spells this out.

## 9. Working with text

A `String` is a sequence of **bytes**, so `len(s)` is a byte count and `for c in s` walks bytes. In practice you
rarely touch either — you split, trim, and search instead:

```rust
let raw = "  Ada, 36 , Berlin  "

println(len(raw))                     // => 20   (bytes, not characters)
println(trim(raw))                    // => Ada, 36 , Berlin
println(indexOf(raw, "Ada"))          // => 2    (byte index, -1 if absent)
println(slice("hello world", 0, 5))   // => hello
println(toUpper("abc"))               // => ABC

for field in split(trim(raw), ",") {
  println("[" + trim(field) + "]")    // => [Ada]  [36]  [Berlin]
}

let doc = "one\ntwo\nthree"
for ln in lines(doc) { println(ln) }  // => one  two  three
```

`split` and `lines` are **lazy** — they hand back an iterator, which is exactly what `for` wants. To keep the
pieces around, collect them into a `Vec` (§11):

```rust
let raw = "  Ada, 36 , Berlin  "
let fields = raw |> trim |> split(",") |> collect
println(len(fields))     // => 3
```

The other everyday helpers: `startsWith` / `endsWith` / `hasSubstr`, `replace`, `padStart` / `padEnd`,
`toLower`, `isEmpty`, `join`. For non-ASCII text, `chars(s)` iterates real Unicode code points instead of
bytes — that is the one place the byte model needs a conscious choice.

## 10. Three ways to call things

You will see all three, so know the split up front:

```rust
fn twice(n: Int) -> Int { n * 2 }

let a = twice(5)        // ordinary call
let b = 5 |> twice      // the PIPE: same call, reads left-to-right; great for chains
```

- The **pipe `|>`** threads a value into any free function: `x |> f |> g` is `g(f(x))`.
- The **dot `.`** calls a *type's own methods*: `rng.nextInt(1, 7)`, `doc.stringify()`. Standard-library types
  that own state read this way.
- **`Type::name(…)`** reaches something that belongs to the type but has no receiver yet — above all its
  constructors: `Rng::fromSeed(42)`, `Json::parse(text)`, `Regex::compile(pattern)`.

A quick rule: standalone verbs (`map`, `filter`, `len`, `sum`) are `|>`; a type's own operations are `.`; and
you *get* one of those types from `Type::something(…)`. The full story — including how you give your own types
methods with `impl` blocks — is in §16 of the complete guide.

## 11. Pipelines: iterators

`map` / `filter` / `sum` and friends work on an **iterator**, not directly on a container. So a pipeline over a
`Vec` starts by lifting it with `intoIter`, and ends with a terminal that turns it back into a value —
`collect` (into a `Vec`), `sum`, `count`, `fold`:

```rust
fn even(n: Int) -> Bool { n % 2 == 0 }
fn dbl(n: Int)  -> Int  { n * 2 }

let mut xs: Vec[Int] = vec()
push(xs, 1)  push(xs, 2)  push(xs, 3)  push(xs, 4)  push(xs, 5)

println(toString(xs |> intoIter |> filter(even) |> collect))   // => [2, 4]
println(xs |> intoIter |> map(dbl) |> sum)                     // => 30
```

Forgetting `intoIter` is the single most common early mistake; the error names it (*"expected
`dyn Iterator[T]`, found `Vec[Int]`"*).

Some sources are already iterators and need no lift — `range`, and the `split`/`lines` of §9:

```rust
fn even(n: Int) -> Bool { n % 2 == 0 }
fn dbl(n: Int)  -> Int  { n * 2 }

println(range(0, 10) |> filter(even) |> count)          // => 5
println(toString(range(1, 4) |> map(dbl) |> collect))   // => [2, 4, 6]
```

Pipelines are **lazy**: nothing runs until the terminal pulls, and elements flow through one at a time rather
than building an intermediate `Vec` per stage. Sorting is separate and eager — `sorted(v)` returns a new sorted
`Vec`, `sort(v)` sorts a `mut` one in place:

```rust
let words = "b a c" |> split(" ") |> collect
println(toString(sorted(words)))   // => ["a", "b", "c"]
```

## 12. Modules and `use`

One `.skn` file is one **module**. Only items marked `pub` leave it; everything else is private to the file.

```rust
// geo.skn
pub fn manhattan(x: Int, y: Int) -> Int { x + y }
fn helper() -> Int { 1 }                          // private to geo
```

```rust
// main.skn
import geo               // load the module (geo.skn, next to this file)
use geo::manhattan       // bring one name into scope unqualified

println(manhattan(3, 4))       // => 7
println(geo::manhattan(3, 4))  // or reach it qualified, without the `use`
```

The same mechanism governs the **standard library**, and this is the part worth knowing before you need it.
Three modules are always in scope with no `use` at all — `std::core` (`Option`, `Result`), `std::iter`
(iterators, sorting), and `std::string` (the text helpers of §9). That is why everything on this page so far
has just worked.

**Everything else is opt-in and has to be `use`d first**: `std::io` (files), `std::env` (arguments,
environment, clocks), `std::process`, `std::math`, `std::json`, `std::time`, `std::random`, `std::set`,
`std::bytes`, `std::cli`, `std::hash`, `std::net`, `std::regex`.

```rust
use std::math::*
println(sqrt(9.0))   // => 3.0
```

Without that `use`, the same line does not compile — and the error names the module you are missing:

```
error: native 'sqrt' requires `use std::math::*`
error: unknown variable 'readTextFile'; it is in 'std::io' -- add `use std::io::*`
error: unknown type 'DateTime'; it is in 'std::time' -- add `use std::time::*`
```

That hint appears for functions, types, and traits alike, and for your own modules as well as `std` (where it
tells you to `import` the module too, if you have not). It is only ever offered for a name some module really
does export — a genuine typo stays a plain "unknown", with nothing invented.

## 13. Files, arguments, and the outside world

With `use` in hand, the outside world opens up. This is also where `?` earns its keep — note the `run`
function, which exists precisely so `?` has something to return from (§7):

```rust
use std::io::*

fn run() -> Result[Int, String] {
  let text = readTextFile("data.txt")?     // an Err short-circuits out of `run`
  let mut count = 0
  for _ in lines(text) { count = count + 1 }
  Ok(count)
}

match run() {
  Ok(n)  => println("${n} lines"),
  Err(e) => println("error: ${e}"),
}
```

Nothing here can throw, and nothing is silently ignored: a failure is a value that you either handle or `?`
passes upward. The rest of the everyday surface:

```rust
use std::io::*
use std::env::*

let _ = writeTextFile("out.txt", "hello")    // -> Result[(), String]
let _ = appendTextFile("out.txt", " again")
println(toString(fileExists("out.txt")))

let argv = toVec(args())                     // the command-line arguments
match getEnv("PATH") { Some(p) => println(len(p) > 0), None => println("unset") }
```

Also available: `readFile`/`writeFile` for raw `Bytes`, `deleteFile` / `rename` / `copyFile` / `mkdir` /
`listDir`, `readLine()` and `readAllStdin()` for standard input, and `std::process` to run an external command.
Nearly every one returns a `Result`, so the pattern above is the pattern everywhere.

## 14. A tiny complete program

Word frequencies — text handling, a `Map` with a default, sorting, and interpolation together:

```rust
fn wordCounts(text: String) -> Map[String, Int] {
  let mut counts: Map[String, Int] = #{}
  for w in split(text, " ") {
    let word = trim(w)
    if len(word) > 0 {
      counts[word] = getOr(counts, word, 0) + 1
    }
  }
  counts
}

let text = "the quick fox the lazy dog the fox"
let counts = wordCounts(text)

for w in sorted(toVec(keys(counts))) {
  println("${w}: ${counts[w]}")
}
// => dog: 1
// => fox: 2
// => lazy: 1
// => quick: 1
// => the: 3
```

---

## What comes next

You now have enough to finish a program, not just start one. When you need more, the full
[Skarn Guide](SkarnGuide.md) covers, in order of how often you will reach for it:

- **Traits and methods** (§16) — give your own types behavior and a `.method()` API; the complete call-form picture.
- **Generics** (§15) and **trait objects `dyn Trait`** (§17) — reusable code over many types.
- **Iterators in full** (§19) — the remaining stages and terminals, and writing your own iterator.
- **The standard library** (§20 and the reference in §26) — `std::json`, `std::time`, `std::random`,
  `std::regex`, `std::net`, and every function's signature.
- **The numeric model** (§6), **strings as bytes** (§7), and **the memory & cost model** (§25) — the three
  places the details actually bite.

Skarn has a Rust-sized *feature* set, but it is layered: this page is the part you hold in your head, and the
rest you learn when a problem asks for it.
