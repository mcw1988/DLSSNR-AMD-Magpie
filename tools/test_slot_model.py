#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""`tools/slot_model.py` 的回归测试。

    python tools/test_slot_model.py           # 独立运行
    python -m unittest discover -s tools      # 或用 unittest 发现

## 测试手段

`slot_model.py` **全程只读**，本文件不 import 它的实现细节，而是：

1. 用 `subprocess` 以脚本方式跑一遍 `slot_model.py`，解析它打印的表格——
   这是文档（`docs/multislot.md` §0.3、§6）真正引用的那几列数字；
2. 再用一次 `python -c` 探针以**全精度**取回 `simulate()` 的返回值，
   因为"逐位相等"这类断言用 4 位小数的打印值证不出来。

## 覆盖的四条断言（对应交付要求）

| # | 断言 | 对应文档 |
|---|---|---|
| 1 | 五点校准实测在 ±0.4 ms 内被复现 | `findings.md`「A/B 天花板实测闭环」、`multislot.md` §2.1 |
| 2 | `ring_lag0` 与 `single` 逐位相等（阶段 1 收益 = 0，可证伪） | `multislot.md` §1.3、§6.1 A2 |
| 3 | `ring_lag1 == max(8.1, 1.4 + 13.7·s²)` | `multislot.md` §0.3、§3.5 |
| 4 | `s=1.0` 档 `harvestMiss` ≈ 100% | `multislot.md` §6.2 B1 |

第 2 条是这套设计最容易被误读的一条：如果哪天有人把阶段 1 当收益项，
本测试会立刻失败——这正是它存在的意义。
"""

import json
import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parent
MODEL = TOOLS_DIR / "slot_model.py"

# 与 slot_model.py 的模型常数同口径（只用于独立复算解析式，不从被测文件 import）。
CAPTURE_PRESENT = 6.7
SLOT = 1.4
NET_1X = 13.7

# 模型残差口径：multislot.md 写死的 ±0.4 ms。
TOL_MS = 0.4
# 逐位比较的容差。真正的"逐位"断言用 ==，这里只给浮点复算留 1e-9。
EPS = 1e-9

SCALES = (1.0, 0.5, 0.35)

# B2 的实测是区间 123–125 fps / 8.0–8.1 ms（multislot.md §0.3），不是单点。
B2_RANGE_MS = (8.0, 8.1)

# (表格标签, 实测 ms)。B2 取区间中值 8.05，另由 B2_RANGE_MS 收紧。
CALIBRATION = (
    ("B1 纯捕获/呈现", 6.67),
    ("B2 只留 FSR4_SR", 8.05),
    ("A NR 全开 s=1.0", 21.50),
    ("A NR 全开 s=0.5", 11.30),
    ("A NR 全开 s=0.35", 9.50),
)

_NUM = r"[-+]?\d+\.\d+"

# 二、lag0 表格： scale single_ms lag0_ms Δms Δfps 相同/不同
_LAG0_ROW = re.compile(
    r"^\s+(\d+\.\d{2})\s+([\d.]+)\s+([\d.]+)\s+([+-][\d.]+)\s+([+-][\d.]+)\s+(相同|不同)\s*$"
)
# 三、lag1 表格： scale 模型ms 模型fps 解析ms 岛/帧 harvestMiss
_LAG1_ROW = re.compile(
    r"^\s+(\d+\.\d{2})\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(≈100%|≈0%)\s*$"
)
# 末尾「三段调度一览」： scale  "a / b"  "c / d"  "e / f"
_SUMMARY_CELL = re.compile(r"(\d+\.\d+) / *(\d+\.\d+)")


def _child_env():
    """强制子进程用 UTF-8 输出，避免 Windows 控制台代码页把表格里的 ≈ / δ 弄坏。"""
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    env["PYTHONUTF8"] = "1"
    return env


def _run(argv, what):
    proc = subprocess.run(
        argv,
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=_child_env(),
        timeout=300,
    )
    if proc.returncode != 0:
        raise AssertionError(
            "{} 退出码 {}（期望 0）\n--- stdout ---\n{}\n--- stderr ---\n{}".format(
                what, proc.returncode, proc.stdout, proc.stderr
            )
        )
    return proc


# 探针：以全精度取回 simulate() 的返回值。断言"逐位相等"时 4 位小数的打印值不够。
_PROBE_SOURCE = """
import json
import sys

sys.path.insert(0, sys.argv[1])
import slot_model

out = {}
for scale in slot_model.SCALES:
    frame_single, _ = slot_model.simulate("single", scale)
    frame_lag0, _ = slot_model.simulate("ring_lag0", scale)
    frame_lag1, island_lag1 = slot_model.simulate("ring_lag1", scale)
    out[repr(scale)] = {
        "single": frame_single,
        "lag0": frame_lag0,
        "lag1": frame_lag1,
        "island": island_lag1,
    }
print("PROBE:" + json.dumps(out))
"""


def parse_calibration(stdout):
    """把「一、校准」表解析成 {标签: 六个数值}。标签本身就是锚点，不依赖列宽。"""
    rows = {}
    for line in stdout.splitlines():
        stripped = line.strip()
        for label, _measured in CALIBRATION:
            if not stripped.startswith(label):
                continue
            rest = stripped[len(label):]
            rows.setdefault(label, []).append([float(x) for x in re.findall(_NUM, rest)])
    return {label: occurrences[0] for label, occurrences in rows.items()}


def parse_flagged_rows(stdout, pattern):
    """按给定行正则抓表，返回 {scale: (匹配组...)}。"""
    rows = {}
    for line in stdout.splitlines():
        match = pattern.match(line)
        if match:
            rows[round(float(match.group(1)), 2)] = match.groups()
    return rows


def parse_summary(stdout):
    """「三段调度一览」：{scale: (single_ms, lag0_ms, lag1_ms)}。"""
    rows = {}
    for line in stdout.splitlines():
        stripped = line.strip()
        scale_match = re.match(r"^(\d+\.\d{2})\s", stripped)
        if not scale_match:
            continue
        cells = _SUMMARY_CELL.findall(stripped)
        if len(cells) == 3:
            rows[round(float(scale_match.group(1)), 2)] = tuple(float(ms) for ms, _fps in cells)
    return rows


def analytic_lag1(scale):
    """§0.3 的取大式：max(6.7 + 1.4, 1.4 + 13.7·s²)。"""
    return max(CAPTURE_PRESENT + SLOT, SLOT + NET_1X * scale * scale)


class SlotModelTest(unittest.TestCase):
    """先跑一遍模型（脚本 + 探针），后面每个用例只做断言。"""

    @classmethod
    def setUpClass(cls):
        assert MODEL.is_file(), "被测文件不存在: {}".format(MODEL)
        cls.script = _run([sys.executable, str(MODEL)], "slot_model.py")
        cls.calibration = parse_calibration(cls.script.stdout)
        cls.lag0_rows = parse_flagged_rows(cls.script.stdout, _LAG0_ROW)
        cls.lag1_rows = parse_flagged_rows(cls.script.stdout, _LAG1_ROW)
        cls.summary = parse_summary(cls.script.stdout)

        probe = _run(
            [sys.executable, "-c", _PROBE_SOURCE, str(TOOLS_DIR)], "slot_model.py 探针"
        )
        payload = None
        for line in probe.stdout.splitlines():
            if line.startswith("PROBE:"):
                payload = json.loads(line[len("PROBE:"):])
        assert payload is not None, "探针没有输出 PROBE 行"
        cls.probe = {round(float(scale), 2): values for scale, values in payload.items()}

    # ------------------------------------------------------------------ #
    # 0. 模型自身可跑、表格结构没被改坏
    # ------------------------------------------------------------------ #
    def test_model_runs_and_prints_self_consistent_footer(self):
        self.assertEqual(self.script.returncode, 0)
        self.assertIn("模型自洽，退出码 0。", self.script.stdout)
        self.assertNotIn("Traceback", self.script.stderr)

    def test_all_tables_parsed(self):
        expected_scales = sorted(SCALES)
        self.assertEqual(len(self.calibration), len(CALIBRATION), self.calibration)
        self.assertEqual(sorted(self.lag0_rows), expected_scales, self.lag0_rows)
        self.assertEqual(sorted(self.lag1_rows), expected_scales, self.lag1_rows)
        self.assertEqual(sorted(self.summary), expected_scales, self.summary)

    # ------------------------------------------------------------------ #
    # 1. 五点校准：模型必须在 ±0.4 ms 内复现实测
    # ------------------------------------------------------------------ #
    def test_calibration_within_tolerance(self):
        for label, measured_ms in CALIBRATION:
            with self.subTest(label=label):
                fields = self.calibration[label]
                self.assertEqual(len(fields), 6, "校准行列数不对: {}".format(fields))
                measured_fps, printed_measured_ms, model_fps, model_ms, d_fps, d_ms = fields

                # 表里回显的"实测"必须就是文档那个数，别在打印时被改掉。
                self.assertAlmostEqual(printed_measured_ms, measured_ms, places=2)
                # 模型复现实测：±0.4 ms。
                self.assertLessEqual(
                    abs(model_ms - measured_ms),
                    TOL_MS,
                    "{}: 模型 {:.2f} ms 与实测 {:.2f} ms 差 {:.2f} ms，超出 ±{:.1f} ms".format(
                        label, model_ms, measured_ms, abs(model_ms - measured_ms), TOL_MS
                    ),
                )
                # Δ 列必须与两列之差自洽（防止打印时把残差写错）。注意 Δ 列是用
                # 全精度算完再打印的，而 fps/ms 两列各自只保留 1/2 位小数，
                # 所以复算只能给到「打印位宽」级别的容差。
                self.assertAlmostEqual(d_ms, model_ms - measured_ms, delta=0.02)
                self.assertAlmostEqual(d_fps, model_fps - measured_fps, delta=0.06)
                self.assertIn("OK", self.script.stdout.split(label, 1)[1].splitlines()[0])

    def test_b2_lands_inside_measured_range(self):
        """B2 是区间 123–125 fps / 8.0–8.1 ms，模型必须落在区间内而不是"附近"。"""
        model_ms = self.calibration["B2 只留 FSR4_SR"][3]
        low, high = B2_RANGE_MS
        self.assertGreaterEqual(model_ms, low, "B2 模型 {:.3f} ms 低于区间下界 {}".format(model_ms, low))
        self.assertLessEqual(model_ms, high, "B2 模型 {:.3f} ms 高于区间上界 {}".format(model_ms, high))

    def test_single_matches_analytic_sum(self):
        """single 的仿真值必须等于解析式 6.7 + 1.4 + 13.7·s²（事件循环没写歪）。"""
        for scale in SCALES:
            with self.subTest(scale=scale):
                analytic = CAPTURE_PRESENT + SLOT + NET_1X * scale * scale
                self.assertAlmostEqual(self.probe[scale]["single"], analytic, delta=EPS)

    # ------------------------------------------------------------------ #
    # 2. 阶段 1 收益 = 0：ring_lag0 与 single 逐位相等（可证伪断言）
    # ------------------------------------------------------------------ #
    def test_ring_lag0_equals_single_bitwise(self):
        for scale in SCALES:
            with self.subTest(scale=scale):
                single_ms = self.probe[scale]["single"]
                lag0_ms = self.probe[scale]["lag0"]
                # 逐位相等，不是"近似"。
                self.assertEqual(
                    lag0_ms,
                    single_ms,
                    "scale={}: ring_lag0 {!r} != single {!r}（阶段 1 的预测收益必须是 0）".format(
                        scale, lag0_ms, single_ms
                    ),
                )

    def test_ring_lag0_table_reports_no_delta(self):
        for scale in SCALES:
            with self.subTest(scale=scale):
                _scale, single_ms, lag0_ms, d_ms, d_fps, verdict = self.lag0_rows[scale]
                self.assertAlmostEqual(float(d_ms), 0.0, delta=1e-4)
                self.assertAlmostEqual(float(d_fps), 0.0, delta=1e-4)
                self.assertEqual(verdict, "相同")

    # ------------------------------------------------------------------ #
    # 3. 阶段 2：ring_lag1 == max(8.1, 1.4 + 13.7·s²)
    # ------------------------------------------------------------------ #
    def test_ring_lag1_matches_max_formula(self):
        for scale in SCALES:
            with self.subTest(scale=scale):
                expected = analytic_lag1(scale)
                frame_ms = self.probe[scale]["lag1"]
                self.assertAlmostEqual(frame_ms, expected, delta=EPS)

    def test_ring_lag1_table_matches_formula(self):
        for scale in SCALES:
            with self.subTest(scale=scale):
                _scale, model_ms, model_fps, closed_ms, _island_ratio, _miss = self.lag1_rows[scale]
                expected = analytic_lag1(scale)
                self.assertAlmostEqual(float(model_ms), expected, delta=1e-3)
                self.assertAlmostEqual(float(closed_ms), expected, delta=1e-3)
                self.assertAlmostEqual(float(model_fps), 1000.0 / expected, delta=0.05)

    def test_ring_lag1_s1_is_the_151ms_design_point(self):
        """s=1.0 的 15.1 ms / 66 fps 是 §0.3 与 §6.2 B4 的设计口径，不能漂。"""
        frame_ms = self.probe[1.0]["lag1"]
        self.assertAlmostEqual(frame_ms, 15.1, places=6)
        # 66 是"通过线取保守"的那一侧；14.3 ms 是 roadmap「约 70」的另一侧。
        self.assertGreaterEqual(1000.0 / frame_ms, 66.0 - 0.1)
        self.assertLessEqual(1000.0 / frame_ms, 70.0)

    def test_ring_lag1_low_scales_hit_d3d11_ceiling(self):
        """0.5 / 0.35 档的 max 由 D3D11 侧的 8.1 ms 决定，两档必须相等。"""
        self.assertAlmostEqual(self.probe[0.5]["lag1"], CAPTURE_PRESENT + SLOT, delta=EPS)
        self.assertAlmostEqual(self.probe[0.35]["lag1"], CAPTURE_PRESENT + SLOT, delta=EPS)
        self.assertAlmostEqual(
            self.probe[0.5]["lag1"], self.probe[0.35]["lag1"], delta=EPS
        )

    def test_lag1_strictly_beats_single(self):
        """阶段 2 的台阶必须为正，否则"取大"模型没兑现。"""
        for scale in SCALES:
            with self.subTest(scale=scale):
                self.assertLess(self.probe[scale]["lag1"], self.probe[scale]["single"])

    # ------------------------------------------------------------------ #
    # 4. harvestMiss：s=1.0 应 ≈100%，0.5 / 0.35 应 ≈0%
    # ------------------------------------------------------------------ #
    def test_harvest_miss_at_scale_1(self):
        frame_ms = self.probe[1.0]["lag1"]
        island_ms = self.probe[1.0]["island"]
        # 岛不短于帧间隔 → 收割必然 miss。模型自己标的就是这条。
        self.assertGreater(island_ms, frame_ms)
        self.assertGreaterEqual(island_ms / frame_ms, 0.999)
        self.assertEqual(self.lag1_rows[1.0][5], "≈100%")

    def test_harvest_miss_at_low_scales(self):
        for scale in (0.5, 0.35):
            with self.subTest(scale=scale):
                self.assertLess(self.probe[scale]["island"], self.probe[scale]["lag1"])
                self.assertEqual(self.lag1_rows[scale][5], "≈0%")

    # ------------------------------------------------------------------ #
    # 5. 末尾三列汇总表与前面的表一致（防止两处口径漂移）
    # ------------------------------------------------------------------ #
    def test_summary_table_agrees_with_sections(self):
        for scale in SCALES:
            with self.subTest(scale=scale):
                single_ms, lag0_ms, lag1_ms = self.summary[scale]
                self.assertAlmostEqual(single_ms, self.probe[scale]["single"], delta=1e-2)
                self.assertAlmostEqual(lag0_ms, self.probe[scale]["lag0"], delta=1e-2)
                self.assertAlmostEqual(lag1_ms, self.probe[scale]["lag1"], delta=1e-2)
                self.assertEqual(single_ms, lag0_ms)


if __name__ == "__main__":
    unittest.main(verbosity=2)
