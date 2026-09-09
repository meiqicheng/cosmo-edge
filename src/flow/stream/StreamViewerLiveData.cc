// StreamViewerLiveData.cc — LiveData processing methods for StreamViewerOverview.
// Handles data collection: ingesting AI detection results and overlay data
// into the local cache (infos_) with deduplication and expiry logic.
// Alarm processing is in StreamViewerAlarm.cc.

#include <algorithm>
#include <cstdlib>
#include <map>

#include "flow/common/PoseSkeleton.h"
#include "flow/stream/StreamViewerOverview.h"
#include "flow/stream/StreamViewerOverviewTypes.h"
#include "service/detail/ServiceRegistry.h"
#include "service/task/ITaskQuery.h"
#include "util/FormatString.h"

namespace cosmo {

// A joint scoring below this is treated as invisible. Pose heads output a
// visibility score per joint, and an occluded joint still yields finite
// coordinates, so without this cutoff every limb is drawn whether or not the
// model actually saw it. Matches the default pose confidence_threshold.
static constexpr float kPoseJointMinConfidence = 0.25F;

void StreamViewerOverview::AddTextToLocal(int64_t streamIndex, uint64_t index, int64_t timestamp,
                                          util::Point pos, StreamOverviewTextEl& text) {
    // data is expired (VOD strictly aligns frame sequence; live preview does not discard by frame seq,
    // otherwise inference frame number lags behind OSD frame, boxes never enter cache)
    if (!live_stream_) {
        if ((streamIndex < frame_identity_.streamIndex) || (index < frame_identity_.index)) {
            return;
        }
    } else {
        // Match decode frame: some detection paths do not write streamIndex (default 0),
        // while demuxer video track stream_idx is often 1, causing all boxes to be discarded.
        streamIndex = frame_identity_.streamIndex;
    }

    auto overviewIt = std::find_if(
        infos_.overviews.begin(), infos_.overviews.end(), [streamIndex, index](const auto& overview) {
            return (streamIndex == overview.streamIndex) && (index == overview.index);
        });

    if (overviewIt == infos_.overviews.end()) {
        // no match found - add new frame entry with position info
        StreamOverviewEl info;
        info.streamIndex = streamIndex;
        info.index       = index;
        info.timestamp   = timestamp;
        StreamOverviewText posText;
        posText.pos = pos;
        posText.posTexts.push_back(text);
        info.texts.push_back(posText);
        infos_.overviews.push_back(info);
        return;
    }

    auto& overview = *overviewIt;
    auto textIt    = std::find_if(overview.texts.begin(), overview.texts.end(), [pos](const auto& localText) {
        return (localText.pos.x == pos.x) && (localText.pos.y == pos.y);
    });

    if (textIt == overview.texts.end()) {
        // found frame but no matching position - add text with new position
        StreamOverviewText posText;
        posText.pos = pos;
        posText.posTexts.push_back(text);
        overview.texts.push_back(posText);
        return;
    }

    auto& localText = *textIt;
    for (auto& posTextValue : localText.posTexts) {
        if (posTextValue.text == text.text) {
            // same position found same subtitle - update priority
            if (posTextValue.attrPriority > text.attrPriority) {
                posTextValue.attrPriority = text.attrPriority;
            }
            return;
        }
        // new text contains existing (e.g. "person:0.96 #23" contains "person:0.96") ->
        // replace
        if (text.text.find(posTextValue.text) == 0) {
            posTextValue = text;
            return;
        }
        // existing text contains new text -> skip
        if (posTextValue.text.find(text.text) == 0) {
            return;
        }
    }

    // same position no matching subtitle - add to position
    localText.posTexts.push_back(text);
}

void StreamViewerOverview::AddLineToLocal(int64_t streamIndex, uint64_t index, int64_t timestamp,
                                          StreamOverviewLine& line) {
    if (!live_stream_) {
        if ((streamIndex < frame_identity_.streamIndex) || (index < frame_identity_.index)) {
            return;
        }
    } else {
        streamIndex = frame_identity_.streamIndex;
    }
    auto it = std::find_if(infos_.overviews.begin(), infos_.overviews.end(),
                           [streamIndex, index](const auto& overview) {
                               return (streamIndex == overview.streamIndex) && (index == overview.index);
                           });
    if (it == infos_.overviews.end()) {
        // no match found - add new frame
        StreamOverviewEl info;
        info.streamIndex = streamIndex;
        info.index       = index;
        info.timestamp   = timestamp;
        info.lines.push_back(line);
        infos_.overviews.push_back(info);
        return;
    }

    auto& overview = *it;
    auto lineIt = std::find_if(overview.lines.begin(), overview.lines.end(), [&line](const auto& localLine) {
        return (localLine.line.first.x == line.line.first.x) &&
               (localLine.line.first.y == line.line.first.y) &&
               (localLine.line.second.x == line.line.second.x) &&
               (localLine.line.second.y == line.line.second.y);
    });
    if (lineIt != overview.lines.end()) {
        if (lineIt->attrPriority > line.attrPriority) {
            lineIt->attrPriority = line.attrPriority;
        }
        return;
    }

    overview.lines.push_back(line);
}

// Alarm processing methods — moved to StreamViewerAlarm.cc

void StreamViewerOverview::LiveDataHandTarget(int64_t streamIndex, uint64_t index, int64_t timestamp,
                                              MsgTarget& target) {
    LOG_DEBUG("[OSD_TARGET] task:{} stream:{} frame:{} track:{} box=({},{} {}x{}) confs:{} landmarks:{}",
             task_id_, streamIndex, index, target.trackId, target.aiBox.x, target.aiBox.y,
             target.aiBox.width, target.aiBox.height, target.confidence.size(), target.landmark.size());
    // skip invalid targets in tracker LOSS state (confidence = -1 is tracker internal sentinel)
    bool hasValidConfidence = std::any_of(target.confidence.begin(), target.confidence.end(),
                                          [](const auto& conf) { return conf.confidence >= 0; });
    if (!hasValidConfidence) {
        return;
    }

    // determine bounding box color by label
    static const media::Color kBoxColorPalette[] = {
        {34, 211, 238},   // cyan    — face/person
        {249, 115, 22},   // orange  — car/vehicle
        {167, 139, 250},  // purple  — pedestrian
        {52, 211, 153},   // green   — bike/motor
        {251, 113, 133},  // coral   — warning
        {250, 204, 21},   // gold    — special
        {148, 163, 184},  // slate   — other/unknown
    };
    static const size_t kPaletteSize = sizeof(kBoxColorPalette) / sizeof(kBoxColorPalette[0]);

    media::Color boxColor{200, 200, 200};
    if (!target.confidence.empty()) {
        size_t hash = std::hash<std::string>{}(target.confidence[0].label) % kPaletteSize;
        boxColor    = kBoxColorPalette[hash];
    }

    // ── update EMA smooth state (record only, does not change stored coordinates) ──
    if (target.trackId >= 0) {
        float bx = static_cast<float>(target.aiBox.x);
        float by = static_cast<float>(target.aiBox.y);
        float bw = static_cast<float>(target.aiBox.width);
        float bh = static_cast<float>(target.aiBox.height);
        auto it  = smoothed_boxes_.find(target.trackId);
        if (it != smoothed_boxes_.end()) {
            auto& sb             = it->second;
            sb.x                 = kEmaAlpha * bx + (1.0f - kEmaAlpha) * sb.x;
            sb.y                 = kEmaAlpha * by + (1.0f - kEmaAlpha) * sb.y;
            sb.w                 = kEmaAlpha * bw + (1.0f - kEmaAlpha) * sb.w;
            sb.h                 = kEmaAlpha * bh + (1.0f - kEmaAlpha) * sb.h;
            sb.lastSeenTimestamp = timestamp;
        } else {
            smoothed_boxes_[target.trackId] = {bx, by, bw, bh, timestamp};
        }
    }

    // ── Keypoint EMA smoothing: same policy as the box, keyed by trackId. ──
    // Only blends when the landmark count is stable (4 for plate quad, 17 for
    // pose); a changed count reseeds the buffer instead of blending mismatched
    // points. Invalid keypoints (negative coords = not detected) stay raw.
    std::vector<MsgPoint> drawLandmarks = target.landmark;
    if (target.trackId >= 0 && !target.landmark.empty()) {
        auto kit = smoothed_keypoints_.find(target.trackId);
        if (kit != smoothed_keypoints_.end() &&
            kit->second.points.size() == target.landmark.size() &&
            timestamp - kit->second.lastSeenTimestamp <= kSmoothExpireMs) {
            auto& sk = kit->second;
            for (size_t i = 0; i < target.landmark.size(); ++i) {
                const auto& raw = target.landmark[i];
                if (raw.x < 0 || raw.y < 0) {
                    sk.points[i] = {raw.x, raw.y};
                    continue;
                }
                sk.points[i].first  = kEmaAlpha * raw.x + (1.0 - kEmaAlpha) * sk.points[i].first;
                sk.points[i].second = kEmaAlpha * raw.y + (1.0 - kEmaAlpha) * sk.points[i].second;
            }
            sk.lastSeenTimestamp = timestamp;
            drawLandmarks.clear();
            drawLandmarks.reserve(sk.points.size());
            for (const auto& p : sk.points) {
                drawLandmarks.push_back({p.first, p.second});
            }
        } else {
            SmoothedKeypoints seed;
            seed.points.reserve(target.landmark.size());
            for (const auto& raw : target.landmark) {
                seed.points.push_back({raw.x, raw.y});
            }
            seed.lastSeenTimestamp = timestamp;
            smoothed_keypoints_[target.trackId] = std::move(seed);
        }
    }

    // ── Determine drawing coordinates: use EMA-smoothed if tracked, else raw ──
    int drawX = target.aiBox.x;
    int drawY = target.aiBox.y;
    int drawW = target.aiBox.width;
    int drawH = target.aiBox.height;

    if (target.trackId >= 0) {
        auto sit = smoothed_boxes_.find(target.trackId);
        if (sit != smoothed_boxes_.end()) {
            drawX = static_cast<int>(sit->second.x + 0.5f);
            drawY = static_cast<int>(sit->second.y + 0.5f);
            drawW = static_cast<int>(sit->second.w + 0.5f);
            drawH = static_cast<int>(sit->second.h + 0.5f);
        }
    }

    util::Point pointTL(drawX, drawY);
    util::Point pointTR(drawX + drawW, drawY);
    util::Point pointBR(drawX + drawW, drawY + drawH);
    util::Point pointBL(drawX, drawY + drawH);
    VideoOverviewAttrPriority attrPriority = VideoOverviewAttrPriority::kBox;

    int minSide = std::min(drawW, drawH);
    if (minSide < 30) {
        // small targets keep full rectangle, avoid cramped corner brackets
        StreamOverviewLine line1{attrPriority, {pointTL, pointTR}, boxColor};
        StreamOverviewLine line2{attrPriority, {pointTR, pointBR}, boxColor};
        StreamOverviewLine line3{attrPriority, {pointBR, pointBL}, boxColor};
        StreamOverviewLine line4{attrPriority, {pointBL, pointTL}, boxColor};
        AddLineToLocal(streamIndex, index, timestamp, line1);
        AddLineToLocal(streamIndex, index, timestamp, line2);
        AddLineToLocal(streamIndex, index, timestamp, line3);
        AddLineToLocal(streamIndex, index, timestamp, line4);
    } else {
        // corner bracket detection box — only draw L-shaped marks at 4 corners, cleaner look
        int cornerLen = std::clamp(minSide / 4, 15, 50);

        // top-left L
        StreamOverviewLine ltH{attrPriority, {pointTL, {pointTL.x + cornerLen, pointTL.y}}, boxColor};
        StreamOverviewLine ltV{attrPriority, {pointTL, {pointTL.x, pointTL.y + cornerLen}}, boxColor};
        // top-right L
        StreamOverviewLine rtH{attrPriority, {{pointTR.x - cornerLen, pointTR.y}, pointTR}, boxColor};
        StreamOverviewLine rtV{attrPriority, {pointTR, {pointTR.x, pointTR.y + cornerLen}}, boxColor};
        // bottom-right L
        StreamOverviewLine rbH{attrPriority, {{pointBR.x - cornerLen, pointBR.y}, pointBR}, boxColor};
        StreamOverviewLine rbV{attrPriority, {{pointBR.x, pointBR.y - cornerLen}, pointBR}, boxColor};
        // bottom-left L
        StreamOverviewLine lbH{attrPriority, {pointBL, {pointBL.x + cornerLen, pointBL.y}}, boxColor};
        StreamOverviewLine lbV{attrPriority, {{pointBL.x, pointBL.y - cornerLen}, pointBL}, boxColor};

        AddLineToLocal(streamIndex, index, timestamp, ltH);
        AddLineToLocal(streamIndex, index, timestamp, ltV);
        AddLineToLocal(streamIndex, index, timestamp, rtH);
        AddLineToLocal(streamIndex, index, timestamp, rtV);
        AddLineToLocal(streamIndex, index, timestamp, rbH);
        AddLineToLocal(streamIndex, index, timestamp, rbV);
        AddLineToLocal(streamIndex, index, timestamp, lbH);
        AddLineToLocal(streamIndex, index, timestamp, lbV);
    }

    // ── Keypoint overlay driven by semantic kind/schema (carried from inference),
    //    never by guessing from the landmark count alone. ─────────────────────────
    // Plate quads ("plate4") must not jump into the COCO-17 skeleton; pose
    // ("coco17"/"human_pose") renders the skeleton. A count==17 fallback is kept for
    // legacy targets that were recorded before the kind/schema fields existed, and it
    // can never mis-trigger for a 4-point plate quad.
    const bool isPlateQuad = (target.keypointKind == "license_plate") ||
                             (target.keypointSchema == "plate4");
    const bool isPose17 = (target.keypointKind == "human_pose") ||
                          (target.keypointSchema == "coco17");

    bool showPlateCaption = false;
    if (isPlateQuad && target.landmark.size() == 4) {
        // Draw a closed four-corner plate outline plus per-corner tick marks from the
        // plate4 keypoints, ordered TL -> TR -> BR -> BL -> TL (pixel coordinates).
        const media::Color quadColor{244, 114, 182};  // plate-pink, distinguishes plate quad
        const auto quadPts = drawLandmarks;
        for (size_t q = 0; q < 4; ++q) {
            const auto& cur = quadPts[q];
            const auto& nxt = quadPts[(q + 1) % 4];
            if (cur.x < 0 || cur.y < 0 || nxt.x < 0 || nxt.y < 0) {
                continue;
            }
            util::Point pCur(static_cast<int>(cur.x), static_cast<int>(cur.y));
            util::Point pNxt(static_cast<int>(nxt.x), static_cast<int>(nxt.y));
            StreamOverviewLine edge{VideoOverviewAttrPriority::kBox, {pCur, pNxt}, quadColor};
            AddLineToLocal(streamIndex, index, timestamp, edge);
        }
        // Emphasize each corner with a small L-shaped tick.
        for (size_t q = 0; q < 4; ++q) {
            const auto& pt = quadPts[q];
            if (pt.x < 0 || pt.y < 0) {
                continue;
            }
            util::Point centre(static_cast<int>(pt.x), static_cast<int>(pt.y));
            const int tick = std::clamp(minSide / 6, 4, 14);
            StreamOverviewLine h{VideoOverviewAttrPriority::kBox,
                                 {centre, {centre.x + tick, centre.y}}, quadColor};
            StreamOverviewLine v{VideoOverviewAttrPriority::kBox,
                                 {centre, {centre.x, centre.y + tick}}, quadColor};
            AddLineToLocal(streamIndex, index, timestamp, h);
            AddLineToLocal(streamIndex, index, timestamp, v);
        }
        showPlateCaption = true;
    } else if (isPose17 || (!isPlateQuad && target.landmark.size() == 17)) {
        // Pose landmarks are carried in pixel coordinates by MsgTarget. Draw the
        // COCO-17 skeleton in the same overview cache as the detection box. Only
        // record the edges whose two endpoints are both valid so a partially-occluded
        // person never draws disconnected segments outside the frame.
        const media::Color poseColor{255, 180, 0};
        // A joint counts as visible only when its coordinates are valid and, when the
        // model supplied a score, that score clears the cutoff. -1 means "no score
        // available", which must not be read as a failed threshold.
        const auto jointVisible = [&](size_t idx) {
            if (idx >= drawLandmarks.size())
                return false;
            if (drawLandmarks[idx].x < 0 || drawLandmarks[idx].y < 0)
                return false;
            if (idx < target.keypointConfidences.size()) {
                const float score = target.keypointConfidences[idx];
                if (score >= 0.0F && score < kPoseJointMinConfidence)
                    return false;
            }
            return true;
        };
        // Add a small cross at every valid joint so points remain visible even
        // when a limb edge is occluded or the video is scaled down.
        for (size_t joint = 0; joint < drawLandmarks.size(); ++joint) {
            if (!jointVisible(joint))
                continue;
            const util::Point center(static_cast<int>(drawLandmarks[joint].x),
                                     static_cast<int>(drawLandmarks[joint].y));
            constexpr int kJointRadius = 3;
            StreamOverviewLine hLine{VideoOverviewAttrPriority::kBox,
                                     {{center.x - kJointRadius, center.y},
                                      {center.x + kJointRadius, center.y}},
                                     poseColor};
            StreamOverviewLine vLine{VideoOverviewAttrPriority::kBox,
                                     {{center.x, center.y - kJointRadius},
                                      {center.x, center.y + kJointRadius}},
                                     poseColor};
            AddLineToLocal(streamIndex, index, timestamp, hLine);
            AddLineToLocal(streamIndex, index, timestamp, vLine);
        }
        for (const auto& edge : kCocoPoseEdges) {
            if (!jointVisible(static_cast<size_t>(edge[0])) || !jointVisible(static_cast<size_t>(edge[1])))
                continue;
            const auto& p1 = drawLandmarks[static_cast<size_t>(edge[0])];
            const auto& p2 = drawLandmarks[static_cast<size_t>(edge[1])];
            StreamOverviewLine poseLine{VideoOverviewAttrPriority::kBox,
                                        {{static_cast<int>(p1.x), static_cast<int>(p1.y)},
                                         {static_cast<int>(p2.x), static_cast<int>(p2.y)}},
                                        poseColor};
            AddLineToLocal(streamIndex, index, timestamp, poseLine);
        }
    }

    bool trackIdShown = false;
    for (auto& confidence : target.confidence) {
        // skip invalid confidence entries
        if (confidence.confidence < 0) {
            continue;
        }
        util::Point pos(drawX, drawY);  // label position follows smoothed box
        StreamOverviewTextEl text;
        text.attrPriority = VideoOverviewAttrPriority::kConfidence;
        text.bgColor      = {5, 8, 22};  // near-black blue bg #050816
        text.hasBgColor   = true;

        // set text color by confidence
        text.hasColor = true;
        if (confidence.confidence >= 0.85f) {
            text.color = {255, 209, 102};  // gold #FFD166 — high confidence
        } else if (confidence.confidence >= 0.6f) {
            text.color = {220, 231, 255};  // cool white-blue #DCE7FF — medium confidence
        } else {
            text.color = {138, 148, 184};  // slate #8A94B8 — low confidence
        }

        if (!trackIdShown && target.trackId >= 0) {
            text.text =
                COSMO_FORMAT("{}:{:.2f} #{}", confidence.label, confidence.confidence, target.trackId);
            trackIdShown = true;
        } else {
            text.text = COSMO_FORMAT("{}:{:.2f}", confidence.label, confidence.confidence);
        }
        AddTextToLocal(streamIndex, index, timestamp, pos, text);
    }

    // ── Level-1 plate caption: draw the recognized number and the color label below
    //    the plate. The number is never fabricated: if ocrString is empty (recognition
    //    had no confident text) only the box/quads and color remain visible. ───────────
    if (showPlateCaption) {
        std::string colorLabel;
        float colorConfidence = 0.0F;
        for (const auto& attr : target.attrs) {
            if (attr.category == "plateColor" && !attr.label.empty()) {
                colorLabel      = attr.label;
                colorConfidence = attr.confidence;
                break;
            }
        }
        std::string caption = target.ocrString;
        if (caption.empty()) {
            caption = colorLabel;  // no number trust → show only the color label
        } else if (!colorLabel.empty()) {
            caption += " [" + colorLabel + "]";
        }
        // Append the real number/color confidences from the recognizer. The number is
        // never fabricated: when ocrString is empty only the color (and its confidence)
        // is shown.
        if (!target.ocrString.empty() && target.ocrConfidence > 0.0F) {
            caption += COSMO_FORMAT(" (num:{:.2f}", target.ocrConfidence);
            if (colorConfidence > 0.0F) {
                caption += COSMO_FORMAT(" col:{:.2f})", colorConfidence);
            } else {
                caption += ")";
            }
        } else if (colorConfidence > 0.0F) {
            caption += COSMO_FORMAT(" (col:{:.2f})", colorConfidence);
        }
        if (!caption.empty()) {
            util::Point plateTextPos(drawX, drawY + drawH + 12);
            StreamOverviewTextEl plateText;
            plateText.attrPriority = VideoOverviewAttrPriority::kConfidence;
            plateText.text         = caption;
            plateText.bgColor      = {5, 8, 22};
            plateText.hasBgColor   = true;
            plateText.hasColor     = true;
            if (!target.ocrString.empty()) {
                plateText.color = {255, 209, 102};  // gold — number trusted
            } else {
                plateText.color = {138, 148, 184};  // slate — color only, number absent
            }
            AddTextToLocal(streamIndex, index, timestamp, plateTextPos, plateText);
        }
    }
}

void StreamViewerOverview::LiveDataAiFrameToLocal(std::vector<MsgAiDetFrame>& aiDatas) {
    for (auto& aiData : aiDatas) {
        LOG_DEBUG("[OSD_FRAME] task:{} stream:{} frame:{} targets:{} ts:{}",
                 task_id_, aiData.streamIndex, aiData.index, aiData.targets.size(), aiData.timestamp);
        for (auto& target : aiData.targets) {
            LiveDataHandTarget(aiData.streamIndex, aiData.index, aiData.timestamp, target);
        }
    }
}

void StreamViewerOverview::LiveDataToLocal() {
    auto liveDatas =
        service::ServiceRegistry::Instance().Get<service::ITaskQuery>().GetTaskLiveOverviewInfo(task_id_);
    LOG_DEBUG("[OSD_QUERY] task:{} live_records:{}", task_id_, liveDatas.size());

    // ── Collect all AIData frames from every pipeline action, then deduplicate ──
    // Multiple actions (Track, Classify, Logic, ...) each record the same target;
    // we merge by (streamIndex, index) and per-target by trackId, keeping the
    // version with the richest confidence list (most downstream stage).
    std::map<std::pair<int64_t, int64_t>, MsgAiDetFrame> mergedFrames;

    for (auto& livedata : liveDatas) {
        if (MsgOverviewMemDataType::MsgOverviewMemDataTypeAIData == livedata.type) {
            for (auto& aiFrame : livedata.aiFrames) {
                auto key = std::make_pair(aiFrame.streamIndex, aiFrame.index);
                auto it  = mergedFrames.find(key);
                if (it == mergedFrames.end()) {
                    mergedFrames[key] = aiFrame;
                } else {
                    // Same frame from a different action: merge targets by trackId
                    for (auto& newTarget : aiFrame.targets) {
                        bool merged = false;
                        if (newTarget.trackId >= 0) {
                            auto targetIt = std::find_if(it->second.targets.begin(), it->second.targets.end(),
                                                         [&](const auto& existTarget) {
                                                             return existTarget.trackId == newTarget.trackId;
                                                         });
                            if (targetIt != it->second.targets.end()) {
                                if (newTarget.confidence.size() > targetIt->confidence.size()) {
                                    targetIt->confidence = newTarget.confidence;
                                    targetIt->attrs      = newTarget.attrs;
                                }
                                // The tracked result is the authoritative OSD target. Preserve
                                // pose/face/plate keypoints when merging another pipeline stage.
                                if (!newTarget.landmark.empty() && targetIt->landmark.empty()) {
                                    targetIt->landmark = newTarget.landmark;
                                    // Carry the semantic family along with the points. Copying
                                    // only the coordinates drops the kind/schema and forces the
                                    // renderer to guess the family from the point count.
                                    targetIt->keypointKind   = newTarget.keypointKind;
                                    targetIt->keypointSchema = newTarget.keypointSchema;
                                }
                                merged = true;
                            }
                        }
                        if (!merged) {
                            it->second.targets.push_back(newTarget);
                        }
                    }
                }
            }
        } else if (MsgOverviewMemDataType::MsgOverviewMemDataTypeSensitity == livedata.type ||
                   MsgOverviewMemDataType::MsgOverviewMemDataTypePosSaveSensitity == livedata.type ||
                   MsgOverviewMemDataType::MsgOverviewMemDataTypeAiFilter == livedata.type) {
            // Sensitivity and filter records currently do not produce live OSD elements.
            continue;
        } else if (MsgOverviewMemDataType::MsgOverviewMemDataTypeAlarm == livedata.type) {
            LiveDataAlarmToLocal(livedata.alarms);
        } else if (MsgOverviewMemDataType::MsgOverviewMemDataTypeParams == livedata.type) {
            params_.areas = livedata.params.areas;
        }
    }

    // ── Process deduplicated AI frames ──
    if (!mergedFrames.empty()) {
        std::vector<MsgAiDetFrame> deduped;
        deduped.reserve(mergedFrames.size());
        for (auto& [k, frame] : mergedFrames) {
            // Remove untracked targets (trackId<0, from Detect action) that overlap
            // with tracked targets (trackId>=0, from Track/Classify). The tracked version
            // is always more informative and already includes the detection bbox.
            auto& tgts = frame.targets;
            tgts.erase(
                std::remove_if(tgts.begin(), tgts.end(),
                               [this, &tgts](const MsgTarget& t) { return IsOverlappingTracked(t, tgts); }),
                tgts.end());
            deduped.push_back(std::move(frame));
        }
        LiveDataAiFrameToLocal(deduped);
    }
}

bool StreamViewerOverview::IsOverlappingTracked(const MsgTarget& target,
                                                const std::vector<MsgTarget>& targets) {
    if (target.trackId >= 0) {
        return false;  // keep tracked
    }
    // check if any tracked target has a similar bbox
    for (const auto& tracked : targets) {
        if (tracked.trackId < 0) {
            continue;
        }
        int dx   = std::abs(target.aiBox.x - tracked.aiBox.x);
        int dy   = std::abs(target.aiBox.y - tracked.aiBox.y);
        int dw   = std::abs(target.aiBox.width - tracked.aiBox.width);
        int dh   = std::abs(target.aiBox.height - tracked.aiBox.height);
        int side = std::max(tracked.aiBox.width, tracked.aiBox.height);
        int tol  = std::max(side / 5, 10);  // 20% tolerance
        if (dx < tol && dy < tol && dw < tol && dh < tol) {
            return true;  // overlapping untracked → remove
        }
    }
    return false;
}

}  // namespace cosmo
