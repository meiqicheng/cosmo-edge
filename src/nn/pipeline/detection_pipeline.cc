#include "nn/pipeline/detection_pipeline.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <nlohmann/json.hpp>

#include "nn/pipeline/pipeline_utils.h"

namespace cosmo::nn {

// ─── Internal Helpers ─────────────────────────────────────────

static bool ReadYoloNpuPostParams(const nlohmann::json& json,
                                  std::vector<std::vector<std::vector<float>>>& anchors,
                                  std::vector<float>& stride) {
    anchors = pipeline_utils::ReadFloat3DArray(json, "anchors", {}, 1, 1, 2);
    stride  = pipeline_utils::ReadFloatArray(json, "stride", {}, 1);

    if (anchors.empty() || stride.empty() || anchors.size() != stride.size())
        return false;

    for (const auto& grid : anchors) {
        if (grid.empty())
            return false;
        for (const auto& anchor : grid) {
            if (anchor.size() < 2)
                return false;
        }
    }
    return true;
}

static std::vector<std::unique_ptr<Op>> MakeDetPreprocess(const nlohmann::json& p) {
    std::vector<std::unique_ptr<Op>> ops;

    std::vector<int> dsize = pipeline_utils::ReadIntArray(p, "input_size", {640, 640}, 2);
    int gravity = pipeline_utils::ReadInt(p, "gravity", pipeline_utils::ReadInt(p, "padding_gravity", 0));
    std::vector<int> color = pipeline_utils::ReadIntArray(p, "padding_color", {114, 114, 114}, 1);

    ops.push_back(pipeline_utils::MakeResizeOp(dsize, gravity, color));

    std::vector<float> mean    = pipeline_utils::ReadFloatArray(p, "normalize_mean", {0.f, 0.f, 0.f}, 3);
    float scale                = pipeline_utils::ReadFloat(p, "normalize_scale", 0.00392157f);
    bool is_bgr                = pipeline_utils::ReadBool(p, "is_bgr", true);
    std::vector<float> std_dev = pipeline_utils::ReadFloatArray(p, "normalize_std", {}, 3);

    ops.push_back(pipeline_utils::MakeNormalizeOp(mean, scale, is_bgr, std_dev));
    return ops;
}

static void BuildLabels(const PipelineConfig& config, const std::string& output_node,
                        const DimsVector& output_shape, CombinedModelInfo& info) {
    pipeline_utils::BuildInstructionsFromLabels(config.labels, output_node, output_shape, info.config);
}

// ─── Detection Forward Common Logic ──────────────────────

static Status DetectionForward(
    ModelPipeline* self, std::initializer_list<std::vector<std::shared_ptr<Blob>>> inputs,
    std::vector<Size>& image_sizes,
    std::function<Status(std::initializer_list<std::vector<std::shared_ptr<Blob>>>)> run_graph) {
    RETURN_ON_FAIL(run_graph(inputs));

    auto iter = inputs.begin();
    image_sizes.clear();
    for (size_t i = 0; i < iter->size(); i++) {
        auto dims   = iter->at(i)->GetBlobDesc().dims;
        auto layout = iter->at(i)->GetBlobDesc().data_format;
        Size size;
        RETURN_ON_FAIL(NetUtils::GetImageSize(dims, layout, size));
        image_sizes.push_back(size);
    }
    return COSMO_NN_OK;
}

static Status DetectionParseOutput(std::vector<std::shared_ptr<Blob>> output_blobs,
                                   const std::vector<Size>& image_sizes, const Size& net_input_size,
                                   const std::vector<int>& indices, const std::vector<float>& thresholds,
                                   const std::vector<std::string>& classnames,
                                   std::vector<std::vector<ObjectInfoV1>>& outputs) {
    outputs.clear();
    std::vector<Size> image_sizes_copy       = image_sizes;
    std::vector<int> indices_copy            = indices;
    std::vector<float> thresholds_copy       = thresholds;
    std::vector<std::string> classnames_copy = classnames;
    return NetUtils::ParseDetectionOutput(output_blobs, image_sizes_copy, net_input_size, indices_copy,
                                          thresholds_copy, classnames_copy, outputs);
}

static float PoseIou(const ObjectInfoV1& lhs, const ObjectInfoV1& rhs) {
    const float left = std::max(lhs.x1, rhs.x1);
    const float top = std::max(lhs.y1, rhs.y1);
    const float right = std::min(lhs.x2, rhs.x2);
    const float bottom = std::min(lhs.y2, rhs.y2);
    const float intersection = std::max(0.0f, right - left) * std::max(0.0f, bottom - top);
    const float lhs_area = std::max(0.0f, lhs.x2 - lhs.x1) * std::max(0.0f, lhs.y2 - lhs.y1);
    const float rhs_area = std::max(0.0f, rhs.x2 - rhs.x1) * std::max(0.0f, rhs.y2 - rhs.y1);
    const float denominator = lhs_area + rhs_area - intersection;
    return denominator > 0.0f ? intersection / denominator : 0.0f;
}

static void ApplyPoseNms(std::vector<ObjectInfoV1>& objects, float threshold, int top_k) {
    std::sort(objects.begin(), objects.end(), [](const ObjectInfoV1& lhs, const ObjectInfoV1& rhs) {
        return lhs.infos.front().confidence > rhs.infos.front().confidence;
    });
    std::vector<ObjectInfoV1> selected;
    selected.reserve(std::min(static_cast<size_t>(std::max(top_k, 0)), objects.size()));
    for (const auto& object : objects) {
        bool suppressed = false;
        for (const auto& kept : selected) {
            if (object.infos.front().class_id == kept.infos.front().class_id && PoseIou(object, kept) >= threshold) {
                suppressed = true;
                break;
            }
        }
        if (!suppressed)
            selected.push_back(object);
        if (top_k > 0 && static_cast<int>(selected.size()) >= top_k)
            break;
    }
    objects = std::move(selected);
}

// ─── Pose Variant Declarations ─────────────────────────────────
//
// A pose head comes in at least two shapes: the YOLOv8/11 channel-major tensor
// ([1, 4+classes+V*K, points]) and the YOLO26 end-to-end row-major tensor
// ([1, points, 6+V*K]). Their channel arithmetic overlaps often enough that
// inferring the layout from the output shape silently decodes the wrong model
// (a 2-class plate detector has 6 channels, exactly what a 4-point/2-value pose
// head would leave behind). Variants are therefore selected by the model
// template instead of by inspection.
//
// Every pose declaration is written once at the template level. A per-model
// params object may still repeat one (older templates) and then wins, so
// existing templates keep loading while the duplicates are removed.

struct PoseContract {
    YoloPoseLayout layout          = YoloPoseLayout::kChannelMajor;
    bool nms_completed             = false;
    int keypoint_count             = 0;  // 0 = not declared, caller keeps its default
    int keypoint_values_per_point  = 0;
    std::string kind;
    std::string schema;
};

static std::string NormalizeDeclaredValue(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(),
                               [](unsigned char c) { return std::isspace(c) != 0; }),
                value.end());
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static std::string DeclaredString(const nlohmann::json& top, const nlohmann::json& params, const char* key) {
    std::string value = pipeline_utils::ReadString(params, key, std::string());
    if (value.empty())
        value = pipeline_utils::ReadString(top, key, std::string());
    return NormalizeDeclaredValue(value);
}

static int DeclaredInt(const nlohmann::json& top, const nlohmann::json& params, const char* key,
                       int fallback) {
    const int from_params = pipeline_utils::ReadInt(params, key, 0);
    if (from_params > 0)
        return from_params;
    const int from_template = pipeline_utils::ReadInt(top, key, 0);
    return from_template > 0 ? from_template : fallback;
}

static bool DeclaredBool(const nlohmann::json& top, const nlohmann::json& params, const char* key,
                         bool fallback) {
    if (params.contains(key))
        return pipeline_utils::ReadBool(params, key, fallback);
    return pipeline_utils::ReadBool(top, key, fallback);
}

// Resolve every pose declaration in one pass. Only the layout is mandatory: a
// template that declares none of keypoint_layout/decoder/output_layout is
// rejected instead of being guessed from the output shape.
static Status ResolvePoseContract(const nlohmann::json& top, const nlohmann::json& params,
                                  PoseContract& contract, std::string& error) {
    contract.kind            = pipeline_utils::ReadString(top, "keypoint_kind", std::string());
    contract.schema          = pipeline_utils::ReadString(top, "keypoint_schema", std::string());
    contract.keypoint_count  = DeclaredInt(top, params, "keypoint_count", contract.keypoint_count);
    contract.keypoint_values_per_point =
        DeclaredInt(top, params, "keypoint_values_per_point", contract.keypoint_values_per_point);
    contract.nms_completed = DeclaredBool(top, params, "nms_completed", contract.nms_completed);

    const std::string keypoint_layout = DeclaredString(top, params, "keypoint_layout");
    if (!keypoint_layout.empty()) {
        if (keypoint_layout == "channel_major") {
            contract.layout = YoloPoseLayout::kChannelMajor;
            return COSMO_NN_OK;
        }
        if (keypoint_layout == "end_to_end" || keypoint_layout == "row_major") {
            contract.layout = YoloPoseLayout::kEndToEnd;
            return COSMO_NN_OK;
        }
        error = "unsupported keypoint_layout '" + keypoint_layout +
                "' (expected channel_major or end_to_end)";
        return Status(COSMO_NN_ERR_PARAM, error);
    }

    const std::string decoder = DeclaredString(top, params, "decoder");
    if (!decoder.empty()) {
        if (decoder == "yolo_pose_tensor") {
            contract.layout = YoloPoseLayout::kChannelMajor;
            return COSMO_NN_OK;
        }
        if (decoder == "yolo_pose_end_to_end") {
            contract.layout = YoloPoseLayout::kEndToEnd;
            return COSMO_NN_OK;
        }
        error = "unsupported pose decoder '" + decoder +
                "' (expected yolo_pose_tensor or yolo_pose_end_to_end)";
        return Status(COSMO_NN_ERR_PARAM, error);
    }

    const std::string output_layout = DeclaredString(top, params, "output_layout");
    if (!output_layout.empty()) {
        if (output_layout == "[b,c,n]") {
            contract.layout = YoloPoseLayout::kChannelMajor;
            return COSMO_NN_OK;
        }
        if (output_layout == "[b,n,c]") {
            contract.layout = YoloPoseLayout::kEndToEnd;
            return COSMO_NN_OK;
        }
        error = "unsupported output_layout '" + output_layout + "' (expected [B,C,N] or [B,N,C])";
        return Status(COSMO_NN_ERR_PARAM, error);
    }

    error = "pose model template must declare keypoint_layout, decoder or output_layout";
    return Status(COSMO_NN_ERR_PARAM, error);
}

// ============================= YOLOv5 =====================================

Status YoloV5DetPipeline::Init(const PipelineConfig& config, const std::string& model_path,
                               DeviceType device_type, int device_id, IProfiler* profiler,
                               const std::string& tokenizer_path, const std::string& word_table_path,
                               bool use_skip) {
    model_info_.algorithmcode = config.algorithm_code;
    model_info_.reduce        = config.reduce;
    model_info_.type          = "yolov5_det";

    for (auto& mc : config.models) {
        nlohmann::json p = pipeline_utils::ParseJsonObject(mc.params_json);
        ModelInfo model;
        model.name           = mc.name;
        model.filename       = mc.file_name;
        model.file_md5       = mc.file_md5;
        model.input_contract = pipeline_utils::ReadString(p, "rknn_input_contract", std::string());
        model.max_batch      = mc.max_batch;
        max_batch_           = mc.max_batch;

        for (auto& in_def : mc.inputs) {
            InputNodeInfo input;
            input.name      = in_def.name;
            input.shape     = in_def.shape;
            input.data_type = in_def.data_type;
            input.ops       = MakeDetPreprocess(p);
            model.input_node_infos.push_back(std::move(input));
        }

        float nms_thresh  = pipeline_utils::ReadFloat(p, "nms_threshold", 0.35f);
        float conf_thresh = pipeline_utils::ReadFloat(p, "confidence_threshold", 0.1f);
        int top_k         = pipeline_utils::ReadInt(p, "top_k", 1000);
        bool use_npu_post = pipeline_utils::ReadBool(p, "use_npu_postprocess", false);

        // Extract input size for coordinate denormalization
        int v5_input_w = 640, v5_input_h = 640;
        std::vector<int> input_size = pipeline_utils::ReadIntArray(p, "input_size", {}, 2);
        if (input_size.size() >= 2) {
            v5_input_w = input_size[0];
            v5_input_h = input_size[1];
        }

        for (size_t i = 0; i < mc.outputs.size(); i++) {
            auto& out_def = mc.outputs[i];
            OutputNodeInfo output;
            output.name      = out_def.name;
            output.shape     = out_def.shape;
            output.data_type = out_def.data_type;

            if (i == 0) {
                if (use_npu_post) {
                    std::vector<std::vector<std::vector<float>>> anchors;
                    std::vector<float> stride;
                    if (!ReadYoloNpuPostParams(p, anchors, stride)) {
                        return Status(COSMO_NN_ERR_PARAM, "Invalid yolo_npu anchors/stride config");
                    }
                    output.op =
                        pipeline_utils::MakeYoloNpuPostOp(nms_thresh, conf_thresh, top_k, anchors, stride);
                } else {
                    output.op = pipeline_utils::MakeYoloPostOp(nms_thresh, conf_thresh, top_k, v5_input_w,
                                                               v5_input_h);
                }
            }
            model.output_node_infos.push_back(std::move(output));
        }
        model_info_.models.push_back(std::move(model));
    }

    if (!config.labels.empty() && !model_info_.models.empty()) {
        auto& last           = model_info_.models.back();
        std::string out_name = "output";
        DimsVector out_shape = {-1, -1, 6};
        if (!last.output_node_infos.empty())
            out_name = last.output_node_infos.front().name;
        BuildLabels(config, out_name, out_shape, model_info_);
    }

    InitThresholdsAndLabels();
    InitNetInputSize();
    return InitGraph(model_path, device_type, device_id, profiler, tokenizer_path, use_skip);
}

Status YoloV5DetPipeline::Forward(std::initializer_list<std::vector<std::shared_ptr<Blob>>> inputs) {
    return DetectionForward(
        this, inputs, image_sizes_,
        [this](std::initializer_list<std::vector<std::shared_ptr<Blob>>> inp) { return RunGraph(inp); });
}

Status YoloV5DetPipeline::ParseDetectionOutput(std::vector<std::vector<ObjectInfoV1>>& outputs) {
    return DetectionParseOutput(GetGraphOutput(), image_sizes_, net_input_size_, selected_indices_,
                                selected_thresholds_, selected_classnames_, outputs);
}

// ============================= YOLOv8 =====================================

Status YoloV8DetPipeline::Init(const PipelineConfig& config, const std::string& model_path,
                               DeviceType device_type, int device_id, IProfiler* profiler,
                               const std::string& tokenizer_path, const std::string& word_table_path,
                               bool use_skip) {
    model_info_.algorithmcode = config.algorithm_code;
    model_info_.reduce        = config.reduce;
    model_info_.type          = "yolov8_det";

    for (auto& mc : config.models) {
        nlohmann::json p = pipeline_utils::ParseJsonObject(mc.params_json);
        ModelInfo model;
        model.name           = mc.name;
        model.filename       = mc.file_name;
        model.file_md5       = mc.file_md5;
        model.input_contract = pipeline_utils::ReadString(p, "rknn_input_contract", std::string());
        model.max_batch      = mc.max_batch;
        max_batch_           = mc.max_batch;

        for (auto& in_def : mc.inputs) {
            InputNodeInfo input;
            input.name      = in_def.name;
            input.shape     = in_def.shape;
            input.data_type = in_def.data_type;
            input.ops       = MakeDetPreprocess(p);
            model.input_node_infos.push_back(std::move(input));
        }

        float nms_thresh  = pipeline_utils::ReadFloat(p, "nms_threshold", 0.7f);
        float conf_thresh = pipeline_utils::ReadFloat(p, "confidence_threshold", 0.25f);
        int top_k         = pipeline_utils::ReadInt(p, "top_k", 300);

        // Extract input size for coordinate denormalization
        int post_input_w = 640, post_input_h = 640;
        std::vector<int> input_size = pipeline_utils::ReadIntArray(p, "input_size", {}, 2);
        if (input_size.size() >= 2) {
            post_input_w = input_size[0];
            post_input_h = input_size[1];
        }

        for (size_t i = 0; i < mc.outputs.size(); i++) {
            auto& out_def = mc.outputs[i];
            OutputNodeInfo output;
            output.name      = out_def.name;
            output.shape     = out_def.shape;
            output.data_type = out_def.data_type;
            if (i == 0)
                output.op = pipeline_utils::MakeYoloV8PostOp(nms_thresh, conf_thresh, top_k, post_input_w,
                                                             post_input_h);
            model.output_node_infos.push_back(std::move(output));
        }
        model_info_.models.push_back(std::move(model));
    }

    if (!config.labels.empty() && !model_info_.models.empty()) {
        auto& last           = model_info_.models.back();
        std::string out_name = "output0";
        DimsVector out_shape = {-1, -1, 6};
        if (!last.output_node_infos.empty())
            out_name = last.output_node_infos.front().name;
        BuildLabels(config, out_name, out_shape, model_info_);
    }

    InitThresholdsAndLabels();
    InitNetInputSize();
    return InitGraph(model_path, device_type, device_id, profiler, tokenizer_path, use_skip);
}

Status YoloV8DetPipeline::Forward(std::initializer_list<std::vector<std::shared_ptr<Blob>>> inputs) {
    return DetectionForward(
        this, inputs, image_sizes_,
        [this](std::initializer_list<std::vector<std::shared_ptr<Blob>>> inp) { return RunGraph(inp); });
}

Status YoloV8DetPipeline::ParseDetectionOutput(std::vector<std::vector<ObjectInfoV1>>& outputs) {
    return DetectionParseOutput(GetGraphOutput(), image_sizes_, net_input_size_, selected_indices_,
                                selected_thresholds_, selected_classnames_, outputs);
}

// ============================= YOLO26 (End-to-End) =======================

Status Yolo26DetPipeline::Init(const PipelineConfig& config, const std::string& model_path,
                               DeviceType device_type, int device_id, IProfiler* profiler,
                               const std::string& tokenizer_path, const std::string& word_table_path,
                               bool use_skip) {
    model_info_.algorithmcode = config.algorithm_code;
    model_info_.reduce        = config.reduce;
    model_info_.type          = "yolo26_det";

    for (auto& mc : config.models) {
        nlohmann::json p = pipeline_utils::ParseJsonObject(mc.params_json);
        ModelInfo model;
        model.name      = mc.name;
        model.filename  = mc.file_name;
        model.file_md5  = mc.file_md5;
        model.max_batch = mc.max_batch;
        max_batch_      = mc.max_batch;

        for (auto& in_def : mc.inputs) {
            InputNodeInfo input;
            input.name      = in_def.name;
            input.shape     = in_def.shape;
            input.data_type = in_def.data_type;
            input.ops       = MakeDetPreprocess(p);
            model.input_node_infos.push_back(std::move(input));
        }

        float conf_thresh = pipeline_utils::ReadFloat(p, "confidence_threshold", 0.25f);
        int top_k         = pipeline_utils::ReadInt(p, "top_k", 300);

        // Extract input size for coordinate denormalization
        int e2e_input_w = 640, e2e_input_h = 640;
        std::vector<int> input_size = pipeline_utils::ReadIntArray(p, "input_size", {}, 2);
        if (input_size.size() >= 2) {
            e2e_input_w = input_size[0];
            e2e_input_h = input_size[1];
        }

        for (size_t i = 0; i < mc.outputs.size(); i++) {
            auto& out_def = mc.outputs[i];
            OutputNodeInfo output;
            output.name      = out_def.name;
            output.shape     = out_def.shape;
            output.data_type = out_def.data_type;
            if (i == 0)
                output.op = pipeline_utils::MakeYoloE2EPostOp(conf_thresh, top_k, e2e_input_w, e2e_input_h);
            model.output_node_infos.push_back(std::move(output));
        }
        model_info_.models.push_back(std::move(model));
    }

    if (!config.labels.empty() && !model_info_.models.empty()) {
        auto& last           = model_info_.models.back();
        std::string out_name = "output";
        DimsVector out_shape = {-1, -1, 6};
        if (!last.output_node_infos.empty())
            out_name = last.output_node_infos.front().name;
        BuildLabels(config, out_name, out_shape, model_info_);
    }

    InitThresholdsAndLabels();
    InitNetInputSize();
    return InitGraph(model_path, device_type, device_id, profiler, tokenizer_path, use_skip);
}

Status Yolo26DetPipeline::Forward(std::initializer_list<std::vector<std::shared_ptr<Blob>>> inputs) {
    return DetectionForward(
        this, inputs, image_sizes_,
        [this](std::initializer_list<std::vector<std::shared_ptr<Blob>>> inp) { return RunGraph(inp); });
}

Status Yolo26DetPipeline::ParseDetectionOutput(std::vector<std::vector<ObjectInfoV1>>& outputs) {
    return DetectionParseOutput(GetGraphOutput(), image_sizes_, net_input_size_, selected_indices_,
                                selected_thresholds_, selected_classnames_, outputs);
}

Status YoloPosePipeline::Init(const PipelineConfig& config, const std::string& model_path,
                              DeviceType device_type, int device_id, IProfiler* profiler,
                              const std::string& tokenizer_path, const std::string& word_table_path,
                              bool use_skip) {
    model_info_.algorithmcode = config.algorithm_code;
    model_info_.reduce = config.reduce;
    model_info_.type = config.model_type;
    // Template-level declarations. They tell us which pose variant this is, so
    // the decoding path never has to guess from the output shape.
    const auto top = pipeline_utils::ParseJsonObject(config.template_json);
    for (const auto& mc : config.models) {
        const auto p = pipeline_utils::ParseJsonObject(mc.params_json);
        ModelInfo model;
        model.name = mc.name;
        model.filename = mc.file_name;
        model.file_md5 = mc.file_md5;
        model.max_batch = mc.max_batch;
        max_batch_ = mc.max_batch;
    class_count_ = pipeline_utils::ReadInt(p, "class_count", 1);
        PoseContract contract;
        std::string contract_error;
        auto contract_status = ResolvePoseContract(top, p, contract, contract_error);
        if (!contract_status)
            return contract_status;
        pose_layout_     = contract.layout;
        // End-to-end heads already ran NMS inside the model; running the CPU pass
        // again would suppress the detections the model deliberately kept.
        nms_completed_   = contract.nms_completed;
        keypoint_kind_   = contract.kind;
        keypoint_schema_ = contract.schema;
        keypoint_count_  = contract.keypoint_count > 0 ? contract.keypoint_count : 17;
        keypoint_values_per_point_ =
            contract.keypoint_values_per_point > 0 ? contract.keypoint_values_per_point : 3;
        // The template declares the geometry, so the declared output shape must be
        // validated against it. A mismatch is a broken template and has to fail at
        // load time instead of producing silently wrong keypoints later.
        if (!mc.outputs.empty()) {
            const auto& out_shape    = mc.outputs.front().shape;
            const int expected_channels =
                PoseChannelCount(pose_layout_, class_count_, keypoint_count_, keypoint_values_per_point_);
            const int actual_channels =
                out_shape.size() == 3
                    ? (pose_layout_ == YoloPoseLayout::kChannelMajor ? out_shape[1] : out_shape[2])
                    : -1;
            if (actual_channels != expected_channels) {
                const std::string kind   = keypoint_kind_.empty() ? std::string("unknown") : keypoint_kind_;
                const std::string schema =
                    keypoint_schema_.empty() ? std::string("unknown") : keypoint_schema_;
                return Status(COSMO_NN_ERR_PARAM, "pose model '" + mc.name + "' (" + kind + "/" + schema +
                                                      ") declares " + std::to_string(actual_channels) +
                                                      " output channels but the configured layout needs " +
                                                      std::to_string(expected_channels));
            }
        }
        confidence_threshold_ = pipeline_utils::ReadFloat(p, "confidence_threshold", 0.25f);
        nms_threshold_ = pipeline_utils::ReadFloat(p, "nms_threshold", 0.7f);
        top_k_ = pipeline_utils::ReadInt(p, "top_k", 300);
        const auto input_size = pipeline_utils::ReadIntArray(p, "input_size", {640, 640}, 2);
        if (input_size.size() >= 2) {
            input_width_ = input_size[0];
            input_height_ = input_size[1];
        }
        resize_gravity_ = pipeline_utils::ReadInt(p, "gravity", pipeline_utils::ReadInt(p, "padding_gravity", 0));
        for (const auto& in_def : mc.inputs) {
            InputNodeInfo input;
            input.name = in_def.name;
            input.shape = in_def.shape;
            input.data_type = in_def.data_type;
            input.ops = MakeDetPreprocess(p);
            model.input_node_infos.push_back(std::move(input));
        }
        for (const auto& out_def : mc.outputs) {
            OutputNodeInfo output;
            output.name = out_def.name;
            output.shape = out_def.shape;
            output.data_type = out_def.data_type;
            model.output_node_infos.push_back(std::move(output));
        }
        model_info_.models.push_back(std::move(model));
    }
    // Build label instructions exactly like the other detection pipelines. Without
    // them the class names stay empty, the decoder falls back to "class_<id>", and
    // a downstream tracker rejects every pose target because its label never
    // matches the configured one.
    if (!config.labels.empty() && !model_info_.models.empty()) {
        auto& last           = model_info_.models.back();
        std::string out_name = "output0";
        if (!last.output_node_infos.empty())
            out_name = last.output_node_infos.front().name;
        BuildLabels(config, out_name, {-1, -1, 6}, model_info_);
    }
    InitThresholdsAndLabels();
    InitNetInputSize();
    return InitGraph(model_path, device_type, device_id, profiler, tokenizer_path, use_skip);
}

Status YoloPosePipeline::Forward(std::initializer_list<std::vector<std::shared_ptr<Blob>>> inputs) {
    RETURN_ON_FAIL(RunGraph(inputs));
    image_sizes_.clear();
    const auto iter = inputs.begin();
    for (size_t i = 0; i < iter->size(); ++i) {
        Size size;
        RETURN_ON_FAIL(NetUtils::GetImageSize(iter->at(i)->GetBlobDesc().dims,
                                              iter->at(i)->GetBlobDesc().data_format, size));
        image_sizes_.push_back(size);
    }
    return COSMO_NN_OK;
}

Status YoloPosePipeline::ParseDetectionOutput(std::vector<std::vector<ObjectInfoV1>>& outputs) {
    outputs.clear();
    const auto blobs = GetGraphOutput();
    if (blobs.size() != 1 || blobs.front()->GetBlobDesc().dims.size() != 3)
        return Status(COSMO_NN_ERR_INVALID_INPUT, "YOLO pose requires one rank-3 output");
    const auto dims = blobs.front()->GetBlobDesc().dims;
    const auto* data = static_cast<const float*>(blobs.front()->GetHandle().base);
    const int batch = dims[0];
    const size_t stride = static_cast<size_t>(dims[1]) * static_cast<size_t>(dims[2]);
    for (int i = 0; i < batch; ++i) {
        std::vector<ObjectInfoV1> decoded;
        std::string error;
        auto status = DecodeYoloPoseTensor(data + static_cast<size_t>(i) * stride, dims, pose_layout_,
                                           input_width_, input_height_, class_count_, keypoint_count_,
                                           confidence_threshold_, decoded, error, keypoint_values_per_point_);
        if (!status)
            return status;
        if (!nms_completed_)
            ApplyPoseNms(decoded, nms_threshold_, top_k_);
        // The decoder can only synthesise "class_<id>". Replace it with the
        // configured label so the target matches the tracker's label set (the
        // tracker drops anything whose label it does not know) and so the overlay
        // shows a meaningful name.
        for (auto& object : decoded) {
            if (object.infos.empty())
                continue;
            const int cls = object.infos.front().class_id;
            if (cls >= 0 && cls < static_cast<int>(selected_classnames_.size()) &&
                !selected_classnames_[static_cast<size_t>(cls)].empty()) {
                object.infos.front().class_name = selected_classnames_[static_cast<size_t>(cls)];
            }
        }
        // Decoded boxes and keypoints live in model-input space. Project them onto
        // the original frame size captured by Forward() before anything consumes
        // them, otherwise the live OSD draws a 640x640 skeleton into the corner of
        // a 1080p picture. Falling back to the net size keeps the mapping a no-op
        // when the frame size is unknown.
        const Size net_size(input_width_, input_height_);
        const Size frame_size =
            i < static_cast<int>(image_sizes_.size()) ? image_sizes_[static_cast<size_t>(i)] : net_size;
        MapYoloPoseToFrame(decoded, net_size, frame_size, resize_gravity_);
        outputs.push_back(std::move(decoded));
    }
    return COSMO_NN_OK;
}

// ========================= Generic Detector ===============================

Status GenericDetectorPipeline::Init(const PipelineConfig& config, const std::string& model_path,
                                     DeviceType device_type, int device_id, IProfiler* profiler,
                                     const std::string& tokenizer_path, const std::string& word_table_path,
                                     bool use_skip) {
    model_info_.algorithmcode = config.algorithm_code;
    model_info_.reduce        = config.reduce;
    model_info_.type          = "detector";

    for (auto& mc : config.models) {
        nlohmann::json p = pipeline_utils::ParseJsonObject(mc.params_json);
        ModelInfo model;
        model.name      = mc.name;
        model.filename  = mc.file_name;
        model.file_md5  = mc.file_md5;
        model.max_batch = mc.max_batch;
        max_batch_      = mc.max_batch;

        for (auto& in_def : mc.inputs) {
            InputNodeInfo input;
            input.name      = in_def.name;
            input.shape     = in_def.shape;
            input.data_type = in_def.data_type;
            input.ops       = MakeDetPreprocess(p);
            model.input_node_infos.push_back(std::move(input));
        }

        std::string post_type = pipeline_utils::ReadString(p, "post_type", std::string("yolo"));
        float nms_thresh      = pipeline_utils::ReadFloat(p, "nms_threshold", 0.35f);
        float conf_thresh     = pipeline_utils::ReadFloat(p, "confidence_threshold", 0.1f);
        int top_k             = pipeline_utils::ReadInt(p, "top_k", 1000);

        for (size_t i = 0; i < mc.outputs.size(); i++) {
            auto& out_def = mc.outputs[i];
            OutputNodeInfo output;
            output.name      = out_def.name;
            output.shape     = out_def.shape;
            output.data_type = out_def.data_type;

            if (i == 0) {
                // Extract input size for coordinate denormalization
                int gd_input_w = 640, gd_input_h = 640;
                std::vector<int> input_size = pipeline_utils::ReadIntArray(p, "input_size", {}, 2);
                if (input_size.size() >= 2) {
                    gd_input_w = input_size[0];
                    gd_input_h = input_size[1];
                }
                if (post_type == "yolov8") {
                    output.op = pipeline_utils::MakeYoloV8PostOp(nms_thresh, conf_thresh, top_k, gd_input_w,
                                                                 gd_input_h);
                } else if (post_type == "yolo_e2e") {
                    output.op = pipeline_utils::MakeYoloE2EPostOp(conf_thresh, top_k, gd_input_w, gd_input_h);
                } else if (post_type == "yolo_npu") {
                    std::vector<std::vector<std::vector<float>>> anchors;
                    std::vector<float> stride;
                    if (!ReadYoloNpuPostParams(p, anchors, stride)) {
                        return Status(COSMO_NN_ERR_PARAM, "Invalid yolo_npu anchors/stride config");
                    }
                    output.op =
                        pipeline_utils::MakeYoloNpuPostOp(nms_thresh, conf_thresh, top_k, anchors, stride);
                } else {
                    output.op = pipeline_utils::MakeYoloPostOp(nms_thresh, conf_thresh, top_k, gd_input_w,
                                                               gd_input_h);
                }
            }
            model.output_node_infos.push_back(std::move(output));
        }
        model_info_.models.push_back(std::move(model));
    }

    if (!config.labels.empty() && !model_info_.models.empty()) {
        auto& last           = model_info_.models.back();
        std::string out_name = "output";
        DimsVector out_shape = {-1, -1, 6};
        if (!last.output_node_infos.empty())
            out_name = last.output_node_infos.front().name;
        BuildLabels(config, out_name, out_shape, model_info_);
    }

    InitThresholdsAndLabels();
    InitNetInputSize();
    return InitGraph(model_path, device_type, device_id, profiler, tokenizer_path, use_skip);
}

Status GenericDetectorPipeline::Forward(std::initializer_list<std::vector<std::shared_ptr<Blob>>> inputs) {
    return DetectionForward(
        this, inputs, image_sizes_,
        [this](std::initializer_list<std::vector<std::shared_ptr<Blob>>> inp) { return RunGraph(inp); });
}

Status GenericDetectorPipeline::ParseDetectionOutput(std::vector<std::vector<ObjectInfoV1>>& outputs) {
    return DetectionParseOutput(GetGraphOutput(), image_sizes_, net_input_size_, selected_indices_,
                                selected_thresholds_, selected_classnames_, outputs);
}

// ===================== Auto-Registration ==================================

REGISTER_MODEL_PIPELINE("yolov5_det", YoloV5DetPipeline);
REGISTER_MODEL_PIPELINE("yolov8_det", YoloV8DetPipeline);
REGISTER_MODEL_PIPELINE("yolov9_det", YoloV8DetPipeline);
REGISTER_MODEL_PIPELINE("yolov11_det", YoloV8DetPipeline);
REGISTER_MODEL_PIPELINE("yolov12_det", YoloV8DetPipeline);
REGISTER_MODEL_PIPELINE("yolo26_det", Yolo26DetPipeline);
REGISTER_MODEL_PIPELINE("yolov8_pose", YoloPosePipeline);
REGISTER_MODEL_PIPELINE("yolo11_pose", YoloPosePipeline);
REGISTER_MODEL_PIPELINE("yolo26_pose", YoloPosePipeline);
REGISTER_MODEL_PIPELINE("detector", GenericDetectorPipeline);

}  // namespace cosmo::nn
