#pragma once

#include <memory>
#include <string>

#include "media/VideoFrame.h"
#include "media/VideoPacket.h"

namespace cosmo {
namespace media {

    struct VideoEncoderCapability {
        bool available{false};
        std::string backend;
        std::string implementation;
        std::string detail;
    };

    /// One NV12 frame described by its planes, so a caller that already holds
    /// decoder output can hand it over without first repacking to compact I420.
    ///
    /// A decoder emits NV12 as two planes (Y, then interleaved UV) with hardware
    /// strides; compact I420 is a third layout that exists only because
    /// SendYUVFrame accepts a single contiguous buffer. Repacking costs a full
    /// frame copy per encoded frame, on the CPU, for a format the hardware
    /// encoder already takes natively.
    ///
    /// `y` and `uv` may point into any CPU-visible memory; a backend that needs
    /// device memory still copies, but one that can import the layout directly
    /// skips the repack. Offsets are in bytes, strides in bytes-per-row.
    struct NV12Planes {
        const uint8_t* y{nullptr};
        const uint8_t* uv{nullptr};
        int width{0};
        int height{0};
        int y_stride{0};
        int uv_stride{0};
        /// Stride multiple the producer guarantees, for backends that must pad.
        int uv_height_stride{0};
    };

    class VideoEncoder {
    public:
        explicit VideoEncoder();

        virtual ~VideoEncoder();

        /**
         * MUST be called before @ref Open()
         */
        void Set(VideoCodecType type, size_t width, size_t height);

        virtual bool Open() = 0;

        VideoPacketPtr Encode(VideoFramePtr frame);

        virtual VideoPacketPtr SendYUVFrame(void*) = 0;

        /// Encodes one NV12 frame straight from its planes.
        ///
        /// The default implementation repacks to compact I420 and calls
        /// SendYUVFrame, so every existing backend keeps working unchanged;
        /// only backends whose hardware takes NV12 natively override this and
        /// drop the repack. Returning nullptr means "not encoded by this
        /// backend" — callers then fall back to SendYUVFrame with a converted
        /// frame rather than losing the frame.
        ///
        /// STATUS: implemented, not yet wired. StreamViewerEncoder still
        /// converts every frame to I420 first, because the viewer path is handed
        /// host VideoFrames and the native NV12 descriptor stays behind in
        /// AlgData. Connecting this entry point means teaching the viewer path
        /// to carry that descriptor, which is a data-type change across the
        /// viewer chain rather than a local edit. Kept here so the encoder side
        /// is ready once measurements justify it.
        virtual VideoPacketPtr SendNV12Planes(const NV12Planes& planes);

        /// True when this backend consumes NV12 planes without a repack, so a
        /// caller can skip converting the decoder output in the first place.
        virtual bool AcceptsNV12Planes() const { return false; }

        size_t GetWidth() const;
        size_t GetHeight() const;

        /// Factory — creates the selected platform backend encoder.
        /// @param mediaHandle Device handle (used by hardware backends, ignored by CPU)
        static std::shared_ptr<VideoEncoder> Create(void* mediaHandle);

        /// Reports the deterministic implementation that Create/Open will use.
        /// A missing approved implementation is reported as unavailable instead
        /// of falling through to an arbitrary FFmpeg hardware encoder.
        static VideoEncoderCapability Probe(VideoCodecType type);

    protected:
        VideoCodecType codec_type_{VideoCodecType::kInvalid};

        size_t width_  = 1920;
        size_t height_ = 1080;
    };

}  // namespace media

}  // namespace cosmo
