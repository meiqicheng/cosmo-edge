#pragma once

#include <memory>
#include <string>
#include <vector>

#include "infer/AiPlateResultParser.h"
#include "util/ErrorCode.h"

#ifdef COSMO_NN_USE_ONNX_BACKEND
#include <onnxruntime_cxx_api.h>
#endif

namespace cosmo {

class AiPlateDetectorUnify {
public:
    AiPlateDetectorUnify(std::string config_path, std::string model_path);
    util::ErrorEnum Init();
    bool IsInitialized() const { return initialized_; }
    util::ErrorEnum Run(const std::vector<float>& input, const std::vector<int64_t>& input_shape,
                        std::vector<AiPlatePoseResult>& results, int image_width, int image_height,
                        float threshold, std::string& error) const;

private:
    std::string config_path_;
    std::string model_path_;
    bool initialized_{false};
#ifdef COSMO_NN_USE_ONNX_BACKEND
    std::unique_ptr<Ort::Env> ort_env_;
    std::unique_ptr<Ort::Session> session_;
    Ort::SessionOptions options_;
#endif
};

using AiPlateDetectorUnifyPtr = std::shared_ptr<AiPlateDetectorUnify>;

}  // namespace cosmo
