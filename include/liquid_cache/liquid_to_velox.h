// liquid_cache/liquid_to_velox.h
// Liquid Cache → Velox Vector direct conversion utilities.
// Conditional on LIQUID_ENABLE_VELOX being defined.
#pragma once

#ifdef LIQUID_ENABLE_VELOX

#include "velox/buffer/Buffer.h"
#include "velox/common/base/Nulls.h"
#include "velox/common/memory/Memory.h"
#include "velox/type/HugeInt.h"
#include "velox/type/StringView.h"
#include "velox/type/Timestamp.h"
#include "velox/type/Type.h"
#include "velox/vector/BaseVector.h"
#include "velox/vector/ComplexVector.h"
#include "velox/vector/FlatVector.h"

#include "liquid_cache/liquid_array.h"
#include "liquid_cache/bit_packed_array.h"
#include "liquid_cache/ipc_header.h"

namespace liquid_cache {

// Use full namespace for Velox types to avoid ambiguity
namespace vx = facebook::velox;

// ═══════════════════════════════════════════════════════════════════════
// Null bitmap conversion: Liquid/Arrow → Velox
// Both use the same convention: bit=1 means valid, bit=0 means null.
// ═══════════════════════════════════════════════════════════════════════

inline vx::BufferPtr copy_null_bitmap_to_velox(
        const BitPackedArray& bpa,
        vx::memory::MemoryPool* pool) {
    if (!bpa.has_nulls()) return nullptr;
    uint32_t len = bpa.length();
    int64_t nbytes = static_cast<int64_t>((len + 7) / 8);
    auto buf = vx::AlignedBuffer::allocate<uint8_t>(nbytes, pool);
    std::memcpy(buf->asMutable<uint8_t>(), bpa.null_bitmap_data(), nbytes);
    buf->setSize(nbytes);
    return buf;
}

// ═══════════════════════════════════════════════════════════════════════
// Timestamp conversion: Liquid int64 → Velox Timestamp
// ═══════════════════════════════════════════════════════════════════════

inline vx::Timestamp int64_to_velox_timestamp(
        int64_t value, PhysicalType pt) {
    switch (pt) {
        case PhysicalType::TimestampSecond:
            return vx::Timestamp(value, 0);
        case PhysicalType::TimestampMillisecond:
            return vx::Timestamp::fromMillis(value);
        case PhysicalType::TimestampMicrosecond:
            return vx::Timestamp::fromMicros(value);
        case PhysicalType::TimestampNanosecond:
            return vx::Timestamp::fromNanos(value);
        default:
            return vx::Timestamp::fromMicros(value);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Liquid PhysicalType → Velox Type mapping
// ═══════════════════════════════════════════════════════════════════════

inline vx::TypePtr liquid_physical_to_velox_type(PhysicalType pt) {
    switch (pt) {
        case PhysicalType::Int8:
        case PhysicalType::UInt8:
            return vx::TINYINT();
        case PhysicalType::Int16:
        case PhysicalType::UInt16:
            return vx::SMALLINT();
        case PhysicalType::Int32:
        case PhysicalType::UInt32:
            return vx::INTEGER();
        case PhysicalType::Int64:
        case PhysicalType::UInt64:
            return vx::BIGINT();
        case PhysicalType::Date32:
        case PhysicalType::Date64:
            return vx::INTEGER();
        case PhysicalType::TimestampSecond:
        case PhysicalType::TimestampMillisecond:
        case PhysicalType::TimestampMicrosecond:
        case PhysicalType::TimestampNanosecond:
            return vx::TIMESTAMP();
        case PhysicalType::Float32:
            return vx::REAL();
        case PhysicalType::Float64:
            return vx::DOUBLE();
        default:
            throw std::runtime_error(
                "Unsupported Liquid PhysicalType for Velox conversion: " +
                std::to_string(static_cast<int>(pt)));
    }
}

inline vx::TypeKind liquid_physical_to_velox_kind(PhysicalType pt) {
    switch (pt) {
        case PhysicalType::Int8:
        case PhysicalType::UInt8:
            return vx::TypeKind::TINYINT;
        case PhysicalType::Int16:
        case PhysicalType::UInt16:
            return vx::TypeKind::SMALLINT;
        case PhysicalType::Int32:
        case PhysicalType::UInt32:
            return vx::TypeKind::INTEGER;
        case PhysicalType::Int64:
        case PhysicalType::UInt64:
            return vx::TypeKind::BIGINT;
        case PhysicalType::Date32:
        case PhysicalType::Date64:
            return vx::TypeKind::INTEGER;
        case PhysicalType::TimestampSecond:
        case PhysicalType::TimestampMillisecond:
        case PhysicalType::TimestampMicrosecond:
        case PhysicalType::TimestampNanosecond:
            return vx::TypeKind::TIMESTAMP;
        case PhysicalType::Float32:
            return vx::TypeKind::REAL;
        case PhysicalType::Float64:
            return vx::TypeKind::DOUBLE;
        default:
            throw std::runtime_error(
                "Unsupported Liquid PhysicalType for Velox kind: " +
                std::to_string(static_cast<int>(pt)));
    }
}

}  // namespace liquid_cache

#endif  // LIQUID_ENABLE_VELOX
