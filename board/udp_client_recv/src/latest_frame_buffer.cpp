#include "latest_frame_buffer.h"

void LatestFrameBuffer::update(FrameData frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_frame_ = std::move(frame);
    has_frame_ = true;
}

bool LatestFrameBuffer::getLatestFrame(FrameData& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_frame_) {
        return false;
    }

    out = latest_frame_;
    return true;
}

void LatestFrameBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_frame_ = FrameData{};
    has_frame_ = false;
}
