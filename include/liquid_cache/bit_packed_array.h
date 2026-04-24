// liquid_cache/bit_packed_array.h
// BitPackedArray - SIMD-friendly bit-packed integer storage
// Mirrors the Rust BitPackedArray from liquid-cache/src/core/src/liquid_array/raw/
#pragma once

#include <arrow/buffer.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <algorithm>

namespace liquid_cache {

/// A bit-packed array of unsigned integers.
///
/// Each element is stored using exactly `bit_width` bits.
/// The storage uses 1024-element blocks (FastLanes convention)
/// for SIMD-friendly access patterns.
///
/// Binary layout (matches Rust BitPackedArray::to_bytes / from_bytes):
///   Header (16 bytes):
///     [0..3]   original_len  (uint32_t, number of elements)
///     [4]      bit_width     (uint8_t, bits per element; 0 = all zero)
///     [5]      has_nulls     (uint8_t, 1 if null bitmap present)
///     [6..9]   nulls_len     (uint32_t, byte length of null bitmap)
///     [10..13] values_len    (uint32_t, byte length of packed values)
///     [14..15] padding       (2 bytes, zeroed)
///   [If has_nulls == 1]: null bitmap (nulls_len bytes)
///   Padding to 8-byte alignment (relative to start of this structure)
///   Packed values data (values_len bytes)
class BitPackedArray {
public:
    BitPackedArray() = default;

    /// Construct from raw unsigned values.
    /// @param values  raw unsigned values (length = count)
    /// @param nulls   optional null bitmap (1 bit per element, LSB first);
    ///                nullptr means no nulls
    /// @param count   number of elements
    /// @param bit_width  bits per element (0..64)
    BitPackedArray(const uint64_t* values, const uint8_t* nulls,
                   uint32_t count, uint8_t bit_width)
        : length_(count), bit_width_(bit_width) {
        if (nulls) {
            size_t bitmap_bytes = (count + 7) / 8;
            null_bitmap_.assign(nulls, nulls + bitmap_bytes);
        }
        pack(values, count, bit_width);
    }

    /// Pack unsigned values with the given bit width.
    /// This is a straightforward scalar implementation; a production version
    /// would use FastLanes SIMD kernels (1024-element blocks).
    void pack(const uint64_t* values, uint32_t count, uint8_t bw) {
        bit_width_ = bw;
        length_ = count;
        if (bw == 0 || count == 0) {
            packed_data_.clear();
            return;
        }
        size_t total_bits = static_cast<size_t>(count) * bw;
        size_t total_bytes = (total_bits + 7) / 8;
        packed_data_.assign(total_bytes, 0);

        for (uint32_t i = 0; i < count; ++i) {
            uint64_t val = values[i];
            if (bw < 64) val &= (1ULL << bw) - 1;
            size_t bit_offset = static_cast<size_t>(i) * bw;
            size_t byte_idx = bit_offset / 8;
            uint8_t bit_idx = bit_offset % 8;
            size_t bytes_needed = (bit_idx + bw + 7) / 8;

            // Write low part: val << bit_idx may overflow when bit_idx + bw > 64
            uint64_t shifted_low = val << bit_idx;
            for (size_t b = 0; b < std::min<size_t>(8, bytes_needed) && (byte_idx + b) < packed_data_.size(); ++b) {
                packed_data_[byte_idx + b] |= static_cast<uint8_t>(shifted_low >> (b * 8));
            }

            // Write high part if value spans more than 8 bytes
            if (bit_idx > 0 && bit_idx + bw > 64) {
                uint64_t shifted_high = val >> (64 - bit_idx);
                for (size_t b = 8; b < bytes_needed && (byte_idx + b) < packed_data_.size(); ++b) {
                    packed_data_[byte_idx + b] |= static_cast<uint8_t>(shifted_high >> ((b - 8) * 8));
                }
            }
        }
    }

    /// Unpack a single element.
    uint64_t get(uint32_t index) const {
        if (bit_width_ == 0) return 0;
        if (index >= length_) return 0;  // bounds check on element index
        size_t bit_offset = static_cast<size_t>(index) * bit_width_;
        size_t byte_idx = bit_offset / 8;
        uint8_t bit_idx = bit_offset % 8;

        // 安全边界检查：防止无符号整数下溢
        if (byte_idx >= packed_data_.size()) {
            return 0;
        }

        size_t available_bytes = packed_data_.size() - byte_idx;

        uint64_t raw = 0;
        if (bit_idx + bit_width_ <= 64) {
            // Fast path: value fits within 8 bytes
            size_t bytes_to_read = std::min<size_t>(sizeof(uint64_t), available_bytes);
            if (bytes_to_read > 0) {
                std::memcpy(&raw, packed_data_.data() + byte_idx, bytes_to_read);
            }
            raw >>= bit_idx;
        } else {
            // Slow path: value spans more than 8 bytes (bit_idx + bit_width > 64)
            // Read low 8 bytes
            uint64_t low = 0;
            size_t low_bytes = std::min<size_t>(8, available_bytes);
            std::memcpy(&low, packed_data_.data() + byte_idx, low_bytes);
            // Read high byte(s) - need at most 1 more byte for bw<=64
            uint64_t high = 0;
            if (available_bytes > 8) {
                size_t high_bytes = std::min<size_t>(available_bytes - 8, sizeof(uint64_t));
                std::memcpy(&high, packed_data_.data() + byte_idx + 8, high_bytes);
            }
            raw = (low >> bit_idx) | (high << (64 - bit_idx));
        }
        if (bit_width_ < 64) {
            raw &= (1ULL << bit_width_) - 1;
        }
        return raw;
    }

    /// Unpack all elements into a vector of uint64_t.
    std::vector<uint64_t> unpack_all() const {
        std::vector<uint64_t> out(length_);
        for (uint32_t i = 0; i < length_; ++i) {
            out[i] = get(i);
        }
        return out;
    }

    /// Check if element at index is null.
    bool is_null(uint32_t index) const {
        if (null_bitmap_.empty()) return false;
        return (null_bitmap_[index / 8] & (1 << (index % 8))) == 0;
    }

    /// Serialize to bytes (matches Rust BitPackedArray 16-byte header format).
    void serialize(std::vector<uint8_t>& out) const {
        size_t start_offset = out.size();

        uint8_t has_nulls_flag = null_bitmap_.empty() ? 0 : 1;
        uint32_t nulls_len = static_cast<uint32_t>(null_bitmap_.size());
        uint32_t values_len = static_cast<uint32_t>(packed_data_.size());

        // Header (16 bytes)
        // [0..3] length
        const uint8_t* lp = reinterpret_cast<const uint8_t*>(&length_);
        out.insert(out.end(), lp, lp + 4);
        // [4] bit_width
        out.push_back(bit_width_);
        // [5] has_nulls
        out.push_back(has_nulls_flag);
        // [6..9] nulls_len
        const uint8_t* nlp = reinterpret_cast<const uint8_t*>(&nulls_len);
        out.insert(out.end(), nlp, nlp + 4);
        // [10..13] values_len
        const uint8_t* vlp = reinterpret_cast<const uint8_t*>(&values_len);
        out.insert(out.end(), vlp, vlp + 4);
        // [14..15] padding
        out.push_back(0);
        out.push_back(0);

        // Null bitmap (if present)
        if (has_nulls_flag) {
            out.insert(out.end(), null_bitmap_.begin(), null_bitmap_.end());
        }

        // Pad to 8-byte alignment (relative to start_offset)
        size_t values_offset_base = 16 + (has_nulls_flag ? nulls_len : 0);
        size_t values_offset = (values_offset_base + 7) & ~static_cast<size_t>(7);
        while ((out.size() - start_offset) < values_offset) {
            out.push_back(0);
        }

        // Packed values data
        out.insert(out.end(), packed_data_.begin(), packed_data_.end());
    }

    /// Deserialize from bytes (16-byte header format).
    static BitPackedArray deserialize(const uint8_t* data, size_t len) {
        if (len < 16) throw std::runtime_error("BitPackedArray: buffer too small for 16-byte header");
        BitPackedArray arr;

        // Read 16-byte header
        std::memcpy(&arr.length_, data, 4);
        arr.bit_width_ = data[4];
        uint8_t has_nulls = data[5];
        uint32_t nulls_len = 0;
        uint32_t values_len = 0;
        std::memcpy(&nulls_len, data + 6, 4);
        std::memcpy(&values_len, data + 10, 4);

        // Read null bitmap if present
        if (has_nulls) {
            size_t bitmap_bytes = static_cast<size_t>(nulls_len);
            if (16 + bitmap_bytes > len) {
                throw std::runtime_error("BitPackedArray: null bitmap extends beyond buffer");
            }
            arr.null_bitmap_.assign(data + 16, data + 16 + bitmap_bytes);
        }

        // Calculate 8-byte aligned offset for values
        size_t values_offset_base = 16 + (has_nulls ? nulls_len : 0);
        size_t values_offset = (values_offset_base + 7) & ~static_cast<size_t>(7);

        // Read packed values
        if (values_len > 0) {
            if (values_offset + values_len > len) {
                throw std::runtime_error("BitPackedArray: packed data extends beyond buffer");
            }
            arr.packed_data_.assign(data + values_offset, data + values_offset + values_len);
        }

        return arr;
    }

    uint32_t length() const { return length_; }
    uint8_t bit_width() const { return bit_width_; }
    bool has_nulls() const { return !null_bitmap_.empty(); }
    size_t memory_size() const {
        return packed_data_.size() + null_bitmap_.size() + sizeof(*this);
    }

    // ── Bulk unpack API (avoids per-element get(i)) ──────────────────

    /// Bulk-unpack all elements into a typed buffer.
    /// T must be an unsigned integer type (uint8_t, uint16_t, uint32_t, uint64_t).
    /// The output buffer must have room for at least length_ elements.
    template <typename T>
    void bulk_unpack_to(T* out) const {
        static_assert(std::is_unsigned<T>::value, "T must be unsigned");
        const uint32_t n = length_;
        const uint8_t bw = bit_width_;

        if (bw == 0 || n == 0) {
            std::memset(out, 0, n * sizeof(T));
            return;
        }

        const uint64_t mask = (bw < 64) ? ((1ULL << bw) - 1) : ~0ULL;
        const uint8_t* src = packed_data_.data();
        const size_t src_size = packed_data_.size();

        // Process elements in bulk, reading 8-byte chunks where possible
        for (uint32_t i = 0; i < n; ++i) {
            size_t bit_offset = static_cast<size_t>(i) * bw;
            size_t byte_idx = bit_offset / 8;
            uint8_t bit_idx = bit_offset % 8;

            uint64_t raw = 0;
            if (bit_idx + bw <= 64 && byte_idx + 8 <= src_size) {
                // Fast path: read 8 bytes directly (no overflow)
                std::memcpy(&raw, src + byte_idx, 8);
                raw >>= bit_idx;
            } else if (bit_idx + bw <= 64) {
                // Near end: partial read
                size_t avail = src_size - byte_idx;
                size_t to_read = std::min<size_t>(sizeof(uint64_t), avail);
                std::memcpy(&raw, src + byte_idx, to_read);
                raw >>= bit_idx;
            } else {
                // Cross 8-byte boundary
                uint64_t low = 0, high = 0;
                size_t avail = src_size - byte_idx;
                size_t low_bytes = std::min<size_t>(8, avail);
                std::memcpy(&low, src + byte_idx, low_bytes);
                if (avail > 8) {
                    size_t hi_bytes = std::min<size_t>(avail - 8, sizeof(uint64_t));
                    std::memcpy(&high, src + byte_idx + 8, hi_bytes);
                }
                raw = (low >> bit_idx) | (high << (64 - bit_idx));
            }
            out[i] = static_cast<T>(raw & mask);
        }
    }

    // ── Null bitmap accessors ────────────────────────────────────────

    /// Raw pointer to null bitmap data (nullptr if no nulls).
    const uint8_t* null_bitmap_data() const {
        return null_bitmap_.empty() ? nullptr : null_bitmap_.data();
    }

    /// Size of null bitmap in bytes.
    size_t null_bitmap_size() const { return null_bitmap_.size(); }

    /// Count of null elements.
    int64_t null_count() const {
        if (null_bitmap_.empty()) return 0;
        int64_t count = 0;
        for (uint32_t i = 0; i < length_; ++i) {
            if ((null_bitmap_[i / 8] & (1 << (i % 8))) == 0) ++count;
        }
        return count;
    }

    /// Create an owning Arrow Buffer copy of the null bitmap.
    /// Returns nullptr if no nulls.
    std::shared_ptr<arrow::Buffer> null_bitmap_arrow_buffer() const {
        if (null_bitmap_.empty()) return nullptr;
        int64_t nbytes = static_cast<int64_t>((length_ + 7) / 8);
        auto buf = arrow::AllocateBuffer(nbytes).ValueOrDie();
        std::memcpy(buf->mutable_data(), null_bitmap_.data(), nbytes);
        return std::move(buf);
    }

private:
    uint32_t length_ = 0;
    uint8_t bit_width_ = 0;
    std::vector<uint8_t> null_bitmap_;
    std::vector<uint8_t> packed_data_;
};

}  // namespace liquid_cache
