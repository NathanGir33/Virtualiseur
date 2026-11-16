#pragma once

#include "core/Bus.h"
#include "core/GuestMemory.h"
#include "boot/LinuxLoader.h"
#include "agent/GuestAgent.h"
#include "devices/VirtioGpu.h"
#include "devices/VirtioInput.h"
#include "devices/VirtioSerial.h"
#include "net/NetworkBackend.h"

#include "Iso9660Image.h"
#include "VirtualDisk.h"
#include "WindowsHypervisorBackend.h"

#include <WinHvEmulation.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace vmm {

class IoApic;
class Pic8259Pair;
class Pit8254;
class CmosRtc;
class Uart16550;
class Ps2Controller;
class AcpiPm;
class PciBus;
class PciFunction;
class IdeController;
class VirtioBlk;
class VirtioNet;
class VgaRegisters;
class PostPort;
class VirtioSound;

enum class DiskBus { Virtio, Ide };
// Network card attachment, as in VirtualBox.
enum class NetworkMode { None, Nat, Bridged, Internal, HostOnly, Disconnected };
enum class BootDevice { Auto, Cdrom, Disk, Kernel };
enum class DisplayMode { Graphics, Text, None };

struct VmConfig {
    std::string name = "VM";
    std::uint32_t cpus = 1;
    std::uint64_t memoryMiB = 2048;
    std::filesystem::path iso;
    std::filesystem::path disk;
    std::uint64_t diskSizeMiB = 0;
    DiskBus diskBus = DiskBus::Virtio;
    BootDevice boot = BootDevice::Auto;
    std::filesystem::path kernel;
    std::filesystem::path initrd;
    std::string commandLine;      // Appended to the boot source arguments.
    bool replaceCommandLine = false;
    DisplayMode display = DisplayMode::Graphics;
    std::uint32_t width = 1280;
    std::uint32_t height = 800;
    NetworkMode network = NetworkMode::None;
    MacAddress mac{0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    std::string bridgeAdapter;           // Host adapter for NetworkMode::Bridged (empty: first connected).
    std::string internalNetwork = "intnet";
    std::vector<PortForward> portForwards; // NetworkMode::Nat and HostOnly.
    bool fullscreen = false;             // Open the window in full screen.
    bool graphicalInstaller = false;
    bool serialConsole = false;   // Kernel console on ttyS0 too.
    bool virtioGpu = true;        // virtio-gpu display adapter (graphics mode).
    bool gpu3d = true;            // virgl 3D acceleration when virglrenderer is available.
    bool tablet = true;           // virtio-input absolute pointer (graphics mode).
    bool guestTools = true;       // Debian installer: install spice-vdagent with the desktop.
    std::filesystem::path preseed; // Debian preseed file injected as /preseed.cfg (ISO boot).
    bool rtcLocalTime = false;     // Hardware clock in local time (Windows) instead of UTC.
    bool audio = true;             // virtio-sound card played on the default Windows output.
};

// What the display window should render.
struct DisplaySnapshot {
    DisplayMode mode = DisplayMode::None;
    const std::uint8_t* framebuffer = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    const volatile std::uint16_t* text = nullptr;
    std::uint16_t cursor = 0;
    bool cursorVisible = false;
};

enum class VmState { Starting, Running, Paused, Stopped };
enum class VmExit { PowerOff, Reset, Error };

class Vm final {
public:
    using SerialOutput = std::function<void(std::uint8_t)>;

    Vm(const VmConfig& config, SerialOutput serialOutput);
    ~Vm();
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

    void Start();
    // Blocks until every vCPU thread has stopped.
    VmExit Wait();

    void Pause();
    void Resume();
    void PowerOff();
    void Reset();
    // Presses the ACPI power button. Returns false if the guest ignores it.
    bool AcpiShutdown();

    VmState State() const noexcept { return state_.load(); }
    const std::string& BootDescription() const noexcept { return bootDescription_; }
    bool CdromEjected() const;
    DisplaySnapshot Display() const;
    // True when the guest framebuffer changed since the previous call.
    bool FramebufferDirty() const { return memory_->Framebuffer() == nullptr || memory_->FramebufferChanged(); }
    // Rows [top, bottom) of the framebuffer changed since the previous call.
    bool FramebufferDirtyRows(std::uint32_t& top, std::uint32_t& bottom) const
    {
        if (!framebuffer_ || memory_->Framebuffer() == nullptr) return false;
        std::uint64_t first = 0;
        std::uint64_t last = 0;
        if (!memory_->FramebufferDirtyRange(first, last)) return false;
        top = static_cast<std::uint32_t>(first / framebuffer_->stride);
        bottom = static_cast<std::uint32_t>((std::min<std::uint64_t>)(framebuffer_->height,
            (last + framebuffer_->stride - 1) / framebuffer_->stride));
        return bottom > top;
    }

    // virtio-gpu screen (thread-safe). CopyGpuScanout returns false while
    // the guest shows the boot framebuffer instead.
    bool CopyGpuScanout(ScanoutCopy& copy, std::uint32_t& top, std::uint32_t& bottom);
    bool GpuCursorSnapshot(GpuCursor& cursor);
    // Asks the guest to adopt this resolution (window resized).
    void RequestDisplaySize(std::uint32_t width, std::uint32_t height);

    // Host input (thread-safe).
    void KeyboardScanCode(std::uint8_t code);
    // Guest keyboard LEDs (bit 1 Num Lock, bit 2 Caps Lock), -1 if unknown.
    int KeyboardLeds() const;
    void MouseMotion(int dx, int dy, int wheel, std::uint8_t buttons);
    // Absolute pointer: x and y in 0..VirtioTablet::kAbsMax over the screen.
    bool TabletReady() const;
    void TabletMove(std::int32_t x, std::int32_t y);
    void TabletButton(VirtioTablet::Button button, bool pressed);
    void TabletWheel(std::int32_t steps, bool horizontal);
    bool SerialInput(std::uint8_t value);

    // Sound card (thread-safe). Muting keeps the guest playing, silently.
    bool HasAudio() const noexcept { return sound_ != nullptr; }
    void SetAudioMuted(bool muted);

    std::string Statistics() const;

    // Guest agent (clipboard, file transfer), or nullptr without a display.
    GuestAgent* Agent() const noexcept { return agent_.get(); }

private:
    struct VcpuContext {
        Vm* vm = nullptr;
        std::uint32_t index = 0;
        WHV_EMULATOR_HANDLE emulator = nullptr;
    };

    void BuildDevices();
    void LoadBootImage();
    void VcpuThread(std::uint32_t index);
    void HandleIoExit(VcpuContext& context, const WHV_RUN_VP_EXIT_CONTEXT& exit);
    void HandleMmioExit(VcpuContext& context, const WHV_RUN_VP_EXIT_CONTEXT& exit);
    void HandleHaltExit(std::uint32_t index);
    void RequestStop(VmExit reason);
    void CancelAll();

    std::uint32_t PortRead(std::uint16_t port, std::uint8_t size);
    void PortWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value);
    void MemoryAccess(std::uint64_t gpa, std::uint8_t* data, std::uint8_t size, bool write);

    static HRESULT CALLBACK EmulatorIo(void* context, WHV_EMULATOR_IO_ACCESS_INFO* access);
    static HRESULT CALLBACK EmulatorMemory(void* context, WHV_EMULATOR_MEMORY_ACCESS_INFO* access);
    static HRESULT CALLBACK EmulatorGetRegisters(void* context, const WHV_REGISTER_NAME* names,
                                                 UINT32 count, WHV_REGISTER_VALUE* values);
    static HRESULT CALLBACK EmulatorSetRegisters(void* context, const WHV_REGISTER_NAME* names,
                                                 UINT32 count, const WHV_REGISTER_VALUE* values);
    static HRESULT CALLBACK EmulatorTranslate(void* context, WHV_GUEST_VIRTUAL_ADDRESS gva,
                                              WHV_TRANSLATE_GVA_FLAGS flags,
                                              WHV_TRANSLATE_GVA_RESULT_CODE* result,
                                              WHV_GUEST_PHYSICAL_ADDRESS* gpa);

    VmConfig config_;
    SerialOutput serialOutput_;
    std::string bootDescription_;

    std::unique_ptr<virtualiseur::whp::Partition> partition_;
    std::unique_ptr<GuestMemory> memory_;
    std::optional<virtualiseur::whp::VirtualDisk> disk_;
    std::unique_ptr<virtualiseur::whp::Iso9660Image> iso_;
    std::optional<FramebufferInfo> framebuffer_;

    mutable DeviceLock deviceLock_;
    IoBus ioBus_;
    MmioBus mmioBus_;
    std::unique_ptr<IoApic> ioApic_;
    std::unique_ptr<Pic8259Pair> pic_;
    std::unique_ptr<Pit8254> pit_;
    std::unique_ptr<CmosRtc> rtc_;
    std::unique_ptr<Uart16550> uart_;
    std::unique_ptr<Ps2Controller> ps2_;
    std::unique_ptr<AcpiPm> acpi_;
    std::unique_ptr<PciBus> pciBus_;
    std::vector<std::unique_ptr<PciFunction>> pciFunctions_;
    IdeController* ide_ = nullptr;
    VirtioBlk* virtioBlk_ = nullptr;
    VirtioNet* virtioNet_ = nullptr;
    VirtioGpu* virtioGpu_ = nullptr;
    VirtioTablet* tablet_ = nullptr;
    VirtioSound* sound_ = nullptr;
    std::unique_ptr<GuestAgent> agent_;
    std::unique_ptr<VgaRegisters> vga_;
    std::unique_ptr<PostPort> post_;
    std::unique_ptr<NetworkBackend> network_;

    std::vector<std::thread> vcpus_;
    std::atomic<VmState> state_ = VmState::Starting;
    std::atomic_bool stopRequested_ = false;
    std::atomic<VmExit> exitReason_ = VmExit::PowerOff;
    std::mutex pauseMutex_;
    std::condition_variable pauseCondition_;
    bool paused_ = false;

    std::atomic_uint64_t exits_ = 0;
    std::atomic_uint64_t ioExits_ = 0;
    std::atomic_uint64_t mmioExits_ = 0;
    std::atomic_uint64_t unhandledPorts_ = 0;
    std::chrono::steady_clock::time_point haltSince_{};
};

} // namespace vmm
