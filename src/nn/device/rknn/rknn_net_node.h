#pragma once

#ifdef COSMO_NN_USE_RKNN_BACKEND

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "nn/device/rknn/rknn_yolov8_adapter.h"
#include "nn/node/net_node.h"
#include "rknn_api.h"

namespace cosmo::nn {

inline constexpr char kRknnRgbUint8InputContract[] = "cosmo.rknn.input.rgb_u8_nhwc_0_255_to_0_1.v1";

bool IsRknnRgbUint8InputContract(const std::string& contract);
bool IsRknnNativeInt8InputCompatible(const rknn_tensor_attr& attr, const BlobDesc& desc);
bool IsRknnNativeYolov8OutputCompatible(const std::vector<rknn_tensor_attr>& attrs,
                                        std::string* reason = nullptr);
bool IsRknnBoundInt8InputCompatible(const rknn_tensor_attr& attr, const BlobDesc& desc,
                                    std::string* reason = nullptr);
bool IsRknnRgaBoundInputCompatible(const rknn_tensor_attr& attr, int height, int width,
                                   std::string* reason = nullptr);
bool CopyRknnPackedInt8Input(const int8_t* source, size_t source_bytes, int8_t* destination,
                             size_t destination_bytes, int height, int width, int channels, int width_stride,
                             std::string* reason = nullptr);
bool CopyRknnPackedNativeInt8ToUint8(const int8_t* source, size_t source_bytes, uint8_t* destination,
                                     size_t destination_bytes, int height, int width, int channels,
                                     int width_stride, std::string* reason = nullptr);
bool ConvertRknnNormalizedFloatToUint8(const float* source, size_t source_count, uint8_t* destination,
                                       size_t destination_count, std::string* reason = nullptr);
bool RequantizeRknnPackedUint8ToInt8InPlace(uint8_t* data, size_t data_bytes, int height, int width,
                                            int channels, int width_stride, std::string* reason = nullptr);
const char* RknnRgaBoundRequantizeImplementation();
bool RknnFastOutputEnabled();
bool RknnDirectCandidatesEnabled();
bool RknnBoundInputEnabled();
bool RknnRgaBoundInputEnabled();

// RKNPU2 exposes one core per silicon NPU cluster: 1 on RV1126B, 2 on RK3576 and
// 3 on RK3588. The count is a hardware fact carried in the packaged
// platform-profile.json (`runtime.npu_core_count`) so the scheduler below can be
// sized from configuration instead of assuming the two-core RK3576 shape.
inline constexpr int kRknnMinNpuCoreCount = 1;
inline constexpr int kRknnMaxNpuCoreCount = 3;

enum class RknnCoreMode : uint8_t {
    Auto = 0,
    Core0,
    Core1,
    Core01,
    // Tri-core selection; only RK3588 can honour it. Lower-core parts degrade.
    Core012,
    Split,
    // Saturate every core the detected part has, without naming a count.
    All,
};

// Result of resolving a requested mode against the detected core count. Resolving
// once keeps the mask and the "should we call rknn_set_core_mask at all" decision
// from disagreeing, which is how a single-core part would receive a mask it rejects.
struct RknnCoreSelection {
    rknn_core_mask mask{RKNN_NPU_CORE_AUTO};
    bool apply{false};
    bool degraded{false};
};

RknnCoreMode ParseRknnCoreMode(const std::string& value, bool* valid = nullptr);
// Clamps a configured/observed core count into [kRknnMinNpuCoreCount, kRknnMaxNpuCoreCount].
int NormalizeRknnNpuCoreCount(int npu_core_count);
RknnCoreSelection ResolveRknnCoreSelection(RknnCoreMode mode, uint64_t context_sequence, int npu_core_count);
rknn_core_mask ResolveRknnCoreMask(RknnCoreMode mode, uint64_t context_sequence, int npu_core_count);
bool ShouldConfigureRknnCoreMask(RknnCoreMode mode, int npu_core_count);
const char* RknnCoreModeName(RknnCoreMode mode);

class RknnNetNode final : public NetNode, public RknnBoundInputProvider {
public:
    RknnNetNode();
    ~RknnNetNode() override;

    RknnNetNode(const RknnNetNode&)            = delete;
    RknnNetNode& operator=(const RknnNetNode&) = delete;

    DeviceType GetTopBlobDeviceType() override;
    Status InferTopShapes() override;
    Status LoadWeight(const char* data, size_t size) override;
    Status AttachOwnedContext(rknn_context context, uint64_t model_fingerprint);
    Status Forward(std::vector<std::shared_ptr<Blob>>& bottom_blobs,
                   std::vector<std::shared_ptr<Blob>>& top_blobs) override;
    // NetNode also declares a three-argument Forward for nodes that take parameter
    // blobs. Overriding only the two-argument form hides that overload by name, which
    // -Woverloaded-virtual reports in strict-warning translation units such as
    // RkllmVlmBackend.cc; re-expose it rather than silently hiding it.
    using NetNode::Forward;
    bool EnsureRgaBoundInput(int height, int width, std::string& reason) override;

private:
    enum class BoundInputMode : uint8_t {
        None = 0,
        NativeInt8,
        RgaNativeInt8,
    };

    Status QueryTensorAttributes();
    Status PrepareInput(const Blob& blob, std::vector<float>& nhwc, int& height, int& width) const;
    Status PrepareNativeCompatibilityInput(const Blob& blob, std::vector<float>& nhwc, int& height,
                                           int& width) const;
    std::vector<int> TensorShape(const rknn_tensor_attr& attr) const;
    size_t TensorElementCount(const rknn_tensor_attr& attr) const;
    bool TryBindNativeInputMemory(const BlobDesc& desc, std::string& reason);
    bool TryBindRgaInputMemory(int height, int width, std::string& reason);
    bool AllocateAndBindInputMemory(rknn_tensor_attr attr, BoundInputMode mode, std::string& reason);
    void PublishRgaBoundInputTarget();
    void ClearRgaBoundInputTarget();
    void DestroyContext();
    Status InitializeLoadedContext(uint64_t context_sequence);

    rknn_context context_{0};
    rknn_input_output_num io_count_{};
    std::vector<rknn_tensor_attr> input_attrs_;
    std::vector<rknn_tensor_attr> output_attrs_;
    std::vector<rknn_output> runtime_outputs_;
    std::vector<RknnYolov8Head> float_yolov8_heads_;
    std::vector<RknnYolov8QuantizedHead> quantized_yolov8_heads_;
    RknnYolov8CandidateScratch yolov8_candidate_scratch_;
    RknnOutputAdapterContract output_adapter_contract_;
    rknn_tensor_attr bound_input_attr_{};
    rknn_tensor_mem* bound_input_memory_{nullptr};
    std::vector<unsigned char> model_data_;
    std::vector<float> input_nhwc_;
    std::vector<uint8_t> input_uint8_;
    bool yolov8_heads_{false};
    bool native_yolov8_outputs_{false};
    bool detector_model_{false};
    bool bound_input_eligible_{true};
    bool rga_bound_input_eligible_{true};
    BoundInputMode bound_input_mode_{BoundInputMode::None};
    uint64_t bound_input_generation_{0};
    int yolov8_class_count_{0};
    int yolov8_point_count_{0};
    mutable std::mutex mutex_;
};

}  // namespace cosmo::nn

#endif  // COSMO_NN_USE_RKNN_BACKEND
