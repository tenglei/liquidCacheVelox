# Optimize to_arrow() Decode Path

## Context

C++ LiquidCacheStore benchmark (512MB, 20 columns, 430 万行) 显示 CacheStore 在所有场景下都比 Parquet 慢 (0.20x - 0.93x)。根因分析发现问题不在缓存层架构，而在 `to_arrow()` 解码路径存在 3 个关键瓶颈：

1. **BitPacking 解压**：逐元素 `get(i)` 标量调用 (N 次函数调用)，Rust 用 fastlanes SIMD 批量 1024 元素
2. **Arrow 构造**：`Builder.Append()` 逐元素追加（虚函数派发、缓冲区管理），Rust 用 `PrimitiveArray::new()` 直接包装缓冲区
3. **String 解码**：展平为 `StringBuilder.Append(string)` 丢失字典结构（N 次 string 拷贝），Rust 保持 `DictionaryArray`

目标：参考 Rust 实现，将所有 `to_arrow()` 从 Builder 逐元素模式改为 **批量解压 + 直接缓冲区构造**。

---

## Step 1: BitPackedArray API 扩展

**File:** `include/liquid_cache/bit_packed_array.h`

在 public 区域 (line ~228 附近) 添加以下方法：

### 1a. Null bitmap 访问器

```cpp
const uint8_t* null_bitmap_data() const {
    return null_bitmap_.empty() ? nullptr : null_bitmap_.data();
}

size_t null_bitmap_bytes() const {
    return null_bitmap_.empty() ? 0 : (length_ + 7) / 8;
}
```

### 1b. Null count 计算

```cpp
int64_t null_count() const {
    if (null_bitmap_.empty()) return 0;
    int64_t count = 0;
    for (uint32_t i = 0; i < length_; ++i) {
        if ((null_bitmap_[i / 8] & (1 << (i % 8))) == 0) ++count;
    }
    return count;
}
```

### 1c. 批量解压模板方法

```cpp
/// Bulk unpack all values into a typed output buffer.
/// Eliminates per-element get() function call overhead.
/// The tight loop enables compiler auto-vectorization.
template<typename T>
void bulk_unpack_to(T* output, uint32_t count) const {
    if (bit_width_ == 0 || count == 0) {
        std::memset(output, 0, count * sizeof(T));
        return;
    }
    const uint8_t* src = packed_data_.data();
    const uint64_t mask = (bit_width_ < 64) ? ((1ULL << bit_width_) - 1) : ~0ULL;
    for (uint32_t i = 0; i < count; ++i) {
        size_t bit_offset = static_cast<size_t>(i) * bit_width_;
        size_t byte_idx = bit_offset >> 3;
        uint8_t bit_idx = bit_offset & 7;
        uint64_t raw = 0;
        std::memcpy(&raw, src + byte_idx,
                     std::min<size_t>(sizeof(uint64_t),
                                      packed_data_.size() - byte_idx));
        output[i] = static_cast<T>((raw >> bit_idx) & mask);
    }
}
```

### 1d. Arrow null bitmap buffer 辅助方法

```cpp
/// Copy null bitmap into an Arrow-allocated Buffer for ArrayData::Make.
std::shared_ptr<arrow::Buffer> null_bitmap_arrow_buffer() const {
    if (null_bitmap_.empty()) return nullptr;
    size_t nbytes = (length_ + 7) / 8;
    auto buf = arrow::AllocateBuffer(static_cast<int64_t>(nbytes)).ValueOrDie();
    std::memcpy(buf->mutable_data(), null_bitmap_.data(), nbytes);
    return std::move(buf);
}
```

> 需要在文件头部添加 `#include <arrow/api.h>`（如尚未包含）。

---

## Step 2: LiquidPrimitiveArray::to_arrow() 重写

**File:** `include/liquid_cache/liquid_arrays.h` — 替换 lines 164-180

将当前 Builder 逐元素模式替换为如下实现：

```cpp
std::shared_ptr<arrow::Array> to_arrow() const {
    uint32_t len = bit_packed_.length();
    if (len == 0) {
        return arrow::MakeEmptyArray(
            arrow::TypeTraits<ArrowType>::type_singleton()).ValueOrDie();
    }

    // Step 1: Allocate Arrow value buffer
    int64_t buf_size = static_cast<int64_t>(len) * sizeof(NativeT);
    auto value_buf = arrow::AllocateBuffer(buf_size).ValueOrDie();
    auto* values = reinterpret_cast<NativeT*>(value_buf->mutable_data());

    // Step 2: Bulk unpack + reference value addition
    if (reference_value_ == 0) {
        // Fast path: bulk unpack directly into value buffer (Rust transmute equivalent)
        bit_packed_.bulk_unpack_to<NativeT>(values, len);
    } else {
        // Unpack to unsigned temp, then add reference in tight vectorizable loop
        std::vector<UnsignedT> temp(len);
        bit_packed_.bulk_unpack_to<UnsignedT>(temp.data(), len);
        for (uint32_t i = 0; i < len; ++i) {
            values[i] = reference_value_ + static_cast<NativeT>(temp[i]);
        }
    }

    // Step 3: Null bitmap + direct construction
    auto null_buf = bit_packed_.null_bitmap_arrow_buffer();
    int64_t nc = bit_packed_.null_count();
    auto data = arrow::ArrayData::Make(
        arrow::TypeTraits<ArrowType>::type_singleton(),
        static_cast<int64_t>(len),
        {std::move(null_buf), std::move(value_buf)},
        nc);
    return arrow::MakeArray(data);
}
```

**与 Rust 实现对照：**
- `reference_value_ == 0` 路径 ≈ Rust `unsafe { std::mem::transmute(values) }`
- `reference_value_ != 0` 路径 ≈ Rust `ScalarBuffer::from_iter(values.iter().map(...))`
- `ArrayData::Make` ≈ Rust `PrimitiveArray::new(values, nulls)`

---

## Step 3: LiquidFloatArray::to_arrow() 重写

**File:** `include/liquid_cache/liquid_arrays.h` — 替换 lines 444-486

将双遍历 Builder 模式替换为单遍历 + 原地 patch：

```cpp
std::shared_ptr<arrow::Array> to_arrow() const {
    uint32_t len = bit_packed_.length();
    if (len == 0) {
        return arrow::MakeEmptyArray(
            arrow::TypeTraits<ArrowType>::type_singleton()).ValueOrDie();
    }

    // Step 1: Allocate output buffer
    int64_t buf_size = static_cast<int64_t>(len) * sizeof(FloatT);
    auto value_buf = arrow::AllocateBuffer(buf_size).ValueOrDie();
    auto* values = reinterpret_cast<FloatT*>(value_buf->mutable_data());

    // Step 2: Bulk unpack offsets
    std::vector<UnsignedInt> temp(len);
    bit_packed_.bulk_unpack_to<UnsignedInt>(temp.data(), len);

    // Step 3: Single-pass decode (tight loop, auto-vectorizable)
    for (uint32_t i = 0; i < len; ++i) {
        SignedInt encoded_val = reference_value_ +
                                static_cast<SignedInt>(temp[i]);
        values[i] = decode_single(encoded_val, exponent_);
    }

    // Step 4: In-place patch (replaces entire second-pass rebuild)
    for (size_t j = 0; j < patch_indices_.size(); ++j) {
        values[patch_indices_[j]] = patch_values_[j];
    }

    // Step 5: Null bitmap + direct construction
    auto null_buf = bit_packed_.null_bitmap_arrow_buffer();
    int64_t nc = bit_packed_.null_count();
    auto data = arrow::ArrayData::Make(
        arrow::TypeTraits<ArrowType>::type_singleton(),
        static_cast<int64_t>(len),
        {std::move(null_buf), std::move(value_buf)},
        nc);
    return arrow::MakeArray(data);
}
```

**与 Rust 实现对照：**
- Step 2+3 ≈ Rust `Vec::from_iter(values.iter().map(|v| T::decode_single(...)))`
- Step 4 ≈ Rust `decoded_values[self.patch_indices[i].as_usize()] = self.patch_values[i]`
- 消除了旧代码的双 Builder 遍历 (原 lines 463-483)

---

## Step 4: LiquidDecimalArray::to_arrow() 重写

**File:** `include/liquid_cache/liquid_decimal_array.h` — 替换 lines 165-188

```cpp
std::shared_ptr<arrow::Array> to_arrow() const {
    uint32_t len = bit_packed_.length();
    auto dec_type = arrow::decimal128(precision_, scale_);
    if (len == 0) {
        return arrow::MakeEmptyArray(dec_type).ValueOrDie();
    }

    // Step 1: Allocate Decimal128 buffer (16 bytes per element)
    int64_t buf_size = static_cast<int64_t>(len) * 16;
    auto value_buf = arrow::AllocateBuffer(buf_size).ValueOrDie();
    auto* out = value_buf->mutable_data();

    // Step 2: Bulk unpack u64 offsets
    std::vector<uint64_t> temp(len);
    bit_packed_.bulk_unpack_to<uint64_t>(temp.data(), len);

    // Step 3: Convert u64 → Decimal128 (little-endian: low=value, high=0)
    for (uint32_t i = 0; i < len; ++i) {
        uint64_t val = temp[i] + reference_value_;
        std::memcpy(out + i * 16, &val, 8);      // low 8 bytes
        std::memset(out + i * 16 + 8, 0, 8);     // high 8 bytes = 0
    }

    // Step 4: Null bitmap + direct construction
    auto null_buf = bit_packed_.null_bitmap_arrow_buffer();
    int64_t nc = bit_packed_.null_count();
    auto data = arrow::ArrayData::Make(
        dec_type, static_cast<int64_t>(len),
        {std::move(null_buf), std::move(value_buf)},
        nc);
    return arrow::MakeArray(data);
}
```

---

## Step 5: LiquidByteViewArray::to_arrow() 重写

**File:** `include/liquid_cache/liquid_byte_view_array.h` — 替换 lines 356-416

将 StringBuilder 展平模式替换为 DictionaryArray 构造：

```cpp
std::shared_ptr<arrow::Array> to_arrow() const {
    uint32_t len = dictionary_keys_.length();

    // ===== Phase A: Build dictionary values (StringArray/BinaryArray) =====
    size_t dict_size = compact_offsets_.len() > 0
                       ? compact_offsets_.len() - 1 : 0;

    // A1. Decompress all dictionary entries (O(dict_size), not O(N))
    std::vector<std::string> dict_entries(dict_size);
    for (size_t d = 0; d < dict_size; ++d) {
        uint32_t comp_start = compact_offsets_.get(d);
        uint32_t comp_end = compact_offsets_.get(d + 1);
        auto entry = FsstCompressor::decompress(
            compressor_.symbol_count() > 0 ? &compressor_.symbol(0) : nullptr,
            compressor_.symbol_count(),
            compressed_data_.data() + comp_start, comp_end - comp_start);
        dict_entries[d].assign(shared_prefix_.begin(), shared_prefix_.end());
        dict_entries[d].append(
            reinterpret_cast<const char*>(entry.data()), entry.size());
    }

    // A2. Build offsets + data buffers for dictionary values
    //     (offsets: int32[dict_size+1], data: concatenated strings)
    size_t total_data_bytes = 0;
    for (auto& s : dict_entries) total_data_bytes += s.size();

    auto offsets_buf = arrow::AllocateBuffer(
        static_cast<int64_t>((dict_size + 1) * sizeof(int32_t))).ValueOrDie();
    auto data_buf = arrow::AllocateBuffer(
        static_cast<int64_t>(total_data_bytes)).ValueOrDie();
    auto* offsets = reinterpret_cast<int32_t*>(offsets_buf->mutable_data());
    auto* data_ptr = data_buf->mutable_data();

    int32_t offset = 0;
    for (size_t d = 0; d < dict_size; ++d) {
        offsets[d] = offset;
        std::memcpy(data_ptr + offset, dict_entries[d].data(),
                     dict_entries[d].size());
        offset += static_cast<int32_t>(dict_entries[d].size());
    }
    offsets[dict_size] = offset;

    // A3. Construct dictionary values array
    auto dict_value_type = is_binary_ ? arrow::binary() : arrow::utf8();
    auto dict_values_data = arrow::ArrayData::Make(
        dict_value_type, static_cast<int64_t>(dict_size),
        {nullptr, std::move(offsets_buf), std::move(data_buf)});

    // ===== Phase B: Build keys (UInt16Array) =====
    auto keys_buf = arrow::AllocateBuffer(
        static_cast<int64_t>(len) * sizeof(uint16_t)).ValueOrDie();
    dictionary_keys_.bulk_unpack_to<uint16_t>(
        reinterpret_cast<uint16_t*>(keys_buf->mutable_data()), len);

    auto null_buf = dictionary_keys_.null_bitmap_arrow_buffer();
    int64_t nc = dictionary_keys_.null_count();

    // ===== Phase C: Assemble DictionaryArray =====
    auto dict_type = arrow::dictionary(arrow::uint16(), dict_value_type);
    auto keys_data = arrow::ArrayData::Make(
        dict_type, static_cast<int64_t>(len),
        {std::move(null_buf), std::move(keys_buf)}, nc);
    keys_data->dictionary = std::move(dict_values_data);
    auto dict_array = arrow::MakeArray(keys_data);

    // ===== Phase D: Cast to original Arrow type =====
    auto target_type = is_binary_ ? arrow::binary() : arrow::utf8();
    auto cast_result = arrow::compute::Cast(*dict_array, target_type);
    if (!cast_result.ok()) {
        throw std::runtime_error("Cast failed: " + cast_result.status().ToString());
    }
    return cast_result.ValueOrDie().make_array();
}
```

**与 Rust 实现对照：**
- Phase A ≈ Rust `fsst_buffer.to_uncompressed()` + `StringArray::new_unchecked()`
- Phase B ≈ Rust `self.dictionary_keys.clone()` (UInt16Array)
- Phase C ≈ Rust `DictionaryArray::new_unchecked(keys, values)`
- Phase D ≈ Rust `cast(&dict, &original_arrow_type.to_arrow_type())`

**关键改进：**
- 旧代码：N 次 `StringBuilder.Append(dict_values[key])` = N 次 string 查找 + 拷贝
- 新代码：字典只构建一次，Arrow Cast 内部通过索引展开，比逐元素拷贝高效

---

## Files to Modify

| File | Change |
|------|--------|
| `include/liquid_cache/bit_packed_array.h` | 添加 `bulk_unpack_to<T>()`, `null_bitmap_data()`, `null_count()`, `null_bitmap_arrow_buffer()` |
| `include/liquid_cache/liquid_arrays.h` | 重写 `LiquidPrimitiveArray::to_arrow()` 和 `LiquidFloatArray::to_arrow()` |
| `include/liquid_cache/liquid_decimal_array.h` | 重写 `LiquidDecimalArray::to_arrow()` |
| `include/liquid_cache/liquid_byte_view_array.h` | 重写 `LiquidByteViewArray::to_arrow()` 返回 DictionaryArray + Cast |

## Implementation Order

1. **BitPackedArray API** (Step 1) — 基础，所有后续步骤依赖此
2. **PrimitiveArray** (Step 2) — 最常用路径 (8 种整数 + 4 种 Timestamp + 2 种 Date = 14 种类型)
3. **FloatArray** (Step 3) — 第二常用路径 (Float32 + Float64)
4. **DecimalArray** (Step 4) — 简单，与 Primitive 同模式
5. **ByteViewArray** (Step 5) — 最复杂，DictionaryArray + Cast

每完成一个 Step 后立即编译验证，确保不引入回归。Step 2/3/4 可在 Step 1 完成后并行开发。

## Verification

### 正确性验证
```bash
# 1. 编译
cd /home/tenglei/code/liquid-cache-cpp/build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-O2"
make -j$(nproc)

# 2. compare 模式验证 encode→decode 一致性
./liquid_cache_example test_data_512mb.parquet compare

# 3. ASAN 构建确认无内存错误
cmake .. -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
make -j$(nproc)
./liquid_cache_example test_data_512mb.parquet compare
```

### 性能验证
```bash
# Release 构建后运行 bench_cache 对比优化前后
cd /home/tenglei/code/liquid-cache-cpp/build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-O2"
make -j$(nproc)
./liquid_cache_example test_data_512mb.parquet bench_cache
```

### 优化前基线 (512MB, 20 cols)
| Scenario | Parquet | CacheStore | Ratio |
|----------|---------|------------|-------|
| Single Int32 | 12.04 ms | 20.44 ms | 0.59x |
| Single Float64 | 16.67 ms | 33.83 ms | 0.49x |
| Single String | 46.55 ms | 236.33 ms | 0.20x |
| Full Table (20) | 536.46 ms | 1180.45 ms | 0.45x |

### 预期优化后目标
- Integer 场景：接近或超过 Parquet (>= 1.0x)
- Float 场景：显著提升，接近 Parquet
- String 场景：从 0.20x 提升到 0.5x+ (主要受 FSST 解压 + Cast 开销限制)
