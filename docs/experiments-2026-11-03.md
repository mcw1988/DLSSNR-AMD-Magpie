# 2026-11-03 三条实验线：推论、方法与实机顺序

> 本文把当天分头推演的三条线并成一份可执行清单：**单槽拆解**、**重编 `amdgx_dlssnr.dll`**、**抗闪烁**。
> 陈述沿用全项目口径：【源码】＝读出来的；【实测】＝已有归档数据；【待测】＝推测或未验证。
> 三条线共用一个事实基础：本机**没有 9070 XT、也没有跑过任何 Magpie 帧**，全部结论都等实机验证。

---

## 0. 一句话状态

| # | 线 | 一句话 | 当前状态 | 卡在哪 |
|---|---|---|---|---|
| 1 | 单槽拆解 | `FSR3Upscaler` 单槽串行把「D3D11 工作」和「NR 岛」相加；加多槽 + 输出晚一帧可望改成取大 | 设计定稿、两个补丁就绪、模型与脚本可跑 | **未编译、未实测**；收益前提（GPU 有空闲）未证明 |
| 2 | `amdgx_dlssnr.dll` | 宿主按名从应用目录加载 `nvngx_dlssnr.dll`，只查导出不验签 → 同名顶替即可把我们的运行时接进 `DLSSNR_AI_Filter` 槽位 | P0 探针桩已编译并自测通过（本机） | 探针**在 9070 上还没跑过**；ABI 细节待日志确认 |
| 3 | 抗闪烁 | 上游稳定的是「总残差 R = XM − X0」而不是模型；A/B/F/G 四条路线全在宿主侧，与运行时无关 | 机制与判据已拆清（引分析文档） | 本项目**从未接线**：`fi.motion`/`TEMPORAL`/`history_strength` 全空 |

**三条线不是并列的**——线 2 成功会同时改写线 1 的收益前提、并顺手把线 3 的大半变成"免费"。这是本文 §4 的重点。

```
                    ┌──────────────────────────────┐
   线 2 成功 ────────► NR 进 DLSSNR 槽（1:1，独立后端）│
                    │  ├─► 线 3：抗闪烁在宿主侧现成可用（A/B/F/G 对任何 raw 都成立）
                    │  ├─► 线 3：mvec/depth 由宿主提供，不必赌 kMotion 是否为空
                    │  └─► 线 1：FSR3Upscaler 岛内只剩 FSR4 放大 → 串行可能不再是瓶颈
                    └──────────────────────────────┘
   线 2 失败（ABI/性能不可行）─────► 线 3 需自研外部时域 pass；线 1 维持原判断
```

---

# 线 1：单槽拆解

## 1.1 问题与已知

现状每帧在 `FSR3Upscaler` 里串了三个点（**三个，不是一个也不是两个**）：

| # | 串行点 | 位置 | 性质 |
|---|---|---|---|
| 1 | CPU 侧等上一帧 | `FSR3Upscaler.cpp:497` | `WaitForFence(lastSubmittedValue)` |
| 2 | D3D11 GPU 侧等岛 | `FSR3Upscaler.cpp:577` | 立即上下文 FIFO，把本帧之后所有 D3D11 工作挡在岛后 |
| 3 | 发布处排空 | `Renderer.cpp:4614-4624` | 每帧无条件 `Signal → SetEventOnCompletion → Flush → wait`，**在宿主里、不在补丁范围** |

【源码】三点的存在都有行号；【实测】"求和"关系由五点吻合：
`frame ≈ 6.7（捕获/呈现）+ 1.4（FSR4 槽位）+ 13.7·s²（NR 岛）`。

| 配置 | 实测 | 求和模型 |
|---|---|---|
| B1（NR 停用） | 150 fps / 6.67 ms | 6.7 |
| B2（NR 全开但短） | 123–125 fps / 8.0–8.1 ms | 8.1 |
| NR 全开 s=1.0 | 47 fps / 21.5 ms | 21.8 |
| NR 全开 s=0.5 | 90 fps / 11.3 ms | 11.5 |
| NR 全开 s=0.35 | 105 fps / 9.5 ms | 9.8 |

## 1.2 推论（三条，都有反直觉成分）

1. **阶段 1（只加 2–3 槽环形缓冲、输出语义不变）的收益是 0，不是 0.1–0.5 ms。**
   因为串行点 3 每帧已经把 D3D11 队列排空，串行点 1 在稳态下本就在空转。
   阶段 1 的价值只是解除 allocator/纹理复用依赖，是阶段 2 的前置。
   → 这条是**可证伪断言**：阶段 1 上线后帧数应落在噪声内；掉 ≥3 fps 才算失败。
2. **要拿到台阶必须让输出晚一帧（阶段 2）。** 帧时间从求和变成 `max(8.0, 1.4+13.7·s²)`：
   0.5 档 → 123–125 fps（即 B2 天花板，网络开销被完全藏起）、1.0 档 → 66–70 fps。
3. **收益前提未证明**：同一个求和式既能解释"延迟串行"，也能解释"GPU 吞吐已满"。
   现有五点数据**无法区分**这两种世界 → 必须先用 G0 判别。

## 1.3 实验方法（顺序不可换）

**G0 判别（零代码，先做）**：量 `ratio = 每帧引擎忙秒数 ÷ frame_time`。

```powershell
scripts\measure-gpu-busy.ps1 -SourceDir <部署目录> -Seconds 30 -OutFile gpu-busy.csv
```

| ratio | 结论 |
|---|---|
| > 0.85 | 停。求和是吞吐的必然，"取大"天花板不存在，主线转缩岛 |
| 0.60–0.85 | 台阶按 `(1−ratio)` 打折，验收线同步下调 |
| < 0.60 | 台阶成立，按 123–125 / 66–70 验收 |

脚本对"Magpie 开着但没在缩放"会给**无效样本**并退出 1（不产生假结论）。

**阶段 1**：`apply-magpie-patch.ps1 -Stage 1` → 编宿主 → `MAGPIE_FSR_SLOTS=3, OUTPUT_LAG=0` → 期望**性能不变**。

**阶段 2**：`-Stage 2` → `MAGPIE_FSR_OUTPUT_LAG=1`，**先把 `mochinr.ini` 的 `inflight` 从 2 改成 4**（3 槽 + 晚一帧需要 4 帧在飞，否则运行时的阻塞门会吃掉收益）。

**采集与判定**：

```powershell
scripts\measure-fsr-ring.ps1 -LogPath logs\magpie.log -Config lag1-s05 -Scale 0.5 -OutCsv slot-test.csv
scripts\measure-fsr-ring.ps1 -OutCsv slot-test.csv -Baseline slot-test.csv -Summary   # 有失败项退出 1
```

## 1.4 判据

| 项 | 通过 |
|---|---|
| 阶段 1 帧数 | 与基线差 ≤ ±3 fps |
| 阶段 2 · 0.5 档 | ≥ 123 fps（主验证点，B2 天花板） |
| 阶段 2 · 1.0 档 | ≥ 66 fps（保守线；66–70 正常，>70 超预期） |
| `harvestMiss / draws` | < 5% |
| `framesDropped` | 不持续增长（偶发 1–2 次属可恢复失败） |
| 画面 | 无闪烁/色偏/拖影；停流无黑帧 |

## 1.5 关键实现约束（已写进设计稿，容易漏）

- **F13**：认领槽的等待加 4000 ms 上界；**超时直接 poison，不走重对齐**——`WaitForQueue` 内部是无限等待，超时后再排空会把"报错"变回"卡死"。
- **F7 三段式**：单帧可恢复失败**不** poison（宿主契约是"失败一帧 → marker pass → 下帧重试"）；只有 `GetDeviceRemovedReason() != S_OK` 或重对齐失败才 poison；`slot.islandValue` 只在 `Signal` 成功后写入。
- **F16**：失败一帧 ≠ 会话失效（否则 `nr == false` 时宿主会逐帧刷日志，120 行/秒）。

## 1.6 产物

| 文件 | 内容 |
|---|---|
| `docs/multislot.md` | 设计定稿（I1–I3 围栏不变量、F1–F16、§5 配置面、§6 验收、§7 拆分） |
| `src/magpie-patches/0001-fsr3-slot-ring.patch` | 阶段 1（`+314/−66`），基准 `6940f84`，只动 `FSR3Upscaler.cpp/.h` |
| `src/magpie-patches/0002-fsr3-output-lag.patch` | 阶段 2（`+87/−16`） |
| `scripts/apply-magpie-patch.ps1` | 基准校验 + 干跑 + 实打；漂移时兜底并**非零退出** |
| `scripts/measure-gpu-busy.ps1` | G0 判别 |
| `scripts/measure-fsr-ring.ps1` | 采集与汇总（无 publication 行时拒绝出表） |
| `tools/slot_model.py` + `test_slot_model.py` | 三种调度纸面模型 + 15 项回归 |

---

# 线 2：重编 `amdgx_dlssnr.dll`

## 2.1 问题

`DLSSNR_AI_Filter` 在 9070 上不可用。能不能不经过 ReShade、直接把我们的运行时接进去？

## 2.2 已知（全部【源码】或实物核对）

| 事实 | 出处 |
|---|---|
| 该效果器是「原生后端 + 空壳 hlsl」：`effects\DLSSNR\DLSSNR_AI_Filter.hlsl` 只提供参数表与槽位，处理在 `DLSSNRFilter.cpp` | 0.6.11 实物 + 源码 |
| 从**应用目录**加载 `nvngx_dlssnr.dll`，只查「存在 + 导出齐全」，**无签名校验** | `DLSSNRFilter.cpp:1435-1470` |
| Magpie 实际只解析 5 个导出：`Init_Ext / CreateFeature / EvaluateFeature / ReleaseFeature / Shutdown1` | 同上 |
| 参数对象由 DLL 侧分配，Magpie 只往上 `Set` | `NgxD3D12Core.cpp:40-52` |
| **必须静态导入 `GetModuleFileNameW`**（宿主要挂钩做"调用方兼容"） | `DLSSNRFilter.cpp:834-841` |
| **有 NVIDIA 适配器门**：非 0x10DE 时 `Initialize` 被短路，失败原因 `UnsupportedAdapter` | `NativeEffectBackendFactory.cpp:145-147` |
| 权重**内嵌在 DLL 里**：158–263 MB、部署目录无任何外部 `.bin/.onnx/.engine` | 实物核对 |
| 参数走自定义命名空间 `DLSSNR.*`，约 35 个键（`Color/Output/MVec/Depth/Style/Intensity/...`） | `DLSSNRFilter.cpp:109-140` |
| 运动矢量/深度**不在 DLL 里**，由宿主的 AMDOF/FFX 光流提供 | 同上 |
| 倍率恒为 1:1（`DLSSNR.Upscaling=0`），放大要靠链里的 `FSR4_SR` | `:1391-1396` |

**关键推论**：因为门在工厂里而不是在 DLL 里，**换 DLL 本身不足以在 AMD 上跑起来**——必须同时处理这个门。这决定了下面两个架构选项。

## 2.3 两个架构选项

|  | **A 独立效果** `DLSSNR\DLSSNR_AMD` | **B 同效果按厂商选 DLL** |
|---|---|---|
| 改动 | 新 hlsl + 工厂分支 + 把 `ngxCore` 依赖抽象成接口（5 处调用点）+ 参数化 DLL 名 | `DLSSNRFilter.cpp:1435` 按 VendorId 选文件名；去掉 `:145` 的 `nonNvidia` 短路 |
| 互斥 | 需要（有现成先例：补帧效果的 `CanAddEffect`/`EffectAddProblem` + `ScalingService:465` 运行期校验，约 40 行） | **天然不需要**（只有一个效果） |
| 主要代价 | `"DLSSNR_AI_Filter"` 这个字符串在 **17 个文件**里被当语义判定（`Renderer.cpp` 独占 8 处），新名字要逐条过一遍 | 面板是 NVIDIA 语义，7 个 `DLSSNR.Style/Intensity/...` 必须在 shim 里映射或标"不适用" |
| 收益 | 面板可重写成我们自己的旋钮；两实现可切换、可 A/B | 最快看到画面；面板就是现有那套（= 最初"能正常使用它现有的面板"） |

**建议**：先 B 跑通（P1/P2），确认槽位成立、值得投入后再决定是否为"自己的面板"付那 17 处的钱。

## 2.4 实验方法

| 阶段 | 做什么 | 判据 |
|---|---|---|
| **P0 探针** | 一个只写日志的 `nvngx_dlssnr.dll`：13 个导出 + 参数对象（32 个"不懂"槽位，布局从日志反推） | 日志出现 `Init_Ext` → `AllocateParameters` → 一串 `PARAM: slot=N name="DLSSNR.*"` → 每帧 `EvaluateFeature` |
| **P1 直通** | `EvaluateFeature` 里把 `DLSSNR.Color` 拷到 `DLSSNR.Output` | 画面正常；面板/多趟/抗闪烁都还能用 → **槽位成立** |
| **P2 接运行时** | 接 `MochizukiNrRuntime`（`Create(device,queue,assets)` + `PrepareFrame/RecordInputs/EnqueueHip/RecordOutputs/Retire`，`src/nrtest`、`src/mochinr` 已有现成代码） | 与 ReShade 方案同画质/同帧时间；不再需要 ReShade |

**P0 的落地形态**：`src/nvngx-shim/`（`probe.cpp` / `probe.def` / `selftest.cpp` / `build.ps1`），本机已编译并自测通过——
13/13 导出、`GetModuleFileNameW` 导入存在、用 Magpie 的调用顺序驱动一遍全部返回 `0x1`、日志能落到槽位
（`Width`→slot 5、`Color`→slot 1、`Style`→slot 6；`float` 走 XMM 所以 `arg` 是垃圾值，属预期）。

**P0 有两个已知前置**：
1. 9070 上必须**先绕过 `UnsupportedAdapter` 门**（P0 挂不到现有效果上）。所以 B 路线的两处改动要先做，否则探针不会被加载。
2. 探针不写输出纹理 → **画面会黑/花**，属预期；日志在 `Init_Ext` 时已落盘。

## 2.5 待测清单（P0 日志回来才能定）

- 成功码是不是 `0x1`（若不是，用 `NGXSHIM_RESULT=0x…` 换值重试，不必重编）；
- `CreateFeature` 的 feature 号是 18 还是别的；
- 35 个 `DLSSNR.*` 键各自落到哪个槽位（→ 决定 shim 的实现工作量）；
- 每帧 `EvaluateFeature` 次数是否等于 `multiPass`；
- 资源格式与 state（直通拷贝要不要加 barrier）。

---

# 线 3：抗闪烁

> 机制部分引自《DLSSNR-抗闪烁机制与借鉴分析》（WorkBuddy 文档），本文只做与两条线对齐的整理。

## 3.1 上游到底在稳定什么

**稳定的是「总修正」，不是模型。** 上游不改 DLSSNR 模型，也不把稳定结果反馈回模型：

```
X0 = 效果器原始输入
XM = 多层 DLSSNR 最终输出
R  = XM − X0            // 有符号、全分辨率、浮点
H  = Stabilize(R)       // 时域滤波
O  = X0 + H             // 输出
```

代码证据：`DLSSNRMultiPass.h:61-63/:90/:108`——NR 链先写 `_rawOutput`，结束后再跑 `_temporal->Draw()`。
所以**首层和后层的修正一起被稳定**，不存在"只稳定后续 pass"的特例；代价是首层本应快速出现的修正也被延迟。

## 3.2 四条路线（同一骨架的不同增强）

| 路线 | `antiFlicker` | 解决什么 | 主要副作用 |
|---|---|---|---|
| A 静态累积 | 1 | 静止/UI/过场/缓慢光照下的幅度抖动、修正开关 | 运动区域基本无效；同色移动会误匹配 |
| B 光流累积 | 2 | 相机平移、物体运动中的抖动（通用性最好） | 光流错误 → 拖影；遮挡/透明/反射泄漏 |
| F 光流累积+ | 3 | **同一片修正反复出现/消失**（不是幅度抖动） | 持续的错误修正也会被保留；弱修正被削弱 |
| G 低频时域重建 | 4 | 大面积低频呼吸、光晕脉动 | 高频闪烁直通；多一次半分辨率 dispatch |

公共骨架五个动作：取残差 → 颜色引导 + 3×3 patch 匹配算置信度 `q` → `w = exp(-Δt/τ)·q` → 范围保护 → 输出与历史写入。
参数：`antiFlickerTimeMs` 默认 **80 ms**（5–200）；历史全分辨率 RGBA16F；`Δt` 取**捕获帧时间戳**，不是显示 FPS。

## 3.3 最值得借鉴的四条（比公式重要）

1. **只稳定残差**：不碰模型输入，因此不会与模型内部历史形成反馈环。
2. **用原图颜色验证历史，不用残差差异验证**：拿 `abs(R_t − H_{t-1})` 做拒绝会把"NR 自己该抑制的波动"误判成新内容，历史永远留不住。
3. **按真实捕获时间定义历史长度**：重复 present、参数重算不增加样本；间隔 > 250 ms 或时间戳倒退直接令 `w = 0`。
4. **历史生命周期必须可失效**：分辨率/资源代际/输入修订/参数/NaN/采样越界全部重建，且**资源要一起 reset**。算法公式只占一半工作量。

显存（我们 3840×2054 口径）：A/B/F ≈ 271 MiB，G 再加 ≈ 30 MiB。

## 3.4 本项目现状（缺口清单）

| 已有 | 缺 |
|---|---|
| `base = job.res[kColor]` 与 `raw = privateOutput` 都在手边、同尺寸 | `mochinr.cpp:701-721` 构造 `LmxxfNrFrameInfo` 时**没给** `fi.motion/depth/smooth_*` 赋值 |
| 插入点明确：`mochinr.cpp:838` `RecordOutputs` 之后、`:845-856` 重放 FSR4 之前 | 未设 `TEMPORAL / SMOOTH_RESIDUAL`；`history_strength` 未在 `SendControls` 下发 |
| 运行时支持时域（`LmxxfNrApi.h:132-153/:76-78`） | 本项目内**没有**任何时域历史/残差/抗闪烁代码 |
| 宿主队列、allocator 环、fence、延迟释放齐备 | `kMotion/kDepth` 从未验证格式/尺寸/非零比例 |

## 3.5 实验方法（按性价比，不按论文完整度）

| 阶段 | 做什么 | 为什么这个顺序 |
|---|---|---|
| **P0** | 先只加诊断：把 `kMotion/kDepth/motionScale` 接到 `fi.*`，只打日志（格式、尺寸、非空、非零比例） | 先证明"有没有运动矢量"，再谈算法。改动小、可回退 |
| **P1** | `TEMPORAL | SMOOTH_RESIDUAL` + 保守的 `smooth_threshold/strength` + 下发 `history_strength`，读 `history_consumed_pct`、`motion_refused_dxgi` | 用手头已有的能力验证最便宜路径 |
| **P2** | 外部「静态累积 A」最小闭环（新纹理写，不回写 `privateOutput`） | 只处理静止/UI/过场；先确认没有拖影 |
| **P3** | 光流累积 B（严格校验方向/单位/尺寸/帧 ID；3×3 邻点差 > 2 px 拒绝整个 patch） | 只在 `kMotion` 真实可用时做 |
| **P4** | F 的 support/amplitude 拆分；G 仅在"低频呼吸"被实测确认后 | F 比 G 更对症且不需要新纹理 |

**不建议一上来做 G**：它只稳低频，细纹理/高频闪烁直通，成本最高、性价比最低。

## 3.6 验收（不要用"看起来更稳"当结论）

固定同一段素材与同一 NR/FSR4/FG 参数，比较 无 / A / B / F：

| 场景 | 观察点 |
|---|---|
| 固定镜头、菜单、过场 | 修正是否还在闪；有无静态拖影 |
| 慢速平移 | 边缘是否干净；历史跟得住吗；是否粘住旧修正 |
| 快速平移/转身 | 光流错误、遮挡边缘、UI 是否被搬错 |
| 树叶/栅栏/细线/棋盘 | 高频闪烁是否仍在 |
| 透明/反射/水面 | 颜色验证是否把背景历史搬到前景 |
| HUD/字幕 | 固定 UI 与运动背景是否冲突 |
| 场景切换/暂停/分辨率变化 | 是否瞬间清历史、有无残留 |
| FG 开/关、30 分钟长跑 | 稳定器滞后是否被 FG 放大 |

指标：残差时间方差、修正保留率 `mean(abs(H))/mean(abs(R))`、新修正响应时间（阶跃后 90%）、GPU 新增 ms、显存、失败/重置次数、NaN 计数。

---

# 4. 三条线的耦合（本文最要紧的一节）

## 4.1 线 2 成功 ⇒ 线 3 的大半不必自研

抗闪烁的 A/B/F/G 全部是**宿主侧 HLSL**，输入只有 `(base, raw)`，**与谁产生 raw 无关**。
所以一旦我们的运行时以 `amdgx_dlssnr.dll` 的形式进槽：

- 抗闪烁**免费**（面板里那 5 档还在）；
- 残差提取/升采样/叠回/多趟**全部免费**；
- `mvec/depth` 由宿主 AMDOF 提供，**不必再赌 `ffxDispatch.desc.res[kMotion]` 是否为空**——这条正是线 3 §3.5 P0 的最大不确定性。

**代价**：显存 +271 MiB（4K），并且要接受宿主实现的质量上限（颜色验证而非几何验证）。

## 4.2 线 2 成功 ⇒ 线 1 的收益前提要重算

现在 `13.7·s²` 这一项几乎全是 NR 岛（在 `FSR3Upscaler` 的队列里）。NR 移到 DLSSNR 槽后：

- `FSR3Upscaler` 岛内只剩 **FSR4 放大**，岛变小、`13.7·s²` 那项大幅缩水；
- 单槽串行的"求和"结构还在，但**可隐藏的空间也变小了**——`max(8.0, 1.4+ε)` 与 `8.0+1.4+ε` 的差趋近于 0；
- 于是 **G0 的 ratio 会因为链变了而变**，必须在"新链"上重测一次。

**这不是说线 1 白做**：多槽 + 输出晚一帧在旧链（NR 仍在槽内）上是唯一能把 0.5 档推到 123 的手段；
但它和线 2 是**两条不同的路线**，最终只应留一条作为主线。

## 4.3 唯一的真互斥

抗闪烁的时域 pass 与单槽的"取大"都在争同一段 GPU 时间：前者要额外一次全分辨率 dispatch + 4 张 RGBA16F，
后者要额外的槽位显存（3 槽 ≈ +193 MB）。16 GB 卡上两者叠加要重新做显存与带宽预算。

## 4.4 实机执行顺序（建议）

| 天 | 做什么 | 产出 |
|---|---|---|
| D1 | **线 2 的两处改动（B 路线）** + **P0 探针** | ABI 日志（成功码/feature 号/键槽位）+ 证明接线正确 |
| D1 附带 | **线 1 的 G0**（当前链，零代码） | ratio 三档判决，决定线 1 是否还有意义 |
| D2 | **线 2 P1 直通** | 槽位成立；同时验证"宿主抗闪烁对陌生 raw 是否工作"（= 线 3 免费路径） |
| D3 | **线 1 阶段 1/阶段 2**（旧链） | 阶段 1 应无变化；阶段 2 看 123 / 66 |
| D4 | **线 2 P2 接运行时** → 复测线 1 的 G0 与阶段 2（新链） | 决定主线走哪条 |
| 兜底 | 线 3 P0/P1（自研时域） | 仅当线 2 不可行时启动 |

## 4.5 统一记录（三条线共用）

每次实机都记同一组字段，便于横向比较：
`配置 | 网络档 | 原生 fps | 补帧后 fps | frame_ms | fence_ms | slotWait | harvestMiss | dropped | ratio | 主观（闪烁/拖影/色偏）`。

采集脚本已经统一：`measure-gpu-busy.ps1`（ratio）、`measure-fsr-ring.ps1`（fps/围栏/丢帧/汇总）。

---

# 5. 待确认问题（会改变结论的）

1. **G0 的 ratio 是多少**：>0.85 则线 1 直接作废；<0.60 才有台阶。
2. **P0 探针在 9070 上能不能被加载**：取决于是否先绕过 `UnsupportedAdapter` 门。
3. **成功码/feature 号**：错一个值就卡在 `Init_Ext`。
4. **`kMotion` 到底是否为空**：决定线 3 的 P0 是否值得做（若线 2 成功则此问自动消失）。
5. **两个效果的取舍**：若走 A 路线（独立效果），那 17 处名字判定要逐条判"NR 语义 vs DLSSNR 实现专属"。
6. **显存预算**：NR 模型 + FSR4 + 抗闪烁 271 MiB + 单槽 193 MiB 能否在 16 GB 内共存。

# 6. 出处

- `docs/multislot.md`（单槽拆解设计定稿）、`src/magpie-patches/README.md`
- `docs/findings.md`（A/B 天花板实测五点、第三串行点）、`docs/measurements.md`
- `docs/nvngx-slot.md` 待补：`src/nvngx-shim/README.md` 里有 ABI 事实表与探针用法
- 《DLSSNR-抗闪烁机制与借鉴分析》（WorkBuddy 文档，2026-11-03）
- 上游：`SAOG0721/Magpie` `experimental` @ `6940f84`；本机发行版 `D:\DLSS-NR-magpie\magpie 0.6.11`
