#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "media/VideoFrameProcCpu.h"

namespace cosmo::media {

/// MPP hardware JPEG codec state (VideoFrameCodecRockchip.cc).
///
/// Kept opaque so this header stays free of MPP and RGA types.
struct RockchipJpegCodecState;

/// General-purpose Rockchip RGA frame processor.
///
/// Frames exposed through the generic VideoFrame API remain owned by
/// CosmoEdge's host memory pool. Each admitted RGA operation imports those
/// buffers for one synchronous operation. Detector and classifier fast paths
/// bypass this host-facing API and bind their MPP/RGA DMA-BUF inputs directly.
///
/// JPEG compress/decompress is likewise delegated to the SoC: the MPP jpegd and
/// jpege blocks handle the codec, and RGA performs the single colour conversion
/// between the codec's native layout and the packed BGR frames this interface
/// promises. The inherited stb implementation stays as the fallback.
class VideoFrameProcRockchip final : public VideoFrameProcCpu {
public:
    explicit VideoFrameProcRockchip(IOsdTextRenderer& osd_service);
    ~VideoFrameProcRockchip() override;

    VideoFramePtr BGR2I420(VideoFramePtr frame) override;
    VideoFramePtr RGB2I420(VideoFramePtr frame) override;
    VideoFramePtr I4202BGR(VideoFramePtr frame) override;
    VideoFramePtr I4202RGB(VideoFramePtr frame) override;
    VideoFramePtr Resize(VideoFramePtr src, int dst_height, int dst_width) override;

    std::vector<u_char> EncodeJpeg(const VideoFramePtr srcPicture) override;
    VideoFramePtr DecodeJpeg(const std::vector<u_int8_t>& data) override;

private:
    VideoFramePtr ConvertWithRga(const VideoFramePtr& frame, PixelFormat dst_format, int src_rga_format,
                                 int dst_rga_format, int color_mode, const char* operation);
    VideoFramePtr ResizeWithRga(const VideoFramePtr& frame, int dst_height, int dst_width,
                                const char* operation);
    void LogFallbackOnce(const char* operation, int status);

    std::atomic<bool> fallback_warned_{false};
    /// Consecutive failed RGA operations; the cooldown only arms after
    /// kRgaCooldownAfterFailures in a row so that isolated failures (a frame
    /// that landed on a malloc-backed pool block) do not suppress the hardware
    /// path for the frames that follow.
    std::atomic<int32_t> rga_consecutive_failures_{0};
    /// Steady-clock nanoseconds timestamp before which RGA is skipped; zero
    /// means RGA is eligible. A failed operation sets a cooldown instead of a
    /// permanent disable, so a transient supply problem (for example a drained
    /// dma32 heap) self-heals once frames from the heap appear again.
    std::atomic<int64_t> rga_retry_after_ns_{0};

    /// MPP contexts and their DMA-BUF pools are not re-entrant, so the codec is
    /// serialised per frame processor. Each processor serves one decode/encode
    /// consumer in practice; the lock keeps a channel reconfiguration safe.
    std::mutex jpeg_codec_mtx_;
    std::unique_ptr<RockchipJpegCodecState> jpeg_codec_;
};

}  // namespace cosmo::media
