#pragma once

#include <string>
#include <vector>

#include "nn/utils/net_utils.h"

namespace cosmo::nn {

// Concrete tensor layout of a pose head. It must be declared by the model
// template (decoder / keypoint_layout / output_layout) and never guessed from the
// output shape: a 4-point plate head and a 2-class detection head can both expose
// channels that accidentally satisfy the other layout's arithmetic.
enum class YoloPoseLayout {
    // [1, 4 + class_count + K*V, points] — YOLOv8/11 classic tensor head.
    kChannelMajor,
    // [1, points, 6 + K*V] — YOLO26 and other NMS-free end-to-end heads.
    kEndToEnd,
};

// Channel count a pose output must expose for the declared layout, where K is the
// keypoint count and V the values per point. Used both by the decoder and by the
// pipeline that validates a template against its declared output shape.
int PoseChannelCount(YoloPoseLayout layout, int class_count, int keypoint_count,
                     int keypoint_values_per_point);

// Decode an Ultralytics pose tensor in either [1, 4+classes+V*K, points]
// (YOLOv8/11, kChannelMajor) or [1, points, 6+V*K] (YOLO26 end-to-end,
// kEndToEnd). The layout is taken from `layout` as declared by the model template;
// a tensor whose channels do not match the declared layout is rejected instead of
// silently decoding with the other layout.
//
// Produced boxes and keypoints are expressed in MODEL INPUT space
// (input_width x input_height), not in original frame space. Callers that render
// or compare against the source frame must project them back themselves.
// A keypoint the model did not find is emitted as (-1, -1) so that renderers can
// skip it with a simple negative-coordinate test.
Status DecodeYoloPoseTensor(const float* data, const std::vector<int>& shape, YoloPoseLayout layout,
                            int input_width, int input_height, int class_count, int keypoint_count,
                            float confidence_threshold, std::vector<ObjectInfoV1>& outputs,
                            std::string& error, int keypoint_values_per_point = 3);

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
