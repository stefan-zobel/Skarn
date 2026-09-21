// Benchmark: idiomatic string building via operator+= on std::string (O(n) amortized).
// n=1_000_000 — identical across Skarn, Python, C++.
// Measures the builder path; strings.cpp measures the O(n^2) concat path.
#include <cstdio>
#include <chrono>
#include <string>

int main() {
    static volatile long long _n_runtime = 1'000'000LL;
    const long long n = _n_runtime;

    auto t0 = std::chrono::high_resolution_clock::now();

    std::string s;
    for (long long i = 0; i < n; ++i) s += 'x';

    auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now() - t0).count();

    std::printf("benchmark=strings_build  n=%lld  len=%zu  wall_ns=%lld  ns/iter=%lld\n",
                n, s.size(), wall_ns, wall_ns / n);
    std::printf("gc_collections=0  gc_bytes=0  gc_ns=0\n");
    return 0;
}
