#include "nn/utils/yolo_pose_decode.h"

#include <algorithm>
#include <cmath>

namespace cosmo::nn {
namespace {
float Sigmoid(float value) { return 1.0f / (1.0f + std::exp(-value)); }
}

Status DecodeYoloPoseTensor(const float* data, const std::vector<int>& shape, int input_width, int input_height,
                            int class_count, int keypoint_count, float confidence_threshold,
                            std::vector<ObjectInfoV1>& outputs, std::string& error,
                            int keypoint_values_per_point) {
    outputs.clear();
    if (!data || input_width <= 0 || input_height <= 0 || class_count <= 0 || keypoint_count <= 0 ||
        !std::isfinite(confidence_threshold) || (keypoint_values_per_point != 2 && keypoint_values_per_point != 3))
        return Status(COSMO_NN_ERR_INVALID_INPUT, "invalid YOLO pose decode arguments");
    if (shape.size() != 3 || shape[0] != 1)
        return Status(COSMO_NN_ERR_INVALID_INPUT, "YOLO pose tensor must have batch 1 and rank 3");

    const int channel_major_channels = 4 + class_count + keypoint_count * keypoint_values_per_point;
    const int end_to_end_channels = 6 + keypoint_count * keypoint_values_per_point;
    const bool channel_major = shape[1] == channel_major_channels;
    const bool row_major = shape[2] == end_to_end_channels;
    if (!channel_major && !row_major) {
        error = "YOLO pose tensor shape does not match configured classes/keypoints";
        return Status(COSMO_NN_ERR_INVALID_INPUT, error);
    }
    const int points = channel_major ? shape[2] : shape[1];
    const auto at = [&](int point, int channel) {
        return channel_major ? data[channel * points + point] : data[point * end_to_end_channels + channel];
    };
    for (int point = 0; point < points; ++point) {
        int best_class = 0;
        float confidence = 0.0f;
        float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
        int keypoint_offset = 0;
        if (channel_major) {
            float best_score = at(point, 4);
            for (int cls = 1; cls < class_count; ++cls) {
                const float score = at(point, 4 + cls);
                if (score > best_score) {
                    best_score = score;
                    best_class = cls;
                }
            }
            confidence = best_score > 1.0f ? Sigmoid(best_score) : best_score;
            const float cx = at(point, 0);
            const float cy = at(point, 1);
            const float width = at(point, 2);
            const float height = at(point, 3);
            x1 = cx - width * 0.5f;
            y1 = cy - height * 0.5f;
            x2 = cx + width * 0.5f;
            y2 = cy + height * 0.5f;
            keypoint_offset = 4 + class_count;
        } else {
            confidence = at(point, 4);
            best_class = static_cast<int>(at(point, 5));
            x1 = at(point, 0);
            y1 = at(point, 1);
            x2 = at(point, 2);
            y2 = at(point, 3);
            keypoint_offset = 6;
        }
        if (confidence < confidence_threshold)
            continue;
        if (!std::isfinite(confidence) || !std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) ||
            !std::isfinite(y2) || x2 <= x1 || y2 <= y1) {
            continue;
        }
        ObjectInfoV1 object;
        object.x1 = std::max(0.0f, x1);
        object.y1 = std::max(0.0f, y1);
        object.x2 = std::min(static_cast<float>(input_width - 1), x2);
        object.y2 = std::min(static_cast<float>(input_height - 1), y2);
        object.key_points.reserve(static_cast<size_t>(keypoint_count));
        object.key_point_confidences.reserve(static_cast<size_t>(keypoint_count));
        for (int keypoint = 0; keypoint < keypoint_count; ++keypoint) {
            const int offset = keypoint_offset + keypoint * keypoint_values_per_point;
            const float keypoint_x = at(point, offset);
            const float keypoint_y = at(point, offset + 1);
            if (!std::isfinite(keypoint_x) || !std::isfinite(keypoint_y)) {
                object.key_points.emplace_back(0.0F, 0.0F);
                object.key_point_confidences.push_back(-1.0F);
                continue;
            }
            object.key_points.emplace_back(
                std::clamp(keypoint_x, 0.0F, static_cast<float>(input_width - 1)),
                std::clamp(keypoint_y, 0.0F, static_cast<float>(input_height - 1)));
            object.key_point_confidences.push_back(keypoint_values_per_point == 3
                                                       ? Sigmoid(at(point, offset + 2))
                                                       : -1.0f);
        }
        object.infos.push_back({confidence, best_class, std::string("class_") + std::to_string(best_class)});
        outputs.push_back(std::move(object));
    }
    return COSMO_NN_OK;
}
}  // namespace cosmo::nn
