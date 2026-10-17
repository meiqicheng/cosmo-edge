#pragma once

extern "C" {
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
}

namespace cosmo::media {

inline bool MppFrameIsUnsupportedLayout(MppFrameFormat format) {
    const auto value = static_cast<RK_U32>(format);
    if ((value & MPP_FRAME_FBC_MASK) != 0) return true;
#ifdef MPP_FRAME_TILE_FLAG
    if ((value & MPP_FRAME_TILE_FLAG) != 0) return true;
#endif
#ifdef MPP_FRAME_FMT_IS_YUV_10BIT
    return MPP_FRAME_FMT_IS_YUV_10BIT(format);
#else
    const auto base = value & MPP_FRAME_FMT_MASK;
    return base == MPP_FMT_YUV420SP_10BIT || base == MPP_FMT_YUV422SP_10BIT;
#endif
}

// Cache-sync shims.
//
// These are no-ops, and that is deliberate rather than lazy. The engine is
// compiled against the media SDK's MPP 1.1.0, where mpp_buffer_sync_begin is a
// macro expanding to mpp_buffer_sync_begin_f -- but the Debian 11 RK3588 image
// ships librockchip-mpp1 1.5.0, whose librockchip_mpp.so.1 exports no
// mpp_buffer_sync* symbol at all (readelf -Ws on the board: nothing; its header
// does not even declare them). Calling the macro directly therefore compiles
// cleanly and only fails at run time, on the lazy bind, inside whichever thread
// first touches a buffer.
//
// No sync is needed anyway: every group this project creates is plain
// MPP_BUFFER_TYPE_DRM (see VideoFrameCodecRockchip's kFrameBufferType), i.e. an
// uncached DMA-BUF that is CPU/HW coherent by construction. The sync calls only
// matter for MPP_BUFFER_FLAGS_CACHABLE pools, which this project does not use.
//
// Define COSMO_MPP_BUFFER_SYNC only on a runtime that exports
// mpp_buffer_sync_begin_f / mpp_buffer_sync_end_f and whose buffers are
// actually allocated cachable.
#define COSMO_MPP_BUFFER_SYNC 0

inline MPP_RET MppBufferReadBegin(MppBuffer buffer) {
#if COSMO_MPP_BUFFER_SYNC
    return mpp_buffer_sync_ro_begin(buffer);
#else
    (void)buffer;
    return MPP_OK;
#endif
}

inline void MppBufferReadEnd(MppBuffer buffer) {
#if COSMO_MPP_BUFFER_SYNC
    mpp_buffer_sync_ro_end(buffer);
#else
    (void)buffer;
#endif
}

inline void MppBufferWriteBegin(MppBuffer buffer) {
#if COSMO_MPP_BUFFER_SYNC
    mpp_buffer_sync_begin(buffer);
#else
    (void)buffer;
#endif
}

inline void MppBufferWriteEnd(MppBuffer buffer) {
#if COSMO_MPP_BUFFER_SYNC
    mpp_buffer_sync_end(buffer);
#else
    (void)buffer;
#endif
}

}  // namespace cosmo::media
