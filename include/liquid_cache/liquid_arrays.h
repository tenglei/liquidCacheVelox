// liquid_cache/liquid_arrays.h
// LiquidPrimitiveArray and LiquidFloatArray - C++ implementation
// Binary-compatible with the Rust liquid-cache serialization format.
#pragma once

#include <arrow/util/logging.h>  // MUST be first: defines ARROW_CHECK_OK macro
#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/type_traits.h>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "liquid_cache/bit_packed_array.h"
#include "liquid_cache/ipc_header.h"

#ifdef LIQUID_ENABLE_VELOX
namespace facebook::velox {
class BaseVector;
using VectorPtr = std::shared_ptr<BaseVector>;
namespace memory { class MemoryPool; }
}  // namespace facebook::velox
#endif

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
    // Return 1 for zero to match Rust's NonZero<u8> convention.
    // BitPackedArray handles bw=0 as a special "all-zero" case
    // internally (packed_data_.clear()); this function's contract is
    // to return the minimum bit width needed, which is 1 even for 0.
    if (value == 0) return 1;
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
template <> struct ArrowPhysicalType<arrow::TimestampType> { static constexpr PhysicalType value = PhysicalType::TimestampMicrosecond; };

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

// Type trait: check if ArrowType has TypeTraits<T>::type_singleton().
// TimestampType needs a time unit parameter, so it does NOT.
template <typename, typename = void>
struct has_arrow_type_singleton : std::false_type {};

template <typename T>
struct has_arrow_type_singleton<T,
    std::void_t<decltype(arrow::TypeTraits<T>::type_singleton())>
> : std::true_type {};

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

        // Compute offsets as unsigned.  Cast to unsigned BEFORE subtraction
        // to avoid signed-integer-overflow UB when min is near type::min
        // and max is near type::max (e.g. INT32_MIN..INT32_MAX).
        // Unsigned arithmetic wraps modulo 2^N by the C++ standard.
        UnsignedT umin = static_cast<UnsignedT>(min_val);
        UnsignedT umax = static_cast<UnsignedT>(max_val);
        UnsignedT range = umax - umin;
        uint8_t bw = get_bit_width(static_cast<uint64_t>(range));

        std::vector<uint64_t> offsets(len);
        for (int64_t i = 0; i < len; ++i) {
            NativeT v = typed->Value(i);
            UnsignedT uv = static_cast<UnsignedT>(v);
            offsets[i] = static_cast<uint64_t>(uv - umin);
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
        // Store original Arrow type for TimestampType etc. which need unit info
        result.type_ = array->type();
        return result;
    }

    /// Decode back to an Arrow array.
    /// Optimized: bulk unpack + ArrayData::Make (no per-element Builder).
    std::shared_ptr<arrow::Array> to_arrow() const {
        uint32_t len = bit_packed_.length();
        std::shared_ptr<arrow::DataType> arrow_type;
        if (type_) {
            arrow_type = type_;
        } else if constexpr (has_arrow_type_singleton<ArrowType>::value) {
            arrow_type = arrow::TypeTraits<ArrowType>::type_singleton();
        }
        if (len == 0) {
            return arrow::MakeEmptyArray(arrow_type).ValueOrDie();
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
            arrow_type,
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
    const BitPackedArray& bit_packed() const { return bit_packed_; }

#ifdef LIQUID_ENABLE_VELOX
    /// Decode directly to Velox FlatVector.
    facebook::velox::VectorPtr to_velox(
        facebook::velox::memory::MemoryPool* pool) const;
#endif

private:
    NativeT reference_value_ = 0;
    BitPackedArray bit_packed_;
    std::shared_ptr<arrow::DataType> type_;
};

// Common type aliases (matching Rust)
using LiquidI32Array = LiquidPrimitiveArray<arrow::Int32Type>;
using LiquidI64Array = LiquidPrimitiveArray<arrow::Int64Type>;
using LiquidU32Array = LiquidPrimitiveArray<arrow::UInt32Type>;
using LiquidU64Array = LiquidPrimitiveArray<arrow::UInt64Type>;
using LiquidDate32Array = LiquidPrimitiveArray<arrow::Date32Type>;
// ═══════════════════════════════════════════════════════════════════════
// Helper: saturating i64 prediction helpers
// ═══════════════════════════════════════════════════════════════════════

inline uint64_t predict_u64_saturated(double pred, uint64_t max_u64) {
    if (std::isnan(pred) || std::isinf(pred) || pred <= 0.0) return 0;
    if (pred >= static_cast<double>(max_u64)) return max_u64;
    return static_cast<uint64_t>(std::llround(pred));
}

inline int64_t predict_i64_saturated(double pred, int64_t min_i64, int64_t max_i64) {
    if (std::isnan(pred) || std::isinf(pred)) return 0;
    if (pred <= static_cast<double>(min_i64)) return min_i64;
    if (pred >= static_cast<double>(max_i64)) return max_i64;
    return static_cast<int64_t>(std::llround(pred));
}

// ═══════════════════════════════════════════════════════════════════════
// L-infinity linear regression helpers
// ═══════════════════════════════════════════════════════════════════════

inline void range_stats(const std::vector<double>& values,
                        const std::vector<uint32_t>& idxs,
                        double m,
                        double& min_s, uint32_t& i_min,
                        double& max_s, uint32_t& i_max) {
    min_s = std::numeric_limits<double>::infinity();
    max_s = -std::numeric_limits<double>::infinity();
    for (size_t k = 0; k < values.size(); ++k) {
        double s = values[k] - m * static_cast<double>(idxs[k]);
        if (s < min_s) { min_s = s; i_min = idxs[k]; }
        if (s > max_s) { max_s = s; i_max = idxs[k]; }
    }
}

inline std::pair<double, double> fit_linf(
        const std::vector<double>& values,
        const std::vector<uint32_t>& idxs) {
    size_t n = values.size();
    if (n == 0) return {0.0, 0.0};
    if (n == 1) return {values[0], 0.0};

    double slope_min = std::numeric_limits<double>::infinity();
    double slope_max = -std::numeric_limits<double>::infinity();
    for (size_t k = 1; k < n; ++k) {
        double di = static_cast<double>(idxs[k] - idxs[k - 1]);
        if (di > 0.0) {
            double s = (values[k] - values[k - 1]) / di;
            if (s < slope_min) slope_min = s;
            if (s > slope_max) slope_max = s;
        }
    }
    if (!std::isfinite(slope_min) || !std::isfinite(slope_max)) {
        slope_min = 0.0;
        slope_max = 0.0;
    }

    double lo = std::min(slope_min, slope_max);
    double hi = std::max(slope_min, slope_max);
    if (std::abs(hi - lo) < 1e-12) {
        double pad = (std::abs(hi) < 1.0) ? 1.0 : std::abs(hi) * 1e-6;
        lo -= pad;
        hi += pad;
    }

    constexpr int MAX_ITERS = 8;
    for (int iter = 0; iter < MAX_ITERS; ++iter) {
        double m = 0.5 * (lo + hi);
        double min_s, max_s;
        uint32_t i_min, i_max;
        range_stats(values, idxs, m, min_s, i_min, max_s, i_max);
        int64_t g = static_cast<int64_t>(i_min) - static_cast<int64_t>(i_max);
        if (g > 0) { hi = m; }
        else if (g < 0) { lo = m; }
        else { lo = m; hi = m; break; }
        if (std::abs(hi - lo) < 1e-12) break;
    }

    double m = 0.5 * (lo + hi);
    double min_s, max_s;
    uint32_t i_min_d, i_max_d;
    range_stats(values, idxs, m, min_s, i_min_d, max_s, i_max_d);
    double b = 0.5 * (max_s + min_s);
    return {b, m};
}

// ═══════════════════════════════════════════════════════════════════════
// LiquidLinearIntegerArray<T>
//
// Linear-model integer array: value[i] = intercept + slope*i + residual[i].
// Residuals are stored as a LiquidPrimitiveArray<Int64Type>.
// Uses L-infinity (Chebyshev) regression for model fitting.
// Recommended for monotonic/near-linear sequences only.
//
// Serialization (matches Rust):
//   [LiquidIPCHeader: 16B] (type=LinearInteger, physical=original_type)
//   [intercept: f64, 8B LE]
//   [slope:     f64, 8B LE]
//   [padding to 8B]
//   [LiquidPrimitiveArray<Int64Type> residuals]
// ═══════════════════════════════════════════════════════════════════════

template <typename ArrowType>
class LiquidLinearIntegerArray {
public:
    using NativeT = typename ArrowType::c_type;
    static constexpr bool IS_UNSIGNED = !std::is_signed_v<NativeT>;

    LiquidLinearIntegerArray() = default;

    static LiquidLinearIntegerArray from_arrow(
            const std::shared_ptr<arrow::Array>& array) {
        using ArrayT = typename arrow::TypeTraits<ArrowType>::ArrayType;
        auto typed = std::static_pointer_cast<ArrayT>(array);
        const int64_t len = typed->length();
        LiquidLinearIntegerArray result;

        if (typed->null_count() == len) {
            auto null_arr = arrow::MakeArrayOfNull(arrow::int64(), len).ValueOrDie();
            result.residuals_ = LiquidPrimitiveArray<arrow::Int64Type>::from_arrow(null_arr);
            return result;
        }

        const auto* vals = typed->raw_values();
        const auto* nulls = typed->null_bitmap_data();
        int64_t off = typed->offset();
        size_t nn = static_cast<size_t>(len - typed->null_count());
        std::vector<double> nn_vals; nn_vals.reserve(nn);
        std::vector<uint32_t> nn_idxs; nn_idxs.reserve(nn);
        for (int64_t i = 0; i < len; ++i) {
            if (!nulls || arrow::bit_util::GetBit(nulls, off + i)) {
                nn_vals.push_back(static_cast<double>(vals[i]));
                nn_idxs.push_back(static_cast<uint32_t>(i));
            }
        }

        auto [b, m_slope] = fit_linf(nn_vals, nn_idxs);
        result.intercept_ = b;
        result.slope_ = m_slope;

        std::vector<int64_t> residuals;
        residuals.reserve(static_cast<size_t>(len));
        uint64_t omin_u = UINT64_MAX, omax_u = 0;
        int64_t omin_i = INT64_MAX, omax_i = INT64_MIN;
        int64_t rmin = INT64_MAX, rmax = INT64_MIN;

        if constexpr (IS_UNSIGNED) {
            constexpr uint64_t mx = static_cast<uint64_t>(std::numeric_limits<NativeT>::max());
            for (int64_t i = 0; i < len; ++i) {
                bool ok = !nulls || arrow::bit_util::GetBit(nulls, off + i);
                if (ok) {
                    uint64_t vu = static_cast<uint64_t>(vals[i]);
                    if (vu < omin_u) omin_u = vu;
                    if (vu > omax_u) omax_u = vu;
                    double pr = m_slope * i + b;
                    uint64_t p = predict_u64_saturated(pr, mx);
                    __int128_t d = static_cast<__int128_t>(vu) - static_cast<__int128_t>(p);
                    int64_t r = (d > INT64_MAX) ? INT64_MAX
                              : (d < INT64_MIN) ? INT64_MIN : static_cast<int64_t>(d);
                    if (r < rmin) rmin = r; if (r > rmax) rmax = r;
                    residuals.push_back(r);
                } else { residuals.push_back(0); }
            }
        } else {
            constexpr int64_t mi = static_cast<int64_t>(std::numeric_limits<NativeT>::min());
            constexpr int64_t ma = static_cast<int64_t>(std::numeric_limits<NativeT>::max());
            for (int64_t i = 0; i < len; ++i) {
                bool ok = !nulls || arrow::bit_util::GetBit(nulls, off + i);
                if (ok) {
                    int64_t vi = static_cast<int64_t>(vals[i]);
                    if (vi < omin_i) omin_i = vi;
                    if (vi > omax_i) omax_i = vi;
                    double pr = m_slope * i + b;
                    int64_t p = predict_i64_saturated(pr, mi, ma);
                    __int128_t d = static_cast<__int128_t>(vi) - static_cast<__int128_t>(p);
                    int64_t r = (d > INT64_MAX) ? INT64_MAX
                              : (d < INT64_MIN) ? INT64_MIN : static_cast<int64_t>(d);
                    if (r < rmin) rmin = r; if (r > rmax) rmax = r;
                    residuals.push_back(r);
                } else { residuals.push_back(0); }
            }
        }

        uint64_t rw = static_cast<uint64_t>(
            static_cast<__int128_t>(rmax) - static_cast<__int128_t>(rmin));
        uint64_t ow = IS_UNSIGNED
            ? (omax_u - omin_u)
            : static_cast<uint64_t>(static_cast<__int128_t>(omax_i) - static_cast<__int128_t>(omin_i));
        if (rw >= ow) {
            result.intercept_ = 0.0; result.slope_ = 0.0;
            residuals.clear();
            if constexpr (IS_UNSIGNED) {
                constexpr uint64_t mx = static_cast<uint64_t>(std::numeric_limits<NativeT>::max());
                uint64_t msk = (mx < static_cast<uint64_t>(INT64_MAX)) ? mx : static_cast<uint64_t>(INT64_MAX);
                for (int64_t i = 0; i < len; ++i) {
                    bool ok = !nulls || arrow::bit_util::GetBit(nulls, off + i);
                    if (ok) residuals.push_back(static_cast<int64_t>(static_cast<uint64_t>(vals[i]) & msk));
                    else residuals.push_back(0);
                }
            } else {
                for (int64_t i = 0; i < len; ++i) {
                    bool ok = !nulls || arrow::bit_util::GetBit(nulls, off + i);
                    if (ok) residuals.push_back(static_cast<int64_t>(vals[i]));
                    else residuals.push_back(0);
                }
            }
        }

        arrow::Int64Builder bld;
        LIQUID_ARROW_OK(bld.Reserve(len));
        for (int64_t i = 0; i < len; ++i) {
            bool ok = !nulls || arrow::bit_util::GetBit(nulls, off + i);
            if (ok) LIQUID_ARROW_OK(bld.Append(residuals[i]));
            else LIQUID_ARROW_OK(bld.AppendNull());
        }
        result.residuals_ = LiquidPrimitiveArray<arrow::Int64Type>::from_arrow(
            bld.Finish().ValueOrDie());
        return result;
    }

    std::shared_ptr<arrow::Array> to_arrow() const {
        auto ra = residuals_.to_arrow();
        auto rt = std::static_pointer_cast<arrow::Int64Array>(ra);
        int64_t len = rt->length();
        if (len == 0) return arrow::MakeEmptyArray(
            arrow::TypeTraits<ArrowType>::type_singleton()).ValueOrDie();

        const int64_t* rd = rt->raw_values();
        const auto* rn = rt->null_bitmap_data();
        int64_t ro = rt->offset();

        typename arrow::TypeTraits<ArrowType>::BuilderType bld;
        LIQUID_ARROW_OK(bld.Reserve(len));

        if constexpr (IS_UNSIGNED) {
            constexpr uint64_t mx = static_cast<uint64_t>(std::numeric_limits<NativeT>::max());
            for (int64_t i = 0; i < len; ++i) {
                bool ok = !rn || arrow::bit_util::GetBit(rn, ro + i);
                if (ok) {
                    double pr = slope_ * i + intercept_;
                    uint64_t p = predict_u64_saturated(pr, mx);
                    __int128_t s = static_cast<__int128_t>(p) + static_cast<__int128_t>(rd[i]);
                    NativeT v = static_cast<NativeT>(std::clamp<__int128_t>(s, __int128_t{0}, mx));
                    LIQUID_ARROW_OK(bld.Append(v));
                } else { LIQUID_ARROW_OK(bld.AppendNull()); }
            }
        } else {
            constexpr int64_t mi = static_cast<int64_t>(std::numeric_limits<NativeT>::min());
            constexpr int64_t ma = static_cast<int64_t>(std::numeric_limits<NativeT>::max());
            for (int64_t i = 0; i < len; ++i) {
                bool ok = !rn || arrow::bit_util::GetBit(rn, ro + i);
                if (ok) {
                    double pr = slope_ * i + intercept_;
                    int64_t p = predict_i64_saturated(pr, mi, ma);
                    __int128_t s = static_cast<__int128_t>(p) + static_cast<__int128_t>(rd[i]);
                    NativeT v = static_cast<NativeT>(std::clamp<__int128_t>(s,
                        static_cast<__int128_t>(mi), static_cast<__int128_t>(ma)));
                    LIQUID_ARROW_OK(bld.Append(v));
                } else { LIQUID_ARROW_OK(bld.AppendNull()); }
            }
        }
        return bld.Finish().ValueOrDie();
    }

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> out; out.reserve(256);
        LiquidIPCHeader hdr(LiquidDataType::LinearInteger, ArrowPhysicalType<ArrowType>::value);
        hdr.serialize(out);
        uint64_t ib; std::memcpy(&ib, &intercept_, 8);
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(&ib),
                   reinterpret_cast<const uint8_t*>(&ib) + 8);
        uint64_t sb; std::memcpy(&sb, &slope_, 8);
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(&sb),
                   reinterpret_cast<const uint8_t*>(&sb) + 8);
        while (out.size() % 8 != 0) out.push_back(0);
        auto rb = residuals_.to_bytes();
        out.insert(out.end(), rb.begin(), rb.end());
        return out;
    }

    static LiquidLinearIntegerArray from_bytes(const uint8_t* data, size_t len) {
        auto hdr = LiquidIPCHeader::deserialize(data, len);
        if (hdr.logical_type_id != static_cast<uint16_t>(LiquidDataType::LinearInteger))
            throw std::runtime_error("Expected LinearInteger logical type");
        LiquidLinearIntegerArray r;
        size_t off = LiquidIPCHeader::SIZE;
        uint64_t ib; std::memcpy(&ib, data + off, 8);
        std::memcpy(&r.intercept_, &ib, 8); off += 8;
        uint64_t sb; std::memcpy(&sb, data + off, 8);
        std::memcpy(&r.slope_, &sb, 8); off += 8;
        off = align8(off);
        r.residuals_ = LiquidPrimitiveArray<arrow::Int64Type>::from_bytes(data + off, len - off);
        return r;
    }

    uint32_t length() const { return residuals_.length(); }
    size_t memory_size() const { return residuals_.memory_size() + sizeof(*this); }
    double intercept() const { return intercept_; }
    double slope() const { return slope_; }
    uint8_t bit_width() const { return residuals_.bit_width(); }

#ifdef LIQUID_ENABLE_VELOX
    facebook::velox::VectorPtr to_velox(
        facebook::velox::memory::MemoryPool* pool) const;
#endif

private:
    double intercept_ = 0.0;
    double slope_ = 0.0;
    LiquidPrimitiveArray<arrow::Int64Type> residuals_;
};

using LiquidLinearI32Array = LiquidLinearIntegerArray<arrow::Int32Type>;
using LiquidLinearI64Array = LiquidLinearIntegerArray<arrow::Int64Type>;
using LiquidLinearU32Array = LiquidLinearIntegerArray<arrow::UInt32Type>;
using LiquidLinearU64Array = LiquidLinearIntegerArray<arrow::UInt64Type>;
using LiquidLinearDate32Array = LiquidLinearIntegerArray<arrow::Date32Type>;
using LiquidLinearDate64Array = LiquidLinearIntegerArray<arrow::Date64Type>;



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

/// Squeeze policy for float arrays.
enum class FloatSqueezePolicy : uint8_t {
    Quantize = 0,
};

/// Comparison operators for predicate pushdown on squeezed arrays.
enum class Operator : uint8_t {
    Eq, NotEq, Lt, LtEq, Gt, GtEq
};

/// Abstract interface for reading squeezed data from persistent storage.
class SqueezeIoHandler {
public:
    virtual ~SqueezeIoHandler() = default;
    virtual std::vector<uint8_t> read(uint64_t offset, uint64_t length) = 0;
};

// Forward declaration for squeeze() return type.
template <typename FloatT> class LiquidFloatQuantizedArray;
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
            result.squeeze_policy_ = FloatSqueezePolicy::Quantize;
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

            // Fill patched positions with a fill value for better compression.
            // Guard: only fill if there is at least one non-patch value.
            // If all values are patches, filling with garbage would corrupt
            // the bit-packing range and produce wrong decoded values.
            if (patch_count > 0 && static_cast<size_t>(len) > patch_count) {
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

        // Build null bitmap (account for sliced-array offset, same as LiquidPrimitiveArray)
        const uint8_t* null_bitmap = nullptr;
        std::vector<uint8_t> null_bits;
        if (typed->null_count() > 0 && typed->null_bitmap()) {
            size_t bitmap_bytes = (len + 7) / 8;
            const uint8_t* bitmap_src = typed->null_bitmap()->data() + typed->offset() / 8;
            null_bits.assign(bitmap_src, bitmap_src + bitmap_bytes);
            null_bitmap = null_bits.data();
        }

        result.bit_packed_ = BitPackedArray(offsets.data(), null_bitmap,
                                             static_cast<uint32_t>(len), bw);
        result.squeeze_policy_ = FloatSqueezePolicy::Quantize;
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
    FloatSqueezePolicy squeeze_policy() const { return squeeze_policy_; }

    /// Squeeze to reduce in-memory footprint. Returns quantized array + disk bytes.
    /// When bit_width >= 8, halves the bit width and stores full precision on disk.
    /// Defined after LiquidFloatQuantizedArray<T> (needs complete type).
    std::optional<std::pair<LiquidFloatQuantizedArray<FloatT>, std::vector<uint8_t> > >
    squeeze(std::shared_ptr<SqueezeIoHandler> io) const;

#ifdef LIQUID_ENABLE_VELOX
    /// Decode directly to Velox FlatVector via ALP + BitPacking + Patching.
    facebook::velox::VectorPtr to_velox(
        facebook::velox::memory::MemoryPool* pool) const;
#endif

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

        result.squeeze_policy_ = FloatSqueezePolicy::Quantize;
        return result;
    }

private:
    /// Find the best ALP exponents by exhaustive search.
    /// Matches Rust LiquidFloatArray::get_best_exponents: samples values
    /// for large arrays, then evaluates all (e, f) pairs over the sample.
    static Exponents find_best_exponents(
            const std::shared_ptr<typename arrow::TypeTraits<ArrowType>::ArrayType>& arr) {
        Exponents best{0, 0};
        size_t min_size = std::numeric_limits<size_t>::max();
        const int64_t len = arr->length();

        // Collect non-null values for exponent evaluation.
        // For len <= 1024: collect all non-null values.
        // For len > 1024: collect a uniform sample of up to ~1024 non-null values.
        std::vector<FloatT> sample;
        if (len > 1024) {
            size_t step = std::max<size_t>(len / 1024, 1);
            for (int64_t i = 0; i < len; i += static_cast<int64_t>(step)) {
                if (!arr->IsNull(i)) sample.push_back(arr->Value(i));
            }
        } else {
            for (int64_t i = 0; i < len; ++i) {
                if (!arr->IsNull(i)) sample.push_back(arr->Value(i));
            }
        }

        // Evaluate each (e, f) pair over the collected sample
        for (uint8_t e = 0; e < Traits::MAX_EXPONENT; ++e) {
            for (uint8_t f = 0; f < e; ++f) {
                Exponents exp{e, f};
                size_t patches = 0;
                SignedInt min_enc = std::numeric_limits<SignedInt>::max();
                SignedInt max_enc = std::numeric_limits<SignedInt>::min();

                for (FloatT v : sample) {
                    SignedInt enc = encode_single(v, exp);
                    FloatT dec = decode_single(enc, exp);
                    if (dec != v) ++patches;
                    if (enc < min_enc) min_enc = enc;
                    if (enc > max_enc) max_enc = enc;
                }

                UnsignedInt range = static_cast<UnsignedInt>(max_enc - min_enc);
                uint8_t bw = get_bit_width(static_cast<uint64_t>(range));
                // Estimate is scaled to full length for fair comparison
                size_t est_size = (static_cast<size_t>(len) * bw + 7) / 8 +
                                  static_cast<size_t>(static_cast<double>(patches) / sample.size() * len) *
                                  (8 + sizeof(FloatT));
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
    FloatSqueezePolicy squeeze_policy_ = FloatSqueezePolicy::Quantize;
};


// ═══════════════════════════════════════════════════════════════════════
// LiquidFloatQuantizedArray<T>
//
// Squeezed (half-resolution) float array for memory-to-disk hybrid storage.
// Created by LiquidFloatArray::squeeze().
// ═══════════════════════════════════════════════════════════════════════

template <typename FloatT>
class LiquidFloatQuantizedArray {
public:
    using Traits = ALPTraits<FloatT>;
    using SignedInt = typename Traits::SignedInt;
    using UnsignedInt = typename Traits::UnsignedInt;
    using ArrowType = typename Traits::ArrowType;

    LiquidFloatQuantizedArray() = default;

    /// Hydrate from disk and decode to Arrow array.
    std::shared_ptr<arrow::Array> to_arrow() const {
        auto bytes = io_->read(disk_offset_, disk_length_);
        auto liquid = LiquidFloatArray<FloatT>::from_bytes(bytes.data(), bytes.size());
        return liquid.to_arrow();
    }

    uint32_t length() const { return quantized_.length(); }

    size_t memory_size() const {
        return quantized_.memory_size() +
               patch_indices_.size() * 8 +
               patch_values_.size() * sizeof(FloatT) +
               sizeof(*this);
    }

    /// Predicate helpers: given [lo, hi] bucket range and literal k,
    /// return true/false if definite, nullopt if ambiguous.
    static std::optional<bool> handle_eq(FloatT lo, FloatT hi, FloatT k) {
        if (k < lo || k > hi) return false; return std::nullopt;
    }
    static std::optional<bool> handle_neq(FloatT lo, FloatT hi, FloatT k) {
        if (k < lo || k > hi) return true; return std::nullopt;
    }
    static std::optional<bool> handle_lt(FloatT lo, FloatT hi, FloatT k) {
        if (k <= lo) return false; if (hi < k) return true; return std::nullopt;
    }
    static std::optional<bool> handle_lteq(FloatT lo, FloatT hi, FloatT k) {
        if (k < lo) return false; if (hi <= k) return true; return std::nullopt;
    }
    static std::optional<bool> handle_gt(FloatT lo, FloatT hi, FloatT k) {
        if (k < lo) return true; if (hi <= k) return false; return std::nullopt;
    }
    static std::optional<bool> handle_gteq(FloatT lo, FloatT hi, FloatT k) {
        if (k <= lo) return true; if (hi < k) return false; return std::nullopt;
    }

    /// Try to evaluate a comparison predicate using only the quantized
    /// representation. Returns a BooleanArray if fully resolved,
    /// or nullptr if NeedsBacking (requires full data from disk).
    std::shared_ptr<arrow::BooleanArray> try_eval_predicate(
            Operator op, FloatT literal) const {
        uint32_t len = quantized_.length();

        using CompFn = std::optional<bool> (*)(FloatT, FloatT, FloatT);
        CompFn comp = nullptr;
        switch (op) {
            case Operator::Eq:    comp = handle_eq;   break;
            case Operator::NotEq: comp = handle_neq;  break;
            case Operator::Lt:    comp = handle_lt;   break;
            case Operator::LtEq:  comp = handle_lteq; break;
            case Operator::Gt:    comp = handle_gt;   break;
            case Operator::GtEq:  comp = handle_gteq; break;
        }

        std::vector<UnsignedInt> unpacked(len);
        quantized_.bulk_unpack_to(unpacked.data());

        SignedInt q_min = reference_value_ >> bucket_width_;
        std::vector<uint8_t> result_bits((len + 7) / 8, 0);
        bool all_decided = true;
        size_t next_patch = 0;
        bool ignore = patch_indices_.empty();

        for (uint32_t i = 0; i < len; ++i) {
            if (quantized_.is_null(i)) continue;
            if (!ignore && next_patch < patch_indices_.size() &&
                patch_indices_[next_patch] == i) {
                ++next_patch;
                if (next_patch == patch_indices_.size()) ignore = true;
                continue;
            }

            SignedInt val = q_min + static_cast<SignedInt>(unpacked[i]);
            UnsignedInt uv = static_cast<UnsignedInt>(val);
            SignedInt loe = static_cast<SignedInt>(
                (uv << bucket_width_) + static_cast<UnsignedInt>(reference_value_));
            UnsignedInt uvp1 = uv + 1;
            SignedInt hie = static_cast<SignedInt>(
                (uvp1 << bucket_width_) + static_cast<UnsignedInt>(reference_value_));

            FloatT lo = LiquidFloatArray<FloatT>::decode_single(loe, exponent_);
            FloatT hi = LiquidFloatArray<FloatT>::decode_single(hie, exponent_);
            auto d = comp(lo, hi, literal);
            if (d.has_value()) {
                if (d.value()) result_bits[i / 8] |= (1 << (i % 8));
            } else {
                all_decided = false; break;
            }
        }

        if (!all_decided) return nullptr;

        for (size_t pi = 0; pi < patch_indices_.size(); ++pi) {
            uint64_t idx = patch_indices_[pi];
            FloatT pv = patch_values_[pi];
            bool ok = false;
            switch (op) {
                case Operator::Eq:    ok = pv == literal; break;
                case Operator::NotEq: ok = pv != literal; break;
                case Operator::Lt:    ok = pv < literal;  break;
                case Operator::LtEq:  ok = pv <= literal; break;
                case Operator::Gt:    ok = pv > literal;  break;
                case Operator::GtEq:  ok = pv >= literal; break;
            }
            if (ok) result_bits[idx / 8] |= (1 << (idx % 8));
        }

        auto vb = arrow::AllocateBuffer((len + 7) / 8).ValueOrDie();
        std::memcpy(vb->mutable_data(), result_bits.data(), (len + 7) / 8);
        auto nb = quantized_.null_bitmap_arrow_buffer();
        return std::make_shared<arrow::BooleanArray>(
            static_cast<int64_t>(len), std::move(vb), std::move(nb),
            static_cast<int64_t>(quantized_.null_count()));
    }

private:
    friend class LiquidFloatArray<FloatT>;

    Exponents exponent_;
    BitPackedArray quantized_;
    SignedInt reference_value_ = 0;
    uint8_t bucket_width_ = 0;
    uint64_t disk_offset_ = 0;
    uint64_t disk_length_ = 0;
    std::shared_ptr<SqueezeIoHandler> io_;
    std::vector<uint64_t> patch_indices_;
    std::vector<FloatT> patch_values_;
};

/// Out-of-line definition of squeeze() — requires complete
/// LiquidFloatQuantizedArray<T> type to construct the result.
template <typename FloatT>
std::optional<std::pair<LiquidFloatQuantizedArray<FloatT>, std::vector<uint8_t> > >
LiquidFloatArray<FloatT>::squeeze(std::shared_ptr<SqueezeIoHandler> io) const {
    uint8_t orig_bw = bit_packed_.bit_width();
    if (orig_bw < 8) return std::nullopt;

    uint8_t new_bw = orig_bw / 2;
    uint8_t shift = orig_bw - new_bw;
    auto full_bytes = to_bytes();

    uint32_t len = bit_packed_.length();
    std::vector<UnsignedInt> orig_offsets(len);
    bit_packed_.bulk_unpack_to(orig_offsets.data());

    SignedInt q_min = reference_value_ >> shift;
    std::vector<uint64_t> quantized_offsets(len, 0);
    for (uint32_t i = 0; i < len; ++i) {
        if (bit_packed_.is_null(i)) continue;
        UnsignedInt uo = orig_offsets[i];
        UnsignedInt ur = static_cast<UnsignedInt>(reference_value_);
        SignedInt full = static_cast<SignedInt>(ur + uo);
        SignedInt qv = full >> shift;
        UnsignedInt qd = static_cast<UnsignedInt>(qv) - static_cast<UnsignedInt>(q_min);
        quantized_offsets[i] = static_cast<uint64_t>(qd);
    }

    auto nb = bit_packed_.null_bitmap_arrow_buffer();
    const uint8_t* nulls = nb ? nb->data() : nullptr;
    BitPackedArray qbp(quantized_offsets.data(), nulls, len, new_bw);

    LiquidFloatQuantizedArray<FloatT> hybrid;
    hybrid.exponent_ = exponent_;
    hybrid.quantized_ = std::move(qbp);
    hybrid.reference_value_ = reference_value_;
    hybrid.bucket_width_ = shift;
    hybrid.disk_offset_ = 0;
    hybrid.disk_length_ = full_bytes.size();
    hybrid.io_ = std::move(io);
    hybrid.patch_indices_ = patch_indices_;
    hybrid.patch_values_ = patch_values_;

    return std::make_pair(std::move(hybrid), std::move(full_bytes));
}

using LiquidFloatQuantized32Array = LiquidFloatQuantizedArray<float>;
using LiquidFloatQuantized64Array = LiquidFloatQuantizedArray<double>;

using LiquidFloat32Array = LiquidFloatArray<float>;
using LiquidFloat64Array = LiquidFloatArray<double>;

}  // namespace liquid_cache
