#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace vmm {

using MacAddress = std::array<std::uint8_t, 6>;

// NAT port forwarding: host 127.0.0.1:hostPort -> guest:guestPort.
struct PortForward {
    bool udp = false;
    std::uint16_t hostPort = 0;
    std::uint16_t guestPort = 0;
};

// The guest side of a backend. `deliver` puts a frame in the guest's RX
// ring; `flush` interrupts the guest for the frames delivered since the
// previous flush. Backends flush at the end of each burst (one poll round,
// one capture batch), so a burst costs one interrupt instead of one per
// frame. Both run on the backend's own thread and take the device lock
// themselves.
struct GuestPort {
    std::function<void(std::span<const std::uint8_t>)> deliver;
    std::function<void()> flush;
};

// Where the virtio-net card's frames go.
class NetworkBackend {
public:
    virtual ~NetworkBackend() = default;
    // Called by virtio-net with the device lock held: must not block for long.
    virtual bool Send(std::span<const std::uint8_t> frame) = 0;
};

std::string FormatMac(const MacAddress& mac);
bool ParseMac(const std::string& text, MacAddress& mac);

inline bool IsMulticast(const std::uint8_t* mac) noexcept { return (mac[0] & 1) != 0; }

} // namespace vmm
