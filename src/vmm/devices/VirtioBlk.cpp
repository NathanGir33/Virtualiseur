#include "VirtioBlk.h"

#include "../core/Log.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace vmm {
namespace {
constexpr std::uint32_t kFeatureSegMax = 1u << 2;
constexpr std::uint32_t kFeatureBlkSize = 1u << 6;
constexpr std::uint32_t kFeatureFlush = 1u << 9;
constexpr std::uint32_t kSegMax = 254; // Queue size minus header and status descriptors.

constexpr std::uint32_t kRequestIn = 0;
constexpr std::uint32_t kRequestOut = 1;
constexpr std::uint32_t kRequestFlush = 4;
constexpr std::uint32_t kRequestGetId = 8;

constexpr std::uint8_t kStatusOk = 0;
constexpr std::uint8_t kStatusIoError = 1;
constexpr std::uint8_t kStatusUnsupported = 2;
} // namespace

VirtioBlk::VirtioBlk(const GuestMemory& memory, InterruptSink& sink, DeviceLock& lock, IoBus& ioBus,
                     std::uint16_t ioBase, std::uint32_t gsi, virtualiseur::whp::VirtualDisk& disk)
    : VirtioPciDevice(0x1001, 2, 0x01, 0x00, memory, sink, gsi, ioBus, ioBase, 1),
      lock_(lock), disk_(disk)
{
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    worker_ = std::jthread([this](std::stop_token stop) { Worker(stop); });
}

VirtioBlk::~VirtioBlk()
{
    worker_.request_stop();
    if (wake_ != nullptr) SetEvent(wake_);
    if (worker_.joinable()) worker_.join();
    if (wake_ != nullptr) CloseHandle(wake_);
}

std::uint32_t VirtioBlk::DeviceFeatures() const
{
    return kFeatureSegMax | kFeatureBlkSize | kFeatureFlush | VirtQueue::kFeatureIndirectDescriptors;
}

std::uint8_t VirtioBlk::ReadDeviceConfig(std::uint32_t offset)
{
    std::uint8_t config[24] = {};
    const auto capacity = disk_.SectorCount();
    std::memcpy(config + 0, &capacity, 8);
    const std::uint32_t sizeMax = 0;
    std::memcpy(config + 8, &sizeMax, 4);
    std::memcpy(config + 12, &kSegMax, 4);
    const std::uint32_t blockSize = 512;
    std::memcpy(config + 20, &blockSize, 4);
    return offset < sizeof(config) ? config[offset] : 0;
}

void VirtioBlk::QueueNotify(std::uint16_t)
{
    SetEvent(wake_);
}

void VirtioBlk::DeviceReset()
{
    // Wait for an in-flight batch before the queue memory is released.
    const std::lock_guard guard(queueMutex_);
}

std::uint8_t VirtioBlk::Execute(const VirtQueue::Element& element, std::uint32_t& written)
{
    written = 0;
    if (element.readable.empty() || element.writable.empty() || element.readable[0].length < 16) {
        return kStatusIoError;
    }
    std::uint32_t type = 0;
    std::uint64_t sector = 0;
    if (!memory_.ReadValue(element.readable[0].address, type) ||
        !memory_.ReadValue(element.readable[0].address + 8, sector)) {
        return kStatusIoError;
    }

    // The last writable byte is the status; everything before it is data.
    std::vector<VirtQueue::Buffer> dataIn(element.writable.begin(), element.writable.end());
    auto& statusBuffer = dataIn.back();
    if (statusBuffer.length == 0) return kStatusIoError;
    statusBuffer.length -= 1;
    std::vector<VirtQueue::Buffer> dataOut(element.readable.begin() + 1, element.readable.end());

    // One host I/O per request: segments (often 4 KiB pages) are gathered in
    // a bounce buffer instead of issuing one ReadFile/WriteFile per segment.
    const auto totalLength = [](const std::vector<VirtQueue::Buffer>& buffers) {
        std::uint64_t total = 0;
        for (const auto& buffer : buffers) total += buffer.length;
        return total;
    };
    try {
        switch (type) {
        case kRequestIn: {
            const auto total = totalLength(dataIn);
            if ((total % 512) != 0 || total > 64ull * 1024 * 1024) return kStatusIoError;
            if (total == 0) return kStatusOk;
            bounce_.resize(static_cast<std::size_t>(total));
            disk_.ReadSectors(sector, std::as_writable_bytes(std::span(bounce_)));
            std::size_t offset = 0;
            for (const auto& buffer : dataIn) {
                if (buffer.length == 0) continue;
                if (!memory_.Write(buffer.address, bounce_.data() + offset, buffer.length)) return kStatusIoError;
                offset += buffer.length;
            }
            written = static_cast<std::uint32_t>(total);
            return kStatusOk;
        }
        case kRequestOut: {
            const auto total = totalLength(dataOut);
            if ((total % 512) != 0 || total > 64ull * 1024 * 1024) return kStatusIoError;
            if (total == 0) return kStatusOk;
            bounce_.resize(static_cast<std::size_t>(total));
            std::size_t offset = 0;
            for (const auto& buffer : dataOut) {
                if (buffer.length == 0) continue;
                if (!memory_.Read(buffer.address, bounce_.data() + offset, buffer.length)) return kStatusIoError;
                offset += buffer.length;
            }
            disk_.WriteSectors(sector, std::as_bytes(std::span(bounce_)));
            return kStatusOk;
        }
        case kRequestFlush:
            disk_.Flush();
            return kStatusOk;
        case kRequestGetId: {
            char id[20] = "VWHP-VIRTIO-DISK";
            if (!dataIn.empty() && dataIn[0].length > 0) {
                const auto length = (std::min<std::uint32_t>)(dataIn[0].length, sizeof(id));
                if (!memory_.Write(dataIn[0].address, id, length)) return kStatusIoError;
                written += length;
            }
            return kStatusOk;
        }
        default:
            return kStatusUnsupported;
        }
    } catch (const std::exception& error) {
        VMM_WARN("virtio-blk: requête %u au secteur %llu en échec: %s", type,
                 static_cast<unsigned long long>(sector), error.what());
        return kStatusIoError;
    }
}

void VirtioBlk::Worker(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        WaitForSingleObject(wake_, INFINITE);
        if (stop.stop_requested()) break;
        bool completedAny = false;
        {
            const std::lock_guard guard(queueMutex_);
            auto& queue = queues_[0];
            VirtQueue::Element element;
            while (queue.Pop(memory_, element)) {
                std::uint32_t written = 0;
                const auto status = Execute(element, written);
                const auto& last = element.writable.empty() ? VirtQueue::Buffer{0, 0} : element.writable.back();
                if (last.length != 0) (void)memory_.WriteValue(last.address + last.length - 1, status);
                queue.Push(memory_, element.head, written + 1);
                ++completed_;
                completedAny = true;
            }
        }
        if (completedAny) {
            const DeviceGuard guard(lock_);
            SignalUsedBuffers(0);
        }
    }
}

} // namespace vmm
