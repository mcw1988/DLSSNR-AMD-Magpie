# 上游、获取与致谢

本仓库**不包含** Magpie、ReShade、运行时二进制与权重（Release 包另含 Magpie，见下）。
下面是我们实测可用的来源、摆放方式，以及必须说清楚的上游归属。

## 0. 上游一览

| 上游 | 提供了什么 | 许可 | 本仓库是否分发其二进制 |
|---|---|---|---|
| [SAOG0721/Magpie](https://github.com/SAOG0721/Magpie)（`experimental` 分支，v0.6.11） | 宿主本体：捕获、效果链、D3D11 渲染、不激活的缩放窗口 | **GPL-3.0** | Release zip 内含，仓库不含 |
| [lmxxf/dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) | `mochinr` 的钩子写法 + lmxxf 权重（`DLSS5-AMD\native-game-tiled-assets`） | MIT | 否 |
| [mochizuki0323/DLSSNR-AMD](https://github.com/mochizuki0323/DLSSNR-AMD) | mochizuki 运行时与 ACO/FP8 权重 | MIT | 否 |
| [zmodelerlover/OpenNR-ReShade](https://github.com/zmodelerlover/OpenNR-ReShade) | OpenNR 宿主、ReShade add-on 工程结构、RenoDX 风格面板写法 | MIT | 仅 `dist/amd-nr.addon64`（该源码的构建产物 + 本仓库补丁） |
| [crosire/reshade](https://github.com/crosire/reshade) | 注入层与 add-on API（6.8.0.2155） | BSD-3-Clause | 否 |
| [clshortfuse/renodx](https://github.com/clshortfuse/renodx) | 参数面板的风格与术语（经 OpenNR 传入本项目） | MIT | 否 |

> **GPL 提醒**：发布包（Release 里的 `Magpie-NR-045-v0.4-core.zip`）包含 Magpie Experimental 的
> 二进制，属 GPL-3.0。按 GPL-3 第 6 条，我们提供对应源码地址（SAOG0721/Magpie 的
> `experimental` 分支、v0.6.11 对应提交），本仓库自身代码为 MIT 且不与其链接（运行时通过
> ReShade add-on API 加载），二者是聚合分发关系。

## 1. Magpie 宿主（Experimental 0.6.11）

- 源码：<https://github.com/SAOG0721/Magpie>，默认分支 `experimental`
  （"Unofficial experimental Magpie fork with colour-only DLSS, FSR2 and NVIDIA RTX Video integrations"）。
- 实机用的是 **Magpie-DLSS5-AMD-0.45** 这一打包（内含 0.6.11 + XeSS/FFX 相关 DLL + 占位效果），
  `logs\magpie.log` 会打印版本号。
- 0.6.11 起设置存在 `config\v4e\config.json`，旧的 `config\config.json` 只在迁移时读取；
  同时还多了自动的 GPU 进程优先级（`GPU process priority verified: REALTIME`）。
- 上游 Blinue/Magpie（MIT）是另一个项目，本项目**没有**基于它。

## 2. ReShade

- 下载页：<https://reshade.me> → 必须选 **“with full add-on support”** 的构建（普通构建不加载 add-on）。
  实测版本 **6.8.0.2155**。
- 用法：把 `ReShade64.dll` 改名为 **`dxgi.dll`**，放在 `Magpie.exe` 同目录。
  `Magpie.exe` 静态导入 `dxgi.dll`，所以 ReShade 会被拉进 Magpie 的进程 —— 这就是整条链路的入口。
- 首次启动会生成 `ReShade.ini`。`[OVERLAY] TutorialProgress=4` 可跳过新手引导；
  覆盖层热键在 `[INPUT] KeyOverlay`（默认 36 = Home），必须与 `overlay-input.ini` 的 `Key` 一致。

## 3. mochizuki 运行时与权重（Vulkan / ACO）

作者仓库：<https://github.com/mochizuki0323/DLSSNR-AMD>（运行时 `MochizukiNrRuntime.dll`、
SPIR-V 变体、ACO 记录、FP8/int4 权重都出自这里）。

**分发方式**：作者本人不提供预编译发布，实际拿到的是 HuggingFace 数据集
<https://huggingface.co/datasets/zmodelerlover/amd-nr> 里 `mochizuki/` 目录下的重新打包版
（版权与许可仍是 mochizuki0323 的 MIT，只是换了托管方）。下表路径都相对该数据集根目录：

| 需要什么 | 路径 | 大小 |
|---|---|---|
| 运行时 + SPIR-V + ACO 记录 + int4 权重 | `mochizuki/0.5.8-opennr/`（与 OpenNR 0.7.15 官方配对；0.5.9 也在同一目录下） | 84 MB |
| RDNA3 专用变体（可选，RDNA4 不需要） | `mochizuki/0.5.9-opennr/mochizuki-rdna3-0.5.9-opennr.zip` | 9 MB |
| 默认网络（**包内没有，必须单独下**） | `mochizuki/model-small-1/dlssnr-small.bin` | 114 MB |
| 310.8 网络（可选，int4 mixed 需要它） | `mochizuki/model-1/dlssnr.bin` | 148 MB |

**注意压缩包里的目录是拍平的**：`dlssnr-amd.shaders.renodx/` 实际是
`dlssnr-amd/shaders/renodx/`，也就是“点 = 路径分隔符”。运行时按 `dlssnr-amd\<子目录>` 找资产，
所以解包时要还原成嵌套结构：

```
MochizukiNrRuntime.dll            ← zip 根目录
dlssnr-amd\
  dlssnr-small.bin                ← model-small-1 那份
  dlssnr-int4.bin
  shaders\  shaders-rdna3\  int4\  aco\  prewarm\
```

（`scripts/fetch-upstream.ps1` 会替你做这件事。）

> **部署目录必须是纯 ASCII 路径**。mochizuki 的 ACO 判定不兼容中文路径，
> 放在 `D://迅雷下载//...` 下会报 `ACO asked for, but ...\aco has no records\ and shaders\`，
> 即使文件齐全；换到 `D://Magpie-NR-045` 立刻变成 `ACO: the network runs ACO's machine code`。

## 4. lmxxf 运行时

- 权重与引擎都在 `dlss5-amd.addon64` 里，随 lmxxf 的 Magpie 发行包提供：
  `DLSS5-AMD\native-game-tiled-assets\`（462 个 `.f32/.f16` + `HIP\gfx1200\*.hsaco`，约 594 MB）。
- 它需要驱动自带的 **HIP 7**（`amdhip64_7.dll`），RX 7000/9000 的 Adrenalin 都带。
- 本项目的 `mochinr` 借鉴了它的**钩子写法**（`ffxDispatch` 拦截、提交期劈批、重放），
  没有复用它的代码。

## 5. OpenNR 宿主（`amd-nr.addon64`，v0.4 起退役）

- 源码：<https://github.com/zmodelerlover/OpenNR-ReShade>（原 `dlss5-neural-amd`，GitHub 会自动跳转）
  → `build.ps1 -Target neural`，产物 `build\amd-nr.addon64`。该工程自带 `3rdparty\reshade`、
  `3rdparty\minhook`、`3rdparty\mochizuki\MochiNrApi.h`。
- 本项目对它的改动在 `src/opennr-patches/opennr-0.7.15-magpie-nr.patch`（含 D3D11↔D3D12 共享围栏）。
- 也提供预编译版本：HF 数据集同仓库的 `addon/<版本>/amd-nr.addon64`。

## 6. 组装检查清单（v0.4）

```
□ Magpie.exe + 依赖同目录，能单独启动；目录是纯 ASCII 路径
□ dxgi.dll = ReShade 6.8 "with full add-on support"
□ ReShade.log 出现 Registered add-on ... using ReShade API version 20
□ 三个后端只有一个 *.addon64，其余为 *.off（nr-router.exe --dry-run 会告诉你该谁）
□ MochizukiNrRuntime.dll + dlssnr-amd\{dlssnr-small.bin, shaders\, int4\, aco\, prewarm\}
□ config\v4e\config.json 里有 NR-preupscale / NR-lmxxf / 仅补帧 三个组
□ overlay-input.ini 的 Key 与 ReShade.ini 的 [INPUT] KeyOverlay 一致
□ nrtest.exe "<目录>" 能跑到 PrepareFrame rc=0；nrbench.exe 能出每帧分解
```

## 7. 版本与体积参考

| 项 | 值 |
|---|---|
| 发布包（v0.4 core） | 约 1.5 GB 解压后（不含 lmxxf 权重与 NVIDIA 专用 DLL） |
| `DLSS5-AMD\native-game-tiled-assets` | 594 MB（不在包内） |
| `dlssnr-amd` | 286 MB |
| `libxess.dll` / `libxess_fg.dll` / `amd_fidelityfx_upscaler_dx12.dll` | 78 MB / 23 MB / 29 MB |

## 8. 致谢

没有下面这些人的工作，这个项目不会存在：

- **SAOG0721** — Magpie Experimental，整套捕获与缩放链路的地基；
- **lmxxf（Kien）** — 第一个把 DLSS 神经渲染搬到 AMD 的人之一，`mochinr` 的钩子写法直接来自其工程；
- **mochizuki0323（望月）** — mochizuki 运行时
  （[mochizuki0323/DLSSNR-AMD](https://github.com/mochizuki0323/DLSSNR-AMD)）：
  用 Vulkan + FP8 把这套网络做得又快又省，是整个 NR 后端的核心；
- **zmodelerlover** — OpenNR 宿主与 RenoDX 风格面板，本项目 add-on 工程结构与参数组织都受益于此；
- **crosire** — ReShade，本项目所有 add-on 的宿主与 API；
- 以及 **danielblnc**、**TheAutomatic** 等把 DLSS-NR 带到 AMD 的先行者。

郑重感谢。
