// Benchmark: naive recursive Fibonacci (call-bound).
// k=35 — identical across Skarn, Python, C++.
#include <cstdio>
#include <chrono>

static long long fib(int k) {
    return k < 2 ? k : fib(k - 1) + fib(k - 2);
}

int main() {
    static volatile int _k_runtime = 35;
    const int k = _k_runtime;
    auto t0 = std::chrono::high_resolution_clock::now();

    long long r = fib(k);

    auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now() - t0).count();

    std::printf("benchmark=fib  k=%d  result=%lld  wall_ns=%lld\n", k, r, wall_ns);
    std::printf("gc_collections=0  gc_bytes=0  gc_ns=0\n");
    return 0;
}
