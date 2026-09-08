#pragma once

#include <string>
#include <vector>

#include "nn/utils/net_utils.h"

namespace cosmo {

struct AiPlateDecodedResult {
    std::string text;
    std::string color;
    float color_score{0.0F};
    float number_score{0.0F};
};

struct AiPlatePoseResult {
    nn::ObjectInfoV1 object;
    int plate_type{0};
};

class AiPlateResultParser {
public:
    static std::string DecodeCtc(const std::vector<int>& indices);
    static const char* ColorName(int index);
    static AiPlateDecodedResult Decode(const std::vector<int>& indices, int color_index,
                                       float color_score);
    static bool DecodePose(const float* data, const std::vector<int>& shape, int image_width, int image_height,
                           float threshold, std::vector<AiPlatePoseResult>& results, std::string& error);
    static bool DecodeOcr(const float* logits, const std::vector<int>& shape, std::vector<int>& indices,
                          float& number_score, std::string& error);
    static bool DecodeColor(const float* logits, const std::vector<int>& shape, int& color_index,
                            float& color_score, std::string& error);
};

}  // namespace cosmo
