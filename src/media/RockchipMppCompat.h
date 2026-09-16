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

inline MPP_RET MppBufferReadBegin(MppBuffer buffer) {
#if defined(MPP_BUFFER_SYNC_RO_BEGIN)
    return mpp_buffer_sync_ro_begin(buffer);
#else
    (void)buffer;
    return MPP_OK;
#endif
}

inline void MppBufferReadEnd(MppBuffer buffer) {
#if defined(MPP_BUFFER_SYNC_RO_BEGIN)
    mpp_buffer_sync_ro_end(buffer);
#else
    (void)buffer;
#endif
}

inline void MppBufferWriteBegin(MppBuffer buffer) {
#if defined(MPP_BUFFER_SYNC_BEGIN)
    mpp_buffer_sync_begin(buffer);
#else
    (void)buffer;
#endif
}

inline void MppBufferWriteEnd(MppBuffer buffer) {
#if defined(MPP_BUFFER_SYNC_BEGIN)
    mpp_buffer_sync_end(buffer);
#else
    (void)buffer;
#endif
}

}  // namespace cosmo::media
