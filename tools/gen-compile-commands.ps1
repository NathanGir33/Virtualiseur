<#
.SYNOPSIS
  Génère build-clangd\compile_commands.json pour clangd à partir des projets Visual Studio.

.DESCRIPTION
  Virtualiseur se compile avec MSBuild (.vcxproj), pas avec CMake. Ce script lit chaque
  projet (sources, dossiers d'en-têtes, macros, norme C++, options) et écrit la commande
  clang-cl équivalente pour chaque fichier .cpp. Les dossiers système (MSVC, Windows SDK)
  sont ceux d'un shell initialisé par vcvars64.bat. Le build Visual Studio (out\, obj\,
  dist\) n'est pas touché : seul build-clangd\ est écrit.

  À relancer après l'ajout d'un fichier .cpp à un projet.

.EXAMPLE
  .\tools\gen-compile-commands.ps1
  .\tools\gen-compile-commands.ps1 -Configuration Release
#>
param([ValidateSet('Debug', 'Release')] [string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$output = Join-Path $root 'build-clangd'

# Visual Studio et son environnement x64 (vcvars64.bat).
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe introuvable : Visual Studio est-il installé ?' }
$vs = & $vswhere -latest -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat introuvable dans $vs (charge de travail C++ absente ?)." }

$projects = @(
    'src\backend\whp\VirtualiseurWHPBackend.vcxproj',
    'src\vmm\VirtualiseurVM.vcxproj',
    'src\app\VirtualiseurManager\VirtualiseurManager.vcxproj'
) | ForEach-Object { Join-Path $root $_ }

# Même Windows SDK que les projets.
$sdk = ([xml](Get-Content $projects[0] -Raw)).Project.PropertyGroup |
    Where-Object { $_.WindowsTargetPlatformVersion } | Select-Object -First 1 -ExpandProperty WindowsTargetPlatformVersion
$environment = cmd /c "`"$vcvars`" $sdk >nul 2>&1 && set INCLUDE"
$includeLine = $environment | Where-Object { $_ -like 'INCLUDE=*' } | Select-Object -First 1
if (-not $includeLine) { throw "vcvars64.bat n'a pas fourni de variable INCLUDE (SDK $sdk installé ?)." }
$systemIncludes = $includeLine.Substring(8).Split(';') | Where-Object { $_ -and (Test-Path $_) }

# clang-cl (mode MSVC de clang) : clangd déduit le dialecte du nom du compilateur.
$compiler = @(
    (Join-Path ${env:ProgramFiles} 'LLVM\bin\clang-cl.exe'),
    (Join-Path $vs 'VC\Tools\Llvm\x64\bin\clang-cl.exe')
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $compiler) { $compiler = 'clang-cl.exe' }

$properties = @{ Configuration = $Configuration; Platform = 'x64'; EnableSlirp = 'true' }

function Expand-Value([string]$text, [string]$projectDir) {
    $text = $text.Replace('$(ProjectDir)', $projectDir)
    foreach ($name in $properties.Keys) { $text = $text.Replace("`$($name)", $properties[$name]) }
    return $text
}

# Conditions MSBuild de ces projets : une seule comparaison 'a'=='b' ou 'a'!='b'.
function Test-Condition([string]$condition, [string]$projectDir) {
    if ([string]::IsNullOrWhiteSpace($condition)) { return $true }
    $expanded = Expand-Value $condition $projectDir
    if ($expanded -match "^\s*'([^']*)'\s*(==|!=)\s*'([^']*)'\s*$") {
        $equal = $Matches[1] -eq $Matches[3]
        return ($Matches[2] -eq '==') -eq $equal
    }
    return $false # Condition non gérée : ignorée.
}

function Split-List([string]$value) {
    if (-not $value) { return @() }
    return $value.Split(';') | ForEach-Object { $_.Trim() } | Where-Object { $_ -and $_ -notmatch '^%\(' }
}

$entries = New-Object System.Collections.Generic.List[object]
foreach ($project in $projects) {
    $projectDir = (Split-Path $project -Parent) + '\'
    $xml = [xml](Get-Content $project -Raw)
    $defines = New-Object System.Collections.Generic.List[string]
    $includes = New-Object System.Collections.Generic.List[string]
    $options = New-Object System.Collections.Generic.List[string]
    $standard = '/std:c++20'
    $unicode = $false

    foreach ($group in $xml.Project.PropertyGroup) {
        if ($group.Label -eq 'Configuration' -and (Test-Condition $group.Condition $projectDir) -and
            $group.CharacterSet -eq 'Unicode') {
            $unicode = $true
        }
    }
    foreach ($group in $xml.Project.ItemDefinitionGroup) {
        if (-not (Test-Condition $group.Condition $projectDir)) { continue }
        foreach ($clCompile in @($group.ClCompile)) {
            if (-not $clCompile -or -not (Test-Condition $clCompile.Condition $projectDir)) { continue }
            Split-List $clCompile.PreprocessorDefinitions | ForEach-Object { $defines.Add($_) }
            Split-List $clCompile.AdditionalIncludeDirectories | ForEach-Object {
                $includes.Add([System.IO.Path]::GetFullPath((Expand-Value $_ $projectDir)))
            }
            if ($clCompile.AdditionalOptions) {
                $clCompile.AdditionalOptions.Split(' ') | Where-Object { $_ -and $_ -notmatch '^%\(' } |
                    ForEach-Object { $options.Add($_) }
            }
            switch ($clCompile.LanguageStandard) {
                'stdcpp17' { $standard = '/std:c++17' }
                'stdcpp20' { $standard = '/std:c++20' }
                'stdcpplatest' { $standard = '/std:c++latest' }
            }
        }
    }
    if ($unicode) { $defines.Add('UNICODE'); $defines.Add('_UNICODE') }
    $defines.Add($(if ($Configuration -eq 'Debug') { '_DEBUG' } else { 'NDEBUG' }))
    # Intégration vcpkg en mode manifeste (libslirp pour le VMM).
    if ($xml.OuterXml -match 'VcpkgTriplet') {
        $vcpkgInclude = Join-Path $root 'vcpkg_installed\x64-windows\x64-windows\include'
        if (Test-Path $vcpkgInclude) { $includes.Add($vcpkgInclude) }
    }

    $flags = New-Object System.Collections.Generic.List[string]
    $flags.AddRange([string[]]@('/nologo', '/EHsc', $standard))
    $flags.AddRange([string[]]$options)
    foreach ($define in $defines) { $flags.Add("/D$define") }
    foreach ($include in ($includes | Select-Object -Unique)) { $flags.Add('/I'); $flags.Add($include) }
    foreach ($include in $systemIncludes) { $flags.Add('/imsvc'); $flags.Add($include) }

    foreach ($item in $xml.Project.ItemGroup.ClCompile) {
        if (-not $item.Include) { continue }
        $file = [System.IO.Path]::GetFullPath((Join-Path $projectDir $item.Include))
        $arguments = New-Object System.Collections.Generic.List[string]
        $arguments.Add($compiler)
        $arguments.AddRange($flags)
        $arguments.AddRange([string[]]@('/c', $file))
        $entries.Add([ordered]@{ directory = $projectDir.TrimEnd('\'); file = $file; arguments = $arguments.ToArray() })
    }
}

New-Item -ItemType Directory -Force $output | Out-Null
$json = ConvertTo-Json -InputObject $entries.ToArray() -Depth 5
[System.IO.File]::WriteAllText((Join-Path $output 'compile_commands.json'), $json, (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("compile_commands.json : {0} fichiers, Windows SDK {1}, {2} dossiers système, compilateur {3}" -f `
    $entries.Count, $sdk, @($systemIncludes).Count, $compiler) -ForegroundColor Green
