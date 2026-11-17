#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <Windows.h>
#include <WinHvPlatform.h>

namespace virtualiseur::whp {

struct HostCapabilities {
    bool apiAvailable = false;
    bool hypervisorPresent = false;
    HRESULT status = E_FAIL;
};

// Queries whether Windows exposes the Windows Hypervisor Platform API and
// whether a hypervisor is currently available to it.
HostCapabilities QueryHostCapabilities() noexcept;

// Owns a WHP partition, its virtual processors, and mapped guest RAM. Firmware,
// devices, guest boot state, exit handling, and the UI are separate work.
class Partition final {
public:
    // `hypervEnlightenments` exposes the Hyper-V synthetic interface that
    // WHP implements itself (CPUID 0x40000000 leaves, reference TSC page,
    // frequency MSRs, synthetic timers): guests then read the time and the
    // TSC frequency without calibration and without VM exits.
    explicit Partition(std::uint32_t virtualProcessorCount, bool enableLocalApic = false,
                       bool hypervEnlightenments = false);
    ~Partition();

    Partition(const Partition&) = delete;
    Partition& operator=(const Partition&) = delete;
    Partition(Partition&&) = delete;
    Partition& operator=(Partition&&) = delete;

    std::uint32_t VirtualProcessorCount() const noexcept;
    // Synthetic features actually enabled (0 without enlightenments).
    WHV_SYNTHETIC_PROCESSOR_FEATURES SyntheticFeatures() const noexcept { return syntheticFeatures_; }
    void* AllocateAndMapGuestMemory(std::uint64_t guestPhysicalAddress, std::size_t sizeInBytes,
                                    bool trackDirtyPages = false);
    // Fills one bit per 4 KiB page written since the previous query (and
    // clears them). Returns false if dirty tracking is unavailable.
    bool QueryDirtyPages(std::uint64_t guestPhysicalAddress, std::size_t sizeInBytes,
                         std::vector<std::uint64_t>& bitmap) noexcept;
    void SetVirtualProcessorRegisters(
        std::uint32_t virtualProcessorIndex,
        std::span<const WHV_REGISTER_NAME> names,
        std::span<const WHV_REGISTER_VALUE> values);
    std::vector<WHV_REGISTER_VALUE> GetVirtualProcessorRegisters(
        std::uint32_t virtualProcessorIndex,
        std::span<const WHV_REGISTER_NAME> names);
    WHV_RUN_VP_EXIT_CONTEXT RunVirtualProcessor(std::uint32_t virtualProcessorIndex);
    void RequestExternalInterrupt(std::uint32_t virtualProcessorIndex, std::uint32_t vector);
    // Thread-safe: WHP queues the interrupt in its emulated local APIC and
    // wakes the target virtual processor if it is waiting.
    void RequestInterrupt(const WHV_INTERRUPT_CONTROL& control);
    WHV_PARTITION_HANDLE Handle() const noexcept { return handle_; }
    void AcknowledgeExternalInterrupt(std::uint32_t virtualProcessorIndex);
    void SetLocalApicRegister(
        std::uint32_t virtualProcessorIndex, std::uint32_t registerOffset, std::uint32_t value);
    std::uint32_t GetLocalApicRegister(
        std::uint32_t virtualProcessorIndex, std::uint32_t registerOffset) const;
    bool CancelRunVirtualProcessor(std::uint32_t virtualProcessorIndex) noexcept;
    // Deletes the partition now (the destructor does it otherwise); returns
    // the WHvDeletePartition result.
    HRESULT Destroy() noexcept;
    // Doorbell: a guest write to `guestPhysicalAddress` (an unmapped GPA)
    // signals `event` inside the hypervisor, without a VM exit to the VMM.
    // Returns nullptr when this host lacks doorbells or refuses the address.
    void* CreateDoorbell(std::uint64_t guestPhysicalAddress, HANDLE event) noexcept;
    void DeleteDoorbell(void* doorbell) noexcept;
    HRESULT LastDoorbellError() const noexcept { return lastDoorbellError_; }
    void* GetHostAddressForGuestPhysicalAddress(std::uint64_t guestPhysicalAddress, std::size_t sizeInBytes) const noexcept;
    WHV_TRANSLATE_GVA_RESULT TranslateGuestVirtualAddress(
        std::uint32_t virtualProcessorIndex,
        std::uint64_t guestVirtualAddress,
        WHV_TRANSLATE_GVA_FLAGS flags,
        std::uint64_t& guestPhysicalAddress);

private:
    struct MemoryMapping {
        std::uint64_t guestPhysicalAddress;
        std::size_t sizeInBytes;
        void* hostAddress;
    };

    void Close() noexcept;

    void EnableEnlightenments();

    WHV_PARTITION_HANDLE handle_ = nullptr;
    WHV_SYNTHETIC_PROCESSOR_FEATURES syntheticFeatures_{};
    HRESULT lastDoorbellError_ = S_OK;
    std::vector<std::uint32_t> createdProcessors_;
    std::vector<MemoryMapping> memoryMappings_;
};

} // namespace virtualiseur::whp
