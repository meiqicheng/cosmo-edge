#pragma once

#if defined(COSMO_NN_USE_RKNN_BACKEND) && defined(COSMO_MEDIA_USE_ROCKCHIP_BACKEND)

#include <cstddef>
#include <cstdint>
#include <vector>

#include "media/RockchipRgaBuffer.h"
#include "nn/device/cpu/cpu_crop_resize_node.h"
#include "nn/node/node.h"

namespace cosmo::nn {

bool RknnFastPreprocessEnabled();
bool RknnForceRgaFailure();
bool RknnMppDmaBufEnabled();
bool RknnForceMppDmaBufFailure();
bool IsRknnDetectorResizeContract(int out_height, int out_width, int gravity,
                                  const std::vector<int>& padding_color);
bool IsRknnNativeNormalizeContract(const std::vector<float>& mean, const std::vector<float>& std_dev,
                                   float scale, const DimsVector& input_dims);
void MapPackedU8ToNativeInt8(const uint8_t* source, int8_t* destination, size_t pixels, bool swap_red_blue);

class RknnResizeNode final : public Node {
public:
    RknnResizeNode();
    ~RknnResizeNode() override;

    void LoadParam(Op* op) override;
    DeviceType GetTopBlobDeviceType() override;
    Status InferTopShapes() override;
    size_t GetBottomCount() override;
    size_t GetTopCount() override;
    Status Forward(std::vector<std::shared_ptr<Blob>>& bottom_blobs,
                   std::vector<std::shared_ptr<Blob>>& top_blobs) override;

private:
    Status ResizeSingle(const std::shared_ptr<Blob>& bottom, const std::shared_ptr<Blob>& top,
                        bool allow_bound_target);
    bool ResizeWithRga(const Blob& bottom, Blob& top, bool allow_bound_target);
    bool AcquireRgaBoundTarget(uint32_t& handle);
    void ReleaseRgaBoundTarget();
    void ResizeWithCpu(const Blob& bottom, Blob& top, bool output_rgb) const;
    Status ResizeNativeWithCpu(const Blob& bottom, Blob& top) const;
    // Letterbox padding is constant for a fixed detector contract, so the
    // full-target memset only runs when the fill fingerprint (target pointer,
    // byte size, padding color, bound-input generation) changes instead of on
    // every frame; RGA overwrites the center window each frame anyway.
    void FillLetterboxIfNeeded(void* target_va, size_t target_bytes, uint8_t fill_color,
                               uint64_t generation);

    int out_height_{0};
    int out_width_{0};
    int gravity_{0};
    std::vector<int> padding_color_{114, 114, 114};
    bool detector_contract_{false};
    // Failed acceleration stays disabled for this node's lifetime. The packed
    // RGB path remains available when only native YUV CSC/import is unsupported.
    bool native_rga_unavailable_{false};
    bool rga_unavailable_{false};
    uint32_t rga_bound_target_handle_{0};
    uint64_t rga_bound_target_generation_{0};
    bool rga_bound_target_unavailable_{false};
    bool rga_bound_guard_logged_{false};
    // Persistent dma32 staging for the host (process-heap) resize path: per-frame
    // alloc+mmap costs more than the CPU fallback it replaces. Single-threaded;
    // only mutated inside ResizeWithRga().
    media::Dma32Buffer rga_host_src_buf_;
    media::Dma32Buffer rga_host_dst_buf_;
    // Imported handles for native MPP source DMA-BUFs, keyed by (fd, bytes).
    // The decoder recycles a fixed buffer pool, so a small LRU removes the
    // per-frame importbuffer_fd/releasebuffer_handle pair that serializes all
    // channels behind RgaGlobalLock. Capacity covers the decoder pool plus a
    // margin (see VideoDecoderRockchip kDecoderBufferCount). Single-threaded;
    // only touched inside ResizeWithRga().
    media::RgaFdHandleCache native_source_handles_{26};
    // Decoder buffer-group epoch the cache was filled under; a mismatch
    // clears the cache before any lookup (fd numbers can be reused by a
    // reconnecting decoder's new group).
    uint64_t native_source_epoch_{0};
    // Letterbox fill fingerprint for FillLetterboxIfNeeded.
    void* letterbox_fill_va_{nullptr};
    size_t letterbox_fill_bytes_{0};
    int letterbox_fill_color_{-1};
    uint64_t letterbox_fill_generation_{0};
};

class RknnCropResizeNode final : public CpuCropResizeNode {
public:
    RknnCropResizeNode();
    ~RknnCropResizeNode() override;

    void LoadParam(Op* op) override;
    Status Forward(std::vector<std::shared_ptr<Blob>>& image_blobs,
                   std::vector<std::shared_ptr<Blob>>& rect_blobs,
                   std::vector<std::shared_ptr<Blob>>& top_blobs) override;

private:
    bool ForwardWithRga(std::vector<std::shared_ptr<Blob>>& image_blobs,
                        std::vector<std::shared_ptr<Blob>>& rect_blobs,
                        std::vector<std::shared_ptr<Blob>>& top_blobs);
    bool AcquireRgaBoundTarget(uint32_t& handle);
    void ReleaseRgaBoundTarget();
    void InvalidateRgaBoundFrame();

    bool fast_contract_{false};
    bool native_rga_unavailable_{false};
    bool rga_unavailable_{false};
    uint32_t rga_bound_target_handle_{0};
    uint64_t rga_bound_target_generation_{0};
    bool rga_bound_target_unavailable_{false};
    bool rga_bound_guard_logged_{false};
    // Imported handles for native MPP source DMA-BUFs (see RknnResizeNode).
    // Single-threaded; only touched inside ForwardWithRga().
    media::RgaFdHandleCache native_source_handles_{26};
    uint64_t native_source_epoch_{0};
};

class RknnNormalizeNode final : public Node {
public:
    RknnNormalizeNode();
    ~RknnNormalizeNode() override = default;

    void LoadParam(Op* op) override;
    DeviceType GetTopBlobDeviceType() override;
    bool NeedBottomShapesInfered() override;
    Status InferTopShapesWithBottoms(std::vector<DimsVector> dims, std::vector<DataType> types) override;
    size_t GetBottomCount() override;
    size_t GetTopCount() override;
    Status Forward(std::vector<std::shared_ptr<Blob>>& bottom_blobs,
                   std::vector<std::shared_ptr<Blob>>& top_blobs) override;

private:
    bool NeedSwapRedBlue(ImageFormat format) const;
    bool CanBypassBoundInput(const Blob& bottom) const;
    Status ForwardNative(const Blob& bottom, Blob& top);
    Status ForwardFloat(const Blob& bottom, Blob& top);

    std::vector<float> mean_{};
    std::vector<float> std_dev_{};
    std::vector<float> scale_{};
    float uniform_scale_{1.0f};
    bool is_bgr_{true};
    bool native_contract_{false};
    bool detector_sized_{false};
};

}  // namespace cosmo::nn

#endif
