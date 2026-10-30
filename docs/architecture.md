# 架构与原理

## 1. 全链路

```
游戏窗口 ──捕获──> Magpie.exe（缩放宿主，0.6.x）
                     │  效果链：NR-lmxxf / NR-mochizuki 组
                     │  ├ FSR4\FSR4_SR  或  XeSS\XeSS_SR   （SR 台阶，只占位）
                     │  ├ FrameRate_Filter
                     │  └ XeSS 多帧生成
                     │
                     └─ Present ──> dxgi.dll = ReShade 6.8（同一进程内注入）
                                      ├ add-on: dlss5-amd.addon64  → lmxxf 神经网络（HIP）
                                      └ add-on: amd-nr.addon64     → MochizukiNrRuntime.dll（Vulkan）
                                         （两者互斥，同一时刻只有一个被加载）
```

ReShade 是**唯一注入点**（`dxgi.dll` 被 `Magpie.exe` 静态导入）。NR 不在 Magpie 的 HLSL
效果里，而是在 ReShade 的 present 阶段改写帧——所以那两个 `*_SR.hlsl` 只是占位存根
（935~937 字节的 1:1 拷贝），它们的作用是**建立 SR 台阶与纹理尺寸**，不参与像素替换。

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
- 面板打开期间：每个按键转发 `WM_KEYDOWN/UP` + `WM_CHAR`（`ToUnicode`），鼠标位置与
  左/右/中键由轮询转发（钩子漏事件也能跟上），滚轮从钩子里转发。
- `Swallow=1` 时原始事件直接吞掉（返回 1），游戏不会同时收到你在面板上的按键与点击。

坐标只用于 `lParam`；ReShade 实际取的是消息的 `MSG.pt`（真实光标位置），所以不要试图在
`MSLLHOOKSTRUCT.pt` 上做换算——那是**物理像素**，高 DPI 下会和客户区坐标打架。

## 4. 两条运行时，为什么只能“加载期互斥”

| | lmxxf | mochizuki |
|---|---|---|
| 形态 | 引擎**编在 host 里**（`dlss5-amd.addon64`），没有可被别的宿主调用的裸 DLL | 独立 `MochizukiNrRuntime.dll`，导出 `LmxxfNrGetApi`（ABI v1，`struct_size 0x88`） |
| 图形 API | HIP（需驱动自带 `amdhip64_7.dll`） | Vulkan 1.3 + `VK_EXT_shader_float8`，与 D3D12 共享 NT handle |
| 权重 | `DLSS5-AMD\native-game-tiled-assets`（约 594 MB） | `dlssnr-amd\`（约 286 MB） |

两个 add-on 都在改帧，同时加载会互相抢帧。而 **ReShade 只在进程启动时扫描 `*.addon64`**，
覆盖层里的勾选也只会写进 `ReShade.ini` 的 `DisabledAddons`，UI 明确写着
`(will be enabled on next application restart)`（`source/runtime_gui.cpp`）。官方 OpenNR
自己的运行时选择器同样标注 `from the next launch` —— **没有任何一条路能免重启切换**。

所以互斥放在**文件层**：同一时刻只有一个 `*.addon64`，另一个叫 `*.off`。这是唯一能保证
“物理上不可能同时开”的位置。

## 5. `nr-router` 的判定

1. 读 `config\config.json` → `profiles[0].scalingMode` 得到当前组下标；
2. 取 `scalingModes[n].effects`，在**组对象内部**匹配（不按组名，改名不影响）：
   - 含 `"XeSS\\XeSS_SR"` → mochizuki → 要 `amd-nr.addon64`
   - 含 `"FSR3\\FSR3_SR"` 或 `"FSR4\\FSR4_SR"` → lmxxf → 要 `dlss5-amd.addon64`
   - 两者都有 → **拒绝执行**，退出码 2，不动任何文件
   - 都没有 → 与 NR 无关，保持现状
3. 把不需要的那个改成 `*.off`，需要的改回来（幂等，已经正确就不动）。
4. `--watch` 每 2 秒复查；`--restart` 在变更后按进程名找 `Magpie.exe`，`WM_CLOSE`
   等 5 秒，超时才强杀，然后原样重启。

## 6. `nrtest` 的调用序列

独立 D3D12 宿主，不经过 Magpie / ReShade，用来把“运行时能不能跑”与“集成对不对”分开：

```
D3D12CreateDevice → 建 1920x1080 RGBA8 渐变贴图
LoadLibrary(MochizukiNrRuntime.dll) → LmxxfNrGetApi(1, &api)（校验 struct_size=0x88）
QueryCapabilities → Create(d3d12Device, d3d12Queue, assetsDir) → PrepareSession
循环 PrepareFrame（首次建网会返回 UNAVAILABLE，最多等 15 分钟）
RecordInputs → ExecuteCommandLists → EnqueueHip → Poll → RecordOutputs → Retire
回读颜色，打印与输入的差异均值
```

第一次跑会触发管线编译（数十秒到数分钟），期间会写 `dlssnr-amd\build.pending`。
**中途杀掉会留下这个文件**，之后宿主会判定“上次建网时崩溃”并退回 danielblnc —— 删掉它即可。

## 7. 已知边界

- 切换运行时必须重启 Magpie（见 §4）；Magpie 不记上次捕获的窗口，需要重选或按缩放热键。
- lmxxf 需要 AMD HIP 7（Adrenalin 自带）；mochizuki 需要 RDNA4 与 FP8 协作矩阵。
  NVIDIA 上 mochizuki 会在建网阶段报
  `e4m3 x e4m3 -> fp32 16x16x16 subgroup config not reported`，这是硬件不匹配，不是配置错误。
- Magpie 的缩放热键若被别的程序占用，日志会写 `注册热键 Scale 失败 LastErrorCode: 1409`，
  此时只能用界面按钮启动缩放。
