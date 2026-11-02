# 桥接研究：换了 FFX 钩子之后，D3D11 → D3D12 → Vulkan 还剩多少

> 2026-11-02。只读上游源码 + 本仓库源码 + 运行时二进制字符串，**没有实机**（9070 XT 不在本机）。
> 所有结论都标了出处；凡是没有直接证据的，写成"待测"，不写成结论。

## 0. 一句话结论

**这条桥仍然存在，但它换了主人，也换了几何形状。**

| 段 | 谁在做 | v0.3（present 层，`amd-nr`） | v0.4（FFX 钩子，`mochinr`） |
|---|---|---|---|
| D3D11 → D3D12 | 宿主 | OpenNR 自己的 D3D11 桥：共享纹理 + 私有 D3D12 队列 + staging | **Magpie 自己的 `FSR3Upscaler`**：共享纹理 + 共享围栏，2 次 4K 拷贝 + 每帧 `Flush` |
| D3D12 → Vulkan | 运行时 | 同左（`MochizukiNrRuntime.dll`） | 同左，但入口从"present 的 4K 输出"变成"FFX 的 4K 颜色纹理" |
| 每帧 CPU | — | 有 `FlushAndWait11` / 队列等待 | **0.17 ms**（Vulkan 围栏 0.09 + 提交 0.05 + 录制 0.02） |

所以：

1. `roadmap.md` 里那句"**D3D11 → D3D12 → Vulkan 桥接 ≈ 1.9 ms**"是 **v0.3 present 层的残差估算**，
   不能直接搬到 v0.4 —— 那 1.9 ms 里有 4K staging 往返、OpenNR 私有 D3D12 队列的等待、
   以及一个当时才有的 CPU 侧跨设备等待。v0.4 的 CPU 侧只剩 0.17 ms，这是发布说明里自己写的数。
2. v0.4 里 **D3D11→D3D12 这一段不再是我们的账**：它在 Magpie 的 `FSR3Upscaler` 里，
   只要效果组里挂 `FSR4\FSR4_SR` 就跑，**lmxxf 跑同一格也要付**。这是"结构性下限"，换 NR 后端去不掉。
3. D3D12→Vulkan 这一段仍然每帧发生一次，但已经是"共享缓冲区 + 时间线信号量"的形状，
   不再经过 staging/回读 —— 和 lmxxf 解决 HIP 那一侧的思路是同一个。

## 1. v0.4 的真实数据通路（都在源码里）

以 Magpie 0.6.11（`SAOG0721/Magpie`，`experimental`）的 `src/Magpie.Core/FSR3Upscaler.cpp` 为准，
`FSR4\FSR4_SR` 就是它实现的（`NativeEffectBackendFactory.cpp:183`，`useFsr4 = (effectName == "FSR4\\FSR4_SR")`）：

```
游戏/捕获帧（D3D11，3840×2054）
  │
  │ ① context11->CopyResource(sharedInput11, input)          FSR3Upscaler.cpp:498
  │    sharedInput11 建的时候就是 SHARED | SHARED_NTHANDLE   :193-195
  │
  │ ② Signal(fence11); Flush()                               :506-508
  │ ③ queue12->Wait(fence12, inputReady)                     :509
  │
  ├─ D3D12 侧：sharedInput12 = OpenSharedHandle(sharedInput11)  :205-208
  │
  │ ④ ffxDispatch(desc{ color=sharedInput12, output=sharedOutput12, ... })
  │        └─ 被 mochinr 的 detour 拦下 → 只记录、返回 0        mochinr.cpp:855-914
  │
  │ ⑤ ExecuteCommandLists 被劈成两段                            mochinr.cpp:918+
  │      [0..at] → RecordInputs（运行时把颜色搬进自己的共享缓冲）:692-707
  │             → EnqueueHip（D3D12 队列等 Vulkan 时间线）       :712-718
  │      [at+1..] → RecordOutputs → 重放原 ffxDispatch，
  │                 color 指向运行时的 private_output            :742-763
  │
  │ ⑥ queue12->Signal(fence12); context11->Wait(fence11)     :575-577
  │ ⑦ context11->CopyResource(output, sharedOutput11)        :579
  ▼
输出帧（D3D11，3840×2160）
```

要点：

- **①⑦ 是两次整帧拷贝**（4K，BGRA8 33 MB/次），**② 是每帧一次 `ID3D11DeviceContext::Flush()`**。
  这三样与 NR 无关：`FSR4_SR` 跑原生 FSR 也要付，lmxxf 也要付。它们才是"D3D11↔D3D12 桥"的主体。
- **⑤ 是唯一属于我们的一段**：运行时要拿到颜色。它可以**原地采样**（把 `sharedInput12` 当 Vulkan
  图像导入），也可以**拷贝**（导入自己的 D3D12 共享缓冲区再拷一次）。
- 出方向上 `mochinr` 已经不拷贝了：消费端直接把放大器的 `color` 换成运行时的 `private_output`
  （`mochinr.cpp:747-758` 的注释写明这是照抄上游 `delivered()` 的做法）。所以"回写 4K 颜色"这一步
  在 v0.4 不存在。

## 2. 那 1.9 ms 到底是什么，为什么不能沿用

v0.3 的数字是**预算残差**，不是直接测量：`NR 总 5.3 = 网络 3.3 + 2.0`，把 2.0 记成"桥接/拷贝"
（`measurements.md` §2026-10-31 补充）。它的构成与 v0.4 完全不同：

| v0.3 有、v0.4 没有 | 说明 |
|---|---|
| 私有 D3D12 工作队列 | OpenNR 自己建的 `workQueue`，与游戏/宿主队列靠 `crossFence`/`ringFence` 排序，每帧一次 GPU 侧等待 |
| `stageIn11` / `stageOut11` 私有 staging 对 | 补丁里后来改成"一次直拷"，但 4K 双跳的带宽已经付过 |
| CPU 侧 `FlushAndWait11` | 补丁改成共享围栏后才消失；早期实现是**每帧阻塞整个 D3D11 上下文** |
| 在 **present** 上工作 | 看到的是放大后的 4K 输出，拷贝/光流都按 4K 计；FG 开时还乘倍率 |

v0.4 把这些全部绕开了：NR 在放大之前、在别人已经建好的 D3D12 队列里、不碰 D3D11 上下文。
发布说明里的实测 **CPU 0.17 ms/帧**（`MOCHINR-README.txt`）就是证据 —— 若还有 staging 或 CPU 等待，
这个数不可能是 0.17。

所以正确说法是：**1.9 ms 属于"present 层移植"这一代架构，v0.4 的桥接成本未知但上限很低**
（每帧一次拷贝 + 一次时间线等待，量级应为 0.1–0.5 ms），**必须实测，不能引用旧数**。

## 3. D3D12 → Vulkan：运行时的两种路径（从二进制里读出来的）

`MochizukiNrRuntime.dll` 只导入 `vulkan-1.dll`（无 `d3d12.dll`），D3D12 侧全靠 vtable 调用。
它自己的字符串把两条路径写得很清楚：

```
'vkCreateBuffer (shared)'                        creating its own D3D12 buffer, SHARED, then importing
'import of a D3D12 buffer'
'vkCreateSemaphore (timeline)' / 'import of the D3D12 fence (%s)'
'[nr] Model resolution: frame read and written in place (VkFormat %u)'   ← 零拷贝路径
"[nr] sampling the caller's %s in place failed; copying instead"          ← 退化成拷贝
'[nr] reading and writing the frame in place failed; copying instead'
'[nr] input check: the copy never landed after %llu recordings; giving up on this one'
```

也就是说：**运行时优先原地读写调用者的纹理，失败才拷。** 能不能原地，取决于两件事：

1. 调用者的 D3D12 资源能不能导出共享句柄（`ID3D12Device::CreateSharedHandle`）；
2. 该格式能不能被 Vulkan 图像导入。

而 v0.4 交给它的 `color` 是 **Magpie 从 D3D11 共享纹理 `OpenSharedHandle` 出来的 D3D12 资源**
（`FSR3Upscaler.cpp:205-208`），不是 `CreateCommittedResource(SHARED)` 建的。
这正是"原地"可能失败的地方 —— 也就是 1.9 ms 之后是否还残留一笔拷贝的关键。

**所以这版 `mochinr.addon64` 加了一个只读探针**（本次提交，默认开启，只打日志一次）：

```
bridge: colour heap flags=0x... type=... shareable=1 (0x00000000) frame_bytes=33177600
bridge: private heap flags=0x... type=... in_place=0
perf: ... inplace=0 copied=300
```

- `shareable=1`：这个纹理可以被导出成 NT 句柄 → 原地采样在原理上可行；
  `shareable=0`（多半是 `0x80070057` E_INVALIDARG）：**拷贝是结构性的**，除非 Magpie 改纹理创建方式。
- `inplace=N copied=M`：运行时实际走的是哪条路（`private_output == color` 即原地）。

这两个数一出来，1.9 ms 是否还以"拷贝"的形式存在，就不用再猜了。

## 4. lmxxf 有没有同一个问题：有，但只在下半段

lmxxf 钩的也是 `ffxDispatch`（`hook-requirements.md` §1），所以 **①⑦ 那两次 4K 拷贝和 `Flush`
它一样跑不掉**，那是宿主的账。它需要自己解决的只有"D3D12 → HIP"这一段，做法在
`Development/HIP/hip_d3d12_bridge.h`（MIT，`lmxxf/dlss5-on-amd-9070xt-porting`，`main`）：

| 手法 | 代码 | 作用 |
|---|---|---|
| D3D12 共享缓冲区 | `:162-163` `CreateCommittedResource(..., D3D12_HEAP_FLAG_SHARED, ...)` + `CreateSharedHandle` + `hipImportExternalMemory` + `hipExternalMemoryGetMappedBuffer` | 显存直通，**没有 staging、没有 CPU 回读** |
| 共享围栏 → HIP 外部信号量 | `:233` `CreateFence(0, D3D12_FENCE_FLAG_SHARED, ...)` + `hipImportExternalSemaphore` | 同步在 GPU 上完成，CPU 不阻塞 |
| 只认宿主那条队列 | `:211` 队列类型必须 DIRECT/COMPUTE，设备必须与 `Create` 时同一台；`staged-bridge.md` §Host sequence | 队列顺序即同步，不另建队列 |
| 拷贝按网络档，不按帧 | `pixels = options.width * options.height`（`:211`），`CopyBufferRegion` 用 `pixels*16`（`:292`） | 1080 档就是 1920×1080，不是 4K |
| direct input：能省就省 | `if(!(direct_input && rgba==input.resource)) copy(rgba, input);`（`:294`） | 生产端已经写进共享缓冲时，**这一帧一次拷贝都不做** |
| GPU 侧轮询替代围栏延迟 | `WriteBufferImmediate` + `hipStreamWaitValue32`，`:101-136` | 可选的快路径，CPU 不进循环 |
| 共享缓冲池 | `:150-160`（HIP 驱动泄漏导入缓冲，按 device+size 复用） | 换分辨率不重新建，避免 VRAM 泄漏 |

数字上他们自己也量过"桥接到底占多少"（`Development/HIP/bridge-isolation.md`）：

```
full-before 24.401 ms / pure-HIP 24.269 ms / full-after 24.548 ms   （同一输入，网络 24 ms）
```

即**前后处理 + 共享资源 + 桥接合计 0.1–0.3 ms**，在 24 ms 的网络面前可以忽略。
我们自己在 9070 XT 上的 A/B 也给了同一量级：lmxxf 侧桥接 **0.3–0.4 ms**（`measurements.md`）。

对照我们的 v0.4：结构上已经有"共享缓冲 + 时间线信号量 + 同队列"这三样，
缺的是 lmxxf 的"**按网络档拷贝**"和"**direct input**"两条 —— 但这两条都在运行时手里，不在宿主手里。

## 5. 差距清单（按可验证性排序）

| # | 可能的残留成本 | 现在的证据 | 怎么验 |
|---|---|---|---|
| 1 | 运行时拷贝 4K 颜色（非原地） | 运行时字符串里两条路径都在，未知走哪条 | 新探针 `bridge:` / `inplace=` |
| 2 | 拷贝尺寸是帧尺寸而不是模型档 | 传入的是 4K 颜色，`model_scale` 只影响网络 | 日志里 `frame_bytes` 与运行时的显存读数 |
| 3 | 在飞帧上限（`inflight=2`）触发的 CPU 等待 | `mochinr.cpp:779-790`，>2 帧就 `SetEventOnCompletion` 等 200 ms | `perf:` 行里的 `waits=` 与 `ms` |
| 4 | Magpie 自己的 2 次 4K 拷贝 + 每帧 `Flush` | `FSR3Upscaler.cpp:498/506/579` | 用 `FSR4_SR` 但禁用 mochinr，看同一场景帧数（这是宿主下限） |
| 5 | Vulkan 时间线往返（GPU 侧气泡） | CPU 0.09 ms，GPU 未知 | `nrbench` 的 enqueue 与帧时间差 |

第 4 条尤其重要：它给出了"**就算 NR 完全不算，这个效果组最高能跑多少帧**"的基准。
如果禁用 mochinr 后帧数与开启时接近，说明瓶颈根本不在 NR/桥接，而在宿主或游戏侧节奏。

## 6. 9070 XT 上的 20 分钟测量方案

```
1) 基线（宿主下限）
   config 里把 NR-preupscale 组的 mochinr 效果器禁用（只留 FSR4_SR），跑同一段场景 60 s
   记 logs\magpie.log 的 Backend publication CPU timing 相邻两行 / 120 = 原生帧率

2) 开启 NR，读新探针（本次构建）
   mochinr.log 里应有：
     bridge: colour heap flags=... shareable=?
     bridge: private heap flags=... in_place=?
     perf: ... inplace=? copied=? waits=? ?.?? ms
   三个判据：
     shareable=1 且 in_place=1  → 零拷贝，桥接只剩时间线等待，1.9 ms 不成立
     shareable=1 且 in_place=0  → 可原地但运行时没走，值得向运行时作者提（或找开关）
     shareable=0                → 拷贝是结构性的，优化方向转向"缩小拷贝尺寸"

3) 分解网络与桥接
   nrbench.exe "D:\Magpie-NR-045" 1920 1080 40 aco 0.5   ← 纯网络基线
   与 (2) 的帧时间对照，差额即桥接 + 宿主拷贝

4) 变量隔离
   model_scale 0.5 → 0.35：网络时间应线性下降；若帧数不动而 `waits=` 上升，
   说明在飞队列在限流，而不是桥接带宽
```

期望读数（预测，非结论）：`inplace=1` 或每帧一次 ~66 MB 的拷贝 → 折合 0.1–0.2 ms；
若日志显示 `copied=` 且 `waits=` 很大，才需要回到"1.9 ms 仍在"的假设。

## 7. 能立刻做的优化（按性价比）

1. **把探针读数拿到手**（已完成，随本提交的 `mochinr.addon64` 生效）。没有这三个数，后面都是猜。
2. **`inflight` 默认值复核**：现在默认 2，且超过就 CPU 等 200 ms。若 `waits=` 明显，
   说明限流点在宿主而不是 GPU；调 3 或改成"只在队列深度 >3 时等"更合理（改动小、可回退）。
3. **请运行时提供"按模型档接收颜色"**：它已经有 `model_scale`，若拷贝能直接 4K→1080 档，
   拷贝带宽降到 1/4。这属于上游改动，只能提需求/找隐藏开关（字符串里有 `--barrier-*`、
   `NR_FORMAT_FALLBACK` 一类内部开关，值得再挖一遍）。
4. **不要试图去掉 `FSR4_SR`**：它是 `ffxDispatch` 的来源，也是 D3D12 队列的来源；
   换成纯 HLSL 钩子就没有可拦截的调用，`mochinr` 与 lmxxf 同时失效（`hook-requirements.md` §2）。
5. **不要在 v0.4 上再引用 1.9 ms**：那是另一代架构的残差，写进对外文档会误导优化方向。

## 8. 帧内串行：同一条桥的第二种代价（2026-11-02 补）

现象（`MOCHINR-README.txt`）：降低网络渲染率只降 GPU 负载与功率，**帧数不升**；游戏内限帧
始终高于捕获/输出约 15–20 帧。嫌疑是 D3D12↔Vulkan 围栏把每帧串起来。

### 8.1 真正的串行点在哪：不是 Vulkan 围栏，是"单槽交接"

把 §1 的通路再读一遍，注意**资源的生命周期**：

- `sharedInput11/12`、`sharedOutput11/12` 在 `Initialize` 里**各建一份、整会话复用**
  （`FSR3Upscaler.cpp:318-319`），没有环形缓冲；
- 每帧的顺序是硬编码的：
  `CopyResource(sharedInput11)` → `Signal(fence11)` → `Flush()` → `queue12->Wait(fence12)`
  → **D3D12 岛（ffx = 我们的 NR + 放大）** → `Signal(fence12)` → `context11->Wait(fence11)`
  → `CopyResource(output, sharedOutput11)`。

因为输入/输出各只有一个槽，**第 N+1 帧的 D3D11 拷入必须等第 N 帧的 D3D12 岛读完**
（安全性正是靠 `outputReady` 这道围栏保证的）。所以 D3D12 岛是一个**不可与任何相邻帧重叠的
串行孤岛**：岛内时间 = 网络时间 + 放大时间 + 两次跨 API 气泡，**每一帧都要完整付一遍**。

这就是"与桥接问题的关系"的答案：**是同一条桥，但两种代价**。
§2 讨论的是带宽（每帧拷多少字节），这一节讨论的是**延迟结构**（岛把每帧串成一条线，
任何岛内时间都直接进帧时间，且不与任何其它工作重叠）。Vulkan 时间线围栏只是岛的边界之一，
真正把帧锁死的是宿主的单槽交接。

### 8.2 为什么"降网络渲染率帧数不升"——三个候选解释

帧时间近似为 `max(D3D11 侧, 岛) + 交接气泡`：

| 解释 | 机制 | 与观测是否吻合 |
|---|---|---|
| A. **D3D11 侧已经是 max** | 捕获 + 效果链 + 两次 4K 拷贝 + `Flush` + present 的固定成本 ≥ 岛时间；网络再小也不改变 max | 吻合：GPU 负载/功率下降（岛变小）但帧数不动 |
| B. **捕获节奏封顶** | Magpie 按游戏 present 节奏捕获，输出 ≤ 捕获率；处理延迟造成"每 N 帧丢 1 帧" → 游戏内 fps 比输出高 15–20 | 部分吻合（这正是观测到的差值），但需要解释为什么丢帧 |
| C. **在飞帧限流** | `inflight`（部署 ini 里是 **2**，代码默认 3，面板可调到 16）超过就 CPU 等 200 ms；若岛延迟 > 2×帧间隔，吞吐被 `2/L` 封顶 | 可直接用日志判：`perf:` 行 `waits=` / `wait ms` |

注意 A 对 **lmxxf 同样成立**——它也在同一个岛里（同样钩 `ffxDispatch`）。这给出一个零成本的
交叉验证：**在 Magpie 里把 lmxxf 的网络档也调低（`DLSS5_NETWORK_HEIGHT`），如果它的帧数同样
不升，就证明串行点在宿主而不是哪个运行时**。

### 8.3 判别实验（全是读日志，30 分钟内可做完）

1. **三档天花板**：同一场景各跑 60 s，取 `logs/magpie.log` 的
   `Backend publication CPU timing` 行间墙钟差 / 120：
   - 组「仅补帧」（无 FSR4_SR、无 NR）→ 捕获/present 天花板；
   - `NR-preupscale` 但**禁用 mochinr 效果器**（只留 FSR4_SR）→ D3D11 侧 + 岛(无网络) 天花板；
   - 完整开启 → 现状。
   三档的差值把"宿主固定成本 / 岛中非网络部分 / 网络"分开。
2. **`waits=` 判 C**：`mochinr.log` 的 `perf:` 行。`waits>0` → 把 `mochinr.ini` 的
   `inflight` 从 2 调到 3–4（面板「在飞帧数」滑条即可，零代码）再跑一轮。
3. **`fenceAvgMs` 对网络率做扫描**：model_scale 1.0 / 0.5 / 0.35 各测一次。
   若 `fenceAvgMs` 随网络线性下降而 fps 不动 → 解释 A 成立（等待的不是瓶颈）；
   若 `fenceAvgMs` 不降 → 等待被别的固定成本垫底，同样指向 A。
4. **lmxxf 交叉验证**（见 8.2）。
5. **上游对照**：lmxxf 在**游戏内**（RE9/圣刃）的帧数是随网络档变化的
   （他们的 `Development/720p/benchmark-frame.md` 整套都在量这个）——说明岛是 **Magpie 特有**的，
   不是运行时的宿主契约必然结果。

### 8.4 解决方向（按可达性排序）

| 方向 | 动作 | 代价/风险 |
|---|---|---|
| 1. 先排除 C | `inflight` 2 → 3/4（ini 或面板滑条） | 零成本；VRAM 多留一帧的运行时缓冲 |
| 2. 缩岛 | `bridge.md` §7 的原地采样（`inplace=1`）去掉拷入；请运行时支持按模型档拷贝 | 依赖上游；收益 = 拷贝与气泡，约 0.1–0.3 ms |
| 3. 拆单槽（**结构性正解**） | 给 Magpie 的 `FSR3Upscaler` 加 2–3 槽共享纹理环形缓冲 + 对应围栏槽位，让第 N+1 帧拷入与第 N 帧岛重叠 | 改 GPL 宿主（fork 允许）；要小心围栏语义与 reset；预计能吃掉大部分"交接气泡 + 岛串行" |
| 4. 异步 NR（一帧延迟） | 放大器本帧读原图、下一帧才换网络输出，把网络彻底移出关键路径 | 视觉上引入一帧 NR 延迟与时域错位（历史对齐要跟着挪）；v0.3 的 `BridgePace=0` 已证明"跳帧补画"会闪，这条路必须保证"永远用上一帧的修正、绝不跳过"，否则回到全屏闪烁 |
| 5. 接受现状 | 若判别实验显示 A 且 D3D11 侧 ≈ 岛，则网络优化空间 ≈ 0，把精力转回画质与面板（目标 4） | 无 |

优先级建议：**先做 8.3 的 1–3**（零代码/一个 ini 项），拿到三个天花板数字后再决定是否投入
方向 3——它是唯一能动"帧数不升"这个现象本身的改动，但工作量在宿主侧，值得等数据说话。

---

## 9. 出处

- Magpie：`SAOG0721/Magpie`，分支 `experimental`，`src/Magpie.Core/FSR3Upscaler.cpp`、
  `NativeEffectBackendFactory.cpp`（本次读取时默认分支 HEAD）。
- lmxxf：`lmxxf/dlss5-on-amd-9070xt-porting`，`main`，`Development/HIP/hip_d3d12_bridge.h`、
  `staged-bridge.md`、`bridge-isolation.md`。
- 运行时：`MochizukiNrRuntime.dll`（0.5.x，随 v0.4 发布包），字符串与导入表由本仓库脚本读取。
- 本项目：`src/mochinr/mochinr.cpp`（钩子与提交契约）、`docs/measurements.md`（v0.3 的数字）。

---
