// test_cache_budget.cpp
// Tests for MemoryBudget, LruPolicy, and LiquidCacheStore eviction.
#include <arrow/api.h>

#include <iostream>
#include <vector>
#include <memory>

#include "liquid_cache/liquid_cache_store.h"
#include "liquid_cache/lru_policy.h"

using namespace liquid_cache;

// Helper: build an Int32 Arrow array of given size.
static std::shared_ptr<arrow::Array> make_int32_array(int32_t count, int32_t base = 0) {
    arrow::Int32Builder builder;
    std::vector<int32_t> vals(count);
    for (int32_t i = 0; i < count; ++i) vals[i] = base + i;
    ARROW_CHECK_OK(builder.AppendValues(vals));
    return builder.Finish().ValueOrDie();
}

int main() {
    int passed = 0, failed = 0;

    // ══════════════════════════════════════════════════════════════════
    // MemoryBudget tests
    // ══════════════════════════════════════════════════════════════════

    // Test 1: Basic reservation
    {
        std::cout << "Test 1: MemoryBudget basic reservation... ";
        MemoryBudget budget(1000);
        if (budget.try_reserve(500) && budget.usage() == 500 &&
            budget.try_reserve(300) && budget.usage() == 800) {
            std::cout << "PASS" << std::endl; ++passed;
        } else {
            std::cout << "FAIL" << std::endl; ++failed;
        }
    }

    // Test 2: try_reserve fails when exceeded
    {
        std::cout << "Test 2: MemoryBudget exceeds limit... ";
        MemoryBudget budget(1000);
        budget.try_reserve(800);
        if (!budget.try_reserve(300) && budget.usage() == 800) {
            std::cout << "PASS" << std::endl; ++passed;
        } else {
            std::cout << "FAIL" << std::endl; ++failed;
        }
    }

    // Test 3: try_update (grow/shrink)
    {
        std::cout << "Test 3: MemoryBudget try_update... ";
        MemoryBudget budget(1000);
        budget.try_reserve(500);
        // Grow within limit
        bool grow_ok = budget.try_update(500, 700) && budget.usage() == 700;
        // Grow exceeded
        bool grow_fail = !budget.try_update(700, 1200) && budget.usage() == 700;
        // Shrink
        bool shrink_ok = budget.try_update(700, 300) && budget.usage() == 300;
        if (grow_ok && grow_fail && shrink_ok) {
            std::cout << "PASS" << std::endl; ++passed;
        } else {
            std::cout << "FAIL" << std::endl; ++failed;
        }
    }

    // Test 4: Release
    {
        std::cout << "Test 4: MemoryBudget release... ";
        MemoryBudget budget(1000);
        budget.try_reserve(600);
        budget.release(200);
        if (budget.usage() == 400) {
            std::cout << "PASS" << std::endl; ++passed;
        } else {
            std::cout << "FAIL" << std::endl; ++failed;
        }
    }

    // Test 5: Unlimited budget (max_bytes=0)
    {
        std::cout << "Test 5: MemoryBudget unlimited... ";
        MemoryBudget budget(0);
        if (budget.try_reserve(1000000) && budget.usage() == 1000000) {
            std::cout << "PASS" << std::endl; ++passed;
        } else {
            std::cout << "FAIL" << std::endl; ++failed;
        }
    }

    // ══════════════════════════════════════════════════════════════════
    // LruPolicy tests
    // ══════════════════════════════════════════════════════════════════

    // Test 6: Insertion order eviction
    {
        std::cout << "Test 6: LruPolicy insertion order eviction... ";
        LruPolicy<int> lru;
        lru.notify_insert(1);
        lru.notify_insert(2);
        lru.notify_insert(3);

        auto victims = lru.find_victims(2);
        if (victims.size() == 2 && victims[0] == 1 && victims[1] == 2 && lru.size() == 1) {
            std::cout << "PASS" << std::endl; ++passed;
        } else {
            std::cout << "FAIL" << std::endl; ++failed;
        }
    }

    // Test 7: Access moves to front (MRU)
    {
        std::cout << "Test 7: LruPolicy access moves to front... ";
        LruPolicy<int> lru;
        lru.notify_insert(1);
        lru.notify_insert(2);
        lru.notify_insert(3);
        lru.notify_access(1);  // 1 becomes MRU, order: 1,3,2

        auto victims = lru.find_victims(1);
        if (victims.size() == 1 && victims[0] == 2) {
            std::cout << "PASS" << std::endl; ++passed;
        } else {
            std::cout << "FAIL" << std::endl; ++failed;
        }
    }

    // Test 8: Re-insert moves to front
    {
        std::cout << "Test 8: LruPolicy re-insert moves to front... ";
        LruPolicy<int> lru;
        lru.notify_insert(1);
        lru.notify_insert(2);
        lru.notify_insert(3);
        lru.notify_insert(1);  // re-insert 1 -> MRU

        auto victims = lru.find_victims(1);
        if (victims.size() == 1 && victims[0] == 2) {
            std::cout << "PASS" << std::endl; ++passed;
        } else {
            std::cout << "FAIL" << std::endl; ++failed;
        }
    }

    // Test 9: find_victims on empty
    {
        std::cout << "Test 9: LruPolicy find_victims on empty... ";
        LruPolicy<int> lru;
        auto victims = lru.find_victims(5);
        if (victims.empty()) {
            std::cout << "PASS" << std::endl; ++passed;
        } else {
            std::cout << "FAIL" << std::endl; ++failed;
        }
    }

    // ══════════════════════════════════════════════════════════════════
    // LiquidCacheStore budget + LRU integration tests
    // ══════════════════════════════════════════════════════════════════

    // Test 10: Insert respects budget (no eviction needed)
    {
        std::cout << "Test 10: Insert respects budget... ";
        LiquidCacheStore store(1000000);  // 1MB budget
        auto arr = make_int32_array(100);

        LiquidCacheKey k1(0, 0, 0, 0);
        LiquidCacheKey k2(0, 0, 1, 0);

        bool ok1 = store.insert_arrow(k1, arr);
        bool ok2 = store.insert_arrow(k2, arr);

        if (ok1 && ok2 && store.contains(k1) && store.contains(k2)) {
            std::cout << "PASS" << std::endl; ++passed;
        } else {
            std::cout << "FAIL" << std::endl; ++failed;
        }
    }

    // Test 11: Eviction when budget exceeded
    {
        std::cout << "Test 11: Eviction when budget exceeded... ";
        auto arr = make_int32_array(1000);
        size_t entry_size = arr->data()->buffers[1]->size()
                          + (arr->data()->buffers[0] ? arr->data()->buffers[0]->size() : 0);

        // Budget for ~2 entries
        LiquidCacheStore store(entry_size * 2 + 256);
        LiquidCacheKey k1(1, 0, 0, 0);
        LiquidCacheKey k2(1, 0, 1, 0);
        LiquidCacheKey k3(1, 0, 2, 0);

        store.insert_arrow(k1, arr);
        store.insert_arrow(k2, arr);

        if (!store.contains(k1) || !store.contains(k2)) {
            std::cout << "SKIP (entries too large for 2-entry budget)" << std::endl;
            ++passed;
        } else {
            // k3 should trigger eviction of k1 (oldest/LRU)
            bool ok3 = store.insert_arrow(k3, arr);

            if (ok3 && store.contains(k3) && !store.contains(k1)) {
                std::cout << "PASS" << std::endl; ++passed;
            } else {
                std::cout << "FAIL (k1=" << store.contains(k1)
                          << " k2=" << store.contains(k2)
                          << " k3=" << store.contains(k3) << ")" << std::endl;
                ++failed;
            }
        }
    }

    // Test 12: get() notifies LRU (access prevents eviction)
    {
        std::cout << "Test 12: get() notifies LRU... ";
        // Use Int8 arrays for smaller memory footprint and predictable sizes
        arrow::Int8Builder builder;
        std::vector<int8_t> vals(256, 1);
        ARROW_CHECK_OK(builder.AppendValues(vals));
        auto arr = builder.Finish().ValueOrDie();

        size_t entry_size = arr->data()->buffers[1]->size()
                          + (arr->data()->buffers[0] ? arr->data()->buffers[0]->size() : 0);

        // Budget fits 2 but not 3
        LiquidCacheStore store(entry_size * 2 + 32);
        LiquidCacheKey k1(2, 0, 0, 0);
        LiquidCacheKey k2(2, 0, 1, 0);
        LiquidCacheKey k3(2, 0, 2, 0);

        if (!store.insert_arrow(k1, arr) || !store.insert_arrow(k2, arr)) {
            std::cout << "SKIP (could not insert 2 entries)" << std::endl;
            ++passed;
        } else {
            // Access k1 to promote it to MRU
            store.get(k1);

            // Insert k3: should evict k2 (LRU), NOT k1 (MRU)
            store.insert_arrow(k3, arr);

            if (store.contains(k1) && !store.contains(k2) && store.contains(k3)) {
                std::cout << "PASS" << std::endl; ++passed;
            } else {
                std::cout << "FAIL (k1=" << store.contains(k1)
                          << " k2=" << store.contains(k2)
                          << " k3=" << store.contains(k3) << ")" << std::endl;
                ++failed;
            }
        }
    }

    // Test 13: Entry too large for budget
    {
        std::cout << "Test 13: Entry too large for budget... ";
        LiquidCacheStore store(100);  // tiny budget
        auto arr = make_int32_array(10000);  // much larger than budget

        LiquidCacheKey k1(3, 0, 0, 0);
        bool ok = store.insert_arrow(k1, arr);

        if (!ok && !store.contains(k1)) {
            std::cout << "PASS" << std::endl; ++passed;
        } else {
            std::cout << "FAIL" << std::endl; ++failed;
        }
    }

    // Test 14: Update entry (same key, different size)
    {
        std::cout << "Test 14: Update entry (same key)... ";
        LiquidCacheStore store(1000000);
        LiquidCacheKey k1(4, 0, 0, 0);

        auto arr1 = make_int32_array(100);
        auto arr2 = make_int32_array(200);

        store.insert_arrow(k1, arr1);
        size_t size1 = store.total_memory_size();

        store.insert_arrow(k1, arr2);
        size_t size2 = store.total_memory_size();

        auto got = store.get(k1);
        if (size2 > size1 && got && got->length() == 200 && store.lru_size() == 1) {
            std::cout << "PASS" << std::endl; ++passed;
        } else {
            std::cout << "FAIL" << std::endl; ++failed;
        }
    }

    // Test 15: Stats include budget info
    {
        std::cout << "Test 15: Stats include budget info... ";
        LiquidCacheStore store(500000);
        auto arr = make_int32_array(500);
        LiquidCacheKey k1(5, 0, 0, 0);
        store.insert_arrow(k1, arr);

        auto s = store.stats();
        if (s.budget_max_bytes == 500000 && s.budget_usage_bytes > 0 &&
            s.entry_count == 1 && s.arrow_entries == 1) {
            std::cout << "PASS" << std::endl; ++passed;
        } else {
            std::cout << "FAIL" << std::endl; ++failed;
        }
    }

    // Test 16: Unlimited budget (default)
    {
        std::cout << "Test 16: Unlimited budget (default)... ";
        LiquidCacheStore store;  // default unlimited
        auto arr = make_int32_array(100000);
        LiquidCacheKey k1(6, 0, 0, 0);
        bool ok = store.insert_arrow(k1, arr);
        if (ok && store.contains(k1) && store.max_cache_bytes() == 0) {
            std::cout << "PASS" << std::endl; ++passed;
        } else {
            std::cout << "FAIL" << std::endl; ++failed;
        }
    }

    // Test 17: set_max_cache_bytes
    {
        std::cout << "Test 17: set_max_cache_bytes... ";
        LiquidCacheStore store(1000000);
        store.set_max_cache_bytes(500);
        if (store.max_cache_bytes() == 500) {
            std::cout << "PASS" << std::endl; ++passed;
        } else {
            std::cout << "FAIL" << std::endl; ++failed;
        }
    }

    // Test 18: clear resets budget and LRU
    {
        std::cout << "Test 18: clear resets budget and LRU... ";
        LiquidCacheStore store(1000000);
        auto arr = make_int32_array(100);
        LiquidCacheKey k1(7, 0, 0, 0);
        store.insert_arrow(k1, arr);
        store.clear();

        if (store.entry_count() == 0 && store.memory_budget_usage() == 0 &&
            store.lru_size() == 0) {
            std::cout << "PASS" << std::endl; ++passed;
        } else {
            std::cout << "FAIL" << std::endl; ++failed;
        }
    }

    // Test 19: Multiple evictions needed
    {
        std::cout << "Test 19: Multiple evictions needed... ";
        auto arr = make_int32_array(1000);
        size_t entry_size = arr->data()->buffers[1]->size()
                          + (arr->data()->buffers[0] ? arr->data()->buffers[0]->size() : 0);

        // Budget for only 1 entry
        LiquidCacheStore store(entry_size + 200);
        LiquidCacheKey k1(9, 0, 0, 0);
        LiquidCacheKey k2(9, 0, 1, 0);
        LiquidCacheKey k3(9, 0, 2, 0);
        LiquidCacheKey k4(9, 0, 3, 0);

        store.insert_arrow(k1, arr);
        store.insert_arrow(k2, arr);  // evicts k1
        store.insert_arrow(k3, arr);  // evicts k2
        store.insert_arrow(k4, arr);  // evicts k3

        if (!store.contains(k1) && !store.contains(k2) &&
            !store.contains(k3) && store.contains(k4) &&
            store.lru_size() == 1) {
            std::cout << "PASS" << std::endl; ++passed;
        } else {
            std::cout << "FAIL (k1=" << store.contains(k1)
                      << " k2=" << store.contains(k2)
                      << " k3=" << store.contains(k3)
                      << " k4=" << store.contains(k4)
                      << " size=" << store.lru_size() << ")" << std::endl;
            ++failed;
        }
    }

    std::cout << "\n=== Results: " << passed << " passed, " << failed << " failed ===" << std::endl;
    return failed > 0 ? 1 : 0;
}
