#ifndef VIRTUALISEUR_NO_SLIRP
#include "SlirpNetworkBackend.h"

#include "../core/Log.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace vmm {

namespace {
using Clock = std::chrono::steady_clock;

std::int64_t MonotonicMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count();
}

short ToWinsockEvents(int events)
{
    short result = 0;
    if ((events & SLIRP_POLL_IN) != 0) result |= POLLRDNORM;
    if ((events & SLIRP_POLL_OUT) != 0) result |= POLLWRNORM;
    // WSAPoll rejects POLLPRI with WSAEINVAL, which would stall every socket.
    // TCP urgent data is obsolete, so it is not polled.
    return result;
}

int ToSlirpEvents(short events)
{
    int result = 0;
    if ((events & (POLLRDNORM | POLLIN)) != 0) result |= SLIRP_POLL_IN;
    if ((events & (POLLWRNORM | POLLOUT)) != 0) result |= SLIRP_POLL_OUT;
    if ((events & POLLPRI) != 0) result |= SLIRP_POLL_PRI;
    if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) result |= SLIRP_POLL_ERR;
    if ((events & POLLHUP) != 0) result |= SLIRP_POLL_HUP;
    return result;
}

void ParseIpv4(const char* text, in_addr& address)
{
    if (InetPtonA(AF_INET, text, &address) != 1) {
        throw std::runtime_error(std::string("Adresse IPv4 libslirp invalide: ") + text);
    }
}
} // namespace

struct SlirpNetworkBackend::Timer {
    SlirpTimerId id{};
    void* callbackOpaque = nullptr;
    std::int64_t expireTimeMs = -1;
    bool armed = false;
};

struct SlirpNetworkBackend::PollContext {
    std::vector<WSAPOLLFD> descriptors;
};

SlirpNetworkBackend::SlirpNetworkBackend(ReceiveFrame receiveFrame, bool restricted,
                                         const std::vector<PortForward>& forwards)
    : receiveFrame_(std::move(receiveFrame))
{
    if (!receiveFrame_) throw std::invalid_argument("Le callback RX de libslirp est obligatoire.");
    WSADATA winsock{};
    const int startupResult = WSAStartup(MAKEWORD(2, 2), &winsock);
    if (startupResult != 0) {
        throw std::runtime_error("WSAStartup a échoué avant l'initialisation de libslirp.");
    }

    try {
        wakeReceiver_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        wakeSender_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (wakeReceiver_ == INVALID_SOCKET || wakeSender_ == INVALID_SOCKET) {
            throw std::runtime_error("Impossible de créer le socket local de réveil libslirp.");
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = 0;
        if (bind(wakeReceiver_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
            throw std::runtime_error("Impossible d'attacher le socket de réveil libslirp à localhost.");
        }
        int addressLength = sizeof(wakeAddress_);
        if (getsockname(wakeReceiver_, reinterpret_cast<sockaddr*>(&wakeAddress_), &addressLength) == SOCKET_ERROR) {
            throw std::runtime_error("Impossible de lire l'adresse du socket de réveil libslirp.");
        }
        u_long nonBlocking = 1;
        if (ioctlsocket(wakeReceiver_, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
            throw std::runtime_error("Impossible de rendre le socket de réveil libslirp non bloquant.");
        }

        SlirpConfig config{};
        config.version = SLIRP_CONFIG_VERSION_MAX;
        config.restricted = restricted ? 1 : 0;
        config.in_enabled = true;
        config.in6_enabled = false;
        ParseIpv4("10.0.2.0", config.vnetwork);
        ParseIpv4("255.255.255.0", config.vnetmask);
        ParseIpv4("10.0.2.2", config.vhost);
        ParseIpv4("10.0.2.15", config.vdhcp_start);
        ParseIpv4("10.0.2.3", config.vnameserver);
        config.if_mtu = 1500;
        config.if_mru = 1500;

        // libslirp keeps a pointer to the callback table: it must outlive slirp_.
        auto& callbacks = callbacks_;
        callbacks = {};
        callbacks.send_packet = &SlirpNetworkBackend::SendPacket;
        callbacks.guest_error = &SlirpNetworkBackend::GuestError;
        callbacks.clock_get_ns = &SlirpNetworkBackend::ClockGetNs;
        callbacks.timer_new_opaque = &SlirpNetworkBackend::TimerNew;
        callbacks.timer_free = &SlirpNetworkBackend::TimerFree;
        callbacks.timer_mod = &SlirpNetworkBackend::TimerMod;
        callbacks.notify = &SlirpNetworkBackend::Notify;
        // Sockets are polled from Run() each iteration: registration is a no-op,
        // but libslirp calls these unconditionally with config version 6.
        callbacks.register_poll_socket = [](slirp_os_socket, void*) {};
        callbacks.unregister_poll_socket = [](slirp_os_socket, void*) {};
        callbacks.init_completed = [](Slirp*, void*) {};
        slirp_ = slirp_new(&config, &callbacks, this);
        if (slirp_ == nullptr) throw std::runtime_error("libslirp n'a pas pu créer le réseau NAT invité.");
        // Port forwards listen on 127.0.0.1 only: no firewall prompt, and
        // the guest is not exposed to the local network.
        in_addr hostAddress{};
        hostAddress.s_addr = htonl(INADDR_LOOPBACK);
        in_addr guestAddress{};
        ParseIpv4("10.0.2.15", guestAddress);
        for (const auto& forward : forwards) {
            const char* protocol = forward.udp ? "UDP" : "TCP";
            if (slirp_add_hostfwd(slirp_, forward.udp ? 1 : 0, hostAddress, forward.hostPort, guestAddress,
                                  forward.guestPort) == 0) {
                VMM_INFO("NAT : redirection %s 127.0.0.1:%u -> invité:%u.", protocol, forward.hostPort, forward.guestPort);
            } else {
                VMM_WARN("NAT : redirection %s du port %u impossible (port déjà utilisé sur l'hôte ?).", protocol,
                         forward.hostPort);
            }
        }
        if (restricted) VMM_INFO("Réseau privé hôte : la machine n'accède ni à Internet ni à l'hôte.");
        worker_ = std::thread(&SlirpNetworkBackend::Run, this);
    } catch (...) {
        if (slirp_ != nullptr) {
            slirp_cleanup(slirp_);
            slirp_ = nullptr;
        }
        if (wakeSender_ != INVALID_SOCKET) closesocket(wakeSender_);
        if (wakeReceiver_ != INVALID_SOCKET) closesocket(wakeReceiver_);
        WSACleanup();
        throw;
    }
}

SlirpNetworkBackend::~SlirpNetworkBackend()
{
    stopping_.store(true, std::memory_order_release);
    Wake();
    if (worker_.joinable()) worker_.join();
    if (slirp_ != nullptr) slirp_cleanup(slirp_);
    if (wakeSender_ != INVALID_SOCKET) closesocket(wakeSender_);
    if (wakeReceiver_ != INVALID_SOCKET) closesocket(wakeReceiver_);
    WSACleanup();
}

bool SlirpNetworkBackend::Send(std::span<const std::uint8_t> frame)
{
    if (frame.size() < 14 || frame.size() > 65536 || stopping_.load(std::memory_order_acquire)) return false;
    {
        const std::lock_guard lock(mutex_);
        if (transmitQueue_.size() >= 1024) return false;
        transmitQueue_.emplace_back(frame.begin(), frame.end());
    }
    Wake();
    return true;
}

std::int64_t SlirpNetworkBackend::ClockGetNs(void*)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}

std::ptrdiff_t SlirpNetworkBackend::SendPacket(const void* packet, std::size_t length, void* opaque)
{
    auto* self = static_cast<SlirpNetworkBackend*>(opaque);
    if (self->receiveFrame_) {
        try {
            self->receiveFrame_(std::span(static_cast<const std::uint8_t*>(packet), length));
        } catch (...) {
            // A guest with no posted RX buffer may drop this frame; TCP retries it.
        }
    }
    return static_cast<std::ptrdiff_t>(length);
}

void SlirpNetworkBackend::GuestError(const char* message, void*)
{
    if (message != nullptr) std::cerr << "libslirp: " << message << '\n';
}

void* SlirpNetworkBackend::TimerNew(SlirpTimerId id, void* callbackOpaque, void* opaque)
{
    auto* self = static_cast<SlirpNetworkBackend*>(opaque);
    auto* timer = new Timer{id, callbackOpaque, -1, false};
    {
        const std::lock_guard lock(self->mutex_);
        self->timers_.push_back(timer);
    }
    return timer;
}

void SlirpNetworkBackend::TimerFree(void* value, void* opaque)
{
    auto* self = static_cast<SlirpNetworkBackend*>(opaque);
    auto* timer = static_cast<Timer*>(value);
    if (timer == nullptr) return;
    {
        const std::lock_guard lock(self->mutex_);
        const auto found = std::find(self->timers_.begin(), self->timers_.end(), timer);
        if (found != self->timers_.end()) self->timers_.erase(found);
    }
    delete timer;
}

void SlirpNetworkBackend::TimerMod(void* value, std::int64_t expireTimeMs, void* opaque)
{
    auto* self = static_cast<SlirpNetworkBackend*>(opaque);
    auto* timer = static_cast<Timer*>(value);
    if (timer == nullptr) return;
    {
        const std::lock_guard lock(self->mutex_);
        timer->expireTimeMs = expireTimeMs;
        timer->armed = expireTimeMs >= 0;
    }
    self->Wake();
}

void SlirpNetworkBackend::Notify(void* opaque)
{
    static_cast<SlirpNetworkBackend*>(opaque)->Wake();
}

int SlirpNetworkBackend::AddPoll(slirp_os_socket socket, int events, void* opaque)
{
    auto* context = static_cast<PollContext*>(opaque);
    WSAPOLLFD descriptor{};
    descriptor.fd = socket;
    descriptor.events = ToWinsockEvents(events);
    context->descriptors.push_back(descriptor);
    return static_cast<int>(context->descriptors.size() - 1);
}

int SlirpNetworkBackend::GetRevents(int index, void* opaque)
{
    const auto* context = static_cast<PollContext*>(opaque);
    if (index < 0 || static_cast<std::size_t>(index) >= context->descriptors.size()) return 0;
    return ToSlirpEvents(context->descriptors[static_cast<std::size_t>(index)].revents);
}

void SlirpNetworkBackend::Wake() noexcept
{
    if (wakeSender_ == INVALID_SOCKET) return;
    constexpr char byte = 1;
    (void)sendto(wakeSender_, &byte, sizeof(byte), 0,
        reinterpret_cast<const sockaddr*>(&wakeAddress_), sizeof(wakeAddress_));
}

void SlirpNetworkBackend::ProcessTimers()
{
    for (;;) {
        Timer* due = nullptr;
        const auto now = MonotonicMilliseconds();
        {
            const std::lock_guard lock(mutex_);
            const auto found = std::find_if(timers_.begin(), timers_.end(), [now](const Timer* timer) {
                return timer->armed && timer->expireTimeMs <= now;
            });
            if (found == timers_.end()) return;
            due = *found;
            due->armed = false;
        }
        slirp_handle_timer(slirp_, due->id, due->callbackOpaque);
    }
}

void SlirpNetworkBackend::Run()
{
    std::array<char, 256> wakeBuffer{};
    while (!stopping_.load(std::memory_order_acquire)) {
        std::vector<std::vector<std::uint8_t>> outbound;
        {
            const std::lock_guard lock(mutex_);
            outbound.swap(transmitQueue_);
        }
        for (const auto& frame : outbound) {
            if (frame.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
                slirp_input(slirp_, frame.data(), static_cast<int>(frame.size()));
            }
        }
        ProcessTimers();

        PollContext pollContext;
        std::uint32_t timeoutMs = 1000;
        slirp_pollfds_fill_socket(slirp_, &timeoutMs, &SlirpNetworkBackend::AddPoll, &pollContext);
        const auto now = MonotonicMilliseconds();
        {
            const std::lock_guard lock(mutex_);
            for (const auto* timer : timers_) {
                if (timer->armed) {
                    const auto remaining = timer->expireTimeMs <= now ? 0 : timer->expireTimeMs - now;
                    timeoutMs = (std::min)(timeoutMs, static_cast<std::uint32_t>(
                        (std::min<std::int64_t>)(remaining, 1000)));
                }
            }
        }
        WSAPOLLFD wakeDescriptor{};
        wakeDescriptor.fd = wakeReceiver_;
        wakeDescriptor.events = POLLRDNORM;
        pollContext.descriptors.push_back(wakeDescriptor);

        const int result = WSAPoll(pollContext.descriptors.data(),
            static_cast<ULONG>(pollContext.descriptors.size()), static_cast<int>(timeoutMs));
        if (result == SOCKET_ERROR) {
            static std::atomic_int reported = 0;
            if (reported.fetch_add(1) < 5) std::cerr << "libslirp: WSAPoll a échoué (" << WSAGetLastError() << ").\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (result != SOCKET_ERROR && !pollContext.descriptors.empty() &&
            (pollContext.descriptors.back().revents & (POLLRDNORM | POLLIN)) != 0) {
            while (recv(wakeReceiver_, wakeBuffer.data(), static_cast<int>(wakeBuffer.size()), 0) > 0) {}
        }
        slirp_pollfds_poll(slirp_, result == SOCKET_ERROR ? 1 : 0,
                           &SlirpNetworkBackend::GetRevents, &pollContext);
        ProcessTimers();
    }
}

} // namespace vmm

#endif // VIRTUALISEUR_NO_SLIRP
