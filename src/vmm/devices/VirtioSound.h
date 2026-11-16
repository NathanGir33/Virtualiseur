#pragma once

#include "VirtioModern.h"
#include "../audio/WasapiOutput.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace vmm {

// virtio-sound card (virtio 1.0, device type 25) with one playback stream,
// handled by the Linux "virtio_snd" driver (CONFIG_SND_VIRTIO). The guest
// sees an ALSA card that PipeWire/PulseAudio use like any sound card.
//
// Queues: 0 control, 1 events (unused: no jacks), 2 transmit, 3 receive.
// Transmit buffers are copied out of the ring at once, then completed by
// the host audio thread as their samples reach the Windows audio device:
// the guest is paced by the host audio clock, as with a real DAC.
class VirtioSound final : public VirtioModernDevice {
public:
    VirtioSound(const GuestMemory& memory, InterruptSink& sink, DeviceLock& lock, IoBus& ioBus,
                std::uint16_t ioBase, std::uint32_t gsi, std::wstring sessionName);
    ~VirtioSound() override;

    void SetMuted(bool muted) noexcept { if (output_) output_->SetMuted(muted); }
    std::uint64_t PlayedBytes() const noexcept { return output_ ? output_->PlayedBytes() : 0; }

protected:
    std::uint64_t DeviceFeatures() const override { return 0; }
    std::uint8_t ReadDeviceConfig(std::uint32_t offset) override;
    void QueueNotify(std::uint16_t queue) override;
    void DeviceReset() override;
    void DriverOk() override;

private:
    struct Pending {
        std::uint16_t head = 0;
        std::vector<VirtQueue::Buffer> writable; // virtio_snd_pcm_status.
        std::vector<std::uint8_t> samples;
        std::size_t consumed = 0;
    };
    struct StreamParams {
        bool set = false;
        std::uint32_t bufferBytes = 0;
        std::uint32_t periodBytes = 0;
        std::uint8_t channels = 2;
        std::uint8_t format = 0;
        std::uint8_t rate = 0;
    };

    void ProcessControl();
    void ProcessTransmit();
    void ProcessReceive();
    std::uint32_t HandleControl(const std::vector<std::uint8_t>& request, std::vector<std::uint8_t>& payload);
    // Writes {status, latency} into the element's device-writable buffers and
    // returns it to the guest (no interrupt: the caller signals once).
    void CompleteTransfer(std::uint16_t queue, std::uint16_t head, const std::vector<VirtQueue::Buffer>& writable,
                          std::uint32_t status);
    void ReleasePending();
    void StopPlayback();
    // Audio thread: copies queued samples, completes the drained buffers.
    std::size_t Pull(std::uint8_t* destination, std::size_t bytes);

    DeviceLock& lock_;
    StreamParams params_;
    bool running_ = false;
    bool announced_ = false;
    std::deque<Pending> pending_;
    std::size_t pendingBytes_ = 0;
    std::uint32_t lastRate_ = 0;
    std::uint8_t lastChannels_ = 0;
    std::unique_ptr<WasapiOutput> output_;
};

} // namespace vmm
