// Benchmark: insertion sort on an array of random ints.
// n=5_000 — identical across Skarn, Python, C++.
#include <cstdio>
#include <chrono>

static unsigned xorshift(unsigned& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s;
}

int main() {
    const int n = 5'000;
    static int arr[5'000];
    unsigned state = 42;
    for (int i = 0; i < n; ++i)
        arr[i] = (int)(xorshift(state) % 1'000'000);

    auto t0 = std::chrono::high_resolution_clock::now();

    for (int i = 1; i < n; ++i) {
        int key = arr[i], j = i - 1;
        while (j >= 0 && arr[j] > key) { arr[j + 1] = arr[j]; --j; }
        arr[j + 1] = key;
    }

    auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now() - t0).count();

    long long checksum = 0;
    for (int i = 0; i < n; ++i) checksum += (long long)arr[i] * i;

    std::printf("benchmark=sort  n=%d  checksum=%lld  wall_ns=%lld  ns/elem=%lld\n",
                n, checksum, wall_ns, wall_ns / n);
    std::printf("gc_collections=0  gc_bytes=0  gc_ns=0\n");
    return 0;
}
