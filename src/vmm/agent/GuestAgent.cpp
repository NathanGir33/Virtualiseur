#include "GuestAgent.h"

#include "../core/Log.h"
#include "../devices/VirtioSerial.h"

#include <Windows.h>

#include <cstring>
#include <fstream>

namespace vmm {
namespace {

constexpr std::uint32_t kProtocol = 1;
constexpr std::uint32_t kClientPort = 1;     // VDP_CLIENT_PORT.
constexpr std::size_t kChunkData = 2048;     // VD_AGENT_MAX_DATA_SIZE.
constexpr std::size_t kMessageHeader = 20;   // VDAgentMessage without data.
constexpr std::uint32_t kMaxMessage = 64u * 1024 * 1024;
constexpr std::size_t kFileBlock = 64 * 1024;
constexpr std::size_t kMaxBacklog = 4u * 1024 * 1024;

// Capabilities announced by the host (as a SPICE client would).
constexpr std::uint32_t kCapClipboardByDemand = 5;

// VD_AGENT_FILE_XFER_STATUS_* results.
constexpr std::uint32_t kXferCanSendData = 0;
constexpr std::uint32_t kXferCancelled = 1;
constexpr std::uint32_t kXferError = 2;
constexpr std::uint32_t kXferSuccess = 3;
constexpr std::uint32_t kXferNotEnoughSpace = 4;
constexpr std::uint32_t kXferSessionLocked = 5;
constexpr std::uint32_t kXferAgentNotConnected = 6;
constexpr std::uint32_t kXferDisabled = 7;

std::uint32_t Load32(const std::uint8_t* data)
{
    std::uint32_t value = 0;
    std::memcpy(&value, data, 4);
    return value;
}

void Append32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + 4);
}

void Append64(std::vector<std::uint8_t>& out, std::uint64_t value)
{
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + 8);
}

std::string Utf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// GKeyFile value escaping, as spice-vdagent parses the transfer header.
std::string EscapeKeyFileValue(const std::string& value)
{
    std::string out;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char c = value[index];
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c == ' ' && index == 0) out += "\\s";
        else out += c;
    }
    return out;
}

const char* XferResult(std::uint32_t result)
{
    switch (result) {
    case kXferCancelled: return "annulé dans le système invité";
    case kXferError: return "erreur dans le système invité";
    case kXferNotEnoughSpace: return "espace disque insuffisant dans le système invité";
    case kXferSessionLocked: return "session invitée verrouillée";
    case kXferAgentNotConnected: return "aucune session graphique avec spice-vdagent dans le système invité";
    case kXferDisabled: return "transfert de fichiers désactivé dans le système invité";
    default: return "sans réponse du système invité";
    }
}

} // namespace

GuestAgent::GuestAgent(DeviceLock& lock) : lock_(lock)
{
    worker_ = std::thread([this] { Worker(); });
}

GuestAgent::~GuestAgent()
{
    {
        const std::lock_guard guard(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

// --- Device side ----------------------------------------------------------------------------------

void GuestAgent::OnOpen(bool open)
{
    open_ = open;
    stream_.clear();
    message_.clear();
    if (!open) {
        ready_ = false;
        {
            const std::lock_guard guard(mutex_);
            guestCapabilities_.clear();
        }
        wake_.notify_all();
        return;
    }
    VMM_INFO("Agent invité (spice-vdagent) connecté.");
    AnnounceCapabilities(true);
}

void GuestAgent::OnBytes(std::span<const std::uint8_t> data)
{
    // VDIChunkHeader {port, size} + data; messages may span several chunks.
    stream_.insert(stream_.end(), data.begin(), data.end());
    std::size_t offset = 0;
    while (stream_.size() - offset >= 8) {
        const auto size = Load32(stream_.data() + offset + 4);
        if (size > kChunkData * 64) { // Corrupt stream: resynchronise on the next open.
            stream_.clear();
            message_.clear();
            return;
        }
        if (stream_.size() - offset < 8 + size) break;
        message_.insert(message_.end(), stream_.begin() + static_cast<std::ptrdiff_t>(offset + 8),
                        stream_.begin() + static_cast<std::ptrdiff_t>(offset + 8 + size));
        offset += 8 + size;

        while (message_.size() >= kMessageHeader) {
            const auto length = Load32(message_.data() + 16);
            if (length > kMaxMessage) {
                message_.clear();
                break;
            }
            if (message_.size() < kMessageHeader + length) break;
            Message parsed;
            parsed.type = Load32(message_.data() + 4);
            parsed.data.assign(message_.begin() + kMessageHeader,
                               message_.begin() + static_cast<std::ptrdiff_t>(kMessageHeader + length));
            message_.erase(message_.begin(), message_.begin() + static_cast<std::ptrdiff_t>(kMessageHeader + length));
            Dispatch(std::move(parsed));
        }
    }
    stream_.erase(stream_.begin(), stream_.begin() + static_cast<std::ptrdiff_t>(offset));
}

void GuestAgent::Dispatch(Message message)
{
    switch (message.type) {
    case kAnnounceCapabilities: {
        if (message.data.size() < 4) return;
        const bool request = Load32(message.data.data()) != 0;
        {
            const std::lock_guard guard(mutex_);
            guestCapabilities_.clear();
            for (std::size_t offset = 4; offset + 4 <= message.data.size(); offset += 4) {
                guestCapabilities_.push_back(Load32(message.data.data() + offset));
            }
            inbox_.push_back(std::move(message)); // The UI announces the host clipboard.
        }
        if (request) AnnounceCapabilities(false);
        ready_ = true;
        return;
    }
    case kFileXferStatus: {
        if (message.data.size() < 8) return;
        {
            const std::lock_guard guard(mutex_);
            status_[Load32(message.data.data())] = Load32(message.data.data() + 4);
        }
        wake_.notify_all();
        return;
    }
    case kClipboard: case kClipboardGrab: case kClipboardRequest: case kClipboardRelease: {
        const std::lock_guard guard(mutex_);
        if (inbox_.size() < 256) inbox_.push_back(std::move(message));
        return;
    }
    default:
        return;
    }
}

void GuestAgent::AnnounceCapabilities(bool request)
{
    std::vector<std::uint8_t> data;
    Append32(data, request ? 1 : 0);
    Append32(data, 1u << kCapClipboardByDemand);
    Send(kAnnounceCapabilities, data);
}

// --- UI side --------------------------------------------------------------------------------------

bool GuestAgent::GuestHasCapability(std::uint32_t bit) const
{
    const std::lock_guard guard(mutex_);
    const auto word = bit / 32;
    return word < guestCapabilities_.size() && (guestCapabilities_[word] & (1u << (bit % 32))) != 0;
}

std::vector<GuestAgent::Message> GuestAgent::Take()
{
    const std::lock_guard guard(mutex_);
    std::vector<Message> messages(std::make_move_iterator(inbox_.begin()), std::make_move_iterator(inbox_.end()));
    inbox_.clear();
    return messages;
}

void GuestAgent::Send(std::uint32_t type, std::span<const std::uint8_t> data)
{
    std::vector<std::uint8_t> message;
    message.reserve(kMessageHeader + data.size());
    Append32(message, kProtocol);
    Append32(message, type);
    Append64(message, 0); // opaque
    Append32(message, static_cast<std::uint32_t>(data.size()));
    message.insert(message.end(), data.begin(), data.end());

    std::vector<std::uint8_t> chunk;
    const DeviceGuard guard(lock_);
    if (serial_ == nullptr || !open_) return;
    for (std::size_t offset = 0; offset < message.size(); offset += kChunkData) {
        const auto size = (std::min)(kChunkData, message.size() - offset);
        chunk.clear();
        Append32(chunk, kClientPort);
        Append32(chunk, static_cast<std::uint32_t>(size));
        chunk.insert(chunk.end(), message.begin() + static_cast<std::ptrdiff_t>(offset),
                     message.begin() + static_cast<std::ptrdiff_t>(offset + size));
        serial_->Send(chunk);
    }
}

std::size_t GuestAgent::Backlog() const
{
    const DeviceGuard guard(lock_);
    return serial_ != nullptr ? serial_->Backlog() : 0;
}

void GuestAgent::Notice(const std::string& text)
{
    VMM_WARN("%s", text.c_str());
    const std::lock_guard guard(mutex_);
    inbox_.push_back({kHostNotice, std::vector<std::uint8_t>(text.begin(), text.end())});
}

// --- File transfer --------------------------------------------------------------------------------

void GuestAgent::SendFiles(std::vector<std::filesystem::path> files)
{
    {
        const std::lock_guard guard(mutex_);
        for (auto& file : files) files_.push_back(std::move(file));
    }
    wake_.notify_all();
}

void GuestAgent::Worker()
{
    SetThreadDescription(GetCurrentThread(), L"Agent invité");
    for (;;) {
        std::filesystem::path file;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !files_.empty(); });
            if (stopping_) return;
            file = std::move(files_.front());
            files_.pop_front();
        }
        if (!TransferFile(file)) {
            // The guest cannot receive: drop the rest of the batch.
            const std::lock_guard guard(mutex_);
            files_.clear();
        }
    }
}

int GuestAgent::WaitStatus(std::uint32_t id, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    const bool got = wake_.wait_for(lock, timeout, [this, id] {
        return stopping_ || !open_ || status_.contains(id);
    });
    if (!got || stopping_ || !status_.contains(id)) return -1;
    const auto result = status_[id];
    status_.erase(id);
    return static_cast<int>(result);
}

bool GuestAgent::TransferFile(const std::filesystem::path& file)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) {
        Notice("« " + Utf8(file.filename()) + " » n'est pas un fichier : seuls les fichiers peuvent être envoyés.");
        return true;
    }
    if (!ready_) {
        Notice("Envoi de fichiers impossible : l'agent invité n'est pas actif. Installe spice-vdagent dans la VM "
               "(sudo apt install spice-vdagent), puis redémarre la session.");
        return false;
    }
    const auto size = std::filesystem::file_size(file, error);
    std::ifstream input(file, std::ios::binary);
    if (error || !input) {
        Notice("Impossible de lire « " + Utf8(file.filename()) + " ».");
        return true;
    }

    std::uint32_t id = 0;
    {
        const std::lock_guard guard(mutex_);
        id = nextId_++;
    }
    const std::string header = "[vdagent-file-xfer]\nname=" + EscapeKeyFileValue(Utf8(file.filename())) +
                               "\nsize=" + std::to_string(size) + "\n";
    std::vector<std::uint8_t> start;
    Append32(start, id);
    start.insert(start.end(), header.begin(), header.end());
    start.push_back(0);
    Send(kFileXferStart, start);

    const int answer = WaitStatus(id, std::chrono::seconds(15));
    if (answer != static_cast<int>(kXferCanSendData)) {
        Notice("Envoi de « " + Utf8(file.filename()) + " » refusé : " +
               XferResult(answer < 0 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(answer)) + ".");
        return answer >= 0 && answer != static_cast<int>(kXferAgentNotConnected) &&
               answer != static_cast<int>(kXferDisabled);
    }

    std::vector<char> block(kFileBlock);
    std::vector<std::uint8_t> message;
    std::uint64_t sent = 0;
    while (sent < size) {
        {
            const std::lock_guard guard(mutex_);
            if (stopping_ || !open_) return false;
            if (const auto found = status_.find(id); found != status_.end()) {
                Notice("Envoi de « " + Utf8(file.filename()) + " » interrompu : " + XferResult(found->second) + ".");
                status_.erase(found);
                return true;
            }
        }
        // Flow control: the guest drains the port at its own pace.
        while (Backlog() > kMaxBacklog) {
            if (stopping_ || !open_) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        input.read(block.data(), static_cast<std::streamsize>(block.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count == 0) break;
        message.clear();
        Append32(message, id);
        Append64(message, count);
        message.insert(message.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(count));
        Send(kFileXferData, message);
        sent += count;
    }
    const int result = WaitStatus(id, std::chrono::seconds(60));
    if (result == static_cast<int>(kXferSuccess)) {
        VMM_INFO("Fichier envoyé au système invité : %s (%llu octets).", Utf8(file.filename()).c_str(),
                 static_cast<unsigned long long>(size));
    } else {
        Notice("Envoi de « " + Utf8(file.filename()) + " » : " +
               XferResult(result < 0 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(result)) + ".");
    }
    return true;
}

} // namespace vmm
