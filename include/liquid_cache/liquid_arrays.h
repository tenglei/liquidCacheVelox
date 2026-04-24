// liquid_cache/liquid_arrays.h
// LiquidPrimitiveArray and LiquidFloatArray - C++ implementation
// Binary-compatible with the Rust liquid-cache serialization format.
#pragma once

#include <arrow/util/logging.h>  // MUST be first: defines ARROW_CHECK_OK macro
#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/type_traits.h>

#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "liquid_cache/bit_packed_array.h"
#include "liquid_cache/ipc_header.h"

// Portable Arrow status check: does not depend on ARROW_CHECK_OK macro
// being available at template definition point (avoids two-phase lookup issues).
#ifndef LIQUID_ARROW_OK
#define LIQUID_ARROW_OK(expr) \
    do { \
        ::arrow::Status _s = (expr); \
        if (!_s.ok()) throw std::runtime_error(_s.ToString()); \
    } while (false)
#endif

namespace liquid_cache {

// ═══════════════════════════════════════════════════════════════════════
// Utility: compute the minimum number of bits to represent `value`
// ═══════════════════════════════════════════════════════════════════════
inline uint8_t get_bit_width(uint64_t value) {
    if (value == 0) return 0;
    return static_cast<uint8_t>(64 - __builtin_clzll(value));
}

// ═══════════════════════════════════════════════════════════════════════
// Type trait helpers
// ═══════════════════════════════════════════════════════════════════════

/// Maps a signed Arrow type to its unsigned counterpart for bit packing.
template <typename ArrowType>
struct UnsignedType;

template <> struct UnsignedType<arrow::Int8Type>  { using type = uint8_t; };
template <> struct UnsignedType<arrow::Int16Type> { using type = uint16_t; };
template <> struct UnsignedType<arrow::Int32Type> { using type = uint32_t; };
template <> struct UnsignedType<arrow::Int64Type> { using type = uint64_t; };
template <> struct UnsignedType<arrow::UInt8Type>  { using type = uint8_t; };
template <> struct UnsignedType<arrow::UInt16Type> { using type = uint16_t; };
template <> struct UnsignedType<arrow::UInt32Type> { using type = uint32_t; };
template <> struct UnsignedType<arrow::UInt64Type> { using type = uint64_t; };
template <> struct UnsignedType<arrow::Date32Type> { using type = uint32_t; };
template <> struct UnsignedType<arrow::Date64Type> { using type = uint64_t; };
template <> struct UnsignedType<arrow::TimestampType> { using type = uint64_t; };

/// Maps Arrow type to PhysicalType enum.
template <typename ArrowType>
struct ArrowPhysicalType;

template <> struct ArrowPhysicalType<arrow::Int8Type>  { static constexpr PhysicalType value = PhysicalType::Int8; };
template <> struct ArrowPhysicalType<arrow::Int16Type> { static constexpr PhysicalType value = PhysicalType::Int16; };
template <> struct ArrowPhysicalType<arrow::Int32Type> { static constexpr PhysicalType value = PhysicalType::Int32; };
template <> struct ArrowPhysicalType<arrow::Int64Type> { static constexpr PhysicalType value = PhysicalType::Int64; };
template <> struct ArrowPhysicalType<arrow::UInt8Type>  { static constexpr PhysicalType value = PhysicalType::UInt8; };
template <> struct ArrowPhysicalType<arrow::UInt16Type> { static constexpr PhysicalType value = PhysicalType::UInt16; };
template <> struct ArrowPhysicalType<arrow::UInt32Type> { static constexpr PhysicalType value = PhysicalType::UInt32; };
template <> struct ArrowPhysicalType<arrow::UInt64Type> { static constexpr PhysicalType value = PhysicalType::UInt64; };
template <> struct ArrowPhysicalType<arrow::Date32Type> { static constexpr PhysicalType value = PhysicalType::Date32; };
template <> struct ArrowPhysicalType<arrow::Date64Type> { static constexpr PhysicalType value = PhysicalType::Date64; };

// ═══════════════════════════════════════════════════════════════════════
// LiquidPrimitiveArray<T>
//
// Frame-of-Reference + BitPacking encoding for integer/date types.
// Encoding:  offset[i] = (value[i] - min_value) as unsigned
//            bit_width  = bits needed for max(offset)
//            packed     = BitPackedArray(offsets, bit_width)
//
// Serialization layout (identical to Rust):
//   [LiquidIPCHeader: 16B]
//   [reference_value: sizeof(NativeT)]
//   [padding to 8-byte alignment]
//   [BitPackedArray serialized data]
// ═══════════════════════════════════════════════════════════════════════

template <typename ArrowType>
class LiquidPrimitiveArray {
public:
    using NativeT = typename ArrowType::c_type;
    using UnsignedT = typename UnsignedType<ArrowType>::type;

    LiquidPrimitiveArray() = default;

    /// Encode an Arrow array into Liquid format.
    ///
    /// Algorithm (matches Rust LiquidPrimitiveArray::from_arrow_array):
    ///   1. Compute min/max over non-null values
    ///   2. reference_value = min
    ///   3. For each value: offset = (value - min) reinterpreted as unsigned
    ///   4. bit_width = bits needed to store max_offset
    ///   5. Bit-pack all offsets
    static LiquidPrimitiveArray from_arrow(
            const std::shared_ptr<arrow::Array>& array) {
        using ArrayT = typename arrow::TypeTraits<ArrowType>::ArrayType;
        auto typed = std::static_pointer_cast<ArrayT>(array);
        const int64_t len = typed->length();

        LiquidPrimitiveArray result;

        // Handle all-null case
        if (typed->null_count() == len) {
            result.reference_value_ = 0;
            std::vector<uint64_t> zeros(len, 0);
            // Build null bitmap (all-null)
            std::vector<uint8_t> null_bits((len + 7) / 8, 0);
            result.bit_packed_ = BitPackedArray(zeros.data(), null_bits.data(),
                                                 static_cast<uint32_t>(len), 0);
            return result;
        }

        // Compute min/max
        auto min_max = arrow::compute::MinMax(array);
        auto min_max_scalar = min_max.ValueOrDie().scalar_as<arrow::StructScalar>();
        NativeT min_val = std::static_pointer_cast<
            typename arrow::TypeTraits<ArrowType>::ScalarType>(
                min_max_scalar.value[0])->value;
        NativeT max_val = std::static_pointer_cast<
            typename arrow::TypeTraits<ArrowType>::ScalarType>(
                min_max_scalar.value[1])->value;

        result.reference_value_ = min_val;

        // Compute offsets as unsigned
        UnsignedT range = static_cast<UnsignedT>(max_val - min_val);
        uint8_t bw = get_bit_width(static_cast<uint64_t>(range));

        std::vector<uint64_t> offsets(len);
        for (int64_t i = 0; i < len; ++i) {
            NativeT v = typed->Value(i);
            offsets[i] = static_cast<uint64_t>(static_cast<UnsignedT>(v - min_val));
        }

        // Build null bitmap
        const uint8_t* null_bitmap = nullptr;
        std::vector<uint8_t> null_bits;
        if (typed->null_count() > 0 && typed->null_bitmap()) {
            size_t bitmap_bytes = (len + 7) / 8;
            null_bits.assign(typed->null_bitmap()->data() + typed->offset() / 8,
                             typed->null_bitmap()->data() + typed->offset() / 8 + bitmap_bytes);
            null_bitmap = null_bits.data();
        }

        result.bit_packed_ = BitPackedArray(offsets.data(), null_bitmap,
                                             static_cast<uint32_t>(len), bw);
        return result;
    }

    /// Decode back to an Arrow array.
    /// Optimized: bulk unpack + ArrayData::Make (no per-element Builder).
    std::shared_ptr<arrow::Array> to_arrow() const {
        uint32_t len = bit_packed_.length();
        if (len == 0) {
            return arrow::MakeEmptyArray(
                arrow::TypeTraits<ArrowType>::type_singleton()).ValueOrDie();
        }

        // Step 1: Allocate Arrow value buffer
        int64_t buf_size = static_cast<int64_t>(len) * sizeof(NativeT);
        auto value_buf = arrow::AllocateBuffer(buf_size).ValueOrDie();
        auto* values = reinterpret_cast<NativeT*>(value_buf->mutable_data());

        // Step 2: Bulk unpack + reference value addition
        std::vector<UnsignedT> temp(len);
        bit_packed_.bulk_unpack_to(temp.data());
        for (uint32_t i = 0; i < len; ++i) {
            values[i] = reference_value_ + static_cast<NativeT>(temp[i]);
        }

        // Step 3: Null bitmap + direct construction
        auto null_buf = bit_packed_.null_bitmap_arrow_buffer();
        int64_t nc = bit_packed_.null_count();
        auto data = arrow::ArrayData::Make(
            arrow::TypeTraits<ArrowType>::type_singleton(),
            static_cast<int64_t>(len),
            {std::move(null_buf), std::move(value_buf)},
            nc);
        return arrow::MakeArray(data);
    }

    /// Serialize to bytes (binary-compatible with Rust).
    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> out;
        out.reserve(256);

        // Write IPC header
        LiquidIPCHeader header(LiquidDataType::Integer,
                               ArrowPhysicalType<ArrowType>::value);
        header.serialize(out);

        // Write reference value
        const uint8_t* ref_bytes = reinterpret_cast<const uint8_t*>(&reference_value_);
        out.insert(out.end(), ref_bytes, ref_bytes + sizeof(NativeT));

        // Pad to 8-byte alignment
        while (out.size() % 8 != 0) out.push_back(0);

        // Write BitPackedArray
        bit_packed_.serialize(out);
        return out;
    }

    /// Deserialize from bytes.
    static LiquidPrimitiveArray from_bytes(const uint8_t* data, size_t len) {
        auto header = LiquidIPCHeader::deserialize(data, len);
        if (header.logical_type_id != static_cast<uint16_t>(LiquidDataType::Integer)) {
            throw std::runtime_error("Expected Integer logical type");
        }

        LiquidPrimitiveArray result;
        size_t offset = LiquidIPCHeader::SIZE;
        if (offset + sizeof(NativeT) > len) {
            throw std::runtime_error("Buffer too small to read reference value");
        }
        std::memcpy(&result.reference_value_, data + offset, sizeof(NativeT));
        offset = align8(offset + sizeof(NativeT));

        result.bit_packed_ = BitPackedArray::deserialize(data + offset, len - offset);
        return result;
    }

    NativeT reference_value() const { return reference_value_; }
    uint32_t length() const { return bit_packed_.length(); }
    size_t memory_size() const { return bit_packed_.memory_size() + sizeof(*this); }
    uint8_t bit_width() const { return bit_packed_.bit_width(); }

private:
    NativeT reference_value_ = 0;
    BitPackedArray bit_packed_;
};

// Common type aliases (matching Rust)
using LiquidI32Array = LiquidPrimitiveArray<arrow::Int32Type>;
using LiquidI64Array = LiquidPrimitiveArray<arrow::Int64Type>;
using LiquidU32Array = LiquidPrimitiveArray<arrow::UInt32Type>;
using LiquidU64Array = LiquidPrimitiveArray<arrow::UInt64Type>;
using LiquidDate32Array = LiquidPrimitiveArray<arrow::Date32Type>;

// ═══════════════════════════════════════════════════════════════════════
// LiquidFloatArray<T>
//
// ALP (Adaptive Lossless floating-Point) encoding + BitPacking.
// Algorithm:
//   1. Sample values to find best exponents (e, f)
//   2. For each value: encoded = round(value * 10^e * 10^(-f))
//   3. Verify decode: if decode(encoded) != original → record as patch
//   4. Bit-pack encoded values (minus min) with determined bit width
//   5. Store patches (indices + original values) separately
//
// Serialization layout (identical to Rust):
//   [LiquidIPCHeader: 16B]
//   [reference_value: sizeof(SignedIntT)]
//   [padding to 8B]
//   [exponent_e: 1B] [exponent_f: 1B] [padding: 6B]
//   [patch_length: 8B]
//   [patch_indices: 8B * N] [patch_values: sizeof(FloatT) * N]
//   [padding to 8B]
//   [BitPackedArray data]
// ═══════════════════════════════════════════════════════════════════════

struct Exponents {
    uint8_t e = 0;
    uint8_t f = 0;
};

/// Precomputed powers of 10 for float32.
static const float F10_F32[] = {
    1.0f, 10.0f, 100.0f, 1000.0f, 10000.0f, 100000.0f,
    1000000.0f, 10000000.0f, 100000000.0f, 1000000000.0f, 10000000000.0f
};
static const float IF10_F32[] = {
    1.0f, 0.1f, 0.01f, 0.001f, 0.0001f, 0.00001f,
    0.000001f, 0.0000001f, 0.00000001f, 0.000000001f, 0.0000000001f
};

/// Precomputed powers of 10 for float64.
static const double F10_F64[] = {
    1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0, 1000000.0,
    10000000.0, 100000000.0, 1000000000.0, 10000000000.0,
    100000000000.0, 1000000000000.0, 10000000000000.0,
    100000000000000.0, 1000000000000000.0, 10000000000000000.0,
    100000000000000000.0, 1000000000000000000.0
};
static const double IF10_F64[] = {
    1.0, 0.1, 0.01, 0.001, 0.0001, 0.00001, 0.000001,
    0.0000001, 0.00000001, 0.000000001, 0.0000000001,
    0.00000000001, 0.000000000001, 0.0000000000001,
    0.00000000000001, 0.000000000000001, 0.0000000000000001,
    0.00000000000000001, 0.000000000000000001
};

/// ALP traits for f32 and f64.
template <typename T> struct ALPTraits;

template <> struct ALPTraits<float> {
    using SignedInt = int32_t;
    using UnsignedInt = uint32_t;
    static constexpr uint8_t FRACTIONAL_BITS = 23;
    static constexpr uint8_t MAX_EXPONENT = 10;
    static constexpr float SWEET = static_cast<float>(1 << 23) +
                                    static_cast<float>(1 << 22);
    static const float* f10() { return F10_F32; }
    static const float* if10() { return IF10_F32; }
    static constexpr PhysicalType PHYSICAL = PhysicalType::Float32;
    using ArrowType = arrow::FloatType;   // arrow::Float32Type does not exist; FloatType = float32
};

template <> struct ALPTraits<double> {
    using SignedInt = int64_t;
    using UnsignedInt = uint64_t;
    static constexpr uint8_t FRACTIONAL_BITS = 52;
    static constexpr uint8_t MAX_EXPONENT = 18;
    static constexpr double SWEET = static_cast<double>(1ULL << 52) +
                                     static_cast<double>(1ULL << 51);
    static const double* f10() { return F10_F64; }
    static const double* if10() { return IF10_F64; }
    static constexpr PhysicalType PHYSICAL = PhysicalType::Float64;
    using ArrowType = arrow::DoubleType;  // arrow::Float64Type does not exist; DoubleType = float64
};

template <typename FloatT>
class LiquidFloatArray {
public:
    using Traits = ALPTraits<FloatT>;
    using SignedInt = typename Traits::SignedInt;
    using UnsignedInt = typename Traits::UnsignedInt;
    using ArrowType = typename Traits::ArrowType;

    LiquidFloatArray() = default;

    /// ALP fast_round: round a float to the nearest integer
    static SignedInt fast_round(FloatT val) {
        return static_cast<SignedInt>((val + Traits::SWEET) - Traits::SWEET);
    }

    /// ALP encode a single value
    static SignedInt encode_single(FloatT val, const Exponents& exp) {
        return fast_round(val * Traits::f10()[exp.e] * Traits::if10()[exp.f]);
    }

    /// ALP decode a single encoded value
    static FloatT decode_single(SignedInt val, const Exponents& exp) {
        FloatT decoded = static_cast<FloatT>(val);
        return decoded * Traits::f10()[exp.f] * Traits::if10()[exp.e];
    }

    /// Encode an Arrow float array into Liquid format.
    static LiquidFloatArray from_arrow(
            const std::shared_ptr<arrow::Array>& array) {
        using ArrayT = typename arrow::TypeTraits<ArrowType>::ArrayType;
        auto typed = std::static_pointer_cast<ArrayT>(array);
        const int64_t len = typed->length();

        LiquidFloatArray result;

        // All-null check
        if (typed->null_count() == len) {
            result.exponent_ = {0, 0};
            result.reference_value_ = 0;
            std::vector<uint64_t> zeros(len, 0);
            std::vector<uint8_t> null_bits((len + 7) / 8, 0);
            result.bit_packed_ = BitPackedArray(zeros.data(), null_bits.data(),
                                                 static_cast<uint32_t>(len), 0);
            return result;
        }

        // Find best exponents by exhaustive search
        result.exponent_ = find_best_exponents(typed);

        // Encode all values
        std::vector<SignedInt> encoded(len);
        size_t patch_count = 0;
        for (int64_t i = 0; i < len; ++i) {
            FloatT v = typed->Value(i);
            encoded[i] = encode_single(v, result.exponent_);
            FloatT decoded = decode_single(encoded[i], result.exponent_);
            if (decoded != v) ++patch_count;
        }

        // Collect patches
        if (patch_count > 0) {
            result.patch_indices_.reserve(patch_count);
            result.patch_values_.reserve(patch_count);
            for (int64_t i = 0; i < len; ++i) {
                FloatT decoded = decode_single(encoded[i], result.exponent_);
                if (decoded != typed->Value(i)) {
                    result.patch_indices_.push_back(static_cast<uint64_t>(i));
                    result.patch_values_.push_back(typed->Value(i));
                }
            }

            // Fill patched positions with a fill value for better compression
            SignedInt fill = encoded[0];
            for (int64_t i = 0; i < len; ++i) {
                bool is_patch = false;
                for (auto pi : result.patch_indices_) {
                    if (pi == static_cast<uint64_t>(i)) { is_patch = true; break; }
                }
                if (!is_patch) { fill = encoded[i]; break; }
            }
            for (auto pi : result.patch_indices_) {
                encoded[pi] = fill;
            }
        }

        // Compute min and offsets
        SignedInt min_val = *std::min_element(encoded.begin(), encoded.end());
        SignedInt max_val = *std::max_element(encoded.begin(), encoded.end());
        result.reference_value_ = min_val;

        UnsignedInt range = static_cast<UnsignedInt>(max_val - min_val);
        uint8_t bw = get_bit_width(static_cast<uint64_t>(range));

        std::vector<uint64_t> offsets(len);
        for (int64_t i = 0; i < len; ++i) {
            offsets[i] = static_cast<uint64_t>(
                static_cast<UnsignedInt>(encoded[i] - min_val));
        }

        // Build null bitmap
        const uint8_t* null_bitmap = nullptr;
        std::vector<uint8_t> null_bits;
        if (typed->null_count() > 0 && typed->null_bitmap()) {
            size_t bitmap_bytes = (len + 7) / 8;
            if (typed->null_bitmap()->size() >= static_cast<int64_t>(bitmap_bytes)) {
                null_bits.assign(typed->null_bitmap()->data(),
                                 typed->null_bitmap()->data() + bitmap_bytes);
            } else if (typed->null_bitmap()) {
                // bitmap 存在但可能比预期小，安全拷贝
                size_t safe_bytes = std::min<size_t>(bitmap_bytes, typed->null_bitmap()->size());
                null_bits.resize(bitmap_bytes, 0xFF);
                std::memcpy(null_bits.data(), typed->null_bitmap()->data(), safe_bytes);
            }
            null_bitmap = null_bits.data();
        }

        result.bit_packed_ = BitPackedArray(offsets.data(), null_bitmap,
                                             static_cast<uint32_t>(len), bw);
        return result;
    }

    /// Decode back to an Arrow array.
    /// Optimized: single-pass decode + in-place patch + ArrayData::Make.
    std::shared_ptr<arrow::Array> to_arrow() const {
        uint32_t len = bit_packed_.length();
        if (len == 0) {
            return arrow::MakeEmptyArray(
                arrow::TypeTraits<ArrowType>::type_singleton()).ValueOrDie();
        }

        // Step 1: Allocate output buffer
        int64_t buf_size = static_cast<int64_t>(len) * sizeof(FloatT);
        auto value_buf = arrow::AllocateBuffer(buf_size).ValueOrDie();
        auto* values = reinterpret_cast<FloatT*>(value_buf->mutable_data());

        // Step 2: Bulk unpack offsets
        std::vector<UnsignedInt> temp(len);
        bit_packed_.bulk_unpack_to(temp.data());

        // Step 3: Single-pass decode
        for (uint32_t i = 0; i < len; ++i) {
            SignedInt encoded_val = reference_value_ +
                                    static_cast<SignedInt>(temp[i]);
            values[i] = decode_single(encoded_val, exponent_);
        }

        // Step 4: In-place patch (replaces entire second-pass rebuild)
        for (size_t j = 0; j < patch_indices_.size(); ++j) {
            values[patch_indices_[j]] = patch_values_[j];
        }

        // Step 5: Null bitmap + direct construction
        auto null_buf = bit_packed_.null_bitmap_arrow_buffer();
        int64_t nc = bit_packed_.null_count();
        auto data = arrow::ArrayData::Make(
            arrow::TypeTraits<ArrowType>::type_singleton(),
            static_cast<int64_t>(len),
            {std::move(null_buf), std::move(value_buf)},
            nc);
        return arrow::MakeArray(data);
    }

    /// Serialize to bytes (binary-compatible with Rust).
    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> out;
        out.reserve(512);

        // IPC header
        LiquidIPCHeader header(LiquidDataType::Float, Traits::PHYSICAL);
        header.serialize(out);

        // Reference value
        auto* ref_p = reinterpret_cast<const uint8_t*>(&reference_value_);
        out.insert(out.end(), ref_p, ref_p + sizeof(SignedInt));
        while (out.size() % 8 != 0) out.push_back(0);

        // Exponents (e, f) + 6 bytes padding
        out.push_back(exponent_.e);
        out.push_back(exponent_.f);
        for (int i = 0; i < 6; ++i) out.push_back(0);

        // Patch data
        uint64_t patch_len = patch_indices_.size();
        auto* plp = reinterpret_cast<const uint8_t*>(&patch_len);
        out.insert(out.end(), plp, plp + 8);

        if (patch_len > 0) {
            size_t actual_patch_bytes = patch_indices_.size() * sizeof(uint64_t);
            auto* pip = reinterpret_cast<const uint8_t*>(patch_indices_.data());
            out.insert(out.end(), pip, pip + actual_patch_bytes);
            auto* pvp = reinterpret_cast<const uint8_t*>(patch_values_.data());
            out.insert(out.end(), pvp, pvp + sizeof(FloatT) * patch_len);
        }
        while (out.size() % 8 != 0) out.push_back(0);

        // BitPackedArray
        bit_packed_.serialize(out);
        return out;
    }

    size_t memory_size() const {
        return bit_packed_.memory_size() +
               patch_indices_.size() * 8 +
               patch_values_.size() * sizeof(FloatT) +
               sizeof(*this);
    }

    size_t patch_count() const { return patch_indices_.size(); }
    uint8_t bit_width() const { return bit_packed_.bit_width(); }
    uint32_t length() const { return bit_packed_.length(); }
    const std::vector<uint64_t>& patch_indices() const { return patch_indices_; }
    const std::vector<FloatT>& patch_values() const { return patch_values_; }
    Exponents exponents() const { return exponent_; }
    SignedInt reference_value() const { return reference_value_; }

    /// Deserialize from bytes (matches Rust LiquidFloatArray::from_bytes).
    static LiquidFloatArray from_bytes(const uint8_t* data, size_t len) {
        auto header = LiquidIPCHeader::deserialize(data, len);
        if (header.logical_type_id != static_cast<uint16_t>(LiquidDataType::Float)) {
            throw std::runtime_error("Expected Float logical type");
        }

        LiquidFloatArray result;
        size_t next = LiquidIPCHeader::SIZE;

        // Read reference value
        if (next + sizeof(SignedInt) > len) {
            throw std::runtime_error("Buffer too small to read float reference value");
        }
        std::memcpy(&result.reference_value_, data + next, sizeof(SignedInt));
        next = (next + sizeof(SignedInt) + 7) & ~static_cast<size_t>(7);

        // Read exponents (e, f) + 6 padding bytes
        if (next + 8 > len) {
            throw std::runtime_error("Buffer too small for exponents");
        }
        result.exponent_.e = data[next];
        result.exponent_.f = data[next + 1];
        next += 8;

        // Read patch_length (u64)
        if (next + 8 > len) {
            throw std::runtime_error("Buffer too small for patch length");
        }
        uint64_t patch_length = 0;
        std::memcpy(&patch_length, data + next, 8);
        next += 8;

        // Read patch data
        if (patch_length > 0) {
            size_t idx_bytes = static_cast<size_t>(patch_length) * sizeof(uint64_t);
            size_t val_bytes = static_cast<size_t>(patch_length) * sizeof(FloatT);
            if (next + idx_bytes + val_bytes > len) {
                throw std::runtime_error("Buffer too small for patch data");
            }
            result.patch_indices_.resize(patch_length);
            std::memcpy(result.patch_indices_.data(), data + next, idx_bytes);
            next += idx_bytes;

            result.patch_values_.resize(patch_length);
            std::memcpy(result.patch_values_.data(), data + next, val_bytes);
            next += val_bytes;
        }

        // Align to 8 bytes for BitPackedArray
        next = (next + 7) & ~static_cast<size_t>(7);

        // Deserialize BitPackedArray
        if (next < len) {
            result.bit_packed_ = BitPackedArray::deserialize(data + next, len - next);
        }

        return result;
    }

private:
    /// Find the best ALP exponents by exhaustive search.
    static Exponents find_best_exponents(
            const std::shared_ptr<typename arrow::TypeTraits<ArrowType>::ArrayType>& arr) {
        Exponents best{0, 0};
        size_t min_size = std::numeric_limits<size_t>::max();
        const int64_t len = arr->length();

        // Sample for large arrays
        std::vector<FloatT> sample;
        if (len > 1024) {
            size_t step = len / 1024;
            for (int64_t i = 0; i < len; i += step) {
                if (!arr->IsNull(i)) sample.push_back(arr->Value(i));
            }
        }

        for (uint8_t e = 0; e < Traits::MAX_EXPONENT; ++e) {
            for (uint8_t f = 0; f < e; ++f) {
                Exponents exp{e, f};
                size_t patches = 0;
                SignedInt min_enc = std::numeric_limits<SignedInt>::max();
                SignedInt max_enc = std::numeric_limits<SignedInt>::min();

                auto& source = sample.empty() ? *arr : *arr;  // use full for small
                for (int64_t i = 0; i < len; i += (len > 1024 ? len / 1024 : 1)) {
                    if (arr->IsNull(i)) continue;
                    FloatT v = arr->Value(i);
                    SignedInt enc = encode_single(v, exp);
                    FloatT dec = decode_single(enc, exp);
                    if (dec != v) ++patches;
                    if (enc < min_enc) min_enc = enc;
                    if (enc > max_enc) max_enc = enc;
                }

                UnsignedInt range = static_cast<UnsignedInt>(max_enc - min_enc);
                uint8_t bw = get_bit_width(static_cast<uint64_t>(range));
                size_t est_size = (static_cast<size_t>(len) * bw + 7) / 8 +
                                  patches * (8 + sizeof(FloatT));
                if (est_size < min_size) {
                    min_size = est_size;
                    best = exp;
                }
            }
        }
        return best;
    }

    Exponents exponent_;
    SignedInt reference_value_ = 0;
    BitPackedArray bit_packed_;
    std::vector<uint64_t> patch_indices_;
    std::vector<FloatT> patch_values_;
};

using LiquidFloat32Array = LiquidFloatArray<float>;
using LiquidFloat64Array = LiquidFloatArray<double>;

}  // namespace liquid_cache
