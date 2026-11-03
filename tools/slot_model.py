#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""FSR3Upscaler 单槽/多槽调度的**纸面模型**（离散事件仿真）。

    python tools/slot_model.py        # 无第三方依赖，退出码 0 = 模型自洽

## 这个文件是什么

把 `docs/multislot.md` §0.3 的「求和 / 取大」两套预测、§4 F4 的 `slack`、§6.2 B1 的
`harvestMiss` 判据写成可跑的东西，并用 `docs/findings.md` 末尾「A/B 天花板实测闭环」的
**五点实测**做校验。

## 这个文件不是什么

**它是纸面模型，不是实测。** 三件事必须分清：

1. 模型里的三个常数（6.7 ms 捕获/呈现、1.4 ms FSR4 槽位、13.7 ms 网络）是**从实测反推**出来的，
   所以下面那张校验表是**自洽性检查**，不是独立验证——常数本来就是为了拟合这五点而选的。
   它的价值在于：改动模型结构（比如把 `slack` 从 2 改成 1）时，能立刻看出五点会不会崩。
2. 「`max()` 能不能拿到东西」取决于 **GPU 忙时间占帧时间的比例**，那要由 G0 判别实验
   （`docs/multislot.md` §2）在真机上量，本文件给不出结论。`ring_lag1` 的预测只在
   **G0 通过（GPU 未占满）** 的前提下成立。
3. 本文件**不测量任何东西**，也不读取日志、不访问显卡。

## 模型被证伪时改哪里

**改这个文件，不要改文档里的实测结论。** 如果实测与模型冲突，先怀疑模型的常数与结构：

- 五点对不上 → 说明实测被重测过、或常数口径变了，先更新 `MEASURED` 与三个常数；
- 0.5/0.35 档没降到 `max(8.0, …)` → 说明「延迟串行」解释不成立，即 G0 的 `busy` 已被占满，
  应当去改 `docs/multislot.md` §2 的结论标记，而不是把模型调成"能对上"；
- `ring_lag0` 实测变快了 → 那是意外发现（§6.1 A2 写着"若实测显著变好，先记录、不要急着庆祝"），
  不要在这里把 `ring_lag0` 改成比 `single` 快——那样这个文件就再也不能证伪任何东西了。

## 实现的三种调度

| 调度 | 对应 | 一句话 |
|---|---|---|
| `single` | 上游 `FSR3Upscaler`（单槽） | 岛嵌在 D3D11 队列里，帧时间 = 捕获 + 槽位 + 网络（**求和**） |
| `ring_lag0` | 补丁 0001（多槽，`OUTPUT_LAG=0`） | 同上，只是去掉了 `:497` 的 CPU 阻塞；**输出语义不变故性能不变** |
| `ring_lag1` | 补丁 0002（`OUTPUT_LAG=1`） | 岛与本帧的 D3D11 段并行，帧时间 = **取大**；输出晚一帧 |

`ring_lag0 == single` 是本设计最容易被误读的一条，也是**可证伪的设计断言**：
`docs/multislot.md` §6.1 A2 写死了"平衡点差 ≤ ±1 fps，掉 ≥3 fps 才算失败"。
如果哪天有人把阶段 1 当成收益项，这张表就是他应该先看的东西。
"""

import sys

# --------------------------------------------------------------------------- #
# 模型常数（单位：ms）。三个数来自 docs/findings.md 的实测反推，不是凭空取的。
# --------------------------------------------------------------------------- #
CAPTURE_PRESENT = 6.7    # B1：纯捕获/呈现（无 SR/FG/NR）的固定底
SLOT = 1.4               # B1 - B2：整套 FSR4 槽位（两次 4K 拷贝 + Flush + 双向围栏 + 放大）
NET_1X = 13.7            # s=1.0 档的网络耗时；s 档按 scale^2 缩放

# 模型残差口径：与文档一致（五点残差 ≤ 0.4 ms）。
TOL_MS = 0.4
TOL_FPS = 0.5

# 五点实测：docs/findings.md「2026-11-02 补2：A/B 天花板实测闭环」（9070 XT，4K 窗口，无 FG）。
# fps / ms 取文档给的值；区间型的点取中值（B2 是 123–125 fps / 8.0–8.1 ms）。
MEASURED = [
    # (标签, 实测 fps, 实测 ms, 档位 or None)
    ("B1 纯捕获/呈现",      150.0,  6.67, None),
    ("B2 只留 FSR4_SR",     124.0,  8.05, None),
    ("A NR 全开 s=1.0",      47.0, 21.50, 1.0),
    ("A NR 全开 s=0.5",      90.0, 11.30, 0.5),
    ("A NR 全开 s=0.35",    105.0,  9.50, 0.35),
]

SCALES = (1.0, 0.5, 0.35)


def fps_of(frame_ms):
    """帧时间（ms）→ 帧率。"""
    return 1000.0 / frame_ms


def simulate(policy, scale, frames=1200, warmup=200):
    """离散事件仿真一个策略的稳态帧时间（ms）。

    两条时间线：
      g11 = D3D11 立即上下文（FIFO，单资源）：捕获/呈现、两次拷贝、Flush
      g12 = 岛（D3D12 队列，单资源）：放大 + 网络

    `single` / `ring_lag0`：岛在 D3D11 队列里，copyOut 必须等岛完成，
    下一帧的捕获也只能在发布之后开始 → 两段**严格相加**。
    `ring_lag1`：岛与本帧 D3D11 段并行，发布只需要"本帧 D3D11 段完成 + 上一帧的岛完成"
    （输出晚一帧）→ 两条时间线各自跑，取**较大**的那条。

    返回 (稳态帧时间 ms, 岛时长 ms)。
    """
    net = NET_1X * scale * scale
    island = SLOT + net          # 保守口径：把 1.4 整笔再算进岛（见 print_lag1_discussion）

    g11 = 0.0                    # D3D11 时间线空闲时刻
    g12 = 0.0                    # 岛（D3D12）时间线空闲时刻
    publish = 0.0                # 上一帧的发布时刻（主机链逐帧串行）
    prev_island_end = 0.0        # 上一帧的岛完成时刻（lag=1 的收割点）
    marks = []

    total = warmup + frames
    for _ in range(total):
        if policy in ("single", "ring_lag0"):
            # 捕获/呈现 → 拷入 → [岛] → 拷回。串行点 2（GPU 侧等待）把岛焊在队列里。
            start11 = max(g11, publish)
            mid11 = start11 + CAPTURE_PRESENT
            start12 = max(g12, mid11)            # 拷入完成才允许岛开跑
            g12 = start12 + net + SLOT           # 岛 = 放大 + 网络（串行，1.4 在这条路径里只算一次）
            g11 = max(mid11, g12)                # 拷回要等岛完成，下一帧的捕获也越过不过去
            publish = g11
        else:  # ring_lag1
            # A 段（收割上一帧）+ 本帧拷入：整段都在 D3D11 时间线上。
            start11 = max(g11, publish)
            g11 = start11 + CAPTURE_PRESENT + SLOT
            start12 = max(g12, start11 + CAPTURE_PRESENT)   # 拷入之后提交岛
            # 保守口径：岛的时长再算一遍 SLOT（1.4 里的放大那部分本来就在岛内，
            # 这里把整笔都算进去，得到 §0.3 的 66 fps 一侧）。
            g12 = start12 + island
            publish = max(g11, prev_island_end)  # lag=1：发布用的是上一帧的岛
            prev_island_end = g12

        if _ >= warmup:
            marks.append(publish)

    span = marks[-1] - marks[0]
    frame_ms = span / (len(marks) - 1)
    return frame_ms, island


def summary_single(scale):
    """`single` 的解析解：捕获 + 槽位 + 网络（串行求和）。"""
    return CAPTURE_PRESENT + SLOT + NET_1X * scale * scale


def summary_lag1(scale):
    """`ring_lag1` 的解析解：max(捕获+槽位, 槽位+网络)。"""
    net = NET_1X * scale * scale
    return max(CAPTURE_PRESENT + SLOT, SLOT + net)


def check_calibration():
    """用五点实测校验 `single` 模型。超差就打印哪一点并返回 False。"""
    rows = []
    bad = []
    for label, measured_fps, measured_ms, scale in MEASURED:
        if scale is None:
            # B1 / B2 是"结构下限"，不是 NR 档：B1 = 只有捕获/呈现，B2 = 再加槽位。
            model_ms = CAPTURE_PRESENT if "B1" in label else CAPTURE_PRESENT + SLOT
        else:
            model_ms = simulate("single", scale)[0]
        model_fps = fps_of(model_ms)
        dfps = model_fps - measured_fps
        dms = model_ms - measured_ms
        ok = abs(dms) <= TOL_MS or abs(dfps) <= TOL_FPS
        rows.append((label, measured_fps, measured_ms, model_fps, model_ms, dfps, dms, ok))
        if not ok:
            bad.append(rows[-1])

    print("=" * 96)
    print("一、校准：single（上游单槽，求和）对五点实测")
    print("    出处 docs/findings.md「2026-11-02 补2：A/B 天花板实测闭环」")
    print("    B1/B2 是结构下限（fps 高、帧时间短），A 组是 NR 全开的三档 scale")
    print("    " + "-" * 90)
    print("    {:<18} {:>10} {:>10} {:>10} {:>10} {:>9} {:>9}".format(
        "测项", "实测fps", "实测ms", "模型fps", "模型ms", "Δfps", "Δms"))
    for label, mfps, mms, pfps, pms, dfps, dms, ok in rows:
        flag = "OK" if ok else "超差"
        print("    {:<18} {:>10.1f} {:>10.2f} {:>10.1f} {:>10.2f} {:>+9.2f} {:>+9.2f}  {}".format(
            label, mfps, mms, pfps, pms, dfps, dms, flag))
    print("    " + "-" * 90)
    print("    判定标准：|Δms| ≤ {:.1f} 或 |Δfps| ≤ {:.1f}（与文档残差同口径）".format(TOL_MS, TOL_FPS))

    if bad:
        print()
        print("    校准失败，以下测项超差：")
        for label, mfps, mms, pfps, pms, dfps, dms, _ in bad:
            print("      - {:<18} 实测 {:.1f} fps / {:.2f} ms，模型 {:.1f} fps / {:.2f} ms"
                  "（Δfps={:+.2f}, Δms={:+.2f}）".format(label, mfps, mms, pfps, pms, dfps, dms))
        print("    先怀疑模型的三个常数被重测过（见文件头「模型被证伪时改哪里」），不要反过来改文档结论。")
        return False
    print("    五点全部在残差内 → single 模型与实测同口径。")
    return True


def check_lag0_equals_single():
    """可证伪的设计断言：阶段 1 性能应当不变。"""
    print()
    print("=" * 96)
    print("二、设计断言（可证伪）：ring_lag0 与 single 相同")
    print("    multislot.md §1.3：主机每发布一帧都要在 Renderer.cpp:4614-4624 把 D3D11 队列")
    print("    排空一次，`:497` 的 CPU 阻塞稳态下本来就空转 → 多槽只能让空转更空转。")
    print("    验收口径见 §6.1 A2：差 ≤ ±1 fps，掉 ≥3 fps 才算失败。")
    print("    " + "-" * 90)
    print("    {:<8} {:>12} {:>12} {:>10} {:>10}".format("scale", "single ms", "lag0 ms", "Δms", "Δfps"))
    ok = True
    for scale in SCALES:
        single_ms, _ = simulate("single", scale)
        lag0_ms, _ = simulate("ring_lag0", scale)
        # 仿真必须与解析解一致，否则说明事件循环里的依赖关系写错了。
        if abs(single_ms - summary_single(scale)) > 1e-6:
            print("    scale={:.2f}: 仿真 {:.4f} ms 与解析 {:.4f} ms 不一致 → simulate() 有 bug".format(
                scale, single_ms, summary_single(scale)))
            ok = False
        dms = lag0_ms - single_ms
        dfps = fps_of(lag0_ms) - fps_of(single_ms)
        same = abs(dms) < 1e-9 and abs(dfps) < 1e-9
        ok = ok and same
        print("    {:<8.2f} {:>12.4f} {:>12.4f} {:>+10.4f} {:>+10.4f}  {}".format(
            scale, single_ms, lag0_ms, dms, dfps, "相同" if same else "不同"))
    print("    " + "-" * 90)
    if ok:
        print("    结论：与 single 完全相同（阶段 1 的预测收益 = 0，不是 0.1–0.5 ms）。")
    else:
        print("    结论：模型里 lag0 与 single 不等价 → 模型结构改坏了，请检查 simulate()。")
    return ok


def check_lag1_formula():
    """ring_lag1 必须等于 max(捕获+槽位, 槽位+网络)。"""
    print()
    print("=" * 96)
    print("三、ring_lag1（阶段 2，取大）")
    print("    预测式：T = max(6.7 + 1.4, 1.4 + 13.7·s²) = max(8.1, 1.4 + 13.7·s²)")
    print("    依据 multislot.md §3.4/§3.5：收割挪到 Draw 开头，岛不再落在本帧的 D3D11 队列里。")
    print("    前提：G0 通过（GPU 未占满）。G0 未过时这张表整列作废。")
    print("    " + "-" * 90)
    print("    {:<8} {:>10} {:>10} {:>10} {:>11} {:>9}".format(
        "scale", "模型ms", "模型fps", "解析ms", "岛/帧", "harvestMiss"))
    ok = True
    rows = {}
    for scale in SCALES:
        frame_ms, island_ms = simulate("ring_lag1", scale)
        closed = summary_lag1(scale)
        match = abs(frame_ms - closed) < 1e-6
        ok = ok and match
        ratio = island_ms / frame_ms
        # harvestMiss 的期望：岛比帧间隔长 → 收割时必然 miss；HARVEST_SLACK=1 帧。
        miss = "≈100%" if island_ms > frame_ms else "≈0%"
        rows[scale] = (frame_ms, island_ms, ratio)
        print("    {:<8.2f} {:>10.3f} {:>10.1f} {:>10.3f} {:>11.2f} {:>9}".format(
            scale, frame_ms, fps_of(frame_ms), closed, ratio, miss))
    print("    " + "-" * 90)
    print("    岛/帧 > 1 表示岛比帧间隔长：此时收割一定 miss（§6.2 B1 记失败），晚一帧的余量不够。")
    print("    harvestMiss 期望值只由「岛是否比帧间隔长」决定，是上限估计——真正的比例要看")
    print("    magpie.log 里 `fsr3-slot: ... harvestMiss=k/120`，本模型不预测抖动。")
    if not ok:
        print("    仿真结果与解析式不一致 → simulate() 的依赖关系改坏了，请检查。")
    return ok, rows


def print_lag1_discussion(rows):
    """66 与 roadmap 的「约 70」差在哪：1.4 ms 里落在岛内的那部分。"""
    frame_ms_10 = rows[1.0][0]
    print()
    print("=" * 96)
    print("四、为什么我这里写 66、roadmap 写「约 70」")
    print("    `1.4 ms` 是 B1−B2 的差值，是**整套 FSR4 槽位**的账，并不全落在岛内：")
    print("    两次 4K 拷贝在 D3D11 侧（:498 / :579），只有放大本身在岛内（:530-573 在 queue12）。")
    print("    " + "-" * 90)
    print("    {:<10} {:>12} {:>12} {:>12}".format("岛内份额δ", "D3D11 ms", "岛 ms", "结果"))
    for delta in (0.0, 0.4, 0.6, 0.8, 1.4):
        lane11 = CAPTURE_PRESENT + SLOT - delta
        isl = delta + NET_1X
        t = max(lane11, isl)
        note = ""
        if abs(delta - 0.6) < 1e-9:
            note = "← roadmap 的「约 70」"
        elif abs(delta - 1.4) < 1e-9:
            note = "← 本模型的保守口径（1.4 全算进岛）"
        print("    {:<10.1f} {:>12.2f} {:>12.2f} {:>7.1f} fps / {:>5.2f} ms  {}".format(
            delta, lane11, isl, fps_of(t), t, note))
    print("    " + "-" * 90)
    print("    本模型取 δ=1.4（最保守），s=1.0 得 {:.1f} fps / {:.2f} ms；δ=0.6 得 {:.1f} fps / {:.2f} ms。".format(
        fps_of(frame_ms_10), frame_ms_10,
        fps_of(max(CAPTURE_PRESENT + SLOT - 0.6, 0.6 + NET_1X)),
        max(CAPTURE_PRESENT + SLOT - 0.6, 0.6 + NET_1X)))
    print("    两者差 {:.2f} ms，正好是 1.4 ms 里落在岛内的那部分（δ=0.6 与 δ=1.4）的不确定量。".format(
        frame_ms_10 - max(CAPTURE_PRESENT + SLOT - 0.6, 0.6 + NET_1X)))
    print("    所以诚实的说法是 66–70 fps，验收取 66 为通过线（§0.4，保守），>70 记超预期。")
    print("    0.5 / 0.35 档不受这个不确定性影响（max 由 8.1 的 D3D11 侧决定），")
    print("    这也是把 0.5 档当主验证点的另一个理由。")


def print_falsification():
    print()
    print("=" * 96)
    print("五、这张表会被什么证伪（去测，不要在这里调参）")
    print("    G0 判别实验：docs/multislot.md §2。在两档各跑 60 s，取稳态 30 s 窗口，算")
    print("      ratio = GPU 忙时间/帧时间（所有 3D/Compute 引擎求和，可能 >1）")
    print("      ratio > 0.85 → 求和是吞吐的必然，max 天花板不存在 → 停，不做阶段 2")
    print("      0.60 ≤ ratio ≤ 0.85 → 台阶打 (1−ratio) 折扣，验收线按实测下调")
    print("      ratio < 0.60 → 台阶成立，按 123–125 / 66–70 验收")
    print("    ratio 只能靠真机的 GPU 忙时间量，本模型算不出来。")


def main():
    print("FSR3Upscaler 调度纸面模型（离散事件仿真）")
    print("常数：捕获/呈现 {:.1f} ms + FSR4 槽位 {:.1f} ms + 网络 {:.1f}·s² ms".format(
        CAPTURE_PRESENT, SLOT, NET_1X))
    print("注意：这是纸面模型，不是实测；GPU 是否已被占满由 G0 判别实验决定。")

    if not check_calibration():
        sys.exit(1)
    if not check_lag0_equals_single():
        sys.exit(1)
    ok, rows = check_lag1_formula()
    if not ok:
        sys.exit(1)
    print_lag1_discussion(rows)
    print_falsification()

    print()
    print("=" * 96)
    print("三段调度一览（帧时间 ms / fps）")
    print("    {:<8} {:>18} {:>18} {:>18}".format("scale", "single", "ring_lag0", "ring_lag1"))
    for scale in SCALES:
        s_ms, _ = simulate("single", scale)
        l0_ms, _ = simulate("ring_lag0", scale)
        l1_ms, _ = simulate("ring_lag1", scale)
        cells = ["{:9.2f} / {:5.1f}".format(x, fps_of(x)) for x in (s_ms, l0_ms, l1_ms)]
        print("    {:<8.2f} {:>18} {:>18} {:>18}".format(scale, cells[0], cells[1], cells[2]))
    print("    " + "-" * 90)
    print("    读法：ring_lag0 一列必须与 single 完全一致（阶段 1 收益 = 0）；")
    print("          ring_lag1 一列只在 G0 通过后才是预测，否则是上限。")
    print("=" * 96)
    print("模型自洽，退出码 0。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
