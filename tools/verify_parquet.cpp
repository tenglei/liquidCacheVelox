// verify_parquet.cpp
// Simple verification of a Parquet file using Arrow/Parquet C++ API.

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>
#include <arrow/compute/initialize.h>

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <parquet_file>\n";
        return 1;
    }

    auto init_status = arrow::compute::Initialize();
    if (!init_status.ok()) {
        std::cerr << "ERROR: Failed to initialize Arrow compute: "
                  << init_status.ToString() << "\n";
        return 1;
    }

    std::string path = argv[1];

    auto maybe_infile = arrow::io::ReadableFile::Open(path);
    if (!maybe_infile.ok()) {
        std::cerr << "Cannot open file: " << maybe_infile.status().ToString() << "\n";
        return 1;
    }
    auto infile = maybe_infile.ValueOrDie();

    auto reader_result = parquet::arrow::OpenFile(infile, arrow::default_memory_pool());
    if (!reader_result.ok()) {
        std::cerr << "Cannot read Parquet: " << reader_result.status().ToString() << "\n";
        return 1;
    }
    auto reader = std::move(reader_result).ValueOrDie();

    auto meta = reader->parquet_reader()->metadata();
    std::shared_ptr<arrow::Schema> schema;
    auto st = reader->GetSchema(&schema);
    if (!st.ok()) {
        std::cerr << "Cannot get schema: " << st.ToString() << "\n";
        return 1;
    }

    int64_t total_rows = 0;
    auto rb_result = reader->GetRecordBatchReader();
    if (!rb_result.ok()) {
        std::cerr << "Cannot get batch reader: " << rb_result.status().ToString() << "\n";
        return 1;
    }
    auto batch_reader = rb_result.MoveValueUnsafe();

    while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;
        st = batch_reader->ReadNext(&batch);
        if (!st.ok() || !batch) break;
        total_rows += batch->num_rows();
    }

    std::cout << "Rows: " << total_rows << "\n";
    std::cout << "Cols: " << schema->num_fields() << "\n";
    std::cout << "Schema: ";
    for (int i = 0; i < schema->num_fields(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << schema->field(i)->name() << ":" << schema->field(i)->type()->ToString();
    }
    std::cout << "\n";
    std::cout << "Row Groups: " << meta->num_row_groups() << "\n";

    return 0;
}
