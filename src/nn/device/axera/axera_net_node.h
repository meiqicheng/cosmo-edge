#pragma once

#ifdef COSMO_NN_USE_AXERA_BACKEND

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "nn/device/axera/axera_yolov8_adapter.h"
#include "nn/node/net_node.h"

namespace cosmo::nn {

inline constexpr char kAxeraRgbUint8InputContract[] = "cosmo.axera.input.rgb_u8_nhwc_0_255_to_0_1.v1";

/// AX650 SoC-mode inference node built on the official ax_engine API
/// (AX_ENGINE_Init / CreateHandle / CreateContext / GetIOInfo / RunSync) with
/// AX_SYS_MemAlloc physical IO buffers. Model files are raw .axmodel loaded
/// from memory; no CENN wrapper is used (native format, same as RKNN).
class AxeraNetNode final : public NetNode {
public:
    AxeraNetNode();
    ~AxeraNetNode() override;

    AxeraNetNode(const AxeraNetNode&)            = delete;
    AxeraNetNode& operator=(const AxeraNetNode&) = delete;

    DeviceType GetTopBlobDeviceType() override;
    Status InferTopShapes() override;
    Status LoadWeight(const char* data, size_t size) override;
    Status Forward(std::vector<std::shared_ptr<Blob>>& bottom_blobs,
                   std::vector<std::shared_ptr<Blob>>& top_blobs) override;

private:
    struct IoBuffer {
        uint64_t phy_addr{0};
        void* vir_addr{nullptr};
        uint32_t size{0};
    };

    struct TensorMeta {
        std::string name;
        std::vector<int> shape;
        int dtype{0};
        int layout{0};
        uint32_t size{0};
    };

    Status QueryTensorMetadata();
    Status PrepareInput(const Blob& blob, std::vector<float>& nhwc, int& height, int& width) const;
    Status ResolveOutputAdapter();
    void DestroyHandle();
    void FreeIoBuffers();

    void* engine_handle_{nullptr};
    void* io_info_{nullptr};
    std::vector<TensorMeta> input_metas_;
    std::vector<TensorMeta> output_metas_;
    std::vector<IoBuffer> io_input_buffers_;
    std::vector<IoBuffer> io_output_buffers_;
    std::vector<unsigned char> model_data_;
    std::vector<float> input_nhwc_;
    AxeraYolov8Layout yolov8_layout_;
    std::vector<AxeraYolov8Head> yolov8_heads_;
    bool is_yolov8_{false};
    int yolov8_class_count_{0};
    int yolov8_point_count_{0};
    bool detector_model_{false};
    mutable std::mutex mutex_;
};

}  // namespace cosmo::nn

#endif  // COSMO_NN_USE_AXERA_BACKEND
