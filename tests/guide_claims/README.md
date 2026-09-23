# `tests/guide_claims/` — executable guide-claim checker

Every categorical promise the guides — `SkarnGuide.md` (the full guide), `SkarnActors.md` (actors) and
`SkarnIn30Minutes.md` (the introduction) — make, from *"a struct compares structurally"* to
*"`xs |> intoIter |> filter(p) |> collect`"*, is a claim about what the **compiler actually does**.
Prose drifts silently: a guide goes on asserting behaviour after the compiler has changed. This
directory turns the load-bearing claims into runnable checks so the drift fails here instead of
misleading a reader. The introduction is covered as thoroughly as the full guide, since it is the
first document a newcomer reads.

Each `*.skn` is a self-contained Skarn program encoding **one** claim, run through
`static_vmrun`. A header comment declares the expected outcome:

```
// CLAIM:  <one-line description of the guide claim>
// GUIDE:  <where in the guide it lives>
// EXPECT: ok   <exact stdout>       -- exit 0, trimmed stdout equals this
//    or   warn <stderr substring>   -- exit 0, but stderr contains this advisory
//    or   fail <stderr substring>   -- exit != 0, stderr contains this (a rejection)
```

The `// …` lines are ordinary Skarn line comments, so the file still compiles; only `EXPECT:`
is read by the runner (first match wins).

## Running

From a developer shell (any cwd), after building the solution (x64 Release by default):

```
powershell -File tests/guide_claims/run_guide_claims.ps1
```

Options: `-Config Debug` (use the Debug `static_vmrun.exe`), `-Exe <path>` (an explicit driver),
`-Dir <path>` (a subset). Exit code is `0` iff every claim held; a drifted claim prints
`FAIL … <why>` and the script exits `1`, so this is CI-gateable.

## Every guide example — `run_guide_examples.ps1`

The claims above are written by hand, so a guide example nobody wrote a fixture for is unguarded. So a
second runner checks **every** ```` ```rust ```` block of all three guides. It extracts them **at run time** — the
guides are the single source of truth, and there are no copies in the repository that could drift from them.
`run_guide_claims.ps1` calls it at the end (skip with `-NoExamples`), so the one command above runs both.

For each block it asserts:

- the program exits `0`;
- if the block has `// =>` comments, stdout is **exactly** those lines, in order. A `// =>` may follow code or
  stand on its own line (for the second line a loop prints). Text after two or more spaces and a `(` is a
  remark, not output: `// => 3   (integer division)`;
- every warning the checker reports is annotated `// warning: <text>` on the line its caret points at.

An annotation inside a comment line that starts with `//` (commented-out code) is not read, except a standalone
`// =>` line.

Markers go into the fence's info string, after `rust`. GitHub still renders the block as Rust and does not show
them:

| marker | meaning |
|---|---|
| *(none)* | run it, as above |
| `group=<name>` | the blocks with this group (in one guide) are concatenated, in order, into one program — for an example that continues an earlier one |
| `file=<path>` | with `group=`: this block is not program text but the module file `<path>` beside it (`file=geo.skn` for `import geo`) |
| `fail` | the program must be rejected. Every line that should be rejected carries `// error: <text>`; the checker must report an error with its caret on that line whose message contains `<text>` (up to the first ` -- `, em dash or ` (`), and must report nothing else |
| `check` | type-check only (`static_vmrun --dump-ast`), never run — for the network examples |
| `ignore=<reason>` | not a program; reported as skipped (no spaces in `<reason>`) |

An unknown marker, a group with only `file=` blocks, `fail` inside a group, `// error:` without `fail`, or
`// =>` inside a `fail` block is a failure. Each program runs in its own temporary directory with empty stdin, so
the file examples write nothing into the repository.

```
powershell -File tests/guide_claims/run_guide_examples.ps1
powershell -File tests/guide_claims/run_guide_examples.ps1 -Only 2356          # the block (or group) at that line
powershell -File tests/guide_claims/run_guide_examples.ps1 -Emit C:\tmp\ex     # also dump the programs
```

`-Guide <file.md>` runs another markdown file; `-Config` / `-Exe` as above. Exit `0` iff every block held.

**Writing a guide example:** annotate what the program *prints*, never a value — `let q = 7 / 2  // => 3` fails,
because nothing is printed; write `println(7 / 2)  // => 3`. Do not leave a binding unused (the unused-variable
warning fails the block). A block with no `// =>` at all is only required to run cleanly, which is the right
choice for output that differs between runs (a random roll, the environment).

## Example programs — `tests/run_examples.ps1`

The third runner, also called at the end of `run_guide_claims.ps1` (and skipped by the same `-NoExamples`),
checks the realistic programs under `examples/` — each a small real task written from the guides alone
(see `examples/README.md`). Every example runs with `--strict` in a temporary copy of its directory and must
exit `0`, print nothing to stderr, and print **exactly** its `expected.out`; an optional `args.txt` holds the
command-line arguments. Where a guide block checks one feature in isolation, these catch what only shows up
when features meet in a whole program.

```
powershell -File tests/run_examples.ps1
powershell -File tests/run_examples.ps1 -Only word_freq
powershell -File tests/run_examples.ps1 -Update      # rewrite expected.out; review the diff before keeping it
```

## Link integrity — `check_anchors.ps1`

A sibling gate for a guide's *internal* cross-references. `SkarnGuide.md` links to its own
sections with `[text](#slug)`; when a heading is renamed but a link is not, the link silently 404s a reader —
a blind spot the claim fixtures cannot cover (a link to `#14-maps` after the heading became `## 14. Collections`,
which slugs to `#14-collections`). `check_anchors.ps1` mechanically validates every internal anchor against the GitHub slug
of every heading (fence-aware, so code-block `#` lines and example `](#…)` are ignored):

```
powershell -File tests/guide_claims/check_anchors.ps1
```

`-File <path>` checks a different markdown file. Exit `0` iff every anchor resolves; a broken anchor prints
`#slug (first at line N)` and the script exits `1`. Run it in the same doc gate as the claim harness.

## Tiers

- **`intro/`** — every runnable example of **`SkarnIn30Minutes.md`**, one fixture per claim, named
  `sNN_…` after the section it locks. Because that document is a newcomer's first contact, the bar is
  *whole-page* coverage rather than a selection: bindings and interpolation, the primitives, functions,
  `if`-as-expression, the byte `for` over a `String`, record update, enums + exhaustive `match`,
  `Option`/`Result`/`?` (including the top-level `?` rejection), `Vec`/`Map` and reference semantics,
  the text helpers with lazy `split`/`lines`, the pipe, the **`intoIter` lift** and its omission as a
  rejection, `range` as a ready-made source, eager `sorted`, `use` (a std module, plus the **missing-`use`
  hint** in all three of its shapes — for a native, for a Skarn-level std fn, for a type — and the
  no-hint-for-a-real-typo case), a user module via `import`/`use`/`mod::name` (a subdirectory, Tier-3 style),
  the `run() -> Result` + top-level `match` shape, `args()`, and the closing word-count program.

- **`tier1/`** — the soundness-critical, categorical claims: the ones where a silent drift means
  either an actual soundness hole or a user-facing lie (object safety, sealed markers, map-key
  gating, erasure-can't-impl-traits, const-array bans, the `mut`-param rule, structural `==` and
  its non-obvious edges, `?`-propagation, the checker's *no-truthiness / no-`==`-promotion*
  restrictions that distinguish the sound Skarn surface from the underlying VM opcodes, that
  shadowing a builtin/native/trait-method name is **module-scoped** — it wins in its own scope and
  never reaches inside the standard library —, that a qualified `Head::m(x)` is **rejected as
  ambiguous** when a trait and a type of that name both declare `m` rather than silently resolving
  to one, that a compound assignment evaluates its place **exactly once** and accepts exactly what
  its plain operator does (arithmetic ⇒ numeric, bitwise/shift ⇒ `Int` only), and
  that a runtime trace names functions **as the author wrote them**: neither the entry program's
  internal module prefix nor a synthetic `$inherent$…` body name ever surfaces).

- **`tier2/`** — breadth: the categorical *feature* claims a reader copies from the guide. Patterns
  (range / or / `if let` / `while let` / record-variant / tuple-destructure / exhaustiveness),
  literals (hex-oct-bin / **`_` digit separators** / char-is-Int / raw strings / interpolation /
  concat-coercion), format
  specifiers (width / base / precision / zero-pad), operators (compound assignment — all eleven forms
  across the three target forms, and its `m[k] += 1`-on-a-missing-key abort), strings (byte `len` / byte `for` / code-point
  `chars` / lexicographic order), the iterator surface (lazy pipeline, the **Iterator-vs-Iterable**
  terminal split — `collect` vs `toVec`, the `intoIter` lift a container needs, and the six shipped
  combinators), impls (an inherent method reached by `.`, by the qualified `Type::method(x)` and in a pipe,
  but never as a bare free call; `Trait::method`; blanket-loses-to-concrete), `const`
  (scalar inlining / array element-literal rule), record update, **struct field shorthand** (in a literal and
  in a pattern, mixed with explicit fields, combined with `..base` — plus the rule that a struct pattern must
  name every field), the numeric conversions (`toDouble` widening, and that `Int / Int` stays integer division
  even in a `Double` context), `CliArgs::parse` accepting both an `Array` and a `Vec`, and by-value closure capture.

  `tier2/newcomer_*.skn` is the **newcomer-diagnostics corpus**: short, deliberately wrong programs of the kind
  a new user writes in the first hour, each locking the part of the message that user needs — the cause and,
  where there is one, the fix. Missing `mut` for `push`, a container piped into `map` without `intoIter`, `s[i]`
  on a `String`, `?` / `return` / `break` outside their construct, an opt-in std name without `use` (fn, type,
  enum, constant), `import` without `use`, a struct as a map key, a list pattern on a `Vec`, a builtin passed as
  a function value, `pub fn` inside an `impl`, plus the located runtime aborts (`unwrap` on `None`, a missing map
  key, an index out of range, division by zero). A message that got worse is a finding, not a fixture update.

- **`tier3/`** — **module-system rules**, which need more than one file. Each claim is its own
  **subdirectory**: one entry `.skn` carries the `// EXPECT:` directive, and the other `.skn` files
  beside it are the imported modules (`static_vmrun` resolves `import foo` to `foo.skn` next to the
  entry). Covered: `import` + `use` + qualified `mod::name` + glob `use mod::*`, cross-module `pub`
  types, **default-private** visibility (a non-`pub` item is unreachable), the **orphan rule**,
  binding precedence (a local def / explicit `use` beats a glob; two explicit `use`s of one name
  collide), and that the **entry program is not importable** (a name defined there is unreachable
  from a module, and the diagnostic says why).

Further tiers can be added as sibling directories; the runner globs `*.skn` recursively.

## Multi-file claims

A `.skn` with **no** `// EXPECT:` directive is treated as a **support module** — imported by some
claim's entry, never run on its own (the runner skips it and reports the skip count). So a multi-file
claim is just a subdirectory whose entry file has the directive and whose siblings do not.

## Adding a claim

1. Write the smallest program that exercises the claim; end an `ok` case with a single
   `println(...)` whose output is stable.
2. Add the `// EXPECT:` header. For `fail`/`warn`, pick a **short, stable** substring of the
   diagnostic (avoid carets, line numbers, and volatile wording).
3. Run the script and confirm `PASS`. If it `FAIL`s, decide which is wrong — the fixture or the
   guide — and fix that one. A fixture that encodes real behaviour contradicting the guide **is a
   finding**: fix the guide, then keep the fixture as the regression lock.

## Scope note

These fixtures run against the **sealed embedded prelude** via `static_vmrun`, exactly as a user
runs a program — so they check the *shipping* surface, not an internal build.
