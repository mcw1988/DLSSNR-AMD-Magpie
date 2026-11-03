<#
.SYNOPSIS
    G0 判别实验：量 GPU 忙时间占帧时间的比例（ratio），判定"取大"的台阶是否存在。

.DESCRIPTION
    背景见 docs/multislot.md §2。一句话：求和式 `frame = 捕获 + 槽位 + 网络` 对两种世界同样成立——
    「延迟串行」（岛可以与本帧 D3D11 段重叠，所以阶段 2 能拿到台阶）和「GPU 吞吐已被占满」
    （重叠也没用，台阶不存在）。现有的五点实测无法区分这两者，唯一能区分的就是 ratio：

        ratio = 每帧的"引擎忙秒数" ÷ frame_time

    ratio 在 0.6 以上说明 GPU 已经忙到接近饱和；>0.85 时"取大"根本没有空间，整条阶段 2 作废。

## 指标定义（不要自己发明口径）

    \GPU Engine(*)\Utilization Percentage 是**每个引擎实例各自 0–100%**。3D 与 Compute 可以
    同时忙，所以**并行引擎求和可以 >100%**：ratio = 1.2 是正常的，它表示"平均有 1.2 个引擎在忙"，
    正是 GPU 已饱和的证据，不是算错。

    忙秒数 = Σ_实例 Σ_样本 (值 / 100 × 采样间隔秒)
    平均忙引擎数 = 忙秒数 ÷ 窗口秒数        （等价于 ratio，见下）
    ratio = (忙秒数 ÷ 窗口帧数) ÷ (frame_time 秒)

## 采样

    计数器：\GPU Engine(*)\Utilization Percentage
    必须按 PID 过滤实例：实例名形如
        pid_12345_luid_0x00000000_0x0000ABCD_phys_0_eng_0_engtype_3D
    只累加 `pid_<PID>_` 开头的实例，否则浏览器/桌面合成会被算进来，ratio 直接虚高。
    默认采样 -SampleInterval 1、-WarmupSeconds 10、-Seconds 30；预热段整段丢弃
    （依据 multislot.md §2.3 的"稳态 30 s 窗口"）。

## frame_time 从哪来

    优先解析 magpie.log 的 `Backend publication CPU timing: samples=120 ...` 行（每 120 帧一行，
    Renderer.cpp:4634-4638），取窗口内首末两行的日志时间戳求差，除以这段时间覆盖的帧数。
    只有两行时等价于「求差 ÷ 120 × 1000」；多行时按 120 × (行数 - 1) 帧算，才是对的。
    回退：-Fps <数值>，则 frame_time_ms = 1000 / fps、frames = fps × 窗口秒数。
    两者都没有 → 退出码 1（不瞎猜）。

## 判据（阈值写死在这里，与 multislot.md §2.3 完全一致）

    ratio > 0.85        停：求和是吞吐的必然，max 天花板不存在，不做阶段 2，主线转缩岛
    0.60 ≤ ratio ≤ 0.85 台阶打 (1-ratio) 折扣，验收线按实测下调
    ratio < 0.60        台阶成立，按 123–125 / 66–70 fps 验收

## 退出码

    0  正常（含 -DryRun）
    1  找不到 Magpie 进程；或日志里没有 publication 行且未给 -Fps；
       或采样窗口内 GPU 近乎空闲（avg_busy_engines < 0.05，样本无效，**不给判据**）
    2  计数器不存在（非 Windows / 无 GPU 计数器 / Get-Counter 失败）

    最后一个 1 是刻意加的闸门：进程开着但没在缩放时 ratio 会接近 0，若照常输出
    "台阶成立"，就会得出"主线成立"的假结论——那比不测更糟。样本无效时 CSV 仍然写出
    （带 `# valid=false`），便于事后申诉。

.PARAMETER SourceDir
    Magpie 源码/部署目录，默认与本仓库同级的 ..\magpie。日志默认取 <SourceDir>\logs\magpie.log。

.PARAMETER ProcessName
    进程名（不含 .exe），默认 Magpie。用它找到 PID 再过滤 GPU Engine 实例。

.PARAMETER LogPath
    覆盖 magpie.log 的路径。

.PARAMETER Fps
    日志不可用时的回退：直接给稳态帧率。给了它就不再依赖日志。

.PARAMETER SampleInterval
    采样间隔（秒，整数，Get-Counter 的限制），默认 1。

.PARAMETER WarmupSeconds
    预热秒数，整段丢弃，默认 10。

.PARAMETER Seconds
    统计窗口秒数，默认 30。

.PARAMETER OutFile
    CSV 输出路径，默认当前目录 gpu-busy-<yyyyMMdd-HHmmss>.csv。

.PARAMETER DryRun
    只打印将要采样的实例数与判据阈值，不做定时采样。没有 Magpie 进程也返回 0，
    便于在开发机/联调机上先确认脚本本身能跑。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts/measure-gpu-busy.ps1 -DryRun

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts/measure-gpu-busy.ps1 -SourceDir D:\Magpie-NR-045

.EXAMPLE
    # 日志没有 publication 行（比如关了计时）时，直接给帧率
    powershell -ExecutionPolicy Bypass -File scripts/measure-gpu-busy.ps1 -Fps 47
#>
[CmdletBinding()]
param(
    [string]$SourceDir,
    [string]$ProcessName = 'Magpie',
    [string]$LogPath,
    [double]$Fps = 0,
    [int]$SampleInterval = 1,
    [int]$WarmupSeconds = 10,
    [int]$Seconds = 30,
    [string]$OutFile,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# --- 判据阈值：与 docs/multislot.md §2.3 写死的三档完全一致，改这里等于改验收标准 ---
$RatioStop = 0.85       # ratio > 0.85 → 停，主线转"缩岛"
$RatioDiscount = 0.60   # 0.60 ≤ ratio ≤ 0.85 → 台阶打 (1-ratio) 折扣

# 发布路径每 120 帧打一行 publication timing（Renderer.cpp:4634-4638）
$FramesPerLogLine = 120
$CounterPath = '\GPU Engine(*)\Utilization Percentage'
$CounterSetName = 'GPU Engine'
$PublicationPattern = 'Backend publication CPU timing: samples=(\d+) '

$repo = Split-Path $PSScriptRoot -Parent

# --- 0. 参数归一与自检 ------------------------------------------------------
if (-not $SourceDir) {
    $SourceDir = Join-Path (Split-Path $repo -Parent) 'magpie'
}
if (-not $LogPath) {
    $LogPath = Join-Path $SourceDir 'logs\magpie.log'
}
if ($SampleInterval -lt 1) {
    Write-Host "警告：-SampleInterval 只能是正整数秒（Get-Counter 的限制），已按 1 处理" -ForegroundColor Yellow
    $SampleInterval = 1
}
if ($Seconds -lt 2) {
    Write-Host '-Seconds 至少 2 秒，否则窗口内凑不出两个样本' -ForegroundColor Red
    exit 1
}
if (-not $OutFile) {
    $OutFile = Join-Path (Get-Location).Path ("gpu-busy-{0}.csv" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}

$warmupSamples = [int][math]::Round($WarmupSeconds / $SampleInterval)
$windowSamples = [int][math]::Round($Seconds / $SampleInterval)
$totalSamples = $warmupSamples + $windowSamples

Write-Host ''
Write-Host '=== G0：GPU 忙时间占帧时间比例 ===' -ForegroundColor Cyan
Write-Host "宿主目录:     $SourceDir"
Write-Host "日志:         $LogPath"
Write-Host "进程名:       $ProcessName.exe"
Write-Host "计数器:       $CounterPath"
Write-Host "采样:         间隔 $SampleInterval s，预热 $WarmupSeconds s（丢弃 $warmupSamples 个样本），窗口 $Seconds s（$windowSamples 个样本）"
Write-Host "判据阈值:     ratio > $RatioStop 停；$RatioDiscount ≤ ratio ≤ $RatioStop 打折；ratio < $RatioDiscount 成立"

# --- 1. 找进程 --------------------------------------------------------------
$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) {
    if ($DryRun) {
        Write-Host ''
        Write-Host "未找到进程 $ProcessName.exe —— -DryRun 继续，但真实采样需要它先跑起来并开始缩放。" -ForegroundColor Yellow
        Write-Host '（实例数无法预览：GPU Engine 的实例名里带 PID）' -ForegroundColor Yellow
        Write-Host ''
        Write-Host '阈值（写死，与 docs/multislot.md §2.3 一致）：' -ForegroundColor Cyan
        Write-Host ("  ratio > {0,-4}      停：求和是吞吐的必然，max 天花板不存在，不做阶段 2，主线转缩岛" -f $RatioStop)
        Write-Host ("  {0,-4} ≤ ratio ≤ {1,-4} 台阶打 (1-ratio) 折扣，验收线按实测下调" -f $RatioDiscount, $RatioStop)
        Write-Host ("  ratio < {0,-4}      台阶成立，按 123–125 / 66–70 fps 验收" -f $RatioDiscount)
        Write-Host ''
        Write-Host 'DryRun 结束（未采样，未写 CSV）' -ForegroundColor Green
        exit 0
    }
    Write-Host ''
    Write-Host "未找到进程 $ProcessName.exe。先启动 Magpie 并让它开始缩放，再跑本脚本。" -ForegroundColor Red
    exit 1
}
$targetPid = $proc.Id
$pidFilter = "^pid_$($targetPid)_"
Write-Host "PID:          $targetPid（只累加实例名匹配 $pidFilter 的引擎）"

# --- 2. 计数器可用性 --------------------------------------------------------
# 非 Windows / 无 GPU 计数器 / 驱动未注册性能计数器时，Get-Counter 会抛非终止错误，
# 这里统一收敛成退出码 2，不抛裸异常。（-DryRun 也走这条路，但失败只警告不退出。）
$counterAvailable = $true
$counterError = ''
try {
    $null = Get-Counter -ListSet $CounterSetName -ErrorAction Stop
} catch {
    $counterAvailable = $false
    $counterError = $_.Exception.Message
}

if (-not $counterAvailable) {
    Write-Host ''
    Write-Host "GPU Engine 计数器不可用：$counterError" -ForegroundColor Red
    Write-Host '  常见原因：非 Windows、虚拟机/远程桌面无 GPU、显卡驱动未注册性能计数器。' -ForegroundColor Yellow
    Write-Host '  此时无法得到 ratio，请换一台有真实 GPU 的机器测；不要在开发机上"估算"。' -ForegroundColor Yellow
    if ($DryRun) {
        Write-Host 'DryRun：计数器缺失不视为失败（只提示），退出码 0。' -ForegroundColor Yellow
        exit 0
    }
    exit 2
}

# --- 3. -DryRun：枚举实例数（1 次采样，仅用于预览），然后退出 ----------------
if ($DryRun) {
    Write-Host ''
    Write-Host 'DryRun：枚举实例（下面这一次采样只用于数实例，不计入统计）' -ForegroundColor Cyan
    try {
        $preview = @(Get-Counter -Counter $CounterPath -MaxSamples 1 -ErrorAction Stop)
        $instances = @($preview[0].CounterSamples | Where-Object { $_.InstanceName -match $pidFilter })
        Write-Host ("将要采样的实例数：{0}（都属于 PID {1}）" -f $instances.Count, $targetPid) -ForegroundColor Green
        if ($instances.Count -gt 0) {
            $instances | ForEach-Object { Write-Host ("  {0}" -f $_.InstanceName) -ForegroundColor DarkGray }
        } else {
            Write-Host "  0 个：进程在跑但当前没有属于它的 GPU 引擎实例（还没开始缩放？）" -ForegroundColor Yellow
        }
    } catch {
        Write-Host "  枚举失败：$($_.Exception.Message)（真实采样时同样会失败，退出码 2）" -ForegroundColor Yellow
    }
    Write-Host ''
    Write-Host '阈值（写死，与 docs/multislot.md §2.3 一致）：' -ForegroundColor Cyan
    Write-Host ("  ratio > {0,-4}      停：求和是吞吐的必然，max 天花板不存在，不做阶段 2，主线转缩岛" -f $RatioStop)
    Write-Host ("  {0,-4} ≤ ratio ≤ {1,-4} 台阶打 (1-ratio) 折扣，验收线按实测下调" -f $RatioDiscount, $RatioStop)
    Write-Host ("  ratio < {0,-4}      台阶成立，按 123–125 / 66–70 fps 验收" -f $RatioDiscount)
    Write-Host ''
    Write-Host ("frame_time 来源：{0}" -f $(if (Test-Path $LogPath) { "日志 $LogPath" } elseif ($Fps -gt 0) { "-Fps $Fps" } else { '无（真实采样会退出码 1）' }))
    Write-Host 'DryRun 结束（未采样，未写 CSV）' -ForegroundColor Green
    exit 0
}

# --- 4. 采样 ---------------------------------------------------------------
Write-Host ''
Write-Host "采样中… 共 $totalSamples 个样本（约 $($totalSamples * $SampleInterval) s），请保持场景稳定" -ForegroundColor Cyan
try {
    $data = @(Get-Counter -Counter $CounterPath -SampleInterval $SampleInterval -MaxSamples $totalSamples -ErrorAction Stop)
} catch {
    Write-Host "采样失败：$($_.Exception.Message)" -ForegroundColor Red
    exit 2
}
if ($data.Count -lt ($warmupSamples + 2)) {
    Write-Host "采样不足：只拿到 $($data.Count) 个样本，凑不出预热 + 至少 2 个统计样本" -ForegroundColor Red
    exit 2
}

# 逐样本逐实例累加；只保留预热之后的部分
$busyEngineSeconds = 0.0
$perEngine = @{}          # 实例名 → 忙秒
$csvRows = New-Object System.Collections.ArrayList
$keptSets = New-Object System.Collections.ArrayList

for ($i = $warmupSamples; $i -lt $data.Count; $i++) {
    $set = $data[$i]
    $null = $keptSets.Add($set)
    foreach ($s in $set.CounterSamples) {
        if ($s.InstanceName -notmatch $pidFilter) { continue }
        $busy = ($s.CookedValue / 100.0) * $SampleInterval
        $busyEngineSeconds += $busy
        if ($perEngine.ContainsKey($s.InstanceName)) {
            $perEngine[$s.InstanceName] += $busy
        } else {
            $perEngine[$s.InstanceName] = $busy
        }
        $null = $csvRows.Add(('{0},{1},{2:F2}' -f $set.Timestamp.ToString('yyyy-MM-dd HH:mm:ss.fff'), $s.InstanceName, $s.CookedValue))
    }
}

$engineCount = $perEngine.Count
$windowStart = $keptSets[0].Timestamp
$windowEnd = $keptSets[$keptSets.Count - 1].Timestamp
# 窗口秒数用"样本数 × 间隔"而不是首末时间戳差：首末差少了最后一个样本的时长。
$windowSeconds = $keptSets.Count * $SampleInterval

# --- 5. frame_time：优先日志，其次 -Fps -------------------------------------
$frameTimeMs = 0.0
$frames = 0.0
$frameTimeSource = ''

if (Test-Path $LogPath) {
    # 日志行格式：2026-11-03 14:20:31.123|info|Renderer.cpp:4636|...|Backend publication CPU timing: ...
    $stamps = New-Object System.Collections.ArrayList
    foreach ($line in (Get-Content -Path $LogPath -Encoding UTF8)) {
        if ($line -notmatch $PublicationPattern) { continue }
        if ($line.Length -lt 23) { continue }
        try {
            $ts = [datetime]::ParseExact($line.Substring(0, 23), 'yyyy-MM-dd HH:mm:ss.fff',
                [System.Globalization.CultureInfo]::InvariantCulture)
        } catch {
            continue
        }
        if ($ts -lt $windowStart -or $ts -gt $windowEnd) { continue }
        $null = $stamps.Add($ts)
    }
    if ($stamps.Count -ge 2) {
        $spanMs = ($stamps[$stamps.Count - 1] - $stamps[0]).TotalMilliseconds
        # 每行覆盖 120 帧，N 行覆盖 120×(N-1) 帧。只有两行时这就是"求差 ÷ 120 × 1000"。
        $frames = $FramesPerLogLine * ($stamps.Count - 1)
        $frameTimeMs = $spanMs / $frames
        $frameTimeSource = "magpie.log 的 $($stamps.Count) 行 publication timing（≈$frames 帧）"
    }
}

if ($frameTimeMs -le 0) {
    if ($Fps -gt 0) {
        $frameTimeMs = 1000.0 / $Fps
        $frames = $Fps * $windowSeconds
        $frameTimeSource = "-Fps $Fps（回退）"
    } else {
        Write-Host ''
        Write-Host "拿不到 frame_time：$LogPath 在采样窗口内没有至少两行 'Backend publication CPU timing'，" -ForegroundColor Red
        Write-Host '也没有给 -Fps。两个办法：' -ForegroundColor Yellow
        Write-Host '  1) 确认 Magpie 的日志开关打在 info 级、且确实在缩放（publication 每 120 帧一行）；' -ForegroundColor Yellow
        Write-Host '  2) 用 -Fps <稳态帧率> 直接给（例：-Fps 47）。' -ForegroundColor Yellow
        Write-Host '不要瞎猜 frame_time——ratio 的分子分母必须来自同一次运行。' -ForegroundColor Yellow
        exit 1
    }
}

# --- 6. ratio 与判据 --------------------------------------------------------
$busyPerFrame = $busyEngineSeconds / $frames
$frameTimeSeconds = $frameTimeMs / 1000.0
$ratio = $busyPerFrame / $frameTimeSeconds
$avgBusyEngines = $busyEngineSeconds / $windowSeconds

Write-Host ''
Write-Host '--- 读数 ---' -ForegroundColor Cyan
Write-Host ("engines={0}" -f $engineCount)
Write-Host ("busy_engine_seconds={0:F3}" -f $busyEngineSeconds)
Write-Host ("window_seconds={0:F1}  avg_busy_engines={1:F3}  (= 忙秒数 ÷ 窗口秒数)" -f $windowSeconds, $avgBusyEngines)
Write-Host ("frames={0:F1}" -f $frames)
Write-Host ("frame_time_ms={0:F3}  fps={1:F1}  (来源：{2})" -f $frameTimeMs, (1000.0 / $frameTimeMs), $frameTimeSource)
Write-Host ("ratio={0:F3}" -f $ratio) -ForegroundColor Green
Write-Host '（ratio = 每帧的引擎忙秒 ÷ frame_time；并行引擎求和可以 >1，ratio=1.2 表示已饱和，不是算错）'

if ($engineCount -eq 0) {
    Write-Host ''
    Write-Host "警告：窗口内没有属于 PID $targetPid 的 GPU 引擎实例，ratio 无意义。" -ForegroundColor Red
    Write-Host '  多半是进程在跑但没在缩放，或者 PID 已经换过一轮。' -ForegroundColor Yellow
}

Write-Host ''
Write-Host '--- 判据 ---' -ForegroundColor Cyan

# 有效性闸门：窗口内平均忙引擎数 < 0.05 等价于"整段 GPU 空闲"——那不是"台阶成立"，
# 是没采到东西（进程在跑但没在缩放 / 场景停在菜单 / PID 换了 / 选错了机器）。
# 这一档必须挡住，否则会得出"主线成立"的假结论，比不测更糟。
$sampleValid = $busyEngineSeconds -ge (0.05 * $windowSeconds)
if (-not $sampleValid) {
    Write-Host '无效样本：采样窗口内 GPU 近乎空闲，不给判据。' -ForegroundColor Red
    Write-Host ('  avg_busy_engines={0:F3} < 0.05，ratio={1:F3} 没有意义。' -f $avgBusyEngines, $ratio) -ForegroundColor Red
    Write-Host '  常见原因：Magpie 没在缩放（停在菜单/未启动缩放）、场景静止、-ProcessName 指错、' -ForegroundColor Yellow
    Write-Host '  或者本来就是在没有游戏负载的机器上测。确认场景在跑后重测。' -ForegroundColor Yellow
} elseif ($ratio -gt $RatioStop) {
    Write-Host '停：求和是吞吐的必然，max 天花板不存在，不做阶段 2，主线转缩岛' -ForegroundColor Red
} elseif ($ratio -ge $RatioDiscount) {
    Write-Host ('台阶打 (1-ratio) 折扣，验收线按实测下调（当前 1-ratio={0:F3}）' -f (1.0 - $ratio)) -ForegroundColor Yellow
} else {
    Write-Host '台阶成立，按 123–125 / 66–70 fps 验收' -ForegroundColor Green
}

# 分引擎明细，贴进 docs/measurements.md 时有用
if ($engineCount -gt 0) {
    Write-Host ''
    Write-Host '--- 按引擎（窗口内忙秒）---' -ForegroundColor Cyan
    $perEngine.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object {
        Write-Host ("  {0,-72} {1,8:F3} s" -f $_.Key, $_.Value)
    }
}

# --- 7. CSV ----------------------------------------------------------------
# 头部用 # 注释写参数与结论，正文每样本每实例一行，便于直接贴进 measurements.md
$header = New-Object System.Collections.ArrayList
$null = $header.Add('# G0 GPU busy / measure-gpu-busy.ps1')
$null = $header.Add(('# {0:yyyy-MM-dd HH:mm:ss}  source={1}' -f (Get-Date), $SourceDir))
$null = $header.Add(('# process={0}.exe pid={1} filter={2}' -f $ProcessName, $targetPid, $pidFilter))
$null = $header.Add(('# counter={0}' -f $CounterPath))
$null = $header.Add(('# sample_interval_s={0} warmup_s={1} window_s={2} kept_samples={3}' -f $SampleInterval, $WarmupSeconds, $Seconds, $keptSets.Count))
$null = $header.Add(('# engines={0} busy_engine_seconds={1:F3} window_seconds={2:F1}' -f $engineCount, $busyEngineSeconds, $windowSeconds))
$null = $header.Add(('# frame_time_source={0}' -f $frameTimeSource))
$null = $header.Add(('# frames={0:F1} frame_time_ms={1:F3} fps={2:F1}' -f $frames, $frameTimeMs, (1000.0 / $frameTimeMs)))
$null = $header.Add(('# ratio={0:F3}  thresholds: stop>{1} discount>={2}' -f $ratio, $RatioStop, $RatioDiscount))
$null = $header.Add(('# valid={0}  (false = 窗口内 GPU 近乎空闲，ratio 不可用于判据)' -f $sampleValid))
$null = $header.Add('timestamp,instance,utilization')
$header + $csvRows | Set-Content -Path $OutFile -Encoding UTF8
Write-Host ''
Write-Host "CSV: $OutFile（$($csvRows.Count) 行，含 # 头部注释）" -ForegroundColor Green
Write-Host ''
if (-not $sampleValid) {
    Write-Host '退出码 1：样本无效，未给判据。' -ForegroundColor Red
    exit 1
}
exit 0
