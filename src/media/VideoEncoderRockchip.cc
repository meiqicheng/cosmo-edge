#include "media/VideoEncoderRockchip.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#define MODULE_TAG "cosmo_mpp_encoder"
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_venc_cfg.h>
#include <rockchip/rk_venc_rc.h>

#include "media/PreviewPipelineMetrics.h"
#include "media/RockchipRgaBuffer.h"
#include "media/VideoEncoderCpu.h"
#include "util/Log.h"

namespace cosmo::media {
namespace {

    constexpr size_t kMppAlignment = 16;
    constexpr RK_S32 kFrameRate    = 25;
    constexpr RK_S32 kGopLength    = 10;
    constexpr RK_S32 kBitRate      = 4'000'000;

    MppCodingType ToMppCoding(VideoCodecType type) {
        return type == VideoCodecType::kH264 ? MPP_VIDEO_CodingAVC : MPP_VIDEO_CodingUnused;
    }

    size_t AlignUp(size_t value, size_t alignment) {
        return (value + alignment - 1) / alignment * alignment;
    }

    bool ContainsH264ParameterSet(const uint8_t* data, size_t size) {
        if (!data || size < 5) {
            return false;
        }
        bool has_sps = false;
        bool has_pps = false;
        for (size_t pos = 0; pos + 4 < size; ++pos) {
            size_t prefix = 0;
            if (data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) {
                prefix = 3;
            } else if (pos + 4 < size && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 0 &&
                       data[pos + 3] == 1) {
                prefix = 4;
            }
            if (prefix != 0 && pos + prefix < size) {
                const auto type = static_cast<uint8_t>(data[pos + prefix] & 0x1f);
                has_sps         = has_sps || type == 7;
                has_pps         = has_pps || type == 8;
            }
        }
        return has_sps && has_pps;
    }

    bool ContainsH264Idr(const uint8_t* data, size_t size) {
        if (!data || size < 5) {
            return false;
        }
        for (size_t pos = 0; pos + 4 < size; ++pos) {
            size_t prefix = 0;
            if (data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) {
                prefix = 3;
            } else if (pos + 4 < size && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 0 &&
                       data[pos + 3] == 1) {
                prefix = 4;
            }
            if (prefix != 0 && pos + prefix < size && (data[pos + prefix] & 0x1f) == 5) {
                return true;
            }
        }
        return false;
    }

    uint64_t ElapsedNanoseconds(std::chrono::steady_clock::time_point started) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                .count());
    }

}  // namespace

struct RockchipEncoderState {
    MppCtx context{nullptr};
    MppApi* api{nullptr};
    MppEncCfg config{nullptr};
    MppBufferGroup buffer_group{nullptr};
    MppBuffer frame_buffer{nullptr};
    MppBuffer packet_buffer{nullptr};
    MppCodingType coding{MPP_VIDEO_CodingUnused};
    size_t horizontal_stride{0};
    size_t vertical_stride{0};
    size_t frame_buffer_size{0};
    int64_t frame_pts{0};
    ScopedRgaBufferHandle frame_rga_handle;
    bool rga_copy_in_available{false};
    std::vector<uint8_t> codec_header;
};

VideoEncoderRockchip::VideoEncoderRockchip() : VideoEncoder() {}

VideoEncoderRockchip::~VideoEncoderRockchip() {
    Clean();
}

VideoEncoderCapability VideoEncoderRockchip::Probe(VideoCodecType type) {
    VideoEncoderCapability capability;
    capability.backend        = "rockchip-mpp-rga";
    capability.implementation = "rockchip-mpp";

    const auto coding = ToMppCoding(type);
    if (coding == MPP_VIDEO_CodingUnused) {
        const auto cpu            = VideoEncoderCpu::Probe(type);
        capability.available      = cpu.available;
        capability.backend        = cpu.available ? "ffmpeg-software-fallback" : capability.backend;
        capability.implementation = cpu.available ? cpu.implementation : std::string{};
        capability.detail =
            cpu.available ? "Rockchip MPP qualification is H264-only; approved FFmpeg fallback is available"
                          : "Rockchip MPP qualification is H264-only; approved FFmpeg fallback unavailable";
        return capability;
    }

    const bool device_accessible = access("/dev/mpp_service", R_OK | W_OK) == 0;
    const bool format_supported  = mpp_check_support_format(MPP_CTX_ENC, coding) == MPP_OK;
    if (device_accessible && format_supported) {
        capability.available = true;
        capability.detail =
            "MPP encoder device and codec are available; RGA writes compact I420 into the "
            "stride-aligned MPP DMA-BUF with an observable CPU fallback";
        return capability;
    }

    const auto cpu = VideoEncoderCpu::Probe(type);
    if (cpu.available) {
        capability.available      = true;
        capability.backend        = "ffmpeg-software-fallback";
        capability.implementation = cpu.implementation;
        capability.detail         = "MPP unavailable; deterministic approved FFmpeg fallback is available";
        return capability;
    }

    capability.detail = std::string("MPP unavailable (") +
                        (device_accessible ? "codec unsupported" : "device inaccessible") +
                        "); approved FFmpeg fallback unavailable";
    return capability;
}

bool VideoEncoderRockchip::Open() {
    Clean();
    if (OpenMpp()) {
        return true;
    }

    const auto cpu_capability = VideoEncoderCpu::Probe(codec_type_);
    if (!cpu_capability.available) {
        LOG_WARN("Rockchip encoder open failed and deterministic CPU fallback is unavailable: {}",
                 cpu_capability.detail);
        return false;
    }

    fallback_ = std::make_unique<VideoEncoderCpu>();
    fallback_->Set(codec_type_, width_, height_);
    if (!fallback_->Open()) {
        fallback_.reset();
        return false;
    }
    LOG_WARN("Rockchip MPP unavailable; using approved CPU encoder {}", cpu_capability.implementation);
    return true;
}

bool VideoEncoderRockchip::OpenMpp() {
    const auto coding = ToMppCoding(codec_type_);
    if (coding == MPP_VIDEO_CodingUnused || width_ == 0 || height_ == 0 || width_ % 2 != 0 ||
        height_ % 2 != 0 || width_ > static_cast<size_t>(std::numeric_limits<RK_S32>::max()) ||
        height_ > static_cast<size_t>(std::numeric_limits<RK_S32>::max()) - kMppAlignment) {
        LOG_WARN("MPP encoder invalid codec or dimensions: codec={} size={}x{}",
                 static_cast<int>(codec_type_), width_, height_);
        return false;
    }

    const auto capability = Probe(codec_type_);
    if (!capability.available || capability.implementation != "rockchip-mpp") {
        LOG_WARN("MPP encoder admission failed: {}", capability.detail);
        return false;
    }

    state_                    = std::make_unique<RockchipEncoderState>();
    state_->coding            = coding;
    state_->horizontal_stride = AlignUp(width_, kMppAlignment);
    state_->vertical_stride   = AlignUp(height_, kMppAlignment);
    if (state_->horizontal_stride > std::numeric_limits<size_t>::max() / state_->vertical_stride) {
        CleanMpp();
        return false;
    }
    const size_t y_plane = state_->horizontal_stride * state_->vertical_stride;
    if (y_plane > std::numeric_limits<size_t>::max() - y_plane / 2) {
        CleanMpp();
        return false;
    }
    state_->frame_buffer_size = y_plane * 3 / 2;

    auto ret = mpp_buffer_group_get_internal(
        &state_->buffer_group, static_cast<MppBufferType>(MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE));
    if (ret != MPP_OK) {
        LOG_WARN("MPP buffer group allocation failed: {}", ret);
        CleanMpp();
        return false;
    }
    ret = mpp_buffer_get(state_->buffer_group, &state_->frame_buffer, state_->frame_buffer_size);
    if (ret != MPP_OK) {
        LOG_WARN("MPP frame buffer allocation failed: {}", ret);
        CleanMpp();
        return false;
    }
    auto* frame_buffer_address = static_cast<uint8_t*>(mpp_buffer_get_ptr(state_->frame_buffer));
    if (!frame_buffer_address) {
        LOG_WARN("{}", "MPP frame buffer is not CPU-addressable for fallback initialization");
        CleanMpp();
        return false;
    }
    mpp_buffer_sync_begin(state_->frame_buffer);
    const size_t y_plane_size = state_->horizontal_stride * state_->vertical_stride;
    std::memset(frame_buffer_address, 0, y_plane_size);
    std::memset(frame_buffer_address + y_plane_size, 128, state_->frame_buffer_size - y_plane_size);
    mpp_buffer_sync_end(state_->frame_buffer);
    state_->rga_copy_in_available =
        state_->frame_rga_handle.ImportFd(mpp_buffer_get_fd(state_->frame_buffer), state_->frame_buffer_size);
    if (!state_->rga_copy_in_available) {
        LOG_WARN("{}", "RGA could not import the MPP encoder DMA-BUF; CPU copy-in remains available");
    }
    ret = mpp_buffer_get(state_->buffer_group, &state_->packet_buffer, state_->frame_buffer_size);
    if (ret != MPP_OK) {
        LOG_WARN("MPP packet buffer allocation failed: {}", ret);
        CleanMpp();
        return false;
    }

    ret = mpp_create(&state_->context, &state_->api);
    if (ret != MPP_OK || !state_->context || !state_->api) {
        LOG_WARN("MPP context creation failed: {}", ret);
        CleanMpp();
        return false;
    }
    MppPollType timeout = MPP_POLL_BLOCK;
    ret                 = state_->api->control(state_->context, MPP_SET_OUTPUT_TIMEOUT, &timeout);
    if (ret != MPP_OK) {
        LOG_WARN("MPP output timeout configuration failed: {}", ret);
        CleanMpp();
        return false;
    }
    ret = mpp_init(state_->context, MPP_CTX_ENC, coding);
    if (ret != MPP_OK) {
        LOG_WARN("MPP encoder initialization failed: {}", ret);
        CleanMpp();
        return false;
    }
    ret = mpp_enc_cfg_init(&state_->config);
    if (ret != MPP_OK || !state_->config) {
        LOG_WARN("MPP encoder config allocation failed: {}", ret);
        CleanMpp();
        return false;
    }
    ret = state_->api->control(state_->context, MPP_ENC_GET_CFG, state_->config);
    if (ret != MPP_OK) {
        LOG_WARN("MPP encoder config query failed: {}", ret);
        CleanMpp();
        return false;
    }

    bool config_ok     = true;
    const auto set_s32 = [&](const char* name, RK_S32 value) {
        const auto result = mpp_enc_cfg_set_s32(state_->config, name, value);
        if (result != MPP_OK) {
            LOG_WARN("MPP encoder config {}={} failed: {}", name, value, result);
            config_ok = false;
        }
    };
    set_s32("prep:width", static_cast<RK_S32>(width_));
    set_s32("prep:height", static_cast<RK_S32>(height_));
    set_s32("prep:hor_stride", static_cast<RK_S32>(state_->horizontal_stride));
    set_s32("prep:ver_stride", static_cast<RK_S32>(state_->vertical_stride));
    set_s32("prep:format", MPP_FMT_YUV420P);
    set_s32("rc:mode", MPP_ENC_RC_MODE_CBR);
    set_s32("rc:fps_in_flex", 0);
    set_s32("rc:fps_in_num", kFrameRate);
    set_s32("rc:fps_in_denom", 1);
    set_s32("rc:fps_out_flex", 0);
    set_s32("rc:fps_out_num", kFrameRate);
    set_s32("rc:fps_out_denom", 1);
    set_s32("rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);
    set_s32("rc:bps_target", kBitRate);
    set_s32("rc:bps_max", kBitRate * 17 / 16);
    set_s32("rc:bps_min", kBitRate * 15 / 16);
    set_s32("rc:qp_init", -1);
    set_s32("rc:qp_max", 51);
    set_s32("rc:qp_min", 10);
    set_s32("rc:qp_max_i", 51);
    set_s32("rc:qp_min_i", 10);
    set_s32("rc:qp_ip", 2);
    set_s32("rc:gop", kGopLength);
    set_s32("codec:type", coding);
    if (coding == MPP_VIDEO_CodingAVC) {
        set_s32("h264:profile", 100);
        set_s32("h264:level", 40);
        set_s32("h264:cabac_en", 1);
        set_s32("h264:cabac_idc", 0);
        set_s32("h264:trans8x8", 1);
    }
    if (!config_ok) {
        CleanMpp();
        return false;
    }
    ret = state_->api->control(state_->context, MPP_ENC_SET_CFG, state_->config);
    if (ret != MPP_OK) {
        LOG_WARN("MPP encoder config commit failed: {}", ret);
        CleanMpp();
        return false;
    }

    MppPacket header = nullptr;
    ret              = mpp_packet_init_with_buffer(&header, state_->packet_buffer);
    if (ret == MPP_OK && header) {
        mpp_packet_set_length(header, 0);
        ret = state_->api->control(state_->context, MPP_ENC_GET_HDR_SYNC, header);
        if (ret == MPP_OK) {
            const auto* bytes = static_cast<const uint8_t*>(mpp_packet_get_pos(header));
            const auto length = mpp_packet_get_length(header);
            if (bytes && length > 0) {
                state_->codec_header.assign(bytes, bytes + length);
            }
        }
        mpp_packet_deinit(&header);
    }
    if (ret != MPP_OK || state_->codec_header.empty()) {
        LOG_WARN("MPP encoder parameter-set query failed: {}", ret);
        CleanMpp();
        return false;
    }

    LOG_INFO("MPP encoder opened: codec={} size={}x{} stride={}x{} header={} bytes",
             static_cast<int>(codec_type_), width_, height_, state_->horizontal_stride,
             state_->vertical_stride, state_->codec_header.size());
    return true;
}

VideoPacketPtr VideoEncoderRockchip::SendYUVFrame(void* data) {
    if (fallback_) {
        return fallback_->SendYUVFrame(data);
    }
    const auto started = std::chrono::steady_clock::now();
    const auto fail    = [&]() -> VideoPacketPtr {
        GetPreviewPipelineMetrics().RecordMppEncode(false, ElapsedNanoseconds(started));
        return nullptr;
    };
    if (!state_ || !state_->context || !state_->api || !state_->frame_buffer || !data) {
        return fail();
    }

    auto* destination = static_cast<uint8_t*>(mpp_buffer_get_ptr(state_->frame_buffer));
    if (!destination) {
        return fail();
    }
    const auto* source           = static_cast<const uint8_t*>(data);
    const size_t compact_y_size  = width_ * height_;
    const size_t compact_uv_size = compact_y_size / 4;
    const size_t mpp_y_size      = state_->horizontal_stride * state_->vertical_stride;
    const size_t mpp_uv_stride   = state_->horizontal_stride / 2;
    const size_t mpp_uv_height   = state_->vertical_stride / 2;

    bool rga_copied = false;
    if (state_->rga_copy_in_available) {
        const auto rga_started = std::chrono::steady_clock::now();
        ScopedRgaBufferHandle source_handle(const_cast<uint8_t*>(source),
                                            compact_y_size + compact_uv_size * 2);
        IM_STATUS status = IM_STATUS_OUT_OF_MEMORY;
        if (source_handle && width_ % 16 == 0) {
            auto source_image = wrapbuffer_handle_t(source_handle.Get(), static_cast<int>(width_),
                                                    static_cast<int>(height_), static_cast<int>(width_),
                                                    static_cast<int>(height_), RK_FORMAT_YCbCr_420_P);
            auto target_image =
                wrapbuffer_handle_t(state_->frame_rga_handle.Get(), static_cast<int>(width_),
                                    static_cast<int>(height_), static_cast<int>(state_->horizontal_stride),
                                    static_cast<int>(state_->vertical_stride), RK_FORMAT_YCbCr_420_P);
            const im_rect source_rect{0, 0, static_cast<int>(width_), static_cast<int>(height_)};
            const im_rect target_rect = source_rect;
            const im_rect empty_rect{};
            const rga_buffer_t empty_buffer{};
            status = improcess(source_image, target_image, empty_buffer, source_rect, target_rect, empty_rect,
                               IM_SYNC);
        }
        rga_copied = RockchipRgaSucceeded(status);
        GetPreviewPipelineMetrics().RecordRgaOperation(rga_copied, ElapsedNanoseconds(rga_started));
        GetPreviewPipelineMetrics().RecordMppRgaCopyIn(rga_copied);
        if (!rga_copied) {
            LOG_WARN("MPP encoder RGA copy-in failed with status {} ({}); disabling it for this encoder",
                     status, imStrError_t(status));
            state_->rga_copy_in_available = false;
        }
    }

    if (!rga_copied) {
        GetPreviewPipelineMetrics().RecordMppCpuCopyInFallback();
        mpp_buffer_sync_begin(state_->frame_buffer);
        std::memset(destination, 0, mpp_y_size);
        std::memset(destination + mpp_y_size, 128, state_->frame_buffer_size - mpp_y_size);
        for (size_t row = 0; row < height_; ++row) {
            std::memcpy(destination + row * state_->horizontal_stride, source + row * width_, width_);
        }
        auto* destination_u  = destination + mpp_y_size;
        auto* destination_v  = destination_u + mpp_uv_stride * mpp_uv_height;
        const auto* source_u = source + compact_y_size;
        const auto* source_v = source_u + compact_uv_size;
        for (size_t row = 0; row < height_ / 2; ++row) {
            std::memcpy(destination_u + row * mpp_uv_stride, source_u + row * (width_ / 2), width_ / 2);
            std::memcpy(destination_v + row * mpp_uv_stride, source_v + row * (width_ / 2), width_ / 2);
        }
        mpp_buffer_sync_end(state_->frame_buffer);
    }

    MppFrame frame = nullptr;
    auto ret       = mpp_frame_init(&frame);
    if (ret != MPP_OK || !frame) {
        return fail();
    }
    mpp_frame_set_width(frame, static_cast<RK_S32>(width_));
    mpp_frame_set_height(frame, static_cast<RK_S32>(height_));
    mpp_frame_set_hor_stride(frame, static_cast<RK_S32>(state_->horizontal_stride));
    mpp_frame_set_ver_stride(frame, static_cast<RK_S32>(state_->vertical_stride));
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420P);
    mpp_frame_set_pts(frame, state_->frame_pts++);
    mpp_frame_set_buffer(frame, state_->frame_buffer);

    MppPacket packet = nullptr;
    ret              = mpp_packet_init_with_buffer(&packet, state_->packet_buffer);
    if (ret != MPP_OK || !packet) {
        mpp_frame_deinit(&frame);
        return fail();
    }
    mpp_packet_set_length(packet, 0);
    auto meta = mpp_frame_get_meta(frame);
    mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, packet);

    ret = state_->api->encode_put_frame(state_->context, frame);
    mpp_frame_deinit(&frame);
    if (ret != MPP_OK) {
        LOG_WARN("MPP encode_put_frame failed: {}", ret);
        mpp_packet_deinit(&packet);
        return fail();
    }
    ret = state_->api->encode_get_packet(state_->context, &packet);
    if (ret != MPP_OK || !packet) {
        LOG_WARN("MPP encode_get_packet failed: {}", ret);
        if (packet) {
            mpp_packet_deinit(&packet);
        }
        return fail();
    }

    const auto* bytes      = static_cast<const uint8_t*>(mpp_packet_get_pos(packet));
    const auto length      = mpp_packet_get_length(packet);
    RK_S32 output_intra    = 0;
    const auto packet_meta = mpp_packet_has_meta(packet) ? mpp_packet_get_meta(packet) : nullptr;
    if (packet_meta) {
        mpp_meta_get_s32(packet_meta, KEY_OUTPUT_INTRA, &output_intra);
    }
    const bool is_intra =
        output_intra != 0 || (codec_type_ == VideoCodecType::kH264 && ContainsH264Idr(bytes, length));
    if (!bytes || length == 0) {
        mpp_packet_deinit(&packet);
        return fail();
    }

    auto output        = std::make_shared<VideoPacket>();
    output->is_i_frame = is_intra;
    output->pts        = mpp_packet_get_pts(packet);
    if (is_intra && codec_type_ == VideoCodecType::kH264 && !ContainsH264ParameterSet(bytes, length)) {
        output->data.reserve(state_->codec_header.size() + length);
        output->data.insert(output->data.end(), state_->codec_header.begin(), state_->codec_header.end());
    } else {
        output->data.reserve(length);
    }
    output->data.insert(output->data.end(), bytes, bytes + length);
    mpp_packet_deinit(&packet);

    GetPreviewPipelineMetrics().RecordMppEncode(true, ElapsedNanoseconds(started));
    return output;
}

void VideoEncoderRockchip::CleanMpp() {
    if (!state_) {
        return;
    }
    if (state_->context && state_->api) {
        state_->api->reset(state_->context);
    }
    if (state_->context) {
        mpp_destroy(state_->context);
        state_->context = nullptr;
        state_->api     = nullptr;
    }
    if (state_->config) {
        mpp_enc_cfg_deinit(state_->config);
        state_->config = nullptr;
    }
    state_->frame_rga_handle.Reset();
    if (state_->frame_buffer) {
        mpp_buffer_put(state_->frame_buffer);
        state_->frame_buffer = nullptr;
    }
    if (state_->packet_buffer) {
        mpp_buffer_put(state_->packet_buffer);
        state_->packet_buffer = nullptr;
    }
    if (state_->buffer_group) {
        mpp_buffer_group_put(state_->buffer_group);
        state_->buffer_group = nullptr;
    }
    state_.reset();
}

void VideoEncoderRockchip::Clean() {
    fallback_.reset();
    CleanMpp();
}

}  // namespace cosmo::media
