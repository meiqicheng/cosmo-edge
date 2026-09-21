#include "catch_amalgamated.hpp"

#ifdef COSMO_NN_USE_RKNN_BACKEND

// Tests for the detector input rectangle contract shared by the RKNN
// preprocessing path (RGA bound-input eligibility, normalize fast contract)
// and the net-node detector admission check. See
// docs: 检测输入矩形化分析-640宽高比优化.md.

#include "nn/device/rknn/rknn_net_node.h"

TEST_CASE("RKNN detector input height accepts stride-32 candidates", "[nn][rknn][detector-input]") {
    using namespace cosmo::nn;

    // Documented candidates for W=640 (all multiples of 32 within [352, 640]).
    CHECK(IsRknnDetectorInputHeight(640));  // square baseline
    CHECK(IsRknnDetectorInputHeight(480));  // 4:3 friendly
    CHECK(IsRknnDetectorInputHeight(448));
    CHECK(IsRknnDetectorInputHeight(416));  // mixed-aspect balance point
    CHECK(IsRknnDetectorInputHeight(384));  // 16:9 friendly
    CHECK(IsRknnDetectorInputHeight(352));  // minimum documented candidate

    // 640x360 is the exact 16:9 ratio but violates the stride-32 grid.
    CHECK_FALSE(IsRknnDetectorInputHeight(360));
    CHECK_FALSE(IsRknnDetectorInputHeight(368));

    // Outside the accepted height window.
    CHECK_FALSE(IsRknnDetectorInputHeight(320));
    CHECK_FALSE(IsRknnDetectorInputHeight(0));
    CHECK_FALSE(IsRknnDetectorInputHeight(-384));
    CHECK_FALSE(IsRknnDetectorInputHeight(672));

    // Off-grid values.
    CHECK_FALSE(IsRknnDetectorInputHeight(383));
    CHECK_FALSE(IsRknnDetectorInputHeight(385));
}

TEST_CASE("RKNN detector input size pins width at 640", "[nn][rknn][detector-input]") {
    using namespace cosmo::nn;

    CHECK(IsRknnDetectorInputSize(640, 640));
    CHECK(IsRknnDetectorInputSize(384, 640));
    CHECK(IsRknnDetectorInputSize(416, 640));
    CHECK(IsRknnDetectorInputSize(480, 640));

    // Width is fixed: other widths stay out of the detector fast contract.
    CHECK_FALSE(IsRknnDetectorInputSize(384, 512));
    CHECK_FALSE(IsRknnDetectorInputSize(288, 512));
    CHECK_FALSE(IsRknnDetectorInputSize(640, 384));  // swapped order rejected
    CHECK_FALSE(IsRknnDetectorInputSize(360, 640));
}

#endif  // COSMO_NN_USE_RKNN_BACKEND
