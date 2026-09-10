#include <limits>
#include <utility>
#include <vector>

#include "catch_amalgamated.hpp"
#include "nn/utils/yolo_pose_decode.h"

namespace {

cosmo::nn::ObjectInfoV1 MakePoseObject(float x1, float y1, float x2, float y2,
                                       const std::vector<std::pair<float, float>>& points) {
    cosmo::nn::ObjectInfoV1 object;
    object.x1         = x1;
    object.y1         = y1;
    object.x2         = x2;
    object.y2         = y2;
    object.key_points = points;
    object.infos.push_back({0.9f, 0, "person"});
    return object;
}

}  // namespace

// Regression guard: pose tensors are decoded in model-input space. A 1080p frame
// resized to 640x640 must have its skeleton projected back onto the person, not
// left inside a 640x640 corner of the picture.
TEST_CASE("Pose projection maps a 640x640 detection onto a 1080p frame", "[nn][yolo][pose]") {
    using namespace cosmo::nn;
    std::vector<ObjectInfoV1> objects;
    objects.push_back(MakePoseObject(280.0f, 200.0f, 360.0f, 440.0f, {{320.0f, 320.0f}, {-1.0f, -1.0f}}));

    MapYoloPoseToFrame(objects, Size(640, 640), Size(1920, 1080), 0);

    REQUIRE(objects.size() == 1);
    // Stretch: 1920/640 = 3.0 on x, 1080/640 = 1.6875 on y.
    CHECK(objects[0].x1 == Catch::Approx(840.0f));
    CHECK(objects[0].x2 == Catch::Approx(1080.0f));
    CHECK(objects[0].y1 == Catch::Approx(337.5f));
    CHECK(objects[0].y2 == Catch::Approx(742.5f));
    // A person standing at the frame centre keeps that centre after projection.
    CHECK(objects[0].key_points[0].first == Catch::Approx(960.0f));
    CHECK(objects[0].key_points[0].second == Catch::Approx(540.0f));
    // The "joint not detected" sentinel must survive the projection, otherwise
    // hidden joints collapse onto the frame origin and fake skeleton edges appear.
    CHECK(objects[0].key_points[1].first == Catch::Approx(-1.0f));
    CHECK(objects[0].key_points[1].second == Catch::Approx(-1.0f));
}

TEST_CASE("Pose projection removes centered letterbox padding", "[nn][yolo][pose]") {
    using namespace cosmo::nn;
    std::vector<ObjectInfoV1> objects;
    // 1080p letterboxed into 640x640: scale 1/3, content height 360, pad_y 140.
    objects.push_back(MakePoseObject(280.0f, 200.0f, 360.0f, 440.0f, {{320.0f, 320.0f}}));

    MapYoloPoseToFrame(objects, Size(640, 640), Size(1920, 1080), 1);

    REQUIRE(objects.size() == 1);
    CHECK(objects[0].x1 == Catch::Approx(840.0f));
    CHECK(objects[0].y1 == Catch::Approx(180.0f));  // (200 - 140) * 3
    CHECK(objects[0].key_points[0].first == Catch::Approx(960.0f));
    CHECK(objects[0].key_points[0].second == Catch::Approx(540.0f));  // (320 - 140) * 3
}

TEST_CASE("Pose projection is a no-op when the frame already matches the net size", "[nn][yolo][pose]") {
    using namespace cosmo::nn;
    std::vector<ObjectInfoV1> objects;
    objects.push_back(MakePoseObject(10.0f, 20.0f, 30.0f, 40.0f, {{15.0f, 25.0f}}));

    MapYoloPoseToFrame(objects, Size(640, 640), Size(640, 640), 0);

    REQUIRE(objects.size() == 1);
    CHECK(objects[0].x1 == Catch::Approx(10.0f));
    CHECK(objects[0].key_points[0].second == Catch::Approx(25.0f));
}

TEST_CASE("Pose decoder marks joints it did not find with a negative sentinel", "[nn][yolo][pose]") {
    using namespace cosmo::nn;
    // One YOLOv8-pose candidate in channel-major layout: box in channels 0..3,
    // score in channel 4, then 17 keypoints of 3 values each from channel 5.
    std::vector<float> tensor(56, 0.0f);
    tensor[0] = 320.0f;                                   // cx
    tensor[1] = 320.0f;                                   // cy
    tensor[2] = 80.0f;                                    // w
    tensor[3] = 160.0f;                                   // h
    tensor[4] = 0.9f;                                     // score
    tensor[5] = 100.0f;                                   // keypoint 0 x
    tensor[6] = 200.0f;                                   // keypoint 0 y
    tensor[9] = std::numeric_limits<float>::quiet_NaN();  // keypoint 1 y -> not detected

    std::vector<ObjectInfoV1> outputs;
    std::string error;
    REQUIRE(bool(DecodeYoloPoseTensor(tensor.data(), {1, 56, 1}, YoloPoseLayout::kChannelMajor, 640, 640, 1,
                                      17, 0.5f, outputs, error)));
    REQUIRE(outputs.size() == 1);
    REQUIRE(outputs[0].key_points.size() == 17);
    CHECK(outputs[0].key_points[0].first == Catch::Approx(100.0f));
    // The missing joint must be negative, never (0, 0).
    CHECK(outputs[0].key_points[1].first == Catch::Approx(-1.0f));
    CHECK(outputs[0].key_points[1].second == Catch::Approx(-1.0f));
    CHECK(outputs[0].key_point_confidences[1] == Catch::Approx(-1.0f));
}
