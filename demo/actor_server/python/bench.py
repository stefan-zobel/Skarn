"""Measures the Skarn actor server against its Python counterparts, the same way for every contender.

Every server is driven by the same client, ../loadgen.skn, which checks every answer; the /work answer
is checked for its exact value. The contenders are:

  skarn      ../server.skn --dispatch=pull
  threads    server.py --model=threads
  subinterp  server.py --model=subinterp
  procs      server.py --model=procs
  prefork    server.py --model=prefork
  ft-threads server.py --model=threads under a free-threaded Python (no GIL), with --python-ft

Four measurements, each in runs of its own so that one does not disturb the other:
  load      requests per second and latency percentiles, per route and worker count; the contenders
            take turns in a different order every round, and each run starts with a short warm-up
  startup   from starting the process to the first correct answer; for Skarn that includes compiling
            the program from source
  memory    private memory of the whole process tree, idle and at its peak under load
  work      the time of one work(8000) on a single thread, to separate interpreter speed from scaling

Run:   python bench.py --vm=<skarnvm> [--python=<python>] [--python-ft=<python3.14t>] [--rounds=5]
                       [--contenders=a,b,...] [--out=DIR] [--smoke]
  --smoke   a quick functional check: one round, few requests, load only

Needs psutil. Measure on a quiet machine: the script refuses to start when the CPU is busy.
"""

import argparse
import csv
import os
import re
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time

import psutil

HERE = os.path.dirname(os.path.abspath(__file__))
DEMO = os.path.dirname(HERE)
SERVER_SKN = os.path.join(DEMO, "server.skn")
LOADGEN_SKN = os.path.join(DEMO, "loadgen.skn")
SERVER_PY = os.path.join(HERE, "server.py")

CONTENDERS = ["skarn", "threads", "subinterp", "procs", "prefork"]
WORK_N = 8000

LOADGEN_LINE = re.compile(r"requests (\d+)\s+failed (\d+)\s+wall (\d+) ms\s+rps (\d+)\s+"
                          r"p50 (\d+) us\s+p95 (\d+) us\s+p99 (\d+) us")


def work(n):
    """The reference value for the /work check; the same loop as in server.skn and workers.py."""
    total = 0
    for i in range(1, n + 1):
        k = i
        while k > 0:
            total += k % 10
            k //= 10
    return total


class Bench:
    def __init__(self, a):
        self.vm = os.path.abspath(a.vm)
        self.py = os.path.abspath(a.python)
        self.py_ft = os.path.abspath(a.python_ft) if a.python_ft else None
        self.tmp = tempfile.mkdtemp(prefix="actor_bench_")

    # --- servers ---

    def server_cmd(self, who, n, port_file=None, port=0, bytecode=None):
        if who == "skarn":
            dispatch = "pull" if n > 0 else "rr"
            if bytecode:
                # The .skn after the image only fills the script slot, so that the options after it
                # reach the program; the program runs from the image.
                cmd = [self.vm, "--run-bytecode", bytecode, SERVER_SKN]
            else:
                cmd = [self.vm, SERVER_SKN]
            cmd += [f"--workers={n}", f"--dispatch={dispatch}", f"--port={port}"]
        elif who == "ft-threads":
            # The same threads server, on the free-threaded interpreter.
            cmd = [self.py_ft, SERVER_PY, "--model=threads", f"--workers={n}", f"--port={port}"]
        else:
            cmd = [self.py, SERVER_PY, f"--model={who}", f"--workers={n}", f"--port={port}"]
        if port_file:
            cmd.append(f"--port-file={port_file}")
        return cmd

    def start(self, who, n):
        port_file = os.path.join(self.tmp, "port.txt")
        if os.path.exists(port_file):
            os.remove(port_file)
        log = os.path.join(self.tmp, "server.log")
        with open(log, "w") as err:
            proc = subprocess.Popen(self.server_cmd(who, n, port_file), stdout=subprocess.DEVNULL,
                                    stderr=err)
        deadline = time.perf_counter() + 30
        while time.perf_counter() < deadline:
            if proc.poll() is not None:
                with open(log, errors="replace") as f:
                    raise RuntimeError(f"{who} n={n} exited: {f.read()}")
            try:
                with open(port_file) as f:
                    text = f.read().strip()
                if text:
                    return proc, int(text)
            except OSError:
                pass
            time.sleep(0.01)
        stop(proc)
        raise RuntimeError(f"{who} n={n}: no port file after 30 s")

    # --- the client ---

    def loadgen(self, port, path, clients, requests):
        expect = f"work {WORK_N} = {work(WORK_N)}" if path.startswith("/work/") else "hello"
        cmd = [self.vm, LOADGEN_SKN, f"--port={port}", f"--clients={clients}",
               f"--requests={requests}", f"--path={path}", f"--expect={expect}"]
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=600).stdout
        m = LOADGEN_LINE.search(out)
        if not m:
            raise RuntimeError(f"loadgen said: {out!r}")
        r = dict(zip(["requests", "failed", "wall_ms", "rps", "p50", "p95", "p99"], map(int, m.groups())))
        if r["failed"]:
            raise RuntimeError(f"{r['failed']} failed requests: {out.strip()}")
        return r


def stop(proc):
    """Kill a server with all its child processes, and wait until they are gone."""
    try:
        parent = psutil.Process(proc.pid)
        tree = parent.children(recursive=True) + [parent]
    except psutil.NoSuchProcess:
        return
    for p in tree:
        try:
            p.kill()
        except psutil.NoSuchProcess:
            pass
    _, alive = psutil.wait_procs(tree, timeout=10)
    if alive:
        raise RuntimeError(f"processes survived the kill: {[p.pid for p in alive]}")


def rotate(xs, k):
    k %= len(xs)
    return xs[k:] + xs[:k]


# --- load ---

def measure_load(b, rounds, workers, routes, warmup, raw):
    for r in range(rounds):
        for path, clients, requests in routes:
            for n in workers:
                arms = [c for c in CONTENDERS if n > 0 or c in ("skarn", "threads", "ft-threads")]
                for who in rotate(arms, r + n):
                    proc, port = b.start(who, n)
                    try:
                        if warmup:
                            b.loadgen(port, path, 4, 10)
                        res = b.loadgen(port, path, clients, requests)
                    finally:
                        stop(proc)
                    row = {"round": r, "path": path, "workers": n, "who": who, **res}
                    raw.append(row)
                    print(f"  round {r} {path:12} n={n:2} {who:10} rps {res['rps']:6}  "
                          f"p50 {res['p50']:6} us  p95 {res['p95']:6} us  p99 {res['p99']:6} us", flush=True)


# --- startup ---

def first_answer(port):
    """One request to /, answered correctly or not."""
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=10) as c:
            c.sendall(b"GET / HTTP/1.0\r\nHost: localhost\r\n\r\n")
            data = b""
            while True:
                part = c.recv(4096)
                if not part:
                    break
                data += part
        return data.startswith(b"HTTP/1.0 200") and b"hello" in data
    except OSError:
        return False


def measure_startup(b, reps, workers):
    """From starting the process to the first correct answer. The first request goes out once the port
    file exists, which every server writes right after listen: probing a port that is not listening
    yet would measure the operating system instead (on Windows a refused connect takes ~0.5 s)."""
    bytecode = os.path.join(b.tmp, "server.skbc")
    subprocess.run([b.vm, "--emit-bytecode", bytecode, SERVER_SKN], check=True, capture_output=True,
                   timeout=60)
    arms = [(c, None) for c in CONTENDERS]
    if "skarn" in CONTENDERS:
        arms.insert(1, ("skarn (.skbc)", bytecode))
    port_file = os.path.join(b.tmp, "startup_port.txt")
    rows = []
    for n in workers:
        for rep in range(reps):
            for label, bc in rotate(arms, rep):
                who = "skarn" if label.startswith("skarn") else label
                if os.path.exists(port_file):
                    os.remove(port_file)
                t0 = time.perf_counter()
                proc = subprocess.Popen(b.server_cmd(who, n, port_file=port_file, bytecode=bc),
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                ok = False
                while not ok and time.perf_counter() - t0 < 30:
                    try:
                        with open(port_file) as f:
                            text = f.read().strip()
                        ok = bool(text) and first_answer(int(text))
                    except OSError:
                        pass
                    if not ok:
                        time.sleep(0.001)
                ms = (time.perf_counter() - t0) * 1000
                stop(proc)
                if not ok:
                    raise RuntimeError(f"startup {label} n={n}: no answer within 30 s")
                rows.append({"who": label, "workers": n, "rep": rep, "ms": ms})
        for label, _ in arms:
            v = statistics.median(r["ms"] for r in rows if r["who"] == label and r["workers"] == n)
            print(f"  startup n={n:2} {label:14} {v:8.1f} ms", flush=True)
    return rows


# --- memory ---

def tree_uss(pid):
    """Private bytes of a process and all its children, in MB, and the plain RSS sum next to it."""
    uss = rss = 0
    try:
        root = psutil.Process(pid)
        for p in [root] + root.children(recursive=True):
            try:
                info = p.memory_full_info()
                uss += info.uss
                rss += info.rss
            except psutil.NoSuchProcess:
                pass
    except psutil.NoSuchProcess:
        pass
    return uss / 2**20, rss / 2**20


def measure_memory(b, reps, n, path, clients, requests):
    rows = []
    for rep in range(reps):
        for who in rotate(CONTENDERS, rep):
            proc, port = b.start(who, n)
            try:
                b.loadgen(port, "/", 4, 10)       # every worker has started and served
                time.sleep(0.5)
                idle_uss, idle_rss = tree_uss(proc.pid)
                peak = [0.0, 0.0]
                done = threading.Event()

                def sample():
                    while not done.is_set():
                        u, r = tree_uss(proc.pid)
                        peak[0] = max(peak[0], u)
                        peak[1] = max(peak[1], r)
                        done.wait(0.1)

                t = threading.Thread(target=sample)
                t.start()
                try:
                    b.loadgen(port, path, clients, requests)
                finally:
                    done.set()
                    t.join()
            finally:
                stop(proc)
            rows.append({"who": who, "workers": n, "rep": rep, "idle_uss": idle_uss, "idle_rss": idle_rss,
                         "peak_uss": peak[0], "peak_rss": peak[1]})
    for who in CONTENDERS:
        mine = [r for r in rows if r["who"] == who]
        print(f"  memory n={n} {who:10} idle {statistics.median(r['idle_uss'] for r in mine):7.1f} MB  "
              f"peak {statistics.median(r['peak_uss'] for r in mine):7.1f} MB private  "
              f"(RSS {statistics.median(r['peak_rss'] for r in mine):7.1f} MB)", flush=True)
    return rows


# --- single-thread work ---

WORK_SKN = """use std::env::*


fn work(n: Int) -> Int {
    let mut total = 0
    let mut i = 1
    while i <= n {
        let mut k = i
        while k > 0 {
            total = total + k % 10
            k = k / 10
        }
        i = i + 1
    }
    total
}

let reps = 200
let mut s = 0
let mut r = 0
let t0 = nanoTime()
while r < reps {
    s = s + work(%N%)
    r = r + 1
}
println("${(nanoTime() - t0) / reps / 1000} ${s}")
"""

WORK_PY = """import sys, time
sys.path.insert(0, %HERE%)
from workers import work
reps = 200
s = 0
t0 = time.perf_counter_ns()
for _ in range(reps):
    s = s + work(%N%)
print((time.perf_counter_ns() - t0) // reps // 1000, s)
"""


def measure_work(b, reps):
    skn = os.path.join(b.tmp, "work.skn")
    with open(skn, "w") as f:
        f.write(WORK_SKN.replace("%N%", str(WORK_N)))
    pysrc = WORK_PY.replace("%N%", str(WORK_N)).replace("%HERE%", repr(HERE))
    interpreters = {"skarn": [b.vm, skn], "python": [b.py, "-c", pysrc]}
    if b.py_ft:
        interpreters["python-ft"] = [b.py_ft, "-c", pysrc]
    times = {who: [] for who in interpreters}
    for rep in range(reps):
        for who in rotate(list(interpreters), rep):
            cmd = interpreters[who]
            res = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            if res.returncode != 0:
                raise RuntimeError(f"{who} work run failed: {res.stdout}{res.stderr}")
            us, total = res.stdout.split()
            if int(total) != 200 * work(WORK_N):
                raise RuntimeError(f"{who}: wrong work sum {total}")
            times[who].append(int(us))
    for who, v in times.items():
        print(f"  work({WORK_N}) {who:7} {statistics.median(v):6} us", flush=True)
    return [{"who": w, "rep": i, "us": us} for w, v in times.items() for i, us in enumerate(v)]


# --- report ---

def summarize_load(raw):
    cells = {}
    for r in raw:
        cells.setdefault((r["path"], r["workers"], r["who"]), []).append(r)
    med = {k: {m: statistics.median(x[m] for x in v) for m in ("rps", "p50", "p95", "p99")}
           for k, v in cells.items()}
    lines = []
    for path in sorted({k[0] for k in med}):
        lines.append(f"\n{path}: requests/s (speedup against the own 1-worker row), p50 / p99 in ms\n")
        lines.append("| workers | " + " | ".join(CONTENDERS) + " |")
        lines.append("|---|" + "---|" * len(CONTENDERS))
        for n in sorted({k[1] for k in med if k[0] == path}):
            row = [str(n)]
            for who in CONTENDERS:
                c = med.get((path, n, who))
                base = med.get((path, 1, who))
                if not c:
                    row.append("")
                    continue
                speed = f" ({c['rps'] / base['rps']:.1f}×)" if base and n > 0 else ""
                row.append(f"{c['rps']:.0f}{speed} · {c['p50'] / 1000:.1f} / {c['p99'] / 1000:.1f}")
            lines.append("| " + " | ".join(row) + " |")
    return "\n".join(lines)


def write_csv(path, rows):
    if rows:
        with open(path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--vm", required=True, help="the skarnvm executable (a Release build)")
    ap.add_argument("--python", default=sys.executable, help="the Python that runs server.py")
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--out", default=".")
    ap.add_argument("--smoke", action="store_true")
    ap.add_argument("--force", action="store_true", help="measure even when the CPU is busy")
    ap.add_argument("--contenders", default=None,
                    help="measure only these, comma-separated (e.g. skarn,threads,ft-threads)")
    ap.add_argument("--python-ft", default=None,
                    help="a free-threaded Python (python3.14t); adds the contender ft-threads")
    a = ap.parse_args()
    if a.python_ft:
        CONTENDERS.append("ft-threads")
    if a.contenders:
        chosen = a.contenders.split(",")
        unknown = [c for c in chosen if c not in CONTENDERS]
        if unknown:
            sys.exit(f"unknown contenders {unknown}; available: {CONTENDERS}")
        CONTENDERS[:] = [c for c in CONTENDERS if c in chosen]
    b = Bench(a)
    try:
        run(a, b)
    finally:
        shutil.rmtree(b.tmp, ignore_errors=True)


def run(a, b):

    def version(py):
        return subprocess.run([py, "-c", "import sys; print(sys.version.split()[0], "
                               "'GIL' if sys._is_gil_enabled() else 'no GIL')"],
                              capture_output=True, text=True).stdout.strip()

    print(f"python {version(a.python)}, {psutil.cpu_count(logical=False)} cores / {psutil.cpu_count()} threads")
    if a.python_ft:
        ft = version(a.python_ft)
        print(f"python-ft {ft}")
        if not ft.endswith("no GIL"):
            sys.exit(f"--python-ft runs with the GIL enabled ({ft}): not a free-threaded interpreter")

    if a.smoke:
        raw = []
        measure_load(b, 1, [0, 1, 4, 12], [("/", 8, 20), (f"/work/{WORK_N}", 8, 10)], False, raw)
        print("smoke test passed: every answer checked, none failed")
        return

    busy = psutil.cpu_percent(interval=3)
    if busy > 10 and not a.force:
        sys.exit(f"the CPU is {busy:.0f} % busy -- measure on a quiet machine (or pass --force)")

    os.makedirs(a.out, exist_ok=True)
    print("work:")
    write_csv(os.path.join(a.out, "work.csv"), measure_work(b, 5))
    print("startup:")
    write_csv(os.path.join(a.out, "startup.csv"), measure_startup(b, 10, [1, 12]))
    print("memory:")
    write_csv(os.path.join(a.out, "memory.csv"),
              measure_memory(b, 3, 12, f"/work/{WORK_N}", 16, 200))
    print("load:")
    raw = []
    measure_load(b, a.rounds, [0, 1, 2, 4, 6, 12], [("/", 16, 100), (f"/work/{WORK_N}", 16, 50)],
                 True, raw)
    write_csv(os.path.join(a.out, "load.csv"), raw)
    summary = summarize_load(raw)
    with open(os.path.join(a.out, "summary.md"), "w", encoding="utf-8") as f:
        f.write(summary + "\n")
    print(summary)


if __name__ == "__main__":
    main()
