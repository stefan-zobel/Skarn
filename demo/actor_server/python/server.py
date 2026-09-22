"""The actor HTTP server of ../server.skn, in Python, four ways.

One acceptor, N workers, and every connection handed from the first to one of the others. The workers
ask for work, as `server.skn --dispatch=pull` does: each reports itself free, and the acceptor hands a
connection only to a worker that did. Routes, work and answers are those of server.skn.

  --model=threads    worker threads, one interpreter and its GIL
  --model=subinterp  one subinterpreter per worker, each on its own thread with its own GIL -- the
                     counterpart of Skarn's isolates; the connection travels as a socket number
  --model=procs      one process per worker; the connection travels via socket.share()
  --model=prefork    one process per worker, each accepting on the shared listening socket itself (the
                     gunicorn way); no acceptor, no hand-off

Run:   python server.py [--model=M] [--workers=N] [--port=P] [--port-file=F]
  --workers=N    workers (default 4). 0 answers on the main thread itself (threads only): the baseline.
  --port=P       port to listen on (default 0: the OS picks a free one)
  --port-file=F  write the port to file F once listening (for a harness), as server.skn does

Needs Python 3.14 (concurrent.interpreters). Serves until killed.
"""

import argparse
import multiprocessing as mp
import os
import queue
import select
import socket
import sys
import threading

import workers

HERE = os.path.dirname(os.path.abspath(__file__))


def listen(port):
    """A dual-stack listener, like tcpListen in Skarn."""
    return socket.create_server(("", port), family=socket.AF_INET6, dualstack_ipv6=True,
                                backlog=socket.SOMAXCONN)


def write_port(path, port):
    if path:
        with open(path, "w") as f:
            f.write(str(port))


def run(model, n, port_file, port):
    srv = listen(port)
    port = srv.getsockname()[1]

    if n == 0:
        if model != "threads":
            sys.exit("--workers=0 is the baseline of --model=threads")
        write_port(port_file, port)
        print(f"listening on port {port} without workers", flush=True)
        while True:
            conn, _ = srv.accept()
            workers.serve(conn)

    if model == "threads":
        free = queue.Queue()
        inboxes = [queue.Queue() for _ in range(n)]
        for wid in range(n):
            threading.Thread(target=workers.thread_worker, args=(wid, inboxes[wid], free),
                             daemon=True).start()

        def hand(wid, conn):
            inboxes[wid].put(conn)

    elif model == "subinterp":
        # One local socket pair per worker carries both directions: connections out, free reports back.
        # (Why not concurrent.interpreters.Queue: see subinterp_worker.)
        import concurrent.interpreters as ci
        channels = []
        for wid in range(n):
            mine, theirs = socket.socketpair()
            channels.append(mine)
            interp = ci.create()
            interp.exec(f"import sys; sys.path.insert(0, {HERE!r})")
            interp.call_in_thread(workers.subinterp_worker, wid, theirs.detach())
        owner = {ch.fileno(): wid for wid, ch in enumerate(channels)}

        class Reports:
            """Free reports from all channels; waits on them together when none is pending."""
            def get(self):
                while not pending:
                    ready, _, _ = select.select(channels, [], [])
                    for ch in ready:
                        for _ in ch.recv(64):
                            pending.append(owner[ch.fileno()])
                return pending.pop(0)

        pending = []
        free = Reports()

        def hand(wid, conn):
            channels[wid].sendall(conn.detach().to_bytes(8, "little"))

    elif model == "procs":
        ctx = mp.get_context("spawn")
        free = ctx.SimpleQueue()
        inboxes = [ctx.SimpleQueue() for _ in range(n)]
        procs = [ctx.Process(target=workers.process_worker, args=(wid, inboxes[wid], free), daemon=True)
                 for wid in range(n)]
        for p in procs:
            p.start()

        def hand(wid, conn):
            if sys.platform == "win32":
                inboxes[wid].put(conn.share(procs[wid].pid))
            else:
                inboxes[wid].put(conn)
            conn.close()

    elif model == "prefork":
        ctx = mp.get_context("spawn")
        if sys.platform == "win32":
            # socket.share() needs the target's pid, so the listener follows the start.
            inboxes = [ctx.SimpleQueue() for _ in range(n)]
            procs = [ctx.Process(target=_prefork_entry, args=(wid, inboxes[wid]), daemon=True)
                     for wid in range(n)]
            for p in procs:
                p.start()
            for wid, p in enumerate(procs):
                inboxes[wid].put(srv.share(p.pid))
        else:
            procs = [ctx.Process(target=workers.prefork_worker, args=(wid, srv), daemon=True)
                     for wid in range(n)]
            for p in procs:
                p.start()
        # Like the other models: the port is announced at once; connections wait in the accept queue
        # until the first process accepts.
        write_port(port_file, port)
        print(f"listening on port {port} with {n} workers (prefork)", flush=True)
        for p in procs:
            p.join()
        return

    else:
        sys.exit(f"unknown --model={model}")

    # The acceptor: accept, wait for a free worker if none is known, hand the connection over. Reports
    # are not collected ahead, as in server.skn: while no worker is free, new connections wait in the
    # system's accept queue. A worker still starting up has simply not reported yet.
    write_port(port_file, port)
    print(f"listening on port {port} with {n} workers ({model})", flush=True)
    while True:
        conn, _ = srv.accept()
        hand(free.get(), conn)


def _prefork_entry(wid, inbox):
    """prefork on Windows: receive the shared listener first, then run the worker."""
    workers.prefork_worker(wid, inbox.get())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--model", default="threads", choices=["threads", "subinterp", "procs", "prefork"])
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--port-file", default=None)
    a = ap.parse_args()
    run(a.model, a.workers, a.port_file, a.port)


if __name__ == "__main__":
    main()
