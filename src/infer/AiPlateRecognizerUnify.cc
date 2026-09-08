#include "infer/AiPlateRecognizerUnify.h"

#include <filesystem>
#include <cstring>

namespace cosmo {

AiPlateRecognizerUnify::AiPlateRecognizerUnify(std::string config_path, std::string model_path)
    : config_path_(std::move(config_path)), model_path_(std::move(model_path)) {}

util::ErrorEnum AiPlateRecognizerUnify::Init() {
    if (config_path_.empty() || model_path_.empty())
        return util::ErrorEnum::MandatoryParamMiss;
    if (!std::filesystem::exists(config_path_) || !std::filesystem::exists(model_path_))
        return util::ErrorEnum::FileNotExist;
#ifdef COSMO_NN_USE_ONNX_BACKEND
    try {
        ort_env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "CosmoPlateRecognizer");
        session_options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        recognizer_session_ = std::make_unique<Ort::Session>(*ort_env_, model_path_.c_str(), session_options_);
        if (recognizer_session_->GetInputCount() != 1 || recognizer_session_->GetOutputCount() != 2)
            return util::ErrorEnum::FileAnalysisFailed;
    } catch (const Ort::Exception&) {
        recognizer_session_.reset();
        ort_env_.reset();
        return util::ErrorEnum::AI_INST_CREATEFAILED;
    }
#endif
    initialized_ = true;
    return util::ErrorEnum::Success;
}

util::ErrorEnum AiPlateRecognizerUnify::RunRecognizerTensor(const std::vector<float>& input,
                                                            const std::vector<int64_t>& input_shape,
                                                            std::vector<float>& ocr_output,
                                                            std::vector<int64_t>& ocr_shape,
                                                            std::vector<float>& color_output,
                                                            std::vector<int64_t>& color_shape,
                                                            std::string& error) const {
#ifndef COSMO_NN_USE_ONNX_BACKEND
    error = "ONNX backend is not enabled";
    return util::ErrorEnum::NotSupport;
#else
    if (!initialized_ || !recognizer_session_)
        return util::ErrorEnum::NotInit;
    try {
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float>(memory, const_cast<float*>(input.data()), input.size(),
                                                       input_shape.data(), input_shape.size());
        Ort::AllocatorWithDefaultOptions allocator;
        std::vector<const char*> names;
        std::vector<Ort::AllocatedStringPtr> owned;
        for (size_t i = 0; i < recognizer_session_->GetOutputCount(); ++i) {
            owned.push_back(recognizer_session_->GetOutputNameAllocated(i, allocator));
            names.push_back(owned.back().get());
        }
        auto input_name = recognizer_session_->GetInputNameAllocated(0, allocator);
        const char* input_name_ptr = input_name.get();
        auto outputs = recognizer_session_->Run(Ort::RunOptions{nullptr}, &input_name_ptr, &tensor, 1,
                                                names.data(), names.size());
        if (outputs.size() != 2)
            return util::ErrorEnum::AI_PARSE_OUTPUT_FAILED;
        auto copy = [](Ort::Value& value, std::vector<float>& data, std::vector<int64_t>& shape) {
            auto info = value.GetTensorTypeAndShapeInfo();
            shape = info.GetShape();
            const float* ptr = value.GetTensorData<float>();
            data.assign(ptr, ptr + info.GetElementCount());
        };
        copy(outputs[0], ocr_output, ocr_shape);
        copy(outputs[1], color_output, color_shape);
        return util::ErrorEnum::Success;
    } catch (const Ort::Exception& exception) {
        error = exception.what();
        return util::ErrorEnum::AI_FORWARD_FAILED;
    }
#endif
}

util::ErrorEnum AiPlateRecognizerUnify::DecodeRecognition(const float* ocr_output,
                                                          const std::vector<int>& ocr_shape,
                                                          const float* color_output,
                                                          const std::vector<int>& color_shape,
                                                          AiPlateDecodedResult& result,
                                                          std::string& error) const {
    if (!initialized_)
        return util::ErrorEnum::NotInit;
    std::vector<int> indices;
    int color_index = -1;
    float color_score = 0.0F;
    float number_score = 0.0F;
    if (!AiPlateResultParser::DecodeOcr(ocr_output, ocr_shape, indices, number_score, error) ||
        !AiPlateResultParser::DecodeColor(color_output, color_shape, color_index, color_score, error))
        return util::ErrorEnum::AI_PARSE_OUTPUT_FAILED;
    result = AiPlateResultParser::Decode(indices, color_index, color_score);
    result.number_score = number_score;
    return util::ErrorEnum::Success;
}

AiKeypointSet AiPlateRecognizerUnify::ToKeypointSet(const AiPlatePoseResult& result) {
    AiKeypointSet set;
    set.kind = AiKeypointKind::LicensePlate;
    set.coordinateSpace = AiKeypointCoordinateSpace::Pixel;
    set.schema = "plate4";
    set.points.reserve(result.object.key_points.size());
    for (size_t i = 0; i < result.object.key_points.size(); ++i) {
        const auto& point = result.object.key_points[i];
        const float confidence = i < result.object.key_point_confidences.size()
                                     ? result.object.key_point_confidences[i]
                                     : -1.0F;
        set.points.push_back({point.first, point.second, confidence, -1.0F, static_cast<int>(i)});
    }
    return set;
}

}  // namespace cosmo
