// test_bit_packed.cpp
// Unit tests for BitPackedArray — the core bit-packing component.
// Tests all supported bit widths (1-64), AVX2 SIMD paths, edge cases,
// serialization, and null bitmap handling.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "liquid_cache/bit_packed_array.h"
#include "liquid_cache/liquid_arrays.h"

using namespace liquid_cache;

static std::vector<uint64_t> generate_values(uint32_t count, uint8_t bw) {
    std::vector<uint64_t> values(count);
    uint64_t mask = (bw < 64) ? ((1ULL << bw) - 1) : ~0ULL;
    for (uint32_t i = 0; i < count; ++i) {
        values[i] = static_cast<uint64_t>(i * 13 + 7) & mask;
    }
    return values;
}

// ═══════════════════════════════════════════════════════════════════════
// Basic pack/unpack tests for each important bit width
// ═══════════════════════════════════════════════════════════════════════

TEST(BitPackedArray, BitWidth1) {
    auto vals = generate_values(256, 1);
    BitPackedArray bpa(vals.data(), nullptr, 256, 1);
    auto unpacked = bpa.unpack_all();
    for (size_t i = 0; i < 256; ++i) {
        EXPECT_EQ(unpacked[i], vals[i]) << "bw=1 mismatch at " << i;
    }
}

TEST(BitPackedArray, BitWidth2) {
    auto vals = generate_values(128, 2);
    BitPackedArray bpa(vals.data(), nullptr, 128, 2);
    auto unpacked = bpa.unpack_all();
    for (size_t i = 0; i < 128; ++i) {
        EXPECT_EQ(unpacked[i], vals[i]);
    }
}

TEST(BitPackedArray, BitWidth4) {
    auto vals = generate_values(256, 4);
    BitPackedArray bpa(vals.data(), nullptr, 256, 4);
    auto unpacked = bpa.unpack_all();
    for (size_t i = 0; i < 256; ++i) {
        EXPECT_EQ(unpacked[i], vals[i]);
    }
}

TEST(BitPackedArray, BitWidth8) {
    auto vals = generate_values(256, 8);
    BitPackedArray bpa(vals.data(), nullptr, 256, 8);
    auto unpacked = bpa.unpack_all();
    for (size_t i = 0; i < 256; ++i) {
        EXPECT_EQ(unpacked[i], vals[i]);
    }
}

TEST(BitPackedArray, BitWidth16) {
    auto vals = generate_values(128, 16);
    BitPackedArray bpa(vals.data(), nullptr, 128, 16);
    auto unpacked = bpa.unpack_all();
    for (size_t i = 0; i < 128; ++i) {
        EXPECT_EQ(unpacked[i], vals[i]);
    }
}

TEST(BitPackedArray, BitWidth32) {
    auto vals = generate_values(64, 32);
    BitPackedArray bpa(vals.data(), nullptr, 64, 32);
    auto unpacked = bpa.unpack_all();
    for (size_t i = 0; i < 64; ++i) {
        EXPECT_EQ(unpacked[i], vals[i]);
    }
}

TEST(BitPackedArray, BitWidth24) {
    // Non-power-of-2 width tests scalar fallback
    auto vals = generate_values(100, 24);
    BitPackedArray bpa(vals.data(), nullptr, 100, 24);
    auto unpacked = bpa.unpack_all();
    for (size_t i = 0; i < 100; ++i) {
        EXPECT_EQ(unpacked[i], vals[i]);
    }
}

TEST(BitPackedArray, BitWidth13) {
    auto vals = generate_values(200, 13);
    BitPackedArray bpa(vals.data(), nullptr, 200, 13);
    auto unpacked = bpa.unpack_all();
    for (size_t i = 0; i < 200; ++i) {
        EXPECT_EQ(unpacked[i], vals[i]);
    }
}

TEST(BitPackedArray, BitWidth64) {
    uint64_t vals[] = {
        0ULL, 1ULL, UINT64_MAX / 2, UINT64_MAX, 42ULL
    };
    BitPackedArray bpa(vals, nullptr, 5, 64);
    auto unpacked = bpa.unpack_all();
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(unpacked[i], vals[i]);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Edge cases
// ═══════════════════════════════════════════════════════════════════════

TEST(BitPackedArray, Empty) {
    BitPackedArray bpa(nullptr, nullptr, 0, 0);
    EXPECT_EQ(bpa.length(), 0u);
    EXPECT_EQ(bpa.bit_width(), 0);
    auto unpacked = bpa.unpack_all();
    EXPECT_TRUE(unpacked.empty());
}

TEST(BitPackedArray, SingleElement) {
    uint64_t vals[] = {7};
    BitPackedArray bpa(vals, nullptr, 1, 3);
    EXPECT_EQ(bpa.length(), 1u);
    EXPECT_EQ(bpa.get(0), 7u);
}

TEST(BitPackedArray, AllMaxValues) {
    std::vector<uint64_t> vals(100, (1ULL << 10) - 1);
    BitPackedArray bpa(vals.data(), nullptr, 100, 10);
    auto unpacked = bpa.unpack_all();
    for (size_t i = 0; i < 100; ++i) {
        EXPECT_EQ(unpacked[i], vals[i]);
    }
}

TEST(BitPackedArray, AllZeroValues) {
    std::vector<uint64_t> vals(100, 0);
    BitPackedArray bpa(vals.data(), nullptr, 100, 8);
    auto unpacked = bpa.unpack_all();
    for (size_t i = 0; i < 100; ++i) {
        EXPECT_EQ(unpacked[i], 0u);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Null bitmap tests
// ═══════════════════════════════════════════════════════════════════════

TEST(BitPackedArray, WithNulls) {
    std::vector<uint64_t> vals(8, 0);
    std::vector<uint8_t> nulls(1, 0x0F); // first 4 valid (bit=1), last 4 null (bit=0)
    BitPackedArray bpa(vals.data(), nulls.data(), 8, 4);
    EXPECT_EQ(bpa.null_count(), 4);
    EXPECT_FALSE(bpa.is_null(0));
    EXPECT_FALSE(bpa.is_null(3));
    EXPECT_TRUE(bpa.is_null(4));
    EXPECT_TRUE(bpa.is_null(7));
}

TEST(BitPackedArray, AllNull) {
    std::vector<uint64_t> vals(16, 0);
    std::vector<uint8_t> nulls(2, 0x00); // all null
    BitPackedArray bpa(vals.data(), nulls.data(), 16, 0);
    EXPECT_EQ(bpa.null_count(), 16);
    EXPECT_TRUE(bpa.is_null(0));
    EXPECT_TRUE(bpa.is_null(15));
}

TEST(BitPackedArray, NoNull) {
    std::vector<uint64_t> vals(32, 5);
    BitPackedArray bpa(vals.data(), nullptr, 32, 3);
    EXPECT_EQ(bpa.null_count(), 0);
    EXPECT_FALSE(bpa.has_nulls());
    EXPECT_FALSE(bpa.is_null(10));
}

// ═══════════════════════════════════════════════════════════════════════
// Bulk unpack tests (AVX2 SIMD) - test via typed output buffers
// ═══════════════════════════════════════════════════════════════════════

TEST(BitPackedArray, BulkUnpackUint8) {
    auto vals = generate_values(256, 4);
    BitPackedArray bpa(vals.data(), nullptr, 256, 4);
    std::vector<uint8_t> out(256);
    bpa.bulk_unpack_to(out.data());
    for (size_t i = 0; i < 256; ++i) {
        EXPECT_EQ(static_cast<uint64_t>(out[i]), vals[i]);
    }
}

TEST(BitPackedArray, BulkUnpackUint16) {
    auto vals = generate_values(128, 12);
    BitPackedArray bpa(vals.data(), nullptr, 128, 12);
    std::vector<uint16_t> out(128);
    bpa.bulk_unpack_to(out.data());
    for (size_t i = 0; i < 128; ++i) {
        EXPECT_EQ(static_cast<uint64_t>(out[i]), vals[i]);
    }
}

TEST(BitPackedArray, BulkUnpackUint32) {
    auto vals = generate_values(64, 20);
    BitPackedArray bpa(vals.data(), nullptr, 64, 20);
    std::vector<uint32_t> out(64);
    bpa.bulk_unpack_to(out.data());
    for (size_t i = 0; i < 64; ++i) {
        EXPECT_EQ(static_cast<uint64_t>(out[i]), vals[i]);
    }
}

TEST(BitPackedArray, BulkUnpackUint64) {
    auto vals = generate_values(32, 48);
    BitPackedArray bpa(vals.data(), nullptr, 32, 48);
    std::vector<uint64_t> out(32);
    bpa.bulk_unpack_to(out.data());
    for (size_t i = 0; i < 32; ++i) {
        EXPECT_EQ(out[i], vals[i]);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Serialization roundtrip tests
// ═══════════════════════════════════════════════════════════════════════

TEST(BitPackedArray, SerializationRoundtrip) {
    auto vals = generate_values(100, 11);
    BitPackedArray bpa(vals.data(), nullptr, 100, 11);
    std::vector<uint8_t> serialized;
    bpa.serialize(serialized);
    ASSERT_GT(serialized.size(), 16u); // at least header

    auto restored = BitPackedArray::deserialize(serialized.data(), serialized.size());
    EXPECT_EQ(restored.length(), 100u);
    EXPECT_EQ(restored.bit_width(), 11);
    EXPECT_FALSE(restored.has_nulls());

    auto unpacked = restored.unpack_all();
    for (size_t i = 0; i < 100; ++i) {
        EXPECT_EQ(unpacked[i], vals[i]);
    }
}

TEST(BitPackedArray, SerializationWithNulls) {
    std::vector<uint64_t> vals(8, 0);
    std::vector<uint8_t> nulls(1, 0x55); // pattern: valid null valid null ... (bit=1=valid, LSB first)
    BitPackedArray bpa(vals.data(), nulls.data(), 8, 3);
    std::vector<uint8_t> serialized;
    bpa.serialize(serialized);

    auto restored = BitPackedArray::deserialize(serialized.data(), serialized.size());
    EXPECT_EQ(restored.length(), 8u);
    EXPECT_EQ(restored.null_count(), 4);
    EXPECT_TRUE(restored.has_nulls());
    EXPECT_FALSE(restored.is_null(0));
    EXPECT_TRUE(restored.is_null(1));
}

TEST(BitPackedArray, SerializationEmpty) {
    BitPackedArray bpa(nullptr, nullptr, 0, 0);
    std::vector<uint8_t> serialized;
    bpa.serialize(serialized);
    EXPECT_GE(serialized.size(), 16u);

    auto restored = BitPackedArray::deserialize(serialized.data(), serialized.size());
    EXPECT_EQ(restored.length(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════
// Large array performance stress test
// ═══════════════════════════════════════════════════════════════════════

TEST(BitPackedArray, LargeArray) {
    constexpr uint32_t N = 100000;
    auto vals = generate_values(N, 10);
    BitPackedArray bpa(vals.data(), nullptr, N, 10);

    // Verify first, last, and random positions
    EXPECT_EQ(bpa.get(0), vals[0]);
    EXPECT_EQ(bpa.get(N - 1), vals[N - 1]);
    EXPECT_EQ(bpa.get(N / 2), vals[N / 2]);

    // Verify memory size is reasonable (N * 10 bits / 8 bytes)
    size_t expected_min = (static_cast<size_t>(N) * 10 + 7) / 8;
    EXPECT_GE(bpa.memory_size(), expected_min);
}

// ═══════════════════════════════════════════════════════════════════════
// Regression: get_bit_width(0) must return 1 for Rust compatibility.
// Rust uses NonZero<u8> with minimum 1.
// ═══════════════════════════════════════════════════════════════════════

TEST(BitPackedUtils, GetBitWidthZero) {
    EXPECT_EQ(get_bit_width(0), 1);
}

TEST(BitPackedUtils, GetBitWidthPowersOfTwo) {
    EXPECT_EQ(get_bit_width(1), 1);
    EXPECT_EQ(get_bit_width(255), 8);
    EXPECT_EQ(get_bit_width(256), 9);
    EXPECT_EQ(get_bit_width(UINT64_MAX), 64);
}
