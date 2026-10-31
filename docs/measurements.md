# 实测数据

记录时间：2026-10-30 ~ 10-31
平台：Windows 10.0.26300 / AMD Radeon RX 9070 XT / 驱动 26.10.1 (LLPC) / 显示 160 Hz
游戏：World of Warcraft 正式服，客户区 3840×2054，Magpie 输出 3840×2160
宿主：Magpie 0.6.11（`Magpie-DLSS5-AMD-0.45`），ReShade 6.8.0.2155 以 `dxgi.dll` 注入

所有数字都来自日志或现场读数，采集方式写在每节末尾。看不懂的单位不用猜：`fenceAvgMs`
是 Magpie 在 `_PublishBackendTexture` 里等 GPU 的平均时间，只统计**内容帧**（120 个一批）。

---

## 1. 关键对照：同为 1080 档 NR、不带补帧

两边都把 NR 网络缩到 1920×1080 档，效果组里去掉补帧，Magpie 不限帧，由游戏自己限帧。

| | lmxxf 实例 | 本项目（mochizuki） |
|---|---|---|
| 原生帧 | **70–72** | **64** |
| 每帧预算 | 14.08 ms | 15.63 ms |
| 网络耗时（上游离线基准） | 5.487 ms | **3.27 ms** |
| NR 渲染档位 | 1920×1088（`DLSS5_NETWORK_HEIGHT=1080` + `FIT_LARGE=1`） | 1920×1080（`Scale=0.5`） |

**网络我们快 2.22 ms，整体却慢 1.55 ms** —— 差额 3.77 ms 全在网络之外。拆解见
[findings.md](findings.md#2-为什么基准里的领先没有出现)。

### 关掉运动矢量之后（同一天补测）

把 OpenNR 面板里的 **Motion 关掉**（此时不再算 4K FidelityFX 光流）：

| | 原生帧 | fenceAvgMs |
|---|---|---|
| Motion 开（FidelityFX 光流） | 64 | 约 9–10 |
| Motion 关 | **68–75**（多次顶到游戏内的 75 限帧） | **4.6–5.5** |

也就是**光流本身约 2.3 ms/帧**，关掉后 NR 的整体开销已经藏进游戏的限帧余量里，
与输入帧率基本持平。`MotionScale` 从 1.0 拖到 0.5 / 0.2 只带来很小改善，说明成本与
矢量缩放无关，主要来自 FFX 光流本身在 4K 上的固定开销。

采集：`amd-nr.log` 的 `menu: useMotion 1 -> 0` 与 `logs\magpie.log` 同步对照。

采集：`logs\magpie.log` 的 `Backend publication CPU timing` 相邻两行时间差 / 120。

---

## 2. 补帧开启时的历史数据（0.6.11，XeSS_SR 1:1，NR 0.6 档）

| 状态 | 原生帧 | fenceAvgMs |
|---|---|---|
| NR 运行（FG 2x） | 33–34 | 12.5 |
| NR 停用（FG 3x，仅作链路上限参考） | 50 | 4.2 |

同一会话内 OpenNR 自己数的处理帧数：

```
swapchain going away (resize 0) after 3311 frames
```

会话 23:33:16.5 → 23:34:05.6 共 49.1 s，同期内容帧 33.6 fps ≈ 1650 帧，
**3311 / 1650 = 2.00**。也就是说在 FG 2x 下，NR 对每个**呈现**帧都跑了一次，
补帧倍率直接变成 NR 成本的倍率。这条只对 mochizuki 有直接计数证据，lmxxf 无逐帧日志。

---

## 3. 网络本身的成本（本项目 `nrbench`，ACO + CC_SMALL_FP8）

| 光栅 | 网络尺寸 | 网络耗时 | 整个 NR 阶段 |
|---|---|---|---|
| 1920×1080 | 1920×1080 | 3.40 ms | 3.78 ms |
| 3840×2160 | 3840×2160 | 13.80 ms | 14.65 ms |

与上游公布的 3.27 ms（1080 档、ACO、CC_SMALL_FP8）吻合，说明**运行时本身没有退化**。
网络耗时随像素线性增长，所以把 `Scale` 从 0.6（2304×1296）降到 0.5（1920×1080）
只省下约 1.9 ms/次。

采集：`nrbench.exe <目录> <宽> <高> <帧数> aco [model_scale]`。

---

## 4. ACO 与路径

同一套 0.5.9 资产，只换目录：

| 路径 | 日志 |
|---|---|
| `D:\迅雷下载\Magpie-NR-20261030` | `ACO asked for, but ...\aco has no records\ and shaders\` |
| `D:\Magpie-NR` | `ACO: the network runs ACO's machine code` |

结论：mochizuki 的 ACO 判定不兼容非 ASCII 路径，**部署目录必须是纯 ASCII**。
0.5.9 与 0.6.11 宿主无关，这是运行时自身的行为。

---

## 5. 固定开销的来源（日志原文）

```
optical flow: FidelityFX, 3840x2160 frame, 480x270 blocks of 8x8, into the 1920x1080 raster.
```

光流按**帧尺寸 3840×2160** 计算，只有网络输入缩到 1920×1080。所以 `Scale` 只影响网络，
不影响光流；入出双向拷贝同样是 4K。

```
hold: each present waits here for the last evaluation, as the same-frame engine needs on
D3D12 and on OpenGL's fences; the stats line has its cost, and D3D12Wait=0 turns it off.
```

`Inline=1`（same frame）时 present 会等评估完成；`Inline=0` 是异步。两者在 FG 2x 的实测里
原生帧都停在 33–34，说明串行等待不是当时的主因，但它仍在 `D3D12Wait=1` 时参与争用。

---

## 6. 输入桥的验收（2026-10-30 更早）

- 旧版：Home 能打开面板，自动点击后仍停在「主页」。
- 新版：同窗口、同坐标点击「设置」成功切页，画面内容随之变化。
- Home 关闭、Esc 关闭：两者都能让画面回到无覆盖层状态（日志出现 `overlay actual OFF`）。

采集：`test-new\before.bmp` / `after.bmp`、`overlay-input.log`、`home-close.bmp`、`esc-close.bmp`。

---

## 7. 复现这些数字

```powershell
# 网络本身
dist\nrbench.exe "D:\Magpie-NR-045" 1920 1080 40 aco 0.6

# 运行时冒烟
dist\nrtest.exe "D:\Magpie-NR-045" aco

# 实机帧数：看 logs\magpie.log 的
#   Backend publication CPU timing: samples=120 ... fenceAvgMs=...
# 相邻两行的墙钟差 / 120 = 原生帧率
```

---

## 2026-10-31 补充：预算模型闭合

三个观测（同机、同场景、4K 输出、NR 网络 1920×1080）联立，误差 <1 帧：

| 状态 | 平衡点 | 每帧 |
|---|---|---|
| NR 开 + AMDOF 开 | 75 fps | 13.33 ms |
| NR 开 + AMDOF 关 | 84 fps | 11.90 ms |
| NR 关 + AMDOF 开 | 125 fps | 8.00 ms |

```
游戏自身 ≈ 6.6 ms   NR ≈ 5.3 ms（网络 3.3 + 桥接/拷贝 2.0）   AMDOF ≈ 1.4 ms
```

> 之前记录的光流“约 2.3 ms/帧”是估算，实测为 **1.4 ms**；`Motion` 关掉省的是 NR 内部
> 那套 FFX 光流（2.3 ms），与 `XeSS_SR` 上那个 AMDOF（1.4 ms）是两笔不同的账。
> 完整过程见 [2026-10-31-session.md](2026-10-31-session.md)。

### 同条件对照：lmxxf 82 vs mochizuki 84

| 运行时 | 后端 | 平衡点 | 网络 | 桥接 | NR 总成本 |
|---|---|---|---|---|---|
| lmxxf | HIP | 82 fps | 5.2–5.3 ms | ≈0.3–0.4 ms | 5.6 ms |
| mochizuki | ACO/Vulkan | **84 fps** | 3.4 ms | ≈1.9 ms | 5.3 ms |

网络领先 1.8–1.9 ms，实机只领先 0.3 ms（+2 fps）。差额来自 D3D11→D3D12→Vulkan 的
两次跨 API 跳与 4K staging 往返（`bridge: private staging 3840x2160 ...`）。

### 2026-10-31 夜：双 addon 共存后的最终对比

| 效果组 | 钩子 | 运行时 | 平衡点 |
|---|---|---|---|
| NR-mochizuki | AMDNR-ACO（纯 HLSL） | mochizuki / ACO | **107-108 fps** |
| NR-lmxxf | FSR4_SR（原生桩） | lmxxf / HIP | 78-80 fps |

XeSS 后端实证（`magpie.1.log:578`）：`XeSS experimental D3D11/D3D12 backend initialized
(quality 106, Zero-MV, BGRA conversion=true): 3840x2054 -> 3840x2054`——9070XT 上跑起来了，
但 1:1/Zero-MV/experimental，画质收益存疑。换 `AMDNR-ACO` 后该初始化计数为 0。
