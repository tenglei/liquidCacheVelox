// liquid_cache/liquid_cache_store.h
// Column-major Liquid Cache Store with column projection and row filtering.
//
// Equivalent to Rust's LiquidCache + ArtIndex + CachedColumn architecture:
//   - src/core/src/cache/core.rs       (LiquidCache)
//   - src/core/src/cache/index.rs      (ArtIndex)
//   - src/core/src/cache/cached_batch.rs (CacheEntry)
//   - src/datafusion/src/cache/id.rs   (ParquetArrayID)
//   - src/datafusion/src/cache/column.rs (CachedColumn)
//
// Key design:
//   - Stores Liquid struct objects in memory (NOT serialized bytes)
//   - Column-major: each column's each batch is independently cached
//   - Column projection: only decode requested columns
//   - Row filtering: apply BooleanArray mask during decode
//   - Zero-deserialization reads: Liquid structs are accessed directly
#pragma once

#include <arrow/api.h>
#include <arrow/compute/api.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <unordered_map>
#include <vector>

#include "liquid_cache/liquid_array.h"

namespace liquid_cache {

// ═══════════════════════════════════════════════════════════════════════
// LiquidCacheKey: identifies a specific column batch in the cache.
// Equivalent to Rust's ParquetArrayID:
//   file_id(16) | rg_id(16) | col_id(16) | batch_id(16)
// ═══════════════════════════════════════════════════════════════════════

struct LiquidCacheKey {
    uint16_t file_id = 0;
    uint16_t rg_id = 0;      // row group id
    uint16_t col_id = 0;
    uint16_t batch_id = 0;

    LiquidCacheKey() = default;
    LiquidCacheKey(uint16_t f, uint16_t rg, uint16_t c, uint16_t b)
        : file_id(f), rg_id(rg), col_id(c), batch_id(b) {}

    /// Pack into a single uint64_t for hashing/comparison.
    uint64_t to_u64() const {
        return (static_cast<uint64_t>(file_id) << 48) |
               (static_cast<uint64_t>(rg_id) << 32) |
               (static_cast<uint64_t>(col_id) << 16) |
               static_cast<uint64_t>(batch_id);
    }

    static LiquidCacheKey from_u64(uint64_t v) {
        return {
            static_cast<uint16_t>(v >> 48),
            static_cast<uint16_t>((v >> 32) & 0xFFFF),
            static_cast<uint16_t>((v >> 16) & 0xFFFF),
            static_cast<uint16_t>(v & 0xFFFF)
        };
    }

    bool operator==(const LiquidCacheKey& o) const {
        return to_u64() == o.to_u64();
    }
};

struct LiquidCacheKeyHash {
    size_t operator()(const LiquidCacheKey& k) const {
        return std::hash<uint64_t>{}(k.to_u64());
    }
};

// ═══════════════════════════════════════════════════════════════════════
// CacheEntryType and CacheEntry
// Equivalent to Rust's CacheEntry enum (simplified: MemoryArrow + MemoryLiquid)
// ═══════════════════════════════════════════════════════════════════════

enum class CacheEntryType {
    MemoryArrow,     // Raw Arrow array in memory (no compression)
    MemoryLiquid,    // Liquid-encoded struct in memory (compressed, no serialization)
};

struct CacheEntry {
    CacheEntryType type;
    std::shared_ptr<arrow::Array> arrow_array;    // For MemoryArrow
    LiquidArrayRef liquid_array;                   // For MemoryLiquid

    /// Read as Arrow array, optionally filtered by selection mask.
    /// Mirrors Rust LiquidCache::read_arrow_array() dispatch.
    std::shared_ptr<arrow::Array> read(
            const std::shared_ptr<arrow::BooleanArray>& selection = nullptr) const {
        if (type == CacheEntryType::MemoryLiquid && liquid_array) {
            if (selection) {
                return liquid_array->filter(selection);
            }
            return liquid_array->to_arrow();
        }
        if (type == CacheEntryType::MemoryArrow && arrow_array) {
            if (selection) {
                auto result = arrow::compute::Filter(arrow_array, selection);
                if (!result.ok()) {
                    throw std::runtime_error(
                        "Filter failed: " + result.status().ToString());
                }
                return result.ValueOrDie().make_array();
            }
            return arrow_array;
        }
        return nullptr;
    }

    /// Memory usage of this entry (bytes).
    size_t memory_size() const {
        if (type == CacheEntryType::MemoryLiquid && liquid_array) {
            return liquid_array->memory_size();
        }
        if (type == CacheEntryType::MemoryArrow && arrow_array) {
            size_t total = 0;
            for (const auto& buf : arrow_array->data()->buffers) {
                if (buf) total += buf->size();
            }
            return total;
        }
        return 0;
    }

    /// Number of elements.
    uint32_t length() const {
        if (type == CacheEntryType::MemoryLiquid && liquid_array) {
            return liquid_array->length();
        }
        if (type == CacheEntryType::MemoryArrow && arrow_array) {
            return static_cast<uint32_t>(arrow_array->length());
        }
        return 0;
    }

    static CacheEntry from_liquid(LiquidArrayRef arr) {
        return {CacheEntryType::MemoryLiquid, nullptr, std::move(arr)};
    }

    static CacheEntry from_arrow(std::shared_ptr<arrow::Array> arr) {
        return {CacheEntryType::MemoryArrow, std::move(arr), nullptr};
    }
};

// ═══════════════════════════════════════════════════════════════════════
// LiquidCacheStore: column-major cache with projection and filtering.
//
// Equivalent to Rust's LiquidCache struct from src/core/src/cache/core.rs,
// simplified to:
//   - In-memory only (no disk tier, no squeeze)
//   - No eviction policy (unbounded)
//   - Thread-safe via std::mutex
//   - Column projection via projection indices
//   - Row filtering via BooleanArray mask
// ═══════════════════════════════════════════════════════════════════════

class LiquidCacheStore {
public:
    LiquidCacheStore() = default;

    // ── Insert operations ────────────────────────────────────────────

    /// Insert a Liquid-encoded array into the cache.
    /// Mirrors Rust: cache.insert(entry_id, array).await
    void insert(const LiquidCacheKey& key, LiquidArrayRef array) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_[key] = CacheEntry::from_liquid(std::move(array));
    }

    /// Insert a raw Arrow array into the cache (MemoryArrow entry).
    void insert_arrow(const LiquidCacheKey& key,
                      std::shared_ptr<arrow::Array> array) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_[key] = CacheEntry::from_arrow(std::move(array));
    }

    // ── Single-entry read ────────────────────────────────────────────

    /// Check if a key is cached.
    bool contains(const LiquidCacheKey& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.count(key) > 0;
    }

    /// Get a single cached array, optionally filtered.
    /// Mirrors Rust: cache.get(&entry_id).with_selection(filter).read().await
    std::shared_ptr<arrow::Array> get(
            const LiquidCacheKey& key,
            const std::shared_ptr<arrow::BooleanArray>& selection = nullptr) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return get_unlocked(key, selection);
    }

    // ── Batch read with column projection and row filtering ──────────

    /// Read a RecordBatch from cache with column projection and optional row filter.
    ///
    /// This is the primary read path, equivalent to Rust's
    /// LiquidCacheReader::read_from_cache() with projection_columns.
    ///
    /// @param file_id     File identifier
    /// @param rg_id       Row group identifier
    /// @param batch_id    Batch identifier within row group
    /// @param schema      Full table schema (all columns)
    /// @param projection  Column indices to read (empty = all columns)
    /// @param selection   Optional row filter mask (BooleanArray)
    /// @return RecordBatch with projected columns and filtered rows, or nullptr
    std::shared_ptr<arrow::RecordBatch> read_batch(
            uint16_t file_id,
            uint16_t rg_id,
            uint16_t batch_id,
            const std::shared_ptr<arrow::Schema>& schema,
            const std::vector<int>& projection = {},
            const std::shared_ptr<arrow::BooleanArray>& selection = nullptr) const {
        std::lock_guard<std::mutex> lock(mutex_);

        // Determine which columns to read
        std::vector<int> cols_to_read;
        if (projection.empty()) {
            cols_to_read.resize(schema->num_fields());
            std::iota(cols_to_read.begin(), cols_to_read.end(), 0);
        } else {
            cols_to_read = projection;
        }

        // Read each projected column
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        arrays.reserve(cols_to_read.size());

        for (int col_idx : cols_to_read) {
            LiquidCacheKey key(file_id, rg_id,
                               static_cast<uint16_t>(col_idx), batch_id);
            auto arr = get_unlocked(key, selection);
            if (!arr) return nullptr;
            arrays.push_back(std::move(arr));
        }

        if (arrays.empty()) return nullptr;

        // Build projected schema
        arrow::FieldVector fields;
        fields.reserve(cols_to_read.size());
        for (int idx : cols_to_read) {
            fields.push_back(schema->field(idx));
        }
        auto proj_schema = arrow::schema(fields);

        // Extract length before moving arrays (C++ argument evaluation
        // order is unspecified — std::move(arrays) could execute first).
        int64_t num_rows = arrays[0]->length();
        return arrow::RecordBatch::Make(
            proj_schema, num_rows, std::move(arrays));
    }

    // ── Bulk load from Parquet ───────────────────────────────────────

    /// Metadata for a loaded row group: number of batches and rows.
    struct RowGroupInfo {
        uint16_t file_id;
        uint16_t rg_id;
        uint16_t num_batches;
        size_t total_rows;
    };

    /// Load Parquet files into the cache, transcoding each column to Liquid format.
    ///
    /// Returns metadata about each row group loaded, and sets transcode_sec
    /// to the total transcoding time.
    ///
    /// @param files          Parquet file paths
    /// @param schema         Output: table schema from first file
    /// @param transcode_sec  Output: total transcoding time in seconds
    /// @param transcode_fn   Function: Arrow array -> LiquidArrayRef
    /// @return Vector of RowGroupInfo for all loaded row groups
    std::vector<RowGroupInfo> load_from_parquet(
            const std::vector<std::string>& files,
            std::shared_ptr<arrow::Schema>& schema,
            double& transcode_sec,
            const std::function<LiquidArrayRef(
                const std::shared_ptr<arrow::Array>&)>& transcode_fn);

    // ── Statistics ───────────────────────────────────────────────────

    struct Stats {
        size_t entry_count = 0;
        size_t total_memory_bytes = 0;
        size_t liquid_entries = 0;
        size_t arrow_entries = 0;
    };

    Stats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Stats s;
        s.entry_count = entries_.size();
        for (const auto& [key, entry] : entries_) {
            s.total_memory_bytes += entry.memory_size();
            if (entry.type == CacheEntryType::MemoryLiquid) ++s.liquid_entries;
            else ++s.arrow_entries;
        }
        return s;
    }

    size_t entry_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

    size_t total_memory_size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t total = 0;
        for (const auto& [key, entry] : entries_) {
            total += entry.memory_size();
        }
        return total;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

private:
    /// Get without locking (caller must hold mutex_).
    std::shared_ptr<arrow::Array> get_unlocked(
            const LiquidCacheKey& key,
            const std::shared_ptr<arrow::BooleanArray>& selection) const {
        auto it = entries_.find(key);
        if (it == entries_.end()) return nullptr;
        return it->second.read(selection);
    }

    mutable std::mutex mutex_;
    std::unordered_map<LiquidCacheKey, CacheEntry, LiquidCacheKeyHash> entries_;
};

}  // namespace liquid_cache
