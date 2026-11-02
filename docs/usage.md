# 使用（v0.4）

## 1. 目录布局

以下文件放在**同一个目录**（`Magpie.exe` 所在目录；ReShade 以 `dxgi.dll` 注入的进程目录）。
**目录路径必须是纯 ASCII**，否则 mochizuki 的 ACO 判定会失败。

```
Magpie.exe                       Magpie Experimental 0.6.11
dxgi.dll                         ReShade 6.8（"with full add-on support"），改名而来
Microsoft.UI.Xaml.dll ...        Magpie 自带依赖

mochinr.addon64                   v0.4 主力：FFX 槽位，调 MochizukiNrRuntime.dll   ┐ 三个后端
dlss5-amd.addon64.off             lmxxf（HIP），FFX 槽位                          ├ 同一时刻
amd-nr.addon64.off                OpenNR present 层（已退役）                      ┘ 只有一个 .addon64

MochizukiNrRuntime.dll            mochizuki 运行时
dlssnr-amd\                       权重与 SPIR-V（约 286 MB）
DLSS5-AMD\native-game-tiled-assets\   lmxxf 权重（594 MB，不在发布包内）

overlay-input.addon64             输入桥（Home 与鼠标）
overlay-input.ini                 Key=36（Home）/ Swallow=1
nr-router.exe  nr-router.cmd      效果组 → 后端的互斥路由
mochinr.ini                       面板自动写入；手改后用 Ctrl+Home 重载
config\v4e\config.json            Magpie 0.6.11 的效果组与 profile
config\config.json               旧版遗留；0.6.11 只在迁移时读它，改它不生效
```

> 0.6.11 把设置拆进了 `config\v4e\`。手改配置时务必改 `v4e\config.json`。

## 2. 效果组（v0.4 是三个）

| 组名 | 效果链 | 后端 |
|---|---|---|
| `NR-preupscale` | `FSR4\FSR4_SR` + `mochinr` | **mochizuki（默认）** |
| `NR-lmxxf` | `FSR4\FSR4_SR` + lmxxf | lmxxf（HIP，对照实例） |
| `仅补帧` | 帧率过滤 + 补帧 | 不挂 NR，用来单独测补帧成本 |

`*_SR` 都是 1:1 拷贝存根，只用来建立 SR 台阶与纹理尺寸；降噪发生在 **FFX 放大调用内部、放大之前**。

`nr-router` **先看组名**：含 `mochinr`/`preupscale` → mochinr；含 `lmxxf`/`dlss5` → lmxxf；
含 `mochi` 但不含 `mochinr` → present 层的 `amd-nr`（仅兼容旧安装）。看不出来时才回退到组内效果名
（`XeSS_SR` → amd-nr，`FSR3/FSR4_SR` → lmxxf），所以把 SR 存根删掉也能正确判定。

**别把组名改成同时命中两个标记**（例如 `NR-preupscale-lmxxf`）：`nr-router` 会拒绝执行并以
退出码 2 报错，不动任何文件。同理，一个组里不允许同时出现 `XeSS\XeSS_SR` 与 `FSR3/FSR4_SR`。

## 3. 切换后端（必须重启 Magpie）

ReShade 只在进程启动时加载 add-on，所以：

1. Magpie 里**停止缩放**（对目标窗口取消启用）；
2. 换到另一个效果组；
3. 双击 `nr-router.cmd` → 看判定结果 → 按 `Y` → 换文件 + 自动重启 Magpie；
4. 重新选窗口 / 按缩放热键，继续缩放。

> **为什么必须重启**：宿主收到设备销毁后会把 NR 置为 off，且不会自愈
> （`off: the game destroyed its D3D12 device; restart the game to re-enable`）。
> Magpie 每次停缩放都会销毁设备，所以“停一下再开”不够，必须换进程。
> `nr-router --restart` 无论文件是否变化都会重启，就是为了这一步。

想省掉第 3 步：`nr-router.exe --watch --restart` 常驻（可开机自启），换完组 2 秒内自动换文件并
重启 Magpie。缺省不这么做，是因为换组瞬间画面会断一下，容易被误认为崩溃。

只看判定、不动文件：`nr-router.exe --dry-run`。缺文件时它会**拒绝整批操作**（退出码 2），
不会把唯一可用的后端也停掉。

## 4. ReShade 面板

- 按 **Home** 打开覆盖层（`overlay-input` 让它能工作）。里面有一页独立的 **mochinr 降噪**：
  - **网络**：启用、层数、渲染率、细节/色彩强度、历史重置；顶部显示实际渲染分辨率与网络 GPU 耗时
  - **风味**：风格、强度、局部色调/结构、皮肤结构、自动遮罩、高光保护、色度钳制、细节稳定、
    链式历史、跟随输入分辨率、多层后续参数
  - **计时**：会话状态、分辨率、已合成/跳过帧、CPU 每帧四项分解、在飞帧数、队列深度
  - **调试**：显示视图（正常/代理/纯网络/差分/着色）、保存
- **Ctrl+Home**：重新读 `mochinr.ini`（面板外手改参数后用这个，不必重启）。
- 覆盖层打开期间 `overlay-input` 会吞掉原始键鼠，游戏不会同时响应；再按 Home 或 Esc 关闭。
- 参数不走 `LmxxfNrFrameInfo`：`MochizukiNrControls.renodx=1` 时由 `reno_*` 字段接管，
  必须经 `MochizukiNrSetControls` 下发——否则「面板除了开关全都无效」。

## 5. 参数落在哪

| 文件 | 管什么 |
|---|---|
| `mochinr.ini` | mochinr 面板的全部项（松手自动写；`Ctrl+Home` 重载） |
| `overlay-input.ini` | 覆盖层热键（`Key`，虚拟键码，36=Home）与是否吞输入（`Swallow`） |
| `config\v4e\config.json` | Magpie 效果组、`profiles[0].scalingMode`、节奏与限帧（0.6.11） |
| `DLSS5-AMD\default-config.txt` | lmxxf 的调参面（升级会覆盖）；个人改动写 `custom-config.txt` |

## 6. 先验运行时（不启动 Magpie）

```
nrtest.exe  "<Magpie 目录>"                        # 跑一帧：能不能起来
nrbench.exe "<Magpie 目录>" 1920 1080 40 aco 0.5   # 跑 40 帧：宽 高 帧数 ACO 网络缩放
```

两者都建自己的 D3D12 设备与队列，直接对 `MochizukiNrRuntime.dll` 跑：

- 成功：`PrepareFrame rc=0`、`private_output` 非 0；`nrbench` 会分别打印 prepare / 生产拷贝 /
  网络 / 消费拷贝的墙钟，用来对照上游权重给出的离线单帧 ms；
- 首次会编译管线（数十秒到数分钟），**别中途关掉**；
- 失败：打印 `rc` 与 `last_error`，并把原因写进 `mochizuki_nr.log`。

跑之前先关 Magpie（两者都要占 1 GB 以上显存）。

## 7. 日志

| 文件 | 看什么 |
|---|---|
| `mochinr.log` | 钩子是否命中、批次怎么劈、网络耗时、跳过帧、错误码 |
| `ReShade.log` | 初始化、add-on 注册（`Registered add-on "..." using ReShade API version 20`） |
| `overlay-input.log` | 是否拿到 swapchain 窗口、面板开关、转发事件 |
| `logs\magpie.log` | Magpie：效果加载、缩放会话、热键注册失败（`1409`）、GPU 优先级 |
| `mochizuki_nr.log` | 只在 nrtest/nrbench 或 OpenNR 路线出现：设备选择、显存、建网结果 |
| `amd-nr.log` | 仅 present 层旧路线：选了哪个运行时、为何拒绝 |
