// liquid_cache/liquid_array.h
// LiquidArrayBase abstract class and type-erased wrappers.
// Equivalent to Rust's `LiquidArray` trait from
// liquid-cache/src/core/src/liquid_array/mod.rs
//
// This provides a polymorphic interface for all Liquid-encoded array types,
// enabling the cache store to hold heterogeneous arrays without serialization.
#pragma once

#include <arrow/util/logging.h>
#include <arrow/api.h>
#include <arrow/compute/api.h>

#include <memory>
#include <stdexcept>

#include "liquid_cache/ipc_header.h"

#ifdef LIQUID_ENABLE_VELOX
namespace facebook::velox {
class BaseVector;
using VectorPtr = std::shared_ptr<BaseVector>;
namespace memory { class MemoryPool; }
}  // namespace facebook::velox
#endif

namespace liquid_cache {

/// Abstract base class for all Liquid-encoded arrays.
///
/// Mirrors Rust's `LiquidArray` trait:
///   - to_arrow_array() -> ArrayRef
///   - filter(BooleanBuffer) -> ArrayRef
///   - get_array_memory_size() -> usize
///   - len() -> usize
///   - data_type() -> LiquidDataType
///   - original_arrow_data_type() -> DataType
class LiquidArrayBase {
public:
    virtual ~LiquidArrayBase() = default;

    /// Decode to Arrow array (full decompression).
    /// Equivalent to Rust LiquidArray::to_arrow_array().
    virtual std::shared_ptr<arrow::Array> to_arrow() const = 0;

    /// Filter rows by boolean selection mask, return filtered Arrow array.
    /// Equivalent to Rust LiquidArray::filter(BooleanBuffer).
    ///
    /// Default implementation: decode to Arrow, then apply arrow::compute::Filter.
    /// Subtypes may override for optimized filtering without full decompression.
    virtual std::shared_ptr<arrow::Array> filter(
            const std::shared_ptr<arrow::BooleanArray>& selection) const {
        auto arr = to_arrow();
        auto result = arrow::compute::Filter(arr, selection);
        if (!result.ok()) {
            throw std::runtime_error("Filter failed: " + result.status().ToString());
        }
        return result.ValueOrDie().make_array();
    }

    /// In-memory size of the Liquid-encoded representation (bytes).
    /// Equivalent to Rust LiquidArray::get_array_memory_size().
    virtual size_t memory_size() const = 0;

    /// Number of elements.
    /// Equivalent to Rust LiquidArray::len().
    virtual uint32_t length() const = 0;

    /// Logical encoding type.
    /// Equivalent to Rust LiquidArray::data_type().
    virtual LiquidDataType data_type() const = 0;

    /// Physical type.
    virtual PhysicalType physical_type() const = 0;

    /// Original Arrow data type (for correct type reconstruction).
    /// Equivalent to Rust LiquidArray::original_arrow_data_type().
    virtual std::shared_ptr<arrow::DataType> original_arrow_type() const = 0;

#ifdef LIQUID_ENABLE_VELOX
    /// Decode directly to Velox Vector (no Arrow intermediate).
    virtual facebook::velox::VectorPtr to_velox(
        facebook::velox::memory::MemoryPool* pool) const = 0;
#endif
};

/// Shared pointer to a polymorphic LiquidArrayBase.
/// Equivalent to Rust's `LiquidArrayRef = Arc<dyn LiquidArray>`.
using LiquidArrayRef = std::shared_ptr<LiquidArrayBase>;

/// Generic wrapper that adapts any concrete Liquid array type to LiquidArrayBase.
///
/// Template parameter LiquidArrayT must provide:
///   - std::shared_ptr<arrow::Array> to_arrow() const
///   - size_t memory_size() const
///   - uint32_t length() const
template <typename LiquidArrayT>
class LiquidArrayWrapper : public LiquidArrayBase {
public:
    LiquidArrayWrapper(LiquidArrayT inner,
                       LiquidDataType dt,
                       PhysicalType pt,
                       std::shared_ptr<arrow::DataType> original_type)
        : inner_(std::move(inner))
        , data_type_(dt)
        , physical_type_(pt)
        , original_type_(std::move(original_type)) {}

    std::shared_ptr<arrow::Array> to_arrow() const override {
        auto arr = inner_.to_arrow();
        // Reinterpret if the original Arrow type differs from what inner produces.
        // This handles Timestamp (stored as Int64) and similar cases.
        if (original_type_ && arr->type()->id() != original_type_->id()) {
            auto data = arr->data();
            auto new_data = arrow::ArrayData::Make(
                original_type_, data->length, data->buffers,
                data->null_count, data->offset);
            return arrow::MakeArray(new_data);
        }
        return arr;
    }

    size_t memory_size() const override { return inner_.memory_size(); }
    uint32_t length() const override { return inner_.length(); }
    LiquidDataType data_type() const override { return data_type_; }
    PhysicalType physical_type() const override { return physical_type_; }
    std::shared_ptr<arrow::DataType> original_arrow_type() const override {
        return original_type_;
    }

#ifdef LIQUID_ENABLE_VELOX
    facebook::velox::VectorPtr to_velox(
        facebook::velox::memory::MemoryPool* pool) const override {
        return inner_.to_velox(pool);
    }
#endif

    /// Access the underlying concrete Liquid array.
    const LiquidArrayT& inner() const { return inner_; }

private:
    LiquidArrayT inner_;
    LiquidDataType data_type_;
    PhysicalType physical_type_;
    std::shared_ptr<arrow::DataType> original_type_;
};

/// Helper to create a LiquidArrayRef from a concrete array type.
template <typename LiquidArrayT>
LiquidArrayRef make_liquid_array(LiquidArrayT inner,
                                  LiquidDataType dt,
                                  PhysicalType pt,
                                  std::shared_ptr<arrow::DataType> original_type) {
    return std::make_shared<LiquidArrayWrapper<LiquidArrayT>>(
        std::move(inner), dt, pt, std::move(original_type));
}

// Forward declaration
class LiquidCompressorStates;

/// Transcode an Arrow array into an in-memory LiquidArrayRef.
/// Defined in transcoder_arrow.cpp. Returns nullptr for unsupported types.
/// This is the key entry point for the arrow → liquid → velox pipeline.
LiquidArrayRef transcode_to_liquid_array(
    const std::shared_ptr<arrow::Array>& array);

/// Transcode with optional compressor states for FSST cross-batch reuse.
/// Pass nullptr to always train new compressors (backward compatible).
/// When non-null, reuses trained FSST compressors across batches within
/// the same column, mirroring Rust's LiquidCompressorStates pattern.
LiquidArrayRef transcode_to_liquid_array(
    const std::shared_ptr<arrow::Array>& array,
    LiquidCompressorStates* states);

}  // namespace liquid_cache
