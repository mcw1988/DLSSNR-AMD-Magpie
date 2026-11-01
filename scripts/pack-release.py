#!/usr/bin/env python3
"""把实机目录打成一个可发布的核心包（不含 lmxxf 权重）。

用法：
    python scripts/pack-release.py --src "D:/Magpie-NR-045" --out dist/Magpie-NR-045-v0.4-core.zip

取舍与 v0.3 一致，只多两处：
  · 不含 DLSS5-AMD 的权重（约 835 MB），lmxxf 需要另下权重分卷；
  · 不含 NVIDIA 的 NGX/NPP/NVVFX 一批 DLL（约 1 GB），v0.3 起就没带；
  · 只保留 mochinr 这条挂载路线：不再打包 present 层的 amd-nr.addon64 与其 ini。

排除的都是运行期产物与历代调试残留（日志、.bak/.orig/.prev、探针与 shim），
以及开发用的 sources/、_dev-src/（v0.3 的包里带过它，属于历史遗留）。
"""
import argparse
import hashlib
import os
import pathlib
import sys
import zipfile

# 整目录带走
DIRS = ["dlssnr-amd", "effects", "shaders", "NVIDIA-VFX-Licenses"]

# config 只带活文件：备份与 recovery 是 Magpie 自己写的
CONFIG_FILES = ["config/config.json", "config/v4e/config.json"]

# 根目录：按扩展名收，再按前缀剔
ROOT_EXT = {".md", ".txt", ".json", ".pri", ".exe", ".ini", ".cmd", ".dll", ".pdf",
            ".addon64", ".off"}   # add-on 本体与"停用"态都要能进包

# 出现即排除（前缀匹配）
DENY_PREFIX = (
    "amd-nr.addon64", "amd-nr.ini",          # present 层的 mochizuki，v0.4 起退役
    "amd-nr.log",
    "ffxshim", "ffxhook", "ffx-shim-probe", "xess-shim", "dlssnr-probe",
    "mochinr.log", "ReShade.log", "启动Magpie-探针模式",
    "nvngx_", "npp", "nvVFX", "NVCVImage", "cudart64",   # NVIDIA 的大件
    "libxess.orig",
)

# 出现即排除（后缀/包含匹配）：历代备份与实验产物
DENY_SUBSTR = (".bak", ".orig", ".prev", ".orig-shipped", "~")

# Magpie 自带的校验清单：列的是它原始发行版的文件，我们删过东西后它就是错的
DENY_EXACT = {"SHA256SUMS.txt"}


def keep_root(name: str) -> bool:
    if os.path.splitext(name)[1].lower() not in ROOT_EXT:
        return False
    if name in DENY_EXACT:
        return False
    if name.startswith(DENY_PREFIX):
        return False
    if any(s in name for s in DENY_SUBSTR):
        return False
    return True


def add_file(zf: zipfile.ZipFile, src: pathlib.Path, rel: str, stat):
    zf.write(src, rel)
    return stat


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--root-name", default="Magpie-NR-045")
    args = ap.parse_args()

    src = pathlib.Path(args.src)
    out = pathlib.Path(args.out)
    if not src.is_dir():
        print(f"源目录不存在: {src}", file=sys.stderr)
        return 2
    out.parent.mkdir(parents=True, exist_ok=True)

    roots = sorted(p for p in src.iterdir() if p.is_file() and keep_root(p.name))
    missing_cfg = [f for f in CONFIG_FILES if not (src / f).is_file()]
    if missing_cfg:
        print(f"缺配置文件: {missing_cfg}", file=sys.stderr)
        return 2

    total = 0
    count = 0
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for p in roots:
            zf.write(p, f"{args.root_name}/{p.name}")
            total += p.stat().st_size
            count += 1
        for rel in CONFIG_FILES:
            zf.write(src / rel, f"{args.root_name}/{rel}")
            total += (src / rel).stat().st_size
            count += 1
        for d in DIRS:
            base = src / d
            if not base.is_dir():
                print(f"跳过不存在的目录: {d}")
                continue
            for p in sorted(base.rglob("*")):
                if p.is_file():
                    rel = p.relative_to(src).as_posix()
                    zf.write(p, f"{args.root_name}/{rel}")
                    total += p.stat().st_size
                    count += 1

    size = out.stat().st_size
    digest = hashlib.sha256()
    with open(out, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)

    print(f"条目 {count} 个，未压缩 {total / 2**20:.1f} MB -> {size / 2**20:.1f} MB")
    print(f"sha256 {digest.hexdigest()}  {out.name}")
    print("\n根目录文件:")
    for p in roots:
        print(f"  {p.stat().st_size:>10}  {p.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
