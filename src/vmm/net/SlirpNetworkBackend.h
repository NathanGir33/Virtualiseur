#pragma once

#ifndef VIRTUALISEUR_NO_SLIRP

#include <winsock2.h>
#include <slirp/libslirp.h>

#include "NetworkBackend.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace vmm {

// User-mode NAT (libslirp): 10.0.2.0/24, gateway 10.0.2.2, DNS 10.0.2.3,
// guest 10.0.2.15 by DHCP. `restricted` isolates the guest (no Internet,
// no host): only the port forwards reach it ("réseau privé hôte").
class SlirpNetworkBackend final : public NetworkBackend {
public:
    explicit SlirpNetworkBackend(ReceiveFrame receiveFrame, bool restricted = false,
                                 const std::vector<PortForward>& forwards = {});
    ~SlirpNetworkBackend() override;
    SlirpNetworkBackend(const SlirpNetworkBackend&) = delete;
    SlirpNetworkBackend& operator=(const SlirpNetworkBackend&) = delete;

    // TX is queued so slirp_input never runs while virtio-net holds its lock.
    bool Send(std::span<const std::uint8_t> frame) override;

private:
    struct Timer;
    struct PollContext;

    static std::int64_t ClockGetNs(void* opaque);
    static std::ptrdiff_t SendPacket(const void* packet, std::size_t length, void* opaque);
    static void GuestError(const char* message, void* opaque);
    static void* TimerNew(SlirpTimerId id, void* callbackOpaque, void* opaque);
    static void TimerFree(void* timer, void* opaque);
    static void TimerMod(void* timer, std::int64_t expireTimeMs, void* opaque);
    static void Notify(void* opaque);
    static int AddPoll(slirp_os_socket socket, int events, void* opaque);
    static int GetRevents(int index, void* opaque);

    void Run();
    void Wake() noexcept;
    void ProcessTimers();

    ReceiveFrame receiveFrame_;
    SlirpCb callbacks_{};
    struct Slirp* slirp_ = nullptr;
    std::thread worker_;
    std::mutex mutex_;
    std::vector<std::vector<std::uint8_t>> transmitQueue_;
    std::vector<Timer*> timers_;
    std::atomic_bool stopping_ = false;
    SOCKET wakeReceiver_ = INVALID_SOCKET;
    SOCKET wakeSender_ = INVALID_SOCKET;
    sockaddr_in wakeAddress_{};
};

} // namespace vmm

#else

#include <cstdint>
#include <span>

#include "NetworkBackend.h"

namespace vmm {
// Placeholder used when the build has no libslirp: NAT is unavailable.
class SlirpNetworkBackend final : public NetworkBackend {
public:
    bool Send(std::span<const std::uint8_t>) override { return false; }
};
} // namespace vmm

#endif // VIRTUALISEUR_NO_SLIRP
