#include "catch_amalgamated.hpp"
#include "service/system/impl/AcceleratorMetricsProvider.h"

#ifdef COSMO_NN_USE_RKNN_BACKEND
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

class ScopedNpuLoadFixture {
public:
    explicit ScopedNpuLoadFixture(const std::string& content)
        : path_(std::filesystem::temp_directory_path() /
                ("cosmo-rknpu-load-" + std::to_string(reinterpret_cast<uintptr_t>(this)))) {
        std::ofstream stream(path_);
        stream << content;
        stream.close();
        REQUIRE(stream.good());
        REQUIRE(setenv("COSMO_RKNPU_LOAD_PATH", path_.c_str(), 1) == 0);
    }

    ~ScopedNpuLoadFixture() {
        unsetenv("COSMO_RKNPU_LOAD_PATH");
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

private:
    std::filesystem::path path_;
};

}  // namespace
#endif

#ifdef COSMO_NN_USE_AXERA_BACKEND
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// Points an AXERA procfs node override at a temporary fixture file so the
// parser is exercised against captured real-device samples.
class ScopedAxNodeFixture {
public:
    ScopedAxNodeFixture(const char* env_name, const std::string& content)
        : env_name_(env_name),
          path_(std::filesystem::temp_directory_path() /
                ("cosmo-axnpu-" + std::to_string(reinterpret_cast<uintptr_t>(this)))) {
        std::ofstream stream(path_);
        stream << content;
        stream.close();
        REQUIRE(stream.good());
        REQUIRE(setenv(env_name_, path_.c_str(), 1) == 0);
    }

    ~ScopedAxNodeFixture() {
        unsetenv(env_name_);
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

private:
    const char* env_name_;
    std::filesystem::path path_;
};

}  // namespace
#endif

#ifdef COSMO_NN_USE_CPU_BACKEND
TEST_CASE("CPU accelerator metrics provider reports no accelerator", "[system][metrics]") {
    auto provider = cosmo::service::detail::CreateAcceleratorMetricsProvider();
    REQUIRE(provider != nullptr);

    const auto metrics = provider->QueryUtilization();
    CHECK(metrics.gpuusage == 0.0);
    CHECK(metrics.gpumemtotal == 0);
    CHECK(metrics.gpudevusage.empty());
    CHECK(metrics.memoryDomain == "none");
    CHECK(provider->QueryAvailableMemoryMB() == 0);
}
#endif

#ifdef COSMO_NN_USE_RKNN_BACKEND
TEST_CASE("RKNN accelerator metrics provider reports per-core busy-time load", "[system][metrics][rknn]") {
    ScopedNpuLoadFixture fixture("NPU load:  Core0:  9%, Core1:  42%,\n");
    auto provider = cosmo::service::detail::CreateAcceleratorMetricsProvider();
    REQUIRE(provider != nullptr);

    const auto metrics = provider->QueryUtilization();
    CHECK(metrics.gpuusageAvailable);
    CHECK(metrics.gpuusage == Catch::Approx(0.42));
    CHECK(metrics.utilizationMetric == "busy-time-load");
    CHECK(metrics.memoryDomain == "shared-system");
    REQUIRE(metrics.coreUtilizations.size() == 2);
    CHECK(metrics.coreUtilizations[0] == Catch::Approx(0.09));
    CHECK(metrics.coreUtilizations[1] == Catch::Approx(0.42));
    REQUIRE(metrics.gpudevusage.size() == 1);
    CHECK(metrics.gpudevusage.front().gpuusage == Catch::Approx(0.42));
}

TEST_CASE("RKNN accelerator metrics provider reports aggregate-only busy-time load",
          "[system][metrics][rknn]") {
    ScopedNpuLoadFixture fixture("NPU load:  37%\n");
    auto provider = cosmo::service::detail::CreateAcceleratorMetricsProvider();
    REQUIRE(provider != nullptr);

    const auto metrics = provider->QueryUtilization();
    CHECK(metrics.gpuusageAvailable);
    CHECK(metrics.gpuusage == Catch::Approx(0.37));
    CHECK(metrics.utilizationMetric == "busy-time-load");
    REQUIRE(metrics.coreUtilizations.size() == 1);
    CHECK(metrics.coreUtilizations.front() == Catch::Approx(0.37));
    REQUIRE(metrics.gpudevusage.size() == 1);
    CHECK(metrics.gpudevusage.front().gpuusage == Catch::Approx(0.37));
}

TEST_CASE("RKNN accelerator metrics provider rejects invalid load", "[system][metrics][rknn]") {
    ScopedNpuLoadFixture fixture("NPU load:  Core0: 105%, Core1: 0%,\n");
    auto provider = cosmo::service::detail::CreateAcceleratorMetricsProvider();
    REQUIRE(provider != nullptr);

    const auto metrics = provider->QueryUtilization();
    CHECK_FALSE(metrics.gpuusageAvailable);
    CHECK(metrics.gpuusage == 0.0);
    CHECK(metrics.coreUtilizations.empty());
}

TEST_CASE("RKNN accelerator metrics provider rejects invalid aggregate-only load",
          "[system][metrics][rknn]") {
    ScopedNpuLoadFixture fixture("NPU load: 101%\n");
    auto provider = cosmo::service::detail::CreateAcceleratorMetricsProvider();
    REQUIRE(provider != nullptr);

    const auto metrics = provider->QueryUtilization();
    CHECK_FALSE(metrics.gpuusageAvailable);
    CHECK(metrics.gpuusage == 0.0);
    CHECK(metrics.coreUtilizations.empty());
}
#endif

#ifdef COSMO_NN_USE_AXERA_BACKEND
TEST_CASE("AXERA accelerator metrics provider parses per-vNPU busy-time blocks", "[system][metrics][axera]") {
    const ScopedAxNodeFixture top_fixture("COSMO_AXNPU_TOP_PATH",
                                          "core:vnpu_1\ntime:9\nperiod:1000000\nutilization:9%\n\n"
                                          "core:vnpu_2\ntime:42\nperiod:1000000\nutilization:42%\n");
    const ScopedAxNodeFixture cmm_fixture(
        "COSMO_AXNPU_CMM_INFO_PATH",
        "---CMM_USE_INFO:\n total size=4194304KB(4096MB),used=283076KB(276MB + 452KB),"
        "remain=3911228KB(3819MB + 572KB),partition_number=1,block_number=36\n");
    auto provider = cosmo::service::detail::CreateAcceleratorMetricsProvider();
    REQUIRE(provider != nullptr);

    const auto metrics = provider->QueryUtilization();
    CHECK(metrics.gpuusageAvailable);
    CHECK(metrics.gpuusage == Catch::Approx(0.42));
    CHECK(metrics.utilizationMetric == "busy-time-load");
    CHECK(metrics.memoryDomain == "cmm");
    REQUIRE(metrics.coreUtilizations.size() == 2);
    CHECK(metrics.coreUtilizations[0] == Catch::Approx(0.09));
    CHECK(metrics.coreUtilizations[1] == Catch::Approx(0.42));
    REQUIRE(metrics.gpudevusage.size() == 1);
    CHECK(metrics.gpudevusage.front().gpuusage == Catch::Approx(0.42));
    CHECK(metrics.gpumemtotal == 4096);
    CHECK(metrics.gpumemavailable == 3819);
    CHECK(metrics.gpumemusage == Catch::Approx((4096.0 - 3819.0) / 4096.0));
    CHECK(provider->QueryAvailableMemoryMB() == 3819);
}

TEST_CASE("AXERA accelerator metrics provider parses the single vnpu-Non block", "[system][metrics][axera]") {
    // Captured on an AX650N (SDK V3.10.2) without vNPU partitioning.
    const ScopedAxNodeFixture top_fixture("COSMO_AXNPU_TOP_PATH",
                                          "core:vnpu-Non\ntime:2237\nperiod:1000000\nutilization:98%\n");
    auto provider = cosmo::service::detail::CreateAcceleratorMetricsProvider();
    REQUIRE(provider != nullptr);

    const auto metrics = provider->QueryUtilization();
    CHECK(metrics.gpuusageAvailable);
    CHECK(metrics.gpuusage == Catch::Approx(0.98));
    REQUIRE(metrics.coreUtilizations.size() == 1);
    CHECK(metrics.coreUtilizations.front() == Catch::Approx(0.98));
}

TEST_CASE("AXERA accelerator metrics provider treats disabled counters as unavailable",
          "[system][metrics][axera]") {
    const ScopedAxNodeFixture top_fixture("COSMO_AXNPU_TOP_PATH", "nputop info is not enabled!\n");
    auto provider = cosmo::service::detail::CreateAcceleratorMetricsProvider();
    REQUIRE(provider != nullptr);

    const auto metrics = provider->QueryUtilization();
    CHECK_FALSE(metrics.gpuusageAvailable);
    CHECK(metrics.gpuusage == 0.0);
    CHECK(metrics.coreUtilizations.empty());
}

TEST_CASE("AXERA accelerator metrics provider rejects invalid utilization", "[system][metrics][axera]") {
    const ScopedAxNodeFixture top_fixture("COSMO_AXNPU_TOP_PATH",
                                          "core:vnpu-Non\ntime:2237\nperiod:1000000\nutilization:105%\n");
    auto provider = cosmo::service::detail::CreateAcceleratorMetricsProvider();
    REQUIRE(provider != nullptr);

    const auto metrics = provider->QueryUtilization();
    CHECK_FALSE(metrics.gpuusageAvailable);
    CHECK(metrics.gpuusage == 0.0);
    CHECK(metrics.coreUtilizations.empty());
}
#endif
