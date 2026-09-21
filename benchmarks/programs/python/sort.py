#!/usr/bin/env python3
# Benchmark: insertion sort on a list of random integers.
# n=5_000 — identical across Skarn, Python, C++.
# Uses the same xorshift32(13, 17, 5) as the C++ program so all three
# languages sort the same input permutation.
import time, gc

_gc_ns = 0; _gc_start = 0
def _gc_cb(phase, info):
    global _gc_ns, _gc_start
    if phase == 'start': _gc_start = time.perf_counter_ns()
    else: _gc_ns += time.perf_counter_ns() - _gc_start
gc.callbacks.append(_gc_cb)

def _xorshift32(s):
    s ^= (s << 13) & 0xFFFFFFFF
    s ^= (s >> 17)
    s ^= (s << 5) & 0xFFFFFFFF
    return s & 0xFFFFFFFF

def main():
    gc_before = sum(s['collections'] for s in gc.get_stats())
    n = 5_000
    state = 42
    arr = []
    for _ in range(n):
        state = _xorshift32(state)
        arr.append(state % 1_000_000)
    t0 = time.perf_counter_ns()
    for i in range(1, n):
        key = arr[i]; j = i - 1
        while j >= 0 and arr[j] > key:
            arr[j + 1] = arr[j]; j -= 1
        arr[j + 1] = key
    wall_ns = time.perf_counter_ns() - t0
    checksum = sum(arr[i] * i for i in range(n))
    gc_collections = sum(s['collections'] for s in gc.get_stats()) - gc_before
    print(f"benchmark=sort  n={n}  checksum={checksum}  wall_ns={wall_ns}  ns/elem={wall_ns // n}")
    print(f"gc_collections={gc_collections}  gc_bytes=0  gc_ns={_gc_ns}")

if __name__ == "__main__":
    main()
