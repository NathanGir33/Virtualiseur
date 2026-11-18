#include "WindowsHypervisorBackend.h"

#include <limits>
#include <array>
#include <bit>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>

namespace virtualiseur::whp {
namespace {

struct LocalApicState {
    struct Register {
        std::uint32_t value;
        std::uint32_t reserved[3];
    } registers[256];
};
static_assert(sizeof(LocalApicState) == 4096);

// Doorbell entry points, resolved at run time: notification ports exist
// from Windows 11 / Server 2022, the older doorbell calls from Windows 10
// 1903. Linking them statically would stop the VMM from starting elsewhere.
struct DoorbellApi {
    using CreatePort = HRESULT(WINAPI*)(WHV_PARTITION_HANDLE, const WHV_NOTIFICATION_PORT_PARAMETERS*, HANDLE,
                                        WHV_NOTIFICATION_PORT_HANDLE*);
    using DeletePort = HRESULT(WINAPI*)(WHV_PARTITION_HANDLE, WHV_NOTIFICATION_PORT_HANDLE);
    using Register = HRESULT(WINAPI*)(WHV_PARTITION_HANDLE, const WHV_DOORBELL_MATCH_DATA*, HANDLE);
    using Unregister = HRESULT(WINAPI*)(WHV_PARTITION_HANDLE, const WHV_DOORBELL_MATCH_DATA*);

    CreatePort createPort = nullptr;
    DeletePort deletePort = nullptr;
    Register registerEvent = nullptr;
    Unregister unregisterEvent = nullptr;

    static const DoorbellApi& Get()
    {
        static const DoorbellApi api = [] {
            DoorbellApi result;
            if (const HMODULE module = GetModuleHandleW(L"WinHvPlatform.dll")) {
                result.createPort = reinterpret_cast<CreatePort>(GetProcAddress(module, "WHvCreateNotificationPort"));
                result.deletePort = reinterpret_cast<DeletePort>(GetProcAddress(module, "WHvDeleteNotificationPort"));
                result.registerEvent = reinterpret_cast<Register>(GetProcAddress(module, "WHvRegisterPartitionDoorbellEvent"));
                result.unregisterEvent = reinterpret_cast<Unregister>(GetProcAddress(module, "WHvUnregisterPartitionDoorbellEvent"));
            }
            return result;
        }();
        return api;
    }
};

struct Doorbell {
    WHV_NOTIFICATION_PORT_HANDLE port = nullptr; // Notification port API.
    WHV_DOORBELL_MATCH_DATA match{};             // Older doorbell API.
};

std::runtime_error MakeWHPError(const char* operation, HRESULT result)
{
    std::ostringstream message;
    message << operation << " failed with HRESULT 0x" << std::hex
            << static_cast<unsigned long>(result);
    return std::runtime_error(message.str());
}

void CheckHRESULT(const char* operation, HRESULT result)
{
    if (FAILED(result)) {
        throw MakeWHPError(operation, result);
    }
}

} // namespace

HostCapabilities QueryHostCapabilities() noexcept
{
    HostCapabilities result{};
    WHV_CAPABILITY capability{};
    UINT32 written = 0;

    result.status = WHvGetCapability(
        WHvCapabilityCodeHypervisorPresent,
        &capability,
        sizeof(capability),
        &written);

    if (SUCCEEDED(result.status) && written >= sizeof(BOOL)) {
        result.apiAvailable = true;
        result.hypervisorPresent = capability.HypervisorPresent != FALSE;
    }
    return result;
}

Partition::Partition(std::uint32_t virtualProcessorCount, bool enableLocalApic, bool hypervEnlightenments,
                     bool unhandledMsrExits)
{
    if (virtualProcessorCount == 0) {
        throw std::invalid_argument("A WHP partition must have at least one vCPU.");
    }

    CheckHRESULT("WHvCreatePartition", WHvCreatePartition(&handle_));

    try {
        CheckHRESULT(
            "WHvSetPartitionProperty(ProcessorCount)",
            WHvSetPartitionProperty(
                handle_,
                WHvPartitionPropertyCodeProcessorCount,
                &virtualProcessorCount,
                sizeof(virtualProcessorCount)));
        if (enableLocalApic) {
            const WHV_X64_LOCAL_APIC_EMULATION_MODE apicMode = WHvX64LocalApicEmulationModeXApic;
            CheckHRESULT(
                "WHvSetPartitionProperty(LocalApicEmulationMode)",
                WHvSetPartitionProperty(
                    handle_,
                    WHvPartitionPropertyCodeLocalApicEmulationMode,
                    &apicMode,
                    sizeof(apicMode)));

            // Keep the virtual APIC timer calibrated to the clock rate exposed
            // by this WHP implementation. Some hosts reject this property, so
            // only set it when the capability query succeeds and returns a value.
            WHV_CAPABILITY interruptClock{};
            UINT32 bytesWritten = 0;
            const HRESULT capabilityResult = WHvGetCapability(
                WHvCapabilityCodeInterruptClockFrequency,
                &interruptClock,
                sizeof(interruptClock),
                &bytesWritten);
            if (SUCCEEDED(capabilityResult) &&
                bytesWritten >= sizeof(interruptClock.InterruptClockFrequency) &&
                interruptClock.InterruptClockFrequency != 0) {
                CheckHRESULT(
                    "WHvSetPartitionProperty(InterruptClockFrequency)",
                    WHvSetPartitionProperty(
                        handle_,
                        WHvPartitionPropertyCodeInterruptClockFrequency,
                        &interruptClock.InterruptClockFrequency,
                        sizeof(interruptClock.InterruptClockFrequency)));
            }
        }
        if (hypervEnlightenments) EnableEnlightenments();
        if (unhandledMsrExits) {
            // Only the MSRs WHP does not implement: with X64MsrExit alone,
            // every MSR access would exit.
            WHV_EXTENDED_VM_EXITS exits{};
            exits.X64MsrExit = 1;
            if (SUCCEEDED(WHvSetPartitionProperty(handle_, WHvPartitionPropertyCodeExtendedVmExits, &exits, sizeof(exits)))) {
                WHV_X64_MSR_EXIT_BITMAP bitmap{};
                bitmap.UnhandledMsrs = 1;
                msrExits_ = SUCCEEDED(WHvSetPartitionProperty(handle_, WHvPartitionPropertyCodeX64MsrExitBitmap, &bitmap,
                                                              sizeof(bitmap)));
                if (!msrExits_) {
                    exits.X64MsrExit = 0;
                    (void)WHvSetPartitionProperty(handle_, WHvPartitionPropertyCodeExtendedVmExits, &exits, sizeof(exits));
                }
            }
        }
        CheckHRESULT("WHvSetupPartition", WHvSetupPartition(handle_));

        createdProcessors_.reserve(virtualProcessorCount);
        for (std::uint32_t index = 0; index < virtualProcessorCount; ++index) {
            CheckHRESULT(
                "WHvCreateVirtualProcessor",
                WHvCreateVirtualProcessor(handle_, index, 0));
            createdProcessors_.push_back(index);
        }
    } catch (...) {
        Close();
        throw;
    }
}

Partition::~Partition()
{
    Close();
}

void Partition::EnableEnlightenments()
{
    WHV_CAPABILITY capability{};
    UINT32 written = 0;
    if (FAILED(WHvGetCapability(WHvCapabilityCodeSyntheticProcessorFeaturesBanks, &capability,
                                sizeof(capability), &written)) ||
        written < sizeof(WHV_SYNTHETIC_PROCESSOR_FEATURES_BANKS) ||
        capability.SyntheticProcessorFeaturesBanks.BanksCount < 1) {
        return; // Host too old: the guest keeps calibrating its own clocks.
    }

    WHV_SYNTHETIC_PROCESSOR_FEATURES wanted{};
    // Linux only recognises Hyper-V with the hypercall and VP index MSRs.
    wanted.HypervisorPresent = 1;
    wanted.Hv1 = 1;
    wanted.AccessHypercallRegs = 1;
    wanted.AccessVpIndex = 1;
    // VP assist page (Linux writes its MSR on every CPU) and the synthetic
    // APIC MSRs (EOI, ICR, TPR without APIC page emulation).
    wanted.AccessIntrCtrlRegs = 1;
    // Clock: reference counter and TSC page (clocksource without exits),
    // TSC and APIC timer frequencies (no PIT calibration at boot).
    wanted.AccessPartitionReferenceCounter = 1;
    wanted.AccessPartitionReferenceTsc = 1;
    wanted.AccessFrequencyRegs = 1;
    wanted.AccessVpRunTimeReg = 1;
    // Synthetic timers in direct mode: per-vCPU timers that fire an APIC
    // vector, without the SynIC message machinery (no VMBus here).
    wanted.AccessSynicRegs = 1;
    wanted.AccessSyntheticTimerRegs = 1;
    wanted.DirectSyntheticTimers = 1;

    WHV_SYNTHETIC_PROCESSOR_FEATURES_BANKS banks{};
    banks.BanksCount = 1;
    banks.AsUINT64[0] = wanted.AsUINT64 & capability.SyntheticProcessorFeaturesBanks.AsUINT64[0];
    WHV_SYNTHETIC_PROCESSOR_FEATURES granted{};
    granted.AsUINT64 = banks.AsUINT64[0];
    if (!granted.HypervisorPresent || !granted.Hv1 || !granted.AccessHypercallRegs || !granted.AccessVpIndex) return;
    if (SUCCEEDED(WHvSetPartitionProperty(handle_, WHvPartitionPropertyCodeSyntheticProcessorFeaturesBanks,
                                          &banks, sizeof(banks)))) {
        syntheticFeatures_ = granted;
    }
}

std::uint32_t Partition::VirtualProcessorCount() const noexcept
{
    return static_cast<std::uint32_t>(createdProcessors_.size());
}

void Partition::SetVirtualProcessorRegisters(
    std::uint32_t virtualProcessorIndex,
    std::span<const WHV_REGISTER_NAME> names,
    std::span<const WHV_REGISTER_VALUE> values)
{
    if (virtualProcessorIndex >= createdProcessors_.size()) {
        throw std::out_of_range("The WHP virtual processor index is out of range.");
    }
    if (names.empty() || names.size() != values.size() ||
        names.size() > (std::numeric_limits<UINT32>::max)()) {
        throw std::invalid_argument("Register names and values must have the same non-zero count.");
    }

    CheckHRESULT(
        "WHvSetVirtualProcessorRegisters",
        WHvSetVirtualProcessorRegisters(
            handle_,
            virtualProcessorIndex,
            names.data(),
            static_cast<UINT32>(names.size()),
            values.data()));
}

std::vector<WHV_REGISTER_VALUE> Partition::GetVirtualProcessorRegisters(
    std::uint32_t virtualProcessorIndex,
    std::span<const WHV_REGISTER_NAME> names)
{
    if (virtualProcessorIndex >= createdProcessors_.size()) {
        throw std::out_of_range("The WHP virtual processor index is out of range.");
    }
    if (names.empty() || names.size() > (std::numeric_limits<UINT32>::max)()) {
        throw std::invalid_argument("At least one valid register name is required.");
    }

    std::vector<WHV_REGISTER_VALUE> values(names.size());
    CheckHRESULT(
        "WHvGetVirtualProcessorRegisters",
        WHvGetVirtualProcessorRegisters(
            handle_,
            virtualProcessorIndex,
            names.data(),
            static_cast<UINT32>(names.size()),
            values.data()));
    return values;
}

WHV_RUN_VP_EXIT_CONTEXT Partition::RunVirtualProcessor(std::uint32_t virtualProcessorIndex)
{
    if (virtualProcessorIndex >= createdProcessors_.size()) {
        throw std::out_of_range("The WHP virtual processor index is out of range.");
    }

    WHV_RUN_VP_EXIT_CONTEXT exitContext{};
    CheckHRESULT(
        "WHvRunVirtualProcessor",
        WHvRunVirtualProcessor(
            handle_,
            virtualProcessorIndex,
            &exitContext,
            sizeof(exitContext)));
    return exitContext;
}

void Partition::RequestExternalInterrupt(std::uint32_t virtualProcessorIndex, std::uint32_t vector)
{
    if (virtualProcessorIndex >= createdProcessors_.size()) {
        throw std::out_of_range("The WHP virtual processor index is out of range.");
    }
    if (vector < 0x10 || vector > 0xFE) {
        throw std::invalid_argument("An x86 external interrupt vector must be in the range 0x10-0xFE.");
    }

    WHV_INTERRUPT_CONTROL interrupt{};
    interrupt.Type = WHvX64InterruptTypeFixed;
    interrupt.DestinationMode = WHvX64InterruptDestinationModePhysical;
    interrupt.TriggerMode = WHvX64InterruptTriggerModeEdge;
    interrupt.Destination = virtualProcessorIndex;
    interrupt.Vector = vector;
    CheckHRESULT(
        "WHvRequestInterrupt",
        WHvRequestInterrupt(handle_, &interrupt, sizeof(interrupt)));
}

void Partition::RequestInterrupt(const WHV_INTERRUPT_CONTROL& control)
{
    CheckHRESULT(
        "WHvRequestInterrupt",
        WHvRequestInterrupt(handle_, &control, sizeof(control)));
}

void Partition::AcknowledgeExternalInterrupt(std::uint32_t virtualProcessorIndex)
{
    SetLocalApicRegister(virtualProcessorIndex, 0x0B0, 0);
}

void Partition::SetLocalApicRegister(
    std::uint32_t virtualProcessorIndex, std::uint32_t registerOffset, std::uint32_t value)
{
    if (virtualProcessorIndex >= createdProcessors_.size()) {
        throw std::out_of_range("The WHP virtual processor index is out of range.");
    }
    if ((registerOffset & 0x0F) != 0 || registerOffset >= 0x1000) {
        throw std::invalid_argument("The local APIC register offset must be 16-byte aligned and below 0x1000.");
    }

    LocalApicState state{};
    UINT32 bytesWritten = 0;
    CheckHRESULT(
        "WHvGetVirtualProcessorInterruptControllerState2",
        WHvGetVirtualProcessorInterruptControllerState2(
            handle_, virtualProcessorIndex, &state, sizeof(state), &bytesWritten));
    if (bytesWritten != sizeof(state)) {
        throw std::runtime_error("WHP returned an unexpected local APIC state size.");
    }

    if (registerOffset == 0x0B0) {
        // EOI retires the highest-priority in-service vector.
        for (int bank = 7; bank >= 0; --bank) {
            auto& inService = state.registers[0x10 + bank].value;
            if (inService != 0) {
                const auto bit = 31u - static_cast<unsigned>(std::countl_zero(inService));
                inService &= ~(1u << bit);
                break;
            }
        }
    } else {
        state.registers[registerOffset / 16].value = value;
    }

    CheckHRESULT(
        "WHvSetVirtualProcessorInterruptControllerState2",
        WHvSetVirtualProcessorInterruptControllerState2(
            handle_, virtualProcessorIndex, &state, sizeof(state)));
}

std::uint32_t Partition::GetLocalApicRegister(
    std::uint32_t virtualProcessorIndex, std::uint32_t registerOffset) const
{
    if (virtualProcessorIndex >= createdProcessors_.size()) {
        throw std::out_of_range("The WHP virtual processor index is out of range.");
    }
    if ((registerOffset & 0x0F) != 0 || registerOffset >= 0x1000) {
        throw std::invalid_argument("The local APIC register offset must be 16-byte aligned and below 0x1000.");
    }

    LocalApicState state{};
    UINT32 bytesWritten = 0;
    CheckHRESULT(
        "WHvGetVirtualProcessorInterruptControllerState2",
        WHvGetVirtualProcessorInterruptControllerState2(
            handle_, virtualProcessorIndex, &state, sizeof(state), &bytesWritten));
    if (bytesWritten != sizeof(state)) {
        throw std::runtime_error("WHP returned an unexpected local APIC state size.");
    }
    return state.registers[registerOffset / 16].value;
}

bool Partition::CancelRunVirtualProcessor(std::uint32_t virtualProcessorIndex) noexcept
{
    if (handle_ == nullptr || virtualProcessorIndex >= createdProcessors_.size()) {
        return false;
    }
    return SUCCEEDED(WHvCancelRunVirtualProcessor(handle_, virtualProcessorIndex, 0));
}

void* Partition::CreateDoorbell(std::uint64_t guestPhysicalAddress, HANDLE event) noexcept
{
    const auto& api = DoorbellApi::Get();
    if (handle_ == nullptr || event == nullptr) return nullptr;
    // One address per queue: any write matches. WHP accepts either Length 0
    // with no matching at all, or a Length together with MatchOnLength
    // (Length without MatchOnLength is E_INVALIDARG).
    WHV_DOORBELL_MATCH_DATA match{};
    match.GuestAddress = guestPhysicalAddress;
    match.Length = 0;
    match.MatchOnValue = 0;
    match.MatchOnLength = 0;
    auto* doorbell = new (std::nothrow) Doorbell;
    if (doorbell == nullptr) return nullptr;
    doorbell->match = match;
    lastDoorbellError_ = E_NOTIMPL;
    if (api.createPort != nullptr && api.deletePort != nullptr) {
        WHV_NOTIFICATION_PORT_PARAMETERS parameters{};
        parameters.NotificationPortType = WHvNotificationPortTypeDoorbell;
        parameters.Doorbell = match;
        lastDoorbellError_ = api.createPort(handle_, &parameters, event, &doorbell->port);
        if (SUCCEEDED(lastDoorbellError_)) return doorbell;
        doorbell->port = nullptr;
    }
    if (api.registerEvent != nullptr && api.unregisterEvent != nullptr) {
        const HRESULT result = api.registerEvent(handle_, &match, event);
        if (SUCCEEDED(result)) return doorbell;
        if (lastDoorbellError_ == E_NOTIMPL) lastDoorbellError_ = result;
    }
    delete doorbell;
    return nullptr;
}

void Partition::DeleteDoorbell(void* value) noexcept
{
    auto* doorbell = static_cast<Doorbell*>(value);
    if (doorbell == nullptr) return;
    const auto& api = DoorbellApi::Get();
    if (handle_ != nullptr) {
        if (doorbell->port != nullptr) (void)api.deletePort(handle_, doorbell->port);
        else (void)api.unregisterEvent(handle_, &doorbell->match);
    }
    delete doorbell;
}

void* Partition::GetHostAddressForGuestPhysicalAddress(
    std::uint64_t guestPhysicalAddress,
    std::size_t sizeInBytes) const noexcept
{
    if (sizeInBytes == 0 || guestPhysicalAddress >
            (std::numeric_limits<std::uint64_t>::max)() - sizeInBytes) {
        return nullptr;
    }
    const auto requestedEnd = guestPhysicalAddress + sizeInBytes;
    for (const auto& mapping : memoryMappings_) {
        if (guestPhysicalAddress >= mapping.guestPhysicalAddress &&
            requestedEnd <= mapping.guestPhysicalAddress + mapping.sizeInBytes) {
            const auto offset = static_cast<std::size_t>(guestPhysicalAddress - mapping.guestPhysicalAddress);
            return static_cast<std::uint8_t*>(mapping.hostAddress) + offset;
        }
    }
    return nullptr;
}

WHV_TRANSLATE_GVA_RESULT Partition::TranslateGuestVirtualAddress(
    std::uint32_t virtualProcessorIndex,
    std::uint64_t guestVirtualAddress,
    WHV_TRANSLATE_GVA_FLAGS flags,
    std::uint64_t& guestPhysicalAddress)
{
    if (virtualProcessorIndex >= createdProcessors_.size()) {
        throw std::out_of_range("The WHP virtual processor index is out of range.");
    }
    WHV_TRANSLATE_GVA_RESULT result{};
    CheckHRESULT(
        "WHvTranslateGva",
        WHvTranslateGva(
            handle_, virtualProcessorIndex, guestVirtualAddress, flags, &result, &guestPhysicalAddress));
    return result;
}

bool Partition::QueryDirtyPages(std::uint64_t guestPhysicalAddress, std::size_t sizeInBytes,
                                std::vector<std::uint64_t>& bitmap) noexcept
{
    const auto pages = (sizeInBytes + 4095) / 4096;
    try {
        bitmap.assign((pages + 63) / 64, 0);
    } catch (...) {
        return false;
    }
    return SUCCEEDED(WHvQueryGpaRangeDirtyBitmap(handle_, guestPhysicalAddress, sizeInBytes,
        bitmap.data(), static_cast<UINT32>(bitmap.size() * sizeof(std::uint64_t))));
}

void* Partition::AllocateAndMapGuestMemory(
    std::uint64_t guestPhysicalAddress,
    std::size_t sizeInBytes,
    bool trackDirtyPages)
{
    constexpr std::uint64_t pageSize = 4096;
    if (sizeInBytes == 0 ||
        (guestPhysicalAddress % pageSize) != 0 ||
        (sizeInBytes % pageSize) != 0 ||
        sizeInBytes > (std::numeric_limits<std::uint64_t>::max)() - guestPhysicalAddress) {
        throw std::invalid_argument("Guest memory must be non-empty, page-aligned, and in range.");
    }
    if (handle_ == nullptr) {
        throw std::logic_error("Cannot map memory into a closed WHP partition.");
    }

    void* hostAddress = VirtualAlloc(
        nullptr,
        sizeInBytes,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);
    if (hostAddress == nullptr) {
        throw std::runtime_error("VirtualAlloc failed while allocating guest RAM.");
    }

    const auto flags = WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute;
    HRESULT result = WHvMapGpaRange(
        handle_,
        hostAddress,
        guestPhysicalAddress,
        static_cast<UINT64>(sizeInBytes),
        trackDirtyPages ? static_cast<WHV_MAP_GPA_RANGE_FLAGS>(flags | WHvMapGpaRangeFlagTrackDirtyPages) : flags);
    if (FAILED(result) && trackDirtyPages) {
        // Older hosts lack dirty tracking: map normally.
        result = WHvMapGpaRange(handle_, hostAddress, guestPhysicalAddress,
                                static_cast<UINT64>(sizeInBytes), flags);
    }
    if (FAILED(result)) {
        (void)VirtualFree(hostAddress, 0, MEM_RELEASE);
        throw MakeWHPError("WHvMapGpaRange", result);
    }

    try {
        memoryMappings_.push_back({guestPhysicalAddress, sizeInBytes, hostAddress});
    } catch (...) {
        (void)WHvUnmapGpaRange(handle_, guestPhysicalAddress, static_cast<UINT64>(sizeInBytes));
        (void)VirtualFree(hostAddress, 0, MEM_RELEASE);
        throw;
    }
    return hostAddress;
}

void Partition::MapDeviceMemory(void* hostAddress, std::uint64_t guestPhysicalAddress, std::size_t sizeInBytes,
                                WHV_MAP_GPA_RANGE_FLAGS flags)
{
    if (handle_ == nullptr) throw std::logic_error("Cannot map memory into a closed WHP partition.");
    CheckHRESULT("WHvMapGpaRange(device)",
                 WHvMapGpaRange(handle_, hostAddress, guestPhysicalAddress, static_cast<UINT64>(sizeInBytes), flags));
}

void Partition::UnmapDeviceMemory(std::uint64_t guestPhysicalAddress, std::size_t sizeInBytes)
{
    if (handle_ == nullptr) return;
    CheckHRESULT("WHvUnmapGpaRange(device)",
                 WHvUnmapGpaRange(handle_, guestPhysicalAddress, static_cast<UINT64>(sizeInBytes)));
}

void Partition::Close() noexcept
{
    (void)Destroy();
}

HRESULT Partition::Destroy() noexcept
{
    if (handle_ == nullptr) {
        return S_OK;
    }

    const HRESULT result = WHvDeletePartition(handle_);
    handle_ = nullptr;

    for (const auto& mapping : memoryMappings_) {
        (void)VirtualFree(mapping.hostAddress, 0, MEM_RELEASE);
    }
    memoryMappings_.clear();
    createdProcessors_.clear();
    return result;
}

} // namespace virtualiseur::whp
