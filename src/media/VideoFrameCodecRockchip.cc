// VideoFrameCodecRockchip.cc — MPP hardware JPEG encode/decode for the Rockchip
// frame processor.
//
// Before this file the Rockchip backend inherited VideoFrameProcCpu's stb
// implementation, so every JPEG entering or leaving the engine was expanded and
// compressed on the CPU while the SoC's jpegd/jpege blocks sat idle. This file
// drives that hardware through MPP and keeps the stb path as the explicit
// fallback, mirroring how VideoFrameCodec.cc splits the same concern for Sophon.
//
// Decode contract (identical to VideoFrameProcCpu::DecodeJpeg so callers cannot
// tell the backends apart): a tightly packed PIXEL_BGR8 frame. The decoder
// writes NV12 into its own DMA-BUF and RGA performs the single NV12 -> BGR888
// conversion, so no pixel is touched by the CPU.
//
// The decoder handshake is the one rockchip's mpi_dec_test uses for MJPEG
// (mpp/test/mpi_dec_test.c: dec_advanced), which differs from the H.264 output
// path in one decisive way: a still JPEG decoder will not allocate its own
// output, so the caller must hand it a preallocated MppFrame through the
// packet's KEY_OUTPUT_FRAME metadata. Without that handshake decode_get_frame
// polls until timeout and the jpegd block is never driven.
//
// Encode side: MPP consumes YUV, but the only conversion the RGA cores will
// accept here is the semi-planar one. The kernel's core-selection log says so
// directly -- rga3 refuses planar YUV420 ("break on feature", rockchip FAQ
// Q4.5: 4:2:2/4:2:0 planar is RGA2-only) and only RGA2 will take it, which then
// cannot reach the engine's host-memory source above 4G:
//
//   rga: ID[563]: RGA3_core0(0x1), break on feature
//   rga: ID[563]: matched cores = 0x4, assign core: RGA2_core0(0x4)
//
// So RGA converts the packed frame straight into the encoder's DMA-BUF as NV12
// (rga3, no 4G ceiling) and MPP is configured with prep:format=YUV420SP. That
// also removes the CPU staging copy. The probe that established this is
// output/agent-runs/mpp-headers-ref/mpp_jpeg_enc_probe8.cc, which measured
// jpege-core +1 / rga3 +2 and a valid 32158-byte JPEG for a 640x426 frame.

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>

#include "media/VideoFrameProcRockchip.h"

#define MODULE_TAG "cosmo_mpp_jpeg"
// librga's headers reference NULL without including <cstddef> themselves.
#include <rga/im2d.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_vdec_cfg.h>
#include <rockchip/rk_venc_cfg.h>

#include "media/EncodedImageInfo.h"
#include "media/PreviewPipelineMetrics.h"
#include "media/RockchipRgaBuffer.h"
#include "media/VideoFrameCodecRockchipInternal.h"
#include "util/Log.h"

namespace cosmo::media {
namespace {

    /// MPP / RGA layout alignment for both directions.
    constexpr int kMppAlignment = 16;
    /// Number of frames the jpegd HAL may need to hold for one still image.
    /// rockchip's own mpi_dec_test sizes the preallocated output the same way,
    /// with the comment that JPEG may be 4:2:0 or 4:2:2 so the buffer has to be
    /// larger than the YUV420 minimum. The RK3588 jpegd HAL enforces this: a
    /// tightly sized NV12 buffer makes mpp_dec_advanced_thread reject the frame.
    constexpr int kDecodeFrameFactor      = 4;
    constexpr int kDecodeBufferCount      = 2;
    constexpr int kDecodeGetFrameAttempts = 16;
    constexpr int kDecodeTimeoutMs        = 1000;

    /// stb's default in VideoFrameProcCpu, kept identical so switching backends
    /// does not change the delivered picture. MPP's jpeg:q_factor is 1..99.
    constexpr int kJpegQuality    = 95;
    constexpr int kJpegQualityMax = 99;
    constexpr int kJpegQualityMin = 1;
    /// MppEncJpegQpMode: JPEG_QFACTOR makes jpeg:q_factor the quality knob.
    constexpr int kJpegQpModeQFactor = 2;

    int AlignUp(int value, int alignment) {
        return (value + alignment - 1) / alignment * alignment;
    }

    uint64_t ElapsedNanoseconds(std::chrono::steady_clock::time_point started) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                .count());
    }

    /// MPP's JPEG decoder only speaks JPEG. PNG/BMP payloads must reach the stb
    /// path without first failing a hardware attempt.
    bool IsJpegPayload(const std::vector<u_int8_t>& data) {
        return data.size() > 2 && data[0] == 0xFF && data[1] == 0xD8;
    }

    bool IsCompactI420(const VideoFrame& frame) {
        const size_t width  = frame.GetWidth();
        const size_t height = frame.GetHeight();
        if (width == 0 || height == 0 || width % 2 != 0 || height % 2 != 0) {
            return false;
        }
        return frame.GetSize() >= width * height * 3 / 2;
    }

    struct MppBufferGuard {
        MppBuffer buffer{nullptr};
        ~MppBufferGuard() {
            if (buffer) {
                mpp_buffer_put(buffer);
            }
        }
    };

    struct MppBufferGroupGuard {
        MppBufferGroup group{nullptr};
        ~MppBufferGroupGuard() {
            if (group) {
                mpp_buffer_group_put(group);
            }
        }
    };

    struct MppFrameGuard {
        MppFrame frame{nullptr};
        ~MppFrameGuard() {
            if (frame) {
                mpp_frame_deinit(&frame);
            }
        }
    };

    struct MppPacketGuard {
        MppPacket packet{nullptr};
        ~MppPacketGuard() {
            if (packet) {
                mpp_packet_deinit(&packet);
            }
        }
    };

    void LogFirstHardwareJpeg(const char* direction, int width, int height) {
        static std::atomic_flag logged = ATOMIC_FLAG_INIT;
        if (!logged.test_and_set(std::memory_order_relaxed)) {
            LOG_INFO("MPP hardware JPEG {} enabled: {}x{} codec=VPE/JPG, colour conversion=RGA", direction,
                     width, height);
        }
    }

    void LogDecoderFallback(const char* reason, int status) {
        static std::atomic_flag logged = ATOMIC_FLAG_INIT;
        if (!logged.test_and_set(std::memory_order_relaxed)) {
            LOG_WARN(
                "MPP hardware JPEG decode unavailable ({} status {}); falling back to the stb software "
                "decoder",
                reason, status);
        }
    }

    void LogEncoderFallback(const char* reason, int status) {
        static std::atomic_flag logged = ATOMIC_FLAG_INIT;
        if (!logged.test_and_set(std::memory_order_relaxed)) {
            LOG_WARN(
                "MPP hardware JPEG encode unavailable ({} status {}); falling back to the stb software "
                "encoder",
                reason, status);
        }
    }

}  // namespace

namespace {

    // ── decoder ─────────────────────────────────────────────────────────────

    bool OpenJpegDecoder(RockchipJpegCodecState& state) {
        if (access("/dev/mpp_service", R_OK | W_OK) != 0) {
            LogDecoderFallback("/dev/mpp_service is not accessible", 0);
            return false;
        }
        if (mpp_check_support_format(MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG) != MPP_OK) {
            LogDecoderFallback("MJPEG decode is not registered", 0);
            return false;
        }
        auto ret = mpp_create(&state.decoder, &state.decoder_api);
        if (ret != MPP_OK || !state.decoder || !state.decoder_api) {
            LogDecoderFallback("mpp_create failed", ret);
            return false;
        }
        ret = mpp_init(state.decoder, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG);
        if (ret != MPP_OK) {
            LogDecoderFallback("mpp_init failed", ret);
            return false;
        }
        ret = mpp_dec_cfg_init(&state.decoder_cfg);
        if (ret != MPP_OK || !state.decoder_cfg) {
            LogDecoderFallback("mpp_dec_cfg_init failed", ret);
            return false;
        }
        ret = state.decoder_api->control(state.decoder, MPP_DEC_GET_CFG, state.decoder_cfg);
        if (ret != MPP_OK) {
            LogDecoderFallback("MPP_DEC_GET_CFG failed", ret);
            return false;
        }
        // split_parse = 1 selects the "advanced" decode path, which is the only
        // one that accepts a caller-provided output frame. MJPEG always uses it.
        ret = mpp_dec_cfg_set_u32(state.decoder_cfg, "base:split_parse", 1);
        if (ret != MPP_OK) {
            LogDecoderFallback("base:split_parse rejected", ret);
            return false;
        }
        ret = state.decoder_api->control(state.decoder, MPP_DEC_SET_CFG, state.decoder_cfg);
        if (ret != MPP_OK) {
            LogDecoderFallback("MPP_DEC_SET_CFG failed", ret);
            return false;
        }

        // NV12 is the layout RGA can convert straight to packed BGR888.
        MppFrameFormat output_format = MPP_FMT_YUV420SP;
        ret = state.decoder_api->control(state.decoder, MPP_DEC_SET_OUTPUT_FORMAT, &output_format);
        if (ret != MPP_OK) {
            LogDecoderFallback("MPP_DEC_SET_OUTPUT_FORMAT(NV12) failed", ret);
            return false;
        }

        MppPollType timeout = static_cast<MppPollType>(kDecodeTimeoutMs);
        ret                 = state.decoder_api->control(state.decoder, MPP_SET_OUTPUT_TIMEOUT, &timeout);
        if (ret != MPP_OK) {
            LogDecoderFallback("MPP_SET_OUTPUT_TIMEOUT failed", ret);
            return false;
        }

        state.decoder_opened = true;
        return true;
    }

    /// Releases the decode context. A fresh context is built on the next call so
    /// a partially consumed image can never be mistaken for the next one.
    void DiscardJpegDecoder(RockchipJpegCodecState& state) {
        if (state.decoder_cfg) {
            mpp_dec_cfg_deinit(&state.decoder_cfg);
            state.decoder_cfg = nullptr;
        }
        if (state.decoder) {
            mpp_destroy(state.decoder);
            state.decoder     = nullptr;
            state.decoder_api = nullptr;
        }
        state.decoder_opened = false;
    }

    VideoFramePtr ConvertDecodedJpegToBgr(MppFrame decoded) {
        const auto buffer = mpp_frame_get_buffer(decoded);
        if (!buffer) {
            return nullptr;
        }
        const int width       = static_cast<int>(mpp_frame_get_width(decoded));
        const int height      = static_cast<int>(mpp_frame_get_height(decoded));
        const int hor_stride  = static_cast<int>(mpp_frame_get_hor_stride(decoded));
        const int ver_stride  = static_cast<int>(mpp_frame_get_ver_stride(decoded));
        const int crop_width  = width & ~1;
        const int crop_height = height & ~1;
        const size_t required = static_cast<size_t>(hor_stride) * ver_stride * 3 / 2;
        if (crop_width <= 0 || crop_height <= 0 || hor_stride < width || ver_stride < height ||
            hor_stride % 2 != 0 || ver_stride % 2 != 0 || hor_stride > std::numeric_limits<int>::max() ||
            ver_stride > std::numeric_limits<int>::max() || mpp_buffer_get_size(buffer) < required) {
            LOG_WARN("MPP JPEG decoder produced an unusable layout: {}x{} stride={}x{}", width, height,
                     hor_stride, ver_stride);
            return nullptr;
        }

        auto output = std::make_shared<VideoFrame>(crop_width, crop_height, PixelFormat::PIXEL_BGR8);
        if (!VideoFrameValid(output, true)) {
            return nullptr;
        }

        const auto started = std::chrono::steady_clock::now();
        ScopedRgaBufferHandle source_handle;
        ScopedRgaBufferHandle target_handle(output->GetData(), output->GetSize());
        source_handle.ImportFd(mpp_buffer_get_fd(buffer), mpp_buffer_get_size(buffer));
        IM_STATUS status = IM_STATUS_OUT_OF_MEMORY;
        if (source_handle && target_handle) {
            // imcvtcolor derives its rect from the source geometry, so the even
            // crop is declared on the source while the real strides are kept.
            auto source = wrapbuffer_handle_t(source_handle.Get(), crop_width, crop_height, hor_stride,
                                              ver_stride, RK_FORMAT_YCbCr_420_SP);
            auto target = wrapbuffer_handle_t(target_handle.Get(), crop_width, crop_height, crop_width,
                                              crop_height, RK_FORMAT_BGR_888);
            // JPEG carries full-range BT.601; the limited-range mode this file
            // would inherit from the video paths washes the picture out.
            status = imcvtcolor_t(source, target, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888,
                                  IM_YUV_TO_RGB_BT601_FULL, 1);
        }
        GetPreviewPipelineMetrics().RecordRgaOperation(RockchipRgaSucceeded(status),
                                                       ElapsedNanoseconds(started));
        if (!RockchipRgaSucceeded(status)) {
            LOG_WARN("RGA NV12 -> BGR888 conversion failed with status {} ({})", status,
                     imStrError_t(status));
            return nullptr;
        }
        return output;
    }

    VideoFramePtr DecodeJpegHardware(RockchipJpegCodecState& state, const std::vector<u_int8_t>& jpeg,
                                     const EncodedImageInfo& info) {
        if (info.width <= 0 || info.height <= 0) {
            return nullptr;
        }
        const int hor_stride     = AlignUp(info.width, kMppAlignment);
        const int ver_stride     = AlignUp(info.height, kMppAlignment);
        const size_t frame_bytes = static_cast<size_t>(hor_stride) * ver_stride * kDecodeFrameFactor;

        MppBufferGroupGuard input_group_guard;
        auto ret = mpp_buffer_group_get_internal(&input_group_guard.group, MPP_BUFFER_TYPE_ION);
        if (ret != MPP_OK || !input_group_guard.group) {
            LogDecoderFallback("ION input group allocation failed", ret);
            return nullptr;
        }
        MppBufferGuard input_buffer_guard;
        ret = mpp_buffer_get(input_group_guard.group, &input_buffer_guard.buffer, jpeg.size());
        if (ret != MPP_OK || !input_buffer_guard.buffer) {
            LogDecoderFallback("ION input buffer allocation failed", ret);
            return nullptr;
        }
        auto* input_address = static_cast<uint8_t*>(mpp_buffer_get_ptr(input_buffer_guard.buffer));
        if (!input_address) {
            return nullptr;
        }
        mpp_buffer_sync_begin(input_buffer_guard.buffer);
        std::memcpy(input_address, jpeg.data(), jpeg.size());
        mpp_buffer_sync_end(input_buffer_guard.buffer);

        MppBufferGroupGuard frame_group_guard;
        ret = mpp_buffer_group_get_internal(
            &frame_group_guard.group,
            static_cast<MppBufferType>(MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE));
        if (ret != MPP_OK || !frame_group_guard.group) {
            LogDecoderFallback("output frame group allocation failed", ret);
            return nullptr;
        }
        ret = mpp_buffer_group_limit_config(frame_group_guard.group, frame_bytes, kDecodeBufferCount);
        if (ret != MPP_OK) {
            LogDecoderFallback("output frame group limit failed", ret);
            return nullptr;
        }
        MppBufferGuard output_buffer_guard;
        ret = mpp_buffer_get(frame_group_guard.group, &output_buffer_guard.buffer, frame_bytes);
        if (ret != MPP_OK || !output_buffer_guard.buffer) {
            LogDecoderFallback("output frame buffer allocation failed", ret);
            return nullptr;
        }

        MppFrameGuard frame_guard;
        ret = mpp_frame_init(&frame_guard.frame);
        if (ret != MPP_OK || !frame_guard.frame) {
            LogDecoderFallback("output frame init failed", ret);
            return nullptr;
        }
        mpp_frame_set_buffer(frame_guard.frame, output_buffer_guard.buffer);

        MppPacketGuard packet_guard;
        ret = mpp_packet_init_with_buffer(&packet_guard.packet, input_buffer_guard.buffer);
        if (ret != MPP_OK || !packet_guard.packet) {
            LogDecoderFallback("input packet init failed", ret);
            return nullptr;
        }
        mpp_packet_set_pos(packet_guard.packet, input_address);
        mpp_packet_set_length(packet_guard.packet, jpeg.size());
        mpp_packet_set_eos(packet_guard.packet);

        // The decisive handshake: hand the decoder the frame it must fill.
        auto meta = mpp_packet_get_meta(packet_guard.packet);
        if (!meta) {
            LogDecoderFallback("input packet has no metadata", 0);
            return nullptr;
        }
        ret = mpp_meta_set_frame(meta, KEY_OUTPUT_FRAME, frame_guard.frame);
        if (ret != MPP_OK) {
            LogDecoderFallback("KEY_OUTPUT_FRAME metadata rejected", ret);
            return nullptr;
        }

        ret = state.decoder_api->decode_put_packet(state.decoder, packet_guard.packet);
        if (ret != MPP_OK) {
            LogDecoderFallback("decode_put_packet failed", ret);
            return nullptr;
        }

        MppFrameGuard decoded_guard;
        bool produced = false;
        for (int attempt = 0; attempt < kDecodeGetFrameAttempts && !produced; ++attempt) {
            MppFrame decoded = nullptr;
            const auto get   = state.decoder_api->decode_get_frame(state.decoder, &decoded);
            if (get != MPP_OK || !decoded) {
                continue;
            }
            if (mpp_frame_get_info_change(decoded)) {
                state.decoder_api->control(state.decoder, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
                mpp_frame_deinit(&decoded);
                continue;
            }
            decoded_guard.frame = decoded;
            produced            = true;
        }
        if (!produced) {
            return nullptr;
        }
        if (mpp_frame_get_errinfo(decoded_guard.frame) != 0 ||
            mpp_frame_get_discard(decoded_guard.frame) != 0) {
            LOG_WARN("MPP JPEG decoder rejected the image: err=0x{:x} discard={}",
                     mpp_frame_get_errinfo(decoded_guard.frame), mpp_frame_get_discard(decoded_guard.frame));
            return nullptr;
        }
        const auto format = static_cast<RK_U32>(mpp_frame_get_fmt(decoded_guard.frame)) & MPP_FRAME_FMT_MASK;
        if (format != MPP_FMT_YUV420SP) {
            LOG_WARN("MPP JPEG decoder ignored the NV12 request: format=0x{:x}", format);
            return nullptr;
        }
        return ConvertDecodedJpegToBgr(decoded_guard.frame);
    }

    // ── encoder ─────────────────────────────────────────────────────────────

    void ReleaseJpegEncoder(RockchipJpegCodecState& state) {
        if (state.encoder_packet) {
            mpp_buffer_put(state.encoder_packet);
            state.encoder_packet = nullptr;
        }
        if (state.encoder_frame) {
            mpp_buffer_put(state.encoder_frame);
            state.encoder_frame = nullptr;
        }
        if (state.encoder_group) {
            mpp_buffer_group_put(state.encoder_group);
            state.encoder_group = nullptr;
        }
        state.encoder_width        = 0;
        state.encoder_height       = 0;
        state.encoder_hor_stride   = 0;
        state.encoder_ver_stride   = 0;
        state.encoder_frame_bytes  = 0;
        state.encoder_input_format = MPP_FMT_YUV420SP;
    }

    bool ConfigureJpegEncoder(RockchipJpegCodecState& state, int width, int height, int quality,
                              MppFrameFormat input_format) {
        if (state.encoder_width == width && state.encoder_height == height && state.encoder_frame &&
            state.encoder_input_format == input_format) {
            // Only the quality knob changed; MPP re-reads the whole cfg.
            auto ret = mpp_enc_cfg_set_s32(state.encoder_cfg, "jpeg:q_factor", quality);
            if (ret != MPP_OK ||
                state.encoder_api->control(state.encoder, MPP_ENC_SET_CFG, state.encoder_cfg) != MPP_OK) {
                LogEncoderFallback("quality update rejected", ret);
                return false;
            }
            return true;
        }

        if (!state.encoder_opened) {
            if (access("/dev/mpp_service", R_OK | W_OK) != 0) {
                LogEncoderFallback("/dev/mpp_service is not accessible", 0);
                return false;
            }
            if (mpp_check_support_format(MPP_CTX_ENC, MPP_VIDEO_CodingMJPEG) != MPP_OK) {
                LogEncoderFallback("MJPEG encode is not registered", 0);
                return false;
            }
            auto ret = mpp_create(&state.encoder, &state.encoder_api);
            if (ret != MPP_OK || !state.encoder || !state.encoder_api ||
                mpp_init(state.encoder, MPP_CTX_ENC, MPP_VIDEO_CodingMJPEG) != MPP_OK) {
                LogEncoderFallback("MPP JPEG encoder could not be created", ret);
                return false;
            }
            MppPollType timeout = MPP_POLL_BLOCK;
            state.encoder_api->control(state.encoder, MPP_SET_OUTPUT_TIMEOUT, &timeout);
            ret = mpp_enc_cfg_init(&state.encoder_cfg);
            if (ret != MPP_OK || !state.encoder_cfg) {
                LogEncoderFallback("MPP encoder config allocation failed", ret);
                return false;
            }
            state.encoder_opened = true;
        }

        ReleaseJpegEncoder(state);

        const int hor_stride     = AlignUp(width, kMppAlignment);
        const int ver_stride     = AlignUp(height, kMppAlignment);
        const size_t frame_bytes = static_cast<size_t>(hor_stride) * ver_stride * 3 / 2;

        auto ret = mpp_buffer_group_get_internal(
            &state.encoder_group,
            static_cast<MppBufferType>(MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE));
        if (ret != MPP_OK || !state.encoder_group) {
            LogEncoderFallback("encoder buffer group allocation failed", ret);
            return false;
        }
        ret = mpp_buffer_get(state.encoder_group, &state.encoder_frame, frame_bytes);
        if (ret != MPP_OK || !state.encoder_frame) {
            LogEncoderFallback("encoder frame buffer allocation failed", ret);
            return false;
        }
        ret = mpp_buffer_get(state.encoder_group, &state.encoder_packet, frame_bytes);
        if (ret != MPP_OK || !state.encoder_packet) {
            LogEncoderFallback("encoder packet buffer allocation failed", ret);
            return false;
        }

        ret = state.encoder_api->control(state.encoder, MPP_ENC_GET_CFG, state.encoder_cfg);
        if (ret != MPP_OK) {
            LogEncoderFallback("MPP_ENC_GET_CFG failed", ret);
            return false;
        }
        bool config_ok     = true;
        const auto set_s32 = [&](const char* name, RK_S32 value) {
            const auto result = mpp_enc_cfg_set_s32(state.encoder_cfg, name, value);
            if (result != MPP_OK) {
                LOG_WARN("MPP JPEG encoder config {}={} rejected: {}", name, value, result);
                config_ok = false;
            }
        };
        set_s32("prep:width", width);
        set_s32("prep:height", height);
        set_s32("prep:hor_stride", hor_stride);
        set_s32("prep:ver_stride", ver_stride);
        set_s32("prep:format", static_cast<RK_S32>(input_format));
        set_s32("codec:type", MPP_VIDEO_CodingMJPEG);
        set_s32("jpeg:q_mode", kJpegQpModeQFactor);
        set_s32("jpeg:q_factor", quality);
        if (!config_ok) {
            return false;
        }
        ret = state.encoder_api->control(state.encoder, MPP_ENC_SET_CFG, state.encoder_cfg);
        if (ret != MPP_OK) {
            LogEncoderFallback("MPP_ENC_SET_CFG failed", ret);
            return false;
        }

        state.encoder_width        = width;
        state.encoder_height       = height;
        state.encoder_hor_stride   = hor_stride;
        state.encoder_ver_stride   = ver_stride;
        state.encoder_frame_bytes  = frame_bytes;
        state.encoder_input_format = input_format;
        return true;
    }

    /// RGA converts the packed frame directly into the MPP encoder's DMA-BUF.
    ///
    /// NV12 (semi-planar) is chosen deliberately: rga3 serves it, whereas
    /// planar YUV420 would fall through to RGA2 and fail on a >4G host source.
    /// Routing it through the DMA-BUF also means no CPU ever touches a pixel.
    ///
    /// Both operands must be wrapped the same way. librga refuses a request
    /// that mixes an imported handle with a raw buffer:
    ///   "librga only supports the use of handles only or no handles,
    ///    [src,dst] = [N, 0]" -> "failed to generate task req!" -> status 0.
    /// The destination must stay an fd (it is the encoder's DMA-BUF), so the
    /// source is wrapped as a plain virtual address instead of going through
    /// ScopedRgaBufferHandle. Adding that handle is precisely what made the
    /// engine fail here while the byte-identical probe8 step passed
    /// (mpp_jpeg_enc_probe9 variants A vs B/C).
    ///
    /// This deliberately does NOT go through ConvertWithRga: that path latches
    /// rga_unavailable_ on failure, which would switch off RGA for the whole
    /// frame processor because of one encode-side hiccup.
    bool ConvertPackedIntoSemiPlanar(RockchipJpegCodecState& state, VideoFrame& source, int rga_format) {
        const int width  = static_cast<int>(source.GetWidth());
        const int height = static_cast<int>(source.GetHeight());

        const auto started = std::chrono::steady_clock::now();
        IM_STATUS status   = IM_STATUS_FAILED;
        if (source.GetData() != nullptr && source.GetSize() > 0) {
            // wrapbuffer_fd takes (fd, width, height, format, wstride, hstride).
            auto target =
                wrapbuffer_fd(mpp_buffer_get_fd(state.encoder_frame), width, height, RK_FORMAT_YCbCr_420_SP,
                              state.encoder_hor_stride, state.encoder_ver_stride);
            // The *_t form is the real exported function. The bare names are
            // variadic macros that expand `int __args[] = {__VA_ARGS__}`, which is
            // a zero-size array -- and therefore a hard error -- in C++ whenever
            // no optional stride arguments are passed. This branch of the macro
            // simply forwards (wstride, hstride) = (width, height), so calling
            // the function directly is equivalent and compiles.
            auto origin =
                wrapbuffer_virtualaddr_t(source.GetData(), width, height, width, height, rga_format);
            mpp_buffer_sync_begin(state.encoder_frame);
            status =
                imcvtcolor_t(origin, target, rga_format, RK_FORMAT_YCbCr_420_SP, IM_RGB_TO_YUV_BT601_FULL, 1);
            mpp_buffer_sync_end(state.encoder_frame);
        }
        GetPreviewPipelineMetrics().RecordRgaOperation(RockchipRgaSucceeded(status),
                                                       ElapsedNanoseconds(started));
        if (!RockchipRgaSucceeded(status)) {
            LOG_WARN("RGA packed -> NV12 into the encoder buffer failed: {} ({})", status,
                     imStrError_t(status));
            return false;
        }
        return true;
    }

    /// Stages a caller-supplied planar I420 frame into the encoder buffer.
    void CopyI420IntoMppFrame(RockchipJpegCodecState& state, VideoFrame& source) {
        const int width      = static_cast<int>(source.GetWidth());
        const int height     = static_cast<int>(source.GetHeight());
        const int hor_stride = state.encoder_hor_stride;
        const int ver_stride = state.encoder_ver_stride;

        auto* destination = static_cast<uint8_t*>(mpp_buffer_get_ptr(state.encoder_frame));
        const auto* y_src = source.GetData();
        const auto* u_src = y_src + static_cast<size_t>(width) * height;
        const auto* v_src = u_src + static_cast<size_t>(width / 2) * (height / 2);

        mpp_buffer_sync_begin(state.encoder_frame);
        for (int row = 0; row < height; ++row) {
            std::memcpy(destination + static_cast<size_t>(row) * hor_stride,
                        y_src + static_cast<size_t>(row) * width, static_cast<size_t>(width));
        }
        auto* u_dst                = destination + static_cast<size_t>(hor_stride) * ver_stride;
        auto* v_dst                = u_dst + static_cast<size_t>(hor_stride / 2) * (ver_stride / 2);
        const size_t chroma_stride = static_cast<size_t>(hor_stride) / 2;
        const size_t chroma_width  = static_cast<size_t>(width) / 2;
        for (int row = 0; row < height / 2; ++row) {
            std::memcpy(u_dst + static_cast<size_t>(row) * chroma_stride,
                        u_src + static_cast<size_t>(row) * chroma_width, chroma_width);
            std::memcpy(v_dst + static_cast<size_t>(row) * chroma_stride,
                        v_src + static_cast<size_t>(row) * chroma_width, chroma_width);
        }
        mpp_buffer_sync_end(state.encoder_frame);
    }

    /// Prepares the encoder input frame: RGA writes the semi-planar conversion
    /// of a packed source straight into the encoder's DMA-BUF, or a caller that
    /// already supplies planar I420 is staged by CPU copy.
    bool PrepareEncoderInput(RockchipJpegCodecState& state, VideoFrame& source, bool packed_source) {
        if (packed_source) {
            const int rga_format =
                source.GetPixelFormat() == PixelFormat::PIXEL_RGB8 ? RK_FORMAT_RGB_888 : RK_FORMAT_BGR_888;
            return ConvertPackedIntoSemiPlanar(state, source, rga_format);
        }
        CopyI420IntoMppFrame(state, source);
        return true;
    }

    std::vector<uint8_t> EncodeWithMpp(RockchipJpegCodecState& state, VideoFrame& source,
                                       bool packed_source) {
        if (!PrepareEncoderInput(state, source, packed_source)) {
            return {};
        }

        MppFrameGuard frame_guard;
        auto ret = mpp_frame_init(&frame_guard.frame);
        if (ret != MPP_OK || !frame_guard.frame) {
            return {};
        }
        mpp_frame_set_width(frame_guard.frame, state.encoder_width);
        mpp_frame_set_height(frame_guard.frame, state.encoder_height);
        mpp_frame_set_hor_stride(frame_guard.frame, state.encoder_hor_stride);
        mpp_frame_set_ver_stride(frame_guard.frame, state.encoder_ver_stride);
        mpp_frame_set_fmt(frame_guard.frame, state.encoder_input_format);
        mpp_frame_set_buffer(frame_guard.frame, state.encoder_frame);

        MppPacketGuard packet_guard;
        ret = mpp_packet_init_with_buffer(&packet_guard.packet, state.encoder_packet);
        if (ret != MPP_OK || !packet_guard.packet) {
            return {};
        }
        mpp_packet_set_length(packet_guard.packet, 0);
        mpp_meta_set_packet(mpp_frame_get_meta(frame_guard.frame), KEY_OUTPUT_PACKET, packet_guard.packet);

        ret = state.encoder_api->encode_put_frame(state.encoder, frame_guard.frame);
        if (ret != MPP_OK) {
            LOG_WARN("MPP encode_put_frame failed: {}", ret);
            return {};
        }
        MppPacket output = nullptr;
        ret              = state.encoder_api->encode_get_packet(state.encoder, &output);
        if (ret != MPP_OK || !output) {
            LOG_WARN("MPP encode_get_packet failed: {}", ret);
            return {};
        }
        // jpege hands back the very packet we staged through KEY_OUTPUT_PACKET.
        // Deinitialising it twice would double-put the buffer and its meta
        // ("put_meta invalid negative ref_count" / "kmpp_obj_put ... double-put"),
        // so in that case only the guard below releases it.
        MppPacketGuard output_guard{output};
        if (output_guard.packet == packet_guard.packet) {
            output_guard.packet = nullptr;
        }

        const auto* bytes = static_cast<const uint8_t*>(mpp_packet_get_pos(output));
        const auto length = mpp_packet_get_length(output);
        if (!bytes || length <= 0) {
            return {};
        }
        return std::vector<uint8_t>(bytes, bytes + length);
    }

}  // namespace

VideoFrameProcRockchip::~VideoFrameProcRockchip() = default;

VideoFramePtr VideoFrameProcRockchip::DecodeJpeg(const std::vector<u_int8_t>& data) {
    if (IsJpegPayload(data)) {
        EncodedImageInfo info;
        if (!InspectEncodedImage(data, info) || !IsEncodedImageWithinFrameCapability(info)) {
            // Unsupported dimensions are the shared capability boundary, not a
            // hardware problem; let the shared path report them once.
            return VideoFrameProcCpu::DecodeJpeg(data);
        }

        std::lock_guard<std::mutex> lock(jpeg_codec_mtx_);
        if (!jpeg_codec_) {
            jpeg_codec_ = std::make_unique<RockchipJpegCodecState>();
        }
        auto& state = *jpeg_codec_;
        if (!state.decoder_unusable) {
            if (!state.decoder_opened) {
                state.decoder_unusable = !OpenJpegDecoder(state);
            }
            if (state.decoder_opened) {
                auto frame = DecodeJpegHardware(state, data, info);
                if (frame) {
                    LogFirstHardwareJpeg("decode", static_cast<int>(frame->GetWidth()),
                                         static_cast<int>(frame->GetHeight()));
                    return frame;
                }
                // A failed image may leave a partially consumed frame queued.
                // Rebuild the context so the next call starts from a clean one
                // instead of returning this image's leftovers.
                DiscardJpegDecoder(state);
            }
        }
    }
    return VideoFrameProcCpu::DecodeJpeg(data);
}

std::vector<u_char> VideoFrameProcRockchip::EncodeJpeg(const VideoFramePtr source) {
    if (VideoFrameValid(source) && source->GetWidth() > 0 && source->GetHeight() > 0) {
        const int width   = static_cast<int>(source->GetWidth());
        const int height  = static_cast<int>(source->GetHeight());
        const auto format = source->GetPixelFormat();
        const bool packed = format == PixelFormat::PIXEL_BGR8 || format == PixelFormat::PIXEL_RGB8;
        const bool planar = format == PixelFormat::PIXEL_I420;
        if ((packed || planar) && width % 2 == 0 && height % 2 == 0 &&
            width <= std::numeric_limits<int>::max() - kMppAlignment &&
            height <= std::numeric_limits<int>::max() - kMppAlignment &&
            (!planar || IsCompactI420(*source))) {
            std::lock_guard<std::mutex> lock(jpeg_codec_mtx_);
            if (!jpeg_codec_) {
                jpeg_codec_ = std::make_unique<RockchipJpegCodecState>();
            }
            auto& state = *jpeg_codec_;
            if (!state.encoder_unusable) {
                // Packed sources are handed to RGA as NV12 straight into the
                // encoder's DMA-BUF with JPEG's full-range colourimetry; planar
                // sources are staged as-is. Either way MPP does the compressing.
                const MppFrameFormat input_format = packed ? MPP_FMT_YUV420SP : MPP_FMT_YUV420P;
                const int quality = std::clamp(kJpegQuality, kJpegQualityMin, kJpegQualityMax);
                if (ConfigureJpegEncoder(state, width, height, quality, input_format)) {
                    auto jpeg = EncodeWithMpp(state, *source, packed);
                    if (!jpeg.empty()) {
                        LogFirstHardwareJpeg("encode", width, height);
                        return std::vector<u_char>(jpeg.begin(), jpeg.end());
                    }
                    LogEncoderFallback("encode_put_frame/encode_get_packet failed", 0);
                }
                // Latch: a failing encoder would otherwise cost the same
                // rejected work on every uploaded picture.
                state.encoder_unusable = true;
            }
        }
    }
    return VideoFrameProcCpu::EncodeJpeg(source);
}

}  // namespace cosmo::media
