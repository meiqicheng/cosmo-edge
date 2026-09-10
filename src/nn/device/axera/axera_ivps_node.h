#pragma once

#ifdef COSMO_NN_USE_AXERA_BACKEND

#include "nn/node/node.h"

namespace cosmo::nn {

/**
 * @brief AX650N IVPS hardware preprocessing node.
 *
 * Replaces the CPU resize + normalize chain on the detector hot path. Reads the
 * decoded NV12 frame from the VDEC device buffer (carried on the bottom blob's
 * native_image.phy) and runs AX_IVPS_CropResizeVpp to perform crop + resize +
 * letterbox + color-space conversion (NV12 -> RGB) in a single hardware pass,
 * writing directly into an AX_SYS physical buffer that the NPU can consume
 * zero-copy.
 *
 * The top blob carries the RGB result as a native_image with phy != 0 so the
 * downstream AxeraNetNode::Forward can bind pInputs[0].phyAddr directly without
 * a host memcpy.
 *
 * Gravity modes mirror CpuResizeNode:
 *   0: stretch to target size
 *   1: keep aspect ratio, center padding (letterbox)
 *   2: keep aspect ratio, top-left align
 */
class AxIvpsNode : public Node {
public:
    AxIvpsNode();
    ~AxIvpsNode() override;

    void LoadParam(Op* op) override;
    DeviceType GetTopBlobDeviceType() override;
    Status InferTopShapes() override;
    size_t GetBottomCount() override;
    size_t GetTopCount() override;

    Status Forward(std::vector<std::shared_ptr<Blob>>& bottom_blobs,
                   std::vector<std::shared_ptr<Blob>>& top_blobs) override;

private:
    // Lazily initializes the IVPS subsystem and allocates the cached RGB output
    // buffer. Returns COSMO_NN_OK on success.
    Status EnsureIvpsReady();

    int out_h_   = 0;
    int out_w_   = 0;
    int gravity_ = 0;
    uint8_t pad_ = 114;

    // Cached AX_SYS physical output buffer (RGB, out_w_ x out_h_).
    uint64_t rgb_phy_ = 0;
    void* rgb_vir_    = nullptr;
    size_t rgb_size_  = 0;
    bool ivps_ready_  = false;

    // The top blob's handle aliases the cached RGB buffer (rgb_phy_/rgb_vir_).
    // Keep a reference so the destructor can clear the blob handle before
    // freeing the buffer; otherwise ~BlobStore::FreeBlob would free() the CMM
    // memory a second time (double-free / SIGABRT on detector teardown).
    std::shared_ptr<Blob> top_blob_;
};

}  // namespace cosmo::nn

#endif  // COSMO_NN_USE_AXERA_BACKEND