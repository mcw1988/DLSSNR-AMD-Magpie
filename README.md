# magpie-nr

在 **Magpie** 的捕获链路上跑 **ReShade 6.8**，由 ReShade add-on 驱动两条 AMD 神经渲染（NR）运行时：
**lmxxf（HIP）** 与 **mochizuki（Vulkan）**，按 Magpie 当前选中的效果组互斥切换，并让 **Home 键的
ReShade 覆盖层在 Magpie 里真正可用**。

> Magpie captures a window and hands the frame to ReShade 6.8 injected as `dxgi.dll`. A ReShade
> add-on drives either the lmxxf (HIP) or the mochizuki (Vulkan) neural-rendering runtime, picked by
> the Magpie effect group; `overlay-input` routes keyboard and mouse into Magpie's swap-chain window
> so the overlay (Home) and its sliders actually work.

本项目只包含**自己写的组件**与文档；ReShade、Magpie、两条运行时的权重与二进制由上游提供，
获取方式见 [docs/upstream.md](docs/upstream.md)。

---

预打包的实机目录见 **[Releases v0.1](https://github.com/mcw1988/magpie-nr/releases/tag/v0.1)**（两个分卷，
解到同一目录即可运行）。

## 状态（v0.1，2026-10-31）

宿主已迁到 **Magpie 0.6.11**（`Magpie-DLSS5-AMD-0.45`），以下都在实机验证过：

- ✅ 两条运行时按效果组互斥切换（`nr-router`，适配 0.6.11 的 `config\v4e\config.json`）
- ✅ Home 打开 ReShade 面板，鼠标可正常点选、拖动滑块、切标签页；Home / Esc 均可关闭
- ✅ mochizuki 在 Magpie + ReShade 下实机跑起来，ACO 生效，网络耗时与上游基准一致
- ✅ WoW 正式服 4K 实测出数（见下）

| 场景（4K，NR 1080 档，无补帧，游戏自限帧） | 原生帧 |
|---|---|
| lmxxf 运行时（对照实例） | 70–72 |
| mochizuki 运行时（本项目） | 64 |

差距 1.55 ms/帧，来源已定位到**网络之外**的固定开销（4K 光流 + 拷贝 + 桥接），
不是运行时本身慢：网络我们比对方快 2.22 ms。详见 [docs/findings.md](docs/findings.md)。

> 补帧同时开启时，NR 的成本会按倍率翻倍（2x 下每个内容帧跑 2 次，有逐帧计数为证），
> 这是选 2x / 3x 时必须算进去的一项。

## 解决的问题

| 现象 | 原因 | 本仓库的处理 |
|---|---|---|
| 在 Magpie 里按 Home 打不开 ReShade 面板 | Magpie 的输出窗口是 `WS_EX_NOACTIVATE` + 渲染子窗口 `WS_EX_TRANSPARENT`，KeyEvent 根本到不了 ReShade；ReShade 的输入完全由窗口消息驱动，自己没装全局钩子 | `overlay-input` 用底层钩子把键鼠**直接贴进 swapchain 窗口** |
| Home 能开，但菜单里点不动 | ReShade 打开覆盖层后会屏蔽 `GetAsyncKeyState`、丢弃发给窗口的 `WM_MOUSEMOVE`；且投递消息里 `MSG.pt` 等于 `lParam`，会被二次 `ScreenToClient` 偏移 | 以底层鼠标事件为准 + 未受屏蔽的状态轮询兜底 + 统一按屏幕坐标投递 + 按帧拆分按下/抬起 |
| 两条 NR 运行时不能同时挂 | 两个 add-on 都在改帧，同时加载会互相抢帧 | `nr-router` 按效果组把不用的那个改名成 `*.off`，物理上只加载一个 |
| 换了效果组，跑的却还是上一个运行时 | ReShade 只在**进程启动时**扫描 `*.addon64` | 同上：切换 = 换文件 + 重启 Magpie；`nr-router --restart` 一条命令做完 |
| 换完组 NR 就"消失"，画面没降噪 | OpenNR 收到设备销毁后置为 off 且不自动恢复，而 Magpie 每次停缩放都会销毁设备 | `nr-router --restart` 现在**无条件重启**（不再只在文件变化时重启） |
| ACO 明明有文件却不生效 | mochizuki 的 ACO 判定不兼容非 ASCII 路径 | 部署目录必须是纯 ASCII（如 `D:\Magpie-NR-045`） |
| 不确定运行时在本机能不能跑 | — | `nrtest.exe` 跑一帧；`nrbench.exe` 量每帧成本 |

## 组件

| 组件 | 产物 | 说明 |
|---|---|---|
| `overlay-input` | `overlay-input.addon64` | ReShade add-on。`WH_KEYBOARD_LL` / `WH_MOUSE_LL` + 15 ms 轮询，把按键、鼠标位置、按键状态、滚轮转发进 `swapchain->get_hwnd()`；热键（默认 Home）切换；覆盖期间吞掉原始输入 |
| `nr-router` | `nr-router.exe` | 读 `config\v4e\config.json`（0.6.11）或 `config\config.json`（旧版）的 `profiles[0].scalingMode`，按组名/效果名判定后端，只留一个 add-on；`--watch` 常驻、`--restart` 自动重启 Magpie |
| `nrtest` | `nrtest.exe` | 独立 D3D12 宿主，直接按 `LmxxfNrApi`（ABI v1）跑一帧并打印结果 |
| `nrbench` | `nrbench.exe` | 同上，但跑 N 帧并分别统计 prepare / 生产拷贝 / 网络 / 消费拷贝的墙钟耗时，可指定光栅尺寸与网络缩放 |

## 快速开始

构建设备需要 **VS 2022 Build Tools（MSVC x64）+ Windows SDK**，见 [docs/build.md](docs/build.md)：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-all.ps1
```

组装运行环境（Magpie + ReShade + 上游运行时与权重）见 [docs/upstream.md](docs/upstream.md) 和
[docs/usage.md](docs/usage.md)。简要：

```
<Magpie 目录>\
  Magpie.exe  dxgi.dll(=ReShade 6.8, 带 add-on 支持)  ...
  amd-nr.addon64            或  amd-nr.addon64.off      ← mochizuki 宿主（OpenNR 0.7.15）
  dlss5-amd.addon64        或  dlss5-amd.addon64.off    ← lmxxf 运行时
  MochizukiNrRuntime.dll  dlssnr-amd\                   ← mochizuki 运行时 0.5.8 与权重
  DLSS5-AMD\native-game-tiled-assets\                   ← lmxxf 权重
  overlay-input.addon64  overlay-input.ini
  nr-router.exe  nr-router.cmd
  config\v4e\config.json  amd-nr.ini
```

两个效果组（Magpie 里自建，0.6.11 会把 `XeSS_FrameGeneration_x2_ZeroMV` 规范化成
`XeSS_FrameGeneration` + `multiplier`，等价）：

```
NR-lmxxf      = FSR4\FSR4_SR   (+ FrameRate_Filter, XeSS FG)
NR-mochizuki  = XeSS\XeSS_SR   (+ FrameRate_Filter, XeSS FG)
```

**换组之后必须重启 Magpie**（跑 `nr-router.cmd` 或托盘退出再开），否则 NR 会停在 off 状态。

## 目录

```
src/nr-router/      效果组 → 运行时的互斥路由（C++ / Win32）
src/overlay-input/  ReShade add-on：把键鼠喂进 Magpie 的渲染窗口
src/nrtest/         运行时冒烟测试与帧成本基准（独立 D3D12 宿主）
scripts/            一键构建、上游获取
docs/               架构、构建、使用、排错、上游清单、实测数据、结论
dist/               预编译产物（由本仓库源码构建，见 dist/README.md）
```

## 文档

- [docs/architecture.md](docs/architecture.md) — 全链路、为什么 Home 会失效、present 阶段的代价模型
- [docs/usage.md](docs/usage.md) — 目录布局、效果组、切换运行时、日志
- [docs/measurements.md](docs/measurements.md) — 所有实测数字与采集方式
- [docs/findings.md](docs/findings.md) — 已确认的结论、**已撤回的结论**、未解问题
- [docs/troubleshooting.md](docs/troubleshooting.md) — 常见故障与处理
- [docs/build.md](docs/build.md) — 构建
- [docs/upstream.md](docs/upstream.md) — 上游版本与获取方式

## 许可

本仓库代码使用 **MIT**（见 [LICENSE](LICENSE)）。所依赖的上游项目各自保持原有许可
（ReShade BSD-3-Clause、Magpie MIT、OpenNR MIT、DLSSNR-AMD/Mochizuki MIT、lmxxf MIT、
MinHook MIT），详见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

**本仓库不包含** ReShade、Magpie、Intel XeSS、AMD FidelityFX 的二进制，也不包含模型权重；
`dist/` 里只有本仓库源码构建的四件产物与上游 OpenNR add-on 的引用。
