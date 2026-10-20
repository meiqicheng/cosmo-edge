// VideoEncoder — Video Encoder implementation.

#include "media/VideoEncoder.h"

#include <cstring>
#include <vector>

#include "util/Log.h"

namespace cosmo {
namespace media {

    VideoEncoder::VideoEncoder() {}

    VideoEncoder::~VideoEncoder() {}

    VideoPacketPtr VideoEncoder::SendNV12Planes(const NV12Planes& planes) {
        // Generic path: repack the two NV12 planes into the compact I420 that
        // SendYUVFrame expects. This is the copy the native path exists to
        // avoid, so it is only reached by backends that have not overridden
        // this method.
        if (planes.y == nullptr || planes.uv == nullptr || planes.width <= 0 || planes.height <= 0) {
            return nullptr;
        }
        if (planes.width != static_cast<int>(width_) || planes.height != static_cast<int>(height_)) {
            LOG_WARN("NV12 input mismatch: expected {}x{}, got {}x{}", width_, height_, planes.width,
                     planes.height);
            return nullptr;
        }

        const size_t w          = static_cast<size_t>(planes.width);
        const size_t h          = static_cast<size_t>(planes.height);
        const size_t luma_bytes = w * h;
        const size_t chroma_w   = w / 2;
        const size_t chroma_h   = h / 2;

        std::vector<uint8_t> packed(luma_bytes + chroma_w * chroma_h * 2);
        // Luma: honour the source stride so padded decoder output copies row by
        // row rather than assuming stride == width.
        const int y_stride = planes.y_stride > 0 ? planes.y_stride : static_cast<int>(w);
        for (size_t row = 0; row < h; ++row) {
            std::memcpy(packed.data() + row * w, planes.y + row * static_cast<size_t>(y_stride), w);
        }
        // Chroma: NV12 interleaves U and V per pixel; I420 keeps them in two
        // separate planes, so every other byte goes to a different destination.
        const int uv_stride = planes.uv_stride > 0 ? planes.uv_stride : static_cast<int>(w);
        uint8_t* dst_u      = packed.data() + luma_bytes;
        uint8_t* dst_v      = dst_u + chroma_w * chroma_h;
        for (size_t row = 0; row < chroma_h; ++row) {
            const uint8_t* src = planes.uv + row * static_cast<size_t>(uv_stride);
            for (size_t col = 0; col < chroma_w; ++col) {
                dst_u[row * chroma_w + col] = src[col * 2];
                dst_v[row * chroma_w + col] = src[col * 2 + 1];
            }
        }
        return SendYUVFrame(packed.data());
    }

    size_t VideoEncoder::GetWidth() const {
        return width_;
    }

    size_t VideoEncoder::GetHeight() const {
        return height_;
    }

    void VideoEncoder::Set(media::VideoCodecType type, size_t w, size_t h) {
        codec_type_ = type;
        width_      = w;
        height_     = h;
    }

    VideoPacketPtr VideoEncoder::Encode(VideoFramePtr frame) {
        if (!VideoFrameValid(frame)) {
            LOG_WARN("{}", "invalid frame");
            return nullptr;
        }
        if (frame->GetWidth() != width_ || frame->GetHeight() != height_ ||
            frame->GetPixelFormat() != PixelFormat::PIXEL_I420) {
            LOG_WARN("Encoder input mismatch: expected {}x{} I420, got {}x{} format {}", width_, height_,
                     frame->GetWidth(), frame->GetHeight(), frame->GetPixelFormat());
            return nullptr;
        }

        auto data = frame->GetData();
        auto pkt  = SendYUVFrame(data);
        if (!pkt) {
            LOG_WARN("{}", "send frame failed");
            return nullptr;
        }

        pkt->width      = frame->GetWidth();
        pkt->height     = frame->GetHeight();
        pkt->codec_type = codec_type_;
        pkt->timestamp  = frame->GetTimestamp();
        pkt->index      = static_cast<int64_t>(frame->GetFrameIndex());
        pkt->stream_idx = frame->GetStreamIndex();
        return pkt;
    }

}  // namespace media

}  // namespace cosmo
