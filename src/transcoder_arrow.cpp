// transcoder_arrow.cpp
// Arrow C++ dependent transcoding functions.
// This file bridges the header-only transcoder.h (raw buffers) with
// the Arrow C++ API, providing convenient wrappers that accept
// std::shared_ptr<arrow::Array> and return LiquidEncodedArray.
//
// Mirrors the Rust transcode_liquid_inner() dispatch logic from
// liquid-cache/src/core/src/cache/transcode.rs

#include <arrow/util/logging.h>  // MUST be first: defines ARROW_CHECK_OK macro
#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/type.h>

#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include "liquid_cache/ipc_header.h"
#include "liquid_cache/bit_packed_array.h"
#include "liquid_cache/transcoder.h"
#include "liquid_cache/liquid_arrays.h"
#include "liquid_cache/liquid_byte_view_array.h"
#include "liquid_cache/liquid_decimal_array.h"
#include "liquid_cache/liquid_fixed_len_byte_array.h"
#include "liquid_cache/liquid_array.h"
#include "liquid_cache/liquid_cache_store.h"
#include "liquid_cache/compressor_states.h"

namespace liquid_cache {

// ═══════════════════════════════════════════════════════════════════════
// Arrow-aware transcoding entry point
// ═══════════════════════════════════════════════════════════════════════

/// Transcode a single Arrow array column into Liquid Cache format.
///
/// Type dispatch mirrors the Rust implementation:
///   - Int8..Int64, UInt8..UInt64, Date32, Date64 → FoR + BitPacking
///   - Float32, Float64 → ALP + BitPacking
///   - Utf8, LargeUtf8, Binary, LargeBinary → (placeholder, FSST not yet implemented)
///   - Timestamp variants → FoR + BitPacking (cast to int64)
///
/// @param array   The Arrow array to transcode
/// @return        LiquidEncodedArray containing the serialized bytes
LiquidEncodedArray transcode_arrow_array(const std::shared_ptr<arrow::Array>& array) {
    switch (array->type_id()) {
        // ── Integer types: Frame-of-Reference + BitPacking ──────────
        case arrow::Type::INT8: {
            auto liquid = LiquidPrimitiveArray<arrow::Int8Type>::from_arrow(array);
            LiquidEncodedArray result;
            result.logical_type = LiquidDataType::Integer;
            result.physical_type = PhysicalType::Int8;
            result.serialized_bytes = liquid.to_bytes();
            result.length = liquid.length();
            result.memory_size = liquid.memory_size();
            return result;
        }
        case arrow::Type::INT16: {
            auto liquid = LiquidPrimitiveArray<arrow::Int16Type>::from_arrow(array);
            LiquidEncodedArray result;
            result.logical_type = LiquidDataType::Integer;
            result.physical_type = PhysicalType::Int16;
            result.serialized_bytes = liquid.to_bytes();
            result.length = liquid.length();
            result.memory_size = liquid.memory_size();
            return result;
        }
        case arrow::Type::INT32: {
            auto liquid = LiquidPrimitiveArray<arrow::Int32Type>::from_arrow(array);
            LiquidEncodedArray result;
            result.logical_type = LiquidDataType::Integer;
            result.physical_type = PhysicalType::Int32;
            result.serialized_bytes = liquid.to_bytes();
            result.length = liquid.length();
            result.memory_size = liquid.memory_size();
            return result;
        }
        case arrow::Type::INT64: {
            auto liquid = LiquidPrimitiveArray<arrow::Int64Type>::from_arrow(array);
            LiquidEncodedArray result;
            result.logical_type = LiquidDataType::Integer;
            result.physical_type = PhysicalType::Int64;
            result.serialized_bytes = liquid.to_bytes();
            result.length = liquid.length();
            result.memory_size = liquid.memory_size();
            return result;
        }

        // ── Unsigned integer types ──────────────────────────────────
        case arrow::Type::UINT8: {
            auto liquid = LiquidPrimitiveArray<arrow::UInt8Type>::from_arrow(array);
            LiquidEncodedArray result;
            result.logical_type = LiquidDataType::Integer;
            result.physical_type = PhysicalType::UInt8;
            result.serialized_bytes = liquid.to_bytes();
            result.length = liquid.length();
            result.memory_size = liquid.memory_size();
            return result;
        }
        case arrow::Type::UINT16: {
            auto liquid = LiquidPrimitiveArray<arrow::UInt16Type>::from_arrow(array);
            LiquidEncodedArray result;
            result.logical_type = LiquidDataType::Integer;
            result.physical_type = PhysicalType::UInt16;
            result.serialized_bytes = liquid.to_bytes();
            result.length = liquid.length();
            result.memory_size = liquid.memory_size();
            return result;
        }
        case arrow::Type::UINT32: {
            auto liquid = LiquidPrimitiveArray<arrow::UInt32Type>::from_arrow(array);
            LiquidEncodedArray result;
            result.logical_type = LiquidDataType::Integer;
            result.physical_type = PhysicalType::UInt32;
            result.serialized_bytes = liquid.to_bytes();
            result.length = liquid.length();
            result.memory_size = liquid.memory_size();
            return result;
        }
        case arrow::Type::UINT64: {
            auto liquid = LiquidPrimitiveArray<arrow::UInt64Type>::from_arrow(array);
            LiquidEncodedArray result;
            result.logical_type = LiquidDataType::Integer;
            result.physical_type = PhysicalType::UInt64;
            result.serialized_bytes = liquid.to_bytes();
            result.length = liquid.length();
            result.memory_size = liquid.memory_size();
            return result;
        }

        // ── Date types (FoR + BitPacking, same as integers) ─────────
        case arrow::Type::DATE32: {
            auto liquid = LiquidPrimitiveArray<arrow::Date32Type>::from_arrow(array);
            LiquidEncodedArray result;
            result.logical_type = LiquidDataType::Integer;
            result.physical_type = PhysicalType::Date32;
            result.serialized_bytes = liquid.to_bytes();
            result.length = liquid.length();
            result.memory_size = liquid.memory_size();
            return result;
        }
        case arrow::Type::DATE64: {
            auto liquid = LiquidPrimitiveArray<arrow::Date64Type>::from_arrow(array);
            LiquidEncodedArray result;
            result.logical_type = LiquidDataType::Integer;
            result.physical_type = PhysicalType::Date64;
            result.serialized_bytes = liquid.to_bytes();
            result.length = liquid.length();
            result.memory_size = liquid.memory_size();
            return result;
        }

        // ── Timestamp types → treat as Int64 with timestamp physical type ──
        case arrow::Type::TIMESTAMP: {
            auto ts_type = std::static_pointer_cast<arrow::TimestampType>(array->type());
            // Reject timestamps with timezone (matching Rust behavior)
            if (!ts_type->timezone().empty()) {
                LiquidEncodedArray result;
                result.length = static_cast<uint32_t>(array->length());
                return result;
            }
            PhysicalType phys;
            switch (ts_type->unit()) {
                case arrow::TimeUnit::SECOND:      phys = PhysicalType::TimestampSecond; break;
                case arrow::TimeUnit::MILLI:       phys = PhysicalType::TimestampMillisecond; break;
                case arrow::TimeUnit::MICRO:       phys = PhysicalType::TimestampMicrosecond; break;
                case arrow::TimeUnit::NANO:        phys = PhysicalType::TimestampNanosecond; break;
                default:                           phys = PhysicalType::TimestampMicrosecond; break;
            }
            // Timestamps are stored as int64 internally.
            // We must create an Int64Array view BEFORE calling from_arrow(),
            // because arrow::compute::MinMax on a TimestampArray returns
            // TimestampScalar (not Int64Scalar), and the template would
            // static_pointer_cast to Int64Scalar incorrectly.
            auto ts_data = array->data();
            auto int64_data = arrow::ArrayData::Make(
                arrow::int64(), ts_data->length,
                ts_data->buffers, ts_data->null_count, ts_data->offset);
            auto int64_view = arrow::MakeArray(int64_data);
            auto liquid = LiquidPrimitiveArray<arrow::Int64Type>::from_arrow(int64_view);
            LiquidEncodedArray result;
            result.logical_type = LiquidDataType::Integer;
            result.physical_type = phys;
            result.serialized_bytes = liquid.to_bytes();
            // Patch the IPC header: to_bytes() writes PhysicalType::Int64 (from
            // template parameter), but we need the actual timestamp physical type.
            // physical_type_id lives at offset 8 in the 16-byte header.
            uint16_t phys_id = static_cast<uint16_t>(phys);
            std::memcpy(result.serialized_bytes.data() + 8, &phys_id, 2);
            result.length = liquid.length();
            result.memory_size = liquid.memory_size();
            return result;
        }

        // ── Float types: ALP + BitPacking ───────────────────────────
        case arrow::Type::FLOAT: {
            auto liquid = LiquidFloatArray<float>::from_arrow(array);
            LiquidEncodedArray result;
            result.logical_type = LiquidDataType::Float;
            result.physical_type = PhysicalType::Float32;
            result.serialized_bytes = liquid.to_bytes();
            result.length = static_cast<uint32_t>(array->length());
            result.memory_size = liquid.memory_size();
            return result;
        }
        case arrow::Type::DOUBLE: {
            auto liquid = LiquidFloatArray<double>::from_arrow(array);
            LiquidEncodedArray result;
            result.logical_type = LiquidDataType::Float;
            result.physical_type = PhysicalType::Float64;
            result.serialized_bytes = liquid.to_bytes();
            result.length = static_cast<uint32_t>(array->length());
            result.memory_size = liquid.memory_size();
            return result;
        }

        // ── String / Binary types: FSST Dictionary compression ──────
        case arrow::Type::STRING:
        case arrow::Type::LARGE_STRING:
        case arrow::Type::BINARY:
        case arrow::Type::LARGE_BINARY: {
            auto liquid = LiquidByteViewArray::from_arrow(array);
            LiquidEncodedArray result;
            result.logical_type = LiquidDataType::ByteViewArray;
            result.physical_type = PhysicalType::Int8;  // placeholder for strings
            result.serialized_bytes = liquid.to_bytes();
            result.length = liquid.length();
            result.memory_size = liquid.memory_size();
            return result;
        }

        // ── StringView / BinaryView: cast to String/Binary first ────
        case arrow::Type::STRING_VIEW: {
            auto cast_result = arrow::compute::Cast(array, arrow::utf8());
            if (!cast_result.ok()) {
                LiquidEncodedArray result;
                result.length = static_cast<uint32_t>(array->length());
                return result;
            }
            auto decoded = cast_result.ValueOrDie().make_array();
            auto liquid = LiquidByteViewArray::from_arrow(decoded);
            LiquidEncodedArray result;
            result.logical_type = LiquidDataType::ByteViewArray;
            result.physical_type = PhysicalType::Int8;
            result.serialized_bytes = liquid.to_bytes();
            result.length = liquid.length();
            result.memory_size = liquid.memory_size();
            return result;
        }
        case arrow::Type::BINARY_VIEW: {
            auto cast_result = arrow::compute::Cast(array, arrow::binary());
            if (!cast_result.ok()) {
                LiquidEncodedArray result;
                result.length = static_cast<uint32_t>(array->length());
                return result;
            }
            auto decoded = cast_result.ValueOrDie().make_array();
            auto liquid = LiquidByteViewArray::from_arrow(decoded);
            LiquidEncodedArray result;
            result.logical_type = LiquidDataType::ByteViewArray;
            result.physical_type = PhysicalType::UInt8;
            result.serialized_bytes = liquid.to_bytes();
            result.length = liquid.length();
            result.memory_size = liquid.memory_size();
            return result;
        }

        // ── Dictionary types (String/Binary with UInt16 keys) ────────
        case arrow::Type::DICTIONARY: {
            auto dict_type = std::static_pointer_cast<arrow::DictionaryType>(array->type());
            auto value_type_id = dict_type->value_type()->id();
            // Only support string/binary dictionary types with UInt16 keys
            if (dict_type->index_type()->id() == arrow::Type::UINT16 &&
                (value_type_id == arrow::Type::STRING ||
                 value_type_id == arrow::Type::BINARY ||
                 value_type_id == arrow::Type::LARGE_STRING ||
                 value_type_id == arrow::Type::LARGE_BINARY)) {
                // Direct encode from DictionaryArray — reuse Arrow's keys+values
                auto liquid = LiquidByteViewArray::from_dict_array(array);
                LiquidEncodedArray result;
                result.logical_type = LiquidDataType::ByteViewArray;
                result.physical_type = PhysicalType::Int8;
                result.serialized_bytes = liquid.to_bytes();
                result.length = liquid.length();
                result.memory_size = liquid.memory_size();
                return result;
            }
            // Unsupported dictionary type
            LiquidEncodedArray result;
            result.length = static_cast<uint32_t>(array->length());
            return result;
        }

        // ── Decimal128: FoR + BitPacking (fits-u64) or FSST Dictionary ──
        case arrow::Type::DECIMAL128: {
            if (LiquidDecimalArray::fits_u64(array)) {
                auto liquid = LiquidDecimalArray::from_arrow(array);
                LiquidEncodedArray result;
                result.logical_type = LiquidDataType::Decimal;
                result.physical_type = PhysicalType::UInt64;
                result.serialized_bytes = liquid.to_bytes();
                result.length = liquid.length();
                result.memory_size = liquid.memory_size();
                return result;
            }
            // Large Decimal128: use Dictionary + FSST compression
            auto liquid = LiquidFixedLenByteArray::from_decimal128(array);
            LiquidEncodedArray result;
            result.logical_type = LiquidDataType::FixedLenByteArray;
            result.physical_type = PhysicalType::UInt16;
            result.serialized_bytes = liquid.to_bytes();
            result.length = liquid.length();
            result.memory_size = liquid.memory_size();
            return result;
        }

        // ── Decimal256: FoR + BitPacking (fits-u64) or FSST Dictionary ──
        case arrow::Type::DECIMAL256: {
            if (LiquidDecimalArray::fits_u64(array)) {
                auto liquid = LiquidDecimalArray::from_arrow(array);
                LiquidEncodedArray result;
                result.logical_type = LiquidDataType::Decimal;
                result.physical_type = PhysicalType::UInt64;
                result.serialized_bytes = liquid.to_bytes();
                result.length = liquid.length();
                result.memory_size = liquid.memory_size();
                return result;
            }
            // Large Decimal256: use Dictionary + FSST compression
            auto liquid = LiquidFixedLenByteArray::from_decimal256(array);
            LiquidEncodedArray result;
            result.logical_type = LiquidDataType::FixedLenByteArray;
            result.physical_type = PhysicalType::UInt16;
            result.serialized_bytes = liquid.to_bytes();
            result.length = liquid.length();
            result.memory_size = liquid.memory_size();
            return result;
        }

        default: {
            // Unsupported type - return empty result
            LiquidEncodedArray result;
            result.length = static_cast<uint32_t>(array->length());
            return result;
        }
    }
}

/// Transcode an entire Arrow RecordBatch into Liquid Cache format.
///
/// Each column is independently transcoded. This mirrors the Rust
/// `transcode_liquid_inner()` being called per-column during cache insertion.
///
/// @param batch  The Arrow RecordBatch to transcode
/// @return       Vector of LiquidEncodedArray, one per column
std::vector<LiquidEncodedArray> transcode_record_batch(
        const std::shared_ptr<arrow::RecordBatch>& batch) {
    std::vector<LiquidEncodedArray> result;
    result.reserve(batch->num_columns());

    for (int i = 0; i < batch->num_columns(); ++i) {
        result.push_back(transcode_arrow_array(batch->column(i)));
    }
    return result;
}

/// Decode a Liquid encoded array back to Arrow format.
///
/// This reads the LiquidIPCHeader to determine the type, then dispatches
/// to the appropriate decoder. Mirrors Rust `read_from_bytes()` in ipc.rs.
///
/// @param encoded  The LiquidEncodedArray to decode
/// @return         Arrow array, or nullptr for unsupported types
std::shared_ptr<arrow::Array> decode_liquid_array(const LiquidEncodedArray& encoded) {
    if (!encoded.is_valid()) return nullptr;

    const uint8_t* data = encoded.serialized_bytes.data();
    size_t len = encoded.serialized_bytes.size();

    auto header = LiquidIPCHeader::deserialize(data, len);
    auto logical = static_cast<LiquidDataType>(header.logical_type_id);
    auto physical = static_cast<PhysicalType>(header.physical_type_id);

    if (logical == LiquidDataType::Integer) {
        switch (physical) {
            case PhysicalType::Int8:
                return LiquidPrimitiveArray<arrow::Int8Type>::from_bytes(data, len).to_arrow();
            case PhysicalType::Int16:
                return LiquidPrimitiveArray<arrow::Int16Type>::from_bytes(data, len).to_arrow();
            case PhysicalType::Int32:
                return LiquidPrimitiveArray<arrow::Int32Type>::from_bytes(data, len).to_arrow();
            case PhysicalType::Int64:
                return LiquidPrimitiveArray<arrow::Int64Type>::from_bytes(data, len).to_arrow();
            case PhysicalType::TimestampSecond:
            case PhysicalType::TimestampMillisecond:
            case PhysicalType::TimestampMicrosecond:
            case PhysicalType::TimestampNanosecond: {
                // Decode as Int64, then reconstruct as TimestampArray (zero-copy)
                auto int64_arr = LiquidPrimitiveArray<arrow::Int64Type>::from_bytes(data, len).to_arrow();
                arrow::TimeUnit::type unit;
                switch (physical) {
                    case PhysicalType::TimestampSecond:      unit = arrow::TimeUnit::SECOND; break;
                    case PhysicalType::TimestampMillisecond:  unit = arrow::TimeUnit::MILLI; break;
                    case PhysicalType::TimestampMicrosecond:  unit = arrow::TimeUnit::MICRO; break;
                    case PhysicalType::TimestampNanosecond:   unit = arrow::TimeUnit::NANO; break;
                    default: unit = arrow::TimeUnit::MICRO; break;
                }
                auto ts_type = arrow::timestamp(unit);
                auto arr_data = int64_arr->data();
                auto new_data = arrow::ArrayData::Make(
                    ts_type, arr_data->length,
                    arr_data->buffers, arr_data->null_count, arr_data->offset);
                return arrow::MakeArray(new_data);
            }
            case PhysicalType::UInt8:
                return LiquidPrimitiveArray<arrow::UInt8Type>::from_bytes(data, len).to_arrow();
            case PhysicalType::UInt16:
                return LiquidPrimitiveArray<arrow::UInt16Type>::from_bytes(data, len).to_arrow();
            case PhysicalType::UInt32:
                return LiquidPrimitiveArray<arrow::UInt32Type>::from_bytes(data, len).to_arrow();
            case PhysicalType::UInt64:
                return LiquidPrimitiveArray<arrow::UInt64Type>::from_bytes(data, len).to_arrow();
            case PhysicalType::Date32:
                return LiquidPrimitiveArray<arrow::Date32Type>::from_bytes(data, len).to_arrow();
            case PhysicalType::Date64:
                return LiquidPrimitiveArray<arrow::Date64Type>::from_bytes(data, len).to_arrow();
            default:
                return nullptr;
        }
    } else if (logical == LiquidDataType::Float) {
        switch (physical) {
            case PhysicalType::Float32:
                return LiquidFloatArray<float>::from_bytes(data, len).to_arrow();
            case PhysicalType::Float64:
                return LiquidFloatArray<double>::from_bytes(data, len).to_arrow();
            default:
                return nullptr;
        }
    } else if (logical == LiquidDataType::ByteViewArray) {
        return LiquidByteViewArray::from_bytes(data, len).to_arrow();
    } else if (logical == LiquidDataType::Decimal) {
        return LiquidDecimalArray::from_bytes(data, len).to_arrow();
    } else if (logical == LiquidDataType::FixedLenByteArray) {
        return LiquidFixedLenByteArray::from_bytes(data, len).to_arrow();
    } else if (logical == LiquidDataType::LinearInteger) {
        switch (physical) {
            case PhysicalType::Int8:
                return LiquidLinearIntegerArray<arrow::Int8Type>::from_bytes(data, len).to_arrow();
            case PhysicalType::Int16:
                return LiquidLinearIntegerArray<arrow::Int16Type>::from_bytes(data, len).to_arrow();
            case PhysicalType::Int32:
                return LiquidLinearIntegerArray<arrow::Int32Type>::from_bytes(data, len).to_arrow();
            case PhysicalType::Int64:
                return LiquidLinearIntegerArray<arrow::Int64Type>::from_bytes(data, len).to_arrow();
            case PhysicalType::UInt8:
                return LiquidLinearIntegerArray<arrow::UInt8Type>::from_bytes(data, len).to_arrow();
            case PhysicalType::UInt16:
                return LiquidLinearIntegerArray<arrow::UInt16Type>::from_bytes(data, len).to_arrow();
            case PhysicalType::UInt32:
                return LiquidLinearIntegerArray<arrow::UInt32Type>::from_bytes(data, len).to_arrow();
            case PhysicalType::UInt64:
                return LiquidLinearIntegerArray<arrow::UInt64Type>::from_bytes(data, len).to_arrow();
            case PhysicalType::Date32:
                return LiquidLinearIntegerArray<arrow::Date32Type>::from_bytes(data, len).to_arrow();
            case PhysicalType::Date64:
                return LiquidLinearIntegerArray<arrow::Date64Type>::from_bytes(data, len).to_arrow();
            default:
                return nullptr;
        }
    }

    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════════
// transcode_to_liquid_array: Arrow → in-memory Liquid struct (no serialization)
//
// This is the key function for the cache store. Unlike transcode_arrow_array()
// which produces serialized bytes (LiquidEncodedArray), this returns a
// LiquidArrayRef holding the Liquid struct directly in memory.
//
// Equivalent to Rust's transcode_liquid_inner() from
// liquid-cache/src/core/src/cache/transcode.rs
// ═══════════════════════════════════════════════════════════════════════

LiquidArrayRef transcode_to_liquid_array(
        const std::shared_ptr<arrow::Array>& array) {
    auto orig_type = array->type();

    switch (array->type_id()) {
        // ── Integer types ────────────────────────────────────────────
        case arrow::Type::INT8:
            return make_liquid_array(
                LiquidPrimitiveArray<arrow::Int8Type>::from_arrow(array),
                LiquidDataType::Integer, PhysicalType::Int8, orig_type);
        case arrow::Type::INT16:
            return make_liquid_array(
                LiquidPrimitiveArray<arrow::Int16Type>::from_arrow(array),
                LiquidDataType::Integer, PhysicalType::Int16, orig_type);
        case arrow::Type::INT32:
            return make_liquid_array(
                LiquidPrimitiveArray<arrow::Int32Type>::from_arrow(array),
                LiquidDataType::Integer, PhysicalType::Int32, orig_type);
        case arrow::Type::INT64:
            return make_liquid_array(
                LiquidPrimitiveArray<arrow::Int64Type>::from_arrow(array),
                LiquidDataType::Integer, PhysicalType::Int64, orig_type);

        // ── Unsigned integer types ───────────────────────────────────
        case arrow::Type::UINT8:
            return make_liquid_array(
                LiquidPrimitiveArray<arrow::UInt8Type>::from_arrow(array),
                LiquidDataType::Integer, PhysicalType::UInt8, orig_type);
        case arrow::Type::UINT16:
            return make_liquid_array(
                LiquidPrimitiveArray<arrow::UInt16Type>::from_arrow(array),
                LiquidDataType::Integer, PhysicalType::UInt16, orig_type);
        case arrow::Type::UINT32:
            return make_liquid_array(
                LiquidPrimitiveArray<arrow::UInt32Type>::from_arrow(array),
                LiquidDataType::Integer, PhysicalType::UInt32, orig_type);
        case arrow::Type::UINT64:
            return make_liquid_array(
                LiquidPrimitiveArray<arrow::UInt64Type>::from_arrow(array),
                LiquidDataType::Integer, PhysicalType::UInt64, orig_type);

        // ── Date types ───────────────────────────────────────────────
        case arrow::Type::DATE32:
            return make_liquid_array(
                LiquidPrimitiveArray<arrow::Date32Type>::from_arrow(array),
                LiquidDataType::Integer, PhysicalType::Date32, orig_type);
        case arrow::Type::DATE64:
            return make_liquid_array(
                LiquidPrimitiveArray<arrow::Date64Type>::from_arrow(array),
                LiquidDataType::Integer, PhysicalType::Date64, orig_type);

        // ── Timestamp types (stored as Int64, original type preserved) ──
        case arrow::Type::TIMESTAMP: {
            auto ts_type = std::static_pointer_cast<arrow::TimestampType>(orig_type);
            // Reject timestamps with timezone (matching Rust behavior)
            if (!ts_type->timezone().empty()) return nullptr;
            PhysicalType phys;
            switch (ts_type->unit()) {
                case arrow::TimeUnit::SECOND:      phys = PhysicalType::TimestampSecond; break;
                case arrow::TimeUnit::MILLI:       phys = PhysicalType::TimestampMillisecond; break;
                case arrow::TimeUnit::MICRO:       phys = PhysicalType::TimestampMicrosecond; break;
                case arrow::TimeUnit::NANO:        phys = PhysicalType::TimestampNanosecond; break;
                default:                           phys = PhysicalType::TimestampMicrosecond; break;
            }
            auto ts_data = array->data();
            auto int64_data = arrow::ArrayData::Make(
                arrow::int64(), ts_data->length,
                ts_data->buffers, ts_data->null_count, ts_data->offset);
            auto int64_view = arrow::MakeArray(int64_data);
            return make_liquid_array(
                LiquidPrimitiveArray<arrow::Int64Type>::from_arrow(int64_view),
                LiquidDataType::Integer, phys, orig_type);
        }

        // ── Float types ──────────────────────────────────────────────
        case arrow::Type::FLOAT:
            return make_liquid_array(
                LiquidFloatArray<float>::from_arrow(array),
                LiquidDataType::Float, PhysicalType::Float32, orig_type);
        case arrow::Type::DOUBLE:
            return make_liquid_array(
                LiquidFloatArray<double>::from_arrow(array),
                LiquidDataType::Float, PhysicalType::Float64, orig_type);

        // ── String / Binary types ────────────────────────────────────
        case arrow::Type::STRING:
        case arrow::Type::LARGE_STRING:
        case arrow::Type::BINARY:
        case arrow::Type::LARGE_BINARY: {
            bool is_binary = (array->type_id() == arrow::Type::BINARY ||
                              array->type_id() == arrow::Type::LARGE_BINARY);
            PhysicalType phys = is_binary ? PhysicalType::UInt8 : PhysicalType::Int8;
            return make_liquid_array(
                LiquidByteViewArray::from_arrow(array),
                LiquidDataType::ByteViewArray, phys, orig_type);
        }

        // ── StringView / BinaryView: cast to String/Binary first ────
        case arrow::Type::STRING_VIEW: {
            auto cast_result = arrow::compute::Cast(array, arrow::utf8());
            if (!cast_result.ok()) return nullptr;
            auto decoded = cast_result.ValueOrDie().make_array();
            return make_liquid_array(
                LiquidByteViewArray::from_arrow(decoded),
                LiquidDataType::ByteViewArray, PhysicalType::Int8, orig_type);
        }
        case arrow::Type::BINARY_VIEW: {
            auto cast_result = arrow::compute::Cast(array, arrow::binary());
            if (!cast_result.ok()) return nullptr;
            auto decoded = cast_result.ValueOrDie().make_array();
            return make_liquid_array(
                LiquidByteViewArray::from_arrow(decoded),
                LiquidDataType::ByteViewArray, PhysicalType::UInt8, orig_type);
        }

        // ── Dictionary types (String/Binary with UInt16/UINT32/INT32 keys) ────────
        case arrow::Type::DICTIONARY:
            {
                auto dict_type = std::static_pointer_cast<arrow::DictionaryType>(orig_type);
                auto value_type_id = dict_type->value_type()->id();
                auto index_type_id = dict_type->index_type()->id();
                if ((index_type_id == arrow::Type::UINT16 || index_type_id == arrow::Type::UINT32 || index_type_id == arrow::Type::INT32) &&
                (value_type_id == arrow::Type::STRING ||
                 value_type_id == arrow::Type::BINARY ||
                 value_type_id == arrow::Type::LARGE_STRING ||
                 value_type_id == arrow::Type::LARGE_BINARY)) {
                bool is_binary = (value_type_id == arrow::Type::BINARY ||
                                  value_type_id == arrow::Type::LARGE_BINARY);
                PhysicalType phys = is_binary ? PhysicalType::UInt8 : PhysicalType::Int8;
                return make_liquid_array(
                    LiquidByteViewArray::from_dict_array(array),
                    LiquidDataType::ByteViewArray, phys, orig_type);
            }
            return nullptr;
        }

        // ── Decimal128 ──────────────────────────────────────────────
        case arrow::Type::DECIMAL128: {
            if (LiquidDecimalArray::fits_u64(array)) {
                return make_liquid_array(
                    LiquidDecimalArray::from_arrow(array),
                    LiquidDataType::Decimal, PhysicalType::UInt64, orig_type);
            }
            // Large Decimal128: use Dictionary + FSST compression
            return make_liquid_array(
                LiquidFixedLenByteArray::from_decimal128(array),
                LiquidDataType::FixedLenByteArray, PhysicalType::UInt16, orig_type);
        }

        // ── Decimal256 ──────────────────────────────────────────────
        case arrow::Type::DECIMAL256: {
            if (LiquidDecimalArray::fits_u64(array)) {
                return make_liquid_array(
                    LiquidDecimalArray::from_arrow(array),
                    LiquidDataType::Decimal, PhysicalType::UInt64, orig_type);
            }
            // Large Decimal256: use Dictionary + FSST compression
            return make_liquid_array(
                LiquidFixedLenByteArray::from_decimal256(array),
                LiquidDataType::FixedLenByteArray, PhysicalType::UInt16, orig_type);
        }

        // ── Run-End Encoded ─────────────────────────────────────────
        case arrow::Type::RUN_END_ENCODED: {
            // Expand run-end encoded array to regular array, then transcode
            auto ree_array = std::static_pointer_cast<arrow::RunEndEncodedArray>(array);
            auto run_ends = ree_array->run_ends();
            auto values = ree_array->values();
            int64_t length = ree_array->length();

            // Build a builder for the values type
            auto value_type = values->type();
            std::shared_ptr<arrow::Array> expanded;
            if (value_type->id() == arrow::Type::INT32) {
                // Expand int32 run-end encoded
                arrow::Int32Builder builder(arrow::int32(), arrow::default_memory_pool());
                auto run_ends_int32 = std::static_pointer_cast<arrow::Int32Array>(run_ends);
                auto values_int32 = std::static_pointer_cast<arrow::Int32Array>(values);
                int32_t prev_end = 0;
                for (int64_t i = 0; i < run_ends_int32->length(); ++i) {
                    int32_t run_end = run_ends_int32->Value(i);
                    int32_t value = values_int32->Value(i);
                    int32_t run_length = run_end - prev_end;
                    auto status = builder.AppendValues(std::vector<int32_t>(run_length, value));
                    if (!status.ok()) {
                        ARROW_LOG(WARNING) << "LiquidCache: Failed to expand RUN_END_ENCODED int32: " << status.ToString();
                        return nullptr;
                    }
                    prev_end = run_end;
                }
                auto finish_result = builder.Finish();
                if (!finish_result.ok()) {
                    ARROW_LOG(WARNING) << "LiquidCache: Failed to finish RUN_END_ENCODED int32 builder: " << finish_result.status().ToString();
                    return nullptr;
                }
                expanded = finish_result.ValueOrDie();
            } else if (value_type->id() == arrow::Type::INT64) {
                // Expand int64 run-end encoded
                arrow::Int64Builder builder(arrow::int64(), arrow::default_memory_pool());
                auto run_ends_int64 = std::static_pointer_cast<arrow::Int64Array>(run_ends);
                auto values_int64 = std::static_pointer_cast<arrow::Int64Array>(values);
                int64_t prev_end = 0;
                for (int64_t i = 0; i < run_ends_int64->length(); ++i) {
                    int64_t run_end = run_ends_int64->Value(i);
                    int64_t value = values_int64->Value(i);
                    int64_t run_length = run_end - prev_end;
                    auto status = builder.AppendValues(std::vector<int64_t>(run_length, value));
                    if (!status.ok()) {
                        ARROW_LOG(WARNING) << "LiquidCache: Failed to expand RUN_END_ENCODED int64: " << status.ToString();
                        return nullptr;
                    }
                    prev_end = run_end;
                }
                auto finish_result = builder.Finish();
                if (!finish_result.ok()) {
                    ARROW_LOG(WARNING) << "LiquidCache: Failed to finish RUN_END_ENCODED int64 builder: " << finish_result.status().ToString();
                    return nullptr;
                }
                expanded = finish_result.ValueOrDie();
            } else {
                // Log unsupported run-end encoded types for debugging
                ARROW_LOG(WARNING) << "LiquidCache: Unsupported RUN_END_ENCODED value type: "
                                   << value_type->ToString()
                                   << " run_ends type: " << run_ends->type()->ToString()
                                   << " length: " << length;
                return nullptr;
            }

            // Transcode the expanded array
            return transcode_to_liquid_array(expanded);
        }

        default:
            return nullptr;
    }
}

/// Transcode with optional compressor states for FSST cross-batch reuse.
/// Mirrors Rust's pattern: LiquidCompressorStates holds per-column FSST
/// compressors that are trained on the first batch and reused for subsequent
/// batches, avoiding redundant FSST training.
LiquidArrayRef transcode_to_liquid_array(
        const std::shared_ptr<arrow::Array>& array,
        LiquidCompressorStates* states) {
    auto orig_type = array->type();

    switch (array->type_id()) {
        // ── Types that use FSST ────────────────────────────────────
        case arrow::Type::STRING:
        case arrow::Type::LARGE_STRING:
        case arrow::Type::BINARY:
        case arrow::Type::LARGE_BINARY: {
            bool is_binary = (array->type_id() == arrow::Type::BINARY ||
                             array->type_id() == arrow::Type::LARGE_BINARY);
            PhysicalType phys = is_binary ? PhysicalType::UInt8 : PhysicalType::Int8;

            if (states) {
                auto liquid = with_fsst_compressor_or_train(*states,
                    [&](std::shared_ptr<FsstCompressor> comp) {
                        return LiquidByteViewArray::from_arrow_with_compressor(
                            array, *comp);
                    },
                    [&]() {
                        auto lq = LiquidByteViewArray::from_arrow(array);
                        auto comp = std::make_shared<FsstCompressor>(
                            lq.get_compressor());
                        return std::make_pair(std::move(comp), std::move(lq));
                    });
                return make_liquid_array(
                    std::move(liquid), LiquidDataType::ByteViewArray,
                    phys, orig_type);
            }
            return make_liquid_array(
                LiquidByteViewArray::from_arrow(array),
                LiquidDataType::ByteViewArray, phys, orig_type);
        }

        case arrow::Type::STRING_VIEW:
        case arrow::Type::BINARY_VIEW:
        case arrow::Type::DICTIONARY: {
            if (!states) {
                // Fall back to original single-arg path
                return transcode_to_liquid_array(array);
            }
            // These types are cast to plain String/Binary first,
            // then go through the same FSST reuse path.
            // Use the original path which handles the casting internally.
            return transcode_to_liquid_array(array);
        }

        case arrow::Type::DECIMAL128: {
            if (LiquidDecimalArray::fits_u64(array)) {
                return make_liquid_array(
                    LiquidDecimalArray::from_arrow(array),
                    LiquidDataType::Decimal, PhysicalType::UInt64, orig_type);
            }
            if (states) {
                auto liquid = with_fsst_compressor_or_train(*states,
                    [&](std::shared_ptr<FsstCompressor> comp) {
                        return LiquidFixedLenByteArray::
                            from_decimal128_with_compressor(array, *comp);
                    },
                    [&]() {
                        auto lq = LiquidFixedLenByteArray::from_decimal128(array);
                        auto comp = std::make_shared<FsstCompressor>(
                            lq.get_compressor());
                        return std::make_pair(std::move(comp), std::move(lq));
                    });
                return make_liquid_array(
                    std::move(liquid), LiquidDataType::FixedLenByteArray,
                    PhysicalType::UInt16, orig_type);
            }
            return make_liquid_array(
                LiquidFixedLenByteArray::from_decimal128(array),
                LiquidDataType::FixedLenByteArray, PhysicalType::UInt16, orig_type);
        }

        case arrow::Type::DECIMAL256: {
            if (LiquidDecimalArray::fits_u64(array)) {
                return make_liquid_array(
                    LiquidDecimalArray::from_arrow(array),
                    LiquidDataType::Decimal, PhysicalType::UInt64, orig_type);
            }
            if (states) {
                auto liquid = with_fsst_compressor_or_train(*states,
                    [&](std::shared_ptr<FsstCompressor> comp) {
                        return LiquidFixedLenByteArray::
                            from_decimal256_with_compressor(array, *comp);
                    },
                    [&]() {
                        auto lq = LiquidFixedLenByteArray::from_decimal256(array);
                        auto comp = std::make_shared<FsstCompressor>(
                            lq.get_compressor());
                        return std::make_pair(std::move(comp), std::move(lq));
                    });
                return make_liquid_array(
                    std::move(liquid), LiquidDataType::FixedLenByteArray,
                    PhysicalType::UInt16, orig_type);
            }
            return make_liquid_array(
                LiquidFixedLenByteArray::from_decimal256(array),
                LiquidDataType::FixedLenByteArray, PhysicalType::UInt16, orig_type);
        }

        // ── All other types: delegate to original function ─────────
        default:
            return transcode_to_liquid_array(array);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// LiquidCacheStore::load_from_parquet implementation
// ═══════════════════════════════════════════════════════════════════════

std::vector<LiquidCacheStore::RowGroupInfo> LiquidCacheStore::load_from_parquet(
        const std::vector<std::string>& files,
        std::shared_ptr<arrow::Schema>& schema,
        double& transcode_sec,
        const std::function<LiquidArrayRef(
            const std::shared_ptr<arrow::Array>&)>& transcode_fn) {
    std::vector<RowGroupInfo> rg_infos;
    auto t0 = std::chrono::steady_clock::now();

    for (const auto& path : files) {
        auto maybe_infile = arrow::io::ReadableFile::Open(path);
        if (!maybe_infile.ok()) { continue; }
        auto infile = maybe_infile.ValueOrDie();

        std::unique_ptr<parquet::arrow::FileReader> reader;
#if ARROW_VERSION_MAJOR >= 19
        auto open_result = parquet::arrow::OpenFile(
            infile, arrow::default_memory_pool());
        if (!open_result.ok()) { continue; }
        reader = std::move(open_result).ValueOrDie();
#else
        auto open_status = parquet::arrow::OpenFile(
            infile, arrow::default_memory_pool(), &reader);
        if (!open_status.ok()) { continue; }
#endif
        reader->set_batch_size(8192);

        if (!schema) {
            std::shared_ptr<arrow::Schema> file_schema;
            auto schema_status = reader->GetSchema(&file_schema);
            if (schema_status.ok()) {
                schema = file_schema;
            }
        }

        uint64_t file_id = std::hash<std::string>{}(path);
        int num_row_groups = reader->num_row_groups();

        // Collect row group file offsets for split->RG mapping
        FileRgMetadata fmeta;
        fmeta.rg_offsets.reserve(num_row_groups);
        {
            auto* pq_reader = reader->parquet_reader();
            auto meta = pq_reader->metadata();
            for (int i = 0; i < num_row_groups; ++i) {
                auto rg_meta = meta->RowGroup(i);
                auto off = static_cast<uint64_t>(rg_meta->file_offset());
                if (off == 0 && rg_meta->num_columns() > 0) {
                    auto cc0 = rg_meta->ColumnChunk(0);
                    // Match Velox's 3-tier fallback in filterRowGroups():
                    // file_offset → dictionary_page_offset → data_page_offset
                    if (cc0->has_dictionary_page())
                        off = static_cast<uint64_t>(cc0->dictionary_page_offset());
                    else
                        off = static_cast<uint64_t>(cc0->data_page_offset());
                }
                fmeta.rg_offsets.push_back(off);
            }
        }

        // Read each row group independently for correct rg_id tracking
        for (int rg_idx = 0; rg_idx < num_row_groups; ++rg_idx) {
            std::shared_ptr<arrow::RecordBatchReader> rg_reader;
#if ARROW_VERSION_MAJOR >= 19
            auto rb_result = reader->GetRecordBatchReader({rg_idx});
            if (!rb_result.ok()) { continue; }
            rg_reader = std::move(rb_result).ValueOrDie();
#else
            auto rb_status = reader->GetRecordBatchReader({rg_idx}, &rg_reader);
            if (!rb_status.ok()) { continue; }
#endif

            uint16_t batch_id = 0;
            size_t rg_rows = 0;

            while (true) {
                std::shared_ptr<arrow::RecordBatch> batch;
                auto st = rg_reader->ReadNext(&batch);
                if (!st.ok() || !batch) break;

                rg_rows += batch->num_rows();

                for (int c = 0; c < batch->num_columns(); ++c) {
                    LiquidCacheKey key(file_id,
                                       static_cast<uint16_t>(rg_idx),
                                       static_cast<uint16_t>(c), batch_id);
                    auto liquid = transcode_fn(batch->column(c));
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (liquid) {
                        entries_[key] = CacheEntry::from_liquid(std::move(liquid));
                    } else {
                        entries_[key] = CacheEntry::from_arrow(batch->column(c));
                    }
                }
                ++batch_id;
            }

            rg_infos.push_back({file_id,
                                static_cast<uint16_t>(rg_idx),
                                batch_id, rg_rows});
        }

        file_metadata_[file_id] = std::move(fmeta);
    }

    auto t1 = std::chrono::steady_clock::now();
    transcode_sec = std::chrono::duration<double>(t1 - t0).count();
    return rg_infos;
}

/// New overload: automatic FSST compressor reuse across batches.
/// Internally maintains per-column LiquidCompressorStates so that
/// FSST training happens only on the first batch and is reused for
/// all subsequent batches of the same column.
std::vector<LiquidCacheStore::RowGroupInfo> LiquidCacheStore::load_from_parquet(
        const std::vector<std::string>& files,
        std::shared_ptr<arrow::Schema>& schema,
        double& transcode_sec) {
    std::vector<RowGroupInfo> rg_infos;
    auto t0 = std::chrono::steady_clock::now();

    // Per-column compressor states — trained lazily on first batch
    std::vector<std::unique_ptr<LiquidCompressorStates>> compressor_states;

    for (const auto& path : files) {
        auto maybe_infile = arrow::io::ReadableFile::Open(path);
        if (!maybe_infile.ok()) { continue; }
        auto infile = maybe_infile.ValueOrDie();

        std::unique_ptr<parquet::arrow::FileReader> reader;
#if ARROW_VERSION_MAJOR >= 19
        auto open_result = parquet::arrow::OpenFile(
            infile, arrow::default_memory_pool());
        if (!open_result.ok()) { continue; }
        reader = std::move(open_result).ValueOrDie();
#else
        auto open_status = parquet::arrow::OpenFile(
            infile, arrow::default_memory_pool(), &reader);
        if (!open_status.ok()) { continue; }
#endif
        reader->set_batch_size(8192);

        if (!schema) {
            std::shared_ptr<arrow::Schema> file_schema;
            auto schema_status = reader->GetSchema(&file_schema);
            if (schema_status.ok()) {
                schema = file_schema;
                // Initialize compressor states once we know column count
                int ncols = schema->num_fields();
                compressor_states.reserve(ncols);
                for (int i = 0; i < ncols; ++i)
                    compressor_states.push_back(std::make_unique<LiquidCompressorStates>());
            }
        }

        uint64_t file_id = std::hash<std::string>{}(path);
        int num_row_groups = reader->num_row_groups();

        // Collect row group file offsets for split->RG mapping
        FileRgMetadata fmeta;
        fmeta.rg_offsets.reserve(num_row_groups);
        {
            auto* pq_reader = reader->parquet_reader();
            auto meta = pq_reader->metadata();
            for (int i = 0; i < num_row_groups; ++i) {
                auto rg_meta = meta->RowGroup(i);
                auto off = static_cast<uint64_t>(rg_meta->file_offset());
                if (off == 0 && rg_meta->num_columns() > 0) {
                    auto cc0 = rg_meta->ColumnChunk(0);
                    // Match Velox's 3-tier fallback in filterRowGroups():
                    // file_offset → dictionary_page_offset → data_page_offset
                    if (cc0->has_dictionary_page())
                        off = static_cast<uint64_t>(cc0->dictionary_page_offset());
                    else
                        off = static_cast<uint64_t>(cc0->data_page_offset());
                }
                fmeta.rg_offsets.push_back(off);
            }
        }

        // Read each row group independently for correct rg_id tracking
        for (int rg_idx = 0; rg_idx < num_row_groups; ++rg_idx) {
            std::shared_ptr<arrow::RecordBatchReader> rg_reader;
#if ARROW_VERSION_MAJOR >= 19
            auto rb_result = reader->GetRecordBatchReader({rg_idx});
            if (!rb_result.ok()) { continue; }
            rg_reader = std::move(rb_result).ValueOrDie();
#else
            auto rb_status = reader->GetRecordBatchReader({rg_idx}, &rg_reader);
            if (!rb_status.ok()) { continue; }
#endif

            uint16_t batch_id = 0;
            size_t rg_rows = 0;

            while (true) {
                std::shared_ptr<arrow::RecordBatch> batch;
                auto st = rg_reader->ReadNext(&batch);
                if (!st.ok() || !batch) break;

                rg_rows += batch->num_rows();

                for (int c = 0; c < batch->num_columns(); ++c) {
                    LiquidCacheKey key(file_id,
                                       static_cast<uint16_t>(rg_idx),
                                       static_cast<uint16_t>(c), batch_id);

                    // Use per-column compressor state for FSST reuse
                    LiquidCompressorStates* states = nullptr;
                    if (c < static_cast<int>(compressor_states.size())) {
                        states = compressor_states[c].get();
                    }
                    auto liquid = transcode_to_liquid_array(
                        batch->column(c), states);

                    std::lock_guard<std::mutex> lock(mutex_);
                    if (liquid) {
                        entries_[key] = CacheEntry::from_liquid(std::move(liquid));
                    } else {
                        entries_[key] = CacheEntry::from_arrow(batch->column(c));
                    }
                }
                ++batch_id;
            }

            rg_infos.push_back({file_id,
                                static_cast<uint16_t>(rg_idx),
                                batch_id, rg_rows});
        }

        file_metadata_[file_id] = std::move(fmeta);
    }

    auto t1 = std::chrono::steady_clock::now();
    transcode_sec = std::chrono::duration<double>(t1 - t0).count();
    return rg_infos;
}

}  // namespace liquid_cache
