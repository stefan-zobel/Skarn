# Skarn / vMachine

## About the Project

This project features a statically-typed programming language running on a dynamically-typed virtual machine. 

* **AI-Generated Codebase:** The majority of this project was written by Opus 4.8. 
* **The Motivation:** To explore the limits of AI capability within an absolutely non-trivial programming project.

**Skarn** is a statically typed language with a Rust-flavored surface: enums with exhaustive `match`,
`Option`/`Result` with `?`, traits with bounds, generics, `dyn` trait objects, lazy iterators, modules, and
everything an expression. It has **no borrow checker**: memory is managed by a garbage collector, so you never
write lifetimes or think about ownership.

**vMachine** is the runtime underneath it: a compact bytecode VM written in C++20, with NaN-boxed 64-bit values,
a register-window calling convention, and a precise, moving (Cheney copying) garbage collector. The Skarn
compiler type-checks a program soundly, then **erases** the types and emits bytecode for this VM. Generic code
compiles to a single body, and no type arguments are passed at run time.

The language and its standard library are a personal project, in a `0.x` preview state: feature-complete for the
programs it was built for, extensively tested, and not yet stable.

```rust
enum Shape {
    Circle(Double),
    Rect(Double, Double),
}

fn area(s: Shape) -> Double {
    match s {
        Circle(r)  => 3.14159 * r * r,
        Rect(w, h) => w * h,
    }
}

fn parseShape(line: String) -> Result[Shape, String] {
    let parts = line |> words |> collect
    match parts[0] {
        "circle" => Ok(Circle(parseDouble(parts[1])?)),
        "rect"   => Ok(Rect(parseDouble(parts[1])?, parseDouble(parts[2])?)),
        other    => Err("unknown shape '${other}'"),
    }
}

let input = ["circle 1.5", "rect 2 4", "hexagon 3"]
for line in input {
    match parseShape(line) {
        Ok(s)  => println("${line}: area ${area(s):.2}"),
        Err(e) => println("${line}: ${e}"),
    }
}

let total = intoIter(input)
    |> map(parseShape)
    |> filter(isOk)
    |> map(fn(r) { area(unwrap(r)) })
    |> fold(0.0, fn(acc, a) { acc + a })
println("total ${total:.2}")
```

```
circle 1.5: area 7.07
rect 2 4: area 8.00
hexagon 3: unknown shape 'hexagon'
total 15.07
```

## Documentation

- [Skarn in 30 minutes](SkarnIn30Minutes.md): the working core, enough to write a real program.
- [The Skarn Language Guide](SkarnGuide.md): the whole language and the cost model.
- [The Skarn Standard Library](SkarnStdlib.md): every function of every `std` module, with worked examples.
- [Actors in Skarn](SkarnActors.md): long-lived workers that share nothing and talk by messages —
  supervision, back-pressure and the rest, for readers who have never used an actor system.
- [`examples/`](examples/README.md): small but complete programs, such as a word counter, a JSON todo list, a
  calculator, a log analyzer, and Dijkstra over a generic heap.
- [`demo/`](demo/README.md): one program per language feature and most standard-library modules, larger
  programs — a [path tracer](demo/raytracer/README.md) on tasks, a [chat server](demo/chat/README.md) and an
  [HTTP server](demo/actor_server/README.md) on actors — and a small benchmark against CPython.
- [docs/VirtualMachine.md](docs/VirtualMachine.md): how the VM works: values, bytecode, calls, the garbage collector.
- [docs/Compiler.md](docs/Compiler.md): how the compiler works: checker, modules, code generation, verification.
- [docs/LanguageServer.md](docs/LanguageServer.md): how the language server works: analysis, error recovery, queries, completion, signature help.
- [`docs/skarn_grammar.ebnf`](docs/skarn_grammar.ebnf): the grammar, also rendered as
  [syntax-highlighted text](docs/skarn_grammar.svg) and as [railroad diagrams](docs/skarn_grammar_railroad.svg).

Every code block in the guides and every program under `examples/` is run by the test harness, and each
must print exactly the output shown.

## Building

### macOS (Apple Silicon)

Requires Xcode Command Line Tools (Clang) and CMake 3.21+. **arm64 only** — no Intel support.

```bash
# Install CMake if needed
brew install cmake

# Configure (once)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -- -j$(sysctl -n hw.logicalcpu)
```

The driver binary is `build/skarnvm`.

### Windows (x64, Visual Studio 2022)

The project also ships a Visual Studio solution for Windows. From a developer command prompt:

```bash
msbuild vMachine.sln /p:Configuration=Release /p:Platform=x64
```

Everything is built into `x64\Release\`. The Debug configuration works the same way.

## Running a program

**macOS:**
```bash
./build/skarnvm hello.skn
```

**Windows:**
```bash
x64\Release\skarnvm.exe hello.skn
```

Arguments after the script are passed to the program. Useful flags:

- `--strict` turns warnings into errors.
- `--dump-ast` type-checks the program and prints the typed syntax tree without running it.
- `--emit-bytecode <file>` / `--run-bytecode <file>` compile to a `.skbc` image and run it later.

Run the driver without arguments for the full list. A multi-file program is a directory of `.skn` files:
`import net::http` loads `net/http.skn` relative to the entry file.

## Tests

**macOS:** `ctest --test-dir build` runs all thirteen entries: the VM suite, the hardware-fault probe, the
compiler suite, the language-server self-test, and the nine documentation, demo and driver gates of the table
below (`doc_claims`, `doc_examples`, `examples`, `doc_links`, `doc_anchors`, `demos`, `stdlib_reference`,
`highlighters`, `live_output`). The individual binaries are
`build/vm_tests`, `build/static_compiler_tests` and `build/skarn_lsp --selftest`. The Python scripts of the
table (Python 3.9 or newer, no packages) also run by hand, as `python3 tests/...`; they find `build/skarnvm` on
their own, and `--exe` names another driver.

**Windows:**

| command | what it checks |
|---|---|
| `x64\Release\vm_tests.exe` | the VM: opcodes, the heap and the collector, natives, the bytecode format |
| `x64\Release\static_compiler_tests.exe` | the compiler: checker, codegen, and differential runs against a reference interpreter over generated programs |
| `python tests\guide_claims\run_guide_claims.py` | the documentation: guide claims, every guide example, every program under `examples/`, the guides' links, that `SkarnStdlib.md` documents every public name of the standard library, and that the three syntax highlighters agree and are complete |
| `python tests\check_doc_anchors.py` | that every section name cited from source still exists in the `docs/` documents |
| `python tests\run_demos.py` | the demos: every program under `demo/` type-checks, and the four that check themselves pass |
| `python tests\check_live_output.py` | that a running program's output reaches a pipe line by line, before the program ends |
| `x64\Release\skarn_lsp.exe --selftest` | the language server: JSON, message framing, diagnostics, error recovery, outline, hover, go to definition, references, rename, completion, signature help, formatting, a scripted session |

## Editor support

`tools/vscode-skarn` is a VS Code extension: syntax highlighting, plus live type checking, an outline, hover
types, go to definition, find references, rename, completion, signature help and formatting through the language server `skarn_lsp` (built with the
rest of the project). The Windows release zip contains both, `skarn_lsp.exe` and the extension as a `.vsix`;
on macOS the server is `build/skarn_lsp` and the same `.vsix` drives it, since the extension is JavaScript
and ships no binary. The extension's README explains how to install it and point it at the server. Syntax highlighting alone is also available for Notepad++ (`tools/skarn.npp-udl.xml`) and any
TextMate-compatible editor (`docs/skarn.tmLanguage.json`).

## Repository layout

| directory | contents |
|---|---|
| `vmcore/` | the VM: value representation, instruction set, interpreter, heap and GC, natives |
| `static_compiler/` | the Skarn front end: lexer, parser, type checker, code generator, module loader; `std/` holds the standard library in Skarn |
| `static_vmrun/` | the command-line driver, `skarnvm` |
| `skarn_lsp/` | the language server (diagnostics, outline, hover, go to definition, references, rename, completion, signature help, formatting) |
| `vm_tests/`, `static_compiler_tests/`, `tests/` | the test suites and the documentation harness |
| `examples/`, `demo/` | Skarn programs |
| `docs/`, `tools/` | the VM and compiler documents and the grammar; editor syntax highlighting and the VS Code extension |

## License

[MIT](LICENSE)
