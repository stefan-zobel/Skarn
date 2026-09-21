#!/usr/bin/env python3
# Benchmark: allocation pressure — create and discard many short-lived lists.
# n=50_000, size=100 — identical across Skarn, Python, C++.
import time, gc

_gc_ns = 0; _gc_start = 0
def _gc_cb(phase, info):
    global _gc_ns, _gc_start
    if phase == 'start': _gc_start = time.perf_counter_ns()
    else: _gc_ns += time.perf_counter_ns() - _gc_start
gc.callbacks.append(_gc_cb)

def main():
    gc_before = sum(s['collections'] for s in gc.get_stats())
    n    = 50_000
    size = 100
    t0   = time.perf_counter_ns()
    total = 0
    for _ in range(n):
        v = []
        for j in range(size):
            v.append(j)
        total += v[size - 1]
    wall_ns = time.perf_counter_ns() - t0
    gc_collections = sum(s['collections'] for s in gc.get_stats()) - gc_before
    print(f"benchmark=alloc  n={n}  size={size}  checksum={total}  wall_ns={wall_ns}")
    print(f"gc_collections={gc_collections}  gc_bytes=0  gc_ns={_gc_ns}")

if __name__ == "__main__":
    main()
