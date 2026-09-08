#pragma once

#include <memory>
#include <string>
#include <vector>

#include "infer/AiPlateResultParser.h"
#include "util/AiTypes.h"
#include "util/ErrorCode.h"

#ifdef COSMO_NN_USE_ONNX_BACKEND
#include <onnxruntime_cxx_api.h>
#endif

namespace cosmo {

class AiPlateRecognizerUnify {
public:
    AiPlateRecognizerUnify(std::string config_path, std::string model_path);
    ~AiPlateRecognizerUnify() = default;

    AiPlateRecognizerUnify(const AiPlateRecognizerUnify&) = delete;
    AiPlateRecognizerUnify& operator=(const AiPlateRecognizerUnify&) = delete;

    util::ErrorEnum Init();
    bool IsInitialized() const { return initialized_; }

    util::ErrorEnum DecodeRecognition(const float* ocr_output, const std::vector<int>& ocr_shape,
                                      const float* color_output, const std::vector<int>& color_shape,
                                      AiPlateDecodedResult& result, std::string& error) const;

    util::ErrorEnum RunRecognizerTensor(const std::vector<float>& input, const std::vector<int64_t>& input_shape,
                                        std::vector<float>& ocr_output, std::vector<int64_t>& ocr_shape,
                                        std::vector<float>& color_output, std::vector<int64_t>& color_shape,
                                        std::string& error) const;

    static AiKeypointSet ToKeypointSet(const AiPlatePoseResult& result);

    const std::string& config_path() const { return config_path_; }
    const std::string& model_path() const { return model_path_; }

private:
    std::string config_path_;
    std::string model_path_;
    bool initialized_{false};
#ifdef COSMO_NN_USE_ONNX_BACKEND
    std::unique_ptr<Ort::Env> ort_env_;
    std::unique_ptr<Ort::Session> recognizer_session_;
    Ort::SessionOptions session_options_;
#endif
};

using AiPlateRecognizerUnifyPtr = std::shared_ptr<AiPlateRecognizerUnify>;

}  // namespace cosmo
