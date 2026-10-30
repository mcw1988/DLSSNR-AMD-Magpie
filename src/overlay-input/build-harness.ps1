$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$sdk = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\Windows\v10.0').InstallationFolder
$sdkVer = Get-ChildItem (Join-Path $sdk 'Include') -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'um\windows.h') } | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
$env:INCLUDE = @((Join-Path $msvc.FullName 'include'), (Join-Path $sdk "Include\$sdkVer\ucrt"), (Join-Path $sdk "Include\$sdkVer\um"), (Join-Path $sdk "Include\$sdkVer\shared")) -join ';'
$env:LIB = @((Join-Path $msvc.FullName 'lib\x64'), (Join-Path $sdk "Lib\$sdkVer\ucrt\x64"), (Join-Path $sdk "Lib\$sdkVer\um\x64")) -join ';'
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
Push-Location $root
try {
    & $cl /nologo /utf-8 /std:c++20 /EHsc /O2 /MT /W3 /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN `
        harness.cpp /link /SUBSYSTEM:WINDOWS /OUT:harness.exe d3d11.lib dxgi.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }
} finally { Pop-Location }
Get-Item (Join-Path $root 'harness.exe') | Select-Object Name, Length
