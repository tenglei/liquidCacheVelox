// bench_bit_packed.cpp
// Microbenchmark: BitPackedArray pack() + bulk_unpack_to() for bw=1..32
//
// Tests each bit width with 1M elements to get stable timing.
// Reports cycles-per-element for pack and unpack separately.

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

#include "liquid_cache/bit_packed_array.h"

using namespace liquid_cache;
using Clock = std::chrono::high_resolution_clock;

static constexpr uint32_t N = 1'000'000;

// Generate values ensuring max fits in bw bits
static std::vector<uint64_t> gen(uint8_t bw) {
    std::vector<uint64_t> v(N);
    std::mt19937_64 rng(42);
    uint64_t mask = (bw < 64) ? ((1ULL << bw) - 1) : ~0ULL;
    for (uint32_t i = 0; i < N; ++i) {
        v[i] = rng() & mask;
    }
    return v;
}

struct Result {
    uint8_t bw;
    double pack_ns;       // nanoseconds per element (pack)
    double unpack_ns;     // nanoseconds per element (unpack)
    size_t packed_bytes;
};

static Result bench(uint8_t bw) {
    auto values = gen(bw);
    // warmup: 3 iterations
    for (int w = 0; w < 3; ++w) {
        BitPackedArray bpa(values.data(), nullptr, N, bw);
        std::vector<uint64_t> out(N);
        bpa.bulk_unpack_to(out.data());
    }

    // Pack benchmark
    auto t0 = Clock::now();
    constexpr int PACK_ITERS = 20;
    for (int r = 0; r < PACK_ITERS; ++r) {
        BitPackedArray bpa(values.data(), nullptr, N, bw);
        // prevent dead-code elimination
        volatile auto s = bpa.memory_size();
        (void)s;
    }
    auto t1 = Clock::now();
    double pack_total_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    double pack_ns_per = pack_total_ns / (N * PACK_ITERS);

    // Unpack benchmark (construct once, unpack many times)
    BitPackedArray bpa(values.data(), nullptr, N, bw);
    std::vector<uint64_t> out(N);
    auto t2 = Clock::now();
    constexpr int UNPACK_ITERS = 200;
    for (int r = 0; r < UNPACK_ITERS; ++r) {
        bpa.bulk_unpack_to(out.data());
        volatile auto x = out[0]; (void)x;
    }
    auto t3 = Clock::now();
    double unpack_total_ns = std::chrono::duration<double, std::nano>(t3 - t2).count();
    double unpack_ns_per = unpack_total_ns / (N * UNPACK_ITERS);

    return {bw, pack_ns_per, unpack_ns_per, bpa.memory_size()};
}

int main() {
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "BitPackedArray benchmark — " << N << " elements\n";
    std::cout << "  Pack: " << 20 << " iters, Unpack: " << 200 << " iters\n\n";
    std::cout << std::setw(5) << "BW"
              << std::setw(12) << "Pack(ns/e)"
              << std::setw(14) << "Unpack(ns/e)"
              << std::setw(12) << "Packed(KB)"
              << "  Note\n";
    std::cout << std::string(65, '-') << "\n";

    // Priority widths that go through AVX2 fast paths
    std::vector<uint8_t> avx2_widths = {1, 2, 4, 8, 16, 32};
    // Non-AVX2 widths — template-unrolled scalar (these are potential hotspots)
    std::vector<uint8_t> scalar_widths = {3, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15,
                                          17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
                                          27, 28, 29, 30, 31};
    // Wide widths (just for comparison)
    std::vector<uint8_t> wide_widths = {48, 56, 64};

    auto print_results = [](const std::vector<uint8_t>& widths, const char* note) {
        for (auto bw : widths) {
            auto r = bench(bw);
            std::cout << std::setw(5) << (int)r.bw
                      << std::setw(12) << r.pack_ns
                      << std::setw(14) << r.unpack_ns
                      << std::setw(12) << (r.packed_bytes / 1024)
                      << "  " << note << "\n";
        }
    };

    print_results(avx2_widths, "AVX2 fast path");
    print_results(scalar_widths, "template-unrolled scalar");
    print_results(wide_widths, "wide (reference)");

    std::cout << "\nSummary:\n";
    std::cout << "  Columns with highlight are non-AVX2 widths that could benefit from optimization.\n";
    std::cout << "  A 'good' target is < 1 ns/e for unpack on modern CPUs.\n";

    return 0;
}
