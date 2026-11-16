#include "WindowsHypervisorBackend.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

namespace {

WHV_X64_SEGMENT_REGISTER MakeFlatSegment(
    std::uint16_t selector,
    std::uint16_t attributes)
{
    WHV_X64_SEGMENT_REGISTER segment{};
    segment.Base = 0;
    segment.Limit = 0xFFFFFFFF;
    segment.Selector = selector;
    segment.Attributes = attributes;
    return segment;
}

int RunDemo()
{
    const auto capabilities = virtualiseur::whp::QueryHostCapabilities();
    if (FAILED(capabilities.status) || !capabilities.apiAvailable || !capabilities.hypervisorPresent) {
        std::cerr << "WHP n'est pas disponible. Activez la fonctionnalite facultative Windows "
                     "'Windows Hypervisor Platform' et verifiez que la virtualisation CPU est active.\n"
                  << "HRESULT: 0x" << std::hex << static_cast<unsigned long>(capabilities.status) << "\n";
        return 2;
    }

    // Tiny 32-bit protected-mode payload using the Linux boot protocol entry
    // contract: ESI points at a zero page, other designated GPRs start at zero.
    constexpr std::uint64_t guestRamBase = 0x1000;
    constexpr std::size_t guestRamSize = 0x1000;
    constexpr std::uint64_t zeroPageGpa = 0x7000;
    constexpr std::size_t zeroPageSize = 0x1000;
    constexpr std::size_t gdtOffset = 0x100;
    constexpr std::array<std::uint8_t, 25> guestCode{
        0x66, 0xBA, 0xE9, 0x00,                   // mov dx, 0x00E9 (operand-size override)
        0xB0, 'B', 0xEE,                         // mov al, 'B' ; out dx, al
        0xB0, '3', 0xEE,
        0xB0, '2', 0xEE,
        0xBE, 0x00, 0x70, 0x00, 0x00,           // mov esi, 0x7000
        0x8B, 0x86, 0xE8, 0x01, 0x00, 0x00,     // mov eax, [esi + e820_entries]
        0xF4,                                     // hlt
    };

    virtualiseur::whp::Partition partition(1);
    auto* guestRam = static_cast<std::uint8_t*>(
        partition.AllocateAndMapGuestMemory(guestRamBase, guestRamSize));
    std::memcpy(guestRam, guestCode.data(), guestCode.size());
    auto* zeroPage = static_cast<std::uint8_t*>(
        partition.AllocateAndMapGuestMemory(zeroPageGpa, zeroPageSize));
    constexpr std::size_t e820EntryCountOffset = 0x1E8;
    zeroPage[e820EntryCountOffset] = 3;

    constexpr std::array<std::uint64_t, 3> gdt{
        0,
        0x00CF9A000000FFFF, // Flat 32-bit ring-0 code segment, selector 0x10.
        0x00CF92000000FFFF, // Flat 32-bit ring-0 data segment, selector 0x18.
    };
    std::memcpy(guestRam + gdtOffset, gdt.data(), sizeof(gdt));
    constexpr std::uint64_t gdtGpa = guestRamBase + gdtOffset;

    const std::array<WHV_REGISTER_NAME, 17> registerNames{
        WHvX64RegisterRax,
        WHvX64RegisterRbx,
        WHvX64RegisterRbp,
        WHvX64RegisterRdi,
        WHvX64RegisterRsi,
        WHvX64RegisterRsp,
        WHvX64RegisterRip,
        WHvX64RegisterRflags,
        WHvX64RegisterGdtr,
        WHvX64RegisterCs,
        WHvX64RegisterDs,
        WHvX64RegisterEs,
        WHvX64RegisterSs,
        WHvX64RegisterCr0,
        WHvX64RegisterCr3,
        WHvX64RegisterCr4,
        WHvX64RegisterEfer,
    };

    std::array<WHV_REGISTER_VALUE, registerNames.size()> registerValues{};
    registerValues[0].Reg64 = 0;
    registerValues[1].Reg64 = 0; // EBX must be zero at the Linux 32-bit entry.
    registerValues[2].Reg64 = 0; // EBP
    registerValues[3].Reg64 = 0; // EDI
    registerValues[4].Reg64 = zeroPageGpa; // ESI points to boot_params.
    registerValues[5].Reg64 = guestRamBase + guestRamSize - 16;
    registerValues[6].Reg64 = guestRamBase;
    registerValues[7].Reg64 = 0x2; // IF=0 on entry.
    registerValues[8].Table.Base = gdtGpa;
    registerValues[8].Table.Limit = static_cast<USHORT>(sizeof(gdt) - 1);
    registerValues[9].Segment = MakeFlatSegment(0x10, 0xC09B);
    registerValues[10].Segment = MakeFlatSegment(0x18, 0xC093);
    registerValues[11].Segment = MakeFlatSegment(0x18, 0xC093);
    registerValues[12].Segment = MakeFlatSegment(0x18, 0xC093);
    registerValues[13].Reg64 = 0x11; // CR0.ET | CR0.PE: protected mode, paging off.
    registerValues[14].Reg64 = 0;
    registerValues[15].Reg64 = 0;
    registerValues[16].Reg64 = 0;

    partition.SetVirtualProcessorRegisters(0, registerNames, registerValues);

    std::string guestConsole;
    WHV_RUN_VP_EXIT_CONTEXT exitContext{};
    for (;;) {
        exitContext = partition.RunVirtualProcessor(0);
        if (exitContext.ExitReason == WHvRunVpExitReasonX64Halt) {
            break;
        }
        if (exitContext.ExitReason != WHvRunVpExitReasonX64IoPortAccess) {
            std::cerr << "Le vCPU s'est arrete avec le motif WHP 0x" << std::hex
                      << static_cast<unsigned long>(exitContext.ExitReason) << " au RIP 0x"
                      << exitContext.VpContext.Rip << ".\n";
            return 3;
        }

        const auto& io = exitContext.IoPortAccess;
        if (!io.AccessInfo.IsWrite || io.AccessInfo.StringOp ||
            io.AccessInfo.AccessSize != 1 || io.PortNumber != 0x00E9) {
            std::cerr << "Acces E/S invite inattendu: port=0x" << std::hex << io.PortNumber
                      << ", taille=" << std::dec << io.AccessInfo.AccessSize << ".\n";
            return 3;
        }
        guestConsole.push_back(static_cast<char>(io.Rax & 0xFF));

        const auto instructionLength = exitContext.VpContext.InstructionLength;
        if (instructionLength == 0 || instructionLength > 15) {
            throw std::runtime_error("WHP returned an invalid I/O instruction length.");
        }
        const WHV_REGISTER_NAME ripName = WHvX64RegisterRip;
        WHV_REGISTER_VALUE nextRip{};
        nextRip.Reg64 = exitContext.VpContext.Rip + instructionLength;
        partition.SetVirtualProcessorRegisters(
            0,
            std::span(&ripName, 1),
            std::span(&nextRip, 1));
    }

    constexpr std::array<WHV_REGISTER_NAME, 1> resultRegister{WHvX64RegisterRax};
    const auto result = partition.GetVirtualProcessorRegisters(0, resultRegister);
    const auto value = static_cast<std::uint16_t>(result.front().Reg64 & 0xFFFF);
    if (guestConsole != "B32" || value != 3) {
        std::cerr << "Le vCPU a atteint HLT, mais le resultat attendu n'a pas ete observe: 0x"
                  << std::hex << value << ", texte='" << guestConsole << "'.\n";
        return 4;
    }

    std::cout << "Sortie console invite (port 0xE9): " << guestConsole << "\n"
              << "Succes: le vCPU a execute du code 32 bits en mode protege via WHP.\n"
              << "E820 entries lues depuis ESI (boot_params): " << std::dec << value << ".\n"
              << "Ce test valide l'etat d'entree 32 bits du protocole Linux; il ne demarre pas encore le noyau.\n";
    return 0;
}

} // namespace

int main()
{
    try {
        return RunDemo();
    } catch (const std::exception& error) {
        std::cerr << "Echec du backend WHP: " << error.what() << "\n";
        return 1;
    }
}
