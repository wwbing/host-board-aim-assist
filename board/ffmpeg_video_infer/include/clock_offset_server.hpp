#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace ffmpeg_video_infer {

class ClockOffsetServer {
public:
    ClockOffsetServer() = default;
    ~ClockOffsetServer();

    ClockOffsetServer(const ClockOffsetServer&) = delete;
    ClockOffsetServer& operator=(const ClockOffsetServer&) = delete;

    bool Start(int listen_port);
    void Stop();

    bool IsRunning() const;
    const std::string& LastError() const;

private:
    void WorkerLoop();
    void SetError(const std::string& message);

    int socket_fd_ = -1;
    int listen_port_ = 0;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
    std::string last_error_;
};

}  // namespace ffmpeg_video_infer
