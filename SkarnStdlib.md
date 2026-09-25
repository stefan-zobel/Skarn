# The Skarn Standard Library

This is the reference for everything the standard library provides: every function and type of every `std`
module, with what it takes, what it returns and what it does, and a worked example for most modules. It is a
companion to [The Skarn Language Guide](SkarnGuide.md), which teaches the language. How modules, `use` and
`pub` work, and why the library comes in three call styles, is explained there, in
[§20](SkarnGuide.md#20-modules).

As in the guide, every code block here is a real program that compiles and runs, and the project's test
harness checks it. A `// => ...` comment is a line the program prints; the network examples are type-checked
but not run.

## How to read this reference

- **Part 1** is what every program can call with no `use`: the built-in functions and the three *ring*
  modules `std::core`, `std::iter` and `std::string`, which every module imports automatically.
- **Part 2** is the opt-in modules. Their names do not resolve until you `use` the module — `use std::json::*`,
  or one name at a time, `use std::json::{Json, JsonEntry}`. That is also what gates the natives: a program
  that never writes `use std::io` cannot touch the filesystem.
- A library function is called as `f(x)`, and `x |> f` is the same thing written left to right. A method of a
  type the library defines is called as `x.f()`, and most of its constructors as `Type::name(…)`.
- In the tables, `→` names what a function returns. The fallible ones return a `Result` or an `Option`.

### Contents

**Part 1 — always available**

1. [Core and output](#1-core-and-output)
2. [Numbers](#2-numbers)
3. [Strings](#3-strings)
4. [Unicode code points](#4-unicode-code-points)
5. [Collections](#5-collections)
6. [Option and Result](#6-option-and-result)
7. [Iterators and sorting](#7-iterators-and-sorting)

**Part 2 — opt-in modules**

8. [`std::io`: files and standard input](#8-stdio-files-and-standard-input)
9. [`std::env`: arguments, environment, clocks](#9-stdenv-arguments-environment-clocks)
10. [`std::process`: running programs](#10-stdprocess-running-programs)
11. [`std::math`](#11-stdmath)
12. [`std::bytes`: binary I/O](#12-stdbytes-binary-io)
13. [`std::json`](#13-stdjson)
14. [`std::random`](#14-stdrandom)
15. [`std::set`](#15-stdset)
16. [`std::time`: dates and times](#16-stdtime-dates-and-times)
17. [`std::cli`: command-line arguments](#17-stdcli-command-line-arguments)
18. [`std::hash`](#18-stdhash)
19. [`std::net`: TCP and HTTP](#19-stdnet-tcp-and-http)
20. [`std::poll`: non-blocking I/O](#20-stdpoll-non-blocking-io)
21. [`std::task`: parallel tasks](#21-stdtask-parallel-tasks)
22. [`std::actor`](#22-stdactor)
23. [`std::supervisor`](#23-stdsupervisor)
24. [`std::regex`](#24-stdregex)
25. [`std::log`](#25-stdlog)

---

## Part 1 — always available

Nothing in this part needs a `use`. Any of these names can also be reached explicitly as `std::name`, which
helps where a definition of your own shadows it.

## 1. Core and output

| Function | Purpose |
|----------|---------|
| `print(a, ...)` / `println(a, ...)` | write values to standard output (`println` adds a newline) |
| `toString(x)` | render any value as text |
| `panic(msg)` | abort the program with a message |
| `assert(cond, msg)` | abort if `cond` is false |
| `gcStats()` / `gcResetStats()` | the garbage collector's counters since the start or the last reset → `GcStats` / clear them, so a region can be measured: `gcResetStats()`, the code, then `gcStats()` |

`print` and `println` accept any value and several arguments; the guide shows them in
[§22](SkarnGuide.md#printing). A `GcStats` is a plain struct of `Double` counters — `collections`,
`objectsAlloced`, `bytesAlloced`, `fromUsedSum`, `survivorsSum`, `gcNanosTotal`, `gcNanosMax` and
`growEvents`; a `Double` holds them exactly, where a 48-bit `Int` might not.

Three marker traits of `std::core` have no methods and cost nothing at run time; they only restrict what
a program may write. The guide lists every built-in trait in
[Built-in traits at a glance](SkarnGuide.md#built-in-traits-at-a-glance).

| Trait | Purpose |
|----------|---------|
| `Hashable` | what may be a `Map` key or a `Set` element: `Int`, `Double`, `Bool`, `String`. Sealed — a program cannot add a type |
| `MustUse` | dropping a value of such a type unused is a warning (fatal under `--strict`); `let _ = value` discards it on purpose. Open: `impl MustUse for Outcome {}` on a type the program owns |
| `Sendable` | what may be copied to another task or actor: plain data, no function values, `dyn` values or handles. Derived by the checker and sealed; generic code writes `M: Sendable` |

## 2. Numbers

The numeric model itself is [§6 of the guide](SkarnGuide.md#6-the-numeric-model).

| Function | Purpose |
|----------|---------|
| `toInt(d)` | `Double` → `Int` (saturating, never aborts) |
| `toDouble(i)` | `Int` → `Double` (exact; a `Double` argument is an error) |
| `floor(d)` / `ceil(d)` / `trunc(d)` | round `Double` → `Double` toward −∞ / +∞ / zero |
| `round(d)` / `roundHalfToEven(d)` | round `Double` → `Double`, ties away from zero / ties to even |
| `parseInt(s)` | `String` → `Result[Int, String]` |
| `parseDouble(s)` | `String` → `Result[Double, String]` |
| `ordinal(e)` | Int-backed `enum` → its `Int` discriminant ([§12](SkarnGuide.md#integer-backed-enums); the pinned value, not the position) |

The checked conversion, `toIntChecked`, is in [`std::math`](#11-stdmath).

## 3. Strings

A `String` is a byte string; [§7 of the guide](SkarnGuide.md#7-strings) explains the model, and
[Building strings efficiently](SkarnGuide.md#building-strings-efficiently--stringbuilder) the
`StringBuilder`.

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

`split`, `lines` and `words` take a string apart lazily; the guide covers them with the iterators, in
[Streaming text](SkarnGuide.md#streaming-text-split-lines-and-words). Every string function:

| Function | Purpose |
|----------|---------|
| `len(x)` | length (bytes of a `String`, element count of a collection) |
| `slice(s, start, end)` | substring over the byte range `[start, end)` |
| `sliceBytes(b, start, end)` | the same over a `Bytes` already in hand — clamped to `[0, len]`, one bulk copy. Slicing many ranges out of one string this way is linear; `slice` converts the whole `String` on every call |
| `charStr(byte)` | a one-byte `String` from an `Int` byte value |
| `charAt(s, i)` / `isEmpty(s)` | the byte at index `i` (traps if out of range) / whether `s` is empty |
| `indexOf(s, sub)` / `lastIndexOf(s, sub)` | first / last byte index of `sub`, or `-1` if absent |
| `startsWith(s, p)` / `endsWith(s, p)` / `hasSubstr(s, sub)` | prefix / suffix / containment test (`Bool`) |
| `trim(s)` / `trimStart(s)` / `trimEnd(s)` | strip ASCII whitespace from both ends / start / end |
| `toUpper(s)` / `toLower(s)` | ASCII case conversion (bytes `≥ 0x80` unchanged) |
| `replace(s, from, to)` | replace **all** occurrences of `from` (empty `from` is a no-op) |
| `padStart(s, width, padByte)` / `padEnd(s, width, padByte)` | pad to a byte `width` with a single `padByte` |
| `repeatStr(s, n)` | `s` repeated `n` times (`n ≤ 0` → `""`) |
| `isAsciiControl(c)` / `isAsciiDigit(c)` / `isAsciiWhitespace(c)` / `isAsciiAlpha(c)` / `isAsciiAlphanumeric(c)` / `isAsciiUpper(c)` / `isAsciiLower(c)` / `isAsciiPrintable(c)` | ASCII classify a byte `Int` → `Bool` (`≥ 128` → `false`) |
| `hasControl(s)` | `Bool` — does `s` contain any ASCII control byte |
| `toBytes(s)` / `fromBytes(b)` | convert between `String` and `Bytes` |
| `appendBytes(b, src)` | append all of a `String` or `Bytes` to `b` in one copy; `b` must be `mut`, and is returned |
| `split(s, sep)` | lazily split into fields on a literal separator (empties kept; inverse of `join`) |
| `lines(s)` | lazily split into lines (terminator semantics; `CRLF` handled) |
| `words(s)` | lazily split on RUNS of ASCII whitespace (no empty field ever; edges contribute none) |
| `stringBuilder()` / `stringBuilderCap(n)` | a `StringBuilder` accumulator over `Bytes` (empty / pre-sized to `n`) |
| `sb.append(s)` / `sb.appendByte(c)` | append a `String` / one byte to a `StringBuilder` (mutates in place; returns `sb`, so calls chain) |
| `sb.build()` / `sb.len()` | the accumulated `String` / its current byte length |
| `format(x, spec)` | the `Format` trait: format a scalar per a `${x:spec}` specifier (width/align/precision/base, sign `+`, `#` prefix, scientific `e`/`E`) → `String` |

## 4. Unicode code points

The opt-in code-point layer over the default byte model, in `std::string` and `std::iter` — `for c in s` still
iterates bytes. The guide explains it in [Char and UTF-8 code points](SkarnGuide.md#char-and-utf-8-code-points).

| Function | Purpose |
|----------|---------|
| `chars(s)` | lazily iterate the UTF-8 code points of `s` as `Char`s (a malformed byte decodes to U+FFFD, so it never fails) |
| `charCount(s)` | number of code points (O(n); note `len(s)` is the byte count) |
| `nthChar(s, i)` | the i-th code point → `Option[Char]` (O(n) — a `String` is not directly indexable) |
| `codePoint(c)` | a `Char`'s `Int` code point |
| `codePointToStr(cp)` | encode an `Int` code point → its UTF-8 `String` (invalid/surrogate → U+FFFD) |
| `fromChars(it)` | build a `String` from an iterator of `Char` (`fromChars(chars(s)) == s` for valid UTF-8) |
| `sb.appendCodePoint(cp)` | append a code point's UTF-8 to a `StringBuilder` (mutates in place; returns `sb`) |

## 5. Collections

The collection types themselves are [§14 of the guide](SkarnGuide.md#14-collections).

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
| `getOrPut(m, k, dflt)` | the value at `k`; on a miss, insert `dflt` first (needs a `mut` binding) |
| `upsert(m, k, dflt, f)` | on a hit store `f(current)`, on a miss store `dflt` as is (`f` is not applied); returns the stored value (needs a `mut` binding) |

## 6. Option and Result

How they are used, and the `?` operator, are [§18 of the guide](SkarnGuide.md#18-option-result-and-the--operator).

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

## 7. Iterators and sorting

What is lazy and what is not, and how to write an iterator of your own, is
[§19 of the guide](SkarnGuide.md#19-iterators).

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
| `collect(it)` | gather the rest of an **iterator** into a `Vec` |
| `toVec(x)` | an eager `Vec` snapshot of an **`Iterable`** — a `Vec`, `Array`, `List`, `Bytes`, `Map` (as pairs) or your own `impl Iterable` type; not an iterator (use `collect`) |
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

**The traits behind them** *(`std::iter`; implement them for your own types)*

| Function | Purpose |
|----------|---------|
| `it.next()` | the `Iterator[T]` trait: the next element → `Option[T]`, `None` when the iterator is done (needs `mut`) |
| `intoIter(x)` | the `IntoIterator[T]` trait: a fresh `dyn Iterator[T]` over `x`; what a lazy `for` and the stages call |
| `iter(x)` | the `Iterable[T]` trait: an eager `Vec[T]` snapshot, which `toVec` returns |
| `a.lessThan(b)` | the `Ord` trait: the total order `sorted`, `sort`, `min` and `max` use |

---

## Part 2 — opt-in modules

Each section below starts with the `use` that brings the module in. The guide's
[standard library overview](SkarnGuide.md#the-standard-library) lists them all on one page.

## 8. `std::io`: files and standard input

Files — read / write / append, copy, rename, delete, `mkdir`, `listDir`, stat — and standard input.
`use std::io::*`

| Function | Purpose |
|----------|---------|
| `readFile(path)` / `writeFile(path, bytes)` / `appendFile(path, bytes)` | whole-file read / write / append (`Result`) |
| `readTextFile` / `writeTextFile` / `appendTextFile` | the same as `String` (byte wrappers over the above) |
| `fileExists` / `isFile` / `isDir` / `fileSize` | entry / regular-file / directory query, byte length |
| `deleteFile` / `rename` / `copyFile` / `listDir` / `mkdir` | delete / move / copy / list / make directory |
| `readLine()` / `readAllStdin()` | read a line / all of standard input |

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
println("isFile: " + isFile(path))                        // => isFile: true   (a regular file, not a directory)
```

`fileExists` is `test -e` (any entry); `isFile` and `isDir` are the finer file-vs-directory queries.
`fileSize(path)` returns the byte length as a `Result[Int, String]`. `rename(from, to)` and
`copyFile(from, to)` move / copy a file (both `Result[(), String]`); `copyFile` fails if the destination
already exists.

### Standard input

`readLine()` reads one line as an `Option[String]` (`None` at end of input); `readAllStdin()` reads everything
to end of input as one `String`.

## 9. `std::env`: arguments, environment, clocks

Command-line arguments, environment variables, and the clocks `nanoTime` / `millisTime`. `use std::env::*`

| Function | Purpose |
|----------|---------|
| `args()` / `getEnv(name)` | command-line arguments / an environment variable (`Option`) |
| `nanoTime()` / `millisTime()` | monotonic timer / wall-clock milliseconds |

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

### Clocks

`nanoTime()` is a monotonic timer (only differences between readings are meaningful); `millisTime()` is
wall-clock milliseconds since the Unix epoch.

```rust
use std::env::*

let t0 = nanoTime()
let t1 = nanoTime()
println("elapsed >= 0: " + (t1 - t0 >= 0))   // => elapsed >= 0: true
```

## 10. `std::process`: running programs

Run an external command and capture its output, and ask which platform you are on. `use std::process::*`

| Function | Purpose |
|----------|---------|
| `run(argv)` / `runText(argv)` / `sh(cmdline)` | spawn a process and capture its output (`sh` goes through the platform's shell) |
| `runWith(argv, input)` | as `run`, with `input: Bytes` fed to the child's standard input |

`run`, `runWith` and `sh` return a `ProcessOutput` — `stdout` and `stderr` as `Bytes`, and `exitCode` —
and `runText` a `ProcessText`, the same with both streams decoded to `String`.
| `currentOs()` | which platform the program is running on (`Os::Windows` / `Os::MacOS` / `Os::Other`) |

### Running processes

`run(argv)` runs a child process and returns a `Result` describing its output and exit code; `runText(argv)`
is the same with the output decoded to `String`. With `run` you name the program yourself, so the argv is
yours to get right on each platform. `sh(cmdline)` hands the whole command line to the platform's shell
instead — `cmd /c` on Windows, `/bin/sh -c` elsewhere — so one call works on both.

A child's output ends with the line terminator the shell wrote (`\r\n` on Windows, `\n` elsewhere); `trim`
is the usual way to get rid of it.

```rust
use std::process::*

match sh("echo from-child") {
    Ok(out) => println("child said: " + trim(fromBytes(out.stdout))),   // => child said: from-child
    Err(e)  => println("spawn failed: " + e)
}
```

Only a *failure to start* is an `Err` — a program that runs and exits non-zero is an `Ok` whose `exitCode`
says so.

### Which platform am I on?

`currentOs()` answers with the enum `Os`, so you match on it:

```rust
use std::process::{Os, currentOs}

let label = match currentOs() {
    Os::Windows => "Windows",
    Os::MacOS   => "macOS",
    Os::Other   => "another platform"
}
println("running on " + label)
```

`Os::Other` covers everything that is neither: Skarn is built and tested on Windows x64 and macOS on Apple
Silicon, and rather than guess at a third platform's name it puts them all in one arm.

## 11. `std::math`

Roots, powers, logarithms, trigonometry, `gcd`/`lcm`, generic `minOf`/`maxOf`/`clamp`, `PI`/`E`, and the
fallible `toIntChecked`. The rounding builtins `floor`/`ceil`/`trunc`/
`round`/`roundHalfToEven`, `toInt` and `toDouble` are **always** available — no `use`. `use std::math::*`

| Function | Purpose |
|----------|---------|
| `sqrt` / `cbrt` / `pow(x,y)` / `hypot(x,y)` | roots and powers |
| `toIntChecked(d)` | `Double` → `Result[Int, String]` (`Err` on NaN / ∞ / overflow); `toInt` saturates instead |
| `exp` / `ln` / `log2` / `log10` | exponential and logarithms (`ln` = natural log) |
| `sin` / `cos` / `tan` / `asin` / `acos` / `atan` / `atan2(y,x)` | trigonometry |
| `abs` / `absInt` / `sign` / `signInt` | magnitude and sign (`Double` and `Int` forms) |
| `gcd(a,b)` / `lcm(a,b)` | integer gcd / lcm |
| `minOf(a,b)` / `maxOf(a,b)` / `clamp(x,lo,hi)` | scalar min/max/clamp, generic over any ordered type |
| `isNaN(x)` / `isInfinite(x)` | IEEE predicates |
| `PI` / `E` | constants |

## 12. `std::bytes`: binary I/O

A little-endian binary reader/writer over `Bytes`, with varints and length-prefixed strings.
`use std::bytes::*`

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

## 13. `std::json`

A JSON parser and serializer over a `Json` value tree. `use std::json::*`

| Function | Purpose |
|----------|---------|
| `Json::parse(s)` | parse JSON text → `Result[Json, String]` |
| `j.stringify()` / `j.stringifyPretty(indent)` | serialize a `Json` to compact / indented text |
| `j.stringifyChecked()` | serialize → `Result[String, String]` (`Err` on a non-finite number) |
| `Json::Obj(entries)` / `JsonEntry { key, val }` | build an object from an ordered `Vec[JsonEntry]`; the other cases build the rest: `Json::Arr(v)`, `Json::Text(s)`, `Json::Integer(n)`, `Json::Number(d)`, `Json::Boolean(b)`, `Json::Null` |
| `j.getField(key)` / `j.at(i)` | look up an object field / array element → `Option[Json]` |
| `j.asInt()`/`j.asDouble()`/`j.asStr()`/`j.asBool()`/`j.asArr()`/`j.asObj()`/`j.asMap()`/`j.isNull()` | extract a `Json` case (each `Option`, except `isNull` → `Bool`; `asArr`/`asObj` hand back the **live** payload `Vec`, not a copy) |

A document round-trips through its `Json` value tree:

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

The same cases **build** a document. An object is `Json::Obj` over a `Vec[JsonEntry]`, where
`JsonEntry { key: String, val: Json }` is one field — a `Vec` rather than a `Map`, so the key order you push is
the order `stringify` writes (and `Json::parse` keeps the document's order the same way):

```rust
use std::json::*

let mut tags: Vec[Json] = vec()
push(tags, Json::Text("admin"))
push(tags, Json::Text("ops"))

let mut fields: Vec[JsonEntry] = vec()
push(fields, JsonEntry { key: "name", val: Json::Text("Ada") })
push(fields, JsonEntry { key: "age", val: Json::Integer(36) })
push(fields, JsonEntry { key: "tags", val: Json::Arr(tags) })
println(Json::Obj(fields).stringify())   // => {"name":"Ada","age":36,"tags":["admin","ops"]}
```

## 14. `std::random`

A seedable PRNG (xoshiro128\*\*) — **deterministic** (same seed → same stream) and **not cryptographically
secure**. It pulls no entropy of its own, so for an unpredictable seed pass one in (`use std::env`, then
`Rng::fromSeed(nanoTime())`). `use std::random::*`

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

Seeded for reproducibility, it rolls a die and shuffles a deck:

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

## 15. `std::set`

A hash `Set[T]` with the usual set algebra. `T` must be `Hashable`; a `Set` is `IntoIterator`, so `for x in s`
and the lazy combinators work — in hash order. `use std::set::*`

| Function | Purpose |
|----------|---------|
| `Set::new()` / `Set::fromVec(v)` | an empty `Set[T]` — `T` comes from how the set is used (`let mut s = Set::new()` then `s.insert(3)`), or from an annotation (`let s: Set[Int] = Set::new()`) when nothing uses it / a set of the distinct elements of a `Vec` |
| `s.insert(x)` / `s.remove(x)` | add / delete `x` → `Bool` (was-new / was-present); `s` must be `mut` |
| `s.isMember(x)` / `s.size()` | membership → `Bool` / cardinality → `Int` (both O(1)) |
| `a.union(b)` / `a.intersect(b)` / `a.difference(b)` | the combined / common / left-only set |
| `a.isSubset(b)` / `a.isDisjoint(b)` | `a ⊆ b` / `a ∩ b = ∅` → `Bool` |

It deduplicates and answers membership and set-algebra questions:

```rust
use std::set::*

let mut v: Vec[Int] = vec()
push(v, 1) push(v, 2) push(v, 2) push(v, 3)
let a = Set::fromVec(v)               // {1, 2, 3}
println(a.size())              // => 3
println(a.isMember(2))         // => true

let mut w: Vec[Int] = vec()
push(w, 2) push(w, 3) push(w, 4)
let b = Set::fromVec(w)
println(a.intersect(b).size()) // => 2   ({2, 3})
```

## 16. `std::time`: dates and times

Strict-UTC dates and times at millisecond precision — `Instant`, `Duration`, `DateTime`, `Stopwatch`. An
`Instant` spans roughly year −2490..6429; sub-millisecond precision and time zones are out of scope for v1.
`use std::time::*`

| Function | Purpose |
|----------|---------|
| `now()` / `instantFromMillis(ms)` / `fromDateTime(dt)` | build an `Instant` (free constructors) |
| `t.toEpochMillis()` | the `Instant` as epoch milliseconds |
| `t.toDateTime()` | `Instant` → decomposed civil `DateTime` (UTC) |
| `dateTime(y,mo,d,h,mi,s,ms)` / `dateOnly(y,mo,d)` | a strictly-validated `DateTime` → `Option` (free; bad fields → `None`) |
| `dt.year` / `dt.month` / `dt.day` / `dt.hour` / `dt.minute` / `dt.second` / `dt.milli` | the `DateTime` fields (plain `Int`s; `month` 1–12) |
| `dt.toIso()` / `parseIso(s)` | ISO-8601 `YYYY-MM-DDThh:mm:ss.sssZ` ↔ `DateTime` (`dt.toIso()` method, `parseIso` free → `Result`) |
| `dt.weekday()` | ISO weekday, Monday=1 .. Sunday=7 |
| `isLeapYear(y)` / `daysInMonth(y, mo)` | calendar predicates (year-based → free) |
| `millis`/`seconds`/`minutes`/`hours`/`days` `(n)` | build a `Duration` (free constructors) |
| `d.inMillis()`/`d.inSeconds()`/`d.inMinutes()`/`d.inHours()`/`d.inDays()` | a `Duration` as an `Int` (truncating) |
| `t.addDuration(d)` / `t.subDuration(d)` / `a.durationBetween(b)` | `Instant` arithmetic |
| `a.add(b)` / `a.sub(b)` / `d.scale(k)` / `d.negate()` | `Duration` algebra |
| `a.isBefore(b)` / `a.isAfter(b)` | order two `Instant`s → `Bool` |
| `startStopwatch()` / `sw.elapsedNanos()` / `sw.elapsedMillis()` | a monotonic stopwatch (`startStopwatch` free) |
| `sleep(ms)` | wait `ms` milliseconds. Only this actor or task waits — each has a thread of its own |

It decomposes an instant, does duration arithmetic, and round-trips ISO-8601:

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

A `DateTime` is a plain struct of `Int` fields — `year`, `month` (1–12), `day` (1–31), `hour`, `minute`,
`second` and `milli` — so you read them directly (`dt.month`) and build one through `dateTime(…)` /
`dateOnly(…)`, which return `None` for a date that does not exist:

```rust
use std::time::*

let d = unwrap(dateOnly(2024, 2, 29))
println("${d.day}.${d.month}.${d.year}, weekday ${d.weekday()}")   // => 29.2.2024, weekday 4
println(isNone(dateOnly(2023, 2, 29)))                             // => true
```

## 17. `std::cli`: command-line arguments

A spec-free command-line parser: `--name=value` options, `--name` / `-abc` flags, `--`, positionals. A
space-separated option value like `--out file` is **not** supported — write `--out=file`. `use std::cli::*`

| Function | Purpose |
|----------|---------|
| `CliArgs::parse(argv)` | parse anything iterable of `String` — an `Array` from `args()` or a `Vec` you built — → `CliArgs` |
| `c.hasFlag(name)` | was `--name` / short `-n` present? → `Bool` |
| `c.getOpt(name)` / `c.getOptOr(name, dflt)` | the value of `--name=value` → `Option[String]` / with a default |
| `c.numPositionals()` / `c.positionalAt(i)` | positional count → `Int` / the i-th positional → `Option[String]` |

It turns a raw argument vector into flags, options, and positionals:

```rust
use std::cli::*

let mut argv: Vec[String] = vec()
push(argv, "build")
push(argv, "--opt=O2")
push(argv, "-v")
push(argv, "main.skn")

let c = CliArgs::parse(argv)                    // real programs: CliArgs::parse(args())
println(c.getOptOr("opt", "O0"))           // => O2
println(toString(c.hasFlag("v")))          // => true
println(toString(c.numPositionals()))      // => 2   ("build", "main.skn")
```

## 18. `std::hash`

CRC-32 and MurmurHash3 (fast, **not** secure) plus SHA-256 (cryptographic) and hex encoding.
`use std::hash::*`

| Function | Purpose |
|----------|---------|
| `crc32(bytes)` / `crc32Str(s)` | CRC-32 (IEEE/zlib) checksum of a `Bytes` / `String` → non-negative 32-bit `Int` (not secure) |
| `murmur3(bytes, seed)` / `murmur3Str(s, seed)` | MurmurHash3 x86_32 of a `Bytes` / `String` with a 32-bit `seed` → non-negative 32-bit `Int` (fast, non-cryptographic) |
| `sha256(bytes)` / `sha256Str(s)` | SHA-256 (FIPS 180-4) → the raw 32-byte digest as `Bytes` |
| `sha256Hex(bytes)` / `sha256HexStr(s)` | SHA-256 as a 64-char lowercase hex `String` |
| `toHex(bytes)` | lowercase hex encoding of any `Bytes` (2 chars/byte) |
| `fromHex(s)` | the inverse: decode hex (either case) → `Result[Bytes, String]`; an odd digit count or a non-hex byte is an `Err` |

A checksum of a string:

```rust
use std::hash::*

println(toString(crc32Str("123456789")))   // => 3421780262
```

## 19. `std::net`: TCP and HTTP

Blocking TCP (IPv4 + IPv6) and a minimal HTTP/1.0 `httpGet` — one connection at a time per thread, and a
connection can be handed to an actor. **Plaintext only** (no TLS, so `http://` not `https://`); close sockets
explicitly. A connection or a listener can also be handed to the runtime to read or accept (active
mode), so an actor waits on its socket and its inbox at once. `use std::net::*`

| Function | Purpose |
|----------|---------|
| `connect(host, port)` | open a client connection → `Result[TcpConn, String]` (IPv4/IPv6, name or literal; free) |
| `c.send(bytes)` / `c.sendStr(s)` | send a whole buffer / string → `Result[(), String]` |
| `c.recv(n)` | receive up to `n` bytes → `Result[Bytes, String]` (an **empty `Bytes` means EOF**) |
| `c.recvLine()` / `c.recvAll()` | read one `\n`-line → `Result[Option[String], String]` / drain to EOF → `Result[Bytes, String]` (both need `mut c`) |
| `c.setTimeout(ms)` / `c.close()` | recv/send timeout (0 = block) / close the connection → `Result[(), String]` |
| `listen(port)` (free) / `l.accept()` / `l.close()` | server: bind+listen → `TcpListener`; block for a client → `TcpConn`; stop listening |
| `l.localPort()` | the port actually bound → `Result[Int, String]`. Pass **`listen(0)`** to let the OS pick a free one and read it back here — safer than naming a fixed port, which may already be in use |
| `httpGet(host, port, path)` | a minimal HTTP/1.0 GET → `Result[HttpResponse, String]` (`.status: Int`, `.body: String`; free) |
| `c.handOff()` / `h.take()` | give a connection to another actor: detach it → `Result[SocketHandOff, String]`, a ticket that can be sent; redeem the ticket in the receiver, once → `Result[TcpConn, String]`. After the hand-off every use of the old `TcpConn` returns `Err` |

**Active sockets.** Only an actor or the main program can activate a socket, neither half can be sent to
another actor, and the runtime closes an active socket when its actor ends. [Actors in
Skarn](SkarnActors.md#17-a-connection-that-also-listens-to-its-inbox) shows both at work.

| Function | Purpose |
|----------|---------|
| `c.activate(framing, capacity)` | hand the READING of a connection to the runtime → `Result[(ActiveConn, SockEvents), String]`: write through the `ActiveConn`, receive what arrives from the `SockEvents`, an inbox of at most `capacity` events — while it is full the runtime stops reading and TCP holds the peer back. The `TcpConn` is dead from then on |
| `Framing::Raw` / `Framing::Lines(max)` | how the input is cut into events: the chunks as they are read / every line without its terminator; a line longer than `max` bytes ends the stream with `Failed` |
| `out.send(bytes)` / `out.sendStr(s)` / `out.close()` | write to an `ActiveConn` → `Result[(), String]` / close it; later sends are an `Err`, events not received yet are dropped, and closing twice is fine |
| `out.setSendTimeout(ms)` | how long a send may wait for a peer that does not read. When it runs out, the send returns an `Err` and the connection is CLOSED; `0`, the default, waits without limit. A waiting send always ends when the actor is told to stop |
| `events.receive()` / `events.receiveTimeout(ms)` / `events.ref()` | the next `SockEvent`, waiting for it / the same → `Option[SockEvent]`, `None` after `ms` milliseconds / the inbox as an `InboxRef` for `select` |
| `SockEvent::Chunk(b)` / `Line(s)` / `Eof` / `Failed(why)` / `Stopping` | what arrives: data, per the framing / the peer closed the stream / an error. After `Eof` or `Failed` nothing more comes; `Stopping` is the actor's own `Stop` |
| `l.activate(capacity)` | hand the ACCEPTING of a listener to the runtime → `Result[(ActiveListener, IncomingClients), String]`: every new connection arrives as a `SocketHandOff` ticket, which can be sent on to a worker unchanged; while the inbox of at most `capacity` events is full, new clients wait in the operating system's queue. The `TcpListener` is dead from then on |
| `lst.close()` | stop an `ActiveListener`; connections accepted but not received yet are closed, and closing twice is fine |
| `incoming.receive()` / `incoming.receiveTimeout(ms)` / `incoming.ref()` | the next `Incoming`, waiting for it / the same → `Option[Incoming]` / the inbox as an `InboxRef` for `select` |
| `Incoming::NewClient(ticket)` / `AcceptFailed(why)` / `ListenerStopping` | a new connection / the listener failed, and nothing more comes / the actor's own `Stop` |

Here a server and a client talk over loopback in one program (it runs
on one thread, so this works because `connect` queues into the listen backlog and `accept` then picks it up):

```rust check
use std::net::*

fn echo() -> Result[(), String] {
  let lst = listen(48080)?                    // dual-stack listener (IPv4 + IPv6)
  let mut cli = connect("127.0.0.1", 48080)?  // client connects
  let mut srv = lst.accept()?                 // server side of the connection

  cli.sendStr("ping\n")?
  let got = srv.recvLine()?                   // Some("ping")
  println(match got { Some(s) => s, None => "<eof>" })   // => ping

  cli.close()?  srv.close()?  lst.close()?
  Ok(())
}
match echo() { Ok(_) => println("done"), Err(e) => println(e) }   // => done
```

A one-line HTTP GET (plain `http://`, no TLS) fetches a page; a JSON body would feed `Json::parse`:

```rust check
use std::net::*
match httpGet("example.com", 80, "/") {
  Ok(r)  => println("status " + toString(r.status)),   // => status 200
  Err(e) => println(e)
}
```

## 20. `std::poll`: non-blocking I/O

Non-blocking sockets and readiness polling — many connections from one thread, with the loop written by the
program; builds on `std::net`, and `connect` stays blocking. The guide shows it at work in
[Serving more than one connection](SkarnGuide.md#serving-more-than-one-connection). `use std::poll::*`

| Function | Purpose |
|----------|---------|
| `nonBlocking(l)` / `nonBlockingConn(c)` | take a `std::net` listener / connection over → `Result[NbListener, String]` / `Result[NbConn, String]` (free) |
| `poll(fds, interest, timeoutMs)` | wait for readiness → `Result[Array[Int], String]`, **index-parallel to `fds`**. `timeoutMs` 0 = return at once, negative = wait indefinitely (free). Watching nothing still waits, so `poll(vec(), vec(), ms)` is a plain sleep — but a negative timeout with no interest is an `Err`, since nothing could end that wait |
| `READABLE` / `WRITABLE` / `CLOSED` | the flag constants, packed in an `Int` — test with `(flags & READABLE) != 0`, combine with `\|` |
| `l.accept()` | take a waiting connection → `Result[Accepted, String]`: `Accepted::Conn(c)` or `Accepted::WouldBlock`. Never blocks |
| `c.recv(n)` | read up to `n` bytes → `Result[Received, String]`: `Received::Data(b)`, `Received::WouldBlock` (nothing **yet**) or `Received::Closed` (peer gone — **not** the same thing) |
| `c.send(bytes)` / `c.sendStr(s)` | offer bytes → `Result[Int, String]`, the count the kernel **accepted** (may be short, or 0). There is no send-all: keep the tail and retry when `WRITABLE` |
| `l.close()` / `c.close()` | close and free the descriptor → `Result[(), String]`. A closed descriptor must leave the `fds` vector |
| `l.fd` / `c.fd` | the descriptor — what goes into `fds`, and the natural key for the program's own state `Map` |

## 21. `std::task`: parallel tasks

Fork-join parallelism — `spawn` runs a function on its own thread and heap, `join` waits for its result. The
argument and result are **copied**, nothing else is shared. The guide shows it at work in
[Running functions in parallel](SkarnGuide.md#running-functions-in-parallel). `use std::task::*`

| Function | Purpose |
|----------|---------|
| `spawn(f, x)` | start `f(x)` on its own thread → `Task[R]` (free). `f` must be a named, non-generic top-level function with one parameter; its parameter and result types must be plain data — no function values, `dyn` values, sockets or tasks |
| `t.join()` | wait for the task → `Result[R, String]`: `Ok(result)`, or `Err(message)` if it faulted. Once per task; what the task printed appears here |
| `taskFn(f)` / `tf.spawn(x)` | `f` as a value that generic code can take and start tasks from → `TaskFn[A, R]`, checked as `spawn` would check `f` / start `f(x)` → `Task[R]` (free). A `TaskFn` can itself be sent |
| `T: Sendable` | the bound that lets generic code copy values of `T` to another task or actor (a built-in marker, in `std::core`; never implemented by hand) |

## 22. `std::actor`

Actors — long-lived functions on their own threads and heaps, with a mailbox; every message is **copied**, and a
crash is reported to the actor's starter. [Actors in Skarn](SkarnActors.md) teaches them from the ground up.
`use std::actor::*`

| Function | Purpose |
|----------|---------|
| `spawnActor(f, init)` | start `f(inbox, init)` → `Pid[M]` (free). `f` must be a named, non-generic top-level `fn(Inbox[M], I) -> ()`; `M` and `I` must be plain data — no function values, `dyn` values, sockets, tasks or inboxes (a connection travels as a `SocketHandOff`, see `c.handOff()`) |
| `send(p, m)` | copy `m` into the mailbox of the actor at `p` → `Bool`: `false` if it no longer runs, and the message is dropped (free) |
| `mainInbox()` | the main program's own mailbox → `Inbox[M]` (free; once; annotate it: `let me: Inbox[T] = mainInbox()`) |
| `inbox.messages()` | the messages as a lazy `dyn Iterator[M]` that ends when the program ends: `for m in inbox.messages() { … }`. Skips crash reports |
| `inbox.receive()` | wait for the next mail → `Mail[M]`: `Mail::Msg(m)`, `Mail::Exited(id, reason)` (an actor this one started has crashed), or `Mail::Stop` (the program is ending) |
| `inbox.receiveTimeout(ms)` | as `receive`, but `None` after `ms` milliseconds → `Option[Mail[M]]` |
| `inbox.pid()` / `p.actorId()` | this actor's address, to hand out → `Pid[M]` / an actor's id, to compare with the one in `Exited` → `ActorId` |
| `ask(p, make, ms)` | request and reply: sends `make(replyAddress)` to `p`, waits at most `ms` milliseconds for the answer on an inbox of its own → `Result[R, AskError]` (`Gone`, `Timeout`, `Stopped`, `Crashed(reason)`) (free). It monitors the receiver, so a crash ends the wait at once instead of after `ms`. The reply type `R` comes from the `Pid[R]` in the request, and must be plain data |
| `newInbox()` / `newBoundedInbox(n)` | a further inbox of this actor (or of the main program), with its own address → `Inbox[M]` (free; annotate it: `let rx: Inbox[T] = newInbox()`; the bounded one holds at most `n` messages) |
| `inbox.close()` | close an inbox made with `newInbox`: later sends answer `false`, what it holds is dropped. A main inbox cannot be closed |
| `spawnActorBounded(f, init, n)` | as `spawnActor`, but its mailbox holds at most `n` messages; a `send` to it waits while it is full (free). A RING of such waits — including one through a `join` — is detected: every actor in it crashes with a fault naming the ring, because no message could have broken it |
| `trySend(p, m)` | send without ever waiting → `SendResult`: `Sent`, `Full` (nothing was queued) or `Gone` (free). MUST-USE: `Full` means nothing was queued |
| `inbox.ref()` | this inbox as an entry for a `select` list → `InboxRef`. It carries no message type, which is what lets inboxes of different types be waited on together. Like an `Inbox`, it cannot be sent |
| `select(boxes, ms)` | wait until one of several inboxes has something → `Option[Int]`, the INDEX into `boxes` (`None` = the timeout passed; a negative `ms` waits indefinitely) (free). Ties go to the LOWEST index, so the order is a priority order. It takes nothing out: the `receive()` that follows cannot wait. An empty list with a negative `ms` is a fault |
| `actorFn(f)` / `a.spawn(init)` / `a.spawnBounded(init, n)` | `f` as a value that generic code can take and start actors from → `ActorFn[M, I]`, checked as `spawnActor` would check `f` / start an actor → `Pid[M]` (free). An `ActorFn` can itself be sent |
| a GENERIC `f` | allowed wherever the call fixes its type parameters (one erased body serves every use). `spawnActor(relay, boss)` learns them from `boss`; `actorFn(relay)` alone needs an annotation |
| `newSlot()` / `newBoundedSlot(n)` | an address that outlives the actors started into it → `Slot[M]` (free; annotate it: `let s: Slot[T] = newSlot()`; the bounded one holds at most `n` messages) |
| `s.pid()` / `s.spawn(a, init)` / `s.release()` | the address to hand out → `Pid[M]` / start an actor at it → `ActorId` (an error if one is running there) / end the address: later sends answer `false`, an actor still there is told to stop |
| `stopActor(p)` | tell the actor at `p` to end → `Bool`: `false` if it no longer runs. It stops when it next receives; an actor that never receives cannot be stopped |
| `monitor(p, rx)` | be told when the actor at `p` ends → `Bool`: `false` if it had already ended. Exactly ONE `Mail::Exited(id, reason)` follows, in your own inbox `rx`; the reason is `"normal"`, `"gone"`, or the fault message. It watches that actor, not the address |
| `inbox.stopRequested()` | has anyone told this actor to end? → `Bool`. Takes no mail out of the inbox. How an actor whose loop is its own work ends itself, since a stop is a message it would never read |
| `a.stop()` / `a.watch(rx)` | `stopActor` / `monitor` by `ActorId`, for code holding actors whose message type is erased — a supervisor over `Vec[dyn Supervised]` |

## 23. `std::supervisor`

Keeping actors running — `supervise` starts a group of child actors and starts again each one that crashes, up
to a restart limit; supervisors nest into trees. It builds on `std::actor`, and
[Actors in Skarn](SkarnActors.md) shows it at work. `use std::supervisor::*`

| Function | Purpose |
|----------|---------|
| `child(a, init)` | a child a supervisor can start again: the `ActorFn` `a` and its start value → `Child[M, I]`, which is `Supervised` (free) |
| `childIn(slot, a, init)` | the same, but always started at the address `slot` (`std::actor`'s `newSlot`), so the child keeps one address across its restarts → `SlotChild[M, I]` (free) |
| `supervise(inbox, children, limit)` | run in an actor with its own inbox: start every `children: Vec[dyn Supervised]`, start again each one that crashes, return at `Stop`. More than `limit.maxRestarts` restarts within `limit.withinMs` milliseconds (`RestartLimit { maxRestarts, withinMs }`), and it panics, so its own starter is told. Messages to it are dropped |
| `superviseWith(inbox, children, spec)` | the same loop with both choices spelled out: `SupervisorSpec { strategy, limit, stopTimeoutMs }`. `supervise` is this with `OneForOne` and no deadline |
| `Strategy::OneForOne` / `OneForAll` / `RestForOne` | what a crash costs the siblings: nothing / stop them all, wait, start them all / the same for those started after the crashed one. A group restart counts as ONE restart |
| `stopTimeoutMs` | how long a child may take to stop. `0` waits (the group stands still, the supervisor keeps receiving); more makes a RESTART give up and crash, naming the child, and a SHUTDOWN abandon it. A child that neither receives nor asks `stopRequested()` cannot be ended at all |
| `s.start()` / `s.release()` | the `Supervised` trait, anything a supervisor can start whatever its message type — what a `Vec[dyn Supervised]` holds: start it once more → the `ActorId` its crash report will carry / give up its address, if it has one of its own, as a supervisor does when it stops supervising. `Child` and `SlotChild` implement it |

## 24. `std::regex`

Linear-time byte-level regular expressions (Thompson NFA / Pike VM) — no catastrophic backtracking, and
therefore **no** backreferences or lookaround. Note the names: matching anywhere is `search`, not `find`, and
rewriting is `replaceRe`, not `replace` — those two belong to `std::iter` / `std::string`. `use std::regex::*`

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

It matches, captures, and rewrites text (compile once, reuse):

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

println(toString(count(re.searchAll("a=1 b=22 c=333"))))   // => 3         (lazy)
println(re.replaceAllRe("a=1 b=22", "$1:$2"))              // => a:1 b:22  (templates)
```

## 25. `std::log`

Timestamped log lines at four levels, filtered by a minimum, written to a file directly or through a logger
actor that owns the file; no rotation and no configuration file. It builds on `std::io`, `std::time` and
`std::actor`. A line is `<iso timestamp> <LEVEL> <text>`; the verbs are methods, so `use std::log::*` claims
none of the names `info`, `warn`, `error` or `debug`. `use std::log::*`

| Function | Purpose |
|----------|---------|
| `Log::toFile(path, min)` | a logger appending straight to `path`; nothing below `min` is written. One append per line, and that call is atomic, so several actors may share the file |
| `Log::toActor(to, min)` | a logger sending its lines to `to: Pid[String]` instead. Costs a message, and buys one order for the whole program and, with a bounded inbox, back-pressure |
| `log.debug(msg)` / `log.info(msg)` / `log.warn(msg)` / `log.error(msg)` | write one line at that level (also `Log::info(log, msg)`, as for any method) |
| `log.at(level, msg)` | the same with the level as a value — the one place that filters, formats and writes |
| `Level::Debug` / `Info` / `Warn` / `Error` | the four levels |
| `l.rank()` / `l.atLeast(min)` / `l.label()` | a level's place in the order → `Int` / whether it is at least as important as `min` — the filter / the fixed-width text a line carries |
| `startLogger(path, capacity)` | start a logger actor owning `path` → `Pid[String]`. Its inbox holds at most `capacity` lines, so a program logging faster than the disk writes is slowed rather than grown |

A `Log` — its `Sink`, `Sink::ToFile(path)` or `Sink::ToActor(pid)`, and its minimum level — is plain
data, so it is sendable: an actor is handed its logger in its start value. Stop a logger
actor **last** — `Stop` is queued at the end of a mailbox, so everything already sent is written, but a line
sent after it has ended is dropped.
