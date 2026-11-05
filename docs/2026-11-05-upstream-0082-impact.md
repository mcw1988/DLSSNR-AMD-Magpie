# 上游 v0.0.8.2 对我方 v0.6 的影响评估

> 2026-11-05。对象：`mochizuki0323/DLSSNR-AMD` v0.0.8.2（GitLab，2026-11-02，commit `c53878b5`）。
> 本文只做评估，不改代码。所有数字标【实测】/【推算】/【源码】并给算式；口径混用处逐条点名。
>
> **本文最重要的一条结论（与任务简报的前提相反）**：上游 v0.0.8.2 **不是**我们运行时的一个更新版本，
> 而是**另一条产品线**。它不提供任何我们能加载的运行时二进制，其 ABI 与我们的 `MochiNrApi.h`
> 完全不相交。我们已经在用的资产（权重 / int4 表 / ACO 记录）**与 v0.0.8.2 逐字节相同**——
> 也就是说"用新权重"这件事，我们在 v0.6 之前就已经做完了。详见 §2、§3。

---

## 0. 结论摘要

| # | 结论 | 依据 |
|---|---|---|
| 1 | **上游 v0.0.8.2 不提供可替换我们运行时的二进制。** 它发行 `dlssnr-amd.asi`（OptiScaler 插件）/ `dlssnr_amd.addon64`（ReShade 插件）/ `_nvngx.dll`（OptiScaler-NR 的 NGX core），全仓库无 `MochiNrGetApi` / `LmxxfNrGetApi` / `MochiNrApi.h` 的任何字样 | §3.1【源码】对 v0.0.8.2 源码包（1203 文件）全量 grep，0 命中 |
| 2 | **"最新的 mochizuki 权重"我们已经有了，逐字节相同。** `dlssnr-small.bin` / `dlssnr.bin` / `dlssnr-int4.bin` 三个 SHA256 与上游 v0.0.8.2 `install.ps1` 内置常量**完全一致**；int4 标定表 279/279 文件一致 | §2.2【实测】逐文件 SHA256 比对 |
| 3 | **上游 v0.0.8.2 的运行时（`nr_runtime.cpp`）与我们手上的运行时是两个分支**，不是版本前后关系。我们那份带 `NR_KEEP_RGBA32F`（上游 0.0.8.1/0.0.8.2 都没有）、不带 `CC_SMALL_FP8_EXPERIMENTAL`、不带 `lazy-images`/`materialize`/`make_noise`/`switch_history` | §3.2【实测】二进制字符串比对 |
| 4 | **帧时间收益 ≈ 0，且不是我们能拿的。** 上游 4K network-only 实测 12.67 ms（CC_SMALL_FP8）/ 11.09 ms（EXPERIMENTAL），但那是在**它自己的运行时**上；把我们的 `I'` 换成它的数字，得到的是"如果我们的岛也变成那么快"的**反事实**，不是可达目标 | §4.1【推算】 |
| 5 | **"线 1 挂起"的结论不被推翻。** 三档收益表在新 `I'` 下数值上移，但决定立项的三个前提量（ratio / D' / I'）在**我们这条链上仍未测**，上游的数字一个都没替我们测 | §4.3 |
| 6 | **显存：上游的 1410 MiB 降幅我们一分拿不到**，因为它来自 `nr_runtime.cpp` 里 0.0.8.2 新增的惰性图像/延迟噪声表等机制，我们的运行时没有这些代码。我们能拿的只有 `h16` 历史精度那一条 | §5.2 |
| 7 | **有一件上游新增的东西我们能直接白拿：上游公开了一份可复现的 PSNR/SSIM 对照语料**（输入 PNG + NVIDIA 原生输出 PNG + 结果 JSON），可作为"对齐"目标的可执行验收 | §6.2 |
| 8 | **面板冲突点已定位**：上游 v0.0.8.2 新增的 `History Precision`（16/32）**在我们的 ABI 里没有对应控制项**，且我们的运行时也不认这个 ini 键；RenoDX 的 `NRSdrBlackRestore` / `NRCodecMode` 我们**已有等价字段**（`reno_sdr_black_restore` / `reno_codec_mode`），只是没接线 | §7.1 |
| 9 | **供应链风险实测为真**：GitHub 账号已 404，仓库搬到了 GitLab；HF 数据集当前从本机不可达（HTTP 000）。我们依赖的 0.5.8/0.5.9 运行时来源（HF `zmodelerlover/amd-nr`）随时可能消失 | §8.1 |

**一句话净作用**：v0.0.8.2 对我们是**一份可读的路线图和一份可复现的验收基准**，不是一份可安装的更新。
它唯一能变成实打实收益的通道是"我们自己把上游 0.0.8.2 的运行时改动移植过来"，那是 v0.7 级别的工程，不是 v0.6。

---

## 1. 上游改了什么（分组）

以下全部来自 v0.0.8.2 release notes 与源码 diff（`v0.0.8.1` → `v0.0.8.2`）。

### 1.1 显存大幅下降

release notes 原文口径：**"One network, 0.0.8.1 -> 0.0.8.2, with the default History precision
(the network alone, without the processing of the styles and routes; measured on Linux with an RX 9070 XT)"**。

| 网络 | 1080p | 1440p | 4K |
|---|---|---|---|
| CC_SILVER_AARDWOLD | 690 → 397 MiB | 1110 → 569 MiB | 2255 → **934** MiB |
| CC_SMALL_FP8 / CC_SMALL_FP8_EXPERIMENTAL | 671 → 320 MiB | 1072 → 461 MiB | 2216 → **806** MiB |
| CC_SILVER_AARDWOLD_INT4_MIXED | 713 → 430 MiB | 1133 → 607 MiB | 2278 → **987** MiB |

口径提醒（三条，缺一不可）：
- **Linux + RX 9070 XT**，不是 Windows、不是我们的卡。
- **网络本身**，不含 styles / routes（RenoDX 那层）。
- **默认 History precision**（= 0.0.8.2 的新默认 16）。

### 1.2 History precision 16/32

release notes：新增 `[Network] History Precision`，**16 是默认**，16 = 保持历史图像为 16-bit 浮点（与 NVIDIA 的 DLL 一致），
32 = 保持 0.0.8.1 的 32-bit 历史。可在游戏内切换。

源码位置【源码】：`windows/src/core/nr_runtime.hpp:522-524`（`set_history_f16`）、
`windows/src/core/nr_runtime.cpp:1253-1259`（读 ini，`NR_HIST16` 环境变量优先）、
`windows/src/ui/nr_control_pages.hpp:1114-1117`（UI 下拉）。

**关键机制**：0.0.8.1 的 `nr_runtime.cpp:283` 只有一行注释
`// (research NR_HIST16=1): the one-pass history images in RGBA16F.`——即 0.0.8.1 里它是**研究开关**；
0.0.8.2 把它变成产品特性，并新增 `h16_ok`（探测 shaders 目录里有没有 16-bit 的 post 及其 grade 孪生）
与 `switch_history()`（切换时重建历史图像、每个 feature 的历史也一起清，历史从头开始）。

**显存归因**【推算】：4K 历史图像 2×133 MiB（RGBA32F）→ 2×66 MiB（RGBA16F），
省 **134 MiB**（`nr_runtime.cpp:302-303` 注释原文 "4K: 2 x 66 MB instead of 2 x 133"）。
1410 MiB 总降幅中，其余约 **1276 MiB** 来自 0.0.8.2 新增的惰性图像/延迟噪声表等机制（见 §5.2）。

### 1.3 每个游戏帧只跑一次 NR

release notes 修了"NR at Present 对每个呈现帧都跑"：在游戏自带 FG（FSR 3 / 游戏内建 XeSS FG / 多帧生成）、
OptiScaler 的 XeSS FG、DLSSG via Nvngx 三种情形下，NR 现在**每个游戏帧只跑一次**，
帧生成拿满收益，NR 效果不再翻倍。

**这条对我们特别相关**：我们的旧链（ReShade `amd-nr.addon64` 在 present 层）**正是**这个毛病。
本仓库 `docs/measurements.md:60-62` 留了计数证据：
`3311 / 1650 = 2.00`——FG 2x 下 NR 对每个**呈现**帧跑了一次，补帧倍率直接变成 NR 成本的倍率。

**但要注意**：上游这条修复是在**它自己的 OptiScaler/ReShade 路线**里做的（`nr_present_cadence.hpp`
按呈现节奏判定游戏帧）。我们的 P2 链是 Magpie 的 `DLSSNRFilter` 效果槽，NR 跑在**每张被捕获的帧**上，
与 Magpie 的捕获节奏绑定、而不是与游戏的 FG 节奏绑定。**这意味着我们不能直接宣称"我们天然拥有
每游戏帧一次 NR"**——那需要在我们自己的链上做同样的逐帧计数对照（§7.4）。

### 1.4 其它修复（与我们无直接关系，列出以求完整）

- wilsjo2 风格 + DLSSFG(Streamline) 下的 GPU hang；NR Control 闪烁；
  HUD-less 画面生成帧无 NR 闪烁；游戏 FG 与 OptiScaler FG 同开时冻结；
  动态分辨率后长时间无 NR（现在预分配尺寸）；Status 页 Vulkan/DX9 路线显存显示 0 MiB；
  HUD FIX + swapchain 被包裹时显存/内存涨到崩溃。

### 1.5 打包与签名

Windows 二进制加签名（自签，"signed by an unknown publisher"，无需安装或信任）；
新增 CJK 语言包（zh-CN/ja/ko + 字体）；`dlssnr-amd.ini` 每次安装重置为新默认（旧设置不迁移）。

### 1.6 新问题：RenoDX 风格在 SDR 下的暗部黑斑

release notes 未提，但上游 README 与键表里有对应机制【源码】：
`windows/src/core/nr_renodx_keys.hpp:90` —
`{"NRSdrBlackRestore", ..., 0..2, "Composition", "SDR black restore", "Auto\0Every block\0Smooth\0"}`
`windows/src/core/nr_renodx_keys.hpp:92` —
`{"NRCodecMode", ..., 0..3, "Composition", "Codec", "Classic\0Anchored\0Auto\0Display\0"}`
默认值分别是 `NRSdrBlackRestore=0`（Auto）、`NRCodecMode=2`（Auto）。

修法（简报所述，`NRSdrBlackRestore = 2` 即 Smooth，或 `NRCodecMode = 0` 即 Classic）**在键表上成立**。
**我们的场景是 SDR（WoW）且用 RenoDX 风格**——这正好是上游点名的触发组合，所以这不是可选项。

### 1.7 供应链搬家

作者原话：GitHub 账号被意外封禁，原仓库不可访问，改在 GitLab 继续更新。

【实测】`https://github.com/mochizuki0323` 返回 **404**（账号不存在/已封）；
`https://gitlab.com/api/v4/users/mochizuki0323/projects` 只返回一个项目 `DLSSNR-AMD`
（创建于 2026-10-30，最后活动 2026-11-04，10 star / 2 fork）。
标签 v0.0.1 … v0.0.8.2 齐全。

---

## 2. "最新的 mochizuki 权重"到底是什么

### 2.1 澄清简报里的一处理解

简报问"最新的 mochizuki 权重到底是什么"。核实后的准确答案是：

**权重不属于 mochizuki，属于 NVIDIA。** 上游 README 原文：模型**不随包分发**，
安装器从**用户自己的** `nvngx_dlssnr.dll` 里抽取。NVIDIA 的两个模型由它们自己的 codename 命名：

| 文件 | NVIDIA codename | 来自 | 条目数 | 大小 |
|---|---|---|---|---|
| `dlssnr-small.bin` | **CC_SMALL_FP8** | 310.9.16 | 255 | 108.6 MiB |
| `dlssnr.bin` | **CC_SILVER_AARDWOLD** | 310.8.0（310.9.16 也含） | 599 | 140.9 MiB |

**项目自研的变体只有两个**，而且都是**从 NVIDIA 模型派生**的，不是新模型：

| 变体 | 怎么来的 | 含 NVIDIA 权重吗 | 前提 |
|---|---|---|---|
| **CC_SMALL_FP8_EXPERIMENTAL** | 改快 CC_SMALL_FP8：Swin 块除首个外用 ReLU；decoder 部分 window-attention 层用固定注意力表（不用 QK^T/softmax，合并 V 与输出投影）。权重在安装时由「原模型 + 差分文件」算出 | 差分文件本身**不含**（`each changed entry XOR the small model's`），但生成物是从 NVIDIA 模型算出来的 | 需 310.9.16 DLL；RX 9000 独占；**仅 ACO 机器码** |
| **CC_SILVER_AARDWOLD_INT4_MIXED** | 部分计算走 int4，权重由本项目对 **1112 帧**游戏画面标定的表生成 | 生成物基于 NVIDIA 模型 | 需 310.8.0 或 310.9.16 |

所以"说'用新权重'"在我们这里的准确含义只有一条：**换成 `CC_SMALL_FP8_EXPERIMENTAL`**（省 1.58 ms @4K）。
而它要求一个我们没有的运行时（§3）。

### 2.2 我们手上的资产已经与上游 v0.0.8.2 逐字节相同【实测】

这是本次评估最硬的一条。逐文件 SHA256 比对结果：

| 资产 | 我们的 SHA256 | 上游 v0.0.8.2 期望值 | 相同？ |
|---|---|---|---|
| `dlssnr-small.bin`（CC_SMALL_FP8） | `7E966E26…43C6E33` | `7E966E26…43C6E33`（`install.ps1` 的 `$SmallSha`） | ✅ |
| `dlssnr.bin`（CC_SILVER_AARDWOLD） | `2B41C888…1F7FBD` | `2B41C888…1F7FBD`（`$ModelSha`） | ✅ |
| `dlssnr-int4.bin` | `22A0E2D3…602319C` | `22A0E2D3…602319C`（`linux/data/int4/rdna4/dlssnr-int4.sha256`） | ✅ |
| int4 标定表 `int4/**` | 279 个文件 | `linux/data/int4/rdna4/**` | ✅ **279/279 全同** |
| ACO 记录 `aco/records/*.nrp` | 62 个 | `windows/data/aco/records/` 101 个 | ✅ 交集 57 个**全同**，另 5 个上游没有 |
| int4 ACO 记录 `aco/int4/records/*.nrp` | 75 个 | `windows/data/aco/int4/records/` 75 个 | ✅ 数量一致 |

上游 `install.ps1` 里写死的两个常量，与我们文件里的哈希**完全吻合**——这不是"版本相近"，是**同一份字节**。

**推论**：简报问题 3 里"能不能不换运行时、只换权重"的分母是错的——**权重这一侧根本不需要换**，
我们早就是 v0.0.8.2 的权重。唯一的"新权重"是 EXPERIMENTAL，而它需要新运行时（§3.3）。

### 2.3 我们能自己造出 EXPERIMENTAL 权重吗？能，但造出来也跑不了

【实测】我用上游的 `linux/build/experimental_weights.py apply`，拿**我们自己的** `dlssnr-small.bin`
加**上游的** `linux/data/experimental/dlssnr-small-experimental.delta`（2,031,749 字节，纯 XOR 差分）
生成了 `dlssnr-small-experimental.bin`，SHA256 = `8bf68796d0994b543e539ce239b2914459c0d4a53fb48dbc1bfc392b08050755`，
与上游 `dlssnr-small-experimental.sha256` **完全一致**。

这证明：**EXPERIMENTAL 权重不需要任何 NVIDIA DLL 就能离线复现**（差分文件不含 NVIDIA 权重）。
但它同时证明了一件更重要的事——**权重从来不是瓶颈，运行时才是**（§3）。

---

## 3. 兼容性判断：换不换上游运行时

### 3.1 上游 v0.0.8.2 没有任何我们能加载的二进制【源码】

我把 v0.0.8.2 的源码包（175 MB，1203 文件）解包后全量 grep：

```
MochiNrGetApi      → 0 文件
LmxxfNrGetApi      → 0 文件
MochiNrApi         → 0 文件
MochizukiNrSetControls → 0 文件
MochizukiNrRuntime → 0 文件
Lmxxf / Mochi / opennr → 0 文件
GetApi             → 0 文件
```

上游 v0.0.8.2 发行的是（`windows/package/install.ps1` + `windows/build/build_package.sh`）：

| 产物 | 形态 | 我们的 `amdgx_dlssnr.dll` 能替代它吗 |
|---|---|---|
| `dlssnr-amd.asi` | **OptiScaler 插件**（`InitializeASI` 导出） | ❌ 完全不同的宿主契约 |
| `dlssnr_amd.addon64` | **ReShade add-on** | ❌ 不同的宿主契约 |
| `_nvngx.dll` | OptiScaler-NR 用的 **NGX core**（NVSDK_NGX_D3D12_* 全套） | ⚠️ **ABI 面最接近**（见下） |
| `dlssnr-amd/` 资产树 | shaders / aco / int4 / lang | ✅ 格式兼容（我们已经在用同格式） |

唯一"可能"对接的是 `_nvngx.dll`：它导出 `NVSDK_NGX_D3D12_AllocateParameters / CreateFeature /
DestroyParameters / EvaluateFeature / GetCapabilityParameters / GetFeatureRequirements /
GetScratchBufferSize / Init / Init_Ext / Init_ProjectID / ReleaseFeature / Shutdown / Shutdown1`，
与 Magpie 宿主 `DLSSNRFilter.cpp` 需要的导出集**高度重合**，且它读的参数名
（`DLSSNR.Color` / `DLSSNR.Output` / `DLSSNR.MVec` / `DLSSNR.Depth` / `DLSSNR.Intensity` /
`DLSSNR.Style` / `DLSSNR.LocalStructureStrength` / `DLSSNR.LocalToneStrength` /
`DLSSNR.SkinStructureStrength` / `DLSSNR.UseAutoMask` / `DLSSNR.Width` / `DLSSNR.Height` /
`DLSSNR.Reset` / `DLSSNR.DepthInverted` / `DLSSNR.MVecScaleX/Y` / `DLSSNR.UICorrection` /
`DLSSNR.Hint.Render.Preset` + 全部 Subrect 键）与**宿主实际设置的那 42 个键几乎完全一致**。

**但这条路对我们不成立**，三条理由【源码】：
1. `_nvngx.dll` 是为 **OptiScaler-NR** 准备的：它要求 OptiScaler 的 NVNGXProxy 去加载、
   由 OptiScaler 建参数块，它自己不导 `NVSDK_NGX_D3D12_AllocateParameters` 之外的宿主侧约定
   （README 明说"no NR helper DLL is required"是给 OptiScaler-NR fork 的）。
2. 它是 **Windows 侧交叉编译产物**（mingw，`windows/build/build_optiscaler_nr.sh`），
   走的是自己的 Vulkan 设备 + 共享纹理 + 时间线信号量的桥接（`nr_pe_bridge.cpp`），
   我们的 `amdgx_dlssnr.dll` 是**直接把手里的 D3D12 资源交给运行时**（`passthrough.cpp` 的
   `PrepareFrame(color)` → `RecordInputs`），两条路的资源所有权模型不同。
3. 它的"每游戏帧一次 NR"是按 **OptiScaler 路线**的呈现节奏算的，
   换到 Magpie 效果槽路线上，"游戏帧"这个概念得重新定义（§7.4）。

### 3.2 我们手上的运行时与上游是两个分支，不是版本前后【实测】

对两份二进制作字符串比对：

| 特征 | 我们的 `MochizukiNrRuntime.dll` | 上游 0.0.8.1 | 上游 0.0.8.2 |
|---|---|---|---|
| 导出 `LmxxfNrGetApi` | ✅ | ❌（无此 ABI） | ❌ |
| 导出 `MochizukiNrSetControls` | ✅ | ❌ | ❌ |
| `NR_HIST16`（历史精度开关） | ✅ | ✅（仅研究开关） | ✅（产品化 + `h16_ok` + 运行时切换） |
| **`NR_KEEP_RGBA32F`** | ✅ | ❌ | ❌ |
| `CC_SMALL_FP8_EXPERIMENTAL` | ❌ | ❌ | ✅ |
| `lazy-images` / `materialize` / `make_noise` / `retired_pools` | ❌ | ❌ | ✅（0.0.8.2 新增） |
| `switch_history` / `h16_ok` | ❌ | ❌ | ✅（0.0.8.2 新增） |
| `NR_RENODX_FUSE` / `NR_RENODX_PROF` | ✅ | — | ✅ |
| `v6_pedestal_reduce` / `SDR (legacy codec)` / `absolute HDR (v6 codec)` | ✅ | — | ✅ |

两个关键读数：

1. **我们有 `NR_KEEP_RGBA32F`、上游 0.0.8.1/0.0.8.2 都没有** → 我们这份不是"上游某个旧版本"，
   是**平行的另一个分支**（上游 v0.0.2.3 的 `nr_renodx_keys.hpp` 里也没有 `NRSdrBlackRestore`/`NRCodecMode`）。
2. **我们缺上游 0.0.8.2 的全部显存机制**（`lazy-images` 等 6 个标记一个都没有）→ §5.2 的 1410 MiB 与我们无关。

另注（顺手纠一处本仓库的隐患）：`src/nrtest/MochiNrApi.h` 声明的唯一导出是 `MochiNrGetApi`，
而**二进制实际导出的是 `LmxxfNrGetApi`**。`src/nvngx-shim/passthrough.cpp:182` 取的正是 `LmxxfNrGetApi`
（并注释"DLL 导出名：历史 ABI 名"），所以**当前是自洽的**；commit `c30e972` 也是这么裁决的
（"GetProcAddress 的 DLL 导出名保留 LmxxfNrGetApi（二进制历史导出，不可改）"）。
风险在于：如果哪天按头文件的名字去找，会静默失败。**建议在 §7 里加一条"导出名解析要同时试两个名字"的加固**。

### 3.3 能不能不换运行时、只换权重？

| 换什么 | 判据 | 结论 |
|---|---|---|
| 换 CC_SMALL_FP8 / CC_SILVER_AARDWOLD / INT4_MIXED 的权重 | 权重是否与上游 v0.0.8.2 一致 | **不需要换**，已经逐字节一致（§2.2） |
| 换 CC_SMALL_FP8_EXPERIMENTAL 的权重 | 运行时是否认这个网络 | **不行**。我们的运行时**没有** `CC_SMALL_FP8_EXPERIMENTAL` 这个概念（字符串 0 命中），也不读 `[Network] Model`；上游要求该网络有专属的 `experimental/shaders` 目录与 `dlssnr-small-experimental.bin` 并配 `Model = CC_SMALL_FP8_EXPERIMENTAL` |
| 换 int4 混精网络 | 运行时是否认 + 资产是否齐 | 我们**已经具备**：`dlssnr-int4.bin`（哈希一致）+ `dlssnr-amd/int4/` 标定表 279 个全同 + 运行时里有 `CC_SILVER_AARDWOLD_INT4_MIXED` 字符串。我们只是**没接线**（见 §7.3） |

**判据总结**：权重侧已就位；**唯一能带来速度的 EXPERIMENTAL 被运行时卡住**；
而 EXPERIMENTAL 那 1.58 ms @4K 的收益，按上游自己的说法还需要"仅 ACO 机器码 + RX 9000 独占"。
换言之：即使我们把 EXPERIMENTAL 跑起来，**收益也只有在 ACO 路径的 RX 9000 上成立**，
而我们的运行环境是 Windows + ACO（`passthrough.cpp` 已传 `MOCHI_NR_CREATE_FLAG_ACO`）——这一条是满足的。

### 3.4 能不能用上游 `dlssnr_extract_model.exe` 从我们已有的 DLL 重抽？

简报问的是 `D:\DLSS-NR-magpie\DLL替换\` 下的几个 DLL。核实【源码 + 实测】：

上游的提取工具**不按版本字符串接受，而按整文件 SHA256 白名单接受**
（`windows/package/model-tools/dlssnr_extract_model.cpp:39-40, 864-872`）：

```
kSmallDllSha256 = 2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f   # 310.9.16
kDllSha256      = e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e   # 310.8.0
```

对我们本地四个 DLL 逐一算 SHA256：

| 文件 | FileVersion | SHA256 | 上游接受？ |
|---|---|---|---|
| `nvngx_dlssnr-310.9.16-50s.dll` | 310,9,16,0 | `2bc19bc0…28789f` | ✅ **精确命中 310.9.16** |
| `nvngx_dlssnr-301.9.16-40&50.dll` | 310,9,16,0 | `a8a237a9…f902b8` | ❌ 被拒（虽是 310.9.16 的改版） |
| `nvngx_dlssnr-30s-0.2.dll` | 310,9,16,0 | `c7d4c8b6…be3f1ed` | ❌ 被拒 |
| `nvngx_dlssnr.dll` | 310,8,0,0 | `984bee0f…5f81014` | ❌ 被拒（虽是 310.8.0 的改版） |

**结论**：**恰好一个能用**——`nvngx_dlssnr-310.9.16-50s.dll` 逐字节就是上游认可的 310.9.16。
用工具重抽会得到与我们现有 `dlssnr-small.bin` / `dlssnr.bin` **相同**的字节（哈希已对过），
所以**重抽没有增量价值**，只是多一条"我们能自证合规"的路径。
（注意：上游 310.8.0 的白名单哈希我们本地没有对应文件，若将来要用 310.8.0 模型需另行取得。）

---

## 4. 定量收益：帧时间

### 4.1 三个 `I'` 下的三档收益表【推算】

沿用 `docs/2026-11-04-line1-recompute.md` §2 的模型与 η 公式（不变）：

```
T_sum(s) = D_side + I'(s)          D_side = 5.7 ms（残差，未独立测量）
I'(s)    = I'(1.0) · s²
T_max(s) = max(D_floor=6.25, max(D_side, I'(s)))
ΔT(s)    = T_sum(s) − T_max(s)
η(r)     = 1 (r≤0.60) / (0.85−r)/0.25 (0.60<r≤0.85) / 0 (r>0.85)
T_target = max(D_floor, T_sum − η(r)·ΔT)
```

**口径声明（关键）**：
- 基线 `I'(1.0) = 13.7 ms` 是**旧链拟合反推值**，不是新链实测（`line1-recompute.md` §1.4 明写
  "旧链五点拟合…**假设新链网络成本不变**"）。
- 上游 12.67 / 11.09 ms 是**它自己的运行时上的 network-only 时间**（Linux + RX 9070 XT + 它的运行时）。
- 因此下表是**反事实**："假如我们的岛也快到那个水平"。**它不是可达目标**，因为我们换不到那个运行时（§3）。
- 三者**不可相加**：12.67 相对我们的 13.7 已经省了 1.03 ms，但那是**两个不同运行时之间**的差，
  不是"我们换成上游权重"的收益。

| 档 | 量 | 基线 I'=13.7 | 上游 CC_SMALL_FP8 I'=12.67 | 上游 EXPERIMENTAL I'=11.09 |
|---|---|---|---|---|
| **1.0** | 现状 fps | 51.5 | **54.4** | **59.6** |
| | 满步上限 fps | 73.0 | **78.9** | **90.2** |
| | r=0.72 现实目标 fps | 60.8 | **64.9** | **72.3** |
| | r=0.76 现实目标 fps | 57.6 | **61.3** | **67.9** |
| **0.5** | 现状 fps | 109.6 | 112.8 | 118.0 |
| | 满步上限 fps | 160（显示器封顶） | 160 | 160 |
| | r=0.72 现实目标 fps | 131.1 | 133.2 | 136.7 |
| **0.35** | 现状 fps | 135.5 | 137.9 | 141.7 |
| | 满步上限 fps | 160（显示器封顶） | 160 | 160 |
| | r=0.72 现实目标 fps | 147.2 | 148.6 | 150.6 |

**算式**（1.0 档，r=0.72，I'=11.09 为例）：
`I' = 11.09`；`T_sum = 5.7 + 11.09 = 16.79`；
`T_max = max(6.25, max(5.7, 11.09)) = 11.09`；`ΔT = 16.79 − 11.09 = 5.70`；
`η(0.72) = (0.85−0.72)/0.25 = 0.52`；`T_target = max(6.25, 16.79 − 0.52×5.70) = 13.83`；
`fps = 1000/13.83 = 72.3`。

### 4.2 1.0 档现状帧时间的直接对照

简报问"换 EXPERIMENTAL 后，新链 1.0 档（现状 51.5 fps / 19.4 ms）的帧时间与 fps 变成多少"。

- **现状（我们实测，P2）**：19.2–19.6 ms，51–52 fps（`2026-11-04-P2-milestone.md`）。
- **若 `I'(1.0)` 变成 11.09**：`T_sum = 5.7 + 11.09 = 16.79 ms` → **59.6 fps**。
  相对现状 **+8.1 fps（+15.7%）**。
- **若 `I'(1.0)` 变成 12.67**：`T_sum = 18.37 ms` → **54.4 fps**，相对现状 **+2.9 fps（+5.6%）**。

**但这两个数都不是我们的可达值**，理由见 §3：EXPERIMENTAL 需要上游 0.0.8.2 的运行时（我们没有，
且我们那份运行时连 `CC_SMALL_FP8_EXPERIMENTAL` 都不认识）。

### 4.3 "线 1 挂起"的结论会被推翻吗？——不会

**结论：不会推翻，而且 §4.1 的数值上移不构成任何推翻理由。** 三条：

1. **前提量一个都没被上游替我们测。** `line1-recompute.md` §5.4 的裁决门槛全部是**我们这条链**的量：
   新链 ratio（M1，逐档）、`D'` 的独立实测（M2）、`slotWait`（M4）。
   上游给的是**它自己运行时上、Linux、另一个分辨率口径**的 network-only ms——
   **这恰好是 M4 要测的东西的"别人家的版本"**，不能替代我们自己的 M4。
   ratio 更是完全无从推断（取决于我们的岛在帧里的占比）。

2. **上游数字的形状提示了一个对我们不利的事实**：上游把"每游戏帧一次 NR"作为独立修复项，
   说明"帧生成倍率放大 NR 成本"是**这一代 NR 实现的普遍形态**，
   我们的旧链也有（`3311/1650 = 2.00`）。上游修好它，是因为它**要靠这个修复来兑现帧生成的收益**；
   我们若将来也要"NR + 帧生成"的组合，**同样需要先解决这个**——这加强了线 1/帧生成这条线的重要性，
   但仍然要靠我们自己实测。

3. **即便 `I'` 变小，线 1 的相对价值也没变。** 线 1 的收益公式里 `I'` 同时出现在 `T_sum` 和 `T_max`
   中，`ΔT` 对 `I'` 的敏感度远低于对 `D'` 的敏感度（`ΔT = T_sum − T_max`，而 `T_max` 在 1.0 档
   恰由 `I'` 主导）。**降 `I'` 主要抬高的是"现状"，而不是线 1 的增量空间**——
   看表：1.0 档现状 51.5→59.6（+8.1），而满步上限 73.0→90.2（+17.2），
   现实目标 60.8→72.3（+11.5）。**三档收益的"绝对上限"仍由 `D'` 和 ratio 决定，不由 `I'` 决定。**
   而 `D'`（5.7 ms）恰恰是我们**唯一还没独立测量**的那个数。

**唯一被上游真正改变判断的，是"我们不必再纠结权重"这件事**：既然权重已是最新，
v0.6 的性能预算就应按"权重固定 = `I'` 固定"来做，而不是留一个"等换权重"的变量。

---

## 5. 定量收益：显存

### 5.1 上游的数字与我方无关（逐条）

| 上游 4K 数字 | 能不能用在我们身上 |
|---|---|
| CC_SMALL_FP8 2216 → 806 MiB | ❌ 这是**上游运行时**的显存。构成它的三个机制：`h16_ok`/`switch_history`（16-bit 历史）、`--lazy-images`/`materialize`（输入拷贝与答案图像按需才实体化）、`make_noise`（噪声表延迟生成）。**我们的运行时这六个标记一个都没有**（§3.2 实测），所以我们的 4K 网络显存**不会**因此下降 |
| 1440p / 1080p 同理 | ❌ 同上 |
| 且这是 Linux + RX 9070 XT + network-only | ❌ 三重口径不符 |

### 5.2 那 1410 MiB 到底由什么构成【推算】

| 成分 | 4K 节省 | 依据 |
|---|---|---|
| 历史图像 32bit → 16bit | **134 MiB** | 上游注释原文 "4K: 2 x 66 MB instead of 2 x 133"（`nr_runtime.cpp:302-303`）；2×133 − 2×66 = 134 |
| 惰性图像 + 延迟噪声表 + 资源退休 | **约 1276 MiB** | 1410 − 134（1410 = 2216 − 806，上游 release notes） |
| **合计** | **1410 MiB** | |

**我们能拿的部分**：只有 16-bit 历史那一条的**前提**——我们的运行时**已经有** `NR_HIST16` 与
`temporal_post_fp32h16.spv` / `temporal_post_fp32h16_grade.spv`（在 `dlssnr-amd/aco/shaders/temporal/` 里，
实测在位），而上游 0.0.8.1 里 `NR_HIST16` 就是这个开关（`nr_runtime.cpp:283` 注释："research NR_HIST16=1"）。
**但我们没有上游 0.0.8.2 的 `h16_ok` 探测与 `switch_history` 运行时切换**——
我们的 `NR_HIST16` 只能在**进程启动时**通过环境变量设定一次，且改格式后历史会重来（上游 0.0.8.2 也是
"a switch is a cut"，即切换即历史清空）。

**结论**：上游的显存大改，**我们最多继承它 10% 的思路（16-bit 历史），且需要自己做**；
`h16_ok`/`switch_history`/`lazy-images`/`materialize`/`make_noise` 五项都得自己移植。
这是一个**明确的 v0.7 候选工程**，不是 v0.6 的行动项。

### 5.3 16 GB 卡的预算：约束没有解除，但也没有原来那么紧

我们**没有**我们这条链的 4K 网络显存实测值——仓库里只有：
- 抗闪烁 A/B/F 路线 **+271 MiB**（4K，我们 3840×2054 口径，`experiments-2026-11-03.md:238`）
- 线 1 槽环 3 槽 **+193 MB**（`experiments-2026-11-03.md:307`）

【推算】若暂借上游 0.0.8.1 口径的 4K network-only 2216 MiB 当我们的网络显存（**这是个占位假设，
我们没测过，必须在 9070 XT 上实测替换**）：

| 项 | MiB |
|---|---|
| 4K 网络（占位：上游 0.0.8.1 口径 network-only） | 2216 |
| 抗闪烁 A/B/F | +271 |
| 线 1 槽环（3 槽） | +193 |
| **小计** | **2680** |
| 占 16 GB（16384 MiB） | **16.4%** |

**这个账说明什么**：**显存从来不是 16 GB 卡的瓶颈**——即使按最保守的 2216 MiB 口径，
网络 + 抗闪烁 + 线 1 槽环三项加起来只占卡容量的 16.4%，剩下 83.6% 留给游戏与 Magpie 自身。
所以 `experiments-2026-11-03.md:364` 那条"显存预算：…能否在 16 GB 内共存"的疑问，
**答案是"能"，而且不需要上游的显存优化**。

**但要注意反向的一条**：真正稀缺的是**带宽与每帧 GPU 时间**，不是容量。
上游把历史从 32bit 降到 16bit，省的是 134 MiB 容量，**不省带宽**。
我们那条"16 GB 卡上两者叠加要重新做显存与带宽预算"（`:307`）里，**带宽那一半至今没算过**。

**明确纠正简报的一处前提**："'显存曾让某些路线不可行'这个约束是否解除"——
**本仓库从来没有记录过任何路线因显存而不可行**。查遍 `docs/`：
`experiments-2026-11-03.md:26` 记的是"线 2 失败（**ABI/性能**不可行）"，
`:307` 记的是"16 GB 卡上两者叠加要**重新做**显存与带宽预算"，`:364` 是同一条的待办。
**没有一处说"显存不够所以某路线不可行"。** 简报这句话把一条"待算的账"读成了"已否决的约束"。

---

## 6. 画质与"对齐"目标

### 6.1 上游给的 PSNR/SSIM 对照

**CC_SMALL_FP8（我们的默认网络）**，RX 9070 XT vs NVIDIA 原生 310.9.16 @ RTX 5090：

| 分辨率 | 单帧 PSNR | 单帧 SSIM | 动态序列 PSNR | 动态序列 SSIM |
|---|---|---|---|---|
| 1920×1080 | 49.15 dB | 0.9971 | 50.6–52.3 dB | 0.998 |
| 2560×1440 | 49.40 dB | 0.9969 | 51.2–52.4 dB | 0.998 |
| 3840×2160 | 50.59 dB | 0.9971 | 52.2–53.1 dB | 0.998 |

作为尺度参照：输入帧本身对 NVIDIA 输出只有 **28.88 / 28.21 / 30.09 dB**（SSIM 0.970–0.977）。
即 **NR 相对输入有 ~20 dB 的提升，而相对 NVIDIA 原生只差 ~1 dB**。
NVIDIA 的 DLL 每次运行输出逐字节一致，所以这些数字是实现差异，不是噪声。

### 6.2 这能不能变成我们的可复现验收？能——而且这是本次最有价值的一件东西

【实测】上游把**验证语料整个放进了仓库**（`docs/ngx-verification/`），我逐个下载验证可取：

| 文件 | 状态 |
|---|---|
| `310.9.16/single-frame-results.json` | ✅ 200（PSNR/SSIM/edits 全套数值） |
| `310.9.16/NGX-VERIFICATION.md` | ✅ 200（方法、设置、输入说明） |
| `310.9.16/single-frame-outputs/*_nvidia.png` | ✅ 200（NVIDIA 原生输出，逐像素） |
| `310.8.0/single-frame-inputs/1920x1080.png` | ✅ 200（输入帧，Tomb Raider 2013） |

**我们的"对齐"目标（roadmap 目标 2）目前只有一句"网络耗时 ≈ 上游离线单帧，差 < 0.2 ms"，
而且这条只在 1080 档验过（`measurements.md:72`：我们 3.40 ms vs 上游 3.27 ms）。
上游这份语料给了**第二个、也是更硬的维度：画面本身**。

**可执行的验收提案**（把"对齐"从口号变成可复现的数）：

1. 用上游的输入 PNG（`310.8.0/single-frame-inputs/1920x1080.png` 等）作为标准输入；
2. 用我们的链跑出输出 PNG；
3. 与上游发布的 `*_nvidia.png` 逐像素算 PSNR/SSIM（8-bit RGB，SSIM 用 11×11 高斯窗 σ=1.5，R/G/B 取均值
   —— 上游方法，`NGX-VERIFICATION.md` 里写明）；
4. **判据**：我们的 PSNR ≥ 上游实测值 − 1 dB（49.15 → ≥48.15 dB @1080p），SSIM ≥ 0.996。
   给 1 dB 余量是因为我们的运行时分支与上游不同（§3.2），不必逐字节一致，但**必须落在同一个量级**。

**我们的工具链已经具备大半**：`src/nrtest/main.cpp` 已经能建 D3D12 纹理、走
`PrepareFrame → RecordInputs → EnqueueHip → RecordOutputs` 并回读（`fi.reset = 1`）。
缺的只有"读 PNG / 写 PNG / 算 PSNR+SSIM"这一步。这是**纯增量、零风险**的工作。

**前提说明**：上游这份对照是**它自己那套实现的**（`docs/ngx-verification/310.9.16/NGX-VERIFICATION.md` 写明
"the Linux build of DLSSNR-AMD 0.0.6-linux-preview"），与我们分支不同；
所以**它给的是"及格线与量级参照"，不是"必须复现的精确值"**。

### 6.3 画质差异的诚实边界

- **我们与上游 v0.0.8.2 用的是同一份 NVIDIA 权重**（§2.2），所以"换上游会不会掉画质"这个问题
  在**权重层不成立**。
- 但**我们的运行时分支与上游不同**（`NR_KEEP_RGBA32F` 的存在、`lazy-images` 的缺失、
  `switch_history` 的缺失），所以"我们的输出与上游的输出会不会有可见差异"**是开放问题**，
  只能用 §6.2 的语料实测。这也是我们**应该做**那个验收的原因。

---

## 7. 我们要不要跟、怎么跟

### 7.1 面板冲突点逐条核对（简报要求）

| 上游 v0.0.8.2 新增/强调 | 我们的 ABI 里有没有 | 我们的运行时认不认 | 面板要不要做 |
|---|---|---|---|
| **`[Network] History Precision`（16/32）** | ❌ **没有**。`MochiNrApi.h` 的 `MochiNrFrameInfo` 无精度字段；`MochizukiNrControls.h` 里只有 `history_strength`（0..1，混合强度，**不是**精度） | ❌ 我们的运行时**不读**这个 ini 键（只读 `[Network] Motion Without Depth`）；但**有 `NR_HIST16` 环境变量** | **先不做成面板项**。见下 |
| **RenoDX `NRSdrBlackRestore`（0..2，2=Smooth）** | ✅ **有等价字段**：`MochizukiNrControls.h:163` `reno_sdr_black_restore` | ✅ 运行时认 `reno_*` 全套 | **要做**。我们场景 SDR + RenoDX 风格，正是上游点名的触发组合 |
| **RenoDX `NRCodecMode`（0..3，0=Classic）** | ✅ **有等价字段**：`MochizukiNrControls.h:165` `reno_codec_mode` | ✅ | **要做**，与 black restore 成对（上游的修法给的就是这两个二选一） |
| **`CC_SMALL_FP8_EXPERIMENTAL` 作为面板网络选项** | ✅ ABI 有 `MOCHI_NR_FRAME_FLAG_ORIGINAL_MODEL`（切 310.8 模型），但**没有** EXPERIMENTAL 对应的 flag | ❌ 运行时不认识 | **不做**。无运行时支持（§3.3） |
| **网络选择器（4 个网络下拉）** | ⚠️ 我们 ABI 有 `MOCHI_NR_FRAME_FLAG_ORIGINAL_MODEL`（= 310.8 模型 = CC_SILVER_AARDWOLD）与 `MOCHI_NR_FRAME_FLAG_INT4`（= int4 混精） | ✅ 都认 | **可以做一个 3 选 1**（CC_SMALL_FP8 / CC_SILVER_AARDWOLD / CC_SILVER_AARDWOLD_INT4_MIXED），**但先不急** |

**关于 History Precision 为什么"先不做成面板项"**：
上游那个开关是**运行时内部**的（`Runtime::set_history_f16`），**不是 NGX ABI 的一部分**——
上游自己的 `_nvngx.dll` 也没把它导出到 NGX 面上，它走的是**自己的 `dlssnr-amd.ini` + 自己的 NR Control UI**。
我们的 ABI 是自研的、面向"宿主效果参数 → 运行时"的映射（v0.6 清单第 1 项），
所以**要把它变成面板项，得先在我们的运行时里加一个对应的控制项**（我们的运行时已经有 `NR_HIST16`
这个环境变量开关，但**没有运行时切换能力**，也不是 ABI 字段）。
**结论：v0.6 不做这个面板项**；把它记为 v0.7 的"面板-运行时控制面补齐"清单里的一项，
与 §5.2 的 16-bit 历史移植一起做。

### 7.2 行动清单（按 v0.6 清单顺序插入）

#### P0 — v0.6 必做

| # | 行动 | 人时 | 验收判据 |
|---|---|---|---|
| **A1** | **文档勘误**：`docs/upstream.md` 全面改写。① GitHub 链接已 404，改指 GitLab `https://gitlab.com/mochizuki0323/DLSSNR-AMD`；② 明确写出"我们用的是 0.5.8-opennr 那条线，v0.0.8.2 是另一条产品线"；③ 补"我们的权重与 v0.0.8.2 逐字节相同（含哈希表）"这一事实；④ 补 int4 混精网络在本地**已具备但未接线**的事实 | **1.5** | `docs/upstream.md` 里 0 处指向 `github.com/mochizuki0323`；含一张三行哈希对照表；含"未接线"清单 |
| **A2** | **README 同样勘误**：`README.md:190` 的上游链接与描述同步 | **0.5** | 同上 |
| **A3** | **面板接线（v0.6 清单第 1 项）**照原计划做，**但按 §7.1 增补两项 RenoDX 旋钮**：`reno_sdr_black_restore`（SDR 暗部黑斑修复，映射 `NRSdrBlackRestore` 0..2）与 `reno_codec_mode`（映射 `NRCodecMode` 0..3）。宿主侧读 `_passSettings` 的既有结论不变 | **3**（在原计划上 +0.5） | ① 面板出现这两个旋钮；② 每个旋钮逐档改图，**SDR 暗部黑斑必须肉眼可见地消失/出现**（我们要拍 before/after 存档）；③ 其余旋钮按原判据 |
| **A4** | **发布说明不写"我们天然拥有每游戏帧一次 NR"**。改成中性表述，并在 v0.6 说明里留一句"FG 下的 NR 节奏待逐帧计数验证" | **0.5** | v0.6 说明里无此断言；`docs/roadmap.md` 目标 1 的"FG 2x 开启后无异常"仍标未实测 |
| **A5** | **导出名解析加固**：`passthrough.cpp:182` 现在只找 `LmxxfNrGetApi`。改成依次尝试 `MochiNrGetApi` → `LmxxfNrGetApi`，两个都失败才回落直通 | **0.5** | 头文件名字与二进制名字任一都能加载；shim 日志打出实际用的是哪个 |

#### P1 — v0.6 可做（做完 v0.6 再排）

| # | 行动 | 人时 | 验收判据 |
|---|---|---|---|
| **B1** | **PSNR/SSIM 可复现验收**（§6.2）：给 `src/nrtest/` 加"读 PNG → 跑网络 → 写 PNG"与 PSNR/SSIM 计算，对上游 `310.8.0/single-frame-inputs/` + `310.9.16/single-frame-outputs/*_nvidia.png` 出数 | **4** | 在我们的链上跑 1920×1080 单帧，PSNR ≥ 48.15 dB、SSIM ≥ 0.996，方法与上游一致（11×11 高斯 σ=1.5，R/G/B 均值） |
| **B2** | **网络选择器**（若 A3 已铺好 controls 通路）：三选一 CC_SMALL_FP8 / CC_SILVER_AARDWOLD / CC_SILVER_AARDWOLD_INT4_MIXED，对应 `MOCHI_NR_FRAME_FLAG_ORIGINAL_MODEL` / `MOCHI_NR_FRAME_FLAG_INT4` | **2** | 三档都能在游戏内切换出可见差异；int4 档的 4K 网络耗时按上游口径应 ≈19.64 ms（Linux 口径），我们 Windows 口径需自测 |
| **B3** | **实测我们自己的 4K 网络显存**（§5.3 的占位假设必须换成真值）。用 `nvidia-smi`/任务管理器/上游 `nr_vram_ledger` 同类手段，在 4K 100% 渲染率下读差值 | **1.5** | 出一个数（MiB），替换 §5.3 的 2216 占位值；并重算 `271 + 193 + 实测` 占 16 GB 的比例 |

#### P2 — 延后到 v0.7（明确不做进 v0.6）

| # | 行动 | 人时 | 理由 |
|---|---|---|---|
| **C1** | **移植上游 0.0.8.2 的显存机制**：`--lazy-images`/`materialize`、`make_noise`、`retired_pools/images/buffers`、`h16_ok`/`switch_history` | **15–25** | 需要读懂并改我们的运行时分支（与上游不是同一分支）。收益待 B3 实测后才知上限。**先出 B3 的数再决定做不做** |
| **C2** | **运行时切换 History Precision**（16↔32 热切） | 随 C1 | 我们运行时目前只有启动期 `NR_HIST16`；热切要加运行时控制项 + ABI 字段 |
| **C3** | **接 EXPERIMENTAL 网络** | 未知 | 需要一个认识 `CC_SMALL_FP8_EXPERIMENTAL` 的运行时分支；上游 0.0.8.2 的 Windows 包不给这种运行时。等我们的运行时分支自己长出这个能力 |
| **C4** | **重新抽取模型**（用上游 `model-tools/dlssnr_extract_model.exe` + `nvngx_dlssnr-310.9.16-50s.dll`） | **0.5** | 产出的字节与现有**完全相同**（§3.4）。只作为"我们能自证合规"的存档动作，不改任何文件 |

### 7.3 权重/网络选择要不要进面板？——进，但只有 3 个，不是 4 个

上游有 4 个网络，我们能进的只有 3 个：

| 上游网络 | 进面板？ | 理由 |
|---|---|---|
| CC_SMALL_FP8 | **默认** | 我们当前用的就是它（§2.2 逐字节相同） |
| CC_SILVER_AARDWOLD | ✅ 可进 | 权重已在手（`dlssnr.bin` 哈希一致），ABI 有 `MOCHI_NR_FRAME_FLAG_ORIGINAL_MODEL`，运行时认 |
| CC_SILVER_AARDWOLD_INT4_MIXED | ✅ 可进 | 权重 + 279 个标定表 + 75 个 int4 ACO 记录**全部在位**，ABI 有 `MOCHI_NR_FRAME_FLAG_INT4`，运行时认 `CC_SILVER_AARDWOLD_INT4_MIXED` 字符串。**但我们从未接线，也从未测过**——先测再进面板 |
| CC_SMALL_FP8_EXPERIMENTAL | ❌ | 运行时不认识（§3.3） |

**注意 int4 混精的两个前提**（上游 README）：RX 9000 独占；Windows 上**只有 ACO 机器码**可用，
驱动自编译的 int4 内核很慢（不可用时该选项置灰）。我们跑在 Windows + ACO
（`passthrough.cpp` 已传 `MOCHI_NR_CREATE_FLAG_ACO`），**前提满足**。

### 7.4 "每游戏帧一次 NR"该怎么表述

**不能宣称**。理由：我们 P2 链的 NR 跑在 Magpie 效果槽里、由**被捕获的帧**驱动，
"游戏帧"在 Magpie 这条链上没有现成定义；而我们旧链的计数证据（`3311/1650 = 2.00`）
恰恰说明**这类链路上 NR 会随 FG 倍率放大**。

**建议的 v0.6 表述**：不写这条断言；改为在 v0.6 说明里写
"NR 在 Magpie 的 DLSSNR 效果槽内按被捕获帧执行；在帧生成开启时的执行节奏待逐帧计数验证"，
并把它登记为 roadmap 目标 1 的一个子项。

**验证方法（廉价、复用现成工具）**：我们的 shim 日志已经有 `g_evalCount`（宿主调用次数）与
`g_nrFrames`（真跑网络的帧数）两个计数器（`passthrough.cpp`），在 `ReleaseFeature` 时打印。
**只要在 v0.6 的 FG 回归会话里，比对"NR 帧数 / 宿主调用次数"与"游戏帧数"的关系**，
就能判定 NR 是否随 FG 倍率放大。成本 ≈ 0（只是多读一行日志）。

---

## 8. 风险与不做的事

### 8.1 供应链

| 风险 | 事实 | 缓解 |
|---|---|---|
| **上游 GitHub 账号已封** | 【实测】`github.com/mochizuki0323` → **404** | 全部改指 GitLab（A1/A2） |
| **GitLab 这条线也可能再消失** | 作者原话："I just hope this account and repository can stay alive longer than they have"；项目仅 10 star / 2 fork | 我们**不依赖**它的二进制（§3），只把它当**文档与语料源**；语料（PSNR 那套）**现在下载并本地存档** |
| **我们真正依赖的运行时来自第三方 HF 镜像** | 我们的 `MochizukiNrRuntime.dll` 来自 HF `zmodelerlover/amd-nr` 的 `mochizuki/0.5.8-opennr/`（**不是** mochizuki 作者直供）；【实测】本机当前访问 HF API **HTTP 000（不可达）** | ① **立刻把运行时 + `dlssnr-amd/` 资产 + 哈希清单本地化并纳入版本控制/备份**（427 MiB，主要是权重）；② `docs/upstream.md` 记录确切来源与哈希；③ 记下"我们这份运行时的来源是第三方镜像"这一事实 |
| **`dlssnr-amd.ini` 每次安装被重置** | 上游 release notes 明写 | 我们不依赖上游的 ini（我们的运行时只读 `[Network] Motion Without Depth`），无影响 |
| **我们打包时会带上 NVIDIA 权重** | 【实测】`scripts/pack-release.py` 的 `DIRS = ["dlssnr-amd", ...]` 会把 `dlssnr-amd/` 整个收进 zip，含 `dlssnr-small.bin` / `dlssnr.bin` / `dlssnr-int4.bin` 共 **300.3 MiB** NVIDIA 权重 | **这是我们自己的分发问题**，与上游无关，但必须处理（见 §8.2） |

### 8.2 许可：权重不可分发

- 上游代码 **MIT**；上游 README 反复强调 **"The model is not included"**、"nothing from NVIDIA is in this repository"，
  权重一律由用户自己的 `nvngx_dlssnr.dll` 抽取。
- **NVIDIA 的权重不是我们的，不能分发。** 我们仓库本身不含权重（`docs/upstream.md` 已如实记录），
  但 `scripts/pack-release.py` 会把含权重的 `dlssnr-amd/` 打进发布 zip（实测 300.3 MiB）。
  **建议 v0.6 修**：打包时排除 `dlssnr-amd/*.bin`（三个 .bin），并在包内放一份
  `model-tools/dlssnr_extract_model.exe` + 说明，让用户从自己的 DLL 抽取——
  **上游 0.0.8.2 的 `model-tools/` 目录正好提供这个工具**（`windows/package/model-tools/dlssnr_extract_model.cpp`，
  MIT），且我们本地就有一份哈希精确匹配的 `nvngx_dlssnr-310.9.16-50s.dll` 可供用户使用。
  这正好把我们的分发模式**转成上游的合规模式**。

### 8.3 自研变体的画质差异

- **EXPERIMENTAL**：上游明说它与原网络输出**不同**（"its output will be different"），
  是拟合权重。**我们不能拿它去承诺"对齐 NVIDIA"**。
- **INT4_MIXED**：上游明说"the picture slightly different"，且它的权重是**对 1112 帧游戏画面标定**的表生成的
  ——**针对特定内容标定的网络**，不是通用加速。
- **对我们最直接的影响**：如果为了省那 ~1.6 ms（4K）而切到 INT4_MIXED 或 EXPERIMENTAL，
  我们就**放弃"对齐 NVIDIA 原生"这个可测目标**（§6.2 的 PSNR 验收会退）。
  **建议：v0.6 一律用 CC_SMALL_FP8**，把它作为默认且唯一的"对齐"基准网络。

### 8.4 明确不做的事

| 不做 | 理由 |
|---|---|
| **不换上游运行时** | 上游不提供我们 ABI 的二进制；`_nvngx.dll` 是 OptiScaler-NR 专用、资源所有权模型不同、且"游戏帧"语义要重新定义（§3.1、§7.4） |
| **不把 EXPERIMENTAL 放进面板** | 我们的运行时不认识它（§3.3） |
| **不做 History Precision 面板项（v0.6）** | ABI 无对应控制项，运行时只有启动期环境变量，无热切能力；与 C1/C2 一起做（§7.1、§7.2） |
| **不因上游的显存数字调整我们的路线优先级** | 那些数字来自我们没有的机制；且我们的显存账本来就宽裕（16.4%）（§5.1、§5.3） |
| **不重抽模型** | 产出字节相同（§3.4） |
| **不宣称"每游戏帧一次 NR"** | 未验证；旧链证据相反（§7.4） |

---

## 9. 仍不清楚的地方

| # | 不清楚 | 为什么重要 | 怎么查 |
|---|---|---|---|
| 1 | **我们这份运行时到底对应上游哪个 commit？** 它有 `NR_KEEP_RGBA32F`（上游 0.0.8.1/0.0.8.2 都没有）、有 `NR_HIST16`（0.0.8.1 起）、无 `CC_SMALL_FP8_EXPERIMENTAL`（0.0.8 起）。这不是上游任何公开 tag | 决定我们能否"往上游靠"：如果只是差几个 commit，移植 §5.2 的机制就便宜；如果是一条独立分支，就是重写 | 我们的运行时来自第三方 HF 镜像（不是 mochizuki 作者直供），**可能带作者的私有改动**。查法：拿运行时里的版本字符串/`.text` 里的构建路径字符串；或直接问 zmodelerlover 该镜像的来源 |
| 2 | 我们这份运行时在 `NR_HIST16=1` 下**实际省多少显存**、画质有无变化 | 决定 §5.2 里那 134 MiB 我们能拿多少 | 9070 XT 上：4K 100% 渲染率，两次跑（env 设/不设），读显存差值 + 截图对比 |
| 3 | 我们这条链的 **4K 网络显存实测值** | §5.3 的 2216 MiB 是**上游口径的占位值**，我们的账建在别人的数上 | 9070 XT，4K，读差值（B3） |
| 4 | **我们的 P2 链在 FG 开启时 NR 跑几次** | 决定 v0.6 说明能不能提"每游戏帧一次"（§7.4） | 现有 shim 日志已够：`g_evalCount` vs `g_nrFrames` vs 游戏帧数 |
| 5 | 我们本地四份 NVIDIA DLL 里，**`nvngx_dlssnr.dll`（标称 310.8.0）**是哪来的？上游 310.8.0 的白名单哈希是 `e16bcf15…`，我们这份是 `984bee0f…`，**不匹配** | 若有人需要 310.8.0 模型（int4 混精的前置），我们缺一份上游认可的 DLL | 查该 DLL 的来源；或从 NVIDIA 官方渠道重新取得 310.8.0 |
| 6 | **HF 数据集当前不可达**（本机 HTTP 000） | 我们运行时的来源地；不可达 = 将来取不到更新 | 从能上网的机器确认 `zmodelerlover/amd-nr` 是否还在、是否有更新的 `mochizuki/0.5.x` |
| 7 | 上游 v0.0.8.2 的 **Windows 包是否可直接在 Magpie 里用** | 若 `_nvngx.dll` 能塞进我们的 `amdgx_dlssnr.dll` 位置，就等于免费拿到 0.0.8.2 的全部修复（含 1410 MiB 显存） | 需要它的 Windows 构建产物 + 它的 `dlssnr-amd/` 资产树；先做可行性纸面评估（§3.1 的三条障碍是否真的挡住） |

---

## 10. 出处

**上游 v0.0.8.2**（GitLab `mochizuki0323/DLSSNR-AMD`，tag `v0.0.8.2`，commit `c53878b5`）
- release notes（`/-/releases`）、`README.md`、`docs/ARCHITECTURE.md`
- `windows/src/core/nr_runtime.cpp` / `.hpp`（`set_history_f16`、`h16_ok`/`switch_history`、
  `materialize`、`make_noise`、`retired_*`）、`windows/src/core/nr_renodx_keys.hpp`
  （`NRSdrBlackRestore` / `NRCodecMode` 键表）、`windows/src/pe/nr_ngx_core.cpp`（NGX 导出与参数读取）、
  `windows/src/pe/nr_ngx_abi.hpp`、`windows/src/ui/nr_control_pages.hpp`（History precision UI）
- `windows/package/install.ps1`（安装布局、两个 SHA256 常量）、`windows/package/model-tools/dlssnr_extract_model.cpp`
  （DLL 白名单）、`linux/build/experimental_weights.py`（EXPERIMENTAL 权重生成）
- `docs/ngx-verification/310.9.16/` 与 `310.8.0/`（PSNR 语料）
- `linux/src/core/nr_runtime.cpp` @ `v0.0.8.1`（用于 diff 定位 0.0.8.2 新增机制）

**本仓库**：`docs/2026-11-04-P2-milestone.md`（P2 实测 51–52 fps）、`docs/2026-11-04-line1-recompute.md`
（帧时间模型、三档表、§5.3/§5.4 门槛）、`docs/measurements.md`（nrbench 3.40/13.80 ms、
FG 2x 计数 3311/1650）、`docs/experiments-2026-11-03.md`（抗闪烁 +271 MiB、槽环 +193 MB）、
`docs/upstream.md`（来源与许可）、`src/nrtest/MochiNrApi.h`（ABI）、
`src/mochinr/MochizukiNrControls.h`（`reno_sdr_black_restore` / `reno_codec_mode`）、
`src/nvngx-shim/passthrough.cpp`（shim 现状、`LmxxfNrGetApi` 解析、计数器）、
`scripts/pack-release.py`（打包范围）、`scripts/fetch-upstream.ps1`（取源脚本，注意它拉的是 0.5.9）

**本机实测**：上游源码包全量 grep 与逐文件 SHA256 比对；`MochizukiNrRuntime.dll` 字符串与导出扫描；
两个运行时副本的 SHA256；NVIDIA 权重三份与上游常量的哈希比对；`experimental_weights.py apply` 复现 EXPERIMENTAL 哈希；
本地四份 `nvngx_dlssnr*.dll` 的 SHA256 与上游白名单比对；`pack-release.py` 打包范围模拟。

---

*本文不含新的实机帧率/显存测量。所有【推算】处已给算式；所有依赖未测量量的结论均标为条件式。
上游 v0.0.8.2 的数字一律标其原始口径（Linux / RX 9070 XT / network-only），不与我们的数字混用。*

---

## 9. 增补：与已落地的 v0.6（`526ba7a`）的关系

本文写作时 v0.6 的面板接线提交尚未并入，事后核对如下（**只列事实，不改上文任何结论**）：

| 本文行动项 | v0.6 落地情况（`526ba7a`，2026-11-05） | 状态 |
|---|---|---|
| ② 面板增补 `reno_sdr_black_restore` / `reno_codec_mode` | `MochizukiNrControls.h` 已有这两个字段，但 `src/` 下**没有任何 .cpp 引用它们** → 仍未接线 | **仍待做** |
| ④ 导出名解析加固（头文件 `MochiNrGetApi` vs 二进制 `LmxxfNrGetApi`） | `src/nvngx-shim/passthrough.cpp:451` 仍只解析 `LmxxfNrGetApi`；v0.6 已新增 ABI 探针与离屏自测 → 双名回退的实现成本已降低 | **仍待做**（更便宜了） |
| ⑤ 打包排除 `dlssnr-amd/*.bin` | 未见相关改动；部署目录 `D:/Magpie-NR-046beta/dlssnr-amd/` 内三个权重合计 314,884,800 字节（≈300.3 MiB） | **仍待做** |
| ③ 发布说明不写"我们天然拥有每游戏帧一次 NR" | v0.6 发行 README 草稿已存在（`docs/RELEASE-README-v0.6.draft.md`），需按本文核对措辞 | **待核** |

另注：v0.6 已决定**关闭时域、不用抗闪烁**，以收紧参数区间替代。因此上游 v0.0.8.2 的
History precision（16/32）与抗闪烁相关的显存机制，对我们是**更低优先级**——我们本来就不走自研时域路线，
本文 P2（移植显存机制 + History Precision 热切）的收益评估需按此重新排序。
