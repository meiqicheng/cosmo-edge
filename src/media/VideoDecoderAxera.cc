#ifdef COSMO_MEDIA_USE_AXERA_BACKEND

#include "media/VideoDecoderAxera.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "ax_global_type.h"
#include "ax_sys_api.h"
#include "ax_vdec_api.h"

#include "media/VideoDecoderCpu.h"
#include "util/Log.h"

namespace cosmo::media {

namespace {
    constexpr int kAxeraDecoderChn  = 0;
    constexpr int kAxeraDecoderGrp  = 0;
    constexpr uint32_t kMaxPicSize  = 4096 * 4096;
    constexpr uint32_t kStreamBuf   = 4 * 1024 * 1024;

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

struct AxeraDecoderState {
    std::mutex mutex;
    bool opened{false};
    AX_VDEC_GRP grp{0};
    uint64_t next_frame_index{0};
};

VideoDecoderAxera::VideoDecoderAxera(size_t name) : VideoDecoder(name) {
    state_ = std::make_unique<AxeraDecoderState>();
}

VideoDecoderAxera::~VideoDecoderAxera() {
    Close();
}

bool VideoDecoderAxera::Open() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->opened) {
        return true;
    }
    // The MPP path is UNVERIFIED on hardware; keep an FFmpeg software fallback
    // so a broken hardware init never silently blocks the pipeline.
    if (!OpenMpp()) {
        LOG_WARN("[Axera] AX650 VDEC init failed, falling back to CPU decoder");
        if (!fallback_) {
            fallback_ = std::make_unique<VideoDecoderCpu>(0);
            fallback_->SetCodecType(codec_type_, static_cast<int>(width_), static_cast<int>(height_));
        }
        return fallback_->Open();
    }
    state_->opened = true;
    return true;
}

bool VideoDecoderAxera::OpenMpp() {
    // System / module init is process-wide; call once and ignore repeat init.
    static std::once_flag ax_sys_init_flag;
    std::call_once(ax_sys_init_flag, []() {
        AX_S32 ret = AX_SYS_Init();
        if (ret != 0) {
            LOG_ERRO("[Axera] AX_SYS_Init failed: {}", ret);
        }
        ret = AX_VDEC_Init(nullptr);
        if (ret != 0) {
            LOG_ERRO("[Axera] AX_VDEC_Init failed: {}", ret);
        }
        std::atexit([]() {
            AX_VDEC_Deinit();
            AX_SYS_Deinit();
        });
    });

    AX_VDEC_GRP_ATTR_T attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.enCodecType       = ToAxPayload(codec_type_);
    attr.enInputMode       = AX_VDEC_INPUT_MODE_STREAM;
    attr.u32MaxPicWidth    = static_cast<AX_U32>(width_);
    attr.u32MaxPicHeight   = static_cast<AX_U32>(height_);
    attr.u32StreamBufSize  = kStreamBuf;
    attr.bSdkAutoFramePool = AX_TRUE;
    attr.u32RefNum         = 2;

    state_->grp = kAxeraDecoderGrp;
    AX_S32 ret = AX_VDEC_CreateGrp(state_->grp, &attr);
    if (ret != 0) {
        LOG_ERRO("[Axera] AX_VDEC_CreateGrp failed: {}", ret);
        return false;
    }
    AX_VDEC_RECV_PIC_PARAM_T recv{};
    recv.s32RecvPicNum = -1;  // receive continuously
    ret = AX_VDEC_StartRecvStream(state_->grp, &recv);
    if (ret != 0) {
        AX_VDEC_DestroyGrp(state_->grp);
        LOG_ERRO("[Axera] AX_VDEC_StartRecvStream failed: {}", ret);
        return false;
    }
    LOG_INFO("[Axera] AX650 VDEC group {} opened ({}x{}, codec {})", state_->grp, width_, height_,
             static_cast<int>(codec_type_));
    return true;
}

bool VideoDecoderAxera::ReuseForStreamRestart(VideoCodecType type, int width, int height) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const bool same = (type == codec_type_) && (width == static_cast<int>(width_)) &&
                      (height == static_cast<int>(height_));
    if (!same && state_->opened) {
        CleanMpp();
        state_->opened = false;
    }
    if (fallback_) {
        return fallback_->ReuseForStreamRestart(type, width, height);
    }
    return true;
}

bool VideoDecoderAxera::SendPacket(const uint8_t* pkt, size_t len, int64_t frame_idx) {
    if (fallback_) {
        return fallback_->SendPacket(pkt, len, frame_idx);
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->opened || pkt == nullptr || len == 0) {
        return false;
    }
    AX_VDEC_STREAM_T stream{};
    stream.u64PTS          = static_cast<AX_U64>(frame_idx);
    stream.pu8Addr         = const_cast<AX_U8*>(pkt);
    stream.u32StreamPackLen = static_cast<AX_U32>(len);
    stream.bEndOfFrame     = AX_TRUE;
    stream.bEndOfStream    = AX_FALSE;
    const AX_S32 ret       = AX_VDEC_SendStream(state_->grp, &stream, 100);
    if (ret != 0) {
        LOG_WARN("[Axera] AX_VDEC_SendStream failed: {}", ret);
        return false;
    }
    return true;
}

VideoFramePtr VideoDecoderAxera::GetFrame() {
    if (fallback_) {
        return fallback_->GetFrame();
    }
    return ReceiveFrame();
}

DecodedVideoFrame VideoDecoderAxera::GetDecodedFrame() {
    if (fallback_) {
        return fallback_->GetDecodedFrame();
    }
    VideoFramePtr frame = ReceiveFrame();
    if (!frame) {
        return DecodedVideoFrame{};
    }
    return DecodedVideoFrame(frame);
}

VideoFramePtr VideoDecoderAxera::ReceiveFrame() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->opened) {
        return nullptr;
    }
    AX_VIDEO_FRAME_INFO_T frame_info{};
    const AX_S32 ret = AX_VDEC_GetChnFrame(state_->grp, kAxeraDecoderChn, &frame_info, 50);
    if (ret != 0) {
        return nullptr;  // timeout / no frame ready
    }
    const auto& frame = frame_info.stVFrame;

    // Materialize NV12 -> I420 compact VideoFrame (host copy-out path).
    const uint32_t width  = frame.u32Width;
    const uint32_t height = frame.u32Height;
    auto out = std::make_shared<VideoFrame>(static_cast<int>(width), static_cast<int>(height),
                                            PixelFormat::PIXEL_I420);
    if (!out || !out->Active()) {
        AX_VDEC_ReleaseChnFrame(state_->grp, kAxeraDecoderChn, &frame_info);
        return nullptr;
    }
    uint8_t* dst = out->GetData();
    // Prefer the virtual mapping when the SDK exposes one; a real board must
    // map the VB pool otherwise. Fall back to the CPU decoder if unmapped.
    const uint8_t* src_y = reinterpret_cast<const uint8_t*>(frame.u64VirAddr[0]);
    if (src_y == nullptr) {
        AX_VDEC_ReleaseChnFrame(state_->grp, kAxeraDecoderChn, &frame_info);
        LOG_WARN("[Axera] AX650 VDEC returned a frame without a virtual address");
        return nullptr;
    }
    // NV12 layout: Y plane with stride, then interleaved UV.
    const size_t stride_y  = frame.u32PicStride[0];
    const size_t stride_uv = frame.u32PicStride[1];
    const uint8_t* src_uv  = src_y + stride_y * height;
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(dst + y * width, src_y + y * stride_y, width);
    }
    // Deinterleave NV12 UV -> planar I420 U/V.
    const size_t y_size = static_cast<size_t>(width) * height;
    const size_t uv_size = y_size / 4;
    uint8_t* dst_u = dst + y_size;
    uint8_t* dst_v = dst + y_size + uv_size;
    for (uint32_t row = 0; row < height / 2; ++row) {
        const uint8_t* uv = src_uv + row * stride_uv;
        for (uint32_t x = 0; x < width / 2; ++x) {
            dst_u[row * (width / 2) + x] = uv[x * 2];
            dst_v[row * (width / 2) + x] = uv[x * 2 + 1];
        }
    }
    AX_VDEC_ReleaseChnFrame(state_->grp, kAxeraDecoderChn, &frame_info);
    out->SetFrameIndex(static_cast<int64_t>(state_->next_frame_index++));
    return out;
}

bool VideoDecoderAxera::IsOpened() {
    if (fallback_) {
        return fallback_->IsOpened();
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->opened;
}

void VideoDecoderAxera::CleanMpp() {
    if (state_->opened) {
        AX_VDEC_StopRecvStream(state_->grp);
        AX_VDEC_DestroyGrp(state_->grp);
        state_->opened = false;
    }
}

bool VideoDecoderAxera::Close() {
    if (fallback_) {
        return fallback_->Close();
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    CleanMpp();
    return true;
}

VideoDecoderCapability VideoDecoderAxera::Probe(VideoCodecType type) {
    VideoDecoderCapability cap;
    cap.available     = (type == VideoCodecType::kH264 || type == VideoCodecType::kH265);
    cap.backend       = "axera";
    cap.implementation = "ax_vdec_ax650_mpp";
    cap.detail        = "AX650 SoC VDEC (libax_vdec) - UNVERIFIED on hardware";
    return cap;
}

}  // namespace cosmo::media

#endif  // COSMO_MEDIA_USE_AXERA_BACKEND
