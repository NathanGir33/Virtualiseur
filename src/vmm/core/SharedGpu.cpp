#include "SharedGpu.h"

#include "Log.h"

#include <d3d10.h> // ID3D10Multithread.

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace vmm {

ID3D11Device* SharedD3DDevice()
{
    static Microsoft::WRL::ComPtr<ID3D11Device> device;
    static std::once_flag once;
    std::call_once(once, [] {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                            D3D_FEATURE_LEVEL_10_0};
        // BGRA support is required by Direct2D.
        HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                           levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, device.GetAddressOf(),
                                           nullptr, nullptr);
        if (FAILED(result)) {
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                       ARRAYSIZE(levels), D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, nullptr);
        }
        if (FAILED(result)) {
            VMM_WARN("Direct3D 11 indisponible (0x%08lx) : affichage GDI.", static_cast<unsigned long>(result));
            device.Reset();
            return;
        }
        Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
        if (SUCCEEDED(device.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
    });
    return device.Get();
}

std::recursive_mutex& SharedD3DLock()
{
    static std::recursive_mutex lock;
    return lock;
}

} // namespace vmm
