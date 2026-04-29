// liquid_cache/fsst.h
// Minimal FSST (Fast Static Symbol Table) compressor/decompressor.
// Binary-compatible with the fsst-rs crate's symbol table format:
//   [symbol_count: u8]
//   [symbol_lengths: u8 x symbol_count]
//   [symbols: u64 x symbol_count]  (each stored as 8-byte LE, first `len` bytes meaningful)
//
// Compressed data format:
//   - Codes 0x00..symbol_count-1 → expand to corresponding symbol
//   - Code 0xFF → escape, next byte is a literal
//   - (When symbol_count < 255, codes symbol_count..0xFE are unused)
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace liquid_cache {

struct FsstSymbol {
    uint8_t bytes[8] = {};
    uint8_t len = 0;
};

class FsstCompressor {
public:
    FsstCompressor() = default;

    uint8_t symbol_count() const { return symbol_count_; }
    const FsstSymbol& symbol(uint8_t idx) const { return symbols_[idx]; }

    /// Train the symbol table from input data.
    /// Uses a greedy bigram/trigram counting approach (simplified vs. full FSST paper).
    /// For large inputs, only samples the first 1MB for speed.
    void train(const uint8_t* data, size_t len) {
        symbol_count_ = 0;
        if (len == 0) return;

        // Sample data for large inputs to avoid O(n) hash map overhead
        static constexpr size_t kMaxTrainBytes = 1 << 20;  // 1MB sample
        size_t train_len = std::min(len, kMaxTrainBytes);

        // Count bigrams (2-byte sequences) - main compression opportunity
        std::unordered_map<uint16_t, uint32_t> bigram_counts;
        for (size_t i = 0; i + 1 < train_len; ++i) {
            uint16_t bg = static_cast<uint16_t>(data[i]) |
                          (static_cast<uint16_t>(data[i + 1]) << 8);
            bigram_counts[bg]++;
        }

        // Also count trigrams for better compression
        std::unordered_map<uint32_t, uint32_t> trigram_counts;
        for (size_t i = 0; i + 2 < train_len; ++i) {
            uint32_t tg = static_cast<uint32_t>(data[i]) |
                          (static_cast<uint32_t>(data[i + 1]) << 8) |
                          (static_cast<uint32_t>(data[i + 2]) << 16);
            trigram_counts[tg]++;
        }

        // Rank symbols by savings (count * (len - 1) bytes saved)
        struct Candidate {
            uint8_t bytes[8];
            uint8_t len;
            int64_t savings;
        };
        std::vector<Candidate> candidates;

        for (auto& [bg, cnt] : bigram_counts) {
            if (cnt >= 4) {  // threshold
                Candidate c;
                c.bytes[0] = bg & 0xFF;
                c.bytes[1] = (bg >> 8) & 0xFF;
                std::memset(c.bytes + 2, 0, 6);
                c.len = 2;
                c.savings = static_cast<int64_t>(cnt) * 1;  // save 1 byte per occurrence
                candidates.push_back(c);
            }
        }

        for (auto& [tg, cnt] : trigram_counts) {
            if (cnt >= 3) {
                Candidate c;
                c.bytes[0] = tg & 0xFF;
                c.bytes[1] = (tg >> 8) & 0xFF;
                c.bytes[2] = (tg >> 16) & 0xFF;
                std::memset(c.bytes + 3, 0, 5);
                c.len = 3;
                c.savings = static_cast<int64_t>(cnt) * 2;  // save 2 bytes per occurrence
                candidates.push_back(c);
            }
        }

        // Sort by savings (descending)
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) {
                      return a.savings > b.savings;
                  });

        // Select top symbols (up to 255)
        for (auto& c : candidates) {
            if (symbol_count_ >= 255) break;
            symbols_[symbol_count_].len = c.len;
            std::memcpy(symbols_[symbol_count_].bytes, c.bytes, 8);
            ++symbol_count_;
        }

        // Build first-byte lookup table for fast compression
        build_lookup_table();
    }

    /// Compress input data using the trained symbol table.
    /// Uses a first-byte lookup table for O(1) symbol narrowing per position.
    std::vector<uint8_t> compress(const uint8_t* input, size_t input_len) const {
        std::vector<uint8_t> output;
        output.reserve(input_len);

        size_t i = 0;
        while (i < input_len) {
            // Use first-byte lookup table to narrow candidates
            int best_sym = -1;
            uint8_t best_len = 0;
            uint8_t first_byte = input[i];

            // Check all symbols that start with this byte
            uint8_t start = first_byte_idx_[first_byte];
            uint8_t end = (first_byte < 255) ? first_byte_idx_[first_byte + 1]
                                             : symbol_count_;
            for (uint8_t idx = start; idx < end; ++idx) {
                uint8_t s = first_byte_syms_[idx];
                uint8_t slen = symbols_[s].len;
                if (slen > best_len && i + slen <= input_len) {
                    if (std::memcmp(input + i, symbols_[s].bytes, slen) == 0) {
                        best_sym = s;
                        best_len = slen;
                    }
                }
            }

            if (best_sym >= 0) {
                output.push_back(static_cast<uint8_t>(best_sym));
                i += best_len;
            } else {
                // Escape literal byte
                output.push_back(0xFF);
                output.push_back(input[i]);
                ++i;
            }
        }
        return output;
    }

    /// Decompress FSST-compressed data.
    static std::vector<uint8_t> decompress(
            const FsstSymbol* symbols, uint8_t symbol_count,
            const uint8_t* input, size_t input_len) {
        std::vector<uint8_t> output;
        output.reserve(input_len * 2);  // rough estimate

        size_t i = 0;
        while (i < input_len) {
            uint8_t code = input[i++];
            if (code == 0xFF) {
                // Escape: next byte is literal
                if (i >= input_len) break;
                output.push_back(input[i++]);
            } else if (code < symbol_count) {
                // Symbol expansion
                output.insert(output.end(),
                              symbols[code].bytes,
                              symbols[code].bytes + symbols[code].len);
            } else {
                // Invalid code - treat as literal for robustness
                output.push_back(code);
            }
        }
        return output;
    }

    /// Serialize the symbol table (fsst-rs compatible format).
    void save_symbol_table(std::vector<uint8_t>& out) const {
        out.push_back(symbol_count_);
        // Symbol lengths
        for (uint8_t i = 0; i < symbol_count_; ++i) {
            out.push_back(symbols_[i].len);
        }
        // Symbols as u64 LE
        for (uint8_t i = 0; i < symbol_count_; ++i) {
            uint64_t sym = 0;
            std::memcpy(&sym, symbols_[i].bytes, 8);
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&sym);
            out.insert(out.end(), p, p + 8);
        }
    }

    /// Load symbol table from bytes.
    static FsstCompressor load_symbol_table(const uint8_t* data, size_t len) {
        FsstCompressor comp;
        if (len == 0) return comp;

        comp.symbol_count_ = data[0];
        size_t offset = 1;

        if (offset + comp.symbol_count_ > len) {
            throw std::runtime_error("FSST: buffer too small for symbol lengths");
        }
        for (uint8_t i = 0; i < comp.symbol_count_; ++i) {
            comp.symbols_[i].len = data[offset++];
        }

        if (offset + static_cast<size_t>(comp.symbol_count_) * 8 > len) {
            throw std::runtime_error("FSST: buffer too small for symbols");
        }
        for (uint8_t i = 0; i < comp.symbol_count_; ++i) {
            uint64_t sym = 0;
            std::memcpy(&sym, data + offset, 8);
            std::memcpy(comp.symbols_[i].bytes, &sym, 8);
            offset += 8;
        }

        comp.build_lookup_table();
        return comp;
    }

    /// Size of the serialized symbol table.
    size_t symbol_table_size() const {
        return 1 + symbol_count_ + static_cast<size_t>(symbol_count_) * 8;
    }

private:
    FsstSymbol symbols_[255];
    uint8_t symbol_count_ = 0;

    // First-byte lookup table: maps first byte → indices in first_byte_syms_
    // first_byte_idx_[b] = start index, first_byte_idx_[b+1] = end index
    uint8_t first_byte_idx_[256];
    // Contiguous array of symbol indices grouped by their first byte
    uint8_t first_byte_syms_[255];

    /// Build first-byte lookup table for fast compression.
    /// Groups symbols by their first byte so compress() can narrow candidates.
    void build_lookup_table() {
        // Count symbols per first byte
        uint8_t counts[256] = {};
        for (uint8_t s = 0; s < symbol_count_; ++s) {
            counts[symbols_[s].bytes[0]]++;
        }

        // Build prefix-sum index (256 entries, where idx[b]..idx[b+1] is the
        // range of symbols starting with byte b)
        first_byte_idx_[0] = 0;
        for (int b = 0; b < 255; ++b) {
            first_byte_idx_[b + 1] = first_byte_idx_[b] + counts[b];
        }

        // Fill symbol indices grouped by first byte
        uint8_t pos[256] = {};
        std::memcpy(pos, first_byte_idx_, 256);
        for (uint8_t s = 0; s < symbol_count_; ++s) {
            uint8_t fb = symbols_[s].bytes[0];
            first_byte_syms_[pos[fb]++] = s;
        }
    }
};

}  // namespace liquid_cache
