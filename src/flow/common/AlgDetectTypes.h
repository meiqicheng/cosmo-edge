// Algorithm Detection and Task Types definitions

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "flow/common/AlgAlarmTypes.h"
#include "flow/common/AlgDataType.h"
#include "infer/AiCommon.h"
#include "media/NativeVideoBuffer.h"
#include "media/VideoFrame.h"
#include "media/VideoPacket.h"
#include "util/Rect.h"
#include "util/VideoInfo.h"

namespace cosmo {

struct AlgChannelDataOrig {
    float fps{0.0};
    VideoPacketPtr packet;
};

/// Frame identity captured by the decode stage before any Materialize
/// decision. Host-frame and native-only (DMA-BUF) paths must carry identical
/// stream/frame/timestamp identity so downstream tasks cannot tell the paths
/// apart. `valid` is explicit: consumers must never fall back to all-zero
/// identity when metadata is missing.
struct AlgFrameMeta {
    bool valid{false};
    int64_t streamIndex{0};
    int64_t frameIndex{0};
    int64_t timestamp{0};
    int width{0};   // Coordinate-space width of the attached pixel source.
    int height{0};  // Coordinate-space height of the attached pixel source.
    media::PixelFormat pixelFormat{media::PixelFormat::PIXEL_UNKNOWN};
};

struct AlgChannelDataDec {
    VideoFramePtr frame;  // AI processed frame, might be YUV or BGR depending on platform
    media::NativeVideoBufferPtr native_buffer;  // Optional borrowed hardware inference source.
    AlgFrameMeta meta;
    int64_t reportTimeStamp{0};
};

/// Frame identity resolved from whichever pixel source the decode stage attached.
///
/// The host-frame path exposes geometry and identity through `frame`; the
/// native-only (DMA-BUF) path materializes no host frame and carries the same
/// values in `meta`. Downstream box-only actions must not observe a difference,
/// so they resolve here instead of dereferencing `frame` directly (which is
/// null on the native-only path).
struct AlgResolvedFrameInfo {
    int width{media::kVideoDefaultWidth};
    int height{media::kVideoDefaultHeight};
    int64_t streamIndex{0};
    int64_t frameIndex{0};
    int64_t timestamp{0};
};

inline AlgResolvedFrameInfo ResolveAlgFrameInfo(const AlgChannelDataDec& dec) {
    AlgResolvedFrameInfo info;
    if (dec.frame && dec.frame->Active()) {
        info.width       = static_cast<int>(dec.frame->GetWidth());
        info.height      = static_cast<int>(dec.frame->GetHeight());
        info.streamIndex = dec.frame->GetStreamIndex();
        info.frameIndex  = static_cast<int64_t>(dec.frame->GetFrameIndex());
        info.timestamp   = dec.frame->GetTimestamp();
        return info;
    }
    if (dec.meta.valid) {
        info.width       = dec.meta.width;
        info.height      = dec.meta.height;
        info.streamIndex = dec.meta.streamIndex;
        info.frameIndex  = dec.meta.frameIndex;
        info.timestamp   = dec.meta.timestamp;
    }
    return info;
}

struct DataDetTrackClassify {
    bool bHaveArea{false};
    bool bHaveShieldedArea{false};
    int64_t streamIndex{0};
    int64_t frameIndex{0};
    int64_t timestamp{0};
    int picWidth{media::kVideoDefaultWidth};
    int picHeight{media::kVideoDefaultHeight};
    AlgDataType dataType{AlgDataType::ChannelDataDetect};

    OnEventsReportType reportType{OnEventsReportType::Realtime};
    AlgTaskDataFaceLogicAreaInfo areaInfo;  // Effective only when OnEventsReportTypeTrigger

    std::vector<AiDetectRstEl> targets;
    bool targetHaveMultRelated{false};
    std::vector<AiGroupEl> groupTargets;
};
using DataDetTrackClassifyPtr = std::shared_ptr<DataDetTrackClassify>;

struct AlgChannelDataDetect {
    std::vector<std::string> lables;
    std::string atomicCode;
    std::vector<std::string> atomicCodes;
    DataDetTrackClassifyPtr detRet{nullptr};
};

struct AlgTaskDataTrack {
    DataDetTrackClassifyPtr trackRst{nullptr};
};

struct AlgTaskDataClassify {
    DataDetTrackClassifyPtr classifyRst{nullptr};
};

struct AlgTaskDataFaceLogic {
    DataDetTrackClassifyPtr faceLogicRst{nullptr};
};

struct AlgTaskDataLandmark {
    DataDetTrackClassifyPtr landmarkRst{nullptr};
};

struct AlgTaskDataFriendDistance {
    DataDetTrackClassifyPtr friendsRst{nullptr};
};

struct AlgTaskDataAssoTarget {
    DataDetTrackClassifyPtr assoRst{nullptr};
};

struct AlgTaskDataClassifyMultPic {
    VideoFramePtr baseFrame;
    DataDetTrackClassifyPtr classifyRst{nullptr};
};

struct AlgTaskDataRecogThings {
    std::string areaId;
    std::string areaName;
    util::Box box;
    bool bLogicResult{false};  // Reused by filtering module. true: moving, false: stationary
    float average{-1.0};       // Filtering result
    AIMotionState motionStatus{AIMotionState::UNCERTAIN};  // Filtering motion state
    AiDetectMatchHighScoreInfo matchInfo;  // Highest score match info during Reid algorithm comparison
};

struct AlgTaskDataRecog {
    std::vector<AlgTaskDataRecogThings> areas;
};

struct AlgTaskDataAiFilter {
    bool bInputArea{false};
    std::vector<AlgTaskDataRecogThings> areas;   // Effective only when bInputArea is true
    DataDetTrackClassifyPtr targetRst{nullptr};  // Effective when bInputArea is false
};

struct AlgTaskDataAiVideoQuality {
    DataDetTrackClassifyPtr targetRst{nullptr};  // Effective when bInputArea is false
};

struct AlgTaskDataFilter {
    DataFilterPtr filterRst{nullptr};
};

struct AlgTaskDataAlarm {
    DataAlarmPtr alarmData{nullptr};
};

}  // namespace cosmo
