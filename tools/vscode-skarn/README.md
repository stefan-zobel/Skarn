# Skarn for VS Code

Syntax highlighting, live type checking, an outline, hover types, go to definition, find
references, rename, completion, signature help and formatting for the
[Skarn](https://github.com/stefan-zobel/Skarn/blob/HEAD/SkarnGuide.md) language (`.skn` files, run by `static_vmrun`, on macOS `skarnvm`).

- **Highlighting** comes from a TextMate grammar and works on its own.
- **Diagnostics** come from `skarn_lsp`, the Skarn language server: type errors, parse
  errors and the checker's warnings appear while you type, with the same wording
  `static_vmrun` prints. A note such as *"expected because of parameter 'n'"* shows up
  as related information pointing at the parameter.
- **Outline** (the Outline view, breadcrumbs, *Go to Symbol* `Ctrl+Shift+O`): functions,
  structs, enums with their variants, traits and impls with their methods, constants.
- **Hover**: a declaration's signature, or the type the checker inferred for a variable,
  field or expression.
- **Go to definition** (`F12`, `Ctrl`+click): functions, types, variants, constants,
  methods, fields and local variables, across the modules of the program. Names from
  the standard library have no file to go to.
- **Find references** (`Shift+F12`) and **highlight**: every use of the name under the
  cursor, across the modules; clicking a name highlights it in the file (assignments and
  the declaration marked as writes).
- **Rename** (`F2`): functions, types, variants, traits, methods, constants and local
  variables, across the modules. The server checks the result before it answers and
  refuses a rename that would clash with another name or change what a name refers to —
  the reason is shown. Fields and standard-library names cannot be renamed yet.
- **Completion** (typing a name, `.`, `Ctrl+Space`): after `recv.` the fields and methods
  of the receiver's type, including the methods of the traits it implements; elsewhere
  the local variables first, then the file's functions and types, then the standard
  library, builtins and keywords. In a type annotation only types are offered. After
  `Type::` the variants and associated functions (or a trait's methods), after
  `module::` the module's public items, and in a `use` path (`use std::`,
  `use std::math::{sqrt, `) the modules and names it can import. It works
  on a line that does not parse yet (`p.` on its own, an unclosed call). Nothing is
  offered inside strings and comments or where a new name is declared; the extension
  turns off VS Code's word-based suggestions for Skarn files
  (`"[skarn]": { "editor.wordBasedSuggestions": "off" }`), which would otherwise fill
  those places with words from the document. A setting of your own takes precedence.
- **Signature help** (typing `(` or `,`, `Ctrl+Shift+Space`): inside a call, the callee's
  signature with the current argument highlighted — functions, methods (`p.m(` without
  `self`), `Type::m`, tuple variants and structs, closures, builtins and natives. It works
  on a line that does not parse yet. A bare trait-method call `m(x)` shows every trait's
  `m`, and a builtin such as `get` one signature per accepted shape.
- **Formatting** (*Format Document*, `Shift+Alt+F`, or format on save): Skarn's one fixed
  format — bodies on their own lines, 4 spaces, 80 columns, one element per line when a
  call or literal does not fit, operator chains broken before the operator. Comments and
  string literals are kept exactly. A file with syntax errors is not formatted; the reason
  is shown.

Syntax errors do not stop the other features: every syntax error is reported, the
parser resumes after it, and outline, hover, definition and references keep working on
the rest of the file. Type errors inside a function that has a syntax error are hidden
until it parses again (most of them would be caused by the missing text), and so are the
warnings of that file. Rename waits until the syntax errors are fixed.


## What it colors

- Line `//` and **nested** block `/* /* */ */` comments
- Strings with escapes and `"${ … }"` interpolation (holes switch back to the
  expression context; `${e:spec}` format specifiers are colored separately)
- Raw strings `r"…"`, `r#"…"#`, `r##"…"##`, … (verbatim, no escapes/interpolation)
- Char literals `'A'` / `'\n'`
- Numbers: decimal, float, `0x` / `0o` / `0b`
- Keywords, storage modifiers, built-in and prelude types, ~430 built-in / prelude
  functions
- Case-based identifiers: `UIdent` → type, `fn name` → function name
- Operators and punctuation (`|>`, `->`/`=>`, `>>>`, `..`, `?`, `::`, …)

## Installing the extension and the server

This extension is pure JavaScript and works on every platform; it carries no binary. What it
needs is the language server `skarn_lsp`, which it starts. Each release of
[Skarn](https://github.com/stefan-zobel/Skarn/releases) contains both, this extension
(`skarn-language-<version>.vsix`) and the server:

- **Windows x64**: `Skarn-<version>-windows-x64.zip`, with `skarn_lsp.exe`.
- **macOS on Apple Silicon**: `Skarn-<version>-macos-arm64.tar.gz`, with `skarn_lsp`.

The `.vsix` in the two archives is the same file. On any other system, build the server from
source (see below).

1. Install the extension: in the Extensions view choose **…** → **Install from VSIX…** and
   pick the `.vsix`, or run `code --install-extension skarn-language-0.3.0.vsix`.
2. Tell it where the server is: open the Settings, search for `skarn`, and set
   **Skarn › Server: Path** to the full path of the server binary — in `settings.json`, on
   Windows:

   ```json
   "skarn.server.path": "C:/Tools/skarn-0.3.0/skarn_lsp.exe"
   ```

   on macOS:

   ```json
   "skarn.server.path": "/Users/you/skarn-0.3.0/skarn_lsp"
   ```

   Or put that folder on the PATH; the default value is just `skarn_lsp`.
3. Reload the window (*Developer: Reload Window*).

On Windows, `skarn_lsp.exe` needs the Visual C++ runtime DLLs that lie next to it in the release
folder; keep them together. On macOS the binaries are not signed or notarized: clear the
quarantine flag once for the release folder (`xattr -dr com.apple.quarantine .`), or macOS
refuses to start the server. A server built from source needs neither.

The key bindings above are the Windows ones; on macOS read `Cmd` for `Ctrl` and `Option` for `Alt`.

## The language server

`skarn_lsp` is built with the rest of the project — `x64/Release/skarn_lsp.exe` from the Visual
Studio solution, `build/skarn_lsp` from the CMake build — and found through the same setting (or
the PATH). Reload the window after changing the setting.

How files are checked: every open `.skn` file that no other open file imports is
checked as a program, together with everything it imports — so open the program's entry
file, and errors inside its imported modules are reported on those modules. Imports
resolve like `static_vmrun`: `import net::http` is `net/http.skn` next to the entry file.
Unsaved edits count: an open module is checked in its current state.

`skarn_lsp --selftest` runs the server's own test suite. How the server works is described in
[docs/LanguageServer.md](https://github.com/stefan-zobel/Skarn/blob/HEAD/docs/LanguageServer.md).

## Install / develop from source

1. Run `npm install` in this folder (it fetches `vscode-languageclient`).
2. Open the folder (`tools/vscode-skarn`) in VS Code.
3. Press **F5** ("Run Extension") to launch an Extension Development Host with the
   extension loaded. Open any `.skn` file.

To install it permanently, package it with [`vsce`](https://github.com/microsoft/vscode-vsce)
(Node.js on the PATH):

```
npm install -g @vscode/vsce
cd tools/vscode-skarn
npm ci
vsce package          # produces skarn-language-0.3.0.vsix
code --install-extension skarn-language-0.3.0.vsix
```

The package contains the extension, its `vscode-languageclient` dependency, this README,
the changelog and the license — not the `skarn_lsp` binary, which is installed separately and is
what makes one `.vsix` usable on every platform.

## Grammar source

`syntaxes/skarn.tmLanguage.json` is a copy of the portable, standalone grammar at
[`docs/skarn.tmLanguage.json`](https://github.com/stefan-zobel/Skarn/blob/HEAD/docs/skarn.tmLanguage.json) (the canonical
version — also consumable directly by Sublime Text, GitHub Linguist, etc.). Keep the
two in sync; when the language's keyword / built-in set changes, update
`docs/skarn.tmLanguage.json` first (it tracks `tools/skarn.npp-udl.xml`) and copy it
here.
