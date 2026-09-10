#ifdef COSMO_MEDIA_USE_AXERA_BACKEND

#include "media/VideoEncoderAxera.h"

#include <cstring>
#include <mutex>

#include "ax_global_type.h"
#include "ax_sys_api.h"
#include "ax_venc_api.h"

#include "media/VideoEncoderCpu.h"
#include "nn/device/axera/axera_sys_init.h"
#include "util/Log.h"

namespace cosmo::media {

namespace {
    constexpr int kAxeraVencChn = 0;
    constexpr uint32_t kBitrate = 4 * 1024 * 1024;  // 4 Mbps default

    AX_PAYLOAD_TYPE_E ToAxPayload(VideoCodecType type) {
        switch (type) {
            case VideoCodecType::kH264:
                return PT_H264;
            case VideoCodecType::kH265:
                return PT_H265;
            default:
                return PT_H264;
        }
    }
}  // namespace

struct AxeraEncoderState {
    std::mutex mutex;
    bool opened{false};
    VENC_CHN chn{0};
};

VideoEncoderAxera::VideoEncoderAxera() = default;

VideoEncoderAxera::~VideoEncoderAxera() {
    if (state_) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->opened) {
            AX_VENC_DestroyChn(state_->chn);
            state_->opened = false;
        }
    }
}

bool VideoEncoderAxera::Open() {
    if (!state_) {
        state_ = std::make_unique<AxeraEncoderState>();
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->opened) {
        return true;
    }
    // The MPP path is UNVERIFIED on hardware; keep an FFmpeg software fallback
    // so a broken hardware init never silently blocks the pipeline.
    // AX_SYS is initialized exactly once process-wide via the shared helper.
    if (!cosmo::nn::EnsureAxeraSysInitialized()) {
        LOG_ERRO("[Axera] AX_SYS_Init failed; cannot init VENC");
        return false;
    }
    static std::once_flag ax_venc_init_flag;
    std::call_once(ax_venc_init_flag, []() {
        AX_VENC_Init(nullptr);
        std::atexit([]() { AX_VENC_Deinit(); });
    });

    AX_VENC_CHN_ATTR_T attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.stVencAttr.enType        = ToAxPayload(codec_type_);
    // VENC infers the source pixel format from the input AX_VIDEO_FRAME_INFO_T.
    attr.stVencAttr.u32PicWidthSrc  = static_cast<AX_U32>(width_);
    attr.stVencAttr.u32PicHeightSrc = static_cast<AX_U32>(height_);
    attr.stVencAttr.u32MaxPicWidth  = static_cast<AX_U32>(width_);
    attr.stVencAttr.u32MaxPicHeight = static_cast<AX_U32>(height_);
    attr.stVencAttr.enProfile     = AX_VENC_H264_MAIN_PROFILE;
    // Level 4.0 covers 1920x1080@25fps; the enum starts at 10 (LEVEL_1), so 0
    // (from memset) is out of range and makes AX_VENC_CreateChn fail with
    // AX_ERR_VENC_ILLEGAL_PARAM.
    attr.stVencAttr.enLevel       = AX_VENC_H264_LEVEL_4;
    attr.stRcAttr.enRcMode        = AX_VENC_RC_MODE_H264CBR;
    attr.stRcAttr.stFrameRate.fSrcFrameRate = 25.0f;
    attr.stRcAttr.stFrameRate.fDstFrameRate = 25.0f;
    attr.stRcAttr.stH264Cbr.u32Gop     = 30;
    attr.stRcAttr.stH264Cbr.u32StatTime = 1;   // [1, 60]
    attr.stRcAttr.stH264Cbr.u32BitRate = kBitrate / 1000;  // kbps
    attr.stRcAttr.stH264Cbr.u32MaxQp   = 36;   // [0, 51]
    attr.stRcAttr.stH264Cbr.u32MinQp   = 28;   // [0, 51]
    attr.stRcAttr.stH264Cbr.u32MaxIQp  = 36;   // [0, 51]
    attr.stRcAttr.stH264Cbr.u32MinIQp  = 28;   // [0, 51]
    attr.stRcAttr.stH264Cbr.u32MaxIprop = 100;  // [1, 100]
    attr.stRcAttr.stH264Cbr.u32MinIprop = 1;    // [1, u32MaxIprop]
    attr.stRcAttr.stH264Cbr.u32IdrQpDeltaRange = 2;  // [2, 10]

    state_->chn = kAxeraVencChn;
    const AX_S32 ret = AX_VENC_CreateChn(state_->chn, &attr);
    if (ret != 0) {
        LOG_ERRO("[Axera] AX_VENC_CreateChn failed: {}", ret);
        if (!fallback_) {
            fallback_ = std::make_unique<VideoEncoderCpu>();
            fallback_->Set(codec_type_, width_, height_);
        }
        return fallback_->Open();
    }
    state_->opened = true;
    LOG_INFO("[Axera] AX650 VENC channel {} opened ({}x{}, codec {})", state_->chn, width_, height_,
             static_cast<int>(codec_type_));
    return true;
}

VideoPacketPtr VideoEncoderAxera::SendYUVFrame(void* data) {
    if (fallback_) {
        return fallback_->SendYUVFrame(data);
    }
    if (!state_ || !state_->opened || data == nullptr) {
        return nullptr;
    }
    AX_VIDEO_FRAME_INFO_T frame_info{};
    auto& frame       = frame_info.stVFrame;
    frame.u32Width    = static_cast<AX_U32>(width_);
    frame.u32Height   = static_cast<AX_U32>(height_);
    frame.enImgFormat = AX_FORMAT_YUV420_SEMIPLANAR;
    // The caller buffer is host I420; a real board must convert to NV12 and
    // fill u64VirAddr/u64PhyAddr with VB-pool addresses. Fallback path (CPU
    // encoder) is the default until hardware validation lands.
    frame.u64VirAddr[0] = reinterpret_cast<AX_U64>(data);
    const AX_S32 ret    = AX_VENC_SendFrame(state_->chn, &frame_info, 100);
    if (ret != 0) {
        LOG_WARN("[Axera] AX_VENC_SendFrame failed: {}", ret);
        return nullptr;
    }
    // Stream retrieval via AX_VENC_GetStream; omitted here until the exact
    // stream-callback contract is verified on a real board.
    return nullptr;
}

VideoEncoderCapability VideoEncoderAxera::Probe(VideoCodecType type) {
    VideoEncoderCapability cap;
    cap.available      = (type == VideoCodecType::kH264 || type == VideoCodecType::kH265);
    cap.backend        = "axera";
    cap.implementation = "ax_venc_ax650_mpp";
    cap.detail         = "AX650 SoC VENC (libax_venc) - UNVERIFIED on hardware";
    return cap;
}

}  // namespace cosmo::media

#endif  // COSMO_MEDIA_USE_AXERA_BACKEND
