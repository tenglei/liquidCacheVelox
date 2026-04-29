// liquid_cache/lru_policy.h
// Memory budget accounting and LRU eviction policy.
//
// Equivalent to Rust's:
//   - src/core/src/cache/budget.rs            (BudgetAccounting)
//   - src/core/src/cache/policies/cache/lru.rs (LruPolicy)
//
// Thread-safe memory budget with lock-free atomic operations,
// and a classic LRU (Least Recently Used) eviction policy
// implemented with std::list + std::unordered_map.
#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>


namespace liquid_cache {

// ═══════════════════════════════════════════════════════════════════════
// MemoryBudget: thread-safe, lock-free memory budget tracker.
//
// Uses std::atomic<size_t> with compare_exchange_weak for lock-free
// reservation. Thread-safe for concurrent reservations.
// ═══════════════════════════════════════════════════════════════════════

class MemoryBudget {
public:
    explicit MemoryBudget(size_t max_bytes = 0)
        : max_bytes_(max_bytes), used_bytes_(0) {}

    /// Maximum memory allowed (0 = unlimited).
    size_t max_bytes() const { return max_bytes_; }
    void set_max_bytes(size_t max) { max_bytes_ = max; }

    /// Current memory usage in bytes.
    size_t usage() const {
        return used_bytes_.load(std::memory_order_relaxed);
    }

    /// Reset usage to zero.
    void reset() {
        used_bytes_.store(0, std::memory_order_relaxed);
    }

    /// Try to reserve `request_bytes` of memory atomically.
    /// Returns true if the reservation succeeded, false if budget exceeded.
    /// Equivalent to Rust: try_reserve_memory(request_bytes)
    bool try_reserve(size_t request_bytes) {
        // Unlimited budget
        if (max_bytes_ == 0) {
            used_bytes_.fetch_add(request_bytes, std::memory_order_relaxed);
            return true;
        }

        size_t used = used_bytes_.load(std::memory_order_relaxed);
        while (true) {
            if (used + request_bytes > max_bytes_) {
                return false;
            }
            if (used_bytes_.compare_exchange_weak(
                    used, used + request_bytes,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return true;
            }
            // CAS failed — `used` is reloaded with current value, retry
        }
    }

    /// Release `bytes` of previously reserved memory.
    void release(size_t bytes) {
        used_bytes_.fetch_sub(bytes, std::memory_order_relaxed);
    }

    /// Atomically update memory usage from old_size to new_size.
    /// If new_size > old_size, tries to reserve the difference.
    /// If new_size < old_size, releases the difference.
    /// Returns true if the update succeeded, false if budget would be exceeded.
    /// Equivalent to Rust: try_update_memory_usage(old_size, new_size)
    bool try_update(size_t old_size, size_t new_size) {
        if (old_size < new_size) {
            return try_reserve(new_size - old_size);
        } else if (old_size > new_size) {
            release(old_size - new_size);
        }
        return true;
    }

private:
    size_t max_bytes_;
    std::atomic<size_t> used_bytes_;
};

// ═══════════════════════════════════════════════════════════════════════
// LruPolicy<Key>: classic LRU eviction policy.
//
// Maintains entries in order of most-recently-used (front) to
// least-recently-used (back). On eviction, entries at the back
// (least recently used) are selected first.
//
// Template parameter:
//   Key - The cache key type (must be hashable and equality-comparable).
//
// Equivalent to Rust: LruPolicy (HashMap + DoublyLinkedList)
// ═══════════════════════════════════════════════════════════════════════

template <typename Key>
class LruPolicy {
public:
    LruPolicy() = default;

    /// Notify the policy that a key was just inserted (or re-inserted).
    /// If the key already exists, it is moved to the front (MRU position).
    void notify_insert(const Key& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            // Key exists — move to front (promote to MRU)
            list_.splice(list_.begin(), list_, it->second);
            it->second = list_.begin();
            return;
        }
        // New key — insert at front
        list_.push_front(key);
        map_[key] = list_.begin();
    }

    /// Notify the policy that a key was accessed.
    /// Moves the key to the front (MRU position) if it exists.
    void notify_access(const Key& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            list_.splice(list_.begin(), list_, it->second);
            it->second = list_.begin();
        }
    }

    /// Find up to `count` victim keys to evict.
    /// Victims are selected from the back (LRU position).
    /// Returns the list of victims (may be fewer than `count` if cache is small).
    std::vector<Key> find_victims(size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Key> victims;
        victims.reserve(count);

        for (size_t i = 0; i < count && !list_.empty(); ++i) {
            Key key = list_.back();
            list_.pop_back();
            map_.erase(key);
            victims.push_back(std::move(key));
        }

        return victims;
    }

    /// Remove a specific key from the policy (e.g., after eviction).
    void remove(const Key& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            list_.erase(it->second);
            map_.erase(it);
        }
    }

    /// Number of tracked entries.
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return list_.size();
    }

    /// Clear all entries.
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        list_.clear();
        map_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::list<Key> list_;  // front = MRU, back = LRU
    std::unordered_map<Key, typename std::list<Key>::iterator> map_;
};

}  // namespace liquid_cache
