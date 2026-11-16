#pragma once

#include "LinuxLoader.h"

#include "Iso9660Image.h"
#include "VirtualDisk.h"

#include <optional>
#include <string>

namespace vmm {

struct BootSelection {
    LinuxImage image;
    std::string description;
};

// Locates the kernel/initrd of a Linux installation or live ISO and returns
// them with the kernel arguments the distribution expects.
std::optional<BootSelection> FindIsoBoot(const virtualiseur::whp::Iso9660Image& iso, bool graphicalInstaller);

// Locates an installed Linux system on a raw disk (MBR or GPT, ext2/3/4):
// reads /boot/grub/grub.cfg (first menu entry) or the /vmlinuz and
// /initrd.img symlinks, and returns the kernel, initrd and command line.
std::optional<BootSelection> FindDiskBoot(const virtualiseur::whp::VirtualDisk& disk);

} // namespace vmm
