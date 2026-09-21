// Benchmark: unordered_map insertion and lookup throughput.
// n=200_000 — identical across Skarn, Python, C++.
// No reserve() so all three languages pay the same rehash/growth cost inside the timer.
#include <cstdio>
#include <chrono>
#include <unordered_map>

int main() {
    const int n = 200'000;

    auto t0 = std::chrono::high_resolution_clock::now();

    std::unordered_map<int, int> m;

    for (int i = 0; i < n; ++i) m[i] = i % 1000;

    long long checksum = 0;
    for (int i = 0; i < n; ++i) {
        auto it = m.find(i);
        if (it != m.end()) checksum += it->second;
    }

    auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now() - t0).count();

    std::printf("benchmark=hashmap  n=%d  checksum=%lld  wall_ns=%lld  ns/op=%lld\n",
                n, checksum, wall_ns, wall_ns / (2LL * n));
    std::printf("gc_collections=0  gc_bytes=0  gc_ns=0\n");
    return 0;
}
