#pragma once

#include <string>
#include <vector>

#include "nn/utils/net_utils.h"

namespace cosmo::nn {

// Decode an Ultralytics pose tensor in either [1, 4+classes+3*K, points]
// (YOLOv8/11) or [1, points, 6+3*K] (YOLO26 end-to-end) layout.
Status DecodeYoloPoseTensor(const float* data, const std::vector<int>& shape, int input_width, int input_height,
                            int class_count, int keypoint_count, float confidence_threshold,
                            std::vector<ObjectInfoV1>& outputs, std::string& error,
                            int keypoint_values_per_point = 3);

}  // namespace cosmo::nn
