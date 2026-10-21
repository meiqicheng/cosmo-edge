// AiClassifierForward.cc — Forward inference for AiClassifierUnify.
// Split from AiClassifierUnify.cc to reduce file size (DEBT-007).
//
// Every graph execution funnels through one of the three Classify/Forward
// entry points below, so the second-stage classifier is instrumented here:
// blob convert / graph forward / result parse are recorded per call together
// with the number of targets pushed into the graph. That makes the effective
// batch size (targets / calls) and the per-target cost directly observable in
// the accelerator metrics without touching the detector counters.

#include <chrono>

#include "infer/AiClassifierUnify.h"
#include "nn/core/inference_pipeline_metrics.h"
#include "util/Log.h"

namespace cosmo {
namespace {
    using MetricsClock = std::chrono::steady_clock;

    uint64_t ElapsedNanoseconds(MetricsClock::time_point started_at) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(MetricsClock::now() - started_at).count());
    }
}  // namespace

util::ErrorEnum AiClassifierUnify::Classify(const VideoFramePtr& image_base, const VideoFramePtr& image,
                                            AiDetectRstEl& io_rst) {
    if (!classifier_) {
        LOG_WARN("{}", "SDK Classifier Not Init");
        return util::ErrorEnum::NotInit;
    }
    if (!image_base->GetData() || !image->GetData()) {
        return util::ErrorEnum::Success;
    }

    auto& metrics           = cosmo::nn::GetInferencePipelineMetrics();
    const auto blob_started = MetricsClock::now();
    auto image_base_blob    = ConvertImageToBlob(image_base);
    auto image_blob         = ConvertImageToBlob(image);
    metrics.RecordClassifierBlobConvert(ElapsedNanoseconds(blob_started), 1);

    const auto forward_started = MetricsClock::now();
    metrics.RecordClassifierForwardCall(1);
    auto status = classifier_->Forward({{image_base_blob}, {image_blob}});
    metrics.RecordClassifierGraphForward(ElapsedNanoseconds(forward_started), 1, bool(status));
    if (!bool(status)) {
        LOG_ERRO("Forward Failed.({})", status.description());
        return util::ErrorEnum::AI_FORWARD_FAILED;
    }

    std::vector<std::vector<cosmo::nn::ObjectInfoV1>> outputs;
    const auto parse_started = MetricsClock::now();
    status                   = classifier_->ParseOutput<cosmo::nn::ObjectInfoV1>(outputs);
    metrics.RecordClassifierResultParse(ElapsedNanoseconds(parse_started), 1, bool(status));
    if (!bool(status)) {
        LOG_ERRO("ParseOutput Failed.({})", status.description());
        return util::ErrorEnum::AI_PARSE_OUTPUT_FAILED;
    }

    if (outputs.size() > 0) {
        std::vector<cosmo::nn::ObjectInfoV1> obj = outputs.at(0);
        if (!obj.empty()) {
            io_rst.box =
                util::Box(0, 0, static_cast<int>(image->GetWidth()), static_cast<int>(image->GetHeight()));
            io_rst.confidence.confidence = 1.0;
            auto classify_infos          = obj.at(0).infos;
            for (auto info : classify_infos) {
                AiConfidence conf_el;
                conf_el.atomic_code = atomic_code_;
                conf_el.confidence  = info.confidence;
                conf_el.label       = info.class_name;
                io_rst.classifyRst.push_back(conf_el);
            }
        }
    }

    return util::ErrorEnum::Success;
}

util::ErrorEnum AiClassifierUnify::Classify(const std::vector<VideoFramePtr>& images_base,
                                            const std::vector<VideoFramePtr>& images,
                                            std::vector<AiDetectRstEl>& io_rst) {
    if (!classifier_) {
        LOG_WARN("{}", "SDK Classifier Not Init");
        return util::ErrorEnum::NotInit;
    }

    if (images_base.empty()) {
        return util::ErrorEnum::Success;
    }

    if (images_base.size() != images.size()) {
        LOG_ERRO("{}", "Images Not Equal Base");
        return util::ErrorEnum::InvalidParam;
    }

    auto& metrics = cosmo::nn::GetInferencePipelineMetrics();
    const auto targets      = static_cast<uint64_t>(images_base.size());
    const auto blob_started = MetricsClock::now();
    std::vector<std::shared_ptr<cosmo::nn::Blob>> images_base_blobs{};
    auto ret = ConvertImagesToBlobs(images_base, images_base_blobs);
    if (util::ErrorEnum::Success != ret) {
        metrics.RecordClassifierBlobConvert(ElapsedNanoseconds(blob_started), targets);
        LOG_ERRO("ConvertImagesToBlobs (images_base) Failed. Ret:{}", ret);
        return ret;
    }

    std::vector<std::shared_ptr<cosmo::nn::Blob>> images_blobs{};
    ret = ConvertImagesToBlobs(images, images_blobs);
    metrics.RecordClassifierBlobConvert(ElapsedNanoseconds(blob_started), targets);
    if (util::ErrorEnum::Success != ret) {
        LOG_ERRO("ConvertImagesToBlobs (images) Failed. Ret:{}", ret);
        return ret;
    }

    const auto forward_started = MetricsClock::now();
    metrics.RecordClassifierForwardCall(targets);
    auto status = classifier_->Forward({images_base_blobs, images_blobs});
    metrics.RecordClassifierGraphForward(ElapsedNanoseconds(forward_started), targets, bool(status));
    if (!bool(status)) {
        LOG_ERRO("Forward Failed.({})", status.description());
        return util::ErrorEnum::AI_FORWARD_FAILED;
    }

    std::vector<std::vector<cosmo::nn::ObjectInfoV1>> outputs;
    const auto parse_started = MetricsClock::now();
    status                   = classifier_->ParseOutput<cosmo::nn::ObjectInfoV1>(outputs);
    metrics.RecordClassifierResultParse(ElapsedNanoseconds(parse_started), targets, bool(status));
    if (!bool(status)) {
        LOG_ERRO("ParseOutput Failed.({})", status.description());
        return util::ErrorEnum::AI_PARSE_OUTPUT_FAILED;
    }

    if (outputs.size() != images.size()) {
        LOG_ERRO("{}", "Outputs Size Not Equal Images Size.");
        return util::ErrorEnum::AI_CLASSIFY_FAILED;
    }

    for (size_t i = 0; i < outputs.size(); i++) {
        AiDetectRstEl rst_el;
        rst_el.box                               = util::Box(0, 0, static_cast<int>(images[i]->GetWidth()),
                                                             static_cast<int>(images[i]->GetHeight()));
        rst_el.confidence.confidence             = 1.0;
        std::vector<cosmo::nn::ObjectInfoV1> obj = outputs.at(i);
        if (obj.empty()) {
            io_rst.push_back(rst_el);
            continue;
        }
        auto classify_infos = obj.at(0).infos;
        for (auto info : classify_infos) {
            AiConfidence conf_el;
            conf_el.atomic_code = atomic_code_;
            conf_el.confidence  = info.confidence;
            conf_el.label       = info.class_name;
            rst_el.classifyRst.push_back(conf_el);
        }
        io_rst.push_back(rst_el);
    }

    return util::ErrorEnum::Success;
}

util::ErrorEnum AiClassifierUnify::Forward(const std::vector<VideoFramePtr>& images,
                                           const std::vector<std::vector<std::vector<int>>>& datas,
                                           std::vector<std::vector<cosmo::nn::ObjectInfoV1>>& outputs,
                                           bool use_box,
                                           const std::vector<media::NativeVideoBufferPtr>& native_buffers) {
    std::vector<std::shared_ptr<cosmo::nn::Blob>> image_blobs{};
    auto& metrics = cosmo::nn::GetInferencePipelineMetrics();
    // Effective batch size: one entry in `datas` per target pushed to the graph.
    uint64_t batch = 0;
    for (const auto& per_image : datas) {
        batch += static_cast<uint64_t>(per_image.size());
    }
    const auto blob_started = MetricsClock::now();
    auto ret                = ConvertImagesToBlobs(images, native_buffers, image_blobs);
    metrics.RecordClassifierBlobConvert(ElapsedNanoseconds(blob_started), batch);
    if (util::ErrorEnum::Success != ret) {
        LOG_ERRO("ConvertImagesToBlobs Failed. Ret:{}", ret);
        return ret;
    }

    std::vector<std::shared_ptr<cosmo::nn::Blob>> data_blobs{};
    if (use_box) {
        ret = ConvertDatasToBlobs(datas, data_blobs);
    } else {
        ret = ConvertDatasToBlobsFloat(datas, data_blobs);
    }
    if (util::ErrorEnum::Success != ret) {
        LOG_ERRO("ConvertDatasToBlobs Failed. Ret:{}", ret);
        return ret;
    }

    const auto forward_started = MetricsClock::now();
    metrics.RecordClassifierForwardCall(batch);
    auto status = classifier_->Forward({image_blobs, data_blobs});
    metrics.RecordClassifierGraphForward(ElapsedNanoseconds(forward_started), batch, bool(status));
    if (!bool(status)) {
        LOG_ERRO("Forward Failed.({})", status.description());
        return util::ErrorEnum::AI_FORWARD_FAILED;
    }

    const auto parse_started = MetricsClock::now();
    status                   = classifier_->ParseOutput<cosmo::nn::ObjectInfoV1>(outputs);
    metrics.RecordClassifierResultParse(ElapsedNanoseconds(parse_started), batch, bool(status));
    if (!bool(status)) {
        LOG_ERRO("ParseOutput Failed.({})", status.description());
        return util::ErrorEnum::AI_PARSE_OUTPUT_FAILED;
    }

    return util::ErrorEnum::Success;
}

util::ErrorEnum AiClassifierUnify::GetMaxBatchSize(size_t& value) const {
    if (!classifier_) {
        LOG_WARN("{}", "SDK Classifier Not Init");
        return util::ErrorEnum::NotInit;
    }
    value = static_cast<size_t>(classifier_->GetMaxBatchSize());
    return util::ErrorEnum::Success;
}

util::ErrorEnum AiClassifierUnify::GetAttributeCategory(AiDetectRstEl& io_rst) {
    for (auto classifyRst : io_rst.classifyRst) {
        cosmo::nn::CategoryInfo info;
        auto status = classifier_->GetCategoryInfo(classifyRst.label, info);
        if (!bool(status)) {
            LOG_ERRO("GetCategoryInfo Failed.({} : {}/{})", status.description(), classifyRst.atomic_code,
                     classifyRst.label);
            return util::ErrorEnum::Failed;
        }

        AiAttribute attr;
        attr.category    = info.class_name;
        attr.label       = classifyRst.label;
        attr.atomic_code = classifyRst.atomic_code;
        attr.confidence  = classifyRst.confidence;
        io_rst.attrRst.push_back(attr);
    }
    return util::ErrorEnum::Success;
}

util::ErrorEnum AiClassifierUnify::GetAttributeCategory(std::vector<AiDetectRstEl>& io_rst) {
    for (auto& rst : io_rst) {
        auto ret = GetAttributeCategory(rst);
        if (ret != util::ErrorEnum::Success) {
            return ret;
        }
    }
    return util::ErrorEnum::Success;
}

}  // namespace cosmo
