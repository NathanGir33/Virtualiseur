<#
.SYNOPSIS
  Assemble dist\LICENCES-TIERCES.txt : la licence de Virtualiseur et celles des
  bibliothèques redistribuées dans dist\ (vcpkg et MSYS2), texte intégral.
  Appelé par build.ps1 ; un texte introuvable est signalé, pas ignoré en silence.
#>
param(
    [string]$Dist = (Join-Path $PSScriptRoot '..\dist'),
    [string]$Vcpkg = (Join-Path $PSScriptRoot '..\vcpkg_installed'),
    [string]$Msys = 'C:\msys64\ucrt64'
)
$ErrorActionPreference = 'Stop'
$root = Join-Path $PSScriptRoot '..'

# Chaque entrée : DLL présente dans dist\, nom, version, licence, source, fichiers de licence (motifs).
$entries = @(
    @{ Dll = 'slirp-0.dll'; Name = 'libslirp'; Version = '4.9.1'; License = 'BSD-3-Clause'
       Source = 'https://gitlab.freedesktop.org/slirp/libslirp'
       Files = @("$Vcpkg\x64-windows\vcpkg\blds\libslirp\src\*\COPYRIGHT") },
    @{ Dll = 'glib-2.0-0.dll'; Name = 'GLib'; Version = '2.88.2'; License = 'LGPL-2.1-or-later'
       Source = 'https://gitlab.gnome.org/GNOME/glib (bibliothèque non modifiée, liée dynamiquement : elle peut être remplacée par une autre version compatible)'
       Files = @("$Vcpkg\x64-windows\vcpkg\blds\glib\src\*\LICENSES\LGPL-2.1-or-later.txt") },
    @{ Dll = 'pcre2-8.dll'; Name = 'PCRE2'; Version = '10.47'; License = 'BSD-3-Clause (avec exception PCRE2)'
       Source = 'https://github.com/PCRE2Project/pcre2'
       Files = @("$Vcpkg\vcpkg\blds\pcre2\src\*\LICENCE.md", "$Vcpkg\x64-windows\vcpkg\blds\pcre2\src\*\LICENCE.md") },
    @{ Dll = 'iconv-2.dll'; Name = 'GNU libiconv'; Version = '1.19'; License = 'LGPL-2.1-or-later'
       Source = 'https://www.gnu.org/software/libiconv/ (bibliothèque non modifiée, liée dynamiquement)'
       Files = @("$Vcpkg\x64-windows\share\libiconv\copyright") },
    @{ Dll = 'intl-8.dll'; Name = 'GNU gettext (libintl)'; Version = '0.22.5'; License = 'LGPL-2.1-or-later'
       Source = 'https://www.gnu.org/software/gettext/ (bibliothèque non modifiée, liée dynamiquement)'
       Files = @("$Vcpkg\x64-windows\share\gettext-libintl\copyright") },
    @{ Dll = 'libvirglrenderer-1.dll'; Name = 'virglrenderer'; Version = '1.3.0'; License = 'MIT'
       Source = 'https://gitlab.freedesktop.org/virgl/virglrenderer'
       Files = @("$Msys\share\licenses\virglrenderer\COPYING") },
    @{ Dll = 'libepoxy-0.dll'; Name = 'libepoxy'; Version = '1.5.10'; License = 'MIT'
       Source = 'https://github.com/anholt/libepoxy'
       Files = @("$Msys\share\licenses\libepoxy\COPYING") },
    @{ Dll = 'libwinpthread-1.dll'; Name = 'winpthreads (MinGW-w64)'; Version = ''; License = 'MIT et BSD'
       Source = 'https://www.mingw-w64.org'
       Files = @("$Msys\share\licenses\libwinpthread\COPYING") },
    @{ Dll = 'libgcc_s_seh-1.dll'; Name = 'Bibliothèque d''exécution de GCC (libgcc)'; Version = ''; License = 'GPL-3.0 avec l''exception GCC Runtime Library'
       Source = 'https://gcc.gnu.org'
       Files = @("$Msys\share\licenses\libgcc\COPYING.RUNTIME", "$Msys\share\licenses\libgcc\COPYING3") }
)

$rule = '=' * 78
$text = New-Object System.Text.StringBuilder
[void]$text.AppendLine('Virtualiseur : licences')
[void]$text.AppendLine($rule)
[void]$text.AppendLine()
[void]$text.AppendLine('Virtualiseur est publié sous licence MIT (texte ci-dessous). Il redistribue les')
[void]$text.AppendLine('bibliothèques suivantes, sans modification, chacune sous sa propre licence.')
[void]$text.AppendLine('Les fichiers vcruntime*/msvcp*/concrt*/vccorlib* sont le runtime Visual C++ de')
[void]$text.AppendLine('Microsoft, redistribué selon les conditions de Visual Studio.')
[void]$text.AppendLine()
$missing = @()
$sections = New-Object System.Text.StringBuilder
foreach ($entry in $entries) {
    if (-not (Test-Path (Join-Path $Dist $entry.Dll))) { continue }
    $version = if ($entry.Version) { ' ' + $entry.Version } else { '' }
    [void]$text.AppendLine(("- {0}{1} ({2}) : {3}" -f $entry.Name, $version, $entry.Dll, $entry.License))
    [void]$sections.AppendLine($rule)
    [void]$sections.AppendLine(("{0}{1} - {2}" -f $entry.Name, $version, $entry.License))
    [void]$sections.AppendLine(("Fichier : {0}" -f $entry.Dll))
    [void]$sections.AppendLine(("Sources : {0}" -f $entry.Source))
    [void]$sections.AppendLine($rule)
    $found = $false
    foreach ($pattern in $entry.Files) {
        $file = Get-ChildItem $pattern -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($file) {
            [void]$sections.AppendLine()
            [void]$sections.AppendLine((Get-Content $file.FullName -Raw -Encoding UTF8).TrimEnd())
            $found = $true
        }
    }
    if (-not $found) { $missing += $entry.Name }
    [void]$sections.AppendLine()
}
[void]$text.AppendLine()
[void]$text.AppendLine($rule)
[void]$text.AppendLine('Virtualiseur - MIT')
[void]$text.AppendLine($rule)
[void]$text.AppendLine()
[void]$text.AppendLine((Get-Content (Join-Path $root 'LICENSE') -Raw -Encoding UTF8).TrimEnd())
[void]$text.AppendLine()
[void]$text.Append($sections.ToString())

$target = Join-Path $Dist 'LICENCES-TIERCES.txt'
[IO.File]::WriteAllText($target, $text.ToString().Replace("`r`n", "`n").Replace("`n", "`r`n"), [Text.UTF8Encoding]::new($true))
if ($missing.Count -gt 0) {
    Write-Host ('Licences introuvables pour : ' + ($missing -join ', ')) -ForegroundColor Yellow
}
Write-Host "Licences tierces : $target"
