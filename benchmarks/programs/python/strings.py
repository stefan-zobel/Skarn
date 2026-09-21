#!/usr/bin/env python3
# Benchmark: string concatenation pressure (O(n^2) copies).
# n=10_000 — identical across Skarn, Python, C++.
# s = s + "x" and s += "x" are identical at module level; CPython's in-place fast path
# only applies inside a function (where refcount == 1 reliably).
import time, gc

_gc_ns = 0; _gc_start = 0
def _gc_cb(phase, info):
    global _gc_ns, _gc_start
    if phase == 'start': _gc_start = time.perf_counter_ns()
    else: _gc_ns += time.perf_counter_ns() - _gc_start
gc.callbacks.append(_gc_cb)
gc_before = sum(s['collections'] for s in gc.get_stats())

n = 10_000
t0 = time.perf_counter_ns()
s = ""
for _ in range(n):
    s = s + "x"
wall_ns = time.perf_counter_ns() - t0

gc_collections = sum(s2['collections'] for s2 in gc.get_stats()) - gc_before
print(f"benchmark=strings  n={n}  len={len(s)}  wall_ns={wall_ns}  ns/iter={wall_ns // n}")
print(f"gc_collections={gc_collections}  gc_bytes=0  gc_ns={_gc_ns}")
