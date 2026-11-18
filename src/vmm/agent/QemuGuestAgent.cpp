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

// Text as a JSON string literal.
std::string JsonString(const std::string& text)
{
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out + "\"";
}

// "out-data"/"err-data" of guest-exec-status are base64.
std::string Base64Decode(const std::string& text)
{
    std::string out;
    unsigned buffer = 0;
    int bits = 0;
    for (const char c : text) {
        int value = -1;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '+') value = 62;
        else if (c == '/') value = 63;
        else continue;
        buffer = (buffer << 6) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xFF);
        }
    }
    return out;
}

// "key": "value" (string) from a JSON reply, empty if absent.
std::string StringField(const std::string& reply, const char* key)
{
    const std::regex pattern(std::string("\"") + key + R"re("\s*:\s*"([^"]*)")re");
    std::smatch match;
    return std::regex_search(reply, match, pattern) ? match[1].str() : std::string();
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

bool QemuGuestAgent::Execute(const std::string& script, int& exitCode, std::string& errors)
{
    if (!synchronized_) {
        synchronized_ = Synchronize();
        if (!synchronized_) return false;
    }
    const auto started = Call(R"({"execute":"guest-exec","arguments":{"path":"/bin/sh","arg":["-c",)" + JsonString(script) +
                                  R"(],"capture-output":true}})",
                              kCallTimeout);
    static const std::regex pidPattern(R"re("pid"\s*:\s*(\d+))re");
    std::smatch match;
    if (!started || !std::regex_search(*started, match, pidPattern)) {
        if (started) errors = ErrorText(*started); // guest-exec disabled in the agent's configuration, for instance.
        else synchronized_ = false;
        return false;
    }
    const auto pid = match[1].str();
    static const std::regex exitPattern(R"re("exitcode"\s*:\s*(-?\d+))re");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = Call(R"({"execute":"guest-exec-status","arguments":{"pid":)" + pid + "}}", kCallTimeout);
        if (!status) {
            synchronized_ = false;
            return false;
        }
        if (status->find("\"exited\": true") != std::string::npos || status->find("\"exited\":true") != std::string::npos) {
            exitCode = std::regex_search(*status, match, exitPattern) ? std::atoi(match[1].str().c_str()) : -1;
            errors = Base64Decode(StringField(*status, "err-data"));
            while (!errors.empty() && (errors.back() == '\n' || errors.back() == '\r')) errors.pop_back();
            return true;
        }
        {
            std::unique_lock lock(mutex_);
            if (wake_.wait_for(lock, std::chrono::milliseconds(100), [this] { return stopping_ || !open_; })) return false;
        }
    }
    errors = "délai dépassé";
    return false;
}

QemuGuestAgent::FreezeResult QemuGuestAgent::Freeze(std::chrono::milliseconds timeout)
{
    const std::lock_guard exchange(exchangeMutex_);
    FreezeResult result;
    if (!open_) {
        result.reason = "qemu-guest-agent absent de l'invité";
        return result;
    }
    {
        const std::lock_guard guard(mutex_);
        if (suspended_) {
            result.reason = "machine en pause";
            return result;
        }
    }
    if (!synchronized_) {
        synchronized_ = Synchronize();
        if (!synchronized_) {
            result.reason = "qemu-guest-agent ne répond pas";
            return result;
        }
    }
    // From here on the agent may freeze, even if its answer comes too late:
    // a thaw is owed in every case.
    result.sent = true;
    const auto reply = Call(R"({"execute":"guest-fsfreeze-freeze"})", timeout);
    if (!reply) {
        synchronized_ = false; // The late reply must not be taken for another one.
        result.reason = "gel sans réponse en " + std::to_string(timeout.count() / 1000) + " s";
        return result;
    }
    const auto error = ErrorText(*reply);
    if (!error.empty()) {
        result.reason = "gel refusé par l'invité : " + error;
        return result;
    }
    const auto count = ReturnInteger(*reply);
    result.frozen = count ? static_cast<int>(*count) : 0;
    if (result.frozen == 0) result.reason = "aucun système de fichiers gelé";
    return result;
}

void QemuGuestAgent::Thaw()
{
    const std::lock_guard exchange(exchangeMutex_);
    if (!synchronized_) {
        // The freeze did not answer in time: the agent is busy (or stuck)
        // with it. It runs commands in order, so the thaw queued behind the
        // freeze will run right after it; waiting here would only delay the
        // snapshot's reply. The next exchange resynchronizes.
        Write("{\"execute\":\"guest-fsfreeze-thaw\"}\n");
        VMM_INFO("Agent QEMU : dégel envoyé à la suite du gel en attente.");
        return;
    }
    const auto reply = Call(R"({"execute":"guest-fsfreeze-thaw"})", std::chrono::seconds(10));
    if (!reply) {
        synchronized_ = false;
        VMM_WARN("Agent QEMU : pas de réponse au dégel (guest-fsfreeze-thaw envoyé).");
        return;
    }
    const auto error = ErrorText(*reply);
    if (!error.empty()) VMM_DEBUG("Agent QEMU : dégel : %s", error.c_str());
    else if (const auto count = ReturnInteger(*reply)) VMM_DEBUG("Agent QEMU : %lld système(s) de fichiers dégelé(s).", *count);
}

void QemuGuestAgent::MountShares()
{
    // Guest tools installed (install.sh of the tools CD): their boot service
    // (systemd or OpenRC, not confined) mounts every share on /mnt/<tag>.
    // The agent leaves it to them: under SELinux (Fedora...) qemu-ga runs
    // confined and may not create mount points.
    {
        int exitCode = -1;
        std::string errors;
        if (Execute("test -x /usr/local/sbin/virtualiseur-mount-shares", exitCode, errors) && exitCode == 0) {
            VMM_INFO("Dossiers partagés : montés sur /mnt/<nom> par le service des outils invité.");
            return;
        }
    }
    // Without them: /mnt/<tag> as well, through guest-exec (may be refused
    // under SELinux: the guest tools then do it). Tags are restricted to [A-Za-z0-9._-]
    // (Virtio9p): safe in the command line; dots are escaped for grep.
    for (const auto& tag : shares_) {
        const auto directory = "/mnt/" + tag;
        std::string pattern;
        for (const char c : tag) {
            if (c == '.') pattern += "\\."; // grep: a literal dot (JsonString escapes the backslash).
            else pattern += c;
        }
        // Already mounted anywhere (by the guest tools service, fstab...): nothing to do.
        const auto script = "if grep -q '^" + pattern + " [^ ]* 9p ' /proc/mounts; then exit 3; fi; d=" + directory +
                            "; mkdir -p \"$d\" && mount -t 9p -o trans=virtio,version=9p2000.L,msize=524288 " + tag + " \"$d\"";
        int exitCode = -1;
        std::string errors;
        if (!Execute(script, exitCode, errors)) {
            if (!errors.empty()) {
                VMM_WARN("Dossier partagé « %s » non monté dans l'invité : l'agent refuse guest-exec (%s).", tag.c_str(),
                         errors.c_str());
            } else {
                mountPending_ = true; // No answer: try again at the next check.
                VMM_DEBUG("Agent QEMU : montage de « %s » reporté.", tag.c_str());
            }
            return;
        }
        if (exitCode == 0) {
            VMM_INFO("Dossier partagé « %s » monté dans l'invité sur %s.", tag.c_str(), directory.c_str());
        } else if (exitCode == 3) {
            VMM_DEBUG("Dossier partagé « %s » déjà monté dans l'invité.", tag.c_str());
        } else {
            VMM_WARN("Dossier partagé « %s » non monté dans l'invité (code %d) : %s. Les outils invité (Périphériques → Insérer le CD des outils invité) le monteront au démarrage.", tag.c_str(), exitCode,
                     errors.empty() ? "pas de message" : errors.c_str());
        }
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
            mountPending_ = open_ && !shares_.empty();
            if (open_) reason = "connexion de l'agent";
        }
        if (!pendingReason_.empty()) reason = std::exchange(pendingReason_, {});
        if (!open_ || suspended_) continue;
        lock.unlock();
        {
            const std::lock_guard exchange(exchangeMutex_);
            CheckTime(reason);
            if (std::exchange(mountPending_, false)) MountShares(); // Sets it again if the agent did not answer.
        }
        lock.lock();
    }
}

} // namespace vmm
