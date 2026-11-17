#include "VirtioSound.h"

#include "../core/Log.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace vmm {
namespace {

constexpr std::uint16_t kControlQueue = 0;
constexpr std::uint16_t kEventQueue = 1;
constexpr std::uint16_t kTransmitQueue = 2;
constexpr std::uint16_t kReceiveQueue = 3;

// Request codes (struct virtio_snd_hdr).
constexpr std::uint32_t kJackInfo = 0x0001;
constexpr std::uint32_t kPcmInfo = 0x0100;
constexpr std::uint32_t kPcmSetParams = 0x0101;
constexpr std::uint32_t kPcmPrepare = 0x0102;
constexpr std::uint32_t kPcmRelease = 0x0103;
constexpr std::uint32_t kPcmStart = 0x0104;
constexpr std::uint32_t kPcmStop = 0x0105;
constexpr std::uint32_t kChmapInfo = 0x0200;

// Status codes.
constexpr std::uint32_t kStatusOk = 0x8000;
constexpr std::uint32_t kStatusBadMessage = 0x8001;
constexpr std::uint32_t kStatusNotSupported = 0x8002;

// Stream 0 plays, stream 1 records. Both share function node 0, so the
// guest driver groups them in one ALSA PCM device (playback + capture).
constexpr std::uint32_t kStreamCount = 2;
constexpr std::uint32_t kPlaybackStream = 0;
constexpr std::uint32_t kCaptureStream = 1;
constexpr std::uint8_t kDirectionOutput = 0;
constexpr std::uint8_t kDirectionInput = 1;
constexpr std::uint8_t kFormatS16 = 5; // VIRTIO_SND_PCM_FMT_S16 (little endian).
constexpr std::size_t kPcmInfoSize = 32;
constexpr std::size_t kStatusSize = 8;  // virtio_snd_pcm_status.
constexpr std::size_t kMaxRequest = 4096;
constexpr std::size_t kMaxTransfer = 4u * 1024 * 1024;
constexpr std::size_t kMaxPendingBytes = 16u * 1024 * 1024;

// VIRTIO_SND_PCM_RATE_* index -> Hz.
constexpr std::array<std::uint32_t, 14> kRates{5512, 8000, 11025, 16000, 22050, 32000, 44100,
                                               48000, 64000, 88200, 96000, 176400, 192000, 384000};
// 8 kHz to 96 kHz: the Windows audio engine converts any of them.
constexpr std::uint64_t kSupportedRates = (1ull << 1) | (1ull << 2) | (1ull << 3) | (1ull << 4) | (1ull << 5) |
                                          (1ull << 6) | (1ull << 7) | (1ull << 8) | (1ull << 9) | (1ull << 10);

std::uint32_t Load32(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    std::uint32_t value = 0;
    if (offset + 4 <= data.size()) std::memcpy(&value, data.data() + offset, 4);
    return value;
}

void Store(std::vector<std::uint8_t>& data, std::size_t offset, const void* value, std::size_t size)
{
    if (data.size() < offset + size) data.resize(offset + size, 0);
    std::memcpy(data.data() + offset, value, size);
}

} // namespace

VirtioSound::VirtioSound(const GuestMemory& memory, const VirtioPlacement& placement, std::wstring sessionName,
                         bool microphone)
    : VirtioModernDevice(25, 0x04, 0x01, memory, placement, 4)
{
    output_ = std::make_unique<WasapiOutput>(std::move(sessionName), [this](std::uint8_t* destination, std::size_t bytes) {
        return Pull(destination, bytes);
    });
    input_ = std::make_unique<WasapiInput>([this](const std::uint8_t* data, std::size_t bytes) {
        PushCapture(data, bytes);
    });
    input_->SetEnabled(microphone);
}

VirtioSound::~VirtioSound()
{
    // Stop the audio threads (which call Pull and PushCapture) while the device is still whole.
    output_.reset();
    input_.reset();
}

std::uint8_t VirtioSound::ReadDeviceConfig(std::uint32_t offset)
{
    // struct virtio_snd_config: jacks, streams, chmaps.
    const std::uint32_t value = offset / 4 == 1 ? kStreamCount : 0;
    return offset < 12 ? static_cast<std::uint8_t>(value >> ((offset % 4) * 8)) : 0;
}

void VirtioSound::DriverOk()
{
    VMM_INFO("virtio-snd : carte son prise en charge par le pilote invité.");
}

void VirtioSound::DeviceReset()
{
    // The rings are being reset: pending buffers are simply forgotten.
    pending_.clear();
    pendingBytes_ = 0;
    capture_.clear();
    params_ = StreamParams{};
    captureParams_ = StreamParams{};
    StopPlayback();
    StopCapture();
}

void VirtioSound::StopPlayback()
{
    running_ = false;
    if (output_) output_->Stop();
}

void VirtioSound::StopCapture()
{
    capturing_ = false;
    if (input_) input_->Stop();
}

void VirtioSound::QueueNotify(std::uint16_t queue)
{
    switch (queue) {
    case kControlQueue: ProcessControl(); break;
    case kTransmitQueue: ProcessTransmit(); break;
    case kReceiveQueue: ProcessReceive(); break;
    case kEventQueue: break; // Buffers stay available for events we never send.
    default: break;
    }
}

void VirtioSound::ProcessControl()
{
    auto& ring = queues_[kControlQueue];
    VirtQueue::Element element;
    std::vector<std::uint8_t> request;
    std::vector<std::uint8_t> response;
    bool any = false;
    while (ring.Pop(memory_, element)) {
        request.clear();
        bool valid = true;
        for (const auto& buffer : element.readable) {
            if (request.size() + buffer.length > kMaxRequest) {
                valid = false;
                break;
            }
            const auto offset = request.size();
            request.resize(offset + buffer.length);
            if (!memory_.Read(buffer.address, request.data() + offset, buffer.length)) {
                valid = false;
                break;
            }
        }
        // Response: virtio_snd_hdr status, then the payload (info arrays),
        // laid out over the device-writable buffers in order.
        std::vector<std::uint8_t> payload;
        const std::uint32_t status = valid && request.size() >= 4 ? HandleControl(request, payload) : kStatusBadMessage;
        response.assign(4, 0);
        std::memcpy(response.data(), &status, 4);
        response.insert(response.end(), payload.begin(), payload.end());
        std::size_t written = 0;
        for (const auto& buffer : element.writable) {
            if (written == response.size()) break;
            const auto count = (std::min<std::size_t>)(buffer.length, response.size() - written);
            if (!memory_.Write(buffer.address, response.data() + written, count)) break;
            written += count;
        }
        ring.Push(memory_, element.head, static_cast<std::uint32_t>(written));
        any = true;
    }
    if (any) SignalUsedBuffers(kControlQueue);
}

std::uint32_t VirtioSound::HandleControl(const std::vector<std::uint8_t>& request, std::vector<std::uint8_t>& payload)
{
    const auto code = Load32(request, 0);
    VMM_DEBUG("virtio-snd : requête 0x%x (%zu octets en attente).", code, pendingBytes_);
    if (code == kPcmInfo) {
        // virtio_snd_query_info: hdr, start_id, count, size.
        if (request.size() < 16) return kStatusBadMessage;
        const auto start = Load32(request, 4);
        const auto count = Load32(request, 8);
        const auto size = Load32(request, 12);
        if (start > kStreamCount || count > kStreamCount - start || size == 0 || size > 1024) return kStatusBadMessage;
        if (!announced_) {
            VMM_INFO("virtio-snd : le pilote invité a trouvé la sortie et l'entrée audio (16 bits, 1-2 canaux, 8-96 kHz).");
            announced_ = true;
        }
        payload.assign(static_cast<std::size_t>(count) * size, 0);
        for (std::uint32_t index = 0; index < count; ++index) {
            // virtio_snd_pcm_info: hda_fn_nid, features, formats, rates,
            // direction, channels_min, channels_max, padding[5].
            std::vector<std::uint8_t> info(kPcmInfoSize, 0);
            const std::uint64_t formats = 1ull << kFormatS16;
            Store(info, 8, &formats, 8);
            Store(info, 16, &kSupportedRates, 8);
            info[24] = start + index == kCaptureStream ? kDirectionInput : kDirectionOutput;
            info[25] = 1;
            info[26] = 2;
            std::memcpy(payload.data() + static_cast<std::size_t>(index) * size, info.data(),
                        (std::min<std::size_t>)(size, info.size()));
        }
        return kStatusOk;
    }
    if (code == kJackInfo || code == kChmapInfo) return kStatusBadMessage; // No jacks, no channel maps advertised.
    if (code < kPcmSetParams || code > kPcmStop) {
        VMM_DEBUG("virtio-snd : requête 0x%x non prise en charge.", code);
        return kStatusNotSupported;
    }

    // PCM stream requests: hdr, stream_id, ...
    if (request.size() < 8) return kStatusBadMessage;
    const auto stream = Load32(request, 4);
    if (stream >= kStreamCount) return kStatusBadMessage;
    const bool capture = stream == kCaptureStream;
    auto& params = capture ? captureParams_ : params_;
    switch (code) {
    case kPcmSetParams: {
        // virtio_snd_pcm_set_params: hdr, stream_id, buffer_bytes,
        // period_bytes, features, channels, format, rate, padding.
        if (request.size() < 24) return kStatusBadMessage;
        const auto channels = request[20];
        const auto format = request[21];
        const auto rate = request[22];
        if (channels < 1 || channels > 2 || format != kFormatS16 || rate >= kRates.size() ||
            (kSupportedRates & (1ull << rate)) == 0) {
            VMM_WARN("virtio-snd : paramètres refusés (flux %u, format %u, fréquence %u, %u canaux).", stream, format,
                     rate, channels);
            return kStatusNotSupported;
        }
        params.set = true;
        params.bufferBytes = Load32(request, 8);
        params.periodBytes = Load32(request, 12);
        params.channels = channels;
        params.format = format;
        params.rate = rate;
        return kStatusOk;
    }
    case kPcmPrepare:
        if (!params.set) return kStatusBadMessage;
        // Open the host device now: START must play at once (see WasapiOutput::Prepare).
        if (!capture && output_) output_->Prepare(kRates[params.rate], params.channels);
        return kStatusOk;
    case kPcmStart: {
        if (!params.set) return kStatusBadMessage;
        const auto hertz = kRates[params.rate];
        if (capture) {
            capturing_ = true;
            VMM_INFO("virtio-snd : enregistrement %u Hz, %u %s, 16 bits (micro de l'hôte %s).", hertz, params.channels,
                     params.channels > 1 ? "canaux" : "canal", MicrophoneEnabled() ? "autorisé" : "coupé : silence");
            if (input_) input_->Start(hertz, params.channels);
            return kStatusOk;
        }
        running_ = true;
        if (hertz != lastRate_ || params.channels != lastChannels_) {
            VMM_INFO("virtio-snd : lecture %u Hz, %u %s, 16 bits.", hertz, params.channels,
                     params.channels > 1 ? "canaux" : "canal");
            lastRate_ = hertz;
            lastChannels_ = params.channels;
        }
        if (output_) output_->Play(hertz, params.channels);
        return kStatusOk;
    }
    case kPcmStop:
        // Queued buffers wait for START (pause) or RELEASE.
        if (capture) StopCapture();
        else StopPlayback();
        return kStatusOk;
    case kPcmRelease:
        // The driver waits for every pending transfer.
        if (capture) {
            StopCapture();
            ReleaseCapture();
        } else {
            StopPlayback();
            ReleasePending();
        }
        return kStatusOk;
    default:
        return kStatusNotSupported;
    }
}

void VirtioSound::ProcessTransmit()
{
    auto& ring = queues_[kTransmitQueue];
    VirtQueue::Element element;
    bool completed = false;
    while (ring.Pop(memory_, element)) {
        // virtio_snd_pcm_xfer (stream_id), samples, then a device-writable
        // virtio_snd_pcm_status.
        Pending transfer;
        transfer.head = element.head;
        transfer.writable = element.writable;
        std::uint64_t skip = 4;
        bool valid = true;
        for (const auto& buffer : element.readable) {
            std::uint64_t address = buffer.address;
            std::uint32_t length = buffer.length;
            const auto header = (std::min<std::uint64_t>)(skip, length);
            skip -= header;
            address += header;
            length -= static_cast<std::uint32_t>(header);
            if (length == 0) continue;
            if (transfer.samples.size() + length > kMaxTransfer) {
                valid = false;
                break;
            }
            const auto offset = transfer.samples.size();
            transfer.samples.resize(offset + length);
            if (!memory_.Read(address, transfer.samples.data() + offset, length)) {
                valid = false;
                break;
            }
        }
        if (!valid || transfer.samples.empty() || pendingBytes_ + transfer.samples.size() > kMaxPendingBytes) {
            CompleteTransfer(kTransmitQueue, transfer.head, transfer.writable,
                             valid ? kStatusOk : kStatusBadMessage);
            completed = true;
            continue;
        }
        pendingBytes_ += transfer.samples.size();
        pending_.push_back(std::move(transfer));
    }
    if (completed) SignalUsedBuffers(kTransmitQueue);
}

void VirtioSound::ProcessReceive()
{
    // virtio_snd_pcm_xfer (stream_id) readable; device-writable sample area
    // followed by virtio_snd_pcm_status. Buffers wait in capture_ until the
    // microphone fills them.
    auto& ring = queues_[kReceiveQueue];
    VirtQueue::Element element;
    bool rejected = false;
    while (ring.Pop(memory_, element)) {
        std::uint64_t total = 0;
        for (const auto& buffer : element.writable) total += buffer.length;
        if (total <= kStatusSize || total - kStatusSize > kMaxTransfer) {
            CompleteTransfer(kReceiveQueue, element.head, element.writable, kStatusBadMessage);
            rejected = true;
            continue;
        }
        Capture capture;
        capture.head = element.head;
        capture.writable = element.writable;
        capture.capacity = total - kStatusSize;
        capture_.push_back(std::move(capture));
    }
    if (rejected) SignalUsedBuffers(kReceiveQueue);
}

void VirtioSound::CompleteTransfer(std::uint16_t queue, std::uint16_t head,
                                   const std::vector<VirtQueue::Buffer>& writable, std::uint32_t status,
                                   std::uint32_t dataBytes)
{
    auto& ring = queues_[queue];
    if (!ring.Ready()) return;
    // virtio_snd_pcm_status {status, latency_bytes} is the last 8 writable bytes.
    std::uint8_t data[kStatusSize] = {};
    std::memcpy(data, &status, 4);
    std::uint64_t total = 0;
    for (const auto& buffer : writable) total += buffer.length;
    std::uint64_t skip = total > sizeof(data) ? total - sizeof(data) : 0;
    std::size_t written = 0;
    for (const auto& buffer : writable) {
        std::uint64_t address = buffer.address;
        std::uint64_t length = buffer.length;
        const auto skipped = (std::min)(skip, length);
        skip -= skipped;
        address += skipped;
        length -= skipped;
        if (length == 0) continue;
        const auto count = (std::min<std::size_t>)(static_cast<std::size_t>(length), sizeof(data) - written);
        if (count == 0 || !memory_.Write(address, data + written, count)) break;
        written += count;
    }
    ring.Push(memory_, head, static_cast<std::uint32_t>(dataBytes + written));
}

void VirtioSound::ReleasePending()
{
    if (pending_.empty()) return;
    for (const auto& transfer : pending_) CompleteTransfer(kTransmitQueue, transfer.head, transfer.writable, kStatusOk);
    pending_.clear();
    pendingBytes_ = 0;
    SignalUsedBuffers(kTransmitQueue);
}

void VirtioSound::ReleaseCapture()
{
    if (capture_.empty()) return;
    for (const auto& capture : capture_) {
        CompleteTransfer(kReceiveQueue, capture.head, capture.writable, kStatusOk,
                         static_cast<std::uint32_t>(capture.filled));
    }
    capture_.clear();
    SignalUsedBuffers(kReceiveQueue);
}

std::size_t VirtioSound::Pull(std::uint8_t* destination, std::size_t bytes)
{
    const DeviceGuard guard(lock_);
    if (!running_ || pending_.empty()) return 0;
    // Whole frames only, so the stream never loses its alignment.
    const std::size_t frameBytes = static_cast<std::size_t>(params_.channels) * 2;
    bytes = (std::min)(bytes, pendingBytes_);
    bytes -= bytes % frameBytes;
    std::size_t copied = 0;
    bool completed = false;
    while (copied < bytes && !pending_.empty()) {
        auto& front = pending_.front();
        const auto count = (std::min)(bytes - copied, front.samples.size() - front.consumed);
        std::memcpy(destination + copied, front.samples.data() + front.consumed, count);
        front.consumed += count;
        copied += count;
        pendingBytes_ -= count;
        if (front.consumed == front.samples.size()) {
            CompleteTransfer(kTransmitQueue, front.head, front.writable, kStatusOk);
            pending_.pop_front();
            completed = true;
        }
    }
    if (completed) SignalUsedBuffers(kTransmitQueue);
    return copied;
}

void VirtioSound::PushCapture(const std::uint8_t* data, std::size_t bytes)
{
    const DeviceGuard guard(lock_);
    if (!capturing_) return;
    bool completed = false;
    while (bytes != 0 && !capture_.empty()) {
        auto& front = capture_.front();
        // Copy into the sample area, which may span several guest buffers.
        auto count = (std::min<std::uint64_t>)(bytes, front.capacity - front.filled);
        std::uint64_t offset = front.filled;
        const std::uint8_t* source = data;
        auto remaining = count;
        for (const auto& buffer : front.writable) {
            if (remaining == 0) break;
            if (offset >= buffer.length) {
                offset -= buffer.length;
                continue;
            }
            const auto chunk = (std::min<std::uint64_t>)(remaining, buffer.length - offset);
            if (!memory_.Write(buffer.address + offset, source, static_cast<std::size_t>(chunk))) {
                remaining = 0;
                break;
            }
            source += chunk;
            remaining -= chunk;
            offset = 0;
        }
        front.filled += count;
        data += count;
        bytes -= static_cast<std::size_t>(count);
        capturedBytes_ += count;
        if (front.filled == front.capacity) {
            CompleteTransfer(kReceiveQueue, front.head, front.writable, kStatusOk,
                             static_cast<std::uint32_t>(front.filled));
            capture_.pop_front();
            completed = true;
        }
    }
    droppedCaptureBytes_ += bytes; // No buffer posted: the guest reads too slowly (overrun).
    if (completed) SignalUsedBuffers(kReceiveQueue);
}

} // namespace vmm
