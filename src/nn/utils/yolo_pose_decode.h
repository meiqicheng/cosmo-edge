#pragma once

#include <string>
#include <vector>

#include "nn/utils/net_utils.h"

namespace cosmo::nn {

// Decode an Ultralytics pose tensor in either [1, 4+classes+3*K, points]
// (YOLOv8/11) or [1, points, 6+3*K] (YOLO26 end-to-end) layout.
//
// Produced boxes and keypoints are expressed in MODEL INPUT space
// (input_width x input_height), not in original frame space. Callers that render
// or compare against the source frame must project them back themselves.
// A keypoint the model did not find is emitted as (-1, -1) so that renderers can
// skip it with a simple negative-coordinate test.
Status DecodeYoloPoseTensor(const float* data, const std::vector<int>& shape, int input_width, int input_height,
                            int class_count, int keypoint_count, float confidence_threshold,
                            std::vector<ObjectInfoV1>& outputs, std::string& error,
                            int keypoint_values_per_point = 3);

// Project decoded boxes and keypoints from model-input space onto the original
// frame. `net_size` is the model input (e.g. 640x640) and `frame_size` the source
// frame (e.g. 1920x1080). `gravity` must mirror the preprocessing resize op:
//   0 — stretch  : per-axis scale, no padding
//   1 — letterbox, content centered inside the padded input
//   2 — letterbox, content top-left aligned
// Detection pipelines cover this with NetUtils::AdjustSize, but that helper
// remaps the bounding box only and leaves key_points untouched, so pose needs its
// own projection. Without it a 1080p frame draws the skeleton inside a 640x640
// corner instead of onto the person. Keypoints already carrying the "not
// detected" sentinel stay negative.
void MapYoloPoseToFrame(std::vector<ObjectInfoV1>& objects, Size net_size, Size frame_size, int gravity);

}  // namespace cosmo::nn
