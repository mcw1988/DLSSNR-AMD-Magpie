# nvngx_dlssnr.dll 探针桩（P0）

**这不是降噪实现**，是一个只写日志的空壳。目的是在动运行时之前，用最低成本确认
「Magpie 的 `DLSSNR\DLSSNR_AI_Filter` 槽位到底怎么和 DLL 打交道」。

## 为什么这条路成立（源码事实，均指上游 `6940f84`）

| 事实 | 出处 |
|---|---|
| Magpie 从**应用目录**加载 `nvngx_dlssnr.dll`，只检查「文件存在 + 导出齐全」，**没有签名校验**（日志里的 "signed" 只是措辞） | `DLSSNRFilter.cpp:1435-1470` |
| 只需要 5 个导出：`NVSDK_NGX_D3D12_{Init_Ext, CreateFeature, EvaluateFeature, ReleaseFeature, Shutdown1}` | 同上 |
| 参数对象由 DLL 侧分配（`NVSDK_NGX_D3D12_AllocateParameters`），Magpie 只往上 `Set` | `NgxD3D12Core.cpp:40-52` |
| 全程无 VendorId / NVIDIA 适配器判断 | `NativeEffectBackendFactory.cpp:141` |
| **必须静态导入 `GetModuleFileNameW`**：Magpie 会去 DLL 的 IAT 里找它并挂钩（调用方兼容），找不到直接放弃 | `DLSSNRFilter.cpp:834-841` |

因此「在 AMD 上跑」不需要改 Magpie，只需要一个同名 DLL 顶上去。

## 为什么不需要 NVIDIA 的 NGX 头文件

参数对象是我们自己 `new` 出来交给 Magpie 的，**vtable 顺序由我们定义**。所以这里故意
声明 32 个"什么都不懂"的槽位，每个只记 `(slot, name, arg)`：

```cpp
virtual void SlotNN(const char* name, void* a, void* b) { Log("PARAM: slot=%d name=..."); }
```

Magpie 按它自己头文件里的顺序调用，落到哪个槽位就记哪个 —— **布局从日志反推**，
一次运行就能把 `slot ↔ 参数类型` 对上，不需要猜、也不需要那份头文件。

已知的一个正常现象：`float`/`double` 走 XMM 寄存器，日志里 `a=` 会是垃圾值。
这没关系，参数字符串仍会打出来，配合下表就能确认槽位。

## 构建（已验证）

```powershell
powershell -ExecutionPolicy Bypass -File src\nvngx-shim\build.ps1 -SelfTest
```

- 用 VS 2022 Build Tools（本机 `14.44.35207`）+ SDK `10.0.26100.0`，`/MT` 静态 CRT。
- 脚本会自检三件事：**13 个导出齐全**、`GetModuleFileNameW` 静态导入存在、构建产物落盘。
- `-SelfTest` 额外编译 `selftest.exe`，用 Magpie 的调用顺序把探针驱动一遍并打印日志。
  这一步不需要 Magpie、不需要 9070、也不需要 NVIDIA 卡，**任何 Windows 机器都能跑通**。

本机实测输出：

```
导出表自检通过：13/13
GetModuleFileNameW 导入存在（调用方兼容钩子可用）
Init_Ext -> 0x1 / AllocateParameters -> 0x1 / CreateFeature -> 0x1 / EvaluateFeature -> 0x1 ...
```

## 在实机上怎么用（P0 只有一步）

1. 备份原 DLL：把 Magpie 目录里的 `nvngx_dlssnr.dll` 改名成 `nvngx_dlssnr.dll.nvidia`。
2. 把 `build\nvngx_dlssnr.dll` 拷进 Magpie 目录，保持同名。
3. 启动 Magpie，进游戏，效果组里放上 `DLSSNR\DLSSNR_AI_Filter`，开始缩放，**跑 10–20 秒**。
4. 关掉 Magpie，取 Magpie 目录下的 `nvngx-shim.log`。

> **画面可能是黑的或花的**——探针不写输出纹理。这是预期行为，不要当故障排查。
> 日志在 `Init_Ext` 那一刻就已经落盘了，跑几秒足够。

## 日志核对表（P0 的验收标准）

按顺序应该看到：

| 期望看到 | 说明 |
|---|---|
| `PROBE: loaded as "...\nvngx_dlssnr.dll"` | DLL 被 Magpie 加载了（而不是被别的同名文件顶掉） |
| `CALL Init_Ext ...` 随后没有 `failed` | `Init_Ext` 被调用且成功码被接受 → **成功码猜对了**；若 Magpie 日志报 `Init_Ext failed (0x...)`，用 `NGXSHIM_RESULT=0x...` 换一个值重试 |
| `CALL AllocateParameters` / `GetCapabilityParameters` | 参数对象走的是哪条路 |
| 一串 `PARAM: slot=N name="DLSSNR.*"` | 参数键与槽位对照 —— **这是 P0 最值钱的产出** |
| `CALL CreateFeature ... feature=18`（或其它数字） | 特征号；`ENABLE_CORE_FEATURE18_DIAGNOSTIC` 说明 18 是 NR 的诊断号，正式路径可能是别的值，**以实测为准** |
| 每帧一次 `CALL EvaluateFeature` | 每帧调用次数 = 每帧跑几次网络（多趟时应为 `multiPass` 次） |
| `PROBE: unloaded` | 干净卸载 |

**判定**：
- 出现 `Init_Ext` + `AllocateParameters` + 一串 `PARAM` → ABI 形状与读码结论一致，P0 通过；
- 卡在 `loaded` 之后没有 `Init_Ext` → Magpie 在加载阶段就放弃了，去看 `logs\magpie.log` 里的
  `DLSSNR` 行（多半是导出名或 `GetModuleFileNameW` 的问题）；
- 有 `Init_Ext` 但 Magpie 报失败 → 只可能是成功码不对，用环境变量换值。

## 明确不做的事

- 不做任何降噪，不碰输出纹理；
- 不 `delete` 参数对象（泄漏一个对象，换取"不会在 Magpie 还持有它时被释放"）；
- 不实现 `Get` 语义（Magpie 只 `Set`，DLL 侧才需要 `Get`）。

## 下一步

P0 的日志回来后：

- **P1**：在 `EvaluateFeature` 里把 `DLSSNR.Color` 拷到 `DLSSNR.Output`（直通），验证
  链路、面板、抗闪烁、多趟是否都正常 —— 即"槽位成立"；
- **P2**：把 `MochizukiNrRuntime` 接进来（`Create(device, queue, assets)` +
  `PrepareFrame/RecordInputs/EnqueueHip/RecordOutputs/Retire`，代码在 `src/nrtest`、`src/mochinr` 已有）。
