// OverviewRecordAiRst — Overview Record Ai Rst implementation.

#include "flow/overview/OverviewRecordAiRst.h"

#include <filesystem>

#include "service/detail/ServiceRegistry.h"
#include "service/system/IOverviewConfig.h"
#include "util/FileUtil.h"
#include "util/JsonStructUtil.h"
#include "util/PathUtil.h"

namespace cosmo {

OverviewRecordAiRst::OverviewRecordAiRst(const std::string& task_id, const std::string& name,
                                         size_t limit_count, size_t limit_duration_ms)
    : task_id_(task_id),
      name_(name),
      limit_count_(limit_count),
      limit_duration_ms_(limit_duration_ms),
      is_overview_enabled_(false) {}

void OverviewRecordAiRst::OverviewRecordFrame(const DataDetTrackClassifyPtr& frame) {
    // Whether to save structured file
    if (!service::ServiceRegistry::Instance().Get<service::IOverviewConfig>().GetOverviewStructureRecord()) {
        is_overview_enabled_ = false;
        return;
    }

    if (!frame) {
        return;
    }

    MsgAiDetFrame rec_data = SrcData2MsgData(frame);

    bool need_clear = false;
    // The switch was turned off in the middle
    if (!is_overview_enabled_) {
        need_clear = true;
        // Local file create directory
        auto path = cosmo::path::GetTaskOverviewDataPath(task_id_);
        if (path.empty() || !cosmo::path::IsSafePathComponent(name_)) {
            is_overview_enabled_ = false;
            return;
        }
        local_file_name_ = (std::filesystem::path(path) / (name_ + ".json")).string();
    }
    is_overview_enabled_ = true;

    SaveMem(rec_data, need_clear);
    SaveLocalFile(rec_data, need_clear);
}

// Interface to get information, mark after each fetch, do not fetch repeatedly
MsgOverviewMem OverviewRecordAiRst::GetOverviewInfoMsg() const {
    MsgOverviewMem info;
    if (frames_.empty()) {
        return info;
    }

    // Multiple consumers like preview and video recording will read concurrently, cannot use single
    // consumption flag (bMsgReq) for mutual exclusion. Changed to returning a snapshot of the recent time
    // window to avoid other threads 'reading it first' causing preview aiFrames to be 0 for a long time.
    constexpr int64_t kLiveSnapshotWindowMs = 5000;
    const int64_t latest_ts                 = frames_.back().timestamp;

    for (const auto& frame : frames_) {
        if ((latest_ts - frame.timestamp) <= kLiveSnapshotWindowMs) {
            info.aiFrames.push_back(frame);
        }
    }

    // Fallback when timestamp is abnormal: return at least the last frame to avoid empty set.
    if (info.aiFrames.empty()) {
        info.aiFrames.push_back(frames_.back());
    }

    return info;
}

// Interface to get information, mark after each fetch, do not fetch repeatedly
MsgOverviewMem OverviewRecordAiRst::GetOverviewInfoDuration(int64_t stream_index, int64_t from,
                                                            int64_t to) const {
    MsgOverviewMem info;
    for (const auto& frame : frames_) {
        if ((frame.streamIndex == stream_index) && (frame.index >= from) && (frame.index <= to)) {
            MsgAiDetFrame node = frame;
            // Frame starts from offset position, used for video recording
            node.index = frame.index - from;
            info.aiFrames.push_back(node);
        }
    }

    return info;
}

MsgOverviewMem OverviewRecordAiRst::GetOverviewInfo(int64_t stream_index, int64_t from, int64_t to) const {
    MsgOverviewMem info;
    // Whether to save structured file
    if (!service::ServiceRegistry::Instance().Get<service::IOverviewConfig>().GetOverviewStructureRecord()) {
        is_overview_enabled_ = false;
        return info;
    }

    std::shared_lock<std::shared_mutex> lock(mtx_);
    if (frames_.empty()) {
        return info;
    }
    if ((stream_index == -1) && (from == -1) && (to == -1)) {
        info = GetOverviewInfoMsg();
    } else {
        info = GetOverviewInfoDuration(stream_index, from, to);
    }
    info.name = name_;
    info.type = MsgOverviewMemDataType::MsgOverviewMemDataTypeAIData;
    return info;
}

void OverviewRecordAiRst::SaveLocalFile(const MsgAiDetFrame& rec_data, bool need_clear) const {
    if (!service::ServiceRegistry::Instance().Get<service::IOverviewConfig>().GetOverviewStructureFile()) {
        return;
    }

    std::string rec_str;
    if (!util::EncodeJson(rec_data, rec_str, -1, ' ', 4)) {
        return;
    }
    rec_str.append("\n");

    if (need_clear) {
        util::WriteFile(local_file_name_, rec_str);
    } else {
        util::WriteFileAppend(local_file_name_, rec_str);
    }
}

void OverviewRecordAiRst::SaveMem(const MsgAiDetFrame& rec_data, bool need_clear) {
    std::lock_guard<std::shared_mutex> lock(mtx_);
    if (need_clear) {
        frames_.clear();
    }

    frames_.push_back(rec_data);
    while (frames_.size() > 1) {
        auto last_timestamp  = frames_.back().timestamp;
        auto front_timestamp = frames_.front().timestamp;
        // If timestamp rolls back (video loop play), or time span exceeds limit, discard old frames
        if ((last_timestamp < front_timestamp) ||
            (static_cast<size_t>(last_timestamp - front_timestamp) > limit_duration_ms_)) {
            frames_.pop_front();
        } else if (frames_.size() > limit_count_) {
            // Fallback logic: discard if count exceeds limit
            frames_.pop_front();
        } else {
            break;
        }
    }
}

MsgAiDetFrame OverviewRecordAiRst::SrcData2MsgData(const DataDetTrackClassifyPtr& frame) const {
    MsgAiDetFrame rec_data;
    rec_data.streamIndex = frame->streamIndex;
    rec_data.index       = frame->frameIndex;
    rec_data.timestamp   = frame->timestamp;
    if ((frame->picWidth <= 0) || (frame->picHeight <= 0)) {
        return rec_data;
    }
    for (const auto& target : frame->targets) {
        MsgTarget rec_target;
        rec_target.trackId           = target.trackId;
        rec_target.trackStatus       = GetTrackStatus(target.trackStatus);
        rec_target.motionStatus      = GetMotionStatus(target.motionStatus);
        rec_target.shapeChangeStatus = GetShapeChangeStatus(target.shapeChangeStatus);
        rec_target.bFilter           = target.bFilter;
        rec_target.filterDesc        = target.filterDesc;
        rec_target.bLogicResult      = target.bLogicResult;
        rec_target.box.x             = static_cast<double>(target.box.x) / frame->picWidth;
        rec_target.box.y             = static_cast<double>(target.box.y) / frame->picHeight;
        rec_target.box.width         = static_cast<double>(target.box.width) / frame->picWidth;
        rec_target.box.height        = static_cast<double>(target.box.height) / frame->picHeight;
        rec_target.aiBox.x           = target.box.x;
        rec_target.aiBox.y           = target.box.y;
        rec_target.aiBox.width       = target.box.width;
        rec_target.aiBox.height      = target.box.height;
        for (const auto& point : target.landmark.landmark) {
            rec_target.landmark.push_back({point.x, point.y});
        }
        // Per-joint scores travel with the points so the overlay can drop joints the
        // model is not confident about instead of drawing them at a guessed position.
        rec_target.keypointConfidences.reserve(target.landmark.keypoints.points.size());
        for (const auto& joint : target.landmark.keypoints.points) {
            rec_target.keypointConfidences.push_back(joint.confidence);
        }
        // Carry the semantic keypoint family so the live overlay composites by kind/schema
        // instead of inferring HumanPose from "count == 17". Empty kind/schema on a target
        // with points only means the model did not label the family; renderers must then stay
        // conservative (never jump to the cobra-pose skeleton from plate/face quad points).
        switch (target.landmark.keypoints.kind) {
            case AiKeypointKind::HumanPose:
                rec_target.keypointKind = "human_pose";
                break;
            case AiKeypointKind::LicensePlate:
                rec_target.keypointKind = "license_plate";
                break;
            case AiKeypointKind::Face:
                rec_target.keypointKind = "face";
                break;
            case AiKeypointKind::OcrQuad:
                rec_target.keypointKind = "ocr_quad";
                break;
            default:
                // Custom/unidentified family: keep "" unless the legacy landmark carries
                // points, in which case it is a generic point set with no semantics known yet.
                if (!target.landmark.landmark.empty())
                    rec_target.keypointKind = "custom";
                break;
        }
        rec_target.keypointSchema = target.landmark.keypoints.schema;
        // OCR text (e.g. plate number) attached by an OCR/plate action. Keep the empty result
        // empty so the UI shows the box/quads without inventing a number.
        for (const auto& ocr : target.ocrRst) {
            if (!ocr.value.empty()) {
                rec_target.ocrString += ocr.value;
                rec_target.ocrConfidence = ocr.confidence;
            }
        }
        rec_target.hwRatio           = target.hwRatio;
        rec_target.hwRatioVariation  = target.hwRatioVariation;
        rec_target.bHaveMatchInfo    = false;
        if (target.matchInfo.setPicCount >= 0) {
            rec_target.bHaveMatchInfo        = true;
            rec_target.matchInfo.setPicCount = target.matchInfo.setPicCount;
            rec_target.matchInfo.matchId     = target.matchInfo.match_id;
            rec_target.matchInfo.matchDegree = target.matchInfo.match_degree;
            rec_target.matchInfo.groupId     = target.matchInfo.group_id;
            rec_target.matchInfo.groupName   = target.matchInfo.group_name;
            rec_target.matchInfo.matched     = target.matchInfo.matched;
        }

        // Group info
        rec_target.groupEls = target.groupTargets;
        // Detection confidence is available only when there is no combination info
        if (rec_target.groupEls.empty()) {
            MsgAiConfidence confidence_det;
            confidence_det.label      = target.confidence.label;
            confidence_det.confidence = target.confidence.confidence;
            if (!confidence_det.label.empty() && confidence_det.confidence >= 0)
                rec_target.confidence.push_back(confidence_det);
        }

        for (const auto& target_history : target.classifyRst) {
            MsgAiConfidence confidence;
            confidence.label      = target_history.label;
            confidence.confidence = target_history.confidence;
            if (!confidence.label.empty())
                rec_target.confidence.push_back(confidence);
        }

        for (const auto& attr_rst : target.attrRst) {
            MsgAiAttribute attr;
            attr.label      = attr_rst.label;
            attr.category   = attr_rst.category;
            attr.confidence = attr_rst.confidence;
            if (!attr.label.empty())
                rec_target.attrs.push_back(attr);
        }

        for (const auto& area : target.areaSign.areas) {
            rec_target.areas.push_back(area.area_id);
        }
        for (const auto& shield_area : target.areaSign.shielded_areas) {
            rec_target.shiledAreas.push_back(shield_area.area_id);
        }
        rec_data.targets.push_back(rec_target);
    }
    return rec_data;
}

}  // namespace cosmo
