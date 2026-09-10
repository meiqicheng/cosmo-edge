#pragma once

#include <cstddef>
#include <memory>

namespace cosmo::media {

enum class NativeVideoBufferFormat {
    Unknown = 0,
    I420,
    NV12,
    NV21,
};

enum class NativeVideoColorSpace {
    Unspecified = 0,
    Bt601,
    Bt709,
    Bt2020,
};

enum class NativeVideoColorRange {
    Unspecified = 0,
    Limited,
    Full,
};

/// Optional, backend-owned image buffer presented to a hardware consumer.
///
/// The file descriptor is borrowed from owner. Keeping this object alive keeps
/// the underlying decoder buffer referenced; consumers must not close fd.
struct NativeVideoBuffer {
    int fd{-1};
    size_t bytes{0};
    int width{0};
    int height{0};
    int width_stride{0};
    int height_stride{0};
    NativeVideoBufferFormat format{NativeVideoBufferFormat::Unknown};
    NativeVideoColorSpace color_space{NativeVideoColorSpace::Unspecified};
    NativeVideoColorRange color_range{NativeVideoColorRange::Unspecified};
    std::shared_ptr<void> owner;

    // Physical and virtual device addresses when the buffer lives in a
    // hardware pool (e.g. AX650 VDEC/IVPS CMM) rather than a DMA-BUF fd.
    // Either fd >= 0 or phy_addr != 0 must be set for the buffer to be valid.
    uint64_t phy_addr{0};
    uint64_t vir_addr{0};

    [[nodiscard]] bool Valid() const {
        const bool has_backing = fd >= 0 || phy_addr != 0;
        return has_backing && bytes > 0 && width > 0 && height > 0 && width_stride >= width &&
               height_stride >= height && format != NativeVideoBufferFormat::Unknown && owner;
    }
};

using NativeVideoBufferPtr = std::shared_ptr<const NativeVideoBuffer>;

}  // namespace cosmo::media
