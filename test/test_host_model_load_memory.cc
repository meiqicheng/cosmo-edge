#include <limits>
#include <sstream>

#include "catch_amalgamated.hpp"
#include "nn/guard/HostModelLoadMemory.h"

using namespace cosmo::nn;

TEST_CASE("Protected model loading preserves host memory headroom", "[nn][model-guard-v2][memory]") {
    // A model-sized plaintext allocation must not start with less available RAM
    // than the artifact, even when the device still has ample NPU memory.
    constexpr std::uint64_t model_bytes = 1024ULL * 1024 * 1024;
    CHECK_FALSE(HasHostModelLoadMemory(model_bytes, 768ULL * 1024 * 1024));
    CHECK_FALSE(HasHostModelLoadMemory(model_bytes, model_bytes + kModelLoadHeadroomBytes - 1));
    CHECK(HasHostModelLoadMemory(model_bytes, model_bytes + kModelLoadHeadroomBytes));
    CHECK(HasHostModelLoadMemory(model_bytes, 1400ULL * 1024 * 1024));
    CHECK_FALSE(HasHostModelLoadMemory(1, kModelLoadHeadroomBytes - 1));
    CHECK_FALSE(HasHostModelLoadMemory(std::numeric_limits<std::uint64_t>::max(),
                                       std::numeric_limits<std::uint64_t>::max()));
}

TEST_CASE("Host model admission reads available RAM without counting swap", "[nn][model-guard-v2][memory]") {
    std::istringstream normal(
        "MemTotal: 2097152 kB\nMemFree: 131072 kB\nMemAvailable: 786432 kB\nSwapFree: 9000000 kB\n");
    REQUIRE(ReadHostAvailableMemory(normal) == 786432ULL * 1024);
    for (const char* text :
         {"MemFree: 999999 kB\n", "MemAvailable: -1 kB\n", "MemAvailable: 42 MB\n",
          "MemAvailable: 18446744073709551616 kB\n", "MemAvailable: 18446744073709551615 kB\n",
          "MemAvailable: 12foo kB\n", "MemAvailable: 12 kB extra\n"}) {
        DYNAMIC_SECTION(text) {
            std::istringstream malformed(text);
            CHECK_FALSE(ReadHostAvailableMemory(malformed).has_value());
        }
    }
    std::istringstream exhausted("MemAvailable: 0 kB\n");
    REQUIRE(ReadHostAvailableMemory(exhausted) == 0);
}
