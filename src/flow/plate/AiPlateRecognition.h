#pragma once

#include "flow/action/AlgActionBase.h"
#include "infer/AiPlateRecognizerUnify.h"

namespace cosmo {

// Independent action for the new plate_rec_color pipeline. It deliberately does not
// share AiOcr state, queues, or model instances with legacy AA_00011.
class AiPlateRecognition final : public AlgActionBase {
public:
    AiPlateRecognition(const std::string& task_id, ActionNode& action);
    ~AiPlateRecognition() override = default;

protected:
    void HandFrame(AlgDataPtr alg_data) override;

private:
    bool AiSdkInit();
    bool RecognizeTarget(const VideoFramePtr& frame, AiDetectRstEl& target);
    std::string alg_code_;
    AiPlateRecognizerUnifyPtr recognizer_;
};

using AiPlateRecognitionPtr = std::shared_ptr<AiPlateRecognition>;

}  // namespace cosmo
