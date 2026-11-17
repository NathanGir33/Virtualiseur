#pragma once

#include "DirtyRows.h"

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace vmm {

// One Direct3D 11 device for the whole process, shared by the display window
// (Direct2D on a DXGI swap chain) and the virgl GPU thread (OpenGL writes
// into Direct3D textures through WGL_NV_DX_interop2). Created on first use;
// nullptr when Direct3D 11 is unavailable.
ID3D11Device* SharedD3DDevice();

// Held around every use of that device: the window while it draws and
// presents, the GPU thread while it creates, locks and fills the shared
// texture. The two threads never drive the immediate context together.
std::recursive_mutex& SharedD3DLock();
using SharedD3DGuard = std::lock_guard<std::recursive_mutex>;

// The guest screen as a GPU texture (virgl 3D mode): the GPU thread copies
// each flushed rectangle of the scanout into it, GPU to GPU, and the window
// draws it as is. No readback to system memory, no upload back to the GPU.
// Fields are guarded by SharedD3DLock().
struct SharedScanout {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture; // B8G8R8A8, guest top row first.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t generation = 0; // Bumped when the texture is replaced.
    DirtyRows dirty;              // Rows written since the window last looked.
};

} // namespace vmm
