#include "nn/pipeline/plate_recognition_pipeline.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <nlohmann/json.hpp>

#include "nn/pipeline/pipeline_utils.h"

namespace cosmo::nn {
namespace {

    const char* const kPlateCharacters[] = {
        "#",  "京", "沪", "津", "渝", "冀", "晋", "蒙", "辽", "吉", "黑", "苏", "浙", "皖", "闽", "赣",
        "鲁", "豫", "鄂", "湘", "粤", "桂", "琼", "川", "贵", "云", "藏", "陕", "甘", "青", "宁", "新",
        "学", "警", "港", "澳", "挂", "使", "领", "民", "航", "危", "0",  "1",  "2",  "3",  "4",  "5",
        "6",  "7",  "8",  "9",  "A",  "B",  "C",  "D",  "E",  "F",  "G",  "H",  "J",  "K",  "L",  "M",
        "N",  "P",  "Q",  "R",  "S",  "T",  "U",  "V",  "W",  "X",  "Y",  "Z",  "险", "品"};

    std::vector<float> ReadScalarOrFloatArray(const nlohmann::json& json, const char* key, float fallback,
                                              size_t size) {
        auto iter = json.find(key);
        if (iter == json.end() || iter->is_null())
            return std::vector<float>(size, fallback);
        if (iter->is_number())
            return std::vector<float>(size, iter->get<float>());
        if (!iter->is_array() || iter->size() != size)
            return std::vector<float>(size, fallback);

        std::vector<float> values;
        values.reserve(size);
        for (const auto& item : *iter) {
            if (!item.is_number())
                return std::vector<float>(size, fallback);
            values.push_back(item.get<float>());
        }
        return values;
    }

    std::vector<std::string> ReadStringArray(const nlohmann::json& json, const char* key,
                                             std::vector<std::string> fallback) {
        auto iter = json.find(key);
        if (iter == json.end() || iter->is_null())
            return fallback;
        if (!iter->is_array())
            return fallback;

        std::vector<std::string> values;
        values.reserve(iter->size());
        for (const auto& item : *iter) {
            if (!item.is_string())
                return fallback;
            values.push_back(item.get<std::string>());
        }
        return values;
    }

    std::vector<int> ResolveImageInputSizeHw(const PipelineModelConfig& model_config,
                                             const std::vector<int>& fallback_size) {
        for (const auto& input : model_config.inputs) {
            if (input.shape.size() == 4 && input.shape[1] == 3 && input.shape[2] > 0 && input.shape[3] > 0) {
                return {input.shape[2], input.shape[3]};
            }
        }

        if (fallback_size.size() >= 2 && fallback_size[0] > 0 && fallback_size[1] > 0)
            return {fallback_size[1], fallback_size[0]};
        return {};
    }

    std::string FindOutputRole(const nlohmann::json& json, const char* role) {
        auto roles = json.find("output_roles");
        if (roles == json.end() || !roles->is_object())
            return {};
        auto value = roles->find(role);
        return value != roles->end() && value->is_string() ? value->get<std::string>() : std::string();
    }

    std::string ResolveOutputRole(const nlohmann::json& top, const nlohmann::json& params, const char* role,
                                  const std::vector<PipelineModelConfig::OutputDef>& outputs,
                                  const char* name_token) {
        std::string declared = FindOutputRole(params, role);
        if (declared.empty())
            declared = FindOutputRole(top, role);
        if (!declared.empty()) {
            const auto found = std::find_if(outputs.begin(), outputs.end(),
                                            [&](const auto& output) { return output.name == declared; });
            return found == outputs.end() ? std::string() : declared;
        }

        std::vector<std::string> matches;
        for (const auto& output : outputs) {
            if (output.name.find(name_token) != std::string::npos)
                matches.push_back(output.name);
        }
        return matches.size() == 1 ? matches.front() : std::string();
    }

    Status DecodePlateOutputs(const float* ocr_logits, const DimsVector& ocr_shape, const float* color_logits,
                              const DimsVector& color_shape, int blank_index, int class_count,
                              const std::vector<std::string>& color_labels,
                              std::vector<PlateRecognitionResult>& outputs) {
        if (!ocr_logits || !color_logits || ocr_shape.size() != 3 || color_shape.size() != 2 ||
            ocr_shape[0] != color_shape[0] || ocr_shape[0] <= 0 || ocr_shape[1] <= 0 || ocr_shape[2] <= 0) {
            return Status(COSMO_NN_ERR_NET, "Plate OCR/color output shapes must be [B,T,C] and [B,C]");
        }
        if (blank_index < 0 || class_count != ocr_shape[2] ||
            static_cast<size_t>(class_count) != std::size(kPlateCharacters)) {
            return Status(COSMO_NN_ERR_PARAM, "Plate CTC class configuration does not match OCR output");
        }
        if (color_shape[1] <= 0 || color_labels.size() != static_cast<size_t>(color_shape[1])) {
            return Status(COSMO_NN_ERR_PARAM, "Plate color labels do not match color output");
        }

        const int batch         = ocr_shape[0];
        const int time_steps    = ocr_shape[1];
        const int ocr_classes   = ocr_shape[2];
        const int color_classes = color_shape[1];
        outputs.clear();
        outputs.reserve(static_cast<size_t>(batch));

        for (int batch_index = 0; batch_index < batch; ++batch_index) {
            const float* batch_ocr = ocr_logits + static_cast<size_t>(batch_index) * time_steps * ocr_classes;
            PlateRecognitionResult result;
            int previous    = blank_index;
            float score_sum = 0.0F;
            int score_count = 0;

            for (int step = 0; step < time_steps; ++step) {
                const float* row = batch_ocr + static_cast<size_t>(step) * ocr_classes;
                int best         = 0;
                for (int cls = 1; cls < ocr_classes; ++cls) {
                    if (!std::isfinite(row[cls]) || !std::isfinite(row[best]))
                        return Status(COSMO_NN_ERR_NET, "Plate OCR output contains non-finite values");
                    if (row[cls] > row[best])
                        best = cls;
                }
                if (best != previous && best != blank_index) {
                    result.text += kPlateCharacters[best];
                    float exp_sum = 0.0F;
                    for (int cls = 0; cls < ocr_classes; ++cls)
                        exp_sum += std::exp(row[cls] - row[best]);
                    score_sum += exp_sum > 0.0F ? 1.0F / exp_sum : 0.0F;
                    ++score_count;
                }
                previous = best;
            }
            result.number_score = score_count > 0 ? score_sum / static_cast<float>(score_count) : 0.0F;

            const float* color_row = color_logits + static_cast<size_t>(batch_index) * color_classes;
            int color_index        = 0;
            for (int cls = 0; cls < color_classes; ++cls) {
                if (!std::isfinite(color_row[cls]))
                    return Status(COSMO_NN_ERR_NET, "Plate color output contains non-finite values");
                if (color_row[cls] > color_row[color_index])
                    color_index = cls;
            }
            float color_exp_sum = 0.0F;
            for (int cls = 0; cls < color_classes; ++cls)
                color_exp_sum += std::exp(color_row[cls] - color_row[color_index]);
            result.color       = color_labels[static_cast<size_t>(color_index)];
            result.color_score = color_exp_sum > 0.0F ? 1.0F / color_exp_sum : 0.0F;
            outputs.push_back(std::move(result));
        }
        return COSMO_NN_OK;
    }

}  // namespace

Status PlateRecognitionPipeline::Init(const PipelineConfig& config, const std::string& model_path,
                                      DeviceType device_type, int device_id, IProfiler* profiler,
                                      const std::string& tokenizer_path, const std::string& word_table_path,
                                      bool use_skip) {
    (void)word_table_path;
    if (config.models.size() != 1)
        return Status(COSMO_NN_ERR_PARAM, "plate_rec_color requires exactly one model");

    model_info_.algorithmcode = config.algorithm_code;
    model_info_.reduce        = config.reduce;
    model_info_.type          = config.model_type;

    const nlohmann::json top    = pipeline_utils::ParseJsonObject(config.template_json);
    auto& model_config          = config.models.front();
    const nlohmann::json params = pipeline_utils::ParseJsonObject(model_config.params_json);

    ocr_output_name_   = ResolveOutputRole(top, params, "ocr", model_config.outputs, "ocr");
    color_output_name_ = ResolveOutputRole(top, params, "color", model_config.outputs, "color");
    if (ocr_output_name_.empty() || color_output_name_.empty() || ocr_output_name_ == color_output_name_)
        return Status(COSMO_NN_ERR_PARAM, "plate_rec_color must declare distinct OCR and color output roles");

    ctc_blank_index_ = pipeline_utils::ReadInt(params, "ctc_blank_index", -1);
    if (ctc_blank_index_ < 0)
        return Status(COSMO_NN_ERR_PARAM, "plate_rec_color requires ctc_blank_index");

    color_labels_ = ReadStringArray(params, "color_labels", {"black", "blue", "green", "white", "yellow"});

    ModelInfo model;
    model.name      = model_config.name;
    model.filename  = model_config.file_name;
    model.file_md5  = model_config.file_md5;
    model.max_batch = model_config.max_batch;
    max_batch_      = model_config.max_batch;

    const std::vector<int> input_size = pipeline_utils::ReadIntArray(params, "input_size", {168, 48}, 2);
    const std::vector<int> input_hw   = ResolveImageInputSizeHw(model_config, input_size);
    if (input_hw.size() != 2 || input_hw[0] <= 0 || input_hw[1] <= 0)
        return Status(COSMO_NN_ERR_PARAM, "plate_rec_color input size is invalid");
    const std::vector<float> mean    = ReadScalarOrFloatArray(params, "normalize_mean", 0.0F, 3);
    const std::vector<float> std_dev = ReadScalarOrFloatArray(params, "normalize_std", 1.0F, 3);
    const bool is_bgr                = pipeline_utils::ReadBool(params, "is_bgr", true);

    std::vector<float> pixel_mean(3, 0.0F);
    std::vector<float> pixel_std(3, 1.0F);
    for (size_t i = 0; i < 3; ++i) {
        pixel_mean[i] = mean[i] * 255.0F;
        pixel_std[i]  = std_dev[i] * 255.0F;
        if (pixel_std[i] <= 0.0F)
            return Status(COSMO_NN_ERR_PARAM, "plate_rec_color normalize_std must be positive");
    }

    std::vector<std::unique_ptr<Op>> preprocess;
    // Prefer the ONNX-declared NCHW shape so imported runtime configs with a
    // different input_size ordering (for example [48,168]) stay compatible.
    preprocess.push_back(pipeline_utils::MakeResizeOp(input_hw, 0, {0, 0, 0}));
    preprocess.push_back(pipeline_utils::MakeNormalizeOp(pixel_mean, 1.0F, is_bgr, pixel_std));

    for (const auto& input_def : model_config.inputs) {
        InputNodeInfo input;
        input.name      = input_def.name;
        input.shape     = input_def.shape;
        input.data_type = input_def.data_type;
        input.ops       = std::move(preprocess);
        model.input_node_infos.push_back(std::move(input));
    }
    for (const auto& output_def : model_config.outputs) {
        OutputNodeInfo output;
        output.name      = output_def.name;
        output.shape     = output_def.shape;
        output.data_type = output_def.data_type;
        model.output_node_infos.push_back(std::move(output));
    }
    model_info_.models.push_back(std::move(model));

    const auto ocr_output = std::find_if(model_config.outputs.begin(), model_config.outputs.end(),
                                         [&](const auto& output) { return output.name == ocr_output_name_; });
    if (ocr_output == model_config.outputs.end() || ocr_output->shape.size() != 3 ||
        ocr_output->shape[2] <= 0) {
        return Status(COSMO_NN_ERR_PARAM, "plate_rec_color OCR output shape is invalid");
    }
    ocr_output_index_ = static_cast<size_t>(std::distance(model_config.outputs.begin(), ocr_output));
    ocr_class_count_  = ocr_output->shape[2];
    if (ocr_class_count_ != static_cast<int>(std::size(kPlateCharacters))) {
        return Status(COSMO_NN_ERR_PARAM, "plate_rec_color OCR class count does not match text table");
    }

    const auto color_output =
        std::find_if(model_config.outputs.begin(), model_config.outputs.end(),
                     [&](const auto& output) { return output.name == color_output_name_; });
    if (color_output == model_config.outputs.end() || color_output->shape.size() != 2 ||
        color_output->shape[1] <= 0 || color_labels_.size() != static_cast<size_t>(color_output->shape[1])) {
        return Status(COSMO_NN_ERR_PARAM, "plate_rec_color color output shape/labels are invalid");
    }
    color_output_index_ = static_cast<size_t>(std::distance(model_config.outputs.begin(), color_output));

    InitThresholdsAndLabels();
    InitNetInputSize();
    return InitGraph(model_path, device_type, device_id, profiler, tokenizer_path, use_skip);
}

Status PlateRecognitionPipeline::Forward(std::initializer_list<std::vector<std::shared_ptr<Blob>>> inputs) {
    if (inputs.size() != 1 || inputs.begin()->empty())
        return Status(COSMO_NN_ERR_INVALID_INPUT, "plate_rec_color requires one image input");
    return RunGraph(inputs);
}

Status PlateRecognitionPipeline::ParsePlateOutput(std::vector<PlateRecognitionResult>& outputs) {
    const auto output_blobs = GetGraphOutput();
    if (ocr_output_index_ >= output_blobs.size() || color_output_index_ >= output_blobs.size() ||
        !output_blobs[ocr_output_index_] || !output_blobs[color_output_index_]) {
        return Status(COSMO_NN_ERR_NET, "plate_rec_color graph outputs are incomplete");
    }
    const auto ocr_blob   = output_blobs[ocr_output_index_];
    const auto color_blob = output_blobs[color_output_index_];

    const auto ocr_desc   = ocr_blob->GetBlobDesc();
    const auto color_desc = color_blob->GetBlobDesc();
    if (ocr_desc.data_type != DataType::DATA_TYPE_FLOAT ||
        color_desc.data_type != DataType::DATA_TYPE_FLOAT) {
        return Status(COSMO_NN_ERR_NET, "plate_rec_color outputs must be float32");
    }
    return DecodePlateOutputs(static_cast<const float*>(ocr_blob->GetHandle().base), ocr_desc.dims,
                              static_cast<const float*>(color_blob->GetHandle().base), color_desc.dims,
                              ctc_blank_index_, ocr_class_count_, color_labels_, outputs);
}

REGISTER_MODEL_PIPELINE("plate_rec_color", PlateRecognitionPipeline);

}  // namespace cosmo::nn
