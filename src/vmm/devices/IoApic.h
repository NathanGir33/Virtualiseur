#pragma once

#include "../core/Bus.h"
#include "WindowsHypervisorBackend.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace vmm {

// Intel 82093AA I/O APIC (24 inputs). Device models drive input lines; the
// I/O APIC turns them into messages delivered through WHP's emulated local
// APICs. Level-triggered inputs keep their Remote IRR bit until WHP reports
// the guest EOI (WHvRunVpExitReasonX64ApicEoi).
//
// Thread-safe on its own (internal lock, taken last): devices raise lines
// under their own locks, vCPUs report EOIs without any device lock.
class IoApic final : public MmioDevice, public InterruptSink {
public:
    static constexpr std::uint32_t kPins = 24;

    IoApic(virtualiseur::whp::Partition& partition, std::uint32_t id);

    std::uint64_t MmioRead(std::uint64_t address, std::uint8_t size) override;
    void MmioWrite(std::uint64_t address, std::uint8_t size, std::uint64_t value) override;

    void SetGsi(std::uint32_t gsi, bool asserted) override;
    // MSI messages bypass the I/O APIC pins: they go straight to WHP's local
    // APICs (thread-safe, no device state touched).
    void SignalMsi(std::uint64_t address, std::uint32_t data) override;
    void EndOfInterrupt(std::uint32_t vector);

    std::uint64_t DeliveredCount() const noexcept { return delivered_.load(); }
    std::uint64_t MsiCount() const noexcept { return msiDelivered_.load(); }
    bool AnyPinProgrammed() const noexcept { return programmed_.load(); }

private:
    std::uint32_t ReadRegister(std::uint32_t index) const;
    void WriteRegister(std::uint32_t index, std::uint32_t value);
    void Service(std::uint32_t pin);
    void Deliver(std::uint32_t pin);

    static bool Masked(std::uint64_t entry) noexcept { return (entry & (1ull << 16)) != 0; }
    static bool LevelTriggered(std::uint64_t entry) noexcept { return (entry & (1ull << 15)) != 0; }
    static bool RemoteIrr(std::uint64_t entry) noexcept { return (entry & (1ull << 14)) != 0; }

    virtualiseur::whp::Partition& partition_;
    std::mutex mutex_;
    std::array<std::uint64_t, kPins> redirection_{};
    std::array<bool, kPins> lines_{};
    std::array<bool, kPins> pendingEdge_{};
    std::uint32_t select_ = 0;
    std::uint32_t id_ = 0;
    std::atomic_uint64_t delivered_ = 0;
    std::atomic_uint64_t msiDelivered_ = 0;
    std::atomic_bool programmed_ = false;
};

} // namespace vmm
