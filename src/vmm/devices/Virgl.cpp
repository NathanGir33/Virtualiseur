#include "Virgl.h"

#include "../core/Log.h"

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

} // namespace

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

    api_.cleanup(this);
    DestroyWindowContext();
}

} // namespace vmm
