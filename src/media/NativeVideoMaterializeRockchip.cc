// Rockchip implementation of NativeVideoMaterialize — P1-2 alarm-materialization
// gate. Converts a borrowed decoder DMA-BUF (NV12/I420/NV21, hardware strides)
// into a compact host I420 frame via a single synchronous RGA pass.
//
// The conversion pattern mirrors the decoder copy-out path
// (MaterializeMppFrame in VideoDecoderRockchip.cc): fd-only source contract
// for librga's legacy scheduler, per-thread dma32 staging buffer to avoid
// per-call allocator churn, and RgaGlobalLock serialization.

#include "media/NativeVideoMaterialize.h"

#include <rga/im2d.h>
#include <rga/rga.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

#include "media/RockchipRgaBuffer.h"
#include "util/Log.h"

namespace cosmo::media {

namespace {

    constexpr const char* kTag = "NativeMaterialize ";

    int ToRga420Format(NativeVideoBufferFormat format) {
        switch (format) {
            case NativeVideoBufferFormat::NV12:
                return RK_FORMAT_YCbCr_420_SP;
            case NativeVideoBufferFormat::NV21:
                return RK_FORMAT_YCrCb_420_SP;
            case NativeVideoBufferFormat::I420:
                return RK_FORMAT_YCbCr_420_P;
            default:
                return RK_FORMAT_UNKNOWN;
        }
    }

}  // namespace

VideoFramePtr MaterializeNativeBuffer(const NativeVideoBuffer& native) {
    if (!native.Valid()) {
        return nullptr;
    }
    const int source_format = ToRga420Format(native.format);
    if (source_format == RK_FORMAT_UNKNOWN) {
        return nullptr;
    }

    auto output = std::make_shared<VideoFrame>(native.width, native.height, PixelFormat::PIXEL_I420);
    if (!output || !output->Active() || !output->GetData()) {
        return nullptr;
    }

    // Per-thread staging buffer: MaterializeNativeBuffer runs on alarm-worker
    // threads, so the reuse must stay thread_local (same rationale as the
    // decoder copy-out path).
    static thread_local Dma32Buffer target_dma;
    if (target_dma.Size() != output->GetSize()) {
        target_dma.Release();
        if (!target_dma.Allocate(output->GetSize())) {
            target_dma.Release();
        }
    }

    IM_STATUS status = IM_STATUS_OUT_OF_MEMORY;
    if (target_dma.Size() == output->GetSize()) {
        // FD-only contract: pass the decoder-owned dma-buf directly so librga
        // retains the plane/stride metadata.
        const auto source = wrapbuffer_fd_t(native.fd, native.width, native.height, native.width_stride,
                                            native.height_stride, source_format);
        const auto target = RgaFdBuffer(target_dma.Fd(), native.width, native.height, RK_FORMAT_YCbCr_420_P);
        std::lock_guard<std::mutex> rga_lock(RgaGlobalLock());
        if (source_format == RK_FORMAT_YCbCr_420_P) {
            // Same-format copy still compacts the padded strides into the
            // staging buffer.
            const im_rect rect{0, 0, native.width, native.height};
            const im_rect empty_rect{};
            const rga_buffer_t empty_buffer{};
            status = improcess(source, target, empty_buffer, rect, rect, empty_rect, IM_SYNC);
        } else {
            status = imcvtcolor_t(source, target, source_format, RK_FORMAT_YCbCr_420_P, IM_COLOR_SPACE_DEFAULT, 1);
        }
    }

    if (RockchipRgaSucceeded(status)) {
        const auto* mapped = static_cast<const uint8_t*>(target_dma.Map());
        if (mapped) {
            std::memcpy(output->GetData(), mapped, output->GetSize());
            return output;
        }
    }

    static std::atomic<bool> warn_logged{false};
    if (!warn_logged.exchange(true)) {
        LOG_WARN("{} RGA DMA-BUF materialization failed with status {} ({}); alarm media will be skipped",
                 kTag, static_cast<int>(status), imStrError_t(status));
    }
    return nullptr;
}

}  // namespace cosmo::media
