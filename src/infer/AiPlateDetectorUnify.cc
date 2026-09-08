#include "infer/AiPlateDetectorUnify.h"

#include <filesystem>

namespace cosmo {

AiPlateDetectorUnify::AiPlateDetectorUnify(std::string config_path, std::string model_path)
    : config_path_(std::move(config_path)), model_path_(std::move(model_path)) {}

util::ErrorEnum AiPlateDetectorUnify::Init() {
    if (config_path_.empty() || model_path_.empty())
        return util::ErrorEnum::MandatoryParamMiss;
    if (!std::filesystem::exists(config_path_) || !std::filesystem::exists(model_path_))
        return util::ErrorEnum::FileNotExist;
#ifdef COSMO_NN_USE_ONNX_BACKEND
    try {
        ort_env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "CosmoPlateDetector");
        options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session_ = std::make_unique<Ort::Session>(*ort_env_, model_path_.c_str(), options_);
        if (session_->GetInputCount() != 1 || session_->GetOutputCount() != 1)
            return util::ErrorEnum::FileAnalysisFailed;
    } catch (const Ort::Exception&) {
        session_.reset();
        ort_env_.reset();
        return util::ErrorEnum::AI_INST_CREATEFAILED;
    }
#endif
    initialized_ = true;
    return util::ErrorEnum::Success;
}

util::ErrorEnum AiPlateDetectorUnify::Run(const std::vector<float>& input,
                                          const std::vector<int64_t>& input_shape,
                                          std::vector<AiPlatePoseResult>& results, const int image_width,
                                          const int image_height, const float threshold,
                                          std::string& error) const {
#ifndef COSMO_NN_USE_ONNX_BACKEND
    (void)input; (void)input_shape; (void)results; (void)image_width; (void)image_height; (void)threshold;
    error = "ONNX backend is not enabled";
    return util::ErrorEnum::NotSupport;
#else
    if (!initialized_ || !session_)
        return util::ErrorEnum::NotInit;
    try {
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float>(memory, const_cast<float*>(input.data()), input.size(),
                                                       input_shape.data(), input_shape.size());
        Ort::AllocatorWithDefaultOptions allocator;
        auto name = session_->GetInputNameAllocated(0, allocator);
        const char* input_name = name.get();
        auto outputs = session_->Run(Ort::RunOptions{nullptr}, &input_name, &tensor, 1, nullptr, 0);
        if (outputs.size() != 1)
            return util::ErrorEnum::AI_PARSE_OUTPUT_FAILED;
        auto info = outputs[0].GetTensorTypeAndShapeInfo();
        std::vector<int> shape;
        for (const auto dim : info.GetShape())
            shape.push_back(static_cast<int>(dim));
        return AiPlateResultParser::DecodePose(outputs[0].GetTensorData<float>(), shape, image_width,
                                               image_height, threshold, results, error)
                   ? util::ErrorEnum::Success
                   : util::ErrorEnum::AI_PARSE_OUTPUT_FAILED;
    } catch (const Ort::Exception& exception) {
        error = exception.what();
        return util::ErrorEnum::AI_FORWARD_FAILED;
    }
#endif
}

}  // namespace cosmo
