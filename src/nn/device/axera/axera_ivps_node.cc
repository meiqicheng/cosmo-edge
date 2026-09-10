#ifdef COSMO_NN_USE_AXERA_BACKEND

#include "nn/device/axera/axera_ivps_node.h"

#include <chrono>

#include <cstring>
#include <mutex>

#include "ax_ivps_api.h"
#include "nn/core/inference_pipeline_metrics.h"
#include "ax_sys_api.h"

#include "nn/device/axera/axera_sys_init.h"
#include "nn/node/node_type_utils.h"
#include "nn/utils/dims_vector_utils.h"
#include "nn/utils/image_format_utils.h"
#include "nn/utils/op.h"
#include "util/Log.h"

namespace cosmo::nn {

// IVPS subsystem initialization is process-wide; mirror the official sample.
// AX_SYS must be initialized before AX_IVPS (done once via the shared helper).
namespace {
    std::once_flag g_ivps_init_flag;
    bool g_ivps_sys_ok = false;
    bool g_ivps_ok     = false;
}  // namespace

AxIvpsNode::AxIvpsNode() : Node() {
    node_type     = NodeType::NODE_RESIZE;
    name          = NodeTypeUtils::NodeTypeToStr(NODE_RESIZE).append("_ivps");
    one_blob_only = true;
}

AxIvpsNode::~AxIvpsNode() {
    // The top blob's handle aliases the cached RGB buffer. Clear it before
    // freeing the buffer so ~BlobStore::FreeBlob sees a null handle and does
    // not free() the CMM memory a second time (double-free / SIGABRT).
    if (top_blob_) {
        auto h = top_blob_->GetHandle();
        if (h.base == rgb_vir_) {
            top_blob_->ClearHandle();
        }
    }
    if (rgb_phy_ && rgb_vir_)
        AX_SYS_MemFree(rgb_phy_, rgb_vir_);
    rgb_phy_ = 0;
    rgb_vir_ = nullptr;
    rgb_size_ = 0;
}

void AxIvpsNode::LoadParam(Op* op) {
    if (!op)
        return;
    auto resize = dynamic_cast<Resize*>(op);
    if (!resize)
        return;
    if (resize->dsize.size() >= 2) {
        out_h_ = resize->dsize[0];
        out_w_ = resize->dsize[1];
    }
    gravity_ = resize->gravity;
    if (!resize->color.empty())
        pad_ = static_cast<uint8_t>(resize->color[0]);
}

DeviceType AxIvpsNode::GetTopBlobDeviceType() {
    return DeviceType::DEVICE_AXERA;
}

Status AxIvpsNode::InferTopShapes() {
    shared_resource->net_input_w = out_w_;
    shared_resource->net_input_h = out_h_;

    top_blob_shapes     = {{1, out_h_, out_w_, 3}};  // NHWC
    top_blob_data_types = {DataType::DATA_TYPE_UINT8};
    return COSMO_NN_OK;
}

size_t AxIvpsNode::GetBottomCount() {
    return 1;
}
size_t AxIvpsNode::GetTopCount() {
    return 1;
}

Status AxIvpsNode::EnsureIvpsReady() {
    if (ivps_ready_)
        return COSMO_NN_OK;

    g_ivps_sys_ok = cosmo::nn::EnsureAxeraSysInitialized();
    if (!g_ivps_sys_ok)
        return Status(COSMO_NN_ERR_NET, "AX_SYS_Init failed for IVPS");

    std::call_once(g_ivps_init_flag, [&] {
        g_ivps_ok = (AX_IVPS_Init() == 0);
    });
    if (!g_ivps_ok)
        return Status(COSMO_NN_ERR_NET, "AX_IVPS_Init failed");

    // Allocate the cached RGB output buffer once. IVPS writes into CMM
    // physical memory that the NPU can read directly (zero-copy).
    if (out_w_ <= 0 || out_h_ <= 0)
        return Status(COSMO_NN_ERR_INVALID_INPUT, "AX_IVPS output size is invalid");
    const size_t stride = static_cast<size_t>(out_w_) * 3u;
    rgb_size_           = stride * static_cast<size_t>(out_h_);
    if (AX_SYS_MemAlloc(reinterpret_cast<AX_U64*>(&rgb_phy_), &rgb_vir_, static_cast<AX_U32>(rgb_size_), 128,
                        (const AX_S8*)"cosmo_ivps") != 0) {
        rgb_phy_ = 0;
        rgb_vir_ = nullptr;
        rgb_size_ = 0;
        return Status(COSMO_NN_ERR_NET, "AX_SYS_MemAlloc failed for IVPS output");
    }

    ivps_ready_ = true;
    return COSMO_NN_OK;
}

Status AxIvpsNode::Forward(std::vector<std::shared_ptr<Blob>>& bottom_blobs,
                           std::vector<std::shared_ptr<Blob>>& top_blobs) {
    if (bottom_blobs.size() != 1 || !bottom_blobs[0])
        return Status(COSMO_NN_ERR_INVALID_INPUT, "AX_IVPS requires exactly one input blob");
    if (top_blobs.size() != 1 || !top_blobs[0])
        return Status(COSMO_NN_ERR_INVALID_INPUT, "AX_IVPS requires exactly one output blob");
    top_blob_ = top_blobs[0];
    LOG_INFO("[Axera][IVPS] fwd step1: input validated");

    const BlobHandle& bottom_handle = bottom_blobs[0]->GetHandle();
    const BlobHandle::NativeImage& native = bottom_handle.native_image;
    LOG_INFO("[Axera][IVPS] fwd step2: native extracted fd={} phy={:#x} fmt={} valid={}",
             native.fd, native.phy, static_cast<int>(native.format), native.Valid());

    // The bottom blob must carry a hardware NV12 buffer (phy != 0). If it only
    // has host data (e.g. a non-VDEC source), fall back to the CPU resize node
    // semantics is not possible here; report a clear error so the caller knows
    // the hardware path was not taken.
    if (!native.Valid() || native.phy == 0) {
        LOG_ERRO("[Axera][IVPS] pre-check failed: blob={} node={} "
                 "native[fd={} bytes={} w={} h={} wstride={} hstride={} fmt={} phy={:#x} vir={} "
                 "valid={} base={} handle_phy={:#x}",
                 static_cast<const void*>(bottom_blobs[0].get()), GetNodeName(),
                 native.fd, native.bytes, native.width, native.height, native.width_stride,
                 native.height_stride, static_cast<int>(native.format), native.phy,
                 static_cast<const void*>(native.vir), native.Valid(),
                 static_cast<const void*>(bottom_handle.base), bottom_handle.phy);
        return Status(COSMO_NN_ERR_INVALID_INPUT,
                      "AX_IVPS requires a hardware NV12 input (native_image.phy != 0)");
    }
    LOG_INFO("[Axera][IVPS] fwd step3: pre-check passed");
    if (native.format != IMAGE_NV12)
        return Status(COSMO_NN_ERR_INVALID_INPUT, "AX_IVPS input must be NV12");
    LOG_INFO("[Axera][IVPS] fwd step4: format NV12 confirmed");

    Status ready = EnsureIvpsReady();
    if (!ready)
        return ready;
    LOG_INFO("[Axera][IVPS] fwd step5: ivps ready");

    // ── Build the source AX_VIDEO_FRAME_T (NV12 in device memory) ─────────
    // u64PhyAddr[0] = Y plane, u64PhyAddr[1] = UV plane (interleaved).
    const uint64_t y_plane = native.phy;
    const uint64_t uv_plane =
        y_plane + static_cast<uint64_t>(native.width_stride) * static_cast<uint64_t>(native.height);

    AX_VIDEO_FRAME_T src{};
    src.u32Width       = static_cast<AX_U32>(native.width);
    src.u32Height      = static_cast<AX_U32>(native.height);
    src.enImgFormat    = AX_FORMAT_YUV420_SEMIPLANAR;  // NV12
    src.u32PicStride[0] = static_cast<AX_U32>(native.width_stride);
    src.u32PicStride[1] = static_cast<AX_U32>(native.width_stride);
    src.u64PhyAddr[0]  = y_plane;
    src.u64PhyAddr[1]  = uv_plane;

    // ── Build the destination AX_VIDEO_FRAME_T (RGB in device memory) ─────
    // AX_FORMAT_BGR888 (0xA5) produces RGB byte order, which is what the YOLO
    // model expects (is_bgr:false). AX_FORMAT_RGB888 (0xA1) is BGR byte order.
    const size_t dst_stride = static_cast<size_t>(out_w_) * 3u;
    AX_VIDEO_FRAME_T dst{};
    dst.u32Width        = static_cast<AX_U32>(out_w_);
    dst.u32Height       = static_cast<AX_U32>(out_h_);
    dst.enImgFormat     = AX_FORMAT_BGR888;  // RGB byte order
    dst.u32PicStride[0] = static_cast<AX_U32>(dst_stride);
    dst.u64PhyAddr[0]   = rgb_phy_;

    // ── Aspect ratio (letterbox) ───────────────────────────────────────────
    // AUTO keeps aspect ratio and letterboxes; center alignment + gray
    // background matches the CPU resize node's gravity==1 behavior.
    AX_IVPS_ASPECT_RATIO_T aspect{};
    aspect.eMode     = AX_IVPS_ASPECT_RATIO_AUTO;
    aspect.eAligns[0] = AX_IVPS_ASPECT_RATIO_HORIZONTAL_CENTER;
    aspect.eAligns[1] = AX_IVPS_ASPECT_RATIO_VERTICAL_CENTER;
    // nBgColor is 0xRRGGBB; pad_ (default 114 = 0x72) -> 0x727272 gray.
    aspect.nBgColor = static_cast<AX_U32>(pad_) * 0x010101u;
    LOG_INFO("[Axera][IVPS] fwd step6: src/dst built src={}x{} dst={}x{} rgb_phy={:#x}",
             native.width, native.height, out_w_, out_h_, rgb_phy_);

    const auto vpp_started = std::chrono::steady_clock::now();
    const int ret = AX_IVPS_CropResizeVpp(&src, &dst, &aspect);
    const auto elapsed_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - vpp_started).count());
    cosmo::nn::GetInferencePipelineMetrics().RecordBlobConvert(elapsed_ns, 1);
    if (ret != 0) {
        LOG_ERRO("[Axera][IVPS] CropResizeVpp failed: ret={} src={}x{} dst={}x{} phy={:#x}",
                 ret, native.width, native.height, out_w_, out_h_, rgb_phy_);
        return Status(COSMO_NN_ERR_NET, "AX_IVPS_CropResizeVpp failed with code " + std::to_string(ret));
    }
    LOG_INFO("[Axera][IVPS] fwd step7: CropResizeVpp ok ret={} elapsed_ns={}", ret, elapsed_ns);

    // ── Publish the RGB result on the top blob as a hardware native_image ──
    BlobHandle top_handle;
    top_handle.base = rgb_vir_;
    top_handle.phy  = rgb_phy_;
    top_handle.native_image.fd           = -1;
    top_handle.native_image.phy          = rgb_phy_;
    top_handle.native_image.bytes        = rgb_size_;
    top_handle.native_image.width        = out_w_;
    top_handle.native_image.height       = out_h_;
    top_handle.native_image.width_stride = static_cast<int>(dst_stride);
    top_handle.native_image.height_stride = out_h_;
    top_handle.native_image.format       = IMAGE_RGB;
    top_blobs[0]->SetHandle(top_handle);
    LOG_INFO("[Axera][IVPS] fwd step8: top handle set");

    return COSMO_NN_OK;
}

}  // namespace cosmo::nn

#endif  // COSMO_NN_USE_AXERA_BACKEND
