#pragma once

#ifdef COSMO_MEDIA_USE_AXERA_BACKEND

#include <memory>
#include <string>

#include "media/VideoDecoder.h"

namespace cosmo::media {

class VideoDecoderCpu;
struct AxeraDecoderState;

/// AX650 hardware decoder built on the SoC-mode MPP stack (libax_vdec).
///
/// Compressed H.264/H.265 packets are sent to a VDEC group/channel
/// (AX_VDEC_SendStream) and decoded frames are received through
/// AX_VDEC_GetChnFrame / AX_VDEC_ReleaseChnFrame. Decoded AX_VIDEO_FRAME_T
/// data is materialized into the compact I420 VideoFrame expected by host-only
/// business consumers (matching the Rockchip copy-out path).
///
/// NOTE: this backend is UNVERIFIED on hardware until a real AX650N board is
/// available; it is compiled against the official AX650 SDK V3.10.2 headers.
class VideoDecoderAxera final : public VideoDecoder {
public:
    explicit VideoDecoderAxera(size_t name);
    ~VideoDecoderAxera() override;

    bool Open() override;
    bool Close() override;
    bool IsOpened() override;
    bool ReuseForStreamRestart(VideoCodecType type, int width, int height) override;

    bool SendPacket(const uint8_t* pkt, size_t len, int64_t frame_idx) override;
    VideoFramePtr GetFrame() override;
    DecodedVideoFrame GetDecodedFrame() override;

    static VideoDecoderCapability Probe(VideoCodecType type);

private:
    bool OpenMpp();
    VideoFramePtr ReceiveFrame();
    void CleanMpp();

    std::unique_ptr<AxeraDecoderState> state_;
    std::unique_ptr<VideoDecoderCpu> fallback_;
};

}  // namespace cosmo::media

#endif  // COSMO_MEDIA_USE_AXERA_BACKEND
