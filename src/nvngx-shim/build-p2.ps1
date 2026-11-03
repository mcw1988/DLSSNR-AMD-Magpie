# P2 shim 一键构建：产出 amdgx_dlssnr.dll（DLSSNR 槽内跑 MochizukiNrRuntime，失败回落直通）
# 用法：powershell -ExecutionPolicy Bypass -File build-p2.ps1 [-Out <部署路径>]
param(
    [string]$Out = ""
)
$ErrorActionPreference = 'Stop'

$vs = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$msvc = (Get-ChildItem "$vs\VC\Tools\MSVC" | Sort-Object Name -Descending | Select-Object -First 1).FullName
$sdk = "C:\Program Files (x86)\Windows Kits\10"
$sdkv = "10.0.26100.0"

$env:INCLUDE = "$msvc\include;$sdk\Include\$sdkv\ucrt;$sdk\Include\$sdkv\um;$sdk\Include\$sdkv\shared;$sdk\Include\$sdkv\winrt;$sdk\Include\$sdkv\cppwinrt"
$env:LIB = "$msvc\lib\x64;$sdk\Lib\$sdkv\ucrt\x64;$sdk\Lib\$sdkv\um\x64"

$here = $PSScriptRoot
& "$msvc\bin\Hostx64\x64\cl.exe" /nologo /utf-8 /O2 /LD /EHsc "$here\passthrough.cpp" `
    /I"C:\Users\ma\WorkBuddy\workagent\magpie-nr-repo\src\nrtest" `
    /Fe:"$here\amdgx_dlssnr.dll" `
    /link /DEF:"$here\passthrough.def" /IMPLIB:"$here\passthrough.lib" ole32.lib
if ($LASTEXITCODE -ne 0) { throw "cl failed: $LASTEXITCODE" }

if ($Out -ne "") {
    Copy-Item "$here\amdgx_dlssnr.dll" $Out -Force
    Write-Host "deployed -> $Out"
}
Write-Host "built: $here\amdgx_dlssnr.dll"
