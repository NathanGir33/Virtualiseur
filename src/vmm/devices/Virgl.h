#pragma once

#include <Windows.h>
#include <d3d11.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace vmm {

// Mirrors of the virglrenderer ABI (C structures of the MinGW build).
struct VirglIovec {
    void* base;
    std::size_t length;
};
struct VirglBox {
    std::uint32_t x, y, z;
    std::uint32_t w, h, d;
};
struct VirglResourceCreateArgs {
    std::uint32_t handle, target, format, bind, width, height, depth, arraySize, lastLevel, samples, flags;
};
struct VirglResourceInfo {
    std::uint32_t handle, format, width, height, depth, flags, texture, stride;
    int fourcc;
    int fd;
};

// virglrenderer (OpenGL 3D for virtio-gpu) loaded from libvirglrenderer-1.dll
// next to the executable. The library is single-threaded: a dedicated GPU
// thread owns the WGL contexts and runs every call, posted as tasks.
class VirglRenderer final {
public:
    struct Api {
        int (*init)(void* cookie, int flags, void* callbacks);
        void (*poll)();
        void (*cleanup)(void* cookie);
        void (*reset)();
        void (*getCapSet)(std::uint32_t set, std::uint32_t* maxVersion, std::uint32_t* maxSize);
        void (*fillCaps)(std::uint32_t set, std::uint32_t version, void* caps);
        int (*contextCreate)(std::uint32_t handle, std::uint32_t nameLength, const char* name);
        void (*contextDestroy)(std::uint32_t handle);
        int (*resourceCreate)(VirglResourceCreateArgs* args, VirglIovec* iov, std::uint32_t count);
        void (*resourceUnref)(std::uint32_t handle);
        int (*attachIov)(int handle, VirglIovec* iov, int count);
        void (*detachIov)(int handle, VirglIovec** iov, int* count);
        int (*submit)(void* buffer, int context, int dwords);
        int (*transferRead)(std::uint32_t handle, std::uint32_t context, std::uint32_t level, std::uint32_t stride,
                            std::uint32_t layerStride, VirglBox* box, std::uint64_t offset, VirglIovec* iov,
                            int count);
        int (*transferWrite)(std::uint32_t handle, std::uint32_t context, int level, std::uint32_t stride,
                             std::uint32_t layerStride, VirglBox* box, std::uint64_t offset, VirglIovec* iov,
                             unsigned int count);
        int (*createFence)(int fence, std::uint32_t context);
        void (*attachResource)(int context, int handle);
        void (*detachResource)(int context, int handle);
        int (*resourceInfo)(int handle, VirglResourceInfo* info);
        void* (*cursorData)(std::uint32_t handle, std::uint32_t* width, std::uint32_t* height);
        void (*setLogCallback)(void (*callback)(int level, const char* message, void* user), void* user,
                               void (*freeUser)(void*));
        void (*free)(void* memory); // The library's C runtime (UCRT) free, for cursorData.
    };

    using FenceCallback = std::function<void(std::uint32_t fence)>;

    // Loads the library, starts the GPU thread and initializes the renderer.
    // Returns nullptr (with the reason logged) when 3D is unavailable.
    static std::unique_ptr<VirglRenderer> Create(FenceCallback onFence);
    ~VirglRenderer();
    VirglRenderer(const VirglRenderer&) = delete;
    VirglRenderer& operator=(const VirglRenderer&) = delete;

    // Runs `task` on the GPU thread, in submission order.
    void Post(std::function<void()> task);
    // GPU thread only.
    const Api& Calls() const noexcept { return api_; }
    // GPU thread only: creates a ctx0 fence; the GPU thread then polls until
    // it retires and calls the fence callback.
    void CreateFence(std::uint32_t fence, std::uint32_t context);

    // --- Direct3D sharing of the screen (GPU thread only) ---
    // True when the host driver offers WGL_NV_DX_interop2 and the bridge
    // works with SharedD3DDevice().
    bool DxInteropAvailable();
    // Makes `texture` (B8G8R8A8, SharedD3DDevice) the copy target; nullptr detaches.
    bool AttachSharedTexture(ID3D11Texture2D* texture);
    // Copies rows [y, y + height) x [x, x + width) of the GL texture of
    // resource `resource` (top-down coordinates) to (destX, destY) of the
    // shared texture, GPU to GPU. Caller holds SharedD3DLock().
    bool CopyToShared(std::uint32_t resource, std::uint32_t x, std::uint32_t y, std::uint32_t width,
                      std::uint32_t height, std::uint32_t destX, std::uint32_t destY);

private:
    VirglRenderer();
    bool Load();
    void Thread(std::promise<bool>* started);
    bool CreateWindowContext();
    void DestroyWindowContext();

    static void* CreateGlContext(void* cookie, int scanout, void* param);
    static void DestroyGlContext(void* cookie, void* context);
    static int MakeCurrent(void* cookie, int scanout, void* context);
    static void WriteFence(void* cookie, std::uint32_t fence);
    static void Log(int level, const char* message, void* user);

    HMODULE library_ = nullptr;
    HMODULE opengl_ = nullptr;
    Api api_{};
    FenceCallback onFence_;

    std::thread thread_;
    std::mutex mutex_;
    HANDLE taskEvent_ = nullptr;  // Auto-reset: tasks were posted.
    HANDLE pollTimer_ = nullptr;  // High resolution: fence polling period.
    std::deque<std::function<void()>> tasks_;
    bool stopping_ = false;
    std::uint32_t lastFence_ = 0;     // GPU thread.
    std::uint32_t retiredFence_ = 0;  // GPU thread.

    // WGL (GPU thread).
    HWND window_ = nullptr;
    HDC dc_ = nullptr;
    HGLRC master_ = nullptr;
    void* createContextAttribs_ = nullptr;
    bool dxInterop_ = false; // WGL_NV_DX_interop2 offered by the host driver.

    // Direct3D bridge (GPU thread). Its own GL context, sharing objects with
    // virgl's ctx0, so virgl's cached GL state is never disturbed.
    struct Interop;
    bool InitInterop();
    void ReleaseInterop();
    std::unique_ptr<Interop> interop_;
    bool interopTried_ = false;
};

} // namespace vmm
