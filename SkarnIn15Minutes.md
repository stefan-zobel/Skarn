# Skarn in 15 minutes

This is the **small core** of Skarn — the subset you need to write real programs. It is deliberately not the
whole language: traits beyond the basics, generics, `dyn`, iterators, the erasure types, and the full standard
library are all covered in the complete [README.md](LanguageIntroduction.md). Start here, reach for
that when you hit something this page does not mention.

If you know Java, C#, Kotlin, or a bit of Rust, almost everything below will look familiar. Run any snippet by
saving it to a `.skn` file:

```
static_vmrun.exe hello.skn
```

---

## 1. Hello, and bindings

```rust
println("Hello, Skarn!")

let x = 41           // immutable by default
let mut n = 0        // opt into mutation with `mut`
n = x + 1
println("n = ${n}")  // string interpolation: n = 42
```

`let` binds a value; `let mut` lets you reassign it later. There is **no `null`** — a possibly-absent value is
an `Option` (below).

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
early exits). Everything is an expression, including `if`.

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

Because `if` is an expression, you assign its result directly — there is no ternary `?:`.

## 5. Structs — group related data

```rust
struct Point { x: Int, y: Int }

let p = Point { x: 3, y: 4 }
println("${p.x}, ${p.y}")          // => 3, 4

// Structs are immutable unless the binding is `mut`; update by building a new one:
let moved = Point { x: p.x + 1, ..p }   // ..p copies the rest (here: y)
println(moved.y)                         // => 4
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

Fixed-size arrays (`array(n)`, `a[i]`) and byte buffers (`Bytes`) exist too; see the full guide.

One thing to internalize early: collections and structs are **reference values**. Binding one to a new name or
passing it to a function shares the *same* object, not a copy — so a `push` (or any `mut` mutation) is seen
through every name that refers to it. `mut` controls which *name* may mutate, not who else can see the change;
an immutable binding is not a private snapshot. Ask for `clone(x)` when you want an independent copy. The full
guide's memory model section spells this out.

## 9. Two ways to call things

You will see both, so know the split up front:

```rust
fn twice(n: Int) -> Int { n * 2 }

let a = twice(5)        // ordinary call
let b = 5 |> twice      // the PIPE: same call, reads left-to-right; great for chains
```

- The **pipe `|>`** threads a value into any free function: `xs |> filter(even) |> toVec`.
- The **dot `.`** calls a *type's own methods*: `rng.nextInt(1, 7)`, `text.stringify()`. Standard-library types
  that own state read this way.

A quick rule: standalone verbs (`map`, `filter`, `len`, `sum`) are `|>`; a type's own operations are `.`. The
full story — including how you give your own types methods with `impl` blocks — is in §16 of the complete guide.

## 10. A tiny complete program

```rust
enum Cmd { Inc, Dec, Reset }

fn apply(state: Int, c: Cmd) -> Int {
  match c {
    Inc   => state + 1,
    Dec   => state - 1,
    Reset => 0,
  }
}

let mut cmds: Vec[Cmd] = vec()
push(cmds, Inc)  push(cmds, Inc)  push(cmds, Dec)  push(cmds, Inc)

let mut state = 0
for c in cmds { state = apply(state, c) }
println("final = ${state}")     // => final = 2
```

---

## What comes next

You now have enough to be productive. When you need more, the full [LanguageIntroduction.md](LanguageIntroduction.md)
covers, in order of how often you will reach for it:

- **Traits and methods** (§16) — give your own types behavior and a `.method()` API; the complete call-form picture.
- **Generics** (§15) and **trait objects `dyn Trait`** (§17) — reusable code over many types.
- **Iterators** (§19) — lazy `map`/`filter`/`fold` pipelines.
- **Modules** (§20) and the **standard library** — `std::json`, `std::time`, `std::random`, `std::net`, and more.
- **The numeric model** (§6) and **strings as bytes** (§7) — the two places the details actually bite.

Skarn has a Rust-sized *feature* set, but it is layered: this page is the part you hold in your head, and the
rest you learn when a problem asks for it.
