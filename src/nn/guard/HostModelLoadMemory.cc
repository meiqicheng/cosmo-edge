#include "nn/guard/HostModelLoadMemory.h"

#include <limits>
#include <sstream>
#include <string>

namespace cosmo::nn {

std::optional<std::uint64_t> ReadHostAvailableMemory(std::istream& meminfo) {
    std::string line;
    while (std::getline(meminfo, line)) {
        std::istringstream fields(line);
        std::string key;
        fields >> key;
        if (key != "MemAvailable:") {
            continue;
        }
        std::string value;
        std::string unit;
        std::string extra;
        fields >> value >> unit;
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos || unit != "kB" ||
            (fields >> extra)) {
            return std::nullopt;
        }
        std::uint64_t kilobytes = 0;
        for (const char digit : value) {
            const auto number = static_cast<std::uint64_t>(digit - '0');
            if (kilobytes > (std::numeric_limits<std::uint64_t>::max() - number) / 10) {
                return std::nullopt;
            }
            kilobytes = kilobytes * 10 + number;
        }
        if (kilobytes > std::numeric_limits<std::uint64_t>::max() / 1024) {
            return std::nullopt;
        }
        return kilobytes * 1024;
    }
    return std::nullopt;
}

bool HasHostModelLoadMemory(std::uint64_t artifact_bytes, std::uint64_t available_bytes) noexcept {
    return available_bytes >= kModelLoadHeadroomBytes &&
           artifact_bytes <= available_bytes - kModelLoadHeadroomBytes;
}

}  // namespace cosmo::nn
