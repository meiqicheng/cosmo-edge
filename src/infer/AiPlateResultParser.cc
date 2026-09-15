#include "infer/AiPlateResultParser.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace cosmo {
namespace {

    constexpr const char* kPlateChars[] = {
        "#",  "京", "沪", "津", "渝", "冀", "晋", "蒙", "辽", "吉", "黑", "苏", "浙", "皖", "闽", "赣",
        "鲁", "豫", "鄂", "湘", "粤", "桂", "琼", "川", "贵", "云", "藏", "陕", "甘", "青", "宁", "新",
        "学", "警", "港", "澳", "挂", "使", "领", "民", "航", "危", "0",  "1",  "2",  "3",  "4",  "5",
        "6",  "7",  "8",  "9",  "A",  "B",  "C",  "D",  "E",  "F",  "G",  "H",  "J",  "K",  "L",  "M",
        "N",  "P",  "Q",  "R",  "S",  "T",  "U",  "V",  "W",  "X",  "Y",  "Z",  "险", "品"};

    constexpr const char* kColors[] = {"black", "blue", "green", "white", "yellow"};

}  // namespace

std::string AiPlateResultParser::DecodeCtc(const std::vector<int>& indices) {
    std::string result;
    int previous = 0;
    for (const int index : indices) {
        if (index > 0 && index < static_cast<int>(std::size(kPlateChars)) && index != previous) {
            result += kPlateChars[index];
        }
        previous = index;
    }
    return result;
}

const char* AiPlateResultParser::ColorName(const int index) {
    if (index < 0 || index >= static_cast<int>(std::size(kColors))) {
        return "unknown";
    }
    return kColors[index];
}

AiPlateDecodedResult AiPlateResultParser::Decode(const std::vector<int>& indices, const int color_index,
                                                 const float color_score) {
    return {DecodeCtc(indices), ColorName(color_index), color_score};
}

bool AiPlateResultParser::DecodePose(const float* data, const std::vector<int>& shape, const int image_width,
                                     const int image_height, const float threshold,
                                     std::vector<AiPlatePoseResult>& results, std::string& error) {
    results.clear();
    if (!data || image_width <= 0 || image_height <= 0 || shape.size() != 3 || shape[0] != 1 ||
        shape[2] != 14 || !std::isfinite(threshold)) {
        error = "plate pose output must be [1,N,14]";
        return false;
    }
    const int count = shape[1];
    for (int row = 0; row < count; ++row) {
        const float* value = data + static_cast<size_t>(row) * 14U;
        if (!std::isfinite(value[4]) || value[4] < threshold)
            continue;
        if (!std::isfinite(value[0]) || !std::isfinite(value[1]) || !std::isfinite(value[2]) ||
            !std::isfinite(value[3]) || value[2] <= value[0] || value[3] <= value[1])
            continue;
        AiPlatePoseResult result;
        result.plate_type = static_cast<int>(value[5]);
        result.object.x1  = std::clamp(value[0], 0.0F, static_cast<float>(image_width - 1));
        result.object.y1  = std::clamp(value[1], 0.0F, static_cast<float>(image_height - 1));
        result.object.x2  = std::clamp(value[2], 0.0F, static_cast<float>(image_width - 1));
        result.object.y2  = std::clamp(value[3], 0.0F, static_cast<float>(image_height - 1));
        result.object.key_point_confidences.assign(4, -1.0F);
        for (int point = 0; point < 4; ++point) {
            const float x = value[6 + point * 2];
            const float y = value[7 + point * 2];
            if (!std::isfinite(x) || !std::isfinite(y)) {
                error = "plate pose contains non-finite keypoint";
                return false;
            }
            result.object.key_points.emplace_back(std::clamp(x, 0.0F, static_cast<float>(image_width - 1)),
                                                  std::clamp(y, 0.0F, static_cast<float>(image_height - 1)));
        }
        result.object.infos.push_back(
            {value[4], result.plate_type, result.plate_type == 1 ? "double" : "single"});
        results.push_back(std::move(result));
    }
    return true;
}

bool AiPlateResultParser::DecodeOcr(const float* logits, const std::vector<int>& shape,
                                    std::vector<int>& indices, float& number_score, std::string& error) {
    indices.clear();
    number_score = 0.0F;
    if (!logits || shape.size() != 3 || shape[0] != 1 || shape[2] != 78) {
        error = "plate OCR output must be [1,21,78] or [1,T,78]";
        return false;
    }
    float score_sum = 0.0F;
    int score_count = 0;
    for (int step = 0; step < shape[1]; ++step) {
        const float* row = logits + static_cast<size_t>(step) * 78U;
        for (int cls = 0; cls < 78; ++cls) {
            if (!std::isfinite(row[cls])) {
                error = "plate OCR output contains non-finite value";
                return false;
            }
        }
        int best = 0;
        for (int cls = 1; cls < 78; ++cls) {
            if (row[cls] > row[best])
                best = cls;
        }
        indices.push_back(best);
        if (best != 0) {
            // softmax probability of the argmax class at this step (numerically stable)
            const float max_value = row[best];
            float sum             = 0.0F;
            for (int cls = 0; cls < 78; ++cls)
                sum += std::exp(row[cls] - max_value);
            const float prob = sum > 0.0F ? 1.0F / sum : 0.0F;
            score_sum += prob;
            ++score_count;
        }
    }
    number_score = score_count > 0 ? score_sum / static_cast<float>(score_count) : 0.0F;
    return true;
}

bool AiPlateResultParser::DecodeColor(const float* logits, const std::vector<int>& shape, int& color_index,
                                      float& color_score, std::string& error) {
    if (!logits || shape.size() != 2 || shape[0] != 1 || shape[1] != 5) {
        error = "plate color output must be [1,5]";
        return false;
    }
    color_index = 0;
    for (int i = 0; i < 5; ++i) {
        if (!std::isfinite(logits[i])) {
            color_index = -1;
            color_score = 0.0F;
            error       = "plate color output contains non-finite value";
            return false;
        }
    }
    for (int i = 1; i < 5; ++i) {
        if (logits[i] > logits[color_index])
            color_index = i;
    }
    const float max_value = logits[color_index];
    float sum             = 0.0F;
    for (int i = 0; i < 5; ++i)
        sum += std::exp(logits[i] - max_value);
    color_score = sum > 0.0F ? 1.0F / sum : 0.0F;
    return true;
}

}  // namespace cosmo
