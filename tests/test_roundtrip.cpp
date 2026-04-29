// tests/test_roundtrip.cpp
// Round-trip correctness tests for all Liquid Cache data types.
// Verifies: Arrow → Liquid encode → Liquid decode → Arrow matches original.
#include <gtest/gtest.h>

#include <arrow/api.h>
#include <arrow/compute/initialize.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "liquid_cache/liquid_arrays.h"
#include "liquid_cache/liquid_byte_view_array.h"
#include "liquid_cache/liquid_decimal_array.h"
#include "liquid_cache/liquid_fixed_len_byte_array.h"
#include "liquid_cache/transcoder.h"

using namespace liquid_cache;

// Helper macros to ignore [[nodiscard]] on builder methods in tests
#define APPEND(builder, val) do { auto _s = (builder).Append(val); (void)_s; } while(0)
#define APPEND_NULL(builder) do { auto _s = (builder).AppendNull(); (void)_s; } while(0)

// ═══════════════════════════════════════════════════════════════════════
// Helper: verify round-trip equality for a given array
// Compares element-by-element to avoid padding/undefined-bit issues
// in Arrow's null bitmaps and value buffers.
// ═══════════════════════════════════════════════════════════════════════
static void assert_roundtrip(
        const std::shared_ptr<arrow::Array>& original,
        const std::shared_ptr<arrow::Array>& roundtripped) {
    ASSERT_EQ(original->length(), roundtripped->length());
    ASSERT_EQ(original->null_count(), roundtripped->null_count());
    ASSERT_EQ(original->type_id(), roundtripped->type_id());

    int64_t n = original->length();
    for (int64_t i = 0; i < n; ++i) {
        bool orig_null = original->IsNull(i);
        bool rt_null = roundtripped->IsNull(i);
        ASSERT_EQ(orig_null, rt_null) << "Null mismatch at index " << i;
        if (orig_null) continue;

        // Compare the raw scalar value via GetScalar
        auto orig_scalar = original->GetScalar(i).ValueOrDie();
        auto rt_scalar = roundtripped->GetScalar(i).ValueOrDie();
        ASSERT_TRUE(orig_scalar->Equals(*rt_scalar))
            << "Value mismatch at index " << i
            << ": original=" << orig_scalar->ToString()
            << ", roundtripped=" << rt_scalar->ToString();
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Integer type round-trip tests
// ═══════════════════════════════════════════════════════════════════════

template <typename ArrowType>
void test_primitive_roundtrip(const std::vector<typename ArrowType::c_type>& values) {
    using BuilderT = typename arrow::TypeTraits<ArrowType>::BuilderType;
    BuilderT builder;
    for (auto v : values) {
        APPEND(builder, v);
    }
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<ArrowType>::from_arrow(array);
    auto decoded = liquid.to_arrow();
    assert_roundtrip(array, decoded);
}

TEST(RoundtripInt8, Basic) {
    test_primitive_roundtrip<arrow::Int8Type>({0, 1, -1, 127, -128, 42, -42});
}
TEST(RoundtripInt16, Basic) {
    test_primitive_roundtrip<arrow::Int16Type>({0, 1000, -1000, 32767, -32768});
}
TEST(RoundtripInt32, Basic) {
    test_primitive_roundtrip<arrow::Int32Type>({0, 100000, -100000, INT32_MAX, INT32_MIN});
}
TEST(RoundtripInt64, Basic) {
    test_primitive_roundtrip<arrow::Int64Type>({0, 1000000000LL, -1000000000LL, INT64_MAX, INT64_MIN});
}
TEST(RoundtripUInt8, Basic) {
    test_primitive_roundtrip<arrow::UInt8Type>({0, 1, 127, 255, 42});
}
TEST(RoundtripUInt16, Basic) {
    test_primitive_roundtrip<arrow::UInt16Type>({0, 1000, 65535, 30000});
}
TEST(RoundtripUInt32, Basic) {
    test_primitive_roundtrip<arrow::UInt32Type>({0, 100000, UINT32_MAX});
}
TEST(RoundtripUInt64, Basic) {
    test_primitive_roundtrip<arrow::UInt64Type>({0, 1000000000ULL, UINT64_MAX});
}

// ═══════════════════════════════════════════════════════════════════════
// Date type round-trip tests
// ═══════════════════════════════════════════════════════════════════════

TEST(RoundtripDate32, Basic) {
    test_primitive_roundtrip<arrow::Date32Type>({0, 1, 365, 19053, -1});
}
TEST(RoundtripDate64, Basic) {
    test_primitive_roundtrip<arrow::Date64Type>(
        {0LL, 86400000LL, 1648000000000LL, -86400000LL});
}

// ═══════════════════════════════════════════════════════════════════════
// Null handling tests
// ═══════════════════════════════════════════════════════════════════════

TEST(RoundtripInt32, WithNulls) {
    arrow::Int32Builder builder;
    APPEND(builder,1);
    APPEND(builder,2);
    APPEND_NULL(builder);
    APPEND(builder,4);
    APPEND_NULL(builder);
    APPEND(builder,6);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::Int32Type>::from_arrow(array);
    auto decoded = liquid.to_arrow();
    assert_roundtrip(array, decoded);
}

TEST(RoundtripInt64, AllNull) {
    arrow::Int64Builder builder;
    for (int i = 0; i < 5; ++i) APPEND_NULL(builder);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::Int64Type>::from_arrow(array);
    auto decoded = liquid.to_arrow();
    assert_roundtrip(array, decoded);
}

// ═══════════════════════════════════════════════════════════════════════
// Float type round-trip tests
// ═══════════════════════════════════════════════════════════════════════

TEST(RoundtripFloat32, Basic) {
    arrow::FloatBuilder builder;
    APPEND(builder,0.0f);
    APPEND(builder,1.5f);
    APPEND(builder,-3.14f);
    APPEND(builder,100.0f);
    APPEND(builder,0.001f);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidFloatArray<float>::from_arrow(array);
    auto decoded = liquid.to_arrow();
    assert_roundtrip(array, decoded);
}

TEST(RoundtripFloat64, Basic) {
    arrow::DoubleBuilder builder;
    APPEND(builder,0.0);
    APPEND(builder,1.5);
    APPEND(builder,-3.14);
    APPEND(builder,100.0);
    APPEND(builder,0.001);
    APPEND(builder,1e15);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidFloatArray<double>::from_arrow(array);
    auto decoded = liquid.to_arrow();
    assert_roundtrip(array, decoded);
}

TEST(RoundtripFloat64, WithNulls) {
    arrow::DoubleBuilder builder;
    APPEND(builder,1.1);
    APPEND_NULL(builder);
    APPEND(builder,3.3);
    APPEND_NULL(builder);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidFloatArray<double>::from_arrow(array);
    auto decoded = liquid.to_arrow();
    assert_roundtrip(array, decoded);
}

// ═══════════════════════════════════════════════════════════════════════
// String/Binary round-trip tests
// ═══════════════════════════════════════════════════════════════════════

TEST(RoundtripString, Basic) {
    arrow::StringBuilder builder;
    APPEND(builder,"hello");
    APPEND(builder,"world");
    APPEND(builder,"");
    APPEND(builder,"a longer string for testing");
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidByteViewArray::from_arrow(array);
    auto decoded = liquid.to_arrow();
    assert_roundtrip(array, decoded);
}

TEST(RoundtripString, WithNulls) {
    arrow::StringBuilder builder;
    APPEND(builder,"first");
    APPEND_NULL(builder);
    APPEND(builder,"third");
    APPEND_NULL(builder);
    APPEND(builder,"fifth");
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidByteViewArray::from_arrow(array);
    auto decoded = liquid.to_arrow();
    assert_roundtrip(array, decoded);
}

TEST(RoundtripBinary, Basic) {
    arrow::BinaryBuilder builder;
    APPEND(builder,"\x00\x01\x02");
    APPEND(builder,"text");
    APPEND(builder,"");
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidByteViewArray::from_arrow(array);
    auto decoded = liquid.to_arrow();
    assert_roundtrip(array, decoded);
}

// ═══════════════════════════════════════════════════════════════════════
// Decimal128 round-trip tests (fits-u64 path)
// ═══════════════════════════════════════════════════════════════════════

TEST(RoundtripDecimal128, FitsU64) {
    auto type = arrow::decimal128(10, 2);
    arrow::Decimal128Builder builder(type);
    for (int i = 0; i < 100; ++i) {
        auto val = arrow::Decimal128(i * 100);
        APPEND(builder, val);
    }
    auto array = builder.Finish().ValueOrDie();

    ASSERT_TRUE(LiquidDecimalArray::fits_u64(array));

    auto liquid = LiquidDecimalArray::from_arrow(array);
    auto decoded = liquid.to_arrow();
    assert_roundtrip(array, decoded);
}

TEST(RoundtripDecimal128, FitsU64WithNulls) {
    auto type = arrow::decimal128(10, 2);
    arrow::Decimal128Builder builder(type);
    for (int i = 0; i < 10; ++i) {
        if (i % 3 == 0) {
            APPEND_NULL(builder);
        } else {
            auto val = arrow::Decimal128(i * 100);
            APPEND(builder, val);
        }
    }
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidDecimalArray::from_arrow(array);
    auto decoded = liquid.to_arrow();
    assert_roundtrip(array, decoded);
}

// ═══════════════════════════════════════════════════════════════════════
// Decimal128 round-trip tests (FSST dictionary path - large values)
// ═══════════════════════════════════════════════════════════════════════

TEST(RoundtripDecimal128, LargeValues) {
    auto type = arrow::decimal128(38, 6);
    arrow::Decimal128Builder builder(type);
    for (int i = 0; i < 50; ++i) {
        auto val_str = std::to_string(i) + std::string(20, '0');
        auto val = arrow::Decimal128(val_str);
        APPEND(builder, val);
    }
    auto array = builder.Finish().ValueOrDie();

    ASSERT_FALSE(LiquidDecimalArray::fits_u64(array));

    auto liquid = LiquidFixedLenByteArray::from_decimal128(array);
    auto decoded = liquid.to_arrow();
    assert_roundtrip(array, decoded);
}

TEST(RoundtripDecimal128, LargeValuesWithNulls) {
    auto type = arrow::decimal128(38, 6);
    arrow::Decimal128Builder builder(type);
    for (int i = 0; i < 20; ++i) {
        if (i % 4 == 0) {
            APPEND_NULL(builder);
        } else {
            auto val_str = std::to_string(i) + std::string(20, '0');
            auto val = arrow::Decimal128(val_str);
            APPEND(builder, val);
        }
    }
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidFixedLenByteArray::from_decimal128(array);
    auto decoded = liquid.to_arrow();
    assert_roundtrip(array, decoded);
}

// ═══════════════════════════════════════════════════════════════════════
// Decimal256 round-trip tests
// ═══════════════════════════════════════════════════════════════════════

TEST(RoundtripDecimal256, LargeValues) {
    auto type = arrow::decimal256(50, 6);
    arrow::Decimal256Builder builder(type);
    for (int i = 0; i < 30; ++i) {
        auto val_str = std::to_string(i) + std::string(40, '0');
        auto val = arrow::Decimal256(val_str);
        APPEND(builder, val);
    }
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidFixedLenByteArray::from_decimal256(array);
    auto decoded = liquid.to_arrow();
    assert_roundtrip(array, decoded);
}

// ═══════════════════════════════════════════════════════════════════════
// Transcoder-level round-trip tests (full encode/decode pipeline)
// ═══════════════════════════════════════════════════════════════════════

TEST(TranscodeRoundtrip, Int32Array) {
    arrow::Int32Builder builder;
    for (int i = 0; i < 100; ++i) APPEND(builder,i * 10);
    auto array = builder.Finish().ValueOrDie();

    auto encoded = transcode_arrow_array(array);
    EXPECT_TRUE(encoded.is_valid());
    auto decoded = decode_liquid_array(encoded);
    ASSERT_NE(decoded, nullptr);
    assert_roundtrip(array, decoded);
}

TEST(TranscodeRoundtrip, Float64Array) {
    arrow::DoubleBuilder builder;
    for (int i = 0; i < 50; ++i) APPEND(builder,i * 1.5);
    auto array = builder.Finish().ValueOrDie();

    auto encoded = transcode_arrow_array(array);
    EXPECT_TRUE(encoded.is_valid());
    auto decoded = decode_liquid_array(encoded);
    ASSERT_NE(decoded, nullptr);
    assert_roundtrip(array, decoded);
}

TEST(TranscodeRoundtrip, StringArray) {
    arrow::StringBuilder builder;
    for (int i = 0; i < 50; ++i) APPEND(builder,"string_" + std::to_string(i));
    auto array = builder.Finish().ValueOrDie();

    auto encoded = transcode_arrow_array(array);
    EXPECT_TRUE(encoded.is_valid());
    auto decoded = decode_liquid_array(encoded);
    ASSERT_NE(decoded, nullptr);
    assert_roundtrip(array, decoded);
}

TEST(TranscodeRoundtrip, Decimal128FitsU64) {
    auto type = arrow::decimal128(10, 2);
    arrow::Decimal128Builder builder(type);
    for (int i = 0; i < 50; ++i) {
        auto val = arrow::Decimal128(i * 100);
        APPEND(builder, val);
    }
    auto array = builder.Finish().ValueOrDie();

    auto encoded = transcode_arrow_array(array);
    EXPECT_TRUE(encoded.is_valid());
    auto decoded = decode_liquid_array(encoded);
    ASSERT_NE(decoded, nullptr);
    assert_roundtrip(array, decoded);
}

TEST(TranscodeRoundtrip, Decimal128Large) {
    auto type = arrow::decimal128(38, 6);
    arrow::Decimal128Builder builder(type);
    for (int i = 0; i < 50; ++i) {
        auto val_str = std::to_string(i) + std::string(20, '0');
        auto val = arrow::Decimal128(val_str);
        APPEND(builder, val);
    }
    auto array = builder.Finish().ValueOrDie();

    auto encoded = transcode_arrow_array(array);
    EXPECT_TRUE(encoded.is_valid());
    auto decoded = decode_liquid_array(encoded);
    ASSERT_NE(decoded, nullptr);
    assert_roundtrip(array, decoded);
}

// ═══════════════════════════════════════════════════════════════════════
// Edge case tests
// ═══════════════════════════════════════════════════════════════════════

TEST(RoundtripEdge, EmptyArray) {
    arrow::Int32Builder builder;
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::Int32Type>::from_arrow(array);
    auto decoded = liquid.to_arrow();
    EXPECT_EQ(decoded->length(), 0);
}

TEST(RoundtripEdge, SingleElement) {
    arrow::Int64Builder builder;
    APPEND(builder,42);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::Int64Type>::from_arrow(array);
    auto decoded = liquid.to_arrow();
    assert_roundtrip(array, decoded);
}

TEST(RoundtripEdge, ConstantValues) {
    // All same value → bit_width = 0, best compression
    arrow::Int32Builder builder;
    for (int i = 0; i < 100; ++i) APPEND(builder,42);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::Int32Type>::from_arrow(array);
    EXPECT_EQ(liquid.bit_width(), 0);  // constant → 0 bits needed
    auto decoded = liquid.to_arrow();
    assert_roundtrip(array, decoded);
}

// ═══════════════════════════════════════════════════════════════════════
// Serialization round-trip tests (to_bytes / from_bytes)
// ═══════════════════════════════════════════════════════════════════════

TEST(SerializationRoundtrip, Int32Array) {
    arrow::Int32Builder builder;
    for (int i = 0; i < 50; ++i) APPEND(builder,i * 7);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::Int32Type>::from_arrow(array);
    auto bytes = liquid.to_bytes();
    auto restored = LiquidPrimitiveArray<arrow::Int32Type>::from_bytes(
        bytes.data(), bytes.size());
    auto decoded = restored.to_arrow();
    assert_roundtrip(array, decoded);
}

TEST(SerializationRoundtrip, Float64Array) {
    arrow::DoubleBuilder builder;
    for (int i = 0; i < 50; ++i) APPEND(builder,i * 1.5);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidFloatArray<double>::from_arrow(array);
    auto bytes = liquid.to_bytes();
    auto restored = LiquidFloatArray<double>::from_bytes(
        bytes.data(), bytes.size());
    auto decoded = restored.to_arrow();
    assert_roundtrip(array, decoded);
}

TEST(SerializationRoundtrip, Decimal128FitsU64) {
    auto type = arrow::decimal128(10, 2);
    arrow::Decimal128Builder builder(type);
    for (int i = 0; i < 30; ++i) {
        auto val = arrow::Decimal128(i * 100);
        APPEND(builder, val);
    }
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidDecimalArray::from_arrow(array);
    auto bytes = liquid.to_bytes();
    auto restored = LiquidDecimalArray::from_bytes(bytes.data(), bytes.size());
    auto decoded = restored.to_arrow();
    assert_roundtrip(array, decoded);
}

TEST(SerializationRoundtrip, StringArray) {
    arrow::StringBuilder builder;
    for (int i = 0; i < 30; ++i) APPEND(builder,"test_string_" + std::to_string(i));
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidByteViewArray::from_arrow(array);
    auto bytes = liquid.to_bytes();
    auto restored = LiquidByteViewArray::from_bytes(bytes.data(), bytes.size());
    auto decoded = restored.to_arrow();
    assert_roundtrip(array, decoded);
}

// ═══════════════════════════════════════════════════════════════════════
// Compression ratio sanity checks
// ═══════════════════════════════════════════════════════════════════════

TEST(Compression, IntegerSmallerThanOriginal) {
    arrow::Int64Builder builder;
    for (int i = 0; i < 1000; ++i) APPEND(builder,i);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::Int64Type>::from_arrow(array);
    // Sequential 0..999 has small range → bit_width should be << 64
    EXPECT_LT(liquid.memory_size(),
              static_cast<size_t>(array->length() * sizeof(int64_t)));
}

TEST(Compression, StringSmallerThanOriginal) {
    arrow::StringBuilder builder;
    // Need enough rows for dictionary+FSST savings to outweigh metadata overhead
    for (int i = 0; i < 10000; ++i) APPEND(builder,"repeated_string_value");
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidByteViewArray::from_arrow(array);
    // Compute raw Arrow size: offsets buffer + data buffer
    size_t arrow_size = 0;
    for (auto& buf : array->data()->buffers) {
        if (buf) arrow_size += buf->size();
    }
    // Dictionary + FSST compression should make repeated strings smaller
    EXPECT_LT(liquid.memory_size(), arrow_size);
}

// ═══════════════════════════════════════════════════════════════════════
// Custom main() to initialize Arrow compute for static linking
// ═══════════════════════════════════════════════════════════════════════

class ArrowEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        // Register Arrow compute kernels (required for static linking)
        auto status = arrow::compute::Initialize();
        if (!status.ok()) {
            fprintf(stderr, "Failed to initialize Arrow compute: %s\n",
                    status.ToString().c_str());
        }
    }
};

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new ArrowEnvironment);
    return RUN_ALL_TESTS();
}
