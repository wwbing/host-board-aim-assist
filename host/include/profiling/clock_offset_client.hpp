#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

enum class ClockOffsetStatus
{
    kDisabled,
    kIdle,
    kRequestSent,
    kResponseReceived,
    kValid,
    kTimeout,
    kInvalidResponse,
    kSocketError,
    kInitFailed,
};

struct ClockOffsetClientConfig
{
    std::string remote_ip = "192.168.7.2";
    int remote_port = 45678;
    int timeout_ms = 250;
};

struct ClockOffsetStats
{
    bool initialized = false;
    bool measurement_in_flight = false;
    bool last_request_sent = false;
    bool last_raw_response_received = false;
    bool last_response_parsed = false;
    std::uint64_t last_request_id = 0;
    std::string remote_ip = "无";
    int remote_port = 0;
    int local_port = 0;
    ClockOffsetStatus status = ClockOffsetStatus::kDisabled;
    std::optional<double> last_valid_offset_ms;
    std::optional<double> last_valid_delay_ms;
    std::optional<std::int64_t> last_request_wall_ms;
    std::optional<std::int64_t> last_response_wall_ms;
    std::optional<std::int64_t> last_t1_ns;
    std::optional<std::int64_t> last_t2_ns;
    std::optional<std::int64_t> last_t3_ns;
    std::optional<std::int64_t> last_t4_ns;
    std::string last_response_ip = "无";
    int last_response_port = 0;
    int last_response_bytes = 0;
    std::string last_payload_preview;
    std::string last_error;
};

class ClockOffsetClient
{
public:
    ClockOffsetClient() = default;
    ~ClockOffsetClient();

    ClockOffsetClient(const ClockOffsetClient&) = delete;
    ClockOffsetClient& operator=(const ClockOffsetClient&) = delete;

    bool Initialize(const ClockOffsetClientConfig& config, std::string& error_message);
    void Shutdown();
    void StartMeasurement();
    void Poll();
    std::optional<double> LatestOffsetMs() const;
    bool HasValidOffset() const;
    const ClockOffsetStats& LastStats() const;

private:
    ClockOffsetClientConfig config_;
    ClockOffsetStats stats_;
    std::uintptr_t socket_handle_ = static_cast<std::uintptr_t>(-1);
    bool started_winsock_ = false;
    std::uint64_t pending_request_id_ = 0;
    std::int64_t pending_t1_ns_ = 0;
    std::chrono::steady_clock::time_point pending_started_at_ = {};
};
