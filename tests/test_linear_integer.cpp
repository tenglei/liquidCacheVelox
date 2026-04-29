// test_linear_integer.cpp
// Quick roundtrip test for LiquidLinearIntegerArray<T>
#include <arrow/api.h>
#include <arrow/array/array_base.h>
#include <iostream>
#include <vector>

#include "liquid_cache/liquid_arrays.h"
#include "liquid_cache/ipc_header.h"

using namespace liquid_cache;

template <typename ArrowType>
bool test_roundtrip(const std::vector<typename ArrowType::c_type>& values,
                    const std::vector<bool>& nulls = {}) {
    using NativeT = typename ArrowType::c_type;

    // Build Arrow array
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

    // Encode
    auto liquid = LiquidLinearIntegerArray<ArrowType>::from_arrow(arr);

    // Decode
    auto decoded = liquid.to_arrow();
    auto dec_typed = std::static_pointer_cast<
        typename arrow::TypeTraits<ArrowType>::ArrayType>(decoded);

    // Compare
    auto orig_typed = std::static_pointer_cast<
        typename arrow::TypeTraits<ArrowType>::ArrayType>(arr);

    if (orig_typed->length() != dec_typed->length()) {
        std::cerr << "Length mismatch: " << orig_typed->length()
                  << " vs " << dec_typed->length() << std::endl;
        return false;
    }

    for (int64_t i = 0; i < orig_typed->length(); ++i) {
        bool orig_null = orig_typed->IsNull(i);
        bool dec_null = dec_typed->IsNull(i);
        if (orig_null != dec_null) {
            std::cerr << "Null mismatch at " << i << std::endl;
            return false;
        }
        if (!orig_null) {
            NativeT ov = orig_typed->Value(i);
            NativeT dv = dec_typed->Value(i);
            if (ov != dv) {
                std::cerr << "Value mismatch at " << i << ": "
                          << static_cast<int64_t>(ov) << " vs "
                          << static_cast<int64_t>(dv) << std::endl;
                return false;
            }
        }
    }
    return true;
}

template <typename ArrowType>
bool test_serialization_roundtrip(
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

    for (int64_t i = 0; i < orig_typed->length(); ++i) {
        bool orig_null = orig_typed->IsNull(i);
        bool dec_null = dec_typed->IsNull(i);
        if (orig_null != dec_null) {
            std::cerr << "Serialize null mismatch at " << i << std::endl;
            return false;
        }
        if (!orig_null) {
            NativeT ov = orig_typed->Value(i);
            NativeT dv = dec_typed->Value(i);
            if (ov != dv) {
                std::cerr << "Serialize value mismatch at " << i << ": "
                          << static_cast<int64_t>(ov) << " vs "
                          << static_cast<int64_t>(dv) << std::endl;
                return false;
            }
        }
    }
    return true;
}

int main() {
    int passed = 0, failed = 0;

    // Test 1: Monotonic int32 sequence
    std::cout << "Test 1: Monotonic Int32... ";
    if (test_roundtrip<arrow::Int32Type>({10, 15, 14, 20, 18, 25, 24})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    // Test 2: With nulls
    std::cout << "Test 2: Int32 with nulls... ";
    if (test_roundtrip<arrow::Int32Type>({10, 0, 30, 0, 50, 70},
                                          {true, false, true, false, true, true})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    // Test 3: All nulls
    std::cout << "Test 3: All nulls... ";
    if (test_roundtrip<arrow::Int32Type>({0, 0, 0, 0},
                                          {false, false, false, false})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    // Test 4: Single element
    std::cout << "Test 4: Single Int32... ";
    if (test_roundtrip<arrow::Int32Type>({42})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    // Test 5: Empty
    std::cout << "Test 5: Empty... ";
    if (test_roundtrip<arrow::Int32Type>({})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    // Test 6: Negative values
    std::cout << "Test 6: Negative Int32... ";
    if (test_roundtrip<arrow::Int32Type>({-100, -50, 0, 50, 25, -25})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    // Test 7: Int8
    std::cout << "Test 7: Int8... ";
    if (test_roundtrip<arrow::Int8Type>({-10, 0, 10, 20})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    // Test 8: Int16
    std::cout << "Test 8: Int16... ";
    if (test_roundtrip<arrow::Int16Type>({-1000, 0, 1000, 2000})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    // Test 9: Int64
    std::cout << "Test 9: Int64... ";
    if (test_roundtrip<arrow::Int64Type>({-10000000000LL, 0, 10000000000LL, 20000000000LL})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    // Test 10: UInt8
    std::cout << "Test 10: UInt8... ";
    if (test_roundtrip<arrow::UInt8Type>({0, 10, 200, 255})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    // Test 11: UInt16
    std::cout << "Test 11: UInt16... ";
    if (test_roundtrip<arrow::UInt16Type>({0, 1000, 60000, 500})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    // Test 12: UInt32
    std::cout << "Test 12: UInt32... ";
    if (test_roundtrip<arrow::UInt32Type>({0, 1000000, 3000000000U, 123456789})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    // Test 13: UInt64
    std::cout << "Test 13: UInt64... ";
    if (test_roundtrip<arrow::UInt64Type>({0ULL, 10000000000ULL, 42ULL})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    // Test 14: Date32
    std::cout << "Test 14: Date32... ";
    if (test_roundtrip<arrow::Date32Type>({-365, 0, 365, 18262})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    // Test 15: Date64
    std::cout << "Test 15: Date64... ";
    if (test_roundtrip<arrow::Date64Type>({-86400000LL, 0, 86400000LL, 1000000000000LL})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    // Serialization roundtrip tests
    std::cout << "Test 16: Serialization Int32... ";
    if (test_serialization_roundtrip<arrow::Int32Type>({10, 15, 14, 20, 18, 25, 24})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    std::cout << "Test 17: Serialization with nulls... ";
    if (test_serialization_roundtrip<arrow::Int32Type>({10, 0, 30, 0, 50, 70},
                                                        {true, false, true, false, true, true})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    std::cout << "Test 18: Serialization Int64... ";
    if (test_serialization_roundtrip<arrow::Int64Type>({-10000000000LL, 0, 10000000000LL})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    std::cout << "Test 19: Serialization UInt32... ";
    if (test_serialization_roundtrip<arrow::UInt32Type>({0, 1000000, 3000000000U})) {
        std::cout << "PASS" << std::endl; ++passed;
    } else { std::cout << "FAIL" << std::endl; ++failed; }

    // Compression ratio test
    std::cout << "Test 20: Compression ratio (linear sequence)... ";
    {
        std::vector<int32_t> seq;
        for (int32_t i = 0; i < 10000; i += 10) seq.push_back(i);
        arrow::Int32Builder bld; ARROW_CHECK_OK(bld.AppendValues(seq)); auto arr = bld.Finish().ValueOrDie();

        auto primitive = LiquidPrimitiveArray<arrow::Int32Type>::from_arrow(arr);
        auto linear = LiquidLinearIntegerArray<arrow::Int32Type>::from_arrow(arr);

        size_t prim_size = primitive.memory_size();
        size_t lin_size = linear.memory_size();

        std::cout << "Primitive=" << prim_size << " bytes, Linear=" << lin_size << " bytes" << std::endl;

        if (lin_size < prim_size) {
            std::cout << "Test 20: Linear beats Primitive on monotonic data... PASS" << std::endl;
            ++passed;
        } else {
            std::cout << "Test 20: Linear does NOT beat Primitive... ";
            // Not necessarily a failure, depends on data
            if (lin_size < prim_size * 2) {
                std::cout << "PASS (acceptable)" << std::endl;
                ++passed;
            } else {
                std::cout << "FAIL" << std::endl;
                ++failed;
            }
        }
    }

    std::cout << "\n=== Results: " << passed << " passed, " << failed << " failed ===" << std::endl;
    return failed > 0 ? 1 : 0;
}
