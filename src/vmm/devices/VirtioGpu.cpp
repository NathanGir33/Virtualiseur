#include "VirtioGpu.h"

#include "../core/Log.h"

#include <algorithm>
#include <cstring>
#include <future>

namespace vmm {
namespace {

// Control commands.
constexpr std::uint32_t kCmdGetDisplayInfo = 0x0100;
constexpr std::uint32_t kCmdResourceCreate2d = 0x0101;
constexpr std::uint32_t kCmdResourceUnref = 0x0102;
constexpr std::uint32_t kCmdSetScanout = 0x0103;
constexpr std::uint32_t kCmdResourceFlush = 0x0104;
constexpr std::uint32_t kCmdTransferToHost2d = 0x0105;
constexpr std::uint32_t kCmdResourceAttachBacking = 0x0106;
constexpr std::uint32_t kCmdResourceDetachBacking = 0x0107;
constexpr std::uint32_t kCmdGetCapsetInfo = 0x0108;
constexpr std::uint32_t kCmdGetCapset = 0x0109;
// 3D commands.
constexpr std::uint32_t kCmdCtxCreate = 0x0200;
constexpr std::uint32_t kCmdCtxDestroy = 0x0201;
constexpr std::uint32_t kCmdCtxAttachResource = 0x0202;
constexpr std::uint32_t kCmdCtxDetachResource = 0x0203;
constexpr std::uint32_t kCmdResourceCreate3d = 0x0204;
constexpr std::uint32_t kCmdTransferToHost3d = 0x0205;
constexpr std::uint32_t kCmdTransferFromHost3d = 0x0206;
constexpr std::uint32_t kCmdSubmit3d = 0x0207;
// Cursor commands.
constexpr std::uint32_t kCmdUpdateCursor = 0x0300;
constexpr std::uint32_t kCmdMoveCursor = 0x0301;
// Responses.
constexpr std::uint32_t kRespOkNoData = 0x1100;
constexpr std::uint32_t kRespOkDisplayInfo = 0x1101;
constexpr std::uint32_t kRespOkCapsetInfo = 0x1102;
constexpr std::uint32_t kRespOkCapset = 0x1103;
constexpr std::uint32_t kRespErrUnspec = 0x1200;
constexpr std::uint32_t kRespErrOutOfMemory = 0x1201;
constexpr std::uint32_t kRespErrInvalidScanoutId = 0x1202;
constexpr std::uint32_t kRespErrInvalidResourceId = 0x1203;
constexpr std::uint32_t kRespErrInvalidParameter = 0x1205;

constexpr std::uint32_t kFlagFence = 1;
constexpr std::uint32_t kEventDisplay = 1;
constexpr std::size_t kHeaderSize = 24;
constexpr std::uint32_t kMaxScanouts = 16;
constexpr std::size_t kMaxRequest = 8u * 1024 * 1024;
constexpr std::uint32_t kMaxDimension = 16384;
constexpr std::uint64_t kMaxResourceBytes = 1024ull * 1024 * 1024;
constexpr std::uint32_t kMaxCursorDimension = 512;

// Formats (byte order in memory). B8G8R8* match the host BGRA surfaces.
constexpr std::uint32_t kFormatB8G8R8A8 = 1;
constexpr std::uint32_t kFormatB8G8R8X8 = 2;
constexpr std::uint32_t kFormatR8G8B8A8 = 67;
constexpr std::uint32_t kFormatR8G8B8X8 = 134;

std::uint32_t Load32(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    std::uint32_t value = 0;
    if (offset + 4 <= data.size()) std::memcpy(&value, data.data() + offset, 4);
    return value;
}

std::uint64_t Load64(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    std::uint64_t value = 0;
    if (offset + 8 <= data.size()) std::memcpy(&value, data.data() + offset, 8);
    return value;
}

void Store32(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value)
{
    if (data.size() < offset + 4) data.resize(offset + 4, 0);
    std::memcpy(data.data() + offset, &value, 4);
}

void SwapRedBlue(std::uint8_t* pixels, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index) std::swap(pixels[index * 4], pixels[index * 4 + 2]);
}

} // namespace

VirtioGpu::VirtioGpu(const GuestMemory& memory, const VirtioPlacement& placement,
                     std::uint32_t width, std::uint32_t height,
                     std::uint64_t framebufferBase, std::uint64_t framebufferBytes, bool enable3d)
    : VirtioModernDevice(16, 0x03, 0x00, memory, placement, 2),
      displayWidth_(width), displayHeight_(height)
{
    if (framebufferBytes != 0) {
        std::uint32_t barSize = 1u << 20;
        while (barSize < framebufferBytes && barSize < (1u << 30)) barSize <<= 1;
        SetMemoryBar(2, static_cast<std::uint32_t>(framebufferBase), barSize, true);
    }
    if (!enable3d) return;
    virgl_ = VirglRenderer::Create([this](std::uint32_t fence) { OnFence(fence); });
    if (!virgl_) return;
    // Capability sets: VIRGL (1) and VIRGL2 (2), queried on the GPU thread.
    std::promise<void> done;
    virgl_->Post([this, &done] {
        for (std::uint32_t set = 1; set <= 2; ++set) {
            virgl_->Calls().getCapSet(set, &capsetVersion_[set - 1], &capsetSize_[set - 1]);
        }
        done.set_value();
    });
    done.get_future().wait();
    numCapsets_ = capsetVersion_[1] != 0 ? 2 : capsetVersion_[0] != 0 ? 1 : 0;
    if (numCapsets_ == 0) {
        VMM_WARN("virglrenderer ne fournit aucun jeu de capacités: accélération 3D désactivée.");
        virgl_.reset();
        return;
    }
    VMM_INFO("virtio-gpu: accélération 3D virgl active (capsets VIRGL v%u, VIRGL2 v%u).",
             capsetVersion_[0], capsetVersion_[1]);
}

VirtioGpu::~VirtioGpu()
{
    // Drain and stop the GPU thread while the device is still whole.
    virgl_.reset();
}

// --- Device configuration ------------------------------------------------------------------------

std::uint8_t VirtioGpu::ReadDeviceConfig(std::uint32_t offset)
{
    // struct virtio_gpu_config: events_read, events_clear, num_scanouts, num_capsets.
    std::uint32_t value = 0;
    switch (offset / 4) {
    case 0: value = eventsRead_; break;
    case 2: value = 1; break;
    case 3: value = numCapsets_; break;
    default: break;
    }
    return static_cast<std::uint8_t>(value >> ((offset % 4) * 8));
}

void VirtioGpu::WriteDeviceConfig(std::uint32_t offset, std::uint8_t value)
{
    if (offset >= 4 && offset < 8) eventsRead_ &= ~(static_cast<std::uint32_t>(value) << ((offset - 4) * 8));
}

void VirtioGpu::RequestDisplaySize(std::uint32_t width, std::uint32_t height)
{
    width = std::clamp<std::uint32_t>(width, 640, 8192);
    height = std::clamp<std::uint32_t>(height, 480, 8192);
    if (width == displayWidth_ && height == displayHeight_) return;
    displayWidth_ = width;
    displayHeight_ = height;
    VMM_INFO("virtio-gpu: taille %ux%u demandée à l'invité.", width, height);
    if (!DriverReady()) return;
    eventsRead_ |= kEventDisplay;
    SignalConfigChange();
}

void VirtioGpu::DeviceReset()
{
    if (virgl_) {
        ++generation_; // Commands still queued on the GPU thread are dropped.
        virgl_->Post([this] { Reset3d(); });
    }
    resources_.clear();
    resourceBytes_ = 0;
    eventsRead_ = 0;
    const std::lock_guard lock(displayMutex_);
    scanout_.reset();
    scanoutActive_ = false;
    fullCopy_ = true;
    cursor_.visible = false;
    ++cursor_.shapeSerial;
}

// --- Queues --------------------------------------------------------------------------------------

void VirtioGpu::QueueNotify(std::uint16_t queue)
{
    if (virgl_) PostCommands(queue);
    else if (queue == 0) ProcessControlQueue();
    else if (queue == 1) ProcessCursorQueue();
}

void VirtioGpu::ProcessControlQueue()
{
    auto& queue = queues_[0];
    VirtQueue::Element element;
    std::vector<std::uint8_t> request;
    std::vector<std::uint8_t> response;
    bool any = false;
    while (queue.Pop(memory_, element)) {
        request.clear();
        response.clear();
        bool valid = true;
        for (const auto& buffer : element.readable) {
            if (request.size() + buffer.length > kMaxRequest) {
                valid = false;
                break;
            }
            const auto offset = request.size();
            request.resize(offset + buffer.length);
            if (!memory_.Read(buffer.address, request.data() + offset, buffer.length)) {
                valid = false;
                break;
            }
        }
        std::size_t length = 0;
        if (valid && request.size() >= kHeaderSize) {
            length = Execute(request, response);
        } else {
            Store32(response, 0, kRespErrUnspec);
            response.resize(kHeaderSize, 0);
            length = kHeaderSize;
        }
        std::size_t written = 0;
        for (const auto& buffer : element.writable) {
            if (written == length) break;
            const auto count = (std::min<std::size_t>)(buffer.length, length - written);
            if (!memory_.Write(buffer.address, response.data() + written, count)) break;
            written += count;
        }
        queue.Push(memory_, element.head, static_cast<std::uint32_t>(written));
        any = true;
    }
    if (any) SignalUsedBuffers(0);
}

void VirtioGpu::ProcessCursorQueue()
{
    auto& queue = queues_[1];
    VirtQueue::Element element;
    std::vector<std::uint8_t> request;
    bool any = false;
    while (queue.Pop(memory_, element)) {
        request.clear();
        for (const auto& buffer : element.readable) {
            if (request.size() + buffer.length > 4096) break;
            const auto offset = request.size();
            request.resize(offset + buffer.length);
            if (!memory_.Read(buffer.address, request.data() + offset, buffer.length)) break;
        }
        if (request.size() >= 52) {
            const auto type = Load32(request, 0);
            if (type == kCmdUpdateCursor) UpdateCursor(request, false);
            else if (type == kCmdMoveCursor) UpdateCursor(request, true);
        }
        queue.Push(memory_, element.head, 0);
        any = true;
    }
    if (any) SignalUsedBuffers(1);
}

std::size_t VirtioGpu::Execute(const std::vector<std::uint8_t>& request, std::vector<std::uint8_t>& response)
{
    const auto type = Load32(request, 0);
    const auto flags = Load32(request, 4);
    std::uint32_t result = kRespOkNoData;
    std::size_t length = kHeaderSize;
    response.assign(kHeaderSize, 0);

    switch (type) {
    case kCmdGetDisplayInfo: {
        // virtio_gpu_resp_display_info: header + 16 x {rect, enabled, flags}.
        result = kRespOkDisplayInfo;
        length = kHeaderSize + kMaxScanouts * 24;
        response.resize(length, 0);
        Store32(response, kHeaderSize + 8, displayWidth_.load());
        Store32(response, kHeaderSize + 12, displayHeight_.load());
        Store32(response, kHeaderSize + 16, 1); // Enabled.
        break;
    }
    case kCmdResourceCreate2d: result = CreateResource(request); break;
    case kCmdResourceUnref: {
        const auto found = resources_.find(Load32(request, 24));
        if (found == resources_.end()) {
            result = kRespErrInvalidResourceId;
        } else {
            resourceBytes_ -= found->second->pixels.size();
            resources_.erase(found); // The scanout keeps its own reference.
        }
        break;
    }
    case kCmdSetScanout: result = SetScanout(request); break;
    case kCmdResourceFlush: result = Flush(request); break;
    case kCmdTransferToHost2d: result = TransferToHost(request); break;
    case kCmdResourceAttachBacking: result = AttachBacking(request); break;
    case kCmdResourceDetachBacking: {
        const auto resource = Find(Load32(request, 24));
        if (!resource) {
            result = kRespErrInvalidResourceId;
        } else {
            resource->backing.clear();
            resource->backingBytes = 0;
        }
        break;
    }
    default:
        VMM_DEBUG("virtio-gpu: commande 0x%x non prise en charge.", type);
        result = kRespErrUnspec;
        break;
    }

    Store32(response, 0, result);
    if ((flags & kFlagFence) != 0) {
        // Commands complete synchronously: the fence is signalled at once.
        Store32(response, 4, kFlagFence);
        std::memcpy(response.data() + 8, request.data() + 8, 16); // fence_id, ctx_id, ring_idx.
    }
    return length;
}

std::shared_ptr<VirtioGpu::Resource> VirtioGpu::Find(std::uint32_t id) const
{
    const auto found = resources_.find(id);
    return found == resources_.end() ? nullptr : found->second;
}

std::uint32_t VirtioGpu::CreateResource(const std::vector<std::uint8_t>& request)
{
    const auto id = Load32(request, 24);
    const auto format = Load32(request, 28);
    const auto width = Load32(request, 32);
    const auto height = Load32(request, 36);
    if (id == 0 || resources_.contains(id)) return kRespErrInvalidResourceId;
    if (format != kFormatB8G8R8A8 && format != kFormatB8G8R8X8 &&
        format != kFormatR8G8B8A8 && format != kFormatR8G8B8X8) {
        VMM_WARN("virtio-gpu: format de ressource %u non pris en charge.", format);
        return kRespErrInvalidParameter;
    }
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) return kRespErrInvalidParameter;
    const auto bytes = static_cast<std::uint64_t>(width) * height * 4;
    if (resourceBytes_ + bytes > kMaxResourceBytes) return kRespErrOutOfMemory;
    auto resource = std::make_shared<Resource>();
    resource->width = width;
    resource->height = height;
    resource->swapRedBlue = format == kFormatR8G8B8A8 || format == kFormatR8G8B8X8;
    try {
        resource->pixels.assign(static_cast<std::size_t>(bytes), 0);
    } catch (const std::bad_alloc&) {
        return kRespErrOutOfMemory;
    }
    resourceBytes_ += bytes;
    resources_[id] = std::move(resource);
    return kRespOkNoData;
}

std::uint32_t VirtioGpu::AttachBacking(const std::vector<std::uint8_t>& request)
{
    const auto resource = Find(Load32(request, 24));
    if (!resource) return kRespErrInvalidResourceId;
    std::vector<BackingEntry> backing;
    std::uint64_t total = 0;
    if (!BuildBacking(request, backing, total)) return kRespErrInvalidParameter;
    resource->backing = std::move(backing);
    resource->backingBytes = total;
    return kRespOkNoData;
}

bool VirtioGpu::BuildBacking(const std::vector<std::uint8_t>& request, std::vector<BackingEntry>& backing,
                             std::uint64_t& total) const
{
    // virtio_gpu_resource_attach_backing: resource_id, nr_entries, then
    // {addr, length, padding} entries in guest-physical memory.
    const auto count = Load32(request, 28);
    if (count == 0 || request.size() < 32 + static_cast<std::size_t>(count) * 16) return false;
    backing.clear();
    backing.reserve(count);
    total = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto address = Load64(request, 32 + index * 16ull);
        const auto length = Load32(request, 40 + index * 16ull);
        if (length == 0) continue;
        if (auto* host = memory_.Pointer(address, length)) {
            backing.push_back({host, total, length});
            total += length;
            continue;
        }
        // The entry spans several host mappings: split it page by page.
        std::uint64_t done = 0;
        while (done < length) {
            const auto chunk = static_cast<std::uint32_t>(
                (std::min<std::uint64_t>)(length - done, 4096 - ((address + done) & 4095)));
            auto* page = memory_.Pointer(address + done, chunk);
            if (page == nullptr) return false;
            backing.push_back({page, total, chunk});
            total += chunk;
            done += chunk;
        }
    }
    return true;
}

bool VirtioGpu::CopyFromBacking(const Resource& resource, std::uint64_t offset, std::uint8_t* destination,
                                std::size_t length) const
{
    const auto& backing = resource.backing;
    if (offset + length > resource.backingBytes) return false;
    auto entry = std::upper_bound(backing.begin(), backing.end(), offset,
                                  [](std::uint64_t value, const BackingEntry& item) { return value < item.offset; });
    if (entry == backing.begin()) return false;
    --entry;
    while (length != 0) {
        if (entry == backing.end()) return false;
        const auto within = offset - entry->offset;
        const auto chunk = (std::min<std::size_t>)(length, static_cast<std::size_t>(entry->length - within));
        std::memcpy(destination, entry->host + within, chunk);
        destination += chunk;
        offset += chunk;
        length -= chunk;
        ++entry;
    }
    return true;
}

std::uint32_t VirtioGpu::TransferToHost(const std::vector<std::uint8_t>& request)
{
    const Rect rect{Load32(request, 24), Load32(request, 28), Load32(request, 32), Load32(request, 36)};
    const auto offset = Load64(request, 40);
    const auto resource = Find(Load32(request, 48));
    if (!resource) return kRespErrInvalidResourceId;
    if (static_cast<std::uint64_t>(rect.x) + rect.width > resource->width ||
        static_cast<std::uint64_t>(rect.y) + rect.height > resource->height) {
        return kRespErrInvalidParameter;
    }
    if (rect.width == 0 || rect.height == 0) return kRespOkNoData;
    if (resource->backing.empty()) return kRespErrUnspec;

    const auto stride = static_cast<std::size_t>(resource->width) * 4;
    const auto rowBytes = static_cast<std::size_t>(rect.width) * 4;
    auto* destination = resource->pixels.data() + rect.y * stride + rect.x * 4ull;
    if (rect.x == 0 && rect.width == resource->width) {
        // Whole rows: one contiguous copy.
        if (!CopyFromBacking(*resource, offset, destination, stride * rect.height)) return kRespErrInvalidParameter;
        if (resource->swapRedBlue) SwapRedBlue(destination, static_cast<std::size_t>(rect.width) * rect.height);
        return kRespOkNoData;
    }
    for (std::uint32_t row = 0; row < rect.height; ++row) {
        auto* line = destination + row * stride;
        if (!CopyFromBacking(*resource, offset + row * stride, line, rowBytes)) return kRespErrInvalidParameter;
        if (resource->swapRedBlue) SwapRedBlue(line, rect.width);
    }
    return kRespOkNoData;
}

std::uint32_t VirtioGpu::SetScanout(const std::vector<std::uint8_t>& request)
{
    const Rect rect{Load32(request, 24), Load32(request, 28), Load32(request, 32), Load32(request, 36)};
    const auto scanoutId = Load32(request, 40);
    const auto resourceId = Load32(request, 44);
    if (scanoutId != 0) return kRespErrInvalidScanoutId;
    if (resourceId == 0) return kRespOkNoData; // Disabled: keep showing the last image (mode switches).
    const auto resource = Find(resourceId);
    if (!resource) return kRespErrInvalidResourceId;
    if (rect.width == 0 || rect.height == 0 ||
        static_cast<std::uint64_t>(rect.x) + rect.width > resource->width ||
        static_cast<std::uint64_t>(rect.y) + rect.height > resource->height) {
        return kRespErrInvalidParameter;
    }
    const std::lock_guard lock(displayMutex_);
    if (!scanoutActive_) {
        VMM_INFO("virtio-gpu: affichage %ux%u activé par le pilote invité.", rect.width, rect.height);
    } else if (rect.width != scanoutRect_.width || rect.height != scanoutRect_.height) {
        VMM_INFO("virtio-gpu: résolution invitée %ux%u.", rect.width, rect.height);
    }
    scanout_ = resource;
    scanoutRect_ = rect;
    scanoutActive_ = true;
    fullCopy_ = true;
    return kRespOkNoData;
}

std::uint32_t VirtioGpu::Flush(const std::vector<std::uint8_t>& request)
{
    const Rect rect{Load32(request, 24), Load32(request, 28), Load32(request, 32), Load32(request, 36)};
    const auto resource = Find(Load32(request, 40));
    if (!resource) return kRespErrInvalidResourceId;
    ++flushes_;
    const std::lock_guard lock(displayMutex_);
    if (resource != scanout_) return kRespOkNoData;
    const auto& screen = scanoutRect_;
    const auto top = (std::max)(rect.y, screen.y);
    const auto bottom = static_cast<std::uint32_t>((std::min<std::uint64_t>)(
        static_cast<std::uint64_t>(rect.y) + rect.height, static_cast<std::uint64_t>(screen.y) + screen.height));
    if (bottom <= top) return kRespOkNoData;
    dirty_.Add(top - screen.y, bottom - screen.y);
    return kRespOkNoData;
}

void VirtioGpu::UpdateCursor(const std::vector<std::uint8_t>& request, bool move)
{
    // virtio_gpu_update_cursor: header, pos {scanout_id, x, y, padding},
    // resource_id, hot_x, hot_y, padding.
    const auto x = static_cast<std::int32_t>(Load32(request, 28));
    const auto y = static_cast<std::int32_t>(Load32(request, 32));
    if (move) {
        const std::lock_guard lock(displayMutex_);
        cursor_.x = x;
        cursor_.y = y;
        ++cursor_.moveSerial;
        return;
    }
    const auto resourceId = Load32(request, 40);
    std::shared_ptr<Resource> resource;
    if (resourceId != 0) {
        resource = Find(resourceId);
        if (!resource || resource->width > kMaxCursorDimension || resource->height > kMaxCursorDimension) return;
    }
    const std::lock_guard lock(displayMutex_);
    cursor_.x = x;
    cursor_.y = y;
    cursor_.visible = resource != nullptr;
    if (resource) {
        cursor_.width = resource->width;
        cursor_.height = resource->height;
        cursor_.hotX = Load32(request, 44);
        cursor_.hotY = Load32(request, 48);
        cursor_.pixels = resource->pixels;
    }
    ++cursor_.shapeSerial;
    ++cursor_.moveSerial;
}

// --- 3D (virgl) mode -----------------------------------------------------------------------------

void VirtioGpu::PostCommands(std::uint16_t queue)
{
    // vCPU thread, device lock held: copy each request out of the ring and
    // hand it to the GPU thread, which completes it later.
    auto& ring = queues_[queue];
    const auto generation = generation_.load();
    VirtQueue::Element element;
    while (ring.Pop(memory_, element)) {
        auto command = std::make_shared<Command>();
        command->queue = queue;
        command->head = element.head;
        command->writable = element.writable;
        command->generation = generation;
        for (const auto& buffer : element.readable) {
            if (command->request.size() + buffer.length > kMaxRequest) {
                command->request.clear();
                break;
            }
            const auto offset = command->request.size();
            command->request.resize(offset + buffer.length);
            if (!memory_.Read(buffer.address, command->request.data() + offset, buffer.length)) {
                command->request.clear();
                break;
            }
        }
        virgl_->Post([this, command] { Execute3d(command); });
    }
}

void VirtioGpu::Execute3d(const std::shared_ptr<Command>& command)
{
    if (command->generation != generation_.load()) return;
    if (command->queue == 1) {
        if (command->request.size() >= 52) Cursor3d(*command);
        Complete(*command);
        return;
    }
    const auto& request = command->request;
    command->response.assign(kHeaderSize, 0);
    if (request.size() < kHeaderSize) {
        Store32(command->response, 0, kRespErrUnspec);
        Complete(*command);
        return;
    }
    Store32(command->response, 0, Execute3dControl(*command));
    if ((Load32(request, 4) & kFlagFence) == 0) {
        Complete(*command);
        return;
    }
    // Fenced: answer once the host GPU has executed everything submitted so far.
    Store32(command->response, 4, kFlagFence);
    std::memcpy(command->response.data() + 8, request.data() + 8, 16);
    fenced_.push_back(command);
    virgl_->CreateFence(static_cast<std::uint32_t>(Load64(request, 8)), 0);
}

void VirtioGpu::OnFence(std::uint32_t fence)
{
    while (!fenced_.empty()) {
        const auto& command = fenced_.front();
        if (static_cast<std::uint32_t>(Load64(command->request, 8)) > fence) break;
        Complete(*command);
        fenced_.pop_front();
    }
}

void VirtioGpu::Complete(const Command& command)
{
    const DeviceGuard guard(lock_);
    if (command.generation != generation_.load()) return;
    auto& ring = queues_[command.queue];
    if (!ring.Ready()) return;
    std::size_t written = 0;
    for (const auto& buffer : command.writable) {
        if (written == command.response.size()) break;
        const auto count = (std::min<std::size_t>)(buffer.length, command.response.size() - written);
        if (!memory_.Write(buffer.address, command.response.data() + written, count)) break;
        written += count;
    }
    ring.Push(memory_, command.head, static_cast<std::uint32_t>(written));
    SignalUsedBuffers(command.queue);
}

void VirtioGpu::Reset3d()
{
    fenced_.clear();
    virgl_->Calls().reset();
    virglResources_.clear();
    scanoutResourceId_ = 0;
}

std::uint32_t VirtioGpu::Execute3dControl(Command& command)
{
    const auto& api = virgl_->Calls();
    const auto& request = command.request;
    auto& response = command.response;
    const auto context = Load32(request, 16);
    const auto type = Load32(request, 0);
    switch (type) {
    case kCmdGetDisplayInfo:
        response.resize(kHeaderSize + kMaxScanouts * 24, 0);
        Store32(response, kHeaderSize + 8, displayWidth_.load());
        Store32(response, kHeaderSize + 12, displayHeight_.load());
        Store32(response, kHeaderSize + 16, 1);
        return kRespOkDisplayInfo;

    case kCmdGetCapsetInfo: {
        const auto index = Load32(request, 24);
        response.resize(40, 0);
        if (index < numCapsets_) {
            Store32(response, 24, index + 1);
            Store32(response, 28, capsetVersion_[index]);
            Store32(response, 32, capsetSize_[index]);
        }
        return kRespOkCapsetInfo;
    }
    case kCmdGetCapset: {
        const auto id = Load32(request, 24);
        const auto version = Load32(request, 28);
        if (id == 0 || id > numCapsets_) return kRespErrInvalidParameter;
        response.resize(kHeaderSize + capsetSize_[id - 1], 0);
        api.fillCaps(id, version, response.data() + kHeaderSize);
        return kRespOkCapset;
    }

    case kCmdResourceCreate2d:
    case kCmdResourceCreate3d: {
        VirglResourceCreateArgs args{};
        args.handle = Load32(request, 24);
        if (type == kCmdResourceCreate2d) {
            // Plain 2D image (dumb buffer): a render-target texture.
            args.target = 2;
            args.format = Load32(request, 28);
            args.bind = 1u << 1;
            args.width = Load32(request, 32);
            args.height = Load32(request, 36);
            args.depth = 1;
            args.arraySize = 1;
            args.flags = 1; // VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP.
        } else {
            args.target = Load32(request, 28);
            args.format = Load32(request, 32);
            args.bind = Load32(request, 36);
            args.width = Load32(request, 40);
            args.height = Load32(request, 44);
            args.depth = Load32(request, 48);
            args.arraySize = Load32(request, 52);
            args.lastLevel = Load32(request, 56);
            args.samples = Load32(request, 60);
            args.flags = Load32(request, 64);
        }
        if (args.handle == 0 || virglResources_.contains(args.handle)) return kRespErrInvalidResourceId;
        if (api.resourceCreate(&args, nullptr, 0) != 0) return kRespErrInvalidParameter;
        auto& resource = virglResources_[args.handle];
        resource.width = args.width;
        resource.height = args.height;
        return kRespOkNoData;
    }
    case kCmdResourceUnref: {
        const auto id = Load32(request, 24);
        const auto found = virglResources_.find(id);
        if (found == virglResources_.end()) return kRespErrInvalidResourceId;
        VirglIovec* iov = nullptr;
        int count = 0;
        api.detachIov(static_cast<int>(id), &iov, &count);
        api.resourceUnref(id);
        virglResources_.erase(found);
        if (scanoutResourceId_ == id) scanoutResourceId_ = 0; // The host image stays on screen.
        return kRespOkNoData;
    }
    case kCmdResourceAttachBacking: {
        const auto id = Load32(request, 24);
        const auto found = virglResources_.find(id);
        if (found == virglResources_.end()) return kRespErrInvalidResourceId;
        std::vector<BackingEntry> backing;
        std::uint64_t total = 0;
        if (!BuildBacking(request, backing, total)) return kRespErrInvalidParameter;
        auto& iov = found->second.iov;
        iov.clear();
        iov.reserve(backing.size());
        for (const auto& entry : backing) iov.push_back({entry.host, entry.length});
        if (api.attachIov(static_cast<int>(id), iov.data(), static_cast<int>(iov.size())) != 0) {
            iov.clear();
            return kRespErrUnspec;
        }
        return kRespOkNoData;
    }
    case kCmdResourceDetachBacking: {
        const auto id = Load32(request, 24);
        const auto found = virglResources_.find(id);
        if (found == virglResources_.end()) return kRespErrInvalidResourceId;
        VirglIovec* iov = nullptr;
        int count = 0;
        api.detachIov(static_cast<int>(id), &iov, &count);
        found->second.iov.clear();
        return kRespOkNoData;
    }
    case kCmdTransferToHost2d: {
        const auto id = Load32(request, 48);
        if (!virglResources_.contains(id)) return kRespErrInvalidResourceId;
        VirglBox box{Load32(request, 24), Load32(request, 28), 0, Load32(request, 32), Load32(request, 36), 1};
        api.transferWrite(id, 0, 0, 0, 0, &box, Load64(request, 40), nullptr, 0);
        return kRespOkNoData;
    }
    case kCmdTransferToHost3d:
    case kCmdTransferFromHost3d: {
        // virtio_gpu_transfer_host_3d: box, offset, resource_id, level, stride, layer_stride.
        const auto id = Load32(request, 56);
        if (!virglResources_.contains(id)) return kRespErrInvalidResourceId;
        VirglBox box{Load32(request, 24), Load32(request, 28), Load32(request, 32),
                     Load32(request, 36), Load32(request, 40), Load32(request, 44)};
        const auto offset = Load64(request, 48);
        const auto level = Load32(request, 60);
        const auto stride = Load32(request, 64);
        const auto layerStride = Load32(request, 68);
        if (type == kCmdTransferToHost3d) {
            api.transferWrite(id, context, static_cast<int>(level), stride, layerStride, &box, offset, nullptr, 0);
        } else {
            api.transferRead(id, context, level, stride, layerStride, &box, offset, nullptr, 0);
        }
        return kRespOkNoData;
    }

    case kCmdSetScanout: {
        const Rect rect{Load32(request, 24), Load32(request, 28), Load32(request, 32), Load32(request, 36)};
        if (Load32(request, 40) != 0) return kRespErrInvalidScanoutId;
        const auto id = Load32(request, 44);
        if (id == 0) return kRespOkNoData; // Disabled: keep the last image (mode switches).
        VirglResourceInfo info{};
        if (!virglResources_.contains(id) || api.resourceInfo(static_cast<int>(id), &info) != 0) {
            return kRespErrInvalidResourceId;
        }
        if (rect.width == 0 || rect.height == 0 ||
            static_cast<std::uint64_t>(rect.x) + rect.width > info.width ||
            static_cast<std::uint64_t>(rect.y) + rect.height > info.height) {
            return kRespErrInvalidParameter;
        }
        // The guest flips between several buffers: the host image is reused
        // and only the flushed (damaged) rectangles are read back.
        bool full = false;
        {
            const std::lock_guard lock(displayMutex_);
            if (!scanout_ || scanout_->width != info.width || scanout_->height != info.height) {
                auto image = std::make_shared<Resource>();
                image->width = info.width;
                image->height = info.height;
                image->pixels.assign(static_cast<std::size_t>(info.width) * info.height * 4, 0);
                scanout_ = std::move(image);
                full = true;
            }
            if (!scanoutActive_) {
                VMM_INFO("virtio-gpu: affichage %ux%u activé (rendu 3D virgl).", rect.width, rect.height);
            } else if (rect.width != scanoutRect_.width || rect.height != scanoutRect_.height) {
                VMM_INFO("virtio-gpu: résolution invitée %ux%u.", rect.width, rect.height);
            }
            scanoutRect_ = rect;
            scanoutActive_ = true;
        }
        scanoutResourceId_ = id;
        scanoutSwapRedBlue_ = info.format == kFormatR8G8B8A8 || info.format == kFormatR8G8B8X8;
        if (full) {
            UpdateScreen(rect.x, rect.y, rect.width, rect.height);
            const std::lock_guard lock(displayMutex_);
            fullCopy_ = true;
        }
        return kRespOkNoData;
    }
    case kCmdResourceFlush: {
        const Rect rect{Load32(request, 24), Load32(request, 28), Load32(request, 32), Load32(request, 36)};
        const auto id = Load32(request, 40);
        ++flushes_;
        if (id == 0 || id != scanoutResourceId_) return kRespOkNoData;
        Rect screen{};
        {
            const std::lock_guard lock(displayMutex_);
            screen = scanoutRect_;
        }
        const auto left = (std::max)(rect.x, screen.x);
        const auto top = (std::max)(rect.y, screen.y);
        const auto right = static_cast<std::uint32_t>((std::min<std::uint64_t>)(
            static_cast<std::uint64_t>(rect.x) + rect.width, static_cast<std::uint64_t>(screen.x) + screen.width));
        const auto bottom = static_cast<std::uint32_t>((std::min<std::uint64_t>)(
            static_cast<std::uint64_t>(rect.y) + rect.height, static_cast<std::uint64_t>(screen.y) + screen.height));
        if (right <= left || bottom <= top) return kRespOkNoData;
        UpdateScreen(left, top, right - left, bottom - top);
        const std::lock_guard lock(displayMutex_);
        dirty_.Add(top - screen.y, bottom - screen.y);
        return kRespOkNoData;
    }

    case kCmdCtxCreate: {
        const auto length = (std::min<std::uint32_t>)(Load32(request, 24), 63);
        char name[64] = {};
        if (request.size() >= 32 + length) std::memcpy(name, request.data() + 32, length);
        return api.contextCreate(context, length, name) == 0 ? kRespOkNoData : kRespErrUnspec;
    }
    case kCmdCtxDestroy:
        api.contextDestroy(context);
        return kRespOkNoData;
    case kCmdCtxAttachResource:
        api.attachResource(static_cast<int>(context), static_cast<int>(Load32(request, 24)));
        return kRespOkNoData;
    case kCmdCtxDetachResource:
        api.detachResource(static_cast<int>(context), static_cast<int>(Load32(request, 24)));
        return kRespOkNoData;
    case kCmdSubmit3d: {
        const auto size = Load32(request, 24);
        if ((size % 4) != 0 || request.size() < 32 + static_cast<std::size_t>(size)) return kRespErrInvalidParameter;
        std::vector<std::uint32_t> buffer(size / 4); // 4-byte aligned copy, as virglrenderer requires.
        if (size != 0) std::memcpy(buffer.data(), request.data() + 32, size);
        ++submits_;
        const int error = api.submit(buffer.data(), static_cast<int>(context), static_cast<int>(buffer.size()));
        if (error != 0) VMM_DEBUG("virgl: soumission rejetée (contexte %u, erreur %d).", context, error);
        return kRespOkNoData;
    }
    default:
        VMM_DEBUG("virtio-gpu (3D): commande 0x%x non prise en charge.", type);
        return kRespErrUnspec;
    }
}

void VirtioGpu::SetTextureDisplay(bool enabled)
{
    if (textureDisplay_.exchange(enabled) == enabled || !virgl_) return;
    virgl_->Post([this] { RefreshScreen(); });
}

void VirtioGpu::SetShareAllowed(bool allowed)
{
    if (shareAllowed_.exchange(allowed) == allowed || !virgl_) return;
    virgl_->Post([this] { RefreshScreen(); });
}

bool VirtioGpu::UseTexture()
{
    return virgl_ && shareAllowed_.load() && textureDisplay_.load() && !sharedFailed_ &&
           virgl_->DxInteropAvailable();
}

bool VirtioGpu::EnsureSharedTexture(std::uint32_t width, std::uint32_t height)
{
    {
        const SharedD3DGuard d3d(SharedD3DLock());
        if (shared_->texture && shared_->width == width && shared_->height == height) return true;
    }
    ID3D11Device* device = SharedD3DDevice();
    if (device == nullptr) return false;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    // Render target: OpenGL draws into it; shader resource: Direct2D reads it.
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    {
        const SharedD3DGuard d3d(SharedD3DLock());
        if (FAILED(device->CreateTexture2D(&description, nullptr, texture.GetAddressOf()))) return false;
    }
    if (!virgl_->AttachSharedTexture(texture.Get())) return false;
    const SharedD3DGuard d3d(SharedD3DLock());
    shared_->texture = std::move(texture);
    shared_->width = width;
    shared_->height = height;
    ++shared_->generation;
    shared_->dirty.Clear();
    textureActive_ = false; // Filled whole before it is shown.
    return true;
}

void VirtioGpu::UpdateScreen(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height)
{
    Rect screen{};
    std::uint32_t imageWidth = 0;
    std::uint32_t imageHeight = 0;
    {
        const std::lock_guard lock(displayMutex_);
        screen = scanoutRect_;
        if (scanout_) {
            imageWidth = scanout_->width;
            imageHeight = scanout_->height;
        }
    }
    if (UseTexture()) {
        bool ok = EnsureSharedTexture(screen.width, screen.height);
        if (ok) {
            if (!textureActive_.load()) {
                // New texture, or coming from readbacks: copy the whole screen.
                x = screen.x;
                y = screen.y;
                width = screen.width;
                height = screen.height;
            }
            const SharedD3DGuard d3d(SharedD3DLock());
            ok = virgl_->CopyToShared(scanoutResourceId_, x, y, width, height, x - screen.x, y - screen.y);
            if (ok) shared_->dirty.Add(y - screen.y, y - screen.y + height);
        }
        if (ok) {
            if (!textureActive_.exchange(true)) VMM_INFO("virtio-gpu : écran affiché depuis la texture partagée.");
            return;
        }
        sharedFailed_ = true;
        VMM_WARN("virtio-gpu : copie vers la texture Direct3D impossible, retour à la relecture.");
    }
    if (textureActive_.exchange(false)) {
        // Back to system memory: the host image missed every update since.
        VMM_INFO("virtio-gpu : écran relu en mémoire système.");
        x = 0;
        y = 0;
        width = imageWidth;
        height = imageHeight;
        const std::lock_guard lock(displayMutex_);
        fullCopy_ = true;
    }
    Readback(x, y, width, height);
}

void VirtioGpu::RefreshScreen()
{
    if (scanoutResourceId_ == 0) return;
    Rect screen{};
    {
        const std::lock_guard lock(displayMutex_);
        if (!scanoutActive_) return;
        screen = scanoutRect_;
    }
    if (UseTexture()) {
        textureActive_ = false; // Whole screen into the texture.
        UpdateScreen(screen.x, screen.y, screen.width, screen.height);
        return;
    }
    UpdateScreen(screen.x, screen.y, screen.width, screen.height);
    const std::lock_guard lock(displayMutex_);
    fullCopy_ = true;
}

void VirtioGpu::Readback(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height)
{
    // Reads the scanout texture back into the host image the window shows.
    std::shared_ptr<Resource> image;
    {
        const std::lock_guard lock(displayMutex_);
        image = scanout_;
    }
    if (!image || scanoutResourceId_ == 0) return;
    const auto stride = image->width * 4;
    VirglBox box{x, y, 0, width, height, 1};
    VirglIovec iov{image->pixels.data(), image->pixels.size()};
    const auto offset = static_cast<std::uint64_t>(y) * stride + x * 4ull;
    if (virgl_->Calls().transferRead(scanoutResourceId_, 0, 0, stride, 0, &box, offset, &iov, 1) != 0) return;
    if (scanoutSwapRedBlue_) {
        for (std::uint32_t row = 0; row < height; ++row) SwapRedBlue(image->pixels.data() + offset + row * stride, width);
    }
}

void VirtioGpu::Cursor3d(const Command& command)
{
    const auto& request = command.request;
    const auto type = Load32(request, 0);
    const auto x = static_cast<std::int32_t>(Load32(request, 28));
    const auto y = static_cast<std::int32_t>(Load32(request, 32));
    if (type == kCmdMoveCursor) {
        const std::lock_guard lock(displayMutex_);
        cursor_.x = x;
        cursor_.y = y;
        ++cursor_.moveSerial;
        return;
    }
    if (type != kCmdUpdateCursor) return;
    const auto id = Load32(request, 40);
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (id != 0) {
        auto* data = static_cast<std::uint8_t*>(virgl_->Calls().cursorData(id, &width, &height));
        if (data == nullptr) return;
        if (width != 0 && height != 0 && width <= kMaxCursorDimension && height <= kMaxCursorDimension) {
            pixels.assign(data, data + static_cast<std::size_t>(width) * height * 4);
        }
        virgl_->Calls().free(data);
        if (pixels.empty()) return;
    }
    const std::lock_guard lock(displayMutex_);
    cursor_.x = x;
    cursor_.y = y;
    cursor_.visible = id != 0;
    if (id != 0) {
        cursor_.width = width;
        cursor_.height = height;
        cursor_.hotX = Load32(request, 44);
        cursor_.hotY = Load32(request, 48);
        cursor_.pixels = std::move(pixels);
    }
    ++cursor_.shapeSerial;
    ++cursor_.moveSerial;
}

// --- UI side -------------------------------------------------------------------------------------

bool VirtioGpu::SnapshotScanout(std::vector<std::uint8_t>& pixels, std::uint32_t& width, std::uint32_t& height)
{
    if (textureActive_.load()) {
        {
            const std::lock_guard lock(displayMutex_);
            if (!scanoutActive_) return false;
            width = scanoutRect_.width;
            height = scanoutRect_.height;
        }
        ID3D11Device* device = SharedD3DDevice();
        if (device == nullptr) return false;
        // The window and the GPU thread use the device under this lock too.
        const SharedD3DGuard d3d(SharedD3DLock());
        if (!shared_->texture) return false;
        D3D11_TEXTURE2D_DESC description{};
        shared_->texture->GetDesc(&description);
        width = (std::min)(width, description.Width);
        height = (std::min)(height, description.Height);
        if (width == 0 || height == 0) return false;
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&description, nullptr, &staging))) return false;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        const D3D11_BOX box{0, 0, 0, width, height, 1};
        context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, shared_->texture.Get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        pixels.resize(static_cast<std::size_t>(width) * height * 4);
        for (std::uint32_t row = 0; row < height; ++row) {
            std::memcpy(pixels.data() + static_cast<std::size_t>(row) * width * 4,
                        static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch,
                        static_cast<std::size_t>(width) * 4);
        }
        context->Unmap(staging.Get(), 0);
        return true;
    }
    // 2D: a copy of the scanout rectangle under the display lock.
    const std::lock_guard lock(displayMutex_);
    if (!scanoutActive_ || !scanout_) return false;
    const auto& rect = scanoutRect_;
    width = rect.width;
    height = rect.height;
    if (width == 0 || height == 0) return false;
    pixels.resize(static_cast<std::size_t>(width) * height * 4);
    const auto sourceStride = static_cast<std::size_t>(scanout_->width) * 4;
    const auto* source = scanout_->pixels.data() + rect.y * sourceStride + rect.x * 4ull;
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(pixels.data() + static_cast<std::size_t>(row) * width * 4, source + row * sourceStride,
                    static_cast<std::size_t>(width) * 4);
    }
    return true;
}

bool VirtioGpu::CopyScanout(ScanoutCopy& copy, DirtyRows& rows)
{
    if (textureActive_.load()) {
        // virgl screen in the shared texture: nothing to copy here.
        {
            const std::lock_guard lock(displayMutex_);
            if (!scanoutActive_) return false;
            copy.width = scanoutRect_.width;
            copy.height = scanoutRect_.height;
        }
        copy.texture = shared_;
        const SharedD3DGuard d3d(SharedD3DLock());
        rows.Add(shared_->dirty);
        shared_->dirty.Clear();
        return true;
    }
    copy.texture.reset();
    const std::lock_guard lock(displayMutex_);
    if (!scanoutActive_ || !scanout_) return false;
    const auto& rect = scanoutRect_;
    const auto bytes = static_cast<std::size_t>(rect.width) * rect.height * 4;
    if (copy.width != rect.width || copy.height != rect.height || copy.pixels.size() != bytes) {
        copy.width = rect.width;
        copy.height = rect.height;
        copy.pixels.assign(bytes, 0);
        fullCopy_ = true;
    }
    if (fullCopy_) {
        fullCopy_ = false;
        dirty_.Clear();
        dirty_.Add(0, rect.height);
    }
    dirty_.Clip(rect.height);
    const auto sourceStride = static_cast<std::size_t>(scanout_->width) * 4;
    const auto rowBytes = static_cast<std::size_t>(rect.width) * 4;
    const auto* source = scanout_->pixels.data() + rect.y * sourceStride + rect.x * 4ull;
    for (const auto& band : dirty_.Bands()) {
        for (auto row = band.top; row < band.bottom; ++row) {
            std::memcpy(copy.pixels.data() + row * rowBytes, source + row * sourceStride, rowBytes);
        }
    }
    rows.Add(dirty_);
    dirty_.Clear();
    return true;
}

bool VirtioGpu::CursorSnapshot(GpuCursor& cursor)
{
    const std::lock_guard lock(displayMutex_);
    if (cursor.shapeSerial != cursor_.shapeSerial) {
        cursor = cursor_;
        return true;
    }
    if (cursor.moveSerial == cursor_.moveSerial) return false;
    cursor.x = cursor_.x;
    cursor.y = cursor_.y;
    cursor.visible = cursor_.visible;
    cursor.moveSerial = cursor_.moveSerial;
    return true;
}

} // namespace vmm
