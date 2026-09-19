# Changelog

## 0.2.0

- The Skarn language server `skarn_lsp` (installed separately, found through the setting
  `skarn.server.path` or the PATH):
  - live diagnostics: type errors, parse errors and warnings while you type, with every
    syntax error reported and the rest of the file still analyzed;
  - outline, hover, go to definition, find references and highlight;
  - rename, checked before it is applied;
  - completion: members after `.`, the names in scope, and after `Type::`, `module::` and
    in `use` paths;
  - signature help while typing the arguments of a call;
  - formatting in Skarn's fixed format (*Format Document*).
- VS Code's word-based suggestions are turned off for Skarn files.

## 0.1.0

- Syntax highlighting, comment toggling and bracket matching for `.skn` files.
