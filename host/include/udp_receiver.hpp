#pragma once

#include "types.hpp"

#include <string>

class UdpReceiver
{
public:
    UdpReceiver() = default;
    ~UdpReceiver();

    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    bool Open(const std::string& listen_ip, int listen_port, std::string& error_message);
    void Close();
    bool ReceiveLatest(UdpPacket& packet);

private:
    bool started_winsock_ = false;
    std::uintptr_t socket_handle_ = static_cast<std::uintptr_t>(-1);
};
