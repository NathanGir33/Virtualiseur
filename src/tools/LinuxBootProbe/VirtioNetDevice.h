#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <atomic>
#include <mutex>
#include <span>
#include <vector>

// Minimal transitional PCI VirtIO-net device. It implements the legacy I/O
// register interface and split RX/TX virtqueues used by Linux virtio_net.
// The caller supplies a GPA mapper and a packet sink for the host networking
// backend; packets are never interpreted as host pointers.
class VirtioNetDevice final {
public:
    using MapGuestMemory = std::function<std::uint8_t*(std::uint64_t, std::size_t)>;
    using TransmitPacket = std::function<bool(std::span<const std::uint8_t>)>;

    VirtioNetDevice(MapGuestMemory mapMemory, TransmitPacket transmit);
    bool HandlesPort(std::uint16_t port) const noexcept;
    std::uint32_t Read(std::uint16_t port, std::uint8_t size);
    void Write(std::uint16_t port, std::uint8_t size, std::uint32_t value);
    bool TakeInterrupt() noexcept;
    bool LinkUp() const noexcept { return linkUp_; }
    void SetLinkUp(bool value) noexcept { linkUp_.store(value, std::memory_order_release); }

    // Queues an Ethernet frame received from the host-side backend. Returns
    // false if the guest has not posted a writable RX buffer yet.
    bool Receive(std::span<const std::uint8_t> frame);

private:
    struct Queue {
        std::uint32_t pfn = 0;
        std::uint16_t size = 0;
        std::uint16_t lastAvailable = 0;
        bool ready = false;
    };
    struct Descriptor {
        std::uint64_t address;
        std::uint32_t length;
        std::uint16_t flags;
        std::uint16_t next;
    };

    std::uint32_t ReadRegister(std::uint8_t offset, std::uint8_t size);
    void WriteRegister(std::uint8_t offset, std::uint8_t size, std::uint32_t value);
    std::uint8_t* Map(std::uint64_t address, std::size_t size) const;
    bool ReadDescriptor(const Queue& queue, std::uint16_t index, Descriptor& descriptor) const;
    std::uint64_t QueueBase(const Queue& queue) const noexcept;
    std::uint64_t AvailableBase(const Queue& queue) const noexcept;
    std::uint64_t UsedBase(const Queue& queue) const noexcept;
    void NotifyQueue(std::uint16_t index);
    bool CompleteRx(std::span<const std::uint8_t> frame);
    void SetInterrupt();

    MapGuestMemory mapMemory_;
    TransmitPacket transmit_;
    std::array<Queue, 2> queues_{}; // 0=receive, 1=transmit.
    std::array<std::uint8_t, 6> mac_{0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    std::uint16_t selectedQueue_ = 0;
    std::uint32_t guestFeatures_ = 0;
    std::uint8_t deviceStatus_ = 0;
    std::uint8_t interruptStatus_ = 0;
    bool interruptDeliveryPending_ = false;
    std::atomic_bool linkUp_ = false;
    mutable std::mutex mutex_;
};
