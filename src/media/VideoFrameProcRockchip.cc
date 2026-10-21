#include "media/VideoFrameProcRockchip.h"

#include <rga/im2d.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "media/PreviewPipelineMetrics.h"
#include "media/RockchipRgaBuffer.h"
#include "media/VideoFrameCodecRockchipInternal.h"
#include "util/Log.h"

namespace cosmo::media {
namespace {

    /// How long the CPU fallback stays in force after one failed RGA operation.
    constexpr int64_t kRgaRetryCooldownNs = 10'000'000'000;
    /// Consecutive failures that arm the cooldown above.
    constexpr int32_t kRgaCooldownAfterFailures = 4;

    uint64_t NowNanoseconds() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    uint64_t ElapsedNanoseconds(std::chrono::steady_clock::time_point started) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                .count());
    }

    /// Diagnostic gate for the RGA staging path. The happy path is otherwise
    /// silent, which makes it impossible to tell from a board log whether
    /// RunRgaWithDma32Staging() ran at all -- a question that matters because
    /// the path is only reachable through specific entry points (a decoder
    /// frame larger than kVideoDefaultWidth x kVideoDefaultHeight, or a
    /// BGR/RGB frame on its way to JPEG). Off by default so hot paths stay
    /// quiet; set COSMO_RGA_PATH_PROBE=1 to log the first entry and the first
    /// outcome of each kind.
    bool RgaPathProbeEnabled() {
        static const bool enabled = [] {
            const char* configured = std::getenv("COSMO_RGA_PATH_PROBE");
            return configured != nullptr && configured[0] != '\0' && configured[0] != '0';
        }();
        return enabled;
    }

    bool ClaimProbeSlot(std::atomic<bool>& slot) {
        return !slot.exchange(true, std::memory_order_relaxed);
    }

}  // namespace

VideoFrameProcRockchip::VideoFrameProcRockchip(IOsdTextRenderer& osd_service)
    : VideoFrameProcCpu(osd_service) {}

template <typename RgaOp>
bool VideoFrameProcRockchip::RunRgaWithDma32Staging(const VideoFramePtr& frame, const VideoFramePtr& output,
                                                    const char* operation, RgaOp&& op) {
    const auto started = std::chrono::steady_clock::now();
    const size_t src_bytes = frame->GetSize();
    const size_t dst_bytes = output->GetSize();
    const bool probe    = RgaPathProbeEnabled();
    if (probe && ClaimProbeSlot(rga_probe_enter_logged_)) {
        LOG_INFO("[RgaPathProbe] staging path entered: op={} src={}x{} ({} B) dst={}x{} ({} B)", operation,
                 frame->GetWidth(), frame->GetHeight(), src_bytes, output->GetWidth(), output->GetHeight(),
                 dst_bytes);
    }
    // Reports the first failing outcome once, so a log can distinguish "the
    // path never ran" from "the path ran and fell back to CPU".
    const auto probe_failure = [&](const char* reason, uint64_t elapsed) {
        if (probe && ClaimProbeSlot(rga_probe_failure_logged_)) {
            LOG_INFO("[RgaPathProbe] staging path failed: op={} reason={} src={}x{} dst={}x{} elapsed={}us",
                     operation, reason, frame->GetWidth(), frame->GetHeight(), output->GetWidth(),
                     output->GetHeight(), elapsed / 1000);
        }
    };
    Dma32Buffer src_dma;
    Dma32Buffer dst_dma;
    if (!src_dma.Allocate(src_bytes) || !dst_dma.Allocate(dst_bytes)) {
        const auto elapsed = ElapsedNanoseconds(started);
        GetPreviewPipelineMetrics().RecordRgaOperation(false, elapsed);
        LogFallbackOnce(operation, IM_STATUS_OUT_OF_MEMORY);
        probe_failure("dma32-allocation", elapsed);
        return false;
    }
    auto* src_va = static_cast<uint8_t*>(const_cast<void*>(src_dma.Map()));
    auto* dst_va = static_cast<uint8_t*>(const_cast<void*>(dst_dma.Map()));
    if (!src_va || !dst_va || frame->GetData() == nullptr || output->GetData() == nullptr) {
        const auto elapsed = ElapsedNanoseconds(started);
        GetPreviewPipelineMetrics().RecordRgaOperation(false, elapsed);
        LogFallbackOnce(operation, IM_STATUS_OUT_OF_MEMORY);
        probe_failure("mapping", elapsed);
        return false;
    }
    std::memcpy(src_va, frame->GetData(), src_bytes);

    const std::lock_guard<std::mutex> rga_lock(media::RgaGlobalLock());
    const auto status = op(src_dma.Fd(), dst_dma.Fd());
    const auto elapsed = ElapsedNanoseconds(started);
    GetPreviewPipelineMetrics().RecordRgaOperation(RockchipRgaSucceeded(status), elapsed);
    if (!RockchipRgaSucceeded(status)) {
        LogFallbackOnce(operation, status);
        probe_failure("rga-status", elapsed);
        return false;
    }
    std::memcpy(output->GetData(), dst_va, dst_bytes);
    rga_consecutive_failures_.store(0, std::memory_order_relaxed);
    fallback_warned_.store(false, std::memory_order_relaxed);
    if (probe && ClaimProbeSlot(rga_probe_success_logged_)) {
        LOG_INFO("[RgaPathProbe] staging path succeeded: op={} src={}x{} dst={}x{} elapsed={}us", operation,
                 frame->GetWidth(), frame->GetHeight(), output->GetWidth(), output->GetHeight(), elapsed / 1000);
    }
    return true;
}

VideoFramePtr VideoFrameProcRockchip::BGR2I420(VideoFramePtr frame) {
    auto converted = ConvertWithRga(frame, PixelFormat::PIXEL_I420, RK_FORMAT_BGR_888, RK_FORMAT_YCbCr_420_P,
                                    IM_RGB_TO_YUV_BT601_LIMIT, "BGR2I420");
    return converted ? converted : VideoFrameProcCpu::BGR2I420(std::move(frame));
}

VideoFramePtr VideoFrameProcRockchip::RGB2I420(VideoFramePtr frame) {
    auto converted = ConvertWithRga(frame, PixelFormat::PIXEL_I420, RK_FORMAT_RGB_888, RK_FORMAT_YCbCr_420_P,
                                    IM_RGB_TO_YUV_BT601_LIMIT, "RGB2I420");
    return converted ? converted : VideoFrameProcCpu::RGB2I420(std::move(frame));
}

VideoFramePtr VideoFrameProcRockchip::I4202BGR(VideoFramePtr frame) {
    auto converted = ConvertWithRga(frame, PixelFormat::PIXEL_BGR8, RK_FORMAT_YCbCr_420_P, RK_FORMAT_BGR_888,
                                    IM_YUV_TO_RGB_BT601_LIMIT, "I4202BGR");
    return converted ? converted : VideoFrameProcCpu::I4202BGR(std::move(frame));
}

VideoFramePtr VideoFrameProcRockchip::I4202RGB(VideoFramePtr frame) {
    auto converted = ConvertWithRga(frame, PixelFormat::PIXEL_RGB8, RK_FORMAT_YCbCr_420_P, RK_FORMAT_RGB_888,
                                    IM_YUV_TO_RGB_BT601_LIMIT, "I4202RGB");
    return converted ? converted : VideoFrameProcCpu::I4202RGB(std::move(frame));
}

VideoFramePtr VideoFrameProcRockchip::Resize(VideoFramePtr frame, int dst_height, int dst_width) {
    auto resized = ResizeWithRga(frame, dst_height, dst_width, "Resize");
    return resized ? resized : VideoFrameProcCpu::Resize(std::move(frame), dst_height, dst_width);
}

VideoFramePtr VideoFrameProcRockchip::ConvertWithRga(const VideoFramePtr& frame, PixelFormat dst_format,
                                                     int src_rga_format, int dst_rga_format, int color_mode,
                                                     const char* operation) {
    if (rga_retry_after_ns_.load(std::memory_order_relaxed) > static_cast<int64_t>(NowNanoseconds()))
        return nullptr;
    if (!VideoFrameValid(frame, true) || frame->GetWidth() == 0 || frame->GetHeight() == 0 ||
        frame->GetWidth() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        frame->GetHeight() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return nullptr;
    }

    const int width  = static_cast<int>(frame->GetWidth());
    const int height = static_cast<int>(frame->GetHeight());
    auto output      = std::make_shared<VideoFrame>(width, height, dst_format, frame->GetFrameIndex(),
                                                    frame->GetTimestamp());
    if (!VideoFrameValid(output, true)) {
        return nullptr;
    }
    output->SetStreamIndex(frame->GetStreamIndex());

    if (!RunRgaWithDma32Staging(frame, output, operation, [&](int src_fd, int dst_fd) {
            const auto src = RgaFdBuffer(src_fd, width, height, src_rga_format);
            const auto dst = RgaFdBuffer(dst_fd, width, height, dst_rga_format);
            return imcvtcolor_t(src, dst, src_rga_format, dst_rga_format, color_mode, 1);
        })) {
        return nullptr;
    }
    return output;
}

VideoFramePtr VideoFrameProcRockchip::ResizeWithRga(const VideoFramePtr& frame, int dst_height, int dst_width,
                                                    const char* operation) {
    if (rga_retry_after_ns_.load(std::memory_order_relaxed) > static_cast<int64_t>(NowNanoseconds()))
        return nullptr;
    if (!VideoFrameValid(frame, true) || frame->GetPixelFormat() != PixelFormat::PIXEL_I420 ||
        dst_width <= 0 || dst_height <= 0 ||
        frame->GetWidth() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        frame->GetHeight() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return nullptr;
    }

    const int src_width  = static_cast<int>(frame->GetWidth());
    const int src_height = static_cast<int>(frame->GetHeight());
    // RGA3 requires I420 width to be 16-pixel aligned; bail so caller uses CPU
    if (src_width % 16 != 0 || dst_width % 16 != 0) {
        return nullptr;
    }
    auto output          = std::make_shared<VideoFrame>(dst_width, dst_height, PixelFormat::PIXEL_I420,
                                                        frame->GetFrameIndex(), frame->GetTimestamp());
    if (!VideoFrameValid(output, true)) {
        return nullptr;
    }
    output->SetStreamIndex(frame->GetStreamIndex());

    if (!RunRgaWithDma32Staging(frame, output, operation, [&](int src_fd, int dst_fd) {
            const auto src = RgaFdBuffer(src_fd, src_width, src_height, RK_FORMAT_YCbCr_420_P);
            const auto dst = RgaFdBuffer(dst_fd, dst_width, dst_height, RK_FORMAT_YCbCr_420_P);
            return imresize_t(src, dst, 0.0, 0.0, INTER_LINEAR, 1);
        })) {
        return nullptr;
    }
    return output;
}

void VideoFrameProcRockchip::LogFallbackOnce(const char* operation, int status) {
    const auto failures = rga_consecutive_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (failures < kRgaCooldownAfterFailures) {
        // Isolated failure: the next frame may land on a low-address block and
        // succeed, so keep the hardware path armed.
        return;
    }
    rga_retry_after_ns_.store(static_cast<int64_t>(NowNanoseconds()) + kRgaRetryCooldownNs,
                              std::memory_order_relaxed);
    if (!fallback_warned_.exchange(true, std::memory_order_relaxed)) {
        LOG_WARN(
            "RGA {} failed {} times in a row (last status {} ({})); pausing RGA for {} s on this frame "
            "processor and using CPU fallback",
            operation, failures, status, imStrError_t(static_cast<IM_STATUS>(status)),
            kRgaRetryCooldownNs / 1'000'000'000);
    }
}

// ── Drawing ─────────────────────────────────────────────────────────────
//
// Annotations are filled in place on the decoded frame. The alternative the
// generic path takes — convert to BGR, draw with per-pixel loops in C++, convert
// back — costs two full-frame colour conversions per annotated frame, and those
// conversions are exactly what the detection overlay cannot afford at 25+
// channels. RGA fills the same rectangles in one pass on the video block instead.

namespace {

/// Mirrors the CPU renderer's accepted set (see VideoFrameProcCpu.cc): the
/// inherited implementation only draws on I420 and packed 8-bit RGB, so the RGA
/// path must not accept formats the fallback cannot render either.
bool IsDrawablePinFormat(PixelFormat format) {
    return format == PixelFormat::PIXEL_I420 || format == PixelFormat::PIXEL_BGR8 ||
           format == PixelFormat::PIXEL_RGB8;
}

/// RGA fill colour is a packed 32-bit value whose byte order follows the
/// destination format, so the channel order has to be chosen per format rather
/// than passed as RGB and left to RGA.
///
/// Returns false for formats RGA cannot fill meaningfully, which keeps packed
/// YUV and unknown layouts on the CPU path instead of writing a wrong colour.
bool ToRgaFillColor(PixelFormat format, const Color& color, int* out) {
    switch (format) {
        case PixelFormat::PIXEL_BGR8:
            // RGA reads the low byte first for BGR888.
            *out = (color.red << 16) | (color.green << 8) | color.blue;
            return true;
        case PixelFormat::PIXEL_RGB8:
            *out = (color.blue << 16) | (color.green << 8) | color.red;
            return true;
        case PixelFormat::PIXEL_I420:
        case PixelFormat::PIXEL_NV12:
            // RGB, not a precomputed luma: librga converts the fill colour into
            // the target's own colourspace. Measured on librga 1.8.1 against an
            // NV12 buffer -- 0x0000FF lands as Y=82 U=90 V=240, 0xFF0000 as
            // Y=41 U=240 V=110, 0x000000 as Y=16 U=128 V=128 -- so imfill
            // writes all three planes itself.
            //
            // The earlier luma-only assumption was wrong, and it justified a
            // chroma pre-pass that turned out to be unnecessary work: one fill
            // already produces the correct tint.
            *out = (color.red << 16) | (color.green << 8) | color.blue;
            return true;
        default:
            return false;
    }
}

/// RGA format id for the formats this path fills. Mirrors the mapping the
/// conversion functions already use.
int ToRgaFillFormat(PixelFormat format) {
    switch (format) {
        case PixelFormat::PIXEL_BGR8:
            return RK_FORMAT_BGR_888;
        case PixelFormat::PIXEL_RGB8:
            return RK_FORMAT_RGB_888;
        case PixelFormat::PIXEL_I420:
            return RK_FORMAT_YCbCr_420_P;
        case PixelFormat::PIXEL_NV12:
            return RK_FORMAT_YCbCr_420_SP;
        default:
            return RK_FORMAT_UNKNOWN;
    }
}

/// YUV420 stores chroma at half resolution, so librga rejects a fill rectangle
/// whose origin or extent is odd ("Error yuv not align to 2"). The annotation
/// geometry is not chroma aware — a horizontal tripwire is one or three pixels
/// tall and an arbitrary detection edge can land on an odd coordinate — so the
/// rectangle is grown outward to the next even boundary before submission.
///
/// Growing rather than shrinking keeps the marker covering the pixels the caller
/// asked for, and the few extra chroma-aligned pixels are invisible on a
/// two-by-two block. The result is clipped to the frame, which can only be an
/// issue for a one-pixel marker on the last row or column; such a rectangle
/// collapses to nothing and is dropped, exactly as an off-frame rect already is.
bool AlignRectToChromaGrid(int width, int height, im_rect* rect) {
    if (rect == nullptr || width <= 0 || height <= 0) {
        return false;
    }

    int x0 = rect->x < 0 ? 0 : rect->x;
    int y0 = rect->y < 0 ? 0 : rect->y;
    int x1 = static_cast<int>(rect->x) + static_cast<int>(rect->width);
    int y1 = static_cast<int>(rect->y) + static_cast<int>(rect->height);
    x1     = x1 > width ? width : x1;
    y1     = y1 > height ? height : y1;
    if (x0 >= x1 || y0 >= y1) {
        return false;
    }

    // Round the exclusive far edge up, then the inclusive origin down, so the box
    // only ever grows. The frame's own extent may be odd (a 1081-line buffer), so
    // re-clipping can land back on an odd edge: that edge is then pulled one step
    // inward, which shrinks the box by a single pixel rather than sending librga
    // an unaligned rectangle.
    x1 = (x1 + 1) & ~1;
    y1 = (y1 + 1) & ~1;
    x0 &= ~1;
    y0 &= ~1;
    if (x1 > width) {
        x1 = width & ~1;
    }
    if (y1 > height) {
        y1 = height & ~1;
    }
    if (x0 >= x1 || y0 >= y1) {
        return false;
    }

    rect->x      = x0;
    rect->y      = y0;
    rect->width  = x1 - x0;
    rect->height = y1 - y0;
    return rect->width > 0 && rect->height > 0;
}

/// True when the RGA format stores chroma at half resolution in both axes.
bool RgaFormatNeedsChromaAlignment(int rga_format) {
    return rga_format == RK_FORMAT_YCbCr_420_P || rga_format == RK_FORMAT_YCbCr_420_SP;
}


}  // namespace

std::vector<util::Box> VideoFrameProcRockchip::BoxToStrokeRects(const util::Box& box, int line_width, int width,
                                                                int height) const {
    std::vector<util::Box> rects;
    if (box.width <= 0 || box.height <= 0 || line_width <= 0 || width <= 0 || height <= 0) {
        return rects;
    }

    // Same geometry as VideoFrameProcCpu::DrawBox: an inclusive pixel span, so
    // the far edge sits at x + width - 1. Clipping happens here because RGA
    // rejects out-of-bounds rectangles instead of clamping them.
    const int x1 = box.x;
    const int y1 = box.y;
    const int x2 = box.x + box.width - 1;
    const int y2 = box.y + box.height - 1;

    const int cl_x1 = x1 < 0 ? 0 : x1;
    const int cl_y1 = y1 < 0 ? 0 : y1;
    const int cl_x2 = x2 > width - 1 ? width - 1 : x2;
    const int cl_y2 = y2 > height - 1 ? height - 1 : y2;
    if (cl_x1 > cl_x2 || cl_y1 > cl_y2) {
        return rects;
    }

    const int span_w   = cl_x2 - cl_x1 + 1;
    const int span_h   = cl_y2 - cl_y1 + 1;
    // A box thinner than the stroke would otherwise draw four overlapping bars
    // and paint over its own interior; one filled rect is the honest result.
    const int stroke   = line_width * 2 >= span_w || line_width * 2 >= span_h
                             ? (span_w < span_h ? span_w : span_h)
                             : line_width;

    // Top and bottom bars span the full width; left and right bars fill the gap
    // between them so corners are covered exactly once.
    rects.push_back(util::Box(cl_x1, cl_y1, span_w, stroke));
    rects.push_back(util::Box(cl_x1, cl_y2 - stroke + 1, span_w, stroke));

    const int inner_y      = cl_y1 + stroke;
    const int inner_height = span_h - stroke * 2;
    if (inner_height > 0) {
        rects.push_back(util::Box(cl_x1, inner_y, stroke, inner_height));
        rects.push_back(util::Box(cl_x2 - stroke + 1, inner_y, stroke, inner_height));
    }
    return rects;
}

bool VideoFrameProcRockchip::FillRectsInPlace(const VideoFramePtr& frame,
                                              const std::vector<util::Box>& rects, const Color& color,
                                              const char* operation) {
    if (!frame || rects.empty()) {
        return false;
    }
    if (rga_retry_after_ns_.load(std::memory_order_relaxed) > static_cast<int64_t>(NowNanoseconds())) {
        return false;
    }

    const int width  = static_cast<int>(frame->GetWidth());
    const int height = static_cast<int>(frame->GetHeight());
    if (width <= 0 || height <= 0) {
        return false;
    }

    int fill_color = 0;
    if (!ToRgaFillColor(frame->GetPixelFormat(), color, &fill_color)) {
        return false;
    }
    const int rga_format = ToRgaFillFormat(frame->GetPixelFormat());
    if (rga_format == RK_FORMAT_UNKNOWN) {
        return false;
    }

    // imfillArray takes every rectangle in one ioctl, so a detection pass costs
    // one RGA submission rather than one per box.
    const bool needs_alignment = RgaFormatNeedsChromaAlignment(rga_format);
    std::vector<im_rect> native_rects;
    native_rects.reserve(rects.size());
    for (const auto& r : rects) {
        if (r.width <= 0 || r.height <= 0) {
            continue;
        }
        im_rect native {};
        native.x      = r.x;
        native.y      = r.y;
        native.width  = r.width;
        native.height = r.height;
        if (needs_alignment && !AlignRectToChromaGrid(width, height, &native)) {
            continue;
        }
        native_rects.push_back(native);
    }
    if (native_rects.empty()) {
        return false;
    }

    // The frame is mapped into a dma32 staging buffer first. Unlike the
    // conversion helpers this path has no separate output, so the staged copy
    // is written back to the same buffer the decoder handed us — which is why
    // drawing is in-place and the frame pointer is returned unchanged.
    const size_t bytes = frame->GetSize();
    if (bytes == 0) {
        return false;
    }
    Dma32Buffer staged;
    if (!staged.Allocate(bytes)) {
        return false;
    }
    auto* staged_va = static_cast<uint8_t*>(const_cast<void*>(staged.Map()));
    if (staged_va == nullptr) {
        return false;
    }
    std::memcpy(staged_va, frame->GetData(), bytes);

    const auto started = std::chrono::steady_clock::now();
    {
        const std::lock_guard<std::mutex> rga_lock(media::RgaGlobalLock());
        auto target = RgaFdBuffer(staged.Fd(), width, height, rga_format);
        const IM_STATUS status =
            imfillArray(target, native_rects.data(), static_cast<int>(native_rects.size()),
                        static_cast<uint32_t>(fill_color), 1, nullptr);
        const auto elapsed = ElapsedNanoseconds(started);
        GetPreviewPipelineMetrics().RecordRgaOperation(RockchipRgaSucceeded(status), elapsed);
        if (!RockchipRgaSucceeded(status)) {
            LogFallbackOnce(operation, status);
            return false;
        }

    }

    std::memcpy(frame->GetData(), staged_va, bytes);
    rga_consecutive_failures_.store(0, std::memory_order_relaxed);
    fallback_warned_.store(false, std::memory_order_relaxed);
    return true;
}

VideoFramePtr VideoFrameProcRockchip::DrawBox(VideoFramePtr srcImage, const util::Box imageRect,
                                              const Color& color, int lineWidth) {
    if (!VideoFrameValid(srcImage, true) || !IsDrawablePinFormat(srcImage->GetPixelFormat())) {
        return srcImage;
    }
    const auto rects = BoxToStrokeRects(imageRect, lineWidth, static_cast<int>(srcImage->GetWidth()),
                                        static_cast<int>(srcImage->GetHeight()));
    if (!FillRectsInPlace(srcImage, rects, color, "DrawBox")) {
        return VideoFrameProcCpu::DrawBox(std::move(srcImage), imageRect, color, lineWidth);
    }
    return srcImage;
}

VideoFramePtr VideoFrameProcRockchip::DrawRects(VideoFramePtr srcImage, const std::vector<util::Box>& rects,
                                                const Color& color, int lineWidth) {
    if (!VideoFrameValid(srcImage, true) || !IsDrawablePinFormat(srcImage->GetPixelFormat()) || rects.empty()) {
        return srcImage;
    }

    // Every box expands to four stroke bars, but they all go into one
    // imfillArray call, so N detections still cost a single submission.
    std::vector<util::Box> strokes;
    strokes.reserve(rects.size() * 4);
    const int width  = static_cast<int>(srcImage->GetWidth());
    const int height = static_cast<int>(srcImage->GetHeight());
    for (const auto& box : rects) {
        const auto expanded = BoxToStrokeRects(box, lineWidth, width, height);
        strokes.insert(strokes.end(), expanded.begin(), expanded.end());
    }
    if (!FillRectsInPlace(srcImage, strokes, color, "DrawRects")) {
        return VideoFrameProcCpu::DrawRects(std::move(srcImage), rects, color, lineWidth);
    }
    return srcImage;
}

VideoFramePtr VideoFrameProcRockchip::DrawLines(VideoFramePtr srcImage,
                                                std::vector<std::pair<util::Point, util::Point>> lines,
                                                const Color& color, int lineWidth) {
    if (!VideoFrameValid(srcImage, true) || !IsDrawablePinFormat(srcImage->GetPixelFormat()) || lines.empty()) {
        return srcImage;
    }

    // RGA fills rectangles and has no line primitive, so each segment becomes
    // the axis-aligned bounding box it spans. A diagonal therefore draws as a
    // box, not a stroke: for the vertical and horizontal reference lines this
    // interface is used for (count lines, tripwires) the result is identical,
    // and for a diagonal the CPU fallback keeps the exact geometry.
    std::vector<util::Box> rects;
    rects.reserve(lines.size());
    for (const auto& line : lines) {
        const int x_lo = line.first.x < line.second.x ? line.first.x : line.second.x;
        const int x_hi = line.first.x < line.second.x ? line.second.x : line.first.x;
        const int y_lo = line.first.y < line.second.y ? line.first.y : line.second.y;
        const int y_hi = line.first.y < line.second.y ? line.second.y : line.first.y;
        const bool axis_aligned = (x_lo == x_hi) || (y_lo == y_hi);
        if (!axis_aligned) {
            return VideoFrameProcCpu::DrawLines(std::move(srcImage), lines, color, lineWidth);
        }
        // Grow a zero-width or zero-height span so the stroke has area.
        const int rect_w = (x_hi - x_lo) + (x_lo == x_hi ? lineWidth : 1);
        const int rect_h = (y_hi - y_lo) + (y_lo == y_hi ? lineWidth : 1);
        rects.push_back(util::Box(x_lo, y_lo, rect_w, rect_h));
    }

    if (!FillRectsInPlace(srcImage, rects, color, "DrawLines")) {
        return VideoFrameProcCpu::DrawLines(std::move(srcImage), lines, color, lineWidth);
    }
    return srcImage;
}

VideoFramePtr VideoFrameProcRockchip::DrawPoint(VideoFramePtr srcImage, util::Point point, const Color& color,
                                                int lineWidth) {
    if (!VideoFrameValid(srcImage, true) || !IsDrawablePinFormat(srcImage->GetPixelFormat())) {
        return srcImage;
    }
    const auto rects = BoxToStrokeRects(util::Box(point.x, point.y, 1, 1), lineWidth,
                                        static_cast<int>(srcImage->GetWidth()),
                                        static_cast<int>(srcImage->GetHeight()));
    if (rects.empty() || !FillRectsInPlace(srcImage, rects, color, "DrawPoint")) {
        return VideoFrameProcCpu::DrawPoint(std::move(srcImage), point, color, lineWidth);
    }
    return srcImage;
}

}  // namespace cosmo::media
