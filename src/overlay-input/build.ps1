$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$repo = Split-Path (Split-Path $root -Parent) -Parent
$vendor = @(
    (Join-Path $repo 'upstream\reshade\include'),
    (Join-Path $repo 'upstream\dlss5-neural-amd\3rdparty\reshade')
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vendor) { throw 'Reshade headers not found: run scripts/fetch-upstream.ps1 first' }
$dist = Join-Path $repo 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vs) { throw 'no VS with C++ tools' }
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$sdk = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\Windows\v10.0').InstallationFolder
$sdkVer = Get-ChildItem (Join-Path $sdk 'Include') -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'um\windows.h') } | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name

$env:INCLUDE = @(
    (Join-Path $msvc.FullName 'include')
    (Join-Path $sdk "Include\$sdkVer\ucrt")
    (Join-Path $sdk "Include\$sdkVer\um")
    (Join-Path $sdk "Include\$sdkVer\shared")
    $vendor
) -join ';'
$env:LIB = @(
    (Join-Path $msvc.FullName 'lib\x64')
    (Join-Path $sdk "Lib\$sdkVer\ucrt\x64")
    (Join-Path $sdk "Lib\$sdkVer\um\x64")
) -join ';'

Write-Host "MSVC $($msvc.Name)  SDK $sdkVer"
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
Push-Location $root
try {
    & $cl /nologo /utf-8 /std:c++20 /EHsc /O2 /MT /W3 /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN /LD `
        overlay_input.cpp /link /DLL /OUT:"$dist\overlay-input.addon64" user32.lib
    if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }
} finally { Pop-Location }
Get-Item (Join-Path $dist 'overlay-input.addon64') | Select-Object Name, Length
