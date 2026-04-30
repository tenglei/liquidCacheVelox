// test_ipc_header.cpp
// Unit tests for LiquidIPCHeader: serialization, deserialization,
// and error handling (invalid magic, version, too-small buffer).
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "liquid_cache/ipc_header.h"

using namespace liquid_cache;

TEST(IPCHeader, ConstructionAndFields) {
    LiquidIPCHeader hdr(LiquidDataType::Integer, PhysicalType::Int32);
    EXPECT_EQ(hdr.version, LIQUID_IPC_VERSION);
    EXPECT_EQ(hdr.logical_type_id, static_cast<uint16_t>(LiquidDataType::Integer));
    EXPECT_EQ(hdr.physical_type_id, static_cast<uint16_t>(PhysicalType::Int32));

    uint32_t expected_magic = LIQUID_IPC_MAGIC;
    EXPECT_EQ(std::memcmp(hdr.magic, &expected_magic, 4), 0);
}

TEST(IPCHeader, SerializeDeserializeRoundtrip) {
    LiquidIPCHeader hdr(LiquidDataType::Float, PhysicalType::Float64);
    std::vector<uint8_t> bytes;
    hdr.serialize(bytes);
    EXPECT_EQ(bytes.size(), LiquidIPCHeader::SIZE);

    auto restored = LiquidIPCHeader::deserialize(bytes.data(), bytes.size());
    EXPECT_EQ(restored.version, LIQUID_IPC_VERSION);
    EXPECT_EQ(restored.logical_type_id, static_cast<uint16_t>(LiquidDataType::Float));
    EXPECT_EQ(restored.physical_type_id, static_cast<uint16_t>(PhysicalType::Float64));
}

TEST(IPCHeader, AllDataTypeCombinations) {
    std::pair<LiquidDataType, PhysicalType> combos[] = {
        {LiquidDataType::Integer, PhysicalType::Int8},
        {LiquidDataType::Integer, PhysicalType::Int16},
        {LiquidDataType::Integer, PhysicalType::Int32},
        {LiquidDataType::Integer, PhysicalType::Int64},
        {LiquidDataType::Integer, PhysicalType::UInt8},
        {LiquidDataType::Integer, PhysicalType::UInt16},
        {LiquidDataType::Integer, PhysicalType::UInt32},
        {LiquidDataType::Integer, PhysicalType::UInt64},
        {LiquidDataType::Integer, PhysicalType::Date32},
        {LiquidDataType::Integer, PhysicalType::Date64},
        {LiquidDataType::Integer, PhysicalType::TimestampSecond},
        {LiquidDataType::Integer, PhysicalType::TimestampMillisecond},
        {LiquidDataType::Integer, PhysicalType::TimestampMicrosecond},
        {LiquidDataType::Integer, PhysicalType::TimestampNanosecond},
        {LiquidDataType::Float, PhysicalType::Float32},
        {LiquidDataType::Float, PhysicalType::Float64},
        {LiquidDataType::FixedLenByteArray, PhysicalType::Int8},
        {LiquidDataType::ByteViewArray, PhysicalType::Int8},
        {LiquidDataType::LinearInteger, PhysicalType::Int32},
        {LiquidDataType::Decimal, PhysicalType::UInt64},
    };

    for (auto& [logical, physical] : combos) {
        LiquidIPCHeader hdr(logical, physical);
        std::vector<uint8_t> bytes;
        hdr.serialize(bytes);

        auto restored = LiquidIPCHeader::deserialize(bytes.data(), bytes.size());
        EXPECT_EQ(restored.logical_type_id, static_cast<uint16_t>(logical));
        EXPECT_EQ(restored.physical_type_id, static_cast<uint16_t>(physical));
    }
}

TEST(IPCHeader, ErrorBadMagic) {
    LiquidIPCHeader hdr(LiquidDataType::Integer, PhysicalType::Int32);
    std::vector<uint8_t> bytes;
    hdr.serialize(bytes);
    // Corrupt the magic bytes
    bytes[0] = 0xDE;
    bytes[1] = 0xAD;
    EXPECT_THROW({
        LiquidIPCHeader::deserialize(bytes.data(), bytes.size());
    }, std::runtime_error);
}

TEST(IPCHeader, ErrorBadVersion) {
    LiquidIPCHeader hdr(LiquidDataType::Integer, PhysicalType::Int32);
    std::vector<uint8_t> bytes;
    hdr.serialize(bytes);
    // Corrupt version
    bytes[4] = 0xFF;
    EXPECT_THROW({
        LiquidIPCHeader::deserialize(bytes.data(), bytes.size());
    }, std::runtime_error);
}

TEST(IPCHeader, ErrorBufferTooSmall) {
    uint8_t tiny[10] = {0};
    EXPECT_THROW({
        LiquidIPCHeader::deserialize(tiny, 10);
    }, std::runtime_error);
}

TEST(IPCHeader, ErrorEmptyBuffer) {
    EXPECT_THROW({
        LiquidIPCHeader::deserialize(nullptr, 0);
    }, std::runtime_error);
}

TEST(IPCHeader, PaddingIsZero) {
    LiquidIPCHeader hdr(LiquidDataType::Integer, PhysicalType::Int32);
    uint8_t padding_sum = 0;
    for (int i = 0; i < 6; ++i) padding_sum |= hdr.padding[i];
    EXPECT_EQ(padding_sum, 0u);
}

TEST(IPCHeader, SizeMatchesStaticAssert) {
    EXPECT_EQ(sizeof(LiquidIPCHeader), LiquidIPCHeader::SIZE);
    EXPECT_EQ(LiquidIPCHeader::SIZE, 16u);
}
