#ifdef COSMO_NN_USE_AXERA_BACKEND

#include "nn/device/axera/axera_ivps_normalize_node.h"
#include "nn/node/node_type.h"
#include "nn/node/node_type_utils.h"

namespace cosmo::nn {

AxIvpsNormalizeNode::AxIvpsNormalizeNode() {
    node_type     = NODE_NORMALIZE;
    name          = NodeTypeUtils::NodeTypeToStr(NODE_NORMALIZE).append("_ivps");
    one_blob_only = true;
}

AxIvpsNormalizeNode::~AxIvpsNormalizeNode() {
    // The top blob aliases the bottom blob's handle (the IVPS RGB buffer).
    // Clear it so ~BlobStore::FreeBlob does not free() the same CMM memory a
    // second time after AxIvpsNode already released it via AX_SYS_MemFree.
    if (top_blob_) {
        auto h = top_blob_->GetHandle();
        if (h.base == aliased_base_) {
            top_blob_->ClearHandle();
        }
    }
}

void AxIvpsNormalizeNode::LoadParam(Op* op) {
    (void)op;
}

DeviceType AxIvpsNormalizeNode::GetTopBlobDeviceType() {
    return DEVICE_AXERA;
}

bool AxIvpsNormalizeNode::NeedBottomShapesInfered() {
    return true;
}

Status AxIvpsNormalizeNode::InferTopShapesWithBottoms(std::vector<DimsVector> dims,
                                                      std::vector<DataType> types) {
    if (dims.empty() || types.empty()) {
        return Status(COSMO_NN_ERR_PARAM, "AxIvpsNormalizeNode: missing bottom dims/types");
    }
    // Passthrough node: the top blob mirrors the bottom blob's shape and type
    // (the resize/letterbox stage already produced the model input layout).
    top_blob_shapes     = {dims.at(0)};
    top_blob_data_types = {types.at(0)};
    return COSMO_NN_OK;
}

size_t AxIvpsNormalizeNode::GetBottomCount() {
    return 1;
}

size_t AxIvpsNormalizeNode::GetTopCount() {
    return 1;
}

Status AxIvpsNormalizeNode::Forward(
    std::vector<std::shared_ptr<Blob>>& bottom_blobs,
    std::vector<std::shared_ptr<Blob>>& top_blobs) {
    if (bottom_blobs.size() != 1 || top_blobs.size() != 1) {
        return Status(COSMO_NN_ERR_INVALID_INPUT, "AxIvpsNormalizeNode requires 1 input + 1 output");
    }
    top_blob_     = top_blobs[0];
    aliased_base_ = bottom_blobs[0]->GetHandle().base;
    top_blobs[0]->SetHandle(bottom_blobs[0]->GetHandle());
    return COSMO_NN_OK;
}

}  // namespace cosmo::nn

#endif  // COSMO_NN_USE_AXERA_BACKEND