// Benchmark: filter -> map -> fold (lazy pipeline matching Skarn semantics).
// n=2_000_000 — identical across Skarn, Python, C++.
// std::views pipeline mirrors Skarn's fold(map(filter(...))).
#include <cstdio>
#include <chrono>
#include <ranges>

int main() {
    const long long n = 2'000'000LL;
    auto t0 = std::chrono::high_resolution_clock::now();

    auto rng = std::views::iota(0LL, n)
             | std::views::filter([](long long x) { return x % 2 == 0; })
             | std::views::transform([](long long x) { return x + 1; });
    long long total = 0;
    for (long long x : rng) total += x;

    auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now() - t0).count();

    std::printf("benchmark=iter  n=%lld  total=%lld  wall_ns=%lld  ns/elem=%lld\n",
                n, total, wall_ns, wall_ns / n);
    std::printf("gc_collections=0  gc_bytes=0  gc_ns=0\n");
    return 0;
}
