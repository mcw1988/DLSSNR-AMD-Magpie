# 给宿主 Magpie（GPL-3.0，SAOG0721 experimental）打 FSR3Upscaler 多槽补丁。
#
# 补丁与设计见 src\magpie-patches\README.md 与 docs\multislot.md。
# 本脚本只做三件事：校验基准提交、<git apply --check> 干跑、实打；不克隆、不构建、不猜冲突。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts/apply-magpie-patch.ps1 -Stage 1 -CheckOnly
#   powershell -ExecutionPolicy Bypass -File scripts/apply-magpie-patch.ps1 -Stage 1
#   powershell -ExecutionPolicy Bypass -File scripts/apply-magpie-patch.ps1 -Stage 2
#   powershell -ExecutionPolicy Bypass -File scripts/apply-magpie-patch.ps1 -SourceDir D:\Magpie-exp -Stage 2
param(
    # Magpie 源码工作副本（默认与本仓库同级的 magpie）
    [string]$SourceDir,
    # 1 = 只打阶段 1；2 = 按序打阶段 1 + 阶段 2
    [ValidateSet(1, 2)]
    [int]$Stage = 1,
    # 只校验，结束时还原到调用前的状态
    [switch]$CheckOnly,
    # git.exe 的完整路径；不填则依次找 PATH、Git for Windows 的默认安装位置、where.exe
    [string]$GitExe
)

$ErrorActionPreference = 'Stop'

# --- 定位 git：PATH 里没有时给出可读的报错，而不是让 git 调用静默失败 -------------
function Resolve-GitExe([string]$Explicit) {
    if ($Explicit) {
        if (-not (Test-Path $Explicit)) { throw "指定的 -GitExe 不存在: $Explicit" }
        return (Resolve-Path $Explicit).Path
    }
    $candidates = @('git.exe', 'git')
    foreach ($c in $candidates) {
        $cmd = Get-Command $c -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($cmd) { return $cmd.Source }
    }
    $guesses = @(
        (Join-Path $env:ProgramFiles 'Git\cmd\git.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Git\cmd\git.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\Git\cmd\git.exe')
    ) | Where-Object { $_ -and (Test-Path $_) }
    if ($guesses.Count -gt 0) { return @($guesses)[0] }
    $where = @(& where.exe git 2>$null)
    if ($LASTEXITCODE -eq 0 -and $where.Count -gt 0) { return @($where)[0] }
    throw '找不到 git.exe：把它加进 PATH，或用 -GitExe "C:\...\git.exe" 指定'
}

$repo = Split-Path $PSScriptRoot -Parent
$patchDir = Join-Path $repo 'src\magpie-patches'
$baseCommit = '6940f84'

if (-not $SourceDir) {
    $SourceDir = Join-Path (Split-Path $repo -Parent) 'magpie'
}
if (-not (Test-Path $SourceDir)) {
    throw "找不到 Magpie 源码目录: $SourceDir（用 -SourceDir 指定）"
}
$SourceDir = (Resolve-Path $SourceDir).Path

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Host '提示：PATH 里没有 git，将按默认安装位置查找' -ForegroundColor Yellow
}

# --- 补丁清单：0002 必须叠在 0001 之上，顺序写死在这里 -----------------------
$patches = @('0001-fsr3-slot-ring.patch')
if ($Stage -ge 2) {
    $patches += '0002-fsr3-output-lag.patch'
}
$lastPatch = $patches[-1]

Write-Host "仓库:     $repo"
Write-Host "宿主源码: $SourceDir"
Write-Host "阶段:     $Stage   补丁: $($patches -join ', ')"
if ($CheckOnly) { Write-Host '模式:     仅校验（-CheckOnly），结束时会还原' -ForegroundColor Cyan }

# --- 1. 基准提交 -----------------------------------------------------------
$git = Resolve-GitExe $GitExe
Write-Host "git:      $git"

$head = (& $git -C $SourceDir rev-parse HEAD 2>$null | Select-Object -First 1)
if ($LASTEXITCODE -ne 0 -or -not $head) {
    throw "$SourceDir 不是 git 工作副本（git rev-parse HEAD 失败）"
}
$head = $head.Trim()
Write-Host "HEAD:     $head"

if (-not $head.StartsWith($baseCommit)) {
    Write-Host ''
    Write-Host "补丁只对基准提交 $baseCommit 有效，当前 HEAD 不匹配。" -ForegroundColor Red
    Write-Host '  上游更新后 git apply 必然失败，这是设计上的行为：不要 --3way 硬合，也不要"看着改改"。' -ForegroundColor Yellow
    Write-Host '  正确做法：按新提交重新核对上下文、重制补丁，并重跑 docs\multislot.md §6.1 的 A1/A3。' -ForegroundColor Yellow
    Write-Host "  参考: git -C `"$SourceDir`" log --oneline -5" -ForegroundColor Yellow
    throw "基准提交不匹配（期望 $baseCommit*，实际 $head）"
}
Write-Host "基准提交校验通过（$baseCommit）" -ForegroundColor Green

# --- 2. 工作区必须干净，否则无法判断冲突是不是补丁造成的 -------------------
$dirty = @(& $git -C $SourceDir status --porcelain)
if ($LASTEXITCODE -ne 0) { throw 'git status 失败' }
if ($dirty.Count -gt 0) {
    Write-Host ''
    Write-Host '宿主工作区不干净，先处理（补丁的失败原因会变得不可判断）：' -ForegroundColor Red
    $dirty | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    Write-Host "  还原: git -C `"$SourceDir`" checkout -- ." -ForegroundColor Yellow
    throw '宿主工作区有未提交改动'
}
Write-Host '宿主工作区干净' -ForegroundColor Green

# --- 3. 按序校验 -----------------------------------------------------------
# 0002 是 0001 之上的增量，在干净树上单独 --check 必然失败；所以除最后一个以外，
# 校验通过后必须先实打（叠加补丁才有可校验的上下文）。-CheckOnly 会在 finally 里全部还原。
$stacked = New-Object System.Collections.ArrayList
$failure = $null
try {
    foreach ($name in $patches) {
        $path = Join-Path $patchDir $name
        if (-not (Test-Path $path)) { throw "补丁不存在: $path" }

        Write-Host "git apply --check $name" -NoNewline
        & $git -C $SourceDir apply --check $path
        if ($LASTEXITCODE -ne 0) {
            Write-Host '   失败' -ForegroundColor Red
            Write-Host '  上面是 git 的原始报错。不要改补丁去"适配"，先确认 HEAD 是不是 6940f84。' -ForegroundColor Yellow
            throw "补丁 $name 无法应用"
        }
        Write-Host '   OK' -ForegroundColor Green

        if ($name -ne $lastPatch) {
            # 中间补丁：实打，给下一个补丁提供上下文
            & $git -C $SourceDir apply $path
            if ($LASTEXITCODE -ne 0) { throw "中间补丁 $name 无法叠加" }
            [void]$stacked.Add($path)
            Write-Host '  （已临时叠加，用于校验下一个补丁）' -ForegroundColor DarkGray
        }
    }
} catch {
    $failure = $_
} finally {
    if ($CheckOnly -and $stacked.Count -gt 0) {
        # 逆序还原，恢复调用前的状态
        for ($i = $stacked.Count - 1; $i -ge 0; $i--) {
            & $git -C $SourceDir apply -R $stacked[$i]
            if ($LASTEXITCODE -ne 0) {
                Write-Host "-R $(Split-Path $stacked[$i] -Leaf) 失败，请手工 git checkout -- ." -ForegroundColor Red
            }
        }
        $left = @(& $git -C $SourceDir status --porcelain)
        if ($left.Count -eq 0) {
            Write-Host '已还原，宿主工作区干净' -ForegroundColor Green
        } else {
            Write-Host '宿主工作区仍有改动，请手工还原：' -ForegroundColor Red
            $left | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
        }
    }
}
if ($failure) { throw $failure }

if ($CheckOnly) {
    Write-Host ''
    Write-Host '干跑通过，未留下任何改动。实打用：' -ForegroundColor Green
    Write-Host "  powershell -ExecutionPolicy Bypass -File scripts/apply-magpie-patch.ps1 -Stage $Stage"
    exit 0
}

# --- 4. 打最后一个补丁 -----------------------------------------------------
$lastPath = Join-Path $patchDir $lastPatch
Write-Host "git apply $lastPatch" -NoNewline
& $git -C $SourceDir apply $lastPath
if ($LASTEXITCODE -ne 0) {
    Write-Host '   失败' -ForegroundColor Red
    Write-Host '  前面的补丁已打上。还原：' -ForegroundColor Yellow
    Write-Host "    git -C `"$SourceDir`" checkout -- src/Magpie.Core/FSR3Upscaler.cpp src/Magpie.Core/FSR3Upscaler.h" -ForegroundColor Yellow
    throw "补丁 $lastPatch 应用失败"
}
Write-Host '   OK' -ForegroundColor Green

Write-Host ''
Write-Host '已应用。改动文件：' -ForegroundColor Green
@(& $git -C $SourceDir status --porcelain) | ForEach-Object { Write-Host "  $_" }

# --- 5. 下一步 -------------------------------------------------------------
$propsUser = Join-Path $SourceDir 'src\BuildOptions.props.user'
Write-Host ''
Write-Host '下一步（宿主用 MSBuild，不是本仓库的 build-all.ps1）：' -ForegroundColor Cyan
Write-Host '  1) 打开 FSR3 编译开关，写入下列文件（BuildOptions.props 要求走 .user 覆盖）：'
Write-Host "       $propsUser" -ForegroundColor Cyan
Write-Host '       <Project><PropertyGroup><EnableFSR3ZeroMV>true</EnableFSR3ZeroMV></PropertyGroup></Project>'
if (Test-Path $propsUser) {
    $override = Select-String -Path $propsUser -Pattern 'EnableFSR3ZeroMV' -SimpleMatch -Quiet
    if ($override) {
        Write-Host '     （已存在该覆盖项）' -ForegroundColor Green
    } else {
        Write-Host '     （文件存在但没有 EnableFSR3ZeroMV，记得补一行）' -ForegroundColor Yellow
    }
} else {
    Write-Host '     （文件当前不存在）' -ForegroundColor Yellow
}
Write-Host "  2) powershell -ExecutionPolicy Bypass -File `"$SourceDir\scripts\Build-Release.ps1`""
Write-Host '  3) 先跑阶段 1（slots=1 与 slots=3 对照），确认"性能不变"这条设计断言：'
Write-Host '       $env:MAGPIE_FSR_SLOTS=3; $env:MAGPIE_FSR_OUTPUT_LAG=0'
Write-Host '       然后在 magpie.log 里 grep "fsr3-slot:"'
Write-Host '  4) 阶段 2 需要 mochinr 的 inflight >= slots + lag（3 槽 + lag1 → 4），否则会被限流成"改了但没变"'
Write-Host '  判据见 docs\multislot.md §6；配置面与显存代价见 src\magpie-patches\README.md'
