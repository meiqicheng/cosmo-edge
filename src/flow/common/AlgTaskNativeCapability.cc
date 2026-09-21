#include "flow/common/AlgTaskNativeCapability.h"

#include <algorithm>

#include "flow/common/AlgDataQueueDistributor.h"
#include "util/dto/ActionCodes.h"

namespace cosmo {
namespace {

    constexpr AlgTaskNativeCapability kFailClosed{};

    // Actions that consume detection geometry and frame identity only. They
    // never dereference decoded pixels, so they run unchanged on the
    // native-only (DMA-BUF) path where the host frame is intentionally absent;
    // frame identity comes from AlgFrameMeta via ResolveAlgFrameInfo.
    AlgTaskNativeCapability ResolveBoxOnlyAtomic() {
        AlgTaskNativeCapability capability;
        capability.supports_native_input           = true;
        capability.requires_host_frame             = false;
        capability.requires_alarm_media            = false;
        capability.requires_crop_or_classification = false;
        return capability;
    }

}  // namespace

AlgTaskNativeCapability ResolveAlgTaskNativeCapability(std::string_view actionId) {
    if (actionId == AADetect_Code || actionId == AATrack_Code || actionId == BAFilter_Code) {
        return ResolveBoxOnlyAtomic();
    }
    // Sensitivity variants score detection geometry (boxes, track ids, frame
    // identity) only; their HandFrame reads are null-safe on the native-only
    // path (measured 2026-09-20, Sensitivity.cc / PosSaveSensitivityCalc.cc).
    if (actionId == BASensitivity_Code || actionId == BAFixCountSensitivity_Code) {
        return ResolveBoxOnlyAtomic();
    }
    // TaskAlarm's per-frame HandFrame reads only frame width/height (null-safe
    // fallback) and combines alarm geometry. Its media generation runs at
    // alarm-event time; native-only frames are materialized on demand through
    // the P1-2 alarm-materialization gate (MaterializeNativeBuffer).
    if (actionId == BATaskAlarm_Code) {
        return ResolveBoxOnlyAtomic();
    }
    // Atomic classification (helmet two-stage) consumes pixels only through
    // the native DMA-BUF blob path (ConvertImagesToBlobs + RKNN preprocess
    // RGA crop); it never requires a host frame. AcceptClassifyNativeData in
    // AiClassifier gates the action-side frame check.
    if (actionId == AAClassify_Code) {
        return ResolveBoxOnlyAtomic();
    }
    // Logical judgment combines classify/track geometry via the calc engine —
    // no pixel access (LogicalJudgment::HandFrame).
    if (actionId == BALogicalJudgment_Code) {
        return ResolveBoxOnlyAtomic();
    }
    // BAPositiveSaveSensitivity retains frame references (idData.frame) for
    // deferred picture generation; enabled together with the P1-2 on-demand
    // materialization helper (materialize at alarm-candidate time only).
    if (actionId == BAPositiveSaveSensitivity_Code) {
        return ResolveBoxOnlyAtomic();
    }
    return kFailClosed;
}

bool AlgTasksNativeOnlyEligible(const std::vector<AlgTaskUnit>& tasks) {
    return !tasks.empty() && std::all_of(tasks.begin(), tasks.end(), [](const AlgTaskUnit& task) {
        return !task.requires_host_frame &&
               ResolveAlgTaskNativeCapability(task.actionId).NativeOnlyEligible();
    });
}

}  // namespace cosmo
