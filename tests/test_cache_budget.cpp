// test_cache_budget.cpp
// GoogleTest-based tests for MemoryBudget, LruPolicy,
// and LiquidCacheStore eviction.
#include <gtest/gtest.h>

#include <arrow/api.h>

#include <iostream>
#include <vector>
#include <memory>

#include "liquid_cache/liquid_cache_store.h"
#include "liquid_cache/lru_policy.h"

using namespace liquid_cache;

// ═══════════════════════════════════════════════════════════════════════
// Helper: build an Int32 Arrow array of given size.
// ═══════════════════════════════════════════════════════════════════════
static std::shared_ptr<arrow::Array> make_int32_array(int32_t count, int32_t base = 0) {
    arrow::Int32Builder builder;
    std::vector<int32_t> vals(count);
    for (int32_t i = 0; i < count; ++i) vals[i] = base + i;
    ARROW_CHECK_OK(builder.AppendValues(vals));
    return builder.Finish().ValueOrDie();
}

// ═══════════════════════════════════════════════════════════════════════
// MemoryBudget tests
// ═══════════════════════════════════════════════════════════════════════

TEST(MemoryBudget, BasicReservation) {
    MemoryBudget budget(1000);
    EXPECT_TRUE(budget.try_reserve(500));
    EXPECT_EQ(budget.usage(), 500u);
    EXPECT_TRUE(budget.try_reserve(300));
    EXPECT_EQ(budget.usage(), 800u);
}

TEST(MemoryBudget, ExceedsLimit) {
    MemoryBudget budget(1000);
    budget.try_reserve(800);
    EXPECT_FALSE(budget.try_reserve(300));
    EXPECT_EQ(budget.usage(), 800u);
}

TEST(MemoryBudget, TryUpdateGrowShrink) {
    MemoryBudget budget(1000);
    budget.try_reserve(500);
    // Grow within limit
    EXPECT_TRUE(budget.try_update(500, 700));
    EXPECT_EQ(budget.usage(), 700u);
    // Grow exceeded
    EXPECT_FALSE(budget.try_update(700, 1200));
    EXPECT_EQ(budget.usage(), 700u);
    // Shrink
    EXPECT_TRUE(budget.try_update(700, 300));
    EXPECT_EQ(budget.usage(), 300u);
}

TEST(MemoryBudget, Release) {
    MemoryBudget budget(1000);
    budget.try_reserve(600);
    budget.release(200);
    EXPECT_EQ(budget.usage(), 400u);
}

TEST(MemoryBudget, Unlimited) {
    MemoryBudget budget(0); // max_bytes=0 means unlimited
    EXPECT_TRUE(budget.try_reserve(1000000));
    EXPECT_EQ(budget.usage(), 1000000u);
}

// ═══════════════════════════════════════════════════════════════════════
// LruPolicy tests
// ═══════════════════════════════════════════════════════════════════════

TEST(LruPolicy, InsertionOrderEviction) {
    LruPolicy<int> lru;
    lru.notify_insert(1);
    lru.notify_insert(2);
    lru.notify_insert(3);

    auto victims = lru.find_victims(2);
    ASSERT_EQ(victims.size(), 2u);
    EXPECT_EQ(victims[0], 1);
    EXPECT_EQ(victims[1], 2);
    EXPECT_EQ(lru.size(), 1u);
}

TEST(LruPolicy, AccessMovesToFront) {
    LruPolicy<int> lru;
    lru.notify_insert(1);
    lru.notify_insert(2);
    lru.notify_insert(3);
    lru.notify_access(1);  // 1 becomes MRU, order: 1,3,2

    auto victims = lru.find_victims(1);
    ASSERT_EQ(victims.size(), 1u);
    EXPECT_EQ(victims[0], 2);
}

TEST(LruPolicy, ReinsertMovesToFront) {
    LruPolicy<int> lru;
    lru.notify_insert(1);
    lru.notify_insert(2);
    lru.notify_insert(3);
    lru.notify_insert(1);  // re-insert 1 -> MRU

    auto victims = lru.find_victims(1);
    ASSERT_EQ(victims.size(), 1u);
    EXPECT_EQ(victims[0], 2);
}

TEST(LruPolicy, FindVictimsOnEmpty) {
    LruPolicy<int> lru;
    auto victims = lru.find_victims(5);
    EXPECT_TRUE(victims.empty());
}

// ═══════════════════════════════════════════════════════════════════════
// LiquidCacheStore budget + LRU integration tests
// ═══════════════════════════════════════════════════════════════════════

TEST(CacheStore, InsertRespectsBudget) {
    LiquidCacheStore store(1000000);  // 1MB budget
    auto arr = make_int32_array(100);

    LiquidCacheKey k1(0, 0, 0, 0);
    LiquidCacheKey k2(0, 0, 1, 0);

    EXPECT_TRUE(store.insert_arrow(k1, arr));
    EXPECT_TRUE(store.insert_arrow(k2, arr));
    EXPECT_TRUE(store.contains(k1));
    EXPECT_TRUE(store.contains(k2));
}

TEST(CacheStore, EvictionWhenBudgetExceeded) {
    auto arr = make_int32_array(1000);
    size_t entry_size = arr->data()->buffers[1]->size()
                      + (arr->data()->buffers[0] ? arr->data()->buffers[0]->size() : 0);

    LiquidCacheStore store(entry_size * 2 + 256);
    LiquidCacheKey k1(1, 0, 0, 0);
    LiquidCacheKey k2(1, 0, 1, 0);
    LiquidCacheKey k3(1, 0, 2, 0);

    store.insert_arrow(k1, arr);
    store.insert_arrow(k2, arr);

    if (store.contains(k1) && store.contains(k2)) {
        bool ok3 = store.insert_arrow(k3, arr);
        EXPECT_TRUE(ok3);
        EXPECT_TRUE(store.contains(k3));
        EXPECT_FALSE(store.contains(k1))
            << "Oldest entry k1 should have been evicted";
    }
}

TEST(CacheStore, GetNotifiesLRU) {
    // Use Int8 arrays for smaller memory footprint
    arrow::Int8Builder builder;
    std::vector<int8_t> vals(256, 1);
    ARROW_CHECK_OK(builder.AppendValues(vals));
    auto arr = builder.Finish().ValueOrDie();

    size_t entry_size = arr->data()->buffers[1]->size()
                      + (arr->data()->buffers[0] ? arr->data()->buffers[0]->size() : 0);

    LiquidCacheStore store(entry_size * 2 + 32);
    LiquidCacheKey k1(2, 0, 0, 0);
    LiquidCacheKey k2(2, 0, 1, 0);
    LiquidCacheKey k3(2, 0, 2, 0);

    if (store.insert_arrow(k1, arr) && store.insert_arrow(k2, arr)) {
        // Access k1 to promote it to MRU
        store.get(k1);
        store.insert_arrow(k3, arr);

        EXPECT_TRUE(store.contains(k1))
            << "k1 should survive (accessed, became MRU)";
        EXPECT_FALSE(store.contains(k2))
            << "k2 should be evicted (LRU)";
        EXPECT_TRUE(store.contains(k3));
    }
}

TEST(CacheStore, EntryTooLargeForBudget) {
    LiquidCacheStore store(100);
    auto arr = make_int32_array(10000);

    LiquidCacheKey k1(3, 0, 0, 0);
    EXPECT_FALSE(store.insert_arrow(k1, arr));
    EXPECT_FALSE(store.contains(k1));
}

TEST(CacheStore, UpdateEntry) {
    LiquidCacheStore store(1000000);
    LiquidCacheKey k1(4, 0, 0, 0);

    auto arr1 = make_int32_array(100);
    auto arr2 = make_int32_array(200);

    store.insert_arrow(k1, arr1);
    size_t size1 = store.total_memory_size();
    store.insert_arrow(k1, arr2);
    size_t size2 = store.total_memory_size();

    EXPECT_GT(size2, size1);
    auto got = store.get(k1);
    EXPECT_TRUE(got != nullptr);
    EXPECT_EQ(got->length(), 200);
    EXPECT_EQ(store.lru_size(), 1u);
}

TEST(CacheStore, StatsIncludeBudget) {
    LiquidCacheStore store(500000);
    auto arr = make_int32_array(500);
    LiquidCacheKey k1(5, 0, 0, 0);
    store.insert_arrow(k1, arr);

    auto s = store.stats();
    EXPECT_EQ(s.budget_max_bytes, 500000u);
    EXPECT_GT(s.budget_usage_bytes, 0u);
    EXPECT_EQ(s.entry_count, 1u);
    EXPECT_EQ(s.arrow_entries, 1u);
}

TEST(CacheStore, UnlimitedBudgetDefault) {
    LiquidCacheStore store;
    auto arr = make_int32_array(100000);
    LiquidCacheKey k1(6, 0, 0, 0);
    EXPECT_TRUE(store.insert_arrow(k1, arr));
    EXPECT_TRUE(store.contains(k1));
    EXPECT_EQ(store.max_cache_bytes(), 0u);
}

TEST(CacheStore, SetMaxCacheBytes) {
    LiquidCacheStore store(1000000);
    store.set_max_cache_bytes(500);
    EXPECT_EQ(store.max_cache_bytes(), 500u);
}

TEST(CacheStore, ClearResetsBudgetAndLRU) {
    LiquidCacheStore store(1000000);
    auto arr = make_int32_array(100);
    LiquidCacheKey k1(7, 0, 0, 0);
    store.insert_arrow(k1, arr);
    store.clear();

    EXPECT_EQ(store.entry_count(), 0u);
    EXPECT_EQ(store.memory_budget_usage(), 0u);
    EXPECT_EQ(store.lru_size(), 0u);
}

TEST(CacheStore, MultipleEvictionsNeeded) {
    auto arr = make_int32_array(1000);
    size_t entry_size = arr->data()->buffers[1]->size()
                      + (arr->data()->buffers[0] ? arr->data()->buffers[0]->size() : 0);

    LiquidCacheStore store(entry_size + 200);
    LiquidCacheKey k1(9, 0, 0, 0);
    LiquidCacheKey k2(9, 0, 1, 0);
    LiquidCacheKey k3(9, 0, 2, 0);
    LiquidCacheKey k4(9, 0, 3, 0);

    store.insert_arrow(k1, arr);
    store.insert_arrow(k2, arr);
    store.insert_arrow(k3, arr);
    store.insert_arrow(k4, arr);

    EXPECT_FALSE(store.contains(k1));
    EXPECT_FALSE(store.contains(k2));
    EXPECT_FALSE(store.contains(k3));
    EXPECT_TRUE(store.contains(k4));
    EXPECT_EQ(store.lru_size(), 1u);
}

TEST(CacheStore, SameKeyReinsert) {
    LiquidCacheStore store(1000000);
    LiquidCacheKey k1(10, 0, 0, 0);

    auto arr1 = make_int32_array(100, 0);
    auto arr2 = make_int32_array(100, 500);

    store.insert_arrow(k1, arr1);
    store.insert_arrow(k1, arr2);  // same key, different data

    // Only one entry should exist
    EXPECT_EQ(store.lru_size(), 1u);
    EXPECT_EQ(store.entry_count(), 1u);

    // The retrieved data should be the latest one
    auto got = store.get(k1);
    ASSERT_NE(got, nullptr);
    auto typed = std::static_pointer_cast<arrow::Int32Array>(got);
    EXPECT_EQ(typed->Value(0), 500);
}
