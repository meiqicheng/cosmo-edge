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
#include <cstdint>
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

/// Number of live Rockchip MPP decoders competing for the low-4G heap. Every
/// concurrent decoder adds an external frame group, an I420 copy-out target
/// and a bound input to the same, physically bounded ZONE_DMA supply, so the
/// consumers size their reservations from this count instead of hard-coding
/// single-channel figures.
uint32_t Dma32DecoderCount();

/// RAII seat in the decoder registry. Held by every Rockchip MPP decoder for
/// its lifetime; the count is read at decoder initialization time, when all
/// concurrently starting channels are already registered.
class Dma32DecoderSeat {
public:
    Dma32DecoderSeat();
    ~Dma32DecoderSeat();

    Dma32DecoderSeat(const Dma32DecoderSeat&)            = delete;
    Dma32DecoderSeat& operator=(const Dma32DecoderSeat&) = delete;
};

/// External frame-group buffers per decoder, scaled by the registered decoder
/// count: 1 -> 8, 2 -> 7, 3 -> 6, 4 or more -> 5. A 720p H.264 group shrinks
/// from ~14.7 MB (8 buffers) to ~11.1 MB (6), which is what lets three
/// concurrent decoders fit into the roughly 58 MB the heap can actually
/// serve, measured on the 8 GiB Debian 12 board after engine shutdown.
size_t Dma32DecoderGroupBufferCount();

/// Process-wide byte budget for every 32-bit-addressable allocation (frame
/// pools, MPP external frame groups and RKNN bound inputs all draw from the
/// same ledger). The low-4G zone on an 8 GiB board yields roughly 58 MB; the
/// default leaves headroom for the kernel and other users of that zone.
/// `COSMO_DMA32_BUDGET_MB` overrides the default.
size_t Dma32GlobalBudgetBytes();

/// A DMA-BUF allocated from a 32-bit-addressable dma-heap.
///
/// The buffer is published both as an fd (consumer: `importbuffer_fd` for RGA
/// and `rknn_create_mem_from_fd` for RKNPU2) and as a writable CPU mapping, so
/// it can stand in for `rknn_create_mem()` while remaining low-address.
///
/// Every allocation is charged to the global budget in `Allocate` and released
/// in `Release`, so all consumers share one reservation ledger and a burst of
/// concurrent startup allocations cannot oversubscribe the heap: a consumer
/// whose reservation is refused degrades to its previous, address-agnostic
/// strategy instead of failing mid-burst.
///
/// A @p critical allocation (RKNN bound inputs) may also draw from a small
/// reserved slice of the budget that non-critical consumers (frame pools,
/// external frame groups) can never touch. The bound input is what unlocks
/// the zero-copy RGA preprocessing path, and losing it costs a CPU resize on
/// every inference frame, so it must not lose an allocation race against the
/// pool's greedy async top-up.
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
    /// and the errno involved. A @p critical allocation may use the reserved
    /// budget slice that ordinary consumers cannot touch.
    bool Allocate(size_t bytes, std::string& reason, bool critical = false);

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
