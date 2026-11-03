# 单槽拆解：`FSR3Upscaler` 多槽环形缓冲与"输出晚一帧"

> 2026-11-03。上游只读参考：`SAOG0721/Magpie`，分支 `experimental`，HEAD `6940f84`。
> 本文所有源码行号均指该提交的 `src/Magpie.Core/FSR3Upscaler.cpp`（601 行）与
> `FSR3Upscaler.h`（45 行）；主机侧行号指同提交的 `src/Magpie.Core/Renderer.cpp`。
>
> **本文没有在 9070 XT 上跑过任何东西。** 三类陈述严格分开：
> **【源码】**＝读出来的；**【实测】**＝已有归档数据（带出处）；**【待测】**＝推测或未验证。
> 预测数字全部标了推导式，任何人可以自己复算。

---

## 0. 结论摘要

### 0.1 三条判断的裁决

| 判断 | 裁决 | 主要依据 |
|---|---|---|
| **A. 串行点有两个，不是一个** | **同意结论，但收益数字要下修到 ≈0** | `:577` 那道 D3D11 侧 GPU 等待确实把本帧之后的所有 D3D11 GPU 工作挡在岛后面【源码】；但 `:497` 的 CPU 阻塞**已经被主机发布处的 `Flush + 等围栏` 吞掉了**（`Renderer.cpp:4614-4624`），所以"只加多槽"连 0.1–0.5 ms 都拿不到，量级是 **0**（见 §1.3） |
| **B. 要真拿到台阶必须让输出晚一帧（阶段 2）** | **同意** | 只有把 `:577` 的等待挪到"下一帧的开头"，D3D11 流水线才不再包含本帧的岛，帧时间从「求和」变成「取大」。阶段 1 是阶段 2 的**前置改造**，本身不是收益项 |
| **C. 收益的前提没被证明，必须先判别** | **同意，且这是开工闸门** | 求和式对"延迟串行"与"GPU 吞吐占满"两种世界同样成立，现有五点数据无法区分。G0 判据见 §2 |

### 0.2 一句话设计

给 `FSR3Upscaler` 加 **N=3 槽**「输入/输出/运动矢量 + allocator + commandList」，每槽记一个
`islandValue`；默认 `OUTPUT_LAG=0`（输出语义与上游逐帧一致，只去掉无谓等待，收益预期 0）；
`OUTPUT_LAG=1` 时开一个"结果 FIFO"，本帧拷出的是上一帧岛的结果，帧时间从 `D3D11 + 岛`
变成 `max(D3D11, 岛)`。

**注意 `lag=1` 的作用域不是本类内部**：`ctx.output` 就是效果链的下游输入（§3.7），所以它把
**整条下游链的图像**整体推后一帧，而下游同时拿到的元数据（`frameId`、HDR metadata、运动矢量、深度）
仍是本帧的。这是阶段 2 唯一的语义代价，也是"FG 开时默认不启用 `lag=1`"的理由（§6.4）。

### 0.3 预测台阶（条件：G0 通过，即 GPU 未占满）

| NR 档 | 现状【实测】 | 求和模型 | 取大模型 | 预期 | 说明 |
|---|---|---|---|---|---|
| s=1.0 | 47 fps / 21.5 ms | 6.7+1.4+13.7 = 21.8 | **66–70 fps / 14.3–15.1 ms** | 15.1 ms 为设计口径（66 fps），70 需岛内非网络部分 ≤0.6 ms | §0.4 |
| s=0.5 | 90 fps / 11.3 ms | 6.7+1.4+3.43 = 11.5 | **8.0–8.1 ms → 123–125 fps** | 与 B2 天花板重合 | 岛 4.8 ms 被 8.0 ms 的 D3D11 侧完全盖住 |
| s=0.35 | 105 fps / 9.5 ms | 6.7+1.4+1.68 = 9.8 | **8.0–8.1 ms → 123–125 fps** | 同上 | 岛 3.1 ms |

**天花板 = B2（123–125 fps / 8.0–8.1 ms），不是 B1。** B1（150 fps / 6.67 ms）要求连
"两次 4K 拷贝 + 一次 `Flush` + 槽位本身"那 1.4 ms 也一起去掉，那是 `bridge.md` §8.5 方案 a 的账，
不在本文范围（`findings.md` 把 150 记作"台阶2"，本文只交付"台阶1"）。

### 0.4 为什么 roadmap 写"约 70"而我写 66

`1.4 ms` 是 **B1−B2** 的差值，它是"整套 FSR4 槽位"的账，**不完全落在岛内**：两次 4K 拷贝在
D3D11 侧，只有放大本身在岛内【源码 `:498`/`:579` 在 `context11`，`:530-573` 在 `queue12`】。

- 若把 1.4 ms 全算进岛：`max(8.1, 1.4+13.7) = 15.1 ms → 66 fps`
- 若岛内只占 0.6 ms：`max(8.1, 14.3) = 14.3 ms → 70 fps`

两者差 0.8 ms，**都在模型 ±0.4 ms 残差的同一量级上再放大一倍**。所以诚实的说法是
**66–70 fps**，验收取 **66 为通过线**（保守），大于 70 记为超预期。0.5/0.35 档不受这个不确定性
影响（`max` 由 8.0 决定），这也是把 0.5 档当主验证点的另一个理由。

---

## 1. 现状：串行点到底有几个

### 1.1 上游 `Draw()` 的每帧顺序【源码】

| # | 位置 | 动作 | 性质 |
|---|---|---|---|
| 1 | `:497` | `WaitForFence(lastSubmittedValue)` | **CPU 阻塞**（`GetCompletedValue` 快路径，否则 `SetEventOnCompletion` 无限等，`:61-70`） |
| 2 | `:498` | `CopyResource(sharedInput11, input)` | 录制 |
| 3 | `:502-503` | 运动矢量：`context11->Wait(sync.fence)` + 拷贝 | GPU 侧等待（游戏给的时间线）+ 录制 |
| 4 | `:505-508` | `Signal(fence11, inputReady)` + `Flush()` | 录制 + 提交（`Flush` 不阻塞） |
| 5 | `:509` | `queue12->Wait(fence12, inputReady)` | **GPU 侧等待**（D3D12 队列） |
| 6 | `:512-513` | `allocator12->Reset()` / `commandList12->Reset()` | CPU 调用；**隐含要求上一帧已执行完** |
| 7 | `:515-528` | 三个资源屏障 | 录制 |
| 8 | `:530-560` | `ffxDispatch`（＝ FSR 放大；被 mochinr 拦下后变成 NR + 重放） | 录制（内部可能自行 `ExecuteCommandLists`，`bridge.md` §1 ⑤） |
| 9 | `:566-573` | 反向屏障 + `Close` + `ExecuteCommandLists` | 录制 + 提交 |
| 10 | `:575-576` | `Signal(fence12, outputReady)`；`lastSubmittedValue = outputReady` | 录制 |
| 11 | `:577` | `context11->Wait(fence11, outputReady)` | **GPU 侧等待（D3D11 队列）** |
| 12 | `:579` | `CopyResource(output, sharedOutput11)` | 录制 |
| 13 | `:580-581` | 清 `resetHistory`、记 `lastGuidanceFrameId` | CPU 状态 |

共享纹理在 `:318-319` 由 `CreateSharedTexture` 建，**整会话只一份**；`fenceValue` 是**单一单调
计数器**，D3D11 与 D3D12 两侧共用同一个内核围栏对象（`:379-386` 从 `fence11` 导出句柄再
`OpenSharedHandle` 成 `fence12`），因此两侧的信号值落在**同一个值空间**——这是本设计能成立的关键前提。

### 1.2 三个串行点，不是一个也不是两个

**串行点 1（CPU）：`:497`。** 安全理由：单份 allocator 与单份共享纹理。

**串行点 2（D3D11 GPU 时间线）：`:577`。** D3D11 立即上下文是 FIFO，`:577` 之后提交的
一切——本帧剩余的链上 pass、`CopyResource(output, ...)`、以及**下一帧的捕获/效果/拷入**——
在 GPU 时间线上都排在岛后面。所以岛不是"和 D3D11 并行"，而是**嵌在 D3D11 队列里的一段**。

**串行点 3（CPU，本文新指出）：主机发布路径 `Renderer.cpp:4614-4624`。**

```cpp
hr = d3dDC->Signal(_d3dFence.get(), ++_fenceValue);
hr = _d3dFence->SetEventOnCompletion(_fenceValue, _fenceEvent.get());
d3dDC->Flush();
_fenceEvent.wait();          // ← 每个发布帧无条件阻塞，等整条 D3D11 队列排空
```

这段**不在我们的补丁范围**（`Renderer.cpp` 不是 `FSR3Upscaler`），但它决定了本设计的上限：
**每发布一帧，CPU 必然等整条 D3D11 队列排空一次**。日志里的 `fenceAvgMs` 就是这次等待的均值
（`Renderer.cpp:4628-4638`）【源码】。

### 1.3 由此得出：阶段 1 的收益是 0，不是 0.1–0.5 ms

把串行点 1 与串行点 3 放在同一条 CPU 时间轴上：

```
Draw(N-1): [等岛 N-2] 录制/提交  ─┐
链上工作、Publish: Flush + 等 D3D11 队列排空  ← 这次等待必然跨过岛 N-1
Draw(N):   [等岛 N-1] ← 围栏值早就到了，GetCompletedValue 快路径，直接返回
```

**`:497` 在稳态下几乎总是空转**，因为紧邻的上一条语句（发布处的排空等待）已经把 D3D11 队列
推过了岛。多槽只是让 `:497` 从"空转"变成"更空转"。

结论（可证伪，见 §6 阶段 1 判据）：**阶段 1 单独上线，帧数与 `fenceAvgMs` 都应落在噪声内不变**。
它的价值是**解除 allocator/纹理的复用依赖**，也就是阶段 2 的前置条件；把它当收益项会得到
一个"改了很多、什么都没变"的假失败。

> 这一条与 `roadmap.md` 2026-11-02 增补里的"预期帧时间收益 0.5 档 −3~4.5 ms"并不冲突：
> 那个数字属于**台阶1（网络重叠）**，而网络重叠只有阶段 2 才能拿到。此前的表述容易让人以为
> 只拆单槽就能拿到 −3~4.5 ms。

---

## 2. 开工闸门 G0：先证明"取大"的天花板存在【待测】

### 2.1 为什么必须测

求和式 `T = 6.7 + 1.4 + 13.7·s²` 对五点残差 ≤0.4 ms，但**它有两个同样成立的解释**：

| 解释 | 机制 | 取大模型是否成立 |
|---|---|---|
| L. 延迟串行 | 岛嵌在 D3D11 队列里，GPU 有空闲，但队列不许重叠 | 成立，台阶最大 |
| T. 吞吐占满 | 6.7 ms 里 GPU 一直在忙，岛只是"把工作量挪了个地方" | **不成立**，`max()` 拿不到任何东西 |

`s=0.35` 时帧时间 9.5 ms、岛只有 3.1 ms——如果 GPU 在这 9.5 ms 里忙了 8.5 ms，那么
"重叠"只是让同一批工作在时间轴上换个位置。**判别只能靠 GPU 忙时间，不能靠帧数。**

### 2.2 三个可用的仪器（按可得性排序）

| 仪器 | 读数 | 状态 |
|---|---|---|
| Windows 性能计数器 `\GPU Engine(pid_*_engtype_*)\Utilization Percentage` | 按进程、按引擎（3D / Compute / Copy）的忙碌百分比，默认 1 Hz | 无需安装，`scripts/measure-gpu-busy.ps1` 取（P0 任务产出） |
| PresentMon 2.x（对 `Magpie.exe` 抓） | 逐帧 GPU 忙时间（列名随版本，`msGPUActive` / "GPU Busy"，**以本机版本导出头为准**） | 需另装；`【待测】`列名 |
| 本仓库既有读数 | `magpie.log` 的 `fenceAvgMs`；`profiling=1` 时 `EffectsProfiler` 的逐 pass GPU ms | 已经在手，作为**辅助**（它只覆盖 D3D11 侧，不含岛） |

`fenceAvgMs` **不能单独当判据**：它只测"CPU 在发布处等了多久"，在 GPU 占满与延迟串行两种
世界里都会很大。它只能用来做前后对照（阶段 2 应显著下降）。

### 2.3 判据（可证伪，写死阈值）

在 `s=0.5` 与 `s=0.35` 两档各跑 60 s，同场景、无 FG、`FrameRate_Filter` 关，取稳态 30 s 窗口：

```
window         = 30 s
frames         = 窗口内发布帧数（magpie.log 每 120 帧一行，数行数即可）
frame_time     = window / frames
busy_per_frame = Σ_engines (util% × window) / frames      # 所有 3D / Compute 引擎求和，单位 ms
ratio          = busy_per_frame / frame_time
```

注意 `busy_per_frame` 里的引擎求和会重复计入并行（3D 与 Copy 可以同时忙），所以 `ratio`
**可能 >1**；>1 一律按"已饱和"处理，不要试图归一化。

| 判据 | 结论 | 动作 |
|---|---|---|
| `ratio > 0.85` | 求和是吞吐的必然，**取大天花板不存在** | **停**：不做阶段 2。保留"阶段 1 无害版"或整体放弃，主线转向缩岛（`bridge.md` §7 原地采样、按档拷贝） |
| `0.60 ≤ ratio ≤ 0.85` | 有部分空闲，台阶打 `(1−ratio)` 折扣 | 做阶段 1 + 阶段 2，但**验收线按实测折扣下调**：`T_target = max(B2, T_sum − 空闲时间)` |
| `ratio < 0.60` | 台阶成立 | 按 §0.3 的 123–125 / 66–70 验收 |
| `Copy` 引擎占 `busy_per_frame` 的比例 > 25% | 带宽（而非算力）是约束 | 额外记一条：方案 a（链纹理直接共享）优先级提高，因为它砍掉两次 4K 拷贝 |

### 2.4 可选的补强 G0b：降频判别（免费，10 分钟）

把 GPU 核心频率压到 ~70%（Adrenalin 手动最小频率或功率上限），重测 B1 与 B2：

- **B1 几乎不动** → 6.7 ms 不是 GPU 工作量（是捕获/呈现节奏），则 `max(8.0, …)` 的 8.0 侧可信度提高；
- **B1 同比上升** → 6.7 ms 里有实打实的 GPU 工作，`max` 的上限被抬高，台阶变小。

这条不作为闸门，只用来解释 G0 的结果方向；两档频率各跑 30 s 即可。

---

## 3. 设计

### 3.1 槽位数据结构

```cpp
// FSR3Upscaler::Impl 内新增（示意，实际落在补丁里）
static constexpr uint32_t kMaxSlots = 4;

struct Slot {
    // 一组需要按帧轮转的资源
    winrt::com_ptr<ID3D11Texture2D>        input11;
    winrt::com_ptr<ID3D11Texture2D>        output11;
    winrt::com_ptr<ID3D11Texture2D>        motion11;   // 仅 enableOpticalFlow
    winrt::com_ptr<ID3D12Resource>         input12;
    winrt::com_ptr<ID3D12Resource>         output12;
    winrt::com_ptr<ID3D12Resource>         motion12;
    winrt::com_ptr<ID3D12CommandAllocator> allocator12;
    winrt::com_ptr<ID3D12GraphicsCommandList> commandList12;

    // 记账（全部是 CPU 侧字段，不进 GPU）
    uint64_t islandValue = 0;      // 本槽最近一次岛完成的围栏值（0 = 从未提交）
    uint64_t submittedAtDraw = 0;  // 提交该岛时 Draw 的序号，仅用于日志
    bool     pending = false;      // 该槽的岛已提交、尚未被 CPU 观察到完成
};
```

`Impl` 另需这几个字段：`uint32_t slots`、`uint32_t lag`、`uint64_t drawSerial`（全局 Draw 计数，
槽游标为 `drawSerial % slots`）、`bool failed`、`std::deque<Result> results`。
**槽游标必须按 Draw 调用次数走，不能按 `captureFrameId`**：补帧开启时一个内容帧对应多次 `Draw`
（`findings.md` §2 的比值 2.00 就是证据），按帧号取模会让两个岛撞进同一个槽。

**保持单份（不进环）**：`flatDepth12`、`exposure12`、`reactive12`、`transparency12`、
`zeroMotion12`、`descriptorHeap12`、`fence11/fence12`、`queue12`、`device12`、ffx `context`。
理由：前五个在 `Initialize` 里被清成常量并一次性转到 `NON_PIXEL_SHADER_RESOURCE`
（`:363-373`，`auxBarriers`），**此后只读**，多个岛并发读同一个只读资源是安全的；
`descriptorHeap12` 只在初始化清 aux 时用过（`:334-344`），运行期 `ffxApiGetResourceDX12`
直接收裸资源、不占描述符。

> 注意：`Initialize` 用**唯一一份** `commandList12` 录 aux 清理并在**建围栏之前**提交（`:374-386`）。
> 多槽后必须给这一步单独挑一个 allocator/list（就用 `slot[0]`），`WaitForQueue` 之后再建其余槽，
> 否则 aux 屏障会落在某个还没建的 list 上。

### 3.2 围栏协议：单计数器的按槽记账

约束：两侧共用一个围栏对象、一个单调计数器（§1.1）。因此**值本身就是全序**，
"每槽围栏"不需要新对象，只需要**每槽记一个值**：

```
值空间（每帧两个信号，与上游一致）
  inputReady_k = ++fenceValue   // D3D11 侧，表示"槽 k 的拷入完成"
  islandValue_k = ++fenceValue  // D3D12 侧，表示"槽 k 的岛完成"
  ⇒ inputReady_k < islandValue_k < inputReady_(k+1)
```

三条不变量（违反任意一条都会死锁或读到脏数据；§4 的 F1/F2/F3 是同一组）：

| # | 不变量 | 为什么 |
|---|---|---|
| I1 | 单调：每次 `++` 都在 CPU 提交序上做，值只增 | `WaitForFence` 用 `GetCompletedValue() >= value` 判定（`:62`），非单调会让它提前放行 |
| I2 | **D3D11 只允许等待"其前置 D3D11 信号已经入队"的值** | 保证等待图上无环。阶段 1 的 `Wait(islandValue_k)` 必须排在 `Signal(inputReady_k)` 之后（上游 `:506` 在 `:577` 之前，天然满足） |
| I3 | 一个岛的 `queue12->Wait(inputReady_k)` 同时充当"该槽输出已被 D3D11 读走"的守护 | D3D11 立即上下文 FIFO：`inputReady_k` 之后入队的 D3D11 命令必然排在更早入队的拷贝之后执行 |

F3 是这套设计里最省事的一点：**每个岛只要一次 GPU 侧等待，就同时覆盖了输入侧和输出侧两种危险**。

### 3.3 阶段 1 伪代码（`OUTPUT_LAG=0`，输出语义与上游一致）

标注：**[CPU阻塞]** ／ **[GPU等待]** ／ **[录制]** ／ **[提交]** ／ **[CPU]**

```
Draw(ctx):
  if (impl.failed) return false
  s = impl.drawSerial++ % impl.slots           // 槽游标：按 Draw 调用次数，不按 captureFrameId（FG 下两者不等）
  slot = impl.slot[s]

  # ── 1. 认领槽：只等"自己的"岛 ────────────────────────────────
  if (!WaitForFence(impl, slot.islandValue)) { impl.failed = true; return false }   # [CPU阻塞]
     # 上游这里等的是"上一帧"；现在等的是"3 帧前的那一帧"，稳态下 GetCompletedValue 直接放行

  # ── 2. 拷入 ─────────────────────────────────────────────────
  context11->CopyResource(slot.input11, ctx.input)                                  # [录制]
  if (enableOpticalFlow) {
     if (!frameGuidance.IsValidFor(...)) return false            # 与上游同位置、同语义
     context11->Wait(sync.fence, sync.value)                                          # [GPU等待]
     context11->CopyResource(slot.motion11, guidance.motion.texture)                  # [录制]
  }

  inputReady = ++fenceValue
  context11->Signal(fence11, inputReady)                                              # [录制]
  context11->Flush()                                                                  # [提交]
     # 注：I3 说明这里不需要额外的"等槽可写"的 GPU 守护——CPU 侧 §1 的等待 + 提交序已足够

  # ── 3. 岛的 GPU 侧入口 ──────────────────────────────────────
  queue12->Wait(fence12, inputReady)                                                  # [GPU等待]
  slot.allocator12->Reset(); slot.commandList12->Reset(slot.allocator12, nullptr)     # [CPU]
     # 安全：§1 已保证本槽自己的岛执行完，比上游"靠上一帧整体完成"更精确
  slot.commandList12->ResourceBarrier(per-slot 屏障)                                   # [录制]
  ffxDispatch(desc{color=slot.input12, output=slot.output12, motion=slot.motion12…})   # [录制]
  slot.commandList12->ResourceBarrier(反向屏障); Close()                               # [录制]
  queue12->ExecuteCommandLists(1, {slot.commandList12})                                # [提交]
  islandValue = ++fenceValue; queue12->Signal(fence12, islandValue)                    # [录制]
  slot.islandValue = islandValue; slot.pending = true; impl.lastSubmittedValue = islandValue

  # ── 4. 本帧就要结果（与上游一致）────────────────────────────
  context11->Wait(fence11, islandValue)                                                # [GPU等待]  ← 串行点 2 保留
  context11->CopyResource(ctx.output, slot.output11)                                    # [录制]

  impl.resetHistory = false; if (guidanceReset) impl.lastGuidanceFrameId = ctx.frameId
  return true
```

与上游的差异只有三处：认领槽等的是 `slot.islandValue`、用每槽的 allocator/list、
屏障与 dispatch 指向每槽的资源。**输出帧号、reset 序列、失败行为、日志一行不差。**

### 3.4 阶段 2 伪代码（`OUTPUT_LAG=1`）

新增一个只有一个消费者的 FIFO（`deque<Result>`，`Result{slotIndex, islandValue, drawSerial}`）。
**FIFO 深度恒等于 `lag`（0 或 1）**，不做无界队列。

```
Draw(ctx):
  if (impl.failed) return false
  const bool hadResult = !impl.results.empty()     // 进函数时先记，A 会 pop

  # ── A. 收割：拷出上一帧岛的结果 ───────────────────────────────
  if (hadResult) {
      r = impl.results.front()
      # 只做 GPU 侧等待，绝不 CPU 阻塞（这是阶段 2 的全部意义）
      if (fence12->GetCompletedValue() < r.islandValue) { ++impl.harvestMiss; }   # [CPU 只读]
      context11->Wait(fence11, r.islandValue)                                      # [GPU等待，通常空转]
      context11->CopyResource(ctx.output, impl.slot[r.slotIndex].output11)          # [录制]
      impl.results.pop_front()
      # 到这里 ctx.output 装的是"上一帧输入"的放大+降噪结果
  }

  # ── B. 为"本帧输入"提交一个新岛 ───────────────────────────────
  s = impl.drawSerial++ % impl.slots
  slot = impl.slot[s]
  assert(impl.results.empty() || impl.results.front().slotIndex != s)   # 见 §4 F4
  if (!WaitForFence(impl, slot.islandValue)) { impl.failed = true; return false }   # [CPU阻塞]
  …（与 §3.3 的 2、3 两步完全相同：拷入 → inputReady → 屏障 → dispatch → islandValue）…
  impl.results.push_back({s, islandValue, impl.drawSerial})

  # ── C. 冷启动：进函数时队列就是空的，本帧没有历史可用 ─────────
  if (!hadResult) {
      # 会话第一帧 / Resize 后 / 从失败恢复。此时没有可拷出的历史，
      # 退回阶段 1 的同步语义，把本帧自己的结果拷出（见 §4 F5 的"一帧重复"）
      context11->Wait(fence11, islandValue)                                        # [GPU等待]
      context11->CopyResource(ctx.output, slot.output11)                            # [录制]
  }

  impl.resetHistory = false; if (guidanceReset) impl.lastGuidanceFrameId = ctx.frameId
  return true
```

**为什么 A 放在 B 之前**：`ctx.output` 在这一帧只被写一次，且必须在本类返回前写好（下游会接着
在这张纹理上录命令）。这个顺序**对正确性无关**——见 §3.7 末尾的说明；放在开头只是为了让
"等旧岛 + 拷出"尽早进 D3D11 队列，好让 GPU 在 CPU 继续录制本帧其余命令时就开始这段。

**为什么 A 的等待几乎免费**：`r.islandValue` 是**上一帧**提交的，到本帧开头已经隔了一个帧间隔，
正常情况下 `GetCompletedValue()` 早就过了；`context11->Wait` 仍会作为一条命令进入队列，
但在 GPU 上是一个立即满足的空操作（量级 <10 µs）【待测：用 `harvestMiss` 与帧时间差反推】。

### 3.5 时间轴对照（ASCII）

```
上游 / 阶段 1（LAG=0）：岛嵌在 D3D11 队列里，两段相加
D3D11 GPU │ copyIn_0 │        [等岛0]         │ copyOut_0 │ 链 │ copyIn_1 │ ...
D3D12 GPU │  (wait)  │────── island_0 ───────│
                                     T = 6.7 + 1.4 + 13.7·s²

阶段 2（LAG=1）：D3D11 不再包含本帧的岛，取大
D3D11 GPU │ copyIn_0 │ copyIn_1 │ copyOut(U0) │ copyIn_2 │ copyOut(U1) │ ...
D3D12 GPU │  (wait)  │ island_0 │ island_1   │ island_2 │ ...
                                     T = max(8.0, 1.4 + 13.7·s²)
                        ↑ 输出晚一帧：第 N 帧拷出 U(N-1)
```

### 3.6 显存代价（4K 输入 3840×2054 / 输出 3840×2160，BGRA8）【源码尺寸，字节数自算】

| 项 | 单槽（上游） | 3 槽 | 备注 |
|---|---|---|---|
| input（31.57 MB） | 31.6 | 94.7 | `:318` |
| output（33.18 MB） | 33.2 | 99.5 | `:319` |
| motion（R16G16 @输入尺寸，31.57 MB） | 31.6 | 94.7 | 仅 `enableOpticalFlow`，`:322-332` |
| aux 五张（单份） | 78.9 | 78.9 | 不变 |
| **合计** | **≈175 MB** | **≈368 MB** | 净增 ≈193 MB |

`MAGPIE_FSR_SLOTS=2` 时净增 ≈96 MB。1080p 输入下同比例缩小（每槽 ≈25 MB）。
如果 `enableOpticalFlow` 实际为假（mochinr 不消费运动矢量，但链上是否请求运动矢量**未核**），
3 槽只增 ≈129 MB。

### 3.7 下游耦合：`ctx.output` 不是私有缓冲，是效果链的下游输入【源码】

这是阶段 2 最容易被忽略的一条。`ctx.output` 来自 `effectDrawer.GetOutputTexture()`
（`Renderer.cpp:4110` 构造 `NativeEffectDrawContext`），`Draw` 返回后（`Renderer.cpp:4121`），
链上**后续效果器（`FrameRate_Filter`、`XeSSFG`、补帧等）直接在同一个输出纹理上继续录命令**。
本类没有任何"私有最后一跳"。

因此 `OUTPUT_LAG=1` 的语义必须写成：

> **本效果器把整条下游链的图像整体推后一帧；下游同时拿到的元数据仍然是本帧的。**

| 对象 | `lag=0` | `lag=1` |
|---|---|---|
| 输出像素 | U(N) | **U(N−1)** |
| `ctx.frameId` / `captureSequence` / `HDR metadata` / 运动矢量 / 深度 | 帧 N | 仍为 **帧 N**（`Renderer.cpp:4565-4598` 发布时按本帧打标） |
| 下游效果器（含 FG）看到的输入 | (I(N), meta N) | **(I(N−1), meta N)** |

这不是"本类内部晚一帧"，而是**整条链的内容晚一帧、元数据不晚**。可接受的场景是所有元数据在
相邻帧间缓变（绝大多数内容）；不可接受的是元数据突变——HDR 传输函数切换、镜头切换、
`inputHistoryReset` 那一帧。

一个缓解事实：`ctx.outputMetadata` 现在就已经复制自输入帧元数据（`Renderer.cpp:4106-4109`，
`outputMetadata = inputFrame.metadata`），所以"输出元数据"本身没有新增错位；新增的错位只在
`frameId` / `captureSequence` / `frameGuidance`（运动矢量与深度）这一侧。

**FG 的建议判据（采纳主理人意见）**：`lag=1` **默认只在 FG 关时启用**。理由：FG 的
prev/current 配对整体平移，理论上是自洽的（配对差仍是相邻两帧），但 **FG 的光流/矢量是本帧的、
颜色是上一帧的**，二者组合的插值质量只能实测；在 A/B 通过之前不作为默认。

顺带一条位置说明（§3.4 的"A 放在 B 之前"）：这件事**对正确性无关**——下游 pass 无论
收割放在 `Draw` 的哪个位置，D3D11 队列里都排在 `Draw` 内所有命令之后。放在开头只是为了让
"等旧岛 + 拷出"尽早入队，好让 GPU 在 CPU 继续录制本帧其余命令时就开始这段。

### 3.8 生命周期：`Drain()` / 析构 / `Resize`

`NativeEffectBackend` 已经有现成的收尾契约：虚函数 `Drain()`（`NativeEffectBackend.h:47`），
由 `Renderer::_DrainNgxConsumers`（`Renderer.cpp:3109-3121`）在**停流时**对每个 backend 调用
（不是每帧），失败会打 `Drain native NGX backend queue failed`。`FSR3Upscaler` 目前**没有覆盖它**。

**`Drain()` 只等不补拷。** 三条理由（均【源码】）：

1. 调用点就是停流。在此之前的最后一次 `_PublishBackendTexture` 已经对最后一帧做完
   `Signal → SetEventOnCompletion → Flush → _fenceEvent.wait()`（`Renderer.cpp:4614-4624`），
   **最后一帧早已拷进呈现环交给系统**；再往 `ctx.output` 写一次 U(N) 不会产生任何画面变化。
2. `Drain()` 签名无参数、**拿不到 `ctx.output`**；要补拷就得缓存指针，而它在 `Resize()` 后悬空
   ——为一个视觉零收益的动作新增一条失效路径，风险/收益倒挂。
3. `lag=1` 下停流时最后一帧呈现 `U(N-1)` 是**延迟语义本身**，不是缺陷，不需要补偿。

```
Drain():
  if (!_impl) return true
  if (_impl->failed) return false
  WaitForQueue(*_impl)                       # D3D12 队列排空（等价于等所有槽的 islandValue）
  const size_t dropped = _impl->results.size()
  _impl->results.clear()                     # 清空结果 FIFO：lag=1 时通常剩 1 条
  context11->Flush()                         # 交出 D3D11 侧仍排队的工作
  Logger::Get().Info(fmt::format(
      "fsr3-slot: drain islands={} results={}",
      _impl->drawSerial, dropped))   # 每个 Draw 恰好提交一个岛，故 drawSerial 即岛数
  return true
```

要点：**必须等所有已提交的岛完成**（不是"直接返回"），否则析构/停流会释放仍被 GPU 引用的
共享纹理；**必须清空** `results` 并把丢了几帧写进日志，不许静默保留 pending 结果；
**不得写入已发布的 `ctx.output`**。

`Resize()`（`:484-487` → `Initialize()`）：重建槽环、清空 `results`、清 `failed`，
之后再吃一次 §4 F5 的"一帧重复"。**`Drain()` 不得释放资源**，只交出工作与记账——资源释放仍由
`Initialize` / 析构负责。

### 3.9 先例与差异：`mochinr` 的 `Submit()` 环

本设计不是新发明。`src/mochinr/mochinr.cpp` 的 `Submitter`（`:286-410`）已经是"多槽 + 每槽
allocator/list + 每槽记账"，并且已在实机 110–130 fps 长时间跑过：

| 手法 | mochinr 出处 | 本设计对应 |
|---|---|---|
| 环形槽 | `constexpr UINT kRing = 32;`（`:284`）、`alloc[kRing]` / `list[kRing]` / `slotValue[kRing]`（`:294-296`） | `Slot slots[N]` + `slot.islandValue` |
| **复用前只等本槽自己上一次的值** | `slot = value % kRing; if (slotValue[slot] != 0) WaitSlot(slotValue[slot]);`（`:386-387`） | `WaitForFence(slot.islandValue)` |
| 之后才 Reset | `alloc[slot]->Reset()` / `list[slot]->Reset()`（`:388-390`） | 同 |
| 快路径 + 事件 + 统计 | `WaitSlot()`（`:360-370`）：`GetCompletedValue()` 快路径，否则 `SetEventOnCompletion` + 4000 ms 上界，累计 `waits`/`waitMs` | `WaitForFence`（`:61-70`）+ §5 的 `slotWaitAvgMs` |

**两处关键差异，不要照搬：**

1. **环大小分两级，不要用同一个常数。** mochinr 的 32 是**命令环**（allocator + command list，
   显存几乎为零，多开不心疼）；宿主的**纹理槽**只有 2–3 个（4K BGRA 每槽 ≈33 MB，加 motion ≈96 MB，
   见 §3.6）。补丁里这两个数必须分开写，且槽数受 §3.6 的显存预算约束。
2. **宿主侧多一层 D3D11 GPU 侧等待。** mochinr 的 `WaitSlot` 是**纯 CPU 侧阻塞**，用在"重置本槽
   allocator 之前"完全够用，因为它覆盖的是命令环。宿主要覆盖的是**共享纹理**——写它的是
   **GPU 命令流**而不是 CPU 内存，所以还需要 `context11->Wait(fence11, value)` 这一层
   （阶段 2 的收割点，§3.4 A 步）。**mochinr 没有这一层，别漏。**

这条同时回答了"运行时能不能同时吃两帧在飞"：mochinr 的 `inflight` 上限可调到 16（默认 3，
`:877`）与 `kRing=32` 说明**它在自己的提交环节并不强制单帧串行**。但**运行时内部**是否串行
仍属【待测】（`bridge.md` §3 的原地采样与时间线信号量都可能隐式串行化），列为风险 R2，
观测点是 `mochinr.log` 的 `perf:` 行：**`waits=` 与 `waitMs` 是否随槽数增加而上升**——
上升说明限流点已经挪进运行时，多槽的收益天花板由它决定（§8 Q2/Q3）。

### 3.10 不在本次范围：`FSR3ZeroMVUpscaler`

`FSR3ZeroMVUpscaler.cpp`（613 行）是与目标文件**同构**的另一份实现，`:589-599` 是同一套
`Signal → context11->Wait → CopyResource` 收尾。**本补丁只改 `FSR3Upscaler`**，
ZeroMV 那条路径**保持单槽**。原因：ZeroMV 变体在链里不承载 NR（`bridge.md` §1 的岛来自
`FSR4\FSR4_SR`），拆它没有收益、只增加风险面。写在这里，防止以后误以为"已经统一"。

---

## 4. 不变量与失败模式

| # | 不变量 / 场景 | 要求 | 违反后果 |
|---|---|---|---|
| F1 | 围栏值单调（§3.2 I1） | 每次 `++` 在提交序上 | `GetCompletedValue` 快速放行 → 读到未写完的纹理（花屏） |
| F2 | 等待图无环（§3.2 I2） | D3D11 只等"前置信号已入队"的值 | 死锁（`WaitForFence` 无限等，`:68`） |
| F3 | 每岛一次 `queue12->Wait(inputReady)` 兼作输出侧守护（§3.2 I3） | 必须保留 | 隔帧复用槽时 D3D12 写与 D3D11 读相撞 |
| F4 | **槽复用年龄 `slack = slots − lag ≥ 1`** | `lag=1 ⇒ slots ≥ 2`；`slots ≥ 3` 时 slack=2 才有"CPU 不阻塞"的余量 | `slack=0` → 同帧同槽既收割又提交 → 数据错乱；`slack=1` → 岛超一帧就退回 CPU 阻塞 |
| F5 | **首帧 / Resize 后无历史可拷出** | 冷启动退回同步语义，接受**一帧重复**（U(1) 出现两次） | 不处理 → 呈现一帧未初始化纹理（黑帧/花屏） |
| F6 | `resetHistory` / `isNewCaptureFrame` / frameGuidance reset | **不改语义**：reset 仍只作用于本帧提交的岛（`desc.reset` 就地消费，`:551`/`:580`） | 改错 → 每帧复位历史 → 场景局部闪烁（`findings.md` 判据） |
| F7 | `Draw` 返回 false 后的状态一致性 | 任何在"认领槽之后"的失败 → `impl.failed = true`，之后每次 `Draw` 立即返回 false，`results` 清空；主机侧不会崩：`!nativeDrawSucceeded` 时改走标记 pass 产出可见占位（`Renderer.cpp:4166-4174`），NR 状态下记 `DlssNrState::Stopped`（`:4122-4132`） | 继续按旧槽值提交 → 提交到已失效的 allocator（D3D12 会 latch `E_INVALIDARG` 并污染整个设备，`mochinr.cpp:787` 的注释是同一类教训） |
| F8 | 设备移除 `0x887A0005` / `0x887A0001` | `Reset`/`Close`/`ExecuteCommandLists` 任一返回即置 `failed`，**不在本类内自愈**；`ComError` 记一次 | 自愈会重演"停效果组再启用"的引用计数红字（`findings.md` 未解问题 3） |
| F9 | 无 NR（只有 `FSR4_SR`）与只开补帧路径 | 不得回归：`slots=3, lag=0` 时行为等价；快路径（无 optical flow、`barrierCount=2`）逐字保留 | 回归即判定阶段 1 失败，回滚 |
| F10 | **FG 与 `lag=1` 的组合** | `lag=1` 下输出像素是 `U(N-1)`，但 `ctx.output` 是**整条下游链的输入**（`Renderer.cpp:4110`/`:4121`，见 §3.7），下游拿到的是"旧图像 + 本帧元数据"（`Renderer.cpp:4565-4598`） | 元数据与内容错位一帧：HDR 元数据突变时可见；FG 用本帧矢量插值上一帧颜色 → **待测**，见 §6.4 与 B9 |
| F11 | `mochinr` 的 `inflight` 门 | 必须 `inflight ≥ slots + lag`（3 槽+lag1 → **≥4**），部署包当前为 2（`mochinr.cpp:877-895`） | 门在岛的提交路径中间做 **CPU 阻塞等待（≤200 ms）**，阶段 2 会在这里被限流，表现为"改了但没变" |
| F12 | 收尾契约：`Drain()` 只等不补拷 | **必须**等所有已提交岛完成并清空结果 FIFO，并以 `fsr3-slot: drain islands=<n> results=<m>` 记录丢弃帧数（§3.8）。主机在停流时对每个 backend 调用（`Renderer.cpp:3109-3121`） | 不得静默保留 pending 结果（会漏掉"丢了几帧"这个证据），也**不得写入已发布的 `ctx.output`**（`Renderer.cpp:4614-4624` 已把最后一帧交出去，补拷是零收益且新增悬空指针路径）；反过来若不等就返回，析构会释放仍被 GPU 引用的纹理 |
| F13 | `WaitForFence` 超时 | 仅 Draw 内新增的按槽等待加 4000 ms 上界（对齐 `mochinr.cpp` 的 `WaitSlot`），超时置 `failed`；`~Impl` 路径保持无限 | 设备挂死时会表现为"卡死而不是报错" |
| F14 | 范围边界：`FSR3ZeroMVUpscaler` | 保持单槽，本次不改（§3.10） | 以后误判"已统一"，或补丁被意外扩散到同构文件 |
| F15 | 运行时内部串行 | 若 `mochinr`/运行时内部把两个岛串起来，多槽收益归零；判据是 `perf:` 行的 `waits=` 随槽数上升（§3.9、§8 Q2） | 归属错误：会去改宿主，实际限流点在运行时 |

> F5 的"一帧重复"值得单独说：开局/Resize 后第 1、2 帧显示同一内容，是一次 ~8–20 ms 的画面静止，
> 不是"未降噪帧"，也不会重复发生。**由此定一条实现纪律：`OUTPUT_LAG` 只在 `Initialize` 读一次，
> 不允许运行期切换**——运行期切换等价于一次 Resize 级抖动。

---

## 5. 配置面

**不动 XAML 设置界面**（`ScalingOptions` / 面板四页签一律不改）。两个环境变量，`Initialize()` 里读一次：

| 变量 | 默认 | 范围 | 含义 |
|---|---|---|---|
| `MAGPIE_FSR_SLOTS` | **3** | 1–4 | 环形槽数。`1` = 完全回到上游行为（含单 allocator） |
| `MAGPIE_FSR_OUTPUT_LAG` | **0** | 0–1 | `0` = 本帧输入本帧输出；`1` = 晚一帧（阶段 2） |

约束在 `Initialize` 里落地并**修正而非报错**：`lag=1 && slots=1 → slots=2`；非法值回落到默认。
生效值打一行到 `magpie.log`：

```
FSR3 slot ring: slots=3 outputLag=0 (from MAGPIE_FSR_SLOTS/MAGPIE_FSR_OUTPUT_LAG)
```

再加一行统计（每 120 个 Draw 一次，与主机 `Backend publication CPU timing` 同频，便于对齐）：

```
FSR3 ring stats: draws=120 slotWaitAvgMs=0.004 harvestMiss=0/120 lag=1 slots=3 framesDropped=0
```

- `slotWaitAvgMs`：§3.3 第 1 步 / §3.4 B 步那个 CPU 等待的均值。**阶段 1 的核心读数**（应 →0）。
- `harvestMiss`：收割时岛还没完成的次数。**阶段 2 的核心读数**（应 →0，>5% 说明岛比帧间隔长）。
- `framesDropped`：`Draw` 返回 false 的次数（应为 0）。

停流时另有一行（只打一次，`Drain()` 里）：

```
fsr3-slot: drain islands=6431 results=1
```

- `results` 是收尾时被清空的结果 FIFO 长度，`lag=1` 应为 1（`lag=0` 为 0）。**它是"最后一帧按
  延迟语义丢掉"的唯一证据**，也是 B10 的判据；`Drain()` 不做补拷（§3.8）。

`Resize()` 走 `Initialize()`，因此会重新读环境变量并重建槽环（顺带再吃一次 F5 的一帧重复）。

---

## 6. 验收标准与回滚

前置：G0（§2.3）已通过。以下每条都要有"通过线"和"记失败"两档。

### 6.1 阶段 1（`slots=3, lag=0`）

| # | 指标 | 通过线 | 记失败 |
|---|---|---|---|
| A1 | 语义等价 | 输出帧号与 `slots=1` 一致；`reset` 序列一致；`magpie.log`/`mochinr.log` 无新增 warning | 出现新的 `E_INVALIDARG`/`DEVICE_REMOVED` |
| A2 | **性能：应当不变** | 平衡点与 `slots=1` 差 ≤ ±1 fps；`fenceAvgMs` 差 ≤ 0.3 ms；`slotWaitAvgMs ≤ 0.05` | 掉 ≥3 fps（说明槽环引入了新等待） |
| A3 | 画质 | 静态场景（暂停游戏）连续 200 帧与 `slots=1` **逐位一致** | 任何一帧不一致 → 并发/复用协议错 |
| A4 | 稳定性 | 30 min，s=0.5，无 TDR；显存增长 < 50 MB | — |
| A5 | 无 NR 路径 | 仅 `FSR4_SR`（禁用 mochinr）跑 60 s，平衡点 ≥ 123 fps 且无新错误 | 掉到 118 以下 |

> A2 的"应当不变"是**设计断言**（§1.3）。若实测显著变好，那是意外发现，先记录、不要急着庆祝：
> 更可能的解释是 G0 的 `busy` 与预期不符。

### 6.2 阶段 2（`slots=3, lag=1`）

| # | 指标 | 通过线 | 记失败 |
|---|---|---|---|
| B1 | `harvestMiss` 比例 | ≤ 5% | > 20% → 岛比帧间隔长，退阶段 1 |
| B2 | **0.5 档**（主验证点） | ≥ 118 fps（B2 天花板 123–125 的 95%） | < 108 fps（即没超过现状 90 的 1.2 倍） |
| B3 | **0.35 档** | ≥ 118 fps | < 108 |
| B4 | **1.0 档** | ≥ 63 fps（模型 66、roadmap 70，取 63 留残差） | < 55 fps |
| B5 | `fenceAvgMs` | 相对阶段 1 至少降 1 档岛的 80%（0.5 档：应降到 ≈4.5±1；参照点：NR 全关、125 fps 时 `fenceAvgMs` 实测 4.2，见 `measurements.md` §2） | 不降 → 岛仍留在 D3D11 队列里，补丁没生效 |
| B6 | 画质 | 与 `slots=1/lag=0` 逐帧平均绝对差 ≤ 噪声基准；**无"整帧未降噪"**（全屏闪烁判据：`findings.md` §2026-10-31 补充） | 出现全屏闪烁 ≈ 步了 `BridgePace=0` 的后尘 |
| B7 | 延迟 | 文档明示 +1 帧（s=1.0 时 ≈ +21 ms，s=0.5 时 ≈ +8 ms） | — |
| B8 | 稳定性 | 30 min，s=0.5，`inflight=4`，无 TDR | — |
| B9 | FG 交互 | `lag=1` + FG 2x 长跑 10 min：无抖动、生成帧无错位 | 出现插值抖动 → FG 下 `lag` 强制 0 |
| B10 | 停流收尾 | `magpie.log` 出现 `fsr3-slot: drain islands=<n> results=<m>`，且 `results` 与实际 lag 相符（`lag=1` 时通常为 1）；停流无黑帧、无回退帧、无 `Drain native NGX backend queue failed` | 无 `drain` 行 → 未覆盖 `Drain()`；`results` 与 lag 不符 → 结果 FIFO 记账错 |

### 6.3 「A 档 1.0 无改善就算失败吗」

**不算。** 判据是 §0.4 的模型：1.0 档 `max(8.1, ~14.3~15.1)` 由岛主导，改善只应体现为
`21.5 → 15.1`（约 −6.4 ms / 47→66 fps）。若落在 66±1，说明"求和→取大"这一步完全兑现，
1.0 档的残余是网络本身，与单槽无关。**真正算失败的只有两种**：

1. 0.5 / 0.35 档没有从 11.3 / 9.5 ms 降到 8.1±0.4 ms（网络重叠没拿到）；
2. G0 显示 `busy > 0.85`（台阶本不存在，整条主线作废）。

### 6.4 何时不该开阶段 2（默认就该留在 `lag=0`）

**基本规则：`lag=1` 默认只在 FG 关时启用。** FG 开时需单独跑 B9 的 A/B 才能开——因为
`ctx.output` 是整条下游链的输入（§3.7），补帧看到的图像整体晚一帧、矢量与深度却是本帧的，
prev/current 配对整体平移"理论自洽、实际待验"。

| 情形 | 理由 |
|---|---|
| G0 的 `busy > 0.85` | `max` 拿不到东西 |
| `harvestMiss > 5%`（岛比帧间隔长） | 晚一帧的余量不够，等于白加延迟 |
| 开 FG 且 B9 未通过 | 颜色晚一帧、矢量与深度是当帧的（F10、§3.7），插值可能抖 |
| `mochinr` 的 `waits=` 随槽数上升 | 限流点已挪进运行时（F15），宿主侧再优化没有收益 |
| 竞技类/开 Reflex 的场景 | +1 帧（8–21 ms）是实打实的输入延迟，用户不可接受 |
| HDR 元数据频繁突变的片源 / 频繁镜头切换 | F10 的元数据错位可见，且 `inputHistoryReset` 那一帧尤其明显 |
| 只开 `FSR4_SR`、无 NR | 岛只有 1.4 ms，`max(8.0, 1.4)` 与求和几乎一样，加了延迟没有收益 |

### 6.5 回滚条件

| 触发 | 回滚动作 |
|---|---|
| 任意 corrupted frame（A3/B6 失败） | 默认值改回 `slots=1, lag=0`，补丁留在库里但不默认启用 |
| 30 min 内一次 TDR / `DEVICE_REMOVED` | 同上 |
| B2/B3 未达标但 A 组全过 | 保留阶段 1（无害），阶段 2 标"未通过"并记录实测，不合并到默认 |
| G0 未通过 | 阶段 2 取消；阶段 1 仅作为未来方案 a 的地基保留 |

---

## 7. 实施拆分

补丁一律落在 `repo/src/magpie-patches/`（该目录**当前不存在，需新建**），构建/测量脚本落在
`repo/scripts/`。与 `src/opennr-patches/opennr-0.7.15-magpie-nr.patch` 同一惯例：
**只入库补丁文本与说明，不分发宿主源码**；补丁头写清上游仓库、分支、基准提交 `6940f84`，
用 `git format-patch` 生成（`git diff` 亦可，但要在说明里写明应用命令）。

### 7.1 文件清单

| 文件 | 作用 |
|---|---|
| `src/magpie-patches/README.md` | 基准提交、应用/回退命令、编译开关（`MP_ENABLE_FSR3_ZEROMV`）、许可说明、与 `mochinr` 的 `inflight` 联动 |
| `src/magpie-patches/0001-fsr3-slot-ring.patch` | 阶段 1：槽位结构 + 每槽围栏 + 日志行 + `Drain()` |
| `src/magpie-patches/0002-fsr3-output-lag.patch` | 阶段 2：结果 FIFO + `OUTPUT_LAG` + `harvestMiss` + 冷启动同步回退（`Drain()` 已在 0001 落地，只多清一条 FIFO） |
| `scripts/apply-magpie-patches.ps1` | 克隆/校验 `6940f84` → 依次 `git apply` → 冲突时明确报错不猜 |
| `scripts/build-magpie-fork.ps1` | 在打补丁的 fork 上出 `Magpie.exe`（独立于 `build-all.ps1`，因为宿主不是本仓库组件） |
| `scripts/measure-gpu-busy.ps1` | G0：按 PID 采 `\GPU Engine(*)` 计数器，输出 `busy`/`frame_time`/`Copy` 占比 |
| `scripts/measure-fsr-ring.ps1` | 三档（1.0/0.5/0.35）× 三配置（slots=1 / slots=3lag0 / slots=3lag1）跑批 + 抓日志判据 |
| `tools/slot_model.py` | 求和/取大两套模型的调度仿真：输入往返时间分布，输出预测 fps 与 `harvestMiss` 期望 |
| `tools/README.md` | 仿真的输入口径与"怎么用实测反推参数" |

### 7.2 任务列表（有序，含依赖）

| ID | 任务 | 源文件 | 依赖 | 优先级 |
|---|---|---|---|---|
| **T01** | 补丁工程与 G0 判别工具：建 `src/magpie-patches/`，写 README/应用脚本/构建脚本/GPU 忙采集脚本。**不含任何源码改动**，产出可先跑 G0 | `src/magpie-patches/README.md`、`scripts/apply-magpie-patches.ps1`、`scripts/build-magpie-fork.ps1`、`scripts/measure-gpu-busy.ps1` | — | **P0** |
| **T02** | 阶段 1 补丁：`Slot` 结构、槽环创建、每槽 allocator/list、按槽围栏、`MAGPIE_FSR_SLOTS`、两行日志、`Drain()` 等槽基线（只等不补拷，§3.8）、`~Impl` | `src/magpie-patches/0001-fsr3-slot-ring.patch`（改 `FSR3Upscaler.cpp`、`FSR3Upscaler.h`）＋ 在本文 §6.1 追加实测记录 | T01 | **P0** |
| **T03** | 阶段 2 补丁：输出晚一帧（结果 FIFO + `MAGPIE_FSR_OUTPUT_LAG` + `harvestMiss` + 冷启动同步回退）。**不含 `Drain()` 改动**——阶段 2 只需要 `Drain()` 多清一条 FIFO，基线已在 T02 交付 | `src/magpie-patches/0002-fsr3-output-lag.patch`（同上两文件）＋ 本文 §6.2 追加实测记录 | T02 | **P0** |
| **T04** | 调度模型仿真：把 §0.3 的求和/取大、§4 F4 的 `slack`、§6.2 B1 的 `harvestMiss` 判据写成可跑脚本，输入实测五点做校验 | `tools/slot_model.py`、`tools/README.md` | T01（可与 T02/T03 并行） | **P1** |
| **T05** | 跑批与判据汇总：三档三配置的批处理、日志解析、自动给出 §6 的通过/失败表 | `scripts/measure-fsr-ring.ps1`、`tools/README.md`（判据段） | T02、T03 | **P1** |

**为什么 T02 不能再拆**：`Slot` 结构、槽环创建、allocator/list 重置点、围栏记账四件事互相咬死
（少一个就是脏读或死锁），拆开交付没有可验证的中间态。T03 必须叠在 T02 之上（同一个文件、
同一段代码），因此补丁按 0001/0002 顺序应用、**不许合并成一个补丁**——回滚单位就是补丁单位。

### 7.3 共享约定（给工程师）

```
- 一律 C++20 + `noexcept` 风格，与上游 FSR3Upscaler 保持一致；不引入新第三方依赖
- 日志统一走 Logger::Get().Info/ComError，一条信息只打一次；不要 printf、不要 OutputDebugString
- 不改 ScalingOptions / XAML / 任何 UI 代码（本文 §5）
- 不改 Renderer.cpp（串行点 3 属于另一条主线，改了就无法判断收益归因）
- 不改 FSR3ZeroMVUpscaler.cpp（§3.10：同构实现，本次保持单槽）
- 补丁必须能在 `6940f84` 上 `git apply` 成功；若上游漂移，先 rebase 补丁再谈功能
- 所有新增资源在 Initialize 内按 slots 一次建齐，运行期不分配
- 失败即 poisoned（§4 F7），不做局部重试
- `Drain()` 是收尾的唯一出口：等完所有已提交的岛、清结果 FIFO、打 `drain` 行；不补拷、不静默（§3.8、F12）
```

---

## 8. 待测清单（显著标注，不要当结论引用）

| # | 待测项 | 怎么测 | 影响的结论 |
|---|---|---|---|
| Q1 | GPU 忙占帧时间比例（G0） | `scripts/measure-gpu-busy.ps1`，s=0.5/0.35 | 整条主线是否成立（§2） |
| Q2 | `mochinr` 的 `inflight` 是否会在 `slots=3` 下变成新瓶颈 | `perf:` 行的 `waits=`/`waitMs`，对比 `inflight=2/4/6` | §4 F11 |
| Q3 | 两个岛并发执行时 ffx/运行时是否正确（私有输出是否按 job 分配） | 静态场景逐位比对（A3）；`mochinr.log` 是否有 `input check: the copy never landed` | 阶段 1/2 的正确性底线 |
| Q4 | optical flow 在本链里是否真的启用（决定显存与屏障数） | `magpie.log` 的 `FSR ... backend initialized (opticalFlow=?)` | §3.6 显存、§4 F9 |
| Q5 | `lag=1` 与 FG 2x 的组合是否抖动 | 10 min 长跑 + 逐帧差（B9） | §6.4 的"FG 默认不启用 lag"判据 |
| Q6 | 收割等待本身在 GPU 上的实际代价 | `harvestMiss` 比例 + 帧时间差反推 | §3.4 的"几乎免费"假设 |
| Q7 | `lag=1` 的真实显示延迟增加值 | PresentMon 的 `msUntilDisplayed` 前后对照 | §6.2 B7 的"≈8–21 ms"是推算值 |
| Q8 | **运行时内部是否串行**（多槽收益的真实天花板） | `slots=1/3` 两配置下比 `mochinr.log` 的 `waits=` 与 `waitMs`；同升则限流点在运行时 | §4 F15、§3.9；决定阶段 2 是否还有意义 |
| Q9 | `Drain()` 的等与清是否正确 | 对比 `fsr3-slot: drain islands=<n> results=<m>` 与日志里累计的 `draws=`：`islands` 应等于总 Draw 数，`results` 应等于 lag；停流无黑帧/回退帧 | §3.8、F12、B10 |

---

## 9. 出处

**上游源码**（`SAOG0721/Magpie`，`experimental`，`6940f84`）

- `src/Magpie.Core/FSR3Upscaler.cpp`：`:61-70`（`WaitForFence` 两条路径）、`:72-77`（`WaitForQueue`）、
  `:186-214`（`CreateSharedTexture`）、`:216-257`（`CreateAuxTexture`）、`:263-482`（`Initialize`）、
  `:318-319`（共享纹理单份）、`:322-332`（运动矢量共享纹理）、`:363-373`（aux 清理与屏障）、
  `:374-386`（aux 提交与建围栏）、`:484-487`（`Resize`→`Initialize`）、`:489-583`（`Draw`，逐行见 §1.1）
- `src/Magpie.Core/FSR3Upscaler.h`：`:11-43`（类声明，`Impl` 前向、`Drain` 未覆盖）
- `src/Magpie.Core/FSR3ZeroMVUpscaler.cpp`：`:589-599`（同构的
  `Signal → context11->Wait → CopyResource` 收尾；本次不改，见 §3.10）
- `src/Magpie.Core/NativeEffectBackend.h`：`:13-32`（`NativeEffectDrawContext`，含
  `isNewCaptureFrame` / `inputHistoryReset` / `frameGuidance`）、`:47`（`Drain()` 虚函数）、
  `:90`（`Draw` 虚函数）
- `src/Magpie.Core/Renderer.cpp`：`:2944`（`_nativeEffectBackends[i]->Draw`）、
  `:3109-3121`（`_DrainNgxConsumers`，对每个 backend 调 `Drain()`）、
  `:4106-4110`（`NativeEffectDrawContext` 构造，`.output = effectDrawer.GetOutputTexture()`）、
  `:4121`（主 `Draw` 调用点，其后下游效果器继续用同一输出纹理）、
  `:4122-4132`（NR 状态上报）、`:4166-4174`（原生 backend 失败时走标记 pass，不发布清屏输出）、
  `:4565-4598`（发布会话与元数据绑定）、
  `:4614-4624`（**串行点 3**：Signal + Flush + 等排空）、`:4628-4638`（`fenceAvgMs` 定义与 120 帧日志）

**本仓库源码**

- `src/mochinr/mochinr.cpp`：`:284`（`kRing = 32`）、`:286-410`（`Submitter`：每槽 allocator/list +
  `slotValue` + `WaitSlot`——**同一套槽环模式的既有先例**，`:384-397` 是"复用前只等本槽自己的值 →
  再 Reset"的完整写法）、`:360-369`（`WaitSlot`，快路径 + 4000 ms 上界）、
  `:738`（`private_output` 逐 job）、`:786`/`:836`（每帧两次 `Submit`）、`:877-895`
  （`inflight` 背压，CPU 阻塞 ≤200 ms）、`:898-901`（`perf:` 行 `waits=`/`inplace=`/`copied=`）、
  `:787`（"transition to itself 会 latch `E_INVALIDARG` 并污染设备"的既有教训）
- `src/opennr-patches/opennr-0.7.15-magpie-nr.patch`（补丁文本入库的既有惯例）

**本仓库文档**

- `docs/bridge.md` §1（真实通路）、§8.1（单槽交接是串行点）、§8.2（三个候选解释）、
  §8.4（方向 3 / 方向 4 的区别）、§8.5（方案 a / b / c，以及"桥拆不掉但可以省拷贝、拆串行"）
- `docs/findings.md` §「2026-10-31 补充」（两种闪烁的判据、`BridgePace=0` 的 2199/13920）、
  §「2026-11-02 补2：A/B 天花板实测闭环」（B1/B2/A 五点与 `6.7 + 1.4 + 13.7·s²` 模型）
- `docs/measurements.md` §1（lmxxf 对照）、§2（FG 2x 下 NR 评估次数 2.00）、§3（`nrbench` 线性）、
  §「2026-10-31 补充」（预算模型闭合）
- `docs/roadmap.md` §「当前瓶颈」第 3 条、§「2026-11-02 增补」第 1 条
- `docs/architecture.md` §1（全链路）

**数据归属**

- B1 150 fps / 6.67 ms、B2 123–125 fps / 8.0–8.1 ms、A@1.0/0.5/0.35 = 47/90/105 fps
  ＝【实测】，原档 `D:/Magpie-NR-045/_test-archive/AB-20261102/RESULTS.md`（本文未复测）。
- §0.3 的求和/取大两列、§3.6 的字节数、§6.2 B4 的 63 fps ＝本文推导。

---

*本文不构成"已优化"的声明。G0 未过之前，§0.3 的台阶是**条件预测**，不是收益承诺。*
