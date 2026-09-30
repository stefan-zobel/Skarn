# check_live_output.py -- checks that a running program's output reaches a pipe while it runs.
#
# A server started with its standard output on a pipe, and stopped by being killed, must not lose what it
# printed: its reader (a supervisor, a log collector, a test) sees each line while the program is still
# running. Each case below starts the driver with standard output and standard error on pipes, waits
# until the expected text has arrived on the one it watches (standard output, except for `stderr`),
# checks that the program is still running (it waits by design, so nothing but the driver's own
# line-by-line output can have pushed the text out), and then kills it.
#
#   line     a println, then a long sleep
#   actor    a println in an actor, while the main program waits for mail that never comes
#   flush    a print without a newline, pushed out by std::io's flushOutput()
#   burst    thousands of lines in a row, then a last one: gathered, but on the pipe shortly after
#   stderr   an eprintln (standard error), then a long sleep
#
# Usage (repo root or anywhere; Python 3.9+):
#   python tests/check_live_output.py
#   python tests/check_live_output.py --config Debug
#   python tests/check_live_output.py --exe build/skarnvm
#
# Exit code: 0 = every case saw its output in time; 1 = at least one did not (or the driver is missing).

import argparse
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from skarn_testlib import add_driver_options, find_driver, green, red, say  # noqa: E402

CASES = [
    ("line",
     "use std::time::*\n"
     "\n"
     "\n"
     "println(\"ready\")\n"
     "sleep(60000)\n",
     "ready\n"),
    ("actor",
     "use std::actor::*\n"
     "\n"
     "\n"
     "fn body(inbox: Inbox[Int], unused: Int) -> () {\n"
     "    println(\"from an actor\")\n"
     "    for _m in inbox.messages() {\n"
     "    }\n"
     "}\n"
     "\n"
     "let me: Inbox[Int] = mainInbox()\n"
     "let _p = spawnActor(body, 0)\n"
     "let _m = me.receive()\n",
     "from an actor\n"),
    ("flush",
     "use std::io::*\n"
     "use std::time::*\n"
     "\n"
     "\n"
     "print(\"prompt> \")\n"
     "flushOutput()\n"
     "sleep(60000)\n",
     "prompt> "),
    ("burst",
     "use std::time::*\n"
     "\n"
     "\n"
     "let mut i = 0\n"
     "while i < 20000 {\n"
     "    println(\"line ${i}\")\n"
     "    i += 1\n"
     "}\n"
     "println(\"last\")\n"
     "sleep(60000)\n",
     "last\n"),
    ("stderr",
     "use std::time::*\n"
     "\n"
     "\n"
     "eprintln(\"trouble\")\n"
     "sleep(60000)\n",
     "trouble\n"),
]


def run_case(exe, src_dir, name, source, expected, wait_s):
    on_err = name == "stderr"
    path = src_dir / (name + ".skn")
    path.write_text(source, encoding="utf-8")
    p = subprocess.Popen([str(exe), str(path)], stdin=subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    got = bytearray()
    lock = threading.Lock()

    def reader():
        while True:
            chunk = stream.read1(65536) if hasattr(stream, "read1") else stream.read(1)
            if not chunk:
                return
            with lock:
                got.extend(chunk)

    stream = p.stderr if on_err else p.stdout
    t = threading.Thread(target=reader, daemon=True)
    t.start()
    want = expected.encode("utf-8")
    deadline = time.monotonic() + wait_s
    seen = False
    while time.monotonic() < deadline:
        with lock:
            text = bytes(got).replace(b"\r\n", b"\n")
        if text.endswith(want):
            seen = True
            break
        if p.poll() is not None:
            break
        time.sleep(0.02)
    running = p.poll() is None
    if running:
        p.kill()   # this process only, by its handle
    t.join(timeout=5)   # the watched pipe ends with the process; only then read the other one
    other = p.stdout if on_err else p.stderr
    try:
        err = other.read()
        p.wait(timeout=10)
    except (subprocess.TimeoutExpired, OSError, ValueError):
        err = b""
    if on_err:
        with lock:
            err = bytes(got)
    if seen and running:
        return None
    if not running:
        return "the program ended (exit {}) instead of waiting: {}".format(
            p.returncode, err.decode("utf-8", errors="replace").strip())
    with lock:
        tail = bytes(got[-60:]).decode("utf-8", errors="replace")
    return "'{}' did not arrive within {} s while the program ran (last bytes seen: {!r})".format(
        expected.strip(), wait_s, tail)


def main():
    ap = argparse.ArgumentParser(description="Check that a running program's output reaches a pipe.")
    add_driver_options(ap)
    ap.add_argument("--wait", type=float, default=5.0, help="seconds to wait for each case's output")
    opts = ap.parse_args()
    exe = find_driver(opts.exe, opts.config)

    failed = 0
    with tempfile.TemporaryDirectory(prefix="skarn_live_") as tmp:
        for name, source, expected in CASES:
            why = run_case(exe, Path(tmp), name, source, expected, opts.wait)
            if why is None:
                say(green("PASS") + "  " + name)
            else:
                failed += 1
                say(red("FAIL") + "  " + name + ": " + why)
    say()
    summary = "==== live output: {} passed, {} failed ====".format(len(CASES) - failed, failed)
    say(red(summary) if failed else green(summary))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
