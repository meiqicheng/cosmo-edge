#include <memory>
#include <string>
#include <vector>

#include "catch_amalgamated.hpp"
#include "infer/AiTrackerUnify.h"
#include "nn/utils/tracker_wrap.h"

namespace cosmo {
namespace {

    void ConfigureTracker(nn::TrackerWrap& tracker) {
        nn::Rect2f region(0.0f, 0.0f, 640.0f, 480.0f);
        REQUIRE(static_cast<int>(tracker.SetRegion(region)) == nn::COSMO_NN_OK);

        nn::TrackerConfig config;
        config.thresh     = {0.5f};
        config.thresh_low = {0.3f};
        config.max_age    = 2;
        config.min_hits   = 1;
        config.motion_iou = 0.0f;
        REQUIRE(static_cast<int>(tracker.SetTrackerConfig(config)) == nn::COSMO_NN_OK);
    }

    nn::TrackingBox MakeDetection(float confidence = 0.9f) {
        nn::TrackingBox detection;
        detection.class_id   = 1;
        detection.box        = nn::Rect2f(100.0f, 100.0f, 40.0f, 40.0f);
        detection.confidence = confidence;
        return detection;
    }

    std::vector<nn::TrackingBox> Trace(nn::TrackerWrap& tracker, const std::vector<nn::TrackingBox>& input) {
        std::vector<nn::TrackingBox> output;
        REQUIRE(static_cast<int>(tracker.Trace(input, output)) == nn::COSMO_NN_OK);
        return output;
    }

    int StartTrack(nn::TrackerWrap& tracker) {
        REQUIRE(Trace(tracker, {MakeDetection()}).empty());
        const auto output = Trace(tracker, {MakeDetection()});
        REQUIRE(output.size() == 1);
        REQUIRE(output.front().id >= 0);
        REQUIRE(output.front().status == nn::TrackingStatus::kTracking);
        return output.front().id;
    }

    void RequireTracking(const std::vector<nn::TrackingBox>& output, int id) {
        REQUIRE(output.size() == 1);
        REQUIRE(output.front().id == id);
        REQUIRE(output.front().class_id == 1);
        REQUIRE(output.front().status == nn::TrackingStatus::kTracking);
        REQUIRE(output.front().motion_state == nn::MotionState::kUncertain);
    }

    TEST_CASE("TrackerWrap initializes and preserves identity across confidence changes", "[nn][tracker]") {
        nn::TrackerWrap tracker;
        ConfigureTracker(tracker);
        const int id = StartTrack(tracker);

        RequireTracking(Trace(tracker, {MakeDetection()}), id);
        const auto low_confidence = Trace(tracker, {MakeDetection(0.4f)});
        RequireTracking(low_confidence, id);
        REQUIRE(low_confidence.front().confidence == Catch::Approx(0.4f));
        RequireTracking(Trace(tracker, {MakeDetection()}), id);
    }

    TEST_CASE("TrackerWrap reports two lost frames before retiring a track", "[nn][tracker]") {
        nn::TrackerWrap tracker;
        ConfigureTracker(tracker);
        const int id = StartTrack(tracker);

        for (int frame = 1; frame <= 2; ++frame) {
            CAPTURE(frame);
            const auto output = Trace(tracker, {});
            REQUIRE(output.size() == 1);
            REQUIRE(output.front().id == id);
            REQUIRE(output.front().status == nn::TrackingStatus::kLoss);
        }
        REQUIRE(Trace(tracker, {}).empty());
        REQUIRE(Trace(tracker, {}).empty());
    }

    TEST_CASE("TrackerWrap instances retain independent lifecycle and identity", "[nn][tracker]") {
        auto first = std::make_unique<nn::TrackerWrap>();
        nn::TrackerWrap second;
        ConfigureTracker(*first);
        ConfigureTracker(second);

        REQUIRE(Trace(*first, {MakeDetection()}).empty());
        REQUIRE(Trace(second, {MakeDetection()}).empty());
        const auto first_output  = Trace(*first, {MakeDetection()});
        const auto second_output = Trace(second, {MakeDetection()});
        REQUIRE(first_output.size() == 1);
        REQUIRE(second_output.size() == 1);
        const int first_id  = first_output.front().id;
        const int second_id = second_output.front().id;
        REQUIRE(first_id >= 0);
        REQUIRE(second_id >= 0);
        RequireTracking(first_output, first_id);
        RequireTracking(second_output, second_id);

        const auto lost = Trace(*first, {});
        REQUIRE(lost.size() == 1);
        REQUIRE(lost.front().status == nn::TrackingStatus::kLoss);
        RequireTracking(Trace(second, {MakeDetection(0.4f)}), second_id);
        RequireTracking(Trace(*first, {MakeDetection()}), first_id);

        first.reset();
        RequireTracking(Trace(second, {MakeDetection()}), second_id);
        first = std::make_unique<nn::TrackerWrap>();
        ConfigureTracker(*first);
        REQUIRE(Trace(*first, {MakeDetection()}).empty());
        RequireTracking(Trace(second, {MakeDetection()}), second_id);
        const auto restarted = Trace(*first, {MakeDetection()});
        REQUIRE(restarted.size() == 1);
        REQUIRE(restarted.front().id >= 0);
        RequireTracking(restarted, restarted.front().id);
        RequireTracking(Trace(second, {MakeDetection()}), second_id);
    }

    TEST_CASE("AiTrackerUnify preserves fire labels and atomic code through tracking states",
              "[infer][tracker]") {
        const std::string atomic_code = "2001003";
        AiTrackerUnify tracker(atomic_code, {"fire"}, {{"fire", atomic_code, 0.5f}}, 640, 480);
        REQUIRE(tracker.SetConfig(0.0f) == util::ErrorEnum::Success);

        AiDetectRstEl detection;
        detection.box.x      = 100;
        detection.box.y      = 100;
        detection.box.width  = 40;
        detection.box.height = 40;
        detection.confidence = {"fire", atomic_code, 0.9f};
        detection.targetId   = "fire-detection";

        auto trace = [&](std::vector<AiDetectRstEl> input) {
            std::vector<AiDetectRstEl> output;
            REQUIRE(tracker.Trace(input, output) == util::ErrorEnum::Success);
            return output;
        };
        REQUIRE(trace({detection}).empty());
        const auto tracked = trace({detection});
        REQUIRE(tracked.size() == 1);
        const int id = tracked.front().trackId;
        REQUIRE(id >= 0);

        auto require_state = [&](const std::vector<AiDetectRstEl>& output, AITrackingStatus status) {
            REQUIRE(output.size() == 1);
            REQUIRE(output.front().trackId == id);
            REQUIRE(output.front().classId == 1);
            REQUIRE(output.front().confidence.label == "fire");
            REQUIRE(output.front().confidence.atomic_code == atomic_code);
            REQUIRE(output.front().trackStatus == status);
            REQUIRE(output.front().motionStatus == AIMotionState::UNCERTAIN);
        };
        require_state(tracked, AITrackingStatus::TRACKING);
        REQUIRE(tracked.front().targetId == detection.targetId);
        detection.confidence.confidence = 0.4f;
        const auto low_confidence       = trace({detection});
        require_state(low_confidence, AITrackingStatus::TRACKING);
        REQUIRE(low_confidence.front().confidence.confidence == Catch::Approx(0.4f));
        require_state(trace({}), AITrackingStatus::LOSS);
        detection.confidence.confidence = 0.9f;
        require_state(trace({detection}), AITrackingStatus::TRACKING);
    }

}  // namespace
}  // namespace cosmo
