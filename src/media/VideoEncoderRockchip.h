#pragma once

#include <memory>

#include "media/VideoEncoder.h"

namespace cosmo::media {

class VideoEncoderCpu;
struct RockchipEncoderState;

/// Shared Rockchip MPP encoder with an RGA-backed host-to-DMA boundary.
///
/// The caller still owns compact I420 host memory. Each frame is copied into a
/// reusable, stride-aligned MPP DMA-BUF through RGA before hardware encoding.
/// A measured CPU fallback remains available for unsupported RGA layouts.
///
/// MPP encodes YUV420SP (aka NV12) natively — see MPP_FMT_YUV420SP in
/// mpp_frame.h — so a caller holding decoder output can hand the planes over
/// directly via SendNV12Planes and skip the I420 repack entirely.
class VideoEncoderRockchip final : public VideoEncoder {
public:
    VideoEncoderRockchip();
    ~VideoEncoderRockchip() override;

    bool Open() override;
    VideoPacketPtr SendYUVFrame(void* data) override;

    /// Encodes NV12 planes without repacking: the luma plane is copied with its
    /// stride honoured and the interleaved UV plane is copied as-is into the
    /// MPP buffer's NV12 layout. One copy instead of "deinterleave to I420,
    /// then repack to the encoder", which is what SendYUVFrame costs.
    VideoPacketPtr SendNV12Planes(const NV12Planes& planes) override;
    bool AcceptsNV12Planes() const override { return true; }

    static VideoEncoderCapability Probe(VideoCodecType type);

private:
    bool OpenMpp();
    void Clean();
    void CleanMpp();

    std::unique_ptr<RockchipEncoderState> state_;
    std::unique_ptr<VideoEncoderCpu> fallback_;
};

}  // namespace cosmo::media
