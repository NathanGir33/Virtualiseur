#include "InternalNetworkBackend.h"

#include "../core/Log.h"

#include <ws2tcpip.h>

#include <cctype>
#include <cstring>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

namespace vmm {
namespace {

constexpr std::uint32_t kMagic = 0x4E575356; // "VSWN".
constexpr std::size_t kTagSize = 8;
constexpr std::size_t kMaxFrame = 65536;

std::uint32_t HashName(const std::string& name)
{
    std::uint32_t hash = 2166136261u; // FNV-1a, case-insensitive.
    for (const unsigned char ch : name) {
        hash ^= static_cast<std::uint32_t>(std::tolower(ch));
        hash *= 16777619u;
    }
    return hash;
}

} // namespace

InternalNetworkBackend::InternalNetworkBackend(ReceiveFrame receive, const std::string& name, const MacAddress& mac)
    : receive_(std::move(receive)), mac_(mac), tag_(HashName(name))
{
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) throw std::runtime_error("WSAStartup a échoué.");
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) {
        WSACleanup();
        throw std::runtime_error("Réseau interne : création du socket impossible.");
    }
    // One port per network name; every VM of the network shares it.
    const auto port = static_cast<std::uint16_t>(47000 + tag_ % 2000);
    group_.sin_family = AF_INET;
    group_.sin_port = htons(port);
    InetPtonA(AF_INET, "239.255.86.1", &group_.sin_addr);

    const BOOL yes = TRUE;
    const DWORD loop = 1;
    const DWORD ttl = 1;
    in_addr loopback{};
    loopback.s_addr = htonl(INADDR_LOOPBACK);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr = loopback;
    ip_mreq membership{};
    membership.imr_multiaddr = group_.sin_addr;
    membership.imr_interface = loopback;
    const int buffer = 1 << 20;
    if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes)) != 0 ||
        bind(socket_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0 ||
        setsockopt(socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&membership), sizeof(membership)) != 0 ||
        setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&loopback), sizeof(loopback)) != 0 ||
        setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop)) != 0 ||
        setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl)) != 0) {
        const int error = WSAGetLastError();
        closesocket(socket_);
        WSACleanup();
        throw std::runtime_error("Réseau interne : configuration du multicast local impossible (erreur " +
                                 std::to_string(error) + ").");
    }
    setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
    setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
    VMM_INFO("Réseau interne « %s » : carte %s reliée (port local %u).", name.c_str(), FormatMac(mac_).c_str(), port);
    thread_ = std::thread([this] { Run(); });
}

InternalNetworkBackend::~InternalNetworkBackend()
{
    stopping_ = true;
    closesocket(socket_); // Unblocks recvfrom.
    if (thread_.joinable()) thread_.join();
    WSACleanup();
}

bool InternalNetworkBackend::Send(std::span<const std::uint8_t> frame)
{
    if (frame.size() < 14 || frame.size() > kMaxFrame || stopping_) return false;
    std::uint32_t header[2] = {kMagic, tag_};
    WSABUF buffers[2] = {{static_cast<ULONG>(kTagSize), reinterpret_cast<char*>(header)},
                         {static_cast<ULONG>(frame.size()), reinterpret_cast<char*>(const_cast<std::uint8_t*>(frame.data()))}};
    DWORD sent = 0;
    return WSASendTo(socket_, buffers, 2, &sent, 0, reinterpret_cast<const sockaddr*>(&group_), sizeof(group_),
                     nullptr, nullptr) == 0;
}

void InternalNetworkBackend::Run()
{
    SetThreadDescription(GetCurrentThread(), L"Réseau interne");
    std::vector<std::uint8_t> packet(kTagSize + kMaxFrame);
    while (!stopping_) {
        const int length = recv(socket_, reinterpret_cast<char*>(packet.data()), static_cast<int>(packet.size()), 0);
        if (length == SOCKET_ERROR) {
            if (stopping_) break;
            if (WSAGetLastError() == WSAEMSGSIZE) continue;
            Sleep(10);
            continue;
        }
        if (length < static_cast<int>(kTagSize + 14)) continue;
        std::uint32_t magic = 0;
        std::uint32_t tag = 0;
        std::memcpy(&magic, packet.data(), 4);
        std::memcpy(&tag, packet.data() + 4, 4);
        if (magic != kMagic || tag != tag_) continue;
        const auto* frame = packet.data() + kTagSize;
        // Our own frames come back through the loopback: skip them. Keep
        // the frames for our MAC and the broadcasts/multicasts.
        if (std::memcmp(frame + 6, mac_.data(), 6) == 0) continue;
        if (!IsMulticast(frame) && std::memcmp(frame, mac_.data(), 6) != 0) continue;
        receive_(std::span(frame, static_cast<std::size_t>(length) - kTagSize));
    }
}

} // namespace vmm
