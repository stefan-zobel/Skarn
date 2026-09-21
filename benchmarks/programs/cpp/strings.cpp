// Benchmark: string concatenation pressure (O(n^2) copies).
// n=10_000 — identical across Skarn, Python, C++.
// s = s + "x" forces a full copy each iteration, matching Python and Skarn semantics.
#include <cstdio>
#include <chrono>
#include <string>

int main() {
    const int n = 10'000;
    auto t0 = std::chrono::high_resolution_clock::now();

    std::string s;
    for (int i = 0; i < n; ++i) s = s + "x";

    auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now() - t0).count();

    std::printf("benchmark=strings  n=%d  len=%zu  wall_ns=%lld  ns/iter=%lld\n",
                n, s.size(), wall_ns, wall_ns / n);
    std::printf("gc_collections=0  gc_bytes=0  gc_ns=0\n");
    return 0;
}
