#!/usr/bin/env python3
# Benchmark: dict insertion and lookup throughput.
# n=200_000 — identical across Skarn, Python, C++.
import time, gc

_gc_ns = 0; _gc_start = 0
def _gc_cb(phase, info):
    global _gc_ns, _gc_start
    if phase == 'start': _gc_start = time.perf_counter_ns()
    else: _gc_ns += time.perf_counter_ns() - _gc_start
gc.callbacks.append(_gc_cb)

def main():
    gc_before = sum(s['collections'] for s in gc.get_stats())
    n = 200_000
    d = {}
    t0 = time.perf_counter_ns()
    for i in range(n):
        d[i] = i % 1000
    checksum = 0
    for i in range(n):
        checksum += d.get(i, 0)
    wall_ns = time.perf_counter_ns() - t0
    gc_collections = sum(s['collections'] for s in gc.get_stats()) - gc_before
    print(f"benchmark=hashmap  n={n}  checksum={checksum}  wall_ns={wall_ns}  ns/op={wall_ns // (n * 2)}")
    print(f"gc_collections={gc_collections}  gc_bytes=0  gc_ns={_gc_ns}")

if __name__ == "__main__":
    main()
