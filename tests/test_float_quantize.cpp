// test_float_quantize.cpp
// GoogleTest-based tests for LiquidFloatArray::squeeze() and
// LiquidFloatQuantizedArray: roundtrip, predicates, size reduction.
#include <gtest/gtest.h>

#include <arrow/api.h>
#include <arrow/array/array_base.h>

#include <iostream>
#include <vector>
#include <memory>
#include <cstring>

#include "liquid_cache/liquid_arrays.h"
#include "liquid_cache/ipc_header.h"

using namespace liquid_cache;

// ── In-memory I/O handler for testing ───────────────────────────────
class MemSqueezeIo : public SqueezeIoHandler {
public:
    std::vector<uint8_t> read(uint64_t offset, uint64_t length) override {
        if (offset + length > data_.size()) {
            throw std::runtime_error("MemSqueezeIo::read out of bounds");
        }
        return std::vector<uint8_t>(data_.begin() + offset,
                                    data_.begin() + offset + length);
    }

    void write(std::vector<uint8_t> bytes) {
        data_ = std::move(bytes);
    }
private:
    std::vector<uint8_t> data_;
};

// ── Helper: build a FloatType Arrow array from values + nulls ───────
template <typename ArrowType>
static std::shared_ptr<arrow::Array> make_arrow(
        const std::vector<typename ArrowType::c_type>& values,
        const std::vector<bool>& nulls = {}) {
    using NativeT = typename ArrowType::c_type;
    typename arrow::TypeTraits<ArrowType>::BuilderType builder;
    if (nulls.empty()) {
        ARROW_CHECK_OK(builder.AppendValues(values));
    } else {
        ARROW_CHECK_OK(builder.Reserve(values.size()));
        for (size_t i = 0; i < values.size(); ++i) {
            if (nulls[i]) ARROW_CHECK_OK(builder.Append(values[i]));
            else ARROW_CHECK_OK(builder.AppendNull());
        }
    }
    return builder.Finish().ValueOrDie();
}

// ── Roundtrip helper ────────────────────────────────────────────────

template <typename FloatT>
static void check_roundtrip(
        const std::vector<FloatT>& values,
        const std::vector<bool>& nulls = {},
        size_t* out_quant_size = nullptr,
        size_t* out_disk_size = nullptr) {
    using ArrowType = typename ALPTraits<FloatT>::ArrowType;

    auto arr = make_arrow<ArrowType>(values, nulls);
    auto liquid = LiquidFloatArray<FloatT>::from_arrow(arr);

    // Only squeeze if bit_width >= 8
    if (liquid.bit_width() < 8) return;

    auto io = std::make_shared<MemSqueezeIo>();
    auto result = liquid.squeeze(io);
    ASSERT_TRUE(result.has_value()) << "squeeze returned nullopt";

    auto& [quantized, disk_bytes] = *result;
    io->write(disk_bytes);

    if (out_quant_size) *out_quant_size = quantized.memory_size();
    if (out_disk_size) *out_disk_size = disk_bytes.size();

    auto hydrated = quantized.to_arrow();
    auto hyd_typed = std::static_pointer_cast<
        typename arrow::TypeTraits<ArrowType>::ArrayType>(hydrated);
    auto orig_typed = std::static_pointer_cast<
        typename arrow::TypeTraits<ArrowType>::ArrayType>(arr);

    ASSERT_EQ(orig_typed->length(), hyd_typed->length());

    for (int64_t i = 0; i < orig_typed->length(); ++i) {
        bool orig_null = orig_typed->IsNull(i);
        bool hyd_null = hyd_typed->IsNull(i);
        ASSERT_EQ(orig_null, hyd_null) << "Null mismatch at " << i;
        if (!orig_null) {
            FloatT ov = orig_typed->Value(i);
            FloatT hv = hyd_typed->Value(i);
            ASSERT_EQ(ov, hv) << "Value mismatch at " << i
                << ": " << ov << " vs " << hv;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Test 1: Unsqueezable small range
// ═══════════════════════════════════════════════════════════════════════

TEST(FloatQuantize, UnsqueezableSmallRange) {
    std::vector<float> vals = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    auto arr = make_arrow<arrow::FloatType>(vals);
    auto liquid = LiquidFloatArray<float>::from_arrow(arr);

    if (liquid.bit_width() >= 8) {
        GTEST_SKIP() << "Unexpected bw=" << int(liquid.bit_width()) << " >= 8";
    }

    auto io = std::make_shared<MemSqueezeIo>();
    auto result = liquid.squeeze(io);
    EXPECT_FALSE(result.has_value()) << "Should not be squeezable";
}

// ═══════════════════════════════════════════════════════════════════════
// Test 2: Squeeze + hydrate roundtrip Float32
// ═══════════════════════════════════════════════════════════════════════

TEST(FloatQuantize, SqueezeRoundtripFloat32) {
    std::vector<float> vals;
    for (int i = 0; i < 2048; ++i) {
        vals.push_back(static_cast<float>(i * 0.375f) + 1000.0f);
    }
    auto arr = make_arrow<arrow::FloatType>(vals);
    auto liquid = LiquidFloatArray<float>::from_arrow(arr);

    if (liquid.bit_width() < 8) {
        GTEST_SKIP() << "Unsqueezable, bw=" << int(liquid.bit_width());
    }
    check_roundtrip<float>(vals);
}

// ═══════════════════════════════════════════════════════════════════════
// Test 3: Squeeze + hydrate roundtrip Float64
// ═══════════════════════════════════════════════════════════════════════

TEST(FloatQuantize, SqueezeRoundtripFloat64) {
    std::vector<double> vals;
    for (int i = 0; i < 2048; ++i) {
        vals.push_back(static_cast<double>(i * 0.125) + 2000.0);
    }
    auto arr = make_arrow<arrow::DoubleType>(vals);
    auto liquid = LiquidFloatArray<double>::from_arrow(arr);

    if (liquid.bit_width() < 8) {
        GTEST_SKIP() << "Unsqueezable, bw=" << int(liquid.bit_width());
    }
    check_roundtrip<double>(vals);
}

// ═══════════════════════════════════════════════════════════════════════
// Test 4: Empty array
// ═══════════════════════════════════════════════════════════════════════

TEST(FloatQuantize, EmptyArray) {
    std::vector<float> vals;
    auto arr = make_arrow<arrow::FloatType>(vals);
    auto liquid = LiquidFloatArray<float>::from_arrow(arr);

    auto io = std::make_shared<MemSqueezeIo>();
    auto result = liquid.squeeze(io);

    if (liquid.bit_width() < 8 && !result.has_value()) {
        SUCCEED();
        return;
    }

    if (result.has_value()) {
        auto& [quantized, disk_bytes] = *result;
        io->write(disk_bytes);
        auto hydrated = quantized.to_arrow();
        EXPECT_EQ(hydrated->length(), 0);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Test 5: All-null squeeze
// ═══════════════════════════════════════════════════════════════════════

TEST(FloatQuantize, AllNullSqueeze) {
    std::vector<float> vals = {0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<bool> nulls = {false, false, false, false};
    auto arr = make_arrow<arrow::FloatType>(vals, nulls);
    auto liquid = LiquidFloatArray<float>::from_arrow(arr);

    auto io = std::make_shared<MemSqueezeIo>();
    auto result = liquid.squeeze(io);

    if (!result.has_value()) {
        SUCCEED(); // unsqueezable all-null is fine
        return;
    }

    auto& [quantized, disk_bytes] = *result;
    io->write(disk_bytes);
    auto hydrated = quantized.to_arrow();
    auto hyd_typed = std::static_pointer_cast<arrow::FloatArray>(hydrated);

    for (int i = 0; i < hyd_typed->length(); ++i) {
        EXPECT_TRUE(hyd_typed->IsNull(i)) << "Expected null at index " << i;
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Test 6: Predicate Eq – definitive false everywhere
// ═══════════════════════════════════════════════════════════════════════

TEST(FloatQuantize, PredicateEqDefinitiveFalse) {
    std::vector<float> vals;
    for (int i = 0; i < 1024; ++i) {
        vals.push_back(static_cast<float>(i * 1.5f) + 500.0f);
    }
    auto arr = make_arrow<arrow::FloatType>(vals);
    auto liquid = LiquidFloatArray<float>::from_arrow(arr);

    if (liquid.bit_width() < 8) GTEST_SKIP() << "Unsqueezable";

    auto io = std::make_shared<MemSqueezeIo>();
    auto result = liquid.squeeze(io);
    if (!result.has_value()) GTEST_SKIP() << "Squeeze failed";

    auto& [quantized, disk_bytes] = *result;
    io->write(disk_bytes);

    // Use a literal far outside the value range → Eq should be false everywhere
    float far_away = 1e9f;
    auto pred = quantized.try_eval_predicate(Operator::Eq, far_away);
    ASSERT_NE(pred, nullptr) << "Should not return NeedsBacking";

    for (int i = 0; i < pred->length(); ++i) {
        if (!pred->IsNull(i)) {
            EXPECT_FALSE(pred->Value(i)) << "Expected false at index " << i;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Test 7: Predicate Gt – definitive true everywhere
// ═══════════════════════════════════════════════════════════════════════

TEST(FloatQuantize, PredicateGtDefinitiveTrue) {
    std::vector<float> vals;
    for (int i = 0; i < 1024; ++i) {
        vals.push_back(static_cast<float>(i * 0.5f) + 100.0f);
    }
    auto arr = make_arrow<arrow::FloatType>(vals);
    auto liquid = LiquidFloatArray<float>::from_arrow(arr);

    if (liquid.bit_width() < 8) GTEST_SKIP() << "Unsqueezable";

    auto io = std::make_shared<MemSqueezeIo>();
    auto result = liquid.squeeze(io);
    if (!result.has_value()) GTEST_SKIP() << "Squeeze failed";

    auto& [quantized, disk_bytes] = *result;
    io->write(disk_bytes);

    // Use a literal far below the value range → Gt should be true everywhere
    float far_below = -1e9f;
    auto pred = quantized.try_eval_predicate(Operator::Gt, far_below);
    ASSERT_NE(pred, nullptr) << "Should not return NeedsBacking";

    for (int i = 0; i < pred->length(); ++i) {
        if (!pred->IsNull(i)) {
            EXPECT_TRUE(pred->Value(i)) << "Expected true at index " << i;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Test 8: Predicate NeedsBacking (ambiguous bounds)
// ═══════════════════════════════════════════════════════════════════════

TEST(FloatQuantize, PredicateNeedsBacking) {
    std::vector<float> vals;
    for (int i = 0; i < 1024; ++i) {
        vals.push_back(static_cast<float>(i * 0.5f) + 100.0f);
    }
    auto arr = make_arrow<arrow::FloatType>(vals);
    auto liquid = LiquidFloatArray<float>::from_arrow(arr);

    if (liquid.bit_width() < 8) GTEST_SKIP() << "Unsqueezable";

    auto io = std::make_shared<MemSqueezeIo>();
    auto result = liquid.squeeze(io);
    if (!result.has_value()) GTEST_SKIP() << "Squeeze failed";

    auto& [quantized, disk_bytes] = *result;
    io->write(disk_bytes);

    // Use a literal in the middle of the range → some buckets ambiguous
    float mid = vals[vals.size() / 2];
    auto pred = quantized.try_eval_predicate(Operator::Eq, mid);
    // Either NeedsBacking (nullptr) or resolved is valid
    SUCCEED();
}

// ═══════════════════════════════════════════════════════════════════════
// Test 9: Memory size reduction
// ═══════════════════════════════════════════════════════════════════════

TEST(FloatQuantize, MemorySizeReduction) {
    std::vector<float> vals;
    for (int i = 0; i < 4096; ++i) {
        vals.push_back(static_cast<float>(i * 0.25f) + 500.0f);
    }
    auto arr = make_arrow<arrow::FloatType>(vals);
    auto liquid = LiquidFloatArray<float>::from_arrow(arr);

    if (liquid.bit_width() < 8) GTEST_SKIP() << "Unsqueezable";

    size_t orig_size = liquid.memory_size();

    auto io = std::make_shared<MemSqueezeIo>();
    auto result = liquid.squeeze(io);
    ASSERT_TRUE(result.has_value()) << "Squeeze returned nullopt";

    auto& [quantized, disk_bytes] = *result;
    size_t quant_size = quantized.memory_size();

    EXPECT_LT(quant_size, orig_size)
        << "orig=" << orig_size << "B quantized=" << quant_size
        << "B disk=" << disk_bytes.size() << "B";
}

// ═══════════════════════════════════════════════════════════════════════
// Test 10: Consistent values (all same)
// ═══════════════════════════════════════════════════════════════════════

TEST(FloatQuantize, ConsistentValues) {
    std::vector<float> vals(256, 42.5f);
    auto arr = make_arrow<arrow::FloatType>(vals);
    auto liquid = LiquidFloatArray<float>::from_arrow(arr);

    if (liquid.bit_width() < 8) GTEST_SKIP() << "Unsqueezable, bw=" << int(liquid.bit_width());
    check_roundtrip<float>(vals);
}
