// liquid_cache/liquid_decimal_array.h
// LiquidDecimalArray - Decimal128 encoding via FoR + BitPacking (fits-u64 path).
// Binary-compatible with the Rust LiquidDecimalArray serialization format.
//
// Serialization layout:
//   [0..16)   LiquidIPCHeader  (magic="LQDA", logical=Decimal, physical=UInt64)
//   [16..24)  DecimalArrayHeader (arrow_type, precision, scale, padding, reserved)
//   [24..32)  reference_value  (u64 LE, Frame-of-Reference minimum)
//   [32..)    BitPackedArray   (16-byte header + null bitmap + packed values)
#pragma once

#include <arrow/util/logging.h>
#include <arrow/api.h>
#include <arrow/compute/api.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "liquid_cache/bit_packed_array.h"
#include "liquid_cache/ipc_header.h"
#include "liquid_cache/liquid_arrays.h"  // for get_bit_width()

namespace liquid_cache {

// ═══════════════════════════════════════════════════════════════════════
// DecimalArrayHeader: 8 bytes - metadata for decimal precision/scale
// ═══════════════════════════════════════════════════════════════════════
#pragma pack(push, 1)
struct DecimalArrayHeader {
    uint8_t arrow_type;    // 0 = Decimal128, 1 = Decimal256
    uint8_t precision;
    int8_t  scale;
    uint8_t padding;
    uint32_t reserved;

    static constexpr size_t SIZE = 8;

    void serialize(std::vector<uint8_t>& out) const {
        // Match Rust's to_bytes(): only writes first 3 fields, rest zeroed
        uint8_t buf[SIZE] = {};
        buf[0] = arrow_type;
        buf[1] = precision;
        buf[2] = static_cast<uint8_t>(scale);
        out.insert(out.end(), buf, buf + SIZE);
    }

    static DecimalArrayHeader deserialize(const uint8_t* data, size_t len) {
        if (len < SIZE) {
            throw std::runtime_error("Buffer too small for DecimalArrayHeader");
        }
        DecimalArrayHeader h;
        h.arrow_type = data[0];
        h.precision = data[1];
        h.scale = static_cast<int8_t>(data[2]);
        h.padding = 0;
        h.reserved = 0;
        return h;
    }
};
#pragma pack(pop)
static_assert(sizeof(DecimalArrayHeader) == DecimalArrayHeader::SIZE,
              "DecimalArrayHeader must be exactly 8 bytes");

// ═══════════════════════════════════════════════════════════════════════
// LiquidDecimalArray: Decimal128 stored as compressed u64 primitives
// ═══════════════════════════════════════════════════════════════════════
class LiquidDecimalArray {
public:
    LiquidDecimalArray() = default;

    /// Check if all non-null values in a Decimal128/256Array fit in uint64_t.
    static bool fits_u64(const std::shared_ptr<arrow::Array>& array) {
        if (array->type_id() == arrow::Type::DECIMAL128) {
            auto dec_arr = std::static_pointer_cast<arrow::Decimal128Array>(array);
            for (int64_t i = 0; i < dec_arr->length(); ++i) {
                if (dec_arr->IsNull(i)) continue;
                arrow::Decimal128 val(dec_arr->Value(i));
                // Fits in u64 iff non-negative and high bits are zero
                if (val < arrow::Decimal128(0) || val.high_bits() != 0) {
                    return false;
                }
            }
            return true;
        } else if (array->type_id() == arrow::Type::DECIMAL256) {
            auto dec_arr = std::static_pointer_cast<arrow::Decimal256Array>(array);
            for (int64_t i = 0; i < dec_arr->length(); ++i) {
                if (dec_arr->IsNull(i)) continue;
                // Decimal256 fits in u64 only if bytes 8..31 are all zero
                // and the low 8 bytes represent a non-negative value
                const uint8_t* raw = dec_arr->Value(i);
                bool high_zero = true;
                for (int b = 8; b < 32; ++b) {
                    if (raw[b] != 0) { high_zero = false; break; }
                }
                if (!high_zero) return false;
                // Check sign: if any high byte is non-zero, value doesn't fit in u64
                // (already checked above). Low 8 bytes are the unsigned value.
            }
            return true;
        }
        return false;
    }

    /// Encode from Arrow Decimal128/256Array (only call if fits_u64() is true).
    static LiquidDecimalArray from_arrow(
            const std::shared_ptr<arrow::Array>& array) {
        LiquidDecimalArray result;

        int64_t len = array->length();

        if (array->type_id() == arrow::Type::DECIMAL128) {
            auto dec_arr = std::static_pointer_cast<arrow::Decimal128Array>(array);
            auto dec_type = std::static_pointer_cast<arrow::Decimal128Type>(array->type());
            result.arrow_type_ = 0;  // Decimal128
            result.precision_ = static_cast<uint8_t>(dec_type->precision());
            result.scale_ = static_cast<int8_t>(dec_type->scale());

            if (dec_arr->null_count() == len) {
                result.reference_value_ = 0;
                std::vector<uint64_t> zeros(len, 0);
                const uint8_t* null_bitmap = nullptr;
                std::vector<uint8_t> null_bits;
                if (dec_arr->null_bitmap()) {
                    size_t bitmap_bytes = (len + 7) / 8;
                    null_bits.assign(dec_arr->null_bitmap()->data(),
                                     dec_arr->null_bitmap()->data() + bitmap_bytes);
                    null_bitmap = null_bits.data();
                }
                result.bit_packed_ = BitPackedArray(
                    zeros.data(), null_bitmap, static_cast<uint32_t>(len), 0);
                return result;
            }

            std::vector<uint64_t> values(len);
            uint64_t min_val = UINT64_MAX;
            uint64_t max_val = 0;

            for (int64_t i = 0; i < len; ++i) {
                if (dec_arr->IsNull(i)) {
                    values[i] = 0;
                    continue;
                }
                arrow::Decimal128 val(dec_arr->Value(i));
                uint64_t u64_val = val.low_bits();
                values[i] = u64_val;
                if (u64_val < min_val) min_val = u64_val;
                if (u64_val > max_val) max_val = u64_val;
            }

            result.reference_value_ = min_val;
            uint64_t range = max_val - min_val;
            uint8_t bw = get_bit_width(range);

            std::vector<uint64_t> offsets(len);
            for (int64_t i = 0; i < len; ++i) {
                offsets[i] = dec_arr->IsNull(i) ? 0 : values[i] - min_val;
            }

            const uint8_t* null_bitmap = nullptr;
            std::vector<uint8_t> null_bits;
            if (dec_arr->null_count() > 0 && dec_arr->null_bitmap()) {
                size_t bitmap_bytes = (len + 7) / 8;
                null_bits.assign(dec_arr->null_bitmap()->data(),
                                 dec_arr->null_bitmap()->data() + bitmap_bytes);
                null_bitmap = null_bits.data();
            }

            result.bit_packed_ = BitPackedArray(
                offsets.data(), null_bitmap, static_cast<uint32_t>(len), bw);
        } else {
            // DECIMAL256 (fits-u64 path)
            auto dec_arr = std::static_pointer_cast<arrow::Decimal256Array>(array);
            auto dec_type = std::static_pointer_cast<arrow::Decimal256Type>(array->type());
            result.arrow_type_ = 1;  // Decimal256
            result.precision_ = static_cast<uint8_t>(dec_type->precision());
            result.scale_ = static_cast<int8_t>(dec_type->scale());

            if (dec_arr->null_count() == len) {
                result.reference_value_ = 0;
                std::vector<uint64_t> zeros(len, 0);
                const uint8_t* null_bitmap = nullptr;
                std::vector<uint8_t> null_bits;
                if (dec_arr->null_bitmap()) {
                    size_t bitmap_bytes = (len + 7) / 8;
                    null_bits.assign(dec_arr->null_bitmap()->data(),
                                     dec_arr->null_bitmap()->data() + bitmap_bytes);
                    null_bitmap = null_bits.data();
                }
                result.bit_packed_ = BitPackedArray(
                    zeros.data(), null_bitmap, static_cast<uint32_t>(len), 0);
                return result;
            }

            std::vector<uint64_t> values(len);
            uint64_t min_val = UINT64_MAX;
            uint64_t max_val = 0;

            for (int64_t i = 0; i < len; ++i) {
                if (dec_arr->IsNull(i)) {
                    values[i] = 0;
                    continue;
                }
                // Decimal256 stored as 32 bytes LE; low 8 bytes = u64 value
                const uint8_t* raw = dec_arr->Value(i);
                uint64_t u64_val = 0;
                std::memcpy(&u64_val, raw, 8);
                values[i] = u64_val;
                if (u64_val < min_val) min_val = u64_val;
                if (u64_val > max_val) max_val = u64_val;
            }

            result.reference_value_ = min_val;
            uint64_t range = max_val - min_val;
            uint8_t bw = get_bit_width(range);

            std::vector<uint64_t> offsets(len);
            for (int64_t i = 0; i < len; ++i) {
                offsets[i] = dec_arr->IsNull(i) ? 0 : values[i] - min_val;
            }

            const uint8_t* null_bitmap = nullptr;
            std::vector<uint8_t> null_bits;
            if (dec_arr->null_count() > 0 && dec_arr->null_bitmap()) {
                size_t bitmap_bytes = (len + 7) / 8;
                null_bits.assign(dec_arr->null_bitmap()->data(),
                                 dec_arr->null_bitmap()->data() + bitmap_bytes);
                null_bitmap = null_bits.data();
            }

            result.bit_packed_ = BitPackedArray(
                offsets.data(), null_bitmap, static_cast<uint32_t>(len), bw);
        }

        return result;
    }

    /// Decode back to Arrow Decimal128/256Array.
    /// Optimized: bulk unpack + direct buffer construction.
    std::shared_ptr<arrow::Array> to_arrow() const {
        uint32_t len = bit_packed_.length();

        if (arrow_type_ == 0) {
            // Decimal128
            auto dec_type = arrow::decimal128(precision_, scale_);
            if (len == 0) {
                return arrow::MakeEmptyArray(dec_type).ValueOrDie();
            }

            // Allocate Decimal128 buffer (16 bytes per element)
            int64_t buf_size = static_cast<int64_t>(len) * 16;
            auto value_buf = arrow::AllocateBuffer(buf_size).ValueOrDie();
            auto* out = value_buf->mutable_data();

            // Bulk unpack u64 offsets
            std::vector<uint64_t> temp(len);
            bit_packed_.bulk_unpack_to(temp.data());

            // Convert u64 -> Decimal128 (little-endian: low=value, high=0)
            for (uint32_t i = 0; i < len; ++i) {
                uint64_t val = temp[i] + reference_value_;
                std::memcpy(out + i * 16, &val, 8);      // low 8 bytes
                std::memset(out + i * 16 + 8, 0, 8);     // high 8 bytes = 0
            }

            auto null_buf = bit_packed_.null_bitmap_arrow_buffer();
            int64_t nc = bit_packed_.null_count();
            auto data = arrow::ArrayData::Make(
                dec_type, static_cast<int64_t>(len),
                {std::move(null_buf), std::move(value_buf)},
                nc);
            return arrow::MakeArray(data);
        } else {
            // Decimal256
            auto dec_type = arrow::decimal256(precision_, scale_);
            if (len == 0) {
                return arrow::MakeEmptyArray(dec_type).ValueOrDie();
            }

            // Allocate Decimal256 buffer (32 bytes per element)
            int64_t buf_size = static_cast<int64_t>(len) * 32;
            auto value_buf = arrow::AllocateBuffer(buf_size).ValueOrDie();
            auto* out = value_buf->mutable_data();

            // Bulk unpack u64 offsets
            std::vector<uint64_t> temp(len);
            bit_packed_.bulk_unpack_to(temp.data());

            // Convert u64 -> Decimal256 (little-endian: low=value, rest=0)
            for (uint32_t i = 0; i < len; ++i) {
                uint64_t val = temp[i] + reference_value_;
                std::memcpy(out + i * 32, &val, 8);      // low 8 bytes
                std::memset(out + i * 32 + 8, 0, 24);    // high 24 bytes = 0
            }

            auto null_buf = bit_packed_.null_bitmap_arrow_buffer();
            int64_t nc = bit_packed_.null_count();
            auto data = arrow::ArrayData::Make(
                dec_type, static_cast<int64_t>(len),
                {std::move(null_buf), std::move(value_buf)},
                nc);
            return arrow::MakeArray(data);
        }
    }

    /// Serialize to bytes (binary-compatible with Rust).
    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> out;
        out.reserve(bit_pack_starting_loc() + 256);

        // 1. IPC header (16 bytes)
        LiquidIPCHeader ipc(LiquidDataType::Decimal, PhysicalType::UInt64);
        ipc.serialize(out);

        // 2. DecimalArrayHeader (8 bytes)
        DecimalArrayHeader dah;
        dah.arrow_type = arrow_type_;
        dah.precision = precision_;
        dah.scale = scale_;
        dah.padding = 0;
        dah.reserved = 0;
        dah.serialize(out);

        // 3. reference_value (8 bytes LE)
        const uint8_t* ref_ptr =
            reinterpret_cast<const uint8_t*>(&reference_value_);
        out.insert(out.end(), ref_ptr, ref_ptr + 8);

        // 4. Pad to bit_pack_starting_loc (should already be 32)
        while (out.size() < bit_pack_starting_loc()) {
            out.push_back(0);
        }

        // 5. BitPackedArray data
        bit_packed_.serialize(out);

        return out;
    }

    /// Deserialize from bytes.
    static LiquidDecimalArray from_bytes(const uint8_t* data, size_t len) {
        if (len < bit_pack_starting_loc()) {
            throw std::runtime_error("Buffer too small for LiquidDecimalArray");
        }

        // 1. Verify IPC header
        auto ipc = LiquidIPCHeader::deserialize(data, len);
        if (ipc.logical_type_id != static_cast<uint16_t>(LiquidDataType::Decimal)) {
            throw std::runtime_error("Expected Decimal logical type");
        }

        // 2. Read DecimalArrayHeader
        auto dah = DecimalArrayHeader::deserialize(
            data + LiquidIPCHeader::SIZE, len - LiquidIPCHeader::SIZE);

        LiquidDecimalArray result;
        result.arrow_type_ = dah.arrow_type;
        result.precision_ = dah.precision;
        result.scale_ = dah.scale;

        // 3. Read reference_value
        size_t ref_start = LiquidIPCHeader::SIZE + DecimalArrayHeader::SIZE;
        std::memcpy(&result.reference_value_, data + ref_start, 8);

        // 4. Read BitPackedArray from offset 32
        size_t bp_start = bit_pack_starting_loc();
        if (bp_start > len) {
            throw std::runtime_error("Buffer too small for BitPackedArray");
        }
        result.bit_packed_ = BitPackedArray::deserialize(
            data + bp_start, len - bp_start);

        return result;
    }

    uint32_t length() const { return bit_packed_.length(); }
    size_t memory_size() const {
        return bit_packed_.memory_size() + sizeof(uint64_t) + sizeof(*this);
    }

#ifdef LIQUID_ENABLE_VELOX
    /// Decode directly to Velox FlatVector<int128_t> (LongDecimal).
    facebook::velox::VectorPtr to_velox(
        facebook::velox::memory::MemoryPool* pool) const;
#endif

private:
    static constexpr size_t bit_pack_starting_loc() {
        // Match Rust: (header_size + sizeof(u64) + 7) & !7
        constexpr size_t header_size =
            LiquidIPCHeader::SIZE + DecimalArrayHeader::SIZE;
        return (header_size + sizeof(uint64_t) + 7) & ~static_cast<size_t>(7);
    }

    uint8_t arrow_type_ = 0;   // 0 = Decimal128
    uint8_t precision_ = 0;
    int8_t  scale_ = 0;
    uint64_t reference_value_ = 0;
    BitPackedArray bit_packed_;
};

}  // namespace liquid_cache
