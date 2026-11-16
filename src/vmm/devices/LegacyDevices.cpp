#include "LegacyDevices.h"

#include "../core/Log.h"

#include <algorithm>
#include <ctime>

namespace vmm {
namespace {

constexpr std::uint64_t kPitFrequency = 1193182;
constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ull;

std::uint64_t NanosecondsToTicks(std::uint64_t nanoseconds) noexcept
{
    return (nanoseconds / kNanosecondsPerSecond) * kPitFrequency +
           ((nanoseconds % kNanosecondsPerSecond) * kPitFrequency) / kNanosecondsPerSecond;
}

std::uint64_t TicksToNanoseconds(std::uint64_t ticks) noexcept
{
    return (ticks / kPitFrequency) * kNanosecondsPerSecond +
           ((ticks % kPitFrequency) * kNanosecondsPerSecond) / kPitFrequency;
}

std::uint8_t ToBcd(unsigned value) noexcept
{
    return static_cast<std::uint8_t>(((value / 10) << 4) | (value % 10));
}

} // namespace

// ---------------------------------------------------------------------------
// 8259A pair

std::uint32_t Pic8259Pair::IoRead(std::uint16_t port, std::uint8_t)
{
    switch (port) {
    case 0x20: case 0xA0: return 0; // IRR/ISR: nothing pending in I/O APIC mode.
    case 0x21: return chips_[0].mask;
    case 0xA1: return chips_[1].mask;
    case 0x4D0: return chips_[0].elcr;
    case 0x4D1: return chips_[1].elcr;
    default: return 0xFF;
    }
}

void Pic8259Pair::IoWrite(std::uint16_t port, std::uint8_t, std::uint32_t value)
{
    const auto byte = static_cast<std::uint8_t>(value);
    switch (port) {
    case 0x20: Command(chips_[0], byte); break;
    case 0xA0: Command(chips_[1], byte); break;
    case 0x21: Data(chips_[0], byte); break;
    case 0xA1: Data(chips_[1], byte); break;
    case 0x4D0: chips_[0].elcr = static_cast<std::uint8_t>(byte & 0xF8); break;
    case 0x4D1: chips_[1].elcr = static_cast<std::uint8_t>(byte & 0xDE); break;
    default: break;
    }
}

void Pic8259Pair::Command(Chip& chip, std::uint8_t value)
{
    if ((value & 0x10) != 0) { // ICW1
        chip.initStep = 1;
        chip.icw4Needed = (value & 0x01) != 0;
        chip.mask = 0;
    } else if ((value & 0x18) == 0x08) { // OCW3
        if ((value & 0x02) != 0) chip.readIsr = (value & 0x01) != 0;
    }
    // OCW2 (EOI and priority commands) has nothing to retire in this model.
}

void Pic8259Pair::Data(Chip& chip, std::uint8_t value)
{
    switch (chip.initStep) {
    case 1:
        chip.vectorBase = static_cast<std::uint8_t>(value & 0xF8);
        chip.initStep = 2;
        break;
    case 2:
        chip.initStep = chip.icw4Needed ? 3 : 0;
        break;
    case 3:
        chip.initStep = 0;
        break;
    default:
        chip.mask = value;
        break;
    }
}

// ---------------------------------------------------------------------------
// 8254 PIT

Pit8254::Pit8254(DeviceLock& lock, InterruptSink& sink) : lock_(lock), sink_(sink)
{
    channels_[2].gate = false;
    reprogramEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::jthread([this](std::stop_token stop) { TimerThread(stop); });
}

Pit8254::~Pit8254()
{
    thread_.request_stop();
    if (reprogramEvent_ != nullptr) SetEvent(reprogramEvent_);
    if (thread_.joinable()) thread_.join();
    if (reprogramEvent_ != nullptr) CloseHandle(reprogramEvent_);
}

std::uint64_t Pit8254::Ticks(const Channel& channel, Clock::time_point now) const
{
    if (!channel.gate) return channel.frozenTicks;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - channel.start).count();
    return channel.frozenTicks + NanosecondsToTicks(static_cast<std::uint64_t>((std::max<std::int64_t>)(0, elapsed)));
}

std::uint16_t Pit8254::Count(const Channel& channel, Clock::time_point now) const
{
    if (!channel.reloadSet) return channel.reload;
    const std::uint64_t period = channel.reload == 0 ? 65536 : channel.reload;
    const auto ticks = Ticks(channel, now);
    switch (channel.mode) {
    case 0: case 1: case 4: case 5:
        // Counts down once, then wraps around through 0xFFFF.
        return static_cast<std::uint16_t>((period - (ticks % 65536)) & 0xFFFF);
    case 3: {
        // Square wave: the counter decrements by two, twice per period.
        const auto phase = ticks % period;
        const auto half = (period + 1) / 2;
        const auto remaining = phase < half ? half - phase : period - phase;
        return static_cast<std::uint16_t>((remaining * 2) & 0xFFFF);
    }
    default:
        return static_cast<std::uint16_t>((period - (ticks % period)) & 0xFFFF);
    }
}

bool Pit8254::Output(const Channel& channel, Clock::time_point now) const
{
    if (!channel.reloadSet) return channel.mode != 0;
    const std::uint64_t period = channel.reload == 0 ? 65536 : channel.reload;
    const auto ticks = Ticks(channel, now);
    switch (channel.mode) {
    case 0: return ticks >= period;
    case 1: return ticks >= period;
    case 4: case 5: return ticks != period;
    case 3: return (ticks % period) < (period + 1) / 2;
    default: return (ticks % period) != period - 1;
    }
}

void Pit8254::LoadReload(std::uint32_t index)
{
    auto& channel = channels_[index];
    channel.reloadSet = true;
    channel.start = Clock::now();
    channel.frozenTicks = 0;
    channel.firedPeriods = 0;
    channel.oneShotFired = false;
    if (index == 0 && reprogramEvent_ != nullptr) SetEvent(reprogramEvent_);
}

std::uint32_t Pit8254::IoRead(std::uint16_t port, std::uint8_t)
{
    const auto now = Clock::now();
    if (port == 0x61) {
        port61_ ^= 0x10; // Refresh toggle, polled by some delay loops.
        const bool out2 = Output(channels_[2], now);
        return static_cast<std::uint8_t>((port61_ & 0x1F) | (out2 ? 0x20 : 0));
    }
    if (port < 0x40 || port > 0x42) return 0xFF;
    auto& channel = channels_[port - 0x40];
    if (channel.statusLatched) {
        channel.statusLatched = false;
        return channel.status;
    }
    std::uint16_t count = channel.latched ? channel.latch : Count(channel, now);
    std::uint8_t result = 0;
    switch (channel.access) {
    case 1:
        result = static_cast<std::uint8_t>(count);
        channel.latched = false;
        break;
    case 2:
        result = static_cast<std::uint8_t>(count >> 8);
        channel.latched = false;
        break;
    default:
        result = channel.readHigh ? static_cast<std::uint8_t>(count >> 8) : static_cast<std::uint8_t>(count);
        if (channel.readHigh) channel.latched = false;
        channel.readHigh = !channel.readHigh;
        break;
    }
    return result;
}

void Pit8254::IoWrite(std::uint16_t port, std::uint8_t, std::uint32_t value)
{
    const auto byte = static_cast<std::uint8_t>(value);
    const auto now = Clock::now();
    if (port == 0x61) {
        const bool oldGate = (port61_ & 0x01) != 0;
        port61_ = static_cast<std::uint8_t>((port61_ & 0x10) | (byte & 0x0F));
        const bool newGate = (byte & 0x01) != 0;
        auto& channel = channels_[2];
        if (newGate != oldGate) {
            if (newGate) {
                channel.gate = true;
                channel.start = now;
                channel.frozenTicks = 0;
            } else {
                channel.frozenTicks = Ticks(channel, now);
                channel.gate = false;
            }
        }
        return;
    }
    if (port == 0x43) {
        const auto select = static_cast<std::uint8_t>(byte >> 6);
        if (select == 3) { // Read-back command.
            for (std::uint32_t index = 0; index < 3; ++index) {
                if ((byte & (2u << index)) == 0) continue;
                auto& channel = channels_[index];
                if ((byte & 0x20) == 0 && !channel.latched) {
                    channel.latch = Count(channel, now);
                    channel.latched = true;
                    channel.readHigh = false;
                }
                if ((byte & 0x10) == 0 && !channel.statusLatched) {
                    channel.status = static_cast<std::uint8_t>(
                        (Output(channel, now) ? 0x80 : 0) | (channel.reloadSet ? 0 : 0x40) |
                        (channel.access << 4) | (channel.mode << 1));
                    channel.statusLatched = true;
                }
            }
            return;
        }
        auto& channel = channels_[select];
        const auto access = static_cast<std::uint8_t>((byte >> 4) & 0x03);
        if (access == 0) { // Counter latch.
            if (!channel.latched) {
                channel.latch = Count(channel, now);
                channel.latched = true;
                channel.readHigh = false;
            }
            return;
        }
        channel.access = access;
        channel.mode = static_cast<std::uint8_t>((byte >> 1) & 0x07);
        if (channel.mode > 5) channel.mode = static_cast<std::uint8_t>(channel.mode - 4);
        channel.writeHigh = false;
        channel.readHigh = false;
        channel.latched = false;
        channel.reloadSet = false;
        if (select == 0 && reprogramEvent_ != nullptr) SetEvent(reprogramEvent_);
        return;
    }
    if (port < 0x40 || port > 0x42) return;
    const auto index = static_cast<std::uint32_t>(port - 0x40);
    auto& channel = channels_[index];
    switch (channel.access) {
    case 1:
        channel.reload = byte;
        LoadReload(index);
        break;
    case 2:
        channel.reload = static_cast<std::uint16_t>(byte << 8);
        LoadReload(index);
        break;
    default:
        if (!channel.writeHigh) {
            channel.reload = static_cast<std::uint16_t>((channel.reload & 0xFF00) | byte);
            channel.writeHigh = true;
        } else {
            channel.reload = static_cast<std::uint16_t>((channel.reload & 0x00FF) | (byte << 8));
            channel.writeHigh = false;
            LoadReload(index);
        }
        break;
    }
}

void Pit8254::TimerThread(std::stop_token stop)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == nullptr) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    while (!stop.stop_requested()) {
        bool armed = false;
        Clock::time_point deadline{};
        {
            const DeviceGuard guard(lock_);
            const auto& channel = channels_[0];
            if (channel.reloadSet) {
                const std::uint64_t period = channel.reload == 0 ? 65536 : channel.reload;
                const bool periodic = channel.mode == 2 || channel.mode == 3;
                if (periodic) {
                    deadline = channel.start + std::chrono::nanoseconds(
                        TicksToNanoseconds((channel.firedPeriods + 1) * period));
                    armed = true;
                } else if (!channel.oneShotFired) {
                    deadline = channel.start + std::chrono::nanoseconds(TicksToNanoseconds(period));
                    armed = true;
                }
            }
        }

        if (armed) {
            const auto remaining = deadline - Clock::now();
            if (remaining > std::chrono::nanoseconds(0)) {
                LARGE_INTEGER due{};
                due.QuadPart = -(std::max<std::int64_t>)(1,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
                HANDLE handles[2] = {reprogramEvent_, timer};
                if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                    const auto result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                    if (result == WAIT_OBJECT_0) continue; // Reprogrammed.
                } else {
                    std::this_thread::sleep_until(deadline);
                }
            }
        } else {
            WaitForSingleObject(reprogramEvent_, INFINITE);
            continue;
        }
        if (stop.stop_requested()) break;

        const DeviceGuard guard(lock_);
        auto& channel = channels_[0];
        if (!channel.reloadSet) continue;
        const auto now = Clock::now();
        const std::uint64_t period = channel.reload == 0 ? 65536 : channel.reload;
        const bool periodic = channel.mode == 2 || channel.mode == 3;
        const auto ticks = Ticks(channel, now);
        if (periodic) {
            const auto elapsedPeriods = ticks / period;
            if (elapsedPeriods <= channel.firedPeriods) continue; // Reprogrammed meanwhile.
            // Deliver one tick; if the host fell far behind, drop the backlog
            // instead of flooding the guest with interrupts.
            channel.firedPeriods = elapsedPeriods - channel.firedPeriods > 8
                ? elapsedPeriods : channel.firedPeriods + 1;
        } else {
            if (channel.oneShotFired || ticks < period) continue;
            channel.oneShotFired = true;
        }
        sink_.SetGsi(IsaIrqToGsi(0), true);
        sink_.SetGsi(IsaIrqToGsi(0), false);
        ++irqCount_;
    }
    if (timer != nullptr) CloseHandle(timer);
}

// ---------------------------------------------------------------------------
// CMOS RTC

CmosRtc::CmosRtc(std::uint64_t ramBytes, bool localTime) : localTime_(localTime)
{
    registers_[0x0A] = 0x26;
    registers_[0x0B] = 0x02; // 24-hour mode, BCD.
    registers_[0x0D] = 0x80; // Valid RAM and time.
    registers_[0x10] = 0x00; // No floppy drives.
    registers_[0x14] = 0x06; // Display: VGA/EGA, keyboard installed, no FPU bit needed.
    constexpr std::uint64_t mib = 1024ull * 1024;
    const unsigned base = 640;
    registers_[0x15] = static_cast<std::uint8_t>(base & 0xFF);
    registers_[0x16] = static_cast<std::uint8_t>(base >> 8);
    const auto extended = ramBytes > mib ? (std::min<std::uint64_t>)(0xFC00, (ramBytes - mib) / 1024) : 0;
    registers_[0x17] = registers_[0x30] = static_cast<std::uint8_t>(extended & 0xFF);
    registers_[0x18] = registers_[0x31] = static_cast<std::uint8_t>(extended >> 8);
    const auto above16 = ramBytes > 16 * mib
        ? (std::min<std::uint64_t>)(0xFFFF, ((std::min)(ramBytes, 0xC0000000ull) - 16 * mib) / 65536) : 0;
    registers_[0x34] = static_cast<std::uint8_t>(above16 & 0xFF);
    registers_[0x35] = static_cast<std::uint8_t>(above16 >> 8);
}

std::uint8_t CmosRtc::Encode(unsigned value) const noexcept
{
    return (registers_[0x0B] & 0x04) != 0 ? static_cast<std::uint8_t>(value) : ToBcd(value);
}

std::uint8_t CmosRtc::ReadData()
{
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    if (localTime_) localtime_s(&local, &now);
    else gmtime_s(&local, &now);
    switch (index_) {
    case 0x00: return Encode(static_cast<unsigned>(local.tm_sec));
    case 0x02: return Encode(static_cast<unsigned>(local.tm_min));
    case 0x04: {
        unsigned hour = static_cast<unsigned>(local.tm_hour);
        if ((registers_[0x0B] & 0x02) != 0) return Encode(hour);
        const bool pm = hour >= 12;
        hour %= 12;
        if (hour == 0) hour = 12;
        return static_cast<std::uint8_t>(Encode(hour) | (pm ? 0x80 : 0));
    }
    case 0x06: return Encode(static_cast<unsigned>(local.tm_wday + 1));
    case 0x07: return Encode(static_cast<unsigned>(local.tm_mday));
    case 0x08: return Encode(static_cast<unsigned>(local.tm_mon + 1));
    case 0x09: return Encode(static_cast<unsigned>(local.tm_year % 100));
    case 0x32: return Encode(static_cast<unsigned>((local.tm_year + 1900) / 100));
    case 0x0A: return static_cast<std::uint8_t>(registers_[0x0A] & 0x7F); // UIP never set.
    case 0x0C: {
        const auto value = registers_[0x0C];
        registers_[0x0C] = 0;
        return value;
    }
    default: return registers_[index_];
    }
}

std::uint32_t CmosRtc::IoRead(std::uint16_t port, std::uint8_t)
{
    return port == 0x70 ? index_ : ReadData();
}

void CmosRtc::IoWrite(std::uint16_t port, std::uint8_t, std::uint32_t value)
{
    const auto byte = static_cast<std::uint8_t>(value);
    if (port == 0x70) {
        index_ = static_cast<std::uint8_t>(byte & 0x7F);
        return;
    }
    if (index_ == 0x0C || index_ == 0x0D) return;
    if (index_ <= 0x09 || index_ == 0x32) return; // Guest time writes are ignored: host clock wins.
    registers_[index_] = byte;
}

// ---------------------------------------------------------------------------
// 16550A UART

Uart16550::Uart16550(InterruptSink& sink, std::uint32_t irq, Output output)
    : sink_(sink), irq_(irq), output_(std::move(output))
{
}

std::uint8_t Uart16550::InterruptIdentification() const noexcept
{
    const std::uint8_t fifo = (fcr_ & 0x01) != 0 ? 0xC0 : 0x00;
    if ((ier_ & 0x01) != 0 && !receive_.empty()) return static_cast<std::uint8_t>(fifo | 0x04);
    if ((ier_ & 0x02) != 0 && thrInterrupt_) return static_cast<std::uint8_t>(fifo | 0x02);
    return static_cast<std::uint8_t>(fifo | 0x01);
}

void Uart16550::UpdateInterrupt()
{
    const bool pending = (InterruptIdentification() & 0x01) == 0;
    const bool asserted = pending && (mcr_ & 0x08) != 0 && (mcr_ & 0x10) == 0;
    if (asserted != lineAsserted_) {
        lineAsserted_ = asserted;
        sink_.SetGsi(IsaIrqToGsi(irq_), asserted);
    }
}

std::uint32_t Uart16550::IoRead(std::uint16_t port, std::uint8_t)
{
    std::uint8_t value = 0xFF;
    switch (port & 7) {
    case 0:
        if ((lcr_ & 0x80) != 0) {
            value = static_cast<std::uint8_t>(divisor_);
        } else if (!receive_.empty()) {
            value = receive_.front();
            receive_.pop_front();
        } else {
            value = 0;
        }
        break;
    case 1:
        value = (lcr_ & 0x80) != 0 ? static_cast<std::uint8_t>(divisor_ >> 8) : ier_;
        break;
    case 2:
        value = InterruptIdentification();
        if ((value & 0x0F) == 0x02) thrInterrupt_ = false;
        break;
    case 3: value = lcr_; break;
    case 4: value = mcr_; break;
    case 5: value = static_cast<std::uint8_t>(0x60 | (receive_.empty() ? 0 : 0x01)); break;
    case 6:
        if ((mcr_ & 0x10) != 0) {
            value = static_cast<std::uint8_t>(((mcr_ & 0x02) ? 0x10 : 0) | ((mcr_ & 0x01) ? 0x20 : 0) |
                                              ((mcr_ & 0x04) ? 0x40 : 0) | ((mcr_ & 0x08) ? 0x80 : 0));
        } else {
            value = 0xB0; // CTS, DSR and DCD asserted.
        }
        break;
    case 7: value = scratch_; break;
    }
    UpdateInterrupt();
    return value;
}

void Uart16550::IoWrite(std::uint16_t port, std::uint8_t, std::uint32_t value)
{
    const auto byte = static_cast<std::uint8_t>(value);
    switch (port & 7) {
    case 0:
        if ((lcr_ & 0x80) != 0) {
            divisor_ = static_cast<std::uint16_t>((divisor_ & 0xFF00) | byte);
        } else {
            if ((mcr_ & 0x10) != 0) {
                if (receive_.size() < 16) receive_.push_back(byte);
            } else if (output_) {
                output_(byte);
            }
            thrInterrupt_ = true; // Transmission is instantaneous.
        }
        break;
    case 1:
        if ((lcr_ & 0x80) != 0) {
            divisor_ = static_cast<std::uint16_t>((divisor_ & 0x00FF) | (byte << 8));
        } else {
            const bool enablingThre = (byte & 0x02) != 0 && (ier_ & 0x02) == 0;
            ier_ = static_cast<std::uint8_t>(byte & 0x0F);
            if (enablingThre) thrInterrupt_ = true;
        }
        break;
    case 2:
        fcr_ = byte;
        if ((byte & 0x02) != 0) receive_.clear();
        break;
    case 3: lcr_ = byte; break;
    case 4: mcr_ = static_cast<std::uint8_t>(byte & 0x1F); break;
    case 7: scratch_ = byte; break;
    default: break;
    }
    UpdateInterrupt();
}

// ---------------------------------------------------------------------------
// VGA registers

std::uint32_t VgaRegisters::IoRead(std::uint16_t port, std::uint8_t size)
{
    std::uint32_t value = 0;
    for (std::uint8_t index = 0; index < size; ++index) {
        value |= static_cast<std::uint32_t>(ReadByte(static_cast<std::uint16_t>(port + index))) << (index * 8);
    }
    return value;
}

void VgaRegisters::IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value)
{
    for (std::uint8_t index = 0; index < size; ++index) {
        WriteByte(static_cast<std::uint16_t>(port + index), static_cast<std::uint8_t>(value >> (index * 8)));
    }
}

std::uint8_t VgaRegisters::ReadByte(std::uint16_t port)
{
    switch (port) {
    case 0x3C0: return attributeIndex_;
    case 0x3C1: return attribute_[attributeIndex_ & 0x1F];
    case 0x3C4: return sequencerIndex_;
    case 0x3C5: return sequencer_[sequencerIndex_];
    case 0x3C7: return 0;
    case 0x3C8: return static_cast<std::uint8_t>(dacWriteIndex_ / 3);
    case 0x3C9: {
        const auto value = palette_[dacReadIndex_ % palette_.size()];
        dacReadIndex_ = static_cast<std::uint16_t>((dacReadIndex_ + 1) % palette_.size());
        return value;
    }
    case 0x3CC: return misc_;
    case 0x3CE: return graphicsIndex_;
    case 0x3CF: return graphics_[graphicsIndex_];
    case 0x3B4: case 0x3D4: return crtcIndex_;
    case 0x3B5: case 0x3D5: return crtc_[crtcIndex_];
    case 0x3BA: case 0x3DA:
        attributeFlipFlop_ = false;
        retrace_ ^= 0x09; // Toggle display-enable and vertical retrace.
        return retrace_;
    default: return 0xFF;
    }
}

void VgaRegisters::WriteByte(std::uint16_t port, std::uint8_t value)
{
    switch (port) {
    case 0x3C0:
        if (!attributeFlipFlop_) attributeIndex_ = value;
        else attribute_[attributeIndex_ & 0x1F] = value;
        attributeFlipFlop_ = !attributeFlipFlop_;
        break;
    case 0x3C2: misc_ = value; break;
    case 0x3C4: sequencerIndex_ = value; break;
    case 0x3C5: sequencer_[sequencerIndex_] = value; break;
    case 0x3C7: dacReadIndex_ = static_cast<std::uint16_t>(value * 3); break;
    case 0x3C8: dacWriteIndex_ = static_cast<std::uint16_t>(value * 3); break;
    case 0x3C9:
        palette_[dacWriteIndex_ % palette_.size()] = value;
        dacWriteIndex_ = static_cast<std::uint16_t>((dacWriteIndex_ + 1) % palette_.size());
        break;
    case 0x3CE: graphicsIndex_ = value; break;
    case 0x3CF: graphics_[graphicsIndex_] = value; break;
    case 0x3B4: case 0x3D4: crtcIndex_ = value; break;
    case 0x3B5: case 0x3D5: crtc_[crtcIndex_] = value; break;
    default: break;
    }
}

void Uart16550::ReceiveByte(std::uint8_t value)
{
    if (receive_.size() < 64) receive_.push_back(value);
    UpdateInterrupt();
}

} // namespace vmm
