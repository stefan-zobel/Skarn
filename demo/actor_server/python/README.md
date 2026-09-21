# The actor server in Python

The HTTP server of [`../server.skn`](../server.skn), written again in Python 3.14 in four ways, one of them
also run on the free-threaded build, and a script that measures all five contenders against the Skarn
original with the same client. The aim is a comparison that holds up when checked: Python may look bad where it is weak, but
must not be put at a disadvantage.

| contender | how the workers run | closest to Skarn? |
|---|---|---|
| `subinterp` | one subinterpreter per worker, each on its own thread with its own GIL; objects are not shared, values are copied | **yes**: the counterpart of Skarn's isolates |
| `ft-threads` | worker threads on the free-threaded build (no GIL): real parallelism, shared memory | the other way to many cores: shared memory, which Skarn decided against |
| `threads` | worker threads in one interpreter, under one GIL | shares memory; no parallel computing |
| `procs` | one process per worker; the connection travels with `socket.share()` | share-nothing, but heavier |
| `prefork` | one process per worker, each accepting on the shared listening socket (the gunicorn way) | a different architecture: no acceptor, no hand-off |

`threads` and `ft-threads` run the very same code (`server.py --model=threads`); only the interpreter differs.

## Files

- `server.py`: the acceptor and the setup of each model. The workers ask for work, as
  `server.skn --dispatch=pull` does: each reports itself free, and the acceptor hands a connection only to
  a worker that did.
- `workers.py`: the request handling (`work`, `route`, `serve`) and one worker loop per model. It is a
  module of its own because a subinterpreter can only run a function it can import by name.
- `bench.py`: the measurement.

```
python server.py [--model=threads|subinterp|procs|prefork] [--workers=N] [--port=P] [--port-file=F]
python bench.py --vm=<static_vmrun> [--python=<python>] [--python-ft=<python3.14t>]
                [--contenders=a,b,...] [--rounds=5] [--out=DIR] [--smoke]
```

`bench.py` needs `psutil` and a Release build of `static_vmrun`. `--python-ft` adds `ft-threads`; the
script checks that its GIL is really off. `--contenders` measures only some. `--smoke` is a quick
functional check. Without it, the script refuses to start while the CPU is busy.

A free-threaded CPython for Windows that needs no installer is the `python-freethreaded` package on
nuget.org: rename the `.nupkg` to `.zip`, unpack it, and use `tools\python3.14t.exe`.

## What keeps the comparison fair

- **The same client for everyone:** `../loadgen.skn`. It checks every answer, and the `/work` answer is
  checked for its exact value, so both sides provably compute the same thing.
- **The same architecture and the same work:**
  - one acceptor, N workers, one request per connection;
  - the same routes and headers;
  - the same digit-sum loop in `work()`, statement for statement.
- **Interpreter speed is reported apart from scaling.** CPython takes about 2.3 times as long per unit of
  work as Skarn. The tables therefore give each contender's speed-up against its own one-worker row, and
  one line times `work(8000)` alone on a single thread.
- **Every Python model gets its best standard-library mechanism.**
  - The subinterpreters talk over a local socket pair, not `concurrent.interpreters.Queue`. That queue's
    `get()` polls and sleeps 10 ms whenever it is empty, which would stall a worker after every
    connection.
  - Processes receive the connection through `socket.share()`, not the slower pickling path.
  - The threads hand over through `queue.Queue`, which blocks properly.
  - `prefork` is there for the objection "a Python server would not hand sockets between processes".
- **The same start for everyone:**
  - every server announces its port right after `listen`, and accepts at once;
  - a worker that is still starting has simply not reported yet;
  - both sides create all their workers before the first `accept`.
- **Every load run starts with a short warm-up.** The contenders take turns in a different order in each
  of five rounds, and the tables show medians.
- **Memory is measured in runs of its own**, so sampling it does not slow the throughput runs.

## Results

On a laptop with six cores and 12 threads, shared by server and load generator. CPython 3.14.7, the GIL
build and the free-threading build of the same version. 16 clients, medians of five rounds (memory: of
three), all six contenders in one run, 330 load runs, no failed request.

**CPU-bound, `/work/8000`:** requests/s, (speed-up against the own one-worker row), p50 / p99 in ms.

| workers | Skarn | subinterp | ft-threads | threads | procs | prefork |
|---|---|---|---|---|---|---|
| 1 | 936 (1.0×) · 17.0 / 17.7 | 406 (1.0×) · 39.2 / 42.2 | 385 (1.0×) · 41.3 / 44.1 | 425 (1.0×) · 37.6 / 39.5 | 410 (1.0×) · 38.6 / 42.6 | 432 (1.0×) · 36.9 / 37.8 |
| 2 | 1 743 (1.9×) · 9.1 / 9.9 | 821 (2.0×) · 19.3 / 22.9 | 763 (2.0×) · 20.7 / 24.8 | 425 (1.0×) · 37.5 / 41.1 | 796 (1.9×) · 19.8 / 23.2 | 858 (2.0×) · 18.5 / 21.5 |
| 4 | 2 907 (3.1×) · 5.4 / 6.4 | 1 347 (3.3×) · 11.7 / 14.4 | 1 204 (3.1×) · 13.0 / 17.1 | 429 (1.0×) · 37.0 / 46.4 | 1 134 (2.8×) · 13.9 / 16.8 | 1 299 (3.0×) · 11.7 / 16.5 |
| 6 | 3 614 (3.9×) · 4.3 / 5.7 | 1 562 (3.8×) · 10.0 / 13.5 | 1 399 (3.6×) · 11.1 / 14.8 | 425 (1.0×) · 37.2 / 49.3 | 1 345 (3.3×) · 11.8 / 14.3 | 1 516 (3.5×) · 10.0 / 14.0 |
| 12 | 5 033 (5.4×) · 3.0 / 4.7 | 1 897 (4.7×) · 7.8 / 13.7 | 1 704 (4.4×) · 9.1 / 12.9 | 424 (1.0×) · 37.3 / 48.2 | 1 770 (4.3×) · 8.7 / 12.3 | 1 861 (4.3×) · 8.4 / 11.2 |

**I/O only, `/`:** requests/s and speed-up.

| workers | Skarn | subinterp | ft-threads | threads | procs | prefork |
|---|---|---|---|---|---|---|
| 1 | 16 731 (1.0×) | 11 257 (1.0×) | 15 682 (1.0×) | 12 320 (1.0×) | 6 487 (1.0×) | 12 746 (1.0×) |
| 4 | 20 645 (1.2×) | 15 392 (1.4×) | 14 643 (0.9×) | 13 766 (1.1×) | 10 000 (1.5×) | 20 176 (1.6×) |
| 12 | 20 280 (1.2×) | 14 996 (1.3×) | 16 882 (1.1×) | 13 624 (1.1×) | 9 846 (1.5×) | 18 430 (1.4×) |

`ft-threads` at four workers had one slow round (10 800), which pulls its median below its neighbours
(16 900 at two, 16 300 at six workers).

**Start, memory, single-thread speed:**

| | Skarn | subinterp | ft-threads | threads | procs | prefork |
|---|---|---|---|---|---|---|
| start to first answer, 1 worker | 54 ms (12 ms from `.skbc`) | 100 ms | 80 ms | 76 ms | 150 ms | 148 ms |
| start to first answer, 12 workers | 44 ms (12 ms from `.skbc`) | 281 ms | 76 ms | 78 ms | 212 ms | 212 ms |
| private memory, 12 workers, peak under load | 6.2 MB | 67 MB | 15.5 MB | 10.7 MB | 122 MB | 122 MB |
| one `work(8000)` on one thread | 0.83 ms | 1.90 ms | 1.98 ms | 1.90 ms | 1.90 ms | 1.90 ms |
| time per request, one worker, `/work/8000` | 1.07 ms | 2.46 ms | 2.60 ms | 2.35 ms | 2.44 ms | 2.31 ms |
| time per request, one worker, `/` | 60 µs | 89 µs | 64 µs | 81 µs | 154 µs | 78 µs |

The time per request is 1 / requests per second with one worker, which is busy all the time: what one
request costs, from accepting to closing. At `/work/8000` it is the work plus about 0.25 ms (Skarn) or
0.4–0.6 ms (Python) of HTTP and hand-off.

Skarn's start from source includes compiling the program; from a `.skbc` image it does not. Private memory
counts each page once; the plain resident size of `procs` is 211 MB, because every process maps the same
libraries again.

**The machine slows down under sustained load.** At twelve workers every contender lost a quarter or more of
its rate from the first round to the last (Skarn 5 495 → 3 959, subinterpreters 2 090 → 1 446), most
likely thermal throttling. The ratios stayed put: Skarn served 2.8 to 3.0 times as many requests as
`ft-threads` in every single round. Absolute numbers on this machine are good to about ±15 %; the
comparisons within one run are what the rotating order protects.

## What the numbers say

- **Skarn is ahead or level on every absolute measure:** requests per second, latency, time per request,
  start time and memory. It is behind on none.
  - For CPU-bound requests it serves 2.0–2.7 times as many as the best Python contender, at every worker
    count, with a lower p50 and p99. At twelve workers its p99 (4.7 ms) is below every Python
    contender's median.
  - Only for pure I/O does one Python model draw level: `prefork` at four and six workers, within 2.3 %
    either way. At the other worker counts Skarn is 7–10 % ahead of the best Python contender.
- **The scaling factor is where Python comes closest.** Up to six workers the subinterpreters scale as well
  as Skarn (3.3× against 3.1× at four); at twelve Skarn pulls ahead, 5.4× against 4.7×. And the factor
  flatters the slower interpreter: the serial part (accepting, handing over) costs both about the same,
  and next to work that takes twice as long it weighs less.
- **Where Skarn's lead comes from:**
  - **Throughput and latency:** mostly the interpreter. One `work(8000)` takes CPython 2.3 times as long.
  - **Start and memory:** the actor model itself, i.e. what an isolate costs compared with a
    subinterpreter or a process, below.
- **Threads with the GIL do not scale at all when there is computing to do:** 1.0× on every worker count,
  and their p99 grows with more threads (39 → 49 ms). This is where classic Python is really weak.
- **Free-threaded Python fixes that, at a small price.** The same threads code scales 4.4× without the
  GIL, starts in 76 ms and needs 15.5 MB for twelve workers. It computes about 4 % slower per thread
  (10 % in an earlier run), and on CPU-bound requests the subinterpreters stayed 7–15 % ahead of it in
  every round. For I/O it is the best Python contender with one worker.
- **The price of isolation is paid in memory and start time.**
  - A subinterpreter costs about 5 MB and 17 ms. Twelve are 67 MB and about 0.3 s before the first
    answer; Skarn's twelve actors are 6 MB and 44 ms.
  - Processes are heavier still: 122 MB private for twelve.
- **Shared memory is what free-threading costs in safety, not in speed.** Its threads can change the same
  object at the same time, and guarding that is the programmer's job. Skarn's actors share nothing, and the
  checker keeps what cannot be copied out of messages.
- **For pure I/O Python is not weak.** The prefork server matches Skarn at four and six workers, at about
  20 000 requests/s. Handing each connection to another process halves the throughput (`procs`).

## What was not measured

- **An asyncio server:** it is one thread, like Skarn's `std::poll` loop, not a comparison of actor models.
- **The POSIX code paths of `procs` and `prefork`:** these pass the socket itself through `multiprocessing`
  instead of `socket.share()`, and they were not run. All numbers are from Windows.
