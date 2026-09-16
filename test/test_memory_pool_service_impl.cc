#include <chrono>
#include <memory>
#include <thread>
#include <utility>

#include "catch_amalgamated.hpp"
#include "media/VideoFrame.h"
#include "mem/MemoryPoolMng.h"
#include "service/detail/ServiceRegistry.h"
#include "service/infra/impl/MemoryPoolServiceImpl.h"
#ifdef COSMO_NN_USE_SOPHON_BACKEND
#include "mem/DeviceContext.h"
#include "mem/IDeviceContext.h"
#include "support/ScopedServiceOverride.h"
#define MEMORY_POOL_TEST_TAGS "[MemoryPoolService][.device]"
#else
#define MEMORY_POOL_TEST_TAGS "[MemoryPoolService]"
#endif

using namespace cosmo::service;

namespace {

using namespace std::chrono_literals;

#ifdef COSMO_NN_USE_SOPHON_BACKEND
class ScopedDeviceContext {
public:
    ScopedDeviceContext()
        : device_context_(std::make_unique<cosmo::mem::DeviceContext>()), registration_(*device_context_) {}

    ScopedDeviceContext(const ScopedDeviceContext&)            = delete;
    ScopedDeviceContext& operator=(const ScopedDeviceContext&) = delete;

private:
    std::unique_ptr<cosmo::mem::DeviceContext> device_context_;
    cosmo::test::ScopedServiceOverride<cosmo::mem::IDeviceContext> registration_;
};
#endif

class MemoryPoolFixture {
public:
    MemoryPoolFixture() = default;

    MemoryPoolFixture(const MemoryPoolFixture&)            = delete;
    MemoryPoolFixture& operator=(const MemoryPoolFixture&) = delete;

private:
#ifdef COSMO_NN_USE_SOPHON_BACKEND
    ScopedDeviceContext device_context_;
#endif
    MemoryPoolServiceImpl service_;
};

size_t UsedBlockCount() {
    size_t count = 0;
    for (const auto& status : cosmo::mem::GetMemoryPool().Status()) {
        count += status.used_cnt;
    }
    return count;
}

struct RecycleBlock {
    void operator()(cosmo::mem::Block* block) const {
        cosmo::mem::GetMemoryPool().Recycle(block);
    }
};

std::unique_ptr<cosmo::mem::Block, RecycleBlock> AcquireBlock() {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto* block = cosmo::mem::GetMemoryPool().Acquire(1024)) {
            return std::unique_ptr<cosmo::mem::Block, RecycleBlock>(block);
        }
        std::this_thread::sleep_for(10ms);
    }
    return {};
}

struct FrameReleaseObservation {
    bool released             = false;
    size_t used_after_release = 0;
};

class FrameHolder {
public:
    FrameHolder(std::unique_ptr<cosmo::media::VideoFrame> frame, FrameReleaseObservation& observation)
        : frame_(std::move(frame)), observation_(observation) {}

    ~FrameHolder() {
        frame_.reset();
        observation_.used_after_release = UsedBlockCount();
        observation_.released           = true;
    }

private:
    std::unique_ptr<cosmo::media::VideoFrame> frame_;
    FrameReleaseObservation& observation_;
};

struct RegistryShutdown {
    ~RegistryShutdown() {
        ServiceRegistry::Instance().ShutdownAll();
    }
};

}  // namespace

TEST_CASE("MemoryPoolServiceImpl: construction installs a usable pool", MEMORY_POOL_TEST_TAGS) {
    MemoryPoolFixture fixture;
    REQUIRE_FALSE(cosmo::mem::GetMemoryPool().Status().empty());
    REQUIRE_FALSE(cosmo::mem::GetMemoryPool().OutputMallocBuf().empty());
}

TEST_CASE("MemoryPoolServiceImpl: global pool acquires and recycles blocks", MEMORY_POOL_TEST_TAGS) {
    MemoryPoolFixture fixture;
    const auto baseline = UsedBlockCount();
    auto block          = AcquireBlock();
    REQUIRE(block != nullptr);
    REQUIRE(block->size >= 1024);
    REQUIRE(UsedBlockCount() == baseline + 1);
    block.reset();
    REQUIRE(UsedBlockCount() == baseline);
    REQUIRE_NOTHROW(cosmo::mem::GetMemoryPool().Recycle(nullptr));
}

TEST_CASE("MemoryPoolServiceImpl: registry releases frames before pool and can restart",
          MEMORY_POOL_TEST_TAGS) {
    auto& registry = ServiceRegistry::Instance();
    REQUIRE(registry.Size() == 0);

    for (int round = 0; round < 2; ++round) {
        INFO("Lifecycle round " << round);
#ifdef COSMO_NN_USE_SOPHON_BACKEND
        ScopedDeviceContext device_context;
#endif
        FrameReleaseObservation observation;
        RegistryShutdown shutdown;
        registry.Register<MemoryPoolServiceImpl>(std::make_unique<MemoryPoolServiceImpl>());
        const auto baseline = UsedBlockCount();
        // Warm the smallest bucket with the same bounded retry as direct allocation.
        auto block = AcquireBlock();
        REQUIRE(block != nullptr);
        block.reset();
        REQUIRE(UsedBlockCount() == baseline);

        auto frame = std::make_unique<cosmo::media::VideoFrame>(16, 16);
        REQUIRE(frame->Active());
        REQUIRE(UsedBlockCount() == baseline + 1);
        registry.Register<FrameHolder>(std::make_unique<FrameHolder>(std::move(frame), observation));
        registry.CompleteRegistration();
        registry.ShutdownAll();

        REQUIRE(observation.released);
        REQUIRE(observation.used_after_release == baseline);
        REQUIRE(registry.Size() == 0);
    }
}

#undef MEMORY_POOL_TEST_TAGS
