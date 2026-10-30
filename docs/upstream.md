# 上游获取与组装

本仓库**不包含** ReShade、Magpie、两条 NR 运行时及其权重。下面是我们实测可用的来源与摆放方式。

## 1. Magpie 宿主

两条路线，任选：

- **lmxxf 的 Magpie 发行包**（本项目的起点，内含 Magpie + 他改的 `DLSS5-AMD` 资产与 add-on）：
  <https://github.com/lmxxf/dlss5-on-amd-9070xt-porting> 的 release，包名形如 `Magpie-DLSS5-AMD-0.41`。
  实测版本：Magpie **0.6.6**（`logs\magpie.log` 会打印）。
- 上游 Magpie 原版：<https://github.com/Blinue/Magpie/releases>。用原版需要自己补
  `libxess.dll`、`amd_fidelityfx_upscaler_dx12.dll` 等 SR 相关 DLL，并自建 `effects\XeSS\XeSS_SR.hlsl`
  等占位效果（内容就是 `INPUT.SampleLevel(sam, pos, 0)` 的 1:1 拷贝）。

## 2. ReShade

- 下载页：<https://reshade.me> → 必须选 **“with full add-on support”** 的构建（普通构建不加载 add-on）。
  实测版本 **6.8.0.2155**。
- 用法：把 `ReShade64.dll` 改名为 **`dxgi.dll`**，放在 `Magpie.exe` 同目录。
  `Magpie.exe` 静态导入 `dxgi.dll`，所以 ReShade 会被拉进 Magpie 的进程 —— 这就是整条链路的入口。
- 首次启动会生成 `ReShade.ini`。`[OVERLAY] TutorialProgress=4` 可跳过新手引导；
  覆盖层热键在 `[INPUT] KeyOverlay`（默认 36 = Home），必须与 `overlay-input.ini` 的 `Key` 一致。

## 3. mochizuki 运行时（Vulkan）

来源：HuggingFace 数据集 <https://huggingface.co/datasets/zmodelerlover/amd-nr>

| 需要什么 | 路径 | 大小 |
|---|---|---|
| 运行时 + SPIR-V + ACO 记录 + int4 权重 | `mochizuki/0.5.9-opennr/mochizuki-0.5.9-opennr.zip` | 84 MB |
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

## 4. lmxxf 运行时

- 权重与引擎都在 `dlss5-amd.addon64` 里，随 Magpie 的 lmxxf 发行包提供：
  `DLSS5-AMD\native-game-tiled-assets\`（462 个 `.f32/.f16` + `HIP\gfx1200\*.hsaco`，约 594 MB）。
- 它需要驱动自带的 **HIP 7**（`amdhip64_7.dll`），RX 7000/9000 的 Adrenalin 都带。

## 5. OpenNR 宿主（`amd-nr.addon64`）

两条路：

- **用 `dist/amd-nr.addon64`**：本仓库已按上游源码构建好（MIT，声明见 THIRD-PARTY-NOTICES）；
- **自己构建**：<https://github.com/zmodelerlover/dlss5-neural-amd> → `build.ps1 -Target neural`
  （需要 VS 2022 Build Tools + Windows SDK，产物 `build\amd-nr.addon64`）。
  该工程自带 `3rdparty\reshade`（头文件）、`3rdparty\minhook`、`3rdparty\mochizuki\LmxxfNrApi.h`。

也提供预编译版本：HF 数据集同仓库的 `addon/<版本>/amd-nr.addon64`。

## 6. 组装检查清单

```
□ Magpie.exe + 它的依赖都在同一目录，能单独启动
□ dxgi.dll = ReShade 6.8 "with full add-on support"
□ ReShade.log 里出现 `Registered add-on ... using ReShade API version 20`
□ DLSS5-AMD\native-game-tiled-assets\ 有 462 个权重文件 + HIP\gfx1200\
□ MochizukiNrRuntime.dll + dlssnr-amd\{dlssnr-small.bin, shaders\, int4\, aco\, prewarm\}
□ 两个 add-on 一个叫 *.addon64、另一个叫 *.off（nr-router.exe --dry-run 会告诉你该谁）
□ overlay-input.ini 的 Key 与 ReShade.ini 的 [INPUT] KeyOverlay 一致
□ tools\nrtest.exe "<目录>" 能跑到 PrepareFrame rc=0
```

## 7. 版本与体积参考

| 项 | 值 |
|---|---|
| 整包解压后 | 约 1.04 GB |
| `DLSS5-AMD\native-game-tiled-assets` | 594 MB |
| `dlssnr-amd` | 286 MB |
| `libxess.dll` / `libxess_fg.dll` / `amd_fidelityfx_upscaler_dx12.dll` | 78 MB / 23 MB / 29 MB |
