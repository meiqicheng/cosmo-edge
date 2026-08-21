#pragma once

#ifdef COSMO_MEDIA_USE_AXERA_BACKEND

#include <memory>

#include "media/IVideoFrameProc.h"

namespace cosmo::media {

class VideoFrameProcCpu;
class IOsdTextRenderer;

/// AX650 hardware image-processing backend built on the SoC-mode IVPS
/// (libax_ivps): scaling, cropping and color-conversion offloaded to the VPU.
///
/// Phase-1 implementation routes every operation through the CPU backend and
/// reserves the IVPS hardware path for board validation; see
/// docs/development/axera-ax650-support-plan.md Phase 4.
class VideoFrameProcAxera final : public IVideoFrameProc {
public:
    explicit VideoFrameProcAxera(IOsdTextRenderer& osd);
    ~VideoFrameProcAxera() override;

    VideoFramePtr CopyFrame(VideoFramePtr srcImage) override;
    bool EnsureHostData(VideoFramePtr frame) override;

    VideoFramePtr BGR2I420(VideoFramePtr srcImage) override;
    VideoFramePtr RGB2I420(VideoFramePtr srcImage) override;
    VideoFramePtr I4202BGR(VideoFramePtr srcImage) override;
    VideoFramePtr I4202RGB(VideoFramePtr srcImage) override;

    VideoFramePtr Crop(const VideoFramePtr srcPicture, const util::Box roi) override;
    VideoFramePtr Resize(VideoFramePtr src, int dst_height, int dst_width) override;
    VideoFramePtr Padding(VideoFramePtr src, size_t top, size_t bottom, size_t left, size_t right,
                          Color color) override;

    std::vector<u_char> EncodeJpeg(const VideoFramePtr srcPicture) override;
    VideoFramePtr DecodeJpeg(const std::vector<u_int8_t>& data) override;

    VideoFramePtr DrawBox(VideoFramePtr srcImage, const util::Box imageRect, const Color& color,
                          int lineWidth = 2) override;
    VideoFramePtr DrawPoint(VideoFramePtr srcImage, util::Point point, const Color& color,
                            int lineWidth = 2) override;
    VideoFramePtr DrawLines(VideoFramePtr srcImage,
                            std::vector<std::pair<util::Point, util::Point>> lines, const Color& color,
                            int lineWidth) override;
    VideoFramePtr DrawRects(VideoFramePtr srcImage, const std::vector<util::Box>& rects,
                            const Color& color, int lineWidth = 2) override;
    VideoFramePtr DrawText(VideoFramePtr srcImage, int x, int y, const std::string& text,
                           const Color& color, int fontSize = 50) override;

    bool BeginOSD(VideoFramePtr frame) override;
    void OSDDrawLines(std::vector<std::pair<util::Point, util::Point>> lines, const Color& color,
                      int lineWidth) override;
    void OSDDrawText(int x, int y, const std::string& text, const Color& color, int fontSize) override;
    void OSDDrawTextEx(int x, int y, const std::string& text, const Color& color, int fontSize,
                       const Color& bgColor, uint8_t bgAlpha, bool outline = true,
                       int bgPadding = 4) override;
    void EndOSD() override;

private:
    // Routes through the CPU backend until the IVPS path is validated.
    std::unique_ptr<VideoFrameProcCpu> cpu_;
};

}  // namespace cosmo::media

#endif  // COSMO_MEDIA_USE_AXERA_BACKEND
