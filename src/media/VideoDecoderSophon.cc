// VideoDecoderSophon — Video Decoder Sophon implementation.

#include "media/VideoDecoderSophon.h"

#include <chrono>
#include <limits>
#include <thread>

#include "media/PixelFormatUtils.h"
#include "util/Log.h"
#include "util/TimingConstants.h"
#include "util/VideoInfo.h"

// libsophon 0.4.x (BM1688/CV186X) reports decoder success as
// BMVidDecRetStatus::BM_ERR_VDEC_SUCCESS; libsophon 0.5.x (BM1684/BM1684X)
// removed that enumerator — BM_SUCCESS (0) is the equivalent success code.
#ifdef COSMO_LIBSOPHON_NEW_VIDEO_API
#define COSMO_VDEC_SUCCESS BM_SUCCESS
#else
#define COSMO_VDEC_SUCCESS BM_ERR_VDEC_SUCCESS
#endif

// BM1688 VPU hardware decoder's create/delete share underlying hardware resources (VPU core, frame buffer
// pool). Concurrent execution of bmvpu_dec_create and bmvpu_dec_delete may corrupt the active decoder's
// handle, returning BM_ERR_VDEC_ILLEGAL_PARAM. This global lock serializes all VPU lifecycle operations.
static std::mutex g_vpuLifecycleMutex;

namespace {
// A newly created VPU channel can transiently report WRONG_RESOLUTION while it is still consuming
// sequence headers. Count only that specific error so successful output does not close the warmup
// window prematurely. AlgChannelDecode recreates the VPU channel for every new stream sequence, so
// Open() naturally starts a fresh bounded budget without tracking packet or frame index rewinds.
constexpr unsigned int kWrongResolutionErrorBudget = 50;
}  // namespace

namespace cosmo {
namespace media {

    struct BmImageDeleter {
        void operator()(bm_image* p) const {
            if (p) {
                if (bm_image_is_attached(*p)) {
                    bm_image_detach(*p);
                }
#ifdef COSMO_LIBSOPHON_NEW_VIDEO_API
                // libsophon 0.5.x (BM1684) takes bm_image by value.
                bm_image_destroy(*p);
#else
                bm_image_destroy(p);
#endif
                delete p;
            }
        }
    };

    using BmImagePtr = std::unique_ptr<bm_image, BmImageDeleter>;

    VideoDecoderSophon::VideoDecoderSophon(size_t name, void* mediaHandle) : VideoDecoder(name) {
        // libsophon 0.4.x (BM1688/CV186X) names the log level ERR; 0.5.x
        // (BM1684/BM1684X) renamed it to ERROR and marks the header deprecated.
        // The bmvpu_dec_* symbols themselves are unchanged across both families.
#ifdef COSMO_LIBSOPHON_NEW_VIDEO_API
        bmvpu_dec_set_logging_threshold(BmVpuDecLogLevel::BMVPU_DEC_LOG_LEVEL_ERROR);
#else
        bmvpu_dec_set_logging_threshold(BmVpuDecLogLevel::BMVPU_DEC_LOG_LEVEL_ERR);
#endif

        bm_handle_ = reinterpret_cast<bm_handle_t>(mediaHandle);
        stop_      = true;
    }

    VideoDecoderSophon::~VideoDecoderSophon() {
        VideoDecoderSophon::Close();
    }

    bool VideoDecoderSophon::IsOpened() {
        return opened_.load() && !stop_.load();
    }

    VideoDecoderCapability VideoDecoderSophon::Probe(VideoCodecType type) {
        VideoDecoderCapability capability;
        capability.backend        = "sophon-vpu";
        capability.implementation = "bmvpu-decoder";
        capability.available      = type == VideoCodecType::kH264 || type == VideoCodecType::kH265;
        capability.detail =
            capability.available ? "Sophon VPU decoder supports the requested codec" : "unsupported codec";
        return capability;
    }

    bool VideoDecoderSophon::Open() {
        std::lock_guard<std::mutex> operation_lock(operation_mutex_);
        if (opened_.load()) {
            if (!stop_.load()) {
                return true;
            }
            LOG_ERRO("{} cannot reopen after an incomplete decoder close", idx_name_);
            return false;
        }
        if (bm_handle_ == nullptr || code_handle_ != nullptr) {
            LOG_ERRO("{} cannot open decoder with an invalid device or lifecycle state", idx_name_);
            return false;
        }

        if (codec_type_ == VideoCodecType::kH265) {
            stream_format_ = BmVpuDecStreamFormat::BMDEC_HEVC;
        } else if (codec_type_ == VideoCodecType::kH264) {
            stream_format_ = BmVpuDecStreamFormat::BMDEC_AVC;
        } else {
            LOG_WARN("{} OpenDecoder But CodecType({}) Not Support", idx_name_, codec_type_);
            return false;
        }

        BMVidDecParam dec_params{};
        dec_params.streamFormat            = stream_format_;
        constexpr size_t kStreamBufferSize = 0x500000;  // 5 MB

        dec_params.extraFrameBufferNum = static_cast<int>(cache_size_);
        dec_params.streamBufferSize    = kStreamBufferSize;
        dec_params.enable_cache        = 1;
        dec_params.bsMode              = BmVpuDecBitStreamMode::BMDEC_BS_MODE_PIC_END;
        dec_params.pcie_board_id       = 0;
        dec_params.cmd_queue_depth     = static_cast<int>(cache_size_);
        dec_params.wtlFormat           = BmVpuDecOutputMapType::BMDEC_OUTPUT_UNMAP;
        dec_params.pixel_format        = BmVpuDecPixFormat::BM_VPU_DEC_PIX_FORMAT_YUV420P;

        auto next_frame            = std::make_unique<BMVidFrame>();
        BMVidCodHandle next_handle = nullptr;
        int ret;
        {
            std::lock_guard<std::mutex> vpuLock(g_vpuLifecycleMutex);
            ret = bmvpu_dec_create(&next_handle, dec_params);
        }
        if (ret != COSMO_VDEC_SUCCESS || next_handle == nullptr) {
            LOG_WARN("{} bmvpu_dec_create failed ret:{}", idx_name_, ret);
            return false;
        }

        code_handle_                       = next_handle;
        frame_                             = std::move(next_frame);
        wrong_resolution_errors_remaining_ = kWrongResolutionErrorBudget;
        stop_.store(false);
        opened_.store(true);
        LOG_INFO("{} VPU decoder opened", idx_name_);
        return true;
    }

    bool VideoDecoderSophon::Close() {
        stop_.store(true);
        std::lock_guard<std::mutex> operation_lock(operation_mutex_);
        if (!opened_.load() && code_handle_ == nullptr) {
            return true;
        }

        {
            std::lock_guard<std::mutex> vpuLock(g_vpuLifecycleMutex);
            if (code_handle_) {
                const auto ret = bmvpu_dec_delete(code_handle_);
                if (ret != COSMO_VDEC_SUCCESS) {
                    LOG_ERRO("{} bmvpu_dec_delete failed: {}", idx_name_, ret);
                    return false;
                }
                code_handle_ = nullptr;
            }
        }

        frame_.reset();
        wrong_resolution_errors_remaining_ = 0;

        opened_.store(false);
        LOG_INFO("{} VPU decoder closed", idx_name_);
        return true;
    }

    bool VideoDecoderSophon::SendPacket(const uint8_t* pkt, size_t len, int64_t frame_idx) {
        if (stop_.load() || (pkt == nullptr && len != 0) || len > std::numeric_limits<unsigned int>::max()) {
            return false;
        }

        std::lock_guard<std::mutex> operation_lock(operation_mutex_);
        if (stop_.load() || !opened_.load() || code_handle_ == nullptr) {
            return false;
        }

        BMVidStream stream{};
        // SAFETY: BMVidStream.buf is non-const but bmvpu_dec_decode only reads
        // the packet data. const_cast required at Sophon VPU C API boundary.
        stream.buf            = const_cast<uint8_t*>(pkt);
        stream.length         = static_cast<unsigned int>(len);
        stream.end_of_stream  = (len == 0 || !pkt) ? 1 : 0;
        stream.dts            = static_cast<uint64_t>(frame_idx);
        stream.pts            = 0;
        stream.header_buf     = nullptr;
        stream.header_size    = 0;
        stream.extradata      = nullptr;
        stream.extradata_size = 0;

        constexpr int kMaxDecodeAttempts = 100;
        for (int attempt = 0; attempt < kMaxDecodeAttempts; ++attempt) {
            const auto ret = bmvpu_dec_decode(code_handle_, stream);
            if (ret == BM_SUCCESS || ret == COSMO_VDEC_SUCCESS) {
                return true;
            }
            if (ret != BMVidDecRetStatus::BM_ERR_VDEC_BUF_FULL &&
                ret != BMVidDecRetStatus::BM_ERR_VDEC_NOBUF && ret != BMVidDecRetStatus::BM_ERR_VDEC_BUSY) {
                LOG_WARN("{} bmvpu_dec_decode failed: {}", idx_name_, ret);
                return false;
            }
            if (stop_.load()) {
                return false;
            }
            std::this_thread::sleep_for(timing::kSpinWaitInterval);
        }
        LOG_WARN("{} bmvpu_dec_decode remained busy after {} attempts", idx_name_, kMaxDecodeAttempts);
        return false;
    }

    VideoFramePtr VideoDecoderSophon::GetFrame() {
        if (stop_.load()) {
            return nullptr;
        }

        std::lock_guard<std::mutex> operation_lock(operation_mutex_);
        if (stop_.load() || !opened_.load() || !code_handle_ || !frame_) {
            return nullptr;
        }

        const auto clear_output = [this]() {
            const auto ret = bmvpu_dec_clear_output(code_handle_, frame_.get());
            if (ret != COSMO_VDEC_SUCCESS) {
                LOG_ERRO("{} bmvpu_dec_clear_output failed: {}", idx_name_, ret);
                return false;
            }
            return true;
        };

        // cppcheck-suppress knownConditionTrueFalse
        while (!stop_.load()) {
            auto ret = bmvpu_dec_get_output(code_handle_, frame_.get());
            if (ret != BM_SUCCESS && ret != COSMO_VDEC_SUCCESS) {
                if (ret == BMVidDecRetStatus::BM_ERR_VDEC_BUF_EMPTY) {
                    return nullptr;
                }
                const auto status = bmvpu_dec_get_status(code_handle_);
                if (ret == BMVidDecRetStatus::BM_ERR_VDEC_ILLEGAL_PARAM &&
                    status == BMDecStatus::BMDEC_WRONG_RESOLUTION && wrong_resolution_errors_remaining_ > 0) {
                    --wrong_resolution_errors_remaining_;
                    return nullptr;
                }
                LOG_WARN(
                    "{} bmvpu_dec_get_output failed: {}, status: {}, wrong-resolution errors remaining: {}",
                    idx_name_, ret, status, wrong_resolution_errors_remaining_);
                return nullptr;
            }

            auto pkt_cnt = bmvpu_dec_get_pkt_in_buf_cnt(code_handle_);
            if (pkt_cnt < 0) {
                LOG_WARN("{} bmvpu_dec_get_pkt_in_buf_cnt failed: {}", idx_name_, pkt_cnt);
                (void)clear_output();
                return nullptr;
            }
            if (static_cast<size_t>(pkt_cnt) > cache_size_ / 2) {
                LOG_WARN("{}", "throw away a frame");
                if (!clear_output()) {
                    return nullptr;
                }
                continue;
            }
            break;
        }

        if (stop_.load()) {
            return nullptr;
        }

        BmImagePtr vpu_frame_image(new bm_image());
        if (!AttachVideoFrame(vpu_frame_image.get(), frame_.get())) {
            (void)clear_output();
            return nullptr;
        }

        width_                    = frame_->width;
        height_                   = frame_->height;
        VideoFramePtr video_frame = std::make_shared<VideoFrame>(width_, height_, PixelFormat::PIXEL_I420);
        if (!VideoFrameValid(video_frame, true)) {
            (void)clear_output();
            return nullptr;
        }

        BmImagePtr output_frame_image(new bm_image());
        if (!AttachOutputFrame(output_frame_image.get(), frame_.get(), video_frame)) {
            video_frame.reset();
            (void)clear_output();
            return nullptr;
        }

        const auto frame_index = frame_->dts;
        const auto convert_ret =
            bmcv_image_storage_convert(bm_handle_, 1, vpu_frame_image.get(), output_frame_image.get());
        const bool clear_success = clear_output();
        if (convert_ret != BM_SUCCESS || !clear_success) {
            LOG_ERRO("{} decoded frame conversion/clear failed: convert={}", idx_name_, convert_ret);
            return nullptr;
        }

        video_frame->SetFrameIndex(frame_index);

        return video_frame;
    }

    bool VideoDecoderSophon::AttachVideoFrame(bm_image* image, BMVidFrame* frame) {
        if (image == nullptr || frame == nullptr || frame->width == 0 || frame->height == 0 ||
            frame->width > static_cast<unsigned int>(kVideoMaxWidth) ||
            frame->height > static_cast<unsigned int>(kVideoMaxHeight) || (frame->width % 2) != 0 ||
            (frame->height % 2) != 0 || frame->stride[4] < static_cast<int>(frame->width) ||
            frame->stride[5] < static_cast<int>(frame->width / 2) ||
            frame->stride[6] < static_cast<int>(frame->width / 2) || frame->buf[4] == nullptr ||
            frame->buf[5] == nullptr || frame->buf[6] == nullptr) {
            LOG_ERRO("{} invalid decoded frame layout", idx_name_);
            return false;
        }

        auto status =
            bm_image_create(bm_handle_, static_cast<int>(frame->height), static_cast<int>(frame->width),
                            bm_image_format_ext::FORMAT_YUV420P, DATA_TYPE_EXT_1N_BYTE, image, frame->stride);
        if (status != bm_status_t::BM_SUCCESS) {
            LOG_ERRO("bm_image_create failed {}", status);
            return false;
        }

        const auto y_stride = static_cast<size_t>(frame->stride[4]);
        const auto u_stride = static_cast<size_t>(frame->stride[5]);
        const auto v_stride = static_cast<size_t>(frame->stride[6]);

        const size_t y_size = y_stride * frame->height;
        const size_t u_size = u_stride * frame->height / 2;
        const size_t v_size = v_stride * frame->height / 2;
        if (y_size == 0 || u_size == 0 || v_size == 0 || y_size > std::numeric_limits<unsigned int>::max() ||
            u_size > std::numeric_limits<unsigned int>::max() ||
            v_size > std::numeric_limits<unsigned int>::max()) {
            LOG_ERRO("{} decoded frame planes exceed device descriptor limits", idx_name_);
            return false;
        }

        bm_device_mem_t devs[3];
        devs[0] = bm_mem_from_device(reinterpret_cast<unsigned long long>(frame->buf[4]),
                                     static_cast<unsigned int>(y_size));

        devs[1] = bm_mem_from_device(reinterpret_cast<unsigned long long>(frame->buf[5]),
                                     static_cast<unsigned int>(u_size));
        devs[2] = bm_mem_from_device(reinterpret_cast<unsigned long long>(frame->buf[6]),
                                     static_cast<unsigned int>(v_size));

        status = bm_image_attach(*image, devs);
        if (status != bm_status_t::BM_SUCCESS) {
            LOG_ERRO("bm_image_attach failed: {}", status);
            return false;
        }

        return true;
    }

    bool VideoDecoderSophon::AttachOutputFrame(bm_image* image, BMVidFrame* frame, VideoFramePtr frame_ptr) {
        if (image == nullptr || frame == nullptr || !VideoFrameValid(frame_ptr, true) || frame->width == 0 ||
            frame->height == 0 || frame->width > static_cast<unsigned int>(kVideoMaxWidth) ||
            frame->height > static_cast<unsigned int>(kVideoMaxHeight) || (frame->width % 2) != 0 ||
            (frame->height % 2) != 0) {
            LOG_ERRO("{} invalid output frame layout", idx_name_);
            return false;
        }

        auto image_width         = frame->width;
        auto image_height        = frame->height;
        const auto expected_size = PixelFormatUtils::CalculateFrameSize(
            static_cast<int>(image_width), static_cast<int>(image_height), PixelFormat::PIXEL_I420);
        if (!expected_size || *expected_size != frame_ptr->GetSize() ||
            *expected_size > std::numeric_limits<unsigned int>::max()) {
            LOG_ERRO("{} output frame size is invalid", idx_name_);
            return false;
        }

        auto status =
            bm_image_create(bm_handle_, static_cast<int>(frame->height), static_cast<int>(frame->width),
                            bm_image_format_ext::FORMAT_YUV420P, DATA_TYPE_EXT_1N_BYTE, image, nullptr);
        if (status != bm_status_t::BM_SUCCESS) {
            LOG_ERRO("bm_image_create failed {}", status);
            return false;
        }

        auto frame_device_mem = reinterpret_cast<bm_mem_desc_t*>(frame_ptr->GetData());
        if (!frame_device_mem) {
            LOG_ERRO("{}", "get bm_mem_desc_t failed");
            return false;
        }
        if (bm_mem_get_device_size(*frame_device_mem) < *expected_size) {
            LOG_ERRO("{} output frame allocation is too small", idx_name_);
            return false;
        }

        const auto addr     = bm_mem_get_device_addr(*frame_device_mem);
        const size_t y_size = static_cast<size_t>(image_width) * image_height;
        const size_t c_size = y_size / 4;

        bm_device_mem_t devs[3];
        devs[0] = bm_mem_from_device(addr, static_cast<unsigned int>(y_size));

        const auto u_addr = addr + y_size;
        devs[1]           = bm_mem_from_device(u_addr, static_cast<unsigned int>(c_size));

        const auto v_addr = u_addr + c_size;
        devs[2]           = bm_mem_from_device(v_addr, static_cast<unsigned int>(c_size));

        status = bm_image_attach(*image, devs);
        if (status != bm_status_t::BM_SUCCESS) {
            LOG_ERRO("bm_image_attach failed {}", status);
            return false;
        }

        return true;
    }

}  // namespace media
}  // namespace cosmo
