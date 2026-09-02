// test_native_only_eligibility.cc — Validates Phase 1 (P1-1) native-only
// eligibility from the FULL task graph and task configuration.
//
// Covers:
//   1. TaskPipelineRequiresHostFrame: action-level full-graph evaluation
//      (detect-only eligible; any classify/track/alarm downstream ineligible).
//   2. TaskPipelineRequiresHostFrame: configuration-level evaluation — Phase 1
//      rejects ordinary areas, shielded areas, and line rules even when the
//      action graph is a pure detector.
//   3. The decoder latch follows the same evaluation, and removing the
//      requiring task restores native-only eligibility.
//   4. requires_host_frame flag on the registered task unit disables the
//      native distribution plan (already covered in test_alg_distributor.cc,
//      re-asserted here for the config dimension).

#include <memory>
#include <string>
#include <vector>

#include "catch_amalgamated.hpp"
#include "flow/channel/AlgChannel.h"
#include "flow/common/AlgDataQueueDistributor.h"
#include "flow/common/AlgTaskNativeCapability.h"
#include "flow/task/TaskBase.h"
#include "mock/MockServiceRegistry.h"
#include "util/dto/ActionCodes.h"
#include "util/dto/TaskAreaTypes.h"

namespace {

using cosmo::AlgChannel;
using cosmo::BAStreamChannel_Code;
using cosmo::MsgTaskArea;
using cosmo::MsgTaskConfig;
using cosmo::TaskAction;
using cosmo::TaskElement;
using cosmo::TaskPipelineRequiresHostFrame;

// Build a minimal task graph whose root is an AlgChannel and whose remaining
// actions are appended verbatim. The root carries the channel instance so the
// decoder latch can be observed.
struct TestTaskGraph {
    std::shared_ptr<TaskElement> task;
    AlgChannel* channel{nullptr};

    explicit TestTaskGraph(const std::string& task_id = "t1") {
        task            = std::make_shared<TaskElement>();
        task->channelId = "ch1";
        task->taskId    = task_id;

        TaskAction root;
        root.action.actionId     = std::string(BAStreamChannel_Code);
        root.action.flowActionId = "root-flow";
        root.actionInst          = std::make_shared<AlgChannel>("ch1", task_id, root.action);
        task->actions.push_back(root);
        channel = dynamic_cast<AlgChannel*>(root.actionInst.get());
    }

    void AddAction(const std::string& action_id, const std::string& flow_id, const std::string& pre_flow_id) {
        TaskAction action;
        action.action.actionId        = action_id;
        action.action.flowActionId    = flow_id;
        action.action.preFlowActionId = pre_flow_id;
        // The action instance is not needed for the eligibility evaluation.
        task->actions.push_back(action);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// 1. Full task graph evaluation (P1-1 requirement 2)
// ---------------------------------------------------------------------------

TEST_CASE("Native-only eligibility evaluates the full task graph, not only the root",
          "[flow][native-inference][eligibility]") {
    cosmo::test::MockServiceRegistry mocks;

    SECTION("pure detector pipeline is eligible") {
        TestTaskGraph graph;
        graph.AddAction(std::string(cosmo::AADetect_Code), "det-flow", "root-flow");
        CHECK_FALSE(TaskPipelineRequiresHostFrame(graph.task));
    }

    SECTION("classify downstream disables native-only") {
        TestTaskGraph graph;
        graph.AddAction(std::string(cosmo::AADetect_Code), "det-flow", "root-flow");
        graph.AddAction(std::string(cosmo::AAClassify_Code), "cls-flow", "det-flow");
        CHECK(TaskPipelineRequiresHostFrame(graph.task));
    }

    SECTION("track downstream disables native-only") {
        TestTaskGraph graph;
        graph.AddAction(std::string(cosmo::AADetect_Code), "det-flow", "root-flow");
        graph.AddAction(std::string(cosmo::AATrack_Code), "trk-flow", "det-flow");
        CHECK(TaskPipelineRequiresHostFrame(graph.task));
    }

    SECTION("unknown action codes fail closed") {
        TestTaskGraph graph;
        graph.AddAction(std::string(cosmo::AADetect_Code), "det-flow", "root-flow");
        graph.AddAction(std::string("ZZ_99999"), "zz-flow", "det-flow");
        CHECK(TaskPipelineRequiresHostFrame(graph.task));
    }
}

// ---------------------------------------------------------------------------
// 2. Task configuration dimension (P1-1 requirement 3)
// ---------------------------------------------------------------------------

TEST_CASE("Native-only eligibility rejects area and line-rule configuration",
          "[flow][native-inference][eligibility][config]") {
    cosmo::test::MockServiceRegistry mocks;

    auto make_graph = []() {
        auto graph = std::make_unique<TestTaskGraph>();
        graph->AddAction(std::string(cosmo::AADetect_Code), "det-flow", "root-flow");
        return graph;
    };

    SECTION("no configuration: eligible") {
        auto graph = make_graph();
        CHECK_FALSE(TaskPipelineRequiresHostFrame(graph->task));
    }

    SECTION("ordinary area disables native-only") {
        auto graph = make_graph();
        MsgTaskArea area;
        area.areaId = "a1";
        area.points = {cosmo::MsgPoint{0, 0}, cosmo::MsgPoint{100, 0}, cosmo::MsgPoint{100, 100},
                       cosmo::MsgPoint{0, 100}};
        graph->task->params.areas = {area};
        CHECK(TaskPipelineRequiresHostFrame(graph->task));
    }

    SECTION("shielded area disables native-only") {
        auto graph = make_graph();
        MsgTaskArea area;
        area.areaId = "shield-1";
        area.points = {cosmo::MsgPoint{10, 10}, cosmo::MsgPoint{20, 10}, cosmo::MsgPoint{20, 20},
                       cosmo::MsgPoint{10, 20}};
        graph->task->params.shieldedAreas = {area};
        CHECK(TaskPipelineRequiresHostFrame(graph->task));
    }

    SECTION("line rule disables native-only") {
        auto graph = make_graph();
        MsgTaskArea area;
        area.areaId               = "line-1";
        area.linePoints           = {cosmo::MsgPoint{0, 50}, cosmo::MsgPoint{100, 50}};
        graph->task->params.areas = {area};
        CHECK(TaskPipelineRequiresHostFrame(graph->task));
    }

    SECTION("removing the configuration restores eligibility") {
        auto graph = make_graph();
        MsgTaskArea area;
        area.areaId               = "a1";
        area.points               = {cosmo::MsgPoint{0, 0}};
        graph->task->params.areas = {area};
        CHECK(TaskPipelineRequiresHostFrame(graph->task));
        graph->task->params.areas.clear();
        CHECK_FALSE(TaskPipelineRequiresHostFrame(graph->task));
    }
}

// ---------------------------------------------------------------------------
// 3. Decoder latch follows the evaluation (P1-1 requirements 4/5/6)
// ---------------------------------------------------------------------------

TEST_CASE("Decoder latch follows eligibility and resets when the task is removed",
          "[flow][native-inference][eligibility][latch]") {
    cosmo::test::MockServiceRegistry mocks;

    SECTION("detect-only task leaves the latch off") {
        TestTaskGraph graph;
        graph.AddAction(std::string(cosmo::AADetect_Code), "det-flow", "root-flow");
        graph.channel->SetDecoderRequiresHostFrame(graph.task->taskId,
                                                   TaskPipelineRequiresHostFrame(graph.task));
        CHECK_FALSE(graph.channel->GetDecoderRequiresHostFrame());
    }

    SECTION("adding area configuration flips the latch on") {
        TestTaskGraph graph;
        graph.AddAction(std::string(cosmo::AADetect_Code), "det-flow", "root-flow");
        graph.channel->SetDecoderRequiresHostFrame(graph.task->taskId,
                                                   TaskPipelineRequiresHostFrame(graph.task));
        CHECK_FALSE(graph.channel->GetDecoderRequiresHostFrame());

        MsgTaskArea area;
        area.areaId              = "a1";
        area.points              = {cosmo::MsgPoint{0, 0}};
        graph.task->params.areas = {area};
        graph.channel->SetDecoderRequiresHostFrame(graph.task->taskId,
                                                   TaskPipelineRequiresHostFrame(graph.task));
        CHECK(graph.channel->GetDecoderRequiresHostFrame());
    }

    SECTION("removing the requiring task restores native-only eligibility") {
        TestTaskGraph graph("t1");
        graph.AddAction(std::string(cosmo::AADetect_Code), "det-flow", "root-flow");
        graph.channel->SetDecoderRequiresHostFrame(graph.task->taskId,
                                                   TaskPipelineRequiresHostFrame(graph.task));
        CHECK_FALSE(graph.channel->GetDecoderRequiresHostFrame());

        // A second task with area configuration keeps the latch on while active.
        graph.channel->SetDecoderRequiresHostFrame("t2", true);
        CHECK(graph.channel->GetDecoderRequiresHostFrame());

        // Removing the last requiring task restores eligibility.
        graph.channel->SetDecoderRequiresHostFrame("t2", false);
        CHECK_FALSE(graph.channel->GetDecoderRequiresHostFrame());
    }
}

// ---------------------------------------------------------------------------
// 4. requires_host_frame flag on the registered unit (P1-1 requirement 4)
// ---------------------------------------------------------------------------

TEST_CASE("Registered unit requires_host_frame disables the native distribution plan",
          "[flow][native-inference][eligibility][plan]") {
    using cosmo::AlgDataQueue;
    using cosmo::AlgDataQueueDistributor;
    using cosmo::AlgFrameDistributionPlan;
    using cosmo::AlgTaskUnit;

    cosmo::test::MockServiceRegistry mocks;

    AlgDataQueueDistributor distributor("eligibility_dist");
    auto queue = std::make_shared<AlgDataQueue<cosmo::AlgDataPtr>>("q", 20);

    AlgTaskUnit detect;
    detect.channel_id          = "ch1";
    detect.task_id             = "t1";
    detect.actionId            = std::string(cosmo::AADetect_Code);
    detect.flowActionId        = "det-flow";
    detect.que                 = queue;
    detect.requires_host_frame = false;
    REQUIRE(distributor.RegistProcQueue(detect));

    auto frame            = std::make_shared<cosmo::AlgData>();
    frame->firstTimePoint = std::chrono::steady_clock::now();
    auto plan             = distributor.PrepareFrameDistribution(frame);
    CHECK(plan.SupportsNativeInference());

    // Re-register the unit with requires_host_frame=true (the state TaskRegist
    // sets when the full task graph or configuration needs host pixels). The
    // plan must stop advertising native inference even though the action is a
    // plain detector.
    REQUIRE(distributor.RemoveProcQueue(detect));
    auto updated                = detect;
    updated.requires_host_frame = true;
    REQUIRE(distributor.RegistProcQueue(updated));

    frame->firstTimePoint = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    auto plan_after       = distributor.PrepareFrameDistribution(frame);
    CHECK_FALSE(plan_after.SupportsNativeInference());
}
