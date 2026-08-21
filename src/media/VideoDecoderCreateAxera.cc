#ifdef COSMO_MEDIA_USE_AXERA_BACKEND

#include "media/VideoDecoder.h"
#include "media/VideoDecoderAxera.h"

namespace cosmo {
namespace media {

    std::unique_ptr<VideoDecoder> VideoDecoder::Create(size_t name, void* mediaHandle) {
        return std::make_unique<VideoDecoderAxera>(name);
    }

    VideoDecoderCapability VideoDecoder::Probe(VideoCodecType type) {
        return VideoDecoderAxera::Probe(type);
    }

}  // namespace media
}  // namespace cosmo

#endif  // COSMO_MEDIA_USE_AXERA_BACKEND
