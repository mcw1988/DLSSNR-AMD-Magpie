# 排错

先看日志，再看现象。每条都能在日志里找到一句原话。

## Home 按了没反应

1. 看 `overlay-input.log`：
   - 只有 `loaded` 没有 `swapchain window ...` → 它没拿到窗口句柄（ReShade 还没 present 过，
     或 add-on 在 ReShade 之前被禁用）。确认 `ReShade.log` 里有
     `Registered add-on "overlay-input" ...`。
   - 有窗口号、按 Home 后没有 `overlay input ON` → 按键没到钩子。多半是**另一个程序也用这个键并且
     抢先**，或者你的键盘软件把它拦了；换一个 `Key`（`overlay-input.ini`，虚拟键码）并同步改
     `ReShade.ini` 的 `[INPUT] KeyOverlay`。
2. 面板开了但鼠标点不动：`Swallow=0` 试试；仍不行就看日志里有没有连续的 `mouse msg=0200`
   （有就说明钩子在收事件）。

## `注册热键 Scale 失败 LastErrorCode: 1409`

`1409 = ERROR_HOTKEY_ALREADY_REGISTERED`：这个组合键被**别的程序**长期占用（远程桌面、输入法、
截图工具都可能）。后果是 Magpie 的缩放热键按不出来，只能用主界面按钮启动缩放。
换组合键即可，与 NR 无关。

## mochizuki 建网失败：`e4m3 x e4m3 -> fp32 16x16x16 subgroup config not reported`

设备不支持 FP8 协作矩阵。这条只在 **RDNA4** 上成立；在 NVIDIA/Intel 或更老的 AMD 卡上一定失败。
判断方法：`mochizuki_nr.log` 开头会写选到的 Vulkan 设备名。

## `no Vulkan device matches the game's D3D12 adapter`

运行时按 LUID 找与 D3D12 同一块显卡的 Vulkan 设备，没找到。常见于双显卡笔记本（核显负责显示、
独显负责渲染）或驱动装重了。`mochizuki_nr.log` 会列出它看到的设备。

## 上次建网时“崩溃”，之后一直退回 danielblnc

`dlssnr-amd\build.pending` 是建网过程中的标记文件；进程被杀就会留在那里，宿主据此判定上次崩过。
**删掉它**再试。第一次建网要 1~5 分钟，别中途关窗口。

## 两条运行时“同时生效”/画面被改两次

`ReShade.log` 里同时出现两个 add-on 的注册行就是没互斥：

```
Registered add-on "DLSS5 AMD single-frame verification" ...
Registered add-on "OpenNR" ...           ← 不该同时出现
```

跑 `nr-router.exe --dry-run` 看它认为该是谁，再跑一次让它改文件，**然后重启 Magpie**
（ReShade 只在进程启动时扫 add-on）。

## 换了组，跑的却还是上一个运行时

同上：换组不会换 add-on。必须 `nr-router` 改文件名 + 重启 Magpie。用
`nr-router.exe --watch --restart` 可以让它在 2 秒内自动做完。

## 换完组，画面没降噪：NR 其实已经停了

`amd-nr.log` 里反复出现这两行，就是它自己关了：

```
swapchain going away (resize 0) after N frames
off: the game destroyed its D3D12 device; restart the game to re-enable
```

Magpie 每次**停止缩放**都会销毁 D3D12 设备，OpenNR 收到后把自己置为 off，而且**不会自愈**
（后面的 `swapchain back ...` 只重建桥接，不会把效果打开）。

处理：换完组跑一次 `nr-router.cmd`（它会重启 Magpie），或从托盘退出 Magpie 再打开。
"停掉捕获再开始"不够——那只是又销毁了一次设备。

## 切到 3x 补帧后出现 D3D12 红字 / 一直不恢复

3x 下每个内容帧会被呈现 3 次，NR 也就评估 3 次。4K 上这个量已经超过桥接的在飞预算
（addon 里有 `four jobs already in flight`、`network skipped: the last list that read the
descriptor heap is still on the GPU` 这类保护），随后停机并报设备错误。

先退回 2x；确实要用 3x，需要先把 NR 单次成本压下来（`Scale`、`Motion`）。

## ACO 明明有文件却不生效

`amd-nr.log` 写 `ACO asked for, but ...\aco has no records\ and shaders\`，而文件其实在。
**路径里有非 ASCII 字符**（例如 `D://迅雷下载//...`）就会这样。把整个实例放到纯 ASCII 目录
（如 `D://Magpie-NR-045`）即可，ACO 日志会变成 `ACO: the network runs ACO's machine code`。

## `already running` / 程序启动后立刻退出

`logs\magpie.log` 里写 `已经有一个实例正在运行`。Magpie 是单实例：先退出旧的。
（可以用托盘图标退出，任务管理器强杀会留下未写的配置。）

## 显存不足

`mochizuki_nr.log` 会写 `VRAM for the network at 1920x1080: about 1367 MB needed, N MB available`。
1080p 单 pass 约 1.4 GB；`dlssnr-amd\pipeline.cache` 与 `prewarm\manifest.txt` 只是编译缓存，
删掉不会省显存但会导致下次重建（慢）。降 `Scale` 或用 `MochizukiInt4=1`（需要 `aco\` 与 `int4\`）。

## `pipeline.cache` 是别的显卡留下的

`dlssnr-amd\pipeline.cache` 是 Vulkan 管线缓存，与驱动/设备绑定。换了显卡或驱动后它没用，
驱动会自己忽略；但如果出现奇怪的建网失败，先删掉它再试。

## 我已经把 `*_SR` 效果从组里删了，还有影响吗

画面不变，但会让 `nr-router` 的**回退判定**失效：它先看组名（含 `mochi` / `lmxxf` / `DLSS5`），
名字看不出来时才靠 `XeSS_SR` / `FSR3/FSR4_SR` 判断。组名规范的话，删掉存根也能正确切换；
组名是中文或自定义的，就保留存根，否则它会把该组当成“与 NR 无关”。

## 需要提供的材料

出问题时把这些一起带上，基本能定位到底：

```
amd-nr.log  mochizuki_nr.log  overlay-input.log  ReShade.log  logs\magpie.log
+ nr-router.exe --dry-run 的输出
+ 显卡型号与驱动版本（Win32_VideoController）
```
