# Changelog

## 0.4.0

- Syntax highlighting for the standard library added since 0.3.0 — 37 further function names and
  six further types:
  - `std::resp`, RESP2, the protocol Redis speaks: the encoder (`appendSimple`, `appendError`,
    `appendInteger`, `appendBulk`, `appendNull`, `appendNullArray`, `appendArrayHeader`,
    `appendResp`, `appendCommand`, `encodeCommand`, `encode`, `render`), the incremental decoder
    (`feed`, `nextCommand`, `buffered`) and the client (`call`, `pipeline`), with the types `Resp`,
    `RespReader` and `RespClient`.
  - Files that stay open in `std::io`: `openFile`, `read`, `readAll`, `write`, `sync`, and the
    types `File` and `FileMode`.
  - `std::deque`, a double-ended queue: `pushFront`, `pushBack`, `popFront`, `popBack`,
    `peekFront`, `peekBack`, `clear`, and the type `Deque`.
  - Output and ending a program: `flushOutput`, `eprint`, `eprintln` and `std::process`'s `exit`.
  - `std::process`'s `cpuCount`, `std::poll`'s `selectIo`, and `std::bytes`' `indexOfByte` and
    `subBytes`.
- The internal `raw*` and `tcp*` natives are **no longer highlighted** — 48 names, among them
  `rawSpawnActor`, `rawSend`, `tcpConnect` and `tcpAccept`. They are the private half of the
  library that the `std` wrappers are written on; a program cannot call them, so highlighting them
  as built-ins was misleading.
- The three word lists (this grammar, the one under `docs/`, and the Notepad++ user-defined
  language) are now checked against the compiler on every documentation run, so the highlighting
  can no longer drift from the language.
- The README names the driver `skarnvm` on both platforms, which is what it is called since 0.3.0
  was published.

## 0.3.0

- Syntax highlighting for the concurrency half of the standard library: `std::task`, `std::actor`,
  `std::supervisor` and `std::log`, together with the socket hand-off in `std::net` — 70 further
  function names and 22 further types, among them `spawnActor`, `trySend`, `select`, `ask`,
  `supervise`, `startLogger`, `Pid`, `Inbox`, `Task`, `Sendable` and `Supervised`.
- Syntax highlighting for active connections and listeners in `std::net`: `activate`, `setSendTimeout`
  and the types `ActiveConn`, `SockEvents`, `SockEvent`, `Framing`, `ActiveListener`, `IncomingClients`
  and `Incoming`.
- The README describes both release archives, Windows and macOS on Apple Silicon, with a server path
  for each, the quarantine flag on macOS and the macOS key bindings.

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
