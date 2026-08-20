#ifdef COSMO_NN_USE_AXERA_BACKEND

#include "nn/device/axera/axera_yolov8_adapter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cosmo::nn {
namespace {

    bool ParseNchw(const std::vector<int>& shape, int& channels, int& height, int& width) {
        if (shape.size() != 4 || shape[0] != 1 || shape[1] <= 0 || shape[2] <= 0 || shape[3] <= 0)
            return false;
        channels = shape[1];
        height   = shape[2];
        width    = shape[3];
        return true;
    }

    bool ParseNhwcPacked(const std::vector<int>& shape, int& height, int& width, int& packed_channels) {
        // {1, h, w, 64 + cls} or {1, h*w, 64+cls}
        if (shape.size() == 4 && shape[0] == 1 && shape[1] > 0 && shape[2] > 0 && shape[3] > 0) {
            height = shape[1];
            width  = shape[2];
            packed_channels = shape[3];
            return true;
        }
        if (shape.size() == 3 && shape[0] == 1 && shape[1] > 0 && shape[2] > 0) {
            // {1, h*w, 64+cls}: reinterpret as h = points, w = 1
            const int points = shape[1];
            const int side   = static_cast<int>(std::sqrt(points));
            if (side * side == points) {
                height = side;
                width  = side;
            } else {
                height = points;
                width  = 1;
            }
            packed_channels = shape[2];
            return true;
        }
        return false;
    }

    size_t ShapeCount(const std::vector<int>& shape) {
        size_t count = 1;
        for (int dim : shape) {
            if (dim <= 0 || count > std::numeric_limits<size_t>::max() / static_cast<size_t>(dim))
                return 0;
            count *= static_cast<size_t>(dim);
        }
        return count;
    }

    float Sigmoid(float value) {
        if (value >= 0.0f)
            return 1.0f / (1.0f + std::exp(-value));
        const float exponential = std::exp(value);
        return exponential / (1.0f + exponential);
    }

    // DFL softmax-weighted bin distance for one side.
    bool DflDistance(const float* data, int spatial_count, int spatial_index, float* distance) {
        constexpr int kRegMax = 16;
        float maximum = -std::numeric_limits<float>::infinity();
        for (int bin = 0; bin < kRegMax; ++bin)
            maximum = std::max(maximum, data[spatial_index + bin * spatial_count]);
        float denominator = 0.0f;
        float numerator   = 0.0f;
        for (int bin = 0; bin < kRegMax; ++bin) {
            const float value = std::exp(data[spatial_index + bin * spatial_count] - maximum);
            denominator += value;
            numerator += value * static_cast<float>(bin);
        }
        if (!(denominator > 0.0f) || !std::isfinite(denominator))
            return false;
        *distance = numerator / denominator;
        return true;
    }

    // DFL for one side on a packed NHWC row layout: row stride is
    // 4*reg_max + class_count, box bins are row-major.
    bool DflDistancePacked(const float* row, int stride, int side, float* distance) {
        constexpr int kRegMax = 16;
        const float* base     = row + side * kRegMax;
        float maximum         = -std::numeric_limits<float>::infinity();
        for (int bin = 0; bin < kRegMax; ++bin)
            maximum = std::max(maximum, base[bin]);
        float denominator = 0.0f;
        float numerator   = 0.0f;
        for (int bin = 0; bin < kRegMax; ++bin) {
            const float value = std::exp(base[bin] - maximum);
            denominator += value;
            numerator += value * static_cast<float>(bin);
        }
        if (!(denominator > 0.0f) || !std::isfinite(denominator))
            return false;
        *distance = numerator / denominator;
        return true;
    }

}  // namespace

bool DetectAxeraYolov8Layout(const std::vector<std::vector<int>>& shapes, AxeraYolov8Layout& layout,
                             std::string& error) {
    layout = AxeraYolov8Layout{};
    if (shapes.size() != 3 && shapes.size() != 6) {
        error = "AXERA YOLOv8 requires 3 packed heads or 6 NCHW split tensors";
        return false;
    }

    const bool nchw_separate = shapes.size() == 6;
    int class_count          = 0;
    int point_count          = 0;
    int previous_points      = std::numeric_limits<int>::max();

    for (size_t branch = 0; branch < 3; ++branch) {
        AxeraYolov8BranchContract contract;
        contract.nchw_separate = nchw_separate;
        int height = 0, width = 0;

        if (nchw_separate) {
            const size_t box_index = branch * 2;
            const size_t cls_index = branch * 2 + 1;
            int box_channels = 0, cls_channels = 0;
            int box_height = 0, box_width = 0, cls_height = 0, cls_width = 0;
            if (!ParseNchw(shapes[box_index], box_channels, box_height, box_width) ||
                !ParseNchw(shapes[cls_index], cls_channels, cls_height, cls_width)) {
                error = "AXERA YOLOv8 NCHW heads must be static {1,C,H,W} tensors";
                return false;
            }
            if (box_channels != 64 || box_height != cls_height || box_width != cls_width) {
                error = "AXERA YOLOv8 NCHW box/class head dimensions do not match";
                return false;
            }
            if (cls_channels <= 0 || (class_count != 0 && cls_channels != class_count)) {
                error = "AXERA YOLOv8 class counts are inconsistent";
                return false;
            }
            contract.tensor_index = box_index;  // class tensor is tensor_index+1
            height               = box_height;
            width                = box_width;
            class_count          = cls_channels;
        } else {
            int packed_channels = 0;
            if (!ParseNhwcPacked(shapes[branch], height, width, packed_channels)) {
                error = "AXERA YOLOv8 packed head must be {1,H,W,64+cls} or {1,H*W,64+cls}";
                return false;
            }
            if (packed_channels < 64 + 1) {
                error = "AXERA YOLOv8 packed head has too few channels";
                return false;
            }
            const int cls_channels = packed_channels - 64;
            if (class_count != 0 && cls_channels != class_count) {
                error = "AXERA YOLOv8 packed class counts are inconsistent";
                return false;
            }
            contract.tensor_index = branch;
            class_count           = cls_channels;
        }

        const int points = height * width;
        if (points >= previous_points || points <= 0) {
            error = "AXERA YOLOv8 heads must be ordered from fine to coarse stride";
            return false;
        }
        previous_points = points;
        contract.height = height;
        contract.width  = width;
        // Infer stride from the fine-to-coarse ratio (8/16/32 for 640 input).
        contract.stride = (previous_points == std::numeric_limits<int>::max())
                              ? 0
                              : static_cast<int>(std::sqrt(static_cast<double>(previous_points) /
                                                           static_cast<double>(points)) * 8);
        if (contract.stride <= 0 || (contract.stride & (contract.stride - 1)) != 0)
            contract.stride = 8 * (1 << branch);

        layout.branches.push_back(contract);
        if (point_count > std::numeric_limits<int>::max() - points) {
            error = "AXERA YOLOv8 point count overflows";
            return false;
        }
        point_count += points;
    }

    layout.detected                 = true;
    layout.class_count              = class_count;
    layout.point_count              = point_count;
    layout.class_scores_are_probabilities = false;
    layout.logical_shape            = {1, 4 + class_count, point_count};
    return true;
}

bool ReconstructAxeraYolov8(const std::vector<AxeraYolov8Head>& heads, int input_height, int input_width,
                            float* output, size_t output_count, std::string& error) {
    if (heads.empty() || !output || input_height <= 0 || input_width <= 0) {
        error = "AXERA YOLOv8 adapter received invalid arguments";
        return false;
    }
    std::vector<std::vector<int>> shapes;
    shapes.reserve(heads.size());
    for (const auto& head : heads)
        shapes.push_back(head.shape);

    AxeraYolov8Layout layout;
    if (!DetectAxeraYolov8Layout(shapes, layout, error))
        return false;
    const size_t required =
        static_cast<size_t>(4 + layout.class_count) * static_cast<size_t>(layout.point_count);
    if (output_count < required) {
        error = "AXERA YOLOv8 logical output buffer is too small";
        return false;
    }

    const bool nchw_separate = layout.branches.front().nchw_separate;
    int point_offset         = 0;
    for (size_t b = 0; b < layout.branches.size(); ++b) {
        const auto& branch = layout.branches[b];
        const int spatial_count = branch.height * branch.width;
        const float stride_x    = static_cast<float>(input_width) / static_cast<float>(branch.width);
        const float stride_y    = static_cast<float>(input_height) / static_cast<float>(branch.height);

        if (nchw_separate) {
            if (branch.tensor_index + 1 >= heads.size()) {
                error = "AXERA YOLOv8 NCHW branch is missing its class tensor";
                return false;
            }
            const auto& box_head = heads[branch.tensor_index];
            const auto& cls_head = heads[branch.tensor_index + 1];
            int box_channels = 0, box_height = 0, box_width = 0;
            int cls_channels = 0, cls_height = 0, cls_width = 0;
            if (!ParseNchw(box_head.shape, box_channels, box_height, box_width) ||
                !ParseNchw(cls_head.shape, cls_channels, cls_height, cls_width) ||
                box_channels != 64 || cls_channels != layout.class_count ||
                !box_head.data || !cls_head.data ||
                box_head.element_count != ShapeCount(box_head.shape) ||
                cls_head.element_count != ShapeCount(cls_head.shape)) {
                error = "AXERA YOLOv8 NCHW head byte count does not match the queried shape";
                return false;
            }
            const size_t box_spatial = static_cast<size_t>(box_height) * static_cast<size_t>(box_width);
            for (int y = 0; y < branch.height; ++y) {
                for (int x = 0; x < branch.width; ++x) {
                    const int spatial_index = y * branch.width + x;
                    float distance[4]{};
                    for (int side = 0; side < 4; ++side) {
                        if (!DflDistance(box_head.data + side * 16 * box_spatial, static_cast<int>(box_spatial),
                                         spatial_index, &distance[side])) {
                            error = "AXERA YOLOv8 DFL softmax produced an invalid denominator";
                            return false;
                        }
                    }
                    const float left   = (static_cast<float>(x) + 0.5f - distance[0]) * stride_x;
                    const float top    = (static_cast<float>(y) + 0.5f - distance[1]) * stride_y;
                    const float right  = (static_cast<float>(x) + 0.5f + distance[2]) * stride_x;
                    const float bottom = (static_cast<float>(y) + 0.5f + distance[3]) * stride_y;
                    const int logical_index = point_offset + spatial_index;
                    output[logical_index]   = (left + right) * 0.5f;
                    output[layout.point_count + logical_index]     = (top + bottom) * 0.5f;
                    output[2 * layout.point_count + logical_index] = right - left;
                    output[3 * layout.point_count + logical_index] = bottom - top;
                    for (int cls = 0; cls < layout.class_count; ++cls) {
                        const float score = cls_head.data[cls * box_spatial + spatial_index];
                        output[(4 + cls) * layout.point_count + logical_index] =
                            layout.class_scores_are_probabilities ? std::clamp(score, 0.0f, 1.0f)
                                                                  : Sigmoid(score);
                    }
                }
            }
        } else {
            if (branch.tensor_index >= heads.size()) {
                error = "AXERA YOLOv8 packed branch is missing its head tensor";
                return false;
            }
            const auto& head = heads[branch.tensor_index];
            if (!head.data || head.element_count != ShapeCount(head.shape)) {
                error = "AXERA YOLOv8 packed head byte count does not match the queried shape";
                return false;
            }
            const int row_stride = 64 + layout.class_count;
            // Determine packed layout: {1,H,W,64+cls} uses spatial H*W rows;
            // {1,H*W,64+cls} uses H*W rows with W=1 (reconstructed above).
            const int rows = spatial_count;
            for (int y = 0; y < branch.height; ++y) {
                for (int x = 0; x < branch.width; ++x) {
                    const int spatial_index = y * branch.width + x;
                    const float* row        = head.data + spatial_index * row_stride;
                    float distance[4]{};
                    for (int side = 0; side < 4; ++side) {
                        if (!DflDistancePacked(row, row_stride, side, &distance[side])) {
                            error = "AXERA YOLOv8 packed DFL softmax produced an invalid denominator";
                            return false;
                        }
                    }
                    const float left   = (static_cast<float>(x) + 0.5f - distance[0]) * stride_x;
                    const float top    = (static_cast<float>(y) + 0.5f - distance[1]) * stride_y;
                    const float right  = (static_cast<float>(x) + 0.5f + distance[2]) * stride_x;
                    const float bottom = (static_cast<float>(y) + 0.5f + distance[3]) * stride_y;
                    const int logical_index = point_offset + spatial_index;
                    output[logical_index]   = (left + right) * 0.5f;
                    output[layout.point_count + logical_index]     = (top + bottom) * 0.5f;
                    output[2 * layout.point_count + logical_index] = right - left;
                    output[3 * layout.point_count + logical_index] = bottom - top;
                    for (int cls = 0; cls < layout.class_count; ++cls) {
                        const float score = row[64 + cls];
                        output[(4 + cls) * layout.point_count + logical_index] =
                            layout.class_scores_are_probabilities ? std::clamp(score, 0.0f, 1.0f)
                                                                  : Sigmoid(score);
                    }
                }
            }
        }
        point_offset += spatial_count;
    }
    return true;
}

}  // namespace cosmo::nn

#endif  // COSMO_NN_USE_AXERA_BACKEND
