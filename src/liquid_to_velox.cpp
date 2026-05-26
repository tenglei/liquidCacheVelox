// liquid_to_velox.cpp
// Core implementation of Liquid → Velox Vector direct conversion.
// Conditional on LIQUID_ENABLE_VELOX being defined.

#ifdef LIQUID_ENABLE_VELOX

#include "liquid_cache/liquid_to_velox.h"
#include "liquid_cache/liquid_arrays.h"
#include "liquid_cache/liquid_byte_view_array.h"
#include "liquid_cache/liquid_decimal_array.h"
#include "liquid_cache/liquid_fixed_len_byte_array.h"
#include "liquid_cache/liquid_cache_store.h"

#include "velox/vector/DictionaryVector.h"

namespace liquid_cache {

using namespace facebook::velox;

// ═══════════════════════════════════════════════════════════════════════
// LiquidPrimitiveArray<T>::to_velox()
//
// Decodes FoR + BitPacking directly to Velox FlatVector.
// For Timestamp types, converts int64 → Velox Timestamp struct.
// ═══════════════════════════════════════════════════════════════════════

template <typename ArrowType>
VectorPtr LiquidPrimitiveArray<ArrowType>::to_velox(
        memory::MemoryPool* pool) const {
    using NativeT = typename ArrowType::c_type;
    using UnsignedT = typename UnsignedType<ArrowType>::type;

    uint32_t len = bit_packed_.length();

    // Timestamps are stored as LiquidPrimitiveArray<Int64Type>.
    // The template parameter ArrowType is always arrow::Int64Type here,
    // so ArrowPhysicalType<ArrowType>::value always returns Int64.
    // We must check the stored type_ field (populated from the original
    // Arrow TimestampType) to determine the actual time unit and produce
    // a Velox TIMESTAMP vector rather than BIGINT.
    auto pt = ArrowPhysicalType<ArrowType>::value;
    if (type_ && type_->id() == arrow::Type::TIMESTAMP) {
        auto ts_type = std::static_pointer_cast<arrow::TimestampType>(type_);
        switch (ts_type->unit()) {
            case arrow::TimeUnit::SECOND: pt = PhysicalType::TimestampSecond; break;
            case arrow::TimeUnit::MILLI:  pt = PhysicalType::TimestampMillisecond; break;
            case arrow::TimeUnit::MICRO:  pt = PhysicalType::TimestampMicrosecond; break;
            case arrow::TimeUnit::NANO:   pt = PhysicalType::TimestampNanosecond; break;
            default: break;
        }
    }

    if (len == 0) {
        return BaseVector::create(
            liquid_physical_to_velox_type(pt), 0, pool);
    }

    auto nulls = copy_null_bitmap_to_velox(bit_packed_, pool);

    // --- Timestamp types: special handling ---
    if (pt == PhysicalType::TimestampSecond ||
        pt == PhysicalType::TimestampMillisecond ||
        pt == PhysicalType::TimestampMicrosecond ||
        pt == PhysicalType::TimestampNanosecond) {

        auto valuesBuf = AlignedBuffer::allocate<Timestamp>(len, pool);
        auto* rawValues = valuesBuf->template asMutable<Timestamp>();

        std::vector<UnsignedT> temp(len);
        bit_packed_.bulk_unpack_to(temp.data());

        for (uint32_t i = 0; i < len; ++i) {
            int64_t val = reference_value_ + static_cast<NativeT>(temp[i]);
            rawValues[i] = int64_to_velox_timestamp(val, pt);
        }

        return std::make_shared<FlatVector<Timestamp>>(
            pool, TIMESTAMP(), nulls,
            static_cast<vector_size_t>(len), valuesBuf,
            std::vector<BufferPtr>{});
    }

    // --- Date64: convert milliseconds to days for Velox DATE ---
    // Arrow Date64 stores milliseconds since epoch (int64),
    // but Velox DATE is days since epoch (int32).
    if (pt == PhysicalType::Date64) {
        auto valuesBuf = AlignedBuffer::allocate<int32_t>(len, pool);
        auto* rawValues = valuesBuf->template asMutable<int32_t>();

        std::vector<UnsignedT> temp(len);
        bit_packed_.bulk_unpack_to(temp.data());

        for (uint32_t i = 0; i < len; ++i) {
            int64_t val = reference_value_ + static_cast<NativeT>(temp[i]);
            rawValues[i] = static_cast<int32_t>(val / 86400000LL);
        }

        return std::make_shared<FlatVector<int32_t>>(
            pool, INTEGER(), nulls,
            static_cast<vector_size_t>(len), valuesBuf,
            std::vector<BufferPtr>{});
    }

    // --- Integer / Date32 types: direct FoR decode ---
    auto valuesBuf = AlignedBuffer::allocate<NativeT>(len, pool);
    auto* rawValues = valuesBuf->template asMutable<NativeT>();

    std::vector<UnsignedT> temp(len);
    bit_packed_.bulk_unpack_to(temp.data());

    for (uint32_t i = 0; i < len; ++i) {
        rawValues[i] = reference_value_ + static_cast<NativeT>(temp[i]);
    }

    auto veloxType = liquid_physical_to_velox_type(pt);
    return std::make_shared<FlatVector<NativeT>>(
        pool, veloxType, nulls,
        static_cast<vector_size_t>(len), valuesBuf,
        std::vector<BufferPtr>{});
}

// Explicit template instantiations for all supported primitive types
template VectorPtr LiquidPrimitiveArray<arrow::Int8Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidPrimitiveArray<arrow::Int16Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidPrimitiveArray<arrow::Int32Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidPrimitiveArray<arrow::Int64Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidPrimitiveArray<arrow::UInt8Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidPrimitiveArray<arrow::UInt16Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidPrimitiveArray<arrow::UInt32Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidPrimitiveArray<arrow::UInt64Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidPrimitiveArray<arrow::Date32Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidPrimitiveArray<arrow::Date64Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidPrimitiveArray<arrow::TimestampType>::to_velox(memory::MemoryPool*) const;

// ═══════════════════════════════════════════════════════════════════════
// LiquidFloatArray<T>::to_velox()
//
// Decodes ALP + BitPacking + Patching directly to Velox FlatVector.
// ═══════════════════════════════════════════════════════════════════════

template <typename FloatT>
VectorPtr LiquidFloatArray<FloatT>::to_velox(
        memory::MemoryPool* pool) const {
    uint32_t len = bit_packed_.length();
    if (len == 0) {
        TypePtr emptyType = std::is_same_v<FloatT, float> ? TypePtr(REAL()) : TypePtr(DOUBLE());
        return BaseVector::create(emptyType, 0, pool);
    }

    auto valuesBuf = AlignedBuffer::allocate<FloatT>(len, pool);
    auto* rawValues = valuesBuf->template asMutable<FloatT>();

    // Bulk unpack offsets
    std::vector<UnsignedInt> temp(len);
    bit_packed_.bulk_unpack_to(temp.data());

    // Single-pass ALP decode
    for (uint32_t i = 0; i < len; ++i) {
        SignedInt encoded_val = reference_value_ + static_cast<SignedInt>(temp[i]);
        rawValues[i] = decode_single(encoded_val, exponent_);
    }

    // In-place patch
    for (size_t j = 0; j < patch_indices_.size(); ++j) {
        rawValues[patch_indices_[j]] = patch_values_[j];
    }

    auto nulls = copy_null_bitmap_to_velox(bit_packed_, pool);
    TypePtr veloxType = std::is_same_v<FloatT, float> ? TypePtr(REAL()) : TypePtr(DOUBLE());

    return std::make_shared<FlatVector<FloatT>>(
        pool, veloxType, nulls,
        static_cast<vector_size_t>(len), valuesBuf,
        std::vector<BufferPtr>{});
}

template VectorPtr LiquidFloatArray<float>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidFloatArray<double>::to_velox(memory::MemoryPool*) const;

// ═══════════════════════════════════════════════════════════════════════
// LiquidLinearIntegerArray<T>::to_velox()
//
// Decodes linear model + residuals directly to Velox FlatVector.
// Reconstruction: value[i] = clamp(intercept + slope*i) + residual[i]
// ═══════════════════════════════════════════════════════════════════════

template <typename ArrowType>
VectorPtr LiquidLinearIntegerArray<ArrowType>::to_velox(
        memory::MemoryPool* pool) const {
    using NativeT = typename ArrowType::c_type;
    constexpr bool IS_UNSIGNED = !std::is_signed_v<NativeT>;

    // Decode residuals as Int64Array
    auto res_arr = residuals_.to_arrow();
    auto res_typed = std::static_pointer_cast<arrow::Int64Array>(res_arr);
    int64_t len = res_typed->length();
    if (len == 0) {
        return BaseVector::create(
            liquid_physical_to_velox_type(ArrowPhysicalType<ArrowType>::value),
            0, pool);
    }

    // Date64: special handling to convert ms to days for Velox DATE
    auto pt = ArrowPhysicalType<ArrowType>::value;
    if (pt == PhysicalType::Date64) {
        auto valuesBuf = AlignedBuffer::allocate<int32_t>(static_cast<size_t>(len), pool);
        auto* rawValues = valuesBuf->template asMutable<int32_t>();
        const int64_t* rd = res_typed->raw_values();
        const uint8_t* rn = res_typed->null_bitmap_data();
        int64_t ro = res_typed->offset();

        constexpr int64_t mi64 = static_cast<int64_t>(std::numeric_limits<NativeT>::min());
        constexpr int64_t ma64 = static_cast<int64_t>(std::numeric_limits<NativeT>::max());
        for (int64_t i = 0; i < len; ++i) {
            bool ok = !rn || arrow::bit_util::GetBit(rn, ro + i);
            if (ok) {
                double pr = slope_ * static_cast<double>(i) + intercept_;
                int64_t p = predict_i64_saturated(pr, mi64, ma64);
                __int128_t sum = static_cast<__int128_t>(p) + static_cast<__int128_t>(rd[i]);
                int64_t val64 = static_cast<int64_t>(std::clamp<__int128_t>(sum,
                    static_cast<__int128_t>(mi64), static_cast<__int128_t>(ma64)));
                rawValues[i] = static_cast<int32_t>(val64 / 86400000LL);
            } else {
                rawValues[i] = 0;
            }
        }

        // Copy nulls from residuals bit_packed
        auto nulls = copy_null_bitmap_to_velox(residuals_.bit_packed(), pool);
        return std::make_shared<FlatVector<int32_t>>(
            pool, INTEGER(), nulls,
            static_cast<vector_size_t>(len), valuesBuf,
            std::vector<BufferPtr>{});
    }

    // Check if this is actually a Timestamp stored via linear model
    if (type_ && type_->id() == arrow::Type::TIMESTAMP) {
        auto ts_type = std::static_pointer_cast<arrow::TimestampType>(type_);
        switch (ts_type->unit()) {
            case arrow::TimeUnit::SECOND: pt = PhysicalType::TimestampSecond; break;
            case arrow::TimeUnit::MILLI:  pt = PhysicalType::TimestampMillisecond; break;
            case arrow::TimeUnit::MICRO:  pt = PhysicalType::TimestampMicrosecond; break;
            case arrow::TimeUnit::NANO:   pt = PhysicalType::TimestampNanosecond; break;
            default: break;
        }
    }

    // Timestamp types: decode int64 values, then convert to velox::Timestamp
    if (pt == PhysicalType::TimestampSecond ||
        pt == PhysicalType::TimestampMillisecond ||
        pt == PhysicalType::TimestampMicrosecond ||
        pt == PhysicalType::TimestampNanosecond) {

        auto valuesBuf = AlignedBuffer::allocate<Timestamp>(static_cast<size_t>(len), pool);
        auto* rawValues = valuesBuf->template asMutable<Timestamp>();
        const int64_t* rd = res_typed->raw_values();
        const uint8_t* rn = res_typed->null_bitmap_data();
        int64_t ro = res_typed->offset();
        constexpr int64_t mi = std::numeric_limits<int64_t>::min();
        constexpr int64_t ma = std::numeric_limits<int64_t>::max();

        for (int64_t i = 0; i < len; ++i) {
            bool ok = !rn || arrow::bit_util::GetBit(rn, ro + i);
            if (ok) {
                double pr = slope_ * static_cast<double>(i) + intercept_;
                int64_t p = predict_i64_saturated(pr, mi, ma);
                __int128_t sum = static_cast<__int128_t>(p) + static_cast<__int128_t>(rd[i]);
                int64_t val = static_cast<int64_t>(std::clamp<__int128_t>(sum,
                    static_cast<__int128_t>(mi), static_cast<__int128_t>(ma)));
                rawValues[i] = int64_to_velox_timestamp(val, pt);
            } else {
                rawValues[i] = Timestamp();
            }
        }

        auto nulls = copy_null_bitmap_to_velox(residuals_.bit_packed(), pool);
        return std::make_shared<FlatVector<Timestamp>>(
            pool, TIMESTAMP(), nulls,
            static_cast<vector_size_t>(len), valuesBuf,
            std::vector<BufferPtr>{});
    }

    // All other types: direct reconstruction into NativeT buffer
    auto nulls = copy_null_bitmap_to_velox(residuals_.bit_packed(), pool);
    auto valuesBuf = AlignedBuffer::allocate<NativeT>(static_cast<size_t>(len), pool);
    auto* rawValues = valuesBuf->template asMutable<NativeT>();
    const int64_t* rd = res_typed->raw_values();
    const uint8_t* rn = res_typed->null_bitmap_data();
    int64_t ro = res_typed->offset();

    if constexpr (IS_UNSIGNED) {
        constexpr uint64_t mx = static_cast<uint64_t>(std::numeric_limits<NativeT>::max());
        for (int64_t i = 0; i < len; ++i) {
            bool ok = !rn || arrow::bit_util::GetBit(rn, ro + i);
            if (ok) {
                double pr = slope_ * static_cast<double>(i) + intercept_;
                uint64_t p = predict_u64_saturated(pr, mx);
                __int128_t sum = static_cast<__int128_t>(p) + static_cast<__int128_t>(rd[i]);
                rawValues[i] = static_cast<NativeT>(std::clamp<__int128_t>(sum, __int128_t{0}, mx));
            } else { rawValues[i] = 0; }
        }
    } else {
        constexpr int64_t mi = static_cast<int64_t>(std::numeric_limits<NativeT>::min());
        constexpr int64_t ma = static_cast<int64_t>(std::numeric_limits<NativeT>::max());
        for (int64_t i = 0; i < len; ++i) {
            bool ok = !rn || arrow::bit_util::GetBit(rn, ro + i);
            if (ok) {
                double pr = slope_ * static_cast<double>(i) + intercept_;
                int64_t p = predict_i64_saturated(pr, mi, ma);
                __int128_t sum = static_cast<__int128_t>(p) + static_cast<__int128_t>(rd[i]);
                rawValues[i] = static_cast<NativeT>(std::clamp<__int128_t>(sum,
                    static_cast<__int128_t>(mi), static_cast<__int128_t>(ma)));
            } else { rawValues[i] = 0; }
        }
    }

    auto veloxType = liquid_physical_to_velox_type(pt);
    return std::make_shared<FlatVector<NativeT>>(
        pool, veloxType, nulls,
        static_cast<vector_size_t>(len), valuesBuf,
        std::vector<BufferPtr>{});
}

// Explicit instantiations
template VectorPtr LiquidLinearIntegerArray<arrow::Int8Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidLinearIntegerArray<arrow::Int16Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidLinearIntegerArray<arrow::Int32Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidLinearIntegerArray<arrow::Int64Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidLinearIntegerArray<arrow::UInt8Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidLinearIntegerArray<arrow::UInt16Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidLinearIntegerArray<arrow::UInt32Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidLinearIntegerArray<arrow::UInt64Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidLinearIntegerArray<arrow::Date32Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidLinearIntegerArray<arrow::Date64Type>::to_velox(memory::MemoryPool*) const;

// ═══════════════════════════════════════════════════════════════════════
// LiquidByteViewArray::to_velox()
//
// Decodes FSST + Dictionary + BitPacking directly to Velox.
//
// For low-cardinality dictionaries (dict_size < len/2), outputs
// DictionaryVector<StringView> to avoid per-element string copies.
// For high-cardinality, falls back to FlatVector<StringView>.
// ═══════════════════════════════════════════════════════════════════════

VectorPtr LiquidByteViewArray::to_velox(
        memory::MemoryPool* pool) const {
    uint32_t len = dictionary_keys_.length();
    TypePtr veloxType = is_binary_ ? TypePtr(VARBINARY()) : TypePtr(VARCHAR());
    if (len == 0) {
        return BaseVector::create(veloxType, 0, pool);
    }

    // Phase A: Get cached decompressed dictionary (lazy, one-time FSST decode)
    const auto& dict = ensure_dict_cached();
    size_t dict_size = dict.entries.size();
    const auto* dict_flat = dict.flat_data.data();
    const auto* dict_flat_offsets = dict.flat_offsets.data();
    const auto* dict_lens = dict.lengths.data();

    // Phase B: Bulk unpack all dictionary keys
    std::vector<uint16_t> keys(len);
    dictionary_keys_.bulk_unpack_to(keys.data());

    auto nulls = copy_null_bitmap_to_velox(dictionary_keys_, pool);

    // ── DictionaryVector path for low-cardinality dictionaries ────────
    // When the dictionary is significantly smaller than the array,
    // output a DictionaryVector<StringView> instead of copying strings
    // into a FlatVector.  This avoids O(N) memcpy per decode.
    if (dict_size > 0 && dict_size < len / 2) {
        // Build the base FlatVector<StringView> from dictionary entries.
        // Zero-copy: wrap flat_data with BufferView, keep cached_dict_ alive
        auto cached_dict = cached_dict_;  // shared_ptr copy extends lifetime
        auto dictStringBuf = BufferView<DecompressedDict::DictReleaser>::create(
            dict_flat, dict.flat_data.size(),
            DecompressedDict::DictReleaser{std::move(cached_dict)});

        auto dictValuesBuf = AlignedBuffer::allocate<StringView>(
            static_cast<vector_size_t>(dict_size), pool);
        auto* dictRawViews = dictValuesBuf->template asMutable<StringView>();

        for (size_t d = 0; d < dict_size; ++d) {
            dictRawViews[d] = StringView(
                reinterpret_cast<const char*>(dict_flat) + dict_flat_offsets[d],
                dict_lens[d]);
        }

        auto dictVec = std::make_shared<FlatVector<StringView>>(
            pool, veloxType, nullptr,
            static_cast<vector_size_t>(dict_size), dictValuesBuf,
            std::vector<BufferPtr>{dictStringBuf});
        // Convert uint16 keys to vector_size_t indices.
        auto indices = AlignedBuffer::allocate<vector_size_t>(
            static_cast<vector_size_t>(len), pool);
        auto* rawIndices = indices->template asMutable<vector_size_t>();
        for (uint32_t i = 0; i < len; ++i) {
            rawIndices[i] = static_cast<vector_size_t>(keys[i]);
        }

        auto result = std::make_shared<DictionaryVector<StringView>>(
            pool, nulls, static_cast<vector_size_t>(len), dictVec, indices);
        result->setAllIsAscii(is_ascii_);
        return result;
    }

    // ── FlatVector path for high-cardinality dictionaries ─────────────
    // Zero-copy: StringViews point directly into dict.flat_data.
    // BufferView with DictReleaser keeps flat_data alive.
    auto cached_dict = cached_dict_;  // shared_ptr copy extends lifetime

    auto valuesBuf = AlignedBuffer::allocate<StringView>(len, pool);
    auto* rawValues = valuesBuf->template asMutable<StringView>();

    // Single pass: build StringViews, no memcpy
    for (uint32_t i = 0; i < len; ++i) {
        if (!dictionary_keys_.is_null(i) && keys[i] < dict_size) {
            uint16_t key = keys[i];
            rawValues[i] = StringView(
                reinterpret_cast<const char*>(dict_flat) + dict_flat_offsets[key],
                dict_lens[key]);
        } else {
            rawValues[i] = StringView();
        }
    }

    auto stringBuffer = BufferView<DecompressedDict::DictReleaser>::create(
        dict_flat, dict.flat_data.size(),
        DecompressedDict::DictReleaser{std::move(cached_dict)});

    auto vec = std::make_shared<FlatVector<StringView>>(
        pool, veloxType, nulls,
        static_cast<vector_size_t>(len), valuesBuf,
        std::vector<BufferPtr>{stringBuffer});
    vec->setAllIsAscii(is_ascii_);
    return vec;
}

// ═══════════════════════════════════════════════════════════════════════
// LiquidDecimalArray::to_velox()
//
// Decodes FoR + BitPacking to Velox FlatVector.
// Uses ShortDecimalType (int64) for precision <= 18,
// LongDecimalType (int128) for precision > 18.
// ═══════════════════════════════════════════════════════════════════════

VectorPtr LiquidDecimalArray::to_velox(
        memory::MemoryPool* pool) const {
    uint32_t len = bit_packed_.length();

    // Bulk unpack u64 offsets
    if (len > 0) {
        std::vector<uint64_t> temp(len);
        bit_packed_.bulk_unpack_to(temp.data());
        auto nulls = copy_null_bitmap_to_velox(bit_packed_, pool);

        // Velox uses ShortDecimal (int64_t) for precision <= kMaxShortDecimalPrecision (18),
        // and LongDecimal (int128_t) for precision > 18.
        constexpr int kMaxShortDecimalPrecision = 18;
        if (precision_ <= kMaxShortDecimalPrecision) {
            auto veloxType = std::make_shared<ShortDecimalType>(precision_, scale_);
            auto valuesBuf = AlignedBuffer::allocate<int64_t>(len, pool);
            auto* rawValues = valuesBuf->template asMutable<int64_t>();
            for (uint32_t i = 0; i < len; ++i) {
                rawValues[i] = static_cast<int64_t>(temp[i] + reference_value_);
            }
            return std::make_shared<FlatVector<int64_t>>(
                pool, veloxType, nulls,
                static_cast<vector_size_t>(len), valuesBuf,
                std::vector<BufferPtr>{});
        } else {
            auto veloxType = std::make_shared<LongDecimalType>(precision_, scale_);
            auto valuesBuf = AlignedBuffer::allocate<int128_t>(len, pool);
            auto* rawValues = valuesBuf->template asMutable<int128_t>();
            for (uint32_t i = 0; i < len; ++i) {
                rawValues[i] = static_cast<int128_t>(temp[i]) + static_cast<int128_t>(reference_value_);
            }
            return std::make_shared<FlatVector<int128_t>>(
                pool, veloxType, nulls,
                static_cast<vector_size_t>(len), valuesBuf,
                std::vector<BufferPtr>{});
        }
    }

    // Empty case
    constexpr int kMaxShortDecimalPrecision = 18;
    if (precision_ <= kMaxShortDecimalPrecision) {
        return BaseVector::create(
            std::make_shared<ShortDecimalType>(precision_, scale_), 0, pool);
    } else {
        return BaseVector::create(
            std::make_shared<LongDecimalType>(precision_, scale_), 0, pool);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// LiquidFixedLenByteArray::to_velox()
//
// Decodes FSST Dictionary + BitPacking directly to Velox
// ShortDecimal (int64) or LongDecimal (int128) FlatVector.
// ═══════════════════════════════════════════════════════════════════════

VectorPtr LiquidFixedLenByteArray::to_velox(
        memory::MemoryPool* pool) const {
    uint32_t len = keys_.length();
    if (len == 0) {
        constexpr int kMaxShortDecimalPrecision = 18;
        if (precision_ <= kMaxShortDecimalPrecision) {
            return BaseVector::create(
                std::make_shared<ShortDecimalType>(precision_, scale_), 0, pool);
        } else {
            return BaseVector::create(
                std::make_shared<LongDecimalType>(precision_, scale_), 0, pool);
        }
    }

    // Get cached decompressed dictionary
    const auto& dict = ensure_dict_cached();
    size_t dict_size = dict.size();

    // Bulk unpack all keys
    std::vector<uint16_t> key_vals(len);
    keys_.bulk_unpack_to(key_vals.data());

    auto nulls = copy_null_bitmap_to_velox(keys_, pool);

    constexpr int kMaxShortDecimalPrecision = 18;
    if (precision_ <= kMaxShortDecimalPrecision) {
        // ShortDecimal: int64_t, value_width = 8 for the low 8 bytes
        auto veloxType = std::make_shared<ShortDecimalType>(precision_, scale_);
        auto valuesBuf = AlignedBuffer::allocate<int64_t>(len, pool);
        auto* rawValues = valuesBuf->template asMutable<int64_t>();

        size_t value_width = fixed_len_value_width(arrow_type_);
        for (uint32_t i = 0; i < len; ++i) {
            if (!keys_.is_null(i) && key_vals[i] < dict_size) {
                // Decimal128: 16 bytes LE, low 8 bytes = int64 value
                const auto& entry = dict[key_vals[i]];
                int64_t val = 0;
                size_t copy_len = std::min(entry.size(), sizeof(int64_t));
                std::memcpy(&val, entry.data(), copy_len);
                rawValues[i] = val;
            } else {
                rawValues[i] = 0;
            }
        }

        return std::make_shared<FlatVector<int64_t>>(
            pool, veloxType, nulls,
            static_cast<vector_size_t>(len), valuesBuf,
            std::vector<BufferPtr>{});
    } else {
        // LongDecimal: int128_t
        auto veloxType = std::make_shared<LongDecimalType>(precision_, scale_);
        auto valuesBuf = AlignedBuffer::allocate<int128_t>(len, pool);
        auto* rawValues = valuesBuf->template asMutable<int128_t>();

        for (uint32_t i = 0; i < len; ++i) {
            if (!keys_.is_null(i) && key_vals[i] < dict_size) {
                const auto& entry = dict[key_vals[i]];
                int128_t val = 0;
                size_t copy_len = std::min(entry.size(), sizeof(int128_t));
                std::memcpy(&val, entry.data(), copy_len);
                rawValues[i] = val;
            } else {
                rawValues[i] = 0;
            }
        }

        return std::make_shared<FlatVector<int128_t>>(
            pool, veloxType, nulls,
            static_cast<vector_size_t>(len), valuesBuf,
            std::vector<BufferPtr>{});
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Arrow Schema → Velox RowType conversion
// ═══════════════════════════════════════════════════════════════════════

namespace {

TypePtr arrow_type_to_velox(const std::shared_ptr<arrow::DataType>& at) {
    switch (at->id()) {
        case arrow::Type::INT8:    return TINYINT();
        case arrow::Type::INT16:   return SMALLINT();
        case arrow::Type::INT32:   return INTEGER();
        case arrow::Type::INT64:   return BIGINT();
        case arrow::Type::UINT8:   return TINYINT();
        case arrow::Type::UINT16:  return SMALLINT();
        case arrow::Type::UINT32:  return INTEGER();
        case arrow::Type::UINT64:  return BIGINT();
        case arrow::Type::FLOAT:   return REAL();
        case arrow::Type::DOUBLE:  return DOUBLE();
        case arrow::Type::STRING:  return VARCHAR();
        case arrow::Type::LARGE_STRING: return VARCHAR();
        case arrow::Type::BINARY:  return VARBINARY();
        case arrow::Type::LARGE_BINARY: return VARBINARY();
        case arrow::Type::DATE32:  return INTEGER();
        case arrow::Type::DATE64:  return INTEGER();
        case arrow::Type::TIMESTAMP: return TIMESTAMP();
        case arrow::Type::DECIMAL128: {
            auto dt = std::static_pointer_cast<arrow::Decimal128Type>(at);
            return DECIMAL(static_cast<uint8_t>(dt->precision()),
                           static_cast<uint8_t>(dt->scale()));
        }
        case arrow::Type::DECIMAL256: {
            auto dt = std::static_pointer_cast<arrow::Decimal256Type>(at);
            return DECIMAL(static_cast<uint8_t>(dt->precision()),
                           static_cast<uint8_t>(dt->scale()));
        }
        default:
            throw std::runtime_error(
                "Unsupported Arrow type for Velox mapping: " + at->ToString());
    }
}

RowTypePtr arrow_schema_to_velox_row_type(
        const std::shared_ptr<arrow::Schema>& schema) {
    std::vector<std::string> names;
    std::vector<TypePtr> types;
    for (int i = 0; i < schema->num_fields(); ++i) {
        names.push_back(schema->field(i)->name());
        types.push_back(arrow_type_to_velox(schema->field(i)->type()));
    }
    return ROW(std::move(names), std::move(types));
}

}  // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════
// LiquidCacheStore::load_from_parquet_for_velox()
//
// Wrapper that hides Arrow types from callers.
// ═══════════════════════════════════════════════════════════════════════

std::vector<LiquidCacheStore::RowGroupInfo>
LiquidCacheStore::load_from_parquet_for_velox(
        const std::vector<std::string>& files,
        facebook::velox::RowTypePtr& veloxRowType,
        double& transcode_sec) {
    std::shared_ptr<arrow::Schema> schema;
    // Use the new overload with automatic FSST compressor reuse
    auto rg_infos = load_from_parquet(files, schema, transcode_sec);
    if (schema) {
        veloxRowType = arrow_schema_to_velox_row_type(schema);
    }
    return rg_infos;
}

// ═══════════════════════════════════════════════════════════════════════
// LiquidCacheStore::read_column_velox()
// ═══════════════════════════════════════════════════════════════════════

VectorPtr LiquidCacheStore::read_column_velox(
        const LiquidCacheKey& key,
        memory::MemoryPool* pool) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return nullptr;
    const auto& entry = it->second;
    if (entry.type == CacheEntryType::MemoryLiquid && entry.liquid_array) {
        return entry.liquid_array->to_velox(pool);
    }
    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════════
// LiquidCacheStore::read_batch_velox()
// ═══════════════════════════════════════════════════════════════════════

VectorPtr LiquidCacheStore::read_batch_velox(
        uint16_t file_id,
        uint16_t rg_id,
        uint16_t batch_id,
        const RowTypePtr& rowType,
        memory::MemoryPool* pool,
        const std::vector<int>& projection) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Determine which columns to read
    std::vector<int> cols_to_read;
    if (projection.empty()) {
        cols_to_read.resize(rowType->size());
        std::iota(cols_to_read.begin(), cols_to_read.end(), 0);
    } else {
        cols_to_read = projection;
    }

    // Read each projected column as Velox Vector
    std::vector<VectorPtr> children;
    children.reserve(cols_to_read.size());

    for (int col_idx : cols_to_read) {
        LiquidCacheKey key(file_id, rg_id,
                           static_cast<uint16_t>(col_idx), batch_id);
        auto it = entries_.find(key);
        if (it == entries_.end()) return nullptr;
        const auto& entry = it->second;
        if (entry.type == CacheEntryType::MemoryLiquid && entry.liquid_array) {
            auto vec = entry.liquid_array->to_velox(pool);
            if (!vec) return nullptr;
            children.push_back(std::move(vec));
        } else {
            return nullptr;
        }
    }

    if (children.empty()) return nullptr;

    // Build projected RowType
    std::vector<std::string> names;
    std::vector<TypePtr> types;
    names.reserve(cols_to_read.size());
    types.reserve(cols_to_read.size());
    for (int idx : cols_to_read) {
        names.push_back(rowType->nameOf(idx));
        types.push_back(rowType->childAt(idx));
    }
    auto projRowType = ROW(std::move(names), std::move(types));

    vector_size_t numRows = children[0]->size();
    return std::make_shared<RowVector>(
        pool, projRowType, nullptr, numRows, std::move(children));
}

// ═══════════════════════════════════════════════════════════════════════
// LiquidPrimitiveDeltaArray<T>::to_velox()
//
// Decodes delta + zigzag encoding directly into Velox FlatVector.
// Reconstructs values via inverse zigzag + cumulative wrapping add.
// ═══════════════════════════════════════════════════════════════════════

template <typename ArrowType>
VectorPtr LiquidPrimitiveDeltaArray<ArrowType>::to_velox(
        memory::MemoryPool* pool) const {
    using NativeT = typename ArrowType::c_type;
    using UnsignedT = typename UnsignedType<ArrowType>::type;

    uint32_t len = bit_packed_.length();
    if (len == 0) {
        auto pt = ArrowPhysicalType<ArrowType>::value;
        return BaseVector::create(liquid_physical_to_velox_type(pt), 0, pool);
    }

    auto nulls = copy_null_bitmap_to_velox(bit_packed_, pool);
    auto valuesBuf = AlignedBuffer::allocate<NativeT>(len, pool);
    auto* rawValues = valuesBuf->template asMutable<NativeT>();

    // Unpack zigzag values
    std::vector<UnsignedT> temp(len);
    bit_packed_.bulk_unpack_to(temp.data());

    // Reconstruct: inverse zigzag + cumulative wrapping add
    NativeT current = anchor_;
    bool have_first = false;

    for (uint32_t i = 0; i < len; ++i) {
        if (bit_packed_.is_null(i)) {
            rawValues[i] = 0;
            continue;
        }
        if (!have_first) {
            rawValues[i] = anchor_;
            current = anchor_;
            have_first = true;
            continue;
        }
        uint64_t zigzag = static_cast<uint64_t>(temp[i]);
        int64_t delta_i64 = static_cast<int64_t>(zigzag >> 1)
                          ^ -static_cast<int64_t>(zigzag & 1);
        UnsignedT udelta = static_cast<UnsignedT>(static_cast<NativeT>(delta_i64));
        UnsignedT ucur = static_cast<UnsignedT>(current);
        rawValues[i] = static_cast<NativeT>(ucur + udelta);
        current = rawValues[i];
    }

    auto pt = ArrowPhysicalType<ArrowType>::value;
    auto veloxType = liquid_physical_to_velox_type(pt);
    return std::make_shared<FlatVector<NativeT>>(
        pool, veloxType, nulls,
        static_cast<vector_size_t>(len), valuesBuf,
        std::vector<BufferPtr>{});
}

// Explicit instantiations
template VectorPtr LiquidPrimitiveDeltaArray<arrow::Int32Type>::to_velox(memory::MemoryPool*) const;
template VectorPtr LiquidPrimitiveDeltaArray<arrow::Int64Type>::to_velox(memory::MemoryPool*) const;

}  // namespace liquid_cache

#endif  // LIQUID_ENABLE_VELOX
