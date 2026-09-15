#include "flow/plate/AiPlateRecognition.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "flow/common/AlgDataUnit.h"
#include "service/detail/ServiceRegistry.h"
#include "service/media/IVideoFrameTransform.h"
#include "service/model/IModelService.h"
#include "util/Log.h"

namespace cosmo {
namespace {

    constexpr int kPlateInputWidth  = 168;
    constexpr int kPlateInputHeight = 48;

    struct PlateProjection {
        double a{0.0};
        double b{0.0};
        double c{0.0};
        double d{0.0};
        double e{0.0};
        double f{0.0};
        double g{0.0};
        double h{0.0};
        bool valid{false};
    };

    PlateProjection BuildPlateProjection(const std::array<util::Point, 4>& quad) {
        const double x0 = quad[0].x;
        const double y0 = quad[0].y;
        const double x1 = quad[1].x;
        const double y1 = quad[1].y;
        const double x2 = quad[2].x;
        const double y2 = quad[2].y;
        const double x3 = quad[3].x;
        const double y3 = quad[3].y;

        const double dx1 = x1 - x2;
        const double dx2 = x3 - x2;
        const double dx3 = x0 - x1 + x2 - x3;
        const double dy1 = y1 - y2;
        const double dy2 = y3 - y2;
        const double dy3 = y0 - y1 + y2 - y3;

        PlateProjection projection;
        if (std::abs(dx3) < 1e-6 && std::abs(dy3) < 1e-6) {
            projection.g = 0.0;
            projection.h = 0.0;
        } else {
            const double denominator = dx1 * dy2 - dx2 * dy1;
            if (std::abs(denominator) < 1e-6) {
                return projection;
            }
            projection.g = (dx3 * dy2 - dx2 * dy3) / denominator;
            projection.h = (dx1 * dy3 - dx3 * dy1) / denominator;
        }

        projection.a     = x1 - x0 + projection.g * x1;
        projection.b     = x3 - x0 + projection.h * x3;
        projection.c     = x0;
        projection.d     = y1 - y0 + projection.g * y1;
        projection.e     = y3 - y0 + projection.h * y3;
        projection.f     = y0;
        projection.valid = true;
        return projection;
    }

    uint8_t SampleBilinear(const uint8_t* src, int width, int height, int channels, double x, double y,
                           int channel) {
        x = std::clamp(x, 0.0, static_cast<double>(width - 1));
        y = std::clamp(y, 0.0, static_cast<double>(height - 1));

        const int x0    = static_cast<int>(std::floor(x));
        const int y0    = static_cast<int>(std::floor(y));
        const int x1    = std::min(x0 + 1, width - 1);
        const int y1    = std::min(y0 + 1, height - 1);
        const double fx = x - x0;
        const double fy = y - y0;

        const double top = src[(y0 * width + x0) * channels + channel] * (1.0 - fx) +
                           src[(y0 * width + x1) * channels + channel] * fx;
        const double bottom = src[(y1 * width + x0) * channels + channel] * (1.0 - fx) +
                              src[(y1 * width + x1) * channels + channel] * fx;
        return static_cast<uint8_t>(std::clamp(top * (1.0 - fy) + bottom * fy, 0.0, 255.0));
    }

    bool WarpPlateQuad(const uint8_t* src, int srcWidth, int srcHeight,
                       const std::array<util::Point, 4>& quad, std::vector<uint8_t>& dst) {
        const auto projection = BuildPlateProjection(quad);
        if (!projection.valid) {
            return false;
        }

        dst.resize(static_cast<size_t>(kPlateInputWidth) * kPlateInputHeight * 3U);
        for (int y = 0; y < kPlateInputHeight; ++y) {
            const double v = kPlateInputHeight > 1
                                 ? static_cast<double>(y) / static_cast<double>(kPlateInputHeight - 1)
                                 : 0.0;
            for (int x = 0; x < kPlateInputWidth; ++x) {
                const double u           = kPlateInputWidth > 1
                                               ? static_cast<double>(x) / static_cast<double>(kPlateInputWidth - 1)
                                               : 0.0;
                const double denominator = projection.g * u + projection.h * v + 1.0;
                if (std::abs(denominator) < 1e-9) {
                    return false;
                }

                const double srcX = (projection.a * u + projection.b * v + projection.c) / denominator;
                const double srcY = (projection.d * u + projection.e * v + projection.f) / denominator;
                auto* pixel       = dst.data() + (static_cast<size_t>(y) * kPlateInputWidth + x) * 3U;
                for (int channel = 0; channel < 3; ++channel) {
                    pixel[channel] = SampleBilinear(src, srcWidth, srcHeight, 3, srcX, srcY, channel);
                }
            }
        }
        return true;
    }

}  // namespace

AiPlateRecognition::AiPlateRecognition(const std::string& task_id, ActionNode& action)
    : AlgActionBase(AlgActionType::AlgActionAiPlateRecognition, action, "", task_id,
                    action.atomicCode + " AiPlateRecognition") {
    alg_code_     = action.atomicCode.empty() ? action.atomAlgName : action.atomicCode;
    action_status = util::ErrorEnum::ActionReady;
}

bool AiPlateRecognition::AiSdkInit() {
    if (recognizer_)
        return true;

    std::string cfg_path;
    std::string model_path;
    std::string dict_path;
    if (!service::ServiceRegistry::Instance().Get<service::IModelService>().GetModelCfg(
            alg_code_, cfg_path, model_path, dict_path)) {
        LOG_WARN("[AiPlateRecognition] model configuration not found for {}", alg_code_);
        action_status = util::ErrorEnum::AI_INST_NOTCREATED;
        return false;
    }

    auto instance  = std::make_shared<AiPlateRecognizerUnify>(cfg_path, model_path);
    const auto ret = instance->Init();
    if (ret != util::ErrorEnum::Success) {
        LOG_WARN("[AiPlateRecognition] model init failed for {}: {}", alg_code_, ret);
        action_status = ret;
        return false;
    }
    recognizer_   = std::move(instance);
    action_status = util::ErrorEnum::AI_INST_CREATED;
    return true;
}

bool AiPlateRecognition::RecognizeTarget(const VideoFramePtr& frame, AiDetectRstEl& target) {
    if (!recognizer_ || !frame || !frame->Active())
        return false;
    const int x = std::max(0, target.box.x);
    const int y = std::max(0, target.box.y);
    const int w = std::min(target.box.width, static_cast<int>(frame->GetWidth()) - x);
    const int h = std::min(target.box.height, static_cast<int>(frame->GetHeight()) - y);
    if (w <= 0 || h <= 0)
        return false;
    auto& transform = service::ServiceRegistry::Instance().Get<service::IVideoFrameTransform>();

    VideoFramePtr resized;
    const auto& landmarks = target.landmark.landmark;
    if (landmarks.size() == 4) {
        VideoFramePtr packed   = frame;
        const auto pixelFormat = frame->GetPixelFormat();
        if (pixelFormat == media::PixelFormat::PIXEL_I420) {
            packed = transform.I4202BGR(frame);
        } else if (pixelFormat != media::PixelFormat::PIXEL_BGR8 &&
                   pixelFormat != media::PixelFormat::PIXEL_RGB8) {
            packed.reset();
        }

        if (packed && transform.EnsureHostData(packed) && packed->GetData()) {
            const std::array<util::Point, 4> quad = {landmarks[0], landmarks[1], landmarks[2], landmarks[3]};
            std::vector<uint8_t> warpedData;
            if (WarpPlateQuad(packed->GetData(), static_cast<int>(packed->GetWidth()),
                              static_cast<int>(packed->GetHeight()), quad, warpedData)) {
                resized = std::make_shared<media::VideoFrame>(kPlateInputWidth, kPlateInputHeight,
                                                              packed->GetPixelFormat(),
                                                              frame->GetFrameIndex(), frame->GetTimestamp());
                if (resized && resized->Active() && resized->GetData()) {
                    std::copy(warpedData.begin(), warpedData.end(), resized->GetData());
                    resized->SetStreamIndex(frame->GetStreamIndex());
                } else {
                    resized.reset();
                }
            }
        }
    }

    if (!resized) {
        auto crop = transform.Crop(frame, util::Box(x, y, w, h));
        resized   = crop ? transform.Resize(crop, kPlateInputHeight, kPlateInputWidth) : nullptr;
    }
    if (!resized || !transform.EnsureHostData(resized) || !resized->GetData())
        return false;
    AiPlateDecodedResult decoded;
    std::string error;
    if (recognizer_->Recognize(resized, decoded, error) != util::ErrorEnum::Success) {
        LOG_WARN("[AiPlateRecognition] inference failed: {}", error);
        return false;
    }
    if (!decoded.text.empty())
        target.ocrRst.push_back({alg_code_, decoded.text, decoded.number_score});
    if (decoded.color != "unknown")
        target.attrRst.push_back({"plateColor", decoded.color, alg_code_, decoded.color_score});
    return true;
}

void AiPlateRecognition::HandFrame(AlgDataPtr alg_data) {
    if (!alg_data || !alg_data->chanDataDec.frame || !alg_data->chanDataDec.frame->Active()) {
        action_status = util::ErrorEnum::FrameDataInvalid;
        return;
    }

    if (!AiSdkInit())
        return;

    auto output = AlgDataCopy(alg_data);
    if (!output)
        return;
    if (output->chanDataDetect.detRet) {
        for (auto& target : output->chanDataDetect.detRet->targets)
            RecognizeTarget(output->chanDataDec.frame, target);
        // Record the recognized frame so the live overlay can show the plate number and
        // colour. The detector published the pre-recognition frame, so this stage is the
        // only place that carries ocrString/attrs to the preview cache.
        if (!overview_rec_)
            overview_rec_ = std::make_shared<OverviewRecordAiRst>(task_id, "plate_rec_" + alg_code_);
        overview_rec_->OverviewRecordFrame(output->chanDataDetect.detRet);
    }
    output->dataType = AlgDataType::ChannelDataDetect;
    action_status    = util::ErrorEnum::Success;
    distributor->DistributorData(output);
}

MsgOverviewMem AiPlateRecognition::GetOverviewInfo(const std::string& /*channelId*/,
                                                   const std::string& /*taskId*/, int64_t streamIndex,
                                                   int64_t from, int64_t to) {
    MsgOverviewMem info;
    info.type = MsgOverviewMemDataType::MsgOverviewMemDataTypeNone;
    if (overview_rec_) {
        info = overview_rec_->GetOverviewInfo(streamIndex, from, to);
    }
    return info;
}

}  // namespace cosmo
