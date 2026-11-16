#include "ClipboardBridge.h"

#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>

namespace vmm {
namespace {

using Microsoft::WRL::ComPtr;
constexpr std::size_t kMaxClipboard = 64u * 1024 * 1024;

void Append32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + 4);
}

std::uint32_t Load32(const std::uint8_t* data)
{
    std::uint32_t value = 0;
    std::memcpy(&value, data, 4);
    return value;
}

// CF_DIB -> .bmp file (adds the 14-byte BITMAPFILEHEADER).
std::vector<std::uint8_t> BmpFromDib(const std::uint8_t* dib, std::size_t size)
{
    if (size < sizeof(BITMAPINFOHEADER)) return {};
    BITMAPINFOHEADER header{};
    std::memcpy(&header, dib, sizeof(header));
    if (header.biSize < sizeof(BITMAPINFOHEADER) || header.biSize > size) return {};
    std::uint32_t colors = header.biClrUsed;
    if (colors == 0 && header.biBitCount <= 8) colors = 1u << header.biBitCount;
    std::uint32_t masks = 0;
    if (header.biSize == sizeof(BITMAPINFOHEADER) && header.biCompression == BI_BITFIELDS) masks = 12;
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42; // "BM"
    file.bfSize = static_cast<DWORD>(sizeof(file) + size);
    file.bfOffBits = static_cast<DWORD>(sizeof(file) + header.biSize + masks + colors * 4);
    std::vector<std::uint8_t> bmp(sizeof(file) + size);
    std::memcpy(bmp.data(), &file, sizeof(file));
    std::memcpy(bmp.data() + sizeof(file), dib, size);
    return bmp;
}

ComPtr<IWICImagingFactory> Wic()
{
    ComPtr<IWICImagingFactory> factory;
    (void)CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    return factory;
}

// Any image WIC reads (PNG, BMP, JPEG...) -> 32-bit BGRA pixels.
bool Decode(std::span<const std::uint8_t> encoded, UINT& width, UINT& height, std::vector<std::uint8_t>& pixels)
{
    auto factory = Wic();
    if (!factory || encoded.empty()) return false;
    std::vector<std::uint8_t> copy(encoded.begin(), encoded.end());
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapSource> bgra;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(copy.data(), static_cast<DWORD>(copy.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame.Get(), &bgra)) ||
        FAILED(bgra->GetSize(&width, &height)) || width == 0 || height == 0 ||
        static_cast<std::uint64_t>(width) * height * 4 > kMaxClipboard) {
        return false;
    }
    pixels.resize(static_cast<std::size_t>(width) * height * 4);
    return SUCCEEDED(bgra->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
}

// BGRA pixels -> PNG file.
bool EncodePng(UINT width, UINT height, std::vector<std::uint8_t>& pixels, std::vector<std::uint8_t>& png)
{
    auto factory = Wic();
    ComPtr<IStream> memory;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (!factory || FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &memory)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(memory.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, &options)) || FAILED(frame->Initialize(options.Get())) ||
        FAILED(frame->SetSize(width, height)) || FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WritePixels(height, width * 4, static_cast<UINT>(pixels.size()), pixels.data())) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        return false;
    }
    STATSTG stat{};
    HGLOBAL global = nullptr;
    if (FAILED(memory->Stat(&stat, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(memory.Get(), &global))) return false;
    const auto* data = static_cast<const std::uint8_t*>(GlobalLock(global));
    if (data == nullptr) return false;
    png.assign(data, data + stat.cbSize.QuadPart);
    GlobalUnlock(global);
    return true;
}

// BGRA pixels -> CF_DIB (32 bpp, bottom-up, what every Windows program reads).
std::vector<std::uint8_t> DibFromPixels(UINT width, UINT height, const std::vector<std::uint8_t>& pixels)
{
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = static_cast<LONG>(width);
    header.biHeight = static_cast<LONG>(height);
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = width * height * 4;
    std::vector<std::uint8_t> dib(sizeof(header) + pixels.size());
    std::memcpy(dib.data(), &header, sizeof(header));
    const std::size_t stride = static_cast<std::size_t>(width) * 4;
    for (UINT row = 0; row < height; ++row) {
        std::memcpy(dib.data() + sizeof(header) + row * stride, pixels.data() + (height - 1 - row) * stride, stride);
    }
    return dib;
}

bool SetClipboardBytes(UINT format, const void* data, std::size_t size)
{
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, size);
    if (global == nullptr) return false;
    void* target = GlobalLock(global);
    if (target == nullptr) {
        GlobalFree(global);
        return false;
    }
    std::memcpy(target, data, size);
    GlobalUnlock(global);
    if (SetClipboardData(format, global) == nullptr) {
        GlobalFree(global);
        return false;
    }
    return true;
}

} // namespace

ClipboardBridge::ClipboardBridge(HWND window) : window_(window)
{
    comInitialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    pngFormat_ = RegisterClipboardFormatW(L"PNG");
    AddClipboardFormatListener(window_);
    DragAcceptFiles(window_, TRUE);
}

ClipboardBridge::~ClipboardBridge()
{
    RemoveClipboardFormatListener(window_);
    if (comInitialized_) CoUninitialize();
}

bool ClipboardBridge::OpenClipboardRetry()
{
    // Another program may hold the clipboard for a moment.
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (OpenClipboard(window_)) return true;
        Sleep(10);
    }
    return false;
}

void ClipboardBridge::OnClipboardUpdate(GuestAgent* agent)
{
    // Our own writes (guest data placed on the clipboard) are not echoed back.
    if (agent == nullptr || !agent->Ready() || GetClipboardOwner() == window_) return;
    Announce(*agent);
}

void ClipboardBridge::Announce(GuestAgent& agent)
{
    std::vector<std::uint8_t> types;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) Append32(types, GuestAgent::kUtf8Text);
    if (IsClipboardFormatAvailable(CF_DIB)) {
        Append32(types, GuestAgent::kImagePng);
        Append32(types, GuestAgent::kImageBmp);
    }
    if (types.empty()) {
        if (hostGrabbed_) agent.Send(GuestAgent::kClipboardRelease, {});
        hostGrabbed_ = false;
        return;
    }
    agent.Send(GuestAgent::kClipboardGrab, types);
    hostGrabbed_ = true;
}

std::vector<std::uint8_t> ClipboardBridge::HostText(bool unixLineEnds)
{
    std::vector<std::uint8_t> text;
    if (!OpenClipboardRetry()) return text;
    if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* wide = static_cast<const wchar_t*>(GlobalLock(handle))) {
            const auto limit = GlobalSize(handle) / sizeof(wchar_t);
            std::size_t length = 0;
            while (length < limit && wide[length] != L'\0') ++length;
            std::wstring source(wide, length);
            GlobalUnlock(handle);
            if (unixLineEnds) {
                std::wstring converted;
                converted.reserve(source.size());
                for (std::size_t index = 0; index < source.size(); ++index) {
                    if (source[index] == L'\r' && index + 1 < source.size() && source[index + 1] == L'\n') continue;
                    converted.push_back(source[index]);
                }
                source.swap(converted);
            }
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, source.data(), static_cast<int>(source.size()),
                                                  nullptr, 0, nullptr, nullptr);
            text.resize(static_cast<std::size_t>((std::max)(bytes, 0)));
            if (bytes > 0) {
                WideCharToMultiByte(CP_UTF8, 0, source.data(), static_cast<int>(source.size()),
                                    reinterpret_cast<char*>(text.data()), bytes, nullptr, nullptr);
            }
        }
    }
    CloseClipboard();
    return text;
}

std::vector<std::uint8_t> ClipboardBridge::HostBmp()
{
    std::vector<std::uint8_t> bmp;
    if (!OpenClipboardRetry()) return bmp;
    if (HANDLE handle = GetClipboardData(CF_DIB)) {
        if (const auto* dib = static_cast<const std::uint8_t*>(GlobalLock(handle))) {
            const auto size = GlobalSize(handle);
            if (size <= kMaxClipboard) bmp = BmpFromDib(dib, size);
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return bmp;
}

void ClipboardBridge::Answer(GuestAgent& agent, std::uint32_t type)
{
    // A guest application pastes: send the host clipboard in the asked type.
    std::vector<std::uint8_t> reply;
    Append32(reply, type);
    std::vector<std::uint8_t> data;
    if (type == GuestAgent::kUtf8Text) {
        data = HostText(agent.GuestHasCapability(GuestAgent::kCapGuestLineEndLf));
    } else if (type == GuestAgent::kImageBmp || type == GuestAgent::kImagePng) {
        data = HostBmp();
        if (type == GuestAgent::kImagePng && !data.empty()) {
            UINT width = 0;
            UINT height = 0;
            std::vector<std::uint8_t> pixels;
            std::vector<std::uint8_t> png;
            if (Decode(data, width, height, pixels) && EncodePng(width, height, pixels, png)) data.swap(png);
            else data.clear();
        }
    }
    reply.insert(reply.end(), data.begin(), data.end());
    agent.Send(GuestAgent::kClipboard, reply); // Empty data still unblocks the guest application.
}

void ClipboardBridge::Receive(GuestAgent&, std::uint32_t type, std::span<const std::uint8_t> data)
{
    if (data.empty() || data.size() > kMaxClipboard) return;
    if (type == GuestAgent::kUtf8Text) {
        const auto* text = reinterpret_cast<const char*>(data.data());
        int length = static_cast<int>(data.size());
        while (length > 0 && text[length - 1] == '\0') --length;
        const int count = MultiByteToWideChar(CP_UTF8, 0, text, length, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>((std::max)(count, 0)), L'\0');
        if (count > 0) MultiByteToWideChar(CP_UTF8, 0, text, length, wide.data(), count);
        std::wstring windows; // Guest LF -> Windows CRLF.
        windows.reserve(wide.size() + 16);
        for (std::size_t index = 0; index < wide.size(); ++index) {
            if (wide[index] == L'\n' && (index == 0 || wide[index - 1] != L'\r')) windows.push_back(L'\r');
            windows.push_back(wide[index]);
        }
        if (!OpenClipboardRetry()) return;
        EmptyClipboard();
        SetClipboardBytes(CF_UNICODETEXT, windows.c_str(), (windows.size() + 1) * sizeof(wchar_t));
        CloseClipboard();
        return;
    }
    if (type == GuestAgent::kImagePng || type == GuestAgent::kImageBmp) {
        UINT width = 0;
        UINT height = 0;
        std::vector<std::uint8_t> pixels;
        if (!Decode(data, width, height, pixels)) return;
        const auto dib = DibFromPixels(width, height, pixels);
        if (!OpenClipboardRetry()) return;
        EmptyClipboard();
        SetClipboardBytes(CF_DIB, dib.data(), dib.size());
        // Programs that understand PNG keep the transparency.
        if (type == GuestAgent::kImagePng && pngFormat_ != 0) SetClipboardBytes(pngFormat_, data.data(), data.size());
        CloseClipboard();
    }
}

void ClipboardBridge::Process(GuestAgent* agent)
{
    if (agent == nullptr) {
        agentReady_ = false;
        return;
    }
    agentReady_ = agent->Ready();
    for (auto& message : agent->Take()) {
        const auto& data = message.data;
        switch (message.type) {
        case GuestAgent::kAnnounceCapabilities:
            // The agent (re)connected: offer it the current host clipboard.
            hostGrabbed_ = false;
            if (GetClipboardOwner() != window_) Announce(*agent);
            break;
        case GuestAgent::kClipboardGrab: {
            // The guest copied something: fetch it in the best type offered.
            hostGrabbed_ = false;
            std::uint32_t wanted = GuestAgent::kNone;
            for (std::size_t offset = 0; offset + 4 <= data.size(); offset += 4) {
                const auto type = Load32(data.data() + offset);
                if (type == GuestAgent::kUtf8Text) wanted = type;
                else if (type == GuestAgent::kImagePng && wanted != GuestAgent::kUtf8Text) wanted = type;
                else if (type == GuestAgent::kImageBmp && wanted == GuestAgent::kNone) wanted = type;
            }
            if (wanted != GuestAgent::kNone) {
                std::vector<std::uint8_t> request;
                Append32(request, wanted);
                agent->Send(GuestAgent::kClipboardRequest, request);
            }
            break;
        }
        case GuestAgent::kClipboardRequest:
            if (data.size() >= 4) Answer(*agent, Load32(data.data()));
            break;
        case GuestAgent::kClipboard:
            if (data.size() >= 4) Receive(*agent, Load32(data.data()), std::span(data).subspan(4));
            break;
        case GuestAgent::kHostNotice: {
            const std::string text(data.begin(), data.end());
            const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring wide(static_cast<std::size_t>((std::max)(length, 0)), L'\0');
            if (length > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
            MessageBoxW(window_, wide.c_str(), L"Virtualiseur — agent invité", MB_ICONINFORMATION);
            break;
        }
        default:
            break;
        }
    }
}

void ClipboardBridge::OnDropFiles(HDROP drop, GuestAgent* agent)
{
    std::vector<std::filesystem::path> files;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring path(length + 1, L'\0');
        DragQueryFileW(drop, index, path.data(), length + 1);
        path.resize(length);
        files.emplace_back(path);
    }
    DragFinish(drop);
    if (files.empty()) return;
    if (agent == nullptr || !agent->Ready()) {
        MessageBoxW(window_,
                    L"Pour recevoir des fichiers (et partager le presse-papiers), le système invité a besoin de "
                    L"l'agent spice-vdagent.\n\nDans Debian/Ubuntu : sudo apt install spice-vdagent\n"
                    L"puis ferme et rouvre la session.\n\nLes fichiers déposés arrivent dans le dossier "
                    L"Téléchargements de l'invité.",
                    L"Virtualiseur — agent invité", MB_ICONINFORMATION);
        return;
    }
    agent->SendFiles(std::move(files));
}

} // namespace vmm
