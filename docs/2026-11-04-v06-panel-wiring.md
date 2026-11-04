# v0.6 清单第 1 项：把 Magpie NR 面板接到运行时（设计与首轮实现）

> 2026-11-04。对象：`docs/2026-11-04-P2-milestone.md` 的「v0.6 前工程清单」第 1 条。
> 本轮**只做面板 → 运行时的下行通道**（清单第 1 项），并在离屏环境把浮点通道验穿；
> 实机逐旋钮对图属清单第 2 项，按 §7 的清单跑。
> 部署位置：`D:\Magpie-NR-046beta`（新 `amdgx_dlssnr.dll` 已就位，旧件留作 `amdgx_dlssnr.dll.prev`）。

---

## 0. 结论速览

| # | 结论 | 证据 |
|---|---|---|
| 1 | **浮点键读不到的原因已定位并修好**：不是"接口不给你值"，而是 32 个槽位被统一声明成 `(name, void*, void*)`。浮点值在 XMM0，槽位却去 R8 取，于是只拿到残值 | P2 日志 `DLSSNR.Intensity = 7FFFFFFFFFFFFFFC (slot 5, first)`；修后 ABI 探针 `ABI.Float = 1.25 (slot 5, float)`（**用上游 nvsdk_ngx_params.h 实测**，见 §2.2） |
| 2 | **不需要动宿主**：宿主本来就把 `_passSettings[i]` 逐 pass 送进 NGX 参数（`DLSSNR.Intensity/Style/LocalTone/LocalStructure/Skin/AutoMask`），shim 侧把槽位签名改对即可全收 | `DLSSNRFilter.cpp:1558-1591`、`:2589` |
| 3 | **逐 pass 天然对齐**：宿主对每一趟 pass 先 `SetEvaluateParameters`、再 `EvaluateFeature`；`SetControls` 在"下一次 `PrepareFrame`"生效，而每趟 pass 正好就是一次 `PrepareFrame` | `DLSSNRFilter.cpp:2589/2598` |
| 4 | **发现一个必须先修的旧账**：运行时默认 `renodx=1`，这一档**忽略模型组**（含 `fi.model_scale`）。所以 P2 期间的 `MOCHI_SHIM_SCALE` 从未真正改过渲染率 —— 清单第 3 项的 M3（0.5/0.35 档）如果照原样跑，会得到一个"什么都没变"的假数据 | `MochizukiNrControls.h` 的 `renodx` 注释；本版已把 s 改走 `reno_resolution_scale` |
| 5 | **`Skin Structure` 有个表达缺口**：面板只有 0..2，表达不了运行时的 −1（跟随 Local Structure）。本版把面板 0 视为"没表态"，保持运行时默认，避免把 −1 悄悄改成 0 | 本版 `ReadPanel()` |
| 6 | 离屏自测已覆盖 float/int/uint/指针四类槽位，**不需要 9070 也能验** | `build-p2.ps1 -SelfTest`，结果见 §6.1 |
| 7 | **已对上游头文件校对**：`abi_probe` 直接 `#include "nvsdk_ngx_params.h"` 驱动我们的参数对象，5 项落点全中（float→Slot5、double→Slot4、int→Slot2、uint→Slot3、`ID3D12Resource*`→Slot0） | §6.2 |

---

## 1. 范围裁决

清单第 1 项原文要求「强度 / 层数 / 渲染率 / 风格全接」。按今天能做与必须做的边界拆：

| 子项 | 今天 | 说明 |
|---|---|---|
| 强度 / 风格 / 三个结构项 / AutoMask（逐 pass） | **已接** | 宿主已在送，shim 侧收下即可，零宿主改动 |
| Reset | **已修正语义** | 原来按"键在不在"判断，现按**值**判断（宿主每帧都 Set） |
| 层数（Multi Pass 1..3） | **无需接** | 宿主用 `laterPasses` 串接，每趟一次 `EvaluateFeature`；shim 保持 `fi.passes = 1` 才是正确语义（若再叠 `passes>1` 会变成 N×M 次网络） |
| 渲染率 s（网络内部档） | **未进面板** | 面板没有这个键，要加键 = 改效果 HLSL + 宿主解析 → 属 §8 的 0004 补丁。本版先把环境变量接到**真正生效的字段**上，让清单第 3 项的 M3 可跑真数据 |
| `NR UI Correction` | 不接 | NVIDIA 侧 UI 校正，运行时没有对应字段；只记日志 |
| `Adjust Input Resolution / (%)` | 不接 | 那是宿主在送进 NR **之前**做下采样，和"网络渲染率"是两条轴（`docs/2026-11-04-line1-recompute.md:§1.5` 已强调过一次），混用会把 M3 的账算错 |

---

## 2. 事实基础

### 2.1 参数对象是"宿主调、我们建"

`NVSDK_NGX_D3D12_AllocateParameters` 由 shim 实现（`passthrough.cpp`），返回的是我们自己的
`PassthroughParameters`。宿主拿到 `NVSDK_NGX_Parameter*` 后按**它自己那份头文件的 vtable 顺序**调
`Set(name, value)`。所以槽位编号由宿主决定，我们只能用日志反推 —— 这也正是 P0 探针设计的初衷。

### 2.2 实测槽位表（对上游头文件校对过）

权威声明：DLSS SDK 的 `nvsdk_ngx_params.h`（宿主 `BuildOptions.props.user` 里 `DLSSSdkDir = D:\_build\DLSS-SDK`，
`Common.Post.props:23` 在 `EnableDLSSNR` 时把它的 `include` 加进来）。接口**没有虚析构**，8 个 `Set` 的**声明顺序**是：

```cpp
virtual void Set(const char*, unsigned long long);
virtual void Set(const char*, float);
virtual void Set(const char*, double);
virtual void Set(const char*, unsigned int);
virtual void Set(const char*, int);
virtual void Set(const char*, ID3D11Resource*);
virtual void Set(const char*, ID3D12Resource*);
virtual void Set(const char*, void*);
```

但 MSVC 把**同名重载在 vtable 里按声明顺序反着摆**，实际 vtable 与我们的槽位对照如下
（`abi_probe.cpp` 直接 include 上面那份头文件实测，见 §6.2）：

| 实际 vtable 下标 | 真实重载 | 本 DLL 的接收槽 | 值所在寄存器 | 状态 |
|---|---|---|---|---|
| `[0]` | `Set(const char*, void*)` | **`~PassthroughParameters`（虚析构）** | R8 | 静默丢弃，见 §2.3 |
| `[1]` | `Set(const char*, ID3D12Resource*)` | `Slot0` | R8 | 一直可用（Color/Output/MVec/Depth 全走这里） |
| `[2]` | `Set(const char*, ID3D11Resource*)` | `Slot1` | R8 | 未用到（DLSSNR 只走 D3D12） |
| `[3]` | `Set(const char*, int)` | `Slot2` | R8 | 一直可用（Style/Reset/Enabled/AutoMask） |
| `[4]` | `Set(const char*, unsigned)` | `Slot3` | R8 | 一直可用（Width/Height/NodeMask） |
| `[5]` | `Set(const char*, double)` | `Slot4` | XMM0 | 未用到 |
| `[6]` | `Set(const char*, float)` | **`Slot5`** | **XMM0** | **本版修好**（Intensity/Tone/Structure/Skin/MVecScale） |
| `[7]` | `Set(const char*, unsigned long long)` | `Slot6` | R8 | 未用到 |

这张表与 P2 实机日志逐条吻合（资源 `slot0`、int `slot2`、uint `slot3`、float `slot5`），
说明"从日志反推"和"按头文件实测"两条路得到同一个结论。

### 2.3 为什么以前是垃圾值，以及两个必须知道的推论

**病根**：32 个槽位一律声明成 `(const char*, void*, void* a, void* b)`。整型/指针的值在 GP 寄存器
（R8），歪打正着拿到；浮点/双精度的值在 XMM0，R8 里是别人的残值 —— 日志里那个
`7FFFFFFFFFFFFFFC` 就是残值本身（它同时还是个 NaN 的双精度位型，很容易被误读成"值"）。

**修法**：给每个槽位声明与真实重载一致的类型签名，编译器自己去 XMM0 取。
**不需要改宿主**，因为宿主本来就在送这些键。

**推论 1：`Set(name, void*)` 会落到我们的虚析构上**。默认析构、类里没有数据成员，等于静默丢弃，不会崩
（注意 vtable 里只有一格析构，调用它不会 `operator delete`，所以也不会把我们 `new` 的对象释放掉）。
已核 `src/Magpie.Core/DLSSNRFilter.cpp` / `NgxD3D12Core.cpp`：宿主对 DLSSNR 参数**从不**调 `Set(…, void*)`。

**推论 2：`Get(...)` 会落到 `Slot7` 之后**（Get 的 8 个重载在同一张 vtable 的尾部）。已核：宿主对这块参数对象
**从不**调 `Get`；`NgxD3D12Core` 只在 Allocate/Destroy 上用它。所以槽 7..31 目前是"永远不会被调用"的余量。

### 2.4 renodx 组遮蔽模型组（旧账）

`MochizukiNrControls.h` 对 `renodx` 的注释：默认 1，此时"**passes、detail/colour strength、model scale、
highlight guard、white point、enlargement 与 pass extras 都不用它**"，改由 `reno_*` 组接管。

**实机证实（2026-11-04 20:30 会话）**：
- 运行时日志：`[nr] RenoDX stage: 3840x2054 (network 3840x2054), SDR (legacy codec)`、
  `building CC_SMALL_FP8 … in the RenoDX stage at 3840x2054, 1 pass`；
- shim 日志：`panel: defaults reno{renodx=1 …} model{…}`。

顺带纠正一个**猜测错误**：`reno_auto_mask` 的默认是 **0**，而模型组的 `automatic_mask` 默认才是 1。
所以"面板 autoMask 默认 0 会造成基线差异"这条担心（原 §9 第一条）**不成立**。

后果有两层：
1. 面板若只写模型组（`intensity/style/local_*`），画面不动 —— `mochinr.cpp` 的 `SendControls` 注释里
   已经为同一个坑写过一次；
2. **`fi.model_scale` 也是模型组**，所以 `MOCHI_SHIM_SCALE=0.5` 在 P2 链上不会改变网络分辨率。
   本版把 s 同时写到 `reno_resolution_scale`，s 轴才真的可调。

### 2.5 逐 pass 的参数

`DLSSNRFilter::Draw` 的 pass 循环：`_passSettings[i]` → `SetEvaluateParametersSafely` → `EvaluateFeature`。
所以**每一趟 pass 的 `Intensity/Style/...` 都可能不同**，shim 必须"和上次真的下发的那份比"，
不能用只在下一次改动时下发的脏标记。（本版用值比对，见 §5。）

---

## 3. 现在的数据流

```
Magpie 面板旋钮
  → EffectOption（effects/DLSSNR/DLSSNR_AI_Filter.hlsl 声明的参数）
  → ParseDLSSNRSettings()            （逐 pass：_passSettings[i]）
  → SetEvaluateParametersUnsafe()    → NGX 参数对象（DLSSNR.Intensity / Style / ...）
  → [ slot0/2/3/5 按类型落地 ]        ← 本版修好的一段
  → ReadPanel() 建 PanelSnapshot
  → MochizukiNrSetControls(session, &controls)     （值变了才发）
  → 下一次 PrepareFrame 生效
```

另一条并行的：`reset` 不进 `SetControls`，直接进 `MochiNrFrameInfo.reset`（每帧真值）。

---

## 4. 接线表（本版已生效）

| 面板（分组 · 标签，**中文界面下的显示**） | 宿主 settings 字段 | NGX 键 | 运行时字段 | 生效时机 | 备注 |
|---|---|---|---|---|---|
| DLSSNR · Pass N / **NR 强度** | `intensity` | `DLSSNR.Intensity` (float) | `reno_intensity` + `intensity` | 下一帧 | 0..2 |
| DLSSNR · Pass N / **NR 风格** | `style` | `DLSSNR.Style` (int) | `reno_style` + `style` | 下一帧 | 0 标准 / 1 自然 / 2 电影；**改它会重建网络**（见 §9） |
| DLSSNR · Pass N / **局部色调强度** | `localToneStrength` | `DLSSNR.LocalToneStrength` | `reno_local_tone` + `local_tone` | 下一帧 | 0..2 |
| DLSSNR · Pass N / **局部结构强度** | `localStructureStrength` | `DLSSNR.LocalStructureStrength` | `reno_local_structure` + `local_structure` | 下一帧 | 0..2 |
| DLSSNR · Pass N / **皮肤结构强度** | `skinStructureStrength` | `DLSSNR.SkinStructureStrength` | `reno_skin_structure` + `skin_structure` | 下一帧 | 面板 0 = 不表态，保持运行时 −1 |
| DLSSNR · Pass N / **自动遮罩** | `useAutoMask` | `DLSSNR.UseAutoMask` (int) | `reno_auto_mask` + `automatic_mask` | 下一帧 | 实测 `reno` 默认 0，与面板默认一致（§9） |
| （宿主内部）Reset 按钮/丢历史 | `impl.resetHistory`、`guidanceReset` | `DLSSNR.Reset` (int) | `MochiNrFrameInfo.reset` | 当帧 | 按值判断，不看键是否存在 |
| DLSSNR · Pass 1 / **Multi Pass**（未译） | `_passSettings` 长度 | — | — | — | 宿主侧 laterPasses；shim 固定 `passes=1` |
| 环境 `MOCHI_SHIM_SCALE` | — | — | `reno_resolution_scale` | 建会话/改值时 | 本版才真正生效 |
| 环境 `MOCHI_SHIM_RENODX` | — | — | `renodx` | 同上 | 0/1，用于 A/B |

日志新增（只在值变化时下发；下发日志限流：前 30 次 + 之后每 600 次一条，
因为逐 pass 不同控制会让"每帧都发"，全量打印会灌爆日志）：
```
panel: controls READY struct_size=...
panel: defaults reno{renodx=1 passes=1 scale=1.00 style=0 intensity=1.00 tone=1.00 struct=1.00 skin=-1.00 mask=1} model{...}
panel: setcontrols rc=0 (f=1234) renodx=1 style=0 intensity=1.25 tone=1.00 struct=1.00 skin=0.50 mask=1 scale=1.00 passes=1(structural)
```

---

## 5. 代码改动清单

文件：`src/nvngx-shim/passthrough.cpp`、`selftest.cpp`、`build-p2.ps1`。

| 位置 | 改动 | 目的 |
|---|---|---|
| `PassthroughParameters` | `Slot0/2/3/4/5` 改为**真实类型签名**（`void*` / `int32_t` / `uint32_t` / `double` / `float`），`Slot1`、`6..31` 保持指针形 | 让浮点值从 XMM0 正确取回 |
| 同上 | 值存储从 `pair<void*,void*>` 改为带 `ParamKind` 的 `ParamValue`，新增 `GetInt/GetFloat` | 区分"键存在且值为 0"与"键不存在" |
| `MochiBridge` | 新增 `setControls/controlDefaults/getInfo`、`defaults`、`sent`(PanelSnapshot)、`controlsReady/controlsSent` | 控制项通道状态 |
| `InitMochi` | `GetProcAddress` 绑三个导出，取一次 `MochizukiNrGetControlDefaults` 并**把默认值打进日志** | 老运行时可降级；默认值是"该写哪一组字段"的唯一事实来源 |
| 新增 `ReadPanel` / `ApplyPanelControls` | 面板键 → `PanelSnapshot` → `MochizukiNrControls`（从默认值起手，只覆盖面板能改的字段），值变化才 `SetControls` | 逐 pass、按值比对 |
| `EvaluateFeature` | `ApplyPanelControls(pp)` 排在 `PrepareFrame` 之前；`Reset` 改为按值 | 生效时机正确 |
| `selftest.cpp` | 假 vtable 改为真实槽位顺序 + 不同函数名，并覆盖 float/int/uint/ptr 四类 | 离屏验穿浮点通道 |
| 新增 `abi_probe.cpp` | **直接 include 上游 `nvsdk_ngx_params.h`**（不重写接口），把 shim 分配的对象强转成 `NVSDK_NGX_Parameter*` 后调 8 类 `Set` | 校对"宿主真实会怎么调"，即"amdgx 有没有写错"的硬证据 |
| `build-p2.ps1` | 头文件路径改为相对脚本（原脚本硬编码了另一个 checkout）；新增 `-SelfTest` / `-AbiProbe` / `-Backup`、`-DlssSdkDir` | 可复现构建 + 两层验收 |

导出表仍是 13 个，`GetModuleFileNameW` 静态导入仍在（宿主 IAT 钩子依赖），没有新增导出。

---

## 6. 离屏验收（两层，均已通过）

### 6.1 自测：我们**以为的**布局对不对（`-SelfTest`）

```powershell
powershell -ExecutionPolicy Bypass -File src\nvngx-shim\build-p2.ps1 -SelfTest
```

```
  [ OK ] float 键 slot 5
  [ OK ] float 键 slot 5（第二项）
  [ OK ] int 键 slot 2
  [ OK ] int 键 slot 2（布尔）
  [ OK ] uint 键 slot 3
  参数通道自测通过：浮点/整型/指针三类槽位都取到真值
```
对应日志：
```
P2: DLSSNR.Intensity = 1.25 (slot 5, float)
P2: DLSSNR.LocalToneStrength = 0.75 (slot 5, float)
P2: DLSSNR.Style = 1 (slot 2, int)
P2: DLSSNR.UseAutoMask = 1 (slot 2, int)
P2: DLSSNR.Width = 1920 (slot 3, int)
```

### 6.2 ABI 校对：**上游头文件**认为的布局对不对（`-AbiProbe`）

这一层是回答"amdgx 是不是写错了"的正解：探针不重写接口，直接 `#include "nvsdk_ngx_params.h"`
（即宿主编译时用的那份），把 shim 分配的对象当 `NVSDK_NGX_Parameter*` 用。

```powershell
powershell -ExecutionPolicy Bypass -File src\nvngx-shim\build-p2.ps1 -AbiProbe
# 换机器时用 -DlssSdkDir <DLSS SDK 目录>
```

实际落点（2026-11-04，SDK `nvsdk_ngx_params.h` @ `NGX_VERSION 1.5.0`）：
```
ABI.Float      = 1.25            (slot 5, float)   ← float  → Slot5  ✅
ABI.Double     = 2.5             (slot 4, float)   ← double → Slot4  ✅
ABI.Int        = 7               (slot 2, int)     ← int    → Slot2  ✅
ABI.UInt       = 1920            (slot 3, int)     ← uint   → Slot3  ✅
ABI.Resource12 = 0000000000001234 (slot 0, ptr)    ← ID3D12Resource* → Slot0 ✅
ABI.ULL        = 0000011F71FB04CB (slot 6, ptr)    ← ull    → Slot6（未用到）
（ABI.VoidPtr 无输出 = 落到 vtable[0] 的虚析构，即 §2.3 推论 1，符合预期）
```

> 这条验收同样**不依赖 Magpie、不依赖 9070**。它比 6.1 更强，因为 6.1 测的是我们自己手写的假 vtable，
> 6.2 测的是宿主的真头文件。

### 6.3 实机预检（2026-11-04 20:30，55 s / 2774 帧）

四道门全绿，两份日志逐项一致（详见 `_test-kit/07-面板逐旋钮/00-先做预检.md` 的实测记录）：

```
panel: controls READY struct_size=520
panel: defaults reno{renodx=1 passes=1 scale=1.00 style=0 intensity=1.00 tone=1.00 struct=1.00 skin=-1.00 mask=0}
                model{intensity=1.00 style=0 tone=1.00 struct=1.00 skin=-1.00 mask=1}
P2: DLSSNR.Intensity = 1 (slot 5, float)          ← P2 时代这里是 7FFFFFFFFFFFFFFC
P2: DLSSNR.LocalToneStrength = 1 (slot 5, float)
P2: DLSSNR.LocalStructureStrength = 1 (slot 5, float)
P2: DLSSNR.SkinStructureStrength = 0 (slot 5, float)
panel: setcontrols rc=0 (f=1 n=1) … intensity=1.00 tone=1.00 struct=1.00 skin=-1.00 mask=0 scale=1.00 passes=1(structural)
CALL ReleaseFeature (nrFrames=2774 nrFails=47 passthrough=47 panelCalls=1)
```

| 项 | 宿主 `DLSSNR STATUS` | shim `panel: setcontrols` | 结论 |
|---|---|---|---|
| style / intensity / tone / struct | 0 / 1 / 1 / 1 | 0 / 1.00 / 1.00 / 1.00 | 一致 |
| skinStructure | 0 | skin=-1.00 | 一致（面板 0 = 不表态） |
| autoMask | false | mask=0 | 一致 |
| 层数 | `chain: passes=1` | `passes=1(structural)` | 一致 |

三条要点：
1. **浮点通道实机通了**：全部 float 键都是真值。
2. **首帧下发与运行时默认完全相同**（`panelCalls=1`）⇒ 接线没有改变运行行为，基线干净；
   `reno` 组 `mask` 默认 0 也顺带纠正了 §9 原先的担心。
3. `nrFails=47` 全在建网的 0.9 s 内，之后 2700+ 帧零失败；会话 2774 帧 / 55 s ≈ **50.4 fps**。

---

## 7. 实机验收清单（清单第 2 项，逐旋钮对图）

> 已做成可直接执行的包：`_test-kit\07-面板逐旋钮\`
> —— `00-先做预检.md`（5 分钟，只看日志，不碰旋钮）→ `01-逐旋钮对图.md`（19 格，按成本排序）
> → `grab.ps1 -Step <名字>` 抓一格 / `-Report` 出 markdown 表。
> 本节保留为判据来源。

前置：
- [ ] `D:\Magpie-NR-046beta` 是纯 ASCII 路径（ACO 要求），目录里 `amdgx_dlssnr.dll` 时间戳是今天。
- [ ] 归档旧的 `nvngx-shim.log` / `mochizuki_nr.log` / `logs\magpie.log`（不要删，留着对比）。
- [ ] 效果组：`scalingMode=3` 的 `P0-DLSSNR`（只有一个 `DLSSNR\DLSSNR_AI_Filter`），Multi Pass = 1，网络档 1.0。
- [ ] 场景固定：一张有皮肤/头发的静止画面 + 一段慢速横移；HUD 关闭。

**双日志判据（比单看 shim 日志强，能直接定位断点）**：

| 看哪里 | 是什么 | 断了意味着 |
|---|---|---|
| `logs\magpie.log` 的 `DLSSNR STATUS: … style= intensity= localTone= localStructure= skinStructure= autoMask=` | 宿主**解析到的**面板值（`DLSSNRFilter.cpp:2314`） | 值没变 = 面板 → 宿主这一段断（没提交/改错组/需重启） |
| `nvngx-shim.log` 的 `panel: setcontrols … intensity= tone= struct= skin= mask=` | shim**真正下发**的值 | 宿主变了而这里没变 = 宿主 → NGX 参数这一段断（槽位/键名） |
| 两者都变了但画面没动 | 运行时忽略了该字段 | 多半写错了组：`renodx=1` 时模型组被忽略（§2.4） |

判据分三层，**顺序不能反**：
1. **日志**：上面两条要逐项相等（`skinStructure=0` ↔ `skin=-1.00` 是同一件事的两种写法）；
2. **画面**：同一帧位置截图对比（差分用截图工具即可），要有可感变化；
3. **帧率**：这些控制项不该改变成本 —— 除 `NR Style`（会重建网络）与 `Multi Pass`（会多跑网络）外，
   帧时间应在 ±0.3 ms 内。若变了，说明字段写错了组（写进了会重建网络的项）。

逐个旋钮：

- [ ] **NR 强度**：1.00 → 1.60。期望 `intensity=1.60`，噪点压得更狠、细节更"平"。
- [ ] **NR 强度**：1.00 → 0.60。期望反向变化，能看出残留噪点。
- [ ] **NR 风格**：0 → 1 → 2。期望每次切换有**1–2 秒顿卡**（网络重建）且色调/对比走向不同。
      若没有顿卡，说明 `reno_style` 没写进去（旧运行时不重建）。
- [ ] **局部色调强度**：1.00 → 0.00 与 → 2.00。期望局部对比度明显变弱/变强，整体亮度基本不动。
- [ ] **局部结构强度**：同上。期望细节/纹理强弱变化，噪点不像 tone 那样整体抬。
- [ ] **皮肤结构强度**：0 → 1.00。期望人脸/皮肤区域细节变化；拉回 0 应回到"跟随 Local Structure"。
- [ ] **自动遮罩**：0 ↔ 1。期望皮肤/纯色区域的处理强度分布变化。
- [ ] **重置历史**（宿主内部 Reset，面板无对应旋钮）：切换后画面应有一次"重新收敛"，日志 `fi.reset=1` 当帧可见
      （shim 只在变化时打 `setcontrols`，reset 用 `EVAL` 行的 `reset` 字段核对；若日志没带，按
      "画面重新收敛"判）。
- [ ] **Multi Pass**（未译）：1 → 2。期望**帧时间近似翻倍**（多跑一次网络），画面更干净。这一项同时验证
      "shim 没有重复叠 passes"（若变成 4 倍，说明 `fi.passes` 与宿主 laterPasses 叠加了）。
- [ ] **渲染率（环境变量）**：`set MOCHI_SHIM_SCALE=0.5` 后重启 Magpie。期望帧时间明显下降、
      画面略软。**这是本版才第一次真的有效**；若无效，读 `panel: defaults reno{... scale=...}` 行确认
      运行时是否仍走 renodx 组。

跑完把 `nvngx-shim.log` + 勾选结果带回，并记下每一项的帧时间（`magpie.log` 的 `fenceAvgMs` 或平衡点法）。

---

## 8. 未做：把「渲染率 / renodx 组 / 预处理」也放进面板（需宿主补丁 0004）

为什么必须动宿主：这些键**不在宿主送给 NGX 的参数里**（`SetEvaluateParametersUnsafe` 没有对应
`Set`），shim 无论怎么改都看不到。两条路：

| 方案 | 做法 | 代价 | 取舍 |
|---|---|---|---|
| **B1（推荐）显式导出** | shim 加 `__declspec(dllexport) int32_t MochiShimSetControls(const MochiShimPanelBlock*)`（带 `struct_size`）；宿主在 `DLSSNRFilter::Draw` 里 `GetModuleHandleW(L"amdgx_dlssnr.dll")` + `GetProcAddress` 后每帧/变更时调用 | 宿主重编一次（VS2022 BuildTools，10–20 min）；shim 多一个导出 | 类型安全、可版本化、与 NGX 布局解耦 |
| B2 借指针键 | 宿主 `parameters->Set("DLSSNR.MochiPanel", (void*)&block)`；`Set(const char*, void*)` 走 `Slot0`，shim 直接读结构体 | 不动导出表 | 依赖 Slot0 布局 + 指针生命周期；跨 DLL 传指针比 B1 脆 |

配套改动：
1. `effects/DLSSNR/DLSSNR_AI_Filter.hlsl` 增参数（**这步不需要重编宿主**）：
   `mochiModelScale`（25–100，step 5）、`mochiRenodxStage`（0/1）、`mochiPreprocess`（0/1）+ 曝光/曲线。
2. `DLSSNRSettings` 增字段 + `ParseDLSSNRSettings` 读取 + `GetParameterApplyMode` 标为 `Live`。
3. `DLSSNRFilter::Draw` 组装 `MochiShimPanelBlock` 并下发（与 §3 的数据流并行，不冲突）。
4. 验收：`mochiModelScale=50` 时 `reno_resolution_scale=0.50`，帧时间与 M3 实测对齐。

同时把「面板优先级高于环境变量」写死：面板有值用面板，没有才回落 `MOCHI_SHIM_*`。

---

## 9. 风险与回退

| 风险 | 说明 | 处理 |
|---|---|---|
| **AutoMask 默认值不一致** | ~~运行时默认 1，面板默认 0~~ → **2026-11-04 实机排除**：`reno_auto_mask` 运行时默认就是 0（模型组才是 1），面板默认 0 与其一致，首帧下发 `mask=0` 未改变任何行为 | 已排除；若将来把面板默认改成 1，需回看本条 |
| **Style 触发网络重建** | 运行时说明 style 写进 kernel，改一次重建一次；**逐 pass 不同 style 会连环重建** | 首测让 pass2/pass3 的 style 与 pass1 相同；文档已注明 |
| **per-pass 参数被误当"全局"** | 不同 pass 的强度本就该不同；但如果面板把 pass2 的强度设成与 pass1 不同，会看到"两层强度叠加"而非"两层相同" | 这是设计意图，不是 bug；验收时逐 pass 单独核对日志 |
| **老运行时不支持 SetControls** | 三个导出缺失时整条通道静默失效 | 启动日志 `panel: controls UNAVAILABLE ...` 明示，不静默 |
| **环境变量残留** | 之前测试遗留的 `MOCHI_SHIM_*` 会覆盖面板默认观感 | 首测前 `set MOCHI_SHIM_SCALE=` 等清空 |
| 回退 | 任意一项异常 | `amdgx_dlssnr.dll.prev`（P2 版）覆盖回去即可，宿主无需改动 |
| **资源键若改用 `Set(name, void*)`** | 那个重载落在 vtable[0] 的虚析构上被静默丢弃 ⇒ shim 读不到 Color/Output，走 "missing" 早退（不崩、不黑屏，但不降噪） | 上游当前用 `ID3D12Resource*` 重载（`DLSSNRFilter.cpp:1564-1567`），不触发；若上游改版，日志里 `EVAL #n missing` 会立刻暴露 |
| **`Get` / `Reset` 若被启用** | `Get(...)` 8 个重载落在 vtable[8..15]（即 Slot7 起），会被当成 `Set` 处理；`Reset()` 落在 vtable[16]（Slot15）且**没有 name 参数**，我们会把 RDX 的残值当字符串 → 可能崩 | 上游当前都不调（已核 `DLSSNRFilter.cpp` / `NgxD3D12Core.cpp`）。若上游改版动了这块，先看日志出现莫名的键名或直接崩，用 `amdgx_dlssnr.dll.prev` 回退 |

---

## 10. 与清单的对应

| 清单项 | 状态 |
|---|---|
| 1. 接通 Magpie NR 效果器调节面板 | **本轮完成**（强度/风格/三个结构项/AutoMask/Reset，逐 pass）；渲染率与 renodx 组待 0004（§8） |
| 2. 复测面板有效性（逐旋钮对图） | **预检已过**（§6.3，四道门 + 双日志一致）；逐旋钮 19 格按 `_test-kit/07-面板逐旋钮/01-逐旋钮对图.md` 进行中，§7 为判据来源 |
| 3. 新链 0.5 平衡点补测（钉死 D'） | **前置已修**：`MOCHI_SHIM_SCALE` 本版才真正生效（§2.4），否则 M3 会得到假数据。**⚠️ 新增疑问**：两次独立会话的运行时日志都是 `session end: network 3840x2054 17.75 ms`（P2）/ **`18.05 ms`**（2026-11-04 20:30，p95 18.59、average 18.00，2774 帧 / 55 s ≈ 50.4 fps），而帧时间 19.4–19.8 ms ⇒ GPU 忙占比 ≈0.91–0.92、理论上限 ≈55–56 fps，与里程碑 `I'=13.7`（⇒`D'=5.7`、73 fps 上限）**对不上**。必须先跑 M4（`EnableNativeBackendTiming`）确认这个 "network ms" 是否含 RenoDX 编解码，再决定是否重算里程碑那张表与线 1 的判决（若真是 ~18 ms，按 ratio>0.85 判据应**关闭**线 1，而不是"挂起"） |
| 4. 发布 v0.6 | 前置：1、2、3 |
