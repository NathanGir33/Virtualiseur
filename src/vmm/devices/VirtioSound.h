#pragma once

#include "VirtioModern.h"
#include "../audio/WasapiInput.h"
#include "../audio/WasapiOutput.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace vmm {

// virtio-sound card (virtio 1.0, device type 25) with one playback stream
// (stream 0) and one capture stream (stream 1), handled by the Linux "virtio_snd" driver (CONFIG_SND_VIRTIO). The guest
// sees an ALSA card that PipeWire/PulseAudio use like any sound card.
//
// Queues: 0 control, 1 events (unused: no jacks), 2 transmit, 3 receive.
// Transmit buffers are copied out of the ring at once, then completed by
// the host audio thread as their samples reach the Windows audio device:
// the guest is paced by the host audio clock, as with a real DAC.
// Receive buffers are filled with microphone samples by the host capture
// thread and completed once full, paced by the microphone clock.
class VirtioSound final : public VirtioModernDevice {
public:
    VirtioSound(const GuestMemory& memory, const VirtioPlacement& placement, std::wstring sessionName,
                bool microphone);
    ~VirtioSound() override;

    void SetMuted(bool muted) noexcept { if (output_) output_->SetMuted(muted); }
    std::uint64_t PlayedBytes() const noexcept { return output_ ? output_->PlayedBytes() : 0; }
    // Host microphone allowed (otherwise the guest records silence).
    void SetMicrophoneEnabled(bool enabled) { if (input_) input_->SetEnabled(enabled); }
    bool MicrophoneEnabled() const noexcept { return input_ && input_->Enabled(); }
    std::uint64_t CapturedBytes() const noexcept { return capturedBytes_.load(); }

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
    struct Capture {
        std::uint16_t head = 0;
        std::vector<VirtQueue::Buffer> writable; // Samples, then virtio_snd_pcm_status.
        std::uint64_t capacity = 0;              // Sample bytes.
        std::uint64_t filled = 0;
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
    // `dataBytes` samples were written before the status (capture).
    void CompleteTransfer(std::uint16_t queue, std::uint16_t head, const std::vector<VirtQueue::Buffer>& writable,
                          std::uint32_t status, std::uint32_t dataBytes = 0);
    void ReleasePending();
    void StopPlayback();
    void StopCapture();
    void ReleaseCapture();
    // Audio thread: copies queued samples, completes the drained buffers.
    std::size_t Pull(std::uint8_t* destination, std::size_t bytes);
    // Capture thread: fills the guest's receive buffers.
    void PushCapture(const std::uint8_t* data, std::size_t bytes);

    StreamParams params_;        // Playback (stream 0).
    StreamParams captureParams_; // Capture (stream 1).
    bool running_ = false;
    bool capturing_ = false;
    std::deque<Capture> capture_;
    std::atomic_uint64_t capturedBytes_ = 0;
    std::uint64_t droppedCaptureBytes_ = 0;
    bool announced_ = false;
    std::deque<Pending> pending_;
    std::size_t pendingBytes_ = 0;
    std::uint32_t lastRate_ = 0;
    std::uint8_t lastChannels_ = 0;
    std::unique_ptr<WasapiOutput> output_;
    std::unique_ptr<WasapiInput> input_;
};

} // namespace vmm
