// MemoryPoolServiceImpl owns the memory pool lifecycle.

#include "service/infra/impl/MemoryPoolServiceImpl.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "mem/IDeviceContext.h"
#include "mem/MemoryPoolMng.h"
#ifdef COSMO_NN_USE_SOPHON_BACKEND
#include "mem/AllocatorSophon.h"
#else
#include "media/DmaHeap32Buffer.h"
#include "mem/AllocatorCpu.h"
#endif
#include "service/detail/ServiceRegistry.h"
#include "util/Log.h"

namespace cosmo::service {
namespace {

#ifndef COSMO_NN_USE_SOPHON_BACKEND

    /// Allocator handing out video-frame blocks that live below the 4 GiB boundary.
    ///
    /// RGA2 drives its own RGA_MMU, which cannot address memory above 4 GiB, and
    /// every operation whose YUV side is planar -- I420 <-> BGR/RGB color conversion
    /// and I420 resize -- can only be served by RGA2 on RK3588. The malloc-backed
    /// blocks are free to land in ZONE_NORMAL on a >4 GiB board, which turns each of
    /// those operations into a hard "no core match" failure and latches the CPU
    /// fallback for the frame processor's lifetime. Sourcing the blocks from a
    /// `*-dma32` dma-heap keeps them addressable, so the same operations run on
    /// RGA2; a block whose dma-heap allocation fails falls back to malloc and the
    /// pool behaves exactly as before, which keeps boards without such a heap on
    /// their previous code path.
    ///
    /// The low-4G zone on an 8 GiB board is a scarce, already-fragmented resource
    /// that also feeds CMA, VPU and camera buffers, so residency is capped per
    /// size class: only the classes that the RGA2 paths actually touch (the
    /// 720p/1080p I420 and BGR frames) get a bounded allowance, the small face
    /// classes get a small one, and the giant snapshot classes stay on malloc.
    /// Without the per-class split a single greedy pool exhausts the whole
    /// allowance before the preview classes ever allocate.
    class AllocatorDma32 final : public cosmo::mem::Allocator {
    public:
        explicit AllocatorDma32(std::string heap_name) : heap_name_(std::move(heap_name)) {}

        ~AllocatorDma32() override = default;

        [[nodiscard]] cosmo::mem::Block* Allocate(size_t size) override {
            if (size == 0) {
                return nullptr;
            }
            if (!ReserveDma32(size)) {
                return AllocateWithMalloc(size);
            }
            // Try the heap first; a failed allocation degrades to the previous
            // malloc behavior instead of starving the frame pipeline.
            media::DmaHeap32Buffer buffer;
            std::string reason;
            if (buffer.Allocate(size, reason)) {
                auto block    = std::make_unique<cosmo::mem::Block>();
                block->data   = static_cast<uint8_t*>(buffer.address());
                block->size   = size;
                block->low_4g = true;
                {
                    std::lock_guard<std::mutex> guard(owned_mutex_);
                    owned_.emplace(block->data, std::move(buffer));
                }
                dma32_bytes_.fetch_add(size, std::memory_order_relaxed);
                return block.release();
            }
            ReleaseReservation(size);
            LOG_WARN("dma32 heap {} could not serve {} bytes ({}); falling back to malloc", heap_name_, size,
                     reason);
            return AllocateWithMalloc(size);
        }

        void Free(cosmo::mem::Block* block) override {
            if (block == nullptr) {
                return;
            }
            auto block_guard = std::unique_ptr<cosmo::mem::Block>(block);
            media::DmaHeap32Buffer buffer;
            {
                std::lock_guard<std::mutex> guard(owned_mutex_);
                const auto it = owned_.find(block_guard->data);
                if (it != owned_.end()) {
                    buffer = std::move(it->second);
                    owned_.erase(it);
                }
            }
            if (buffer.valid()) {
                dma32_bytes_.fetch_sub(block_guard->size, std::memory_order_relaxed);
                ReleaseReservation(block_guard->size);
            } else if (block_guard->data != nullptr) {
                // A block that is not tracked here came from the malloc fallback.
                std::free(block_guard->data);
            }
            // The buffer destructor releases the mapping and the dma-buf fd.
        }

        [[nodiscard]] const std::string& heap_name() const {
            return heap_name_;
        }

    private:
        static constexpr size_t kMaxBlocksPerClass = 8;

        /// Maximum blocks per size class that may live on the dma32 heap. The
        /// thresholds mirror the pool size table in MemoryPoolMng.h and the
        /// frame flow: the I420 decode target (720p frames land in the
        /// 2073600 class) and the BGR preview target (720p BGR rounds up to
        /// the 3133440 class) are the operands of the RGA2-only planar
        /// operations, so they get the largest allowances; the 4K BGR and
        /// snapshot classes never reach RGA2 at meaningful rates and stay on
        /// malloc.
        [[nodiscard]] static size_t ClassBlockLimit(size_t size) {
            constexpr size_t kFaceI420 = 98304;    // 256x256x1.5
            constexpr size_t kBodyI420 = 614400;   // 640x640x1.5
            constexpr size_t k720I420  = 2073600;  // decode copy-out target class
            constexpr size_t k1088I420 = 3133440;  // 1920x1088x1.5, also 720p BGR
            constexpr size_t k1088Bgr  = 6266880;  // 1920x1088x3
            if (size <= kFaceI420) {
                return 4;
            }
            if (size <= kBodyI420) {
                return 4;
            }
            if (size <= k720I420) {
                return kMaxBlocksPerClass;
            }
            if (size <= k1088I420) {
                return kMaxBlocksPerClass;
            }
            if (size <= k1088Bgr) {
                return 2;
            }
            return 0;
        }

        /// Accounting guard for one allocation. Returns false when the block's
        /// size class has no dma32 allowance left, or when the global backstop
        /// budget is exhausted.
        [[nodiscard]] bool ReserveDma32(size_t size) {
            const size_t class_limit = ClassBlockLimit(size) * size;
            if (class_limit == 0) {
                return false;
            }
            std::lock_guard<std::mutex> guard(owned_mutex_);
            const auto it = class_bytes_.find(size);
            if (it != class_bytes_.end() && it->second + size > class_limit) {
                return false;
            }
            constexpr size_t kGlobalBudgetBytes = 64ULL * 1024ULL * 1024ULL;
            if (dma32_bytes_.load(std::memory_order_relaxed) + size > kGlobalBudgetBytes) {
                return false;
            }
            if (it != class_bytes_.end()) {
                it->second += size;
            } else {
                class_bytes_.emplace(size, size);
            }
            return true;
        }

        void ReleaseReservation(size_t size) {
            std::lock_guard<std::mutex> guard(owned_mutex_);
            const auto it = class_bytes_.find(size);
            if (it != class_bytes_.end() && it->second >= size) {
                it->second -= size;
            }
        }

        [[nodiscard]] static cosmo::mem::Block* AllocateWithMalloc(size_t size) {
            auto* data = static_cast<uint8_t*>(std::malloc(size));
            if (data == nullptr) {
                return nullptr;
            }
            auto block  = std::make_unique<cosmo::mem::Block>();
            block->data = data;
            block->size = size;
            return block.release();
        }

        std::string heap_name_;
        std::atomic<size_t> dma32_bytes_{0};
        std::mutex owned_mutex_;
        std::unordered_map<uint8_t*, media::DmaHeap32Buffer> owned_;
        /// dma32 bytes resident per block size, guarded by owned_mutex_.
        std::unordered_map<size_t, size_t> class_bytes_;
    };

#endif

}  // namespace

MemoryPoolServiceImpl::MemoryPoolServiceImpl() {
#ifdef COSMO_NN_USE_SOPHON_BACKEND
    auto allocator = std::make_unique<cosmo::mem::AllocatorSophon>(
        ServiceRegistry::Instance().Get<cosmo::mem::IDeviceContext>());
#else
    std::unique_ptr<cosmo::mem::Allocator> allocator;
    const std::string& heap = cosmo::media::Dma32HeapName();
    if (heap.empty()) {
        allocator = std::make_unique<cosmo::mem::AllocatorCpu>();
        LOG_INFO("{}",
                 "MemoryPoolServiceImpl: video-frame pool uses malloc "
                 "(no 32-bit-addressable dma-heap exposed)");
    } else {
        allocator = std::make_unique<AllocatorDma32>(heap);
        LOG_INFO(
            "MemoryPoolServiceImpl: video-frame pool uses 32-bit-addressable heap={} "
            "(keeps RGA2-only planar I420 operations on RGA)",
            heap);
    }
#endif
    pool_ = std::make_unique<cosmo::mem::MemoryPoolMng>(std::move(allocator));
    cosmo::mem::SetMemoryPoolContext(pool_.get());
    LOG_INFO("{}", "MemoryPoolServiceImpl: pool initialized and context registered");
}

MemoryPoolServiceImpl::~MemoryPoolServiceImpl() {
    LOG_INFO("{}", "MemoryPoolServiceImpl: destroying pool");
    cosmo::mem::SetMemoryPoolContext(nullptr);
    pool_.reset();
}

}  // namespace cosmo::service
