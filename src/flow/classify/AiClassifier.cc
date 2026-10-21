// AI classifier action implementation.
// Consumes data via registered processing queues.

#include "flow/classify/AiClassifier.h"

#include <algorithm>

#include "flow/common/AlgDataRecord.h"
#include "flow/common/FlowTaskUtil.h"
#include "nn/core/inference_pipeline_metrics.h"
#include "service/detail/ServiceRegistry.h"
#include "service/system/IConfigReadService.h"
#include "util/Keys.h"
#include "util/Log.h"
#include "util/SafeParse.h"

namespace cosmo {

namespace {
constexpr int64_t kCacheTtlMsMin       = 0;
constexpr int64_t kCacheTtlMsMax       = 600000;  // 10 min hard ceiling
constexpr size_t  kCacheMaxEntriesMin  = 1;
constexpr size_t  kCacheMaxEntriesMax  = 4096;
constexpr size_t  kCacheLogEveryNLookups = 500;
}  // namespace

AiClassifier::AiClassifier(const std::string& taskId, ActionNode& action)
    : AiClassifierBase(AlgActionType::AlgActionAiClassify, taskId, action, "AI-CLASSIFIER ",
                       "classify_" + action.atomicCode) {
    batch_count_ = 1;
    LoadCacheParams(action);
    LOG_INFO("{}[{} {} {}] Init ", log_tag_, GetTaskId(), GetName(), uuid);
}

// Read the classifyCache* parameters. Unknown / out-of-range values fall back
// to the defaults so a bad config can never change runtime behaviour silently.
void AiClassifier::LoadCacheParams(ActionNode& action) {
    for (auto& el : action.configObject.params) {
        if (key::classify_cache::ENABLED == el.key.ToString()) {
            cache_enabled_ = (util::ParseInt(el.value, 0) != 0);
        } else if (key::classify_cache::TTL_MS == el.key.ToString()) {
            const auto v = util::ParseInt<int64_t>(el.value, cache_ttl_ms_);
            cache_ttl_ms_ = std::clamp(v, kCacheTtlMsMin, kCacheTtlMsMax);
        } else if (key::classify_cache::MIN_CONFIDENCE == el.key.ToString()) {
            const auto v = util::ParseFloat(el.value, cache_min_confidence_);
            if (v >= 0.0f && v <= 1.0f) {
                cache_min_confidence_ = v;
            }
        } else if (key::classify_cache::MIN_MARGIN == el.key.ToString()) {
            const auto v = util::ParseFloat(el.value, cache_min_margin_);
            if (v >= 0.0f && v <= 1.0f) {
                cache_min_margin_ = v;
            }
        } else if (key::classify_cache::STABLE_IOU == el.key.ToString()) {
            const auto v = util::ParseFloat(el.value, cache_stable_iou_);
            if (v >= 0.0f && v <= 1.0f) {
                cache_stable_iou_ = v;
            }
        } else if (key::classify_cache::MAX_ENTRIES == el.key.ToString()) {
            const auto v =
                static_cast<size_t>(util::ParseInt<int64_t>(el.value, static_cast<int64_t>(cache_max_entries_)));
            cache_max_entries_ = std::clamp(v, kCacheMaxEntriesMin, kCacheMaxEntriesMax);
        }
    }
    LOG_INFO("{}[{} {} {}] ClassifyCache enabled:{} ttlMs:{} minConf:{} minMargin:{} stableIou:{} maxEntries:{}",
             log_tag_, GetTaskId(), GetName(), uuid, cache_enabled_, cache_ttl_ms_, cache_min_confidence_,
             cache_min_margin_, cache_stable_iou_, cache_max_entries_);
}

// The boxes the classifier crops the model input from. Mirrors
// AiClassifierUnify::CollectBoxOrLandmarkData / ClassifyMultSub so the IoU
// guard compares exactly what the model would have seen.
std::vector<util::Box> AiClassifier::CropBoxes(const AiDetectRstEl& el, bool mult_related) {
    std::vector<util::Box> boxes;
    if (mult_related) {
        boxes.reserve(el.relatedEls.size());
        for (const auto& sub : el.relatedEls) {
            boxes.push_back(sub.box);
        }
        return boxes;
    }
    boxes.push_back(el.relatedEl.bActive ? el.relatedEl.box : el.box);
    return boxes;
}

// A result is cached only when it is decisive: the top-1 confidence clears
// min_confidence and the gap to the runner-up clears min_margin. With the
// default (both 0) every non-empty result is cached.
bool AiClassifier::IsConfidentResult(const std::vector<AiConfidence>& rst, float min_confidence,
                                     float min_margin) {
    if (rst.empty()) {
        return false;
    }
    float top1 = -1.0f;
    float top2 = -1.0f;
    for (const auto& c : rst) {
        if (c.confidence > top1) {
            top2 = top1;
            top1 = c.confidence;
        } else if (c.confidence > top2) {
            top2 = c.confidence;
        }
    }
    if (top1 < min_confidence) {
        return false;
    }
    if (top2 >= 0.0f && (top1 - top2) < min_margin) {
        return false;
    }
    return true;
}

bool AiClassifier::TryReuseCachedResult(AiDetectRstEl& el, bool mult_related) {
    // No tracking identity ⇒ no safe way to bind a cached result to this
    // target. Fall through to the normal classification path.
    if (!cache_enabled_ || el.trackIdInfo.empty()) {
        return false;
    }

    const auto boxes = CropBoxes(el, mult_related);
    const auto now   = std::chrono::steady_clock::now();
    bool hit         = false;

    {
        std::lock_guard<std::mutex> lock(cache_mtx_);
        ++cache_lookups_;
        auto it = cache_.find(el.trackIdInfo);
        if (it != cache_.end()) {
            const auto& entry = it->second.second;
            const auto age_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.updated_at).count();
            bool fresh = (age_ms >= 0) && (age_ms <= cache_ttl_ms_);
            bool stable = boxes.size() == entry.boxes.size();
            for (size_t k = 0; stable && k < boxes.size(); k++) {
                if (util::IntersectionUnionRatio(boxes[k], entry.boxes[k]) < cache_stable_iou_) {
                    stable = false;
                }
            }
            if (fresh && stable) {
                cache_lru_.splice(cache_lru_.begin(), cache_lru_, it->second.first);
                el.classifyRst = entry.classify_rst;
                if (el.relatedEls.size() == entry.related_els_rst.size()) {
                    for (size_t k = 0; k < el.relatedEls.size(); k++) {
                        el.relatedEls[k].classifyRst = entry.related_els_rst[k];
                    }
                }
                el.relatedEl.classifyRst = entry.related_el_rst;
                ++cache_hits_;
                hit = true;
            }
        }
        if (0 == cache_lookups_ % kCacheLogEveryNLookups) {
            LOG_INFO("{}[{} {} {}] ClassifyCache lookups:{} hits:{} size:{}", log_tag_, GetTaskId(),
                     GetName(), uuid, cache_lookups_, cache_hits_, cache_.size());
        }
    }

    cosmo::nn::GetInferencePipelineMetrics().RecordClassifierCacheLookup(hit);
    return hit;
}

void AiClassifier::UpdateClassifyCache(const AiDetectRstEl& el, bool mult_related) {
    if (!cache_enabled_ || el.trackIdInfo.empty()) {
        return;
    }
    if (!IsConfidentResult(el.classifyRst, cache_min_confidence_, cache_min_margin_)) {
        return;
    }

    ClassifyCacheEntry entry;
    entry.boxes            = CropBoxes(el, mult_related);
    entry.classify_rst     = el.classifyRst;
    entry.related_el_rst   = el.relatedEl.classifyRst;
    entry.related_els_rst.reserve(el.relatedEls.size());
    for (const auto& sub : el.relatedEls) {
        entry.related_els_rst.push_back(sub.classifyRst);
    }
    entry.updated_at = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(cache_mtx_);
    auto it = cache_.find(el.trackIdInfo);
    if (it != cache_.end()) {
        it->second.second = std::move(entry);
        cache_lru_.splice(cache_lru_.begin(), cache_lru_, it->second.first);
        return;
    }
    cache_lru_.push_front(el.trackIdInfo);
    cache_.emplace(el.trackIdInfo, std::make_pair(cache_lru_.begin(), std::move(entry)));
    while (cache_.size() > cache_max_entries_) {
        cache_.erase(cache_lru_.back());
        cache_lru_.pop_back();
    }
}

bool AiClassifier::CheckDataAvailable(AlgDataPtr algData) {
    if (!algData) {
        invalid_frame_cnt += 1;
        if (0 == invalid_frame_cnt % 100) {
            LOG_WARN("{}[{} {} {}] Filter {} Frames", log_tag_, GetTaskId(), GetName(), uuid,
                     invalid_frame_cnt);
        }
        action_status = util::ErrorEnum::FlowDataInvalid;
        return false;
    }
    if (!classifier_) {
        invalid_frame_cnt += 1;
        if (0 == invalid_frame_cnt % 100) {
            LOG_WARN("{}[{} {} {}] Filter {} Frames", log_tag_, GetTaskId(), GetName(), uuid,
                     invalid_frame_cnt);
        }
        action_status = util::ErrorEnum::AI_INST_NOTCREATED;
        if (!AiSdkInit()) {
            return false;
        }
    }

    // Native-only (DMA-BUF) frames carry no host frame; classification is
    // native-input capable (ConvertImagesToBlobs wraps the native fd and the
    // RKNN preprocess node performs the RGA box crop), so accept them when a
    // valid native buffer is attached.
    const bool has_host_frame  = algData->chanDataDec.frame && algData->chanDataDec.frame->Active();
    const bool has_native_buf = algData->chanDataDec.native_buffer && algData->chanDataDec.native_buffer->Valid();
    if (!has_host_frame && !has_native_buf) {
        action_status = util::ErrorEnum::FrameDataInvalid;
        return false;
    }

    // Validate the incoming data type is supported for classification.
    if (!((AlgDataType::ChannelDataDec == algData->dataType) ||
          (AlgDataType::ChannelDataDetect == algData->dataType) ||
          (AlgDataType::TaskDataTrack == algData->dataType) ||
          (AlgDataType::TaskDataPersonFace == algData->dataType) ||
          (AlgDataType::TaskDataClassify == algData->dataType) ||
          (AlgDataType::TaskDataLandmark == algData->dataType))) {
        invalid_frame_cnt += 1;
        if (0 == invalid_frame_cnt % 100) {
            LOG_WARN("{}[{} {} {}] Filter {} Frames dataType:{}", log_tag_, GetTaskId(), GetName(), uuid,
                     invalid_frame_cnt, algData->dataType);
        }
        action_status = util::ErrorEnum::FlowDataInvalid;
        return false;
    }

    return true;
}

void AiClassifier::HandFramesEx(std::vector<AlgDataPtr> alg_datas) {
    std::vector<AlgDataPtr> alg_out_datas;
    std::vector<VideoFramePtr> in_images;
    std::vector<media::NativeVideoBufferPtr> in_native_buffers;
    std::vector<std::vector<AiDetectRstEl>> io_puts;
    std::vector<int> img_record;
    size_t image_num              = alg_datas.size();
    bool target_have_mult_related = false;
    for (size_t i = 0; i < image_num; i++) {
        auto alg_data               = alg_datas[i];
        AlgDataPtr alg_out_data     = AlgDataCopy(alg_data);
        alg_out_data->dataType      = AlgDataType::TaskDataClassify;
        alg_out_data->bHaveClassify = true;
        auto classify_result        = std::make_shared<DataDetTrackClassify>();
        alg_out_data->SetTaskResult(AlgDataType::TaskDataClassify, classify_result);
        if (alg_data->chanDataDetect.detRet) {
            classify_result->frameIndex        = alg_data->chanDataDetect.detRet->frameIndex;
            classify_result->streamIndex       = alg_data->chanDataDetect.detRet->streamIndex;
            classify_result->timestamp         = alg_data->chanDataDetect.detRet->timestamp;
            classify_result->picWidth          = alg_data->chanDataDetect.detRet->picWidth;
            classify_result->picHeight         = alg_data->chanDataDetect.detRet->picHeight;
            classify_result->bHaveArea         = alg_data->chanDataDetect.detRet->bHaveArea;
            classify_result->bHaveShieldedArea = alg_data->chanDataDetect.detRet->bHaveShieldedArea;
        } else if (alg_data->chanDataDec.frame) {
            classify_result->frameIndex  = alg_data->chanDataDec.frame->GetFrameIndex();
            classify_result->streamIndex = alg_data->chanDataDec.frame->GetStreamIndex();
            classify_result->timestamp   = alg_data->chanDataDec.frame->GetTimestamp();
            classify_result->picWidth    = alg_data->chanDataDec.frame->GetWidth();
            classify_result->picHeight   = alg_data->chanDataDec.frame->GetHeight();
        } else if (alg_data->chanDataDec.meta.valid) {
            // Native-only path: identity comes from the decode-stage metadata.
            const auto& meta           = alg_data->chanDataDec.meta;
            classify_result->frameIndex  = meta.frameIndex;
            classify_result->streamIndex = meta.streamIndex;
            classify_result->timestamp   = meta.timestamp;
            classify_result->picWidth    = meta.width;
            classify_result->picHeight   = meta.height;
        }

        auto input = alg_data->GetTaskResult(AlgDataType::TaskDataTrack);
        if (AlgDataType::ChannelDataDetect == alg_data->dataType) {
            input = alg_data->chanDataDetect.detRet;
        } else if (AlgDataType::TaskDataClassify == alg_data->dataType) {
            input = alg_data->GetTaskResult(AlgDataType::TaskDataClassify);
        } else if (AlgDataType::TaskDataLandmark == alg_data->dataType) {
            input = alg_data->GetTaskResult(AlgDataType::TaskDataLandmark);
        }

        if (input) {
            classify_result->bHaveArea         = input->bHaveArea;
            classify_result->bHaveShieldedArea = input->bHaveShieldedArea;
        }
        alg_out_datas.push_back(alg_out_data);

        bool has_upstream_target_source = FlowHasUpstreamTargetSource(action_alg, action_node.flowActionId);
        if (!input && has_upstream_target_source) {
            continue;
        }

        std::vector<AiDetectRstEl> io_puts_el;
        std::vector<AiDetectRstEl> targets = input ? input->targets : std::vector<AiDetectRstEl>{};
        if (targets.empty() && !has_upstream_target_source) {
            std::vector<MsgTaskArea> areas;
            {
                std::shared_lock<std::shared_mutex> lock(mtx);
                areas = task_area_.areas;
            }
            targets = BuildAreaFallbackTargets(areas, classify_result->picWidth, classify_result->picHeight);
            classify_result->bHaveArea = !targets.empty();
        }
        size_t box_nums          = targets.size();
        target_have_mult_related = input ? input->targetHaveMultRelated : false;
        for (size_t j = 0; j < box_nums; j++) {
            if ((!targets[j].bForceClassify) &&
                ((targets[j].bFilter)                                   // Skip filtered targets
                 || (input && input->bHaveArea                          // When areas are defined
                     && ((!targets[j].areaSign.shielded_areas.empty())  // Skip targets in shielded areas
                         || (targets[j].areaSign.areas.empty())))))     // Skip targets outside all areas
            {
                classify_result->targets.push_back(targets[j]);
                continue;
            }
            // L2 dedup — reuse a still-valid classification for the same
            // tracked target instead of re-running the model. Targets with no
            // tracking identity and forced targets always fall through.
            if ((!targets[j].bForceClassify) && TryReuseCachedResult(targets[j], target_have_mult_related)) {
                classify_result->targets.push_back(targets[j]);
                continue;
            }
            io_puts_el.push_back(targets[j]);
        }

        if (io_puts_el.size() < 1) {
            continue;
        }

        io_puts.push_back(io_puts_el);
        in_images.push_back(alg_data->chanDataDec.frame);
        in_native_buffers.push_back(alg_data->chanDataDec.native_buffer);
        img_record.push_back(i);
    }

    action_status =
        classifier_->ClassifyMultSub(in_images, io_puts, target_have_mult_related, true, in_native_buffers);
    for (size_t out = 0; out < io_puts.size(); out++) {
        auto classify_result = alg_out_datas[img_record[out]]->GetTaskResult(AlgDataType::TaskDataClassify);
        if (classify_result) {
            for (const auto& el : io_puts[out]) {
                UpdateClassifyCache(el, target_have_mult_related);
                classify_result->targets.push_back(el);
            }
        }
    }

    for (size_t i = 0; i < alg_out_datas.size(); i++) {
        RecordHistory(alg_out_datas[i]);
        distributor->DistributorData(alg_out_datas[i]);
    }
}

}  // namespace cosmo
