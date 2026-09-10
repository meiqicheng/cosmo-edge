#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>

#include "nn/core/common.h"
#include "nn/core/macros.h"
#include "nn/utils/image_format_utils.h"

namespace cosmo::nn {

enum class NativeImageColorSpace {
    Unspecified = 0,
    Bt601,
    Bt709,
    Bt2020,
};

enum class NativeImageColorRange {
    Unspecified = 0,
    Limited,
    Full,
};

struct PUBLIC BlobDesc {
    DeviceType device_type = DEVICE_NAIVE;

    DataType data_type = DATA_TYPE_FLOAT;

    DataFormat data_format   = DATA_FORMAT_NCHW;
    ImageFormat image_format = IMAGE_UNKNOWN;

    DimsVector dims;

    std::string name = "";

    std::string description();
};

struct PUBLIC BlobHandle {
    void* base        = nullptr;
    unsigned long phy = 0;

    // Optional borrowed DMA-BUF image. The object supplying base owns the
    // descriptor lifetime; Blob does not close fd or alter that ownership.
    struct NativeImage {
        int fd{-1};
        size_t bytes{0};
        int width{0};
        int height{0};
        int width_stride{0};
        int height_stride{0};
        ImageFormat format{IMAGE_UNKNOWN};
        NativeImageColorSpace color_space{NativeImageColorSpace::Unspecified};
        NativeImageColorRange color_range{NativeImageColorRange::Unspecified};

        // Physical (device) address of the image when it lives in a hardware
        // buffer (e.g. AX650 VDEC/IVPS CMM pool) rather than a DMA-BUF fd.
        // Either fd >= 0 or phy != 0 must be set for the image to be valid.
        unsigned long phy{0};
        // Virtual (host) mapping of the same hardware buffer, when exposed.
        void* vir{nullptr};

        [[nodiscard]] bool Valid() const {
            const bool has_backing = fd >= 0 || phy != 0;
            return has_backing && bytes > 0 && width > 0 && height > 0 && width_stride >= width &&
                   height_stride >= height && format != IMAGE_UNKNOWN;
        }
    } native_image;
};

class BlobImpl;

class PUBLIC Blob {
public:
    explicit Blob(BlobDesc desc);

    Blob(BlobDesc desc, bool alloc_memory);

    Blob(BlobDesc desc, BlobHandle handle);

    ~Blob();

    BlobDesc& GetBlobDesc();

    void SetBlobDesc(BlobDesc desc);

    BlobHandle GetHandle();

    void SetHandle(BlobHandle handle);

    void ClearHandle();

private:
    std::unique_ptr<BlobImpl> impl;
};

using BlobMap = std::map<std::string, std::shared_ptr<Blob>>;

}  // namespace cosmo::nn
