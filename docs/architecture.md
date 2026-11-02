# 架构与原理

## 1. 全链路（v0.4）

```
游戏窗口 ──捕获──> Magpie.exe（Experimental 0.6.11，D3D11 渲染）
                     │
                     │  效果链 NR-preupscale：FSR4\FSR4_SR(占位) + mochinr + 帧率过滤 + 补帧
                     │        │
                     │        └─ ffxDispatch 被 mochinr 拦截 → 只记录、不执行 → return 0
                     │
                     └─ ExecuteCommandLists ──> 批次劈成两段：
                                                 [0..at] 提交 → 网络跑完 → [at+1..] 提交
                                                 （其中重放原 ffxDispatch，输入指向网络输出）

注入点：Magpie.exe 静态导入 dxgi.dll = ReShade 6.8.0.2155（唯一入口）
add-on：mochinr.addon64 / dlss5-amd.addon64 / amd-nr.addon64 —— 同一时刻只加载一个
```

- NR **不在** Magpie 的 HLSL 链里：`FSR4\FSR4_SR` / `XeSS\XeSS_SR` 都是 935~937 字节的 1:1 拷贝
  存根，只负责建立 SR 台阶与纹理尺寸。真正的降噪在 `mochinr` 里、在放大之前。
- v0.3 曾把 mochizuki 挂在 ReShade 的 present 层（`amd-nr.addon64`），代价与退役原因见 §9。

## 2. 为什么 Home 键在 Magpie 里失效

Magpie 的输出窗口按设计不接受输入（`src/Magpie.Core/ScalingWindow.cpp`）：

```cpp
CreateWindowEx(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, ...)  // 缩放主窗
CreateWindowEx(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | ...) // 渲染子窗，swapchain 在此
case WM_MOUSEACTIVATE: return MA_NOACTIVATE;   // 注释：「缩放窗口不应该得到焦点」
```

而 ReShade 的输入**完全由窗口消息驱动**（`source/input_windows.cpp` 的
`handle_window_message()` 只处理 `WM_KEYDOWN/UP`、`WM_MOUSEMOVE`、`WM_INPUT`），它自己
不安装任何全局键盘钩子。窗口没有焦点、子窗口又是鼠标穿透，消息根本不存在 —— 所以 Home 按了
也没人听。这也解释了为什么包内另一个 add-on 的 F6/F7/F9 能用：它在自己代码里挂了底层钩子。

## 3. `overlay-input` 的做法

```
DllMain → 线程 1：SetWindowsHookEx(WH_KEYBOARD_LL / WH_MOUSE_LL) + 消息泵
        → 线程 2：15 ms 轮询（仅在面板打开时）：GetCursorPos / GetAsyncKeyState
present 事件 → 记下 swapchain->get_hwnd()（ReShade 就是在这个窗口上收消息）
```

- 热键（默认 `VK_HOME`）按下时：切换状态，并向该窗口 `PostMessage` 一对
  `WM_KEYDOWN` / `WM_KEYUP`，中间隔 **90 ms** —— ReShade 用 `is_key_pressed` 判断
  “本帧按下”，down/up 落在同一帧会被抵消，所以必须跨帧。
- 面板打开期间：每个按键转发 `WM_KEYDOWN/UP` + `WM_CHAR`（`ToUnicode`），滚轮从钩子里转发。
- `Swallow=1` 时原始事件直接吞掉（返回 1），游戏不会同时收到你在面板上的按键与点击。

两处坑（2026-10-31 实机修掉，见 [2026-10-31-session.md](2026-10-31-session.md)）：

1. **`MSG.pt` 不等于 `lParam`**。对投递出来的消息，系统用**当前光标位置**填 `MSG.pt`，
   而 ReShade 正是拿 `MSG.pt` 做 `ScreenToClient`。所以我们一律按**屏幕坐标**投递，
   并且以底层钩子里的真实事件为准；轮询只作兜底（覆盖层打开后 ReShade 会屏蔽
   `GetAsyncKeyState`、丢弃部分 `WM_MOUSEMOVE`）。
2. **按下与抬起必须分帧**，否则 `is_key_pressed` 判定不成立（与上面的 90 ms 同源）。

## 4. 三个后端，为什么只能“加载期互斥”

| | `mochinr`（v0.4 主力） | lmxxf | OpenNR（已退役） |
|---|---|---|---|
| 位置 | FFX 放大调用内部 | FFX 放大调用内部 | ReShade present 层 |
| 形态 | 本仓库自建 add-on，调 `MochizukiNrRuntime.dll`（ABI v1，`struct_size 0x88`） | 引擎编在 host 里（`dlss5-amd.addon64`），无可复用的裸 DLL | `amd-nr.addon64`（OpenNR + 本仓库补丁） |
| 图形 API | D3D12 ↔ Vulkan（VK 1.3 + `VK_EXT_shader_float8`） | HIP（`amdhip64_7.dll`） | D3D12 → Vulkan |
| 权重 | `dlssnr-amd\`（286 MB，ACO + FP8） | `DLSS5-AMD\native-game-tiled-assets`（594 MB） | 同 mochizuki |

`mochinr` 与 lmxxf **钩的是同一个 `ffxDispatch` 导出**，同时加载必然互相抢帧。而 ReShade 只在
进程启动时扫描 `*.addon64`，覆盖层的勾选也只写进 `ReShade.ini` 的 `DisabledAddons`，UI 明确
写着 `(will be enabled on next application restart)`；官方 OpenNR 的运行时选择器同样标注
`from the next launch` —— **没有任何一条路能免重启切换**。

所以互斥放在**文件层**：同一时刻只有一个 `*.addon64`，其余叫 `*.off`。这是唯一能保证
“物理上不可能同时开”的位置。规则见 §6。

## 5. 同步发生在哪一层（别把上游的活儿算成自己的）

一句话：**Vulkan ↔ D3D12 的互操作是 mochizuki 运行时自己做的，不是本项目写的。**

| 环节 | 谁实现 | 证据 |
|---|---|---|
| D3D11 → D3D12（v0.4） | **Magpie 的 `FSR3Upscaler`**，不是本项目 | `CopyResource(sharedInput11, input)`、`Signal(fence11)`/`Flush`、`queue12->Wait`、`OpenSharedHandle`（`FSR3Upscaler.cpp:186-215/498-509/575-579`） |
| D3D12 共享句柄 → Vulkan 内存/图像 | `MochizukiNrRuntime.dll` | 它导入 `vkGetMemoryWin32HandlePropertiesKHR`、`vkImportSemaphoreWin32HandleKHR`，创建时间线信号量 |
| Vulkan 提交与完成通知（`EnqueueHip` 的往返） | 同上 | 面板「计时」页的 **Vulkan 围栏** 一栏（实机 ≈0.09 ms）就是等这一次往返 |
| 提交节奏 + `Retire` 延迟 | **本仓库**（`src/mochinr/mochinr.cpp`） | 在宿主队列的设备上 `CreateFence`，`Signal` / `WaitSlot` / `GetCompletedValue`，保证 GPU 读完才释放运行时资源 |
| D3D11 ↔ D3D12 共享围栏（v0.3，present 层） | **本仓库**（`src/opennr-patches/`） | 补丁里的 `crossFence` / `ringFence` / `backFence`：Magpie 是 D3D11，桥接队列是 D3D12 |

也就是说：*“自建 Vulkan-D3D12 围栏”这个说法不准确*。本项目自建的是 **D3D12 侧的一条节奏围栏**
与 **D3D11↔D3D12 的共享 cross fence**；Vulkan 那一半（D3D12 句柄导入 + 时间线信号量）在运行时里，
我们只是**等**它，并把等待时间计进 `enqueue`。

v0.4 换了挂载点之后，这条链的归属再变一次：**D3D11→D3D12 完全交给 Magpie**（`FSR4_SR` 这个效果器
自己的实现），因此 v0.3 补丁里的 `crossFence` 那一套在 v0.4 **不再参与**；本项目只剩"把颜色交给
运行时、把放大器的 color 指向运行时的输出"两件事。三段拆解、每段的拷贝次数与待测量见
[bridge.md](bridge.md)。

## 6. `nr-router` 的判定（v0.4）

1. 读 `config\v4e\config.json`（0.6.11）或 `config\config.json`（旧版）→ `profiles[0].scalingMode`；
2. 取该组的 `name` 与 `effects`，**组名优先**：

   | 组名含 | 启用 | 位置 |
   |---|---|---|
   | `mochinr` / `preupscale` | `mochinr.addon64` | FFX 放大之前 |
   | `lmxxf` / `dlss5` | `dlss5-amd.addon64` | FFX 放大之前（lmxxf） |
   | `mochi`（且不含 `mochinr`） | `amd-nr.addon64` | present 层（仅兼容旧安装） |

   没有组名标记时按效果器判定：`XeSS\XeSS_SR` → `amd-nr`；`FSR3/FSR4_SR` → `dlss5-amd`
   （两个 FSR 后端钩同一个导出，仅凭效果链无法区分，故保留旧行为并打印提示）。
3. 组名同时出现两个后端标记、或链里同时挂 `XeSS_SR` 与 FSR → **拒绝，不动任何文件**（退出码 2）；
4. `--watch` 每 2 秒复查；`--restart` 按进程名找 `Magpie.exe`，`WM_CLOSE` 等 5 秒，
   超时才强杀，然后原样重启。

## 7. `nrtest` / `nrbench`

独立 D3D12 宿主，不经过 Magpie / ReShade，把“运行时能不能跑”与“集成对不对”分开：

```
D3D12CreateDevice → 建渐变贴图
LoadLibrary(MochizukiNrRuntime.dll) → LmxxfNrGetApi(1, &api)（校验 struct_size=0x88）
QueryCapabilities → Create(d3d12Device, d3d12Queue, assetsDir) → PrepareSession
循环 PrepareFrame（首次建网返回 UNAVAILABLE，最多等 15 分钟）
RecordInputs → ExecuteCommandLists → EnqueueHip → Poll → RecordOutputs → Retire
```

`nrbench` 是同一套流程跑 N 帧，把 prepare / 生产拷贝 / 网络 / 消费拷贝拆开计时，
用来对照上游权重自己给出的离线单帧 ms 数。

第一次跑会触发管线编译（数十秒到数分钟），期间会写 `dlssnr-amd\build.pending`。
**中途杀掉会留下这个文件**，之后宿主会判定“上次建网时崩溃”并退回 danielblnc —— 删掉它即可。

## 8. 已知边界

- 切换后端必须重启 Magpie（见 §4）；Magpie 不记上次捕获的窗口，需要重选或按缩放热键。
- `mochinr` 需要 RDNA4 与 FP8 协作矩阵；NVIDIA 上会在建网阶段报
  `e4m3 x e4m3 -> fp32 16x16x16 subgroup config not reported`，是硬件不匹配。
- ACO 判定不兼容非 ASCII 路径，部署目录必须是纯 ASCII。
- Magpie 的缩放热键若被别的程序占用，日志会写 `注册热键 Scale 失败 LastErrorCode: 1409`。

## 9. v0.3 的历史结论：present 阶段的代价模型（已退役）

v0.3 的 NR 挂在 ReShade 的 present 上，看到的是**补帧之后**的输出帧，于是：

```
游戏渲染 → Magpie 捕获 → XeSS_SR(1:1) → FrameRate_Filter → XeSS FG(xN) → Present
                                                                          ↑
                                                              NR 在这里，被调用 N 次
```

- **补帧倍率 = NR 成本倍率**：FG 2x 时一个内容帧呈现 2 次，NR 就跑 2 次（实测 3311 帧 / 49.1 s
  vs 33.6 内容帧 = 2.00）。
- **`Scale` 只缩网络**：光流和入出拷贝仍按帧尺寸算，所以 0.6→0.5 只省约 1.9 ms/次。
- **网络只是一部分**：4K、1080 档、无补帧时网络 3.27–3.40 ms，而整个 add-on 阶段约 15 ms/帧，
  其中 3.8 ms 是网络之外（4K 光流 + 双向拷贝 + D3D12↔Vulkan 桥接）。

v0.4 把 NR 挪到放大之前，上面三条的适用条件都变了（补帧不再乘算 NR，光流也可以挪到补帧效果上），
但**新数字尚未测全**，见 [measurements.md](measurements.md) 与 [findings.md](findings.md)。
