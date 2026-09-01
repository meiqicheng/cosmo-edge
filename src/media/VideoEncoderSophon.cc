// VideoEncoderSophon — Video Encoder Sophon implementation.

#include "media/VideoEncoderSophon.h"

#include <chrono>
#include <iterator>
#include <limits>
#include <new>
#include <thread>
#include <utility>

#include "media/PixelFormatUtils.h"
#include "util/Log.h"
#include "util/TimingConstants.h"
#include "util/VideoInfo.h"

namespace cosmo {

namespace media {

    namespace {
        constexpr size_t kMaxEncodedOutputBytes = 64U * 1024 * 1024;
        constexpr auto kFramebufferDrainTimeout = std::chrono::seconds(1);

        bool IsPendingEncodedOutput(int result, const BmVpuEncodedFrame& frame) {
            // libsophon 0.4.11's split send/get API returns the generic ERROR
            // code when the asynchronous encoder has no stream available yet.
            // Treat only that empty result as transient; all structured errors
            // and ERROR results carrying inconsistent output remain fatal.
            // data_size is initialized for the legacy SDK and may be left
            // unchanged when get_stream returns before producing output.
            return result == BM_VPU_ENC_RETURN_CODE_ERROR && frame.data == nullptr &&
                   frame.acquired_handle == nullptr;
        }

        void ReleaseEncodedOutput(BmVpuEncodedFrame* frame) {
            if (frame == nullptr) {
                return;
            }
            void* allocation = frame->data != nullptr ? frame->data : frame->acquired_handle;
            if (allocation != nullptr) {
                free(allocation);
            }
            frame->data            = nullptr;
            frame->data_size       = 0;
            frame->acquired_handle = nullptr;
        }
    }  // namespace

    static void LoggingFn(BmVpuEncLogLevel level, char const* file, int const line, char const* fn,
                          const char* format, ...) {
        va_list args;

        char const* lvlstr = "";
        switch (level) {
            case BMVPU_ENC_LOG_LEVEL_ERROR:
                lvlstr = "ERROR";
                break;
            case BMVPU_ENC_LOG_LEVEL_WARNING:
                lvlstr = "WARNING";
                break;
            case BMVPU_ENC_LOG_LEVEL_INFO:
                lvlstr = "INFO";
                break;
            case BMVPU_ENC_LOG_LEVEL_DEBUG:
                lvlstr = "DEBUG";
                break;
            case BMVPU_ENC_LOG_LEVEL_TRACE:
                lvlstr = "TRACE";
                break;
            case BMVPU_ENC_LOG_LEVEL_LOG:
                lvlstr = "LOG";
                break;
            default:
                break;
        }
        fprintf(stderr, "[%zx] %s:%d (%s)   %s: ", pthread_self(), file, line, fn, lvlstr);

        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);

        fprintf(stderr, "\n");
    }

    // VPU SDK callback — signature is fixed by bmvpu_enc API.
    // Memory allocated here is returned in both data and acquired_handle by
    // libsophon. ReleaseEncodedOutput() owns the single matching free().
    static void* AcquireOutputBuffer(void* context, size_t size, void** acquired_handle) {
        ((void)(context));
        if (acquired_handle == nullptr || size == 0 || size > kMaxEncodedOutputBytes) {
            return nullptr;
        }
        void* mem        = malloc(size);
        *acquired_handle = mem;
        return mem;
    }

    static void FinishOutputBuffer(void* context, void* /*acquired_handle*/) {
        ((void)(context));
    }

    VideoEncoderSophon::VideoEncoderSophon(void* mediaHandle) : VideoEncoder() {
        bmvpu_enc_set_logging_threshold(BMVPU_ENC_LOG_LEVEL_ERROR);
        bmvpu_enc_set_logging_function(LoggingFn);

        handle_ = reinterpret_cast<bm_handle_t>(mediaHandle);
        closed_ = true;

        soc_idx_  = 0;
        core_idx_ = bmvpu_enc_get_core_idx(soc_idx_);
    }

    VideoEncoderSophon::~VideoEncoderSophon() {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        if (encoder_) {
            SendEndFrame();
        }
        Clean();
    }

    bool VideoEncoderSophon::Open() {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        if (!closed_) {
            return true;
        }
        if (handle_ == nullptr || width_ == 0 || height_ == 0 || width_ > kVideoMaxWidth ||
            height_ > kVideoMaxHeight || width_ > std::numeric_limits<uint32_t>::max() ||
            height_ > std::numeric_limits<uint32_t>::max() || (width_ % 2) != 0 || (height_ % 2) != 0 ||
            !PixelFormatUtils::CalculateFrameSize(static_cast<int>(width_), static_cast<int>(height_),
                                                  PixelFormat::PIXEL_I420)) {
            LOG_ERRO("Invalid encoder dimensions or device handle: {}x{}", width_, height_);
            return false;
        }

        if (codec_type_ == VideoCodecType::kH264) {
            codec_fmt_ = BM_VPU_CODEC_FORMAT_H264;
        } else if (codec_type_ == VideoCodecType::kH265) {
            codec_fmt_ = BM_VPU_CODEC_FORMAT_H265;
        } else {
            LOG_WARN("invalid codec type: {}", codec_type_);
            return false;
        }

        bmvpu_enc_set_default_open_params(&open_params_, codec_fmt_);
        open_params_.pix_format   = BM_VPU_ENC_PIX_FORMAT_YUV420P;
        open_params_.frame_width  = static_cast<uint32_t>(width_);
        open_params_.frame_height = static_cast<uint32_t>(height_);
        open_params_.timebase_num = 1;
        open_params_.timebase_den = 1;
        open_params_.fps_num      = 25;
        open_params_.fps_den      = 1;
        open_params_.bitrate      = 0;
        open_params_.cqp          = 32;
        open_params_.gop_preset   = BMVpuEncGopPreset::BM_VPU_ENC_GOP_PRESET_IPPPP;
        // IDR interval (frames between intra/keyframes). SDK default is 28, which at
        // the overlay preview feed rate (~10fps) yields a keyframe only every ~2.8s.
        // SRS WebRTC starts a new subscriber from the next keyframe, so that long
        // interval shows up as multi-second black when switching to overlay. 10 frames
        // ≈ 1s at 10fps — frequent enough for snappy switch-in, cheap enough for a
        // preview-only encoder (recording uses a separate path).
        open_params_.intra_period = 10;

        auto ret = bmvpu_enc_load(soc_idx_);
        if (ret != BM_VPU_ENC_RETURN_CODE_OK) {
            LOG_ERRO("Failed to load VPU encoder_: {}", ret);
            return false;
        }
        vpu_loaded_ = true;

        bmvpu_enc_get_bitstream_buffer_info(&bs_buffer_size_, &bs_buffer_alignment_);
        constexpr unsigned int bs_buffer_arry[12] = {0, 7, 7, 7, 10, 13, 7, 7, 18, 7};
        const auto gop_index                      = static_cast<size_t>(open_params_.gop_preset);
        constexpr size_t kPageSize                = 4U * 1024;
        if (bs_buffer_size_ == 0 || gop_index >= std::size(bs_buffer_arry) ||
            bs_buffer_arry[gop_index] == 0 ||
            bs_buffer_size_ > std::numeric_limits<size_t>::max() - (kPageSize - 1)) {
            LOG_ERRO("{}", "Invalid VPU bitstream buffer metadata");
            Clean();
            return false;
        }
        const size_t aligned_bitstream_size = (bs_buffer_size_ + (kPageSize - 1)) & ~(kPageSize - 1);
        if (aligned_bitstream_size > std::numeric_limits<unsigned int>::max() / bs_buffer_arry[gop_index]) {
            LOG_ERRO("{}", "VPU bitstream buffer size overflow");
            Clean();
            return false;
        }
        bs_buffer_size_ = aligned_bitstream_size * bs_buffer_arry[gop_index];

        bs_dma_buffer_ = std::make_unique<BmVpuEncDMABuffer>();
        ret            = bmvpu_enc_dma_buffer_allocate(core_idx_, bs_dma_buffer_.get(),
                                                       static_cast<unsigned int>(bs_buffer_size_));
        if (ret != 0) {
            LOG_ERRO("bmvpu_enc_dma_buffer_allocate failed {}", ret);
            Clean();
            return false;
        }
        bs_dma_allocated_ = true;

        initial_info_ = {};
        ret           = bmvpu_enc_open(&encoder_, &open_params_, bs_dma_buffer_.get(), &initial_info_);
        if (ret != 0) {
            LOG_ERRO("Failed to open VPU encoder_: {}", ret);
            Clean();
            return false;
        }

        num_src_fb_ = static_cast<int>(initial_info_.min_num_src_fb);
        if (num_src_fb_ <= 0 || initial_info_.src_fb.size <= 0 ||
            static_cast<uint64_t>(initial_info_.src_fb.size) > std::numeric_limits<unsigned int>::max()) {
            LOG_ERRO("Invalid number of source framebuffers: {}", num_src_fb_);
            Clean();
            return false;
        }

        src_fbs_                 = std::make_unique<BmVpuFramebuffer[]>(static_cast<size_t>(num_src_fb_));
        src_dma_bufs_            = std::make_unique<BmVpuEncDMABuffer[]>(static_cast<size_t>(num_src_fb_));
        allocated_src_dma_count_ = 0;
        for (int i = 0; i < num_src_fb_; i++) {
            int src_id       = i;
            auto src_dma_buf = src_dma_bufs_.get() + i;
            ret              = bmvpu_enc_dma_buffer_allocate(core_idx_, src_dma_buf,
                                                             static_cast<unsigned int>(initial_info_.src_fb.size));
            if (ret != 0) {
                LOG_ERRO("Failed to allocate source framebuffer DMA buffer: {}", ret);
                Clean();
                return false;
            }
            ++allocated_src_dma_count_;

            auto fb = src_fbs_.get() + i;
            ret     = bmvpu_fill_framebuffer_params(fb, &initial_info_.src_fb, src_dma_buf, src_id, nullptr);
            if (ret != BM_VPU_ENC_RETURN_CODE_OK) {
                LOG_ERRO("Failed to fill source framebuffer parameters: {}", ret);
                Clean();
                return false;
            }

            ret = bmvpu_dma_buffer_map(core_idx_, src_dma_buf, BM_VPU_ENC_MAPPING_FLAG_WRITE);
            if (ret != 0) {
                LOG_ERRO("Failed to mmap source framebuffer DMA buffer: {}", ret);
                Clean();
                return false;
            }

            if (src_dma_buf->virt_addr == 0 || src_dma_buf->size == 0) {
                LOG_ERRO("{}", "VPU source framebuffer mapping is invalid");
                (void)bmvpu_dma_buffer_unmap(core_idx_, src_dma_buf);
                Clean();
                return false;
            }

            memset(reinterpret_cast<void*>(src_dma_buf->virt_addr), 0, src_dma_buf->size);
            ret = bmvpu_dma_buffer_unmap(core_idx_, src_dma_buf);
            if (ret != 0) {
                LOG_ERRO("Failed to unmap source framebuffer DMA buffer: {}", ret);
                Clean();
                return false;
            }

            frame_unused_queue_.push(fb);
        }

        memset(&input_frame_, 0, sizeof(BmVpuRawFrame));

        memset(&output_frame_, 0, sizeof(BmVpuEncodedFrame));
        output_frame_.data_size = ENC_PKT_DATA_SIZE;

        enc_params_                       = {};
        enc_params_.acquire_output_buffer = AcquireOutputBuffer;
        enc_params_.finish_output_buffer  = FinishOutputBuffer;
        enc_params_.output_buffer_context = nullptr;
        enc_params_.skip_frame            = 0;

        closed_ = false;

        return true;
    }

    void VideoEncoderSophon::Clean() {
        closed_ = true;

        if (encoder_) {
            const int ret = bmvpu_enc_close(encoder_);
            if (ret != BM_VPU_ENC_RETURN_CODE_OK) {
                LOG_ERRO("bmvpu_enc_close failed: {}", ret);
            }
            encoder_ = nullptr;
        }

        src_fbs_.reset();

        if (src_dma_bufs_) {
            for (int i = 0; i < allocated_src_dma_count_; ++i) {
                const int ret = bmvpu_enc_dma_buffer_deallocate(core_idx_, src_dma_bufs_.get() + i);
                if (ret != 0) {
                    LOG_ERRO("VPU source DMA deallocation failed: {}", ret);
                }
            }
            src_dma_bufs_.reset();
        }
        allocated_src_dma_count_ = 0;
        num_src_fb_              = 0;

        if (bs_dma_buffer_) {
            if (bs_dma_allocated_) {
                const int ret = bmvpu_enc_dma_buffer_deallocate(core_idx_, bs_dma_buffer_.get());
                if (ret != 0) {
                    LOG_ERRO("VPU bitstream DMA deallocation failed: {}", ret);
                }
            }
            bs_dma_buffer_.reset();
        }
        bs_dma_allocated_ = false;

        ReleaseEncodedOutput(&output_frame_);
        input_frame_ = {};
        while (!frame_unused_queue_.empty()) {
            frame_unused_queue_.pop();
        }
        while (!pending_packets_.empty()) {
            pending_packets_.pop();
        }
        first_pkt_received_ = false;

        if (vpu_loaded_) {
            const int ret = bmvpu_enc_unload(soc_idx_);
            if (ret != BM_VPU_ENC_RETURN_CODE_OK) {
                LOG_ERRO("bmvpu_enc_unload failed: {}", ret);
            }
            vpu_loaded_ = false;
        }
    }

    void VideoEncoderSophon::CollectFrameBuffer(const BmVpuEncodedFrame& frame) {
        if (!src_fbs_) {
            return;
        }
        for (int j = 0; j < num_src_fb_; j++) {
            BmVpuFramebuffer* fb = src_fbs_.get() + j;
            if (frame.src_idx == fb->myIndex) {
                frame_unused_queue_.push(fb);
            }
        }
    }

    VideoPacketPtr VideoEncoderSophon::SendYUVFrame(void* data) {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        if (closed_) {
            LOG_ERRO("{}", "Encoder closed_");
            return nullptr;
        }

        enum class PollResult {
            kPending,
            kPacket,
            kEnd,
            kFatal,
        };

        const auto pop_pending_packet = [this]() -> VideoPacketPtr {
            if (pending_packets_.empty()) {
                return nullptr;
            }
            auto packet = pending_packets_.front();
            pending_packets_.pop();
            return packet;
        };

        const auto poll_encoded_stream = [this]() -> PollResult {
            const int result = bmvpu_enc_get_stream(encoder_, &output_frame_, &enc_params_);
            if (IsPendingEncodedOutput(result, output_frame_)) {
                ReleaseEncodedOutput(&output_frame_);
                return PollResult::kPending;
            }
            if (result == BM_VPU_ENC_RETURN_CODE_ENC_END) {
                ReleaseEncodedOutput(&output_frame_);
                return PollResult::kEnd;
            }
            if (result != BM_VPU_ENC_RETURN_CODE_OK) {
                LOG_ERRO("bmvpu_enc_get_stream failed: {}", result);
                ReleaseEncodedOutput(&output_frame_);
                Clean();
                return PollResult::kFatal;
            }
            if (output_frame_.data == nullptr || output_frame_.data_size == 0) {
                if (output_frame_.data != nullptr || output_frame_.acquired_handle != nullptr) {
                    LOG_ERRO("{}", "bmvpu_enc_get_stream returned inconsistent empty output");
                    ReleaseEncodedOutput(&output_frame_);
                    Clean();
                    return PollResult::kFatal;
                }
                ReleaseEncodedOutput(&output_frame_);
                return PollResult::kPending;
            }

            CollectFrameBuffer(output_frame_);
            if (output_frame_.data_size > kMaxEncodedOutputBytes) {
                LOG_ERRO("Encoded packet exceeds safety limit: {}", output_frame_.data_size);
                ReleaseEncodedOutput(&output_frame_);
                Clean();
                return PollResult::kFatal;
            }

            auto* output_data = static_cast<uint8_t*>(output_frame_.data);
            try {
                auto packet  = std::make_shared<VideoPacket>();
                packet->data = std::vector<uint8_t>(output_data, output_data + output_frame_.data_size);
                pending_packets_.push(std::move(packet));
            } catch (const std::bad_alloc&) {
                LOG_ERRO("Failed to allocate encoded packet buffer: {}", output_frame_.data_size);
                ReleaseEncodedOutput(&output_frame_);
                return PollResult::kPending;
            }
            ReleaseEncodedOutput(&output_frame_);
            return PollResult::kPacket;
        };

        // When all source framebuffers are in flight, drain the asynchronous
        // encoder before accepting another frame. This avoids permanently
        // closing a healthy but busy encoder after a short fixed poll window.
        if (frame_unused_queue_.empty()) {
            const auto deadline      = std::chrono::steady_clock::now() + kFramebufferDrainTimeout;
            size_t drain_count       = 0;
            bool framebuffer_drained = false;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto result = poll_encoded_stream();
                ++drain_count;
                if (result == PollResult::kFatal) {
                    return nullptr;
                }
                if (result == PollResult::kEnd) {
                    LOG_INFO("{}", "Encoder end while waiting for a source framebuffer");
                    return pop_pending_packet();
                }
                if (result == PollResult::kPacket) {
                    framebuffer_drained = true;
                    break;
                }
                std::this_thread::sleep_for(timing::kSpinWaitInterval);
            }
            if (!framebuffer_drained) {
                LOG_WARN("No source framebuffer became available after {} drain attempts", drain_count);
                return pop_pending_packet();
            }
        }

        auto src_fb = GetUnusedFrameBuffer();
        if (!src_fb) {
            LOG_ERRO("{}", "Failed to get unused framebuffer");
            return nullptr;
        }

        if (!first_pkt_received_ && frame_unused_queue_.empty()) {
            first_pkt_received_ = true;
        }

        if (!CopyToDMA(data, src_fb)) {
            LOG_ERRO("{}", "Failed to copy data to DMA buffer");
            frame_unused_queue_.push(src_fb);
            return nullptr;
        }

        input_frame_.framebuffer = src_fb;

        size_t send_frame_count = 0;

        while (true) {
            const int send_frame_ret = bmvpu_enc_send_frame(encoder_, &input_frame_, &enc_params_);
            if (send_frame_ret != BM_VPU_ENC_RETURN_CODE_OK &&
                send_frame_ret != BM_VPU_ENC_RETURN_CODE_RESEND_FRAME) {
                LOG_ERRO("bmvpu_enc_send_frame failed: {}", send_frame_ret);
                Clean();
                return nullptr;
            }

            // Packets drained during RESEND_FRAME belong to older accepted
            // inputs. Once the current framebuffer is accepted, return the
            // oldest packet without polling another output.
            if (send_frame_ret == BM_VPU_ENC_RETURN_CODE_OK && !pending_packets_.empty()) {
                return pop_pending_packet();
            }

            size_t get_stream_count = 0;
            while (true) {
                const auto result = poll_encoded_stream();
                if (result == PollResult::kFatal) {
                    return nullptr;
                }
                if (result == PollResult::kEnd) {
                    LOG_INFO("{}", "Encoder end");
                    if (send_frame_ret == BM_VPU_ENC_RETURN_CODE_RESEND_FRAME) {
                        frame_unused_queue_.push(src_fb);
                    }
                    return pop_pending_packet();
                }
                if (result == PollResult::kPacket) {
                    break;
                }

                if (!first_pkt_received_ || get_stream_count >= GET_STREAM_BUFFER_MAX_COUNT) {
                    break;
                }
                std::this_thread::sleep_for(timing::kSpinWaitInterval);
                ++get_stream_count;
            }

            if (send_frame_ret == BM_VPU_ENC_RETURN_CODE_OK) {
                return pop_pending_packet();
            }

            if (++send_frame_count >= SEND_FRAME_BUFFER_MAX_COUNT) {
                LOG_ERRO("bmvpu_enc_send_frame remained busy after {} attempts", send_frame_count);
                frame_unused_queue_.push(src_fb);
                return pop_pending_packet();
            }
            std::this_thread::sleep_for(timing::kEncoderBusyWait);
        }
    }

    void VideoEncoderSophon::SendEndFrame() {
        constexpr size_t kMaxFlushAttempts = 100;
        for (size_t attempt = 0; !closed_ && attempt < kMaxFlushAttempts; ++attempt) {
            input_frame_.framebuffer = nullptr;
            input_frame_.dts         = 0;
            input_frame_.pts         = 0;

            const auto send_ret = bmvpu_enc_send_frame(encoder_, &input_frame_, &enc_params_);
            if (send_ret != BM_VPU_ENC_RETURN_CODE_OK && send_ret != BM_VPU_ENC_RETURN_CODE_RESEND_FRAME) {
                LOG_ERRO("bmvpu_enc_send_frame flush failed: {}", send_ret);
                break;
            }

            const auto ret = bmvpu_enc_get_stream(encoder_, &output_frame_, &enc_params_);
            if (ret == BM_VPU_ENC_RETURN_CODE_ENC_END)
                break;
            if (IsPendingEncodedOutput(ret, output_frame_)) {
                ReleaseEncodedOutput(&output_frame_);
                std::this_thread::sleep_for(timing::kEncoderBusyWait);
                continue;
            }
            if (ret != BM_VPU_ENC_RETURN_CODE_OK) {
                LOG_ERRO("bmvpu_enc_get_stream flush failed: {}", ret);
                ReleaseEncodedOutput(&output_frame_);
                break;
            }

            if (!output_frame_.data || output_frame_.data_size == 0) {
                std::this_thread::sleep_for(timing::kEncoderBusyWait);
                continue;
            }

            CollectFrameBuffer(output_frame_);
            ReleaseEncodedOutput(&output_frame_);
        }
        ReleaseEncodedOutput(&output_frame_);
    }

    BmVpuFramebuffer* VideoEncoderSophon::GetUnusedFrameBuffer() {
        if (frame_unused_queue_.empty()) {
            LOG_ERRO("{}", "Frame unused queue is empty");
            return nullptr;
        }

        BmVpuFramebuffer* fb = frame_unused_queue_.front();
        frame_unused_queue_.pop();
        if (!fb) {
            LOG_ERRO("{}", "Failed to pop framebuffer from unused queue");
            return nullptr;
        }
        for (int i = 0; i < num_src_fb_; i++) {
            auto src_fb = src_fbs_.get() + i;
            if (src_fb == fb)
                return fb;
        }

        LOG_ERRO("{}", " Framebuffer not found in source framebuffers");
        return nullptr;
    }

    bool VideoEncoderSophon::CopyToDMA(void* data, BmVpuFramebuffer* fb) {
        if (!fb || fb->dma_buffer == nullptr) {
            LOG_ERRO("{}", "CopyToDMA() - Invalid framebuffer");
            return false;
        }

        if (!data) {
            LOG_ERRO("{}", "CopyToDMA() - Invalid block");
            return false;
        }

        size_t frame_width       = open_params_.frame_width;
        size_t frame_height      = open_params_.frame_height;
        const auto expected_size = PixelFormatUtils::CalculateFrameSize(
            static_cast<int>(frame_width), static_cast<int>(frame_height), PixelFormat::PIXEL_I420);
        if (!expected_size || fb->y_stride < frame_width || fb->cbcr_stride < frame_width / 2 ||
            fb->dma_buffer->size == 0) {
            LOG_ERRO("{}", "CopyToDMA() - Invalid frame layout");
            return false;
        }

        auto src_mem            = reinterpret_cast<bm_device_mem_t*>(data);
        bm_device_mem_t dst_mem = bm_mem_from_device(fb->dma_buffer->phys_addr, fb->dma_buffer->size);
        if (bm_mem_get_device_size(*src_mem) < *expected_size) {
            LOG_ERRO("CopyToDMA() - Source allocation too small: {} < {}", bm_mem_get_device_size(*src_mem),
                     *expected_size);
            return false;
        }

        size_t src_y_size     = frame_width * frame_height;
        size_t w_c            = frame_width / 2;
        size_t h_c            = frame_height / 2;
        size_t src_c_size     = w_c * h_c;
        const auto plane_fits = [fb](size_t offset, size_t stride, size_t rows, size_t row_bytes) {
            if (rows == 0 || row_bytes == 0 || stride < row_bytes || offset > fb->dma_buffer->size) {
                return false;
            }
            const size_t last_row = rows - 1;
            if (last_row > (std::numeric_limits<size_t>::max() - offset) / stride) {
                return false;
            }
            const size_t row_offset = offset + last_row * stride;
            return row_offset <= fb->dma_buffer->size && row_bytes <= fb->dma_buffer->size - row_offset;
        };
        if (!plane_fits(fb->y_offset, fb->y_stride, frame_height, frame_width) ||
            !plane_fits(fb->cb_offset, fb->cbcr_stride, h_c, w_c) ||
            !plane_fits(fb->cr_offset, fb->cbcr_stride, h_c, w_c)) {
            LOG_ERRO("{}", "CopyToDMA() - Destination allocation too small");
            return false;
        }

        const auto copy = [this, &dst_mem, src_mem](size_t dst_offset, size_t src_offset, size_t size) {
            return bm_memcpy_d2d_byte(handle_, dst_mem, dst_offset, *src_mem, src_offset, size) == BM_SUCCESS;
        };
        if (frame_width == fb->y_stride) {
            if (!copy(fb->y_offset, 0, src_y_size)) {
                return false;
            }
        } else {
            for (size_t i = 0; i < frame_height; i++) {
                size_t src_offset = i * frame_width;
                size_t dst_offset = fb->y_offset + i * fb->y_stride;
                if (!copy(dst_offset, src_offset, frame_width)) {
                    return false;
                }
            }
        }

        if (w_c == fb->cbcr_stride) {
            if (!copy(fb->cb_offset, src_y_size, src_c_size) ||
                !copy(fb->cr_offset, src_y_size + src_c_size, src_c_size)) {
                return false;
            }
        } else {
            for (size_t i = 0; i < h_c; i++) {
                size_t src_offset = src_y_size + i * w_c;
                size_t dst_offset = fb->cb_offset + i * fb->cbcr_stride;
                if (!copy(dst_offset, src_offset, w_c)) {
                    return false;
                }

                src_offset += src_c_size;
                dst_offset = fb->cr_offset + i * fb->cbcr_stride;
                if (!copy(dst_offset, src_offset, w_c)) {
                    return false;
                }
            }
        }

        return true;
    }

}  // namespace media

}  // namespace cosmo
