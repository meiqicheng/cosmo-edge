#pragma once

#ifdef COSMO_MEDIA_USE_AXERA_BACKEND

#include <memory>
#include <string>

#include "media/VideoEncoder.h"

namespace cosmo::media {

class VideoEncoderCpu;
struct AxeraEncoderState;

/// AX650 hardware encoder built on the SoC-mode MPP stack (libax_venc).
///
/// I420 frames are sent to a VENC channel (AX_VENC_SendFrame) and the
/// compressed H.264 bitstream is received through the VENC stream API.
///
/// NOTE: this backend is UNVERIFIED on hardware until a real AX650N board is
/// available; it is compiled against the official AX650 SDK V3.10.2 headers.
class VideoEncoderAxera final : public VideoEncoder {
public:
    explicit VideoEncoderAxera();
    ~VideoEncoderAxera() override;

    bool Open() override;
    VideoPacketPtr SendYUVFrame(void*) override;

    static VideoEncoderCapability Probe(VideoCodecType type);

private:
    std::unique_ptr<AxeraEncoderState> state_;
    std::unique_ptr<VideoEncoderCpu> fallback_;
};

}  // namespace cosmo::media

#endif  // COSMO_MEDIA_USE_AXERA_BACKEND
