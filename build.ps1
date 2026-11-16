<#
.SYNOPSIS
  Compile Virtualiseur (backend WHP, VMM, gestionnaire) et assemble dist\.

.EXAMPLE
  .\build.ps1                 # Release, avec le NAT libslirp (vcpkg)
  .\build.ps1 -NoNetwork      # Sans libslirp : aucun vcpkg requis
  .\build.ps1 -Installer      # Puis installer\Output\Virtualiseur-Setup.exe (Inno Setup 6)
  .\build.ps1 -Configuration Debug
#>
param(
    [ValidateSet('Release', 'Debug')] [string]$Configuration = 'Release',
    [switch]$NoNetwork,
    [switch]$Installer
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = $null
if (Test-Path $vswhere) {
    $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
}
if (-not $msbuild) { $msbuild = (Get-Command msbuild.exe -ErrorAction SilentlyContinue).Source }
if (-not $msbuild) { throw 'MSBuild introuvable : installe Visual Studio avec la charge de travail C++.' }

$slirp = if ($NoNetwork) { 'false' } else { 'true' }
$common = @('/m', '/nologo', '/v:minimal', "/p:Configuration=$Configuration", '/p:Platform=x64')

Write-Host "== VMM (VirtualiseurVM.exe, réseau NAT: $slirp)" -ForegroundColor Cyan
& $msbuild "$root\src\vmm\VirtualiseurVM.vcxproj" @common "/p:EnableSlirp=$slirp"
if ($LASTEXITCODE -ne 0) { throw 'Échec de compilation du VMM.' }

Write-Host '== Gestionnaire (VirtualiseurManager.exe)' -ForegroundColor Cyan
& $msbuild "$root\src\app\VirtualiseurManager\VirtualiseurManager.vcxproj" @common
if ($LASTEXITCODE -ne 0) { throw 'Échec de compilation du gestionnaire.' }

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item "$root\src\app\VirtualiseurManager\out\x64\$Configuration\VirtualiseurManager.exe" $dist -Force
Copy-Item "$root\src\vmm\out\x64\$Configuration\VirtualiseurVM.exe" $dist -Force
# Accélération 3D : virglrenderer et ses dépendances (MSYS2 UCRT64), chargés
# dynamiquement par le VMM. Sans elles, la VM reste en virtio-gpu 2D.
$msys = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { 'C:\msys64' }
$virglDlls = 'libvirglrenderer-1.dll', 'libepoxy-0.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll'
if (Test-Path "$msys\ucrt64\bin\libvirglrenderer-1.dll") {
    foreach ($dll in $virglDlls) {
        Copy-Item "$msys\ucrt64\bin\$dll" "$root\src\vmm\out\x64\$Configuration\" -Force
    }
} else {
    Write-Host "virglrenderer absent ($msys\ucrt64) : pas d'accélération 3D. Installe-le avec :" -ForegroundColor Yellow
    Write-Host "  pacman -S mingw-w64-ucrt-x86_64-virglrenderer   (shell MSYS2 UCRT64)" -ForegroundColor Yellow
}
# DLL de libslirp/glib (vcpkg) et de virglrenderer déployées à côté du VMM.
Get-ChildItem "$root\src\vmm\out\x64\$Configuration\*.dll" -ErrorAction SilentlyContinue | Copy-Item -Destination $dist -Force
Copy-Item "$root\docs\preseed-debian.cfg" $dist -Force

# Runtime Visual C++ déployé à côté des programmes (déploiement « app-local »
# autorisé par Microsoft) : dist\ fonctionne sur un Windows sans le
# Redistributable Visual C++.
if ($Configuration -eq 'Release' -and (Test-Path $vswhere)) {
    $vs = & $vswhere -latest -property installationPath
    $crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if ($crt) {
        Copy-Item "$($crt.FullName)\*.dll" $dist -Force
    } else {
        Write-Host 'Runtime Visual C++ introuvable : dist\ demandera le Redistributable sur les autres PC.' -ForegroundColor Yellow
    }
}
Write-Host "Terminé : $dist" -ForegroundColor Green

if ($Installer) {
    $iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
              "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
              "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) { throw 'Inno Setup 6 introuvable : winget install JRSoftware.InnoSetup' }
    Write-Host '== Installateur (Inno Setup)' -ForegroundColor Cyan
    & $iscc /Q "$root\installer\Virtualiseur.iss"
    if ($LASTEXITCODE -ne 0) { throw "Échec de la compilation de l'installateur." }
    Write-Host "Installateur : $root\installer\Output\Virtualiseur-Setup.exe" -ForegroundColor Green
}
