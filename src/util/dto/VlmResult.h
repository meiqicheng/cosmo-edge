#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace cosmo {

// An answer is evidence even when its business decision is negative or unknown.
struct VlmResult {
    std::string nodeId;
    std::string targetId;
    std::string modelCode;
    std::string question;
    std::string answer;
    std::string decision;
    double durationMs{0};
    std::string frameIndex;
    std::string timestamp;
    size_t batchSize{1};
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(VlmResult, nodeId, targetId, modelCode, question, answer,
                                                decision, durationMs, frameIndex, timestamp, batchSize)

}  // namespace cosmo
