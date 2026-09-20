/// @file DmaHeap32Buffer.h
/// @brief 32-bit-addressable dma-heap buffers for RGA targets.
///
/// RGA2 drives its own RGA_MMU, which cannot address memory above the 4 GiB
/// boundary. Every operation that only RGA2 can serve -- color fill, the
/// YUV422/420 planar formats, sub-68 px sources and scaling beyond 8x -- must
/// therefore be handed a buffer taken from a heap that guarantees low pages.
///
/// On a board with more than 4 GiB of RAM, `system`/`cma` are free to satisfy
/// allocations from ZONE_NORMAL, so the same request succeeds on a 4 GiB board
/// and fails on an 8 GiB one. The Rockchip RGA multicore driver (v1.3.x)
/// disables swiotlb and reports the mismatch as a plain failure instead of
/// silently slowing down, which is what makes this an availability problem.
/// A BSP that never registers a `*-dma32` heap leaves RGA2 unusable on such
/// boards; see `docs`/`RGA_ROOTCAUSE.md` for the measured evidence.
#pragma once

#include <cstddef>
#include <string>

namespace cosmo::media {

/// Heaps whose buffers are guaranteed to live below the 4 GiB boundary, in
/// preference order. `system-dma32` keeps the same cacheability as `system`,
/// so switching to it reproduces the allocation RKNPU2 would otherwise make.
inline constexpr const char* kDma32HeapCandidates[] = {"system-dma32", "system-uncached-dma32"};

/// Resolves the heap used for 32-bit-addressable allocations, probing once per
/// process. `COSMO_DMA32_HEAP` pins the choice explicitly and an empty value
/// disables the feature entirely. Returns an empty string when the running
/// kernel exposes no such heap, in which case callers must keep their previous,
/// address-agnostic allocation strategy.
const std::string& Dma32HeapName();

/// True when this board exposes a usable 32-bit-addressable dma-heap.
bool Dma32HeapAvailable();

/// A DMA-BUF allocated from a 32-bit-addressable dma-heap.
///
/// The buffer is published both as an fd (consumer: `importbuffer_fd` for RGA
/// and `rknn_create_mem_from_fd` for RKNPU2) and as a writable CPU mapping, so
/// it can stand in for `rknn_create_mem()` while remaining low-address.
class DmaHeap32Buffer {
public:
    DmaHeap32Buffer() = default;
    ~DmaHeap32Buffer();

    DmaHeap32Buffer(const DmaHeap32Buffer&)            = delete;
    DmaHeap32Buffer& operator=(const DmaHeap32Buffer&) = delete;
    DmaHeap32Buffer(DmaHeap32Buffer&& other) noexcept;
    DmaHeap32Buffer& operator=(DmaHeap32Buffer&& other) noexcept;

    /// Allocates at least @p bytes from the resolved heap, reusing an existing
    /// buffer that is already large enough. On failure @p reason names the heap
    /// and the errno involved.
    bool Allocate(size_t bytes, std::string& reason);

    /// Releases the mapping and the fd. Safe to call repeatedly or when the
    /// buffer was never allocated.
    void Release();

    [[nodiscard]] bool valid() const {
        return fd_ >= 0 && address_ != nullptr;
    }

    [[nodiscard]] int fd() const {
        return fd_;
    }

    [[nodiscard]] void* address() const {
        return address_;
    }

    [[nodiscard]] size_t bytes() const {
        return bytes_;
    }

    [[nodiscard]] const std::string& heap_name() const {
        return heap_name_;
    }

private:
    int fd_{-1};
    void* address_{nullptr};
    size_t bytes_{0};
    std::string heap_name_;
};

}  // namespace cosmo::media
