# Skarn examples

Small but complete programs, written the way a newcomer would write them after reading the two guides
([Skarn in 30 minutes](../SkarnIn30Minutes.md) and the [Skarn Guide](../SkarnGuide.md)). Where `demo/` shows
one feature at a time, each program here solves a small real task and uses whatever it needs.

Run one from its directory, so relative paths find its data files:

```
cd examples/word_freq
..\..\x64\Release\static_vmrun.exe main.skn --top=6 --min-len=4 input.txt
```

| example | what it does | what it shows |
|---|---|---|
| [`word_freq`](word_freq/main.skn) | word frequencies of a text file | `std::cli` options, file reading, `?` in a `run` fn, a Map counter, `sortBy`, format specifiers |
| [`todo_json`](todo_json/main.skn) | a todo list saved to and loaded from JSON | inherent `impl` + `mut self`, building and reading `std::json`, file I/O |
| [`bank_account`](bank_account/main.skn) | accounts, transfers and an audit log | an error enum in `Result`, `?` chains, record update `..acc`, a trait on the error type |
| [`text_adventure`](text_adventure/main.skn) | a scripted adventure game | enums as states and commands, tuple patterns, `std::set`, `while let` |
| [`calculator`](calculator/main.skn) | tokenizer, parser and evaluator with variables | a recursive enum AST, recursive descent via `mut self`, match guards, errors with positions |
| [`csv_stats`](csv_stats/main.skn) | column statistics and grouping for a CSV file | `lines`/`split`, `mapErr`, folds over `Double`, `getOrPut`, `maxBy`, `std::math` |
| [`inventory`](inventory/main.skn) | stock, tags and an order queue | `upsert`/`getOrPut`/`delete`, set union/intersect/difference, an int-backed enum |
| [`log_analysis`](log_analysis/main.skn) | a summary of a web-server access log | `std::regex` named captures, `?` on `Option` in a method, an `Array` histogram |
| [`game_of_life`](game_of_life/main.skn) | Conway's Game of Life on a wrapping board | `Array[Bool]` as a grid, associated functions, or-patterns over tuples, `StringBuilder` |
| [`priority_queue`](priority_queue/main.skn) | a task scheduler and Dijkstra's shortest paths | a generic `impl[T: Ord]` heap, implementing `Ord`, Maps of Vecs |
| [`dates`](dates/main.skn) | a project plan over working days, plus a calendar | `std::time` instants, durations, ISO-8601, weekdays, `?` on `Option` |
| [`shapes_modules`](shapes_modules/main.skn) | a shape report split into modules | `import` of a nested module, `use`, `pub`, `Vec[dyn Shape]`, a fallible constructor |
| [`binary_tree`](binary_tree/main.skn) | a persistent binary search tree | a recursive generic enum with an inherent `impl`, structural sharing, folds |

## How they are checked

`tests/run_examples.ps1` runs every example with `--strict` in a temporary copy of its directory and
compares the output with `expected.out` byte for byte; `args.txt` holds the command-line arguments where a
program takes any. It is part of the documentation gate, `tests/guide_claims/run_guide_claims.ps1`.

After an intended change in output, regenerate with `powershell -File tests/run_examples.ps1 -Update` and
review the diff of the `expected.out` files before keeping it.
