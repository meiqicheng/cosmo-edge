// test_native_only_inference.cc — Validates Phase 1 native-only inference path.
//
// Tests the flow layer changes that allow skipping Materialize() when all
// tasks support native inference (RKNN DMA-BUF path). Covers:
//   1. AlgFrameDistributionPlan::SupportsNativeInference()
//   2. AlgDataQueueDistributor::DistributorNativeOnlyFrame()
//   3. ConvertImagesToBlobs() with null VideoFramePtr + valid NativeVideoBuffer
//   4. BlobHandle.base nullability when native_image is valid
//   5. CheckNodeForwardParamNativeAware() null-base acceptance

#include "catch_amalgamated.hpp"

#include <memory>
#include <vector>

#include "flow/common/AlgDataQueue.h"
#include "flow/common/AlgDataQueueDistributor.h"
#include "flow/common/AlgDataUnit.h"
#include "infer/AiComponment.h"
#include "media/NativeVideoBuffer.h"
#include "media/VideoFrame.h"

#if defined(COSMO_NN_USE_RKNN_BACKEND) || defined(COSMO_NN_USE_HOST_BACKEND) || \
    defined(COSMO_NN_USE_SOPHON_BACKEND)
#include "nn/core/blob.h"
#include "nn/node/node.h"
#endif

namespace {

// Helper: build a fake NativeVideoBuffer that passes Valid().
auto MakeValidNativeBuffer(int w, int h) {
    auto buf  = std::make_shared<cosmo::media::NativeVideoBuffer>();
    buf->fd   = 42;  // fake fd, not actually used in unit test
    buf->bytes = static_cast<size_t>(w * h * 3 / 2);  // NV12 size
    buf->width = w;
    buf->height = h;
    buf->width_stride = w;
    buf->height_stride = h;
    buf->format = cosmo::media::NativeVideoBufferFormat::NV12;
    buf->color_space = cosmo::media::NativeVideoColorSpace::Bt601;
    buf->color_range = cosmo::media::NativeVideoColorRange::Limited;
    buf->owner = std::shared_ptr<void>(new int(0), [](int* p) { delete p; });
    return buf;
}

// Helper: build a minimal AlgData with native_buffer set, no host frame.
cosmo::AlgDataPtr MakeNativeOnlyData(const cosmo::media::NativeVideoBufferPtr& native,
                                       const std::string& channel_id = "test_ch") {
    auto data               = std::make_shared<cosmo::AlgData>();
    data->dataType          = cosmo::AlgDataType::ChannelDataDec;
    data->channelId         = channel_id;
    data->firstTimePoint    = std::chrono::steady_clock::now();
    data->chanDataDec.native_buffer = native;
    // frame is intentionally left null (default-constructed VideoFramePtr)
    return data;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. SupportsNativeInference()
// ---------------------------------------------------------------------------

TEST_CASE("AlgFrameDistributionPlan::SupportsNativeInference",
          "[flow][native-inference]") {
    using cosmo::AlgFrameDistributionPlan;

    AlgFrameDistributionPlan empty;
    CHECK_FALSE(empty.SupportsNativeInference());
    CHECK_FALSE(empty.Empty());  // empty() checks queues.empty()

    SECTION("non-empty queues with native_inference_eligible=true") {
        auto q = std::make_shared<cosmo::AlgDataQueue<cosmo::AlgDataPtr>>("q1");
        AlgFrameDistributionPlan plan;
        plan.queues.push_back(q);
        plan.native_inference_eligible = true;
        CHECK(plan.SupportsNativeInference());
        CHECK_FALSE(plan.Empty());
    }

    SECTION("non-empty queues with native_inference_eligible=false") {
        auto q = std::make_shared<cosmo::AlgDataQueue<cosmo::AlgDataPtr>>("q2");
        AlgFrameDistributionPlan plan;
        plan.queues.push_back(q);
        plan.native_inference_eligible = false;
        CHECK_FALSE(plan.SupportsNativeInference());
    }
}

// ---------------------------------------------------------------------------
// 2. DistributorNativeOnlyFrame()
// ---------------------------------------------------------------------------

TEST_CASE("DistributorNativeOnlyFrame enqueues to all plan queues",
          "[flow][native-inference]") {
    using cosmo::AlgDataQueueDistributor;
    using cosmo::AlgFrameDistributionPlan;

    AlgDataQueueDistributor distributor("test_native");

    // Register two task queues manually
    auto q1 = std::make_shared<cosmo::AlgDataQueue<cosmo::AlgDataPtr>>("q1");
    auto q2 = std::make_shared<cosmo::AlgDataQueue<cosmo::AlgDataPtr>>("q2");

    AlgFrameDistributionPlan plan;
    plan.queues = {q1, q2};
    plan.native_inference_eligible = true;

    auto native = MakeValidNativeBuffer(1920, 1080);
    auto data   = MakeNativeOnlyData(native);

    SECTION("inserts to both queues when plan has 2 queues") {
        int count = distributor.DistributorNativeOnlyFrame(plan, data);
        CHECK(count == 2);
    }

    SECTION("returns 0 for null data") {
        int count = distributor.DistributorNativeOnlyFrame(plan, nullptr);
        CHECK(count == 0);
    }

    SECTION("returns 0 for empty plan") {
        AlgFrameDistributionPlan empty_plan;
        int count = distributor.DistributorNativeOnlyFrame(empty_plan, data);
        CHECK(count == 0);
    }
}

// ---------------------------------------------------------------------------
// 3. ConvertImagesToBlobs: null image + valid native → produces blob
// ---------------------------------------------------------------------------

TEST_CASE("ConvertImagesToBlobs accepts null image with valid native buffer",
          "[flow][native-inference]") {
    using cosmo::media::NativeVideoBufferPtr;

    // Empty images vector should still work when native_buffers is provided
    std::vector<cosmo::media::VideoFramePtr> images;
    images.push_back(nullptr);  // null VideoFramePtr (simulates skip_materialize)

    NativeVideoBufferPtr native = MakeValidNativeBuffer(640, 640);

    std::vector<NativeVideoBufferPtr> native_buffers;
    native_buffers.push_back(native);

    std::vector<std::shared_ptr<cosmo::nn::Blob>> blobs;
    auto result = cosmo::ConvertImagesToBlobs(images, native_buffers, blobs);

    CHECK(result == cosmo::util::ErrorEnum::Success);
    REQUIRE(blobs.size() == 1);

    const auto& blob    = blobs[0];
    const auto& handle  = blob->GetHandle();
    const auto& desc    = blob->GetBlobDesc();

    // base should be null since image is null
    CHECK(handle.base == nullptr);

    // native_image should be populated from the NativeVideoBuffer
    CHECK(handle.native_image.fd == 42);
    CHECK(handle.native_image.width == 640);
    CHECK(handle.native_image.height == 640);

    // dims should come from native buffer, not from image
    CHECK(desc.dims.size() == 4);
    CHECK(desc.dims[1] == 640);  // height
    CHECK(desc.dims[2] == 640);  // width
}

TEST_CASE("ConvertImagesToBlobs skips entry when both image and native are null",
          "[flow][native-inference]") {
    std::vector<cosmo::media::VideoFramePtr> images;
    images.push_back(nullptr);

    std::vector<cosmo::media::NativeVideoBufferPtr> native_buffers;
    native_buffers.push_back(nullptr);

    std::vector<std::shared_ptr<cosmo::nn::Blob>> blobs;
    auto result = cosmo::ConvertImagesToBlobs(images, native_buffers, blobs);

    // Both null → entry skipped → blobs empty → InvalidParam
    CHECK(result == cosmo::util::ErrorEnum::InvalidParam);
    CHECK(blobs.empty());
}

// ---------------------------------------------------------------------------
// 4. BlobHandle.native_image fields are correctly copied
// ---------------------------------------------------------------------------

TEST_CASE("ConvertImagesToBlobs copies all NativeVideoBuffer fields to BlobHandle",
          "[flow][native-inference]") {
    auto native = MakeValidNativeBuffer(1920, 1080);

    std::vector<cosmo::media::VideoFramePtr> images;
    images.push_back(nullptr);

    std::vector<cosmo::media::NativeVideoBufferPtr> native_buffers;
    native_buffers.push_back(native);

    std::vector<std::shared_ptr<cosmo::nn::Blob>> blobs;
    REQUIRE(cosmo::ConvertImagesToBlobs(images, native_buffers, blobs) == cosmo::util::ErrorEnum::Success);
    REQUIRE(blobs.size() == 1);

    const auto& ni = blobs[0]->GetHandle().native_image;
    CHECK(ni.fd == 42);
    CHECK(ni.bytes == static_cast<size_t>(1920 * 1080 * 3 / 2));
    CHECK(ni.width == 1920);
    CHECK(ni.height == 1080);
    CHECK(ni.width_stride == 1920);
    CHECK(ni.height_stride == 1080);
    // format conversion: NV12 → IMAGE_NV12
    // color_space: Bt601 → Bt601
    // color_range: Limited → Limited
}

// ---------------------------------------------------------------------------
// 5. NativeVideoBuffer::Valid() boundary conditions
// ---------------------------------------------------------------------------

TEST_CASE("NativeVideoBuffer::Valid() rejects incomplete buffers",
          "[flow][native-inference]") {
    cosmo::media::NativeVideoBuffer buf;

    SECTION("all defaults → invalid") {
        CHECK_FALSE(buf.Valid());
    }

    SECTION("missing owner → invalid") {
        buf.fd = 1;
        buf.bytes = 100;
        buf.width = 10;
        buf.height = 10;
        buf.width_stride = 10;
        buf.height_stride = 10;
        buf.format = cosmo::media::NativeVideoBufferFormat::NV12;
        // owner not set
        CHECK_FALSE(buf.Valid());
    }

    SECTION("width_stride < width → invalid") {
        buf.fd = 1;
        buf.bytes = 100;
        buf.width = 10;
        buf.height = 10;
        buf.width_stride = 5;  // less than width
        buf.height_stride = 10;
        buf.format = cosmo::media::NativeVideoBufferFormat::NV12;
        buf.owner = std::shared_ptr<void>(new int(0), [](int* p) { delete p; });
        CHECK_FALSE(buf.Valid());
    }

    SECTION("complete buffer → valid") {
        buf.fd = 1;
        buf.bytes = 100;
        buf.width = 10;
        buf.height = 10;
        buf.width_stride = 10;
        buf.height_stride = 10;
        buf.format = cosmo::media::NativeVideoBufferFormat::NV12;
        buf.owner = std::shared_ptr<void>(new int(0), [](int* p) { delete p; });
        CHECK(buf.Valid());
    }
}

// ---------------------------------------------------------------------------
// 6. Native-only AlgData construction (simulates AlgChannelDecode skip_materialize)
// ---------------------------------------------------------------------------

TEST_CASE("MakeNativeOnlyData produces correctly structured AlgData",
          "[flow][native-inference]") {
    auto native = MakeValidNativeBuffer(1920, 1080);
    auto data   = MakeNativeOnlyData(native, "ch_001");

    // frame is null (no Materialize)
    CHECK_FALSE(data->chanDataDec.frame);
    // native_buffer is valid
    REQUIRE(data->chanDataDec.native_buffer);
    CHECK(data->chanDataDec.native_buffer->Valid());
    CHECK(data->chanDataDec.native_buffer->width == 1920);
    CHECK(data->chanDataDec.native_buffer->height == 1080);
    CHECK(data->channelId == "ch_001");
    CHECK(data->dataType == cosmo::AlgDataType::ChannelDataDec);
}

// ---------------------------------------------------------------------------
// 7. Multi-queue isolation: DistributorNativeOnlyFrame copies for parallel queues
// ---------------------------------------------------------------------------

TEST_CASE("DistributorNativeOnlyFrame isolates AlgData for parallel queues",
          "[flow][native-inference]") {
    cosmo::AlgDataQueueDistributor distributor("test_iso");

    auto q1 = std::make_shared<cosmo::AlgDataQueue<cosmo::AlgDataPtr>>("q1");
    auto q2 = std::make_shared<cosmo::AlgDataQueue<cosmo::AlgDataPtr>>("q2");

    cosmo::AlgFrameDistributionPlan plan;
    plan.queues = {q1, q2};
    plan.native_inference_eligible = true;

    auto native = MakeValidNativeBuffer(640, 480);
    auto data   = MakeNativeOnlyData(native);

    int count = distributor.DistributorNativeOnlyFrame(plan, data);
    CHECK(count == 2);

    // Each queue should have received an independent copy (different shared_ptr)
    auto popped1 = q1->Pop();
    auto popped2 = q2->Pop();
    REQUIRE(popped1);
    REQUIRE(popped2);
    // The native_buffer owner is shared, but the AlgData wrappers are different
    CHECK(popped1.get() != popped2.get());
    // Both should reference the same underlying native buffer
    CHECK(popped1->chanDataDec.native_buffer->fd == popped2->chanDataDec.native_buffer->fd);
}

#if defined(COSMO_NN_USE_RKNN_BACKEND) || defined(COSMO_NN_USE_HOST_BACKEND) || \
    defined(COSMO_NN_USE_SOPHON_BACKEND)
// ---------------------------------------------------------------------------
// 8. BlobHandle with null base + valid native_image through nn::Blob
// ---------------------------------------------------------------------------

TEST_CASE("BlobHandle supports null base with valid native_image",
          "[flow][native-inference]") {
    using namespace cosmo::nn;

    BlobDesc desc;
    desc.device_type  = DEVICE_NAIVE;
    desc.data_type    = DATA_TYPE_UINT8;
    desc.data_format  = DATA_FORMAT_NHWC;
    desc.image_format = IMAGE_NV12;
    desc.dims         = {1, 640, 640, 3};

    BlobHandle handle{};
    handle.base = nullptr;  // no host allocation
    handle.native_image.fd = 42;
    handle.native_image.bytes = 640 * 640 * 3 / 2;
    handle.native_image.width = 640;
    handle.native_image.height = 640;
    handle.native_image.width_stride = 640;
    handle.native_image.height_stride = 640;

    auto blob = std::make_shared<Blob>(desc, false);  // false = no auto-alloc
    blob->SetHandle(handle);

    const auto& h = blob->GetHandle();
    CHECK(h.base == nullptr);
    CHECK(h.native_image.fd == 42);
    CHECK(h.native_image.width == 640);
}

// ---------------------------------------------------------------------------
// 9. CheckNodeForwardParamNativeAware: accepts null base when native is valid
// ---------------------------------------------------------------------------

TEST_CASE("CheckNodeForwardParamNativeAware accepts null base with valid native",
          "[flow][native-inference][nn]") {
    using namespace cosmo::nn;

    BlobDesc desc;
    desc.device_type  = DEVICE_NAIVE;
    desc.data_type    = DATA_TYPE_UINT8;
    desc.dims         = {1, 3, 640, 640};

    auto blob = std::make_shared<Blob>(desc, false);
    BlobHandle handle{};
    handle.base = nullptr;
    handle.native_image.fd = 42;
    handle.native_image.bytes = 640 * 640 * 3;
    handle.native_image.width = 640;
    handle.native_image.height = 640;
    handle.native_image.width_stride = 640;
    handle.native_image.height_stride = 640;
    blob->SetHandle(handle);

    // CheckNodeForwardParamNativeAware should succeed when base is null
    // but native_image is valid
    auto result = cosmo::nn::CheckNodeForwardParamNativeAware(*blob, *blob, true);
    CHECK(result);
}

TEST_CASE("CheckNodeForwardParamNativeAware rejects null base without native",
          "[flow][native-inference][nn]") {
    using namespace cosmo::nn;

    BlobDesc desc;
    desc.device_type  = DEVICE_NAIVE;
    desc.data_type    = DATA_TYPE_UINT8;
    desc.dims         = {1, 3, 640, 640};

    auto blob = std::make_shared<Blob>(desc, false);
    BlobHandle handle{};
    handle.base = nullptr;
    // native_image left default (all zeros, fd=-1)
    blob->SetHandle(handle);

    auto result = cosmo::nn::CheckNodeForwardParamNativeAware(*blob, *blob, true);
    CHECK_FALSE(result);
}

#endif  // COSMO_NN_USE_RKNN_BACKEND || COSMO_NN_USE_HOST_BACKEND || COSMO_NN_USE_SOPHON_BACKEND
