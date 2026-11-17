#include "WasapiOutput.h"

#include "../core/Log.h"

#include <mmsystem.h> // WAVEFORMATEX (excluded by WIN32_LEAN_AND_MEAN).
#include <mmreg.h>
#include <Audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "Avrt.lib")
#pragma comment(lib, "Ole32.lib")

namespace vmm {
namespace {

using Microsoft::WRL::ComPtr;

// PKEY_Device_FriendlyName, defined here to avoid initguid.h side effects.
constexpr PROPERTYKEY kDeviceFriendlyName{
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// Device buffer: large enough to ride out scheduling hiccups, small enough
// to keep the guest's audio in sync with its video.
constexpr REFERENCE_TIME kBufferDuration = 60 * 10'000; // 60 ms, in 100 ns units.
// How far ahead of the speaker the guest's samples are taken. A real DAC
// reads at the playback rate; taking the whole 60 ms at once, PipeWire's
// small ALSA buffers (~50 ms) were emptied the moment playback started and
// the guest saw an underrun right away, then again after every restart.
constexpr std::uint32_t kLeadMilliseconds = 30;

std::string Narrow(const std::wstring& text)
{
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr,
                        nullptr);
    return result;
}

std::wstring DeviceId(IMMDevice* device)
{
    LPWSTR id = nullptr;
    if (device == nullptr || FAILED(device->GetId(&id)) || id == nullptr) return {};
    std::wstring result(id);
    CoTaskMemFree(id);
    return result;
}

std::wstring FriendlyName(IMMDevice* device)
{
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(properties->GetValue(kDeviceFriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

} // namespace

WasapiOutput::WasapiOutput(std::wstring sessionName, Pull pull)
    : sessionName_(std::move(sessionName)), pull_(std::move(pull))
{
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (wake_ == nullptr) throw std::runtime_error("Son : création de l'événement impossible.");
    thread_ = std::jthread([this](std::stop_token stop) { Thread(stop); });
}

WasapiOutput::~WasapiOutput()
{
    thread_.request_stop();
    SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
    CloseHandle(wake_);
}

void WasapiOutput::Prepare(std::uint32_t rate, std::uint16_t channels)
{
    {
        const std::lock_guard lock(mutex_);
        if (request_.active || (request_.rate == rate && request_.channels == channels)) return;
        request_.rate = rate;
        request_.channels = channels;
        ++request_.serial;
    }
    SetEvent(wake_);
}

void WasapiOutput::Play(std::uint32_t rate, std::uint16_t channels)
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

void WasapiOutput::Stop()
{
    {
        const std::lock_guard lock(mutex_);
        if (!request_.active) return;
        request_.active = false;
        ++request_.serial;
    }
    SetEvent(wake_);
}

void WasapiOutput::Thread(std::stop_token stop)
{
    SetThreadDescription(GetCurrentThread(), L"Audio WASAPI");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    const HANDLE bufferEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        VMM_WARN("Son : MMDeviceEnumerator indisponible, le son de la VM sera ignoré.");
    }

    // Device state. Owned by the device worker while it runs, by this thread
    // otherwise.
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    std::wstring deviceId;
    UINT32 bufferFrames = 0;
    bool started = false;
    std::uint32_t rate = 0;
    std::uint16_t channels = 0;

    ULONGLONG retryAt = 0;
    ULONGLONG nextDeviceCheck = 0;
    bool warned = false;
    std::vector<std::uint8_t> scratch;
    double nullFrames = 0.0;
    auto nullClock = std::chrono::steady_clock::now();

    // Consumes the guest's samples at the stream rate and discards them: the
    // guest keeps its real-time clock while no host device plays.
    const auto consumeAtStreamRate = [&] {
        const auto current = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(current - nullClock).count();
        nullClock = current;
        nullFrames += (std::min)(seconds, 0.2) * rate;
        const auto frames = static_cast<std::size_t>(nullFrames);
        nullFrames -= static_cast<double>(frames);
        if (frames != 0) {
            scratch.resize(frames * channels * 2u);
            playedBytes_ += pull_(scratch.data(), scratch.size());
        }
    };

    const auto closeClient = [&] {
        if (client && started) client->Stop();
        render.Reset();
        client.Reset();
        started = false;
        bufferFrames = 0;
    };

    const auto openClient = [&]() -> bool {
        const auto openStart = GetTickCount64();
        ComPtr<IMMDevice> device;
        if (!enumerator || FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return false;
        ComPtr<IAudioClient> audio;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audio))) return false;
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = channels;
        format.nSamplesPerSec = rate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(channels * 2);
        format.nAvgBytesPerSec = rate * format.nBlockAlign;
        // The audio engine converts the guest's rate and channel count to
        // the device mix format: no resampling code here.
        HRESULT result = audio->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                           AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                               AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                           kBufferDuration, 0, &format, nullptr);
        if (FAILED(result)) {
            VMM_WARN("Son : IAudioClient::Initialize a échoué (0x%08lx, %u Hz, %u canaux).",
                     static_cast<unsigned long>(result), rate, channels);
            return false;
        }
        ComPtr<IAudioRenderClient> renderClient;
        if (FAILED(audio->SetEventHandle(bufferEvent)) || FAILED(audio->GetBufferSize(&bufferFrames)) ||
            FAILED(audio->GetService(IID_PPV_ARGS(&renderClient)))) {
            return false;
        }
        ComPtr<IAudioSessionControl> session;
        if (SUCCEEDED(audio->GetService(IID_PPV_ARGS(&session)))) {
            (void)session->SetDisplayName(sessionName_.c_str(), nullptr); // Name in the Windows volume mixer.
        }
        deviceId = DeviceId(device.Get());
        VMM_INFO("Son : sortie sur « %s » (%u Hz, %u %s, tampon %u ms, ouverte en %llu ms).",
                 Narrow(FriendlyName(device.Get())).c_str(), rate, channels, channels > 1 ? "canaux" : "canal",
                 bufferFrames * 1000 / (std::max)(rate, 1u), GetTickCount64() - openStart);
        client = std::move(audio);
        render = std::move(renderClient);
        return true;
    };

    // Opening (IAudioClient::Initialize) and starting (IAudioClient::Start)
    // block for up to ~1 s when Windows has powered the output down, after
    // some 10-30 s of silence. ALSA in the guest gives up after buffer time
    // x 1.1 without progress (~350 ms for aplay, less for PipeWire), so this
    // work runs on a helper thread while this one keeps consuming the guest's
    // samples at the stream rate: at worst the start of a sound is lost,
    // never the stream.
    // Releasing a client can block as long, so the worker also closes the
    // previous one (format or default device changed).
    std::thread worker;
    std::atomic_bool workerDone = false;
    bool workerFailed = false;
    bool needReopen = false;
    const auto launchWorker = [&](bool start, bool reopen) {
        workerDone = false;
        workerFailed = false;
        worker = std::thread([&, start, reopen] {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (reopen) closeClient();
            bool ok = client || openClient();
            if (ok && start && !started) {
                const auto before = GetTickCount64();
                const HRESULT result = client->Start();
                started = SUCCEEDED(result);
                VMM_DEBUG("Son : Start() en %llu ms (0x%08lx).", GetTickCount64() - before,
                          static_cast<unsigned long>(result));
                ok = started;
            }
            if (!ok) {
                closeClient();
                workerFailed = true;
            }
            if (SUCCEEDED(apartment)) CoUninitialize();
            workerDone = true;
            SetEvent(wake_);
        });
    };

    const auto fill = [&]() -> HRESULT {
        UINT32 padding = 0;
        HRESULT result = client->GetCurrentPadding(&padding);
        if (FAILED(result)) return result;
        const UINT32 lead = (std::min)(bufferFrames, rate * kLeadMilliseconds / 1000);
        const UINT32 available = lead > padding ? lead - padding : 0;
        if (available == 0) return S_OK;
        BYTE* data = nullptr;
        result = render->GetBuffer(available, &data);
        if (FAILED(result)) return result;
        const std::size_t frameBytes = channels * 2u;
        std::size_t bytes = pull_(data, available * frameBytes);
        bytes -= bytes % frameBytes;
        playedBytes_ += bytes;
        return render->ReleaseBuffer(static_cast<UINT32>(bytes / frameBytes), muted_ ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
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
                // The device is being opened or woken up.
                if (request.active) consumeAtStreamRate();
                WaitForSingleObject(wake_, 10);
                continue;
            }
            worker.join();
            if (workerFailed) {
                retryAt = now + 3000;
                if (!warned) {
                    VMM_WARN("Son : aucune sortie audio Windows utilisable ; le son de la VM est ignoré.");
                    warned = true;
                }
            } else {
                warned = false;
                nextDeviceCheck = now + 2000;
            }
        }

        if (!request.active) {
            if (started) {
                client->Stop();
                client->Reset(); // Drop what is still buffered: pause is immediate.
                started = false;
            }
            // Keep a client open for the prepared format, so START plays at once.
            if ((!client || needReopen || request.rate != rate || request.channels != channels) && now >= retryAt) {
                rate = request.rate;
                channels = request.channels;
                launchWorker(false, true);
                needReopen = false;
                continue;
            }
            WaitForSingleObject(wake_, client ? INFINITE : 3000);
            continue;
        }

        if (request.serial != seenSerial) {
            seenSerial = request.serial;
            if (client && (request.rate != rate || request.channels != channels)) needReopen = true;
            rate = request.rate;
            channels = request.channels;
            nullFrames = 0.0;
            nullClock = std::chrono::steady_clock::now();
        }
        if (client && !needReopen && now >= nextDeviceCheck) {
            // Follow the default device (headphones plugged, output changed).
            nextDeviceCheck = now + 2000;
            ComPtr<IMMDevice> current;
            if (enumerator && SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &current)) &&
                DeviceId(current.Get()) != deviceId) {
                VMM_INFO("Son : le périphérique de sortie par défaut de Windows a changé.");
                needReopen = true;
                retryAt = 0;
            }
        }
        if ((!client || !started || needReopen) && now >= retryAt) {
            launchWorker(true, needReopen);
            needReopen = false;
            nullClock = std::chrono::steady_clock::now();
            continue;
        }
        if (!client || !started || needReopen) {
            // No host device for now: keep the guest's clock running.
            consumeAtStreamRate();
            WaitForSingleObject(wake_, 10);
            continue;
        }

        const HRESULT result = fill();
        if (FAILED(result)) {
            VMM_WARN("Son : flux WASAPI interrompu (0x%08lx), reconnexion.", static_cast<unsigned long>(result));
            closeClient();
            retryAt = now + 500;
            continue;
        }
        const HANDLE handles[2] = {bufferEvent, wake_};
        WaitForMultipleObjects(2, handles, FALSE, 100);
        nullClock = std::chrono::steady_clock::now();
    }

    if (worker.joinable()) worker.join();
    closeClient();
    enumerator.Reset();
    if (bufferEvent != nullptr) CloseHandle(bufferEvent);
    if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(com)) CoUninitialize();
}

} // namespace vmm
