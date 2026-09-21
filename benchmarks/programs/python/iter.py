#!/usr/bin/env python3
# Benchmark: filter -> map -> fold (lazy pipeline matching Skarn semantics).
# n=2_000_000 — identical across Skarn, Python, C++.
# functools.reduce(map(filter())) mirrors Skarn's fold(map(filter(...))).
from functools import reduce
import time, gc

_gc_ns = 0; _gc_start = 0
def _gc_cb(phase, info):
    global _gc_ns, _gc_start
    if phase == 'start': _gc_start = time.perf_counter_ns()
    else: _gc_ns += time.perf_counter_ns() - _gc_start
gc.callbacks.append(_gc_cb)

def main():
    gc_before = sum(s['collections'] for s in gc.get_stats())
    n = 2_000_000
    t0 = time.perf_counter_ns()
    total = reduce(lambda acc, x: acc + x,
                   map(lambda x: x + 1,
                       filter(lambda x: x % 2 == 0, range(n))),
                   0)
    wall_ns = time.perf_counter_ns() - t0
    gc_collections = sum(s['collections'] for s in gc.get_stats()) - gc_before
    print(f"benchmark=iter  n={n}  total={total}  wall_ns={wall_ns}  ns/elem={wall_ns // n}")
    print(f"gc_collections={gc_collections}  gc_bytes=0  gc_ns={_gc_ns}")

if __name__ == "__main__":
    main()
