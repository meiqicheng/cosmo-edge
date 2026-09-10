#pragma once

#ifdef COSMO_NN_USE_AXERA_BACKEND

#include "nn/node/node.h"

namespace cosmo::nn {

/**
 * @brief AX650N passthrough node for the normalize stage.
 *
 * When the AX_IVPS hardware path is active, the resize + letterbox + color-space
 * conversion (NV12 -> RGB) is already performed by AxIvpsNode. The model expects
 * RGB uint8 NHWC 0-255 input (see kAxeraRgbUint8InputContract), so no further
 * normalization is required on the hardware path. This node simply forwards the
 * bottom blob's handle (including its native_image.phy) to the top blob so the
 * downstream AxeraNetNode::Forward can bind the physical buffer zero-copy.
 *
 * The host (CPU) normalize path remains available for non-AXERA inputs; this node
 * is only selected by AxeraNodeCreator for the AXERA backend.
 */
class AxIvpsNormalizeNode : public Node {
public:
    AxIvpsNormalizeNode();
    ~AxIvpsNormalizeNode() override;

    void LoadParam(Op* op) override;
    DeviceType GetTopBlobDeviceType() override;
    bool NeedBottomShapesInfered() override;
    Status InferTopShapesWithBottoms(std::vector<DimsVector> dims, std::vector<DataType> types) override;
    size_t GetBottomCount() override;
    size_t GetTopCount() override;

    Status Forward(std::vector<std::shared_ptr<Blob>>& bottom_blobs,
                   std::vector<std::shared_ptr<Blob>>& top_blobs) override;

private:
    // The top blob aliases the bottom blob's handle (the IVPS RGB buffer).
    // Clear it in the destructor so ~BlobStore does not free() the same CMM
    // memory twice (the IVPS node frees it via AX_SYS_MemFree).
    std::shared_ptr<Blob> top_blob_;
    void* aliased_base_ = nullptr;
};

}  // namespace cosmo::nn

#endif  // COSMO_NN_USE_AXERA_BACKEND