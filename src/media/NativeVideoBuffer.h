#pragma once

#include <cstddef>
#include <limits>
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

    [[nodiscard]] bool Valid() const {
        return fd >= 0 && bytes > 0 && width > 0 && height > 0 && width_stride >= width &&
               height_stride >= height && format != NativeVideoBufferFormat::Unknown && owner;
    }
};

using NativeVideoBufferPtr = std::shared_ptr<const NativeVideoBuffer>;

/// Explicitly derived per-plane geometry of a compact 4:2:0 buffer.
///
/// Every consumer-relevant quantity is either recorded on the buffer or derived
/// here exactly once so callers never guess a layout:
///
/// - Y plane: `width_stride` bytes per row, `height_stride` allocated rows.
/// - NV12: one interleaved chroma plane directly after the Y plane, with
///   `width_stride` bytes per row and `height_stride / 2` allocated rows; within
///   each row the U byte of a 2x2 block sits at an even plane offset and the V
///   byte at the following odd offset. `u` and `v` therefore describe the same
///   interleaved plane; `u.visible_width` counts U,V pairs (`width / 2`).
/// - I420: U and V planes follow the Y plane, each with `width_stride / 2`
///   bytes per row and `height_stride / 2` allocated rows, V directly after U.
/// - Normal total capacity is 1.5 * (width_stride * height_stride) bytes.
///
/// NV21 is intentionally unsupported: derivation fails closed so producers keep
/// NV21 frames on the host materialization path instead of exporting a layout
/// that consumers would have to guess.
struct NativeVideoPlaneLayout {
    struct Plane {
        size_t offset{0};          // bytes from the start of the buffer
        size_t stride{0};          // bytes between consecutive plane rows
        int visible_width{0};      // columns consumed per row (U,V pairs for NV12)
        int visible_height{0};     // rows consumed
        size_t allocated_rows{0};  // rows available in the allocation
    };

    Plane y;
    Plane u;
    Plane v;
    size_t required_bytes{0};  // minimum buffer size described by this layout
};

/// Overflow-checked size_t multiplication. Returns false when `a * b` would wrap.
inline bool CheckedMulSize(size_t a, size_t b, size_t& result) {
    if (a != 0 && b > std::numeric_limits<size_t>::max() / a) {
        return false;
    }
    result = a * b;
    return true;
}

/// Derives the plane layout of a compact 4:2:0 buffer. Fails closed on
/// unsupported formats (including NV21), odd visible dimensions, odd or
/// undersized strides, and integer overflow.
inline bool DeriveNativeVideoPlaneLayout(NativeVideoBufferFormat format, int width, int height,
                                         int width_stride, int height_stride, NativeVideoPlaneLayout& out) {
    out = NativeVideoPlaneLayout{};
    if (format != NativeVideoBufferFormat::NV12 && format != NativeVideoBufferFormat::I420) {
        return false;
    }
    if (width <= 0 || height <= 0 || width % 2 != 0 || height % 2 != 0) {
        return false;
    }
    if (width_stride < width || height_stride < height || width_stride % 2 != 0 || height_stride % 2 != 0) {
        return false;
    }
    if (width > static_cast<int>(std::numeric_limits<int>::max() / 2) ||
        height > static_cast<int>(std::numeric_limits<int>::max() / 2) ||
        width_stride > static_cast<int>(std::numeric_limits<int>::max() / 2) ||
        height_stride > static_cast<int>(std::numeric_limits<int>::max() / 2)) {
        return false;
    }

    size_t y_bytes = 0;
    if (!CheckedMulSize(static_cast<size_t>(width_stride), static_cast<size_t>(height_stride), y_bytes)) {
        return false;
    }
    size_t u_plane_bytes = 0;
    if (!CheckedMulSize(static_cast<size_t>(width_stride / 2), static_cast<size_t>(height_stride / 2),
                        u_plane_bytes)) {
        return false;
    }

    out.y.offset         = 0;
    out.y.stride         = static_cast<size_t>(width_stride);
    out.y.visible_width  = width;
    out.y.visible_height = height;
    out.y.allocated_rows = static_cast<size_t>(height_stride);

    out.u.offset         = y_bytes;
    out.u.visible_width  = width / 2;
    out.u.visible_height = height / 2;
    out.u.allocated_rows = static_cast<size_t>(height_stride / 2);
    out.v.visible_width  = out.u.visible_width;
    out.v.visible_height = out.u.visible_height;
    out.v.allocated_rows = out.u.allocated_rows;
    if (format == NativeVideoBufferFormat::NV12) {
        // Single interleaved chroma plane: stride covers one U,V pair per
        // visible column, and V bytes live at odd plane offsets.
        out.u.stride = static_cast<size_t>(width_stride);
        out.v        = out.u;
    } else {
        out.u.stride = static_cast<size_t>(width_stride / 2);
        out.v.stride = out.u.stride;
        out.v.offset = y_bytes + u_plane_bytes;
    }

    const size_t chroma_bytes =
        format == NativeVideoBufferFormat::NV12 ? out.u.stride * out.u.allocated_rows : u_plane_bytes * 2;
    if (chroma_bytes > std::numeric_limits<size_t>::max() - y_bytes) {
        return false;
    }
    out.required_bytes = y_bytes + chroma_bytes;
    return true;
}

}  // namespace cosmo::media
