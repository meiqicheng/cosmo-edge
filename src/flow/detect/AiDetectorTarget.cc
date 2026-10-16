// AiDetectorTarget.cc — Target area marking, overview recording, and history for AiDetector.
// Split from AiDetector.cc to reduce file size (DEBT-007).

#include "flow/common/AlgDataRecord.h"
#include "flow/detect/AiDetector.h"
#include "util/GeometricPos.h"
#include "util/Log.h"

static constexpr const char* kTag = "AI-DETECTER ";
namespace cosmo {

void AiDetector::TargetAddArea(AiDetectRstEl& target, TargetPosition pos, TargetAreaType type,
                               MsgTaskArea& area, int pic_width, int pic_height, bool associated_area,
                               const std::string& main_area_id) {
    if (BoxInArea(target.box, pos, target.point, area.points, pic_width, pic_height)) {
        TargetAreaUnit targetArea;
        targetArea.area_id            = area.areaId;
        targetArea.area_name          = area.name;
        targetArea.is_associated_area = associated_area;
        targetArea.main_area_id       = main_area_id;
        targetArea.position           = pos;
        targetArea.type               = type;
        target.targetPos              = pos;
        if (TargetAreaType::kNormal == type) {
            target.areaSign.areas.push_back(targetArea);
        } else {
            target.areaSign.shielded_areas.push_back(targetArea);
        }
    }
    return;
}

void AiDetector::TargetAddLine(AiDetectRstEl& target, TargetPosition pos, MsgTaskArea& area, int pic_width,
                               int pic_height, bool associated_area, const std::string& main_area_id) {
    TargetLineUnit targetonLineUnit;
    targetonLineUnit.area_id            = area.areaId;
    targetonLineUnit.area_name          = area.name;
    targetonLineUnit.is_associated_area = associated_area;
    targetonLineUnit.main_area_id       = main_area_id;
    targetonLineUnit.position           = pos;
    targetonLineUnit.type =
        BoxOnLinePos(target.box, pos, target.point, area.linePoints, pic_width, pic_height);
    target.targetPos = pos;
    target.areaSign.lines.push_back(targetonLineUnit);

    return;
}

// Mark area information on targets
void AiDetector::SignTargetAreas(AlgDataPtr data_ptr, const std::string& taskId) {
    if (!data_ptr) {
        LOG_ERRO("{}[{}] SignTargetAreas: null AlgData, skip area marking", kTag, taskId);
        return;
    }

    if (!data_ptr->chanDataDetect.detRet) {
        LOG_ERRO("{}[{}] SignTargetAreas: null detection result, skip area marking", kTag, taskId);
        return;
    }

    // Frame identity comes from the host image when present, otherwise from the
    // decode-stage AlgFrameMeta attached to native-only frames. Skipping area
    // marking on a null frame would silently drop area/line business (P1-1),
    // so fall back to the metadata instead of returning early.
    int width        = 0;
    int height       = 0;
    const auto& meta = data_ptr->chanDataDec.meta;
    if (data_ptr->chanDataDec.frame && data_ptr->chanDataDec.frame->Active()) {
        width  = static_cast<int>(data_ptr->chanDataDec.frame->GetWidth());
        height = static_cast<int>(data_ptr->chanDataDec.frame->GetHeight());
    } else if (meta.valid) {
        width  = meta.width;
        height = meta.height;
    } else {
        LOG_ERRO("{}[{}] SignTargetAreas: no frame identity, skip area marking", kTag, taskId);
        return;
    }

    auto detRet = data_ptr->chanDataDetect.detRet;
    std::shared_lock<std::shared_mutex> lock(mtx);
    auto areaIt = task_areas_.find(taskId);
    if (areaIt == task_areas_.end())
        return;  // Task deleted, skip area marking
    auto& taskArea = areaIt->second;
    if (taskArea.areas.size()) {
        detRet->bHaveArea = true;
    }
    if (taskArea.shieldedAreas.size()) {
        detRet->bHaveShieldedArea = true;
    }

    auto labelParam = GetTaskLabelParams(taskId);

    for (auto& target : detRet->targets) {
        auto pos = GetTaskLabelPos(target.confidence.label, labelParam);
        for (auto area : taskArea.areas) {
            if (!area.points.empty()) {
                TargetAddArea(target, pos, TargetAreaType::kNormal, area, width, height, false, "");
            }
            if (!area.linePoints.empty()) {
                TargetAddLine(target, pos, area, width, height, false, "");
            }
            // Associated areas
            for (auto assArea : area.associatedAreas) {
                if (!assArea.points.empty()) {
                    TargetAddArea(target, pos, TargetAreaType::kNormal, assArea, width, height, true,
                                  area.areaId);
                }
                if (!assArea.linePoints.empty()) {
                    TargetAddLine(target, pos, assArea, width, height, true, area.areaId);
                }
            }
        }

        for (auto area : taskArea.shieldedAreas) {
            if (!area.points.empty()) {
                TargetAddArea(target, pos, TargetAreaType::kShield, area, width, height, false, "");
            }
        }
    }
}

void AiDetector::RecordHistory(AlgDataPtr data_ptr, const std::string& taskId) {
    if (!data_ptr)
        return;
    if (!data_ptr->chanDataDetect.detRet)
        return;

    std::lock_guard<std::shared_mutex> lock(mtx);
    auto it = task_histories_.find(taskId);
    if (it == task_histories_.end())
        return;  // Task deleted, skip recording
    auto& taskHistory = it->second;
    taskHistory.push_back(*data_ptr->chanDataDetect.detRet);
    if (data_ptr->chanDataDetect.detRet->timestamp - taskHistory.front().timestamp >
        media::kVideoInfoMaxDuration) {
        taskHistory.pop_front();
    }
}

std::vector<DataDetTrackClassify> AiDetector::GetHistory(const std::string& /*channel_id*/,
                                                         const std::string& taskId, int64_t from, int64_t ts,
                                                         int64_t to) {
    std::vector<DataDetTrackClassify> rst;
    std::lock_guard<std::shared_mutex> lock(mtx);
    auto histIt = task_histories_.find(taskId);
    if (histIt == task_histories_.end())
        return rst;
    const auto& taskHistory = histIt->second;
    bool bStart             = false;
    for (auto historyEl : taskHistory) {
        auto timestampDiff = (ts == 0) ? 0 : abs(ts - historyEl.timestamp);
        if ((from <= historyEl.frameIndex) && (timestampDiff < media::kTimestampDiff)) {
            bStart = true;
        }

        if (bStart) {
            historyEl.dataType = AlgDataType::ChannelDataDetect;
            rst.push_back(historyEl);
            if (to <= historyEl.frameIndex) {
                break;
            }
        }
    }

    return rst;
}

void AiDetector::AddOverviewTask(const std::string& taskId) {
    auto it = overview_rec_insts_.find(taskId);
    if (it == overview_rec_insts_.end()) {
        OverviewRecordAiRstPtr recordInst = std::make_shared<OverviewRecordAiRst>(taskId, "detect_" + name_);
        overview_rec_insts_[taskId]       = recordInst;
    }
}

void AiDetector::OverviewRecord(const std::string& taskId, DataDetTrackClassifyPtr detRet) {
    auto it = overview_rec_insts_.find(taskId);
    if (it != overview_rec_insts_.end() && it->second) {
        it->second->OverviewRecordFrame(detRet);
    }
}

MsgOverviewMem AiDetector::GetOverviewInfo(const std::string& /*channel_id*/, const std::string& taskId,
                                           int64_t streamIndex, int64_t from, int64_t to) {
    MsgOverviewMem info;
    auto it = overview_rec_insts_.find(taskId);
    if (it != overview_rec_insts_.end() && it->second) {
        info = it->second->GetOverviewInfo(streamIndex, from, to);
    }

    return info;
}

}  // namespace cosmo
