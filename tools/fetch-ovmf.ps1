<#
.SYNOPSIS
  Downloads the UEFI firmware (OVMF, EDK2) from the Debian package at a
  pinned version, checks it (SHA-256 of the package and of each file) and
  copies it to <Target>: OVMF_CODE_4M.fd (without Secure Boot nor SMM),
  OVMF_VARS_4M.fd (empty variable store, template of each VM's vars.fd)
  and the package's copyright file. Nothing is stored in git: the package
  is cached in third_party\ovmf. Called by build.ps1.
#>
param(
    [string]$Target = (Join-Path $PSScriptRoot '..\dist\firmware'),
    [string]$Cache = (Join-Path $PSScriptRoot '..\third_party\ovmf')
)
$ErrorActionPreference = 'Stop'

# Debian 13 (trixie), security-maintained. Variant: "without Secure Boot,
# optional SMM" (usr/share/qemu/firmware/60-edk2-x86_64.json, machines
# pc-i440fx-* and pc-q35-*).
$version = '2025.02-8+deb13u1'
$package = "ovmf_${version}_all.deb"
$url = "https://deb.debian.org/debian/pool/main/e/edk2/$package"
$packageHash = '78E0D54DF11FC77406CB7A0BC9A39E5BCA6D1CBE06556B91D9A73491C52DECDF'
$files = [ordered]@{
    'usr/share/OVMF/OVMF_CODE_4M.fd' = '624E06DE18B4FA535E90DB7160D00D3D07D206422B89999BF1E27D920264E4E0'
    'usr/share/OVMF/OVMF_VARS_4M.fd' = '5D2AC383371B408398ACCEE7EC27C8C09EA5B74A0DE0CEEA6513388B15BE5D1E'
}

function Hash([string]$path) { (Get-FileHash -Algorithm SHA256 $path).Hash }

New-Item -ItemType Directory -Force $Cache, $Target | Out-Null
$deb = Join-Path $Cache $package
if (-not (Test-Path $deb) -or (Hash $deb) -ne $packageHash) {
    Write-Host "OVMF : téléchargement de $package"
    $temporary = "$deb.tmp"
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $temporary
    if ((Hash $temporary) -ne $packageHash) {
        Remove-Item $temporary
        throw "OVMF : somme SHA-256 du paquet inattendue ($url)."
    }
    Move-Item -Force $temporary $deb
}

# The Windows tar (bsdtar) reads .deb (ar) and .tar.xz archives.
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
$work = Join-Path $Cache 'extract'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $work | Out-Null
& $tar -xf $deb -C $work data.tar.xz
if ($LASTEXITCODE -ne 0) { throw 'OVMF : extraction du paquet impossible.' }
$members = @($files.Keys | ForEach-Object { "./$_" }) + './usr/share/doc/ovmf/copyright'
& $tar -xf (Join-Path $work 'data.tar.xz') -C $work @members
if ($LASTEXITCODE -ne 0) { throw 'OVMF : extraction des fichiers du firmware impossible.' }

foreach ($entry in $files.GetEnumerator()) {
    $source = Join-Path $work $entry.Key
    if ((Hash $source) -ne $entry.Value) { throw "OVMF : somme SHA-256 inattendue pour $($entry.Key)." }
    Copy-Item -Force $source (Join-Path $Target (Split-Path $entry.Key -Leaf))
}
Copy-Item -Force (Join-Path $work 'usr/share/doc/ovmf/copyright') (Join-Path $Target 'OVMF-copyright.txt')
Remove-Item -Recurse -Force $work
Write-Host "OVMF $version : $Target"
