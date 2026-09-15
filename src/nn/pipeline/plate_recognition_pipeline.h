#pragma once

#include <string>

#include "nn/pipeline/model_pipeline.h"

namespace cosmo::nn {

class PlateRecognitionPipeline : public ModelPipeline {
public:
    Status Init(const PipelineConfig& config, const std::string& model_path, DeviceType device_type,
                int device_id, IProfiler* profiler, const std::string& tokenizer_path,
                const std::string& word_table_path, bool use_skip) override;
    Status Forward(std::initializer_list<std::vector<std::shared_ptr<Blob>>> inputs) override;

    int GetMaxBatchSize() const override {
        return max_batch_;
    }
    std::string GetModelType() const override {
        return "plate_rec_color";
    }
    OutputCategory GetOutputCategory() const override {
        return OutputCategory::PLATE_RECOGNITION;
    }
    Status ParsePlateOutput(std::vector<PlateRecognitionResult>& outputs) override;

private:
    int max_batch_{1};
    int ctc_blank_index_{0};
    int ocr_class_count_{0};
    std::string ocr_output_name_;
    std::string color_output_name_;
    size_t ocr_output_index_{0};
    size_t color_output_index_{0};
    std::vector<std::string> color_labels_;
};

}  // namespace cosmo::nn
