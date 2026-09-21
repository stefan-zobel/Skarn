#!/usr/bin/env python3
# Benchmark: tight integer arithmetic loop (compute-bound).
# n=10_000_000 — identical across Skarn, Python, C++.
import time, gc

_gc_ns = 0; _gc_start = 0
def _gc_cb(phase, info):
    global _gc_ns, _gc_start
    if phase == 'start': _gc_start = time.perf_counter_ns()
    else: _gc_ns += time.perf_counter_ns() - _gc_start
gc.callbacks.append(_gc_cb)

def main():
    gc_before = sum(s['collections'] for s in gc.get_stats())
    n = 10_000_000
    t0 = time.perf_counter_ns()
    s = 0; i = 0
    while i < n:
        s += i; i += 1
    wall_ns = time.perf_counter_ns() - t0
    gc_collections = sum(s2['collections'] for s2 in gc.get_stats()) - gc_before
    print(f"benchmark=arith  n={n}  checksum={s}  wall_ns={wall_ns}  ns/iter={wall_ns // n}")
    print(f"gc_collections={gc_collections}  gc_bytes=0  gc_ns={_gc_ns}")

if __name__ == "__main__":
    main()
