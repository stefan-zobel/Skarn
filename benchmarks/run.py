#!/usr/bin/env python3
"""
Skarn benchmark runner — multi-language, comprehensive metrics.

Runs identical workloads in Skarn, Python, and C++ and writes a single
results.csv (and results.json) with a `language` column so every row is
directly comparable.

Metrics per row:
  language, benchmark, exit_code
  wall_vm_ns/ms    — program-internal hot-region (nanoTime / perf_counter_ns / chrono)
  wall_total_ns/ms — runner-side wall time (includes startup + I/O)
  user_ns/ms, sys_ns/ms — CPU time from /usr/bin/time
  rss_bytes, rss_mb      — peak RSS from /usr/bin/time
  gc_collections, gc_bytes, gc_mb, gc_ns, gc_ms, gc_pct
  + all benchmark-specific extras (n, k, checksum, ns_per_iter, …)
  + run metadata: timestamp, vm, os, cpu

Usage:
  python3 benchmarks/run.py
  python3 benchmarks/run.py --vm /path/to/skarnvm
  python3 benchmarks/run.py --lang skarn,cpp        # comma-separated subset
  python3 benchmarks/run.py --filter arith           # name prefix filter
  python3 benchmarks/run.py --out results.json --csv results.csv
"""

import argparse, csv, json, os, platform, re, subprocess, sys, time
import datetime
from pathlib import Path

BENCHMARKS_DIR = Path(__file__).parent
REPO_ROOT      = BENCHMARKS_DIR.parent
PROGRAMS_DIR   = BENCHMARKS_DIR / "programs"
CPP_BIN_DIR    = PROGRAMS_DIR / "cpp" / "bin"


# ── VM discovery ──────────────────────────────────────────────────────────────

def find_vm(override=None):
    if override:
        p = Path(override)
        if not p.exists():
            sys.exit(f"error: skarnvm not found at {override}")
        return str(p)
    # env override (used inside Docker)
    if env := os.environ.get("VM_PATH"):
        return env
    candidate = REPO_ROOT / "build" / "skarnvm"
    if candidate.exists():
        return str(candidate)
    import shutil
    v = shutil.which("skarnvm")
    if v:
        return v
    sys.exit(
        f"error: skarnvm not found at {candidate} or in PATH\n"
        "       Build with: cmake --build build --config Release"
    )


# ── C++ compilation ───────────────────────────────────────────────────────────

def compile_cpp(verbose=False):
    src_dir = PROGRAMS_DIR / "cpp"
    CPP_BIN_DIR.mkdir(parents=True, exist_ok=True)
    compiler = os.environ.get("CXX", "g++")
    errors = []
    for src in sorted(src_dir.glob("*.cpp")):
        out = CPP_BIN_DIR / src.stem
        if out.exists() and out.stat().st_mtime >= src.stat().st_mtime:
            continue
        cmd = [compiler, "-O3", "-std=c++20", "-o", str(out), str(src)]
        if verbose:
            print(f"  compile: {' '.join(cmd)}")
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            errors.append(f"    {src.name}: {r.stderr.strip()}")
        elif verbose:
            print(f"  compiled {src.name}")
    if errors:
        print("C++ compile errors:")
        print("\n".join(errors))
        sys.exit(1)


# ── Language configurations ───────────────────────────────────────────────────

def make_lang_configs(vm: str) -> dict:
    return {
        "skarn": {
            "dir": PROGRAMS_DIR / "skarn",
            "ext": ".skn",
            "cmd": lambda prog: [vm, str(prog)],
        },
        "python": {
            "dir": PROGRAMS_DIR / "python",
            "ext": ".py",
            "cmd": lambda prog: [sys.executable, str(prog)],
        },
        "cpp": {
            "dir": PROGRAMS_DIR / "cpp",
            "ext": ".cpp",
            "bin_dir": CPP_BIN_DIR,
            "cmd": lambda prog: [str(CPP_BIN_DIR / prog.stem)],
        },
    }


# ── Run one benchmark ─────────────────────────────────────────────────────────

def run_benchmark(cmd: list, name: str, lang: str) -> dict:
    is_mac = platform.system() == "Darwin"
    timed_cmd = (["/usr/bin/time", "-l"] if is_mac else ["/usr/bin/time", "-v"]) + cmd

    t0 = time.perf_counter_ns()
    result = subprocess.run(timed_cmd, capture_output=True, text=True)
    wall_total_ns = time.perf_counter_ns() - t0

    rss_bytes, user_ns, sys_ns = _parse_time_output(result.stderr, is_mac)
    vm_metrics = _parse_vm_output(result.stdout)

    return {
        "language":      lang,
        "name":          name,
        "exit_code":     result.returncode,
        "wall_vm_ns":    vm_metrics.get("wall_ns", 0),
        "wall_total_ns": wall_total_ns,
        "user_ns":       user_ns,
        "sys_ns":        sys_ns,
        "rss_bytes":     rss_bytes,
        "gc_collections":vm_metrics.get("gc_collections", 0),
        "gc_bytes":      vm_metrics.get("gc_bytes", 0),
        "gc_ns":         vm_metrics.get("gc_ns", 0),
        "extra":         {k: v for k, v in vm_metrics.items()
                         if k not in ("benchmark", "wall_ns", "gc_pct",
                                      "gc_collections", "gc_bytes", "gc_ns")},
        "_stdout":       result.stdout,
    }


def _parse_time_output(stderr: str, is_mac: bool):
    rss_bytes = user_ns = sys_ns = None
    if is_mac:
        m = re.search(r'(\d+)\s+maximum resident set size', stderr, re.I)
        if m:
            rss_bytes = int(m.group(1))
        m = re.search(r'([\d.]+)\s+real\s+([\d.]+)\s+user\s+([\d.]+)\s+sys', stderr)
        if m:
            user_ns = int(float(m.group(2)) * 1e9)
            sys_ns  = int(float(m.group(3)) * 1e9)
    else:
        m = re.search(r'Maximum resident set size \(kbytes\):\s*(\d+)', stderr)
        if m:
            rss_bytes = int(m.group(1)) * 1024
        m = re.search(r'User time \(seconds\):\s*([\d.]+)', stderr)
        if m:
            user_ns = int(float(m.group(1)) * 1e9)
        m = re.search(r'System time \(seconds\):\s*([\d.]+)', stderr)
        if m:
            sys_ns = int(float(m.group(1)) * 1e9)
    return rss_bytes, user_ns, sys_ns


def _parse_vm_output(stdout: str) -> dict:
    metrics = {}
    for line in stdout.splitlines():
        if line.startswith("benchmark=") or line.startswith("gc_"):
            for token in line.split():
                if "=" in token:
                    k, v = token.split("=", 1)
                    try:
                        metrics[k] = int(v)
                    except ValueError:
                        try:
                            metrics[k] = float(v)
                        except ValueError:
                            metrics[k] = v
    return metrics


# ── Formatting helpers ────────────────────────────────────────────────────────

def fmt_ns(ns):
    if ns is None:
        return "N/A"
    ns = int(ns)
    if ns == 0:
        return "0ms"
    if ns >= 60_000_000_000:
        return f"{ns/1e9:.1f}s"
    if ns >= 1_000_000_000:
        return f"{ns/1e9:.2f}s"
    if ns >= 1_000_000:
        return f"{ns/1e6:.1f}ms"
    if ns >= 1_000:
        return f"{ns/1e3:.0f}µs"
    return f"{ns}ns"


def fmt_bytes(b):
    if b is None or b == 0:
        return "N/A"
    b = int(b)
    if b >= 1024 ** 3:
        return f"{b/1024**3:.1f}GB"
    if b >= 1024 ** 2:
        return f"{b/1024**2:.1f}MB"
    if b >= 1024:
        return f"{b/1024:.0f}KB"
    return f"{b}B"


# ── Terminal table ────────────────────────────────────────────────────────────

def print_table(results: list[dict]):
    cols = [
        ("Language",    8,  lambda r: r["language"]),
        ("Benchmark",   14, lambda r: r["name"]),
        ("Wall (VM)",   11, lambda r: fmt_ns(r["wall_vm_ns"])),
        ("Wall (tot)",  10, lambda r: fmt_ns(r["wall_total_ns"])),
        ("User CPU",    9,  lambda r: fmt_ns(r["user_ns"])),
        ("Sys CPU",     7,  lambda r: fmt_ns(r["sys_ns"])),
        ("Peak RSS",    9,  lambda r: fmt_bytes(r["rss_bytes"])),
        ("GC%",         5,  lambda r: (
            f"{100*r['gc_ns']/r['wall_vm_ns']:.1f}%"
            if r["wall_vm_ns"] else "N/A")),
    ]
    header = "  ".join(label.ljust(w) for label, w, _ in cols)
    sep    = "  ".join("-" * w for _, w, _ in cols)
    print(header)
    print(sep)
    prev_lang = None
    for r in results:
        if prev_lang and r["language"] != prev_lang:
            print()
        prev_lang = r["language"]
        row = "  ".join(fn(r).ljust(w) for _, w, fn in cols)
        mark = " !" if r["exit_code"] != 0 else ""
        print(row + mark)


# ── CSV output ────────────────────────────────────────────────────────────────

def _csv_key(k: str) -> str:
    return k.replace("/", "_per_").replace("-", "_")


def save_csv(results: list[dict], path: Path, meta: dict):
    """Comprehensive CSV — all metrics + all VM extras, one row per (language, benchmark)."""
    def _f(v, divisor):
        return "" if v is None else f"{v / divisor:.4f}"
    def _i(v):
        return "" if v is None else str(v)

    # Collect all extra keys across all results (preserving first-seen order)
    all_extra_keys: list[str] = []
    seen: set[str] = set()
    for r in results:
        for k in r.get("extra", {}):
            ck = _csv_key(k)
            if ck not in seen:
                seen.add(ck)
                all_extra_keys.append(ck)

    fixed = [
        "timestamp", "vm", "os", "cpu",
        "language", "benchmark", "exit_code",
        "wall_vm_ns",    "wall_vm_ms",
        "wall_total_ns", "wall_total_ms",
        "user_ns",       "user_ms",
        "sys_ns",        "sys_ms",
        "rss_bytes",     "rss_mb",
        "gc_collections",
        "gc_bytes",      "gc_mb",
        "gc_ns",         "gc_ms",
        "gc_pct",
    ]
    fieldnames = fixed + all_extra_keys

    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        w.writeheader()
        for r in results:
            wv   = r["wall_vm_ns"] or 0
            gc_n = r["gc_ns"] or 0
            row: dict = {
                "timestamp":     meta["timestamp"],
                "vm":            meta["vm"],
                "os":            meta["os"],
                "cpu":           meta["cpu"],
                "language":      r["language"],
                "benchmark":     r["name"],
                "exit_code":     r["exit_code"],
                "wall_vm_ns":    _i(r["wall_vm_ns"]),
                "wall_vm_ms":    _f(r["wall_vm_ns"], 1e6),
                "wall_total_ns": _i(r["wall_total_ns"]),
                "wall_total_ms": _f(r["wall_total_ns"], 1e6),
                "user_ns":       _i(r["user_ns"]),
                "user_ms":       _f(r["user_ns"], 1e6),
                "sys_ns":        _i(r["sys_ns"]),
                "sys_ms":        _f(r["sys_ns"], 1e6),
                "rss_bytes":     _i(r["rss_bytes"]),
                "rss_mb":        _f(r["rss_bytes"], 1024 * 1024),
                "gc_collections":_i(r["gc_collections"]),
                "gc_bytes":      _i(r["gc_bytes"]),
                "gc_mb":         _f(r["gc_bytes"], 1024 * 1024),
                "gc_ns":         _i(r["gc_ns"]),
                "gc_ms":         _f(r["gc_ns"], 1e6),
                "gc_pct":        f"{100 * gc_n / wv:.4f}" if wv else "",
            }
            for k, v in r.get("extra", {}).items():
                row[_csv_key(k)] = str(v)
            w.writerow(row)

    print(f"CSV  saved to {path}")


def save_json(results: list[dict], path: Path):
    serializable = [{k: v for k, v in r.items() if not k.startswith("_")}
                    for r in results]
    path.write_text(json.dumps(serializable, indent=2))
    print(f"JSON saved to {path}")


# ── System info ───────────────────────────────────────────────────────────────

def cpu_info() -> str:
    try:
        if platform.system() == "Darwin":
            return subprocess.check_output(
                ["sysctl", "-n", "machdep.cpu.brand_string"], text=True).strip()
        # Linux: try lscpu first (works on x86 and ARM)
        try:
            out = subprocess.check_output(["lscpu"], text=True)
            for line in out.splitlines():
                if line.startswith("Model name:"):
                    v = line.split(":", 1)[1].strip()
                    if v:
                        return v
        except Exception:
            pass
        # Fallback: /proc/cpuinfo — x86 has "model name", ARM has "Hardware"
        with open("/proc/cpuinfo") as f:
            content = f.read()
        for field in ("model name", "Hardware", "cpu model"):
            for line in content.splitlines():
                if line.lower().startswith(field):
                    v = line.split(":", 1)[1].strip()
                    if v:
                        return v
    except Exception:
        pass
    return "unknown"


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="Skarn multi-language benchmark runner")
    ap.add_argument("--vm",     metavar="PATH", help="path to skarnvm binary")
    ap.add_argument("--lang",   metavar="LIST", default="skarn,python,cpp",
                    help="comma-separated languages to run (default: skarn,python,cpp)")
    ap.add_argument("--filter", metavar="NAME",
                    help="run only benchmarks whose name starts with NAME")
    ap.add_argument("--out",    metavar="FILE",
                    default=str(BENCHMARKS_DIR / "results" / "results.json"))
    ap.add_argument("--csv",    metavar="FILE",
                    default=str(BENCHMARKS_DIR / "results" / "results.csv"))
    ap.add_argument("--no-compile", action="store_true",
                    help="skip C++ compilation step")
    args = ap.parse_args()

    langs     = [l.strip() for l in args.lang.split(",")]
    vm        = find_vm(args.vm)
    lang_cfgs = make_lang_configs(vm)

    # Compile C++ unless skipped or not in the run list
    if "cpp" in langs and not args.no_compile:
        print("Compiling C++ benchmarks...")
        compile_cpp(verbose=False)
        print("Done.\n")

    ts     = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    cpu    = cpu_info()
    os_str = f"{platform.system()} {platform.machine()}"

    print(f"VM:   {vm}")
    print(f"OS:   {os_str}")
    print(f"CPU:  {cpu}")
    print(f"Date: {ts}")
    print(f"Langs: {', '.join(langs)}\n")

    meta = {"timestamp": ts, "vm": vm, "os": os_str, "cpu": cpu}

    results = []
    for lang in langs:
        if lang not in lang_cfgs:
            print(f"Unknown language '{lang}', skipping.")
            continue
        cfg = lang_cfgs[lang]
        programs = sorted(cfg["dir"].glob(f"*{cfg['ext']}"))
        if args.filter:
            programs = [p for p in programs if p.stem.startswith(args.filter)]
        if not programs:
            print(f"  [{lang}] no programs found in {cfg['dir']}")
            continue

        for prog in programs:
            cmd = cfg["cmd"](prog)
            label = f"{lang}/{prog.stem}"
            print(f"  {label}...", end="", flush=True)
            r = run_benchmark(cmd, prog.stem, lang)
            results.append(r)
            status = "ok" if r["exit_code"] == 0 else f"exit {r['exit_code']}"
            print(f"\r  {label:<26} {fmt_ns(r['wall_vm_ns']):>10}  [{status}]")

    print()
    print_table(results)
    print()

    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.csv).parent.mkdir(parents=True, exist_ok=True)
    save_json(results, Path(args.out))
    save_csv(results, Path(args.csv), meta)


if __name__ == "__main__":
    main()
