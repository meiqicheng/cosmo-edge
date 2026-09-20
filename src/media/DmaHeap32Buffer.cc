#include "media/DmaHeap32Buffer.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "util/Log.h"

namespace cosmo::media {
namespace {

    constexpr const char* kDmaHeapDir  = "/dev/dma_heap/";
    constexpr size_t kDmaHeapPageBytes = 4096;

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

bool DmaHeap32Buffer::Allocate(size_t bytes, std::string& reason) {
    if (bytes == 0) {
        reason = "cannot allocate an empty 32-bit-addressable buffer";
        return false;
    }
    if (valid() && bytes_ >= bytes) {
        reason.clear();
        return true;
    }
    Release();

    const std::string heap = Dma32HeapName();
    if (heap.empty()) {
        reason = "no 32-bit-addressable dma-heap is exposed by this kernel";
        return false;
    }

    const std::string path = std::string(kDmaHeapDir) + heap;
    const int heap_fd      = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (heap_fd < 0) {
        reason = "open " + path + " failed: " + std::strerror(errno);
        return false;
    }

    DmaHeapAllocationData request{};
    request.len        = static_cast<uint64_t>(AlignUp(bytes, kDmaHeapPageBytes));
    request.fd         = 0;
    request.fd_flags   = static_cast<uint32_t>(O_RDWR | O_CLOEXEC);
    request.heap_flags = 0;

    if (::ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &request) != 0) {
        const int error = errno;
        ::close(heap_fd);
        reason = "DMA_HEAP_IOCTL_ALLOC on " + heap + " failed: " + std::strerror(error);
        return false;
    }

    const int buffer_fd = static_cast<int>(request.fd);
    const size_t mapped = static_cast<size_t>(request.len);
    void* address       = ::mmap(nullptr, mapped, PROT_READ | PROT_WRITE, MAP_SHARED, buffer_fd, 0);
    if (address == MAP_FAILED) {
        const int error = errno;
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
    bytes_ = 0;
    heap_name_.clear();
}

}  // namespace cosmo::media
