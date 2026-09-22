# Actors

Programs built from actors (`std::actor`): functions on their own threads that share nothing and talk only
by messages. See "Tasks and actors" in [VirtualMachine.md](../../docs/VirtualMachine.md) for the runtime and
"Actors" in [Compiler.md](../../docs/Compiler.md) for the rules the checker applies.

## `wordcount.skn`

A reader, N counters and a collector:

```
main (reader) --batches of lines--> counter 1..N --partial counts--> collector --top 10--> main
```

The main program deals the text out in batches, round robin. Each counter keeps its own `Map` of counts and,
when told to finish, sends it to the collector. The collector merges the partial counts and sends the ten
most frequent words to the main program. The program also counts sequentially and checks that both results
agree.

```
static_vmrun demo/actors/wordcount.skn [file] [--counters=N] [--lines=N] [--batch=N] [--capacity=N]
```

| option | effect |
|---|---|
| `file` | count the words of this file instead of a generated text |
| `--counters=N` | counter actors (default 4) |
| `--lines=N` | lines of generated text (default 40 000; ignored with a file) |
| `--batch=N` | lines per message (default 250) |
| `--capacity=N` | each counter's mailbox holds at most N messages (default: no limit) |

On a six-core laptop, with 40 000 generated lines, counting sequentially takes about 545 ms. With actors:

| counters | 1 line per message | 25 lines | 250 lines |
|---|---|---|---|
| 1 | ~570 ms | ~565 ms | ~550 ms |
| 2 | ~300 ms | ~285 ms | ~285 ms |
| 4 | ~180 ms | ~170 ms | ~170 ms |
| 6 | ~170 ms | ~150 ms | ~160 ms |
| 12 | ~190–230 ms | ~130 ms | ~115 ms |

- **A message is cheap next to the work it carries.** Even with one message per line — 40 000 messages, each
  copied into the receiver's heap — up to six counters are barely slower than with batches.
- **With many counters, the reader becomes the limit.** It alone encodes and sends every message, so at
  twelve counters single lines no longer keep them busy. Batches of lines do, and scale to ~4.7×.
- **One counter costs about 5 % more than no actor at all**: the copies, and a collector that has nothing
  to merge.

**Back-pressure.** With one or two counters the reader sends faster than they count, and without a limit
the rest of the text piles up in their mailboxes. `--capacity=N` starts the counters with
`spawnActorBounded`, so a send to a full mailbox waits until the counter has taken a message. With a million
generated lines and two counters, `--capacity=2` lowers the peak working set from ~349 MB to ~265 MB at the
same speed (~10.2 s for the actor phase either way).

## `supervised.skn`

A pool of workers that crash, kept running by a supervisor (`std::supervisor`):

```
main --> dispatcher <--pull-- worker 1..4  (children of the keeper, restarted when they crash)
```

The workers pull their jobs from a dispatcher and report each result back to it. A worker crashes the first
time it is handed a multiple of 5. The keeper starts it again, and the dispatcher hands out again every job
whose result is missing once its queue is empty. The main program checks the total against a sequential
sum. Because the workers pull, a restarted worker's new address does no harm: nobody needs to reach it.

`--strategy=` picks what a crash costs the other workers: `one_for_one` (the default) restarts only the
worker that crashed, `one_for_all` stops all of them, waits until they have ended and starts them all
again, and `rest_for_one` does that for the workers started after the crashed one. All three give the same
total, because pull dispatch hands out again every job whose result is missing — a worker stopped mid-job
simply stops asking.

```
static_vmrun demo/actors/supervised.skn [--strategy=one_for_one|one_for_all|rest_for_one]
```

## `stable_address.skn`

A service that keeps ONE address across its restarts (`std::actor`'s `Slot` plus `std::supervisor`):

```
main --requests--> [ slot ] <-- the supervisor starts a service here, again after every crash
```

The service squares the numbers it is sent, and one request makes it crash. The supervisor starts a new
service at the same address, and the main program goes on using the `Pid` it has had from the start — it
never learns a new one. At the end the address is released, and a send to it answers `false`.

Each service announces itself when it starts, which is what makes the output the same under every
schedule: a request goes out only once its receiver is known to be up.

```
static_vmrun demo/actors/stable_address.skn
```
