// Benchmark: filter -> map -> fold (as explicit loop).
// n=2_000_000 — identical across Skarn, Python, C++.
#include <cstdio>
#include <chrono>

int main() {
    const long long n = 2'000'000LL;
    auto t0 = std::chrono::high_resolution_clock::now();

    long long total = 0;
    for (long long x = 0; x < n; ++x) {
        if (x % 2 == 0) total += x + 1;
    }

    auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now() - t0).count();

    std::printf("benchmark=iter  n=%lld  total=%lld  wall_ns=%lld  ns/elem=%lld\n",
                n, total, wall_ns, wall_ns / n);
    std::printf("gc_collections=0  gc_bytes=0  gc_ns=0\n");
    return 0;
}
