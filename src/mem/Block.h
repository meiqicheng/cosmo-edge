#pragma once

#include <unistd.h>

#include <cstdint>

namespace cosmo::mem {

struct BlockStatus {
    pid_t thread_id          = 0;
    int64_t duration         = 0;
    int64_t malloc_timepoint = 0;
};

struct Block {
    size_t size   = 0;
    uint8_t* data = nullptr;
    /// True when the block's pages are guaranteed to sit below the 4 GiB
    /// boundary (dma32 dma-heap). The frame pool serves these blocks first so
    /// the RGA2-only planar operations systematically receive addressable
    /// memory; blocks from the malloc fallback only serve once the low-address
    /// ones are in flight.
    bool low_4g = false;

    BlockStatus status;
};

}  // namespace cosmo::mem
