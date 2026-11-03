# 结论、撤回的结论与未解问题

这份文档只记「现在能站得住的判断」，以及**被推翻的判断**——后者同样重要，避免后来的人
沿着已经证伪的思路再走一遍。数据出处见 [measurements.md](measurements.md)。

---

## 1. 已经确认

| # | 结论 | 证据 |
|---|---|---|
| 1 | Magpie 里 Home 打不开面板，是三处叠加：`GetAsyncKeyState` 被 ReShade 屏蔽、`PostMessage(WM_MOUSEMOVE)` 被 ReShade 丢弃、`MSG.pt` 与 `lParam` 相同导致坐标被二次换算 | 三版对照 + `test-new\*.bmp` |
| 2 | 两条 NR add-on 不能同时加载，会互相抢 present | 旧包用 `.off` 互斥；同时加载时帧生成异常 |
| 3 | mochizuki 的 ACO 判定**不兼容非 ASCII 路径**，同一套资产换目录即失效 | `ACO asked for, but ... has no records` vs `ACO: the network runs ACO's machine code` |
| 4 | OpenNR 在收到 swapchain/设备销毁后会把自己置为 off，且**不会自动恢复**，只有重启进程才重新挂载 | `off: the game destroyed its D3D12 device; restart the game to re-enable` |
| 5 | 因此：Magpie 里换效果组 = 停缩放 = 设备销毁 = NR 失效。这不是"效果组坏了" | 同上；停/开捕获无效，重启 Magpie 有效 |
| 6 | 真实 4K 下 NR 不是只有网络那 5.3 ms：光流、双向拷贝、桥接都在里面 | `optical flow: FidelityFX, 3840x2160 frame ... into the 1920x1080 raster` |
| 7 | `Scale` 只缩网络，不缩光流与拷贝；0.6→0.5 只省约 1.9 ms/次 | 同上 + `nrbench` 线性关系 |
| 8 | 补帧开启时，NR 的成本按倍率翻倍（mochizuki 有逐帧计数，比值 2.00） | `after 3311 frames` / 49.1 s / 33.6 fps |

---

## 2. 为什么基准里的领先没有出现

上游基准：mochizuki 3.27 ms vs lmxxf 5.487 ms（同为 1080 档）。实机却是 64 帧 vs 71 帧。

```
lmxxf      ：14.08 ms = 游戏 + 5.49 (网络) + 大约 1 ms (拷贝/回写)
mochizuki  ：15.63 ms = 游戏 + 3.27 (网络) + 光流 + 4K 拷贝 + 桥接等待
```

两边 `游戏` 相同，网络我们省 2.22 ms，但要额外付出约 3.77 ms 的非网络开销，净亏 1.55 ms。

这 3.77 ms 里最大的一块是 **4K FidelityFX 光流**：DLSS5/lmxxf 的时域不使用运动矢量，
根本没有这一步；mochizuki 的时域要对齐历史，就必须有矢量，而它把矢量算在帧尺寸上。

> 已补测：把 Motion 关掉后原生帧 64 → **68–75**（fence 9–10 → 4.6–5.5），
> 即 **4K 光流约 2.3 ms/帧**；剩下的约 1.4 ms 是 4K 拷贝与 D3D12↔Vulkan 桥接。
> 关掉后整体已顶到游戏内的 75 限帧，与 lmxxf 持平甚至略好。

---

## 3. 已撤回的结论

| 曾经的判断 | 为什么撤回 |
|---|---|
| 「原生 34 帧是帧同步量化（错过一格等一整格）」 | 0.6.11 上 NR 开/关对照：NR 关 50 帧、NR 开 33–34 帧，且 `fenceAvgMs` 从 4.2 涨到 12.5。34 是真实开销，不是节奏问题 |
| 「Bicubic 替掉 4K XeSS_SR 省 6–7 ms」 | 那两段日志里 NR 已被停用，比较条件不成立；且按要求已回滚为 XeSS_SR，不再采用 Bicubic |
| 「x3 比 x2 慢 2.9 ms，说明 NR 每帧多跑一次」 | 9070 XT 已跑满 304 W，FG 自身多生成一帧就能解释这个差值，不能作为 NR 计数的证据 |

第三条的教训：**只有在同一倍率下开关 NR 的差值**，才能把 NR 的成本从补帧里分离出来。

---

## 4. 未解问题

1. ~~3.77 ms 的构成~~ —— 已测：光流约 2.3 ms，其余约 1.4 ms 是拷贝与桥接。
   注意 **Motion 是开关，不是三选一**（面板里没有"estimator"可选项，estimator 只在
   FidelityFX 失败时自动接管）。
2. **lmxxf 到底跑几次**：它没有逐帧日志，只有 `on_present` 符号。若它确实每个呈现帧都跑，
   说明它单次足够便宜；若只跑一次，说明挂载点不同，需要查它绑的是哪个 swapchain。
3. **x3 下的 D3D12 报错**（2026-11-01 归因修正）：该红字**与 3x 补帧无关**，
   实为"停止效果组再启用"触发——效果组停止时 D3D12 设备引用未干净释放
   （ReShade：`Reference count for ID3D12Device2 ... is inconsistent (35)`），
   同进程重启效果组即红字报错、NR 失效，重启 Magpie 进程才恢复；x2 可复现。
   3x 的真实影响是每内容帧评估三次（`four jobs already in flight` 类保护会被
   打到），那是预算问题，不是本错误的成因。
4. **停机状态能否自愈**：目前只能重启进程。若 OpenNR 有未暴露的恢复开关，值得找。

---

## 5. 判断与建议

- **瓶颈已经不在网络**。3.27 ms 是真的，`nrbench` 也复现了 3.40 ms。继续在网络参数上抠
  没有意义，下一步只动 Motion。
- **4K 下的固定开销是结构性的**。NR 挂在 present 上，看到的永远是 4K 输出帧；只要
  ReShade 在 Magpie 进程里，这一层就省不掉。除非把 NR 挪到补帧之前——那需要改宿主，
  已明确不走注入游戏的路（安全考量优先）。
- **不要为了省几毫秒去改 Magpie 的效果链**。`XeSS_SR` 虽然 1:1 看起来像空转，但它是
  SR 台阶与纹理尺寸的来源，换掉它收益不确定、风险明确（已试过一次并回滚）。
- **补帧与 NR 在 4K 下会互相放大**：倍率乘 NR 成本，这一点在选 2x / 3x 时要算进去。
  纯净对照（无补帧）下我们 64、对方 71，差距 1.55 ms，属于可以继续收窄的量级。

---

## 2026-10-31 更新

- 平衡点 64 → **84 fps**：关 `Motion`、关 `History`、关掉 `XeSS_SR` 上无人消费的 AMDOF。
- 预算模型闭合（游戏 6.6 / NR 5.3 / AMDOF 1.4 ms），见 [2026-10-31-session.md](2026-10-31-session.md)。
- 新增撤回项：`Inline=0` + `D3D12Wait=0` 零收益（0 skipped、fence 不变）；
  “跳过生成帧在视觉上更正确”不成立（FG 在 NR 之前，跳过的帧会原样输出导致闪烁）。
- 未解问题收敛为一个可测的量：**开 FG 后 NR 的评估次数 ÷ 内容帧数**（历史值 2.00）。
  若仍为 2，唯一不牺牲安全的解法是把 NR 挪进 Magpie 链内的 `DLSSNR` 槽位
  （需要 `nvngx_dlssnr.dll` 替身，先用探针桩确认调用序列）。
---

## 2026-10-31 补充：两种闪烁的区分（重要诊断判据）

| 症状 | 机制 | 触发条件 | 解法 |
|---|---|---|---|
| **全屏闪烁** | 整帧在"已降噪 / 未降噪"之间交替 | 跳帧（网络未在本帧完成），`BridgePace=0` 或去掉 `WaitForWorkQueue` 时出现 | `BridgePace=1`（恢复 present 对自己队列的等待） |
| **场景局部闪烁** | 逐帧修正的时间噪声，只在纹理/运动区域跳 | `History=0`，每帧独立求解 | `History=1`（约 3–4 帧代价）或调 `OutputSmoothLimit` |

判据：全屏闪意味着"这一帧整个没有修正"；局部闪意味着"修正有，但每帧算得不一样"。
两者可以并存（`BridgePace=0 + History=0`），此前一直混在一起，导致归因困难。

`BridgePace` 是 ini-only 开关（不回写）：`=1` 恢复节奏等待（跳帧≈0），`=0` 保留 95fps 快路径。
实测 `BridgePace=0` 时跳帧 2199/13920 ≈ 16%，75fps 以下跳帧归零、全屏闪随之消失。

---

## 2026-10-31 夜更新：里程碑与 XeSS 裁决

- 同场景最终对比（双 addon 共存）：**mochizuki/ACO 107-108 fps vs lmxxf/HIP 78-80 fps**，
  差距 27-30 fps。钩子从 `XeSS_SR` 换成纯 HLSL `AMDNR-ACO` 是关键一步。
- **XeSS "需要 Intel ARC" 的疑问裁决**：目录原文只说"受支持的渲染 GPU"（Intel 补充说明
  在 XeSSFG 条目）；且 `magpie.1.log` 有实证——`XeSS experimental D3D11/D3D12 backend
  initialized (quality 106, Zero-MV, BGRA conversion=true): 3840x2054 -> 3840x2054`。
  **9070XT 上 XeSS 后端确实跑起来了**，88→105-110 的提升=去掉这次真实推理（约 1.6-1.8 ms）。
  但它 1:1、Zero-MV、experimental，**"是否真正有效（画质收益）"仍存疑**，列入方向 3 验证。
- 钩子需求结构性结论（详见 [hook-requirements.md](hook-requirements.md)）：
  OpenNR 挂 present 层→纯 HLSL 钩子即可；dlss5-amd 拦截 SR SDK 导出函数
  （`ffxDispatch`/`xessD3D12Execute`）→必须用原生桩（`FSR4_SR`），否则空转。
  五个 SR 桩的 HLSL 逐行等价，差异只在厂商 SDK。双 addon 可同时常驻。
- 后续四个方向（session 文档 10.3）：① lmxxf 挂真 XeSS_SR 试 SR-NR/NR-SR 双管线；
  ② mochizuki 提 NR 渲染率 vs lmxxf+XeSS_SR 放大到 4K 的帧数曲线/画质对比；
  ③ 验证 XeSS_SR 是否真实有效；④ FG 适配（前三项完成后）。

---

## 2026-11-02 桥接分层（读上游源码得出，均无实机）

**已确认**

| # | 结论 | 证据 |
|---|---|---|
| B1 | v0.4 的 D3D11→D3D12 不是我们的账：`FSR4_SR` 由 Magpie 的 `FSR3Upscaler` 实现，每帧两次 4K 拷贝 + 一次 `Flush` + 共享围栏，lmxxf 跑同一格同样要付 | `FSR3Upscaler.cpp:498/506-509/575-579`，`NativeEffectBackendFactory.cpp:183` |
| B2 | v0.4 出方向不拷贝：消费端把放大器的 `color` 直接指向运行时的 `private_output` | `mochinr.cpp:747-758` |
| B3 | 运行时**优先原地读写**调用者的纹理，失败才拷贝；两条路径都有日志字符串 | `MochizukiNrRuntime.dll`：`frame read and written in place` / `copying instead` / `input check: the copy never landed` |
| B4 | lmxxf 只解决 D3D12→HIP 一段：共享缓冲 + 共享围栏导入 HIP、只认宿主队列、按网络档拷贝、`direct_input` 时零拷贝；它自量桥接 0.1–0.3 ms（网络 24 ms 下） | `hip_d3d12_bridge.h:162/211/233/292/294`，`staging-bridge.md`，`bridge-isolation.md` |

**已撤回**

| 曾经的判断 | 为什么撤回 |
|---|---|
| 「D3D11 → D3D12 → Vulkan 桥接 ≈ 1.9 ms」适用于 v0.4 | 那是 v0.3 present 层的预算残差（含 staging、OpenNR 私有队列等待与 CPU 跨设备等待）。v0.4 每帧 CPU 合计 0.17 ms（发布说明实测），几何形状已变 |
| 「换 FFX 钩子就绕过了 D3D11↔D3D12」 | 没有绕过，只是**换了执行者**：从 OpenNR 的桥，换成 Magpie 自己的共享纹理路径，成本仍每帧发生 |

**待测（9070 XT）**

1. `bridge: colour ... shareable=?` —— Magpie 从 D3D11 共享纹理开出来的 D3D12 资源能否再导出句柄；
2. `inplace=` / `copied=` —— 运行时实际走零拷贝还是拷贝；
3. `waits=` / `wait ms` —— `inflight=2` 的宿主限流是否在限帧；
4. 「禁用 mochinr、只留 `FSR4_SR`」的基线帧率 —— 宿主侧结构性下限。

完整推导与测量步骤见 [bridge.md](bridge.md)。

**2026-11-02 补：帧内串行的归属**

- 主要串行点改判：不是 D3D12↔Vulkan 围栏，而是 **Magpie `FSR3Upscaler` 的单槽交接**
  （`sharedInput/Output11/12` 各一份、整会话复用，`FSR3Upscaler.cpp:318-319`）——
  第 N+1 帧的 D3D11 拷入必须等第 N 帧整个 D3D12 岛读完，岛不可跨帧重叠；lmxxf 同样被关在里面。
  三个候选解释与判别实验见 [bridge.md](bridge.md) §8。
- 「降网络渲染率帧数不升」与 GPU 负载下降同时成立，说明瓶颈不是 GPU 吞吐而是
  固定成本/节奏（岛结构、D3D11 侧、捕获节奏或 `inflight`）——**在拿到 8.3 的三个天花板之前，
  不要再往网络参数上投入**。
- 零成本可试项：部署包 `mochinr.ini` 的 `inflight=2` 低于代码默认 3，面板滑条调到 3–4 即可排除
  在飞限流这一支。

**2026-11-02 补2：A/B 天花板实测闭环（平衡点法，9070 XT，4K 窗口）**

上面「待测」第 4 项与三个天花板已测完，模型端到端闭环：

| 测项 | 配置 | 平衡点 | 帧时间 |
|---|---|---|---|
| B1 | 仅 FrameRate_Filter（无 SR/FG/NR） | 150 fps | 6.67 ms |
| B2 | 只留 FSR4_SR（关 NR） | 123–125 fps | 8.0–8.1 ms |
| A@1.0/0.5/0.35 | NR 全开 | 47 / 90 / 105 fps | 21.5 / 11.3 / 9.5 ms |

- 统一模型：`frame ≈ 6.7 ms 捕获/呈现 + 1.4 ms FSR4 槽位 + 13.7×scale² ms 网络`，五点残差 ≤ 0.4 ms。
- **B1−B2 ≈ 1.4 ms 即 FSR4 放大器的账**；固定底以捕获/呈现（6.7 ms，占 83%）为主。
- 网络成本 1:1 进帧时间（串行求和）实锤；「降渲染率不升帧数」正式作废（47→90→105）。
- 单槽拆解收益台阶（理论上限，不涉及降质）：0.5/0.35 档台阶1（网络重叠）约 123 fps
  （即 B2 天花板），台阶2（再砍槽位约 1 ms 冗余）约 150 fps（即 B1）；1.0 档受网络 13.7 ms
  主导，重叠后约 73 fps。
- 完整数据与归档见实机 `D:/Magpie-NR-045/_test-archive/AB-20261102/RESULTS.md`。
- **运行时自报 ms 判为误报**（不复测）：scale 1.0 自报网络 20.7–23 ms 与反推 13.7 ms 矛盾，
  而 1.0 档 46–47 fps 长时间稳定运行即反证——若网络真占 20.7 ms+，帧时间下限约 29 ms
  （≤35 fps 封顶）。自报口径是壁钟含等待（队列/Vulkan 围栏往返），非 GPU 活跃时间；
  网络成本以反推 13.7 ms 为准，引用自报值时不得当作网络耗时。

## 2026-11-03：单槽拆解的第三个串行点

读上游源码得出（`SAOG0721/Magpie` `experimental` `6940f84`，**本机未跑任何实验**）。
三类陈述仍然严格分开：**【源码】**＝读出来的；**【实测】**＝已有归档数据；**【待测】**＝未验证。
完整推导见 [multislot.md](multislot.md) §1–§2。

**已确认（源码）**

| # | 结论 | 证据 |
|---|---|---|
| S1 | 串行点有**三个**，不是一个也不是两个：①`FSR3Upscaler.cpp:497` 的 CPU 阻塞（等上一帧的岛）；②`:577` 的 D3D11 GPU 侧等待（把岛焊在 D3D11 队列里）；③**主机发布路径 `Renderer.cpp:4614-4624` 的 `Signal + Flush + 等围栏`**（每发布一帧把整条 D3D11 队列排空一次） | 三处源码；`fenceAvgMs` 就是 ③ 的等待均值（`Renderer.cpp:4628-4638`） |
| S2 | **阶段 1（只加多槽、输出语义不变）的收益是 0**——不是 0.1–0.5 ms | ③ 在稳态下已经把 D3D11 队列推过了岛，于是 ① 的 `WaitForFence` 走 `GetCompletedValue` 快路径直接放行（`:62`），①本来就空转；多槽只能让空转更空转 |
| S3 | 阶段 1 的真实价值是**解除 allocator/纹理的复用依赖**，即阶段 2 的前置条件 | 单份 `allocator12`/`commandList12` 与单份共享纹理（`:318-319`）是 ① 与 `:512-513` 的 `Reset` 必须串行的唯一理由 |
| S4 | ③ **不在补丁范围**（`Renderer.cpp` 属另一条主线）；一旦顺手改掉，阶段 1/阶段 2 的收益就再也分不清归属 | [multislot.md](multislot.md) §7.3 的共享约定 |
| S5 | `Drain()` **只等不补拷**：调用点 `_DrainNgxConsumers` 是停流时（`Renderer.cpp:3109-3121`），而发布路径已对最后一帧做过 `Signal+Flush+等围栏`（`:4614-4624`），那一帧早交给系统了；再往 `ctx.output` 写一次 U(N) 视觉零收益，缓存 `ctx.output` 指针则在 `Resize()` 后悬空 | 契约见 multislot.md §3.8 与 §4 F12；实现见 `src/magpie-patches/README.md`「`Drain()` 的语义」 |

**待测**

| # | 待测项 | 怎么测 | 影响 |
|---|---|---|---|
| T1 | **G0：GPU 忙时间占帧时间的比例**（`ratio`） | `s=0.5/0.35` 各跑 60 s，按引擎采 `\GPU Engine(*)\Utilization Percentage`，`ratio = busy_per_frame / frame_time` | **整条主线是否成立**：`>0.85` → 求和是吞吐的必然，"取大"天花板不存在，阶段 2 取消 |
| T2 | 台阶（阶段 2）：0.5/0.35 档 → 123–125 fps；1.0 档 → 66–70 fps | 同上跑批，三档 × 三配置（slots=1 / slots=3 lag0 / slots=3 lag1） | [multislot.md](multislot.md) §6.2 B2–B4 的验收线 |
| T3 | `harvestMiss` 比例（收割时岛还没完成） | `magpie.log` 的 `fsr3-slot:` 行；模型预测 s=1.0 时接近 100%（岛 ≈ 帧间隔），s=0.5/0.35 时 ≈0 | §6.2 B1：>20% 说明岛比帧间隔长，退阶段 1 |
| T4 | `slotWaitAvgMs` 是否真的 →0 | 同上 | 阶段 1 的核心读数；若不为 0 说明 ③ 的排空没有覆盖到 ① |

**阶段 1 的验收判据是可证伪的**：`slots=3/lag=0` 与 `slots=1` 的平衡点差 ≤ ±1 fps、
`fenceAvgMs` 差 ≤ 0.3 ms，**掉 ≥3 fps 才算失败**（[multislot.md](multislot.md) §6.1 A2）。
若实测显著变好，那是意外发现，先记录不要庆祝——更可能的解释是 T1 的 `busy` 与预期不符。

补丁（`src/magpie-patches/` 0001/0002）与纸面模型（`tools/slot_model.py`）已就绪，
**但一行都没有在 9070 XT 上编译运行过**（本机无 C++ 编译器）。
