// AI classifier action for per-target classification within the flow pipeline.

#pragma once

#include <chrono>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "flow/classify/AiClassifierBase.h"
#include "infer/AiCommon.h"
#include "util/AiTypes.h"
#include "util/GeometricPos.h"

namespace cosmo {
class AiClassifier : public AiClassifierBase {
public:
    AiClassifier(const std::string& taskId, ActionNode& action);
    ~AiClassifier() = default;

private:
    bool CheckDataAvailable(AlgDataPtr algData) override;
    void HandFramesEx(std::vector<AlgDataPtr> algDatas) override;

    // ---------------------------------------------------------------------
    // L2 cross-frame dedup: reuse the previous classification result for a
    // target that is still being tracked, instead of re-running the model.
    // Keyed by AiDetectRstEl::trackIdInfo (globally unique tracking UUID).
    // Disabled by default; a target without trackIdInfo always bypasses it.
    // ---------------------------------------------------------------------

    /// Snapshot of the classification outputs produced for one target.
    struct ClassifyCacheEntry {
        /// Boxes that actually fed the model input, in the order used.
        std::vector<util::Box> boxes;
        std::vector<AiConfidence> classify_rst;
        /// Per-element results for AiDetectRstEl::relatedEls (mult-sub path).
        std::vector<std::vector<AiConfidence>> related_els_rst;
        /// Results for AiDetectRstEl::relatedEl (single path).
        std::vector<AiConfidence> related_el_rst;
        std::chrono::steady_clock::time_point updated_at{};
    };

    /// Read the classifyCache* parameters from the action configuration.
    void LoadCacheParams(ActionNode& action);

    /// The boxes the classifier would crop from for @p el.
    static std::vector<util::Box> CropBoxes(const AiDetectRstEl& el, bool mult_related);

    /// A result is worth caching only when it is decisive enough.
    static bool IsConfidentResult(const std::vector<AiConfidence>& rst, float min_confidence,
                                  float min_margin);

    /// On hit, copies the cached classification into @p el and returns true.
    bool TryReuseCachedResult(AiDetectRstEl& el, bool mult_related);

    /// Store (or refresh) the classification produced for @p el.
    void UpdateClassifyCache(const AiDetectRstEl& el, bool mult_related);

    bool cache_enabled_{false};
    int64_t cache_ttl_ms_{1000};
    // A6 measured helmet-classifier distribution (RK3588 .3, 1ch 5fps/180s,
    // n=1528 classified targets): top-1 >= 0.95 in 80% of samples, only 2.1%
    // below 0.7; top1-top2 margin >= 0.8 in 87.8%, only ~1.7% below 0.3.
    // These gates therefore drop only the ~3% most ambiguous results from the
    // cache while keeping coin-flip labels out of it. Cost measured: negligible
    // (hit rate 72.7% -> ~70%).
    float cache_min_confidence_{0.7f};
    float cache_min_margin_{0.3f};
    float cache_stable_iou_{0.50f};
    size_t cache_max_entries_{256};

    /// Debug counters for the periodic hit-rate log (guarded by cache_mtx_).
    uint64_t cache_lookups_{0};
    uint64_t cache_hits_{0};

    std::mutex cache_mtx_;
    /// Most-recently-used key first.
    std::list<std::string> cache_lru_;
    std::unordered_map<std::string, std::pair<std::list<std::string>::iterator, ClassifyCacheEntry>>
        cache_;
};
using AiClassifierPtr = std::shared_ptr<AiClassifier>;
}  // namespace cosmo
