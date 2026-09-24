# An HTTP server on actors

A web server built from actors (`std::actor`) and a load generator built from tasks (`std::task`). See
"Tasks and actors" in [VirtualMachine.md](../../docs/VirtualMachine.md) for the runtime and "Actors" in
[Compiler.md](../../docs/Compiler.md) for the rules the checker applies.

## `server.skn`

One acceptor, N workers:

```
main (acceptor) --accept--> c.handOff() --send--> worker k --h.take()--> answer, close
```

A `TcpConn` cannot be sent: its descriptor means something only in the actor that opened it. So the acceptor
detaches each new connection with `c.handOff()`, which returns a ticket that CAN be sent, and the worker
turns the ticket back into a connection with `h.take()`. From then on the acceptor's `TcpConn` is dead:
every operation on it returns an `Err`. Each worker is one `for h in inbox.messages()` loop.

Two ways to choose the worker, both plain Skarn:

- **`rr`, round robin** (the default): worker 0, 1, 2, … The acceptor does not know which worker is busy,
  so a request may wait behind one while another is idle.
- **`pull`, the workers ask for work**: each worker sends its own address to the acceptor's inbox
  (`mainInbox()`) when it is free, and the acceptor hands a connection only to a worker that did. While
  none is free, new connections wait in the system's accept queue, in arrival order. A worker that crashes
  arrives as an exit report and is dropped.

Two routes, so the load can be I/O or CPU:

| route | answer |
|---|---|
| `/` | a greeting; almost pure I/O |
| `/work/<n>` | the digit sum of 1..n, computed first; `/work/8000` is about 1 ms of work |

```
skarnvm demo/actor_server/server.skn [--workers=N] [--dispatch=rr|pull] [--credits=C] [--port=P]
                                          [--requests=K] [--port-file=F]
```

| option | effect |
|---|---|
| `--workers=N` | worker actors (default 4); 0 answers on the main thread, without actors |
| `--dispatch=D` | `rr` (default) or `pull`, see above |
| `--credits=C` | `pull` only: connections a worker may hold at once (default 1) |
| `--port=P` | port to listen on (default 0: the system picks a free one, printed at start) |
| `--requests=K` | stop after K connections (default 0: serve until killed) |
| `--port-file=F` | write the port to file F once listening, for scripts |

Try it with `curl http://127.0.0.1:<port>/work/100`.

## `loadgen.skn`

C clients at once, each a task sending R requests one after another, one connection per request. It checks
every answer (status line and expected body) and prints requests per second and the latency percentiles.

```
skarnvm demo/actor_server/loadgen.skn --port=P [--clients=C] [--requests=R] [--path=/] [--expect=TEXT]
```

In Git Bash, set `MSYS_NO_PATHCONV=1` first: otherwise `--path=/work/8000` reaches the program as a Windows
path.

## Results

On a six-core laptop (12 threads, shared by server and load generator), 16 clients, medians of five rounds:

| workers | `/` requests/s | p50 | `/work/8000` requests/s | p50 | p95 |
|---|---|---|---|---|---|
| 0 | ~16 500 | 0.9 ms | ~1 040 | 15.2 ms | 16.1 ms |
| 1 | ~21 900 | 0.7 ms | ~1 060 | 15.0 ms | 15.9 ms |
| 2 | ~20 800 | 0.7 ms | ~1 990 | 7.7 ms | 13.0 ms |
| 4 | ~21 200 | 0.7 ms | ~3 210 | 4.3 ms | 10.4 ms |
| 6 | ~21 400 | 0.7 ms | ~3 880 | 3.0 ms | 8.1 ms |
| 12 | ~20 900 | 0.7 ms | ~5 140 | 2.3 ms | 5.6 ms |

No request failed in any run.

- **Requests that need a core scale**: 1.9× with two workers, 4.9× with twelve.
- **Requests that are only I/O do not.** One worker is a third faster than none, because accepting and
  answering now overlap; more workers change nothing, since they are not the bottleneck.
- **Round robin costs latency.** The acceptor does not know which worker is busy, so a request may wait
  behind another while a worker is idle: at four workers the p95 is more than twice the p50.

With `--dispatch=pull`, measured side by side with round robin in a second run (`/work/8000`):

| workers | dispatch | requests/s | p50 | p95 | p99 |
|---|---|---|---|---|---|
| 4 | rr | ~2 900 | 3.7 ms | 10.5 ms | 11.9 ms |
| 4 | pull | ~3 020 | 5.2 ms | 6.0 ms | 6.7 ms |
| 6 | rr | ~3 760 | 3.6 ms | 9.6 ms | 10.6 ms |
| 6 | pull | ~3 940 | 3.9 ms | 4.6 ms | 5.1 ms |
| 12 | rr | ~4 800 | 2.4 ms | 6.5 ms | 8.3 ms |
| 12 | pull | ~5 250 | 2.8 ms | 4.0 ms | 5.2 ms |

- **Pull halves the slowest answers at the same throughput.** The median rises a little: requests are
  served in arrival order, so everyone waits about the same instead of some quickly and some long.
- **A single worker with one credit is ~10 % slower** than round robin: after each answer it waits for
  the acceptor's next connection. `--credits=2` closes that gap when workers are few; with many it brings
  back the waiting behind a busy worker.
- **For `/` the dispatch makes no difference** from two workers on.

## The same server in Python

[`python/`](python/README.md) has this server in Python 3.14 in four ways: threads, subinterpreters,
processes, and prefork. The threads also run on the free-threaded build, without the GIL. A script measures
all of them against `server.skn` with this load generator. In short:
- Skarn is ahead or level on every measure;
- subinterpreters and free-threaded threads scale almost as well as Skarn's actors (4.7× and 4.4× against
  5.4× on twelve workers), at well under half the requests per second;
- threads with the GIL do not scale at all;
- isolation costs Python several times the memory and start time.
