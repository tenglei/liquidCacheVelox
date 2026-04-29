// liquid_cache/liquid_fixed_len_byte_array.h
// LiquidFixedLenByteArray - Dictionary + FSST compression for Decimal128/256
// values that don't fit in uint64_t.
// Binary-compatible with the Rust LiquidFixedLenByteArray serialization format.
//
// Encoding: dictionary-deduplicate decimal values → UInt16 keys (BitPackedArray)
//           + FSST-compress the unique decimal byte representations.
//
// Serialization layout:
//   [LiquidIPCHeader: 16B]  (logical=FixedLenByteArray, physical=UInt16)
//   [FixedLenByteArrayHeader: 12B]
//     - key_size: u32   (bytes of serialized BitPackedArray)
//     - value_size: u32 (bytes of serialized FSST values section)
//     - arrow_type: u8  (0=Decimal128, 1=Decimal256)
//     - precision: u8
//     - scale: i8
//     - padding: u8
//   [BitPackedArray data for keys]
//   [padding to 8-byte alignment]
//   [FSST values section:
//     [symbol_table]
//     [uncompressed_bytes: u64]
//     [compressed_size: u32]
//     [compressed_data]
//     [CompactOffsets data]
//   ]
#pragma once

#include <arrow/util/logging.h>
#include <arrow/api.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "liquid_cache/bit_packed_array.h"
#include "liquid_cache/fsst.h"
#include "liquid_cache/ipc_header.h"
#include "liquid_cache/liquid_arrays.h"  // for get_bit_width()

namespace liquid_cache {

// ═══════════════════════════════════════════════════════════════════════
// FixedLenByteArrayHeader: 12 bytes - metadata for fixed-len encoding
// Binary-compatible with Rust's FixedLenByteArrayHeader
// ═══════════════════════════════════════════════════════════════════════
struct FixedLenByteArrayHeader {
    uint32_t key_size;     // size of serialized BitPackedArray (keys)
    uint32_t value_size;   // size of serialized FSST values section
    uint8_t  arrow_type;   // 0 = Decimal128, 1 = Decimal256
    uint8_t  precision;
    int8_t   scale;
    uint8_t  padding;

    static constexpr size_t SIZE = 12;

    void serialize(std::vector<uint8_t>& out) const {
        auto write_u32 = [&](uint32_t v) {
            auto* p = reinterpret_cast<const uint8_t*>(&v);
            out.insert(out.end(), p, p + 4);
        };
        write_u32(key_size);
        write_u32(value_size);
        out.push_back(arrow_type);
        out.push_back(precision);
        out.push_back(static_cast<uint8_t>(scale));
        out.push_back(padding);
    }

    static FixedLenByteArrayHeader deserialize(const uint8_t* data, size_t len) {
        if (len < SIZE) {
            throw std::runtime_error(
                "Buffer too small for FixedLenByteArrayHeader");
        }
        FixedLenByteArrayHeader h;
        std::memcpy(&h.key_size, data, 4);
        std::memcpy(&h.value_size, data + 4, 4);
        h.arrow_type = data[8];
        h.precision = data[9];
        h.scale = static_cast<int8_t>(data[10]);
        h.padding = 0;
        return h;
    }
};
static_assert(sizeof(FixedLenByteArrayHeader) == FixedLenByteArrayHeader::SIZE,
              "FixedLenByteArrayHeader must be exactly 12 bytes");

// ═══════════════════════════════════════════════════════════════════════
// ArrowFixedLenByteArrayType: tracks the original Arrow decimal type
// ═══════════════════════════════════════════════════════════════════════
enum class ArrowFixedLenByteArrayType : uint8_t {
    Decimal128 = 0,
    Decimal256 = 1,
};

/// Get the value width in bytes for a fixed-len byte array type.
inline size_t fixed_len_value_width(ArrowFixedLenByteArrayType t) {
    switch (t) {
        case ArrowFixedLenByteArrayType::Decimal128: return 16;
        case ArrowFixedLenByteArrayType::Decimal256: return 32;
    }
    return 16;  // unreachable
}

// ═══════════════════════════════════════════════════════════════════════
// LiquidFixedLenByteArray
// ═══════════════════════════════════════════════════════════════════════
class LiquidFixedLenByteArray {
public:
    LiquidFixedLenByteArray() = default;

    // --- Encoding from Arrow ---

    /// Encode a Decimal128Array that doesn't fit in u64 (dictionary + FSST).
    static LiquidFixedLenByteArray from_decimal128(
            const std::shared_ptr<arrow::Array>& array) {
        auto dec_arr = std::static_pointer_cast<arrow::Decimal128Array>(array);
        auto dec_type = std::static_pointer_cast<arrow::Decimal128Type>(array->type());

        LiquidFixedLenByteArray result;
        result.arrow_type_ = ArrowFixedLenByteArrayType::Decimal128;
        result.precision_ = static_cast<uint8_t>(dec_type->precision());
        result.scale_ = static_cast<int8_t>(dec_type->scale());

        build_dictionary_and_compress(dec_arr, 16, result);
        return result;
    }

    /// Encode a Decimal256Array (dictionary + FSST).
    static LiquidFixedLenByteArray from_decimal256(
            const std::shared_ptr<arrow::Array>& array) {
        auto dec_arr = std::static_pointer_cast<arrow::Decimal256Array>(array);
        auto dec_type = std::static_pointer_cast<arrow::Decimal256Type>(array->type());

        LiquidFixedLenByteArray result;
        result.arrow_type_ = ArrowFixedLenByteArrayType::Decimal256;
        result.precision_ = static_cast<uint8_t>(dec_type->precision());
        result.scale_ = static_cast<int8_t>(dec_type->scale());

        build_dictionary_and_compress(dec_arr, 32, result);
        return result;
    }

    // --- Decoding to Arrow ---

    /// Decode back to an Arrow Decimal128Array or Decimal256Array.
    std::shared_ptr<arrow::Array> to_arrow() const {
        uint32_t len = keys_.length();
        size_t value_width = fixed_len_value_width(arrow_type_);

        // Get cached decompressed dictionary values
        const auto& dict = ensure_dict_cached();
        size_t dict_size = dict.size();

        // Bulk unpack all keys
        std::vector<uint16_t> key_vals(len);
        keys_.bulk_unpack_to(key_vals.data());

        // Allocate output buffer: len * value_width bytes
        int64_t buf_size = static_cast<int64_t>(len) * value_width;
        auto value_buf = arrow::AllocateBuffer(buf_size).ValueOrDie();
        auto* out = value_buf->mutable_data();

        // Reconstruct values from dictionary
        for (uint32_t i = 0; i < len; ++i) {
            if (!keys_.is_null(i) && key_vals[i] < dict_size) {
                std::memcpy(out + i * value_width,
                            dict[key_vals[i]].data(), value_width);
            } else {
                std::memset(out + i * value_width, 0, value_width);
            }
        }

        // Build Arrow array
        auto null_buf = keys_.null_bitmap_arrow_buffer();
        int64_t nc = keys_.null_count();

        std::shared_ptr<arrow::DataType> arr_type;
        if (arrow_type_ == ArrowFixedLenByteArrayType::Decimal128) {
            arr_type = arrow::decimal128(precision_, scale_);
        } else {
            arr_type = arrow::decimal256(precision_, scale_);
        }

        auto data = arrow::ArrayData::Make(
            arr_type, static_cast<int64_t>(len),
            {std::move(null_buf), std::move(value_buf)}, nc);
        return arrow::MakeArray(data);
    }

    // --- Serialization ---

    /// Serialize to bytes (binary-compatible with Rust).
    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> out;
        out.reserve(256);

        // 1. IPC header (16 bytes)
        LiquidIPCHeader ipc(LiquidDataType::FixedLenByteArray,
                            PhysicalType::UInt16);
        ipc.serialize(out);

        // 2. FixedLenByteArrayHeader (12 bytes) - will be filled after we know sizes
        size_t header_pos = out.size();
        FixedLenByteArrayHeader flbah;
        flbah.arrow_type = static_cast<uint8_t>(arrow_type_);
        flbah.precision = precision_;
        flbah.scale = scale_;
        flbah.padding = 0;
        flbah.key_size = 0;    // placeholder
        flbah.value_size = 0;  // placeholder
        flbah.serialize(out);

        // 3. Serialize BitPackedArray (keys)
        size_t keys_start = out.size();
        keys_.serialize(out);
        uint32_t keys_size = static_cast<uint32_t>(out.size() - keys_start);

        // 4. Pad to 8-byte alignment
        while (out.size() % 8 != 0) out.push_back(0);

        // 5. Serialize FSST values section
        size_t values_start = out.size();
        serialize_fsst_values(out);
        uint32_t values_size = static_cast<uint32_t>(out.size() - values_start);

        // 6. Go back and fill in the header
        flbah.key_size = keys_size;
        flbah.value_size = values_size;
        // Rewrite the header at header_pos
        std::vector<uint8_t> header_bytes;
        flbah.serialize(header_bytes);
        std::memcpy(out.data() + header_pos, header_bytes.data(),
                    FixedLenByteArrayHeader::SIZE);

        return out;
    }

    /// Deserialize from bytes.
    static LiquidFixedLenByteArray from_bytes(const uint8_t* data, size_t len) {
        auto ipc = LiquidIPCHeader::deserialize(data, len);
        if (ipc.logical_type_id !=
            static_cast<uint16_t>(LiquidDataType::FixedLenByteArray)) {
            throw std::runtime_error("Expected FixedLenByteArray logical type");
        }

        size_t pos = LiquidIPCHeader::SIZE;
        if (pos + FixedLenByteArrayHeader::SIZE > len) {
            throw std::runtime_error(
                "Buffer too small for FixedLenByteArrayHeader");
        }
        auto flbah = FixedLenByteArrayHeader::deserialize(data + pos, len - pos);
        pos += FixedLenByteArrayHeader::SIZE;

        LiquidFixedLenByteArray result;
        result.arrow_type_ = static_cast<ArrowFixedLenByteArrayType>(
            flbah.arrow_type);
        result.precision_ = flbah.precision;
        result.scale_ = flbah.scale;

        // Read keys
        size_t keys_end = pos + flbah.key_size;
        if (keys_end > len) {
            throw std::runtime_error("Keys data extends beyond buffer");
        }
        result.keys_ = BitPackedArray::deserialize(data + pos, flbah.key_size);
        pos = keys_end;

        // Align to 8 bytes
        pos = (pos + 7) & ~static_cast<size_t>(7);

        // Read FSST values section
        size_t values_end = pos + flbah.value_size;
        if (values_end > len) {
            throw std::runtime_error("Values data extends beyond buffer");
        }
        result.deserialize_fsst_values(data + pos, flbah.value_size);

        return result;
    }

    uint32_t length() const { return keys_.length(); }

    size_t memory_size() const {
        return keys_.memory_size() + compressed_data_.size() +
               compact_offsets_bytes_ + sizeof(*this);
    }

    /// Check if a Decimal128Array's values do NOT fit in uint64.
    static bool needs_fixed_len(const std::shared_ptr<arrow::Array>& array) {
        return !LiquidDecimalArray::fits_u64(array);
    }

#ifdef LIQUID_ENABLE_VELOX
    /// Decode directly to Velox ShortDecimal or LongDecimal FlatVector.
    facebook::velox::VectorPtr to_velox(
        facebook::velox::memory::MemoryPool* pool) const;
#endif

private:
    // --- Type metadata ---
    ArrowFixedLenByteArrayType arrow_type_ = ArrowFixedLenByteArrayType::Decimal128;
    uint8_t precision_ = 0;
    int8_t  scale_ = 0;

    // --- Dictionary keys (BitPackedArray of u16) ---
    BitPackedArray keys_;

    // --- FSST-compressed dictionary values ---
    FsstCompressor compressor_;
    uint64_t uncompressed_bytes_ = 0;
    std::vector<uint8_t> compressed_data_;
    // Offsets into compressed_data_ for each dictionary value.
    // Stored as CompactOffsets (linear-regression compressed),
    // but we also keep the raw offsets for fast access.
    std::vector<uint32_t> value_offsets_;  // N+1 entries for N dict values

    // Serialized form of CompactOffsets (for binary compatibility with Rust).
    // We store this separately because Rust serializes offsets as CompactOffsets.
    size_t compact_offsets_bytes_ = 0;
    std::vector<uint8_t> compact_offsets_serialized_;

    // --- Cached decompressed dictionary (lazy) ---
    mutable std::shared_ptr<std::vector<std::vector<uint8_t>>> cached_dict_;

    // --- Internal helpers ---

    /// Build dictionary (deduplicate) and FSST-compress decimal values.
    template <typename DecimalArrayType>
    static void build_dictionary_and_compress(
            const std::shared_ptr<DecimalArrayType>& dec_arr,
            size_t value_width,
            LiquidFixedLenByteArray& result) {
        int64_t len = dec_arr->length();

        // Step 1: Build dictionary (deduplicate values by their byte representation)
        // Use a map from byte-string → uint16_t index
        struct BytesHash {
            size_t operator()(const std::vector<uint8_t>& v) const {
                // Simple FNV-1a hash
                size_t h = 2166136261UL;
                for (auto b : v) {
                    h ^= b;
                    h *= 16777619UL;
                }
                return h;
            }
        };
        std::unordered_map<std::vector<uint8_t>, uint16_t, BytesHash> dict_map;
        std::vector<std::vector<uint8_t>> dict_values;
        std::vector<uint16_t> key_list(len);

        for (int64_t i = 0; i < len; ++i) {
            if (dec_arr->IsNull(i)) {
                key_list[i] = 0;
                continue;
            }

            // Get raw bytes for this decimal value
            std::vector<uint8_t> val_bytes(value_width);
            if constexpr (std::is_same_v<DecimalArrayType, arrow::Decimal128Array>) {
                auto view = dec_arr->Value(i);
                // Decimal128 is stored as 16 bytes little-endian
                std::memcpy(val_bytes.data(), view, 16);
            } else {
                auto view = dec_arr->Value(i);
                // Decimal256 is stored as 32 bytes little-endian
                std::memcpy(val_bytes.data(), view, 32);
            }

            auto it = dict_map.find(val_bytes);
            if (it != dict_map.end()) {
                key_list[i] = it->second;
            } else {
                uint16_t idx = static_cast<uint16_t>(dict_values.size());
                dict_map[val_bytes] = idx;
                dict_values.push_back(std::move(val_bytes));
                key_list[i] = idx;
            }
        }

        // Step 2: Concatenate all unique values for FSST training
        std::vector<uint8_t> all_bytes;
        for (const auto& dv : dict_values) {
            all_bytes.insert(all_bytes.end(), dv.begin(), dv.end());
        }

        // Step 3: Train FSST compressor on the concatenated bytes
        result.compressor_.train(all_bytes.data(), all_bytes.size());

        // Step 4: Compress each unique value individually, track offsets
        result.value_offsets_.push_back(0);
        std::vector<uint8_t> compressed_buf;
        for (const auto& dv : dict_values) {
            auto compressed = result.compressor_.compress(dv.data(), dv.size());
            compressed_buf.insert(compressed_buf.end(),
                                  compressed.begin(), compressed.end());
            result.value_offsets_.push_back(
                static_cast<uint32_t>(compressed_buf.size()));
        }
        result.uncompressed_bytes_ = all_bytes.size();
        result.compressed_data_ = std::move(compressed_buf);

        // Step 5: Build CompactOffsets from value_offsets_ (for serialization)
        CompactOffsets co(result.value_offsets_);
        result.compact_offsets_serialized_ = co.to_bytes();
        result.compact_offsets_bytes_ = result.compact_offsets_serialized_.size();

        // Step 6: Pack dictionary keys into BitPackedArray
        uint16_t max_key = dict_values.empty() ? 0
            : static_cast<uint16_t>(dict_values.size() - 1);
        uint8_t bw = get_bit_width(static_cast<uint64_t>(max_key));

        std::vector<uint64_t> key_offsets(len);
        for (int64_t i = 0; i < len; ++i) {
            key_offsets[i] = static_cast<uint64_t>(key_list[i]);
        }

        // Build null bitmap
        const uint8_t* null_bitmap = nullptr;
        std::vector<uint8_t> null_bits;
        if (dec_arr->null_count() > 0 && dec_arr->null_bitmap()) {
            size_t bitmap_bytes = (len + 7) / 8;
            null_bits.assign(dec_arr->null_bitmap()->data(),
                             dec_arr->null_bitmap()->data() + bitmap_bytes);
            null_bitmap = null_bits.data();
        }

        result.keys_ = BitPackedArray(
            key_offsets.data(), null_bitmap, static_cast<uint32_t>(len), bw);
    }

    /// Ensure decompressed dictionary is cached.
    const std::vector<std::vector<uint8_t>>& ensure_dict_cached() const {
        if (cached_dict_) return *cached_dict_;

        auto dict = std::make_shared<std::vector<std::vector<uint8_t>>>();
        size_t dict_size = value_offsets_.size() > 0
                           ? value_offsets_.size() - 1 : 0;
        dict->resize(dict_size);

        for (size_t d = 0; d < dict_size; ++d) {
            uint32_t start = value_offsets_[d];
            uint32_t end = value_offsets_[d + 1];
            auto decompressed = FsstCompressor::decompress(
                compressor_.symbol_count() > 0 ? &compressor_.symbol(0) : nullptr,
                compressor_.symbol_count(),
                compressed_data_.data() + start, end - start);
            (*dict)[d] = std::move(decompressed);
        }

        cached_dict_ = std::move(dict);
        return *cached_dict_;
    }

    /// Serialize the FSST values section.
    void serialize_fsst_values(std::vector<uint8_t>& out) const {
        // Format: [symbol_table] [uncompressed_bytes(u64)] [compressed_size(u32)]
        //         [compressed_data] [compact_offsets_data]
        compressor_.save_symbol_table(out);

        // uncompressed_bytes (u64 LE)
        uint64_t ub = uncompressed_bytes_;
        auto* ubp = reinterpret_cast<const uint8_t*>(&ub);
        out.insert(out.end(), ubp, ubp + 8);

        // compressed_size (u32 LE)
        uint32_t cs = static_cast<uint32_t>(compressed_data_.size());
        auto* csp = reinterpret_cast<const uint8_t*>(&cs);
        out.insert(out.end(), csp, csp + 4);

        // compressed data
        out.insert(out.end(), compressed_data_.begin(), compressed_data_.end());

        // Compact offsets
        out.insert(out.end(),
                   compact_offsets_serialized_.begin(),
                   compact_offsets_serialized_.end());
    }

    /// Deserialize the FSST values section.
    void deserialize_fsst_values(const uint8_t* data, size_t len) {
        size_t pos = 0;

        // Parse symbol table
        compressor_ = FsstCompressor::load_symbol_table(data, len);
        size_t st_size = compressor_.symbol_table_size();
        pos += st_size;

        // Read uncompressed_bytes (u64)
        if (pos + 12 > len) {
            throw std::runtime_error(
                "Buffer too small for FSST values header");
        }
        std::memcpy(&uncompressed_bytes_, data + pos, 8);
        pos += 8;

        // Read compressed_size (u32)
        uint32_t comp_size;
        std::memcpy(&comp_size, data + pos, 4);
        pos += 4;

        // Read compressed data
        if (pos + comp_size > len) {
            throw std::runtime_error("FSST compressed data overflow");
        }
        compressed_data_.assign(data + pos, data + pos + comp_size);
        pos += comp_size;

        // Remaining bytes are CompactOffsets
        if (pos < len) {
            compact_offsets_serialized_.assign(data + pos, data + len);
            compact_offsets_bytes_ = compact_offsets_serialized_.size();

            // Parse CompactOffsets to rebuild value_offsets_
            auto co = CompactOffsets::from_bytes(
                compact_offsets_serialized_.data(),
                compact_offsets_serialized_.size());
            value_offsets_.resize(co.len());
            for (size_t i = 0; i < co.len(); ++i) {
                value_offsets_[i] = co.get(i);
            }
        }
    }
};

}  // namespace liquid_cache
