# 结论、撤回的结论与未解问题

这份文档只记「现在能站得住的判断」，以及**被推翻的判断**——后者同样重要，避免后来的人
沿着已经证伪的思路再走一遍。数据出处见 [measurements.md](measurements.md)。

---

## 1. 已经确认

| # | 结论 | 证据 |
|---|---|---|
| 1 | Magpie 里 Home 打不开面板，是三处叠加：`GetAsyncKeyState` 被 ReShade 屏蔽、`PostMessage(WM_MOUSEMOVE)` 被 ReShade 丢弃、`MSG.pt` 与 `lParam` 相同导致坐标被二次换算 | 三版对照 + `test-new\*.bmp` |
| 2 | 两条 NR add-on 不能同时加载，会互相抢 present | 旧包用 `.off` 互斥；同时加载时帧生成异常 |
| 3 | mochizuki 的 ACO 判定**不兼容非 ASCII 路径**，同一套资产换目录即失效 | `ACO asked for, but ... has no records` vs `ACO: the network runs ACO's machine code` |
| 4 | OpenNR 在收到 swapchain/设备销毁后会把自己置为 off，且**不会自动恢复**，只有重启进程才重新挂载 | `off: the game destroyed its D3D12 device; restart the game to re-enable` |
| 5 | 因此：Magpie 里换效果组 = 停缩放 = 设备销毁 = NR 失效。这不是"效果组坏了" | 同上；停/开捕获无效，重启 Magpie 有效 |
| 6 | 真实 4K 下 NR 不是只有网络那 5.3 ms：光流、双向拷贝、桥接都在里面 | `optical flow: FidelityFX, 3840x2160 frame ... into the 1920x1080 raster` |
| 7 | `Scale` 只缩网络，不缩光流与拷贝；0.6→0.5 只省约 1.9 ms/次 | 同上 + `nrbench` 线性关系 |
| 8 | 补帧开启时，NR 的成本按倍率翻倍（mochizuki 有逐帧计数，比值 2.00） | `after 3311 frames` / 49.1 s / 33.6 fps |

---

## 2. 为什么基准里的领先没有出现

上游基准：mochizuki 3.27 ms vs lmxxf 5.487 ms（同为 1080 档）。实机却是 64 帧 vs 71 帧。

```
lmxxf      ：14.08 ms = 游戏 + 5.49 (网络) + 大约 1 ms (拷贝/回写)
mochizuki  ：15.63 ms = 游戏 + 3.27 (网络) + 光流 + 4K 拷贝 + 桥接等待
```

两边 `游戏` 相同，网络我们省 2.22 ms，但要额外付出约 3.77 ms 的非网络开销，净亏 1.55 ms。

这 3.77 ms 里最大的一块是 **4K FidelityFX 光流**：DLSS5/lmxxf 的时域不使用运动矢量，
根本没有这一步；mochizuki 的时域要对齐历史，就必须有矢量，而它把矢量算在帧尺寸上。

> 这一条只到"量级吻合"。光流具体占几毫秒尚未单独测出，下一次实验就是把它关掉/降级来量。

---

## 3. 已撤回的结论

| 曾经的判断 | 为什么撤回 |
|---|---|
| 「原生 34 帧是帧同步量化（错过一格等一整格）」 | 0.6.11 上 NR 开/关对照：NR 关 50 帧、NR 开 33–34 帧，且 `fenceAvgMs` 从 4.2 涨到 12.5。34 是真实开销，不是节奏问题 |
| 「Bicubic 替掉 4K XeSS_SR 省 6–7 ms」 | 那两段日志里 NR 已被停用，比较条件不成立；且按要求已回滚为 XeSS_SR，不再采用 Bicubic |
| 「x3 比 x2 慢 2.9 ms，说明 NR 每帧多跑一次」 | 9070 XT 已跑满 304 W，FG 自身多生成一帧就能解释这个差值，不能作为 NR 计数的证据 |

第三条的教训：**只有在同一倍率下开关 NR 的差值**，才能把 NR 的成本从补帧里分离出来。

---

## 4. 未解问题

1. **3.77 ms 的构成**：光流 / 拷贝 / 桥接各占多少。下一步把 Motion 从 FidelityFX 换成
   内置 estimator，再关掉，看 64 → ? → ?。
2. **lmxxf 到底跑几次**：它没有逐帧日志，只有 `on_present` 符号。若它确实每个呈现帧都跑，
   说明它单次足够便宜；若只跑一次，说明挂载点不同，需要查它绑的是哪个 swapchain。
3. **x3 下的 D3D12 报错**：怀疑是每次内容帧 3 次评估把桥接的在飞预算打满
   （addon 内有 `four jobs already in flight`、`network skipped: the last list that read the
   descriptor heap is still on the GPU` 这类保护）。未验证。
4. **停机状态能否自愈**：目前只能重启进程。若 OpenNR 有未暴露的恢复开关，值得找。

---

## 5. 判断与建议

- **瓶颈已经不在网络**。3.27 ms 是真的，`nrbench` 也复现了 3.40 ms。继续在网络参数上抠
  没有意义，下一步只动 Motion。
- **4K 下的固定开销是结构性的**。NR 挂在 present 上，看到的永远是 4K 输出帧；只要
  ReShade 在 Magpie 进程里，这一层就省不掉。除非把 NR 挪到补帧之前——那需要改宿主，
  已明确不走注入游戏的路（安全考量优先）。
- **不要为了省几毫秒去改 Magpie 的效果链**。`XeSS_SR` 虽然 1:1 看起来像空转，但它是
  SR 台阶与纹理尺寸的来源，换掉它收益不确定、风险明确（已试过一次并回滚）。
- **补帧与 NR 在 4K 下会互相放大**：倍率乘 NR 成本，这一点在选 2x / 3x 时要算进去。
  纯净对照（无补帧）下我们 64、对方 71，差距 1.55 ms，属于可以继续收窄的量级。
