# Actors in Skarn

A program that must do several things at once usually shares memory between threads and guards it with
locks. Skarn does not. Work that runs at the same time runs in **actors**: separate workers that share
nothing at all and talk only by sending messages.

This guide assumes you have read [Skarn in 30 minutes](SkarnIn30Minutes.md) or know the language from the
[Skarn Guide](SkarnGuide.md). It assumes **no** previous contact with actors, Erlang or OTP — the last
section says what those names mean once you have seen the thing itself.

Every example here is a real program that the project's test harness compiles and runs. A `// => ...`
comment is a line the program prints, in order, and a block that has them prints exactly those lines; a
block that shows a compile error marks each rejected line with `// error: ...`.

---

## 1. Why actors

If you have written concurrent code in Java, C# or Python, you know the shape of it: start threads, let
them touch the same objects, and put a lock around every place they might collide. It works, and it is
hard to get right. Forget one lock and you have a race that appears once a week on the customer's machine
and never in your tests. Take two locks in the wrong order and you have a deadlock.

An actor removes the problem instead of guarding it. An actor is a function running on its own thread,
with its own memory and a **mailbox** — a queue only it may read. Nothing else can reach what it holds.
Other actors do not call it; they **send it a message**, and the message is **copied** on the way.

There is nothing to lock because there is nothing to share.

| what you would reach for | what replaces it here |
|---|---|
| a shared object plus `synchronized` / `lock` | an actor that owns the state; others send it messages |
| `ExecutorService` / `Task.Run` / a thread pool | `spawnActor` — but the worker stays alive and keeps state |
| `BlockingQueue` / `queue.Queue` | the mailbox, and only its owner may read it |
| a `Future` / `Task<T>` / `await` for the result | a reply message, or `ask` (§6) |
| restarting a crashed worker by hand | a supervisor (§9) |

If you come from Python, the closest thing you know is **`multiprocessing`, not `threading`**: separate
memory, values copied across, no GIL to fight. The difference is that an actor costs about 0.2 ms and
half a megabyte to start rather than a whole process, so having a few hundred is normal.

The copy is what buys the safety, and it is also the price. Send small messages often rather than one
large object graph. In exchange, a whole class of bug — the one that does not reproduce — cannot occur.

## 2. Your first actor

Three things make an actor, and each has a name you will see everywhere:

- **`spawnActor(f, init)`** runs `f(inbox, init)` on a thread of its own. It returns the actor's
  **address**, a `Pid[M]` — a handle you can copy, store and send around. It is not a reference to the
  actor's memory; it is more like a postal address than a pointer.
- **`send(pid, m)`** puts a copy of `m` into that actor's mailbox and returns immediately. It does not
  wait for the actor to read it, and it gives you no result — there is no return value from a message.
- **`inbox.messages()`** is the actor's own end: the messages, one at a time, as a loop.

```rust
use std::actor::*

// The start value is whatever plain data the actor needs -- here a struct with two fields.
struct Setup { label: String, from: Int }

// The message carries the address to answer to: that is how a reply finds its way back.
enum Cmd { Add(Int), Total(Pid[String]) }

fn counter(inbox: Inbox[Cmd], s: Setup) -> () {
  let mut total = s.from
  for cmd in inbox.messages() {
    match cmd {
      Cmd::Add(n)     => { total = total + n },
      Cmd::Total(ask) => { send(ask, "${s.label}: ${total}") },
    }
  }
}

let me: Inbox[String] = mainInbox()   // the main program has one mailbox too
let c = spawnActor(counter, Setup { label: "hits", from: 100 })
send(c, Cmd::Add(5))
send(c, Cmd::Add(7))
send(c, Cmd::Total(me.pid()))
match me.receive() {
  Mail::Msg(t) => println(t),         // => hits: 112
  _            => println("no answer"),
}
```

Four things in that program are worth naming, because they are the whole model:

- **`total` is an ordinary `mut` binding.** In a threaded program it would be the field you must guard.
  Here nobody else can reach it, so nothing guards it — and no lock can be forgotten.
- **The loop is the actor's life.** `for cmd in inbox.messages()` blocks when the mailbox is empty and
  resumes when something arrives. It ends when the program ends, and the function returns — that is how
  an actor stops.
- **The start value is any plain data.** `Setup` is a struct here, and it could as well be a `String`, a
  `Vec`, or an address to report to. It is copied in at birth, like every message after it, and the actor
  owns its copy — `s.label` is the actor's.
- **The reply address travels in the message.** `send` has no return value, so `Cmd::Total(Pid[String])`
  carries the address to answer to. This is the shape of nearly every request in an actor program: *here
  is what I want, and here is where to put the answer.*
- **The main program is an actor too**, near enough: `mainInbox()` gives it one mailbox, and `me.pid()` is
  its address. That is what the counter answers to.

`send(c, Cmd::Add(5))` returns before the counter has looked at it. If you need to know the answer, you
wait for a message back — which the next sections make progressively less tedious.

## 3. The second parameter

`counter` above took a start value and used it. An actor that needs no starting state still has to name
the parameter, because that value is the one thing that crosses into the new actor with it — everything
else it must be sent. The convention is `unused: Int`, started with `0`.

```rust
use std::actor::*

fn greeter(inbox: Inbox[String], unused: Int) -> () {
  for who in inbox.messages() { println("hello, ${who}") }
}

let g = spawnActor(greeter, 0)
send(g, "Ada")                        // => hello, Ada
```

It is a wart with a reason: removing it would mean a second `spawnActor`, a second bounded one, a second
function value and a second supervisor child — the whole start surface twice.

## 4. The mail an actor gets

A mailbox carries more than the messages you send. `inbox.messages()` is the convenient form — it hands
you the messages and quietly deals with the rest. `inbox.receive()` is the full one, and what it answers
is a `Mail`, an enum with three cases:

- **`Msg(m)`** — a message somebody sent.
- **`Exited(id, reason)`** — an actor that *this* actor started has crashed. The crash does not spread;
  you are told about it, in the same queue as everything else, and you decide what to do.
- **`Stop`** — the program is ending. `messages()` ends the loop here by itself.

`receiveTimeout(ms)` is the same thing with a deadline: `None` if nothing arrived in time.

```rust
use std::actor::*

fn once(inbox: Inbox[Int], boss: Pid[Int]) -> () { send(boss, 1) }

let me: Inbox[Int] = mainInbox()
spawnActor(once, me.pid())

match me.receive() {
  Mail::Msg(n)        => println("message ${n}"),   // => message 1
  Mail::Exited(_, _)  => println("it crashed"),
  Mail::Stop          => println("ending"),
}
match me.receiveTimeout(20) {
  Some(_) => println("more mail"),
  None    => println("nothing else"),               // => nothing else
}
```

Use `messages()` unless you need the other two cases. An actor that starts children of its own needs
`receive()`, because those crash reports are the whole point — that is what a supervisor is built on (§9).

Note what did **not** happen: the crash report arrives as ordinary mail, in order, in the queue you were
already reading. There is no exception unwinding through your code and no handler to install. A failure
somewhere else is just another message.

## 5. What may cross between actors

This is where an actor program differs most from what you are used to, so it is worth being blunt about
it. In Java, C# or Python, passing an object to another thread passes a **reference**: both sides see the
same object, and that is exactly why you needed the lock. Here, sending a value **copies** it.

The consequence is worth stating twice: after `send(p, myList)`, the receiver has its *own* list.
Changing yours does not change theirs, and changing theirs does not change yours. There is no aliasing to
reason about — and no way to accidentally share something you meant to hand over.

Because it must be copied, a message must be **plain data**: numbers, text, `Bytes`, and collections,
tuples, structs and enums of those. An address (`Pid`) is plain data and may travel — it is just a number
that means the same thing everywhere. An `Inbox` may not: it is the *reading* end, and a copy of it in
another actor would let that actor read your mail.

```rust fail
use std::actor::*
fn keeper(inbox: Inbox[Inbox[Int]], unused: Int) -> () {}
let k = spawnActor(keeper, 0)   // error: cannot be sent to an actor
```

Function values, trait objects and open sockets are out for the same reason: each means something only in
the memory it came from. A lambda captures variables that live in *your* memory; a socket is a number the
operating system gave to *your* thread.

Notice **when** the compiler said no. Not at the `send`, but at `spawnActor` — where the address was made.
That is the rule throughout: a `Pid[M]` is proof that `M` can be sent, so by the time you hold one, every
`send` to it is already known to be safe. You never annotate anything to get this; the checker works it
out from the type's parts.

## 6. Asking for an answer

The counter in §2 answered into the main program's mailbox. That works, but it has two problems that grow
with the program. The answer arrives among everything else, so you have to sort it out from unrelated
mail; and its type must fit that one mailbox, so every kind of answer has to become a case of the same
enum.

`ask` is the request/reply pattern packaged: it makes a **private inbox for this one answer**, sends your
request, waits at most the given milliseconds, and closes the inbox again. It is the nearest thing here to
a `Future<T>` or an `await` — except that the waiting is explicit and bounded.

```rust
use std::actor::*

enum Query { Square(Int, Pid[Int]) }

fn squarer(inbox: Inbox[Query], unused: Int) -> () {
  for q in inbox.messages() {
    match q { Query::Square(n, replyTo) => { send(replyTo, n * n) } }
  }
}

let s = spawnActor(squarer, 0)
match ask(s, fn(me) { Query::Square(7, me) }, 1000) {
  Ok(n)  => println(n),               // => 49
  Err(e) => println(e),
}
```

The second argument looks odd at first: `fn(me) { Query::Square(7, me) }`. You pass a **function that
builds the request**, not the request itself, because the request has to contain the reply address — and
only `ask` knows what that address is, since it created the inbox a moment ago. So `ask` hands it to you
and you finish the message.

The result is a `Result`, and its error side is honest about the four ways an answer can fail to arrive:
`Gone` (the actor had already ended), `Timeout` (it did not answer in time), `Stopped` (the program is
ending) and `Crashed(reason)` (it died while you waited). The last one is why `ask` does not simply sit
out its timeout when the service dies — §8 shows how it knows.

## 7. More than one inbox

Actor systems usually solve "wait for *that particular* message" by letting you search the mailbox for
one that matches, leaving the rest queued. Skarn does not, on purpose: searching a queue costs more the
longer the queue gets, and a message you skipped still occupies space in a bounded mailbox (§13).

The answer here is a **second mailbox**. `newInbox()` gives an actor another one, with its own address
and its own message type — one per purpose, so a reply never queues behind unrelated work and does not
have to share a type with it. `inbox.close()` ends one; the end of an actor closes all of them.

```rust
use std::actor::*

let _me: Inbox[Int] = mainInbox()
let rx: Inbox[Int]  = newInbox()

println(send(rx.pid(), 1))            // => true
rx.close()
println(send(rx.pid(), 2))            // => false
```

A send to a closed inbox answers `false` rather than failing — the same thing that happens when you send
to an actor that has ended. Messages to the departed are dropped, quietly and by design; the alternative
would be an error at a place that can do nothing about it.

`ask` is exactly this pattern, packaged: make an inbox, use it for one reply, close it. Section 14 shows
what to do when you want to keep two of them and serve whichever speaks first.

## 8. When an actor crashes

In a threaded program an unhandled exception usually kills the thread quietly, or takes the process with
it, and you defend with try/catch at every level. Actor systems take the opposite line, and it is the one
idea from this model most worth carrying away: **let it crash.**

An actor that hits a `panic` ends. The crash does **not** spread to anyone else — their memory is
separate, so there is nothing to corrupt. What happens instead is that the actor that *started* it
receives a `Mail::Exited(id, reason)`, and every later `send` to the dead actor answers `false`. Somebody
who knows what the failure means then decides what to do, instead of the code at the point of failure
guessing.

Anyone other than the starter who needs to know sets a **monitor**. `monitor(p, rx)` promises exactly one
`Exited` report in your inbox `rx` when that actor ends — for a crash, for a normal return (the reason is
`"normal"`), and immediately if it had already ended (`"gone"`, and the call answers `false`).

```rust
use std::actor::*

enum Query { Square(Int, Pid[Int]) }

fn squarer(inbox: Inbox[Query], unused: Int) -> () {
  for q in inbox.messages() {
    match q {
      Query::Square(n, replyTo) => {
        if n < 0 { panic("a negative square is beyond me") }
        send(replyTo, n * n)
      },
    }
  }
}

let s = spawnActor(squarer, 0)
let watch: Inbox[Int] = newInbox()
println(monitor(s, watch))                                        // => true
match ask(s, fn(me) { Query::Square(-1, me) }, 60000) {
  Err(AskError::Crashed(_)) => println("the service crashed"),     // => the service crashed
  Ok(n)                     => println(n),
  Err(e)                    => println(e),
}
match watch.receive() {
  Mail::Exited(_, _) => println("the monitor was told too"),       // => the monitor was told too
  _                  => println("?"),
}
```

Two things happened there. The `ask` came back with `Crashed` **at once**, not after its 60-second
timeout — because `ask` sets a monitor on the actor it asks, so a death is reported into the very inbox it
is waiting on. And the separate monitor was told as well: a monitor is per watcher, so several parties can
each get their own report.

A monitor watches *that actor*, not its address. If a supervisor later starts a replacement at the same
address (§11), your monitor has already fired and will not fire again — you monitor the new one if you
still care.

## 9. Supervisors: keeping them alive

"Let it crash" only works if something starts the actor again. That something is a **supervisor**: an
actor whose entire job is to start a list of children and restart them when their crash reports arrive.
If you have used a process manager — systemd, Kubernetes restarting a pod — it is that idea, one level
down and in-process.

`std::supervisor` is an ordinary Skarn library, not a runtime feature. It needs nothing the last two
sections did not already give you: crash reports arrive as mail, so a supervisor is a loop over `receive()`
that calls `spawnActor` again. You could write one yourself in about thirty lines; this one is written,
tested and handles the cases below.

A child is described by `child(actorFn(f), init)` — the function and its start value as a value you can
hold in a list, because the supervisor has to be able to start it *again* later.

```rust
use std::actor::*
use std::supervisor::*

struct Job { attempt: Pid[Pid[Int]], boss: Pid[String] }

// Hands out 1, 2, 3, ... so each start can tell which attempt it is.
fn counter(inbox: Inbox[Pid[Int]], unused: Int) -> () {
  let mut n = 0
  for replyTo in inbox.messages() { n = n + 1  send(replyTo, n) }
}

// Fails twice, then succeeds.
fn flaky(inbox: Inbox[Int], job: Job) -> () {
  let n = match ask(job.attempt, fn(me) { me }, 5000) { Ok(v) => v, Err(_) => 0 }
  if n < 3 { panic("attempt ${n} failed") }
  send(job.boss, "up at attempt ${n}")
}

fn keeper(inbox: Inbox[()], job: Job) -> () {
  let mut kids: Vec[dyn Supervised] = vec()
  push(kids, child(actorFn(flaky), job))
  supervise(inbox, kids, RestartLimit { maxRestarts: 5, withinMs: 10000 })
}

let me: Inbox[String] = mainInbox()
spawnActor(keeper, Job { attempt: spawnActor(counter, 0), boss: me.pid() })
match me.receive() {
  Mail::Msg(s) => println(s),           // => up at attempt 3
  _            => {},
}
```

The restart limit is the safety valve, and it matters more than it looks. A child that fails because of a
transient problem recovers on the second or third try. A child that fails because of a *bug* fails every
time, and restarting it forever would hide the fault behind a busy loop. So past `maxRestarts` within
`withinMs` the supervisor gives up — by crashing itself.

That is not a cop-out. A supervisor is an actor, so its crash is reported to *its* starter, which may be
another supervisor. Failures therefore travel up a **tree** until they reach a level that can do something
about them, and the top of the tree is your main program. Three rules are worth knowing before you build
one:

- **Crashes only.** A child that returns normally stays ended; it finished its work.
- **New address after a restart**, unless you use a slot — §11.
- **Not in the main program.** `supervise` returns when it is told to stop, and the main program is never
  told, so it would never return. A supervisor is always its own actor.

## 10. When a crash costs the siblings

Restarting only the child that crashed is right when children are independent — four workers pulling from
the same queue, say. It is wrong when they are not. If two children hold two halves of one conversation,
restarting one leaves the other talking to a partner that no longer remembers anything. The fresh child is
fine; the pair is inconsistent.

`superviseWith` lets you say which it is.

| strategy | a crash restarts |
|---|---|
| `OneForOne` | that child only — the default, and right when children are independent |
| `OneForAll` | all of them, stopped first and started again together |
| `RestForOne` | that child and every child started *after* it |

```rust
use std::actor::*
use std::supervisor::*

struct Kid { i: Int, boss: Pid[Int] }

fn pair(inbox: Inbox[Int], k: Kid) -> () {
  send(k.boss, k.i)                     // announce every start, so the output is ordered
  for n in inbox.messages() { if n == 0 { panic("boom") } }
}

fn keeper(inbox: Inbox[()], boss: Pid[Int]) -> () {
  let mut kids: Vec[dyn Supervised] = vec()
  push(kids, child(actorFn(pair), Kid { i: 0, boss: boss }))
  push(kids, child(actorFn(pair), Kid { i: 1, boss: boss }))
  superviseWith(inbox, kids, SupervisorSpec { strategy: Strategy::OneForAll,
    limit: RestartLimit { maxRestarts: 3, withinMs: 60000 }, stopTimeoutMs: 0 })
}

let me: Inbox[Int] = mainInbox()
spawnActor(keeper, me.pid())

let mut up = 0
while up < 2 { match me.receive() { Mail::Msg(_) => { up += 1 }, _ => { up = 2 } } }
println("both up")                      // => both up
```

`RestForOne` is the one that looks arbitrary until you meet its case: children started in dependency
order, each using the ones before it. A crash then invalidates everything downstream of it and nothing
upstream.

One detail decides whether a group restart is correct or merely fast. Stopping a sibling is a **message**,
so the supervisor does not assume it has stopped — it waits for that child's end report before starting
the group again. Otherwise the new children would come up alongside the old ones, which is the bug a group
restart exists to prevent. What to do about a child that never stops is `stopTimeoutMs`, in §12.

## 11. An address that survives a restart

A restarted child is a *new* actor, so it has a new address. Everyone holding the old one is now holding a
dead address, and their sends answer `false`. For a pool of workers that is fine — nobody addresses a
worker by name. For a service that other parts of the program call by name, it is the whole problem.

A **slot** solves it by turning the order around: the address is made *first*, and actors are started
into it. `newSlot()` gives you a `Slot[M]`, `slot.pid()` is the address you hand out once and never
again, and `childIn(slot, a, init)` tells the supervisor to start the child there every time.

```rust
use std::actor::*
use std::supervisor::*

struct Ask { n: Int, replyTo: Pid[Int] }

fn service(inbox: Inbox[Ask], boss: Pid[Int]) -> () {
  send(boss, -1)                        // "I am up", so requests are never sent into a gap
  for a in inbox.messages() {
    if a.n == 0 { panic("zero is beyond me") }
    send(a.replyTo, a.n * a.n)
  }
}

struct Setup { at: Slot[Ask], boss: Pid[Int] }

fn keeper(inbox: Inbox[()], s: Setup) -> () {
  let mut kids: Vec[dyn Supervised] = vec()
  push(kids, childIn(s.at, actorFn(service), s.boss))
  supervise(inbox, kids, RestartLimit { maxRestarts: 3, withinMs: 10000 })
}

let me: Inbox[Int] = mainInbox()
let at: Slot[Ask] = newSlot()
let door = at.pid()                     // the address, handed out once and never again
spawnActor(keeper, Setup { at: at, boss: me.pid() })

match me.receive() { Mail::Msg(_) => {}, _ => {} }        // the first service is up
send(door, Ask { n: 0, replyTo: me.pid() })               // crashes it
match me.receive() { Mail::Msg(_) => {}, _ => {} }        // the next one is up
send(door, Ask { n: 8, replyTo: me.pid() })
match me.receive() {
  Mail::Msg(v) => println("still ${v} at the same address"),   // => still 64 at the same address
  _            => println("?"),
}
```

`door` was taken once, before any actor existed, and it kept working across the crash. The caller never
learned that anything had happened — which is the point.

Two rules about the gap between one actor and the next:

- **A message sent during the gap waits in the slot** and is delivered to the successor. It is not lost
  and the sender is not blocked.
- **What the crashed actor had not read is dropped.** This is deliberate, and it is the difference
  between a service that recovers and one that crash-loops: handing the successor the very message that
  killed its predecessor would kill it too, and again, and again.

`slot.release()` ends the address for good when the service is finished; sends to it answer `false`
afterwards.

## 12. Stopping

Java's `Thread.stop` was deprecated decades ago, and for the reason you would expect: stopping a thread
between two statements leaves whatever it was holding half-finished. Skarn does not offer the operation at
all. **`stopActor(p)` is a message**, queued like any other, and the actor stops when it next *receives* —
at a point of its own choosing, with its own invariants intact.

That leaves one gap, and it is worth seeing rather than discovering: an actor whose loop is its own work —
a long computation, a busy poll — never reaches a receive, so the message never arrives. Such an actor
asks instead. `inbox.stopRequested()` reads the flag without consuming any mail.

```rust
use std::actor::*

fn busy(inbox: Inbox[Int], boss: Pid[Int]) -> () {
  send(boss, 1)                         // "I am running"
  let mut n = 0
  while !inbox.stopRequested() { n = n + 1 }
  send(boss, 2)                         // "and I ended myself"
}

let me: Inbox[Int] = mainInbox()
let b = spawnActor(busy, me.pid())
match me.receive() { Mail::Msg(_) => {}, _ => {} }
println(stopActor(b))                   // => true
match me.receive() {
  Mail::Msg(n) => println("ended: ${n}"),   // => ended: 2
  _            => println("?"),
}
```

There is **no kill**, and that is a deliberate trade: an actor that neither receives nor asks cannot be
ended from outside. Writing one is a bug, but the runtime will not paper over it by tearing the actor down
mid-operation.

A supervisor meets exactly this when it has to stop a group. `stopTimeoutMs` says what it does about a
child that will not end: `0`, the default, waits as long as it takes — that group stands still, but the
supervisor keeps serving, so the program still ends. A deadline instead makes it give up: a restart
crashes, naming the child, and a shutdown abandons it. At its own `Stop` a supervisor stops its children
in **reverse** start order, each waited for before the next is told — the order a child that depends on an
earlier one needs.

## 13. Back-pressure

A mailbox grows for as long as messages arrive faster than the actor reads them. A reader that is 10 %
slower than its writer does not fall 10 % behind — it falls behind without limit, and the process grows
until it dies. This is the failure mode an unbounded `queue.Queue` or `LinkedBlockingQueue` has too, and
it is why those take a capacity.

A **bounded** mailbox is the same answer: `spawnActorBounded(f, init, n)` holds at most `n` messages, and
a `send` to a full one **waits** until the actor has taken one. The fast side is slowed to the pace of the
slow side, which is what "back-pressure" means.

```rust
use std::actor::*

fn adder(inbox: Inbox[Int], unused: Int) -> () {
  let mut sum = 0
  for n in inbox.messages() { sum = sum + n }
  println(sum)                          // => 5050
}

let a = spawnActorBounded(adder, 0, 10)   // at most ten wait for it
let mut i = 1
while i <= 100 { send(a, i)  i = i + 1 }
```

The sender above never sees a bound; it just runs slower. Where you would rather drop the message than
wait, `trySend(p, m)` never waits and answers `Sent`, `Full` or `Gone`. That answer is **must-use** and
the compiler insists you look at it, because `Full` means nothing was queued — ignoring it loses a message
while you believe you sent one. (`send`'s `false` is not must-use: it only says the receiver has ended,
which is the ordinary end of things.)

Waiting brings back the one hazard share-nothing had removed. If A waits for room in B's full mailbox
while B waits for room in A's, both wait for ever. Nothing can break that ring from outside: `stopActor`
is a message, and messages ignore the bound, so a `Stop` reaches a full mailbox without freeing anyone
waiting to put something in it.

So the runtime **detects it** and crashes **every actor in the ring**, each with a fault naming the cycle.
A ring that runs through a `join` is found the same way.

Two things about that are deliberate, and both look severe until you see the alternative:

- **Everyone in the ring dies, not just one of them.** Killing one would be enough to get the others
  moving — their `send` would answer `false` and they would carry on. But `send`'s `false` is the one
  result programs routinely ignore (§13, above), so the survivors would continue with a message silently
  lost, and only one starter would ever hear that anything went wrong. A ring is a property of the
  *group*: reporting it to everyone lets each supervisor apply the strategy you chose for it, rather than
  having the runtime quietly pick a victim.
- **The main program is not exempt.** It has a mailbox like any actor, so it can be part of a ring — and
  then it takes the fault too, and the program ends. That is the right end: the program really is
  deadlocked, and stopping with a message that names the cycle and the line it happened on is better than
  hanging for ever with no output at all.

What stays yours to find is an actor waiting for a message nobody will ever send. That one is not
decidable — a blocked `send` names exactly who must act, but a `receive` could be answered by anyone.

## 14. Two queues in one loop

Section 7 gave each purpose its own inbox, which leaves one thing unsolved: `receive()` waits on **one**
of them. A worker with a job queue and a control queue would have to pick which to block on, and ignore
the other until something arrives on the first.

`select` waits on several at once and answers **which** one has something. If you have used `select()`
or `poll()` on sockets, or a Java `Selector`, it is that idea for mailboxes: it reports readiness and
takes nothing out. The `receive()` you then do on the inbox it named cannot block, because only you take
mail out of your own inbox and it has just told you there is some.

```rust
use std::actor::*

enum Ctl { Report }

fn worker(jobs: Inbox[Int], boss: Pid[String]) -> () {
  let ctl: Inbox[Ctl] = newInbox()
  send(boss, "ready")
  let watching = toVec([ctl.ref(), jobs.ref()])   // control first: it wins a tie
  let mut done = 0
  let mut go = true
  while go {
    match select(watching, 2000) {
      Some(0) => { match ctl.receive() { Mail::Msg(_) => { send(boss, "done ${done}")  go = false },
                                         _            => { go = false } } },
      Some(1) => { match jobs.receive() { Mail::Msg(_) => { done = done + 1 },
                                          _            => { go = false } } },
      _       => { go = false },
    }
  }
}
```

The list holds `InboxRef`s rather than the inboxes themselves, and the reason is the type system: `ctl` is
an `Inbox[Ctl]` and `jobs` an `Inbox[Int]`, two different types, which cannot sit in one `Vec`.
`inbox.ref()` strips the message type and leaves "this mailbox", which is all a readiness question needs.
An `InboxRef` is no more sendable than an `Inbox` — it names the same private queue.

`select` answers an index, not a message, and that is what keeps the types straight: you receive on the
typed inbox you already hold, so the message arrives as `Ctl` or `Int` with no casting anywhere.

**Ties go to the lowest index**, so the order of the list is a priority order — putting `ctl` first is why
a control message is served before waiting work. The same rule is the trap: an inbox that is always ready
starves everything after it.

## 15. Generic actor code

Everything so far used concrete message types. To write a *reusable* piece — a relay, a pool, a cache —
you need a function that works for any message type, and there the checker needs one thing from you.

Sendability is normally read off a type's parts — but inside a generic function `T` has no parts yet. The
bound **`Sendable`** is the promise that whatever `T` turns out to be will be plain data. It is a marker
with no methods: you never implement it, and the compiler checks it at each call from the concrete type.

```rust
use std::actor::*

// One compiled body serves every message type.
fn relay[T: Sendable](inbox: Inbox[T], boss: Pid[T]) -> () {
  for m in inbox.messages() { send(boss, m) }
}

fn start[T: Sendable](boss: Pid[T]) -> Pid[T] { spawnActor(relay, boss) }

let me: Inbox[Int] = mainInbox()
let ints = start(me.pid())
send(ints, 21)
match me.receive() {
  Mail::Msg(n) => println(n),           // => 21
  _            => println("?"),
}
```

There is no cost to this at run time. Skarn erases generics: `relay` compiles to **one** body that serves
every message type, and `start` is not a template — it is a function that happens to be polymorphic. Two
actors at two different message types run the same compiled code.

`actorFn(f)` is the other half. `spawnActor` wants a named function, which is checked where you write it;
`actorFn(f)` does that check once and hands back a **value** — something a pool or a supervisor can hold
in a list, send in a message, and start later. That is why `child(actorFn(worker), init)` in §9 could
treat a function as data.

## 16. Handing over a connection

A server wants one actor to accept connections and others to serve them — so a connection has to move
between actors. But a connection is a number the operating system handed to one thread, not plain data, so
it cannot be copied into a message like the values in §5.

It moves in two steps instead. `c.handOff()` **detaches** the connection and returns a `SocketHandOff` —
a ticket, which *is* plain data and can be sent. The receiving actor redeems it once with `h.take()` and
gets a working `TcpConn` back. Anything `recvLine` had already read ahead travels with the ticket, so no
bytes are stranded.

```rust
use std::net::*
use std::actor::*

fn worker(inbox: Inbox[SocketHandOff], unused: Int) -> () {
  for h in inbox.messages() {
    match h.take() {
      Ok(mut c) => {
        match c.recvLine() {
          Ok(Some(line)) => { let _ = c.sendStr("echo " + line + "\n") },
          _              => {},
        }
        let _ = c.close()
      },
      Err(e) => println(e),
    }
  }
}

fn demo() -> Result[(), String] {
  let lst = listen(0)?                              // the system picks a free port
  let mut client = connect("127.0.0.1", lst.localPort()?)?
  let conn = lst.accept()?
  let w = spawnActor(worker, 0)
  send(w, conn.handOff()?)
  client.sendStr("hi\n")?
  println(match client.recvLine()? { Some(s) => s, None => "<eof>" })   // => echo hi
  client.close()?
  lst.close()?
  Ok(())
}
match demo() { Ok(_) => {}, Err(e) => println(e) }
```

The sender's `TcpConn` is dead from the moment it is handed off: every operation on it returns an `Err`.
It still type-checks — this is a run-time rule, not a compile-time one — so treat `handOff()` as the last
thing you do with that connection.

That is how a server spreads over cores: one actor accepts, and hands each connection to whichever worker
is free. `demo/actor_server/` is the worked version, with a pool and two ways of dispatching work to it.

## 17. Logging from several actors

`println` is fine until there are actors. Then the output is interleaved by arrival, it is gone when the
window closes, and there is no way to turn the noisy parts off. `std::log` is the small answer: a line is
a timestamp, a level and your text, and a `Log` knows where to put it and what to leave out.

```rust
use std::log::*
use std::io::*

let path = "app.log"
let _ = deleteFile(path)

let log = Log::toFile(path, Level::Info)
log.debug("not written")                  // below the minimum: dropped, silently and on purpose
log.info("started")
log.warn("disk is filling up")

match readTextFile(path) {
  Ok(text) => {
    for line in lines(text) {
      if len(line) > 0 { println(slice(line, indexOf(line, " ") + 1, len(line))) }
    }
  },
  Err(e) => println(e),
}
// => INFO  started
// => WARN  disk is filling up
```

The example cuts the timestamp off so the output is the same on every run; a real line looks like
`2025-03-04T09:12:41.007Z INFO  started`.

Two things about the shape. `info`, `warn`, `error` and `debug` are **methods**, not free functions, so
`use std::log::*` does not claim four of the names your program is most likely to want for itself — it
brings in the type `Log` and nothing else. And a `Log` is plain data, which by §5 makes it **sendable**: an
actor is handed its logger in its start value, like any other address.

### Two sinks

`Log::toFile(path, min)` appends straight to the file. Each line is one append, and an append is atomic at
the operating-system level, so several actors writing to one file neither lose a line nor tear one in half.
For most programs that is the whole story.

`Log::toActor(to, min)` sends the line to one actor that owns the file instead. It costs a message and buys
two things the file sink cannot give: the lines of the whole program are in **one** order, and — because
`startLogger` gives its actor a bounded inbox — a program that logs faster than the disk can write is
**slowed down** rather than grown, which is §13 applied to logging.

```rust
use std::log::*
use std::io::*
use std::actor::*

fn worker(inbox: Inbox[Int], log: Log) -> () {
  for n in inbox.messages() { log.info("job ${n}") }
}

// Stop `p` and wait for its end, so nothing below races with what it is still doing.
fn endOf[M](p: Pid[M]) -> () {
  let done: Inbox[Int] = newInbox()
  let _ = monitor(p, done)
  let _ = stopActor(p)
  let _ = done.receiveTimeout(10000)
  done.close()
}

let path = "jobs.log"
let _ = deleteFile(path)

let sink = startLogger(path, 2)                  // at most 2 lines waiting: senders wait for room
let w = spawnActor(worker, Log::toActor(sink, Level::Info))

let mut i = 0
while i < 8 { send(w, i)  i = i + 1 }

endOf(w)                                         // first the worker ...
endOf(sink)                                      // ... then the logger, once nothing writes any more

match readTextFile(path) {
  Ok(text) => {
    let mut n = 0
    for line in lines(text) { if len(line) > 0 { n = n + 1 } }
    println("lines: ${n}")                       // => lines: 8
  },
  Err(e) => println(e),
}
println("after it ended: ${send(sink, "too late\n")}")   // => after it ended: false
```

**Shut the logger down last.** A `Stop` goes in at the *end* of a mailbox (§12), so a logger told to stop
still writes everything already sent to it — all eight lines are there. What it cannot write is a line sent
*after* it ended, and `send` returns `false` to say so.

### What it does not do

There is no rotation, no truncation and no configuration file; nothing writes to stderr; and a line is
written in one call, so a message with newlines in it arrives as several lines and only the first carries a
timestamp. If you want any of that, `Log` is forty lines of ordinary Skarn — read `std/log.skn` and write
the one you want.

## 18. What Skarn does not have

This is the Erlang/OTP model, and the names match where the ideas do. Four differences are deliberate:

- **Mailboxes are typed.** An `Inbox[M]` carries one message type, checked ahead of time.
- **An actor is an OS thread.** Hundreds are fine, hundreds of thousands are not — Erlang's processes are
  scheduled by its runtime, Skarn's are scheduled by the operating system.
- **No selective receive.** Erlang scans one mailbox for a matching message; Skarn gives each purpose its
  own inbox and `select` over them.
- **No links and no kill.** A crash is reported, never propagated, and an actor is asked to stop rather
  than forced. Supervision is a library on top of those reports.

## 19. Where to look next

- `demo/actors/stable_address.skn` — a service behind a slot, restarted under one address.
- `demo/actors/supervised.skn` — a pool of crashing workers that pull their jobs, all three strategies.
- `demo/actors/select.skn` — one worker, two queues, a priority order.
- `demo/actors/wordcount.skn` — a reader, N counters and a collector, checked against a sequential count.
- `demo/actor_server/` — an HTTP server on actors, with a load generator.

The [Skarn Guide](SkarnGuide.md)'s concurrency section covers the two simpler tools beside actors:
`std::poll`, for many connections on one thread, and `std::task`, for one computation on several cores.
