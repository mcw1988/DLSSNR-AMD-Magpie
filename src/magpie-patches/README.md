# `magpie-patches/` — 对宿主 Magpie（GPL-3.0）的实验性补丁

本目录**只存放补丁文本与说明，不分发宿主源码**，政策与 [`../opennr-patches/`](../opennr-patches/) 一致：
Magpie 是 GPL-3.0，我们按 GPL-3 第 6 条只做聚合分发并给出对应源码地址（见
[../../THIRD-PARTY-NOTICES.md](../../THIRD-PARTY-NOTICES.md)），本仓库自身代码仍是 MIT，两者不合并。

补丁内容对应设计稿 **[docs/multislot.md](../../docs/multislot.md)**（§3 数据结构与围栏协议、§4 不变量、
§5 配置面、§6 验收、§7 实施拆分）。**改补丁前先读那一篇**，不要在这里另起设计。

## 基准

| 项 | 值 |
|---|---|
| 上游仓库 | <https://github.com/SAOG0721/Magpie> |
| 分支 | `experimental` |
| 基准提交 | **`6940f84`** `fix(release): keep worktree outputs in the workspace release directory` |
| 改动文件 | `src/Magpie.Core/FSR3Upscaler.cpp`、`src/Magpie.Core/FSR3Upscaler.h` |

**`FSR3ZeroMVUpscaler.cpp` 不在补丁范围**：它与目标文件同构，但在链里不承载 NR（岛只有 1.4 ms），
拆它没有收益、只增加风险面（multislot.md §3.10）。**`Renderer.cpp` 也不在范围**——发布排空
（`Renderer.cpp:4614-4624`）是第三个串行点，一旦顺手改掉，"阶段 1 收益为 0"这个结论就再也无法归因
（multislot.md §7.3）。

## 两个补丁

必须**按序**应用，不能合并成一个：回滚单位就是补丁单位。

| 补丁 | 阶段 | 作用 | 环境变量 | 预期收益 |
|---|---|---|---|---|
| `0001-fsr3-slot-ring.patch` | 阶段 1 | `FSR3Upscaler` 的共享纹理/allocator/command list 由单份改为 **N 槽环形**（默认 3，上限 4），每槽记自己的 `islandValue`；认领槽只等自己的岛；新增 `Drain()` 覆盖与 `fsr3-slot:` 统计行 | `MAGPIE_FSR_SLOTS`（默认 3） | **0**（设计断言：输出语义与上游逐帧一致，性能应当不变） |
| `0002-fsr3-output-lag.patch` | 阶段 2 | 在 0001 之上加"输出晚一帧"：`deque<Result>` 深度恒等于 lag，收割放在 `Draw` 开头（只做 GPU 侧等待），冷启动退回同步语义 | `MAGPIE_FSR_OUTPUT_LAG`（默认 0，仅 0/1） | 帧时间从「求和」变「取大」：s=0.5/0.35 → 约 123 fps；s=1.0 → 66（保守）～70 |

阶段 1 **单独上线不会变快**，这不是失败：主机每发布一帧都要在 `Renderer.cpp:4614-4624` 把 D3D11
队列排空一次，`:497` 的 CPU 阻塞在稳态下本来就是空转。阶段 1 的价值是**解除 allocator / 纹理的复用
依赖**，也就是阶段 2 的前置条件。详见 multislot.md §1.3。

`0002` 必须叠在 `0001` 之上（同一个文件、同一段代码），单独应用会失败。

## 怎么应用

```powershell
# 干跑：只校验基准提交与 git apply --check，不改文件
powershell -ExecutionPolicy Bypass -File scripts/apply-magpie-patch.ps1 -Stage 1 -CheckOnly

# 应用阶段 1 / 阶段 2（-Stage 2 会按序打两个）
powershell -ExecutionPolicy Bypass -File scripts/apply-magpie-patch.ps1 -Stage 2

# 默认 -SourceDir 是 <本仓库>\..\magpie，也可以指到任意 fork 工作副本
powershell -ExecutionPolicy Bypass -File scripts/apply-magpie-patch.ps1 -SourceDir D:\Magpie-exp -Stage 1
```

手工等价命令：

```powershell
git -C <Magpie 源码目录> rev-parse --short HEAD      # 必须 6940f84
git -C <Magpie 源码目录> apply --check <本目录>\0001-fsr3-slot-ring.patch
git -C <Magpie 源码目录> apply <本目录>\0001-fsr3-slot-ring.patch
git -C <Magpie 源码目录> apply <本目录>\0002-fsr3-output-lag.patch   # 阶段 2
```

> **补丁只适用于基准提交 `6940f84`。** 宿主更新（哪怕只动了 `FSR3Upscaler.cpp` 的相邻行）后
> `git apply` 会失败，这是设计上的行为——**不要** `--3way` 硬合，也不要"看着改改"，必须按新提交
> 重新核对上下文重制补丁，并重新跑 multislot.md §6.1 的 A1/A3（语义等价与逐位一致）。

## 构建

宿主用 MSBuild，不是本仓库的 `build-all.ps1`：

```powershell
# 1) 打开 FSR3 编译开关（BuildOptions.props 明确要求走 .user 覆盖，不要直接改 props）
#    src\BuildOptions.props.user:
#      <Project><PropertyGroup><EnableFSR3ZeroMV>true</EnableFSR3ZeroMV></PropertyGroup></Project>
# 2) 出 Release
powershell -ExecutionPolicy Bypass -File scripts\Build-Release.ps1
```

开关名是 `EnableFSR3ZeroMV`（`src/BuildOptions.props:41`），它定义 `MP_ENABLE_FSR3_ZEROMV`；
本补丁的全部代码都在该宏的 `#ifdef` 内，宏关掉时只多一个 `Drain()` 空实现，行为不变。

## 运行期配置

| 变量 | 默认 | 范围 | 含义 |
|---|---|---|---|
| `MAGPIE_FSR_SLOTS` | 3 | 1–4（越界夹到 4） | 环形槽数。`1` = 完全回到上游行为 |
| `MAGPIE_FSR_OUTPUT_LAG` | 0 | 0–1（`lag=1` 且 `slots<2` 时回落 0 并 warn） | `1` = 输出晚一帧（阶段 2） |

**两个值都只在 `Initialize()` 里读一次，不允许运行期切换**（`lag` 切换等价于一次 Resize 级抖动）。
`Resize()` 走 `Initialize()`，所以改分辨率会重新读一次并重建槽环，顺带再吃一次"一帧重复"。

生效值与统计都打在 `magpie.log` 里：

```
FSR3 slot ring: slots=3 lag=0
fsr3-slot: draws=120 slotWaitAvgMs=0.0040 islandValueLagAvg=0.50 islandValueLagMax=2 harvestMiss=0/120 failed=0 slots=3 lag=0
fsr3-slot: drain islands=3412 results=1
```

第三行只在**停流**时出现一次（`Renderer.cpp:3109-3121` 的 `_DrainNgxConsumers`）。

### `Drain()` 的语义：只等，不补拷

`Drain()` 是 `WaitForQueue`（等所有已提交的岛完成）→ 清空结果 FIFO → 打上面那行日志 → 返回 `true`。
**它不往任何纹理里拷东西**，理由三条（2026-11-03 裁决，优先于 multislot.md §3.8）：

1. 调用点是**停流**（`_ReleaseNgxConsumers` 之前），不是每帧；
2. 在此之前 `_PublishBackendTexture` 已经对最后一帧做过
   `Signal → SetEventOnCompletion → Flush → 等围栏`（`Renderer.cpp:4614-4624`），
   那一帧早被拷进呈现环交给系统了——再往 `ctx.output` 写一次 U(N)，画面上不会有任何变化；
3. `Drain()` 没有目标纹理参数，**缓存最近一次 `ctx.output` 指针**在 `Resize()` 重建 effect drawer
   纹理后立即悬空，是"零视觉收益 + 多一条失效路径"。

`lag=1` 时停流丢掉最后一帧（呈现 U(N−1)）是**延迟语义本身**，不是 bug；丢了几帧由
`results=` 字段记账，不在析构里静默丢弃。

判据见 multislot.md §6：阶段 1 看 `slotWaitAvgMs`（应 →0）与"性能应当不变 ≥3 fps 才算失败"；
阶段 2 看 `harvestMiss`（应 ≤5%）与三档帧数。

## 显存代价（multislot.md §3.6）

4K 输入（3840×2054）/ 输出（3840×2160）BGRA8 下每槽约 96 MB（输入 31.6 + 输出 33.2 + 运动矢量 31.6），
3 槽净增约 193 MB，2 槽约 96 MB。aux 五张、descriptor heap、围栏、队列保持单份不变。

## 与 `mochinr` 的联动

`src/mochinr/mochinr.cpp` 的 `inflight` 门是**岛提交路径中间**的 CPU 阻塞等待（≤200 ms，
`:877-895`）。必须满足 `inflight ≥ slots + lag`：3 槽 + lag=1 → **≥4**；部署包当前是 2。
不满足时阶段 2 会被这个门限流，表现为"改了但没变"（multislot.md §4 F11）。
观测点是 `mochinr.log` 的 `perf:` 行——`waits=` 与 `waitMs` 若随槽数上升，说明限流点已经挪进运行时。

## 回退

补丁不提交到宿主的 git 历史，所以回退就是：

```powershell
git -C <Magpie 源码目录> checkout -- src/Magpie.Core/FSR3Upscaler.cpp src/Magpie.Core/FSR3Upscaler.h
```

只保留文件但想关掉功能，不需要还原源码：`MAGPIE_FSR_SLOTS=1` + `MAGPIE_FSR_OUTPUT_LAG=0` 即等价于
上游行为（`slots=1` 时只有一个槽、一个 allocator，认领等待等价于上游的"等上一帧"）。

任意 corrupted frame、30 min 内一次 TDR/`DEVICE_REMOVED`、或阶段 2 未达 B2/B3，按 multislot.md §6.5
回滚；默认值改回 `slots=1, lag=0`，补丁留在库里但不默认启用。
