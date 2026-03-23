#include "profiling/clock_offset_client.hpp"
#include "logging/logger.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <Winsock2.h>
#include <Ws2tcpip.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <limits>
#include <sstream>
#include <string>

namespace
{

using json = nlohmann::json;

constexpr std::uintptr_t kInvalidSocketHandle = static_cast<std::uintptr_t>(INVALID_SOCKET);
constexpr int kMaxDatagramSize = 4096;
constexpr std::size_t kMaxPayloadPreviewLength = 240;

std::int64_t CurrentWallTimeNs()
{
    const auto now = std::chrono::time_point_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now());
    return now.time_since_epoch().count();
}

std::string GetLastSocketErrorMessage(const char* prefix)
{
    return logging::FormatLastSocketError(prefix);
}

void PrintClockOffsetLine(const std::string& line)
{
    spdlog::info("{}", line);
}

std::string FormatPayloadPreview(const char* data, const std::size_t length)
{
    std::string preview(data, length);
    for (char& ch : preview)
    {
        if (ch == '\r' || ch == '\n' || ch == '\t')
        {
            ch = ' ';
        }
    }

    if (preview.size() > kMaxPayloadPreviewLength)
    {
        preview.resize(kMaxPayloadPreviewLength);
        preview += "...";
    }
    return preview;
}

std::string SockaddrToIpString(const sockaddr_in& address)
{
    char ip_buffer[INET_ADDRSTRLEN] = {};
    if (InetNtopA(AF_INET, const_cast<IN_ADDR*>(&address.sin_addr), ip_buffer, sizeof(ip_buffer)) == nullptr)
    {
        return "unknown";
    }
    return ip_buffer;
}

int QueryLocalPort(const SOCKET sock)
{
    sockaddr_in local_address = {};
    int address_length = sizeof(local_address);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&local_address), &address_length) == SOCKET_ERROR)
    {
        return 0;
    }
    return static_cast<int>(ntohs(local_address.sin_port));
}

bool ReadOptionalInt64(const json& object, const char* key, std::int64_t& value)
{
    const auto it = object.find(key);
    if (it == object.end() || (!it->is_number_integer() && !it->is_number_unsigned()))
    {
        return false;
    }

    value = it->get<std::int64_t>();
    return true;
}

bool ReadOptionalUint64(const json& object, const char* key, std::uint64_t& value)
{
    const auto it = object.find(key);
    if (it == object.end() || (!it->is_number_integer() && !it->is_number_unsigned()))
    {
        return false;
    }

    value = it->get<std::uint64_t>();
    return true;
}

void ResetAttemptStats(ClockOffsetStats& stats)
{
    stats.last_raw_response_received = false;
    stats.last_response_parsed = false;
    stats.last_response_wall_ms.reset();
    stats.last_t1_ns.reset();
    stats.last_t2_ns.reset();
    stats.last_t3_ns.reset();
    stats.last_t4_ns.reset();
    stats.last_response_ip = "None";
    stats.last_response_port = 0;
    stats.last_response_bytes = 0;
    stats.last_payload_preview.clear();
    stats.last_error.clear();
}

} // namespace

ClockOffsetClient::~ClockOffsetClient()
{
    Shutdown();
}

bool ClockOffsetClient::Initialize(const ClockOffsetClientConfig& config, std::string& error_message)
{
    Shutdown();
    config_ = config;
    stats_.remote_ip = config.remote_ip;
    stats_.remote_port = config.remote_port;
    stats_.status = ClockOffsetStatus::kInitFailed;

    if (config.remote_port <= 0 || config.remote_port > std::numeric_limits<std::uint16_t>::max())
    {
        error_message = "clock offset remote_port 超出范围";
        stats_.last_error = error_message;
        return false;
    }

    WSADATA wsa_data = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
    {
        error_message = GetLastSocketErrorMessage("clock offset WSAStartup");
        stats_.last_error = error_message;
        return false;
    }
    started_winsock_ = true;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
    {
        error_message = GetLastSocketErrorMessage("clock offset socket(AF_INET, SOCK_DGRAM)");
        Shutdown();
        stats_.remote_ip = config.remote_ip;
        stats_.remote_port = config.remote_port;
        stats_.status = ClockOffsetStatus::kInitFailed;
        stats_.last_error = error_message;
        return false;
    }

    socket_handle_ = static_cast<std::uintptr_t>(sock);

    u_long non_blocking = 1;
    if (ioctlsocket(sock, FIONBIO, &non_blocking) != NO_ERROR)
    {
        error_message = GetLastSocketErrorMessage("clock offset ioctlsocket(FIONBIO)");
        Shutdown();
        stats_.remote_ip = config.remote_ip;
        stats_.remote_port = config.remote_port;
        stats_.status = ClockOffsetStatus::kInitFailed;
        stats_.last_error = error_message;
        return false;
    }

    sockaddr_in local_address = {};
    local_address.sin_family = AF_INET;
    local_address.sin_port = htons(0);
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, reinterpret_cast<const sockaddr*>(&local_address), sizeof(local_address)) == SOCKET_ERROR)
    {
        error_message = GetLastSocketErrorMessage("clock offset bind(local port)");
        Shutdown();
        stats_.remote_ip = config.remote_ip;
        stats_.remote_port = config.remote_port;
        stats_.status = ClockOffsetStatus::kInitFailed;
        stats_.last_error = error_message;
        return false;
    }

    stats_.initialized = true;
    stats_.local_port = QueryLocalPort(sock);
    stats_.status = ClockOffsetStatus::kIdle;
    stats_.last_error.clear();
    PrintClockOffsetLine(
        "[clock_offset] 已初始化, remote=" + stats_.remote_ip + ":" + std::to_string(stats_.remote_port) +
        ", local_port=" + std::to_string(stats_.local_port));
    return true;
}

void ClockOffsetClient::Shutdown()
{
    if (socket_handle_ != kInvalidSocketHandle)
    {
        closesocket(static_cast<SOCKET>(socket_handle_));
        socket_handle_ = kInvalidSocketHandle;
    }

    if (started_winsock_)
    {
        WSACleanup();
        started_winsock_ = false;
    }

    stats_.initialized = false;
    stats_.measurement_in_flight = false;
    stats_.last_request_sent = false;
    stats_.last_raw_response_received = false;
    stats_.last_response_parsed = false;
    stats_.local_port = 0;
    stats_.status = ClockOffsetStatus::kDisabled;
    pending_request_id_ = 0;
    pending_t1_ns_ = 0;
    pending_started_at_ = {};
}

void ClockOffsetClient::StartMeasurement()
{
    if (socket_handle_ == kInvalidSocketHandle)
    {
        if (stats_.status == ClockOffsetStatus::kDisabled)
        {
            return;
        }
        stats_.status = ClockOffsetStatus::kInitFailed;
        return;
    }
    if (stats_.measurement_in_flight)
    {
        return;
    }

    sockaddr_in remote_address = {};
    remote_address.sin_family = AF_INET;
    remote_address.sin_port = htons(static_cast<u_short>(config_.remote_port));
    if (InetPtonA(AF_INET, config_.remote_ip.c_str(), &remote_address.sin_addr) != 1)
    {
        stats_.last_error = "clock offset remote_ip 无效: " + config_.remote_ip;
        stats_.status = ClockOffsetStatus::kSocketError;
        return;
    }

    pending_request_id_ += 1;
    pending_t1_ns_ = CurrentWallTimeNs();
    ResetAttemptStats(stats_);

    json request = {
        { "type", "clock_offset_request" },
        { "request_id", pending_request_id_ },
        { "t1_ns", pending_t1_ns_ }
    };
    const std::string payload = request.dump();

    const int send_result = sendto(
        static_cast<SOCKET>(socket_handle_),
        payload.data(),
        static_cast<int>(payload.size()),
        0,
        reinterpret_cast<const sockaddr*>(&remote_address),
        sizeof(remote_address));

    if (send_result == SOCKET_ERROR)
    {
        stats_.last_error = GetLastSocketErrorMessage("send clock offset request");
        stats_.last_request_sent = false;
        stats_.status = ClockOffsetStatus::kSocketError;
        return;
    }

    stats_.measurement_in_flight = true;
    stats_.last_request_sent = true;
    stats_.last_request_id = pending_request_id_;
    stats_.last_request_wall_ms = pending_t1_ns_ / 1000000;
    stats_.status = ClockOffsetStatus::kRequestSent;
    pending_started_at_ = std::chrono::steady_clock::now();
    PrintClockOffsetLine(
        "[clock_offset] 已发送请求, request_id=" + std::to_string(pending_request_id_) +
        ", remote=" + config_.remote_ip + ":" + std::to_string(config_.remote_port) +
        ", local_port=" + std::to_string(stats_.local_port) +
        ", timeout=" + std::to_string(config_.timeout_ms) + "ms");
    PrintClockOffsetLine(
        "[clock_offset] 等待响应, request_id=" + std::to_string(pending_request_id_) +
        ", timeout=" + std::to_string(config_.timeout_ms) + "ms");
}

void ClockOffsetClient::Poll()
{
    if (socket_handle_ == kInvalidSocketHandle)
    {
        return;
    }

    std::array<char, kMaxDatagramSize> buffer = {};
    for (;;)
    {
        sockaddr_in from = {};
        int from_length = sizeof(from);
        const int received = recvfrom(
            static_cast<SOCKET>(socket_handle_),
            buffer.data(),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &from_length);

        if (received == SOCKET_ERROR)
        {
            if (WSAGetLastError() != WSAEWOULDBLOCK)
            {
                stats_.last_error = GetLastSocketErrorMessage("recv clock offset response");
                stats_.status = ClockOffsetStatus::kSocketError;
                spdlog::error("[错误] [clock_offset] {}", stats_.last_error);
            }
            break;
        }

        const std::int64_t t4_ns = CurrentWallTimeNs();
        stats_.last_raw_response_received = true;
        stats_.last_response_wall_ms = t4_ns / 1000000;
        stats_.last_response_ip = SockaddrToIpString(from);
        stats_.last_response_port = static_cast<int>(ntohs(from.sin_port));
        stats_.last_response_bytes = received;
        stats_.last_payload_preview =
            FormatPayloadPreview(buffer.data(), static_cast<std::size_t>(received));
        if (stats_.measurement_in_flight)
        {
            stats_.status = ClockOffsetStatus::kResponseReceived;
        }

        PrintClockOffsetLine(
            "[clock_offset] 收到 UDP 响应, source=" + stats_.last_response_ip + ":" +
            std::to_string(stats_.last_response_port) + ", bytes=" +
            std::to_string(stats_.last_response_bytes) + ", preview=" + stats_.last_payload_preview);

        try
        {
            const json response = json::parse(std::string(buffer.data(), static_cast<std::size_t>(received)));
            if (!response.is_object())
            {
                if (stats_.measurement_in_flight)
                {
                    stats_.measurement_in_flight = false;
                    stats_.status = ClockOffsetStatus::kInvalidResponse;
                    stats_.last_error = "clock offset response 的 root 不是 object";
                    spdlog::warn("[警告] [clock_offset] 响应无效: root 不是 object");
                }
                continue;
            }

            std::uint64_t request_id = 0;
            std::int64_t t1_ns = 0;
            std::int64_t t2_ns = 0;
            std::int64_t t3_ns = 0;
            if (!ReadOptionalUint64(response, "request_id", request_id) ||
                !ReadOptionalInt64(response, "t1_ns", t1_ns) ||
                !ReadOptionalInt64(response, "t2_ns", t2_ns) ||
                !ReadOptionalInt64(response, "t3_ns", t3_ns))
            {
                if (stats_.measurement_in_flight)
                {
                    stats_.measurement_in_flight = false;
                    stats_.status = ClockOffsetStatus::kInvalidResponse;
                    stats_.last_error = "clock offset response 缺少 request_id 或 t1/t2/t3";
                    spdlog::warn("[警告] [clock_offset] 响应无效: 缺少 request_id 或 t1/t2/t3");
                }
                continue;
            }

            stats_.last_response_parsed = true;
            stats_.last_t1_ns = t1_ns;
            stats_.last_t2_ns = t2_ns;
            stats_.last_t3_ns = t3_ns;
            stats_.last_t4_ns = t4_ns;

            if (!stats_.measurement_in_flight || request_id != pending_request_id_)
            {
                PrintClockOffsetLine(
                    "[clock_offset] 忽略过期响应, received_request_id=" + std::to_string(request_id) +
                    ", pending_request_id=" + std::to_string(pending_request_id_));
                continue;
            }
            if (t1_ns != pending_t1_ns_)
            {
                stats_.measurement_in_flight = false;
                stats_.status = ClockOffsetStatus::kInvalidResponse;
                stats_.last_error = "clock offset response 的 t1_ns 与 pending request 不匹配";
                spdlog::warn("[警告] [clock_offset] 响应无效: t1_ns 不匹配");
                continue;
            }

            const double offset_ns =
                (static_cast<double>(t2_ns - t1_ns) + static_cast<double>(t3_ns - t4_ns)) * 0.5;
            const double delay_ns =
                static_cast<double>(t4_ns - t1_ns) - static_cast<double>(t3_ns - t2_ns);

            stats_.last_valid_offset_ms = offset_ns / 1000000.0;
            stats_.last_valid_delay_ms = delay_ns / 1000000.0;
            stats_.measurement_in_flight = false;
            stats_.status = ClockOffsetStatus::kValid;
            stats_.last_error.clear();
            pending_t1_ns_ = 0;
            pending_started_at_ = {};
            PrintClockOffsetLine(
                "[clock_offset] 校准有效, request_id=" + std::to_string(request_id) +
                ", t1_ns=" + std::to_string(t1_ns) +
                ", t2_ns=" + std::to_string(t2_ns) +
                ", t3_ns=" + std::to_string(t3_ns) +
                ", t4_ns=" + std::to_string(t4_ns) +
                ", offset=" + std::to_string(*stats_.last_valid_offset_ms) + "ms" +
                ", rtt=" + std::to_string(*stats_.last_valid_delay_ms) + "ms");
        }
        catch (const std::exception& ex)
        {
            if (stats_.measurement_in_flight)
            {
                stats_.measurement_in_flight = false;
                stats_.last_response_parsed = false;
                stats_.status = ClockOffsetStatus::kInvalidResponse;
                stats_.last_error = std::string("解析 clock offset response 失败: ") + ex.what();
                spdlog::warn("[警告] [clock_offset] {}", stats_.last_error);
            }
            continue;
        }
    }

    if (stats_.measurement_in_flight &&
        (std::chrono::steady_clock::now() - pending_started_at_) >
            std::chrono::milliseconds(config_.timeout_ms))
    {
        stats_.measurement_in_flight = false;
        stats_.status = ClockOffsetStatus::kTimeout;
        stats_.last_error = "clock offset request 超时";
        PrintClockOffsetLine(
            "[clock_offset] 请求超时, request_id=" + std::to_string(pending_request_id_) +
            ", local_port=" + std::to_string(stats_.local_port) +
            ", remote=" + config_.remote_ip + ":" + std::to_string(config_.remote_port));
    }
}

std::optional<double> ClockOffsetClient::LatestOffsetMs() const
{
    if (!HasValidOffset())
    {
        return std::nullopt;
    }
    return stats_.last_valid_offset_ms;
}

bool ClockOffsetClient::HasValidOffset() const
{
    return stats_.last_valid_offset_ms.has_value();
}

const ClockOffsetStats& ClockOffsetClient::LastStats() const
{
    return stats_;
}
