// Benchmark: heap allocation pressure — create and destroy short-lived vectors.
// n=50_000, size=100 — identical across Skarn, Python, C++.
#include <cstdio>
#include <chrono>
#include <vector>

int main() {
    const int n    = 50'000;
    const int size = 100;
    auto t0 = std::chrono::high_resolution_clock::now();

    long long total = 0;
    for (int i = 0; i < n; ++i) {
        std::vector<int> v(size);
        for (int j = 0; j < size; ++j) v[j] = j;
        total += v[size - 1];
    }

    auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now() - t0).count();

    std::printf("benchmark=alloc  n=%d  size=%d  checksum=%lld  wall_ns=%lld\n",
                n, size, total, wall_ns);
    std::printf("gc_collections=0  gc_bytes=0  gc_ns=0\n");
    return 0;
}
