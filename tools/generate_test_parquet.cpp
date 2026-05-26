// generate_test_parquet.cpp
// Generate a test Parquet file with all supported Liquid Cache field types.
//
// Usage:
//   generate_test_parquet                   → default ~512MB file
//   generate_test_parquet <output_path>     → custom output path
//   generate_test_parquet <output_path> <total_rows>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>
#include <arrow/compute/initialize.h>

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#define CHECK_OK(expr)                                          \
    do {                                                        \
        auto _s = (expr);                                       \
        if (!_s.ok()) {                                         \
            std::cerr << "FAILED at " << __FILE__ << ":"        \
                      << __LINE__ << ": " << _s.ToString()      \
                      << "\n";                                  \
            return 1;                                           \
        }                                                       \
    } while (false)

int main(int argc, char* argv[]) {
    CHECK_OK(arrow::compute::Initialize());

    // ── Configuration ────────────────────────────────────────────────
    // 5M rows × ~20 cols × ~100B/row (compressed) ≈ 512MB Parquet
    int64_t total_rows = 5'000'000;
    int64_t batch_size = 100'000;
    unsigned int seed = 42;
    std::string output_path =
        "/home/tenglei/code/liquid-cache-cpp/build/test_data_512mb.parquet";

    if (argc >= 2) output_path = argv[1];
    if (argc >= 3) total_rows = std::stoll(argv[2]);
    if (argc >= 4) seed = static_cast<unsigned int>(std::stoul(argv[3]));

    // ── Random generators ────────────────────────────────────────────
    std::mt19937 rng(seed);

    const std::vector<std::string> categories = {
        "electronics", "clothing", "food", "books", "sports",
        "home", "garden", "automotive", "toys", "health"
    };
    const std::vector<std::string> first_names = {
        "Alice", "Bob", "Charlie", "Diana", "Eve", "Frank", "Grace",
        "Hank", "Ivy", "Jack", "Kate", "Leo", "Mia", "Noah", "Olivia",
        "Paul", "Quinn", "Rose", "Sam", "Tina", "Uma", "Vince", "Wendy",
        "Xavier", "Yara", "Zack", "Anna", "Ben", "Cara", "David"
    };

    auto now_us = std::chrono::time_point_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now());
    int64_t base_ts = now_us.time_since_epoch().count();

    // ── Schema: all supported Liquid Cache types ─────────────────────
    auto schema = arrow::schema({
        // Integer types (4 signed + 4 unsigned = 8 columns)
        arrow::field("col_int8",    arrow::int8()),
        arrow::field("col_int16",   arrow::int16()),
        arrow::field("col_int32",   arrow::int32()),
        arrow::field("col_int64",   arrow::int64()),
        arrow::field("col_uint8",   arrow::uint8()),
        arrow::field("col_uint16",  arrow::uint16()),
        arrow::field("col_uint32",  arrow::uint32()),
        arrow::field("col_uint64",  arrow::uint64()),
        // Float types (2 columns)
        arrow::field("col_float32", arrow::float32()),
        arrow::field("col_float64", arrow::float64()),
        // Date types (2 columns)
        arrow::field("col_date32",  arrow::date32()),
        arrow::field("col_date64",  arrow::date64()),
        // Timestamp types, all 4 units (4 columns)
        arrow::field("col_ts_s",    arrow::timestamp(arrow::TimeUnit::SECOND)),
        arrow::field("col_ts_ms",   arrow::timestamp(arrow::TimeUnit::MILLI)),
        arrow::field("col_ts_us",   arrow::timestamp(arrow::TimeUnit::MICRO)),
        arrow::field("col_ts_ns",   arrow::timestamp(arrow::TimeUnit::NANO)),
        // String types: high-cardinality + low-cardinality (2 columns)
        arrow::field("col_string_high", arrow::utf8()),
        arrow::field("col_string_low",  arrow::utf8()),
        // Binary type (1 column)
        arrow::field("col_binary",  arrow::binary()),
        // Decimal128 type (1 column)
        arrow::field("col_decimal", arrow::decimal128(10, 2)),
    });

    // ── Open Parquet FileWriter (streaming, memory-efficient) ────────
    auto maybe_outfile = arrow::io::FileOutputStream::Open(output_path);
    if (!maybe_outfile.ok()) {
        std::cerr << "Cannot open output: " << maybe_outfile.status() << "\n";
        return 1;
    }
    auto outfile = maybe_outfile.ValueOrDie();

    auto writer_result = parquet::arrow::FileWriter::Open(
        *schema, arrow::default_memory_pool(), outfile,
        parquet::WriterProperties::Builder()
            .compression(arrow::Compression::SNAPPY)
            ->build(),
        parquet::ArrowWriterProperties::Builder().build());
    if (!writer_result.ok()) {
        std::cerr << "Cannot create writer: " << writer_result.status() << "\n";
        return 1;
    }
    auto writer = std::move(writer_result).ValueOrDie();

    // ── Distributions ────────────────────────────────────────────────
    std::uniform_int_distribution<int>      dist_i8(-100, 100);
    std::uniform_int_distribution<int>      dist_i16(-10000, 10000);
    std::uniform_int_distribution<int32_t>  dist_i32(-1000000, 1000000);
    std::uniform_int_distribution<int64_t>  dist_i64(-100000000LL, 100000000LL);
    std::uniform_int_distribution<int>      dist_u8(0, 200);
    std::uniform_int_distribution<int>      dist_u16(0, 50000);
    std::uniform_int_distribution<uint32_t> dist_u32(0, 2000000);
    std::uniform_int_distribution<uint64_t> dist_u64(0, 200000000ULL);
    std::uniform_real_distribution<float>   dist_f32(-1e6f, 1e6f);
    std::uniform_real_distribution<double>  dist_f64(-1e12, 1e12);
    std::uniform_int_distribution<int32_t>  dist_date32(0, 20000);
    std::uniform_int_distribution<int64_t>  dist_date64(0, 1700000000000LL);
    std::uniform_int_distribution<int64_t>  dist_ts(-31536000LL, 31536000LL);
    std::uniform_int_distribution<int>      dist_name(0, static_cast<int>(first_names.size()) - 1);
    std::uniform_int_distribution<int>      dist_cat(0, static_cast<int>(categories.size()) - 1);
    std::uniform_int_distribution<int64_t>  dist_price(0, 9999999);
    std::uniform_int_distribution<int>      dist_bin_len(4, 32);
    std::uniform_int_distribution<int>      dist_byte(0, 255);

    // ── Generate and write in batches ────────────────────────────────
    int64_t rows_written = 0;
    int batch_num = 0;
    auto t_start = std::chrono::steady_clock::now();

    std::cout << "Generating " << total_rows << " rows with "
              << schema->num_fields() << " columns...\n"
              << "Output: " << output_path << "\n\n";

    while (rows_written < total_rows) {
        int64_t this_batch = std::min(batch_size, total_rows - rows_written);
        ++batch_num;

        // ── Build arrays for this batch ──────────────────────────────
        arrow::Int8Builder    b_i8;
        arrow::Int16Builder   b_i16;
        arrow::Int32Builder   b_i32;
        arrow::Int64Builder   b_i64;
        arrow::UInt8Builder   b_u8;
        arrow::UInt16Builder  b_u16;
        arrow::UInt32Builder  b_u32;
        arrow::UInt64Builder  b_u64;
        arrow::FloatBuilder   b_f32;
        arrow::DoubleBuilder  b_f64;
        arrow::Date32Builder  b_date32;
        arrow::Date64Builder  b_date64;
        arrow::TimestampBuilder b_ts_s(
            arrow::timestamp(arrow::TimeUnit::SECOND),
            arrow::default_memory_pool());
        arrow::TimestampBuilder b_ts_ms(
            arrow::timestamp(arrow::TimeUnit::MILLI),
            arrow::default_memory_pool());
        arrow::TimestampBuilder b_ts_us(
            arrow::timestamp(arrow::TimeUnit::MICRO),
            arrow::default_memory_pool());
        arrow::TimestampBuilder b_ts_ns(
            arrow::timestamp(arrow::TimeUnit::NANO),
            arrow::default_memory_pool());
        arrow::StringBuilder  b_str_hi;
        arrow::StringBuilder  b_str_lo;
        arrow::BinaryBuilder  b_bin;
        arrow::Decimal128Builder b_dec(arrow::decimal128(10, 2));

        CHECK_OK(b_i8.Reserve(this_batch));
        CHECK_OK(b_i16.Reserve(this_batch));
        CHECK_OK(b_i32.Reserve(this_batch));
        CHECK_OK(b_i64.Reserve(this_batch));
        CHECK_OK(b_u8.Reserve(this_batch));
        CHECK_OK(b_u16.Reserve(this_batch));
        CHECK_OK(b_u32.Reserve(this_batch));
        CHECK_OK(b_u64.Reserve(this_batch));
        CHECK_OK(b_f32.Reserve(this_batch));
        CHECK_OK(b_f64.Reserve(this_batch));
        CHECK_OK(b_date32.Reserve(this_batch));
        CHECK_OK(b_date64.Reserve(this_batch));
        CHECK_OK(b_ts_s.Reserve(this_batch));
        CHECK_OK(b_ts_ms.Reserve(this_batch));
        CHECK_OK(b_ts_us.Reserve(this_batch));
        CHECK_OK(b_ts_ns.Reserve(this_batch));
        CHECK_OK(b_dec.Reserve(this_batch));

        for (int64_t i = 0; i < this_batch; ++i) {
            int64_t gidx = rows_written + i;

            CHECK_OK(b_i8.Append(static_cast<int8_t>(dist_i8(rng))));
            CHECK_OK(b_i16.Append(static_cast<int16_t>(dist_i16(rng))));
            CHECK_OK(b_i32.Append(dist_i32(rng)));
            CHECK_OK(b_i64.Append(dist_i64(rng)));
            CHECK_OK(b_u8.Append(static_cast<uint8_t>(dist_u8(rng))));
            CHECK_OK(b_u16.Append(static_cast<uint16_t>(dist_u16(rng))));
            CHECK_OK(b_u32.Append(dist_u32(rng)));
            CHECK_OK(b_u64.Append(dist_u64(rng)));

            CHECK_OK(b_f32.Append(dist_f32(rng)));
            CHECK_OK(b_f64.Append(dist_f64(rng)));

            CHECK_OK(b_date32.Append(dist_date32(rng)));
            CHECK_OK(b_date64.Append(dist_date64(rng)));

            int64_t ts_off = dist_ts(rng);
            CHECK_OK(b_ts_s.Append(base_ts / 1000000 + ts_off));
            CHECK_OK(b_ts_ms.Append(base_ts / 1000 + ts_off * 1000));
            CHECK_OK(b_ts_us.Append(base_ts + ts_off * 1000000));
            CHECK_OK(b_ts_ns.Append(base_ts * 1000 + ts_off * 1000000000LL));

            std::string name =
                first_names[dist_name(rng)] + "_" + std::to_string(gidx);
            CHECK_OK(b_str_hi.Append(name));

            CHECK_OK(b_str_lo.Append(categories[dist_cat(rng)]));

            int bin_len = dist_bin_len(rng);
            std::vector<uint8_t> bin_data(bin_len);
            for (int b = 0; b < bin_len; ++b) {
                bin_data[b] = static_cast<uint8_t>(dist_byte(rng));
            }
            CHECK_OK(b_bin.Append(bin_data.data(), bin_len));

            arrow::Decimal128 dec_val(dist_price(rng));
            CHECK_OK(b_dec.Append(dec_val));
        }

        // ── Finish arrays ────────────────────────────────────────────
        std::shared_ptr<arrow::Array> a_i8, a_i16, a_i32, a_i64;
        std::shared_ptr<arrow::Array> a_u8, a_u16, a_u32, a_u64;
        std::shared_ptr<arrow::Array> a_f32, a_f64;
        std::shared_ptr<arrow::Array> a_date32, a_date64;
        std::shared_ptr<arrow::Array> a_ts_s, a_ts_ms, a_ts_us, a_ts_ns;
        std::shared_ptr<arrow::Array> a_str_hi, a_str_lo, a_bin, a_dec;

        CHECK_OK(b_i8.Finish(&a_i8));
        CHECK_OK(b_i16.Finish(&a_i16));
        CHECK_OK(b_i32.Finish(&a_i32));
        CHECK_OK(b_i64.Finish(&a_i64));
        CHECK_OK(b_u8.Finish(&a_u8));
        CHECK_OK(b_u16.Finish(&a_u16));
        CHECK_OK(b_u32.Finish(&a_u32));
        CHECK_OK(b_u64.Finish(&a_u64));
        CHECK_OK(b_f32.Finish(&a_f32));
        CHECK_OK(b_f64.Finish(&a_f64));
        CHECK_OK(b_date32.Finish(&a_date32));
        CHECK_OK(b_date64.Finish(&a_date64));
        CHECK_OK(b_ts_s.Finish(&a_ts_s));
        CHECK_OK(b_ts_ms.Finish(&a_ts_ms));
        CHECK_OK(b_ts_us.Finish(&a_ts_us));
        CHECK_OK(b_ts_ns.Finish(&a_ts_ns));
        CHECK_OK(b_str_hi.Finish(&a_str_hi));
        CHECK_OK(b_str_lo.Finish(&a_str_lo));
        CHECK_OK(b_bin.Finish(&a_bin));
        CHECK_OK(b_dec.Finish(&a_dec));

        auto batch = arrow::RecordBatch::Make(schema, this_batch, {
            a_i8, a_i16, a_i32, a_i64,
            a_u8, a_u16, a_u32, a_u64,
            a_f32, a_f64,
            a_date32, a_date64,
            a_ts_s, a_ts_ms, a_ts_us, a_ts_ns,
            a_str_hi, a_str_lo, a_bin, a_dec
        });

        CHECK_OK(writer->WriteRecordBatch(*batch));

        rows_written += this_batch;
        if (batch_num % 10 == 0 || rows_written >= total_rows) {
            auto elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t_start).count();
            double pct = 100.0 * rows_written / total_rows;
            double rate = rows_written / elapsed / 1e6;
            std::cout << "  " << std::setw(3) << static_cast<int>(pct)
                      << "%  " << rows_written << " / " << total_rows
                      << " rows  (" << std::fixed << std::setprecision(2)
                      << rate << " M rows/s)\n";
        }
    }

    CHECK_OK(writer->Close());
    CHECK_OK(outfile->Close());

    // ── Report ───────────────────────────────────────────────────────
    auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t_start).count();

    auto file_result = arrow::io::ReadableFile::Open(output_path);
    if (file_result.ok()) {
        auto size_result = file_result.ValueOrDie()->GetSize();
        if (size_result.ok()) {
            double mb = size_result.ValueOrDie() / (1024.0 * 1024.0);
            std::cout << "\nDone in " << std::fixed << std::setprecision(1)
                      << elapsed << " s\n"
                      << "  Rows:    " << rows_written << "\n"
                      << "  Columns: " << schema->num_fields() << "\n"
                      << "  File:    " << output_path << "\n"
                      << "  Size:    " << std::setprecision(1) << mb
                      << " MB\n";
        }
    }

    return 0;
}
