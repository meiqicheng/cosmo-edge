#ifdef COSMO_MEDIA_USE_AXERA_BACKEND

#include "media/VideoFrameProcAxera.h"

#include <cstring>

#include "media/IOsdTextRenderer.h"
#include "media/VideoFrameProcCpu.h"
#include "util/Log.h"

namespace cosmo::media {

VideoFrameProcAxera::VideoFrameProcAxera(IOsdTextRenderer& osd)
    : cpu_(std::make_unique<VideoFrameProcCpu>(osd)) {}

VideoFrameProcAxera::~VideoFrameProcAxera() = default;

VideoFramePtr VideoFrameProcAxera::CopyFrame(VideoFramePtr srcImage) {
    return cpu_->CopyFrame(std::move(srcImage));
}

bool VideoFrameProcAxera::EnsureHostData(VideoFramePtr frame) {
    return cpu_->EnsureHostData(std::move(frame));
}

VideoFramePtr VideoFrameProcAxera::BGR2I420(VideoFramePtr srcImage) {
    return cpu_->BGR2I420(std::move(srcImage));
}

VideoFramePtr VideoFrameProcAxera::RGB2I420(VideoFramePtr srcImage) {
    return cpu_->RGB2I420(std::move(srcImage));
}

VideoFramePtr VideoFrameProcAxera::I4202BGR(VideoFramePtr srcImage) {
    return cpu_->I4202BGR(std::move(srcImage));
}

VideoFramePtr VideoFrameProcAxera::I4202RGB(VideoFramePtr srcImage) {
    return cpu_->I4202RGB(std::move(srcImage));
}

VideoFramePtr VideoFrameProcAxera::Crop(const VideoFramePtr srcPicture, const util::Box roi) {
    return cpu_->Crop(srcPicture, roi);
}

VideoFramePtr VideoFrameProcAxera::Resize(VideoFramePtr src, int dst_height, int dst_width) {
    return cpu_->Resize(std::move(src), dst_height, dst_width);
}

VideoFramePtr VideoFrameProcAxera::Padding(VideoFramePtr src, size_t top, size_t bottom, size_t left,
                                           size_t right, Color color) {
    return cpu_->Padding(std::move(src), top, bottom, left, right, color);
}

std::vector<u_char> VideoFrameProcAxera::EncodeJpeg(const VideoFramePtr srcPicture) {
    return cpu_->EncodeJpeg(srcPicture);
}

VideoFramePtr VideoFrameProcAxera::DecodeJpeg(const std::vector<u_int8_t>& data) {
    return cpu_->DecodeJpeg(data);
}

VideoFramePtr VideoFrameProcAxera::DrawBox(VideoFramePtr srcImage, const util::Box imageRect,
                                           const Color& color, int lineWidth) {
    return cpu_->DrawBox(std::move(srcImage), imageRect, color, lineWidth);
}

VideoFramePtr VideoFrameProcAxera::DrawPoint(VideoFramePtr srcImage, util::Point point,
                                             const Color& color, int lineWidth) {
    return cpu_->DrawPoint(std::move(srcImage), point, color, lineWidth);
}

VideoFramePtr VideoFrameProcAxera::DrawLines(VideoFramePtr srcImage,
                                             std::vector<std::pair<util::Point, util::Point>> lines,
                                             const Color& color, int lineWidth) {
    return cpu_->DrawLines(std::move(srcImage), std::move(lines), color, lineWidth);
}

VideoFramePtr VideoFrameProcAxera::DrawRects(VideoFramePtr srcImage,
                                             const std::vector<util::Box>& rects, const Color& color,
                                             int lineWidth) {
    return cpu_->DrawRects(std::move(srcImage), rects, color, lineWidth);
}

VideoFramePtr VideoFrameProcAxera::DrawText(VideoFramePtr srcImage, int x, int y,
                                            const std::string& text, const Color& color, int fontSize) {
    return cpu_->DrawText(std::move(srcImage), x, y, text, color, fontSize);
}

bool VideoFrameProcAxera::BeginOSD(VideoFramePtr frame) {
    return cpu_->BeginOSD(std::move(frame));
}

void VideoFrameProcAxera::OSDDrawLines(std::vector<std::pair<util::Point, util::Point>> lines,
                                       const Color& color, int lineWidth) {
    cpu_->OSDDrawLines(std::move(lines), color, lineWidth);
}

void VideoFrameProcAxera::OSDDrawText(int x, int y, const std::string& text, const Color& color,
                                      int fontSize) {
    cpu_->OSDDrawText(x, y, text, color, fontSize);
}

void VideoFrameProcAxera::OSDDrawTextEx(int x, int y, const std::string& text, const Color& color,
                                        int fontSize, const Color& bgColor, uint8_t bgAlpha,
                                        bool outline, int bgPadding) {
    cpu_->OSDDrawTextEx(x, y, text, color, fontSize, bgColor, bgAlpha, outline, bgPadding);
}

void VideoFrameProcAxera::EndOSD() {
    cpu_->EndOSD();
}

}  // namespace cosmo::media

#endif  // COSMO_MEDIA_USE_AXERA_BACKEND
