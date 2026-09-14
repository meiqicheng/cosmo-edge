#pragma once

#include "flow/action/AlgActionBase.h"
#include "flow/overview/OverviewRecordAiRst.h"
#include "infer/AiPlateRecognizerUnify.h"

namespace cosmo {

// Independent action for the new plate_rec_color pipeline. It deliberately does not
// share AiOcr state, queues, or model instances with legacy AA_00011.
class AiPlateRecognition final : public AlgActionBase {
public:
    AiPlateRecognition(const std::string& task_id, ActionNode& action);
    ~AiPlateRecognition() override = default;

    // Publish the post-recognition frame for the live preview overlay. The upstream
    // detector action records the detection stage, so without this the preview shows
    // the box/quads but never the recognized number or colour.
    MsgOverviewMem GetOverviewInfo(const std::string& channelId, const std::string& taskId,
                                   int64_t streamIndex = -1, int64_t from = -1,
                                   int64_t to = -1) override;

protected:
    void HandFrame(AlgDataPtr alg_data) override;

private:
    bool AiSdkInit();
    bool RecognizeTarget(const VideoFramePtr& frame, AiDetectRstEl& target);
    std::string alg_code_;
    AiPlateRecognizerUnifyPtr recognizer_;
    OverviewRecordAiRstPtr overview_rec_;
};

using AiPlateRecognitionPtr = std::shared_ptr<AiPlateRecognition>;

}  // namespace cosmo
