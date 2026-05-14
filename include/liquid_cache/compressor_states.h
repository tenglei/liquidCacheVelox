// liquid_cache/compressor_states.h
// Per-column FSST compressor state for cross-batch reuse.
// Mirrors Rust's LiquidCompressorStates from
// liquid-cache/src/core/src/cache/utils.rs
#pragma once

#include <memory>
#include <mutex>
#include <utility>

#include "liquid_cache/fsst.h"

namespace liquid_cache {

/// Holds a lazily-trained FSST compressor, shared across batches within
/// the same column. The first batch triggers training; subsequent batches
/// reuse the trained compressor via with_fsst_compressor_or_train().
///
/// Thread-safe: all access is serialized through a std::mutex.
class LiquidCompressorStates {
public:
    LiquidCompressorStates() = default;

    explicit LiquidCompressorStates(std::shared_ptr<FsstCompressor> comp)
        : compressor_(std::move(comp)) {}

    /// Returns true if a trained compressor is available.
    bool has_compressor() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return compressor_ != nullptr;
    }

    /// Returns the shared compressor, or nullptr if not yet trained.
    std::shared_ptr<FsstCompressor> get_compressor() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return compressor_;
    }

    /// Store a trained compressor for future reuse.
    void set_compressor(std::shared_ptr<FsstCompressor> comp) {
        std::lock_guard<std::mutex> lock(mutex_);
        compressor_ = std::move(comp);
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<FsstCompressor> compressor_;
};

/// Helper that mirrors Rust's with_fsst_compressor_or_train().
///
/// If `states` already holds a compressor, calls `use_compressor(comp)`.
/// Otherwise calls `train()` which must return a pair
/// (shared_ptr<FsstCompressor>, ResultType), stores the compressor in
/// `states`, and returns the result.
///
/// @param states          Per-column compressor state (non-owning pointer)
/// @param use_compressor  Callable(shared_ptr<FsstCompressor>) -> R
///                        Invoked when a trained compressor already exists.
/// @param train           Callable() -> pair<shared_ptr<FsstCompressor>, R>
///                        Invoked on first call to train and store a new compressor.
/// @return The R produced by either use_compressor or train.
template <typename UseFunc, typename TrainFunc>
auto with_fsst_compressor_or_train(
        LiquidCompressorStates& states,
        UseFunc&& use_compressor,
        TrainFunc&& train)
    -> decltype(use_compressor(std::shared_ptr<FsstCompressor>{}))
{
    // Fast path: compressor already trained
    if (auto comp = states.get_compressor()) {
        return use_compressor(comp);
    }

    // Slow path: train and store
    auto [compressor, result] = train();
    states.set_compressor(compressor);
    return result;
}

}  // namespace liquid_cache
