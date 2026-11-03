# 构建 nvngx_dlssnr.dll 探针桩。
#
# 需要：VS 2022 Build Tools（VC Tools x64）+ Windows SDK。本机已装，
#       只是 cl.exe 不在 PATH 里，所以这里自己定位。
# 用法：
#   powershell -ExecutionPolicy Bypass -File src\nvngx-shim\build.ps1
#   powershell -ExecutionPolicy Bypass -File src\nvngx-shim\build.ps1 -OutDir "D:\Magpie-NR-045"
param(
    # 产物输出目录；默认本目录下的 build\
    [string]$OutDir,
    # 顺带编译并运行自测宿主（不启动 Magpie，验证导出/参数对象/句柄链路）
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = Split-Path (Split-Path $here -Parent) -Parent
if (-not $OutDir) { $OutDir = Join-Path $here 'build' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# --- 工具链定位 -------------------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe 不存在：先装 VS 2022 Build Tools' }
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

$env:INCLUDE = @(
    (Join-Path $msvc.FullName 'include')
    (Join-Path $sdkRoot "Include\$sdkVer\ucrt")
    (Join-Path $sdkRoot "Include\$sdkVer\um")
    (Join-Path $sdkRoot "Include\$sdkVer\shared")
) -join ';'
$env:LIB = @(
    (Join-Path $msvc.FullName 'lib\x64')
    (Join-Path $sdkRoot "Lib\$sdkVer\ucrt\x64")
    (Join-Path $sdkRoot "Lib\$sdkVer\um\x64")
) -join ';'

Write-Host "MSVC $($msvc.Name)   SDK $sdkVer"

# --- 构建 -------------------------------------------------------------------
# /MT 静态 CRT：宿主目录里常躺着旧版运行库，/MD 会被私有副本覆盖。
$common = @('/nologo', '/utf-8', '/std:c++20', '/EHsc', '/O2', '/MT', '/W3',
            '/DNDEBUG', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/D_CRT_SECURE_NO_WARNINGS')

Push-Location $here
try {
    & $cl @common /LD probe.cpp /link /DLL /DEF:probe.def /OUT:"$OutDir\nvngx_dlssnr.dll" /PDB:"$OutDir\nvngx_dlssnr.pdb" user32.lib
    if ($LASTEXITCODE -ne 0) { throw "探针桩构建失败 ($LASTEXITCODE)" }
} finally {
    Pop-Location
}

Write-Host ''
Get-ChildItem $OutDir -Filter 'nvngx_dlssnr.*' | Select-Object Name, Length, LastWriteTime

# --- 导出表自检 -------------------------------------------------------------
$dumpbin = Join-Path $msvc.FullName 'bin\Hostx64\x64\dumpbin.exe'
if (Test-Path $dumpbin) {
    $exports = & $dumpbin /nologo /exports "$OutDir\nvngx_dlssnr.dll" |
        Select-String -Pattern 'NVSDK_NGX_D3D12_\w+' | ForEach-Object { $_.Matches[0].Value } | Select-Object -Unique
    $expect = @(
        'NVSDK_NGX_D3D12_Init_Ext','NVSDK_NGX_D3D12_Init_with_ProjectID',
        'NVSDK_NGX_D3D12_AllocateParameters','NVSDK_NGX_D3D12_GetCapabilityParameters',
        'NVSDK_NGX_D3D12_DestroyParameters','NVSDK_NGX_D3D12_CreateFeature',
        'NVSDK_NGX_D3D12_EvaluateFeature','NVSDK_NGX_D3D12_ReleaseFeature',
        'NVSDK_NGX_D3D12_Shutdown','NVSDK_NGX_D3D12_Shutdown1',
        'NVSDK_NGX_D3D12_GetFeatureRequirements','NVSDK_NGX_D3D12_GetScratchBufferSize',
        'NVSDK_NGX_D3D12_PopulateParameters_Impl'
    )
    $missing = $expect | Where-Object { $exports -notcontains $_ }
    if ($missing) { throw ("导出缺失: " + ($missing -join ', ')) }
    Write-Host "导出表自检通过：$($expect.Count)/$($expect.Count)" -ForegroundColor Green

    # GetModuleFileNameW 必须是静态导入（Magpie 的调用方兼容钩子依赖它）
    $imports = & $dumpbin /nologo /imports "$OutDir\nvngx_dlssnr.dll" | Out-String
    if ($imports -match 'GetModuleFileNameW') {
        Write-Host 'GetModuleFileNameW 导入存在（调用方兼容钩子可用）' -ForegroundColor Green
    } else {
        throw '本 DLL 没有导入 GetModuleFileNameW —— Magpie 会在加载后直接判定"片段不完整"'
    }
}

# --- 自测（可选）-------------------------------------------------------------
if ($SelfTest) {
    Push-Location $here
    try {
        & $cl @common selftest.cpp /link /SUBSYSTEM:CONSOLE /OUT:"$OutDir\selftest.exe"
        if ($LASTEXITCODE -ne 0) { throw "自测宿主构建失败 ($LASTEXITCODE)" }
    } finally {
        Pop-Location
    }
    Write-Host ''
    Write-Host '--- 自测：用 Magpie 的调用顺序驱动探针 ---' -ForegroundColor Cyan
    Push-Location $OutDir
    try {
        & ".\selftest.exe" ".\nvngx_dlssnr.dll"
        if ($LASTEXITCODE -ne 0) { throw "自测失败 ($LASTEXITCODE)" }
    } finally {
        Pop-Location
    }
    $log = Join-Path $OutDir 'nvngx-shim.log'
    if (Test-Path $log) {
        Write-Host ''
        Write-Host "--- 探针日志（$log）---" -ForegroundColor Cyan
        Get-Content $log -Encoding UTF8 | Select-Object -Last 20
    } else {
        throw '自测跑完但没有生成 nvngx-shim.log'
    }
}
