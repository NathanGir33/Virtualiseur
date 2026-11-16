#pragma once

#include "NetworkBackend.h"

#include <Windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace vmm {

// Bridged networking ("accès par pont"): the guest sits on the physical
// network of a host adapter, with its own address from the LAN's DHCP.
// Frames are captured and injected with Npcap (wpcap.dll, as Wireshark
// uses), so no Virtualiseur driver is needed.
//
// Wired adapters are bridged transparently (promiscuous capture, the guest
// keeps its MAC). A Wi-Fi access point only accepts frames from the MAC
// that associated with it, so on Wi-Fi the bridge translates addresses like
// VirtualBox: the guest's frames leave with the host's MAC, ARP is rewritten,
// DHCP asks for broadcast replies, and incoming frames for the guest's IPv4
// address are handed back to it. IPv6 is not bridged over Wi-Fi.
class BridgeNetworkBackend final : public NetworkBackend {
public:
    // `adapter`: friendly name ("Wi-Fi"), description or GUID; empty picks
    // the first connected Ethernet or Wi-Fi adapter.
    BridgeNetworkBackend(ReceiveFrame receive, const std::string& adapter, const MacAddress& guestMac);
    ~BridgeNetworkBackend() override;

    bool Send(std::span<const std::uint8_t> frame) override;

private:
    struct Api;
    void Run();
    void Handle(const std::uint8_t* data, std::size_t length);
    void ApplyFilter();
    void LearnGuestIp(std::uint32_t address);

    ReceiveFrame receive_;
    std::unique_ptr<Api> api_;
    void* rx_ = nullptr;
    void* tx_ = nullptr;
    MacAddress guestMac_{};
    MacAddress hostMac_{};
    bool wireless_ = false;
    std::string adapterName_;
    std::atomic<std::uint32_t> guestIp_ = 0; // Network byte order, 0 = unknown (Wi-Fi).
    std::atomic_bool refilter_ = false;
    // One-time diagnostics in vmm.log.
    bool loggedSend_ = false;
    bool loggedSendError_ = false;
    bool loggedDhcpRequest_ = false;
    std::atomic_bool loggedReceive_ = false;
    std::atomic_bool loggedDhcpReply_ = false;
    std::vector<std::uint8_t> transmit_;     // Send() scratch (device lock held).
    std::vector<std::uint8_t> incoming_;     // Receive thread scratch.
    HANDLE stop_ = nullptr;
    std::thread thread_;
};

} // namespace vmm
