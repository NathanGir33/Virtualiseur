#pragma once

#include "../core/Bus.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <thread>

namespace vmm {

// Dual 8259A register model. Interrupts are routed through the I/O APIC
// (declared in the MADT); the PICs only need to accept Linux's initialization
// and masking sequence. Linux masks them once the I/O APIC is configured.
class Pic8259Pair final : public IoDevice {
public:
    std::uint32_t IoRead(std::uint16_t port, std::uint8_t size) override;
    void IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value) override;

private:
    struct Chip {
        std::uint8_t mask = 0xFF;
        std::uint8_t vectorBase = 0;
        std::uint8_t initStep = 0;
        bool icw4Needed = false;
        bool readIsr = false;
        std::uint8_t elcr = 0;
    };
    void Command(Chip& chip, std::uint8_t value);
    void Data(Chip& chip, std::uint8_t value);

    std::array<Chip, 2> chips_{};
};

// 8254 PIT. Channel 0 drives ISA IRQ 0 from a high-resolution host timer
// thread; channel 2 is gated by port 0x61 and is used by Linux to calibrate
// the TSC. Timing follows host QueryPerformanceCounter.
class Pit8254 final : public IoDevice {
public:
    Pit8254(DeviceLock& lock, InterruptSink& sink);
    ~Pit8254() override;

    std::uint32_t IoRead(std::uint16_t port, std::uint8_t size) override;
    void IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value) override;

    std::uint64_t TimerInterrupts() const noexcept { return irqCount_; }

private:
    using Clock = std::chrono::steady_clock;
    struct Channel {
        std::uint8_t mode = 0;
        std::uint8_t access = 3;
        std::uint16_t reload = 0;
        bool reloadSet = false;
        bool gate = true;
        bool writeHigh = false;
        bool readHigh = false;
        bool latched = false;
        std::uint16_t latch = 0;
        bool statusLatched = false;
        std::uint8_t status = 0;
        Clock::time_point start = Clock::now();
        std::uint64_t frozenTicks = 0;
        std::uint64_t firedPeriods = 0;
        bool oneShotFired = false;
    };

    std::uint64_t Ticks(const Channel& channel, Clock::time_point now) const;
    std::uint16_t Count(const Channel& channel, Clock::time_point now) const;
    bool Output(const Channel& channel, Clock::time_point now) const;
    void LoadReload(std::uint32_t index);
    void TimerThread(std::stop_token stop);

    DeviceLock& lock_;
    InterruptSink& sink_;
    std::array<Channel, 3> channels_{};
    std::uint8_t port61_ = 0;
    std::atomic_uint64_t irqCount_ = 0;
    HANDLE reprogramEvent_ = nullptr;
    std::jthread thread_;
};

// MC146818 real-time clock and CMOS RAM. Time comes from the host local clock.
class CmosRtc final : public IoDevice {
public:
    // Linux guests expect the hardware clock in UTC; Windows guests in local time.
    CmosRtc(std::uint64_t ramBytes, bool localTime);
    std::uint32_t IoRead(std::uint16_t port, std::uint8_t size) override;
    void IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value) override;

private:
    std::uint8_t ReadData();
    std::uint8_t Encode(unsigned value) const noexcept;

    std::array<std::uint8_t, 128> registers_{};
    std::uint8_t index_ = 0;
    bool localTime_ = false;
};

// 16550A UART with FIFO, loopback and proper THRE/RDA interrupt semantics.
class Uart16550 final : public IoDevice {
public:
    using Output = std::function<void(std::uint8_t)>;
    Uart16550(InterruptSink& sink, std::uint32_t irq, Output output);

    std::uint32_t IoRead(std::uint16_t port, std::uint8_t size) override;
    void IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value) override;

    // Called with the device lock held.
    void ReceiveByte(std::uint8_t value);
    bool ReceiveFifoFull() const noexcept { return receive_.size() >= 16; }

private:
    void UpdateInterrupt();
    std::uint8_t InterruptIdentification() const noexcept;

    InterruptSink& sink_;
    std::uint32_t irq_;
    Output output_;
    std::deque<std::uint8_t> receive_;
    std::uint8_t ier_ = 0;
    std::uint8_t lcr_ = 0;
    std::uint8_t mcr_ = 0;
    std::uint8_t scratch_ = 0;
    std::uint8_t fcr_ = 0;
    std::uint16_t divisor_ = 12;
    bool thrInterrupt_ = false;
    bool lineAsserted_ = false;
};

// VGA register file (0x3B0-0x3DF). Only what text mode needs is modelled:
// the CRTC cursor registers drive the host window's cursor.
class VgaRegisters final : public IoDevice {
public:
    std::uint32_t IoRead(std::uint16_t port, std::uint8_t size) override;
    void IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value) override;

    std::uint16_t CursorOffset() const noexcept
    {
        return static_cast<std::uint16_t>((crtc_[0x0E] << 8) | crtc_[0x0F]);
    }
    bool CursorVisible() const noexcept { return (crtc_[0x0A] & 0x20) == 0; }
    std::uint16_t StartOffset() const noexcept
    {
        return static_cast<std::uint16_t>((crtc_[0x0C] << 8) | crtc_[0x0D]);
    }

private:
    void WriteByte(std::uint16_t port, std::uint8_t value);
    std::uint8_t ReadByte(std::uint16_t port);

    std::array<std::uint8_t, 256> crtc_{};
    std::array<std::uint8_t, 256> sequencer_{};
    std::array<std::uint8_t, 256> graphics_{};
    std::array<std::uint8_t, 32> attribute_{};
    std::array<std::uint8_t, 768> palette_{};
    std::uint8_t crtcIndex_ = 0;
    std::uint8_t sequencerIndex_ = 0;
    std::uint8_t graphicsIndex_ = 0;
    std::uint8_t attributeIndex_ = 0;
    bool attributeFlipFlop_ = false;
    std::uint8_t misc_ = 0x67;
    std::uint16_t dacReadIndex_ = 0;
    std::uint16_t dacWriteIndex_ = 0;
    std::uint8_t retrace_ = 0;
};

// Port 0x80 POST diagnostics and port 0xE9 debug console sinks.
class PostPort final : public IoDevice {
public:
    std::uint32_t IoRead(std::uint16_t, std::uint8_t) override { return 0xFFFFFFFF; }
    void IoWrite(std::uint16_t, std::uint8_t, std::uint32_t) override {}
};

} // namespace vmm
