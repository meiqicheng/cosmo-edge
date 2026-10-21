#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "catch_amalgamated.hpp"
#include "flow/task/PTaskBase.h"
#include "util/dto/ActionCodes.h"

using namespace cosmo;

namespace {

constexpr std::array<std::string_view, 5> kActionCodes{PADetect_Code, PAClassify_Code, PALandmark_Code,
                                                       PARecognizer_Code, PALogicalJudgment_Code};

ActionAlgPtr MakeAlgorithm(const std::string& flow_id = "flow-a") {
    auto algorithm     = std::make_shared<ActionAlg>();
    std::string parent = "-1";
    for (const auto code : kActionCodes) {
        ActionNode action;
        action.actionId        = code;
        action.flowActionId    = flow_id + "-" + std::to_string(algorithm->workFlow.size());
        action.preFlowActionId = parent;
        action.actionName      = "Original action";
        action.atomicCode      = "original-model";
        if (code == PALogicalJudgment_Code) {
            action.configObject.condition.type = LogicType::GE;
            action.configObject.condition.keyL = "aiOut.person.threshold";
            action.configObject.condition.keyR = "0.5";
        }
        parent = action.flowActionId;
        algorithm->workFlow.push_back(action);
    }
    return algorithm;
}

void CheckIdentity(const PActionBasePtr& instance, const std::string& task_id, const ActionNode& action) {
    REQUIRE(instance);
    CHECK(instance->GetTaskId() == task_id);
    CHECK(instance->GetActionId() == action.actionId);
    CHECK(instance->GetFlowActionId() == action.flowActionId);
    CHECK(instance->GetName() == action.actionName);
    CHECK(instance->GetAtomicCode() == action.atomicCode);
}

}  // namespace

TEST_CASE("PTaskBase owns distinct picture nodes for repeated action types",
          "[PicTaskService][PTaskOwnership]") {
    PTaskBase base;
    auto algorithm        = MakeAlgorithm();
    const auto other_flow = MakeAlgorithm("flow-b");
    algorithm->workFlow.insert(algorithm->workFlow.end(), other_flow->workFlow.begin(),
                               other_flow->workFlow.end());

    const auto task = base.TaskCreate("task-a", algorithm);
    REQUIRE(task);
    REQUIRE(task->actions.size() == 2 * kActionCodes.size());
    for (size_t i = 0; i < kActionCodes.size(); ++i) {
        CAPTURE(kActionCodes[i]);
        const auto& original = task->actions[i].actionInst;
        CheckIdentity(original, "task-a", algorithm->workFlow[i]);
        const auto& separate_flow = task->actions[i + kActionCodes.size()].actionInst;
        CheckIdentity(separate_flow, "task-a", other_flow->workFlow[i]);
        CHECK(separate_flow != original);
        for (size_t j = 0; j < i; ++j) {
            CHECK(task->actions[j].actionInst != original);
        }
    }

    // Validated workflows cannot reuse a flow ID, even for the same action type.
    algorithm->workFlow.push_back(algorithm->workFlow.front());
    REQUIRE_FALSE(base.TaskCreate("invalid-duplicate", algorithm));
}

TEST_CASE("PTaskBase never shares picture nodes between task objects", "[PicTaskService][PTaskOwnership]") {
    PTaskBase base;
    const auto first_algorithm     = MakeAlgorithm("bc");
    const auto first               = base.TaskCreate("a", first_algorithm);
    const auto repeated_id         = base.TaskCreate("a", first_algorithm);
    const auto collision_algorithm = MakeAlgorithm("c");
    const auto collision           = base.TaskCreate("ab", collision_algorithm);
    REQUIRE(first);
    REQUIRE(repeated_id);
    REQUIRE(collision);
    REQUIRE(first->actions.size() == kActionCodes.size());
    REQUIRE(repeated_id->actions.size() == kActionCodes.size());
    REQUIRE(collision->actions.size() == kActionCodes.size());

    // Concatenating taskId and flowActionId would yield "abc-0" for both tasks.
    for (size_t i = 0; i < kActionCodes.size(); ++i) {
        CAPTURE(kActionCodes[i]);
        CheckIdentity(first->actions[i].actionInst, "a", first_algorithm->workFlow[i]);
        CheckIdentity(repeated_id->actions[i].actionInst, "a", first_algorithm->workFlow[i]);
        CheckIdentity(collision->actions[i].actionInst, "ab", collision_algorithm->workFlow[i]);
        CHECK(first->actions[i].actionInst != repeated_id->actions[i].actionInst);
        CHECK(first->actions[i].actionInst != collision->actions[i].actionInst);
    }
}

TEST_CASE("Picture task deletion and direct release relinquish every node",
          "[PicTaskService][PTaskOwnership]") {
    PTaskBase base;
    auto algorithm = MakeAlgorithm();
    auto task      = base.TaskCreate("task-a", algorithm);
    REQUIRE(task);
    // Exercise repeated shared owners without passing an invalid graph to TaskCreate.
    {
        const auto duplicates = task->actions;
        task->actions.insert(task->actions.end(), duplicates.begin(), duplicates.end());
    }
    std::vector<std::weak_ptr<PActionBase>> nodes;
    for (const auto& action : task->actions) {
        REQUIRE(action.actionInst);
        nodes.push_back(action.actionInst);
    }

    SECTION("Explicit deletion releases all repeated action entries") {
        REQUIRE(base.TaskActionDestroy(task));
        REQUIRE(base.TaskDelete(task));
    }
    SECTION("Releasing a task without TaskDelete leaves no manager owner") {
        task.reset();
    }
    for (const auto& node : nodes) {
        CHECK(node.expired());
    }
}

TEST_CASE("Picture nodes honor an external shared owner after task deletion",
          "[PicTaskService][PTaskOwnership]") {
    PTaskBase base;
    auto task = base.TaskCreate("task-a", MakeAlgorithm());
    REQUIRE(task);
    REQUIRE_FALSE(task->actions.empty());
    auto survivor                   = task->actions.front().actionInst;
    std::weak_ptr<PActionBase> node = survivor;
    REQUIRE(base.TaskDelete(task));
    CHECK_FALSE(node.expired());
    CHECK(survivor->GetTaskId() == "task-a");
    survivor.reset();
    CHECK(node.expired());
}

TEST_CASE("Failed picture task creation retries with fresh node configuration",
          "[PicTaskService][PTaskOwnership]") {
    PTaskBase base;
    auto failed_algorithm  = MakeAlgorithm();
    auto& failed_condition = failed_algorithm->workFlow.back().configObject.condition;
    failed_condition.type  = LogicType::Less;
    failed_condition.keyL  = "aiOut.person.threshold";
    failed_condition.keyR  = "0.5";
    ActionNode unsupported;
    unsupported.actionId        = "unsupported-picture-action";
    unsupported.flowActionId    = "unsupported";
    unsupported.preFlowActionId = failed_algorithm->workFlow.back().flowActionId;
    failed_algorithm->workFlow.push_back(unsupported);
    REQUIRE_FALSE(base.TaskCreate("retry", failed_algorithm));

    auto retry_algorithm = std::make_shared<ActionAlg>(*failed_algorithm);
    retry_algorithm->workFlow.pop_back();
    for (auto& action : retry_algorithm->workFlow) {
        action.actionName = "Replacement action";
        action.atomicCode = "replacement-model";
    }
    retry_algorithm->workFlow.back().configObject.condition.type = LogicType::GE;
    const auto retried                                           = base.TaskCreate("retry", retry_algorithm);
    REQUIRE(retried);
    REQUIRE(retried->actions.size() == kActionCodes.size());
    for (size_t i = 0; i < kActionCodes.size(); ++i) {
        CAPTURE(kActionCodes[i]);
        CheckIdentity(retried->actions[i].actionInst, "retry", retry_algorithm->workFlow[i]);
    }

    // The replacement GE condition succeeds; the failed attempt's LT would fail.
    auto data                   = std::make_shared<AlgData>();
    data->chanDataDetect.detRet = std::make_shared<DataDetTrackClassify>();
    data->chanDataDetect.detRet->targets.emplace_back();
    data->chanDataDetect.detRet->targets.front().confidence.label      = "person";
    data->chanDataDetect.detRet->targets.front().confidence.confidence = 0.9F;
    REQUIRE(retried->actions.back().actionInst->HandPic(data) == util::ErrorEnum::Success);
    CHECK(data->chanDataDetect.detRet->targets.front().bLogicResult);
}
