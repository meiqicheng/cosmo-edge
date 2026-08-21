#ifdef COSMO_MEDIA_USE_AXERA_BACKEND

#include "media/VideoEncoder.h"
#include "media/VideoEncoderAxera.h"

namespace cosmo {
namespace media {

    std::shared_ptr<VideoEncoder> VideoEncoder::Create(void* mediaHandle) {
        return std::make_shared<VideoEncoderAxera>();
    }

    VideoEncoderCapability VideoEncoder::Probe(VideoCodecType type) {
        return VideoEncoderAxera::Probe(type);
    }

}  // namespace media
}  // namespace cosmo

#endif  // COSMO_MEDIA_USE_AXERA_BACKEND
