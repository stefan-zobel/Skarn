"""The request handling and the worker loops of server.py.

This is its own module because a subinterpreter can only run a function that it can import by name.
Everything here mirrors ../server.skn: the same routes, the same work, the same answer.
"""

import socket
import sys


def work(n):
    """Sum of the decimal digits of 1..n -- the same loop as work() in server.skn."""
    total = 0
    i = 1
    while i <= n:
        k = i
        while k > 0:
            total = total + k % 10
            k = k // 10
        i = i + 1
    return total


def route(path):
    """The body for a request path."""
    if path.startswith("/work/"):
        try:
            n = int(path[6:])
        except ValueError:
            return "bad number\n"
        return f"work {n} = {work(n)}\n"
    return "hello from python\n"


def serve(conn):
    """Read one request, answer it, close. HTTP/1.0 style: one request per connection."""
    try:
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = conn.recv(4096)
            if not chunk:
                break
            buf += chunk
        if buf:
            parts = buf.split(b"\r\n", 1)[0].decode("latin-1").split()
            path = parts[1] if len(parts) >= 2 else "/"
            body = route(path).encode()
            conn.sendall(b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                         + f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n".encode()
                         + body)
    except OSError as e:
        print(f"worker: {e}", file=sys.stderr)
    finally:
        conn.close()


# --- the worker loops, one per model. Each reports itself free with its number, once at the start and
# --- again after every connection; the acceptor hands a connection only to a worker that did.

def thread_worker(wid, inbox, free):
    """threads: the connection arrives as a socket object."""
    free.put(wid)
    while True:
        serve(inbox.get())
        free.put(wid)


def process_worker(wid, inbox, free):
    """procs: the connection arrives as the bytes of socket.share() on Windows, as a socket elsewhere."""
    free.put(wid)
    while True:
        item = inbox.get()
        serve(socket.fromshare(item) if sys.platform == "win32" else item)
        free.put(wid)


def prefork_worker(wid, listener):
    """prefork: no acceptor. Every process accepts on the shared listening socket itself (the bytes of
    socket.share() on Windows, the socket elsewhere)."""
    srv = socket.fromshare(listener) if sys.platform == "win32" else listener
    while True:
        conn, _ = srv.accept()
        serve(conn)


def subinterp_worker(wid, channel_fd):
    """subinterp: the connection arrives as a socket number (a socket handle is valid process-wide),
    over this worker's end of a local socket pair; a free report is one byte back on the same pair.

    Not concurrent.interpreters.Queue: its get() polls, sleeping 10 ms whenever the queue is empty, so a
    worker would oversleep up to 10 ms after every connection. The socket pair blocks in the OS."""
    channel = socket.socket(fileno=channel_fd)
    channel.sendall(b"f")
    while True:
        msg = b""
        while len(msg) < 8:
            part = channel.recv(8 - len(msg))
            if not part:
                return
            msg += part
        serve(socket.socket(fileno=int.from_bytes(msg, "little")))
        channel.sendall(b"f")
