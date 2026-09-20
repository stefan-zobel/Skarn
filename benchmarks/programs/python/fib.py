#!/usr/bin/env python3
# Benchmark: naive recursive Fibonacci (call-bound).
# k=35 — identical across Skarn, Python, C++.
import sys, time, gc
sys.setrecursionlimit(100_000)

_gc_ns = 0; _gc_start = 0
def _gc_cb(phase, info):
    global _gc_ns, _gc_start
    if phase == 'start': _gc_start = time.perf_counter_ns()
    else: _gc_ns += time.perf_counter_ns() - _gc_start
gc.callbacks.append(_gc_cb)
gc_before = sum(s['collections'] for s in gc.get_stats())

def fib(k):
    return k if k < 2 else fib(k - 1) + fib(k - 2)

k = 35
t0 = time.perf_counter_ns()
r = fib(k)
wall_ns = time.perf_counter_ns() - t0

gc_collections = sum(s['collections'] for s in gc.get_stats()) - gc_before
print(f"benchmark=fib  k={k}  result={r}  wall_ns={wall_ns}")
print(f"gc_collections={gc_collections}  gc_bytes=0  gc_ns={_gc_ns}")
