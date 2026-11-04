# P2 shim 一键构建：产出 amdgx_dlssnr.dll（DLSSNR 槽内跑 MochizukiNrRuntime，失败回落直通）
# 用法：powershell -ExecutionPolicy Bypass -File build-p2.ps1 [-Out <部署路径>] [-Backup] [-SelfTest] [-AbiProbe]
param(
    [string]$Out = "",
    [switch]$Backup,
    [switch]$SelfTest,
    [switch]$AbiProbe,
    [string]$DlssSdkDir = "D:\_build\DLSS-SDK"
)
$ErrorActionPreference = 'Stop'

$vs = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$msvc = (Get-ChildItem "$vs\VC\Tools\MSVC" | Sort-Object Name -Descending | Select-Object -First 1).FullName
$sdk = "C:\Program Files (x86)\Windows Kits\10"
$sdkv = "10.0.26100.0"
$cl = "$msvc\bin\Hostx64\x64\cl.exe"

$env:INCLUDE = "$msvc\include;$sdk\Include\$sdkv\ucrt;$sdk\Include\$sdkv\um;$sdk\Include\$sdkv\shared;$sdk\Include\$sdkv\winrt;$sdk\Include\$sdkv\cppwinrt"
$env:LIB = "$msvc\lib\x64;$sdk\Lib\$sdkv\ucrt\x64;$sdk\Lib\$sdkv\um\x64"

# 头文件以本脚本为基准解析：..\nrtest（MochiNrApi.h）、..\mochinr（MochizukiNrControls.h）
$here = $PSScriptRoot
$srcRoot = Split-Path $here -Parent
& $cl /nologo /utf-8 /O2 /LD /EHsc "$here\passthrough.cpp" `
    /I"$srcRoot\nrtest" /I"$srcRoot\mochinr" `
    /Fe:"$here\amdgx_dlssnr.dll" `
    /link /DEF:"$here\passthrough.def" /IMPLIB:"$here\passthrough.lib" ole32.lib
if ($LASTEXITCODE -ne 0) { throw "cl failed: $LASTEXITCODE" }
Write-Host "built: $here\amdgx_dlssnr.dll"

# --- 离屏自测：不启动 Magpie，验证「浮点键能被读到」这条面板通道 ---
if ($SelfTest) {
    $kit = Join-Path $here '_selftest'
    New-Item -ItemType Directory -Force -Path $kit | Out-Null
    & $cl /nologo /utf-8 /EHsc "$here\selftest.cpp" /Fe:"$kit\selftest.exe" `
        /Fo:"$kit\" /link /SUBSYSTEM:CONSOLE
    if ($LASTEXITCODE -ne 0) { throw "selftest build failed: $LASTEXITCODE" }
    Copy-Item "$here\amdgx_dlssnr.dll" "$kit\amdgx_dlssnr.dll" -Force
    Remove-Item "$kit\nvngx-shim.log" -ErrorAction SilentlyContinue

    Write-Host ''
    Write-Host '--- 自测：按宿主的方式驱动参数对象 ---' -ForegroundColor Cyan
    Push-Location $kit
    try { & ".\selftest.exe" "amdgx_dlssnr.dll" } finally { Pop-Location }

    # 进程退出后再读日志（DLL 的日志句柄要到进程退出才释放）
    $log = Get-Content "$kit\nvngx-shim.log" -Encoding UTF8 | Out-String
    $expects = @(
        @{ Needle = 'DLSSNR.Intensity = 1.25 (slot 5, float)';        What = 'float 键 slot 5' },
        @{ Needle = 'DLSSNR.LocalToneStrength = 0.75 (slot 5, float)'; What = 'float 键 slot 5（第二项）' },
        @{ Needle = 'DLSSNR.Style = 1 (slot 2, int)';                 What = 'int 键 slot 2' },
        @{ Needle = 'DLSSNR.UseAutoMask = 1 (slot 2, int)';           What = 'int 键 slot 2（布尔）' },
        @{ Needle = 'DLSSNR.Width = 1920 (slot 3, int)';              What = 'uint 键 slot 3' }
    )
    $bad = 0
    foreach ($e in $expects) {
        if ($log.Contains($e.Needle)) {
            Write-Host ("  [ OK ] {0}" -f $e.What) -ForegroundColor Green
        } else {
            Write-Host ("  [FAIL] {0}  期望: {1}" -f $e.What, $e.Needle) -ForegroundColor Red
            $bad++
        }
    }
    if ($bad -gt 0) { throw "参数通道自测失败 $bad 项（日志 $kit\nvngx-shim.log）" }
    Write-Host '  参数通道自测通过：浮点/整型/指针三类槽位都取到真值' -ForegroundColor Green
}

# --- ABI 校对：用**上游那份 nvsdk_ngx_params.h** 声明接口，验证槽位没写错 ---
# 与 -SelfTest 的区别：SelfTest 用的是我们自己手写的假 vtable（只能测"我们以为的布局"），
# AbiProbe 直接 include 宿主编译时用的头文件，测的是"宿主真实会怎么调"。
if ($AbiProbe) {
    if (-not (Test-Path "$DlssSdkDir\include\nvsdk_ngx_params.h")) {
        throw "找不到 $DlssSdkDir\include\nvsdk_ngx_params.h（用 -DlssSdkDir 指定 DLSS SDK 目录）"
    }
    $kit = Join-Path $here '_abi'
    New-Item -ItemType Directory -Force -Path $kit | Out-Null
    & $cl /nologo /utf-8 /EHsc "$here\abi_probe.cpp" /I"$DlssSdkDir\include" `
        /Fe:"$kit\abi_probe.exe" /Fo:"$kit\" /link /SUBSYSTEM:CONSOLE
    if ($LASTEXITCODE -ne 0) { throw "abi_probe build failed: $LASTEXITCODE" }
    Copy-Item "$here\amdgx_dlssnr.dll" "$kit\amdgx_dlssnr.dll" -Force
    Remove-Item "$kit\nvngx-shim.log" -ErrorAction SilentlyContinue

    Write-Host ''
    Write-Host '--- ABI 校对：用上游头文件驱动参数对象 ---' -ForegroundColor Cyan
    Push-Location $kit
    try { & ".\abi_probe.exe" "amdgx_dlssnr.dll" } finally { Pop-Location }

    $log = Get-Content "$kit\nvngx-shim.log" -Encoding UTF8 | Out-String
    $expects = @(
        @{ Needle = 'ABI.Float = 1.25 (slot 5, float)';       What = 'float → Slot5' },
        @{ Needle = 'ABI.Double = 2.5 (slot 4, float)';       What = 'double → Slot4' },
        @{ Needle = 'ABI.Int = 7 (slot 2, int)';              What = 'int → Slot2' },
        @{ Needle = 'ABI.UInt = 1920 (slot 3, int)';          What = 'unsigned → Slot3' },
        @{ Needle = 'ABI.Resource12 = 0000000000001234 (slot 0, ptr)'; What = 'ID3D12Resource* → Slot0' }
    )
    $bad = 0
    foreach ($e in $expects) {
        if ($log.Contains($e.Needle)) {
            Write-Host ("  [ OK ] {0}" -f $e.What) -ForegroundColor Green
        } else {
            Write-Host ("  [FAIL] {0}  期望: {1}" -f $e.What, $e.Needle) -ForegroundColor Red
            $bad++
        }
    }
    if ($bad -gt 0) { throw "ABI 校对失败 $bad 项（日志 $kit\nvngx-shim.log）" }
    Write-Host '  槽位与上游头文件一致' -ForegroundColor Green
}

if ($Out -ne "") {
    $target = Join-Path $Out "amdgx_dlssnr.dll"
    if ($Backup -and (Test-Path $target)) {
        Move-Item $target "$target.prev" -Force
        Write-Host "backup -> $target.prev"
    }
    Copy-Item "$here\amdgx_dlssnr.dll" $target -Force
    Write-Host "deployed -> $target"
}
