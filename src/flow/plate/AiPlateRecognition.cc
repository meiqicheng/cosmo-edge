#include "flow/plate/AiPlateRecognition.h"

#include <algorithm>

#include "flow/common/AlgDataUnit.h"
#include "service/detail/ServiceRegistry.h"
#include "service/model/IModelService.h"
#include "service/media/IVideoFrameTransform.h"
#include "util/Log.h"

namespace cosmo {

AiPlateRecognition::AiPlateRecognition(const std::string& task_id, ActionNode& action)
    : AlgActionBase(AlgActionType::AlgActionAiPlateRecognition, action, "", task_id,
                    action.atomicCode + " AiPlateRecognition") {
    alg_code_ = action.atomicCode.empty() ? action.atomAlgName : action.atomicCode;
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

    auto instance = std::make_shared<AiPlateRecognizerUnify>(cfg_path, model_path);
    const auto ret = instance->Init();
    if (ret != util::ErrorEnum::Success) {
        LOG_WARN("[AiPlateRecognition] model init failed for {}: {}", alg_code_, ret);
        action_status = ret;
        return false;
    }
    recognizer_ = std::move(instance);
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
    auto crop = transform.Crop(frame, util::Box(x, y, w, h));
    auto resized = crop ? transform.Resize(crop, 48, 168) : nullptr;
    if (!resized || !transform.EnsureHostData(resized) || !resized->GetData())
        return false;
    const auto* src = resized->GetData();
    const size_t pixels = 168U * 48U;
    std::vector<float> input(pixels * 3U);
    for (size_t i = 0; i < pixels; ++i) {
        input[i] = (static_cast<float>(src[i * 3U]) / 255.0F - 0.588F) / 0.193F;
        input[pixels + i] = (static_cast<float>(src[i * 3U + 1U]) / 255.0F - 0.588F) / 0.193F;
        input[pixels * 2U + i] = (static_cast<float>(src[i * 3U + 2U]) / 255.0F - 0.588F) / 0.193F;
    }
    std::vector<float> ocr, color;
    std::vector<int64_t> ocr_shape, color_shape;
    std::string error;
    if (recognizer_->RunRecognizerTensor(input, {1, 3, 48, 168}, ocr, ocr_shape, color, color_shape, error) !=
            util::ErrorEnum::Success ||
        ocr_shape.size() != 3 || color_shape.size() != 2) {
        LOG_WARN("[AiPlateRecognition] inference failed: {}", error);
        return false;
    }
    std::vector<int> ocr_dims(ocr_shape.begin(), ocr_shape.end());
    std::vector<int> color_dims(color_shape.begin(), color_shape.end());
    AiPlateDecodedResult decoded;
    if (recognizer_->DecodeRecognition(ocr.data(), ocr_dims, color.data(), color_dims, decoded, error) !=
        util::ErrorEnum::Success)
        return false;
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
    }
    output->dataType = AlgDataType::ChannelDataDetect;
    action_status = util::ErrorEnum::Success;
    distributor->DistributorData(output);
}

}  // namespace cosmo
