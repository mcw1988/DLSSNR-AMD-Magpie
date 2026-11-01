$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vs) { throw 'no VS with C++ tools' }
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$sdk = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\Windows\v10.0').InstallationFolder
$sdkVer = Get-ChildItem (Join-Path $sdk 'Include') -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'um\windows.h') } | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name

$mh = 'C:\Users\ma\WorkBuddy\workagent\opennr-src\3rdparty\minhook'
$reshade = 'C:\Users\ma\WorkBuddy\workagent\opennr-src\3rdparty\reshade'
if (-not (Test-Path (Join-Path $reshade 'reshade.hpp'))) {
    $reshade = 'C:\Users\ma\WorkBuddy\workagent\reshade-6.8.0\include'
}

$env:INCLUDE = @((Join-Path $msvc.FullName 'include'), (Join-Path $sdk "Include\$sdkVer\ucrt"), (Join-Path $sdk "Include\$sdkVer\um"), (Join-Path $sdk "Include\$sdkVer\shared"), (Join-Path $mh 'include'), (Join-Path $mh 'src'), $reshade) -join ';'
$env:LIB = @((Join-Path $msvc.FullName 'lib\x64'), (Join-Path $sdk "Lib\$sdkVer\ucrt\x64"), (Join-Path $sdk "Lib\$sdkVer\um\x64")) -join ';'
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'

Push-Location $root
try {
    & $cl /nologo /utf-8 /std:c++20 /EHsc /O2 /MT /W3 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX `
        src\mochinr.cpp `
        (Join-Path $mh 'src\hook.c') (Join-Path $mh 'src\trampoline.c') (Join-Path $mh 'src\buffer.c') `
        (Join-Path $mh 'src\hde\hde64.c') `
        /link /DLL /OUT:"$dist\mochinr.addon64" d3d12.lib dxgi.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }
} finally { Pop-Location }
Get-Item (Join-Path $dist 'mochinr.addon64') | Select-Object FullName, Length
