#if defined(COSMO_NN_USE_RKNN_BACKEND) && defined(COSMO_MEDIA_USE_ROCKCHIP_BACKEND)

#include "nn/device/rknn/rknn_preprocess_node.h"

#include <rga/im2d.h>

#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>

#include "media/RockchipRgaBuffer.h"
#include "nn/core/inference_pipeline_metrics.h"
#include "nn/device/rknn/rknn_net_node.h"
#include "nn/node/node_type_utils.h"
#include "nn/utils/op.h"
#include "util/Log.h"

namespace cosmo::nn {
namespace {

    using MetricsClock = std::chrono::steady_clock;

    constexpr int kDetectorInputSize = 640;
    constexpr float kNormalizeScale  = 0.00392157f;
    // RGA2-class cores support at least 1/8..8 scaling. Keep the shared path
    // conservative so the same implementation is valid on both RGA2 and RGA3.
    constexpr int kConservativeRgaScaleLimit = 8;

    struct YuvToBgrCoeffs {
        int y_offset;  // studio swing subtracts 16 from luma
        int y_scale;   // <<8 fixed point
        int ub;        // U contribution to B
        int ug;        // U contribution to G
        int vg;        // V contribution to G
        int vr;        // V contribution to R
    };

    // Canonical studio-swing and full-swing matrices scaled by 256 (JFIF and
    // ITU-R derivations); gray maps to gray for every row.
    constexpr YuvToBgrCoeffs kYuv601Limited{16, 298, 516, -100, -208, 409};
    constexpr YuvToBgrCoeffs kYuv601Full{0, 256, 454, -88, -183, 359};
    constexpr YuvToBgrCoeffs kYuv709Limited{16, 298, 541, -54, -136, 459};
    constexpr YuvToBgrCoeffs kYuv709Full{0, 256, 477, -34, -86, 405};

    YuvToBgrCoeffs ResolveYuvToBgrCoeffs(NativeImageColorSpace color_space,
                                         NativeImageColorRange color_range) {
        const bool full = color_range == NativeImageColorRange::Full;
        switch (color_space) {
            case NativeImageColorSpace::Bt709:
            case NativeImageColorSpace::Bt2020:  // co-sited with 709 in this fallback
                return full ? kYuv709Full : kYuv709Limited;
            case NativeImageColorSpace::Bt601:
                return full ? kYuv601Full : kYuv601Limited;
            case NativeImageColorSpace::Unspecified:
            default:
                return kYuv601Limited;  // legacy behavior of this fallback
        }
    }

    uint64_t ElapsedNanoseconds(MetricsClock::time_point started_at) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(MetricsClock::now() - started_at).count());
    }

    bool EnvironmentFlag(const char* name, bool default_value) {
        const char* raw = std::getenv(name);
        if (!raw || *raw == '\0')
            return default_value;
        std::string value(raw);
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (value == "0" || value == "false" || value == "off" || value == "no")
            return false;
        if (value == "1" || value == "true" || value == "on" || value == "yes")
            return true;
        return default_value;
    }

    void LogRgaFallbackOnce(IM_STATUS status) {
        static std::atomic_flag logged = ATOMIC_FLAG_INIT;
        if (!logged.test_and_set(std::memory_order_relaxed)) {
            LOG_WARN("RKNN detector RGA preprocessing failed with status {} ({}); using CPU fallback", status,
                     imStrError_t(status));
        }
    }

    void LogRgaBoundInputFallbackOnce(const std::string& reason) {
        static std::atomic_flag logged = ATOMIC_FLAG_INIT;
        if (!logged.test_and_set(std::memory_order_relaxed)) {
            LOG_WARN("RKNN RGA bound input unavailable; retaining the host preprocessing path: {}", reason);
        }
    }

    void LogMppDmaBufFallbackOnce(IM_STATUS status) {
        static std::atomic_flag logged = ATOMIC_FLAG_INIT;
        if (!logged.test_and_set(std::memory_order_relaxed)) {
            LOG_WARN("RKNN MPP DMA-BUF preprocessing failed with status {} ({}); using host RGA input",
                     status, imStrError_t(status));
        }
    }

    void LogRgaCropResizeFallbackOnce(IM_STATUS status) {
        static std::atomic_flag logged = ATOMIC_FLAG_INIT;
        if (!logged.test_and_set(std::memory_order_relaxed)) {
            LOG_WARN("RKNN RGA crop-resize failed with status {} ({}); using CPU fallback", status,
                     imStrError_t(status));
        }
    }

    void LogRgaBt2020DowngradeOnce() {
        static std::atomic_flag logged = ATOMIC_FLAG_INIT;
        if (!logged.test_and_set(std::memory_order_relaxed)) {
            LOG_WARN(
                "This librga header/runtime does not expose BT.2020 full-CSC modes; "
                "using the matching BT.709 range for this source");
        }
    }

    void BilinearResizePacked(const uint8_t* source, int source_width, int source_height, int channels,
                              uint8_t* destination, int destination_width, int destination_height,
                              bool swap_red_blue) {
        const float x_ratio = static_cast<float>(source_width) / destination_width;
        const float y_ratio = static_cast<float>(source_height) / destination_height;
        for (int destination_y = 0; destination_y < destination_height; ++destination_y) {
            const float source_y = (destination_y + 0.5f) * y_ratio - 0.5f;
            int y0               = static_cast<int>(source_y);
            const float y_part   = source_y - y0;
            y0                   = std::max(0, std::min(y0, source_height - 1));
            const int y1         = std::min(y0 + 1, source_height - 1);
            for (int destination_x = 0; destination_x < destination_width; ++destination_x) {
                const float source_x = (destination_x + 0.5f) * x_ratio - 0.5f;
                int x0               = static_cast<int>(source_x);
                const float x_part   = source_x - x0;
                x0                   = std::max(0, std::min(x0, source_width - 1));
                const int x1         = std::min(x0 + 1, source_width - 1);
                for (int channel = 0; channel < channels; ++channel) {
                    const int source_channel = swap_red_blue && channel != 1 ? 2 - channel : channel;
                    const float v00          = source[(y0 * source_width + x0) * channels + source_channel];
                    const float v01          = source[(y0 * source_width + x1) * channels + source_channel];
                    const float v10          = source[(y1 * source_width + x0) * channels + source_channel];
                    const float v11          = source[(y1 * source_width + x1) * channels + source_channel];
                    const float value = v00 * (1 - x_part) * (1 - y_part) + v01 * x_part * (1 - y_part) +
                                        v10 * (1 - x_part) * y_part + v11 * x_part * y_part;
                    destination[(destination_y * destination_width + destination_x) * channels + channel] =
                        static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, value + 0.5f)));
                }
            }
        }
    }

    size_t PackedByteCount(int width, int height) {
        if (width <= 0 || height <= 0)
            return 0;
        const auto pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (pixels > std::numeric_limits<size_t>::max() / 3)
            return 0;
        return pixels * 3;
    }

    IM_COLOR_SPACE_MODE ResolveRgaYuvColorSpace(const BlobHandle::NativeImage& image) {
        const bool full_range = image.color_range == NativeImageColorRange::Full;
        switch (image.color_space) {
            case NativeImageColorSpace::Bt709:
                return full_range ? IM_YUV_BT709_FULL_RANGE : IM_YUV_BT709_LIMIT_RANGE;
            case NativeImageColorSpace::Bt2020:
                if (!media::RockchipRgaHasBt2020ColorSpace())
                    LogRgaBt2020DowngradeOnce();
                return media::RockchipRgaBt2020ColorSpace(full_range);
            case NativeImageColorSpace::Bt601:
            case NativeImageColorSpace::Unspecified:
            default:
                return full_range ? IM_YUV_BT601_FULL_RANGE : IM_YUV_BT601_LIMIT_RANGE;
        }
    }

    im_rect AlignYuv420Crop(int x, int y, int width, int height, int image_width, int image_height) {
        const int aligned_image_width  = image_width & ~1;
        const int aligned_image_height = image_height & ~1;
        const int left                 = std::max(0, x & ~1);
        const int top                  = std::max(0, y & ~1);
        int right                      = std::min(aligned_image_width, (x + width + 1) & ~1);
        int bottom                     = std::min(aligned_image_height, (y + height + 1) & ~1);
        if (right <= left)
            right = std::min(aligned_image_width, left + 2);
        if (bottom <= top)
            bottom = std::min(aligned_image_height, top + 2);
        return {left, top, right - left, bottom - top};
    }

    int NextRgaScaleDimension(int current, int target) {
        if (current <= 0 || target <= 0)
            return 0;
        const auto current_wide = static_cast<int64_t>(current);
        const auto target_wide  = static_cast<int64_t>(target);
        if (target_wide > current_wide * kConservativeRgaScaleLimit) {
            return static_cast<int>(current_wide * kConservativeRgaScaleLimit);
        }
        if (current_wide > target_wide * kConservativeRgaScaleLimit) {
            return static_cast<int>((current_wide + kConservativeRgaScaleLimit - 1) /
                                    kConservativeRgaScaleLimit);
        }
        return target;
    }

    bool RunStagedRgaCropResize(rga_buffer_t source, const im_rect& source_rect,
                                IM_COLOR_SPACE_MODE source_color_space, rga_buffer_t target,
                                const im_rect& target_rect, IM_STATUS& last_status) {
        if (source_rect.width <= 0 || source_rect.height <= 0 || target_rect.width <= 0 ||
            target_rect.height <= 0) {
            last_status = IM_STATUS_INVALID_PARAM;
            return false;
        }

        std::vector<std::pair<int, int>> destinations;
        int current_width  = source_rect.width;
        int current_height = source_rect.height;
        while (true) {
            const int next_width  = NextRgaScaleDimension(current_width, target_rect.width);
            const int next_height = NextRgaScaleDimension(current_height, target_rect.height);
            if (next_width <= 0 || next_height <= 0 || destinations.size() >= 16) {
                last_status = IM_STATUS_INVALID_PARAM;
                return false;
            }
            destinations.emplace_back(next_width, next_height);
            if (next_width == target_rect.width && next_height == target_rect.height)
                break;
            if (next_width == current_width && next_height == current_height) {
                last_status = IM_STATUS_INVALID_PARAM;
                return false;
            }
            current_width  = next_width;
            current_height = next_height;
        }

        std::vector<std::vector<uint8_t>> stage_storage;
        std::vector<media::ScopedRgaBufferHandle> stage_handles;
        std::vector<rga_buffer_t> stage_buffers;
        if (destinations.size() > 1) {
            const auto intermediate_count = destinations.size() - 1;
            stage_storage.reserve(intermediate_count);
            stage_handles.reserve(intermediate_count);
            stage_buffers.reserve(intermediate_count);
        }

        rga_buffer_t current_source = source;
        im_rect current_source_rect = source_rect;
        for (size_t index = 0; index < destinations.size(); ++index) {
            const bool final_pass = index + 1 == destinations.size();
            rga_buffer_t current_target{};
            im_rect current_target_rect{};
            if (final_pass) {
                current_target      = target;
                current_target_rect = target_rect;
            } else {
                const int width    = destinations[index].first;
                const int height   = destinations[index].second;
                const size_t bytes = PackedByteCount(width, height);
                stage_storage.emplace_back(bytes);
                stage_handles.emplace_back();
                if (bytes == 0 || !stage_handles.back().ImportVirtual(stage_storage.back().data(), bytes)) {
                    last_status = IM_STATUS_OUT_OF_MEMORY;
                    return false;
                }
                stage_buffers.push_back(wrapbuffer_handle_t(stage_handles.back().Get(), width, height, width,
                                                            height, RK_FORMAT_RGB_888));
                current_target      = stage_buffers.back();
                current_target_rect = {0, 0, width, height};
            }

            if (index == 0 && source_color_space != IM_COLOR_SPACE_DEFAULT) {
                media::SetRgaYuvToRgbColorSpace(current_source, current_target, source_color_space);
            }
            const im_rect empty_rect{};
            const rga_buffer_t empty_buffer{};
            const auto started = MetricsClock::now();
            last_status        = improcess(current_source, current_target, empty_buffer, current_source_rect,
                                           current_target_rect, empty_rect, IM_SYNC);
            const bool success = media::RockchipRgaSucceeded(last_status);
            GetInferencePipelineMetrics().RecordRknnRgaCropResize(ElapsedNanoseconds(started), success);
            if (!success)
                return false;

            current_source      = current_target;
            current_source_rect = {0, 0, current_target_rect.width, current_target_rect.height};
        }
        return true;
    }

}  // namespace

bool RknnFastPreprocessEnabled() {
    return EnvironmentFlag("COSMO_RKNN_FAST_PREPROCESS", true);
}

bool RknnForceRgaFailure() {
    return EnvironmentFlag("COSMO_RKNN_RGA_FORCE_FAIL", false);
}

bool RknnMppDmaBufEnabled() {
    return EnvironmentFlag("COSMO_RKNN_MPP_DMABUF", true);
}

bool RknnForceMppDmaBufFailure() {
    return EnvironmentFlag("COSMO_RKNN_MPP_DMABUF_FORCE_FAIL", false);
}

bool IsRknnDetectorResizeContract(int out_height, int out_width, int gravity,
                                  const std::vector<int>& padding_color) {
    return out_height == kDetectorInputSize && out_width == kDetectorInputSize && gravity == 1 &&
           padding_color.size() >= 3 && padding_color[0] == 114 && padding_color[1] == 114 &&
           padding_color[2] == 114;
}

bool IsRknnNativeNormalizeContract(const std::vector<float>& mean, const std::vector<float>& std_dev,
                                   float scale, const DimsVector& input_dims) {
    if (input_dims.size() != 4 || input_dims[0] != 1 || input_dims[1] <= 0 || input_dims[2] <= 0 ||
        input_dims[3] != 3 || mean.size() < 3 || !std_dev.empty() ||
        std::fabs(scale - kNormalizeScale) > 1e-8f) {
        return false;
    }
    return std::all_of(mean.begin(), mean.begin() + 3, [](float value) { return std::fabs(value) <= 1e-8f; });
}

void MapPackedU8ToNativeInt8(const uint8_t* source, int8_t* destination, size_t pixels, bool swap_red_blue) {
    if (!source || !destination)
        return;
    for (size_t pixel = 0; pixel < pixels; ++pixel) {
        const auto source_offset      = pixel * 3;
        const auto destination_offset = pixel * 3;
        for (size_t channel = 0; channel < 3; ++channel) {
            const size_t source_channel = swap_red_blue && channel != 1 ? 2 - channel : channel;
            destination[destination_offset + channel] =
                static_cast<int8_t>(static_cast<int>(source[source_offset + source_channel]) - 128);
        }
    }
}

RknnResizeNode::RknnResizeNode() : Node() {
    node_type     = NodeType::NODE_RESIZE;
    name          = NodeTypeUtils::NodeTypeToStr(NODE_RESIZE).append("_0");
    one_blob_only = true;
}

RknnResizeNode::~RknnResizeNode() {
    ReleaseRgaBoundTarget();
}

void RknnResizeNode::LoadParam(Op* op) {
    const auto* resize = dynamic_cast<Resize*>(op);
    if (!resize)
        return;
    if (resize->dsize.size() >= 2) {
        out_height_ = resize->dsize[0];
        out_width_  = resize->dsize[1];
    }
    gravity_           = resize->gravity;
    padding_color_     = resize->color;
    detector_contract_ = IsRknnDetectorResizeContract(out_height_, out_width_, gravity_, padding_color_);
}

DeviceType RknnResizeNode::GetTopBlobDeviceType() {
    return DeviceType::DEVICE_NAIVE;
}

Status RknnResizeNode::InferTopShapes() {
    if (out_height_ <= 0 || out_width_ <= 0)
        return Status(COSMO_NN_ERR_INVALID_CFG, "RKNN resize output dimensions must be positive");
    shared_resource->net_input_w = out_width_;
    shared_resource->net_input_h = out_height_;
    top_blob_shapes              = {{1, out_height_, out_width_, 3}};
    top_blob_data_types          = {DataType::DATA_TYPE_UINT8};
    return COSMO_NN_OK;
}

size_t RknnResizeNode::GetBottomCount() {
    return 1;
}

size_t RknnResizeNode::GetTopCount() {
    return 1;
}

void RknnResizeNode::ReleaseRgaBoundTarget() {
    if (rga_bound_target_handle_ != 0) {
        releasebuffer_handle(static_cast<rga_buffer_handle_t>(rga_bound_target_handle_));
        rga_bound_target_handle_     = 0;
        rga_bound_target_generation_ = 0;
    }
}

bool RknnResizeNode::AcquireRgaBoundTarget(uint32_t& handle) {
    handle                = 0;
    const bool enabled    = RknnRgaBoundInputEnabled();
    const bool compatible = shared_resource && shared_resource->rknn_bound_input_preprocess_compatible;
    const bool provider_available = shared_resource && shared_resource->rknn_bound_input_provider;
    if (!detector_contract_ || rga_bound_target_unavailable_ || !enabled || !compatible ||
        !provider_available) {
        if (detector_contract_ && !rga_bound_target_unavailable_ && !rga_bound_guard_logged_) {
            LOG_WARN(
                "RKNN detector cannot acquire the RGA-bound input target: enabled={} shared_resource={} "
                "preprocess_compatible={} provider={}",
                enabled, shared_resource != nullptr, compatible, provider_available);
            rga_bound_guard_logged_ = true;
        }
        return false;
    }
    std::string reason;
    auto* provider = shared_resource->rknn_bound_input_provider;
    if (!provider->EnsureRgaBoundInput(out_height_, out_width_, reason)) {
        rga_bound_target_unavailable_ = true;
        LogRgaBoundInputFallbackOnce(reason);
        return false;
    }
    const auto& target = shared_resource->rknn_bound_input_target;
    if (target.owner != provider || !target.Matches(out_height_, out_width_) ||
        target.bytes > static_cast<size_t>(std::numeric_limits<int>::max())) {
        rga_bound_target_unavailable_ = true;
        LogRgaBoundInputFallbackOnce("provider returned an incompatible target");
        return false;
    }
    if (rga_bound_target_handle_ != 0 && rga_bound_target_generation_ == target.generation) {
        handle = rga_bound_target_handle_;
        return true;
    }
    ReleaseRgaBoundTarget();
    const auto import_started = MetricsClock::now();
    const auto imported       = importbuffer_fd(target.fd, static_cast<int>(target.bytes));
    GetInferencePipelineMetrics().RecordRknnRgaBoundInputImport(ElapsedNanoseconds(import_started),
                                                                imported != 0);
    if (imported == 0) {
        rga_bound_target_unavailable_ = true;
        GetInferencePipelineMetrics().RecordRknnRgaFailure();
        LogRgaBoundInputFallbackOnce("RGA could not import the RKNN DMA-BUF fd");
        return false;
    }
    rga_bound_target_handle_     = static_cast<uint32_t>(imported);
    rga_bound_target_generation_ = target.generation;
    handle                       = rga_bound_target_handle_;
    return true;
}

bool RknnResizeNode::ResizeWithRga(const Blob& bottom, Blob& top, bool allow_bound_target) {
    if (!detector_contract_ || RknnForceRgaFailure()) {
        if (detector_contract_ && RknnForceRgaFailure())
            GetInferencePipelineMetrics().RecordRknnRgaFailure();
        return false;
    }

    auto& mutable_bottom = const_cast<Blob&>(bottom);
    auto bottom_desc     = mutable_bottom.GetBlobDesc();
    auto bottom_handle   = mutable_bottom.GetHandle();
    auto top_handle      = top.GetHandle();
    const bool bottom_has_native = bottom_handle.native_image.Valid();
    if ((!bottom_handle.base && !bottom_has_native) || bottom_desc.dims.size() != 4 ||
        bottom_desc.dims[0] != 1 || bottom_desc.dims[3] != 3 ||
        (bottom_desc.image_format != IMAGE_BGR && bottom_desc.image_format != IMAGE_RGB)) {
        static std::atomic<bool> desc_warned{false};
        if (!desc_warned.exchange(true)) {
            const auto& n = bottom_handle.native_image;
            LOG_WARN("RknnResize RGA rejected source: base={} dims=[{},{},{},{}] fmt={} native(fd={} bytes={} "
                     "{}x{} stride={}x{} fmt={} valid={})",
                     bottom_handle.base != nullptr, bottom_desc.dims.size(),
                     bottom_desc.dims.empty() ? -1 : bottom_desc.dims[0],
                     bottom_desc.dims.size() < 3 ? -1 : bottom_desc.dims[1],
                     bottom_desc.dims.size() < 3 ? -1 : bottom_desc.dims[2],
                     bottom_desc.dims.size() < 4 ? -1 : bottom_desc.dims[3],
                     static_cast<int>(bottom_desc.image_format), n.fd, n.bytes, n.width, n.height,
                     n.width_stride, n.height_stride, static_cast<int>(n.format),
                     bottom_has_native);
        }
        GetInferencePipelineMetrics().RecordRknnRgaFailure();
        return false;
    }

    const int source_height = bottom_desc.dims[1];
    const int source_width  = bottom_desc.dims[2];
    const auto source_size  = PackedByteCount(source_width, source_height);
    const auto target_size  = PackedByteCount(out_width_, out_height_);
    media::ScopedRgaBufferHandle host_target_handle;
    uint32_t target_handle  = 0;
    const bool bound_target = allow_bound_target && AcquireRgaBoundTarget(target_handle);
    if (!bound_target) {
        if (!top_handle.base) {
            GetInferencePipelineMetrics().RecordRknnRgaFailure();
            return false;
        }
        host_target_handle.ImportVirtual(top_handle.base, target_size);
        target_handle = host_target_handle.Get();
    }
    if (target_handle == 0) {
        GetInferencePipelineMetrics().RecordRknnRgaFailure();
        LogRgaFallbackOnce(IM_STATUS_OUT_OF_MEMORY);
        return false;
    }

    int target_width_stride = out_width_;
    if (bound_target)
        target_width_stride = shared_resource->rknn_bound_input_target.width_stride;
    auto target = wrapbuffer_handle_t(static_cast<rga_buffer_handle_t>(target_handle), out_width_,
                                      out_height_, target_width_stride, out_height_, RK_FORMAT_RGB_888);

    // letterbox 底色填充。旧版 RGA 多核驱动会以内核级 "no core match" 拒绝独立的
    // FILL 任务，而同一缓冲区上的普通拷贝/CSC/resize 任务可正常提交；因此只要目标
    // 存在 host 映射就用 CPU 填充，imfill_t 仅保留给没有 host 映射的 DMA-BUF bound 目标。
    const uint8_t padding_byte =
        static_cast<uint8_t>(padding_color_.empty() ? 114 : padding_color_[0]);
    IM_STATUS last_status  = IM_STATUS_FAILED;
    const auto fill_started = MetricsClock::now();
    bool fill_ok = true;
    if (!bound_target && top_handle.base) {
        std::memset(top_handle.base, padding_byte, target_size);
    } else {
        const im_rect full_target{0, 0, out_width_, out_height_};
        const int fill_color = (padding_color_[0] << 16) | (padding_color_[1] << 8) | padding_color_[2];
        last_status          = imfill_t(target, full_target, fill_color, 1);
        fill_ok              = media::RockchipRgaSucceeded(last_status);
    }
    GetInferencePipelineMetrics().RecordRknnRgaFill(ElapsedNanoseconds(fill_started));
    if (!fill_ok)
        return false;

    const auto run_resize_once = [&](rga_buffer_handle_t source_handle, int visible_width,
                                     int visible_height, int width_stride, int height_stride,
                                     int source_format, IM_COLOR_SPACE_MODE yuv_color_space,
                                     bool apply_color_space) {
        auto source = wrapbuffer_handle_t(source_handle, visible_width, visible_height, width_stride,
                                          height_stride, source_format);
        if (yuv_color_space != IM_COLOR_SPACE_DEFAULT && apply_color_space) {
            // Modern librga interprets color_space_mode as the color range of
            // each buffer. Writing the legacy conversion selector (0x1)
            // directly into the source descriptor is rejected by legacy
            // RGA2-Pro runtimes. Describe source and destination ranges explicitly;
            // improcess then performs CSC and resize in the same DMA-BUF job.
            media::SetRgaYuvToRgbColorSpace(source, target, yuv_color_space);
        }

        const float scale        = std::min(static_cast<float>(out_width_) / visible_width,
                                            static_cast<float>(out_height_) / visible_height);
        const int resized_width  = static_cast<int>(visible_width * scale);
        const int resized_height = static_cast<int>(visible_height * scale);
        const int offset_x       = (out_width_ - resized_width) / 2;
        const int offset_y       = (out_height_ - resized_height) / 2;
        const im_rect source_rect{0, 0, visible_width, visible_height};
        const im_rect target_rect{offset_x, offset_y, resized_width, resized_height};
        const im_rect empty_rect{};
        const rga_buffer_t empty_buffer{};
        const auto resize_started = MetricsClock::now();
        last_status = improcess(source, target, empty_buffer, source_rect, target_rect, empty_rect, IM_SYNC);
        GetInferencePipelineMetrics().RecordRknnRgaResizeColor(ElapsedNanoseconds(resize_started));
        return media::RockchipRgaSucceeded(last_status);
    };
    // Older kernel drivers (e.g. RGA multicore v1.3.x paired with userspace
    // 1.10.x) can reject jobs that carry color_space_mode bits with a driver
    // level "no core match" submission failure. Detect that once and keep the
    // pipeline alive by submitting subsequent YUV->RGB jobs without explicit
    // color space descriptors; BT.601 limited-range is the safe default there.
    static std::atomic<bool> yuv_colorspace_unsupported{false};
    const auto run_resize = [&](rga_buffer_handle_t source_handle, int visible_width, int visible_height,
                                int width_stride, int height_stride, int source_format,
                                IM_COLOR_SPACE_MODE yuv_color_space) {
        if (yuv_colorspace_unsupported.load(std::memory_order_relaxed))
            return run_resize_once(source_handle, visible_width, visible_height, width_stride, height_stride,
                                   source_format, yuv_color_space, false);
        if (run_resize_once(source_handle, visible_width, visible_height, width_stride, height_stride,
                            source_format, yuv_color_space, true))
            return true;
        if (yuv_color_space == IM_COLOR_SPACE_DEFAULT)
            return false;
        yuv_colorspace_unsupported.store(true, std::memory_order_relaxed);
        LOG_WARN("RKNN RGA rejected YUV color-space descriptors; retrying without them "
                 "(legacy RGA driver detected)");
        return run_resize_once(source_handle, visible_width, visible_height, width_stride, height_stride,
                               source_format, yuv_color_space, false);
    };

    const auto& native           = bottom_handle.native_image;
    const bool native_compatible = RknnMppDmaBufEnabled() && native.Valid() && native.width == source_width &&
                                   native.height == source_height &&
                                   (native.format == IMAGE_NV12 || native.format == IMAGE_I420);
    if (native_compatible) {
        bool native_success = false;
        media::ScopedRgaBufferHandle native_source_handle;
        if (!RknnForceMppDmaBufFailure()) {
            const auto import_started = MetricsClock::now();
            native_source_handle.ImportFd(native.fd, native.bytes);
            GetInferencePipelineMetrics().RecordRknnMppDmaBufImport(ElapsedNanoseconds(import_started),
                                                                    native_source_handle.Get() != 0);
            if (native_source_handle.Get() != 0) {
                const int native_format =
                    native.format == IMAGE_NV12 ? RK_FORMAT_YCbCr_420_SP : RK_FORMAT_YCbCr_420_P;
                native_success =
                    run_resize(native_source_handle.Get(), native.width, native.height, native.width_stride,
                               native.height_stride, native_format, ResolveRgaYuvColorSpace(native));
            }
        }
        if (native_success) {
            GetInferencePipelineMetrics().RecordRknnMppDmaBufFrame(native.bytes);
        } else {
            GetInferencePipelineMetrics().RecordRknnMppDmaBufFallback();
            LogMppDmaBufFallbackOnce(last_status);
        }
        if (native_success) {
            if (bound_target) {
                auto& bound_input = shared_resource->rknn_bound_input_target;
                if (bound_input.owner == shared_resource->rknn_bound_input_provider &&
                    bound_input.generation == rga_bound_target_generation_) {
                    bound_input.frame_ready = true;
                }
            }
            return true;
        }
    }

    media::ScopedRgaBufferHandle host_source_handle(bottom_handle.base, source_size);
    const int host_source_format =
        bottom_desc.image_format == IMAGE_RGB ? RK_FORMAT_RGB_888 : RK_FORMAT_BGR_888;
    if (host_source_handle.Get() == 0 ||
        !run_resize(host_source_handle.Get(), source_width, source_height, source_width, source_height,
                    host_source_format, IM_COLOR_SPACE_DEFAULT)) {
        GetInferencePipelineMetrics().RecordRknnRgaFailure();
        LogRgaFallbackOnce(host_source_handle.Get() == 0 ? IM_STATUS_OUT_OF_MEMORY : last_status);
        return false;
    }
    if (bound_target) {
        auto& bound_input = shared_resource->rknn_bound_input_target;
        if (bound_input.owner == shared_resource->rknn_bound_input_provider &&
            bound_input.generation == rga_bound_target_generation_) {
            bound_input.frame_ready = true;
        }
    }
    return true;
}

void RknnResizeNode::ResizeWithCpu(const Blob& bottom, Blob& top, bool output_rgb) const {
    auto& mutable_bottom     = const_cast<Blob&>(bottom);
    auto bottom_desc         = mutable_bottom.GetBlobDesc();
    const int source_height  = bottom_desc.dims[1];
    const int source_width   = bottom_desc.dims[2];
    const int channels       = bottom_desc.dims[3];
    const auto* source       = static_cast<const uint8_t*>(mutable_bottom.GetHandle().base);
    auto* destination        = static_cast<uint8_t*>(top.GetHandle().base);
    const bool swap_red_blue = output_rgb && bottom_desc.image_format != IMAGE_RGB;

    if (gravity_ == 0) {
        BilinearResizePacked(source, source_width, source_height, channels, destination, out_width_,
                             out_height_, swap_red_blue);
        return;
    }

    const float scale        = std::min(static_cast<float>(out_width_) / source_width,
                                        static_cast<float>(out_height_) / source_height);
    const int resized_width  = static_cast<int>(source_width * scale);
    const int resized_height = static_cast<int>(source_height * scale);
    const uint8_t padding    = static_cast<uint8_t>(padding_color_.empty() ? 114 : padding_color_[0]);
    std::memset(destination, padding, PackedByteCount(out_width_, out_height_));
    std::vector<uint8_t> resized(PackedByteCount(resized_width, resized_height));
    BilinearResizePacked(source, source_width, source_height, channels, resized.data(), resized_width,
                         resized_height, swap_red_blue);
    int offset_x = 0;
    int offset_y = 0;
    if (gravity_ == 1) {
        offset_x = (out_width_ - resized_width) / 2;
        offset_y = (out_height_ - resized_height) / 2;
    }
    for (int row = 0; row < resized_height; ++row) {
        std::memcpy(destination + ((offset_y + row) * out_width_ + offset_x) * channels,
                    resized.data() + row * resized_width * channels,
                    static_cast<size_t>(resized_width) * channels);
    }
}

Status RknnResizeNode::ResizeNativeWithCpu(const Blob& bottom, Blob& top) const {
    // Read-only use; accessors are non-const (same idiom as line ~1225).
    const auto& native = const_cast<Blob&>(bottom).GetHandle().native_image;
    if (native.fd < 0 || native.bytes == 0 || native.width <= 0 || native.height <= 0 ||
        native.width_stride < native.width || native.height_stride < native.height)
        return Status(COSMO_NN_ERR_INVALID_INPUT,
                      "RKNN native CPU fallback received an invalid DMA-BUF descriptor");
    if (native.format != IMAGE_NV12 && native.format != IMAGE_I420)
        return Status(COSMO_NN_ERR_INVALID_INPUT, "RKNN native CPU fallback unsupported pixel format");

    const bool nv12 = native.format == IMAGE_NV12;
    const size_t luma_bytes =
        static_cast<size_t>(native.width_stride) * static_cast<size_t>(native.height_stride);
    const size_t chroma_rows = (static_cast<size_t>(native.height_stride) + 1) / 2;
    const size_t chroma_plane_bytes = static_cast<size_t>(native.width_stride) * chroma_rows;
    const size_t required_bytes = luma_bytes + chroma_plane_bytes * (nv12 ? 1 : 2);
    if (required_bytes > native.bytes)
        return Status(COSMO_NN_ERR_INVALID_INPUT,
                      "RKNN native CPU fallback plane layout exceeds DMA-BUF bytes");

    void* mapped = mmap(nullptr, native.bytes, PROT_READ, MAP_SHARED, native.fd, 0);
    if (mapped == MAP_FAILED)
        return Status(COSMO_NN_ERR_NODE_FORWARD, "RKNN native CPU fallback could not map DMA-BUF fd");

    const YuvToBgrCoeffs coeffs = ResolveYuvToBgrCoeffs(native.color_space, native.color_range);
    const int src_w = native.width;
    const int src_h = native.height;
    std::vector<uint8_t> bgr(static_cast<size_t>(src_w) * src_h * 3);
    const auto* luma = static_cast<const uint8_t*>(mapped);
    const auto* chroma_base = luma + luma_bytes;
    const size_t second_chroma_offset =
        nv12 ? 0 : static_cast<size_t>(native.width_stride) * chroma_rows;
    for (int row = 0; row < src_h; ++row) {
        const auto* y_row = luma + static_cast<size_t>(row) * native.width_stride;
        const auto* uv_row = chroma_base + static_cast<size_t>(row / 2) * native.width_stride;
        auto* dst = bgr.data() + static_cast<size_t>(row) * src_w * 3;
        for (int col = 0; col < src_w; ++col) {
            const int y = y_row[col] - coeffs.y_offset;
            const int u = uv_row[(col & ~1)] - 128;
            const int v = nv12 ? uv_row[(col & ~1) + 1] - 128
                               : chroma_base[second_chroma_offset +
                                             static_cast<size_t>(row / 2) * native.width_stride +
                                             col] -
                                     128;
            dst[col * 3 + 0] =
                static_cast<uint8_t>(std::clamp((coeffs.y_scale * y + coeffs.ub * u + 128) >> 8, 0, 255));
            dst[col * 3 + 1] = static_cast<uint8_t>(
                std::clamp((coeffs.y_scale * y + coeffs.ug * u + coeffs.vg * v + 128) >> 8, 0, 255));
            dst[col * 3 + 2] =
                static_cast<uint8_t>(std::clamp((coeffs.y_scale * y + coeffs.vr * v + 128) >> 8, 0, 255));
        }
    }
    munmap(mapped, native.bytes);

    BlobDesc temp_desc     = const_cast<Blob&>(bottom).GetBlobDesc();
    temp_desc.dims         = {1, src_h, src_w, 3};
    temp_desc.image_format = IMAGE_BGR;
    BlobHandle temp_handle;
    temp_handle.base     = bgr.data();
    Blob temp_bottom(temp_desc, temp_handle);
    ResizeWithCpu(temp_bottom, top, detector_contract_);
    return COSMO_NN_OK;
}

Status RknnResizeNode::ResizeSingle(const std::shared_ptr<Blob>& bottom, const std::shared_ptr<Blob>& top,
                                    bool allow_bound_target) {
    const auto& bottom_handle = bottom ? bottom->GetHandle() : BlobHandle{};
    const bool bottom_has_native = bottom_handle.native_image.Valid();
    if (!bottom || !top || !top->GetHandle().base ||
        (!bottom_handle.base && !bottom_handle.native_image.Valid()))
        return Status(COSMO_NN_ERR_NULL_PARAM, "RKNN resize input or output is null");
    const auto bottom_desc = bottom->GetBlobDesc();
    if (bottom_desc.data_type != DATA_TYPE_UINT8 || bottom_desc.data_format != DATA_FORMAT_NHWC ||
        bottom_desc.dims.size() != 4 || bottom_desc.dims[0] != 1 || bottom_desc.dims[1] <= 0 ||
        bottom_desc.dims[2] <= 0 || bottom_desc.dims[3] != 3) {
        return Status(COSMO_NN_ERR_INVALID_INPUT, "RKNN detector resize requires packed batch-1 uint8 input");
    }

    bool rga_success = false;
    if (detector_contract_) {
        rga_success = ResizeWithRga(*bottom, *top, allow_bound_target);
    } else {
        static std::atomic<bool> contract_warned{false};
        if (!contract_warned.exchange(true))
            LOG_WARN("RKNN resize detector contract inactive: out={}x{} gravity={} padding_size={}",
                     out_height_, out_width_, gravity_, padding_color_.size());
    }
    if (!rga_success && bottom_handle.base) {
        const auto cpu_started = MetricsClock::now();
        try {
            ResizeWithCpu(*bottom, *top, detector_contract_);
        } catch (const std::bad_alloc&) {
            return Status(COSMO_NN_ERR_OUT_OF_MEMORY, "RKNN CPU resize fallback allocation failed");
        }
        if (detector_contract_)
            GetInferencePipelineMetrics().RecordRknnCpuResizeFallback(ElapsedNanoseconds(cpu_started));
    } else if (!rga_success && bottom_has_native) {
        const auto cpu_started = MetricsClock::now();
        Status native_status;
        try {
            native_status = ResizeNativeWithCpu(*bottom, *top);
        } catch (const std::bad_alloc&) {
            return Status(COSMO_NN_ERR_OUT_OF_MEMORY, "RKNN native CPU resize fallback allocation failed");
        }
        if (native_status != COSMO_NN_OK)
            return native_status;
        GetInferencePipelineMetrics().RecordRknnMppDmaBufFallback();
    } else if (!rga_success) {
        return Status(COSMO_NN_ERR_NULL_PARAM, "RKNN resize requires a host or native source buffer");
    }

    auto top_desc         = top->GetBlobDesc();
    top_desc.data_format  = DATA_FORMAT_NHWC;
    top_desc.image_format = detector_contract_ ? IMAGE_RGB : bottom_desc.image_format;
    top->SetBlobDesc(top_desc);
    return COSMO_NN_OK;
}

Status RknnResizeNode::Forward(std::vector<std::shared_ptr<Blob>>& bottom_blobs,
                               std::vector<std::shared_ptr<Blob>>& top_blobs) {
    timer.Start();
    if (top_blobs.size() != 1 || !top_blobs[0])
        return Status(COSMO_NN_ERR_INVALID_INPUT, "RKNN resize requires exactly one output");
    const int batch = static_cast<int>(bottom_blobs.size());
    if (batch <= 0 || batch > max_batch)
        return Status(COSMO_NN_ERR_INVALID_INPUT, "RKNN resize batch size is invalid");

    auto top_desc    = top_blobs[0]->GetBlobDesc();
    top_desc.dims[0] = batch;
    top_blobs[0]->SetBlobDesc(top_desc);
    const size_t slice_size = PackedByteCount(out_width_, out_height_);
    if (shared_resource &&
        shared_resource->rknn_bound_input_target.owner == shared_resource->rknn_bound_input_provider) {
        shared_resource->rknn_bound_input_target.frame_ready = false;
    }
    for (int index = 0; index < batch; ++index) {
        BlobDesc slice_desc = top_desc;
        slice_desc.dims[0]  = 1;
        BlobHandle slice_handle;
        slice_handle.base =
            static_cast<uint8_t*>(top_blobs[0]->GetHandle().base) + static_cast<size_t>(index) * slice_size;
        auto slice = std::make_shared<Blob>(slice_desc, slice_handle);
        RETURN_ON_FAIL(ResizeSingle(bottom_blobs[index], slice, batch == 1));
        top_desc.image_format = slice->GetBlobDesc().image_format;
        top_desc.data_format  = slice->GetBlobDesc().data_format;
    }
    top_blobs[0]->SetBlobDesc(top_desc);
    timer.Stop();
    return COSMO_NN_OK;
}

RknnCropResizeNode::RknnCropResizeNode() : CpuCropResizeNode() {
    node_type     = NodeType::NODE_CROP_RESIZE;
    name          = NodeTypeUtils::NodeTypeToStr(NODE_CROP_RESIZE).append("_0");
    one_blob_only = false;
}

RknnCropResizeNode::~RknnCropResizeNode() {
    ReleaseRgaBoundTarget();
}

void RknnCropResizeNode::LoadParam(Op* op) {
    CpuCropResizeNode::LoadParam(op);
    fast_contract_ = dst_height > 0 && dst_width > 0 && !h_top_crop.empty() && !h_bottom_crop.empty() &&
                     !w_left_crop.empty() && !w_right_crop.empty();
}

void RknnCropResizeNode::ReleaseRgaBoundTarget() {
    if (rga_bound_target_handle_ != 0) {
        releasebuffer_handle(static_cast<rga_buffer_handle_t>(rga_bound_target_handle_));
        rga_bound_target_handle_     = 0;
        rga_bound_target_generation_ = 0;
    }
}

void RknnCropResizeNode::InvalidateRgaBoundFrame() {
    if (!shared_resource)
        return;
    auto& target = shared_resource->rknn_bound_input_target;
    if (target.owner == shared_resource->rknn_bound_input_provider)
        target.frame_ready = false;
}

bool RknnCropResizeNode::AcquireRgaBoundTarget(uint32_t& handle) {
    handle                = 0;
    const bool enabled    = RknnRgaBoundInputEnabled();
    const bool compatible = shared_resource && shared_resource->rknn_bound_input_preprocess_compatible;
    const bool provider_available = shared_resource && shared_resource->rknn_bound_input_provider;
    if (!enabled || !compatible || !provider_available || rga_bound_target_unavailable_) {
        if (!rga_bound_target_unavailable_ && !rga_bound_guard_logged_) {
            LOG_WARN(
                "RKNN classifier cannot acquire the RGA-bound input target: enabled={} "
                "shared_resource={} preprocess_compatible={} provider={}",
                enabled, shared_resource != nullptr, compatible, provider_available);
            rga_bound_guard_logged_ = true;
        }
        return false;
    }
    std::string reason;
    auto* provider = shared_resource->rknn_bound_input_provider;
    if (!provider->EnsureRgaBoundInput(dst_height, dst_width, reason)) {
        rga_bound_target_unavailable_ = true;
        LogRgaBoundInputFallbackOnce(reason);
        return false;
    }
    const auto& target = shared_resource->rknn_bound_input_target;
    if (target.owner != provider || !target.Matches(dst_height, dst_width) ||
        target.bytes > static_cast<size_t>(std::numeric_limits<int>::max())) {
        rga_bound_target_unavailable_ = true;
        LogRgaBoundInputFallbackOnce("provider returned an incompatible crop-resize target");
        return false;
    }
    if (rga_bound_target_handle_ != 0 && rga_bound_target_generation_ == target.generation) {
        handle = rga_bound_target_handle_;
        return true;
    }
    ReleaseRgaBoundTarget();
    const auto import_started = MetricsClock::now();
    const auto imported       = importbuffer_fd(target.fd, static_cast<int>(target.bytes));
    GetInferencePipelineMetrics().RecordRknnRgaBoundInputImport(ElapsedNanoseconds(import_started),
                                                                imported != 0);
    if (imported == 0) {
        rga_bound_target_unavailable_ = true;
        GetInferencePipelineMetrics().RecordRknnRgaFailure();
        LogRgaBoundInputFallbackOnce("RGA could not import the classifier RKNN DMA-BUF fd");
        return false;
    }
    rga_bound_target_handle_     = static_cast<uint32_t>(imported);
    rga_bound_target_generation_ = target.generation;
    handle                       = rga_bound_target_handle_;
    return true;
}

bool RknnCropResizeNode::ForwardWithRga(std::vector<std::shared_ptr<Blob>>& image_blobs,
                                        std::vector<std::shared_ptr<Blob>>& rect_blobs,
                                        std::vector<std::shared_ptr<Blob>>& top_blobs) {
    if (!fast_contract_ || RknnForceRgaFailure() || image_blobs.empty() ||
        image_blobs.size() != rect_blobs.size() || top_blobs.size() != 1 || !top_blobs[0] ||
        !top_blobs[0]->GetHandle().base) {
        if (fast_contract_ && RknnForceRgaFailure()) {
            GetInferencePipelineMetrics().RecordRknnRgaCropResize(0, false);
            GetInferencePipelineMetrics().RecordRknnRgaFailure();
        }
        return false;
    }

    int current_batch = 0;
    for (const auto& rect_blob : rect_blobs) {
        if (!rect_blob || rect_blob->GetBlobDesc().dims.size() != 2)
            return false;
        current_batch += rect_blob->GetBlobDesc().dims[0];
    }
    if (current_batch <= 0 || current_batch > max_batch)
        return false;

    auto top_desc    = top_blobs[0]->GetBlobDesc();
    top_desc.dims[0] = current_batch;
    top_blobs[0]->SetBlobDesc(top_desc);
    auto* top_data          = static_cast<uint8_t*>(top_blobs[0]->GetHandle().base);
    const size_t slice_size = PackedByteCount(dst_width, dst_height);
    int processed           = 0;
    bool bound_target       = false;
    uint32_t bound_handle   = 0;
    if (current_batch == 1)
        bound_target = AcquireRgaBoundTarget(bound_handle);

    IM_STATUS last_status = IM_STATUS_FAILED;
    for (size_t image_index = 0; image_index < image_blobs.size(); ++image_index) {
        const auto& image_blob = image_blobs[image_index];
        const bool image_has_host = image_blob && image_blob->GetHandle().base;
        const bool image_has_native = image_blob && image_blob->GetHandle().native_image.Valid();
        if (!image_has_host && !image_has_native)
            return false;
        const auto image_desc   = image_blob->GetBlobDesc();
        const auto image_handle = image_blob->GetHandle();
        if (image_desc.data_type != DATA_TYPE_UINT8 || image_desc.data_format != DATA_FORMAT_NHWC ||
            image_desc.dims.size() != 4 || image_desc.dims[0] != 1 || image_desc.dims[1] <= 0 ||
            image_desc.dims[2] <= 0 || image_desc.dims[3] != 3 ||
            (image_desc.image_format != IMAGE_BGR && image_desc.image_format != IMAGE_RGB)) {
            return false;
        }
        const int source_height = image_desc.dims[1];
        const int source_width  = image_desc.dims[2];
        auto rect_status        = PrepareRect(rect_blobs[image_index], source_width, source_height);
        if (!rect_status)
            return false;

        const auto& native           = image_handle.native_image;
        const bool native_compatible = RknnMppDmaBufEnabled() && native.Valid() &&
                                       native.width == source_width && native.height == source_height &&
                                       (native.format == IMAGE_NV12 || native.format == IMAGE_I420);
        media::ScopedRgaBufferHandle native_source_handle;
        bool native_source_ready = false;
        int native_source_format = RK_FORMAT_UNKNOWN;
        if (native_compatible && !RknnForceMppDmaBufFailure()) {
            const auto import_started = MetricsClock::now();
            native_source_handle.ImportFd(native.fd, native.bytes);
            native_source_ready = bool(native_source_handle);
            GetInferencePipelineMetrics().RecordRknnMppDmaBufImport(ElapsedNanoseconds(import_started),
                                                                    native_source_ready);
            native_source_format =
                native.format == IMAGE_NV12 ? RK_FORMAT_YCbCr_420_SP : RK_FORMAT_YCbCr_420_P;
        }
        if (native_compatible && !native_source_ready) {
            GetInferencePipelineMetrics().RecordRknnMppDmaBufFallback();
            LogMppDmaBufFallbackOnce(IM_STATUS_OUT_OF_MEMORY);
        }

        media::ScopedRgaBufferHandle host_source_handle;
        const int host_source_format =
            image_desc.image_format == IMAGE_RGB ? RK_FORMAT_RGB_888 : RK_FORMAT_BGR_888;

        const int rect_count = rect_blobs[image_index]->GetBlobDesc().dims[0];
        for (int rect_index = 0; rect_index < rect_count; ++rect_index) {
            const int crop_x      = calculated_rects[4 * rect_index];
            const int crop_y      = calculated_rects[4 * rect_index + 1];
            const int crop_width  = calculated_rects[4 * rect_index + 2];
            const int crop_height = calculated_rects[4 * rect_index + 3];
            if (crop_width <= 0 || crop_height <= 0)
                return false;

            media::ScopedRgaBufferHandle host_target_handle;
            uint32_t target_handle = bound_target ? bound_handle : 0;
            int target_stride      = dst_width;
            if (bound_target) {
                target_stride = shared_resource->rknn_bound_input_target.width_stride;
            } else {
                host_target_handle.ImportVirtual(top_data + static_cast<size_t>(processed) * slice_size,
                                                 slice_size);
                target_handle = host_target_handle.Get();
            }
            if (target_handle == 0) {
                last_status = IM_STATUS_OUT_OF_MEMORY;
                GetInferencePipelineMetrics().RecordRknnRgaCropResize(0, false);
                GetInferencePipelineMetrics().RecordRknnRgaFailure();
                InvalidateRgaBoundFrame();
                LogRgaCropResizeFallbackOnce(last_status);
                return false;
            }

            auto target = wrapbuffer_handle_t(static_cast<rga_buffer_handle_t>(target_handle), dst_width,
                                              dst_height, target_stride, dst_height, RK_FORMAT_RGB_888);
            int resized_width  = dst_width;
            int resized_height = dst_height;
            int offset_x       = 0;
            int offset_y       = 0;
            if (gravity != 0) {
                const float scale = std::min(static_cast<float>(dst_width) / crop_width,
                                             static_cast<float>(dst_height) / crop_height);
                resized_width     = static_cast<int>(crop_width * scale);
                resized_height    = static_cast<int>(crop_height * scale);
                if (gravity == 1) {
                    offset_x = (dst_width - resized_width) / 2;
                    offset_y = (dst_height - resized_height) / 2;
                }
                const uint8_t padding = static_cast<uint8_t>(color.empty() ? 114 : color[0]);
                const auto fill_started = MetricsClock::now();
                bool fill_ok            = true;
                if (!bound_target) {
                    // 与 RknnResizeNode 相同的旧驱动 FILL 限制：
                    // host 目标用 CPU 填充替代 imfill_t。
                    std::memset(top_data + static_cast<size_t>(processed) * slice_size, padding,
                                slice_size);
                } else {
                    const int fill_color = (padding << 16) | (padding << 8) | padding;
                    const im_rect full_target{0, 0, dst_width, dst_height};
                    last_status = imfill_t(target, full_target, fill_color, 1);
                    fill_ok     = media::RockchipRgaSucceeded(last_status);
                }
                GetInferencePipelineMetrics().RecordRknnRgaFill(ElapsedNanoseconds(fill_started));
                if (!fill_ok) {
                    GetInferencePipelineMetrics().RecordRknnRgaCropResize(0, false);
                    GetInferencePipelineMetrics().RecordRknnRgaFailure();
                    InvalidateRgaBoundFrame();
                    LogRgaCropResizeFallbackOnce(last_status);
                    return false;
                }
            }
            if (resized_width <= 0 || resized_height <= 0)
                return false;

            const im_rect target_rect{offset_x, offset_y, resized_width, resized_height};
            const auto run_crop = [&](rga_buffer_t source, const im_rect& source_rect,
                                      IM_COLOR_SPACE_MODE source_color_space) {
                return RunStagedRgaCropResize(source, source_rect, source_color_space, target, target_rect,
                                              last_status);
            };

            bool success = false;
            if (native_source_ready) {
                auto native_source =
                    wrapbuffer_handle_t(native_source_handle.Get(), source_width, source_height,
                                        native.width_stride, native.height_stride, native_source_format);
                const auto native_rect =
                    AlignYuv420Crop(crop_x, crop_y, crop_width, crop_height, source_width, source_height);
                success = native_rect.width > 0 && native_rect.height > 0 &&
                          run_crop(native_source, native_rect, ResolveRgaYuvColorSpace(native));
                if (success) {
                    GetInferencePipelineMetrics().RecordRknnMppDmaBufFrame(native.bytes);
                    GetInferencePipelineMetrics().RecordRknnRgaCropSource(true);
                } else {
                    native_source_ready = false;
                    GetInferencePipelineMetrics().RecordRknnMppDmaBufFallback();
                    GetInferencePipelineMetrics().RecordRknnRgaFailure();
                    LogMppDmaBufFallbackOnce(last_status);
                }
            }

            if (!success) {
                if (!host_source_handle) {
                    host_source_handle.ImportVirtual(image_handle.base,
                                                     PackedByteCount(source_width, source_height));
                }
                if (host_source_handle) {
                    auto host_source =
                        wrapbuffer_handle_t(host_source_handle.Get(), source_width, source_height,
                                            source_width, source_height, host_source_format);
                    const im_rect source_rect{crop_x, crop_y, crop_width, crop_height};
                    success = run_crop(host_source, source_rect, IM_COLOR_SPACE_DEFAULT);
                    if (success)
                        GetInferencePipelineMetrics().RecordRknnRgaCropSource(false);
                } else {
                    last_status = IM_STATUS_OUT_OF_MEMORY;
                    GetInferencePipelineMetrics().RecordRknnRgaCropResize(0, false);
                }
            }
            if (!success) {
                GetInferencePipelineMetrics().RecordRknnRgaFailure();
                InvalidateRgaBoundFrame();
                LogRgaCropResizeFallbackOnce(last_status);
                return false;
            }
            GetInferencePipelineMetrics().RecordRknnPreprocessFastHit();
            ++processed;
        }
    }

    if (bound_target) {
        auto& target = shared_resource->rknn_bound_input_target;
        if (target.owner == shared_resource->rknn_bound_input_provider &&
            target.generation == rga_bound_target_generation_) {
            target.frame_ready = true;
        }
    }
    top_desc.data_format  = DATA_FORMAT_NHWC;
    top_desc.image_format = IMAGE_RGB;
    top_blobs[0]->SetBlobDesc(top_desc);
    return processed == current_batch;
}

Status RknnCropResizeNode::Forward(std::vector<std::shared_ptr<Blob>>& image_blobs,
                                   std::vector<std::shared_ptr<Blob>>& rect_blobs,
                                   std::vector<std::shared_ptr<Blob>>& top_blobs) {
    InvalidateRgaBoundFrame();
    timer.Start();
    if (ForwardWithRga(image_blobs, rect_blobs, top_blobs)) {
        timer.Stop();
        return COSMO_NN_OK;
    }
    InvalidateRgaBoundFrame();
    timer.Stop();
    const auto fallback_started = MetricsClock::now();
    auto status                 = CpuCropResizeNode::Forward(image_blobs, rect_blobs, top_blobs);
    GetInferencePipelineMetrics().RecordRknnCpuCropResizeFallback(ElapsedNanoseconds(fallback_started));
    if (status && !top_blobs.empty() && top_blobs[0] && !image_blobs.empty() && image_blobs[0]) {
        auto fallback_desc         = top_blobs[0]->GetBlobDesc();
        fallback_desc.data_format  = DATA_FORMAT_NHWC;
        fallback_desc.image_format = image_blobs[0]->GetBlobDesc().image_format;
        top_blobs[0]->SetBlobDesc(fallback_desc);
    }
    return status;
}

RknnNormalizeNode::RknnNormalizeNode() : Node() {
    node_type     = NodeType::NODE_NORMALIZE;
    name          = NodeTypeUtils::NodeTypeToStr(NODE_NORMALIZE).append("_0");
    one_blob_only = true;
}

void RknnNormalizeNode::LoadParam(Op* op) {
    const auto* normalize = dynamic_cast<Normalize*>(op);
    if (!normalize)
        return;
    mean_          = normalize->mean;
    std_dev_       = normalize->std;
    uniform_scale_ = normalize->scale;
    is_bgr_        = normalize->is_bgr;
    if (std_dev_.empty()) {
        scale_.assign(mean_.size(), uniform_scale_);
    } else {
        scale_.resize(std_dev_.size());
        std::transform(std_dev_.begin(), std_dev_.end(), scale_.begin(),
                       [](float value) { return 1.0f / value; });
    }
}

DeviceType RknnNormalizeNode::GetTopBlobDeviceType() {
    return DeviceType::DEVICE_NAIVE;
}

bool RknnNormalizeNode::NeedBottomShapesInfered() {
    return true;
}

Status RknnNormalizeNode::InferTopShapesWithBottoms(std::vector<DimsVector> dims,
                                                    std::vector<DataType> types) {
    if (dims.size() != 1 || types.size() != 1 || dims[0].size() != 4)
        return Status(COSMO_NN_ERR_INVALID_INPUT, "RKNN normalize input shape is invalid");
    detector_sized_ = dims[0][1] == kDetectorInputSize && dims[0][2] == kDetectorInputSize && dims[0][3] == 3;
    native_contract_ = types[0] == DATA_TYPE_UINT8 &&
                       IsRknnNativeNormalizeContract(mean_, std_dev_, uniform_scale_, dims[0]);
    if (shared_resource)
        shared_resource->rknn_bound_input_preprocess_compatible = native_contract_ && !is_bgr_;
    if (native_contract_) {
        top_blob_shapes     = {dims[0]};
        top_blob_data_types = {DataType::DATA_TYPE_INT8};
    } else {
        top_blob_shapes     = {{dims[0][0], dims[0][3], dims[0][1], dims[0][2]}};
        top_blob_data_types = {DataType::DATA_TYPE_FLOAT};
    }
    return COSMO_NN_OK;
}

size_t RknnNormalizeNode::GetBottomCount() {
    return 1;
}

size_t RknnNormalizeNode::GetTopCount() {
    return 1;
}

bool RknnNormalizeNode::NeedSwapRedBlue(ImageFormat format) const {
    if (format == IMAGE_BGR || format == IMAGE_BGRA)
        return !is_bgr_;
    if (format == IMAGE_RGB || format == IMAGE_RGBA)
        return is_bgr_;
    return false;
}

bool RknnNormalizeNode::CanBypassBoundInput(const Blob& bottom) const {
    if (!native_contract_ || !shared_resource || !shared_resource->rknn_bound_input_provider) {
        return false;
    }
    const auto& target = shared_resource->rknn_bound_input_target;
    const auto desc    = const_cast<Blob&>(bottom).GetBlobDesc();
    return target.frame_ready && target.owner == shared_resource->rknn_bound_input_provider &&
           desc.data_type == DATA_TYPE_UINT8 && desc.data_format == DATA_FORMAT_NHWC &&
           desc.image_format == IMAGE_RGB && desc.dims.size() == 4 && desc.dims[0] == 1 &&
           target.Matches(desc.dims[1], desc.dims[2]) && !NeedSwapRedBlue(desc.image_format);
}

Status RknnNormalizeNode::ForwardNative(const Blob& bottom, Blob& top) {
    auto& mutable_bottom   = const_cast<Blob&>(bottom);
    const auto bottom_desc = mutable_bottom.GetBlobDesc();
    auto top_desc          = top.GetBlobDesc();
    if (bottom_desc.data_type != DATA_TYPE_UINT8 || bottom_desc.data_format != DATA_FORMAT_NHWC ||
        top_desc.data_type != DATA_TYPE_INT8 || top_desc.data_format != DATA_FORMAT_NHWC) {
        return Status(COSMO_NN_ERR_INVALID_INPUT, "RKNN native preprocessing blob contract mismatch");
    }
    const size_t pixels =
        static_cast<size_t>(bottom_desc.dims[0]) * bottom_desc.dims[1] * bottom_desc.dims[2];
    const auto started = MetricsClock::now();
    MapPackedU8ToNativeInt8(static_cast<const uint8_t*>(mutable_bottom.GetHandle().base),
                            static_cast<int8_t*>(top.GetHandle().base), pixels,
                            NeedSwapRedBlue(bottom_desc.image_format));
    GetInferencePipelineMetrics().RecordRknnNativeInputMap(ElapsedNanoseconds(started));
    GetInferencePipelineMetrics().RecordRknnPreprocessFastHit();
    top_desc.image_format = is_bgr_ ? IMAGE_BGR : IMAGE_RGB;
    top.SetBlobDesc(top_desc);
    return COSMO_NN_OK;
}

Status RknnNormalizeNode::ForwardFloat(const Blob& bottom, Blob& top) {
    auto& mutable_bottom   = const_cast<Blob&>(bottom);
    const auto bottom_desc = mutable_bottom.GetBlobDesc();
    const auto top_desc    = top.GetBlobDesc();
    if (bottom_desc.data_type != DATA_TYPE_UINT8 || top_desc.data_type != DATA_TYPE_FLOAT ||
        top_desc.data_format != DATA_FORMAT_NCHW || mean_.size() < 3 || scale_.size() < 3) {
        return Status(COSMO_NN_ERR_INVALID_INPUT, "RKNN float normalize blob contract mismatch");
    }
    const int batch          = bottom_desc.dims[0];
    const int height         = bottom_desc.dims[1];
    const int width          = bottom_desc.dims[2];
    const int channels       = bottom_desc.dims[3];
    const int plane          = height * width;
    const bool swap_red_blue = NeedSwapRedBlue(bottom_desc.image_format);
    const auto* source       = static_cast<const uint8_t*>(mutable_bottom.GetHandle().base);
    auto* destination        = static_cast<float*>(top.GetHandle().base);
    for (int batch_index = 0; batch_index < batch; ++batch_index) {
        const auto* batch_source = source + batch_index * plane * channels;
        auto* batch_destination  = destination + batch_index * plane * 3;
        for (int pixel = 0; pixel < plane; ++pixel) {
            const int source_offset = pixel * channels;
            const int first_channel = swap_red_blue ? 2 : 0;
            const int third_channel = swap_red_blue ? 0 : 2;
            batch_destination[pixel] =
                (static_cast<float>(batch_source[source_offset + first_channel]) - mean_[0]) * scale_[0];
            batch_destination[plane + pixel] =
                (static_cast<float>(batch_source[source_offset + 1]) - mean_[1]) * scale_[1];
            batch_destination[2 * plane + pixel] =
                (static_cast<float>(batch_source[source_offset + third_channel]) - mean_[2]) * scale_[2];
        }
    }
    return COSMO_NN_OK;
}

Status RknnNormalizeNode::Forward(std::vector<std::shared_ptr<Blob>>& bottom_blobs,
                                  std::vector<std::shared_ptr<Blob>>& top_blobs) {
    timer.Start();
    if (bottom_blobs.size() != 1 || top_blobs.size() != 1 || !bottom_blobs[0] || !top_blobs[0] ||
        !bottom_blobs[0]->GetHandle().base || !top_blobs[0]->GetHandle().base) {
        return Status(COSMO_NN_ERR_INVALID_INPUT,
                      "RKNN normalize requires exactly one valid input and output");
    }
    const auto bottom_desc = bottom_blobs[0]->GetBlobDesc();
    if (bottom_desc.dims.size() != 4 || bottom_desc.dims[0] <= 0 || bottom_desc.dims[0] > max_batch) {
        return Status(COSMO_NN_ERR_INVALID_INPUT, "RKNN normalize batch size is invalid");
    }
    SetCurrentBatch(top_blobs[0], bottom_desc.dims[0]);
    auto runtime_top_desc        = top_blobs[0]->GetBlobDesc();
    runtime_top_desc.data_format = native_contract_ ? DATA_FORMAT_NHWC : DATA_FORMAT_NCHW;
    top_blobs[0]->SetBlobDesc(runtime_top_desc);
    Status status;
    if (native_contract_) {
        if (CanBypassBoundInput(*bottom_blobs[0])) {
            runtime_top_desc.image_format = is_bgr_ ? IMAGE_BGR : IMAGE_RGB;
            top_blobs[0]->SetBlobDesc(runtime_top_desc);
            GetInferencePipelineMetrics().RecordRknnPreprocessFastHit();
            GetInferencePipelineMetrics().RecordRknnRgaBoundInputNormalizeBypass();
            status = COSMO_NN_OK;
        } else {
            if (shared_resource && shared_resource->rknn_bound_input_target.owner ==
                                       shared_resource->rknn_bound_input_provider) {
                shared_resource->rknn_bound_input_target.frame_ready = false;
            }
            status = ForwardNative(*bottom_blobs[0], *top_blobs[0]);
        }
    } else {
        const auto fallback_started = MetricsClock::now();
        status                      = ForwardFloat(*bottom_blobs[0], *top_blobs[0]);
        if (detector_sized_)
            GetInferencePipelineMetrics().RecordRknnCpuNormalizeFallback(
                ElapsedNanoseconds(fallback_started));
    }
    timer.Stop();
    return status;
}

}  // namespace cosmo::nn

#endif
