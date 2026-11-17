#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace vmm {

// Host audio input: records 16-bit PCM from the default Windows capture
// device (WASAPI shared mode, event driven) while the guest records, and
// hands the samples to the guest sound card as they arrive, so the guest is
// paced by the real microphone clock.
//
// The microphone is opened only while the guest records and while it is
// enabled (privacy: it is off by default). Otherwise, and without a usable
// device, silence is produced at the stream rate: the guest never stalls.
class WasapiInput final {
public:
    // Audio thread: `bytes` bytes of interleaved S16 PCM (whole frames).
    // Never called with the input's internal lock held.
    using Push = std::function<void(const std::uint8_t* data, std::size_t bytes)>;

    explicit WasapiInput(Push push);
    ~WasapiInput();
    WasapiInput(const WasapiInput&) = delete;
    WasapiInput& operator=(const WasapiInput&) = delete;

    // Any thread.
    void Start(std::uint32_t rate, std::uint16_t channels);
    void Stop();
    void SetEnabled(bool enabled);
    bool Enabled() const noexcept { return enabled_.load(); }
    std::uint64_t CapturedBytes() const noexcept { return capturedBytes_.load(); }

private:
    struct Request {
        bool active = false;
        std::uint32_t rate = 48000;
        std::uint16_t channels = 2;
        std::uint64_t serial = 0;
    };
    void Thread(std::stop_token stop);

    Push push_;
    std::mutex mutex_;
    Request request_;
    HANDLE wake_ = nullptr; // Auto-reset: the request changed or the thread must stop.
    std::atomic_bool enabled_ = false;
    std::atomic_uint64_t capturedBytes_ = 0;
    std::jthread thread_;
};

} // namespace vmm
