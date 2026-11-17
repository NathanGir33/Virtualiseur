#pragma once

// Small helpers shared by the WASAPI output and input.

#include <Windows.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <string>

namespace vmm::audio {

// PKEY_Device_FriendlyName, defined here to avoid initguid.h side effects.
inline constexpr PROPERTYKEY kDeviceFriendlyName{
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

inline std::string Narrow(const std::wstring& text)
{
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr,
                        nullptr);
    return result;
}

inline std::wstring DeviceId(IMMDevice* device)
{
    LPWSTR id = nullptr;
    if (device == nullptr || FAILED(device->GetId(&id)) || id == nullptr) return {};
    std::wstring result(id);
    CoTaskMemFree(id);
    return result;
}

inline std::wstring FriendlyName(IMMDevice* device)
{
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
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

} // namespace vmm::audio
