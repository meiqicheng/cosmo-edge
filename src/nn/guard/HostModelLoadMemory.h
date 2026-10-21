#pragma once

#include <cstdint>
#include <istream>
#include <optional>

namespace cosmo::nn {

// Headroom for the runtime's metadata and other work while a protected model's
// plaintext is resident. MemAvailable already accounts for reclaimable cache.
inline constexpr std::uint64_t kModelLoadHeadroomBytes = 128ULL * 1024 * 1024;

[[nodiscard]] std::optional<std::uint64_t> ReadHostAvailableMemory(std::istream& meminfo);

// The guard materializes a complete plaintext segment before loading BMRuntime.
// Use the artifact's on-disk size to budget that transient allocation.
// This is a conservative admission check, not a reservation of system RAM.
[[nodiscard]] bool HasHostModelLoadMemory(std::uint64_t artifact_bytes,
                                          std::uint64_t available_bytes) noexcept;

}  // namespace cosmo::nn
