#pragma once

#include "nn/pipeline/model_pipeline.h"
#include "nn/utils/yolo_pose_decode.h"

namespace cosmo::nn {

class YoloV5DetPipeline : public ModelPipeline {
public:
    Status Init(const PipelineConfig& config, const std::string& model_path, DeviceType device_type,
                int device_id, IProfiler* profiler, const std::string& tokenizer_path,
                const std::string& word_table_path, bool use_skip) override;

    Status Forward(std::initializer_list<std::vector<std::shared_ptr<Blob>>> inputs) override;

    int GetMaxBatchSize() const override {
        return max_batch_;
    }
    std::string GetModelType() const override {
        return "yolov5_det";
    }
    OutputCategory GetOutputCategory() const override {
        return OutputCategory::DETECTION;
    }

    Status ParseDetectionOutput(std::vector<std::vector<ObjectInfoV1>>& outputs) override;

private:
    int max_batch_ = 1;
};

class YoloV8DetPipeline : public ModelPipeline {
public:
    Status Init(const PipelineConfig& config, const std::string& model_path, DeviceType device_type,
                int device_id, IProfiler* profiler, const std::string& tokenizer_path,
                const std::string& word_table_path, bool use_skip) override;

    Status Forward(std::initializer_list<std::vector<std::shared_ptr<Blob>>> inputs) override;

    int GetMaxBatchSize() const override {
        return max_batch_;
    }
    std::string GetModelType() const override {
        return "yolov8_det";
    }
    OutputCategory GetOutputCategory() const override {
        return OutputCategory::DETECTION;
    }

    Status ParseDetectionOutput(std::vector<std::vector<ObjectInfoV1>>& outputs) override;

private:
    int max_batch_ = 1;
};

class Yolo26DetPipeline : public ModelPipeline {
public:
    Status Init(const PipelineConfig& config, const std::string& model_path, DeviceType device_type,
                int device_id, IProfiler* profiler, const std::string& tokenizer_path,
                const std::string& word_table_path, bool use_skip) override;

    Status Forward(std::initializer_list<std::vector<std::shared_ptr<Blob>>> inputs) override;

    int GetMaxBatchSize() const override {
        return max_batch_;
    }
    std::string GetModelType() const override {
        return "yolo26_det";
    }
    OutputCategory GetOutputCategory() const override {
        return OutputCategory::DETECTION;
    }

    Status ParseDetectionOutput(std::vector<std::vector<ObjectInfoV1>>& outputs) override;

private:
    int max_batch_ = 1;
};

class YoloPosePipeline : public ModelPipeline {
public:
    Status Init(const PipelineConfig& config, const std::string& model_path, DeviceType device_type,
                int device_id, IProfiler* profiler, const std::string& tokenizer_path,
                const std::string& word_table_path, bool use_skip) override;
    Status Forward(std::initializer_list<std::vector<std::shared_ptr<Blob>>> inputs) override;
    int GetMaxBatchSize() const override {
        return max_batch_;
    }
    std::string GetModelType() const override {
        return "yolov8_pose";
    }
    OutputCategory GetOutputCategory() const override {
        return OutputCategory::DETECTION;
    }
    Status ParseDetectionOutput(std::vector<std::vector<ObjectInfoV1>>& outputs) override;

private:
    int max_batch_{1};
    int input_width_{640};
    int input_height_{640};
    int class_count_{1};
    int keypoint_count_{17};
    int keypoint_values_per_point_{3};
    AiKeypointKind keypoint_kind_{AiKeypointKind::Unknown};
    std::string keypoint_schema_;
    float confidence_threshold_{0.25f};
    float nms_threshold_{0.7f};
    int top_k_{300};
    // Tensor layout declared by the model template (decoder / keypoint_layout /
    // output_layout). Never inferred from the output shape, because v8/v11
    // channel-major heads and YOLO26 end-to-end heads can both produce channel
    // counts that accidentally satisfy the other layout's arithmetic.
    YoloPoseLayout pose_layout_{YoloPoseLayout::kChannelMajor};
    // True when the model already ran NMS internally (YOLO26 end-to-end). The
    // CPU-side NMS is then skipped; running it again would drop valid detections
    // that the model intentionally kept.
    bool nms_completed_{false};
    // Resize gravity used by the preprocessing resize op. It decides how the
    // model input maps back onto the original frame (0=stretch, 1=letterbox
    // centered, 2=letterbox top-left). Must mirror MakeDetPreprocess.
    int resize_gravity_{0};
};

class GenericDetectorPipeline : public ModelPipeline {
public:
    Status Init(const PipelineConfig& config, const std::string& model_path, DeviceType device_type,
                int device_id, IProfiler* profiler, const std::string& tokenizer_path,
                const std::string& word_table_path, bool use_skip) override;

    Status Forward(std::initializer_list<std::vector<std::shared_ptr<Blob>>> inputs) override;

    int GetMaxBatchSize() const override {
        return max_batch_;
    }
    std::string GetModelType() const override {
        return "detector";
    }
    OutputCategory GetOutputCategory() const override {
        return OutputCategory::DETECTION;
    }

    Status ParseDetectionOutput(std::vector<std::vector<ObjectInfoV1>>& outputs) override;

private:
    int max_batch_ = 1;
};

}  // namespace cosmo::nn
