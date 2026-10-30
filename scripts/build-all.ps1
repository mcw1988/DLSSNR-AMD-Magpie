# 一键构建本仓库的三个组件（以及可选的上游 amd-nr.addon64）。
#
# 需要：VS 2022 Build Tools（VC Tools x64）+ Windows SDK。
# 用法：powershell -ExecutionPolicy Bypass -File scripts/build-all.ps1
#       powershell -ExecutionPolicy Bypass -File scripts/build-all.ps1 -SkipUpstream
param(
    [switch]$SkipUpstream
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$dist = Join-Path $repo 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null

# --- 工具链定位 -------------------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe 不存在：先装 VS 2022 Build Tools（VC Tools x64）' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
    Select-Object -First 1
if (-not $vs) { throw '找不到带 C++ 工具集的 Visual Studio' }

$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
if (-not (Test-Path $cl)) { throw "cl.exe 不存在: $cl" }

$sdkRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\Windows\v10.0' -ErrorAction SilentlyContinue).InstallationFolder
if (-not $sdkRoot) { throw '找不到 Windows SDK（注册表 v10.0）' }
$sdkVer = Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName 'um\windows.h') } |
    Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name

$reshadeInclude = @(
    (Join-Path $repo 'upstream\reshade\include')
    (Join-Path $repo 'upstream\dlss5-neural-amd\3rdparty\reshade')
) | Where-Object { Test-Path $_ } | Select-Object -First 1

$env:INCLUDE = @(
    (Join-Path $msvc.FullName 'include')
    (Join-Path $sdkRoot "Include\$sdkVer\ucrt")
    (Join-Path $sdkRoot "Include\$sdkVer\um")
    (Join-Path $sdkRoot "Include\$sdkVer\shared")
) + @($reshadeInclude | Where-Object { $_ }) -join ';'
$env:LIB = @(
    (Join-Path $msvc.FullName 'lib\x64')
    (Join-Path $sdkRoot "Lib\$sdkVer\ucrt\x64")
    (Join-Path $sdkRoot "Lib\$sdkVer\um\x64")
) -join ';'

Write-Host "MSVC $($msvc.Name)   SDK $sdkVer"
if (-not $reshadeInclude) { Write-Host '警告：没有 ReShade 头文件，overlay-input 会跳过（先跑 scripts/fetch-upstream.ps1）' -ForegroundColor Yellow }

$common = @('/nologo', '/utf-8', '/std:c++20', '/EHsc', '/O2', '/MT', '/W3', '/DNDEBUG',
            '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/D_CRT_SECURE_NO_WARNINGS')

# --- nr-router --------------------------------------------------------------
Push-Location (Join-Path $repo 'src\nr-router')
try {
    & $cl @common nr_router.cpp /link /SUBSYSTEM:CONSOLE /OUT:"$dist\nr-router.exe" user32.lib
    if ($LASTEXITCODE -ne 0) { throw "nr-router 构建失败 ($LASTEXITCODE)" }
} finally { Pop-Location }
Write-Host 'OK nr-router.exe'

# --- nrtest -----------------------------------------------------------------
Push-Location (Join-Path $repo 'src\nrtest')
try {
    & $cl @common main.cpp /link /SUBSYSTEM:CONSOLE /OUT:"$dist\nrtest.exe" d3d12.lib dxgi.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw "nrtest 构建失败 ($LASTEXITCODE)" }
} finally { Pop-Location }
Write-Host 'OK nrtest.exe'

# --- overlay-input ----------------------------------------------------------
if ($reshadeInclude) {
    Push-Location (Join-Path $repo 'src\overlay-input')
    try {
        & $cl @common /LD overlay_input.cpp /link /DLL /OUT:"$dist\overlay-input.addon64" user32.lib
        if ($LASTEXITCODE -ne 0) { throw "overlay-input 构建失败 ($LASTEXITCODE)" }
    } finally { Pop-Location }
    Write-Host 'OK overlay-input.addon64'
}

# --- 上游宿主（可选） -------------------------------------------------------
$upstream = Join-Path $repo 'upstream\dlss5-neural-amd'
if (-not $SkipUpstream -and (Test-Path (Join-Path $upstream 'build.ps1'))) {
    Push-Location $upstream
    try {
        powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $upstream 'build.ps1') -Target neural
        if ($LASTEXITCODE -ne 0) { throw "上游 add-on 构建失败 ($LASTEXITCODE)" }
        Copy-Item (Join-Path $upstream 'build\amd-nr.addon64') (Join-Path $dist 'amd-nr.addon64') -Force
    } finally { Pop-Location }
    Write-Host 'OK amd-nr.addon64（上游 OpenNR 源码构建）'
} elseif (-not $SkipUpstream) {
    Write-Host '跳过上游 add-on：upstream\dlss5-neural-amd 不存在（scripts/fetch-upstream.ps1）' -ForegroundColor Yellow
}

Write-Host ''
Get-ChildItem $dist -File | Select-Object Name, Length, LastWriteTime
