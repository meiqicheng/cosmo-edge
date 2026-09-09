#include "infer/AiPlateResultParser.h"
#include "infer/AiPlateRecognizerUnify.h"

#include "flow/common/AlgDetectTypes.h"
#include "flow/overview/OverviewRecordAiRst.h"
#include "service/detail/ServiceRegistry.h"
#include "service/system/IOverviewConfig.h"

// clang-format off
#include "catch_amalgamated.hpp"
// clang-format on

#include <limits>
#include <memory>

TEST_CASE("plate CTC parser keeps blank and collapses repeats") {
    const auto result = cosmo::AiPlateResultParser::Decode({1, 0, 0, 52, 52, 43, 43, 49}, 2, 0.9F);
    REQUIRE(result.text == "京A粤7");
    REQUIRE(result.color == "green");
    REQUIRE(result.color_score == Catch::Approx(0.9F));
}

TEST_CASE("plate color parser rejects invalid class") {
    REQUIRE(std::string(cosmo::AiPlateResultParser::ColorName(-1)) == "unknown");
    REQUIRE(std::string(cosmo::AiPlateResultParser::ColorName(5)) == "unknown");
}

TEST_CASE("plate parser rejects non-finite OCR and color logits", "[ai][plate]") {
    std::vector<float> ocr(78, 0.0F);
    ocr[3] = std::numeric_limits<float>::quiet_NaN();
    std::vector<int> indices;
    float number_score = 0.0F;
    std::string error;
    CHECK_FALSE(cosmo::AiPlateResultParser::DecodeOcr(ocr.data(), {1, 1, 78}, indices, number_score, error));
    CHECK(error.find("non-finite") != std::string::npos);

    const float colors[] = {0.0F, 1.0F, std::numeric_limits<float>::infinity(), 0.0F, 0.0F};
    int color_index = -1;
    float color_score = 0.0F;
    error.clear();
    CHECK_FALSE(cosmo::AiPlateResultParser::DecodeColor(colors, {1, 5}, color_index, color_score, error));
    CHECK(color_index == -1);
}

TEST_CASE("plate parser decodes end-to-end pose and multi-output heads", "[ai][plate]") {
    std::vector<float> pose(14, 0.0F);
    pose[0] = 10.0F; pose[1] = 20.0F; pose[2] = 110.0F; pose[3] = 60.0F; pose[4] = 0.9F;
    pose[6] = 10.0F; pose[7] = 20.0F; pose[8] = 110.0F; pose[9] = 20.0F;
    pose[10] = 110.0F; pose[11] = 60.0F; pose[12] = 10.0F; pose[13] = 60.0F;
    std::vector<cosmo::AiPlatePoseResult> poses;
    std::string error;
    REQUIRE(cosmo::AiPlateResultParser::DecodePose(pose.data(), {1, 1, 14}, 200, 100, 0.5F, poses, error));
    REQUIRE(poses.size() == 1);
    CHECK(poses[0].plate_type == 0);
    CHECK(poses[0].object.key_points.size() == 4);
    CHECK(poses[0].object.key_point_confidences[0] == Catch::Approx(-1.0F));

    std::vector<float> ocr(21 * 78, 0.0F);
    ocr[1 * 78 + 1] = 3.0F;
    ocr[2 * 78 + 1] = 4.0F;
    ocr[3 * 78 + 52] = 5.0F;
    std::vector<int> indices;
    float number_score = 0.0F;
    REQUIRE(cosmo::AiPlateResultParser::DecodeOcr(ocr.data(), {1, 21, 78}, indices, number_score, error));
    REQUIRE(indices == std::vector<int>{1, 52});

    const float colors[] = {0.0F, 1.0F, 5.0F, 0.0F, 0.0F};
    int color_index = -1;
    float color_score = 0.0F;
    REQUIRE(cosmo::AiPlateResultParser::DecodeColor(colors, {1, 5}, color_index, color_score, error));
    CHECK(color_index == 2);
    CHECK(color_score > 0.9F);
}

TEST_CASE("plate OCR decoder computes real number confidence", "[ai][plate]") {
    // Two non-blank steps with near-certain argmax -> number_score close to 1.
    std::vector<float> ocr(3 * 78, 0.0F);
    ocr[0 * 78 + 1]  = 10.0F;   // 京
    ocr[1 * 78 + 52] = 10.0F;   // A
    ocr[2 * 78 + 0]  = 10.0F;   // blank
    std::vector<int> indices;
    float number_score = 0.0F;
    std::string error;
    REQUIRE(cosmo::AiPlateResultParser::DecodeOcr(ocr.data(), {1, 3, 78}, indices, number_score, error));
    REQUIRE(indices == std::vector<int>{1, 52});
    CHECK(number_score > 0.99F);
    CHECK(number_score <= 1.0F);

    // Ambiguous logits -> lower confidence.
    std::vector<float> ocr2(2 * 78, 0.0F);
    ocr2[0 * 78 + 1]  = 1.0F;
    ocr2[1 * 78 + 52] = 1.0F;
    indices.clear();
    number_score = 0.0F;
    REQUIRE(cosmo::AiPlateResultParser::DecodeOcr(ocr2.data(), {1, 2, 78}, indices, number_score, error));
    REQUIRE(indices.size() == 2);
    CHECK(number_score > 0.0F);
    CHECK(number_score < 0.99F);
}

namespace {

class MockOverviewConfig : public cosmo::service::IOverviewConfig {
public:
    void SetOverviewStructureRecord(bool) override {}
    bool GetOverviewStructureRecord() override { return true; }
    void SetOverviewStructureFile(bool) override {}
    bool GetOverviewStructureFile() override { return false; }
    std::string GetTaskOverviewDataPath() override { return "output/agent-runs"; }
};

}  // namespace

TEST_CASE("plate number and color confidences propagate to overview targets", "[ai][plate][overview]") {
    MockOverviewConfig mock;
    cosmo::service::ServiceRegistry::Instance().Set<cosmo::service::IOverviewConfig>(&mock);

    auto frame = std::make_shared<cosmo::DataDetTrackClassify>();
    frame->streamIndex = 0;
    frame->frameIndex  = 0;
    frame->timestamp   = 1000;

    cosmo::AiDetectRstEl target;
    target.box = cosmo::util::Box(10, 20, 100, 40);
    target.confidence = {"plate", "9003001", 0.95F};
    target.ocrRst.push_back({"9003002", "京A12345", 0.87F});
    target.attrRst.push_back({"plateColor", "blue", "9003002", 0.92F});
    frame->targets.push_back(target);

    cosmo::OverviewRecordAiRst rec("task1", "test");
    rec.OverviewRecordFrame(frame);
    const auto info = rec.GetOverviewInfo();

    REQUIRE(info.aiFrames.size() == 1);
    REQUIRE(info.aiFrames[0].targets.size() == 1);
    const auto& t = info.aiFrames[0].targets[0];
    CHECK(t.ocrString == "京A12345");
    CHECK(t.ocrConfidence == Catch::Approx(0.87F));
    REQUIRE(t.attrs.size() == 1);
    CHECK(t.attrs[0].category == "plateColor");
    CHECK(t.attrs[0].label == "blue");
    CHECK(t.attrs[0].confidence == Catch::Approx(0.92F));

    cosmo::service::ServiceRegistry::Instance().Set<cosmo::service::IOverviewConfig>(nullptr);
}

TEST_CASE("plate recognizer exports the unified plate4 keypoint set", "[ai][keypoint][plate]") {
    cosmo::AiPlatePoseResult pose;
    pose.object.key_points = {{10.0F, 20.0F}, {110.0F, 20.0F}, {110.0F, 60.0F}, {10.0F, 60.0F}};
    pose.object.key_point_confidences = {-1.0F, -1.0F, -1.0F, -1.0F};
    const auto set = cosmo::AiPlateRecognizerUnify::ToKeypointSet(pose);
    CHECK(set.kind == cosmo::AiKeypointKind::LicensePlate);
    CHECK(set.coordinateSpace == cosmo::AiKeypointCoordinateSpace::Pixel);
    CHECK(set.schema == "plate4");
    REQUIRE(set.points.size() == 4);
    CHECK(set.points[0].index == 0);
    CHECK(set.points[3].x == Catch::Approx(10.0F));
    CHECK(set.points[3].confidence == Catch::Approx(-1.0F));
}
