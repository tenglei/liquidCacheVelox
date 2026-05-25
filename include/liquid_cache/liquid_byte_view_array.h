// liquid_cache/liquid_byte_view_array.h
// LiquidByteViewArray - Dictionary + FSST compression for string/binary types.
// Binary-compatible with the Rust LiquidByteViewArray serialization format.
#pragma once

#include <arrow/util/logging.h>
#include <arrow/api.h>
#include <arrow/compute/api.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

#include "liquid_cache/bit_packed_array.h"
#include "liquid_cache/fsst.h"
#include "liquid_cache/ipc_header.h"
#include "liquid_cache/liquid_arrays.h"  // for get_bit_width()

namespace liquid_cache {

// ═══════════════════════════════════════════════════════════════════════
// PrefixKey: 8 bytes per dictionary entry (7-byte prefix + 1-byte length)
// ═══════════════════════════════════════════════════════════════════════
struct PrefixKey {
    uint8_t prefix7[7] = {};
    uint8_t len_byte = 0;

    PrefixKey() = default;

    /// Create from suffix bytes (after shared prefix removal).
    explicit PrefixKey(const uint8_t* suffix, size_t suffix_len) {
        size_t copy_len = std::min<size_t>(7, suffix_len);
        std::memcpy(prefix7, suffix, copy_len);
        len_byte = suffix_len >= 255 ? 255 : static_cast<uint8_t>(suffix_len);
    }
};
static_assert(sizeof(PrefixKey) == 8, "PrefixKey must be exactly 8 bytes");

// ═══════════════════════════════════════════════════════════════════════
// CompactOffsets: Linear-regression compressed byte offsets
// ═══════════════════════════════════════════════════════════════════════
class CompactOffsets {
public:
    CompactOffsets() = default;

    /// Build from raw uint32 offsets (N+1 entries for N dictionary values).
    explicit CompactOffsets(const std::vector<uint32_t>& offsets) {
        size_t n = offsets.size();
        if (n == 0) return;

        // Linear regression: fit line through (i, offset[i])
        double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
        for (size_t i = 0; i < n; ++i) {
            double x = static_cast<double>(i);
            double y = static_cast<double>(offsets[i]);
            sum_x += x;
            sum_y += y;
            sum_xy += x * y;
            sum_xx += x * x;
        }
        double dn = static_cast<double>(n);
        double denom = dn * sum_xx - sum_x * sum_x;
        if (std::abs(denom) < 1e-10) {
            slope_ = 0;
            intercept_ = n > 0 ? static_cast<int32_t>(offsets[0]) : 0;
        } else {
            slope_ = static_cast<int32_t>((dn * sum_xy - sum_x * sum_y) / denom);
            intercept_ = static_cast<int32_t>(
                (sum_y - slope_ * sum_x) / dn);
        }

        // Compute residuals
        residuals_.resize(n);
        int32_t min_res = 0, max_res = 0;
        for (size_t i = 0; i < n; ++i) {
            int32_t predicted = slope_ * static_cast<int32_t>(i) + intercept_;
            residuals_[i] = static_cast<int32_t>(offsets[i]) - predicted;
            if (i == 0 || residuals_[i] < min_res) min_res = residuals_[i];
            if (i == 0 || residuals_[i] > max_res) max_res = residuals_[i];
        }

        // Determine smallest byte width for residuals
        if (min_res >= -128 && max_res <= 127) {
            offset_bytes_ = 1;
        } else if (min_res >= -32768 && max_res <= 32767) {
            offset_bytes_ = 2;
        } else {
            offset_bytes_ = 4;
        }
    }

    size_t len() const { return residuals_.size(); }

    uint32_t get(size_t i) const {
        int32_t predicted = slope_ * static_cast<int32_t>(i) + intercept_;
        return static_cast<uint32_t>(predicted + residuals_[i]);
    }

    /// Serialize to bytes.
    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> out;
        // Header: slope(4) + intercept(4) + offset_bytes(1) = 9 bytes
        auto* sp = reinterpret_cast<const uint8_t*>(&slope_);
        out.insert(out.end(), sp, sp + 4);
        auto* ip = reinterpret_cast<const uint8_t*>(&intercept_);
        out.insert(out.end(), ip, ip + 4);
        out.push_back(offset_bytes_);

        // Residuals
        for (auto r : residuals_) {
            if (offset_bytes_ == 1) {
                out.push_back(static_cast<uint8_t>(static_cast<int8_t>(r)));
            } else if (offset_bytes_ == 2) {
                int16_t r16 = static_cast<int16_t>(r);
                auto* rp = reinterpret_cast<const uint8_t*>(&r16);
                out.insert(out.end(), rp, rp + 2);
            } else {
                auto* rp = reinterpret_cast<const uint8_t*>(&r);
                out.insert(out.end(), rp, rp + 4);
            }
        }
        return out;
    }

    /// Deserialize from bytes.
    static CompactOffsets from_bytes(const uint8_t* data, size_t len) {
        if (len < 9) throw std::runtime_error("CompactOffsets: buffer too small");
        CompactOffsets co;
        std::memcpy(&co.slope_, data, 4);
        std::memcpy(&co.intercept_, data + 4, 4);
        co.offset_bytes_ = data[8];

        size_t residual_bytes = len - 9;
        size_t count = 0;
        if (co.offset_bytes_ == 1) count = residual_bytes;
        else if (co.offset_bytes_ == 2) count = residual_bytes / 2;
        else count = residual_bytes / 4;

        co.residuals_.resize(count);
        const uint8_t* rp = data + 9;
        for (size_t i = 0; i < count; ++i) {
            if (co.offset_bytes_ == 1) {
                co.residuals_[i] = static_cast<int8_t>(rp[i]);
            } else if (co.offset_bytes_ == 2) {
                int16_t v;
                std::memcpy(&v, rp + i * 2, 2);
                co.residuals_[i] = v;
            } else {
                std::memcpy(&co.residuals_[i], rp + i * 4, 4);
            }
        }
        return co;
    }

private:
    int32_t slope_ = 0;
    int32_t intercept_ = 0;
    uint8_t offset_bytes_ = 1;
    std::vector<int32_t> residuals_;
};

// ═══════════════════════════════════════════════════════════════════════
// ByteViewArrayHeader: 20-byte header for serialized ByteViewArray
// ═══════════════════════════════════════════════════════════════════════
struct ByteViewArrayHeader {
    uint32_t keys_size;
    uint32_t compact_offsets_size;
    uint32_t shared_prefix_size;
    uint32_t fsst_raw_size;
    uint32_t fingerprint_size;
    uint8_t reserved[3] = {};
    uint8_t flags = 0;  // bit 0 = is_ascii

    static constexpr size_t SIZE = 24;  // 5x u32 + 3 reserved + 1 flags

    void serialize(std::vector<uint8_t>& out) const {
        auto write_u32 = [&](uint32_t v) {
            auto* p = reinterpret_cast<const uint8_t*>(&v);
            out.insert(out.end(), p, p + 4);
        };
        write_u32(keys_size);
        write_u32(compact_offsets_size);
        write_u32(shared_prefix_size);
        write_u32(fsst_raw_size);
        write_u32(fingerprint_size);
        out.insert(out.end(), reserved, reserved + 3);
        out.push_back(flags);
    }

    static ByteViewArrayHeader deserialize(const uint8_t* data) {
        ByteViewArrayHeader h;
        std::memcpy(&h.keys_size, data, 4);
        std::memcpy(&h.compact_offsets_size, data + 4, 4);
        std::memcpy(&h.shared_prefix_size, data + 8, 4);
        std::memcpy(&h.fsst_raw_size, data + 12, 4);
        std::memcpy(&h.fingerprint_size, data + 16, 4);
        std::memcpy(h.reserved, data + 20, 3);
        h.flags = data[23];
        return h;
    }
};

// ═══════════════════════════════════════════════════════════════════════
// LiquidByteViewArray
// ═══════════════════════════════════════════════════════════════════════
class LiquidByteViewArray {
public:
    LiquidByteViewArray() = default;

    /// Encode an Arrow String/Binary Array into Liquid ByteView format.
    static LiquidByteViewArray from_arrow(
            const std::shared_ptr<arrow::Array>& array) {
        LiquidByteViewArray result;
        result.is_binary_ = (array->type_id() == arrow::Type::BINARY ||
                             array->type_id() == arrow::Type::LARGE_BINARY);
        if (!result.is_binary_) {
            result.is_ascii_ = check_all_ascii(array);
        }
        const int64_t len = array->length();

        // Step 1: Build dictionary (deduplicate strings)
        std::unordered_map<std::string, uint16_t> dict_map;
        std::vector<std::string> dict_values;
        std::vector<uint16_t> keys(len);

        auto get_value = [&](int64_t i, const uint8_t*& data, uint32_t& length) {
            if (array->type_id() == arrow::Type::STRING) {
                auto sa = std::static_pointer_cast<arrow::StringArray>(array);
                int32_t vlen;
                data = sa->GetValue(i, &vlen);
                length = static_cast<uint32_t>(vlen);
            } else if (array->type_id() == arrow::Type::LARGE_STRING) {
                auto sa = std::static_pointer_cast<arrow::LargeStringArray>(array);
                int64_t vlen;
                data = sa->GetValue(i, &vlen);
                length = static_cast<uint32_t>(vlen);
            } else if (array->type_id() == arrow::Type::BINARY) {
                auto ba = std::static_pointer_cast<arrow::BinaryArray>(array);
                int32_t vlen;
                data = ba->GetValue(i, &vlen);
                length = static_cast<uint32_t>(vlen);
            } else {
                auto ba = std::static_pointer_cast<arrow::LargeBinaryArray>(array);
                int64_t vlen;
                data = ba->GetValue(i, &vlen);
                length = static_cast<uint32_t>(vlen);
            }
        };

        for (int64_t i = 0; i < len; ++i) {
            if (array->IsNull(i)) {
                keys[i] = 0;
                continue;
            }
            const uint8_t* data;
            uint32_t vlen;
            get_value(i, data, vlen);
            std::string val(reinterpret_cast<const char*>(data), vlen);

            auto it = dict_map.find(val);
            if (it != dict_map.end()) {
                keys[i] = it->second;
            } else {
                uint16_t idx = static_cast<uint16_t>(dict_values.size());
                dict_map[val] = idx;
                dict_values.push_back(std::move(val));
                keys[i] = idx;
            }
        }

        // Step 2: Shared prefix → FSST → PrefixKeys → BitPackedArray
        const uint8_t* null_bitmap = nullptr;
        std::vector<uint8_t> null_bits;
        if (array->null_count() > 0 && array->null_bitmap()) {
            size_t bitmap_bytes = (len + 7) / 8;
            null_bits.assign(array->null_bitmap()->data(),
                             array->null_bitmap()->data() + bitmap_bytes);
            null_bitmap = null_bits.data();
        }
        build_encoded_dict(result, dict_values, keys.data(), null_bitmap, len);
        return result;
    }

    /// Encode directly from an Arrow DictionaryArray with UInt16 keys.
    /// Reuses Arrow's existing dictionary values and keys unchanged,
    /// avoiding the round-trip through flat StringArray + hash map rebuild.
    /// This is the fast path for Parquet dictionary-encoded string columns.
    static LiquidByteViewArray from_dict_array(
            const std::shared_ptr<arrow::Array>& array) {
        auto dict_array = std::static_pointer_cast<arrow::DictionaryArray>(array);
        LiquidByteViewArray result;
        auto dict_type = dict_array->dict_type();
        auto value_type_id = dict_type->value_type()->id();
        result.is_binary_ = (value_type_id == arrow::Type::BINARY ||
                             value_type_id == arrow::Type::LARGE_BINARY);
        const int64_t len = dict_array->length();

        // Step 1: Extract dictionary values (already unique, no hash map needed)
        auto values_array = dict_array->dictionary();
        if (!result.is_binary_) {
            result.is_ascii_ = check_all_ascii(values_array);
        }
        int64_t dict_size = values_array->length();
        std::vector<std::string> dict_values(dict_size);

        auto get_dict_value = [&](int64_t i, const uint8_t*& data, uint32_t& length) {
            if (value_type_id == arrow::Type::STRING) {
                auto sa = std::static_pointer_cast<arrow::StringArray>(values_array);
                int32_t vlen;
                data = sa->GetValue(i, &vlen);
                length = static_cast<uint32_t>(vlen);
            } else if (value_type_id == arrow::Type::LARGE_STRING) {
                auto sa = std::static_pointer_cast<arrow::LargeStringArray>(values_array);
                int64_t vlen;
                data = sa->GetValue(i, &vlen);
                length = static_cast<uint32_t>(vlen);
            } else if (value_type_id == arrow::Type::BINARY) {
                auto ba = std::static_pointer_cast<arrow::BinaryArray>(values_array);
                int32_t vlen;
                data = ba->GetValue(i, &vlen);
                length = static_cast<uint32_t>(vlen);
            } else {
                auto ba = std::static_pointer_cast<arrow::LargeBinaryArray>(values_array);
                int64_t vlen;
                data = ba->GetValue(i, &vlen);
                length = static_cast<uint32_t>(vlen);
            }
        };

        for (int64_t d = 0; d < dict_size; ++d) {
            if (values_array->IsNull(d)) {
                dict_values[d].clear();
                continue;
            }
            const uint8_t* data;
            uint32_t vlen;
            get_dict_value(d, data, vlen);
            dict_values[d].assign(reinterpret_cast<const char*>(data), vlen);
        }

        // Step 2: Extract keys from Arrow indices (already uint16)
        auto indices_array = std::static_pointer_cast<arrow::UInt16Array>(
            dict_array->indices());
        const uint16_t* raw_indices = indices_array->raw_values();

        std::vector<uint16_t> keys(len);
        for (int64_t i = 0; i < len; ++i) {
            keys[i] = dict_array->IsNull(i) ? 0 : raw_indices[i];
        }

        // Step 3: Shared prefix → FSST → PrefixKeys → BitPackedArray
        build_encoded_dict(result, dict_values, keys.data(),
                          array->null_bitmap_data(), len);
        return result;
    }

    /// Encode using a pre-trained FSST compressor (avoids retraining per batch).
    /// Mirrors Rust's from_byte_array_inner() which takes Arc<Compressor>.
    static LiquidByteViewArray from_arrow_with_compressor(
            const std::shared_ptr<arrow::Array>& array,
            const FsstCompressor& compressor) {
        LiquidByteViewArray result;
        result.is_binary_ = (array->type_id() == arrow::Type::BINARY ||
                             array->type_id() == arrow::Type::LARGE_BINARY);
        if (!result.is_binary_) {
            result.is_ascii_ = check_all_ascii(array);
        }
        result.compressor_ = compressor;  // copy pre-trained compressor
        const int64_t len = array->length();

        // Step 1: Build dictionary (deduplicate strings)
        std::unordered_map<std::string, uint16_t> dict_map;
        std::vector<std::string> dict_values;
        std::vector<uint16_t> keys(len);

        auto get_value = [&](int64_t i, const uint8_t*& data, uint32_t& length) {
            if (array->type_id() == arrow::Type::STRING) {
                auto sa = std::static_pointer_cast<arrow::StringArray>(array);
                int32_t vlen;
                data = sa->GetValue(i, &vlen);
                length = static_cast<uint32_t>(vlen);
            } else if (array->type_id() == arrow::Type::LARGE_STRING) {
                auto sa = std::static_pointer_cast<arrow::LargeStringArray>(array);
                int64_t vlen;
                data = sa->GetValue(i, &vlen);
                length = static_cast<uint32_t>(vlen);
            } else if (array->type_id() == arrow::Type::BINARY) {
                auto ba = std::static_pointer_cast<arrow::BinaryArray>(array);
                int32_t vlen;
                data = ba->GetValue(i, &vlen);
                length = static_cast<uint32_t>(vlen);
            } else {
                auto ba = std::static_pointer_cast<arrow::LargeBinaryArray>(array);
                int64_t vlen;
                data = ba->GetValue(i, &vlen);
                length = static_cast<uint32_t>(vlen);
            }
        };

        for (int64_t i = 0; i < len; ++i) {
            if (array->IsNull(i)) {
                keys[i] = 0;
                continue;
            }
            const uint8_t* data;
            uint32_t vlen;
            get_value(i, data, vlen);
            std::string val(reinterpret_cast<const char*>(data), vlen);

            auto it = dict_map.find(val);
            if (it != dict_map.end()) {
                keys[i] = it->second;
            } else {
                uint16_t idx = static_cast<uint16_t>(dict_values.size());
                dict_map[val] = idx;
                dict_values.push_back(std::move(val));
                keys[i] = idx;
            }
        }

        // Step 2: Shared prefix → FSST → PrefixKeys → BitPackedArray
        // (compressor already set above, passed to skip training)
        const uint8_t* null_bitmap = nullptr;
        std::vector<uint8_t> null_bits;
        if (array->null_count() > 0 && array->null_bitmap()) {
            size_t bitmap_bytes = (len + 7) / 8;
            null_bits.assign(array->null_bitmap()->data(),
                             array->null_bitmap()->data() + bitmap_bytes);
            null_bitmap = null_bits.data();
        }
        build_encoded_dict(result, dict_values, keys.data(), null_bitmap, len,
                          &result.compressor_);
        return result;
    }

    /// Decode back to an Arrow StringArray or BinaryArray.
    /// Optimized: cached FSST decompression + direct flat buffer construction.
    std::shared_ptr<arrow::Array> to_arrow() const {
        uint32_t len = dictionary_keys_.length();

        // ===== Phase A: Get cached decompressed dictionary =====
        const auto& dict = ensure_dict_cached();
        size_t dict_size = dict.entries.size();
        const auto* dict_flat = dict.flat_data.data();
        const auto* dict_flat_offsets = dict.flat_offsets.data();
        const auto* dict_lens = dict.lengths.data();

        // ===== Phase B: Bulk unpack all N keys =====
        std::vector<uint16_t> keys(len);
        dictionary_keys_.bulk_unpack_to(keys.data());

        // ===== Phase C: Compute output offsets in single pass =====
        auto offsets_buf = arrow::AllocateBuffer(
            static_cast<int64_t>(len + 1) * sizeof(int32_t)).ValueOrDie();
        auto* offsets = reinterpret_cast<int32_t*>(offsets_buf->mutable_data());

        const uint8_t* null_bm = dictionary_keys_.null_bitmap_data();
        int64_t total_bytes = 0;
        for (uint32_t i = 0; i < len; ++i) {
            offsets[i] = static_cast<int32_t>(total_bytes);
            bool is_null = null_bm && ((null_bm[i >> 3] & (1 << (i & 7))) == 0);
            if (!is_null && keys[i] < dict_size) {
                total_bytes += dict_lens[keys[i]];
            }
        }
        offsets[len] = static_cast<int32_t>(total_bytes);

        // ===== Phase D: Copy string data from flat dictionary buffer =====
        auto data_buf = arrow::AllocateBuffer(total_bytes).ValueOrDie();
        auto* data_ptr = data_buf->mutable_data();

        for (uint32_t i = 0; i < len; ++i) {
            bool is_null = null_bm && ((null_bm[i >> 3] & (1 << (i & 7))) == 0);
            if (!is_null && keys[i] < dict_size) {
                uint16_t key = keys[i];
                std::memcpy(data_ptr + offsets[i],
                            dict_flat + dict_flat_offsets[key],
                            dict_lens[key]);
            }
        }

        // ===== Phase E: Assemble flat StringArray/BinaryArray =====
        auto null_buf = dictionary_keys_.null_bitmap_arrow_buffer();
        int64_t nc = dictionary_keys_.null_count();
        auto value_type = is_binary_ ? arrow::binary() : arrow::utf8();

        auto data = arrow::ArrayData::Make(
            value_type, static_cast<int64_t>(len),
            {std::move(null_buf), std::move(offsets_buf), std::move(data_buf)},
            nc);
        return arrow::MakeArray(data);
    }

    /// Serialize to bytes (binary-compatible with Rust).
    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> out;
        out.reserve(4096);

        // 1. IPC header (use UInt8 for binary, Int8 for string)
        LiquidIPCHeader ipc(LiquidDataType::ByteViewArray,
                            is_binary_ ? PhysicalType::UInt8 : PhysicalType::Int8);
        ipc.serialize(out);

        // Prepare section data
        // RawFsstBuffer: [symbol_table] [uncompressed_bytes(u64)] [compressed_size(u32)] [data]
        std::vector<uint8_t> fsst_section;
        compressor_.save_symbol_table(fsst_section);
        // uncompressed_bytes (u64 LE)
        uint64_t ub = uncompressed_bytes_;
        const uint8_t* ubp = reinterpret_cast<const uint8_t*>(&ub);
        fsst_section.insert(fsst_section.end(), ubp, ubp + 8);
        // compressed_size (u32 LE)
        uint32_t cs = static_cast<uint32_t>(compressed_data_.size());
        const uint8_t* csp = reinterpret_cast<const uint8_t*>(&cs);
        fsst_section.insert(fsst_section.end(), csp, csp + 4);
        // compressed data
        fsst_section.insert(fsst_section.end(),
                            compressed_data_.begin(), compressed_data_.end());

        auto keys_data = keys_to_bytes();
        auto offsets_data = compact_offsets_.to_bytes();
        auto prefix_keys_data = prefix_keys_to_bytes();

        // 2. ByteViewArrayHeader
        ByteViewArrayHeader bvh;
        bvh.keys_size = static_cast<uint32_t>(keys_data.size());
        bvh.compact_offsets_size = static_cast<uint32_t>(offsets_data.size());
        bvh.shared_prefix_size = static_cast<uint32_t>(shared_prefix_.size());
        bvh.fsst_raw_size = static_cast<uint32_t>(fsst_section.size());
        bvh.fingerprint_size = 0;
        bvh.flags = is_ascii_ ? 1 : 0;
        bvh.serialize(out);

        // 3. Align to 8 bytes, then write sections
        pad_to_8(out);

        // FSST buffer
        out.insert(out.end(), fsst_section.begin(), fsst_section.end());
        pad_to_8(out);

        // BitPackedArray (dictionary keys)
        out.insert(out.end(), keys_data.begin(), keys_data.end());
        pad_to_8(out);

        // Compact offsets
        out.insert(out.end(), offsets_data.begin(), offsets_data.end());
        pad_to_8(out);

        // Prefix keys
        out.insert(out.end(), prefix_keys_data.begin(), prefix_keys_data.end());
        pad_to_8(out);

        // Shared prefix
        out.insert(out.end(), shared_prefix_.begin(), shared_prefix_.end());
        pad_to_8(out);

        // No fingerprints for now

        return out;
    }

    /// Deserialize from bytes.
    static LiquidByteViewArray from_bytes(const uint8_t* data, size_t len) {
        auto ipc = LiquidIPCHeader::deserialize(data, len);
        if (ipc.logical_type_id !=
            static_cast<uint16_t>(LiquidDataType::ByteViewArray)) {
            throw std::runtime_error("Expected ByteViewArray logical type");
        }

        LiquidByteViewArray result;
        result.is_binary_ = (ipc.physical_type_id ==
                             static_cast<uint16_t>(PhysicalType::UInt8));
        size_t pos = LiquidIPCHeader::SIZE;

        if (pos + ByteViewArrayHeader::SIZE > len) {
            throw std::runtime_error("Buffer too small for ByteViewArrayHeader");
        }
        auto bvh = ByteViewArrayHeader::deserialize(data + pos);
        result.is_ascii_ = (bvh.flags & 0x01) != 0;
        pos += ByteViewArrayHeader::SIZE;
        pos = align8(pos);

        // Read FSST buffer
        if (pos + bvh.fsst_raw_size > len) {
            throw std::runtime_error("Buffer too small for FSST data");
        }
        const uint8_t* fsst_data = data + pos;
        size_t fsst_len = bvh.fsst_raw_size;

        // Parse symbol table
        result.compressor_ = FsstCompressor::load_symbol_table(
            fsst_data, fsst_len);
        size_t st_size = result.compressor_.symbol_table_size();

        // Parse RawFsstBuffer after symbol table
        if (st_size + 12 > fsst_len) {
            throw std::runtime_error("Buffer too small for RawFsstBuffer");
        }
        std::memcpy(&result.uncompressed_bytes_,
                     fsst_data + st_size, 8);
        uint32_t comp_size;
        std::memcpy(&comp_size, fsst_data + st_size + 8, 4);
        if (st_size + 12 + comp_size > fsst_len) {
            throw std::runtime_error("FSST compressed data overflow");
        }
        result.compressed_data_.assign(
            fsst_data + st_size + 12,
            fsst_data + st_size + 12 + comp_size);

        pos += bvh.fsst_raw_size;
        pos = align8(pos);

        // Read BitPackedArray (dictionary keys)
        if (pos + bvh.keys_size > len) {
            throw std::runtime_error("Buffer too small for keys");
        }
        result.dictionary_keys_ = BitPackedArray::deserialize(
            data + pos, bvh.keys_size);
        pos += bvh.keys_size;
        pos = align8(pos);

        // Read compact offsets
        if (pos + bvh.compact_offsets_size > len) {
            throw std::runtime_error("Buffer too small for offsets");
        }
        result.compact_offsets_ = CompactOffsets::from_bytes(
            data + pos, bvh.compact_offsets_size);
        pos += bvh.compact_offsets_size;
        pos = align8(pos);

        // Read prefix keys
        size_t dict_count = result.compact_offsets_.len() > 0 ?
            result.compact_offsets_.len() - 1 : 0;
        size_t prefix_keys_bytes = dict_count * 8;
        if (pos + prefix_keys_bytes > len) {
            throw std::runtime_error("Buffer too small for prefix keys");
        }
        result.prefix_keys_.resize(dict_count);
        for (size_t i = 0; i < dict_count; ++i) {
            std::memcpy(&result.prefix_keys_[i], data + pos + i * 8, 8);
        }
        pos += prefix_keys_bytes;
        pos = align8(pos);

        // Read shared prefix
        if (pos + bvh.shared_prefix_size > len) {
            throw std::runtime_error("Buffer too small for shared prefix");
        }
        result.shared_prefix_.assign(
            data + pos, data + pos + bvh.shared_prefix_size);

        return result;
    }

public:
    uint32_t length() const { return dictionary_keys_.length(); }
    size_t memory_size() const {
        return compressed_data_.size() + shared_prefix_.size() +
               dictionary_keys_.memory_size() +
               prefix_keys_.size() * 8 + sizeof(*this);
    }

    /// Access the FSST compressor (e.g., for cross-batch reuse).
    const FsstCompressor& get_compressor() const { return compressor_; }

    /// Mutable compressor access for internal use.
    FsstCompressor& mutable_compressor() { return compressor_; }

#ifdef LIQUID_ENABLE_VELOX
    /// Decode directly to Velox FlatVector<StringView>.
    facebook::velox::VectorPtr to_velox(
        facebook::velox::memory::MemoryPool* pool) const;
#endif

private:
    /// Shared encoding tail: given already-built unique dict_values and
    /// per-element uint16 keys, compute shared prefix, compress via FSST,
    /// build PrefixKeys, and pack keys into BitPackedArray.
    /// If `compressor` is non-null, it is used directly (no training).
    /// Otherwise a fresh FSST compressor is trained on the suffix data.
    static void build_encoded_dict(
            LiquidByteViewArray& result,
            const std::vector<std::string>& dict_values,
            const uint16_t* keys,
            const uint8_t* null_bitmap_data,
            int64_t len,
            const FsstCompressor* compressor = nullptr) {
        // Step A: Compute shared prefix
        if (!dict_values.empty()) {
            result.shared_prefix_.assign(
                dict_values[0].begin(), dict_values[0].end());
            for (size_t d = 1; d < dict_values.size(); ++d) {
                size_t match_len = 0;
                size_t max_check = std::min(
                    result.shared_prefix_.size(), dict_values[d].size());
                while (match_len < max_check &&
                       result.shared_prefix_[match_len] ==
                       static_cast<uint8_t>(dict_values[d][match_len])) {
                    ++match_len;
                }
                result.shared_prefix_.resize(match_len);
            }
        }
        size_t sp_len = result.shared_prefix_.size();

        // Step B: Concatenate suffix bytes
        std::vector<uint8_t> all_suffixes;
        std::vector<uint32_t> suffix_offsets;
        suffix_offsets.push_back(0);
        for (auto& dv : dict_values) {
            size_t suffix_len = dv.size() >= sp_len ? dv.size() - sp_len : 0;
            const uint8_t* suffix_data =
                reinterpret_cast<const uint8_t*>(dv.data()) + sp_len;
            all_suffixes.insert(all_suffixes.end(),
                                suffix_data, suffix_data + suffix_len);
            suffix_offsets.push_back(static_cast<uint32_t>(all_suffixes.size()));
        }

        // Step C: Train FSST (skip if compressor provided)
        if (compressor) {
            result.compressor_ = *compressor;
        } else {
            result.compressor_.train(all_suffixes.data(), all_suffixes.size());
        }

        // Step D: Compress each suffix and build CompactOffsets
        std::vector<uint8_t> compressed_buffer;
        std::vector<uint32_t> compressed_offsets;
        compressed_offsets.push_back(0);
        for (size_t d = 0; d < dict_values.size(); ++d) {
            uint32_t start = suffix_offsets[d];
            uint32_t end = suffix_offsets[d + 1];
            auto compressed = result.compressor_.compress(
                all_suffixes.data() + start, end - start);
            compressed_buffer.insert(compressed_buffer.end(),
                                     compressed.begin(), compressed.end());
            compressed_offsets.push_back(
                static_cast<uint32_t>(compressed_buffer.size()));
        }
        result.uncompressed_bytes_ = all_suffixes.size();
        result.compressed_data_ = std::move(compressed_buffer);
        result.compact_offsets_ = CompactOffsets(compressed_offsets);

        // Step E: Build prefix keys
        for (auto& dv : dict_values) {
            size_t suffix_len = dv.size() >= sp_len ? dv.size() - sp_len : 0;
            const uint8_t* suffix = reinterpret_cast<const uint8_t*>(
                dv.data()) + sp_len;
            result.prefix_keys_.emplace_back(suffix, suffix_len);
        }

        // Step F: BitPackedArray for keys
        uint16_t max_key = dict_values.empty() ? 0 :
            static_cast<uint16_t>(dict_values.size() - 1);
        uint8_t bw = get_bit_width(static_cast<uint64_t>(max_key));
        std::vector<uint64_t> key_offsets(len);
        for (int64_t i = 0; i < len; ++i) {
            key_offsets[i] = static_cast<uint64_t>(keys[i]);
        }
        result.dictionary_keys_ = BitPackedArray(
            key_offsets.data(), null_bitmap_data, static_cast<uint32_t>(len), bw);
    }

    static void pad_to_8(std::vector<uint8_t>& out) {
        while (out.size() % 8 != 0) out.push_back(0);
    }

    std::vector<uint8_t> keys_to_bytes() const {
        std::vector<uint8_t> out;
        dictionary_keys_.serialize(out);
        return out;
    }

    std::vector<uint8_t> prefix_keys_to_bytes() const {
        std::vector<uint8_t> out;
        out.reserve(prefix_keys_.size() * 8);
        for (auto& pk : prefix_keys_) {
            out.insert(out.end(), pk.prefix7, pk.prefix7 + 7);
            out.push_back(pk.len_byte);
        }
        return out;
    }

    FsstCompressor compressor_;
    uint64_t uncompressed_bytes_ = 0;
    std::vector<uint8_t> compressed_data_;
    CompactOffsets compact_offsets_;
    BitPackedArray dictionary_keys_;
    std::vector<PrefixKey> prefix_keys_;
    std::vector<uint8_t> shared_prefix_;
    /// Returns true if every non-null string in the Arrow array is pure ASCII.
    static bool check_all_ascii(const std::shared_ptr<arrow::Array>& array) {
        if (array->type_id() == arrow::Type::BINARY ||
            array->type_id() == arrow::Type::LARGE_BINARY) {
            return false;
        }
        const uint8_t* null_bm = array->null_bitmap_data();
        for (int64_t i = 0; i < array->length(); ++i) {
            if (null_bm && ((null_bm[i >> 3] & (1 << (i & 7))) == 0)) continue;
            int32_t vlen = 0;
            const uint8_t* data = nullptr;
            if (array->type_id() == arrow::Type::STRING) {
                auto sa = std::static_pointer_cast<const arrow::StringArray>(array);
                data = sa->GetValue(i, &vlen);
            } else if (array->type_id() == arrow::Type::LARGE_STRING) {
                auto sa = std::static_pointer_cast<const arrow::LargeStringArray>(array);
                int64_t lvlen;
                data = sa->GetValue(i, &lvlen);
                vlen = static_cast<int32_t>(lvlen);
            } else {
                return false;  // Not a string type
            }
            for (int32_t j = 0; j < vlen; ++j) {
                if (data[j] >= 0x80) return false;
            }
        }
        return true;
    }

    bool is_binary_ = false;  // true for Binary/LargeBinary, false for String/LargeString
    bool is_ascii_ = false;   // true if all strings contain only ASCII bytes

    // ── Cached decompressed dictionary (lazy, avoids repeated FSST decompress) ──
    struct DecompressedDict {
        std::vector<std::string> entries;     // decompressed string values
        std::vector<int32_t> lengths;         // per-entry byte lengths
        // Pre-built contiguous data buffer for fast memcpy
        std::vector<uint8_t> flat_data;       // all entries concatenated
        std::vector<int32_t> flat_offsets;    // offset[d] = start of entry d in flat_data
    };
    mutable std::shared_ptr<DecompressedDict> cached_dict_;

    /// Ensure decompressed dictionary is cached.
    const DecompressedDict& ensure_dict_cached() const {
        if (cached_dict_) return *cached_dict_;

        auto dict = std::make_shared<DecompressedDict>();
        size_t dict_size = compact_offsets_.len() > 0
                           ? compact_offsets_.len() - 1 : 0;

        dict->entries.resize(dict_size);
        dict->lengths.resize(dict_size);
        dict->flat_offsets.resize(dict_size + 1);

        // Pass 1: decompress all entries
        size_t total_bytes = 0;
        for (size_t d = 0; d < dict_size; ++d) {
            uint32_t comp_start = compact_offsets_.get(d);
            uint32_t comp_end = compact_offsets_.get(d + 1);
            auto entry = FsstCompressor::decompress(
                compressor_.symbol_count() > 0 ? &compressor_.symbol(0) : nullptr,
                compressor_.symbol_count(),
                compressed_data_.data() + comp_start, comp_end - comp_start);
            dict->entries[d].assign(shared_prefix_.begin(), shared_prefix_.end());
            dict->entries[d].append(
                reinterpret_cast<const char*>(entry.data()), entry.size());
            dict->lengths[d] = static_cast<int32_t>(dict->entries[d].size());
            total_bytes += dict->entries[d].size();
        }

        // Pass 2: build contiguous flat buffer
        dict->flat_data.resize(total_bytes);
        int32_t off = 0;
        for (size_t d = 0; d < dict_size; ++d) {
            dict->flat_offsets[d] = off;
            std::memcpy(dict->flat_data.data() + off,
                        dict->entries[d].data(), dict->lengths[d]);
            off += dict->lengths[d];
        }
        dict->flat_offsets[dict_size] = off;

        cached_dict_ = std::move(dict);
        return *cached_dict_;
    }
};

}  // namespace liquid_cache
