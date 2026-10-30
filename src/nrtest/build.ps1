$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$repo = Split-Path (Split-Path $root -Parent) -Parent
$dist = Join-Path $repo 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vs) { throw 'no VS with C++ tools' }
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$sdk = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\Windows\v10.0').InstallationFolder
$sdkVer = Get-ChildItem (Join-Path $sdk 'Include') -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'um\windows.h') } | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name

$env:INCLUDE = @(
    (Join-Path $msvc.FullName 'include')
    (Join-Path $sdk "Include\$sdkVer\ucrt")
    (Join-Path $sdk "Include\$sdkVer\um")
    (Join-Path $sdk "Include\$sdkVer\shared")
    $root
) -join ';'
$env:LIB = @(
    (Join-Path $msvc.FullName 'lib\x64')
    (Join-Path $sdk "Lib\$sdkVer\ucrt\x64")
    (Join-Path $sdk "Lib\$sdkVer\um\x64")
) -join ';'

Write-Host "MSVC $($msvc.Name)  SDK $sdkVer"
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$common = @('/nologo', '/utf-8', '/std:c++20', '/EHsc', '/O2', '/MT', '/W3', '/DNDEBUG',
            '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN')
Push-Location $root
try {
    # 冒烟测试：跑一帧，看运行时能不能用
    & $cl @common main.cpp /link /SUBSYSTEM:CONSOLE /OUT:"$dist\nrtest.exe" d3d12.lib dxgi.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw "nrtest compile failed ($LASTEXITCODE)" }

    # 帧成本基准：跑 N 帧，分别统计 prepare / 生产拷贝 / 网络 / 消费拷贝
    & $cl @common nrbench.cpp /link /SUBSYSTEM:CONSOLE /OUT:"$dist\nrbench.exe" d3d12.lib dxgi.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw "nrbench compile failed ($LASTEXITCODE)" }
} finally { Pop-Location }

Get-ChildItem (Join-Path $dist 'nrtest.exe'), (Join-Path $dist 'nrbench.exe') |
    Select-Object Name, Length
