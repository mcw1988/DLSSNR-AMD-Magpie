# Magpie NR v0.6 · 发行说明（DLSSNR 原生槽 + mochizuki ACO 权重）

> 本文件是 v0.6 发行包里的 `README.md` 草稿，内容按 2026-11-04 的实测与裁决固化。
> 相关细节文档：`docs/2026-11-04-v06-panel-wiring.md`（面板接线）、
> `docs/2026-11-04-旋钮实测与半分辨率长跑.md`（实测数据与模型复核）。

## 1. 这个包是什么

在 Magpie 0.6.11（`DLSSNR\DLSSNR_AI_Filter` 原生效果槽）里跑 **mochizuki-ACO 权重**的
神经降噪，**不再经过 ReShade**：

```
游戏 → Magpie 捕获 → (可选)宿主降采样 → DLSSNR 槽 → amdgx_dlssnr.dll → MochizukiNrRuntime.dll
                                                              ↑ 权重在 dlssnr-amd\
```

- NR 与 Magpie 自己的残差合成在同一条链里：底图恒为**原始分辨率输入**，低分辨率只承担残差。
- 需要 RDNA4（RX 9000）与 `vk_cooperative_matrix` FP8；RDNA3 走降级路径尚未验证。

## 2. 渲染档位：`s` 恒为 1.0（**钉住的设定**）

| 项 | 值 |
|---|---|
| 网络档 `s`（`reno_resolution_scale`） | **恒 1.00**，发行包默认且不建议改 |
| 档位表达方式 | **只用面板「输入分辨率（%）」**：100 / 75 / 50 |
| 配置文件 | `MochiShimScale.txt`（与本 DLL 同目录，内容一行 `1.00`） |

**为什么钉 1.0**：有效残差分辨率 = 宿主输入尺寸 × `s`，而画面基底恒为原始 4K（见 `CompositeResidual`）。
输入降到 50% 时网络已经只有 1920×1027 的工作量，再降 `s` 只会让残差更粗、省不下多少，纯亏画质。
要更高画质只能抬**输入分辨率**，不是抬 `s`。

**现行档位速查**（4K 源，实测）：

| 输入分辨率 | 网络/残差尺寸 | 帧时间 | 帧率 |
|---|---|---|---|
| 100% | 3840×2054 | 19.67 ms | 50.8 fps |
| 50% | 1920×1027（≈1080p 的 95% 像素） | 9.61 ms | 104 fps |

> `MochiShimScale.txt` 是**实验通道**：写 0.25–1.0 会在运行中热重建网络（约 0.4 s，画面不中断）。
> 发行包保持 `1.00`；删掉该文件等价于回落默认 1.0。**不是 ini，也不在 `config.json` 里**
> （`mochinr.ini` 的 `model_scale` 属于已退役的 ReShade 路线，与此无关）。

## 2.1 时域与光流：两个文件开关

运行时的**运动矢量与历史默认是不工作的**——只有打开 `TEMPORAL` 帧标志它才取 motion 并跑历史
（2026-11-04 实测：关=0%、开=100%）。开关是 DLL 旁的两个文本文件，**只在 Magpie 启动时读一次**：

| 文件 | 值 | 作用 |
|---|---|---|
| `MochiShimTemporal.txt` | `0`/`1` | 打开/关闭 TEMPORAL（时域）。**v0.6 默认 `0`（关）**，原因见下 |
| `MochiShimPasses.txt` | `1`/`2`/`3` | **网络层数（叠层）**。运行时在**一个 job 内**跑 N 趟，热生效（重建网络约 0.5 s） |
| `MochiShimMotion.txt` | `0` | 关掉"把宿主光流交给运行时"（默认交给，一般不用动） |
| `MochiShimHalo.txt` | `0`/`1` | 抗光晕（ANTI_HALO）。文件不存在时**跟随层数**：≥2 层自动开 |

> **叠层请用 `MochiShimPasses.txt`，不要用面板的 `Multi Pass`**：面板那个走宿主的 `laterPasses`，
> 每趟是一次独立 `EvaluateFeature`，而我们的 producer 跑在自有队列上、上一趟的写回要等宿主帧末提交，
> 会读到上一帧的中间结果 → 全画面闪烁（§6 限制 1.1）。文件这条路是旧 ReShade 路线与上游 OpenNR 的做法
> （`mochinr.cpp:719 fi.passes`、`mochizuki.inc:357 fi.passes` + `reno_passes`），单 job 内串行，没有跨队列问题。

**v0.6 现行组合：`Temporal=0`、`Passes=1`（可选 2）、`Scale=1.00`、`Halo` 跟随层数、面板抗闪烁 = `None`。**

### 为什么 v0.6 默认关时域（2026-11-05 决定）

时域确实能压闪烁（开=闪烁明显减轻），但它的代价在 AMD 这条链上无法回避：

1. **深度传不进运行时**（`depth DXGI 0`）⇒ 遮挡/轮廓边缘的重投影没有深度判据 ⇒ 拖影/鬼影。
2. 面板的**抗闪烁五档本质都是时域**，实测：静态累积"拖影严重且响应时间越高越重"；光流累积在转视角时出现"吃毒蘑菇"式扭曲；低频时域重建效果最好但同样属于时域。
3. **FG 插帧不依赖 NR 时域**：XeSS FG 用 Magpie 自己的光流，与运行时时域是两套，关掉不影响 2× 补帧。

所以 v0.6 的选择是：**画面干净优先，用收紧参数范围（§3.1）抵消闪烁**，接受"无历史、降噪上限略低"。

> 想要更强降噪、能接受拖影的用户，可以自行把 `MochiShimTemporal.txt` 设为 `1` 并把层数保持 1。


## 2.2 帧生成（XeSS FG，2×）

v0.6 的 `Magpie.exe` 已经把 **XeSS Frame Generation** 编进去了（官方 0.6.11 原版带 FG 但 DLSSNR 只认
NVIDIA；amd 版默认没开 FG，v0.6 这个 exe 是两者合并后的构建，见 §6 限制 6）。

用法：

1. 效果组里保留 `XeSSFG\XeSS_FrameGeneration`（和 `DLSSNR\DLSSNR_AI_Filter`、`FrameRate_Filter` 并列）
2. 光流用 Magpie 自己的 **AMDOF / Quality**
3. **帧刷新 → 内容帧率 = 跟随源，空闲 = 关**（否则呈现被 idle 的 30 FPS 压住，看起来像掉帧）

实测（2026-11-05，输入 60% = 2304×1232，DLSSNR 1 层）：

| 项 | 值 |
|---|---|
| DLSSNR 源帧率 | 58.5 / 59.3 / 59.4 fps |
| 呈现帧率 | **约 118 fps**（2×） |
| SDK 日志 | `requested=2x frames=7679 submissions=3840 lastFGResult=0` ← 7679/3840 = 2.00 |
| `motionFrames` | 3839 ≈ submissions（每帧都有运动矢量，用的就是 AMDOF） |

代价：**输入延迟略增**（插帧的固有代价），手感是"更顺但略微不跟手"。
FG 与 NR 时域互相独立——FG 走宿主光流，所以 v0.6 关掉 NR 时域不影响补帧。

## 3. 面板参数与生效方式
中文界面下的六个常用旋钮都在 `DLSSNR · Pass 1` 组里（Pass 2/3 的标签仍是英文）：

| 参数（中文标签） | 生效方式 |
|---|---|
| NR 强度 / NR 风格 / 局部色调强度 / 局部结构强度 / 皮肤结构强度 / 自动遮罩 | **即时生效**（`Live`） |
| 调整输入分辨率 / 输入分辨率（%） | **需重启效果器**（停缩放再开始；不必退 Magpie） |
| Multi Pass（未译）/ 抗闪烁 | **需重启效果器** |
| 抗闪烁响应时间（ms） | 即时生效 |
| 光流方法 / 光流质量 | **需重启效果器** |

> 「输入分辨率」改变的是整条链的资源尺寸，所以必须重建链；这是宿主的设计，不是 bug。
> 「皮肤结构强度」填 0 表示"不表态"（跟随局部结构），不是"关闭"。

## 3.1 参数安全区间（v0.6 关时域后的替代方案）

关掉时域后，闪烁只能靠**不让参数进极端区**来规避。下表来自 2026-11-04 的 150 组实测与
2026-11-05 的抗闪烁对照，标注了每条是**实测**还是**推断**：

| 参数 | 建议区间 | 依据 |
|---|---|---|
| NR 强度 `intensity` | **0.9 – 1.4** | 实测：≥1.9 与 ≤0.1 明显闪（>1 会抬亮度比，进非线性区） |
| 局部色调强度 `localTone` | **0.4 – 1.2** | 实测：调高时大面积亮度区出现呼吸感 |
| 局部结构强度 `localStructure` | **0.8 – 1.3** | 实测：两端（0.05 / 1.95）在运动边缘闪 |
| 皮肤结构强度 `skinStructure` | **-1（不表态）或 0.2 – 0.8** | 推断：>1 与自动遮罩叠加时交界处跳动 |
| NR 风格 `style` | **0 或 1，避开 2** | 实测：style 2 边缘闪烁明显加重（电影风格的高光守护更激进） |
| 自动遮罩 `useAutoMask` | 关时更稳、开时局部更好 | 推断：遮罩在皮肤/纯色交界逐帧翻判；两者可 A/B |
| NR 层数 `MochiShimPasses.txt` | **1**（想要更干净可试 2） | 实测：2 层画面更干净但成本 +4 ms、边缘拖影更明显 |

**调参顺序建议**：先把 `intensity` 定在 1.0–1.2，再用 `localTone` / `localStructure` 找质感，
最后动 `style` 与 `skinStructure`。**别同时改三个以上**——无时域时参数之间的非线性叠乘很难归因。

**不要期待有反应的旋钮**（运行时没有对应字段，调了也不动）：

| 面板项 | 为什么无效 |
|---|---|
| `NR UI Correction`（UI 修正） | shim 从未读取 `DLSSNR.UICorrection`，且运行时结构里没有 UI 字段；它是 NVIDIA 模型的参数 |
| 「细节控制」整组（总强度 / 色度强度 / 明度总强度 / 暗部结构强度 / 亮部发光强度 / 过校正抑制 / 低频·高频范围强度 等 `residual*`） | 属于宿主侧的残差修正（NVIDIA 路径的量），`renodx=1` 的运行时路径不消费 |
| Pass 2 / Pass 3 的所有参数 | 只在面板 `Multi Pass ≥ 2` 时才有意义，而 v0.6 规定面板 `Multi Pass` 恒为 1 |


## 4. ReShade 已退役

本包**不含** `dxgi.dll`（ReShade）、`ReShade.ini`、`*.addon64`。
Magpie 会加载系统 `dxgi.dll`，Home 键不再弹 ReShade 面板——调参用 Magpie 自己的效果面板。

## 5. 日志与自检

`amdgx_dlssnr.dll` 同目录的 `nvngx-shim.log`：

| 行 | 含义 |
|---|---|
| `panel: controls READY struct_size=…` | 面板通道建立 |
| `panel: defaults reno{renodx=1 … mask=0}` | 运行时默认值（`mask` 默认 0，与面板一致） |
| `panel: setcontrols rc=0 … intensity=… tone=…` | 面板值已下发（按 500 ms 限流） |
| `motion: 已接上 motion=…(WxH scale=…) depth=…` | 宿主的运动矢量/深度已交给运行时 |
| `info: frames=… history=…% motion_refused_dxgi=… gpu_ms=…` | 每 300 帧一条，判"历史/光流到底用没用" |
| `CALL ReleaseFeature (nrFrames=… panelCalls=…)` | 会话收尾，帧数与下发次数 |

运行时自己的日志 `mochizuki_nr.log`：`RenoDX stage: A (network B)`、`session end: network … ms`。
宿主日志 `logs\magpie.log`：`DLSSNR STATUS: … style= intensity= …`、`DLSSNR chain: passes=…`。

## 6. 已知限制

1. **改「输入分辨率 / Multi Pass / 抗闪烁 / 光流」必须重启效果器**（停缩放再开始即可，不必退 Magpie）。
1.1 **面板的 `Multi Pass` 必须保持 1**（叠层请改用 `MochiShimPasses.txt`，见 §2.2）。
   原因：面板那个走宿主的 `laterPasses`，每趟一次独立的 `EvaluateFunction`；而本 shim 的 producer 跑在自己的
   队列上、上一趟的消费者拷贝却记在宿主命令表里**帧末才提交**，于是第二趟读到上一帧的中间结果 →
   **全画面频繁跳变闪烁**（2026-11-04 实测，Temporal 开/关都闪）。
   内部叠层（`MochiShimPasses.txt`）在**一个 job 内**串行跑 N 趟，不存在跨队列时序问题，已实测可用（§2.2）。
1.2 **深度（`DLSSNR.Depth`）传不进运行时**：本运行时 DLL 的 `MochiNrFrameInfo` 在 depth 字段之前结束
   （motion 能进、depth 恒 `DXGI 0`）。所以轮廓处的历史重投影没有深度可用，极端残差参数下会出现拖影/鬼影。
2. **极端旋钮值会出现闪烁** —— 已定性：关掉时域后运行时是**逐帧独立推断**（`history=0%`），
   逐帧误差没有历史压住。缓解办法是遵守 §3.1 的参数区间，不要指望抗闪烁面板（它本质也是时域，
   会带来拖影/扭曲；五档实测结论见归档文档 §5.2）。
3. 关掉光流时运行时日志为 `motion 0x0`，此时一切时域相关能力都不生效；**关掉 `Temporal` 后
   即使光流正常，运行时也不使用历史**（`history=0%`），这是 v0.6 的预期行为。
4. HDR 路径下「调整输入分辨率」等参数变为 `Restricted`（不可改），需在 SDR 下配置。
5. **Magpie 的空闲降帧**（帧刷新 → 空闲 30 FPS）会让"看起来只有 30 帧"——测帧率前请把
   `帧刷新 → 空闲` 关掉、并把内容帧率设为「跟随源」，否则读数会被 idle 的 30 覆盖。
6. **XeSS Frame Generation 需要本仓库自己编译的 `Magpie.exe`**：官方发布的 `0.6.11` 原版带 FG
   但 DLSSNR 只认 NVIDIA（`Selected adapter is not NVIDIA`），而 amd 版默认没开 FG。
   v0.6 提供的 `Magpie.exe` 是"AMD DLSSNR 补丁 + `EnableXeSSFrameGeneration=true`"的合并构建
   （来源：`D:\_build\Magpie` @6940f84 + 本次编译，构建脚本 `_test-kit\10-重编带XeSSFG\build.ps1`）。


## 7. 致谢与许可

运行时（`MochizukiNrRuntime.dll`）、权重（`dlssnr-amd\`）、Magpie 本体与 DLSS SDK 均为上游提供，
许可与来源见 `THIRD-PARTY-NOTICES.md`；本包只含自研的 `amdgx_dlssnr.dll` 与配置。
