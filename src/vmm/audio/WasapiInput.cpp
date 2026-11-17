#include "WasapiInput.h"

#include "../core/Log.h"

#include <mmsystem.h> // WAVEFORMATEX (excluded by WIN32_LEAN_AND_MEAN).
#include <mmreg.h>
#include <Audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include "WasapiCommon.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "Avrt.lib")
#pragma comment(lib, "Ole32.lib")

namespace vmm {
namespace {

using Microsoft::WRL::ComPtr;

constexpr REFERENCE_TIME kBufferDuration = 60 * 10'000; // 60 ms, in 100 ns units.

} // namespace

WasapiInput::WasapiInput(Push push) : push_(std::move(push))
{
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (wake_ == nullptr) throw std::runtime_error("Micro : création de l'événement impossible.");
    thread_ = std::jthread([this](std::stop_token stop) { Thread(stop); });
}

WasapiInput::~WasapiInput()
{
    thread_.request_stop();
    SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
    CloseHandle(wake_);
}

void WasapiInput::Start(std::uint32_t rate, std::uint16_t channels)
{
    {
        const std::lock_guard lock(mutex_);
        request_.active = true;
        request_.rate = rate;
        request_.channels = channels;
        ++request_.serial;
    }
    SetEvent(wake_);
}

void WasapiInput::Stop()
{
    {
        const std::lock_guard lock(mutex_);
        if (!request_.active) return;
        request_.active = false;
        ++request_.serial;
    }
    SetEvent(wake_);
}

void WasapiInput::SetEnabled(bool enabled)
{
    if (enabled_.exchange(enabled) == enabled) return;
    VMM_INFO("Micro de l'hôte %s pour la VM.", enabled ? "autorisé" : "coupé");
    SetEvent(wake_);
}

void WasapiInput::Thread(std::stop_token stop)
{
    SetThreadDescription(GetCurrentThread(), L"Micro WASAPI");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    const HANDLE bufferEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        VMM_WARN("Micro : MMDeviceEnumerator indisponible, la VM recevra du silence.");
    }

    // Device state. Owned by the device worker while it runs, by this thread
    // otherwise.
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    std::wstring deviceId;
    bool started = false;
    std::uint32_t rate = 48000;
    std::uint16_t channels = 2;
    HRESULT lastError = S_OK;

    ULONGLONG retryAt = 0;
    ULONGLONG nextDeviceCheck = 0;
    bool warned = false;
    std::vector<std::uint8_t> zeros;
    double silenceFrames = 0.0;
    auto silenceClock = std::chrono::steady_clock::now();

    // Silence at the stream rate: the guest's recording keeps its clock.
    const auto pushSilence = [&] {
        const auto current = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(current - silenceClock).count();
        silenceClock = current;
        silenceFrames += (std::min)(seconds, 0.2) * rate;
        const auto frames = static_cast<std::size_t>(silenceFrames);
        silenceFrames -= static_cast<double>(frames);
        if (frames == 0) return;
        zeros.assign(frames * channels * 2u, 0);
        push_(zeros.data(), zeros.size());
    };
    const auto restartSilence = [&] {
        silenceFrames = 0.0;
        silenceClock = std::chrono::steady_clock::now();
    };

    const auto closeClient = [&] {
        if (client && started) client->Stop();
        capture.Reset();
        client.Reset();
        started = false;
    };

    const auto openClient = [&]() -> bool {
        ComPtr<IMMDevice> device;
        lastError = enumerator ? enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device) : E_FAIL;
        if (FAILED(lastError)) return false;
        ComPtr<IAudioClient> audio;
        lastError = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audio);
        if (FAILED(lastError)) return false;
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = channels;
        format.nSamplesPerSec = rate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(channels * 2);
        format.nAvgBytesPerSec = rate * format.nBlockAlign;
        // The audio engine converts the microphone to the guest's format.
        lastError = audio->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                          AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      kBufferDuration, 0, &format, nullptr);
        if (FAILED(lastError)) return false;
        ComPtr<IAudioCaptureClient> captureClient;
        lastError = audio->SetEventHandle(bufferEvent);
        if (SUCCEEDED(lastError)) lastError = audio->GetService(IID_PPV_ARGS(&captureClient));
        if (FAILED(lastError)) return false;
        deviceId = audio::DeviceId(device.Get());
        VMM_INFO("Micro : capture sur « %s » (%u Hz, %u %s).", audio::Narrow(audio::FriendlyName(device.Get())).c_str(),
                 rate, channels, channels > 1 ? "canaux" : "canal");
        client = std::move(audio);
        capture = std::move(captureClient);
        return true;
    };

    // Opening, starting and releasing a client can block for up to ~1 s:
    // a helper thread does it while this one keeps feeding silence.
    std::thread worker;
    std::atomic_bool workerDone = false;
    bool workerFailed = false;
    const auto launchWorker = [&](bool open) {
        workerDone = false;
        workerFailed = false;
        worker = std::thread([&, open] {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            closeClient();
            if (open) {
                bool ok = openClient();
                if (ok) {
                    lastError = client->Start();
                    started = ok = SUCCEEDED(lastError);
                }
                if (!ok) {
                    closeClient();
                    workerFailed = true;
                }
            }
            if (SUCCEEDED(apartment)) CoUninitialize();
            workerDone = true;
            SetEvent(wake_);
        });
    };

    const auto drain = [&]() -> HRESULT {
        UINT32 packet = 0;
        HRESULT result = S_OK;
        while (SUCCEEDED(result = capture->GetNextPacketSize(&packet)) && packet != 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            result = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(result)) return result;
            const std::size_t bytes = static_cast<std::size_t>(frames) * channels * 2u;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                zeros.assign(bytes, 0);
                push_(zeros.data(), bytes);
            } else {
                push_(data, bytes);
            }
            capturedBytes_ += bytes;
            result = capture->ReleaseBuffer(frames);
            if (FAILED(result)) return result;
        }
        return result;
    };

    std::stop_callback wakeOnStop(stop, [this] { SetEvent(wake_); });
    std::uint64_t seenSerial = ~0ull;
    while (!stop.stop_requested()) {
        Request request;
        {
            const std::lock_guard lock(mutex_);
            request = request_;
        }
        const ULONGLONG now = GetTickCount64();

        if (worker.joinable()) {
            if (!workerDone.load()) {
                if (request.active) pushSilence();
                WaitForSingleObject(wake_, 10);
                continue;
            }
            worker.join();
            if (workerFailed) {
                retryAt = now + 3000;
                if (!warned) {
                    if (lastError == E_ACCESSDENIED) {
                        VMM_WARN("Micro : accès refusé par Windows (Paramètres > Confidentialité et sécurité > "
                                 "Microphone > accès des applications de bureau) ; la VM reçoit du silence.");
                    } else {
                        VMM_WARN("Micro : aucune entrée audio Windows utilisable (0x%08lx) ; la VM reçoit du silence.",
                                 static_cast<unsigned long>(lastError));
                    }
                    warned = true;
                }
            } else {
                warned = false;
                nextDeviceCheck = now + 2000;
            }
            restartSilence();
        }

        if (request.serial != seenSerial) {
            seenSerial = request.serial;
            if (client && (request.rate != rate || request.channels != channels)) {
                launchWorker(false); // Reopened below in the new format.
                continue;
            }
            rate = request.rate;
            channels = request.channels;
            restartSilence();
        }

        // The microphone is only open while the guest records and the user allows it.
        if (!request.active || !enabled_.load()) {
            if (client) {
                launchWorker(false);
                continue;
            }
            if (!request.active) {
                WaitForSingleObject(wake_, INFINITE);
                restartSilence();
                continue;
            }
            pushSilence();
            WaitForSingleObject(wake_, 10);
            continue;
        }
        rate = request.rate;
        channels = request.channels;

        if (client && started && now >= nextDeviceCheck) {
            // Follow the default device (headset plugged, input changed).
            nextDeviceCheck = now + 2000;
            ComPtr<IMMDevice> current;
            if (enumerator && SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &current)) &&
                audio::DeviceId(current.Get()) != deviceId) {
                VMM_INFO("Micro : l'entrée audio par défaut de Windows a changé.");
                retryAt = 0;
                launchWorker(true);
                continue;
            }
        }
        if (!client || !started) {
            if (now >= retryAt) {
                launchWorker(true);
                continue;
            }
            pushSilence();
            WaitForSingleObject(wake_, 10);
            continue;
        }

        const HRESULT result = drain();
        if (FAILED(result)) {
            VMM_WARN("Micro : flux WASAPI interrompu (0x%08lx), reconnexion.", static_cast<unsigned long>(result));
            closeClient();
            retryAt = now + 500;
            continue;
        }
        const HANDLE handles[2] = {bufferEvent, wake_};
        WaitForMultipleObjects(2, handles, FALSE, 100);
        restartSilence();
    }

    if (worker.joinable()) worker.join();
    closeClient();
    enumerator.Reset();
    if (bufferEvent != nullptr) CloseHandle(bufferEvent);
    if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(com)) CoUninitialize();
}

} // namespace vmm
