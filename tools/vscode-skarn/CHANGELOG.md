# Changelog

## 0.3.0

- Syntax highlighting for the concurrency half of the standard library: `std::task`, `std::actor`,
  `std::supervisor` and `std::log`, together with the socket hand-off in `std::net` — 70 further
  function names and 22 further types, among them `spawnActor`, `trySend`, `select`, `ask`,
  `supervise`, `startLogger`, `Pid`, `Inbox`, `Task`, `Sendable` and `Supervised`.
- Syntax highlighting for active connections and listeners in `std::net`: `activate` and the types
  `ActiveConn`, `SockEvents`, `SockEvent`, `Framing`, `ActiveListener`, `IncomingClients` and `Incoming`.

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
