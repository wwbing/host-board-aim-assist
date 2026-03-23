#include "clock_offset_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace ffmpeg_video_infer {
namespace {

constexpr int kReceiveTimeoutMs = 200;
constexpr std::size_t kReceiveBufferSize = 2048;
constexpr const char* kClockOffsetRequestType = "clock_offset_request";

std::int64_t NowWallClockNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t ReadInt64OrDefault(const nlohmann::json& json_value, const char* key) {
    if (!json_value.contains(key) || !json_value[key].is_number_integer()) {
        return 0;
    }
    return json_value[key].get<std::int64_t>();
}

bool TryReadUInt64(const nlohmann::json& json_value, const char* key, std::uint64_t* value) {
    if (!json_value.contains(key)) {
        return false;
    }
    if (json_value[key].is_number_unsigned()) {
        *value = json_value[key].get<std::uint64_t>();
        return true;
    }
    if (json_value[key].is_number_integer()) {
        const auto signed_value = json_value[key].get<std::int64_t>();
        if (signed_value < 0) {
            return false;
        }
        *value = static_cast<std::uint64_t>(signed_value);
        return true;
    }
    return false;
}

bool TryParseClockOffsetRequest(const nlohmann::json& request_json,
                                std::uint64_t* request_id,
                                std::int64_t* t1_ns) {
    if (!request_json.is_object()) {
        return false;
    }

    if (request_json.contains("type")) {
        if (!request_json["type"].is_string() ||
            request_json["type"].get<std::string>() != kClockOffsetRequestType) {
            return false;
        }
    }

    if (request_json.contains("request_id") && request_json.contains("t1_ns")) {
        if (!TryReadUInt64(request_json, "request_id", request_id) ||
            !request_json["t1_ns"].is_number_integer()) {
            return false;
        }
        *t1_ns = ReadInt64OrDefault(request_json, "t1_ns");
        return true;
    }

    if (request_json.contains("seq") && request_json.contains("t1")) {
        if (!TryReadUInt64(request_json, "seq", request_id) || !request_json["t1"].is_number_integer()) {
            return false;
        }
        *t1_ns = ReadInt64OrDefault(request_json, "t1");
        return true;
    }

    return false;
}

}  // namespace

ClockOffsetServer::~ClockOffsetServer() {
    Stop();
}

bool ClockOffsetServer::Start(int listen_port) {
    Stop();

    if (listen_port <= 0 || listen_port > 65535) {
        SetError("时钟偏移服务端口超出范围。");
        return false;
    }

    socket_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        SetError("创建时钟偏移服务 socket 失败: " + std::string(std::strerror(errno)));
        return false;
    }

    const int reuse_addr = 1;
    ::setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));

    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = kReceiveTimeoutMs * 1000;
    ::setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<std::uint16_t>(listen_port));
    if (::bind(socket_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        SetError("绑定时钟偏移服务端口失败: " + std::string(std::strerror(errno)));
        ::close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    listen_port_ = listen_port;
    stop_requested_.store(false);
    running_.store(true);
    worker_thread_ = std::thread(&ClockOffsetServer::WorkerLoop, this);
    last_error_.clear();
    return true;
}

void ClockOffsetServer::Stop() {
    stop_requested_.store(true);

    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    listen_port_ = 0;
    running_.store(false);
}

bool ClockOffsetServer::IsRunning() const {
    return running_.load();
}

const std::string& ClockOffsetServer::LastError() const {
    return last_error_;
}

void ClockOffsetServer::WorkerLoop() {
    while (!stop_requested_.load()) {
        sockaddr_in peer_address{};
        socklen_t peer_address_size = sizeof(peer_address);
        char receive_buffer[kReceiveBufferSize] = {};

        const ssize_t received_bytes = ::recvfrom(
            socket_fd_,
            receive_buffer,
            sizeof(receive_buffer),
            0,
            reinterpret_cast<sockaddr*>(&peer_address),
            &peer_address_size);
        if (received_bytes < 0) {
            if (stop_requested_.load()) {
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            if (errno == EBADF || errno == ENOTSOCK) {
                break;
            }
            continue;
        }

        const std::int64_t t2_ns = NowWallClockNs();
        const nlohmann::json request_json =
            nlohmann::json::parse(receive_buffer, receive_buffer + received_bytes, nullptr, false);
        if (request_json.is_discarded() || !request_json.is_object()) {
            continue;
        }

        std::uint64_t request_id = 0;
        std::int64_t t1_ns = 0;
        if (!TryParseClockOffsetRequest(request_json, &request_id, &t1_ns)) {
            continue;
        }

        const std::int64_t t3_ns = NowWallClockNs();

        nlohmann::ordered_json response_json;
        response_json["request_id"] = request_id;
        response_json["t1_ns"] = t1_ns;
        response_json["t2_ns"] = t2_ns;
        response_json["t3_ns"] = t3_ns;
        const std::string response_payload = response_json.dump();

        ::sendto(
            socket_fd_,
            response_payload.data(),
            response_payload.size(),
            0,
            reinterpret_cast<const sockaddr*>(&peer_address),
            peer_address_size);
    }
    running_.store(false);
}

void ClockOffsetServer::SetError(const std::string& message) {
    last_error_ = message;
}

}  // namespace ffmpeg_video_infer
