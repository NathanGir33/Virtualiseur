#include "Virgl.h"

#include "../core/Log.h"
#include "../core/SharedGpu.h"

#include <gl/GL.h>

#include <string>
#include <type_traits>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace vmm {
namespace {

// struct virgl_renderer_callbacks, version 1 fields only.
struct Callbacks {
    int version;
    void (*writeFence)(void* cookie, std::uint32_t fence);
    void* (*createGlContext)(void* cookie, int scanout, void* param);
    void (*destroyGlContext)(void* cookie, void* context);
    int (*makeCurrent)(void* cookie, int scanout, void* context);
};

// struct virgl_renderer_gl_ctx_param.
struct GlContextParam {
    int version;
    bool shared;
    int major;
    int minor;
    int compatibility;
};

constexpr int kWglContextMajorVersion = 0x2091;
constexpr int kWglContextMinorVersion = 0x2092;
constexpr int kWglContextProfileMask = 0x9126;
constexpr int kWglContextCoreProfile = 0x0001;
constexpr int kWglContextCompatibilityProfile = 0x0002;
using CreateContextAttribs = HGLRC(WINAPI*)(HDC, HGLRC, const int*);

std::wstring ExecutableDirectory()
{
    std::wstring path(MAX_PATH, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
}

// OpenGL 3.0 framebuffer objects and WGL_NV_DX_interop(2), loaded with
// wglGetProcAddress.
constexpr GLenum kReadFramebuffer = 0x8CA8;
constexpr GLenum kDrawFramebuffer = 0x8CA9;
constexpr GLenum kColorAttachment0 = 0x8CE0;
constexpr GLenum kFramebufferComplete = 0x8CD5;
constexpr GLenum kAccessReadWrite = 0x0001; // WGL_ACCESS_READ_WRITE_NV.
constexpr std::uint32_t kResourceY0Top = 1;  // VIRGL_RESOURCE_Y_0_TOP.

using GenFramebuffers = void(APIENTRY*)(GLsizei, GLuint*);
using DeleteFramebuffers = void(APIENTRY*)(GLsizei, const GLuint*);
using BindFramebuffer = void(APIENTRY*)(GLenum, GLuint);
using FramebufferTexture2D = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
using CheckFramebufferStatus = GLenum(APIENTRY*)(GLenum);
using BlitFramebuffer = void(APIENTRY*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
using DxOpenDevice = HANDLE(WINAPI*)(void*);
using DxCloseDevice = BOOL(WINAPI*)(HANDLE);
using DxRegisterObject = HANDLE(WINAPI*)(HANDLE, void*, GLuint, GLenum, GLenum);
using DxUnregisterObject = BOOL(WINAPI*)(HANDLE, HANDLE);
using DxLockObjects = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);

// Makes a GL context current for a scope, then puts back whatever virgl had.
class ContextScope final {
public:
    ContextScope(HDC dc, HGLRC context) : previousDc_(wglGetCurrentDC()), previous_(wglGetCurrentContext())
    {
        // Another context's rendering must be flushed before this one reads it.
        if (previous_ != nullptr) glFlush();
        ok_ = wglMakeCurrent(dc, context) != FALSE;
        dc_ = dc;
    }
    ~ContextScope() { wglMakeCurrent(previousDc_ != nullptr ? previousDc_ : dc_, previous_); }
    bool Ok() const noexcept { return ok_; }

private:
    HDC previousDc_;
    HGLRC previous_;
    HDC dc_ = nullptr;
    bool ok_ = false;
};

} // namespace

struct VirglRenderer::Interop {
    HGLRC context = nullptr;
    HANDLE device = nullptr;      // wglDXOpenDeviceNV(SharedD3DDevice()).
    HANDLE object = nullptr;      // Registered shared texture.
    GLuint texture = 0;           // Its GL name.
    GLuint readFramebuffer = 0;
    GLuint drawFramebuffer = 0;
    GenFramebuffers genFramebuffers = nullptr;
    DeleteFramebuffers deleteFramebuffers = nullptr;
    BindFramebuffer bindFramebuffer = nullptr;
    FramebufferTexture2D framebufferTexture2D = nullptr;
    CheckFramebufferStatus checkFramebufferStatus = nullptr;
    BlitFramebuffer blitFramebuffer = nullptr;
    DxOpenDevice openDevice = nullptr;
    DxCloseDevice closeDevice = nullptr;
    DxRegisterObject registerObject = nullptr;
    DxUnregisterObject unregisterObject = nullptr;
    DxLockObjects lockObjects = nullptr;
    DxLockObjects unlockObjects = nullptr;
};

VirglRenderer::VirglRenderer() = default;

bool VirglRenderer::InitInterop()
{
    if (!dxInterop_ || master_ == nullptr || SharedD3DDevice() == nullptr) return false;
    auto interop = std::make_unique<Interop>();
    const int attributes[] = {kWglContextMajorVersion, 3, kWglContextMinorVersion, 0, 0};
    interop->context = reinterpret_cast<CreateContextAttribs>(createContextAttribs_)(dc_, master_, attributes);
    if (interop->context == nullptr) return false;
    const ContextScope scope(dc_, interop->context);
    if (!scope.Ok()) {
        wglDeleteContext(interop->context);
        return false;
    }
    const auto load = [](auto& slot, const char* name) {
        slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(wglGetProcAddress(name));
        return slot != nullptr;
    };
    bool ok = load(interop->genFramebuffers, "glGenFramebuffers") &&
              load(interop->deleteFramebuffers, "glDeleteFramebuffers") &&
              load(interop->bindFramebuffer, "glBindFramebuffer") &&
              load(interop->framebufferTexture2D, "glFramebufferTexture2D") &&
              load(interop->checkFramebufferStatus, "glCheckFramebufferStatus") &&
              load(interop->blitFramebuffer, "glBlitFramebuffer") &&
              load(interop->openDevice, "wglDXOpenDeviceNV") && load(interop->closeDevice, "wglDXCloseDeviceNV") &&
              load(interop->registerObject, "wglDXRegisterObjectNV") &&
              load(interop->unregisterObject, "wglDXUnregisterObjectNV") &&
              load(interop->lockObjects, "wglDXLockObjectsNV") && load(interop->unlockObjects, "wglDXUnlockObjectsNV");
    if (ok) {
        const SharedD3DGuard d3d(SharedD3DLock());
        interop->device = interop->openDevice(SharedD3DDevice());
        ok = interop->device != nullptr;
        if (!ok) VMM_INFO("virtio-gpu : partage OpenGL/Direct3D refusé par le pilote (%lu).", GetLastError());
    }
    if (!ok) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(interop->context);
        return false;
    }
    interop->genFramebuffers(1, &interop->readFramebuffer);
    interop->genFramebuffers(1, &interop->drawFramebuffer);
    interop_ = std::move(interop);
    return true;
}

void VirglRenderer::ReleaseInterop()
{
    if (!interop_) return;
    {
        const ContextScope scope(dc_, interop_->context);
        AttachSharedTexture(nullptr);
        interop_->deleteFramebuffers(1, &interop_->readFramebuffer);
        interop_->deleteFramebuffers(1, &interop_->drawFramebuffer);
        const SharedD3DGuard d3d(SharedD3DLock());
        interop_->closeDevice(interop_->device);
    }
    wglDeleteContext(interop_->context);
    interop_.reset();
}

bool VirglRenderer::DxInteropAvailable()
{
    if (!interopTried_) {
        interopTried_ = true;
        const bool ok = InitInterop();
        VMM_INFO("virtio-gpu : écran virgl %s.", ok ? "affiché depuis une texture Direct3D partagée (sans relecture)"
                                                       : "relu en mémoire pour l'affichage");
    }
    return interop_ != nullptr;
}

bool VirglRenderer::AttachSharedTexture(ID3D11Texture2D* texture)
{
    if (!interop_) return false;
    const ContextScope scope(dc_, interop_->context);
    if (!scope.Ok()) return false;
    const SharedD3DGuard d3d(SharedD3DLock());
    if (interop_->object != nullptr) {
        interop_->unregisterObject(interop_->device, interop_->object);
        interop_->object = nullptr;
    }
    if (interop_->texture != 0) {
        glDeleteTextures(1, &interop_->texture);
        interop_->texture = 0;
    }
    if (texture == nullptr) return true;
    glGenTextures(1, &interop_->texture);
    interop_->object = interop_->registerObject(interop_->device, texture, interop_->texture, GL_TEXTURE_2D,
                                                kAccessReadWrite);
    if (interop_->object == nullptr) {
        VMM_WARN("virtio-gpu : texture Direct3D refusée par OpenGL (%lu).", GetLastError());
        glDeleteTextures(1, &interop_->texture);
        interop_->texture = 0;
        return false;
    }
    return true;
}

bool VirglRenderer::CopyToShared(std::uint32_t resource, std::uint32_t x, std::uint32_t y, std::uint32_t width,
                                 std::uint32_t height, std::uint32_t destX, std::uint32_t destY)
{
    if (!interop_ || interop_->object == nullptr || width == 0 || height == 0) return false;
    VirglResourceInfo info{};
    if (api_.resourceInfo(static_cast<int>(resource), &info) != 0 || info.texture == 0) return false;
    const ContextScope scope(dc_, interop_->context);
    if (!scope.Ok()) return false;
    auto& gl = *interop_;
    if (!gl.lockObjects(gl.device, 1, &gl.object)) return false;
    gl.bindFramebuffer(kReadFramebuffer, gl.readFramebuffer);
    gl.framebufferTexture2D(kReadFramebuffer, kColorAttachment0, GL_TEXTURE_2D, info.texture, 0);
    gl.bindFramebuffer(kDrawFramebuffer, gl.drawFramebuffer);
    gl.framebufferTexture2D(kDrawFramebuffer, kColorAttachment0, GL_TEXTURE_2D, gl.texture, 0);
    const bool complete = gl.checkFramebufferStatus(kReadFramebuffer) == kFramebufferComplete &&
                          gl.checkFramebufferStatus(kDrawFramebuffer) == kFramebufferComplete;
    if (complete) {
        // Shared texture rows are top-down (Direct3D) and GL addresses them
        // from memory row 0 too. A virgl resource WITHOUT Y_0_TOP holds its
        // top row first (dumb buffers, uploads); one WITH Y_0_TOP was rendered
        // bottom-up by Mesa and is flipped on the way. Same rule as QEMU,
        // which flips when the flag is absent because it presents into an
        // OpenGL framebuffer (bottom-up).
        const bool topDown = (info.flags & kResourceY0Top) == 0;
        const auto sourceY = static_cast<GLint>(topDown ? y : info.height - y - height);
        const auto top = static_cast<GLint>(destY);
        const auto bottom = static_cast<GLint>(destY + height);
        gl.blitFramebuffer(static_cast<GLint>(x), sourceY, static_cast<GLint>(x + width),
                           sourceY + static_cast<GLint>(height), static_cast<GLint>(destX), topDown ? top : bottom,
                           static_cast<GLint>(destX + width), topDown ? bottom : top, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    gl.framebufferTexture2D(kReadFramebuffer, kColorAttachment0, GL_TEXTURE_2D, 0, 0);
    gl.framebufferTexture2D(kDrawFramebuffer, kColorAttachment0, GL_TEXTURE_2D, 0, 0);
    gl.bindFramebuffer(kReadFramebuffer, 0);
    gl.bindFramebuffer(kDrawFramebuffer, 0);
    gl.unlockObjects(gl.device, 1, &gl.object);
    return complete;
}

std::unique_ptr<VirglRenderer> VirglRenderer::Create(FenceCallback onFence)
{
    std::unique_ptr<VirglRenderer> renderer(new VirglRenderer());
    renderer->onFence_ = std::move(onFence);
    if (!renderer->Load()) return nullptr;
    renderer->taskEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    renderer->pollTimer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (renderer->pollTimer_ == nullptr) renderer->pollTimer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    if (renderer->taskEvent_ == nullptr || renderer->pollTimer_ == nullptr) return nullptr;

    std::promise<bool> started;
    auto ready = started.get_future();
    auto* self = renderer.get();
    renderer->thread_ = std::thread([self, &started] { self->Thread(&started); });
    if (!ready.get()) {
        renderer->thread_.join();
        return nullptr;
    }
    return renderer;
}

VirglRenderer::~VirglRenderer()
{
    if (thread_.joinable()) {
        {
            const std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        SetEvent(taskEvent_);
        thread_.join();
    }
    if (taskEvent_ != nullptr) CloseHandle(taskEvent_);
    if (pollTimer_ != nullptr) CloseHandle(pollTimer_);
    // The library keeps global state: it stays loaded for the next VM (guest reset).
}

bool VirglRenderer::Load()
{
    const auto directory = ExecutableDirectory();
    library_ = LoadLibraryExW((directory + L"libvirglrenderer-1.dll").c_str(), nullptr,
                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (library_ == nullptr) {
        VMM_INFO("Accélération 3D indisponible: libvirglrenderer-1.dll introuvable à côté du VMM (erreur %lu).",
                 GetLastError());
        return false;
    }
    opengl_ = LoadLibraryW(L"opengl32.dll");
    bool complete = opengl_ != nullptr;
    const auto bind = [this, &complete](auto& slot, const char* name) {
        slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(GetProcAddress(library_, name));
        if (slot == nullptr) {
            VMM_WARN("virglrenderer: fonction %s absente.", name);
            complete = false;
        }
    };
    bind(api_.init, "virgl_renderer_init");
    bind(api_.poll, "virgl_renderer_poll");
    bind(api_.cleanup, "virgl_renderer_cleanup");
    bind(api_.reset, "virgl_renderer_reset");
    bind(api_.getCapSet, "virgl_renderer_get_cap_set");
    bind(api_.fillCaps, "virgl_renderer_fill_caps");
    bind(api_.contextCreate, "virgl_renderer_context_create");
    bind(api_.contextDestroy, "virgl_renderer_context_destroy");
    bind(api_.resourceCreate, "virgl_renderer_resource_create");
    bind(api_.resourceUnref, "virgl_renderer_resource_unref");
    bind(api_.attachIov, "virgl_renderer_resource_attach_iov");
    bind(api_.detachIov, "virgl_renderer_resource_detach_iov");
    bind(api_.submit, "virgl_renderer_submit_cmd");
    bind(api_.transferRead, "virgl_renderer_transfer_read_iov");
    bind(api_.transferWrite, "virgl_renderer_transfer_write_iov");
    bind(api_.createFence, "virgl_renderer_create_fence");
    bind(api_.attachResource, "virgl_renderer_ctx_attach_resource");
    bind(api_.detachResource, "virgl_renderer_ctx_detach_resource");
    bind(api_.resourceInfo, "virgl_renderer_resource_get_info");
    bind(api_.cursorData, "virgl_renderer_get_cursor_data");
    bind(api_.setLogCallback, "virgl_set_log_callback");
    if (HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll")) {
        api_.free = reinterpret_cast<void (*)(void*)>(GetProcAddress(ucrt, "free"));
    }
    if (api_.free == nullptr) complete = false;
    return complete;
}

bool VirglRenderer::CreateWindowContext()
{
    // A hidden window provides the device context for every GL context.
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"VirtualiseurVirgl";
    RegisterClassW(&windowClass);
    window_ = CreateWindowExW(0, windowClass.lpszClassName, L"virgl", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16,
                              nullptr, nullptr, windowClass.hInstance, nullptr);
    if (window_ == nullptr) return false;
    dc_ = GetDC(window_);
    PIXELFORMATDESCRIPTOR format{};
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    format.cDepthBits = 24;
    format.cStencilBits = 8;
    const int index = ChoosePixelFormat(dc_, &format);
    if (index == 0 || !SetPixelFormat(dc_, index, &format)) return false;

    // A legacy context is needed to look up wglCreateContextAttribsARB.
    HGLRC legacy = wglCreateContext(dc_);
    if (legacy == nullptr || !wglMakeCurrent(dc_, legacy)) {
        if (legacy != nullptr) wglDeleteContext(legacy);
        return false;
    }
    createContextAttribs_ = reinterpret_cast<void*>(wglGetProcAddress("wglCreateContextAttribsARB"));
    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    VMM_INFO("OpenGL hôte: %s (%s).", renderer != nullptr ? renderer : "?", version != nullptr ? version : "?");
    // Sharing textures with Direct3D lets the display skip the readback.
    using GetExtensions = const char*(WINAPI*)(HDC);
    if (const auto getExtensions = reinterpret_cast<GetExtensions>(wglGetProcAddress("wglGetExtensionsStringARB"))) {
        const std::string extensions = getExtensions(dc_) != nullptr ? getExtensions(dc_) : "";
        dxInterop_ = extensions.find("WGL_NV_DX_interop2") != std::string::npos;
        VMM_INFO("OpenGL hôte : partage avec Direct3D (WGL_NV_DX_interop2) %s.", dxInterop_ ? "disponible" : "absent");
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(legacy);
    return createContextAttribs_ != nullptr;
}

void VirglRenderer::DestroyWindowContext()
{
    wglMakeCurrent(nullptr, nullptr);
    if (master_ != nullptr) wglDeleteContext(master_);
    master_ = nullptr;
    if (dc_ != nullptr) ReleaseDC(window_, dc_);
    if (window_ != nullptr) DestroyWindow(window_);
    dc_ = nullptr;
    window_ = nullptr;
}

void* VirglRenderer::CreateGlContext(void* cookie, int, void* param)
{
    auto& self = *static_cast<VirglRenderer*>(cookie);
    const auto& request = *static_cast<const GlContextParam*>(param);
    const int attributes[] = {
        kWglContextMajorVersion, request.major,
        kWglContextMinorVersion, request.minor,
        kWglContextProfileMask, request.compatibility != 0 ? kWglContextCompatibilityProfile : kWglContextCoreProfile,
        0};
    const auto create = reinterpret_cast<CreateContextAttribs>(self.createContextAttribs_);
    // Every context shares its objects with the first one (virgl's ctx0).
    HGLRC context = create(self.dc_, self.master_, attributes);
    if (context == nullptr) return nullptr; // virglrenderer retries with older versions.
    if (self.master_ == nullptr) self.master_ = context;
    return context;
}

void VirglRenderer::DestroyGlContext(void* cookie, void* context)
{
    auto& self = *static_cast<VirglRenderer*>(cookie);
    if (context == nullptr) return;
    if (static_cast<HGLRC>(context) == self.master_) self.master_ = nullptr;
    wglDeleteContext(static_cast<HGLRC>(context));
}

int VirglRenderer::MakeCurrent(void* cookie, int, void* context)
{
    auto& self = *static_cast<VirglRenderer*>(cookie);
    if (context == nullptr) return wglMakeCurrent(nullptr, nullptr) ? 0 : -1;
    return wglMakeCurrent(self.dc_, static_cast<HGLRC>(context)) ? 0 : -1;
}

void VirglRenderer::WriteFence(void* cookie, std::uint32_t fence)
{
    auto& self = *static_cast<VirglRenderer*>(cookie);
    self.retiredFence_ = fence;
    if (self.onFence_) self.onFence_(fence);
}

void VirglRenderer::Log(int level, const char* message, void*)
{
    if (message == nullptr) return;
    std::string text(message);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    // Guest probes (e.g. GStreamer asking for video decoding, which this
    // build lacks) can repeat errors by the hundred: keep the first ones.
    static int errors = 0;
    if (level >= 3 && errors < 20) {
        if (++errors == 20) VMM_WARN("virglrenderer: %s (erreurs suivantes en mode --verbose seulement)", text.c_str());
        else VMM_WARN("virglrenderer: %s", text.c_str());
    } else {
        VMM_DEBUG("virglrenderer: %s", text.c_str());
    }
}

void VirglRenderer::CreateFence(std::uint32_t fence, std::uint32_t context)
{
    lastFence_ = fence;
    (void)api_.createFence(static_cast<int>(fence), context);
}

void VirglRenderer::Post(std::function<void()> task)
{
    {
        const std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    SetEvent(taskEvent_);
}

void VirglRenderer::Thread(std::promise<bool>* started)
{
    SetThreadDescription(GetCurrentThread(), L"GPU virgl");
    static Callbacks callbacks{1, &VirglRenderer::WriteFence, &VirglRenderer::CreateGlContext,
                               &VirglRenderer::DestroyGlContext, &VirglRenderer::MakeCurrent};
    api_.setLogCallback(&VirglRenderer::Log, nullptr, nullptr);
    bool ok = CreateWindowContext();
    if (!ok) {
        VMM_WARN("Accélération 3D indisponible: création du contexte OpenGL (WGL) impossible.");
    } else if (const int error = api_.init(this, 0, &callbacks); error != 0) {
        VMM_WARN("Accélération 3D indisponible: virgl_renderer_init a échoué (%d).", error);
        ok = false;
    }
    if (!ok) {
        DestroyWindowContext();
        started->set_value(false);
        return;
    }
    started->set_value(true);

    for (;;) {
        std::deque<std::function<void()>> batch;
        {
            const std::lock_guard lock(mutex_);
            if (stopping_ && tasks_.empty()) break;
            batch.swap(tasks_);
        }
        for (auto& task : batch) task();
        api_.poll();

        if (lastFence_ != retiredFence_) {
            // Fences in flight: poll every 0.5 ms (the system timer tick is
            // 15.6 ms, far too slow for frame pacing).
            LARGE_INTEGER due{};
            due.QuadPart = -5000;
            SetWaitableTimer(pollTimer_, &due, 0, nullptr, nullptr, FALSE);
            const HANDLE handles[2] = {taskEvent_, pollTimer_};
            WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        } else {
            WaitForSingleObject(taskEvent_, INFINITE);
        }
    }

    ReleaseInterop();
    api_.cleanup(this);
    DestroyWindowContext();
}

} // namespace vmm
