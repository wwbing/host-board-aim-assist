#pragma once

#include <mutex>

#include "receiver.h"

class LatestFrameBuffer {
public:
    LatestFrameBuffer() = default;

    void update(FrameData frame);
    bool getLatestFrame(FrameData& out) const;
    void clear();

private:
    mutable std::mutex mutex_;
    bool has_frame_ = false;
    FrameData latest_frame_;
};
