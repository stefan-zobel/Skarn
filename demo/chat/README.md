# Chat

A chat server with topics and a terminal client for it. It uses **active sockets**: an actor that waits
on its connection and its inbox at once (see "A connection that also listens to its inbox" in
[SkarnActors.md](../../SkarnActors.md)).

| file | what it is |
|---|---|
| `server.skn` | the server; `--selftest` runs a scripted conversation against it instead |
| `hub.skn` | the server's actors: acceptor, sessions, registry, topics |
| `chat_client.skn` | a terminal client |
| `selftest.skn` | the script `--selftest` runs |

## Running it

Start the server and a client in each of two or more terminals, from the repository root:

```
x64\Release\static_vmrun.exe demo\chat\server.skn [--port=P] [--port-file=F]
x64\Release\static_vmrun.exe demo\chat\chat_client.skn [--host=H] [--port=P]
```

The port is 7070 unless you name another one; `--port=0` lets the system pick a free one, which the server
prints and, with `--port-file=F`, writes to the file `F`. The server runs until you end it (Ctrl+C).

A session:

```
* welcome -- /nick <name>, /join <topic>, /leave <topic>, /topics, /quit
/nick ann
* you are now ann
/join rust
* ann joined #rust
* ben joined #rust
hi ben
[#rust] <ann> hi ben
[#rust] <ben> hi ann
/quit
* bye
* the server closed the connection
```

| command | effect |
|---|---|
| `/nick <name>` | choose a name (one word) |
| `/join <topic>` | join a topic, or switch to it if you are in it already; it becomes the current one |
| `/leave <topic>` | leave a topic |
| `/topics` | list all topics |
| `/quit` | end the connection |
| anything else | goes to the current topic |

A topic is written with or without `#`. Lines from the server start with `* `, and chat lines look like
`[#topic] <name> text`.

## The self-test

```
x64\Release\static_vmrun.exe demo\chat\server.skn --selftest
```

starts the server on a free port and three clients in the same process: alice and bob as actors with
active connections, and carol as a plain blocking connection. They follow a fixed script: names, joining
and switching topics, chat lines, `/topics`, `/leave`, an unknown command, the slow client below, and a
`/quit`. The output is a transcript, `who > line` for what a client sends and `who < line` for what it
receives, followed by `selftest: 24 steps ok` and exit code 0. A line that is missing, extra or different
ends the test with `selftest FAILED: ...` and exit code 1.

The clients run concurrently, yet the transcript is the same on every run. After every step the script
waits until each line that step must cause has arrived at its client, and only then prints them, in the
order it lists them.

## How it is built

```
acceptor --new client--> session (one per connection)
session  --ask--> registry --starts on first use--> topic (one per topic)
session  --join / leave / post--> topic --one line each--> every member's session
```

- **The acceptor** activates its listener (`l.activate(...)`), so new clients arrive as tickets in an
  inbox and it can `select` over them and its own mail. When the program ends, it hears
  `ListenerStopping` and stops.
- **A session** owns one connection. It activates it (`c.activate(Framing::Lines(1024), 16)`), so every
  line the client types arrives as a `SockEvent::Line` in an inbox of the session. One `select` waits on
  that and on the session's own inbox, where the topics put the lines of the others. This is the actor
  that could not be written with a blocking `recvLine`, which waits for the client alone.
- **The registry** knows each topic by name and starts a topic actor the first time one is asked for.
- **A topic** holds its members and hands every line to each of them. It also monitors them, so a
  session that ends without leaving (it crashed) is dropped as well.

The client splits its two jobs the same way. The main program reads the keyboard, because only the main
program may read stdin and `readLine` blocks, and hands every line to an actor that owns the activated
connection and prints what the server sends. So a line from someone else appears while you type.

## A client that stops reading

A session's inbox is bounded: it holds at most 32 lines (the acceptor starts it with
`spawnActorBounded`). A topic never waits for a member. It hands each line on with `trySend`, and when a
member's inbox is full it drops that member, tells its session to stop, and tells the others:

```
* carol was too slow and was disconnected
```

A session's inbox fills only when the session itself cannot keep up. Normally that means its client has
stopped reading: the session's writes then block once the system's socket buffers are full. So one
stalled client cannot hold up a topic, and nothing is lost silently.

The self-test checks this. carol joins `#rust` and never reads again, and alice writes blocks of lines to
`#rust` until the topic drops carol, while alice and bob keep receiving. A block is 24 lines, fewer than an
inbox holds, and the next one starts only after alice and bob have both received the last line of the
previous one. A single burst larger than the inbox could drop a client that was just a moment slower. That
would be the rule doing its job, but it would make the test depend on timing.

**The session is freed too.** When the topic drops carol, her session is waiting in a `send` to her,
because her socket buffers are full. The topic's `stopActor` ends that wait: a send on an active
connection gives up with an `Err` as soon as its actor is told to stop. The session then leaves its
topics and closes the connection. The self-test shows this: afterwards carol reads again, gets what was
still on its way, and then the end of the stream. For a client that stops reading while no topic drops
it, each session also sets a deadline, `out.setSendTimeout(5000)`: a send that makes no progress for
5 seconds fails, and the connection is closed.

The client has a limit of its own: `readLine` cannot be interrupted. When the server ends
the connection, the client says so at once but exits only after the next line you enter (or at the end of
the input: Ctrl+Z on Windows, Ctrl+D elsewhere).
