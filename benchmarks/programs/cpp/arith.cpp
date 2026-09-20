// Benchmark: tight integer arithmetic loop (compute-bound).
// n=10_000_000 — identical across Skarn, Python, C++.
#include <cstdio>
#include <chrono>

int main() {
    const long long n = 10'000'000LL;
    auto t0 = std::chrono::high_resolution_clock::now();

    long long s = 0;
    for (long long i = 0; i < n; ++i) s += i;

    auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now() - t0).count();

    std::printf("benchmark=arith  n=%lld  checksum=%lld  wall_ns=%lld  ns/iter=%lld\n",
                n, s, wall_ns, wall_ns / n);
    std::printf("gc_collections=0  gc_bytes=0  gc_ns=0\n");
    return 0;
}
