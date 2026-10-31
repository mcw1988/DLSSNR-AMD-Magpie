# 钩子需求研究：为什么两条运行时要用不同的效果器

> 2026-10-31。本项目的 `AMDNR-ACO` / `AMDNR-HIP` 纯 HLSL 钩子（`src/hooks/`）不是"更好的
> SR 效果器"，而是**给 ReShade add-on 的挂点**。本文解释为什么 mochizuki 可以用纯 HLSL 钩子、
> lmxxf 必须用带原生后端的桩，以及由此得出的双 addon 共存方案。

## 1. 两条运行时的挂载位置完全不同

```
Magpie 效果链:  捕获 → [效果器 1] → ... → 补帧 → present
                                                    ↑
                                      OpenNR (mochizuki) 挂在这里
                                      事后处理，看的是最终帧

Magpie 效果链:  捕获 → [效果器 X]        ...
                            ↑
                  dlss5-amd (lmxxf) 挂在这里
                  拦截效果器内部对 SR SDK 的调用
```

- **OpenNR（mochizuki 宿主）**挂在 ReShade 的 `present` 事件上。它不关心效果链里跑什么，
  只要在帧呈现前拿到画面即可。效果器只是"占位"。
- **dlss5-amd（lmxxf 宿主）**挂在**原生超分 SDK 的导出函数**上。二进制里写明了拦截目标：

  ```
  amd_fidelityfx_dx12.dll  ->  ffxDispatch, ffxDestroyContext
  libxess.dll              ->  xessD3D12Execute, xess_begin / xess_end
  DLSS5_PRE_UPSCALE=auto|0|xess|ffx     ← 选择拦截哪家
  ```

  也就是说它**等上游调用 FSR/XeSS 的 SDK 函数时截胡**，然后换自己的网络跑。

## 2. 推论：两种钩子不能互换

| 钩子类型 | 例子 | mochizuki (OpenNR) | lmxxf (dlss5-amd) |
|---|---|---|---|
| 纯 HLSL（一次采样，无 SDK 调用） | `AMDNR-ACO` / `AMDNR-HIP` | ✅ 正好，无多余开销 | ❌ 无目标可钩，插件空转 |
| 原生后端桩（Magpie 用 SDK 代码替换该 pass） | `XeSS_SR` / `FSR3_SR` / `FSR4_SR` | ⚠️ 能跑，但每帧白付一次 SR 推理 | ✅ 必需，才有 SDK 调用可拦 |

实测印证：把 `AMDNR-HIP` 挂到 NR-lmxxf 组后"钩子没勾住 lmxxf 的 addon"——因为纯 HLSL
从不调用 `ffxDispatch`，dlss5-amd 什么都看不到。换回 `FSR4\FSR4_SR` 立即恢复。

## 3. 五个 SR 桩逐行等价（重要）

`DLSS_SR` / `FSR2_SR` / `FSR3_SR` / `FSR4_SR` / `XeSS_SR` 的 HLSL 主体是同一行：

```hlsl
// Captured-colour SR adapter. Native SDK code replaces this pass.
MF4 Pass1(float2 pos) { return INPUT.SampleLevel(sam, pos, 0); }
```

同为 `//!FILTER LINEAR`、同一句头注释。**标签不同，HLSL 行为完全相同**。
真正让它们跑出不同结果的是头注释里那句 *"Native SDK code replaces this pass"*：
Magpie 会调用厂商 SDK（`libxess.dll`、`amd_fidelityfx_loader_dx12.dll` 等）替换掉这行采样。

- 这就是为什么"给 XeSS_SR 丢权重文件"没用：权重由厂商 SDK 管理，HLSL 只是占位。
- 也是为什么 `XeSS_SR` 在 mochizuki 组里又慢又糊：它每帧真跑一次 XeSS 后端。

## 4. XeSS 后端在 9070XT 上的真实状态（实测）

用户质疑"XeSS_SR 介绍写需要 Intel ARC，9070XT 应该跑不了"。**目录原文**是
"需要受支持的渲染 GPU 与配套 XeSS 运行库"（XeSSFG 条目才写"非 Intel 显卡的 3×／4× 使用
本项目实验兼容路径"）。**日志给出了实证**：

```
magpie.1.log:578  XeSSUpscaler.cpp:483 | Initialize |
  XeSS experimental D3D11/D3D12 backend initialized
  (quality 106, Zero-MV, BGRA conversion=true, color=LDR UNORM):
  3840x2054 -> 3840x2054
```

（同一行在 18:35:50 与 19:08:33 两次出现，对应 88 fps 测试时段。换成 `AMDNR-ACO` 后
当前日志中该行计数为 **0**。）

结论分两层：

1. **"跑不了"不成立**：Magpie 带一条 *experimental* D3D11/D3D12 兼容后端，在 9070XT 上
   确实初始化成功了。88→105-110 fps 的提升因此可以归因：去掉了一次真实的 XeSS 推理
   （1:1、Zero-MV、还带 BGRA 转换），约 1.6-1.8 ms/帧。
2. **"是否真正有效"保留为疑问**：日志证明它*跑了*，没证明它*有用*。
   `3840x2054 -> 3840x2054` 是 1:1，Zero-MV（无运动矢量）、实验路径——这更像时序平滑
   而非超分。这正是后续方向 3 要回答的问题（mochizuki NR 之后再挂 XeSS_SR 量画质）。

## 5. 双 addon 共存（去掉 nr-router 切换的依赖）

既然 OpenNR 不需要 SDK 调用、dlss5-amd 没有 SDK 调用就自然空转，两个 addon 可以**同时常驻**：

```
NR-mochizuki 组: AMDNR-ACO（纯 HLSL）
  → 没有 SDK 调用 → dlss5-amd 天然空转
  → OpenNR 运行（组判定: OURS）

NR-lmxxf 组: FSR4_SR（原生桩）
  → dlss5-amd 钩住 ffxDispatch → 跑 lmxxf 网络
  → OpenNR 自动停机（组判定: stands down for another backend）
```

OpenNR 侧由 `GroupWantsThisBackend()`（本仓库补丁，见 `src/opennr-patches/`）每秒读
`config\v4e\config.json`，按组名（含 `mochi` / `lmxxf` / `dlss5`）或钩子名
（`AMDNR-ACO`/`XeSS_SR` 归 OpenNR，`AMDNR-HIP`/`FSR3_SR`/`FSR4_SR` 归 lmxxf）自动启停。

**实测结果**：两 addon 同时注册成功，mochizuki 组 107-108 fps、lmxxf 组 78-80 fps，
切换即生效。`nr-router.cmd` 降级为"重启 Magpie"的辅助工具（OpenNR 收到设备销毁后不自愈，
换组时仍建议重启一次），不再承担文件切换。

## 6. 设计约定（后续新钩子请遵守）

| 约定 | 理由 |
|---|---|
| 钩子按后端命名：`AMDNR-ACO`（mochizuki/ACO）、`AMDNR-HIP`（lmxxf/HIP） | 组判定按名识别，中文组名时的回退依据 |
| 钩子**不声明任何光流参数** | 声明 `opticalFlowMethod` 就会让 Magpie 挂上光流链路 |
| 默认 `Filter=Point` | 避免 LINEAR 重采样糊掉网络刚算出的细节 |
| `scalingType=3`（填充） | 与实测更锐利的那组一致 |
| 光流参数挂在**补帧效果器**上，不挂钩子 | 补帧才需要运动场 |
