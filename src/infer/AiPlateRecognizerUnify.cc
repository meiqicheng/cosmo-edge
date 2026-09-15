#include "infer/AiPlateRecognizerUnify.h"

#include <exception>
#include <filesystem>

#include "infer/AiComponment.h"
#include "nn/pipeline/model_pipeline.h"

namespace cosmo {

AiPlateRecognizerUnify::AiPlateRecognizerUnify(std::string config_path, std::string model_path)
    : config_path_(std::move(config_path)), model_path_(std::move(model_path)) {}

util::ErrorEnum AiPlateRecognizerUnify::Init() {
    if (config_path_.empty() || model_path_.empty())
        return util::ErrorEnum::MandatoryParamMiss;
    if (!std::filesystem::exists(config_path_) || !std::filesystem::exists(model_path_))
        return util::ErrorEnum::FileNotExist;
    try {
        cosmo::nn::DefaultComponent::Options options;
        options.device_id = 0;
        recognizer_       = std::make_unique<cosmo::nn::DefaultComponent>(options, config_path_, model_path_,
                                                                          GetDeviceType());
    } catch (const std::exception&) {
        recognizer_.reset();
        return util::ErrorEnum::AI_INST_CREATEFAILED;
    }
    initialized_ = true;
    return util::ErrorEnum::Success;
}

util::ErrorEnum AiPlateRecognizerUnify::Recognize(const VideoFramePtr& image, AiPlateDecodedResult& result,
                                                  std::string& error) const {
    if (!initialized_ || !recognizer_) {
        error = "plate recognizer is not initialized";
        return util::ErrorEnum::NotInit;
    }

    auto image_blob = ConvertImageToBlob(image);
    if (!image_blob) {
        error = "failed to create plate input blob";
        return util::ErrorEnum::InvalidParam;
    }
    auto forward_status = recognizer_->Forward({{image_blob}});
    if (!bool(forward_status)) {
        error = forward_status.description();
        return util::ErrorEnum::AI_FORWARD_FAILED;
    }

    std::vector<cosmo::nn::PlateRecognitionResult> outputs;
    auto parse_status = recognizer_->ParsePlateOutput(outputs);
    if (!bool(parse_status) || outputs.empty()) {
        error = parse_status.description();
        return util::ErrorEnum::AI_PARSE_OUTPUT_FAILED;
    }

    result.text         = outputs.front().text;
    result.color        = outputs.front().color;
    result.number_score = outputs.front().number_score;
    result.color_score  = outputs.front().color_score;
    return util::ErrorEnum::Success;
}

util::ErrorEnum AiPlateRecognizerUnify::DecodeRecognition(
    const float* ocr_output, const std::vector<int>& ocr_shape, const float* color_output,
    const std::vector<int>& color_shape, AiPlateDecodedResult& result, std::string& error) const {
    std::vector<int> indices;
    int color_index    = -1;
    float color_score  = 0.0F;
    float number_score = 0.0F;
    if (!AiPlateResultParser::DecodeOcr(ocr_output, ocr_shape, indices, number_score, error) ||
        !AiPlateResultParser::DecodeColor(color_output, color_shape, color_index, color_score, error))
        return util::ErrorEnum::AI_PARSE_OUTPUT_FAILED;
    result              = AiPlateResultParser::Decode(indices, color_index, color_score);
    result.number_score = number_score;
    return util::ErrorEnum::Success;
}

AiKeypointSet AiPlateRecognizerUnify::ToKeypointSet(const AiPlatePoseResult& result) {
    AiKeypointSet set;
    set.kind            = AiKeypointKind::LicensePlate;
    set.coordinateSpace = AiKeypointCoordinateSpace::Pixel;
    set.schema          = "plate4";
    set.points.reserve(result.object.key_points.size());
    for (size_t i = 0; i < result.object.key_points.size(); ++i) {
        const auto& point = result.object.key_points[i];
        const float confidence =
            i < result.object.key_point_confidences.size() ? result.object.key_point_confidences[i] : -1.0F;
        set.points.push_back({point.first, point.second, confidence, -1.0F, static_cast<int>(i)});
    }
    return set;
}

}  // namespace cosmo
