#pragma once

#include <memory>

#include "nn/core/status.h"
#include "nn/utils/rect.h"
#include "nn/utils/tracker_common.h"

namespace cosmo::nn {

class DefaultTracker;

class PUBLIC TrackerWrap {
public:
    TrackerWrap();
    ~TrackerWrap();

    /**
     * @return 0 if success, otherwise fail
     */
    Status GetRegion(Rect2f& region);
    Status SetRegion(Rect2f& region);

    /**
     * @return 0 if success, otherwise fail
     */
    Status GetTrackerConfig(TrackerConfig& c);
    Status SetTrackerConfig(TrackerConfig& c);

    /**
     * @param input
     * @param output
     * @return 0 if success, otherwise fail
     */
    Status Trace(const std::vector<TrackingBox>& input, std::vector<TrackingBox>& output);

private:
    std::shared_ptr<DefaultTracker> impl = nullptr;
};

}  // namespace cosmo::nn
