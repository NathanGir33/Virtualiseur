<#
.SYNOPSIS
  Builds dist\VirtualiseurGuestTools.iso (ISO 9660 + Joliet, volume
  VIRTUALISEUR) from src\guest-tools, with IMAPI2 (part of Windows, no extra
  tool). Called by build.ps1.
#>
param(
    [string]$Source = (Join-Path $PSScriptRoot '..\src\guest-tools'),
    [string]$Target = (Join-Path $PSScriptRoot '..\dist\VirtualiseurGuestTools.iso')
)
$ErrorActionPreference = 'Stop'

# IStream -> file (IMAPI2 hands the image over as a COM stream).
if (-not ('GuestToolsIso.StreamCopy' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
namespace GuestToolsIso {
    public static class StreamCopy {
        public static void ToFile(object comStream, string path) {
            var stream = (IStream)comStream;
            var buffer = new byte[1 << 16];
            var read = Marshal.AllocHGlobal(sizeof(int));
            try {
                using (var file = File.Create(path)) {
                    for (;;) {
                        stream.Read(buffer, buffer.Length, read);
                        int count = Marshal.ReadInt32(read);
                        if (count <= 0) break;
                        file.Write(buffer, 0, count);
                    }
                }
            } finally {
                Marshal.FreeHGlobal(read);
            }
        }
    }
}
'@
}

$Source = (Resolve-Path $Source).Path
foreach ($file in Get-ChildItem $Source -File) {
    if ($file.Extension -in '.sh', '.txt') {
        $bytes = [IO.File]::ReadAllBytes($file.FullName)
        if ([Array]::IndexOf($bytes, [byte]13) -ge 0) { throw "$($file.Name) contient des fins de ligne CRLF : il doit être en LF pour Linux." }
    }
}

$image = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
$image.FileSystemsToCreate = 3 # ISO 9660 + Joliet (long, case-preserving names under Linux).
$image.VolumeName = 'VIRTUALISEUR'
$image.Root.AddTree($Source, $false)
$result = $image.CreateResultImage()
$temporary = "$Target.tmp"
[GuestToolsIso.StreamCopy]::ToFile($result.ImageStream, $temporary)
Move-Item -Force $temporary $Target
[void][Runtime.InteropServices.Marshal]::ReleaseComObject($image)
Write-Host ("Outils invité : {0} ({1:N0} Ko)" -f $Target, ((Get-Item $Target).Length / 1KB))
