// PQwen3VLWorker — Qwen3VL vision-language model wrapper for single-image analysis mode.

#include "flow/qwen3vl/PQwen3VLWorker.h"

#include <algorithm>

#include "flow/common/LlmYesNoJudge.h"
#include "flow/common/VisualRoi.h"
#include "flow/qwen3vl/OpenAiVlmClient.h"
#include "service/ai/ILlmInferService.h"
#include "service/detail/ServiceRegistry.h"
#include "service/media/IVideoFrameCodec.h"
#include "service/media/IVideoFrameOSD.h"
#include "service/media/IVideoFrameTransform.h"
#include "util/Log.h"
#include "util/SafeParse.h"
#include "util/StringUtil.h"
#include "util/UuidUtil.h"

namespace cosmo {

PQwen3VLWorker::PQwen3VLWorker(ActionNode& action, const std::string& task_id, bool has_upstream_targets)
    : PActionBase(action, task_id), prompt_(""), gen_param_({}), has_upstream_targets_(has_upstream_targets) {
    advanced_mode_    = false;
    generation_style_ = Qwen3VLGenerationStyle::STANDARD;
    for (auto& param : action.configObject.params) {
        AnalysisKey(param);
    }
    ApplyGenerationStyle();
    RebuildVisualJudgment();
    LOG_INFO("[{} {}] PQwen3VLWorker Init Prompt:{}", GetTaskId(), GetFlowActionId(), prompt_);
}

PQwen3VLWorker::~PQwen3VLWorker() {
    LOG_INFO("[{} {}] PQwen3VLWorker Stop & Delete", GetTaskId(), GetFlowActionId());
    PQwen3VLWorker::ActionDestroy();
}

bool PQwen3VLWorker::ActionInit() {
    std::lock_guard<std::shared_mutex> lock(mtx_);
    stopped_ = false;
    if (open_ai_config_.provider == "laya_v") {
        if (!visual_judgment_ || !visual_judgment_->Run()->Active())
            RebuildVisualJudgment();
        return true;
    }
    if (worker_registered_)
        return true;
    if (open_ai_config_.Enabled()) {
        LOG_INFO("[{} {}] PQwen3VLWorker using OpenAI VLM provider", GetTaskId(), GetFlowActionId());
        return true;
    }

    // Ensure shared LLM instance is loaded
    if (!service::ServiceRegistry::Instance().Get<service::ILlmInferService>().EnsureInit(GetAtomicCode())) {
        LOG_WARN("[{} {}] LlmInferService Init Failed", GetTaskId(), GetFlowActionId());
        return false;
    }
    // Participate in the reference-counted lifecycle so that the shared model is
    // only released when ALL users (video workers + single-image workers) are done.
    if (!worker_registered_) {
        service::ServiceRegistry::Instance().Get<service::ILlmInferService>().NotifyWorkerStart();
        worker_registered_ = true;
    }
    LOG_INFO("[{} {}] Init Qwen3VL Shared Sdk Success", GetTaskId(), GetFlowActionId());
    return true;
}

void PQwen3VLWorker::ActionDestroy() {
    std::lock_guard<std::shared_mutex> lock(mtx_);
    stopped_ = true;
    if (visual_judgment_)
        visual_judgment_->Invalidate();
    // Decrement the active worker count only if we successfully registered.
    // Without this guard, a failed ActionInit() (which never called
    // NotifyWorkerStart) followed by destruction would drive the count negative.
    if (worker_registered_) {
        worker_registered_ = false;
        service::ServiceRegistry::Instance().Get<service::ILlmInferService>().NotifyWorkerStop();
    }
}

bool PQwen3VLWorker::ValidKey(MsgDynamicKeyValue& param) {
    return !param.keys.empty() &&
           (param.keys[0] == "keywords" || param.keys[0] == "advanced_mode" ||
            param.keys[0] == "generationStyle" || param.keys[0] == "doSample" || param.keys[0] == "topK" ||
            param.keys[0] == "topP" || param.keys[0] == "temperature" || param.keys[0] == "vlmProvider" ||
            param.keys[0] == "provider" || param.keys[0] == "base_url" || param.keys[0] == "api_key" ||
            param.keys[0] == "model" || param.keys[0] == "endpoint" || param.keys[0] == "timeout_ms" ||
            param.keys[0] == "max_tokens" || param.keys[0] == "openai");
}

bool PQwen3VLWorker::AnalysisKey(MsgDynamicKeyValue& param) {
    if (param.key == "inputType") {
        target_input_ = param.value == "targets";
        return true;
    }
    if (param.key.ToRefString().rfind("visual.", 0) == 0) {
        UpdateVisualParameters(visual_parameters_, {param});
        return true;
    }
    auto parts = util::Split(param.key.ToRefString(), ".");
    param.keys.assign(parts.begin(), parts.end());
    if (!ValidKey(param))
        return false;

    if (param.keys[0] == "advanced_mode") {
        advanced_mode_ = (param.value == "1" || param.value == "true");
        LOG_INFO("[{} {}] param advanced_mode {}", GetTaskId(), GetFlowActionId(), advanced_mode_);
        return true;
    } else if (param.keys[0] == "keywords") {
        prompt_ = param.value;
        LOG_INFO("[{} {}] param keywords {}", GetTaskId(), GetFlowActionId(), prompt_);
        return true;
    } else if (param.keys[0] == "generationStyle") {
        if (param.value == "strict")
            generation_style_ = Qwen3VLGenerationStyle::RIGOROUS;
        else if (param.value == "standard")
            generation_style_ = Qwen3VLGenerationStyle::STANDARD;
        else if (param.value == "creative")
            generation_style_ = Qwen3VLGenerationStyle::DIVERGENT;
        else if (param.value == "custom")
            generation_style_ = Qwen3VLGenerationStyle::CUSTOM;
        LOG_INFO("[{} {}] param generationStyle {}", GetTaskId(), GetFlowActionId(), param.value);
        return true;
    } else if (param.keys[0] == "doSample") {
        gen_param_.do_sample = (param.value == "1" || param.value == "true");
        return true;
    } else if (param.keys[0] == "topK") {
        gen_param_.top_k = static_cast<int32_t>(util::ParseInt(param.value));
        return true;
    } else if (param.keys[0] == "topP") {
        gen_param_.top_p = util::ParseFloat(param.value);
        return true;
    } else if (param.keys[0] == "temperature") {
        gen_param_.temperature = util::ParseFloat(param.value);
        return true;
    } else if (param.keys[0] == "vlmProvider" || param.keys[0] == "provider" ||
               (param.keys.size() >= 2 && param.keys[0] == "openai" && param.keys[1] == "provider")) {
        open_ai_config_.provider = param.value;
        return true;
    } else if (param.keys[0] == "base_url" ||
               (param.keys.size() >= 2 && param.keys[0] == "openai" && param.keys[1] == "base_url")) {
        open_ai_config_.base_url = param.value;
        return true;
    } else if (param.keys[0] == "api_key" ||
               (param.keys.size() >= 2 && param.keys[0] == "openai" && param.keys[1] == "api_key")) {
        open_ai_config_.api_key = param.value;
        return true;
    } else if (param.keys[0] == "model" ||
               (param.keys.size() >= 2 && param.keys[0] == "openai" && param.keys[1] == "model")) {
        open_ai_config_.model = param.value;
        return true;
    } else if (param.keys[0] == "endpoint" ||
               (param.keys.size() >= 2 && param.keys[0] == "openai" && param.keys[1] == "endpoint")) {
        open_ai_config_.endpoint = param.value;
        return true;
    } else if (param.keys[0] == "timeout_ms" ||
               (param.keys.size() >= 2 && param.keys[0] == "openai" && param.keys[1] == "timeout_ms")) {
        open_ai_config_.timeout_ms = util::ParseInt(param.value, open_ai_config_.timeout_ms);
        return true;
    } else if (param.keys[0] == "max_tokens" ||
               (param.keys.size() >= 2 && param.keys[0] == "openai" && param.keys[1] == "max_tokens")) {
        open_ai_config_.max_tokens = util::ParseInt(param.value, open_ai_config_.max_tokens);
        return true;
    } else if (param.keys.size() >= 2 && param.keys[0] == "openai" && param.keys[1] == "temperature") {
        open_ai_config_.temperature = util::ParseFloat(param.value, open_ai_config_.temperature);
        return true;
    } else if (param.keys.size() >= 2 && param.keys[0] == "openai" && param.keys[1] == "top_p") {
        open_ai_config_.top_p = util::ParseFloat(param.value, open_ai_config_.top_p);
        return true;
    }

    return false;
}

void PQwen3VLWorker::ApplyGenerationStyle() {
    switch (generation_style_) {
        case Qwen3VLGenerationStyle::RIGOROUS:
            gen_param_.do_sample   = false;
            gen_param_.top_k       = 1;
            gen_param_.top_p       = 0.0f;
            gen_param_.temperature = 0.0f;
            break;
        case Qwen3VLGenerationStyle::STANDARD:
            gen_param_.do_sample   = true;
            gen_param_.top_k       = 20;
            gen_param_.top_p       = 0.5f;
            gen_param_.temperature = 0.5f;
            break;
        case Qwen3VLGenerationStyle::DIVERGENT:
            gen_param_.do_sample   = true;
            gen_param_.top_k       = 50;
            gen_param_.top_p       = 0.8f;
            gen_param_.temperature = 0.8f;
            break;
        case Qwen3VLGenerationStyle::CUSTOM:
            // Custom mode relies on externally-provided values; do not override
            break;
    }
}

bool PQwen3VLWorker::ModifyParam(const std::string& task_id, std::vector<MsgDynamicKeyValue>& params) {
    if (task_id != GetTaskId())
        return false;
    std::lock_guard<std::shared_mutex> lock(mtx_);
    parameters_applied_ = false;
    for (auto& param : params) {
        AnalysisKey(param);
    }
    ApplyGenerationStyle();
    RebuildVisualJudgment();
    return true;
}

bool PQwen3VLWorker::SetParam(const std::string& task_id, std::vector<MsgDynamicKeyValue>& params) {
    std::lock_guard<std::shared_mutex> lock(mtx_);
    if (task_id != GetTaskId())
        return false;
    std::map<std::string, std::string> parameterValues;
    for (const auto& param : params)
        parameterValues[param.key.ToString()] = param.value.ToString();
    if (parameters_applied_ && parameterValues == applied_parameters_)
        return true;
    applied_parameters_ = std::move(parameterValues);
    parameters_applied_ = true;
    visual_parameters_.clear();
    // Reset to default parameters
    prompt_           = "";
    target_input_     = false;
    advanced_mode_    = false;
    generation_style_ = Qwen3VLGenerationStyle::STANDARD;
    gen_param_        = {};
    open_ai_config_   = {};

    for (auto& param : params) {
        AnalysisKey(param);
    }
    ApplyGenerationStyle();
    RebuildVisualJudgment();
    return true;
}

void PQwen3VLWorker::RebuildVisualJudgment() {
    if (visual_judgment_)
        visual_judgment_->Invalidate();
    visual_judgment_.reset();
    if (worker_registered_ && (open_ai_config_.provider == "laya_v" || open_ai_config_.Enabled())) {
        service::ServiceRegistry::Instance().Get<service::ILlmInferService>().NotifyWorkerStop();
        worker_registered_ = false;
    }
    if (!stopped_ && open_ai_config_.provider == "laya_v")
        visual_judgment_ = std::make_shared<VisualJudgment>(
            GetTaskId(), prompt_, advanced_mode_, visual_parameters_, areas_, std::nullopt, GetAtomicCode());
}

bool PQwen3VLWorker::SetArea(const std::string& task_id, std::vector<MsgTaskArea>& areas,
                             std::vector<MsgTaskArea>& /*shielded_areas*/) {
    if (task_id != GetTaskId())
        return false;
    std::lock_guard<std::shared_mutex> lock(mtx_);
    if (nlohmann::json(areas_) == nlohmann::json(areas))
        return true;
    areas_ = areas;
    RebuildVisualJudgment();
    return true;
}

util::ErrorEnum PQwen3VLWorker::HandPic(AlgDataPtr alg_data) {
    if (!alg_data || !alg_data->chanDataDec.frame) {
        return util::ErrorEnum::InvalidParam;
    }

    OpenAiVlmConfig open_ai_config;
    std::shared_ptr<VisualJudgment> visual;
    std::vector<MsgTaskArea> areas;
    std::string prompt;
    std::string question;
    Qwen3VLGenerationParam gen_param;
    bool targets_input;
    {
        std::shared_lock<std::shared_mutex> lock(mtx_);
        if (stopped_)
            return util::ErrorEnum::ActionStop;
        open_ai_config = open_ai_config_;
        visual         = visual_judgment_;
        areas          = areas_;
        gen_param      = gen_param_;
        targets_input  = target_input_;
        question       = prompt_;
        prompt         = advanced_mode_
                             ? prompt_ + "，回答是或者否,不要换行，不要其他内容。"
                             : "判断图片中是否存在【" + prompt_ + "】目标，回答是或者否,不要换行，不要其他内容。";
    }
    if (open_ai_config.provider == "laya_v") {
        if (!visual || !visual->Run()->Active())
            return util::ErrorEnum::ActionStop;
        if (alg_data->visualFrameId.empty())
            alg_data->visualFrameId = util::GenerateUUID();
        VisualDecisionAudit audit{visual->Run(), {}};
        std::vector<AiDetectRstEl> retained;
        bool qualifiedFilter = false;
        for (const auto& roi : PrepareVisualRois(*alg_data, areas, has_upstream_targets_, true)) {
            auto result =
                visual->Decide(alg_data->visualFrameId, roi.roi_id, roi.area_id, [frame = roi.frame] {
                    service::VisualDecisionImage image;
                    if (!VideoFrameValid(frame))
                        return image;
                    image.width  = static_cast<int>(frame->GetWidth());
                    image.height = static_cast<int>(frame->GetHeight());
                    image.jpeg =
                        service::ServiceRegistry::Instance().Get<service::IVideoFrameCodec>().EncodeJpeg(
                            frame);
                    return image;
                });
            audit.records.push_back(VisualRoiRecord(roi, result, GetFlowActionId()));
            const auto decision = result.response.value("decision", nlohmann::json::object());
            if (decision.value("mode", "review") == "filter" && decision.value("business_qualified", false)) {
                qualifiedFilter = true;
            }
            if (result.Retain()) {
                AiDetectRstEl target;
                bool originalFound = false;
                if (alg_data->chanDataDetect.detRet && !roi.source_target_id.empty()) {
                    const auto& targets = alg_data->chanDataDetect.detRet->targets;
                    const auto original = std::find_if(targets.begin(), targets.end(), [&](const auto& item) {
                        return item.targetId == roi.source_target_id;
                    });
                    if (original != targets.end()) {
                        target        = *original;
                        originalFound = true;
                    }
                }
                if (!originalFound) {
                    target.box          = roi.roi;
                    target.bLogicResult = true;
                    target.targetId     = roi.source_target_id;
                    target.trackIdInfo  = roi.source_track_id;
                    target.trackId      = roi.source_track_index;
                    target.classifyRst.push_back({decision.value("verdict", "unknown"), "", 1.0f});
                }
                target.bLogicResult = true;
                target.areaSign.areas.clear();
                if (!roi.area_id.empty()) {
                    TargetAreaUnit area;
                    area.area_id          = roi.area_id;
                    const auto configured = std::find_if(areas.begin(), areas.end(), [&](const auto& value) {
                        return value.areaId == roi.area_id;
                    });
                    if (configured != areas.end())
                        area.area_name = configured->name;
                    target.areaSign.areas.push_back(std::move(area));
                }
                // The public picture response groups targets by areaSign. Keep
                // only accepted region memberships when several ROIs share a target.
                auto previous = std::find_if(retained.begin(), retained.end(), [&](const auto& item) {
                    return !target.targetId.empty() && item.targetId == target.targetId;
                });
                if (previous == retained.end())
                    retained.push_back(std::move(target));
                else
                    previous->areaSign.areas.insert(previous->areaSign.areas.end(),
                                                    target.areaSign.areas.begin(),
                                                    target.areaSign.areas.end());
            }
            if (result.audit.lease)
                audit.leases.push_back(result.audit.lease);
        }
        if (visual->Run()->CommitIfCurrent([&] {
                if (qualifiedFilter) {
                    alg_data->chanDataDetect.detRet          = std::make_shared<DataDetTrackClassify>();
                    alg_data->chanDataDetect.detRet->targets = std::move(retained);
                }
                alg_data->visualDecisions.push_back(std::move(audit));
            }))
            return util::ErrorEnum::Success;
        for (const auto& lease : audit.leases)
            lease->Seal("cancelled");
        return util::ErrorEnum::ActionStop;
    }

    if (!open_ai_config.Enabled() &&
        !service::ServiceRegistry::Instance().Get<service::ILlmInferService>().IsInitialized()) {
        if (!service::ServiceRegistry::Instance().Get<service::ILlmInferService>().EnsureInit(
                GetAtomicCode())) {
            return util::ErrorEnum::NotInit;
        }
    }

    if (!alg_data->chanDataDetect.detRet) {
        if (targets_input)
            return util::ErrorEnum::FlowDataInvalid;
        alg_data->chanDataDetect.detRet = std::make_shared<DataDetTrackClassify>();
    }
    auto& targets = alg_data->chanDataDetect.detRet->targets;
    if (!targets_input) {
        targets.clear();
        AiDetectRstEl target;
        target.targetId = GetFlowActionId() + ":0";
        targets.push_back(target);
    }
    auto& transform = service::ServiceRegistry::Instance().Get<service::IVideoFrameTransform>();
    for (auto& target : targets) {
        if (target.bFilter)
            continue;
        auto frame = alg_data->chanDataDec.frame;
        if (targets_input) {
            const int x = std::max(0, target.box.x), y = std::max(0, target.box.y);
            const int right = std::min(static_cast<int>(frame->GetWidth()), target.box.x + target.box.width);
            const int bottom =
                std::min(static_cast<int>(frame->GetHeight()), target.box.y + target.box.height);
            if (right <= x || bottom <= y)
                return util::ErrorEnum::InvalidParam;
            frame = transform.Crop(frame, util::Box(x, y, right - x, bottom - y));
        }
        if (!frame || !transform.EnsureHostData(frame))
            return util::ErrorEnum::InvalidParam;
        std::vector<Qwen3VLResult> results;
        const auto started = std::chrono::steady_clock::now();
        const auto status =
            open_ai_config.Enabled()
                ? OpenAiVlmClient::Generate(open_ai_config, {frame}, {prompt}, gen_param, results)
                : service::ServiceRegistry::Instance().Get<service::ILlmInferService>().Generate(
                      {frame}, {prompt}, gen_param, results);
        if (status != util::ErrorEnum::Success)
            return status;
        if (results.size() != 1)
            return util::ErrorEnum::FlowDataInvalid;
        const auto durationMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        const auto judge           = ParseJudgeYesNo(results.front().text);
        const std::string decision = judge == LlmJudgeYesNo::Unknown
                                         ? "unknown"
                                         : (judge == LlmJudgeYesNo::Yes ? "matched" : "not_matched");
        target.bLogicResult        = judge == LlmJudgeYesNo::Yes;
        AiConfidence confidence;
        confidence.label       = results.front().text;
        confidence.atomic_code = GetAtomicCode();
        confidence.confidence  = 1.0F;
        target.classifyRst.push_back(confidence);
        VlmResult answer;
        answer.nodeId     = GetFlowActionId();
        answer.targetId   = target.targetId;
        answer.modelCode  = open_ai_config.Enabled() ? open_ai_config.model : GetAtomicCode();
        answer.question   = question;
        answer.answer     = results.front().text;
        answer.decision   = decision;
        answer.durationMs = durationMs;
        answer.frameIndex = std::to_string(alg_data->chanDataDec.frame->GetFrameIndex());
        answer.timestamp  = std::to_string(alg_data->chanDataDec.frame->GetTimestamp());
        alg_data->vlmResults.push_back(std::move(answer));
        alg_data->pictureRules[GetFlowActionId()][target.targetId] = decision;
        alg_data->pictureDecisions[target.targetId]                = decision;
        if (!targets_input) {
            alg_data->pictureDecision                          = decision;
            alg_data->pictureRules[GetFlowActionId()]["image"] = decision;
        }
    }
    alg_data->bHaveLogic = true;

    return util::ErrorEnum::Success;
}

}  // namespace cosmo
