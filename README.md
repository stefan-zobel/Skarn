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
- [The Skarn Language Guide](SkarnGuide.md): the whole language, the standard library and the cost model.
- [`examples/`](examples/README.md): small but complete programs, such as a word counter, a JSON todo list, a
  calculator, a log analyzer, and Dijkstra over a generic heap.
- [`demo/`](demo/README.md): one program per feature or standard-library module, plus a
  [raytracer](demo/raytracer/README.md) and a small benchmark against CPython.
- [docs/VirtualMachine.md](docs/VirtualMachine.md): how the VM works: values, bytecode, calls, the garbage collector.
- [docs/Compiler.md](docs/Compiler.md): how the compiler works: checker, modules, code generation, verification.
- [`docs/skarn_grammar.ebnf`](docs/skarn_grammar.ebnf): the grammar, also rendered as
  [syntax-highlighted text](docs/skarn_grammar.svg) and as [railroad diagrams](docs/skarn_grammar_railroad.svg).

Every code block in both guides and every program under `examples/` is run by the test harness, and each
must print exactly the output shown.

## Building

The project targets **Windows x64 with Visual Studio 2022** (MSVC toolset v143, C++20). It is a Visual Studio
solution without CMake. From a developer command prompt:

```bash
msbuild vMachine.sln /p:Configuration=Release /p:Platform=x64
```

Everything is built into `x64\Release\`. The Debug configuration works the same way.

## Running a program

```bash
x64\Release\static_vmrun.exe hello.skn
```

Arguments after the script are passed to the program. Useful flags:

- `--strict` turns warnings into errors.
- `--dump-ast` type-checks the program and prints the typed syntax tree without running it.
- `--emit-bytecode <file>` / `--run-bytecode <file>` compile to a `.skbc` image and run it later.

Run `static_vmrun` without arguments for the full list. A multi-file program is a directory of `.skn` files:
`import net::http` loads `net/http.skn` relative to the entry file.

## Tests

| command | what it checks |
|---|---|
| `x64\Release\vm_tests.exe` | the VM: opcodes, the heap and the collector, natives, the bytecode format |
| `x64\Release\static_compiler_tests.exe` | the compiler: checker, codegen, and differential runs against a reference interpreter over generated programs |
| `powershell -File tests\guide_claims\run_guide_claims.ps1` | the documentation: guide claims, every guide example, and every program under `examples/` |

## Repository layout

| directory | contents |
|---|---|
| `vmcore/` | the VM: value representation, instruction set, interpreter, heap and GC, natives |
| `static_compiler/` | the Skarn front end: lexer, parser, type checker, code generator, module loader; `std/` holds the standard library in Skarn |
| `static_vmrun/` | the command-line driver |
| `vm_tests/`, `static_compiler_tests/`, `tests/` | the test suites and the documentation harness |
| `examples/`, `demo/` | Skarn programs |
| `docs/`, `tools/` | the VM and compiler documents and the grammar; editor syntax highlighting |

## License

[MIT](LICENSE)
