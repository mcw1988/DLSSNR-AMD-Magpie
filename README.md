# magpie-nr

在 **Magpie** 的捕获链路上跑 **ReShade 6.8**，由 ReShade add-on 驱动两条 AMD 神经渲染（NR）运行时：
**lmxxf（HIP）** 与 **mochizuki（Vulkan）**，按 Magpie 当前选中的效果组互斥切换，并让 **Home 键的
ReShade 覆盖层在 Magpie 里真正可用**。

> Magpie captures a window and hands the frame to ReShade 6.8 injected as `dxgi.dll`. A ReShade
> add-on drives either the lmxxf (HIP) or the mochizuki (Vulkan) neural-rendering runtime, picked by
> the Magpie effect group; `overlay-input` routes keyboard and mouse into Magpie's swap-chain window
> so the overlay (Home) and its sliders actually work.

本项目只包含**自己写的三个组件**与文档；ReShade、Magpie、两条运行时的权重与二进制由上游提供，
获取方式见 [docs/upstream.md](docs/upstream.md)。

## 解决的问题

| 现象 | 原因 | 本仓库的处理 |
|---|---|---|
| 在 Magpie 里按 Home 打不开 ReShade 面板 | Magpie 的输出窗口是 `WS_EX_NOACTIVATE` + 渲染子窗口 `WS_EX_TRANSPARENT`，KeyEvent 根本到不了 ReShade；ReShade 的输入完全由窗口消息驱动，自己没装全局钩子 | `overlay-input` 用底层钩子把键鼠**直接贴进 swapchain 窗口** |
| 两条 NR 运行时不能同时挂 | 两个 add-on 都在改帧，同时加载会互相抢帧 | `nr-router` 按效果组把不用的那个改名成 `*.off`，物理上只加载一个 |
| 换了效果组，跑的却还是上一个运行时 | ReShade 只在**进程启动时**扫描 `*.addon64` | 同上：切换 = 换文件 + 重启 Magpie；`nr-router --restart` 一条命令做完 |
| 不确定运行时在本机能不能跑 | — | `nrtest.exe` 不启动 Magpie / ReShade，直接对运行时跑一帧并打印结论 |

## 组件

| 组件 | 产物 | 说明 |
|---|---|---|
| `overlay-input` | `overlay-input.addon64` | ReShade add-on。`WH_KEYBOARD_LL` / `WH_MOUSE_LL` + 15 ms 轮询，把按键、鼠标位置、按键状态、滚轮转发进 `swapchain->get_hwnd()`；热键（默认 Home）切换；覆盖期间吞掉原始输入，游戏不会同时响应 |
| `nr-router` | `nr-router.exe` | 读 `config\config.json` 的 `profiles[0].scalingMode` → 该组的效果链，判定含 `XeSS\XeSS_SR` 还是 `FSR3/FSR4_SR`，据此只留一个 add-on；同组混挂则拒绝执行；`--watch` 常驻、`--restart` 自动重启 Magpie |
| `nrtest` | `nrtest.exe` | 独立 D3D12 宿主，直接按 `LmxxfNrApi`（ABI v1）走 `QueryCapabilities → Create → PrepareSession → PrepareFrame → RecordInputs → EnqueueHip → RecordOutputs → Poll → Retire`，用一张合成渐变图跑一帧并打印结果 |

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
  amd-nr.addon64            或  amd-nr.addon64.off      ← mochizuki 宿主（OpenNR 构建）
  dlss5-amd.addon64        或  dlss5-amd.addon64.off    ← lmxxf 运行时
  MochizukiNrRuntime.dll  dlssnr-amd\                   ← mochizuki 运行时与权重
  DLSS5-AMD\native-game-tiled-assets\                   ← lmxxf 权重
  overlay-input.addon64  overlay-input.ini
  nr-router.exe  nr-router.cmd
  config\config.json  amd-nr.ini
```

两个效果组（Magpie 里自建，名字随意，判定只看组内效果名）：

```
NR-lmxxf      = FSR4\FSR4_SR   (+ FrameRate_Filter, XeSS MFG)
NR-mochizuki  = XeSS\XeSS_SR   (+ FrameRate_Filter, XeSS MFG)
```

## 目录

```
src/nr-router/      效果组 → 运行时的互斥路由（C++ / Win32）
src/overlay-input/  ReShade add-on：把键鼠喂进 Magpie 的渲染窗口
src/nrtest/         运行时冒烟测试（独立 D3D12 宿主）
scripts/            一键构建、上游获取
docs/               架构、构建、使用、排错、上游清单
dist/               预编译产物（由本仓库源码构建，见 dist/README.md）
```

## 许可

本仓库代码使用 **MIT**（见 [LICENSE](LICENSE)）。所依赖的上游项目各自保持原有许可
（ReShade BSD-3-Clause、Magpie MIT、OpenNR MIT、DLSSNR-AMD/Mochizuki MIT、lmxxf MIT、
MinHook MIT），详见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

**本仓库不分发** ReShade、Magpie、Intel XeSS、AMD FidelityFX 的二进制，也不分发任何模型权重。
