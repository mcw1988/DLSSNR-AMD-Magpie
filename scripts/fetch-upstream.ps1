# 拉取构建与运行所需的上游内容（本仓库不随附它们）。
#
#   powershell -ExecutionPolicy Bypass -File scripts/fetch-upstream.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/fetch-upstream.ps1 -SkipRuntime
#
# 产出：
#   upstream\reshade\include\*             ReShade 头文件（构建 overlay-input 用）
#   upstream\dlss5-neural-addon\...        实为 dlss5-neural-amd 源码（构建 amd-nr.addon64 用）
#   stage\MochizukiNrRuntime.dll           运行时
#   stage\dlssnr-amd\...                   SPIR-V / ACO / int4 权重 / 默认网络
#
# 说明：HuggingFace 上的压缩包把子目录用 '.' 拍平了（dlssnr-amd.shaders.renodx 实为
# dlssnr-amd/shaders/renodx），这里会还原成嵌套结构。
param(
    [string]$Root = (Split-Path $PSScriptRoot -Parent),
    [string]$StageDir = '',
    [switch]$SkipRuntime
)

$ErrorActionPreference = 'Stop'
if (-not $StageDir) { $StageDir = Join-Path $Root 'stage' }
$upstream = Join-Path $Root 'upstream'
New-Item -ItemType Directory -Force -Path $upstream | Out-Null

function Get-Repo([string]$url, [string]$dest) {
    if (Test-Path $dest) { Write-Host "已存在，跳过: $dest"; return }
    # 某些环境（公司代理/证书吊销检查）会让 schannel 报 CRYPT_E_NO_REVOCATION_CHECK
    git -c http.schannelCheckRevoke=false clone --depth 1 $url $dest
    if ($LASTEXITCODE -ne 0) { throw "git clone 失败: $url" }
}

# --- ReShade 头文件 ---------------------------------------------------------
Get-Repo 'https://github.com/crosire/reshade.git' (Join-Path $upstream 'reshade')

# --- OpenNR 宿主源码 --------------------------------------------------------
Get-Repo 'https://github.com/zmodelerlover/dlss5-neural-amd.git' (Join-Path $upstream 'dlss5-neural-amd')

if ($SkipRuntime) {
    Write-Host '按要求跳过运行时下载'
    return
}

# --- mochizuki 运行时 + 权重 ------------------------------------------------
$hf = 'https://huggingface.co/datasets/zmodelerlover/amd-nr/resolve/main/'
$tmp = Join-Path $env:TEMP 'magpie-nr-fetch'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

$zipUrl = $hf + 'mochizuki/0.5.9-opennr/mochizuki-0.5.9-opennr.zip'
$zipPath = Join-Path $tmp 'mochizuki-0.5.9-opennr.zip'
if (-not (Test-Path $zipPath)) {
    Write-Host '下载运行时包（84 MB）...'
    Invoke-WebRequest -Uri $zipUrl -OutFile $zipPath -UseBasicParsing
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$stage = New-Item -ItemType Directory -Force -Path $StageDir

Write-Host "解包到 $($stage.FullName)（还原被拍平的目录）..."
$archive = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    foreach ($entry in $archive.Entries) {
        if ([string]::IsNullOrEmpty($entry.Name)) { continue }   # 目录项
        $parts = $entry.FullName -split '/'
        $head = $parts[0]
        if ($head -like 'dlssnr-amd.*' -and $head -ne 'dlssnr-amd') {
            $head = ($head -split '\.') -join '/'
        }
        $rest = @()
        if ($parts.Length -gt 1) { $rest = $parts[1..($parts.Length - 1)] }
        $rel = (@($head) + $rest) -join '\'
        $out = Join-Path $stage.FullName $rel
        New-Item -ItemType Directory -Force -Path (Split-Path $out -Parent) | Out-Null
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $out, $true)
    }
} finally { $archive.Dispose() }

# 压缩包里没有默认网络，必须单独取
$modelUrl = $hf + 'mochizuki/model-small-1/dlssnr-small.bin'
$modelOut = Join-Path $stage.FullName 'dlssnr-amd\dlssnr-small.bin'
if (-not (Test-Path $modelOut)) {
    Write-Host '下载默认网络 dlssnr-small.bin（114 MB）...'
    Invoke-WebRequest -Uri $modelUrl -OutFile $modelOut -UseBasicParsing
}

Write-Host ''
Write-Host '完成。stage\ 应包含：'
Get-ChildItem $stage.FullName | Select-Object Name, Length
Write-Host ''
Write-Host "把 stage\ 里的内容（MochizukiNrRuntime.dll 与 dlssnr-amd\）复制到 Magpie.exe 所在目录。"
