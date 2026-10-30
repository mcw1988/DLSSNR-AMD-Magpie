# 使用

## 1. 目录布局

把下面这些放进**同一个目录**（就是 `Magpie.exe` 所在目录，ReShade 以 `dxgi.dll` 注入的进程目录）：

```
Magpie.exe
dxgi.dll                        ReShade 6.8（"with full add-on support" 构建，改名而来）
Microsoft.UI.Xaml.dll ...       Magpie 自带依赖

dlss5-amd.addon64               lmxxf 运行时（HIP）      ┐ 同一时刻只允许一个存在为 .addon64
amd-nr.addon64.off              mochizuki 宿主            ┘ 另一个必须是 .off，由 nr-router 维护
DLSS5-AMD\native-game-tiled-assets\    lmxxf 权重（约 594 MB）
MochizukiNrRuntime.dll          mochizuki 运行时
dlssnr-amd\                     mochizuki 权重与 SPIR-V（约 286 MB）

overlay-input.addon64           输入桥
overlay-input.ini               Key=36（Home）/ Swallow=1
nr-router.exe  nr-router.cmd    效果组 → 运行时的互斥路由

amd-nr.ini                      [amd-nr] NrBackend=mochizuki, StartOn=1
config\v4e\config.json          Magpie 0.6.11 的实际配置（效果组、节奏、限帧都在这里）
config\config.json             旧版遗留文件；0.6.11 只在迁移时读它，改它不生效
```

> 0.6.11 把设置拆进了 `config\v4e\`。手改配置时务必改 `v4e\config.json`，
> 改 `config\config.json` 会被静默忽略。

## 2. 两个效果组

在 Magpie 里建两个缩放模式（`nr-router` 先看组名，再回退到组内效果名）：

| 组 | 效果链 | 对应运行时 |
|---|---|---|
| `NR-lmxxf` | `FSR4\FSR4_SR` + `FrameRate_Filter` + `XeSS\XeSS_MultiFrameGeneration_ZeroMV` | lmxxf（HIP） |
| `NR-mochizuki` | `XeSS\XeSS_SR` + `FrameRate_Filter` + `XeSS\XeSS_MultiFrameGeneration_ZeroMV` | mochizuki（Vulkan） |

`*_SR` 都是 1:1 拷贝存根，只用来建立 SR 台阶与纹理尺寸，不参与像素替换 —— NR 是在 ReShade
的 present 阶段做的。

组名里含 `mochi` → mochizuki；含 `lmxxf` / `DLSS5` → lmxxf。名字看不出来时，
才回退到「组里有没有 `XeSS_SR` / `FSR3/FSR4_SR`」——所以把 SR 存根删掉也能正确判定。

**同一个组里不允许同时出现 `XeSS\XeSS_SR` 与 `FSR3/FSR4_SR`**：`nr-router` 会拒绝执行并
以退出码 2 报错（“按规则不允许”）。

## 3. 切换运行时（必须重启 Magpie）

ReShade 只在进程启动时加载 add-on，所以：

1. Magpie 里**停止缩放**（对目标窗口取消启用）；
2. 换到另一个效果组；
3. 双击 `nr-router.cmd` → 它会先打印判定结果，问你要不要立刻生效 → 按 `Y`
   → 换文件 + 自动重启 Magpie；
4. 重新选窗口 / 按缩放热键，继续缩放。

> **为什么必须重启**：OpenNR 收到 swapchain/设备销毁后会把自己置为 off，且不会自愈
> （`off: the game destroyed its D3D12 device; restart the game to re-enable`）。
> Magpie 每次停缩放都会销毁设备，所以"停一下再开"不够，必须换进程。
> `nr-router --restart` 现在无论文件是否变化都会重启，就是为了这一步。

想省掉第 3 步：让 `nr-router.exe --watch --restart` 常驻（可开机自启）。换完组 2 秒内
自动换文件并重启 Magpie。缺省不这么做，是因为换组瞬间画面会断一下，容易被误认为崩溃。

只看判定、不动文件：

```
nr-router.exe --dry-run
```

## 4. ReShade 面板

- 按 **Home** 打开（这是 `overlay-input` 让它能工作的；没有它，Magpie 的窗口永远收不到按键）。
- **插件**页里能看到 `DLSS5 AMD single-frame verification`（lmxxf）、`overlay-input`、
  `OpenNR`（mochizuki 宿主）。NR 参数在对应 add-on 的页面里调。
- 覆盖层打开期间，`overlay-input` 会吞掉原始键鼠，游戏不会同时响应；再按一次 Home 关闭。
- LMxxf 的开关热键是它自己的 F6/F7/F9；OpenNR 的是 Ctrl+End / `amd-nr.ini` 里的 `StartOn`。

## 5. 参数落在哪

| 文件 | 管什么 |
|---|---|
| `amd-nr.ini` | mochizuki 宿主：`NrBackend=mochizuki`、`StartOn`、`Scale`、`Passes`、`MochizukiInt4`、`MochizukiAco`、UI 语言等 |
| `overlay-input.ini` | 面板热键（`Key`，虚拟键码，36=Home）与是否吞输入（`Swallow`） |
| `DLSS5-AMD\default-config.txt` | lmxxf 的调参面（升级会覆盖）；个人改动写 `custom-config.txt` |
| `config\v4e\config.json` | Magpie 效果组、`profiles[0].scalingMode`、节奏与限帧（0.6.11） |

## 6. 先验运行时（不启动 Magpie）

```
tools\nrtest.exe "<Magpie 目录>"
```

它会建一个自己的 D3D12 设备和队列，直接对 `MochizukiNrRuntime.dll` 跑一帧。
量成本用 `nrbench.exe "<目录>" 1920 1080 40 aco 0.5`（宽 高 帧数 是否 ACO 网络缩放）：

- 成功：`PrepareFrame rc=0`、`private_output` 非 0、末尾打印与输入的差异均值；
- 首次会编译管线，可能要 1~5 分钟，窗口没反应是正常的，**别中途关掉**；
- 失败：打印 `rc` 与 `last_error`，并把原因写进 `mochizuki_nr.log`。

跑之前请先关掉 Magpie（两者都要占 1 GB 以上显存）。

## 7. 日志

| 文件 | 看什么 |
|---|---|
| `amd-nr.log` | 宿主选了哪个运行时、为什么拒绝、每帧耗时 |
| `mochizuki_nr.log` | Vulkan 运行时自己：设备选择、显存、建网（`building CC_SMALL_FP8 at 1920x1080`）、失败原因 |
| `overlay-input.log` | 是否拿到 swapchain 窗口、面板开关、转发事件 |
| `ReShade.log` | ReShade 初始化、add-on 注册（`Registered add-on "..." using ReShade API version 20`） |
| `logs\magpie.log` | Magpie：效果加载、缩放会话、热键注册失败（`1409`） |
