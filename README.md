# DLSSNR-AMD-Magpie

> **项目愿景**：让 AMD RX 9000 / 7000 系显卡在 **Magpie** 里稳定运行**带帧生成的 mochizuki-ACO 权重
> NR 神经渲染**——网络耗时对齐上游权重自身测得的离线单帧 ms 数，在此之上优化实机帧率，并配一套
> 有效、好调、开箱可用的 NR 风格面板。

在 **Magpie Experimental 0.6.11** 的捕获链路上注入 **ReShade 6.8**，由 ReShade add-on 在
**FFX 放大调用内部、放大之前**跑神经网络；三个后端（`mochinr` / lmxxf / OpenNR-present）由
效果组互斥切换，Home 键的 ReShade 面板在 Magpie 里真正可用。

> Magpie captures a window; ReShade 6.8 (injected as `dxgi.dll`) drives a neural-rendering add-on
> inside the FFX upscale call. Three backends are mutually exclusive and picked by the Magpie effect
> group: `mochinr` (mochizuki ACO weights, Vulkan), lmxxf (HIP), and the retired OpenNR present-layer
> route. `overlay-input` routes keyboard and mouse into Magpie's non-activating swap-chain window so
> the overlay (Home) and its sliders actually work.

本项目只包含**自己写的组件**与文档；Magpie、ReShade、两条运行时的权重与二进制由上游提供，
获取方式与致谢见 [docs/upstream.md](docs/upstream.md)。

---

预打包的实机目录见 **[Releases v0.4](https://github.com/mcw1988/DLSSNR-AMD-Magpie/releases/tag/v0.4)**
的 `Magpie-NR-045-v0.4-core.zip`——**一份包，解压即用**，自带 mochizuki 权重。
lmxxf 权重体积太大不在包内，需要时另取
**[v0.1 的权重分卷](https://github.com/mcw1988/DLSSNR-AMD-Magpie/releases/tag/v0.1)**
（v0.2 / v0.3 的 Release 都只传了核心包，没有权重分卷）。

## 状态（v0.4，2026-11-01 夜）

**mochizuki 换了挂载位置**：不再跑在 present 层，而是接进 **FFX 放大调用内部、放大之前**，
和 lmxxf 站在同一条挂载点上。实现与踩坑记录见 **[docs/mochinr.md](docs/mochinr.md)**。

**旧路线已退役**：present 层的 `amd-nr.addon64` 不再随包发布，依赖它的两个效果组（`NR-mochizuki`、
`NR-mochizuki-FG`）也从配置里删掉了。mochizuki 现在只有一条挂载路线。包内效果组剩三个：
`NR-preupscale`（默认）、`NR-lmxxf`、`仅补帧`。

```
v0.3:  捕获 → 放大(FSR) → 帧率过滤 → 补帧 → present ← (NR 在这里，每帧 N 次)
v0.4:  捕获 → [NR] → 放大(FSR) → 帧率过滤 → 补帧      ← (NR 在放大之前)
```

实机（效果组 **NR-preupscale**：`FSR4\FSR4_SR` + `mochinr.addon64`，WoW 正式服，9070 XT，
输入 1920×1080）：

| 项 | 数值 |
|---|---|
| 输出帧率 | **110–130 fps** |
| 每帧 CPU | 0.17 ms（Vulkan 围栏 0.09 + 提交 0.05 + 录制 0.02） |
| 稳定性 | **尚不稳定**，高负载或分辨率变化时仍有起伏 |

推论（**未实测**）：NR 位于放大之前，补帧只看到降噪后的帧，理论上已解决「mochi 运行时移植后
无法开 FG」的问题；AMOF 光流是否仍需要、闪烁是否消失，均**未验证**。

已知问题：降低网络渲染率只降低 GPU 负载与功率，**帧数不升**（游戏限帧已排除，嫌疑是
D3D12 ↔ Vulkan 围栏造成的帧内串行）；仅 1080p 实测；面板版式待打磨。

后续方向：优化 UI、修正性能（消除串行而非降低质量）、适配 1080p 以上输入、调优 NR 风味、适配 MFG。

配套文件：`src/mochinr/`（源码 + 构建脚本 + 运行时扩展 ABI 头）、`dist/mochinr.addon64`、
`scripts/pack-release.py`（打发布包，排除项与 v0.3 的政策一致）。
换后端不再需要手改文件名：`nr-router.exe` 按组名在 add-on 之间只留一个启用，
规则见 [docs/mochinr.md](docs/mochinr.md) 第 6 节。

## 状态（v0.3，2026-10-31 夜）

当日调优全过程见 **[docs/2026-10-31-session.md](docs/2026-10-31-session.md)**。
最终里程碑（同场景、NR 1080 档、双 addon 共存）：

| 效果组 | 钩子 | 平衡点 |
|---|---|---|
| NR-mochizuki | `AMDNR-ACO`（纯 HLSL） | **107-108 fps** |
| NR-lmxxf | `FSR4_SR`（原生桩） | 78-80 fps |

调优路径：64 → 84（预算模型闭合）→ 88（桥接 GPU 围栏 + BridgePace）→ 107-108
（钩子换纯 HLSL，去掉每帧一次的真实 XeSS 推理）。钩子需求研究见
**[docs/hook-requirements.md](docs/hook-requirements.md)**。

宿主已迁到 **Magpie 0.6.11**（`Magpie-DLSS5-AMD-0.45`），以下都在实机验证过：

- ✅ 两条运行时按效果组互斥切换（`nr-router`，适配 0.6.11 的 `config\v4e\config.json`）
- ✅ Home 打开 ReShade 面板，鼠标可正常点选、拖动滑块、切标签页；Home / Esc 均可关闭
- ✅ mochizuki 在 Magpie + ReShade 下实机跑起来，ACO 生效，网络耗时与上游基准一致
- ✅ WoW 正式服 4K 实测出数（见下）

| 场景（4K，NR 1080 档，无补帧，游戏自限帧） | 原生帧 |
|---|---|
| lmxxf 运行时（对照实例） | 70–72 |
| mochizuki，Motion 开（FidelityFX 光流） | 64 |
| mochizuki，Motion 关 | 68–75 |
| mochizuki，Motion 关 + History 关 | 75 |
| mochizuki，**Motion 关 + History 关 + SR 上的 AMDOF 关** | **84** |

差距来自**网络之外**的固定开销，不是运行时本身慢：网络我们比对方快 2.22 ms，
但当时多背了一笔 4K 光流（`Motion` 使用 FFX 时约 2.3 ms/帧）与约 2.0 ms 的
4K 拷贝 + D3D12↔Vulkan 桥接。把光流逐步挪出关键路径后（关 Motion → 关 `XeSS_SR`
上无人消费的 AMDOF），本机已从 64 抬到 **84**，反超对照实例。

实测分量（三个观测联立，误差 <1 帧）：**游戏 6.6 / NR 5.3 / AMDOF 1.4 ms**。

同条件（`XeSS_SR` 上 AMDOF 关、FG 关）下 lmxxf 也测了 **82 fps**，两边只差 2 帧，
而网络本身差 1.8–1.9 ms —— 差额被 mochizuki 的桥接吃掉了：

```
lmxxf      5.2–5.3 (网络, HIP)   + ≈0.3–0.4 (进程内互操作)      = 5.6 -> 82 fps
mochizuki  3.4     (网络, ACO)   + ≈1.9 (D3D11→D3D12→Vulkan)    = 5.3 -> 84 fps
```

所以**“网络快 2.22 ms”不能当成实机领先**，端到端只有 0.3 ms。桥接那 1.9 ms 是目前
唯一还没动过的大块，详见 [docs/2026-10-31-session.md](docs/2026-10-31-session.md) 第 9 节。

> 补帧同时开启时，NR 的成本会按倍率翻倍（2x 下每个内容帧跑 2 次，有逐帧计数为证），
> 这是选 2x / 3x 时必须算进去的一项。v0.4 把 NR 挪到放大之前后，这一条不再成立（未实测）。

## 解决的问题

| 现象 | 原因 | 本仓库的处理 |
|---|---|---|
| 在 Magpie 里按 Home 打不开 ReShade 面板 | Magpie 的输出窗口是 `WS_EX_NOACTIVATE` + 渲染子窗口 `WS_EX_TRANSPARENT`，KeyEvent 根本到不了 ReShade；ReShade 的输入完全由窗口消息驱动，自己没装全局钩子 | `overlay-input` 用底层钩子把键鼠**直接贴进 swapchain 窗口** |
| Home 能开，但菜单里点不动 | ReShade 打开覆盖层后会屏蔽 `GetAsyncKeyState`、丢弃发给窗口的 `WM_MOUSEMOVE`；且投递消息里 `MSG.pt` 等于 `lParam`，会被二次 `ScreenToClient` 偏移 | 以底层鼠标事件为准 + 未受屏蔽的状态轮询兜底 + 统一按屏幕坐标投递 + 按帧拆分按下/抬起 |
| 三个 NR 后端不能同时挂 | 其中两个钩的是同一个 `ffxDispatch` 导出，同时加载会互相抢帧 | `nr-router` 按组名/效果名只留一个 `*.addon64`，其余改名 `.off`；缺文件时拒绝整批操作，见 [docs/mochinr.md](docs/mochinr.md) §6 |
| 换了效果组，跑的却还是上一个后端 | ReShade 只在**进程启动时**扫描 `*.addon64` | `nr-router --restart` 换文件并重启 Magpie（`--watch` 跟随效果组自动做） |
| 换完组 NR 就"消失"，画面没降噪 | OpenNR 收到设备销毁后置为 off 且不自动恢复，而 Magpie 每次停缩放都会销毁设备 | `nr-router --restart` **无条件重启**（不再只在文件变化时重启） |
| ACO 明明有文件却不生效 | mochizuki 的 ACO 判定不兼容非 ASCII 路径 | 部署目录必须是纯 ASCII（如 `D:\Magpie-NR-045`） |
| 不确定运行时在本机能不能跑 | — | `nrtest.exe` 跑一帧；`nrbench.exe` 量每帧成本 |

## 组件

| 组件 | 产物 | 说明 |
|---|---|---|
| `mochinr` | `mochinr.addon64` | v0.4 主力。拦截 `ffxDispatch`（放大之前），把批次在 `ExecuteCommandLists` 处劈成两段，网络跑完再重放；自带中文四页签面板（网络 / 风味 / 计时 / 调试） |
| `overlay-input` | `overlay-input.addon64` | ReShade add-on。`WH_KEYBOARD_LL` / `WH_MOUSE_LL` + 15 ms 轮询，把按键、鼠标位置、按键状态、滚轮转发进 `swapchain->get_hwnd()`；热键（默认 Home）切换；覆盖期间吞掉原始输入 |
| `nr-router` | `nr-router.exe` | 读 `config\v4e\config.json`（0.6.11）或 `config\config.json`（旧版）的 `profiles[0].scalingMode`，按组名/效果名判定后端，只留一个 add-on；`--watch` 常驻、`--restart` 自动重启 Magpie |
| `nrtest` | `nrtest.exe` | 独立 D3D12 宿主，直接按 `LmxxfNrApi`（ABI v1）跑一帧并打印结果 |
| `nrbench` | `nrbench.exe` | 同上，但跑 N 帧并分别统计 prepare / 生产拷贝 / 网络 / 消费拷贝的墙钟耗时 |

上游 OpenNR add-on 仍以 `dist/amd-nr.addon64` 形式保留（配合 `src/opennr-patches/` 的补丁），
但 v0.4 起不再随发布包分发。

## 上游与致谢

本项目站在这些上游之上，**郑重感谢**：

| 上游 | 提供了什么 | 许可 |
|---|---|---|
| [**SAOG0721/Magpie**](https://github.com/SAOG0721/Magpie)（Experimental 分支，v0.6.11） | 宿主本体：捕获、效果链、D3D11 渲染、缩放窗口 | **GPL-3.0** |
| [**lmxxf/dlss5-on-amd-9070xt-porting**](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) | `mochinr` 的**钩子写法**（`ffxDispatch` 拦截 / 提交期劈批 / 重放）与 **lmxxf 权重** | MIT |
| [**mochizuki0323/DLSSNR-AMD**](https://github.com/mochizuki0323/DLSSNR-AMD) | **mochizuki（ACO / Vulkan）权重与运行时**（仓库当前返回 404，实际镜像见 HF 数据集） | MIT |
| [**zmodelerlover/OpenNR-ReShade**](https://github.com/zmodelerlover/OpenNR-ReShade) | OpenNR 宿主本体；**ReShade add-on 工程结构**与 **RenoDX 风格参数面板**的写法（其风格源自 [RenoDX](https://github.com/clshortfuse/renodx)） | MIT |
| [crosire/reshade](https://github.com/crosire/reshade) | 注入与 add-on API（6.8.0.2155，BSD-3-Clause） | BSD-3-Clause |

各上游的完整声明见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)，获取方式见
[docs/upstream.md](docs/upstream.md)。**发布包里含 Magpie（GPL-3.0）的二进制**，按 GPL-3
第 6 条我们只做聚合分发并提供对应源码地址；本仓库自身代码仍是 MIT，两者不合并。

## 快速开始

构建设备需要 **VS 2022 Build Tools（MSVC x64）+ Windows SDK**，见 [docs/build.md](docs/build.md)：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-all.ps1
```

组装运行环境（Magpie 0.6.11 + ReShade + 上游运行时与权重）见 [docs/upstream.md](docs/upstream.md)
和 [docs/usage.md](docs/usage.md)。简要：

```
<Magpie 目录>\                       ← 路径必须是纯 ASCII
  Magpie.exe  dxgi.dll(=ReShade 6.8, 带 add-on 支持)  ...
  mochinr.addon64          或  mochinr.addon64.off       ← v0.4 主力（FFX 槽位）
  dlss5-amd.addon64        或  dlss5-amd.addon64.off     ← lmxxf（FFX 槽位）
  amd-nr.addon64.off                                     ← OpenNR present 层（已退役）
  MochizukiNrRuntime.dll  dlssnr-amd\                    ← mochizuki 运行时 0.5.8 与权重
  DLSS5-AMD\native-game-tiled-assets\                    ← lmxxf 权重（594 MB，不在发布包内）
  overlay-input.addon64  overlay-input.ini
  nr-router.exe  nr-router.cmd
  config\v4e\config.json
```

三个效果组（0.6.11 会把 `XeSS_FrameGeneration_x2_ZeroMV` 规范化成
`XeSS_FrameGeneration` + `multiplier`，等价）：

```
NR-preupscale  = FSR4\FSR4_SR + mochinr        （默认，v0.4 主力）
NR-lmxxf       = FSR4\FSR4_SR + lmxxf          （对照实例）
仅补帧          = 只留帧率过滤与补帧
```

**换组之后必须重启 Magpie**（跑 `nr-router.cmd` 或托盘退出再开），否则 NR 会停在 off 状态。

## 目录

```
src/mochinr/        FFX 槽位的 mochizuki 宿主（v0.4 主力）+ 运行时扩展 ABI 头
src/hooks/          AMDNR-ACO / AMDNR-HIP 纯 HLSL 钩子
src/nr-router/      效果组 → 后端的互斥路由（C++ / Win32）
src/overlay-input/  ReShade add-on：把键鼠喂进 Magpie 的渲染窗口
src/nrtest/         运行时冒烟测试（nrtest）与帧成本基准（nrbench）
src/opennr-patches/ 对 OpenNR 0.7.15 的补丁（含 D3D11↔D3D12 共享围栏）
scripts/            一键构建、上游获取、打发布包
docs/               架构、构建、使用、排错、上游清单、实测数据、结论
dist/               预编译产物（由本仓库源码构建，见 dist/README.md）
```

## 文档

- [docs/roadmap.md](docs/roadmap.md) — 愿景拆成四条可验证目标、当前瓶颈、下一步
- [docs/mochinr.md](docs/mochinr.md) — v0.4 的挂载契约、8 个踩坑、面板、路由器规则
- [docs/architecture.md](docs/architecture.md) — 全链路、为什么 Home 会失效、同步与边界
- [docs/usage.md](docs/usage.md) — 目录布局、效果组、切换后端、日志
- [docs/measurements.md](docs/measurements.md) — 所有实测数字与采集方式
- [docs/findings.md](docs/findings.md) — 已确认的结论、**已撤回的结论**、未解问题
- [docs/hook-requirements.md](docs/hook-requirements.md) — 钩子需求研究：为何用不同效果器、双 addon 共存
- [docs/troubleshooting.md](docs/troubleshooting.md) — 常见故障与处理
- [docs/build.md](docs/build.md) — 构建
- [docs/upstream.md](docs/upstream.md) — 上游版本、获取方式与致谢

## 许可

本仓库代码使用 **MIT**（见 [LICENSE](LICENSE)）。所依赖的上游各自保持原有许可：
Magpie Experimental **GPL-3.0**、ReShade BSD-3-Clause、OpenNR / DLSSNR-AMD / lmxxf / RenoDX /
MinHook MIT，详见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

**本仓库不包含** ReShade、Magpie、Intel XeSS、AMD FidelityFX 的二进制，也不包含模型权重；
`dist/` 里只有本仓库源码构建的产物与对上游 add-on 的引用。发布包（Release zip）另含 Magpie
二进制，其分发遵循 GPL-3.0，见上面「上游与致谢」最后一行的说明。
