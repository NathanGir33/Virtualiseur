#pragma once

#include "../core/Bus.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace vmm {

class VirtioSerial;

// Host end of the SPICE guest agent protocol (spice-vdagent, packaged by
// most distributions), spoken over the virtio-serial port
// "com.redhat.spice.0". It provides clipboard sharing and file transfer
// (files dropped on the window land in the guest's Downloads folder).
//
// Guest bytes arrive on a vCPU thread with the serial port's lock held;
// complete messages are queued for the UI thread, which polls them with
// Take().
class GuestAgent final {
public:
    static constexpr char kPortName[] = "com.redhat.spice.0";

    // VDAgentMessage types.
    enum Type : std::uint32_t {
        kClipboard = 4,
        kAnnounceCapabilities = 6,
        kClipboardGrab = 7,
        kClipboardRequest = 8,
        kClipboardRelease = 9,
        kFileXferStart = 10,
        kFileXferStatus = 11,
        kFileXferData = 12,
        kHostNotice = 0x10000, // Not a protocol message: text for the user.
    };
    // VD_AGENT_CLIPBOARD_* data types.
    enum ClipboardType : std::uint32_t { kNone = 0, kUtf8Text = 1, kImagePng = 2, kImageBmp = 3 };
    static constexpr std::uint32_t kCapGuestLineEndLf = 8;

    struct Message {
        std::uint32_t type = 0;
        std::vector<std::uint8_t> data;
    };

    GuestAgent();
    ~GuestAgent();
    GuestAgent(const GuestAgent&) = delete;
    GuestAgent& operator=(const GuestAgent&) = delete;

    // Before the VM starts.
    void Attach(VirtioSerial* serial) noexcept;

    // Device side (serial port lock held).
    void OnBytes(std::span<const std::uint8_t> data);
    void OnOpen(bool open);

    // UI side (any thread).
    bool Ready() const noexcept { return ready_.load(); }
    // The guest daemon (spice-vdagentd) has the port open.
    bool Connected() const noexcept { return open_.load(); }
    bool GuestHasCapability(std::uint32_t bit) const;
    std::vector<Message> Take();
    void Send(std::uint32_t type, std::span<const std::uint8_t> data);
    // Queues files for transfer to the guest (in order, on a worker thread).
    void SendFiles(std::vector<std::filesystem::path> files);

private:
    void Dispatch(Message message);
    void AnnounceCapabilities(bool request);
    void Notice(const std::string& text);
    void Worker();
    bool TransferFile(const std::filesystem::path& file);
    // Waits for a status of transfer `id`; returns -1 on timeout or stop.
    int WaitStatus(std::uint32_t id, std::chrono::milliseconds timeout);
    std::size_t Backlog() const;

    DeviceLock* lock_ = nullptr; // The serial port's lock.
    VirtioSerial* serial_ = nullptr;
    std::vector<std::uint8_t> stream_;  // Partial chunk data (device lock).
    std::vector<std::uint8_t> message_; // Partial message (device lock).
    std::atomic_bool open_ = false;
    std::atomic_bool ready_ = false;

    mutable std::mutex mutex_;
    std::vector<std::uint32_t> guestCapabilities_;
    std::deque<Message> inbox_;

    // File transfers.
    std::condition_variable wake_;
    std::deque<std::filesystem::path> files_;
    std::map<std::uint32_t, std::uint32_t> status_;
    std::uint32_t nextId_ = 1;
    bool stopping_ = false;
    std::thread worker_;
};

} // namespace vmm
