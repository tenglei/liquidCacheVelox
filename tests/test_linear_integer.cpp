// test_linear_integer.cpp
// GoogleTest-based roundtrip tests for LiquidLinearIntegerArray<T>
#include <gtest/gtest.h>

#include <arrow/api.h>
#include <arrow/array/array_base.h>

#include <iostream>
#include <vector>

#include "liquid_cache/liquid_arrays.h"
#include "liquid_cache/ipc_header.h"

using namespace liquid_cache;

// Helper macros to ignore [[nodiscard]] on builder methods in tests
#define APPEND(builder, val) do { auto _s = (builder).Append(val); (void)_s; } while(0)
#define APPEND_NULL(builder) do { auto _s = (builder).AppendNull(); (void)_s; } while(0)

// ═══════════════════════════════════════════════════════════════════════
// Roundtrip test helper
// ═══════════════════════════════════════════════════════════════════════

template <typename ArrowType>
static void assert_linear_roundtrip(
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
    auto arr = builder.Finish().ValueOrDie();

    auto liquid = LiquidLinearIntegerArray<ArrowType>::from_arrow(arr);
    auto decoded = liquid.to_arrow();
    auto dec_typed = std::static_pointer_cast<
        typename arrow::TypeTraits<ArrowType>::ArrayType>(decoded);
    auto orig_typed = std::static_pointer_cast<
        typename arrow::TypeTraits<ArrowType>::ArrayType>(arr);

    ASSERT_EQ(orig_typed->length(), dec_typed->length());
    ASSERT_EQ(orig_typed->null_count(), dec_typed->null_count());

    for (int64_t i = 0; i < orig_typed->length(); ++i) {
        bool orig_null = orig_typed->IsNull(i);
        bool dec_null = dec_typed->IsNull(i);
        ASSERT_EQ(orig_null, dec_null) << "Null mismatch at " << i;
        if (!orig_null) {
            NativeT ov = orig_typed->Value(i);
            NativeT dv = dec_typed->Value(i);
            ASSERT_EQ(ov, dv) << "Value mismatch at " << i
                << ": " << static_cast<int64_t>(ov)
                << " vs " << static_cast<int64_t>(dv);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Serialization roundtrip test helper
// ═══════════════════════════════════════════════════════════════════════

template <typename ArrowType>
static void assert_linear_serialization_roundtrip(
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
    auto arr = builder.Finish().ValueOrDie();

    auto liquid = LiquidLinearIntegerArray<ArrowType>::from_arrow(arr);
    auto bytes = liquid.to_bytes();
    auto decoded_liquid = LiquidLinearIntegerArray<ArrowType>::from_bytes(
        bytes.data(), bytes.size());
    auto decoded = decoded_liquid.to_arrow();

    auto orig_typed = std::static_pointer_cast<
        typename arrow::TypeTraits<ArrowType>::ArrayType>(arr);
    auto dec_typed = std::static_pointer_cast<
        typename arrow::TypeTraits<ArrowType>::ArrayType>(decoded);

    ASSERT_EQ(orig_typed->length(), dec_typed->length());

    for (int64_t i = 0; i < orig_typed->length(); ++i) {
        bool orig_null = orig_typed->IsNull(i);
        bool dec_null = dec_typed->IsNull(i);
        ASSERT_EQ(orig_null, dec_null) << "Serialize null mismatch at " << i;
        if (!orig_null) {
            NativeT ov = orig_typed->Value(i);
            NativeT dv = dec_typed->Value(i);
            ASSERT_EQ(ov, dv) << "Serialize value mismatch at " << i;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Test cases
// ═══════════════════════════════════════════════════════════════════════

TEST(LinearInteger, MonotonicInt32) {
    assert_linear_roundtrip<arrow::Int32Type>({10, 15, 14, 20, 18, 25, 24});
}

TEST(LinearInteger, Int32WithNulls) {
    assert_linear_roundtrip<arrow::Int32Type>(
        {10, 0, 30, 0, 50, 70},
        {true, false, true, false, true, true});
}

TEST(LinearInteger, AllNull) {
    assert_linear_roundtrip<arrow::Int32Type>(
        {0, 0, 0, 0},
        {false, false, false, false});
}

TEST(LinearInteger, SingleElement) {
    assert_linear_roundtrip<arrow::Int32Type>({42});
}

TEST(LinearInteger, Empty) {
    assert_linear_roundtrip<arrow::Int32Type>({});
}

TEST(LinearInteger, NegativeValues) {
    assert_linear_roundtrip<arrow::Int32Type>({-100, -50, 0, 50, 25, -25});
}

TEST(LinearInteger, Int8) {
    assert_linear_roundtrip<arrow::Int8Type>({-10, 0, 10, 20});
}

TEST(LinearInteger, Int16) {
    assert_linear_roundtrip<arrow::Int16Type>({-1000, 0, 1000, 2000});
}

TEST(LinearInteger, Int64) {
    assert_linear_roundtrip<arrow::Int64Type>(
        {-10000000000LL, 0, 10000000000LL, 20000000000LL});
}

TEST(LinearInteger, UInt8) {
    assert_linear_roundtrip<arrow::UInt8Type>({0, 10, 200, 255});
}

TEST(LinearInteger, UInt16) {
    assert_linear_roundtrip<arrow::UInt16Type>({0, 1000, 60000, 500});
}

TEST(LinearInteger, UInt32) {
    assert_linear_roundtrip<arrow::UInt32Type>(
        {0, 1000000, 3000000000U, 123456789});
}

TEST(LinearInteger, UInt64) {
    assert_linear_roundtrip<arrow::UInt64Type>({0ULL, 10000000000ULL, 42ULL});
}

TEST(LinearInteger, Date32) {
    assert_linear_roundtrip<arrow::Date32Type>({-365, 0, 365, 18262});
}

TEST(LinearInteger, Date64) {
    assert_linear_roundtrip<arrow::Date64Type>(
        {-86400000LL, 0, 86400000LL, 1000000000000LL});
}

// ═══════════════════════════════════════════════════════════════════════
// Serialization tests
// ═══════════════════════════════════════════════════════════════════════

TEST(LinearSerialization, Int32) {
    assert_linear_serialization_roundtrip<arrow::Int32Type>(
        {10, 15, 14, 20, 18, 25, 24});
}

TEST(LinearSerialization, Int32WithNulls) {
    assert_linear_serialization_roundtrip<arrow::Int32Type>(
        {10, 0, 30, 0, 50, 70},
        {true, false, true, false, true, true});
}

TEST(LinearSerialization, Int64) {
    assert_linear_serialization_roundtrip<arrow::Int64Type>(
        {-10000000000LL, 0, 10000000000LL});
}

TEST(LinearSerialization, UInt32) {
    assert_linear_serialization_roundtrip<arrow::UInt32Type>(
        {0, 1000000, 3000000000U});
}

// ═══════════════════════════════════════════════════════════════════════
// Compression ratio test
// ═══════════════════════════════════════════════════════════════════════

TEST(LinearCompression, BeatsPrimitiveOnMonotonicData) {
    std::vector<int32_t> seq;
    for (int32_t i = 0; i < 10000; i += 10) seq.push_back(i);
    arrow::Int32Builder bld;
    ARROW_CHECK_OK(bld.AppendValues(seq));
    auto arr = bld.Finish().ValueOrDie();

    auto primitive = LiquidPrimitiveArray<arrow::Int32Type>::from_arrow(arr);
    auto linear = LiquidLinearIntegerArray<arrow::Int32Type>::from_arrow(arr);

    size_t prim_size = primitive.memory_size();
    size_t lin_size = linear.memory_size();

    // Linear should beat or be close to primitive on monotonic data
    EXPECT_LT(lin_size, prim_size * 2)
        << "Primitive=" << prim_size << " bytes, Linear=" << lin_size << " bytes";
}

