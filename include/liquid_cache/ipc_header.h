// liquid_cache/ipc_header.h
// Liquid Cache IPC Header - binary compatible with Rust LiquidIPCHeader
#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace liquid_cache {

/// Magic number for Liquid IPC format: "LQDA" = 0x4C514441
static constexpr uint32_t LIQUID_IPC_MAGIC = 0x4C514441;
static constexpr uint16_t LIQUID_IPC_VERSION = 1;

/// Logical type identifiers (matches Rust LiquidDataType)
enum class LiquidDataType : uint16_t {
    Integer = 1,
    Float = 2,
    FixedLenByteArray = 3,
    ByteViewArray = 4,
    LinearInteger = 5,
    Decimal = 6,
};

/// Physical type identifiers (matches Rust PrimitivePhysicalType)
enum class PhysicalType : uint16_t {
    Int8 = 0,
    Int16 = 1,
    Int32 = 2,
    Int64 = 3,
    UInt8 = 4,
    UInt16 = 5,
    UInt32 = 6,
    UInt64 = 7,
    Float32 = 8,
    Float64 = 9,
    Date32 = 10,
    Date64 = 11,
    TimestampSecond = 12,
    TimestampMillisecond = 13,
    TimestampMicrosecond = 14,
    TimestampNanosecond = 15,
};

/// 16-byte IPC header, binary-compatible with Rust's LiquidIPCHeader.
///
/// Memory layout:
///   [0..3]   magic           ("LQDA", little-endian 0x4C514441)
///   [4..5]   version         (currently 1)
///   [6..7]   logical_type_id (LiquidDataType enum value)
///   [8..9]   physical_type_id(PhysicalType enum value)
///   [10..15] padding         (zeroed)
#pragma pack(push, 1)
struct LiquidIPCHeader {
    uint8_t magic[4];
    uint16_t version;
    uint16_t logical_type_id;
    uint16_t physical_type_id;
    uint8_t padding[6];

    static constexpr size_t SIZE = 16;

    LiquidIPCHeader() = default;

    LiquidIPCHeader(LiquidDataType logical, PhysicalType physical) {
        uint32_t m = LIQUID_IPC_MAGIC;
        std::memcpy(magic, &m, 4);
        version = LIQUID_IPC_VERSION;
        logical_type_id = static_cast<uint16_t>(logical);
        physical_type_id = static_cast<uint16_t>(physical);
        std::memset(padding, 0, sizeof(padding));
    }

    void serialize(std::vector<uint8_t>& out) const {
        out.insert(out.end(), magic, magic + 4);
        auto v = reinterpret_cast<const uint8_t*>(&version);
        out.insert(out.end(), v, v + 2);
        auto l = reinterpret_cast<const uint8_t*>(&logical_type_id);
        out.insert(out.end(), l, l + 2);
        auto p = reinterpret_cast<const uint8_t*>(&physical_type_id);
        out.insert(out.end(), p, p + 2);
        out.insert(out.end(), padding, padding + 6);
    }

    static LiquidIPCHeader deserialize(const uint8_t* data, size_t len) {
        if (len < SIZE) {
            throw std::runtime_error("Buffer too small for LiquidIPCHeader");
        }
        LiquidIPCHeader h;
        std::memcpy(h.magic, data, 4);
        std::memcpy(&h.version, data + 4, 2);
        std::memcpy(&h.logical_type_id, data + 6, 2);
        std::memcpy(&h.physical_type_id, data + 8, 2);
        std::memset(h.padding, 0, 6);

        uint32_t expected_magic = LIQUID_IPC_MAGIC;
        if (std::memcmp(h.magic, &expected_magic, 4) != 0) {
            throw std::runtime_error("Invalid Liquid IPC magic number");
        }
        if (h.version != LIQUID_IPC_VERSION) {
            throw std::runtime_error("Unsupported Liquid IPC version");
        }
        return h;
    }
};
#pragma pack(pop)

static_assert(sizeof(LiquidIPCHeader) == LiquidIPCHeader::SIZE,
              "LiquidIPCHeader must be exactly 16 bytes");

/// Align a byte offset up to the next 8-byte boundary.
inline size_t align8(size_t offset) {
    return (offset + 7) & ~static_cast<size_t>(7);
}

}  // namespace liquid_cache
