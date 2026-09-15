#pragma once

#include <memory>
#include <string>
#include <vector>

#include "infer/AiPlateResultParser.h"
#include "media/VideoFrame.h"
#include "nn/utils/default_component.h"
#include "util/AiTypes.h"
#include "util/ErrorCode.h"

namespace cosmo {

class AiPlateRecognizerUnify {
public:
    AiPlateRecognizerUnify(std::string config_path, std::string model_path);
    ~AiPlateRecognizerUnify() = default;

    AiPlateRecognizerUnify(const AiPlateRecognizerUnify&)            = delete;
    AiPlateRecognizerUnify& operator=(const AiPlateRecognizerUnify&) = delete;

    util::ErrorEnum Init();
    bool IsInitialized() const {
        return initialized_;
    }

    util::ErrorEnum DecodeRecognition(const float* ocr_output, const std::vector<int>& ocr_shape,
                                      const float* color_output, const std::vector<int>& color_shape,
                                      AiPlateDecodedResult& result, std::string& error) const;

    util::ErrorEnum Recognize(const VideoFramePtr& image, AiPlateDecodedResult& result,
                              std::string& error) const;

    static AiKeypointSet ToKeypointSet(const AiPlatePoseResult& result);

    const std::string& config_path() const {
        return config_path_;
    }
    const std::string& model_path() const {
        return model_path_;
    }

private:
    std::string config_path_;
    std::string model_path_;
    bool initialized_{false};
    std::unique_ptr<cosmo::nn::DefaultComponent> recognizer_;
};

using AiPlateRecognizerUnifyPtr = std::shared_ptr<AiPlateRecognizerUnify>;

}  // namespace cosmo
