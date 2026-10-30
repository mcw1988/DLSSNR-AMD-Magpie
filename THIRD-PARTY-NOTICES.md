# 第三方组件与许可

本仓库自身的代码（`src/`、`scripts/`、`docs/`）为 **MIT**，见 [LICENSE](LICENSE)。

下表列出本项目**使用**的上游组件。除特别标注外，本仓库**不随附**它们的二进制或权重，
使用者需按 [docs/upstream.md](docs/upstream.md) 自行获取。

| 组件 | 来源 | 许可 | 本仓库是否分发 |
|---|---|---|---|
| ReShade 6.8 | <https://github.com/crosire/reshade> | BSD-3-Clause | 否（运行时以 `dxgi.dll` 注入，需“with full add-on support”构建） |
| ReShade 头文件 `reshade.hpp` 等 | 同上 | BSD-3-Clause | 否（构建 `overlay-input` 时引用） |
| Magpie | <https://github.com/Blinue/Magpie> | MIT | 否（宿主程序） |
| OpenNR `dlss5-neural-amd` | <https://github.com/zmodelerlover/dlss5-neural-amd> | MIT | 是（`dist/amd-nr.addon64` 是该源码的**构建产物**，附本文件与版权声明） |
| DLSSNR-AMD / `MochizukiNrRuntime.dll` | <https://github.com/mochizuki0323/DLSSNR-AMD> | MIT | 否（运行时 + `dlssnr-amd\` 权重，约 286 MB） |
| lmxxf `dlss5-on-amd-9070xt-porting` | <https://github.com/lmxxf/dlss5-on-amd-9070xt-porting> | MIT | 否（`dlss5-amd.addon64` 与 594 MB 权重） |
| MinHook | <https://github.com/TsudaKageyu/minhook> | MIT | 否（构建 `amd-nr.addon64` 时由上游源码携带） |
| Intel XeSS / `libxess*.dll` | Intel | Intel 自有许可（见上游 `INTEL-XESS-LICENSE.txt`） | 否 |
| AMD FidelityFX / `amd_fidelityfx_*.dll` | AMD | MIT（FidelityFX SDK） | 否 |
| NVIDIA DLSS 模型权重 | NVIDIA | NVIDIA 许可 | 否（由使用时自行提取） |

## 分发 `dist/` 中的产物时请注意

`dist/amd-nr.addon64` 由 MIT 许可的 OpenNR 源码构建，可再分发，但必须保留其版权与许可声明
（见下）。`dist/overlay-input.addon64` 与 `dist/nr-router.exe`、`dist/nrtest.exe` 为本仓库代码，
随本仓库的 MIT 许可分发。

请**不要**把 ReShade、Magpie、Intel/AMD 的 DLL 以及任何模型权重提交到本仓库或发布页——
这些文件的再分发条款由各自权利人决定。

## 上游版权声明

### OpenNR — AMD Neural Rendering add-on（MIT）

```
MIT License

Copyright (c) 2026 zmodelerlover

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### DLSSNR-AMD — mochizuki 运行时（MIT）

```
MIT License

Copyright (c) 2026 mochizuki0323

（条款同上，全文见 https://github.com/mochizuki0323/DLSSNR-AMD 的 LICENSE）
```

### lmxxf — dlss5-on-amd-9070xt-porting（MIT）

```
MIT License — Copyright (c) 2026 Kien
全文见 https://github.com/lmxxf/dlss5-on-amd-9070xt-porting
```

### ReShade（BSD-3-Clause）

```
Copyright (C) 2014 Patrick Mours

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.
```

### Magpie（MIT）

Copyright (c) Blinue — 全文见 <https://github.com/Blinue/Magpie/blob/main/LICENSE>。

### MinHook（MIT）

Copyright (c) 2009-2017 Tsuda Kageyu — 全文见上游仓库。
