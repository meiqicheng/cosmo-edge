#pragma once

// Internal MPP JPEG codec state for the Rockchip media backend.
//
// VideoFrameProcRockchip owns this state through a std::unique_ptr. The class
// constructor instantiates that pointer's deleter (the constructor's unwind
// path must be able to destroy the member), so the translation unit defining
// the constructor needs the complete type even though the destructor itself is
// defined in VideoFrameCodecRockchip.cc. The public header stays opaque and
// free of MPP headers for every other consumer, hence this internal header —
// included only by the two Rockchip media translation units.

#include <rockchip/mpp_buffer.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_vdec_cfg.h>
#include <rockchip/rk_venc_cfg.h>

#include <cstddef>

namespace cosmo::media {

/// Decoder and encoder contexts for the MPP hardware JPEG codec.
///
/// Both halves are lazily opened and latched unusable on failure, so a board
/// without jpegd/jpege never pays for a repeated failed probe.
struct RockchipJpegCodecState {
    // Decoder (jpegd).
    MppCtx decoder{nullptr};
    MppApi* decoder_api{nullptr};
    MppDecCfg decoder_cfg{nullptr};
    bool decoder_opened{false};
    bool decoder_unusable{false};

    // Encoder (jpege). The MPP contexts and their DMA-BUF pool are reusable
    // only while the picture geometry stays the same.
    MppCtx encoder{nullptr};
    MppApi* encoder_api{nullptr};
    MppEncCfg encoder_cfg{nullptr};
    MppBufferGroup encoder_group{nullptr};
    MppBuffer encoder_frame{nullptr};
    MppBuffer encoder_packet{nullptr};
    int encoder_width{0};
    int encoder_height{0};
    int encoder_hor_stride{0};
    int encoder_ver_stride{0};
    size_t encoder_frame_bytes{0};
    /// Layout the input frame is prepared in: MPP_FMT_YUV420SP when RGA wrote
    /// it (packed BGR/RGB source), MPP_FMT_YUV420P when the caller already
    /// handed us planar I420 and we staged it ourselves.
    MppFrameFormat encoder_input_format{MPP_FMT_YUV420SP};
    bool encoder_opened{false};
    bool encoder_unusable{false};
};

}  // namespace cosmo::media
