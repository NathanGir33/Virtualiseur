#pragma once

#include "../core/Bus.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace vmm {

class VirtioSerial;

// Host end of the QEMU guest agent (qemu-guest-agent, packaged by most
// distributions), a JSON command channel on the virtio-serial port
// "org.qemu.guest_agent.0". It keeps the guest wall clock on the host's:
// guest-get-time is compared with the host clock when the agent connects,
// when the VM resumes from a pause, when the host wakes up from sleep, and
// every minute; a gap of more than kThreshold (the boot clock comes from the
// CMOS RTC, accurate to the second only) is fixed with guest-set-time.
//
// Guest bytes arrive on a vCPU thread with the serial port's lock held;
// commands are sent by a worker thread that waits for their replies.
class QemuGuestAgent final {
public:
    static constexpr char kPortName[] = "org.qemu.guest_agent.0";
    static constexpr std::chrono::milliseconds kThreshold{500};

    QemuGuestAgent();
    ~QemuGuestAgent();
    QemuGuestAgent(const QemuGuestAgent&) = delete;
    QemuGuestAgent& operator=(const QemuGuestAgent&) = delete;

    // Before the VM starts.
    void Attach(VirtioSerial* serial) noexcept;

    // Device side (serial port lock held).
    void OnBytes(std::span<const std::uint8_t> data);
    void OnOpen(bool open);

    // Any thread.
    bool Connected() const noexcept { return open_.load(); }
    // While suspended (VM paused) the guest cannot answer: no command is sent.
    void SetSuspended(bool suspended);
    // Checks the guest clock soon; `reason` goes to the log.
    void RequestTimeSync(const char* reason);
    // Last measured offset (guest - host), in milliseconds, if any.
    std::optional<std::int64_t> LastOffsetMs() const;
    // Before the VM starts: shared folders (virtio-9p tags) the agent mounts
    // in the guest each time it connects (guest-exec): through the guest tools
    // service when installed, else directly; always on /mnt/<tag>.
    void SetShares(std::vector<std::string> tags) { shares_ = std::move(tags); }

    // Live snapshots. Freeze sends guest-fsfreeze-freeze and waits at most
    // `timeout`; `sent` tells whether a thaw is owed. Thaw sends
    // guest-fsfreeze-thaw (after a resynchronization, the freeze reply may be late).
    struct FreezeResult {
        bool sent = false;
        int frozen = 0;     // File systems frozen (0: not frozen).
        std::string reason; // Why not, in French, when frozen == 0.
    };
    FreezeResult Freeze(std::chrono::milliseconds timeout);
    void Thaw();

private:
    void Worker();
    // Sends one command and waits for its reply line (without the 0xFF
    // delimiter); nullopt on timeout, disconnection or stop.
    std::optional<std::string> Call(const std::string& command, std::chrono::milliseconds timeout);
    bool Synchronize();
    void CheckTime(const std::string& reason);
    void MountShares();
    // Runs /bin/sh -c `script` in the guest; false if the agent refused or timed out.
    bool Execute(const std::string& script, int& exitCode, std::string& errors);
    void Write(const std::string& text);

    DeviceLock* lock_ = nullptr; // The serial port's lock.
    VirtioSerial* serial_ = nullptr;
    std::string partial_;        // Incomplete reply line (device lock).
    std::atomic_bool open_ = false;

    // One command exchange at a time (worker checks, freeze, thaw).
    std::mutex exchangeMutex_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> replies_;
    std::string pendingReason_;  // Non-empty: a check was requested.
    bool connectedChanged_ = false;
    bool suspended_ = false;
    bool stopping_ = false;
    bool synchronized_ = false;  // A guest-sync handshake succeeded since the port opened.
    bool mountPending_ = false;  // Shares to mount since the port opened.
    std::vector<std::string> shares_;
    std::uint64_t nextSyncId_ = 0;
    std::optional<std::int64_t> lastOffsetMs_;
    std::thread worker_;
};

} // namespace vmm
