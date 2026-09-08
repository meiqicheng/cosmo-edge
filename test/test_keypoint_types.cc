#include "catch_amalgamated.hpp"

#include <limits>

#include "util/AiTypes.h"
#include "nn/utils/yolo_pose_decode.h"

TEST_CASE("YOLO pose decoder accepts channel-major and end-to-end layouts", "[nn][yolo][pose]") {
    using namespace cosmo::nn;
    std::string error;
    std::vector<ObjectInfoV1> outputs;
    std::vector<float> channel_major(56, 0.0f);
    channel_major[0] = 50.0f;
    channel_major[1] = 50.0f;
    channel_major[2] = 40.0f;
    channel_major[3] = 60.0f;
    channel_major[4] = 0.9f;
    channel_major[5] = 12.0f;
    channel_major[6] = 20.0f;
    channel_major[7] = 4.0f;
    REQUIRE(bool(DecodeYoloPoseTensor(channel_major.data(), {1, 56, 1}, 100, 100, 1, 17, 0.5f, outputs, error)));
    REQUIRE(outputs.size() == 1);
    CHECK(outputs[0].key_points.size() == 17);
    CHECK(outputs[0].key_point_confidences[0] == Catch::Approx(0.982f).margin(0.01f));

    std::vector<float> end_to_end(57, 0.0f);
    end_to_end[0] = 10.0f;
    end_to_end[1] = 10.0f;
    end_to_end[2] = 20.0f;
    end_to_end[3] = 20.0f;
    end_to_end[4] = 0.9f;
    end_to_end[5] = 0.0f;
    REQUIRE(bool(DecodeYoloPoseTensor(end_to_end.data(), {1, 1, 57}, 100, 100, 1, 17, 0.5f, outputs, error)));
    REQUIRE(outputs.size() == 1);
    CHECK(outputs[0].key_points.size() == 17);
}

TEST_CASE("Generic keypoint set represents human COCO pose", "[ai][keypoint]") {
    cosmo::AiKeypointSet set;
    set.kind = cosmo::AiKeypointKind::HumanPose;
    set.schema = "coco17";
    set.points.resize(17);
    for (int i = 0; i < 17; ++i) {
        set.points[static_cast<size_t>(i)] = {static_cast<float>(i), static_cast<float>(i + 1), 0.8f, -1.0f, i};
    }
    REQUIRE(set.points.size() == 17);
    CHECK(set.points[0].index == 0);
    CHECK(set.points[16].confidence == Catch::Approx(0.8f));
}

TEST_CASE("Generic keypoint set represents ordered license plate corners", "[ai][keypoint]") {
    cosmo::AiLandmarkData landmark;
    landmark.landmark = {{10, 20}, {110, 20}, {110, 60}, {10, 60}};
    landmark.keypoints.kind = cosmo::AiKeypointKind::LicensePlate;
    landmark.keypoints.schema = "plate4";
    for (size_t i = 0; i < landmark.landmark.size(); ++i) {
        const auto& point = landmark.landmark[i];
        landmark.keypoints.points.push_back(
            {static_cast<float>(point.x), static_cast<float>(point.y), 0.95f, -1.0f, static_cast<int>(i)});
    }
    REQUIRE(landmark.keypoints.points.size() == 4);
    CHECK(landmark.keypoints.points[0].x == Catch::Approx(10.0f));
    CHECK(landmark.keypoints.points[2].y == Catch::Approx(60.0f));
    CHECK(landmark.keypoints.schema == "plate4");
}

TEST_CASE("YOLO26 plate pose decoder preserves plate4 points without fake confidence", "[nn][yolo][plate]") {
    using namespace cosmo::nn;
    std::vector<float> tensor(14, 0.0F);
    tensor[0] = 10.0F;
    tensor[1] = 20.0F;
    tensor[2] = 110.0F;
    tensor[3] = 60.0F;
    tensor[4] = 0.9F;
    tensor[5] = 0.0F;
    tensor[6] = 10.0F;
    tensor[7] = 20.0F;
    tensor[8] = 110.0F;
    tensor[9] = 20.0F;
    tensor[10] = 110.0F;
    tensor[11] = 60.0F;
    tensor[12] = 10.0F;
    tensor[13] = 60.0F;
    std::vector<ObjectInfoV1> outputs;
    std::string error;
    REQUIRE(bool(DecodeYoloPoseTensor(tensor.data(), {1, 1, 14}, 200, 100, 1, 4, 0.5F, outputs, error, 2)));
    REQUIRE(outputs.size() == 1);
    REQUIRE(outputs[0].key_points.size() == 4);
    CHECK(outputs[0].key_points[0].first == Catch::Approx(10.0F));
    CHECK(outputs[0].key_points[2].second == Catch::Approx(60.0F));
    CHECK(outputs[0].key_point_confidences[0] == Catch::Approx(-1.0F));
}

TEST_CASE("YOLO26 plate decoder drops invalid boxes and sanitizes invalid points", "[nn][yolo][plate]") {
    using namespace cosmo::nn;
    std::vector<float> tensor(28, 0.0F);
    tensor[0] = 10.0F;
    tensor[1] = 20.0F;
    tensor[2] = 110.0F;
    tensor[3] = 60.0F;
    tensor[4] = 0.9F;
    tensor[5] = 0.0F;
    tensor[6] = std::numeric_limits<float>::quiet_NaN();
    tensor[7] = 20.0F;
    tensor[8] = 110.0F;
    tensor[9] = 20.0F;
    tensor[10] = 110.0F;
    tensor[11] = 60.0F;
    tensor[12] = 10.0F;
    tensor[13] = 60.0F;
    tensor[14] = 10.0F;
    tensor[15] = 20.0F;
    tensor[16] = 10.0F;
    tensor[17] = 20.0F;
    tensor[18] = 10.0F;
    tensor[19] = 20.0F;
    tensor[20] = 10.0F;
    tensor[21] = 20.0F;
    tensor[22] = 10.0F;
    tensor[23] = 20.0F;
    tensor[24] = 10.0F;
    tensor[25] = 20.0F;
    tensor[26] = 10.0F;
    tensor[27] = 20.0F;
    std::vector<ObjectInfoV1> outputs;
    std::string error;
    REQUIRE(bool(DecodeYoloPoseTensor(tensor.data(), {1, 2, 14}, 200, 100, 1, 4, 0.5F, outputs, error, 2)));
    REQUIRE(outputs.size() == 1);
    CHECK(outputs[0].key_point_confidences[0] == Catch::Approx(-1.0F));
}
