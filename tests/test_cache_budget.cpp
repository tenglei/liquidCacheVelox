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

// ═══════════════════════════════════════════════════════════════════════
// Row group boundary detection tests
// Verifies that load_from_parquet correctly tracks actual Parquet
// row group boundaries via cumulative row counts from footer metadata.
// ═══════════════════════════════════════════════════════════════════════

#include <arrow/io/file.h>
#include <arrow/table.h>
#include <parquet/arrow/writer.h>
#include <parquet/arrow/reader.h>

TEST(CacheStore, MultiRowGroupBoundaryTracking) {
    // Create a small Arrow table that will be split into multiple row groups
    const int rows_per_rg = 50;
    const int num_rgs = 5;
    const int total_rows = rows_per_rg * num_rgs;

    arrow::Int32Builder i32_builder;
    arrow::DoubleBuilder f64_builder;
    for (int i = 0; i < total_rows; ++i) {
        ARROW_CHECK_OK(i32_builder.Append(i));
        ARROW_CHECK_OK(f64_builder.Append(static_cast<double>(i) * 1.5));
    }
    auto col0 = i32_builder.Finish().ValueOrDie();
    auto col1 = f64_builder.Finish().ValueOrDie();
    auto table = arrow::Table::Make(
        arrow::schema({arrow::field("c0", arrow::int32()),
                       arrow::field("c1", arrow::float64())}),
        {col0, col1});

    // Write to Parquet with explicit row groups for reliable multi-RG testing
    std::string tmp_path = "/tmp/test_multi_rg_boundary.parquet";
    {
        auto maybe_out = arrow::io::FileOutputStream::Open(tmp_path);
        ASSERT_TRUE(maybe_out.ok());
        auto out = maybe_out.ValueOrDie();

        auto writer_props = parquet::WriterProperties::Builder()
            .max_row_group_length(rows_per_rg)->build();
        auto arrow_props = parquet::ArrowWriterProperties::Builder().build();

        auto maybe_writer = parquet::arrow::FileWriter::Open(
            *table->schema(), arrow::default_memory_pool(),
            out, writer_props, arrow_props);
        ASSERT_TRUE(maybe_writer.ok());
        auto writer = std::move(maybe_writer).ValueOrDie();

        // Write each row group explicitly to guarantee multi-RG output
        for (int rg = 0; rg < num_rgs; ++rg) {
            int64_t start = rg * rows_per_rg;
            auto chunk = table->Slice(start, rows_per_rg);
            ARROW_CHECK_OK(writer->NewRowGroup(rows_per_rg));
            for (int c = 0; c < chunk->num_columns(); ++c) {
                ARROW_CHECK_OK(writer->WriteColumnChunk(
                    *chunk->column(c)->chunk(0)));
            }
        }
        ARROW_CHECK_OK(writer->Close());
        ARROW_CHECK_OK(out->Close());
    }

    // Verify the file has the expected number of row groups
    {
        auto maybe_in = arrow::io::ReadableFile::Open(tmp_path);
        ASSERT_TRUE(maybe_in.ok());
        std::unique_ptr<parquet::arrow::FileReader> reader;
        ASSERT_TRUE(parquet::arrow::OpenFile(
            maybe_in.ValueOrDie(), arrow::default_memory_pool(), &reader).ok());
        EXPECT_EQ(reader->num_row_groups(), num_rgs);
    }

    // Load via LiquidCacheStore and verify RowGroupInfo tracking
    LiquidCacheStore store;
    std::shared_ptr<arrow::Schema> schema;
    double transcode_sec = 0;

    auto rg_infos = store.load_from_parquet(
        {tmp_path}, schema, transcode_sec,
        [](const std::shared_ptr<arrow::Array>& arr) -> LiquidArrayRef {
            return nullptr;  // store as raw Arrow (simplest path)
        });

    // Clean up temp file
    std::remove(tmp_path.c_str());

    // Verify row group count
    ASSERT_EQ(rg_infos.size(), static_cast<size_t>(num_rgs))
        << "Should have " << num_rgs << " RowGroupInfo entries, one per RG";

    // Verify each RowGroupInfo has correct rg_id and row count
    for (int i = 0; i < num_rgs; ++i) {
        EXPECT_EQ(rg_infos[i].rg_id, static_cast<uint16_t>(i))
            << "RowGroupInfo[" << i << "] should have rg_id=" << i;
        EXPECT_EQ(rg_infos[i].total_rows, static_cast<size_t>(rows_per_rg))
            << "RowGroupInfo[" << i << "] should have " << rows_per_rg << " rows";
    }

    // Verify cache entries use correct rg_id in their keys
    // Each RG has 2 columns × some batches → entries with rg_id in ascending order
    auto s = store.stats();
    EXPECT_GT(s.entry_count, 0u) << "Should have cached entries";

    // Spot-check: entries for RG 0 must exist, entries for non-existent RG must not
    uint64_t fid = rg_infos[0].file_id;  // file_id from load_from_parquet (path hash)
    LiquidCacheKey k_rg0(fid, 0, 0, 0);
    LiquidCacheKey k_rg_last(fid, static_cast<uint16_t>(num_rgs - 1), 0, 0);
    LiquidCacheKey k_rg_bad(fid, static_cast<uint16_t>(num_rgs), 0, 0);
    EXPECT_TRUE(store.contains(k_rg0))
        << "RG 0 col 0 batch 0 should be cached";
    EXPECT_TRUE(store.contains(k_rg_last))
        << "Last RG col 0 batch 0 should be cached";
    EXPECT_FALSE(store.contains(k_rg_bad))
        << "Row group " << num_rgs << " should not exist";
}
