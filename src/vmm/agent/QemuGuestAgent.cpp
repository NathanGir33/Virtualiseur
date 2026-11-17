#include "QemuGuestAgent.h"

#include "../core/Log.h"
#include "../devices/VirtioSerial.h"

#include <Windows.h>

#include <cstdlib>
#include <regex>
#include <utility>

namespace vmm {
namespace {

constexpr std::chrono::seconds kPeriod{60};
constexpr std::chrono::milliseconds kCallTimeout{3000};
constexpr std::size_t kMaxLine = 64 * 1024;

std::int64_t HostNowNs()
{
    // system_clock counts from the Unix epoch (C++20), as guest-get-time does.
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// {"return": <integer>} -> the integer.
std::optional<std::int64_t> ReturnInteger(const std::string& reply)
{
    static const std::regex pattern(R"re("return"\s*:\s*(-?\d+))re");
    std::smatch match;
    if (!std::regex_search(reply, match, pattern)) return std::nullopt;
    return std::strtoll(match[1].str().c_str(), nullptr, 10);
}

// {"error": {"class": ..., "desc": "<text>"}} -> text (empty if not an error).
std::string ErrorText(const std::string& reply)
{
    if (reply.find("\"error\"") == std::string::npos) return {};
    static const std::regex pattern(R"re("desc"\s*:\s*"((?:[^"\\]|\\.)*)")re");
    std::smatch match;
    return std::regex_search(reply, match, pattern) ? match[1].str() : reply;
}

} // namespace

QemuGuestAgent::QemuGuestAgent()
{
    worker_ = std::thread([this] { Worker(); });
}

QemuGuestAgent::~QemuGuestAgent()
{
    {
        const std::lock_guard guard(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void QemuGuestAgent::Attach(VirtioSerial* serial) noexcept
{
    serial_ = serial;
    lock_ = serial != nullptr ? &serial->Lock() : nullptr;
}

void QemuGuestAgent::OnBytes(std::span<const std::uint8_t> data)
{
    std::deque<std::string> lines;
    for (const auto byte : data) {
        if (byte == 0xFF) {
            partial_.clear(); // Delimiter of a guest-sync-delimited reply.
        } else if (byte == '\n') {
            if (partial_.find_first_not_of(" \r\t") != std::string::npos) lines.push_back(std::move(partial_));
            partial_.clear();
        } else if (partial_.size() < kMaxLine) {
            partial_.push_back(static_cast<char>(byte));
        }
    }
    if (lines.empty()) return;
    {
        const std::lock_guard guard(mutex_);
        for (auto& line : lines) replies_.push_back(std::move(line));
    }
    wake_.notify_all();
}

void QemuGuestAgent::OnOpen(bool open)
{
    open_ = open;
    partial_.clear();
    {
        const std::lock_guard guard(mutex_);
        replies_.clear();
        connectedChanged_ = true;
    }
    wake_.notify_all();
    if (open) VMM_INFO("Agent invité QEMU (qemu-guest-agent) connecté.");
}

void QemuGuestAgent::SetSuspended(bool suspended)
{
    {
        const std::lock_guard guard(mutex_);
        suspended_ = suspended;
    }
    wake_.notify_all();
}

void QemuGuestAgent::RequestTimeSync(const char* reason)
{
    {
        const std::lock_guard guard(mutex_);
        pendingReason_ = reason;
    }
    wake_.notify_all();
}

std::optional<std::int64_t> QemuGuestAgent::LastOffsetMs() const
{
    const std::lock_guard guard(mutex_);
    return lastOffsetMs_;
}

void QemuGuestAgent::Write(const std::string& text)
{
    if (serial_ == nullptr) return;
    const DeviceGuard guard(*lock_);
    if (!open_) return;
    serial_->Send(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::optional<std::string> QemuGuestAgent::Call(const std::string& command, std::chrono::milliseconds timeout)
{
    {
        const std::lock_guard guard(mutex_);
        replies_.clear();
    }
    Write(command + "\n");
    std::unique_lock lock(mutex_);
    if (!wake_.wait_for(lock, timeout, [this] { return stopping_ || !open_ || !replies_.empty(); })) return std::nullopt;
    if (stopping_ || replies_.empty()) return std::nullopt;
    auto reply = std::move(replies_.front());
    replies_.pop_front();
    return reply;
}

bool QemuGuestAgent::Synchronize()
{
    // A 0xFF byte makes the agent drop any partial input; guest-sync-delimited
    // then tells our reply apart from stale ones (0xFF before the reply too).
    const auto id = static_cast<std::int64_t>(GetTickCount64() & 0x3FFFFFFF) + static_cast<std::int64_t>(++nextSyncId_);
    {
        const std::lock_guard guard(mutex_);
        replies_.clear();
    }
    Write("\xFF");
    Write("{\"execute\":\"guest-sync-delimited\",\"arguments\":{\"id\":" + std::to_string(id) + "}}\n");
    const auto deadline = std::chrono::steady_clock::now() + kCallTimeout;
    std::unique_lock lock(mutex_);
    for (;;) {
        if (!wake_.wait_until(lock, deadline, [this] { return stopping_ || !open_ || !replies_.empty(); })) return false;
        if (stopping_ || !open_) return false;
        const auto reply = std::move(replies_.front());
        replies_.pop_front();
        if (ReturnInteger(reply) == id) return true; // Anything else is stale or a parse error of the 0xFF.
    }
}

void QemuGuestAgent::CheckTime(const std::string& reason)
{
    if (!synchronized_) {
        synchronized_ = Synchronize();
        if (!synchronized_) {
            VMM_DEBUG("Agent QEMU : pas de réponse à guest-sync-delimited.");
            return;
        }
    }
    const auto before = HostNowNs();
    const auto reply = Call(R"({"execute":"guest-get-time"})", kCallTimeout);
    const auto after = HostNowNs();
    const auto guest = reply ? ReturnInteger(*reply) : std::nullopt;
    if (!guest) {
        synchronized_ = false; // Resynchronize before the next command.
        if (reply) VMM_WARN("Agent QEMU : réponse inattendue à guest-get-time : %s", reply->c_str());
        return;
    }
    const auto offsetMs = (*guest - (before + (after - before) / 2)) / 1'000'000;
    {
        const std::lock_guard guard(mutex_);
        lastOffsetMs_ = offsetMs;
    }
    if (std::llabs(offsetMs) < kThreshold.count()) {
        if (!reason.empty()) VMM_INFO("Heure de l'invité (%s) : écart de %+.3f s, rien à corriger.", reason.c_str(), offsetMs / 1000.0);
        return;
    }

    // guest-set-time also copies the time to the hardware clock with
    // hwclock, which may be missing: the system clock is set even then.
    const auto set = Call(R"({"execute":"guest-set-time","arguments":{"time":)" + std::to_string(HostNowNs()) + "}}",
                          kCallTimeout);
    const auto error = set ? ErrorText(*set) : std::string("pas de réponse");
    const auto check = Call(R"({"execute":"guest-get-time"})", kCallTimeout);
    const auto now = HostNowNs();
    const auto corrected = check ? ReturnInteger(*check) : std::nullopt;
    const auto residualMs = corrected ? (*corrected - now) / 1'000'000 : offsetMs;
    {
        const std::lock_guard guard(mutex_);
        lastOffsetMs_ = residualMs;
    }
    const char* why = reason.empty() ? "contrôle périodique" : reason.c_str();
    if (corrected && std::llabs(residualMs) < kThreshold.count()) {
        VMM_INFO("Heure de l'invité corrigée (%s) : elle avait %+.3f s d'écart avec l'hôte.", why, offsetMs / 1000.0);
        if (!error.empty()) VMM_DEBUG("Agent QEMU : guest-set-time : %s", error.c_str());
    } else {
        VMM_WARN("Heure de l'invité : écart de %+.3f s (%s) non corrigé (%s).", offsetMs / 1000.0, why,
                 error.empty() ? "l'invité a refusé" : error.c_str());
    }
}

void QemuGuestAgent::Worker()
{
    SetThreadDescription(GetCurrentThread(), L"Agent QEMU");
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait_for(lock, kPeriod, [this] { return stopping_ || connectedChanged_ || !pendingReason_.empty(); });
        if (stopping_) return;
        std::string reason;
        if (connectedChanged_) {
            connectedChanged_ = false;
            synchronized_ = false;
            if (open_) reason = "connexion de l'agent";
        }
        if (!pendingReason_.empty()) reason = std::exchange(pendingReason_, {});
        if (!open_ || suspended_) continue;
        lock.unlock();
        CheckTime(reason);
        lock.lock();
    }
}

} // namespace vmm
