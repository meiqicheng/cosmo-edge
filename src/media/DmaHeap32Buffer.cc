#include "media/DmaHeap32Buffer.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "util/Log.h"

namespace cosmo::media {
namespace {

    constexpr const char* kDmaHeapDir  = "/dev/dma_heap/";
    constexpr size_t kDmaHeapPageBytes = 4096;

    /// Measured low-4G supply on the 8 GiB Debian 12 target board with the
    /// engine stopped: saturating the dma32 heap from userspace yields about
    /// 58 MB regardless of the probe chunk size. The default budget fits the
    /// full three-channel 720p50 worst case (external groups ~32 MB, five
    /// I420 copy-out targets ~10.4 MB, two BGR-class blocks ~6.3 MB and three
    /// RKNN bound inputs ~3.7 MB) and keeps ~4 MB of that zone for the kernel
    /// and everyone else living in it.
    constexpr size_t kDefaultDma32BudgetBytes = 54ULL * 1024ULL * 1024ULL;

    /// Slice of the budget that only critical allocations (RKNN bound inputs)
    /// may use. Without the slice the pool's greedy async top-up can beat the
    /// first inference to the last megabytes and push the bound input onto
    /// rknn_create_mem, which turns the whole preprocessing stage into CPU
    /// resize for that channel's lifetime.
    constexpr size_t kDefaultDma32CriticalReserveBytes = 6ULL * 1024ULL * 1024ULL;

    size_t Dma32CriticalReserveBytes() {
        const char* configured = std::getenv("COSMO_DMA32_CRITICAL_RESERVE_MB");
        if (configured != nullptr && configured[0] != '\0') {
            const long megabytes = std::strtol(configured, nullptr, 10);
            if (megabytes >= 0 && megabytes < 4096) {
                return static_cast<size_t>(megabytes) * 1024ULL * 1024ULL;
            }
            LOG_WARN("COSMO_DMA32_CRITICAL_RESERVE_MB={} is not a sane figure; using the default",
                     configured);
        }
        return kDefaultDma32CriticalReserveBytes;
    }

    /// Byte-for-byte mirror of the kernel's `struct dma_heap_allocation_data`.
    ///
    /// `<linux/dma-heap.h>` is part of the kernel uapi, but the sysroot shipped
    /// with some board images predates it, and this BSP layers a private
    /// `DMA_HEAP_IOCTL_GET_PHYS` on top of the same ioctl range. Declaring the ABI
    /// locally keeps the engine buildable against either generation: the ioctl
    /// number is derived from the payload size, which both layouts agree on.
    struct DmaHeapAllocationData {
        uint64_t len;
        uint32_t fd;
        uint32_t fd_flags;
        uint64_t heap_flags;
    };

    size_t AlignUp(size_t value, size_t alignment) {
        return ((value + alignment - 1U) / alignment) * alignment;
    }

    bool HeapExists(const std::string& name) {
        struct stat info {};
        return ::stat((std::string(kDmaHeapDir) + name).c_str(), &info) == 0;
    }

    std::string ProbeDma32Heap() {
        const char* configured = std::getenv("COSMO_DMA32_HEAP");
        if (configured != nullptr) {
            if (configured[0] == '\0') {
                return {};
            }
            if (HeapExists(configured)) {
                return configured;
            }
            LOG_WARN("COSMO_DMA32_HEAP={} does not exist under {}", configured, kDmaHeapDir);
            return {};
        }
        for (const char* candidate : kDma32HeapCandidates) {
            if (HeapExists(candidate)) {
                return candidate;
            }
        }
        return {};
    }

    std::atomic<uint32_t> g_dma32_decoder_count{0};

    /// The shared reservation ledger. Every DmaHeap32Buffer allocation charges
    /// its page-aligned size here, so frame pools, MPP external frame groups
    /// and RKNN bound inputs cannot together oversubscribe the physically
    /// bounded low-4G zone during a concurrent startup burst.
    std::mutex g_dma32_ledger_mutex;
    size_t g_dma32_ledger_bytes{0};
    bool g_dma32_budget_logged{false};

    bool Dma32ReserveBytes(size_t bytes, bool critical) {
        std::lock_guard<std::mutex> guard(g_dma32_ledger_mutex);
        if (!g_dma32_budget_logged) {
            g_dma32_budget_logged = true;
            LOG_INFO("dma32 reservation ledger armed: global budget {} bytes, critical reserve {} bytes",
                     Dma32GlobalBudgetBytes(), Dma32CriticalReserveBytes());
        }
        const size_t ceiling =
            critical ? Dma32GlobalBudgetBytes() : Dma32GlobalBudgetBytes() - Dma32CriticalReserveBytes();
        if (g_dma32_ledger_bytes + bytes > ceiling) {
            return false;
        }
        g_dma32_ledger_bytes += bytes;
        return true;
    }

    void Dma32ReleaseBytes(size_t bytes) {
        std::lock_guard<std::mutex> guard(g_dma32_ledger_mutex);
        g_dma32_ledger_bytes -= std::min(bytes, g_dma32_ledger_bytes);
    }

}  // namespace

// Fallback definition for sysroots whose `<linux/dma-heap.h>` is absent. When
// the platform header does supply it, both spellings encode the same request.
#ifndef DMA_HEAP_IOCTL_ALLOC
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct DmaHeapAllocationData)
#endif

const std::string& Dma32HeapName() {
    static const std::string resolved = ProbeDma32Heap();
    return resolved;
}

bool Dma32HeapAvailable() {
    return !Dma32HeapName().empty();
}

uint32_t Dma32DecoderCount() {
    return g_dma32_decoder_count.load(std::memory_order_relaxed);
}

Dma32DecoderSeat::Dma32DecoderSeat() {
    g_dma32_decoder_count.fetch_add(1, std::memory_order_relaxed);
}

Dma32DecoderSeat::~Dma32DecoderSeat() {
    g_dma32_decoder_count.fetch_sub(1, std::memory_order_relaxed);
}

size_t Dma32DecoderGroupBufferCount() {
    const uint32_t decoders = Dma32DecoderCount();
    if (decoders <= 1) {
        return 8;
    }
    if (decoders == 2) {
        return 7;
    }
    if (decoders == 3) {
        return 6;
    }
    return 5;
}

size_t Dma32GlobalBudgetBytes() {
    const char* configured = std::getenv("COSMO_DMA32_BUDGET_MB");
    if (configured != nullptr && configured[0] != '\0') {
        const long megabytes = std::strtol(configured, nullptr, 10);
        if (megabytes > 0 && megabytes < 4096) {
            return static_cast<size_t>(megabytes) * 1024ULL * 1024ULL;
        }
        LOG_WARN("COSMO_DMA32_BUDGET_MB={} is not a sane megabyte figure; using the default", configured);
    }
    return kDefaultDma32BudgetBytes;
}

DmaHeap32Buffer::~DmaHeap32Buffer() {
    Release();
}

DmaHeap32Buffer::DmaHeap32Buffer(DmaHeap32Buffer&& other) noexcept {
    *this = std::move(other);
}

DmaHeap32Buffer& DmaHeap32Buffer::operator=(DmaHeap32Buffer&& other) noexcept {
    if (this != &other) {
        Release();
        fd_            = other.fd_;
        address_       = other.address_;
        bytes_         = other.bytes_;
        heap_name_     = std::move(other.heap_name_);
        other.fd_      = -1;
        other.address_ = nullptr;
        other.bytes_   = 0;
        other.heap_name_.clear();
    }
    return *this;
}

bool DmaHeap32Buffer::Allocate(size_t bytes, std::string& reason, bool critical) {
    if (bytes == 0) {
        reason = "cannot allocate an empty 32-bit-addressable buffer";
        return false;
    }
    if (valid() && bytes_ >= bytes) {
        reason.clear();
        return true;
    }
    // Growth path: drop the previous mapping and its ledger charge first, so
    // the old and new charges never coexist. Matches the previous semantics
    // where a failed reallocation did not keep the old buffer alive.
    Release();

    const size_t charge = AlignUp(bytes, kDmaHeapPageBytes);
    if (!Dma32ReserveBytes(charge, critical)) {
        reason = "the global dma32 reservation budget (" + std::to_string(Dma32GlobalBudgetBytes()) +
                 " bytes) is exhausted; degrading this consumer to its address-agnostic path";
        return false;
    }

    const std::string heap = Dma32HeapName();
    if (heap.empty()) {
        Dma32ReleaseBytes(charge);
        reason = "no 32-bit-addressable dma-heap is exposed by this kernel";
        return false;
    }

    const std::string path = std::string(kDmaHeapDir) + heap;
    const int heap_fd      = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (heap_fd < 0) {
        Dma32ReleaseBytes(charge);
        reason = "open " + path + " failed: " + std::strerror(errno);
        return false;
    }

    DmaHeapAllocationData request{};
    request.len        = static_cast<uint64_t>(charge);
    request.fd         = 0;
    request.fd_flags   = static_cast<uint32_t>(O_RDWR | O_CLOEXEC);
    request.heap_flags = 0;

    if (::ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &request) != 0) {
        const int error = errno;
        Dma32ReleaseBytes(charge);
        ::close(heap_fd);
        reason = "DMA_HEAP_IOCTL_ALLOC on " + heap + " failed: " + std::strerror(error);
        return false;
    }

    const int buffer_fd = static_cast<int>(request.fd);
    const size_t mapped = static_cast<size_t>(request.len);
    void* address       = ::mmap(nullptr, mapped, PROT_READ | PROT_WRITE, MAP_SHARED, buffer_fd, 0);
    if (address == MAP_FAILED) {
        const int error = errno;
        Dma32ReleaseBytes(charge);
        ::close(buffer_fd);
        ::close(heap_fd);
        reason = "mmap of a " + heap + " buffer failed: " + std::strerror(error);
        return false;
    }

    // The heap directory fd is only needed for the allocation ioctl.
    ::close(heap_fd);
    fd_        = buffer_fd;
    address_   = address;
    bytes_     = mapped;
    heap_name_ = heap;
    reason.clear();
    return true;
}

void DmaHeap32Buffer::Release() {
    if (address_ != nullptr) {
        ::munmap(address_, bytes_);
        address_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (bytes_ != 0) {
        Dma32ReleaseBytes(bytes_);
        bytes_ = 0;
    }
    heap_name_.clear();
}

}  // namespace cosmo::media
