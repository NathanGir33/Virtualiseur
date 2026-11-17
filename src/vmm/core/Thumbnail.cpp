#include "Thumbnail.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <vector>

namespace vmm {

bool WritePngThumbnail(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height, std::size_t stride,
                       const std::filesystem::path& path, std::uint32_t maxWidth)
{
    if (pixels == nullptr || width == 0 || height == 0 || maxWidth == 0) return false;
    const std::uint32_t targetWidth = (std::min)(width, maxWidth);
    const std::uint32_t targetHeight = (std::max)(1u, static_cast<std::uint32_t>(static_cast<std::uint64_t>(height) * targetWidth / width));

    // Box filter: each target pixel averages its source rectangle; 24-bit BGR out.
    const std::size_t targetStride = (static_cast<std::size_t>(targetWidth) * 3 + 3) & ~std::size_t{3};
    std::vector<std::uint8_t> scaled(targetStride * targetHeight);
    for (std::uint32_t y = 0; y < targetHeight; ++y) {
        const auto top = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * height / targetHeight);
        const auto bottom = (std::max)(top + 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(y + 1) * height / targetHeight));
        for (std::uint32_t x = 0; x < targetWidth; ++x) {
            const auto left = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * width / targetWidth);
            const auto right = (std::max)(left + 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(x + 1) * width / targetWidth));
            std::uint32_t sum[3] = {};
            for (auto sy = top; sy < bottom; ++sy) {
                const auto* row = pixels + static_cast<std::size_t>(sy) * stride;
                for (auto sx = left; sx < right; ++sx) {
                    sum[0] += row[sx * 4];
                    sum[1] += row[sx * 4 + 1];
                    sum[2] += row[sx * 4 + 2];
                }
            }
            const auto count = (bottom - top) * (right - left);
            auto* out = scaled.data() + static_cast<std::size_t>(y) * targetStride + static_cast<std::size_t>(x) * 3;
            out[0] = static_cast<std::uint8_t>(sum[0] / count);
            out[1] = static_cast<std::uint8_t>(sum[1] / count);
            out[2] = static_cast<std::uint8_t>(sum[2] / count);
        }
    }

    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto temporary = path;
    temporary += L".tmp";
    bool ok = false;
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
             SUCCEEDED(factory->CreateStream(&stream)) &&
             SUCCEEDED(stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE)) &&
             SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
             SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
             SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
             SUCCEEDED(frame->SetSize(targetWidth, targetHeight)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
             IsEqualGUID(format, GUID_WICPixelFormat24bppBGR) &&
             SUCCEEDED(frame->WritePixels(targetHeight, static_cast<UINT>(targetStride), static_cast<UINT>(scaled.size()), scaled.data())) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    } // The stream is released (file closed) before the rename.
    if (ok) ok = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    if (!ok) DeleteFileW(temporary.c_str());
    if (SUCCEEDED(com)) CoUninitialize();
    return ok;
}

} // namespace vmm
