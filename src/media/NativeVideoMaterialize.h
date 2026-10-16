#pragma once

#include "media/NativeVideoBuffer.h"
#include "media/VideoFrame.h"

namespace cosmo::media {

/// On-demand host materialization of a native DMA-BUF frame — the P1-2
/// alarm-materialization gate. Native-only pipeline frames carry no host
/// pixels; consumer actions that need media (alarm pictures, best-shot
/// retention) call this at event time instead of forcing per-frame copy-out.
///
/// @param native A valid native buffer (fd borrowed from `owner`; the caller
///               must keep the owner alive for the duration of the call).
/// @return A compact I420 host frame, or nullptr when the backend cannot
///         materialize (non-Rockchip backends, unsupported format, RGA
///         failure). Null returns are safe: callers degrade to alarm-without-
///         media exactly as before this gate existed.
VideoFramePtr MaterializeNativeBuffer(const NativeVideoBuffer& native);

}  // namespace cosmo::media
