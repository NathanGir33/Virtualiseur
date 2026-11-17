#pragma once

#include "Virgl.h"
#include "VirtioModern.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace vmm {

// Host copy of the guest screen, filled by VirtioGpu::CopyScanout.
struct ScanoutCopy {
    std::vector<std::uint8_t> pixels; // BGRX, stride = width * 4.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Guest cursor plane (hardware cursor) as set by the guest driver.
struct GpuCursor {
    bool visible = false;
    std::int32_t x = 0; // Top-left corner on the guest screen.
    std::int32_t y = 0;
    std::uint32_t hotX = 0;
    std::uint32_t hotY = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels; // Premultiplied BGRA.
    std::uint64_t shapeSerial = 0;
    std::uint64_t moveSerial = 0;
};

// virtio-gpu 2D display adapter (no 3D). The guest renders into resources
// in its own RAM and transfers the damaged rectangles to host copies; the
// host shows the scanout resource and draws the cursor plane itself, so
// moving the pointer never forces the guest to redraw the screen.
//
// The PCI function has the VGA class and a memory BAR over the boot
// framebuffer: Linux then hands the screen over from simpledrm/simplefb to
// virtio-gpu, as with a real graphics card.
//
// With `enable3d` and virglrenderer available, the device also offers
// VIRTIO_GPU_F_VIRGL: the guest Mesa "virgl" driver then renders with the
// host GPU through OpenGL. Every command then runs on the GPU thread, and
// fenced commands complete when the host GPU has finished them.
class VirtioGpu final : public VirtioModernDevice {
public:
    VirtioGpu(const GuestMemory& memory, InterruptSink& sink, DeviceLock& lock, IoBus& ioBus, std::uint16_t ioBase,
              std::uint32_t gsi, std::uint32_t width, std::uint32_t height, std::uint64_t framebufferBase,
              std::uint64_t framebufferBytes, bool enable3d);
    ~VirtioGpu() override;

    bool Accelerated() const noexcept { return virgl_ != nullptr; }

    // UI side (any thread, no device lock). Returns false while the guest
    // driver shows nothing (the boot framebuffer is then the screen). Copies
    // the rows changed since the previous call into `copy` and adds them to
    // `rows`.
    bool CopyScanout(ScanoutCopy& copy, DirtyRows& rows);
    // Copies the cursor state; the image is copied only when its serial
    // differs from `cursor.shapeSerial`. Returns true if anything changed.
    bool CursorSnapshot(GpuCursor& cursor);

    // Device lock held: asks the guest to switch to a new resolution.
    void RequestDisplaySize(std::uint32_t width, std::uint32_t height);

    std::uint64_t Flushes() const noexcept { return flushes_.load(); }
    std::uint64_t Submits() const noexcept { return submits_.load(); }

protected:
    std::uint64_t DeviceFeatures() const override { return virgl_ ? 1ull : 0; } // VIRTIO_GPU_F_VIRGL.
    std::uint8_t ReadDeviceConfig(std::uint32_t offset) override;
    void WriteDeviceConfig(std::uint32_t offset, std::uint8_t value) override;
    void QueueNotify(std::uint16_t queue) override;
    void DeviceReset() override;

private:
    struct BackingEntry {
        std::uint8_t* host;
        std::uint64_t offset; // Offset of this entry in the backing store.
        std::uint32_t length;
    };
    struct Resource {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        bool swapRedBlue = false;
        std::vector<std::uint8_t> pixels; // Host copy, BGRA, stride = width * 4.
        std::vector<BackingEntry> backing;
        std::uint64_t backingBytes = 0;
    };
    struct Rect {
        std::uint32_t x, y, width, height;
    };

    void ProcessControlQueue();
    void ProcessCursorQueue();
    // Executes one control command; returns the response length written in `response`.
    std::size_t Execute(const std::vector<std::uint8_t>& request, std::vector<std::uint8_t>& response);
    std::uint32_t CreateResource(const std::vector<std::uint8_t>& request);
    std::uint32_t AttachBacking(const std::vector<std::uint8_t>& request);
    std::uint32_t TransferToHost(const std::vector<std::uint8_t>& request);
    std::uint32_t SetScanout(const std::vector<std::uint8_t>& request);
    std::uint32_t Flush(const std::vector<std::uint8_t>& request);
    void UpdateCursor(const std::vector<std::uint8_t>& request, bool move);
    bool CopyFromBacking(const Resource& resource, std::uint64_t offset, std::uint8_t* destination,
                         std::size_t length) const;
    std::shared_ptr<Resource> Find(std::uint32_t id) const;

    // --- 3D (virgl) mode: everything below runs on the GPU thread. ---
    struct Command {
        std::uint16_t queue = 0;
        std::uint16_t head = 0;
        std::vector<VirtQueue::Buffer> writable;
        std::vector<std::uint8_t> request;
        std::vector<std::uint8_t> response;
        std::uint64_t generation = 0;
    };
    struct VirglResource {
        std::vector<VirglIovec> iov; // Owned here, referenced by virglrenderer while attached.
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };
    void PostCommands(std::uint16_t queue);
    void Execute3d(const std::shared_ptr<Command>& command);
    std::uint32_t Execute3dControl(Command& command);
    void Cursor3d(const Command& command);
    void Complete(const Command& command);
    void OnFence(std::uint32_t fence);
    void Readback(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height);
    void Reset3d();
    bool BuildBacking(const std::vector<std::uint8_t>& request, std::vector<BackingEntry>& backing,
                      std::uint64_t& total) const;

    DeviceLock& lock_;
    std::unique_ptr<VirglRenderer> virgl_;
    std::uint32_t numCapsets_ = 0;
    std::uint32_t capsetVersion_[2] = {};
    std::uint32_t capsetSize_[2] = {};
    std::atomic_uint64_t generation_ = 0;
    std::map<std::uint32_t, VirglResource> virglResources_;
    std::deque<std::shared_ptr<Command>> fenced_;
    std::uint32_t scanoutResourceId_ = 0;
    bool scanoutSwapRedBlue_ = false;

    std::atomic_uint32_t displayWidth_;
    std::atomic_uint32_t displayHeight_;
    std::uint32_t eventsRead_ = 0;
    std::map<std::uint32_t, std::shared_ptr<Resource>> resources_;
    std::uint64_t resourceBytes_ = 0;

    // Shared with the UI thread.
    std::mutex displayMutex_;
    std::shared_ptr<Resource> scanout_;
    Rect scanoutRect_{};
    bool scanoutActive_ = false;
    bool fullCopy_ = true;
    DirtyRows dirty_; // Scanout rows flushed since the last copy.
    GpuCursor cursor_;

    std::atomic_uint64_t flushes_ = 0;
    std::atomic_uint64_t submits_ = 0;
};

} // namespace vmm
