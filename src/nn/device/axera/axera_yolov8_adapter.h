#pragma once

#ifdef COSMO_NN_USE_AXERA_BACKEND

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cosmo::nn {

/// AXERA YOLOv8 DFL head contract, resolved from AX_ENGINE_IO_INFO_T output
/// metadata. The logical output matches the RKNN adapter layout
/// ({1, 4 + class_count, point_count} cxcywh) so downstream decode is reused.
struct AxeraYolov8BranchContract {
    static constexpr size_t kNoTensor = static_cast<size_t>(-1);

    size_t tensor_index{kNoTensor};
    int height{0};
    int width{0};
    int stride{0};
    bool nchw_separate{false};  // true: [1,64,h,w]+[1,cls,h,w]; false: [1,h,w,64+cls]
};

struct AxeraYolov8Layout {
    bool detected{false};
    int class_count{0};
    int point_count{0};
    bool class_scores_are_probabilities{false};
    std::vector<int> logical_shape;
    std::vector<AxeraYolov8BranchContract> branches;
};

struct AxeraYolov8Head {
    const float* data{nullptr};
    size_t element_count{0};
    std::vector<int> shape;
};

/// Detect a 3-head YOLOv8 DFL output layout from the AXERA model output shapes.
/// Supports the official ax-samples NHWC packing ({1,h,w,64+cls}) and the
/// NCHW split packing ({1,64,h,w}+{1,cls,h,w}); heads must be ordered from
/// fine to coarse stride (largest h*w first).
bool DetectAxeraYolov8Layout(const std::vector<std::vector<int>>& shapes, AxeraYolov8Layout& layout,
                             std::string& error);

/// Reconstruct the DFL-encoded YOLOv8 outputs into the CosmoEdge logical blob
/// {1, 4 + class_count, point_count} with cxcywh boxes. `heads` must contain
/// one entry per resolved branch tensor (3 or 6 entries, in io_info order).
bool ReconstructAxeraYolov8(const std::vector<AxeraYolov8Head>& heads, int input_height, int input_width,
                            float* output, size_t output_count, std::string& error);

}  // namespace cosmo::nn

#endif  // COSMO_NN_USE_AXERA_BACKEND
