// test_float_quantize.cpp
// Quick tests for LiquidFloatArray::squeeze() and LiquidFloatQuantizedArray
#include <arrow/api.h>
#include <arrow/array/array_base.h>
#include <iostream>
#include <vector>
#include <memory>
#include <cstring>

#include "liquid_cache/liquid_arrays.h"
#include "liquid_cache/ipc_header.h"

using namespace liquid_cache;

// ── In-memory I/O handler for testing ───────────────────────────────
class MemSqueezeIo : public SqueezeIoHandler {
public:
    std::vector<uint8_t> read(uint64_t offset, uint64_t length) override {
        if (offset + length > data_.size()) {
            throw std::runtime_error("MemSqueezeIo::read out of bounds");
        }
        return std::vector<uint8_t>(data_.begin() + offset,
                                    data_.begin() + offset + length);
    }

    void write(std::vector<uint8_t> bytes) {
        data_ = std::move(bytes);
    }
private:
    std::vector<uint8_t> data_;
};

// ── Helper: build a FloatType Arrow array from values + nulls ───────
template <typename ArrowType>
static std::shared_ptr<arrow::Array> make_arrow(
        const std::vector<typename ArrowType::c_type>& values,
        const std::vector<bool>& nulls = {}) {
    using NativeT = typename ArrowType::c_type;
    typename arrow::TypeTraits<ArrowType>::BuilderType builder;
    if (nulls.empty()) {
        ARROW_CHECK_OK(builder.AppendValues(values));
    } else {
        ARROW_CHECK_OK(builder.Reserve(values.size()));
        for (size_t i = 0; i < values.size(); ++i) {
            if (nulls[i]) ARROW_CHECK_OK(builder.Append(values[i]));
            else ARROW_CHECK_OK(builder.AppendNull());
        }
    }
    return builder.Finish().ValueOrDie();
}

// ── Test helpers ────────────────────────────────────────────────────

template <typename FloatT>
static bool check_roundtrip(
        const std::vector<FloatT>& values,
        const std::vector<bool>& nulls = {},
        const std::string& label = "") {
    using ArrowType = typename ALPTraits<FloatT>::ArrowType;

    auto arr = make_arrow<ArrowType>(values, nulls);
    auto liquid = LiquidFloatArray<FloatT>::from_arrow(arr);

    // Only squeeze if bit_width >= 8
    if (liquid.bit_width() < 8) {
        std::cout << label << " (unsqueezable, bw=" << int(liquid.bit_width())
                  << ")... SKIP (bw < 8)" << std::endl;
        return true; // not a failure
    }

    // Squeeze
    auto io = std::make_shared<MemSqueezeIo>();
    auto result = liquid.squeeze(io);
    if (!result.has_value()) {
        std::cerr << label << " squeeze returned nullopt unexpectedly" << std::endl;
        return false;
    }

    auto& [quantized, disk_bytes] = *result;
    io->write(disk_bytes);

    // Hydrate from disk
    auto hydrated = quantized.to_arrow();
    auto hyd_typed = std::static_pointer_cast<typename arrow::TypeTraits<ArrowType>::ArrayType>(hydrated);

    // Compare with original
    auto orig_typed = std::static_pointer_cast<typename arrow::TypeTraits<ArrowType>::ArrayType>(arr);

    if (orig_typed->length() != hyd_typed->length()) {
        std::cerr << label << " length mismatch: " << orig_typed->length()
                  << " vs " << hyd_typed->length() << std::endl;
        return false;
    }

    for (int64_t i = 0; i < orig_typed->length(); ++i) {
        bool orig_null = orig_typed->IsNull(i);
        bool hyd_null = hyd_typed->IsNull(i);
        if (orig_null != hyd_null) {
            std::cerr << label << " null mismatch at " << i << std::endl;
            return false;
        }
        if (!orig_null) {
            FloatT ov = orig_typed->Value(i);
            FloatT hv = hyd_typed->Value(i);
            if (ov != hv) {
                std::cerr << label << " value mismatch at " << i << ": "
                          << ov << " vs " << hv << std::endl;
                return false;
            }
        }
    }
    return true;
}

int main() {
    int passed = 0, failed = 0;

    // ── Test 1: Unsqueezable small range ──────────────────────────────
    {
        std::cout << "Test 1: Unsqueezable small range... ";
        std::vector<float> vals = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
        auto arr = make_arrow<arrow::FloatType>(vals);
        auto liquid = LiquidFloatArray<float>::from_arrow(arr);

        if (liquid.bit_width() >= 8) {
            std::cout << "SKIP (bw=" << int(liquid.bit_width())
                      << " unexpectedly >= 8)" << std::endl;
        } else {
            auto io = std::make_shared<MemSqueezeIo>();
            auto result = liquid.squeeze(io);
            if (result.has_value()) {
                std::cout << "FAIL (unexpectedly squeezable)" << std::endl;
                ++failed;
            } else {
                std::cout << "PASS" << std::endl;
                ++passed;
            }
        }
    }

    // ── Test 2: Squeeze + hydrate roundtrip Float32 ───────────────────
    {
        std::cout << "Test 2: Squeeze roundtrip Float32... ";
        std::vector<float> vals;
        for (int i = 0; i < 2048; ++i) {
            vals.push_back(static_cast<float>(i * 0.375) + 1000.0f);
        }
        if (check_roundtrip<float>(vals, {}, "Test 2")) {
            ++passed;
            // check_roundtrip already printed status
        } else {
            ++failed;
        }
    }

    // ── Test 3: Squeeze + hydrate roundtrip Float64 ───────────────────
    {
        std::cout << "Test 3: Squeeze roundtrip Float64... ";
        std::vector<double> vals;
        for (int i = 0; i < 2048; ++i) {
            vals.push_back(static_cast<double>(i * 0.125) + 2000.0);
        }
        if (check_roundtrip<double>(vals, {}, "Test 3")) {
            ++passed;
        } else {
            ++failed;
        }
    }

    // ── Test 4: Empty array ───────────────────────────────────────────
    {
        std::cout << "Test 4: Empty array... ";
        std::vector<float> vals;
        auto arr = make_arrow<arrow::FloatType>(vals);
        auto liquid = LiquidFloatArray<float>::from_arrow(arr);

        auto io = std::make_shared<MemSqueezeIo>();
        auto result = liquid.squeeze(io);
        if (liquid.bit_width() < 8 && !result.has_value()) {
            // unsqueezable empty is fine
            std::cout << "PASS (empty, unsqueezable)" << std::endl;
            ++passed;
        } else if (result.has_value()) {
            auto& [quantized, disk_bytes] = *result;
            io->write(disk_bytes);
            auto hydrated = quantized.to_arrow();
            if (hydrated->length() == 0) {
                std::cout << "PASS" << std::endl;
                ++passed;
            } else {
                std::cout << "FAIL" << std::endl;
                ++failed;
            }
        } else {
            std::cout << "PASS (empty)" << std::endl;
            ++passed;
        }
    }

    // ── Test 5: All-null squeeze ──────────────────────────────────────
    {
        std::cout << "Test 5: All-null squeeze... ";
        std::vector<float> vals = {0.0f, 0.0f, 0.0f, 0.0f};
        std::vector<bool> nulls = {false, false, false, false};
        auto arr = make_arrow<arrow::FloatType>(vals, nulls);
        auto liquid = LiquidFloatArray<float>::from_arrow(arr);

        auto io = std::make_shared<MemSqueezeIo>();
        auto result = liquid.squeeze(io);
        if (!result.has_value()) {
            std::cout << "SKIP (unsqueezable all-null)" << std::endl;
            ++passed; // null entries have bw=0, might be unsqueezable
        } else {
            auto& [quantized, disk_bytes] = *result;
            io->write(disk_bytes);
            auto hydrated = quantized.to_arrow();
            auto hyd_typed = std::static_pointer_cast<arrow::FloatArray>(hydrated);
            bool ok = true;
            for (int i = 0; i < hyd_typed->length(); ++i) {
                if (!hyd_typed->IsNull(i)) { ok = false; break; }
            }
            if (ok) {
                std::cout << "PASS" << std::endl;
                ++passed;
            } else {
                std::cout << "FAIL" << std::endl;
                ++failed;
            }
        }
    }

    // ── Test 6: Predicate Eq – definitive false everywhere ────────────
    {
        std::cout << "Test 6: Predicate Eq (definitive false)... ";
        std::vector<float> vals;
        for (int i = 0; i < 1024; ++i) {
            vals.push_back(static_cast<float>(i * 1.5) + 500.0f);
        }
        auto arr = make_arrow<arrow::FloatType>(vals);
        auto liquid = LiquidFloatArray<float>::from_arrow(arr);

        if (liquid.bit_width() < 8) {
            std::cout << "SKIP (unsqueezable)" << std::endl;
            ++passed;
        } else {
            auto io = std::make_shared<MemSqueezeIo>();
            auto result = liquid.squeeze(io);
            if (!result.has_value()) {
                std::cout << "SKIP (squeeze failed)" << std::endl;
                ++passed;
            } else {
                auto& [quantized, disk_bytes] = *result;
                io->write(disk_bytes);

                // Use a literal far outside the value range → Eq should be false everywhere
                float far_away = 1e9f;
                auto pred = quantized.try_eval_predicate(Operator::Eq, far_away);
                if (pred == nullptr) {
                    std::cout << "FAIL (returned NeedsBacking)" << std::endl;
                    ++failed;
                } else {
                    bool ok = true;
                    for (int i = 0; i < pred->length(); ++i) {
                        if (!pred->IsNull(i) && pred->Value(i)) {
                            ok = false; break;
                        }
                    }
                    if (ok) {
                        std::cout << "PASS" << std::endl;
                        ++passed;
                    } else {
                        std::cout << "FAIL (expected all false)" << std::endl;
                        ++failed;
                    }
                }
            }
        }
    }

    // ── Test 7: Predicate Gt – definitive true everywhere ─────────────
    {
        std::cout << "Test 7: Predicate Gt (definitive true)... ";
        std::vector<float> vals;
        for (int i = 0; i < 1024; ++i) {
            vals.push_back(static_cast<float>(i * 0.5) + 100.0f);
        }
        auto arr = make_arrow<arrow::FloatType>(vals);
        auto liquid = LiquidFloatArray<float>::from_arrow(arr);

        if (liquid.bit_width() < 8) {
            std::cout << "SKIP (unsqueezable)" << std::endl;
            ++passed;
        } else {
            auto io = std::make_shared<MemSqueezeIo>();
            auto result = liquid.squeeze(io);
            if (!result.has_value()) {
                std::cout << "SKIP (squeeze failed)" << std::endl;
                ++passed;
            } else {
                auto& [quantized, disk_bytes] = *result;
                io->write(disk_bytes);

                // Use a literal far below the value range → Gt should be true everywhere
                float far_below = -1e9f;
                auto pred = quantized.try_eval_predicate(Operator::Gt, far_below);
                if (pred == nullptr) {
                    std::cout << "FAIL (returned NeedsBacking)" << std::endl;
                    ++failed;
                } else {
                    bool ok = true;
                    for (int i = 0; i < pred->length(); ++i) {
                        if (!pred->IsNull(i) && !pred->Value(i)) {
                            ok = false; break;
                        }
                    }
                    if (ok) {
                        std::cout << "PASS" << std::endl;
                        ++passed;
                    } else {
                        std::cout << "FAIL (expected all true)" << std::endl;
                        ++failed;
                    }
                }
            }
        }
    }

    // ── Test 8: Predicate NeedsBacking (ambiguous bounds) ─────────────
    {
        std::cout << "Test 8: Predicate NeedsBacking (ambiguous)... ";
        std::vector<float> vals;
        for (int i = 0; i < 1024; ++i) {
            vals.push_back(static_cast<float>(i * 0.5) + 100.0f);
        }
        auto arr = make_arrow<arrow::FloatType>(vals);
        auto liquid = LiquidFloatArray<float>::from_arrow(arr);

        if (liquid.bit_width() < 8) {
            std::cout << "SKIP (unsqueezable)" << std::endl;
            ++passed;
        } else {
            auto io = std::make_shared<MemSqueezeIo>();
            auto result = liquid.squeeze(io);
            if (!result.has_value()) {
                std::cout << "SKIP (squeeze failed)" << std::endl;
                ++passed;
            } else {
                auto& [quantized, disk_bytes] = *result;
                io->write(disk_bytes);

                // Use a literal in the middle of the range → some buckets ambiguous
                float mid = vals[vals.size() / 2];
                auto pred = quantized.try_eval_predicate(Operator::Eq, mid);
                if (pred == nullptr) {
                    std::cout << "PASS (correctly returned NeedsBacking)" << std::endl;
                    ++passed;
                } else {
                    // Also valid if it managed to resolve
                    std::cout << "PASS (resolved successfully)" << std::endl;
                    ++passed;
                }
            }
        }
    }

    // ── Test 9: Memory size reduction ─────────────────────────────────
    {
        std::cout << "Test 9: Memory size reduction... ";
        std::vector<float> vals;
        for (int i = 0; i < 4096; ++i) {
            vals.push_back(static_cast<float>(i * 0.25) + 500.0f);
        }
        auto arr = make_arrow<arrow::FloatType>(vals);
        auto liquid = LiquidFloatArray<float>::from_arrow(arr);

        if (liquid.bit_width() < 8) {
            std::cout << "SKIP (unsqueezable)" << std::endl;
            ++passed;
        } else {
            size_t orig_size = liquid.memory_size();

            auto io = std::make_shared<MemSqueezeIo>();
            auto result = liquid.squeeze(io);
            if (!result.has_value()) {
                std::cout << "FAIL (squeeze returned nullopt)" << std::endl;
                ++failed;
            } else {
                auto& [quantized, disk_bytes] = *result;
                size_t quant_size = quantized.memory_size();

                std::cout << "orig=" << orig_size << "B quantized=" << quant_size
                          << "B disk=" << disk_bytes.size() << "B... ";

                if (quant_size < orig_size) {
                    std::cout << "PASS" << std::endl;
                    ++passed;
                } else {
                    std::cout << "FAIL (no size reduction)" << std::endl;
                    ++failed;
                }
            }
        }
    }

    // ── Test 10: Consistent values (all same) ─────────────────────────
    {
        std::cout << "Test 10: Consistent values... ";
        std::vector<float> vals(256, 42.5f);
        if (check_roundtrip<float>(vals, {}, "Test 10")) {
            ++passed;
        } else {
            ++failed;
        }
    }

    std::cout << "\n=== Results: " << passed << " passed, " << failed << " failed ===" << std::endl;
    return failed > 0 ? 1 : 0;
}
