#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace vmm {

// Host audio output: plays 16-bit PCM on the default Windows render device
// (WASAPI shared mode, event driven). The audio thread pulls samples from
// the guest sound card as space frees up in the device buffer, so the guest
// is paced by the real audio clock.
//
// Without a usable audio device (none plugged, device lost), samples are
// still consumed at the stream rate and discarded, so the guest never stalls.
class WasapiOutput final {
public:
    // Audio thread: fills `destination` with up to `bytes` bytes (a whole
    // number of frames) and returns the count written. Never called with the
    // output's internal lock held.
    using Pull = std::function<std::size_t(std::uint8_t* destination, std::size_t bytes)>;

    WasapiOutput(std::wstring sessionName, Pull pull);
    ~WasapiOutput();
    WasapiOutput(const WasapiOutput&) = delete;
    WasapiOutput& operator=(const WasapiOutput&) = delete;

    // Any thread: opens the device for this format without playing yet.
    // Opening a WASAPI client takes up to ~1 s; done at START, it outlasts
    // ALSA's wait for the first period (buffer time x 1.1) and the guest gives
    // up with an I/O error. The output also opens 48 kHz stereo on its own at
    // creation.
    void Prepare(std::uint32_t rate, std::uint16_t channels);
    // Any thread: starts (or reconfigures) playback of interleaved S16 PCM.
    void Play(std::uint32_t rate, std::uint16_t channels);
    // Any thread: stops playback and drops what the host device still holds.
    void Stop();
    void SetMuted(bool muted) noexcept { muted_ = muted; }
    bool Muted() const noexcept { return muted_; }
    std::uint64_t PlayedBytes() const noexcept { return playedBytes_.load(); }

private:
    struct Request {
        bool active = false;
        std::uint32_t rate = 48000;
        std::uint16_t channels = 2;
        std::uint64_t serial = 0;
    };
    void Thread(std::stop_token stop);

    std::wstring sessionName_;
    Pull pull_;
    std::mutex mutex_;
    Request request_;
    HANDLE wake_ = nullptr; // Auto-reset: the request changed or the thread must stop.
    std::atomic_bool muted_ = false;
    std::atomic_uint64_t playedBytes_ = 0;
    std::jthread thread_;
};

} // namespace vmm
