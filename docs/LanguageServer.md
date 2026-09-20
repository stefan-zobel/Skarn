# The Skarn language server

This document describes `skarn_lsp`, the language server that gives an editor live diagnostics, an outline,
hover, go to definition, find references, rename, completion, signature help and formatting for Skarn. It is written for
someone who wants to read or change the server. `tools/vscode-skarn` is its VS Code client; the Windows
release zip ships both, `skarn_lsp.exe` and the extension as a `.vsix`. The compiler whose
front half it runs is described in [Compiler.md](Compiler.md), the language in the
[Skarn Guide](../SkarnGuide.md).

## Overview

`skarn_lsp` speaks the Language Server Protocol over stdin/stdout. It runs the front half of the compiler
only — loading and checking — and never generates or runs code, so it has no platform dependencies beyond the
compiler's own and builds with CMake as well as with the Visual Studio solution. It uses three entry points
the compiler provides for tools:

- `svc::load_modules_for_tools` lexes and parses in an **error-tolerant** mode the compiler itself never uses
  (see "Error recovery").
- `svc::check_modules_for_tools` returns the checked program together with every error and warning instead
  of throwing, and writes the final inferred type into every expression (a type is otherwise recorded when
  its node is checked, before a later statement may have fixed it).
- The same call returns the list of builtins and natives a program may call, each with the module whose
  `use` makes it reachable and its signature. Only the checker knows this list.
- For the formatter, the lexer reports each token's source bytes and the comments it skips, and the parser
  reports where statements start and what each `{` opens (see "Formatting").

The parser records the position of every name token for the server: declared names (item, method,
parameter), the member after a `.`, the tail of a qualified path (`util::f`, `Shape::Square`), the head of a
qualified pattern, the names in a `use` list, a trait impl's trait. Nodes carry a start position but no end.

The server keeps no incremental state: every change re-checks the whole program, standard library included,
which is fast enough for programs of this size. Requests that need a modified text (completion, signature
help, rename's self-check) check a copy once more.

## Source files

Everything lives in namespace `lsp`.

| file | responsibility |
|---|---|
| `Json` | a small JSON reader and writer (insertion-ordered objects) |
| `Transport` | the `Content-Length` message framing |
| `Analyze` | one analysis: load and check a program with open buffers overriding the disk, map the compiler's findings to diagnostics, keep the checked program and the sources for the queries |
| `Symbols` | the outline, and the source-like rendering of declarations that hover and completion share |
| `Query` | hover, definition, references, highlight and rename; the position lookups that completion and signature help ask (`probe`, `call_at`) |
| `Complete` | completion and signature help |
| `Format` | formatting |
| `Server` | the message loop: text synchronization, re-checking, publishing diagnostics, answering requests |
| `SelfTest` | `skarn_lsp --selftest` |

Standard output carries nothing but protocol frames: on Windows it is switched to binary mode, and
`std::cout` is redirected to standard error.

## Analysis and diagnostics

- Every open file that no other open file imports is checked as a program, with its imports resolved the way
  the driver resolves them (`import net::http` is `net/http.skn` next to the entry file). An open editor
  buffer takes precedence over the file on disk, so unsaved edits count.
- Type errors, parse errors, load errors and warnings become diagnostics with the driver's wording; a
  secondary note (such as the parameter an argument failed to match) becomes related information. An error
  inside an imported module is reported on that module, a missing import on its `import` or `use` line.
- Nodes have no end position, so a diagnostic underlines the identifier that starts at the reported column,
  or a single character.
- After each change the server publishes only the diagnostics that changed, including an empty list for a
  file whose last problem just went away. A program that cannot be loaded at all (a missing module, an
  import cycle) has no tree; the outline then shows the file's last good outline.

## Error recovery

Syntax errors do not stop the analysis. In the tolerant mode each error is reported, and parsing resumes at
the next statement, match arm, impl or trait member, field, variant or item. The construct that failed is
left out whole, so the tree has no error nodes and the checker runs on it unchanged.

- A keyword that only an item can start with (`fn name`, `struct`, `impl`, `pub`, ...) closes every open
  body, so a missing `}` costs one error at the unclosed `{` instead of the rest of the file.
- Which `{` is reported follows the indentation: the first `}` that does not line up with the line of its
  `{` marks where the braces started to pair up wrongly. A statement that begins with `else` means the `}`
  right before it is missing, and a `}` indented deeper than the line of the body's `{` belongs to the
  construct that failed (it lost its own `{`).
- Where a block is required and its `{` is missing (`if c  x } else ...`), the code up to a `}` on the same
  line, or on the indentation of the `if` / `fn` line, is read as that block, so the construct survives with
  one error. In an impl or trait body, a `fn` indented past the impl is a method.
- A lex error drops the rest of its line.

The checker's findings inside an item that lost part of itself are not shown (they are mostly caused by the
missing text), and a file with a syntax error shows no warnings; every other item keeps its diagnostics, and
outline, hover, definition, references, completion and signature help keep working. Rename is refused until
the syntax errors are fixed, because text left out of the tree could hold a use.

## Outline, hover and go to definition

- **Outline** lists the file's functions, types, traits, impls and constants, with variants and methods
  nested. A symbol's range runs to the line before the next declaration.
- **Hover** shows a declaration's signature, or the type the checker inferred. A builtin is typed per call,
  so its hover shows what the call produces (`len(..) -> Int`).
- **Go to definition** follows a name to its declaration in any module of the program: a top-level name
  through the mangled name the checker writes back into each use, a method call through the trait the checker
  dispatched it to (a bare `m(x)` as well as `x.m()`) or through the receiver's type, a field through the
  receiver's type, a type name in an annotation by its name (this module first), and a local through a scope
  walk of the file that mirrors the checker's. Names declared in the standard library have no file to go to.

## References, highlight and rename

- **Find references** and **document highlight** invert that relation: every name that resolves to the same
  declaration, across all modules of every open program. Highlight marks declarations and assignments as
  writes.
- **Rename** edits exactly those names (a struct shorthand `P { x }` becomes `P { x: y }`), and it verifies
  itself before answering: the edits are applied in memory, every affected program is checked again, and the
  rename is refused unless no new error appears and exactly the edited names now refer to the renamed
  declaration. That rules out capturing another binding and colliding with another declaration. What it
  cannot see completely is refused with the reason: fields, names from the standard library, and a trait
  method while a call `m(x)` of that name exists that the checker could not resolve (a call it did resolve
  is renamed with the method, since the trait it dispatched to is recorded in the tree).

## Completion

- **Members after `recv.`:** the fields, the methods with `self` and the methods of every trait the
  receiver's type implements, by an impl on its head or by a blanket impl whose bounds the head meets
  (checked one level deep); for `dyn Tr` and a bounded type parameter, the traits' and their supertraits'
  methods.
- **Names elsewhere:** locals first, then generic parameters in a type position, the file's declarations,
  the standard library's always-visible names, what the file's `use` items bring in, the modules it imports,
  the trait methods (callable bare), the builtins and the natives reachable there, and keywords. In a type
  annotation only types are offered.
- **After `::`** the path before it decides. `Type::` offers an enum's variants and the functions of the
  type's inherent impls (or a trait's methods), with the head resolved as the checker resolves a type name;
  `module::` offers the public items of a module the file imports (the standard library's always-visible
  names for `std::`), types only in a type annotation; a `use` path offers the modules below it, a module's
  public items and the natives it gates, or an enum's variants. An `import` path is not completed.

How it finds the context: at `p.` the text does not parse, so the server inserts a placeholder name at the
cursor, checks that text, and reads what the placeholder became — the member of a field access, an
expression, or a type. A line still being typed gets a second try with its open brackets closed, an empty
block after an `if` / `while` / `match` / `for` head, a value for a `let` without one and a body for a `fn`
header without one; if the statement is still left out, the names come from the enclosing block. The lexer
decides whether the cursor is in code at all, so nothing is offered inside a string or a comment, where a
new name is declared, or after a number's `.`. The VS Code extension turns off the editor's word-based
suggestions for Skarn files, which would otherwise fill those places with words from the document.

## Signature help

Signature help shows the signature of the call the cursor is in and marks the argument it is at. The lexer
finds the call's open `(` — the innermost one after a name, a `)` or a `]`, not crossing a block — and counts
the commas before the cursor. The call node is found in the checked tree by the position of that `(`, and
while the line does not parse yet, in a re-check with a placeholder argument at the cursor (and then the
line's brackets closed).

The callee resolves like go to definition, the standard library included: a function; a method through the
receiver's type (its `self` is shown but is no argument there); `Type::m` and `Trait::m`, where `self` is the
first argument; a tuple variant or tuple struct; a local of function type; a builtin or native. A builtin the
checker types per call (`len`, `get`, `print`) has a hand-written signature per accepted shape. A bare
trait-method call `m(x)` shows the method of the trait the receiver picks — every trait with that name only
while nothing is typed there yet — and after `x |> f(` the first argument is `x`. Declared types are shown, not those a call instantiates.

## Formatting

**Format Document** prints the file in Skarn's one fixed format; there are no options.

| topic | rule |
|---|---|
| bodies | every fn, method, impl, trait, struct and enum body and every statement block on its own lines, `{` at the end of the line, `}` alone, 4 spaces of indentation |
| members | struct fields, enum variants and match arms one per line, a comma after each but the last |
| blank lines | one between items, two after the `use` / `import` block; between methods and inside bodies as written, at most one in a row |
| inline | a lambda or an `if` expression whose branches are single expressions, and a struct or map literal, stay on one line when they fit |
| 80 columns | a call, parameter list or literal that does not fit gets one element per line with the closer on its own line; an operator chain breaks before each operator (`\|>`, `&&`, `+`, ...), a method chain before each `.` |
| spacing | `name: Type`, `enum E : Int`, spaces around binary operators, `->`, `=>` and `=`, one after a comma, none inside `()` and `[]` |

The formatter reprints the file's **tokens** with new whitespace. It does not print the tree, which has lost
too much: interpolation is desugared, parentheses and comments are gone. Strings, raw strings, whole
interpolated strings, chars, numbers and comments are copied byte for byte; only commas are normalized and
statement ends follow the layout. The structure comes from the parser, which reports for the formatter where
every statement, match arm and member starts (statements may share a line with nothing between them) and
what each `{` opens. The lexer reports each token's source bytes and every comment it skips.

A line break is only placed where Skarn's automatic statement separation cannot split a statement: after
`(`, `[`, `{` and `,`, before a closer, and before an operator or a `.`. A comment at the end of a line
stays there, without making the code before it break; a comment on its own line keeps its own line. A
string longer than the line is not broken, and a call with only that string as its argument stays on one
line. The file's line breaks (CRLF or LF) are kept.

Before answering, the result is checked: it must parse, its tree must equal the input's (compared without
positions), its tokens must be the input's apart from commas and statement ends, and its comments the same.
Otherwise nothing changes and the reason is shown. A file with a syntax error is not formatted.
`skarn_lsp --format <file>` prints a file formatted to standard output.

## Testing

`skarn_lsp --selftest` runs the server's own test suite and exits non-zero on a failure: the JSON reader and
writer, the message framing, the mapping from compiler errors to diagnostics, error recovery, outline, hover,
definition, references and rename (including the refusals) over a two-module program, completion, signature
help, formatting (every rule, idempotence, refusals), and a scripted protocol session.
