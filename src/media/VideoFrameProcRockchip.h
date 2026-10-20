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

    // ── Drawing ─────────────────────────────────────────────────────────
    //
    // RGA fills the annotation rectangles in place, so the frame stays in its
    // decoded format instead of being converted to BGR, drawn on by the CPU,
    // and converted back. That round trip is the single most expensive thing a
    // detection overlay does per frame.
    //
    // Only fill-based primitives are offloaded. Text is composed from glyph
    // bitmaps already built by the CPU renderer, so it keeps the inherited
    // implementation: moving it would trade a memcpy for a much larger RGA
    // staging cost and lose the font raster.
    VideoFramePtr DrawBox(VideoFramePtr srcImage, const util::Box imageRect, const Color& color,
                          int lineWidth) override;
    VideoFramePtr DrawRects(VideoFramePtr srcImage, const std::vector<util::Box>& rects, const Color& color,
                            int lineWidth) override;
    VideoFramePtr DrawLines(VideoFramePtr srcImage,
                            std::vector<std::pair<util::Point, util::Point>> lines, const Color& color,
                            int lineWidth) override;
    VideoFramePtr DrawPoint(VideoFramePtr srcImage, util::Point point, const Color& color,
                            int lineWidth) override;

private:
    VideoFramePtr ConvertWithRga(const VideoFramePtr& frame, PixelFormat dst_format, int src_rga_format,
                                 int dst_rga_format, int color_mode, const char* operation);
    VideoFramePtr ResizeWithRga(const VideoFramePtr& frame, int dst_height, int dst_width,
                                const char* operation);
    void LogFallbackOnce(const char* operation, int status);

    /// Fills `rects` on the frame through RGA, in place. Returns false when RGA
    /// cannot serve the request (unsupported pixel format, import failure, or a
    /// non-OK status), so the caller can fall back to the CPU implementation
    /// with the frame untouched.
    ///
    /// Clip rectangles to the frame before they reach here: RGA rejects
    /// out-of-bounds coordinates rather than clamping them.
    bool FillRectsInPlace(const VideoFramePtr& frame, const std::vector<util::Box>& rects, const Color& color,
                          const char* operation);
    /// Converts a util::Box stroke into the four filled rectangles the CPU
    /// implementation would draw, clipped to the frame. Returns an empty vector
    /// when the box lies entirely outside or degenerates after clipping.
    std::vector<util::Box> BoxToStrokeRects(const util::Box& box, int line_width, int width, int height) const;

    /// Runs one synchronous RGA operation on a frame staged through the dma32
    /// heap. Frames handed in through the generic VideoFrame API are owned by
    /// the host memory pool, whose pages can sit above 4 GiB, where RGA's
    /// 32-bit MMU cannot import them; the source is therefore copied into a
    /// physically-low dma32 pair, op runs against the staged dma-buf fds, and
    /// the result is copied back into the caller's heap buffer. op returns the
    /// RGA status and must not take any lock of its own: the staging helper
    /// holds the backend-wide RGA lock across the operation, the metrics
    /// record and the copy back, exactly as the two callers did before this
    /// preamble was factored out.
    template <typename RgaOp>
    bool RunRgaWithDma32Staging(const VideoFramePtr& frame, const VideoFramePtr& output, const char* operation,
                                RgaOp&& op);

    std::atomic<bool> fallback_warned_{false};
    /// Set once the first probe line (see RunRgaWithDma32Staging) has been
    /// emitted, so enabling COSMO_RGA_PATH_PROBE produces one line per
    /// outcome instead of one per frame.
    std::atomic<bool> rga_probe_enter_logged_{false};
    std::atomic<bool> rga_probe_success_logged_{false};
    std::atomic<bool> rga_probe_failure_logged_{false};
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
