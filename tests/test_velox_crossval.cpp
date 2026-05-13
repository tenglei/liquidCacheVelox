// tests/test_velox_crossval.cpp
// Cross-validation tests: Arrow → Liquid → Velox vs Arrow direct values.
// Only compiled when LIQUID_ENABLE_VELOX is defined.
#include <gtest/gtest.h>

#include <arrow/api.h>
#if ARROW_VERSION_MAJOR >= 19
#include <arrow/compute/initialize.h>
#endif

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "velox/common/memory/Memory.h"
#include "velox/vector/FlatVector.h"
#include "velox/type/HugeInt.h"
#include "velox/type/Timestamp.h"

#include "liquid_cache/liquid_arrays.h"
#include "liquid_cache/liquid_byte_view_array.h"
#include "liquid_cache/liquid_decimal_array.h"
#include "liquid_cache/liquid_fixed_len_byte_array.h"

using namespace liquid_cache;
using namespace facebook::velox;

// Helper macros to ignore [[nodiscard]] on builder methods in tests
#define APPEND(builder, val) do { auto _s = (builder).Append(val); (void)_s; } while(0)
#define APPEND_NULL(builder) do { auto _s = (builder).AppendNull(); (void)_s; } while(0)

// Shared memory pool for all tests - initialized in main()
static memory::MemoryPool* g_pool = nullptr;
static memory::MemoryPool* test_pool() { return g_pool; }

// ═══════════════════════════════════════════════════════════════════════
// Integer type cross-validation: Arrow → Liquid → Velox
// ═══════════════════════════════════════════════════════════════════════

template <typename ArrowType, typename VeloxT = typename ArrowType::c_type>
void test_primitive_velox(const std::vector<typename ArrowType::c_type>& values) {
    using BuilderT = typename arrow::TypeTraits<ArrowType>::BuilderType;
    BuilderT builder;
    for (auto v : values) {
        APPEND(builder, v);
    }
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<ArrowType>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->template asFlatVector<VeloxT>();
    ASSERT_EQ(flat->size(), static_cast<vector_size_t>(values.size()));

    for (size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(flat->valueAt(i), values[i])
            << "Mismatch at index " << i << " for type " << array->type()->ToString();
    }
}

TEST(VeloxCrossVal, Int8) {
    test_primitive_velox<arrow::Int8Type>({0, 1, -1, 127, -128, 42, -42});
}
TEST(VeloxCrossVal, Int16) {
    test_primitive_velox<arrow::Int16Type>({0, 1000, -1000, 32767, -32768});
}
TEST(VeloxCrossVal, Int32) {
    test_primitive_velox<arrow::Int32Type>({0, 100000, -100000, INT32_MAX, INT32_MIN});
}
TEST(VeloxCrossVal, Int64) {
    test_primitive_velox<arrow::Int64Type>({0, 1000000000LL, -1000000000LL, INT64_MAX, INT64_MIN});
}
TEST(VeloxCrossVal, UInt8) {
    test_primitive_velox<arrow::UInt8Type, uint8_t>({0, 1, 127, 255, 42});
}
TEST(VeloxCrossVal, UInt16) {
    test_primitive_velox<arrow::UInt16Type, uint16_t>({0, 1000, 65535, 30000});
}
TEST(VeloxCrossVal, UInt32) {
    test_primitive_velox<arrow::UInt32Type, uint32_t>({0, 100000, UINT32_MAX});
}
TEST(VeloxCrossVal, UInt64) {
    test_primitive_velox<arrow::UInt64Type, uint64_t>({0, 1000000000ULL, UINT64_MAX});
}

// ═══════════════════════════════════════════════════════════════════════
// Date type cross-validation
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, Date32) {
    arrow::Date32Builder builder;
    APPEND(builder, int32_t(0));
    APPEND(builder, int32_t(1));
    APPEND(builder, int32_t(365));
    APPEND(builder, int32_t(19053));
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::Date32Type>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<int32_t>();
    ASSERT_EQ(flat->size(), 4);
    EXPECT_EQ(flat->valueAt(0), 0);
    EXPECT_EQ(flat->valueAt(1), 1);
    EXPECT_EQ(flat->valueAt(2), 365);
    EXPECT_EQ(flat->valueAt(3), 19053);
}

TEST(VeloxCrossVal, Date64) {
    // Date64 stores ms since epoch, Velox DATE is days since epoch
    arrow::Date64Builder builder;
    APPEND(builder, int64_t(0));
    APPEND(builder, int64_t(86400000LL));       // 1 day
    APPEND(builder, int64_t(1648000000000LL));  // ~19053 days
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::Date64Type>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<int32_t>();
    ASSERT_EQ(flat->size(), 3);
    EXPECT_EQ(flat->valueAt(0), 0);
    EXPECT_EQ(flat->valueAt(1), 1);
    EXPECT_EQ(flat->valueAt(2), static_cast<int32_t>(1648000000000LL / 86400000LL));
}

// ═══════════════════════════════════════════════════════════════════════
// Null coverage for types missing it
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, Int8WithNulls) {
    arrow::Int8Builder b;
    APPEND(b, 0); APPEND_NULL(b); APPEND(b, 127); APPEND_NULL(b); APPEND(b, -128);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidPrimitiveArray<arrow::Int8Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<int8_t>();
    ASSERT_EQ(flat->size(), 5);
    EXPECT_FALSE(flat->isNullAt(0)); EXPECT_EQ(flat->valueAt(0), 0);
    EXPECT_TRUE(flat->isNullAt(1));
    EXPECT_FALSE(flat->isNullAt(2)); EXPECT_EQ(flat->valueAt(2), 127);
    EXPECT_TRUE(flat->isNullAt(3));
    EXPECT_FALSE(flat->isNullAt(4)); EXPECT_EQ(flat->valueAt(4), -128);
}

TEST(VeloxCrossVal, Int16WithNulls) {
    arrow::Int16Builder b;
    APPEND(b, 0); APPEND_NULL(b); APPEND(b, 32767); APPEND_NULL(b); APPEND(b, -32768);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidPrimitiveArray<arrow::Int16Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<int16_t>();
    EXPECT_FALSE(flat->isNullAt(0)); EXPECT_TRUE(flat->isNullAt(1));
    EXPECT_FALSE(flat->isNullAt(2)); EXPECT_TRUE(flat->isNullAt(3));
}

TEST(VeloxCrossVal, UInt8WithNulls) {
    arrow::UInt8Builder b;
    APPEND(b, 0); APPEND_NULL(b); APPEND(b, 255); APPEND_NULL(b); APPEND(b, 128);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidPrimitiveArray<arrow::UInt8Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<uint8_t>();
    EXPECT_FALSE(flat->isNullAt(0)); EXPECT_TRUE(flat->isNullAt(1));
}

TEST(VeloxCrossVal, UInt16WithNulls) {
    arrow::UInt16Builder b;
    APPEND(b, 0); APPEND_NULL(b); APPEND(b, 65535);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidPrimitiveArray<arrow::UInt16Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<uint16_t>();
    EXPECT_FALSE(flat->isNullAt(0)); EXPECT_TRUE(flat->isNullAt(1));
}

TEST(VeloxCrossVal, UInt32WithNulls) {
    arrow::UInt32Builder b;
    APPEND(b, 0); APPEND_NULL(b); APPEND(b, UINT32_MAX);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidPrimitiveArray<arrow::UInt32Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<uint32_t>();
    EXPECT_FALSE(flat->isNullAt(0)); EXPECT_TRUE(flat->isNullAt(1));
}

TEST(VeloxCrossVal, UInt64WithNulls) {
    arrow::UInt64Builder b;
    APPEND(b, 0); APPEND_NULL(b); APPEND(b, UINT64_MAX);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidPrimitiveArray<arrow::UInt64Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<uint64_t>();
    EXPECT_FALSE(flat->isNullAt(0)); EXPECT_TRUE(flat->isNullAt(1));
}

TEST(VeloxCrossVal, Date32WithNulls) {
    arrow::Date32Builder b;
    APPEND(b, 0); APPEND_NULL(b); APPEND(b, 19053);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidPrimitiveArray<arrow::Date32Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<int32_t>();
    EXPECT_FALSE(flat->isNullAt(0)); EXPECT_TRUE(flat->isNullAt(1));
}

TEST(VeloxCrossVal, Date64WithNulls) {
    arrow::Date64Builder b;
    APPEND(b, 0LL); APPEND_NULL(b); APPEND(b, 1648000000000LL);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidPrimitiveArray<arrow::Date64Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<int32_t>();
    EXPECT_FALSE(flat->isNullAt(0)); EXPECT_TRUE(flat->isNullAt(1));
}

TEST(VeloxCrossVal, BinaryWithNulls) {
    arrow::BinaryBuilder b;
    APPEND(b, "hello"); APPEND_NULL(b); APPEND(b, "world");
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidByteViewArray::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<StringView>();
    EXPECT_FALSE(flat->isNullAt(0)); EXPECT_TRUE(flat->isNullAt(1));
}

TEST(VeloxCrossVal, LinearInt64WithNulls) {
    arrow::Int64Builder b;
    APPEND(b, 100LL); APPEND_NULL(b); APPEND(b, 200LL); APPEND_NULL(b); APPEND(b, 300LL);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidLinearIntegerArray<arrow::Int64Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<int64_t>();
    EXPECT_FALSE(flat->isNullAt(0)); EXPECT_TRUE(flat->isNullAt(1));
    EXPECT_FALSE(flat->isNullAt(2)); EXPECT_TRUE(flat->isNullAt(3));
}

// ═══════════════════════════════════════════════════════════════════════
// Timestamp cross-validation — all four time units
// Verifies that LiquidPrimitiveArray<Int64Type>::to_velox() correctly
// detects the stored TimestampType and produces TIMESTAMP (not BIGINT).
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, TimestampSecond) {
    auto type = arrow::timestamp(arrow::TimeUnit::SECOND);
    arrow::TimestampBuilder builder(type, arrow::default_memory_pool());
    APPEND(builder, int64_t(0));
    APPEND(builder, int64_t(1));
    APPEND(builder, int64_t(1000));
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::TimestampType>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<Timestamp>();
    ASSERT_EQ(flat->size(), 3);
    EXPECT_EQ(flat->valueAt(0).getSeconds(), 0);
    EXPECT_EQ(flat->valueAt(0).getNanos(), 0);
    EXPECT_EQ(flat->valueAt(1).getSeconds(), 1);
    EXPECT_EQ(flat->valueAt(2).getSeconds(), 1000);
}

TEST(VeloxCrossVal, TimestampMillisecond) {
    auto type = arrow::timestamp(arrow::TimeUnit::MILLI);
    arrow::TimestampBuilder builder(type, arrow::default_memory_pool());
    APPEND(builder, int64_t(0));
    APPEND(builder, int64_t(1000));
    APPEND(builder, int64_t(86400000));  // 1 day in ms
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::TimestampType>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<Timestamp>();
    ASSERT_EQ(flat->size(), 3);
    EXPECT_EQ(flat->valueAt(0).toMillis(), 0);
    EXPECT_EQ(flat->valueAt(1).toMillis(), 1000);
    EXPECT_EQ(flat->valueAt(2).toMillis(), 86400000);
}

TEST(VeloxCrossVal, TimestampMicrosecond) {
    auto type = arrow::timestamp(arrow::TimeUnit::MICRO);
    arrow::TimestampBuilder builder(type, arrow::default_memory_pool());
    APPEND(builder, int64_t(0));
    APPEND(builder, int64_t(1000000));
    APPEND(builder, int64_t(1000000000));
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::TimestampType>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<Timestamp>();
    ASSERT_EQ(flat->size(), 3);
    EXPECT_EQ(flat->valueAt(0).getSeconds(), 0);
    EXPECT_EQ(flat->valueAt(1).getSeconds(), 1);
    EXPECT_EQ(flat->valueAt(2).getSeconds(), 1000);
}

TEST(VeloxCrossVal, TimestampNanosecond) {
    auto type = arrow::timestamp(arrow::TimeUnit::NANO);
    arrow::TimestampBuilder builder(type, arrow::default_memory_pool());
    APPEND(builder, int64_t(0));
    APPEND(builder, int64_t(1000000000));  // 1 second in ns
    APPEND(builder, int64_t(60000000000)); // 60 seconds in ns
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::TimestampType>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<Timestamp>();
    ASSERT_EQ(flat->size(), 3);
    EXPECT_EQ(flat->valueAt(0).toNanos(), 0);
    EXPECT_EQ(flat->valueAt(1).toNanos(), 1000000000);
    EXPECT_EQ(flat->valueAt(2).toNanos(), 60000000000);
}

TEST(VeloxCrossVal, TimestampWithNulls) {
    auto type = arrow::timestamp(arrow::TimeUnit::MICRO);
    arrow::TimestampBuilder builder(type, arrow::default_memory_pool());
    APPEND(builder, int64_t(1000000));
    APPEND_NULL(builder);
    APPEND(builder, int64_t(3000000));
    APPEND_NULL(builder);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::TimestampType>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<Timestamp>();
    ASSERT_EQ(flat->size(), 4);
    EXPECT_EQ(flat->valueAt(0).getSeconds(), 1);
    EXPECT_TRUE(flat->isNullAt(1));
    EXPECT_EQ(flat->valueAt(2).getSeconds(), 3);
    EXPECT_TRUE(flat->isNullAt(3));
}

// ═══════════════════════════════════════════════════════════════════════
// LinearInteger Velox cross-validation (was completely missing)
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, LinearInt32) {
    arrow::Int32Builder builder;
    // Monotonic sequence: 10, 15, 14, 20, 18, 25, 24
    std::vector<int32_t> vals = {10, 15, 14, 20, 18, 25, 24};
    for (auto v : vals) APPEND(builder, v);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidLinearIntegerArray<arrow::Int32Type>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<int32_t>();
    ASSERT_EQ(flat->size(), static_cast<vector_size_t>(vals.size()));
    for (size_t i = 0; i < vals.size(); ++i) {
        EXPECT_EQ(flat->valueAt(i), vals[i])
            << "LinearInt32 mismatch at index " << i;
    }
}

TEST(VeloxCrossVal, LinearInt64) {
    arrow::Int64Builder builder;
    std::vector<int64_t> vals = {-1000LL, 0LL, 1000LL, 2000LL, 3000LL};
    for (auto v : vals) APPEND(builder, v);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidLinearIntegerArray<arrow::Int64Type>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<int64_t>();
    ASSERT_EQ(flat->size(), static_cast<vector_size_t>(vals.size()));
    for (size_t i = 0; i < vals.size(); ++i) {
        EXPECT_EQ(flat->valueAt(i), vals[i])
            << "LinearInt64 mismatch at index " << i;
    }
}

TEST(VeloxCrossVal, LinearInt32WithNulls) {
    arrow::Int32Builder builder;
    APPEND(builder, 10);
    APPEND_NULL(builder);
    APPEND(builder, 30);
    APPEND_NULL(builder);
    APPEND(builder, 50);
    APPEND(builder, 70);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidLinearIntegerArray<arrow::Int32Type>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<int32_t>();
    ASSERT_EQ(flat->size(), 6);
    EXPECT_EQ(flat->valueAt(0), 10);
    EXPECT_TRUE(flat->isNullAt(1));
    EXPECT_EQ(flat->valueAt(2), 30);
    EXPECT_TRUE(flat->isNullAt(3));
    EXPECT_EQ(flat->valueAt(4), 50);
    EXPECT_EQ(flat->valueAt(5), 70);
}

// ═══════════════════════════════════════════════════════════════════════
// LinearInteger Velox tests for all 8 remaining specializations
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, LinearInt8) {
    arrow::Int8Builder b;
    for (int8_t v = 0; v < 50; ++v) APPEND(b, v);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidLinearIntegerArray<arrow::Int8Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<int8_t>();
    ASSERT_EQ(flat->size(), 50);
    EXPECT_EQ(flat->valueAt(0), 0);
    EXPECT_EQ(flat->valueAt(49), 49);
}

TEST(VeloxCrossVal, LinearInt16) {
    arrow::Int16Builder b;
    for (int16_t v = 100; v < 150; ++v) APPEND(b, v);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidLinearIntegerArray<arrow::Int16Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<int16_t>();
    ASSERT_EQ(flat->size(), 50);
    EXPECT_EQ(flat->valueAt(0), 100);
    EXPECT_EQ(flat->valueAt(49), 149);
}

TEST(VeloxCrossVal, LinearUInt8) {
    arrow::UInt8Builder b;
    for (uint8_t v = 10; v < 60; ++v) APPEND(b, v);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidLinearIntegerArray<arrow::UInt8Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<uint8_t>();
    ASSERT_EQ(flat->size(), 50);
}

TEST(VeloxCrossVal, LinearUInt16) {
    arrow::UInt16Builder b;
    for (uint16_t v = 1000; v < 1050; ++v) APPEND(b, v);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidLinearIntegerArray<arrow::UInt16Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<uint16_t>();
    ASSERT_EQ(flat->size(), 50);
}

TEST(VeloxCrossVal, LinearUInt32) {
    arrow::UInt32Builder b;
    for (uint32_t v = 0; v < 50; ++v) APPEND(b, v + 100000);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidLinearIntegerArray<arrow::UInt32Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<uint32_t>();
    ASSERT_EQ(flat->size(), 50);
}

TEST(VeloxCrossVal, LinearUInt64) {
    arrow::UInt64Builder b;
    for (uint64_t v = 0; v < 50; ++v) APPEND(b, v + 10000000000ULL);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidLinearIntegerArray<arrow::UInt64Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<uint64_t>();
    ASSERT_EQ(flat->size(), 50);
}

TEST(VeloxCrossVal, LinearDate32) {
    arrow::Date32Builder b;
    for (int32_t v = 19000; v < 19050; ++v) APPEND(b, v);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidLinearIntegerArray<arrow::Date32Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<int32_t>();
    ASSERT_EQ(flat->size(), 50);
}

TEST(VeloxCrossVal, LinearDate64) {
    arrow::Date64Builder b;
    for (int64_t v = 0; v < 50; ++v) APPEND(b, v * 86400000LL + 1648000000000LL);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidLinearIntegerArray<arrow::Date64Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<int32_t>();
    ASSERT_EQ(flat->size(), 50);
}

// ═══════════════════════════════════════════════════════════════════════
// Float type cross-validation
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, Float32) {
    arrow::FloatBuilder builder;
    APPEND(builder, 0.0f);
    APPEND(builder, 1.5f);
    APPEND(builder, -3.14f);
    APPEND(builder, 100.0f);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidFloatArray<float>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<float>();
    ASSERT_EQ(flat->size(), 4);
    EXPECT_FLOAT_EQ(flat->valueAt(0), 0.0f);
    EXPECT_FLOAT_EQ(flat->valueAt(1), 1.5f);
    EXPECT_FLOAT_EQ(flat->valueAt(2), -3.14f);
    EXPECT_FLOAT_EQ(flat->valueAt(3), 100.0f);
}

TEST(VeloxCrossVal, Float64) {
    arrow::DoubleBuilder builder;
    APPEND(builder, 0.0);
    APPEND(builder, 1.5);
    APPEND(builder, -3.14);
    APPEND(builder, 100.0);
    APPEND(builder, 1e15);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidFloatArray<double>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<double>();
    ASSERT_EQ(flat->size(), 5);
    EXPECT_DOUBLE_EQ(flat->valueAt(0), 0.0);
    EXPECT_DOUBLE_EQ(flat->valueAt(1), 1.5);
    EXPECT_DOUBLE_EQ(flat->valueAt(2), -3.14);
    EXPECT_DOUBLE_EQ(flat->valueAt(3), 100.0);
    EXPECT_DOUBLE_EQ(flat->valueAt(4), 1e15);
}

// ═══════════════════════════════════════════════════════════════════════
// String/Binary cross-validation
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, String) {
    arrow::StringBuilder builder;
    APPEND(builder, "hello");
    APPEND(builder, "world");
    APPEND(builder, "");
    APPEND(builder, "a longer string for testing");
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidByteViewArray::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<StringView>();
    ASSERT_EQ(flat->size(), 4);
    EXPECT_EQ(flat->valueAt(0), StringView("hello"));
    EXPECT_EQ(flat->valueAt(1), StringView("world"));
    EXPECT_EQ(flat->valueAt(2), StringView(""));
    EXPECT_EQ(flat->valueAt(3), StringView("a longer string for testing"));
}

TEST(VeloxCrossVal, StringWithNulls) {
    arrow::StringBuilder builder;
    APPEND(builder, "first");
    APPEND_NULL(builder);
    APPEND(builder, "third");
    APPEND_NULL(builder);
    APPEND(builder, "fifth");
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidByteViewArray::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<StringView>();
    ASSERT_EQ(flat->size(), 5);
    EXPECT_EQ(flat->valueAt(0), StringView("first"));
    EXPECT_TRUE(flat->isNullAt(1));
    EXPECT_EQ(flat->valueAt(2), StringView("third"));
    EXPECT_TRUE(flat->isNullAt(3));
    EXPECT_EQ(flat->valueAt(4), StringView("fifth"));
}

TEST(VeloxCrossVal, Binary) {
    arrow::BinaryBuilder builder;
    APPEND(builder, "\x00\x01\x02");
    APPEND(builder, "text");
    APPEND(builder, "");
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidByteViewArray::from_arrow(array);

    // First verify Arrow roundtrip is correct
    auto decoded = liquid.to_arrow();
    auto bin_decoded = std::static_pointer_cast<arrow::BinaryArray>(decoded);
    ASSERT_EQ(bin_decoded->length(), 3);

    // Now test Velox conversion
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<StringView>();
    ASSERT_EQ(flat->size(), 3);

    // Compare Velox values with decoded Arrow values
    for (int i = 0; i < 3; ++i) {
        auto arrow_view = bin_decoded->GetView(i);
        auto velox_val = flat->valueAt(i);
        ASSERT_EQ(velox_val.size(), static_cast<size_t>(arrow_view.size()))
            << "Size mismatch at index " << i;
        if (arrow_view.size() > 0) {
            ASSERT_EQ(0, std::memcmp(velox_val.data(), arrow_view.data(), arrow_view.size()))
                << "Data mismatch at index " << i;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Decimal128 cross-validation (fits-u64 path)
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, Decimal128FitsU64) {
    auto type = arrow::decimal128(10, 2);
    arrow::Decimal128Builder builder(type);
    std::vector<int64_t> expected_vals;
    for (int i = 0; i < 100; ++i) {
        auto val = arrow::Decimal128(i * 100);
        APPEND(builder, val);
        // Extract low 8 bytes as the raw int64 value
        int64_t raw = 0;
#if ARROW_VERSION_MAJOR >= 19
        auto native_val = val.ToBytes().ValueOrDie();
#else
        auto native_val = val.ToBytes();
#endif
        std::memcpy(&raw, native_val.data(), sizeof(int64_t));
        expected_vals.push_back(raw);
    }
    auto array = builder.Finish().ValueOrDie();

    ASSERT_TRUE(LiquidDecimalArray::fits_u64(array));
    auto liquid = LiquidDecimalArray::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<int64_t>();
    ASSERT_EQ(flat->size(), 100);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(flat->valueAt(i), expected_vals[i])
            << "Mismatch at index " << i;
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Decimal128 cross-validation (FSST dictionary path)
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, Decimal128LargeValues) {
    auto type = arrow::decimal128(38, 6);
    arrow::Decimal128Builder builder(type);
    std::vector<int128_t> expected_vals;
    for (int i = 0; i < 30; ++i) {
        auto val_str = std::to_string(i) + std::string(20, '0');
        auto val = arrow::Decimal128(val_str);
        APPEND(builder, val);
        // Extract 16 bytes as the raw int128 value
        int128_t raw = 0;
#if ARROW_VERSION_MAJOR >= 19
        auto native_val = val.ToBytes().ValueOrDie();
#else
        auto native_val = val.ToBytes();
#endif
        std::memcpy(&raw, native_val.data(), std::min(native_val.size(), sizeof(int128_t)));
        expected_vals.push_back(raw);
    }
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidFixedLenByteArray::from_decimal128(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    // precision 38 > 18 → LongDecimal (int128_t)
    auto flat = vec->asFlatVector<int128_t>();
    ASSERT_EQ(flat->size(), 30);
    for (int i = 0; i < 30; ++i) {
        EXPECT_EQ(flat->valueAt(i), expected_vals[i])
            << "Mismatch at index " << i;
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Null handling cross-validation
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, Int32WithNulls) {
    arrow::Int32Builder builder;
    APPEND(builder, 1);
    APPEND(builder, 2);
    APPEND_NULL(builder);
    APPEND(builder, 4);
    APPEND_NULL(builder);
    APPEND(builder, 6);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::Int32Type>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<int32_t>();
    ASSERT_EQ(flat->size(), 6);
    EXPECT_EQ(flat->valueAt(0), 1);
    EXPECT_EQ(flat->valueAt(1), 2);
    EXPECT_TRUE(flat->isNullAt(2));
    EXPECT_EQ(flat->valueAt(3), 4);
    EXPECT_TRUE(flat->isNullAt(4));
    EXPECT_EQ(flat->valueAt(5), 6);
}

TEST(VeloxCrossVal, Float64WithNulls) {
    arrow::DoubleBuilder builder;
    APPEND(builder, 1.1);
    APPEND_NULL(builder);
    APPEND(builder, 3.3);
    APPEND_NULL(builder);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidFloatArray<double>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<double>();
    ASSERT_EQ(flat->size(), 4);
    EXPECT_DOUBLE_EQ(flat->valueAt(0), 1.1);
    EXPECT_TRUE(flat->isNullAt(1));
    EXPECT_DOUBLE_EQ(flat->valueAt(2), 3.3);
    EXPECT_TRUE(flat->isNullAt(3));
}

// ═══════════════════════════════════════════════════════════════════════
// Edge case: all-null arrays, Date64, Float32-with-nulls
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, AllNullInt64) {
    arrow::Int64Builder builder;
    for (int i = 0; i < 10; ++i) APPEND_NULL(builder);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::Int64Type>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);
    ASSERT_EQ(vec->size(), 10);

    auto flat = vec->asFlatVector<int64_t>();
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(flat->isNullAt(i)) << "Expected null at index " << i;
    }
}

TEST(VeloxCrossVal, Date64Conversion) {
    // Date64 stores ms since epoch; Velox DATE is days since epoch (int32)
    arrow::Date64Builder builder;
    APPEND(builder, int64_t(0));
    APPEND(builder, int64_t(86400000LL));          // 1 day
    APPEND(builder, int64_t(1648000000000LL));     // ~19053 days
    APPEND(builder, int64_t(86400000LL * 365));    // 365 days
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::Date64Type>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<int32_t>();
    ASSERT_EQ(flat->size(), 4);
    EXPECT_EQ(flat->valueAt(0), 0);
    EXPECT_EQ(flat->valueAt(1), 1);
    EXPECT_EQ(flat->valueAt(2), static_cast<int32_t>(1648000000000LL / 86400000LL));
    EXPECT_EQ(flat->valueAt(3), 365);
}

TEST(VeloxCrossVal, Float32WithNulls) {
    arrow::FloatBuilder builder;
    APPEND(builder, 1.5f);
    APPEND_NULL(builder);
    APPEND(builder, -3.14f);
    APPEND_NULL(builder);
    APPEND(builder, 0.0f);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidFloatArray<float>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<float>();
    ASSERT_EQ(flat->size(), 5);
    EXPECT_FLOAT_EQ(flat->valueAt(0), 1.5f);
    EXPECT_TRUE(flat->isNullAt(1));
    EXPECT_FLOAT_EQ(flat->valueAt(2), -3.14f);
    EXPECT_TRUE(flat->isNullAt(3));
    EXPECT_FLOAT_EQ(flat->valueAt(4), 0.0f);
}

// ═══════════════════════════════════════════════════════════════════════
// Full pipeline test: Parquet → Liquid (in-memory) → Velox Vector
//
// This is the critical end-to-end test that validates the complete
// parquet → liquid cache → velox vector data path. Previously only
// individual Arrow→Liquid and Arrow→Liquid→Velox tests existed, but
// no test exercised the transcoder dispatch (transcode_to_liquid_array)
// followed by Velox conversion.
// ═══════════════════════════════════════════════════════════════════════

#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>
#include "liquid_cache/transcoder.h"
#include "liquid_cache/liquid_array.h"

TEST(FullPipeline, ParquetToLiquidToVelox) {
    // Look for the test Parquet file in multiple locations:
    //   1. TEST_PARQUET_PATH environment variable (absolute path)
    //   2. Relative to cwd (ctest runs from build/ directory)
    //   3. Relative to project root
    std::string parquet_path;
    const char* env_path = std::getenv("TEST_PARQUET_PATH");
    if (env_path) {
        parquet_path = env_path;
    } else {
        for (const auto& candidate : {
            "test_data_512mb.parquet",           // cwd = build/
            "build/test_data_512mb.parquet",     // cwd = project root
        }) {
            auto result = arrow::io::ReadableFile::Open(candidate);
            if (result.ok()) {
                parquet_path = candidate;
                break;
            }
        }
    }

    if (parquet_path.empty()) {
        GTEST_SKIP() << "Test Parquet file not found. "
                     << "Run scripts/generate_test_parquet.sh first, "
                     << "or set TEST_PARQUET_PATH env var.";
    }

    // Open the Parquet file
    auto infile_result = arrow::io::ReadableFile::Open(parquet_path);
    ASSERT_TRUE(infile_result.ok()) << "Failed to re-open: " << parquet_path;
    auto infile = infile_result.ValueOrDie();

    std::unique_ptr<parquet::arrow::FileReader> reader;
#if ARROW_VERSION_MAJOR >= 19
    auto open_result = parquet::arrow::OpenFile(infile, arrow::default_memory_pool());
    ASSERT_TRUE(open_result.ok()) << "Failed to open Parquet: " << open_result.status();
    reader = std::move(open_result).ValueOrDie();
#else
    auto open_status = parquet::arrow::OpenFile(
        infile, arrow::default_memory_pool(), &reader);
    ASSERT_TRUE(open_status.ok()) << "Failed to open Parquet: " << open_status;
#endif

    // Read first batch via RecordBatchReader
    reader->set_batch_size(1024);
    std::shared_ptr<arrow::RecordBatchReader> batch_reader;
#if ARROW_VERSION_MAJOR >= 19
    auto rb_result = reader->GetRecordBatchReader();
    ASSERT_TRUE(rb_result.ok()) << "GetRecordBatchReader failed";
    batch_reader = std::move(rb_result).ValueOrDie();
#else
    auto st = reader->GetRecordBatchReader(&batch_reader);
    ASSERT_TRUE(st.ok()) << "GetRecordBatchReader failed: " << st;
#endif
    std::shared_ptr<arrow::RecordBatch> batch;
    auto read_st = batch_reader->ReadNext(&batch);
    ASSERT_TRUE(read_st.ok() && batch) << "Failed to read batch: " << read_st;

    int num_cols = batch->num_columns();
    ASSERT_GT(num_cols, 0) << "Empty test Parquet file";

    int tested = 0;
    int skipped = 0;

    for (int c = 0; c < num_cols; ++c) {
        auto arrow_arr = batch->column(c);
        if (!arrow_arr || arrow_arr->length() == 0) continue;

        // Step 1: Transcode Arrow → Liquid (in-memory)
        auto liquid = transcode_to_liquid_array(arrow_arr);
        if (!liquid) {
            // unsupported type is acceptable
            skipped++;
            continue;
        }

        // Step 2: Convert Liquid → Velox
        auto vec = liquid->to_velox(test_pool());
        ASSERT_NE(vec, nullptr)
            << "to_velox returned null for column " << c
            << " type " << arrow_arr->type()->ToString();

        // Step 3: Verify Velox row count matches
        ASSERT_EQ(vec->size(), arrow_arr->length())
            << "Row count mismatch for column " << c;

        // Step 4: Verify null positions match
        for (int64_t i = 0; i < arrow_arr->length(); ++i) {
            bool arrow_null = arrow_arr->IsNull(i);
            bool velox_null = vec->isNullAt(i);
            ASSERT_EQ(arrow_null, velox_null)
                << "Null mismatch at column " << c << " row " << i;
        }

        // Step 5: Spot-check first non-null value for each column
        // (Per-type value correctness is exhaustively tested by
        //  the 30 VeloxCrossVal tests above.)
        for (int64_t i = 0; i < arrow_arr->length(); ++i) {
            if (arrow_arr->IsNull(i) || vec->isNullAt(i)) continue;
            auto scalar = arrow_arr->GetScalar(i).ValueOrDie();
            auto type_id = scalar->type->id();

            if (type_id == arrow::Type::INT32) {
                EXPECT_EQ(vec->asFlatVector<int32_t>()->valueAt(i),
                          std::static_pointer_cast<arrow::Int32Scalar>(scalar)->value);
            } else if (type_id == arrow::Type::INT64) {
                EXPECT_EQ(vec->asFlatVector<int64_t>()->valueAt(i),
                          std::static_pointer_cast<arrow::Int64Scalar>(scalar)->value);
            } else if (type_id == arrow::Type::FLOAT) {
                EXPECT_FLOAT_EQ(vec->asFlatVector<float>()->valueAt(i),
                          std::static_pointer_cast<arrow::FloatScalar>(scalar)->value);
            } else if (type_id == arrow::Type::DOUBLE) {
                EXPECT_DOUBLE_EQ(vec->asFlatVector<double>()->valueAt(i),
                          std::static_pointer_cast<arrow::DoubleScalar>(scalar)->value);
            }
            break;  // one spot-check per column is sufficient
        }

        tested++;
    }

    ASSERT_GT(tested, 0) << "No columns were tested (all unsupported?)";
    std::cout << "  FullPipeline: tested " << tested << " columns, "
              << skipped << " skipped\n";
}

// ═══════════════════════════════════════════════════════════════════════
// Large string/binary and view type Velox tests
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, LargeString) {
    arrow::LargeStringBuilder b;
    APPEND(b, "hello"); APPEND(b, "world"); APPEND(b, "large_string_test");
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidByteViewArray::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<StringView>();
    ASSERT_EQ(flat->size(), 3);
    EXPECT_EQ(std::string(flat->valueAt(0)), "hello");
    EXPECT_EQ(std::string(flat->valueAt(2)), "large_string_test");
}

TEST(VeloxCrossVal, LargeBinary) {
    arrow::LargeBinaryBuilder b;
    APPEND(b, "bin1"); APPEND(b, "binary_data");
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidByteViewArray::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<StringView>();
    ASSERT_EQ(flat->size(), 2);
    EXPECT_EQ(std::string(flat->valueAt(0)), "bin1");
}

TEST(VeloxCrossVal, StringView) {
    arrow::StringViewBuilder b;
    APPEND(b, "view1"); APPEND(b, "view2");
    auto arr = b.Finish().ValueOrDie();
    auto cast_arr = arrow::compute::Cast(*arr, arrow::utf8()).ValueOrDie();
    auto liquid = LiquidByteViewArray::from_arrow(cast_arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<StringView>();
    ASSERT_EQ(flat->size(), 2);
}

TEST(VeloxCrossVal, BinaryView) {
    arrow::BinaryViewBuilder b;
    APPEND(b, "bview1"); APPEND(b, "bview2");
    auto arr = b.Finish().ValueOrDie();
    auto cast_arr = arrow::compute::Cast(*arr, arrow::binary()).ValueOrDie();
    auto liquid = LiquidByteViewArray::from_arrow(cast_arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<StringView>();
    ASSERT_EQ(flat->size(), 2);
}

TEST(VeloxCrossVal, DictionaryString) {
    arrow::StringBuilder vb;
    APPEND(vb, "alpha"); APPEND(vb, "beta"); APPEND(vb, "gamma");
    auto dict_values = vb.Finish().ValueOrDie();
    arrow::UInt16Builder kb;
    APPEND(kb, 0); APPEND(kb, 1); APPEND(kb, 2); APPEND(kb, 0); APPEND(kb, 1);
    auto dict_keys = kb.Finish().ValueOrDie();
    auto dict_arr = arrow::DictionaryArray::FromArrays(
        arrow::dictionary(arrow::uint16(), arrow::utf8()),
        dict_keys, dict_values).ValueOrDie();
    auto dense = arrow::compute::Cast(*dict_arr, arrow::utf8()).ValueOrDie();
    auto liquid = LiquidByteViewArray::from_arrow(dense);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<StringView>();
    ASSERT_EQ(flat->size(), 5);
    EXPECT_EQ(std::string(flat->valueAt(0)), "alpha");
    EXPECT_EQ(std::string(flat->valueAt(3)), "alpha");
}

// ═══════════════════════════════════════════════════════════════════════
// Decimal256 Velox tests (both fits_u64 and large-values paths)
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, Decimal256FitsU64) {
    auto type = arrow::decimal256(18, 2);
    arrow::Decimal256Builder b(type);
    APPEND(b, arrow::Decimal256::FromString("12345").ValueOrDie());
    APPEND(b, arrow::Decimal256::FromString("67890").ValueOrDie());
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidDecimalArray::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);
    auto flat = vec->asFlatVector<int64_t>();
    ASSERT_EQ(flat->size(), 2);
    EXPECT_EQ(flat->valueAt(0), 12345);
}

TEST(VeloxCrossVal, Decimal256LargeValues) {
    auto type = arrow::decimal256(38, 0);
    arrow::Decimal256Builder b(type);
    auto large = arrow::Decimal256::FromString("123456789012345678901234567890").ValueOrDie();
    APPEND(b, large);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidFixedLenByteArray::from_decimal256(arr);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);
    auto flat = vec->asFlatVector<int128_t>();
    ASSERT_EQ(flat->size(), 1);
}

// ═══════════════════════════════════════════════════════════════════════
// Edge case Velox tests
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, EmptyArray) {
    auto arr = arrow::MakeEmptyArray(arrow::int32()).ValueOrDie();
    auto liquid = LiquidPrimitiveArray<arrow::Int32Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);
    EXPECT_EQ(vec->size(), 0);
}

TEST(VeloxCrossVal, SingleElement) {
    arrow::Int64Builder b;
    APPEND(b, 42LL);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidPrimitiveArray<arrow::Int64Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<int64_t>();
    ASSERT_EQ(flat->size(), 1);
    EXPECT_EQ(flat->valueAt(0), 42);
}

TEST(VeloxCrossVal, AllNullFloat64) {
    arrow::DoubleBuilder b;
    APPEND_NULL(b); APPEND_NULL(b); APPEND_NULL(b);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidFloatArray<double>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);
    EXPECT_EQ(vec->size(), 3);
    EXPECT_TRUE(vec->isNullAt(0));
    EXPECT_TRUE(vec->isNullAt(1));
}

TEST(VeloxCrossVal, AllNullString) {
    arrow::StringBuilder b;
    APPEND_NULL(b); APPEND_NULL(b);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidByteViewArray::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);
    EXPECT_EQ(vec->size(), 2);
    EXPECT_TRUE(vec->isNullAt(0));
}

TEST(VeloxCrossVal, Float32Extremes) {
    std::vector<float> vals = {0.0f, 1.0f, -1.0f,
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::min()};
    arrow::FloatBuilder b;
    for (auto v : vals) APPEND(b, v);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidFloatArray<float>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<float>();
    for (size_t i = 0; i < vals.size(); ++i) {
        EXPECT_FLOAT_EQ(flat->valueAt(i), vals[i]);
    }
}

TEST(VeloxCrossVal, TimestampNegativeAndExtreme) {
    auto type = arrow::timestamp(arrow::TimeUnit::MICRO);
    arrow::TimestampBuilder b(type, arrow::default_memory_pool());
    APPEND(b, -86400000000LL);
    APPEND(b, 0LL);
    APPEND(b, 253402300799999999LL);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidPrimitiveArray<arrow::TimestampType>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<Timestamp>();
    EXPECT_FALSE(flat->isNullAt(0));
    EXPECT_FALSE(flat->isNullAt(1));
}

// ═══════════════════════════════════════════════════════════════════════
// Delta encoding Velox cross-validation
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, DeltaInt32Monotonic) {
    arrow::Int32Builder b;
    for (int32_t i = 0; i < 200; ++i) APPEND(b, i);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidPrimitiveDeltaArray<arrow::Int32Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<int32_t>();
    ASSERT_EQ(flat->size(), 200);
    EXPECT_EQ(flat->valueAt(0), 0);
    EXPECT_EQ(flat->valueAt(199), 199);
}

TEST(VeloxCrossVal, DeltaInt32WithNulls) {
    arrow::Int32Builder b;
    APPEND(b, 100); APPEND_NULL(b); APPEND(b, 200); APPEND(b, 300);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidPrimitiveDeltaArray<arrow::Int32Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<int32_t>();
    EXPECT_FALSE(flat->isNullAt(0)); EXPECT_EQ(flat->valueAt(0), 100);
    EXPECT_TRUE(flat->isNullAt(1));
    EXPECT_FALSE(flat->isNullAt(2)); EXPECT_EQ(flat->valueAt(2), 200);
}

TEST(VeloxCrossVal, DeltaInt64Monotonic) {
    arrow::Int64Builder b;
    for (int64_t i = 0; i < 200; ++i) APPEND(b, i * 1000LL);
    auto arr = b.Finish().ValueOrDie();
    auto liquid = LiquidPrimitiveDeltaArray<arrow::Int64Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    auto flat = vec->asFlatVector<int64_t>();
    ASSERT_EQ(flat->size(), 200);
    EXPECT_EQ(flat->valueAt(0), 0);
    EXPECT_EQ(flat->valueAt(199), 199000LL);
}

TEST(VeloxCrossVal, DeltaInt64AllNull) {
    auto arr = arrow::MakeArrayOfNull(arrow::int64(), 5).ValueOrDie();
    auto liquid = LiquidPrimitiveDeltaArray<arrow::Int64Type>::from_arrow(arr);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);
    EXPECT_EQ(vec->size(), 5);
    EXPECT_TRUE(vec->isNullAt(0));
}

// ═══════════════════════════════════════════════════════════════════════
// Custom main() to initialize Arrow compute for static linking
// ═══════════════════════════════════════════════════════════════════════

class ArrowEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        // Arrow 24+ requires explicit Initialize() for static linking.
        // Arrow 18 uses --whole-archive which includes static initializers.
#if ARROW_VERSION_MAJOR >= 19
        auto status = arrow::compute::Initialize();
        if (!status.ok()) {
            fprintf(stderr, "Failed to initialize Arrow compute: %s\n",
                    status.ToString().c_str());
        }
#endif
    }
};

int main(int argc, char** argv) {
    // Initialize Velox MemoryManager before any tests
    memory::MemoryManager::initialize(memory::MemoryManager::Options{});
    auto pool = memory::memoryManager()->addLeafPool("test_pool");
    g_pool = pool.get();

    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new ArrowEnvironment);
    return RUN_ALL_TESTS();
}
