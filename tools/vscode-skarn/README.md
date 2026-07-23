# Skarn for VS Code

Syntax highlighting for the [Skarn](../../LanguageIntroduction.md) language (`.skn`
files, run by `static_vmrun`).

This is a **grammar-only** extension: it provides TextMate-based syntax highlighting
and editor niceties (comment toggling, bracket matching, auto-closing). It is **not**
semantically aware — it does not type-check, resolve names, or offer completion. A
language server would add those on top.

## What it colors

- Line `//` and **nested** block `/* /* */ */` comments
- Strings with escapes and `"${ … }"` interpolation (holes switch back to the
  expression context; `${e:spec}` format specifiers are colored separately)
- Raw strings `r"…"`, `r#"…"#`, `r##"…"##`, … (verbatim, no escapes/interpolation)
- Char literals `'A'` / `'\n'`, atoms `:name`
- Numbers: decimal, float, `0x` / `0o` / `0b`
- Keywords, storage modifiers, built-in and prelude types, ~230 built-in / prelude
  functions
- Case-based identifiers: `UIdent` → type, `fn name` → function name
- Operators and punctuation (`|>`, `->`/`=>`, `>>>`, `..`, `?`, `::`, …)

## Install / develop from source

1. Open this folder (`tools/vscode-skarn`) in VS Code.
2. Press **F5** ("Run Extension") to launch an Extension Development Host with the
   extension loaded. Open any `.skn` file to see highlighting.

To install it permanently, package it with [`vsce`](https://github.com/microsoft/vscode-vsce):

```
npm install -g @vscode/vsce
cd tools/vscode-skarn
vsce package          # produces skarn-language-0.1.0.vsix
code --install-extension skarn-language-0.1.0.vsix
```

## Grammar source

`syntaxes/skarn.tmLanguage.json` is a copy of the portable, standalone grammar at
[`docs/skarn.tmLanguage.json`](../../docs/skarn.tmLanguage.json) (the canonical
version — also consumable directly by Sublime Text, GitHub Linguist, etc.). Keep the
two in sync; when the language's keyword / built-in set changes, update
`docs/skarn.tmLanguage.json` first (it tracks `tools/skarn.npp-udl.xml`) and copy it
here.
