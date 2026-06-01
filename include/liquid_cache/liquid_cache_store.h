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
#include "liquid_cache/lru_policy.h"

#ifdef LIQUID_ENABLE_VELOX
namespace facebook::velox {
class RowType;
using RowTypePtr = std::shared_ptr<const RowType>;
}  // namespace facebook::velox
#endif

namespace liquid_cache {

// ═══════════════════════════════════════════════════════════════════════
// LiquidCacheKey: identifies a specific column batch in the cache.
// Equivalent to Rust's ParquetArrayID:
//   file_id(64) | rg_id(16) | col_id(16) | batch_id(16)
// ═══════════════════════════════════════════════════════════════════════

struct LiquidCacheKey {
    uint64_t file_id = 0;
    uint16_t rg_id = 0;      // row group id
    uint16_t col_id = 0;
    uint16_t batch_id = 0;

    LiquidCacheKey() = default;
    LiquidCacheKey(uint64_t f, uint16_t rg, uint16_t c, uint16_t b)
        : file_id(f), rg_id(rg), col_id(c), batch_id(b) {}

    bool operator==(const LiquidCacheKey& o) const {
        return file_id == o.file_id && rg_id == o.rg_id &&
               col_id == o.col_id && batch_id == o.batch_id;
    }
};

struct LiquidCacheKeyHash {
    size_t operator()(const LiquidCacheKey& k) const {
        size_t h = std::hash<uint64_t>{}(k.file_id);
        h ^= std::hash<uint16_t>{}(k.rg_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint16_t>{}(k.col_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint16_t>{}(k.batch_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// ═══════════════════════════════════════════════════════════════════════
// FileRgMetadata: per-file row group byte offsets for split→RG mapping.
// Populated during load_from_parquet, used by read_split_velox.
// ═══════════════════════════════════════════════════════════════════════

struct FileRgMetadata {
    std::vector<uint64_t> rg_offsets;     // file_offset of each row group
};

}  // namespace liquid_cache

// std::hash specialization for LiquidCacheKey — needed by LruPolicy
// (which uses std::unordered_map<LiquidCacheKey, ...> internally).
namespace std {
template <>
struct hash<liquid_cache::LiquidCacheKey> {
    size_t operator()(const liquid_cache::LiquidCacheKey& k) const noexcept {
        return liquid_cache::LiquidCacheKeyHash{}(k);
    }
};
}  // namespace std

namespace liquid_cache {

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
//   - LRU eviction with configurable memory budget
//   - Memory budget via atomic lock-free accounting
//   - Thread-safe via std::mutex
//   - Column projection via projection indices
//   - Row filtering via BooleanArray mask
// ═══════════════════════════════════════════════════════════════════════

class LiquidCacheStore {
public:
    LiquidCacheStore() = default;

    /// Create a cache store with a memory budget limit.
    /// @param max_cache_bytes Maximum memory in bytes (0 = unlimited, default).
    explicit LiquidCacheStore(size_t max_cache_bytes)
        : max_cache_bytes_(max_cache_bytes)
        , budget_(max_cache_bytes) {}

    /// Set the maximum cache memory budget.
    /// Setting to 0 disables the budget (unlimited).
    /// Does NOT evict existing entries — only affects future insertions.
    void set_max_cache_bytes(size_t max_bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        max_cache_bytes_ = max_bytes;
        budget_.set_max_bytes(max_bytes);
    }

    /// Current memory budget usage in bytes.
    size_t memory_budget_usage() const {
        return budget_.usage();
    }

    /// Maximum cache memory budget in bytes (0 = unlimited).
    size_t max_cache_bytes() const {
        return max_cache_bytes_;
    }

    // ── Insert operations ────────────────────────────────────────────

    /// Insert a Liquid-encoded array into the cache.
    /// If the memory budget is exceeded, evicts LRU entries to make room.
    /// @return true if inserted successfully, false if entry too large for budget.
    bool insert(const LiquidCacheKey& key, LiquidArrayRef array) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto entry = CacheEntry::from_liquid(std::move(array));
        size_t entry_size = entry.memory_size();

        auto existing = entries_.find(key);
        if (existing != entries_.end()) {
            // Update: evict if growing, then update budget
            size_t old_size = existing->second.memory_size();
            if (entry_size > old_size) {
                if (!make_budget_space(entry_size - old_size, lock)) return false;
            }
            if (!budget_.try_update(old_size, entry_size)) return false;
            entries_[key] = std::move(entry);
        } else {
            // New entry: first evict if needed, then reserve
            if (!make_budget_space(entry_size, lock)) return false;
            if (!budget_.try_reserve(entry_size)) return false;
            entries_[key] = std::move(entry);
        }

        lru_.notify_insert(key);
        return true;
    }

    /// Insert a raw Arrow array into the cache.
    /// If the memory budget is exceeded, evicts LRU entries to make room.
    /// @return true if inserted successfully, false if entry too large for budget.
    bool insert_arrow(const LiquidCacheKey& key,
                      std::shared_ptr<arrow::Array> array) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto entry = CacheEntry::from_arrow(std::move(array));
        size_t entry_size = entry.memory_size();

        auto existing = entries_.find(key);
        if (existing != entries_.end()) {
            // Update: evict if growing, then update budget
            size_t old_size = existing->second.memory_size();
            if (entry_size > old_size) {
                if (!make_budget_space(entry_size - old_size, lock)) return false;
            }
            if (!budget_.try_update(old_size, entry_size)) return false;
            entries_[key] = std::move(entry);
        } else {
            // New entry: first evict if needed, then reserve
            if (!make_budget_space(entry_size, lock)) return false;
            if (!budget_.try_reserve(entry_size)) return false;
            entries_[key] = std::move(entry);
        }

        lru_.notify_insert(key);
        return true;
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
            const std::shared_ptr<arrow::BooleanArray>& selection = nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = get_unlocked(key, selection);
        if (result) {
            lru_.notify_access(key);
        }
        return result;
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
            uint64_t file_id,
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
        uint64_t file_id;
        uint16_t rg_id;
        uint16_t num_batches;
        size_t total_rows;
    };

    /// Load Parquet files with customizable transcoding (original overload).
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

    /// Load Parquet files with automatic FSST compressor reuse.
    ///
    /// This overload internally manages per-column LiquidCompressorStates,
    /// mirroring Rust's pattern of training FSST once per column and reusing
    /// the compressor across all batches. This is the recommended path
    /// for workloads with string or large decimal columns.
    ///
    /// @param files          Parquet file paths
    /// @param schema         Output: table schema from first file
    /// @param transcode_sec  Output: total transcoding time in seconds
    /// @return Vector of RowGroupInfo for all loaded row groups
    std::vector<RowGroupInfo> load_from_parquet(
            const std::vector<std::string>& files,
            std::shared_ptr<arrow::Schema>& schema,
            double& transcode_sec);

    // ── Statistics ───────────────────────────────────────────────────

    struct Stats {
        size_t entry_count = 0;
        size_t total_memory_bytes = 0;
        size_t liquid_entries = 0;
        size_t arrow_entries = 0;
        size_t budget_usage_bytes = 0;
        size_t budget_max_bytes = 0;
    };

    Stats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Stats s;
        s.entry_count = entries_.size();
        for (const auto& [key, entry] : entries_) {
            size_t entrySize = entry.memory_size();
            s.total_memory_bytes += entrySize;
            if (entry.type == CacheEntryType::MemoryLiquid) ++s.liquid_entries;
            else ++s.arrow_entries;
        }
        s.budget_usage_bytes = budget_.usage();
        s.budget_max_bytes = budget_.max_bytes();
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
        budget_.reset();
        lru_.clear();
        file_metadata_.clear();
    }

    /// Number of entries tracked by the LRU policy.
    size_t lru_size() const {
        return lru_.size();
    }

#ifdef LIQUID_ENABLE_VELOX
    // ── Velox Vector read operations ─────────────────────────────────

    /// Load Parquet files and return Velox RowType (no Arrow types exposed).
    /// Internally calls load_from_parquet() with transcode_to_liquid_array
    /// and converts the Arrow Schema to Velox RowType.
    std::vector<RowGroupInfo> load_from_parquet_for_velox(
            const std::vector<std::string>& files,
            facebook::velox::RowTypePtr& veloxRowType,
            double& transcode_sec);

    /// Read a single cached column as a Velox Vector.
    facebook::velox::VectorPtr read_column_velox(
            const LiquidCacheKey& key,
            facebook::velox::memory::MemoryPool* pool) const;

    /// Read multiple columns as a Velox RowVector.
    ///
    /// @param file_id     File identifier
    /// @param rg_id       Row group identifier
    /// @param batch_id    Batch identifier within row group
    /// @param rowType     Velox RowType describing the full table schema
    /// @param pool        Velox memory pool for allocations
    /// @param projection  Column indices to read (empty = all columns)
    /// @return RowVector with projected columns, or nullptr
    facebook::velox::VectorPtr read_batch_velox(
            uint64_t file_id,
            uint16_t rg_id,
            uint16_t batch_id,
            const facebook::velox::RowTypePtr& rowType,
            facebook::velox::memory::MemoryPool* pool,
            const std::vector<int>& projection = {}) const;

    /// Read all row groups covered by a byte-range split (Velox).
    /// Maps (offset, length) → row group indices via file_metadata_ using
    /// Velox ParquetReader::filterRowGroups() semantics. Iterates all batches
    /// within each covered row group.
    /// Returns nullptr if any batch of any column is missing.
    facebook::velox::VectorPtr read_split_velox(
            uint64_t file_id,
            uint64_t offset,
            uint64_t length,
            const facebook::velox::RowTypePtr& rowType,
            facebook::velox::memory::MemoryPool* pool,
            const std::vector<int>& projection = {}) const;
#endif  // LIQUID_ENABLE_VELOX

private:
    /// Get without locking (caller must hold mutex_).
    std::shared_ptr<arrow::Array> get_unlocked(
            const LiquidCacheKey& key,
            const std::shared_ptr<arrow::BooleanArray>& selection) const {
        auto it = entries_.find(key);
        if (it == entries_.end()) return nullptr;
        return it->second.read(selection);
    }

    /// Ensure enough budget for a new entry of `needed_bytes`.
    /// Evicts LRU entries if necessary. Caller must hold mutex_.
    /// @param needed_bytes Size of the entry to be inserted.
    /// @param lock Reference to lock_guard (proof lock is held).
    /// @return true if budget was reserved, false if entry exceeds total budget.
    /// Evict LRU entries until enough space is freed for `needed_bytes`.
    /// Does NOT reserve the budget — the caller must call try_reserve() after.
    /// Caller must hold mutex_.
    /// @param needed_bytes Space needed in bytes.
    /// @param lock Reference to lock_guard.
    /// @return true if enough space was freed or already available.
    bool make_budget_space(size_t needed_bytes,
                           const std::lock_guard<std::mutex>& /*lock*/) {
        if (max_cache_bytes_ == 0) return true;
        if (needed_bytes > max_cache_bytes_) return false;

        // Evict until budget_.usage() + needed_bytes <= max_cache_bytes_
        const size_t max_attempts = 1024;
        for (size_t attempt = 0; attempt < max_attempts; ++attempt) {
            size_t used = budget_.usage();
            if (used + needed_bytes <= max_cache_bytes_) return true;

            auto victims = lru_.find_victims(8);
            if (victims.empty()) break;

            for (const auto& victim : victims) {
                auto it = entries_.find(victim);
                if (it != entries_.end()) {
                    size_t freed = it->second.memory_size();
                    entries_.erase(it);
                    budget_.release(freed);
                    if (budget_.usage() + needed_bytes <= max_cache_bytes_)
                        return true;
                }
            }
        }
        return budget_.usage() + needed_bytes <= max_cache_bytes_;
    }

    /// Map (offset, length) → row group indices via cached footer metadata.
    /// Uses Velox ParquetReader::filterRowGroups() semantics:
    ///   rowGroupInRange = (fileOffset >= offset && fileOffset < limit)
    std::vector<uint16_t> getRowGroupsForRange(
            uint64_t file_id, uint64_t offset, uint64_t length) const {
        auto it = file_metadata_.find(file_id);
        if (it == file_metadata_.end()) return {};
        uint64_t limit = offset + length;
        std::vector<uint16_t> rgs;
        for (size_t i = 0; i < it->second.rg_offsets.size(); ++i) {
            if (it->second.rg_offsets[i] >= offset &&
                it->second.rg_offsets[i] < limit) {
                rgs.push_back(static_cast<uint16_t>(i));
            }
        }
        return rgs;
    }

    mutable std::mutex mutex_;
    std::unordered_map<LiquidCacheKey, CacheEntry, LiquidCacheKeyHash> entries_;
    std::unordered_map<uint64_t, FileRgMetadata> file_metadata_;
    size_t max_cache_bytes_ = 0;
    MemoryBudget budget_;
    mutable LruPolicy<LiquidCacheKey> lru_;
};

}  // namespace liquid_cache
