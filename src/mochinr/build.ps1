# mochinr.addon64 - the mochizuki network inside the FFX upscale slot.
#
# Needs: VS 2022 Build Tools (VC Tools x64), Windows SDK, and two header sets:
#   - MinHook   (include/MinHook.h + src/*.c)
#   - ReShade   (reshade.hpp; the "with full add-on support" SDK)
#
# Sources are searched, in this order:
#   1. -MinHookDir / -ReshadeIncludeDir
#   2. $env:MINHOOK_DIR / $env:RESHADE_INCLUDE_DIR
#   3. <repo>\upstream\minhook, <repo>\upstream\reshade\include,
#      <repo>\upstream\dlss5-neural-amd\3rdparty\{minhook,reshade}
#      (scripts/fetch-upstream.ps1 puts them there)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File src\mochinr\build.ps1
#   powershell -ExecutionPolicy Bypass -File src\mochinr\build.ps1 -OutFile D:\Magpie-NR\mochinr.addon64
param(
    [string]$MinHookDir,
    [string]$ReshadeIncludeDir,
    [string]$OutFile,
    [switch]$Map     # 出 .map：崩溃偏移对函数用（WER 的 fault offset 直接查表）
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$repo = Split-Path (Split-Path $root -Parent) -Parent
if (-not $OutFile) {
    $dist = Join-Path $repo 'dist'
    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    $OutFile = Join-Path $dist 'mochinr.addon64'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found: install VS 2022 Build Tools (VC Tools x64)' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
    Select-Object -First 1
if (-not $vs) { throw 'no Visual Studio with the C++ toolset' }
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1

$sdkRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\Windows\v10.0' -ErrorAction SilentlyContinue).InstallationFolder
if (-not $sdkRoot) { throw 'Windows SDK not found (registry v10.0)' }
$sdkVer = Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName 'um\windows.h') } |
    Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name

if (-not $MinHookDir) { $MinHookDir = $env:MINHOOK_DIR }
if (-not $MinHookDir) {
    $MinHookDir = @(
        (Join-Path $repo 'upstream\minhook')
        (Join-Path $repo 'upstream\dlss5-neural-amd\3rdparty\minhook')
    ) | Where-Object { Test-Path (Join-Path $_ 'include\MinHook.h') } | Select-Object -First 1
}
if (-not $MinHookDir -or -not (Test-Path (Join-Path $MinHookDir 'include\MinHook.h'))) {
    throw 'MinHook not found: pass -MinHookDir or run scripts/fetch-upstream.ps1'
}

if (-not $ReshadeIncludeDir) { $ReshadeIncludeDir = $env:RESHADE_INCLUDE_DIR }
if (-not $ReshadeIncludeDir) {
    $ReshadeIncludeDir = @(
        (Join-Path $repo 'upstream\reshade\include')
        (Join-Path $repo 'upstream\dlss5-neural-amd\3rdparty\reshade')
    ) | Where-Object { Test-Path (Join-Path $_ 'reshade.hpp') } | Select-Object -First 1
}
if (-not $ReshadeIncludeDir -or -not (Test-Path (Join-Path $ReshadeIncludeDir 'reshade.hpp'))) {
    throw 'ReShade headers not found: pass -ReshadeIncludeDir or run scripts/fetch-upstream.ps1'
}

$env:INCLUDE = @(
    (Join-Path $msvc.FullName 'include')
    (Join-Path $sdkRoot "Include\$sdkVer\ucrt")
    (Join-Path $sdkRoot "Include\$sdkVer\um")
    (Join-Path $sdkRoot "Include\$sdkVer\shared")
    (Join-Path $MinHookDir 'include')
    (Join-Path $MinHookDir 'src')
    $ReshadeIncludeDir
) -join ';'
$env:LIB = @(
    (Join-Path $msvc.FullName 'lib\x64')
    (Join-Path $sdkRoot "Lib\$sdkVer\ucrt\x64")
    (Join-Path $sdkRoot "Lib\$sdkVer\um\x64")
) -join ';'
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'

Write-Host "MSVC $($msvc.Name)  SDK $sdkVer"
Write-Host "MinHook  $MinHookDir"
Write-Host "ReShade  $ReshadeIncludeDir"

Push-Location $root
try {
    & $cl /nologo /utf-8 /std:c++20 /EHsc /O2 /MT /W3 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX `
        mochinr.cpp `
        (Join-Path $MinHookDir 'src\hook.c') (Join-Path $MinHookDir 'src\trampoline.c') `
        (Join-Path $MinHookDir 'src\buffer.c') (Join-Path $MinHookDir 'src\hde\hde64.c') `
        /link $(if ($Map) { '/MAP' }) /DLL /OUT:"$OutFile" d3d12.lib dxgi.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }
} finally { Pop-Location }

Get-Item $OutFile | Select-Object FullName, Length
