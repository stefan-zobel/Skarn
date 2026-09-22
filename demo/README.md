# Demos

Small Skarn programs, most of them one per language feature or standard-library module. Each file starts
with a comment that explains what it shows. Unlike the programs under [`examples/`](../examples/README.md),
the demos are not checked by the documentation harness, so their exact output is not guaranteed.

Run a demo from the repository root:

```bash
x64\Release\static_vmrun.exe demo\json.skn
```

## Language features

| file | shows |
|---|---|
| `interp.skn` | string interpolation `"${expr}"` |
| `pattern_alt.skn` | or-patterns `A \| B => …`, including alternatives that bind |
| `dyn_traits.skn` | trait objects: one `Vec[dyn Draw]` holding values of different types |
| `iterator_for.skn` | lazy `for` over the `Iterator` / `IntoIterator` protocol |
| `string_iter.skn` | `for c in s` over the bytes of a string |
| `clone.skn` | shallow copies of containers and a user-defined `impl Clone` |
| `const_crc_table.skn` | a `const Array[Int]` lookup table driving CRC-32; checks itself |

## Standard library

| file | module |
|---|---|
| `map_helpers.skn` | `getOr`, `getOrElse`, `getOrPut`, `upsert` on maps |
| `set.skn` | `std::set` |
| `math.skn` | `std::math` |
| `bytes.skn` | `std::bytes`: a little-endian binary reader and writer |
| `json.skn` | `std::json`: parse, navigate, serialize |
| `regex.skn` | `std::regex`; checks itself |
| `random.skn` | `std::random` |
| `time.skn` | `std::time` |
| `cli.skn` | `std::cli`: pass arguments, e.g. `demo\cli.skn build --opt=O2 -v main.skn` |
| `file_io.skn` | `std::io`: writes, reads and removes a file in the current directory |
| `net_echo.skn` | `std::net`: a TCP echo over the loopback interface |
| `http_get.skn` | `std::net`: an HTTP GET to example.com; needs a network connection |
| `poll_server.skn` | `std::poll`: an HTTP server and two clients on ONE thread, driven by one readiness loop |
| `gc_stats.skn` | `gcResetStats` / `gcStats`: measuring garbage-collector activity from Skarn |

## Larger programs

| path | what it is |
|---|---|
| `json_parser.skn` | a recursive-descent JSON parser written from scratch |
| `aes256.skn` | AES-256 with table-driven and byte-oriented paths, checked against the FIPS-197 and NIST test vectors (not constant-time; not for real use) |
| [`raytracer/`](raytracer/README.md) | a path tracer spread over several modules that writes a BMP; `--preview` renders in about two seconds, `--threads=N` renders on N tasks |
| [`actors/`](actors/README.md) | actors: a word count by a reader, N counter actors and a collector, checked against a sequential count |
| [`actor_server/`](actor_server/README.md) | an HTTP server whose acceptor hands each connection to one of N worker actors, and a load generator on tasks |

## Benchmark

`bench/bench.skn` times three workloads (an integer loop, recursive Fibonacci, a lazy pipeline) and prints
the cost per iteration, call or element. `bench/bench.py` runs the same workloads in CPython. The timings
depend on the machine; a run takes several seconds.
