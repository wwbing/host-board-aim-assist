#pragma once

#include <memory>
#include <string>

#include "video_infer_options.hpp"

namespace ffmpeg_video_infer {

class VideoInferApp {
public:
    explicit VideoInferApp(VideoInferOptions options);
    ~VideoInferApp();

    VideoInferApp(const VideoInferApp&) = delete;
    VideoInferApp& operator=(const VideoInferApp&) = delete;

    bool Initialize();
    int Run();
    void RequestStop();
    const std::string& LastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ffmpeg_video_infer
