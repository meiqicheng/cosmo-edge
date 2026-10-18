#include "media/VideoFrameProcRockchip.h"

#include <rga/im2d.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

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

}  // namespace cosmo::media
