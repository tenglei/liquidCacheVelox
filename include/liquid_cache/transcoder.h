// liquid_cache/transcoder.h
// Transcode Arrow arrays into Liquid Cache format.
// Mirrors the Rust transcode_liquid_inner() function from
// liquid-cache/src/core/src/cache/transcode.rs
#pragma once

#include <memory>
#include <variant>
#include <vector>
#include <string>

#include "liquid_cache/ipc_header.h"
#include "liquid_cache/bit_packed_array.h"

namespace liquid_cache {

// ═══════════════════════════════════════════════════════════════════════
// LiquidArrayRef: a type-erased handle to any Liquid encoded array.
// In a full implementation this would be an abstract base class;
// here we use a simple struct holding the serialized bytes and metadata.
// ═══════════════════════════════════════════════════════════════════════

/// Holds a transcoded Liquid array in its serialized (IPC) form.
/// This is the C++ equivalent of Rust's `LiquidArrayRef = Arc<dyn LiquidArray>`.
struct LiquidEncodedArray {
    LiquidDataType logical_type;
    PhysicalType   physical_type;
    std::vector<uint8_t> serialized_bytes;   ///< Full IPC-format bytes
    size_t         memory_size = 0;          ///< Approximate in-memory size
    uint32_t       length = 0;               ///< Number of elements

    bool is_valid() const { return !serialized_bytes.empty(); }
};

// ═══════════════════════════════════════════════════════════════════════
// PhysicalType mapping from Arrow DataTypeId
// ═══════════════════════════════════════════════════════════════════════

/// Returns the Liquid PhysicalType for a given Arrow type ID.
/// Returns PhysicalType(-1) for unsupported types.
inline PhysicalType physical_type_from_arrow(int arrow_type_id) {
    switch (arrow_type_id) {
        case 6:  return PhysicalType::Int8;      // arrow::Type::INT8
        case 7:  return PhysicalType::Int16;     // arrow::Type::INT16
        case 8:  return PhysicalType::Int32;     // arrow::Type::INT32
        case 9:  return PhysicalType::Int64;     // arrow::Type::INT64
        case 2:  return PhysicalType::UInt8;     // arrow::Type::UINT8
        case 3:  return PhysicalType::UInt16;    // arrow::Type::UINT16
        case 4:  return PhysicalType::UInt32;    // arrow::Type::UINT32
        case 5:  return PhysicalType::UInt64;    // arrow::Type::UINT64
        case 11: return PhysicalType::Float32;   // arrow::Type::FLOAT
        case 12: return PhysicalType::Float64;   // arrow::Type::DOUBLE
        case 16: return PhysicalType::Date32;    // arrow::Type::DATE32
        case 17: return PhysicalType::Date64;    // arrow::Type::DATE64
        // Timestamps require additional handling for unit/timezone
        default: return static_cast<PhysicalType>(0xFFFF);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Standalone transcode functions (no Arrow dependency)
//
// These work on raw value buffers, suitable for use from JNI or Velox.
// ═══════════════════════════════════════════════════════════════════════

/// Compute bit_width needed to represent max unsigned offset.
inline uint8_t compute_bit_width(uint64_t max_value) {
    if (max_value == 0) return 0;
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<uint8_t>(64 - __builtin_clzll(max_value));
#else
    uint8_t bits = 0;
    while (max_value > 0) { max_value >>= 1; ++bits; }
    return bits;
#endif
}

/// Transcode a primitive integer column using Frame-of-Reference + BitPacking.
///
/// @tparam NativeT  The C native type (e.g. int32_t, int64_t, uint32_t)
/// @param values       Pointer to raw values (length = count)
/// @param null_bitmap  Null bitmap (1 bit per value, LSB first); nullptr = no nulls
/// @param count        Number of elements
/// @param physical     PhysicalType enum for the IPC header
/// @return             LiquidEncodedArray with serialized bytes
template <typename NativeT>
LiquidEncodedArray transcode_primitive(
        const NativeT* values,
        const uint8_t* null_bitmap,
        uint32_t count,
        PhysicalType physical) {
    using UnsignedT = std::make_unsigned_t<NativeT>;

    LiquidEncodedArray result;
    result.logical_type = LiquidDataType::Integer;
    result.physical_type = physical;
    result.length = count;

    if (count == 0) {
        // Empty array - just header
        LiquidIPCHeader hdr(LiquidDataType::Integer, physical);
        hdr.serialize(result.serialized_bytes);
        return result;
    }

    // Step 1: Find min/max (skipping nulls)
    NativeT min_val = std::numeric_limits<NativeT>::max();
    NativeT max_val = std::numeric_limits<NativeT>::min();
    bool found_any = false;

    for (uint32_t i = 0; i < count; ++i) {
        if (null_bitmap && (null_bitmap[i / 8] & (1 << (i % 8))) == 0) {
            continue;  // null
        }
        if (!found_any || values[i] < min_val) min_val = values[i];
        if (!found_any || values[i] > max_val) max_val = values[i];
        found_any = true;
    }

    NativeT reference_value = found_any ? min_val : NativeT(0);

    // Step 2: Compute unsigned offsets
    std::vector<uint64_t> offsets(count);
    for (uint32_t i = 0; i < count; ++i) {
        UnsignedT diff = static_cast<UnsignedT>(values[i] - reference_value);
        offsets[i] = static_cast<uint64_t>(diff);
    }

    // Step 3: Compute bit width
    UnsignedT range = static_cast<UnsignedT>(max_val - min_val);
    uint8_t bit_width = compute_bit_width(static_cast<uint64_t>(range));

    // Step 4: Build BitPackedArray
    BitPackedArray bpa(offsets.data(), null_bitmap, count, bit_width);

    // Step 5: Serialize
    std::vector<uint8_t>& out = result.serialized_bytes;
    out.reserve(LiquidIPCHeader::SIZE + sizeof(NativeT) + 8 + bpa.memory_size());

    // IPC header
    LiquidIPCHeader header(LiquidDataType::Integer, physical);
    header.serialize(out);

    // Reference value
    const uint8_t* ref_bytes = reinterpret_cast<const uint8_t*>(&reference_value);
    out.insert(out.end(), ref_bytes, ref_bytes + sizeof(NativeT));

    // Pad to 8-byte alignment
    while (out.size() % 8 != 0) out.push_back(0);

    // BitPackedArray data
    bpa.serialize(out);

    result.memory_size = bpa.memory_size() + sizeof(NativeT);
    return result;
}

/// Transcode a float column using ALP (Adaptive Lossless floating-Point) encoding.
///
/// Simplified version: uses exponents (0, 0) = identity transform for demonstration.
/// A production version should search for optimal exponents like the Rust implementation.
///
/// @tparam FloatT  float or double
/// @param values       Pointer to raw float values
/// @param null_bitmap  Null bitmap; nullptr = no nulls
/// @param count        Number of elements
/// @param physical     PhysicalType (Float32 or Float64)
/// @return             LiquidEncodedArray with serialized bytes
template <typename FloatT>
LiquidEncodedArray transcode_float(
        const FloatT* values,
        const uint8_t* null_bitmap,
        uint32_t count,
        PhysicalType physical) {
    static_assert(std::is_floating_point_v<FloatT>,
                  "FloatT must be float or double");

    using SignedInt = std::conditional_t<sizeof(FloatT) == 4, int32_t, int64_t>;
    using UnsignedInt = std::make_unsigned_t<SignedInt>;

    // ALP constants
    constexpr uint8_t FRACTIONAL_BITS =
        (sizeof(FloatT) == 4) ? 23 : 52;
    constexpr FloatT SWEET =
        (sizeof(FloatT) == 4)
            ? static_cast<FloatT>((1 << 23) + (1 << 22))
            : static_cast<FloatT>((1ULL << 52) + (1ULL << 51));
    constexpr uint8_t MAX_EXPONENT = (sizeof(FloatT) == 4) ? 10 : 18;

    // Powers of 10 tables
    static const FloatT* f10 = []() -> const FloatT* {
        if constexpr (sizeof(FloatT) == 4) {
            static const float t[] = {1.0f,10.0f,100.0f,1000.0f,10000.0f,
                100000.0f,1000000.0f,10000000.0f,100000000.0f,1000000000.0f,10000000000.0f};
            return t;
        } else {
            static const double t[] = {1.0,10.0,100.0,1000.0,10000.0,100000.0,
                1000000.0,10000000.0,100000000.0,1000000000.0,10000000000.0,
                100000000000.0,1000000000000.0,10000000000000.0,
                100000000000000.0,1000000000000000.0,10000000000000000.0,
                100000000000000000.0,1000000000000000000.0};
            return t;
        }
    }();
    static const FloatT* if10 = []() -> const FloatT* {
        if constexpr (sizeof(FloatT) == 4) {
            static const float t[] = {1.0f,0.1f,0.01f,0.001f,0.0001f,
                0.00001f,0.000001f,0.0000001f,0.00000001f,0.000000001f,0.0000000001f};
            return t;
        } else {
            static const double t[] = {1.0,0.1,0.01,0.001,0.0001,0.00001,
                0.000001,0.0000001,0.00000001,0.000000001,0.0000000001,
                0.00000000001,0.000000000001,0.0000000000001,
                0.00000000000001,0.000000000000001,0.0000000000000001,
                0.00000000000000001,0.000000000000000001};
            return t;
        }
    }();

    auto fast_round = [&](FloatT v) -> SignedInt {
        return static_cast<SignedInt>((v + SWEET) - SWEET);
    };

    auto encode_one = [&](FloatT v, uint8_t e, uint8_t f) -> SignedInt {
        return fast_round(v * f10[e] * if10[f]);
    };

    auto decode_one = [&](SignedInt v, uint8_t e, uint8_t f) -> FloatT {
        return static_cast<FloatT>(v) * f10[f] * if10[e];
    };

    LiquidEncodedArray result;
    result.logical_type = LiquidDataType::Float;
    result.physical_type = physical;
    result.length = count;

    // Find best exponents (simplified: search over (e, f) pairs)
    uint8_t best_e = 0, best_f = 0;
    size_t best_cost = SIZE_MAX;

    for (uint8_t e = 0; e < MAX_EXPONENT; ++e) {
        for (uint8_t f = 0; f < e; ++f) {
            size_t patches = 0;
            SignedInt local_min = std::numeric_limits<SignedInt>::max();
            SignedInt local_max = std::numeric_limits<SignedInt>::min();
            uint32_t step = count > 1024 ? count / 1024 : 1;
            for (uint32_t i = 0; i < count; i += step) {
                if (null_bitmap && (null_bitmap[i/8] & (1 << (i%8))) == 0) continue;
                SignedInt enc = encode_one(values[i], e, f);
                FloatT dec = decode_one(enc, e, f);
                if (dec != values[i]) ++patches;
                if (enc < local_min) local_min = enc;
                if (enc > local_max) local_max = enc;
            }
            UnsignedInt range = static_cast<UnsignedInt>(local_max - local_min);
            uint8_t bw = compute_bit_width(static_cast<uint64_t>(range));
            size_t cost = (static_cast<size_t>(count) * bw + 7) / 8 +
                          patches * (8 + sizeof(FloatT));
            if (cost < best_cost) {
                best_cost = cost;
                best_e = e;
                best_f = f;
            }
        }
    }

    // Encode all values with best exponents
    std::vector<SignedInt> encoded(count);
    std::vector<uint64_t> patch_indices;
    std::vector<FloatT> patch_values;

    for (uint32_t i = 0; i < count; ++i) {
        encoded[i] = encode_one(values[i], best_e, best_f);
        FloatT dec = decode_one(encoded[i], best_e, best_f);
        if (dec != values[i]) {
            patch_indices.push_back(i);
            patch_values.push_back(values[i]);
        }
    }

    // Fill patched positions with fill value
    if (!patch_indices.empty()) {
        SignedInt fill = encoded[0];
        for (uint32_t i = 0; i < count; ++i) {
            bool is_patch = false;
            for (auto pi : patch_indices) {
                if (pi == i) { is_patch = true; break; }
            }
            if (!is_patch) { fill = encoded[i]; break; }
        }
        for (auto pi : patch_indices) encoded[pi] = fill;
    }

    // Compute reference and offsets
    SignedInt min_enc = *std::min_element(encoded.begin(), encoded.end());
    SignedInt max_enc = *std::max_element(encoded.begin(), encoded.end());
    UnsignedInt range = static_cast<UnsignedInt>(max_enc - min_enc);
    uint8_t bw = compute_bit_width(static_cast<uint64_t>(range));

    std::vector<uint64_t> offsets(count);
    for (uint32_t i = 0; i < count; ++i) {
        offsets[i] = static_cast<uint64_t>(
            static_cast<UnsignedInt>(encoded[i] - min_enc));
    }

    BitPackedArray bpa(offsets.data(), null_bitmap, count, bw);

    // Serialize
    std::vector<uint8_t>& out = result.serialized_bytes;
    out.reserve(512);

    LiquidIPCHeader header(LiquidDataType::Float, physical);
    header.serialize(out);

    // Reference value
    const uint8_t* ref_p = reinterpret_cast<const uint8_t*>(&min_enc);
    out.insert(out.end(), ref_p, ref_p + sizeof(SignedInt));
    while (out.size() % 8 != 0) out.push_back(0);

    // Exponents
    out.push_back(best_e);
    out.push_back(best_f);
    for (int i = 0; i < 6; ++i) out.push_back(0);

    // Patches
    uint64_t patch_len = patch_indices.size();
    const uint8_t* plp = reinterpret_cast<const uint8_t*>(&patch_len);
    out.insert(out.end(), plp, plp + 8);
    if (patch_len > 0) {
        auto* pip = reinterpret_cast<const uint8_t*>(patch_indices.data());
        out.insert(out.end(), pip, pip + 8 * patch_len);
        auto* pvp = reinterpret_cast<const uint8_t*>(patch_values.data());
        out.insert(out.end(), pvp, pvp + sizeof(FloatT) * patch_len);
    }
    while (out.size() % 8 != 0) out.push_back(0);

    bpa.serialize(out);

    result.memory_size = bpa.memory_size() + patch_indices.size() * 8 +
                         patch_values.size() * sizeof(FloatT) + sizeof(SignedInt);
    return result;
}

}  // namespace liquid_cache
