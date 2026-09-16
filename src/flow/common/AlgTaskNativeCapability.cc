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
    return kFailClosed;
}

bool AlgTasksNativeOnlyEligible(const std::vector<AlgTaskUnit>& tasks) {
    return !tasks.empty() && std::all_of(tasks.begin(), tasks.end(), [](const AlgTaskUnit& task) {
        return !task.requires_host_frame &&
               ResolveAlgTaskNativeCapability(task.actionId).NativeOnlyEligible();
    });
}

}  // namespace cosmo
