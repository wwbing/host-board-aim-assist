#include "udp_receiver.hpp"
#include "logging/logger.hpp"

#include <Winsock2.h>
#include <Ws2tcpip.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>

namespace
{

constexpr std::uintptr_t kInvalidSocketHandle = static_cast<std::uintptr_t>(INVALID_SOCKET);
constexpr int kMaxDatagramSize = 64 * 1024;

std::string GetLastSocketErrorMessage(const char* prefix)
{
    return logging::FormatLastSocketError(prefix);
}

} // namespace

UdpReceiver::~UdpReceiver()
{
    Close();
}

bool UdpReceiver::Open(const std::string& listen_ip, const int listen_port, std::string& error_message)
{
    Close();

    if (listen_port <= 0 || listen_port > std::numeric_limits<std::uint16_t>::max())
    {
        error_message = "listen_port 超出范围";
        return false;
    }

    WSADATA wsa_data = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
    {
        error_message = GetLastSocketErrorMessage("WSAStartup");
        return false;
    }
    started_winsock_ = true;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
    {
        error_message = GetLastSocketErrorMessage("socket(AF_INET, SOCK_DGRAM)");
        Close();
        return false;
    }
    socket_handle_ = static_cast<std::uintptr_t>(sock);

    u_long non_blocking = 1;
    if (ioctlsocket(sock, FIONBIO, &non_blocking) != NO_ERROR)
    {
        error_message = GetLastSocketErrorMessage("ioctlsocket(FIONBIO)");
        Close();
        return false;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(listen_port));

    if (listen_ip == "0.0.0.0" || listen_ip.empty())
    {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    else if (InetPtonA(AF_INET, listen_ip.c_str(), &address.sin_addr) != 1)
    {
        error_message = "listen_ip 无效: " + listen_ip;
        Close();
        return false;
    }

    if (bind(sock, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        error_message = GetLastSocketErrorMessage("bind(listen address)");
        Close();
        return false;
    }

    return true;
}

void UdpReceiver::Close()
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
}

bool UdpReceiver::ReceiveLatest(UdpPacket& packet)
{
    if (socket_handle_ == kInvalidSocketHandle)
    {
        return false;
    }

    SOCKET sock = static_cast<SOCKET>(socket_handle_);
    std::array<char, kMaxDatagramSize> buffer = {};
    bool received_any = false;

    for (;;)
    {
        sockaddr_in from = {};
        int from_length = sizeof(from);
        const int received = recvfrom(
            sock,
            buffer.data(),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &from_length);

        if (received == SOCKET_ERROR)
        {
            if (WSAGetLastError() == WSAEWOULDBLOCK)
            {
                break;
            }
            break;
        }

        char ip_buffer[INET_ADDRSTRLEN] = {};
        if (InetNtopA(AF_INET, &from.sin_addr, ip_buffer, sizeof(ip_buffer)) == nullptr)
        {
            strcpy_s(ip_buffer, sizeof(ip_buffer), "unknown");
        }

        packet.payload.assign(buffer.data(), received);
        packet.sender_ip = ip_buffer;
        packet.sender_port = ntohs(from.sin_port);
        received_any = true;
    }

    return received_any;
}
