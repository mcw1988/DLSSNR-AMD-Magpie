<#
.SYNOPSIS
    从 magpie.log 采集 FSR3 槽环的现场读数，落一行 CSV；-Summary 时按 docs/multislot.md §6 判通过/失败。

.DESCRIPTION
    **只做"采集与汇总"，不做"跑批"**：不启动/重启 Magpie、不改 config、不轮换场景。
    跑批由人来做（换效果组、换 scale、等稳态），本脚本只负责把日志变成可比较的数字，
    避免"每次手抄一遍、抄错一位"。

## 采什么

两类行（都在 `logs\magpie.log`，格式 `%Y-%m-%d %H:%M:%S.%e|%l|%s:%#|%!|%v`）：

1. `Backend publication CPU timing: samples=120 ... fenceAvgMs=...`
   —— 发布路径每 120 帧一行（`Renderer.cpp:4634-4638`）。
   **相邻两行的时间戳差 ÷ 120 = 该段帧时间**；窗口内 `fenceAvgMs` 取均值。
   `fenceAvgMs` 就是串行点 ③（发布排空）的等待均值，阶段 1 的判据要用它。
2. `fsr3-slot: draws=... slotWaitAvgMs=... islandValueLagAvg=... islandValueLagMax=... harvestMiss=... framesDropped=... slots=... lag=...`
   —— 补丁每 120 次 Draw 一行。`slotWaitAvgMs` / `islandValueLagAvg` 取均值，
   `islandValueLagMax` / `framesDropped` 取窗口内最大，`slots` / `lag` 取末值（配置）

> 兼容 `failed=` 老字段名（P2-5 改名前抓的日志），两个名字都认。

## 判据（阈值见脚本头部常量，出处写在注释里）

- **阶段 1（`lag=0`）**：性能**应当不变**（可证伪断言）。与基线（`-Baseline` 指的 CSV，
  通常是 `slots=1` 那一次）的 fps 差 ≤ ±3 才算通过；掉 ≥3 fps 算失败。
- **阶段 2（`lag=1`）**：0.5 / 0.35 档 ≥ 123 fps，1.0 档 ≥ 66 fps（1.0 档取保守下界，
  >70 记超预期）；另看 `harvestMiss`：>20% 说明岛比帧间隔长，应退阶段 1。

## 示例

```powershell
# 采集一次（Magpie 已经跑过一段稳态），落一行
powershell -ExecutionPolicy Bypass -File scripts/measure-fsr-ring.ps1 `
    -LogPath D:\Magpie-NR-045\logs\magpie.log -Config 'lag1-s0.5' -Scale 0.5 `
    -OutCsv .\ring.csv

# 与基线比阶段 1
powershell -ExecutionPolicy Bypass -File scripts/measure-fsr-ring.ps1 `
    -LogPath ...\magpie.log -Config 'slot3-lag0' -Baseline .\ring-baseline.csv -OutCsv .\ring.csv -Summary

# 只汇总已有 CSV
powershell -ExecutionPolicy Bypass -File scripts/measure-fsr-ring.ps1 -OutCsv .\ring.csv -Summary
```

## 退出码

    0  正常
    1  参数缺失 / 日志不存在 / 日志里一行都解析不出来 / -Summary 时 CSV 不存在
#>
[CmdletBinding()]
param(
    # magpie.log；只在"采集"时需要，纯 -Summary 时可省
    [string]$LogPath,
    # 配置标签，写进 CSV 的 config 列（例：slots=1 / slot3-lag0 / lag1-s0.5）
    [string]$Config = 'default',
    # 网络档位；阶段 2 的判据按它选阈值
    [ValidateSet('0.5', '0.35', '1.0')]
    [string]$Scale,
    # 基线 CSV（阶段 1 判据用；通常是 slots=1 那一次采集的结果）
    [string]$Baseline,
    # 输出/汇总的 CSV
    [string]$OutCsv,
    # 汇总模式：读 CSV 打对照表并判定
    [switch]$Summary
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# --- 判据阈值：全部来自 docs/multislot.md §6，改这里等于改验收标准 ---------------
# §6.1 A2：阶段 1 性能应当不变（可证伪断言）。与基线差 ≤ ±3 fps 才算通过。
$FpsToleranceStage1 = 3.0
# §6.2 B2/B4：阶段 2 的帧数地板。0.5 与 0.35 同档，1.0 档取保守下界（>70 记超预期）。
$Stage2FpsFloor = @{
    '0.5'  = 123.0
    '0.35' = 123.0
    '1.0'  = 66.0
}
# §6.2 B1：harvestMiss > 20% 说明岛比帧间隔长，退阶段 1；> 5% 要记录（§6.3）。
$HarvestMissFailPct = 20.0
$HarvestMissWarnPct = 5.0
# 发布路径每 120 帧一行（Renderer.cpp:4634-4638）
$FramesPerPublicationLine = 120

if (-not $OutCsv) {
    Write-Host '-OutCsv 必填（采集要写、汇总要读）' -ForegroundColor Red
    exit 1
}
if (-not $Summary -and -not $LogPath) {
    Write-Host '要么给 -LogPath 采集，要么给 -Summary 汇总；两者都没给。' -ForegroundColor Red
    exit 1
}

# --- 小工具 ----------------------------------------------------------------
function Get-LineTimestamp([string]$line) {
    # spdlog 格式固定：yyyy-MM-dd HH:mm:ss.fff 在前 23 个字符
    if ($line.Length -lt 23) { return $null }
    try {
        return [datetime]::ParseExact($line.Substring(0, 23), 'yyyy-MM-dd HH:mm:ss.fff',
            [System.Globalization.CultureInfo]::InvariantCulture)
    } catch {
        return $null
    }
}

function Get-Field([string]$line, [string]$pattern) {
    $m = [regex]::Match($line, $pattern)
    if (-not $m.Success) { return $null }
    return $m.Groups[1].Value
}

function Get-FieldDouble([string]$line, [string]$pattern) {
    $v = Get-Field $line $pattern
    if ($null -eq $v) { return $null }
    return [double]::Parse($v, [System.Globalization.CultureInfo]::InvariantCulture)
}

# --- 采集 ------------------------------------------------------------------
$row = $null
if ($LogPath) {
    if (-not (Test-Path $LogPath)) {
        Write-Host "日志不存在：$LogPath" -ForegroundColor Red
        exit 1
    }

    $publicationStamps = New-Object System.Collections.ArrayList
    $fenceSamples = New-Object System.Collections.ArrayList
    $slotLines = New-Object System.Collections.ArrayList

    foreach ($line in (Get-Content -Path $LogPath -Encoding UTF8)) {
        if ($line -match 'Backend publication CPU timing:') {
            $ts = Get-LineTimestamp $line
            $fence = Get-FieldDouble $line 'fenceAvgMs=([\d.]+)'
            if ($null -ne $ts -and $null -ne $fence) {
                $null = $publicationStamps.Add($ts)
                $null = $fenceSamples.Add($fence)
            }
            continue
        }
        if ($line -match 'fsr3-slot: draws=') {
            $null = $slotLines.Add($line)
        }
    }

    if ($publicationStamps.Count -lt 2 -and $slotLines.Count -eq 0) {
        Write-Host "日志里没有可用的 publication timing 行，也没有 fsr3-slot: 行：$LogPath" -ForegroundColor Red
        Write-Host '  确认日志级别是 info、且确实在缩放（publication 每 120 帧一行）。' -ForegroundColor Yellow
        exit 1
    }

    # 帧时间：相邻两行时间戳差 ÷ 120，逐段求平均（N 行 → N-1 段）
    $frameTimeMs = 0.0
    $frames = 0.0
    if ($publicationStamps.Count -ge 2) {
        $sumMs = 0.0
        for ($i = 1; $i -lt $publicationStamps.Count; $i++) {
            $sumMs += ($publicationStamps[$i] - $publicationStamps[$i - 1]).TotalMilliseconds
        }
        $frameTimeMs = $sumMs / ($publicationStamps.Count - 1) / $FramesPerPublicationLine
        $frames = $FramesPerPublicationLine * ($publicationStamps.Count - 1)
    } else {
        Write-Host "只有 $($publicationStamps.Count) 行 publication timing，算不出帧时间（至少要 2 行）" -ForegroundColor Yellow
    }

    $fenceAvg = 0.0
    if ($fenceSamples.Count -gt 0) {
        $acc = 0.0
        foreach ($v in $fenceSamples) { $acc += $v }
        $fenceAvg = $acc / $fenceSamples.Count
    }

    # fsr3-slot: 行：均值 / 末值 / 最大
    $slotWaitAvg = 0.0
    $lagAvg = 0.0
    $lagMax = 0
    $droppedMax = 0
    $harvestMissPct = -1.0
    $slots = -1
    $lag = -1
    if ($slotLines.Count -gt 0) {
        $n = $slotLines.Count
        $swSum = 0.0
        $lagSum = 0.0
        $hmSum = 0.0
        $hmTotal = 0.0
        $hmSeen = $false
        foreach ($line in $slotLines) {
            $sw = Get-FieldDouble $line 'slotWaitAvgMs=([\d.]+)'
            if ($null -ne $sw) { $swSum += $sw }
            $la = Get-FieldDouble $line 'islandValueLagAvg=([\d.]+)'
            if ($null -ne $la) { $lagSum += $la }
            $lm = Get-Field $line 'islandValueLagMax=(\d+)'
            if ($null -ne $lm) { $v = [int]$lm; if ($v -gt $lagMax) { $lagMax = $v } }
            # failed= 是改名前的老字段名，两个都认
            $fd = Get-Field $line '(?:framesDropped|failed)=(\d+)'
            if ($null -ne $fd) { $v = [int]$fd; if ($v -gt $droppedMax) { $droppedMax = $v } }
            $hm = Get-Field $line 'harvestMiss=(\d+)/(\d+)'
            if ($null -ne $hm) {
                $m = [regex]::Match($line, 'harvestMiss=(\d+)/(\d+)')
                $hmSum += [double]$m.Groups[1].Value
                $hmTotal += [double]$m.Groups[2].Value
                $hmSeen = $true
            }
            $sl = Get-Field $line 'slots=(\d+)'
            if ($null -ne $sl) { $slots = [int]$sl }
            $lg = Get-Field $line 'lag=(\d+)'
            if ($null -ne $lg) { $lag = [int]$lg }
        }
        $slotWaitAvg = $swSum / $n
        $lagAvg = $lagSum / $n
        if ($hmSeen -and $hmTotal -gt 0) { $harvestMissPct = 100.0 * $hmSum / $hmTotal }
    }

    $fps = 0.0
    if ($frameTimeMs -gt 0) { $fps = 1000.0 / $frameTimeMs }

    # 必须用有序字典：普通 @{} 是 HashTable，属性顺序不定，CSV 列会与表头错位
    $row = [pscustomobject][ordered]@{
        config           = $Config
        scale            = $(if ($Scale) { $Scale } else { '' })
        slots            = $slots
        lag              = $lag
        frames           = [math]::Round($frames, 0)
        fps              = [math]::Round($fps, 2)
        frame_time_ms    = [math]::Round($frameTimeMs, 3)
        fence_avg_ms     = [math]::Round($fenceAvg, 3)
        slot_wait_avg_ms = [math]::Round($slotWaitAvg, 4)
        lag_avg          = [math]::Round($lagAvg, 2)
        lag_max          = $lagMax
        harvest_miss_pct = [math]::Round($harvestMissPct, 1)
        frames_dropped   = $droppedMax
        publication_lines = $publicationStamps.Count
        slot_lines       = $slotLines.Count
        log              = (Split-Path $LogPath -Leaf)
    }

    Write-Host ''
    Write-Host "--- 采集：$Config ---" -ForegroundColor Cyan
    Write-Host ("publication 行={0}  fsr3-slot 行={1}  slots={2} lag={3}" -f `
        $publicationStamps.Count, $slotLines.Count, $slots, $lag)
    Write-Host ("frames={0}  frame_time_ms={1:F3}  fps={2:F2}" -f $frames, $frameTimeMs, $fps)
    Write-Host ("fenceAvgMs 均值={0:F3}  slotWaitAvgMs 均值={1:F4}  lagAvg={2:F2}  lagMax={3}  framesDropped(max)={4}" -f `
        $fenceAvg, $slotWaitAvg, $lagAvg, $lagMax, $droppedMax)
    if ($harvestMissPct -ge 0) {
        Write-Host ("harvestMiss={0:F1}%  (>{1}% 判失败，退阶段 1；>{2}% 要记录)" -f `
            $harvestMissPct, $HarvestMissFailPct, $HarvestMissWarnPct)
    }

    # 写 CSV：首次写 # 头 + 列名；之后只追加数据行
    $columns = 'config,scale,slots,lag,frames,fps,frame_time_ms,fence_avg_ms,slot_wait_avg_ms,lag_avg,lag_max,harvest_miss_pct,frames_dropped,publication_lines,slot_lines,log'
    $dataLine = ($row.PSObject.Properties | ForEach-Object { "$($_.Value)" }) -join ','
    if (-not (Test-Path $OutCsv)) {
        $header = @(
            '# measure-fsr-ring.ps1（只采集与汇总，不跑批）',
            ('# {0:yyyy-MM-dd HH:mm:ss}' -f (Get-Date)),
            ('# thresholds: stage1_tolerance_fps={0} stage2_floor={{0.5:123, 0.35:123, 1.0:66}} harvestMiss_fail={1}%' -f $FpsToleranceStage1, $HarvestMissFailPct),
            $columns
        )
        Set-Content -Path $OutCsv -Value $header -Encoding UTF8
    }
    Add-Content -Path $OutCsv -Value $dataLine -Encoding UTF8
    Write-Host "CSV: $OutCsv（已追加 1 行）" -ForegroundColor Green
}

# --- 汇总 ------------------------------------------------------------------
# 在 if 外初始化：末尾的退出码要读它，而 StrictMode 下未定义的变量会直接报错。
$failures = 0
if ($Summary) {
    if (-not (Test-Path $OutCsv)) {
        Write-Host "要汇总的 CSV 不存在：$OutCsv" -ForegroundColor Red
        exit 1
    }
    $rows = @(Get-Content -Path $OutCsv -Encoding UTF8 |
        Where-Object { $_ -and -not $_.StartsWith('#') } |
        Select-Object -Skip 1 |                       # 跳过列名行
        ForEach-Object {
            $f = $_ -split ','
            if ($f.Count -lt 16) { return }
            [pscustomobject]@{
                config = $f[0]; scale = $f[1]; slots = $f[2]; lag = $f[3]
                fps = [double]$f[5]; frame_time_ms = [double]$f[6]
                fence_avg_ms = [double]$f[7]; slot_wait_avg_ms = [double]$f[8]
                lag_avg = [double]$f[9]; harvest_miss_pct = [double]$f[11]
                frames_dropped = [int]$f[12]
            }
        })
    if ($rows.Count -eq 0) {
        Write-Host 'CSV 里没有数据行' -ForegroundColor Red
        exit 1
    }

    # 基线 fps：给了 -Baseline 就按 scale 匹配，否则取第一行
    $baselineFps = $null
    if ($Baseline) {
        if (-not (Test-Path $Baseline)) {
            Write-Host "基线 CSV 不存在：$Baseline" -ForegroundColor Red
            exit 1
        }
        $baseRows = @(Get-Content -Path $Baseline -Encoding UTF8 |
            Where-Object { $_ -and -not $_.StartsWith('#') } |
            Select-Object -Skip 1 |
            ForEach-Object { $f = $_ -split ','; if ($f.Count -ge 16) { [pscustomobject]@{ scale = $f[1]; fps = [double]$f[5] } } })
        if ($baseRows.Count -gt 0) { $baselineFps = $baseRows[0] }
    }

    Write-Host ''
    Write-Host '=== 汇总（判据出处：docs/multislot.md §6）===' -ForegroundColor Cyan
    Write-Host ('{0,-14} {1,-6} {2,-7} {3,8} {4,9} {5,9} {6,9} {7,8} {8,7}  {9}' -f `
        'config', 'scale', 'slots/lag', 'fps', 'frame_ms', 'fence_ms', 'slotWait', 'harvest', 'drop', '判定')
    $failures = 0
    foreach ($r in $rows) {
        $verdict = ''
        $color = 'Gray'
        $isStage2 = ($r.lag -eq '1')
        if ($r.slots -eq '-1' -or $r.lag -eq '-1') {
            $verdict = '无 fsr3-slot 行，判不了阶段'
            $color = 'Gray'
        } elseif ($isStage2) {
            $floor = $null
            if ($r.scale -and $Stage2FpsFloor.ContainsKey($r.scale)) { $floor = $Stage2FpsFloor[$r.scale] }
            if ($null -eq $floor) {
                $verdict = '阶段 2 需 -Scale 0.5/0.35/1.0'
                $color = 'Yellow'
            } elseif ($r.fps -lt $floor) {
                $verdict = ('失败：{0} < {1} fps' -f $r.fps, $floor)
                $color = 'Red'
                $failures++
            } elseif ($r.harvest_miss_pct -gt $HarvestMissFailPct) {
                $verdict = ('失败：harvestMiss {0}% > {1}%' -f $r.harvest_miss_pct, $HarvestMissFailPct)
                $color = 'Red'
                $failures++
            } else {
                $verdict = ('通过：≥{0} fps' -f $floor)
                $color = 'Green'
            }
        } else {
            # 阶段 1：性能应当不变
            if ($null -eq $baselineFps) {
                $verdict = '阶段 1 需 -Baseline 对照'
                $color = 'Yellow'
            } else {
                $delta = $r.fps - $baselineFps.fps
                if ([math]::Abs($delta) -le $FpsToleranceStage1) {
                    $verdict = ('通过：与基线差 {0:+0.0;-0.0} fps（≤±{1}）' -f $delta, $FpsToleranceStage1)
                    $color = 'Green'
                } elseif ($delta -lt 0) {
                    $verdict = ('失败：比基线低 {0:F1} fps（掉 ≥{1} 算失败）' -f (-$delta), $FpsToleranceStage1)
                    $color = 'Red'
                    $failures++
                } else {
                    $verdict = ('意外变快 {0:+0.0} fps —— 记下来查原因，别急着庆祝' -f $delta)
                    $color = 'Yellow'
                }
            }
        }
        Write-Host ('{0,-14} {1,-6} {2,-7} {3,8:F2} {4,9:F3} {5,9:F3} {6,9:F4} {7,7:F1}% {8,7}  {9}' -f `
            $r.config, $r.scale, ("{0}/{1}" -f $r.slots, $r.lag), $r.fps, $r.frame_time_ms, `
            $r.fence_avg_ms, $r.slot_wait_avg_ms, $(if ($r.harvest_miss_pct -ge 0) { $r.harvest_miss_pct } else { 0 }), $r.frames_dropped, $verdict) -ForegroundColor $color
    }
    Write-Host ''
    if ($failures -eq 0) {
        Write-Host '汇总：没有失败项。' -ForegroundColor Green
        exit 0
    }
    # 汇总的全部意义就是给 CI/验收当闸门：有失败项还退 0，等于把闸门废掉。
    Write-Host ("汇总：{0} 项失败。" -f $failures) -ForegroundColor Red
    exit 1
}

exit 0
