#pragma once

#include "NetworkBackend.h"

#include <winsock2.h>

#include <atomic>
#include <string>
#include <thread>

namespace vmm {

// "Internal network" (as in VirtualBox): the VMs attached to the same name
// share a virtual Ethernet segment, without the host or the outside world.
//
// The segment is a hub carried by UDP multicast on the loopback interface
// only (bound to 127.0.0.1: no firewall prompt, nothing leaves the PC). Each
// datagram is a tag (magic + network name hash) followed by the Ethernet
// frame; every VMM receives every frame and keeps those for its own MAC,
// broadcasts and multicasts.
class InternalNetworkBackend final : public NetworkBackend {
public:
    InternalNetworkBackend(GuestPort guest, const std::string& name, const MacAddress& mac);
    ~InternalNetworkBackend() override;

    bool Send(std::span<const std::uint8_t> frame) override;

private:
    void Run();

    GuestPort guest_;
    MacAddress mac_;
    std::uint32_t tag_ = 0;
    sockaddr_in group_{};
    SOCKET socket_ = INVALID_SOCKET;
    std::atomic_bool stopping_ = false;
    std::thread thread_;
};

} // namespace vmm
