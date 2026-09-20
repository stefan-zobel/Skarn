#!/usr/bin/env python3
"""
Compare two benchmark JSON result files produced by run.py.

Prints a side-by-side table with speedup ratios for wall time, CPU, and RSS.
A ratio < 1 means the 'after' run is faster / smaller.

Usage:
  python3 benchmarks/compare.py results_before.json results_after.json
  python3 benchmarks/compare.py results_before.json results_after.json --sort wall
"""

import argparse
import json
import sys
from pathlib import Path


# ── Formatting ────────────────────────────────────────────────────────────────

def fmt_ns(ns):
    if ns is None or ns == 0:
        return "N/A"
    ns = int(ns)
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
    if b >= 1024 ** 2:
        return f"{b/1024**2:.1f}MB"
    if b >= 1024:
        return f"{b/1024:.0f}KB"
    return f"{b}B"


def fmt_ratio(before, after):
    """Return a ratio string. < 1 = improvement (faster/smaller)."""
    if not before or not after or before == 0:
        return "   N/A"
    r = after / before
    if r < 0.95:
        arrow = "▼"   # improvement
    elif r > 1.05:
        arrow = "▲"   # regression
    else:
        arrow = "~"   # within 5%
    return f"{arrow} {r:.2f}x"


def fmt_delta_pct(before, after):
    if not before or before == 0:
        return "N/A"
    pct = 100 * (after - before) / before
    sign = "+" if pct > 0 else ""
    return f"{sign}{pct:.1f}%"


# ── Main ──────────────────────────────────────────────────────────────────────

def load(path: str) -> dict:
    data = json.loads(Path(path).read_text())
    # Support both list and dict formats from run.py
    if isinstance(data, list):
        return {r["name"]: r for r in data}
    return data


def main():
    ap = argparse.ArgumentParser(description="Compare two Skarn benchmark result files")
    ap.add_argument("before", help="JSON file from the baseline run")
    ap.add_argument("after",  help="JSON file from the comparison run")
    ap.add_argument("--sort", choices=["name", "wall", "rss", "ratio"],
                    default="name", help="sort column (default: name)")
    args = ap.parse_args()

    before = load(args.before)
    after  = load(args.after)

    all_names = sorted(set(before) | set(after))

    rows = []
    for name in all_names:
        b = before.get(name, {})
        a = after.get(name,  {})
        wb = b.get("wall_vm_ns") or 0
        wa = a.get("wall_vm_ns") or 0
        ub = b.get("user_ns") or 0
        ua = a.get("user_ns") or 0
        rb = b.get("rss_bytes") or 0
        ra = a.get("rss_bytes") or 0
        ratio = (wa / wb) if wb else None
        rows.append((name, wb, wa, ub, ua, rb, ra, ratio))

    if args.sort == "wall":
        rows.sort(key=lambda r: r[2] or 0, reverse=True)
    elif args.sort == "rss":
        rows.sort(key=lambda r: r[6] or 0, reverse=True)
    elif args.sort == "ratio":
        rows.sort(key=lambda r: r[7] or 1.0, reverse=True)

    cols = [18, 11, 11, 9, 9, 9, 9, 8, 9]
    header = ["Benchmark",
              "Wall before", "Wall after",
              "User bef",    "User aft",
              "RSS bef",     "RSS aft",
              "Δ Wall",      "Δ Wall %"]
    print("  ".join(h.ljust(w) for h, w in zip(header, cols)))
    print("  ".join("-" * w for w in cols))

    for name, wb, wa, ub, ua, rb, ra, ratio in rows:
        row = [
            name,
            fmt_ns(wb),
            fmt_ns(wa),
            fmt_ns(ub),
            fmt_ns(ua),
            fmt_bytes(rb),
            fmt_bytes(ra),
            fmt_ratio(wb, wa),
            fmt_delta_pct(wb, wa),
        ]
        print("  ".join(v.ljust(w) for v, w in zip(row, cols)))

    print()
    improvements = sum(1 for _, wb, wa, *_ in rows if wb and wa and wa < wb * 0.95)
    regressions  = sum(1 for _, wb, wa, *_ in rows if wb and wa and wa > wb * 1.05)
    print(f"Summary: {improvements} improved, {regressions} regressed, "
          f"{len(rows) - improvements - regressions} neutral  "
          f"(▼ = faster/smaller, ▲ = slower/larger)")


if __name__ == "__main__":
    main()
