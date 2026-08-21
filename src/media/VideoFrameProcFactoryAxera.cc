#ifdef COSMO_MEDIA_USE_AXERA_BACKEND

#include "media/IOsdTextRenderer.h"
#include "media/VideoFrameProcAxera.h"
#include "media/VideoFrameProcFactory.h"
#include "mem/IDeviceContext.h"

namespace cosmo {
namespace media {

    std::unique_ptr<IVideoFrameProc> CreateVideoFrameProc(mem::IDeviceContext& ctx, IOsdTextRenderer& osd) {
        static_cast<void>(ctx);  // AX650 IVPS does not need a host device context
        return std::make_unique<VideoFrameProcAxera>(osd);
    }

}  // namespace media
}  // namespace cosmo

#endif  // COSMO_MEDIA_USE_AXERA_BACKEND
