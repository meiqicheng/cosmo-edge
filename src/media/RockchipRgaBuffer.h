#pragma once

#include <rga/im2d.h>
#include <rga/im2d_version.h>

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <limits>
#include <mutex>
#include <vector>

extern "C" {
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
}

namespace cosmo::media {

/// Absolute path to the physically-low (< 4 GiB) dma32 heap. RGA's MMU
/// color-convert/import path cannot map buffers backed by upper-RAM physical
/// pages, so hot RGA inputs/outputs that come from process heap memory are
/// staged here instead.
inline constexpr const char* kDma32HeapPath = "/dev/dma_heap/system-uncached-dma32";

/// Monotonic epoch bumped whenever a Rockchip decoder (re)creates or clears
/// its MPP DMA-BUF buffer group (decoder open/close and stream-resolution
/// changes). RGA fd-handle caches key on it so stale handles for closed
/// dma-bufs are dropped before a reconnecting decoder's reused fd numbers can
/// resolve to a wrong buffer.
inline std::atomic<uint64_t>& RockchipDecoderEpoch() {
    static std::atomic<uint64_t> epoch{0};
    return epoch;
}

/// Invalidates every RGA fd-handle cache in the process. Call after the
/// decoder's buffer group is destroyed or cleared (never while a frame from
/// the old group is still being processed — the decoder call sites satisfy
/// this because frame flow has already stopped at that point).
inline void BumpRockchipDecoderEpoch() {
    RockchipDecoderEpoch().fetch_add(1, std::memory_order_acq_rel);
}

// librga's scheduler is not thread-safe: concurrent RGA hardware calls from
// multiple threads corrupt job/fence state and fail intermittently (verified by
// an on-device probe: 4-thread improcess 298/400 fail-free, 400/400 when
// serialized). All RGA calls in the process must share one lock.
inline std::mutex& RgaGlobalLock() {
    static std::mutex lock;
    return lock;
}

/// One imported RGA buffer handle with deterministic release semantics.
///
/// The wrapper is intentionally backend-wide rather than SoC-specific. It is
/// used for MPP DMA-BUFs, RKNN DMA-BUFs, and the existing host-frame boundary.
class ScopedRgaBufferHandle {
public:
    ScopedRgaBufferHandle() = default;

    ScopedRgaBufferHandle(void* address, size_t bytes) {
        ImportVirtual(address, bytes);
    }

    ~ScopedRgaBufferHandle() {
        Reset();
    }

    ScopedRgaBufferHandle(const ScopedRgaBufferHandle&)            = delete;
    ScopedRgaBufferHandle& operator=(const ScopedRgaBufferHandle&) = delete;

    ScopedRgaBufferHandle(ScopedRgaBufferHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = 0;
    }

    ScopedRgaBufferHandle& operator=(ScopedRgaBufferHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_       = other.handle_;
            other.handle_ = 0;
        }
        return *this;
    }

    bool ImportVirtual(void* address, size_t bytes) {
        if (handle_ != 0 || !address || !ValidSize(bytes)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(RgaGlobalLock());
        handle_ = importbuffer_virtualaddr(address, static_cast<int>(bytes));
        return handle_ != 0;
    }

    bool ImportFd(int fd, size_t bytes) {
        if (handle_ != 0 || fd < 0 || !ValidSize(bytes)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(RgaGlobalLock());
        handle_ = importbuffer_fd(fd, static_cast<int>(bytes));
        return handle_ != 0;
    }

    void Reset() {
        if (handle_ != 0) {
            std::lock_guard<std::mutex> lock(RgaGlobalLock());
            releasebuffer_handle(handle_);
            handle_ = 0;
        }
    }

    [[nodiscard]] rga_buffer_handle_t Get() const {
        return handle_;
    }

    [[nodiscard]] explicit operator bool() const {
        return handle_ != 0;
    }

private:
    static bool ValidSize(size_t bytes) {
        return bytes > 0 && bytes <= static_cast<size_t>(std::numeric_limits<int>::max());
    }

    rga_buffer_handle_t handle_{0};
};

inline bool RockchipRgaSucceeded(IM_STATUS status) {
    return status == IM_STATUS_SUCCESS || status == IM_STATUS_NOERROR;
}

/// Bounded LRU cache of imported RGA handles keyed by (fd, bytes).
///
/// MPP decoders hand out frames from a fixed buffer pool, so the same small
/// set of DMA-BUF fds recurs every frame. Re-importing each frame costs an
/// importbuffer_fd + releasebuffer_handle ioctl pair that maps/unmaps the
/// whole buffer in the RGA IOMMU while holding RgaGlobalLock — historically
/// the dominant serialization point of the native fast path. A handle is a
/// pure address mapping: it stays valid across buffer content updates, so
/// caching by (fd, bytes) is safe while the fd keeps referring to the same
/// underlying dma-buf (true for a live MPP buffer group; buffers are recycled
/// inside the group, not closed and reopened).
///
/// Not internally synchronized: a cache instance is owned by a single node
/// and only touched from that node's serialized forward path, matching the
/// node-held Dma32Buffer reuse in RknnResizeNode. Capacity should cover the
/// decoder pool (kDecoderBufferCount) so steady-state hit rate is ~100%.
class RgaFdHandleCache {
public:
    struct Entry {
        rga_buffer_handle_t handle{0};
        bool cache_hit{false};
    };

    explicit RgaFdHandleCache(size_t capacity) : capacity_(capacity < 1 ? 1 : capacity) {
        slots_.reserve(capacity_);
    }

    ~RgaFdHandleCache() {
        Clear();
    }

    RgaFdHandleCache(const RgaFdHandleCache&)            = delete;
    RgaFdHandleCache& operator=(const RgaFdHandleCache&) = delete;

    /// Returns the imported handle for (fd, bytes), importing it on first use
    /// and evicting the least-recently-used entry when full. A failed import
    /// returns {0, false} and leaves the cache unchanged.
    Entry Get(int fd, size_t bytes) {
        if (fd < 0 || bytes == 0 || bytes > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return {};
        }
        ++clock_;
        for (auto& slot : slots_) {
            if (slot.handle != 0 && slot.fd == fd && slot.bytes == bytes) {
                slot.stamp = clock_;
                return {slot.handle, true};
            }
        }
        Slot* slot = FindFreeOrEvictSlot();
        std::lock_guard<std::mutex> lock(RgaGlobalLock());
        const auto handle = importbuffer_fd(fd, static_cast<int>(bytes));
        if (handle == 0) {
            return {};
        }
        slot->fd     = fd;
        slot->bytes  = bytes;
        slot->stamp  = clock_;
        slot->handle = handle;
        return {handle, false};
    }

    /// Releases every cached handle (each release takes RgaGlobalLock).
    void Clear() {
        for (auto& slot : slots_) {
            if (slot.handle != 0) {
                std::lock_guard<std::mutex> lock(RgaGlobalLock());
                releasebuffer_handle(slot.handle);
                slot.handle = 0;
            }
            slot.fd    = -1;
            slot.bytes = 0;
        }
    }

    [[nodiscard]] size_t Capacity() const {
        return capacity_;
    }

private:
    struct Slot {
        int fd{-1};
        size_t bytes{0};
        uint64_t stamp{0};
        rga_buffer_handle_t handle{0};
    };

    Slot* FindFreeOrEvictSlot() {
        for (auto& slot : slots_) {
            if (slot.handle == 0) {
                return &slot;
            }
        }
        if (slots_.size() < capacity_) {
            slots_.emplace_back();
            return &slots_.back();
        }
        Slot* oldest = &slots_.front();
        for (auto& slot : slots_) {
            if (slot.stamp < oldest->stamp) {
                oldest = &slot;
            }
        }
        if (oldest->handle != 0) {
            std::lock_guard<std::mutex> lock(RgaGlobalLock());
            releasebuffer_handle(oldest->handle);
            oldest->handle = 0;
        }
        return oldest;
    }

    size_t capacity_;
    uint64_t clock_{0};
    std::vector<Slot> slots_;
};

inline constexpr bool RockchipRgaHasBt2020ColorSpace() {
#if defined(RGA_CURRENT_API_VERSION) && RGA_CURRENT_API_VERSION >= 0x010a0600
    return true;
#else
    return false;
#endif
}

inline IM_COLOR_SPACE_MODE RockchipRgaBt2020ColorSpace(bool full_range) {
#if defined(RGA_CURRENT_API_VERSION) && RGA_CURRENT_API_VERSION >= 0x010a0600
    return full_range ? IM_YUV_BT2020_FULL_RANGE : IM_YUV_BT2020_LIMIT_RANGE;
#else
    // librga 1.10.1 and earlier do not expose BT.2020 full-CSC modes. Keep
    // compilation and runtime behavior inside the advertised header contract;
    // the caller emits a warning if this colorimetry downgrade is exercised.
    return full_range ? IM_YUV_BT709_FULL_RANGE : IM_YUV_BT709_LIMIT_RANGE;
#endif
}

inline void SetRgaYuvToRgbColorSpace(rga_buffer_t& source, rga_buffer_t& target,
                                     IM_COLOR_SPACE_MODE source_mode = IM_YUV_BT601_LIMIT_RANGE) {
    imsetColorSpace(&source, source_mode);
    // IM_RGB_FULL_RANGE is a newer alias; IM_RGB_FULL is ABI-identical and is
    // present in both supported librga header generations.
    imsetColorSpace(&target, IM_RGB_FULL);
}

/// A dma-buf allocation from the physically-low (< 4 GiB) dma32 heap. RGA's
/// MMU import/color-convert path cannot map buffers backed by upper-RAM pages,
/// so frame data sourced from process heap memory is staged here for RGA and
/// copied back to the caller's (cached) pool buffer afterwards.
class Dma32Buffer {
public:
    Dma32Buffer() = default;
    Dma32Buffer(const Dma32Buffer&)            = delete;
    Dma32Buffer& operator=(const Dma32Buffer&) = delete;

    ~Dma32Buffer() {
        Release();
    }

    [[nodiscard]] bool Allocate(size_t bytes) {
        Release();
        fd_ = open(kDma32HeapPath, O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            return false;
        }
        struct dma_heap_allocation_data alloc {};
        alloc.len       = static_cast<__u64>(bytes);
        alloc.fd        = 0;
        alloc.fd_flags  = O_RDWR | O_CLOEXEC;
        if (ioctl(fd_, DMA_HEAP_IOCTL_ALLOC, &alloc) != 0) {
            close(fd_);
            fd_ = -1;
            return false;
        }
        close(fd_);
        fd_   = static_cast<int>(alloc.fd);
        size_ = bytes;
        return true;
    }

    const void* Map() {
        if (!Mapped() && fd_ >= 0) {
            void* base = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
            if (base == MAP_FAILED) {
                return nullptr;
            }
            base_ = base;
        }
        return base_;
    }

    [[nodiscard]] bool Mapped() const {
        return base_ != nullptr;
    }

    [[nodiscard]] int Fd() const {
        return fd_;
    }

    [[nodiscard]] size_t Size() const {
        return size_;
    }

    void Release() {
        if (base_) {
            munmap(base_, size_);
            base_ = nullptr;
        }
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        size_ = 0;
    }

private:
    int fd_      = -1;
    size_t size_ = 0;
    void* base_  = nullptr;
};

inline rga_buffer_t RgaFdBuffer(int fd, int width, int height, int format) {
    rga_buffer_t buffer {};
    buffer.fd      = fd;
    buffer.width   = width;
    buffer.height  = height;
    buffer.wstride = width;
    buffer.hstride = height;
    buffer.format  = format;
    return buffer;
}

}  // namespace cosmo::media
