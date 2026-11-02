# 第三方组件与许可

本仓库自身的代码（`src/`、`scripts/`、`docs/`）为 **MIT**，见 [LICENSE](LICENSE)。

下表中「仓库」指本 git 仓库，「发布包」指 GitHub Releases 里的 `Magpie-NR-045-v0.4-core.zip`。
两者范围不同：仓库只放本仓库源码与自建产物，发布包才含宿主程序。

| 组件 | 来源 | 许可 | 仓库 | 发布包 |
|---|---|---|---|---|
| Magpie Experimental v0.6.11 | <https://github.com/SAOG0721/Magpie>（`experimental`） | **GPL-3.0** | 否 | **是**（`Magpie.exe` 及其 DLL） |
| ReShade 6.8 | <https://github.com/crosire/reshade> | BSD-3-Clause | 否 | 是（`dxgi.dll`） |
| ReShade 头文件 `reshade.hpp` 等 | 同上 | BSD-3-Clause | 否（构建时引用） | — |
| OpenNR | <https://github.com/zmodelerlover/OpenNR-ReShade> | MIT | 仅 `dist/amd-nr.addon64`（构建产物 + 本仓库补丁） | 是 |
| DLSSNR-AMD / `MochizukiNrRuntime.dll` | <https://github.com/mochizuki0323/DLSSNR-AMD> | MIT | 否 | 是（运行时 + `dlssnr-amd\`） |
| lmxxf `dlss5-on-amd-9070xt-porting` | <https://github.com/lmxxf/dlss5-on-amd-9070xt-porting> | MIT | 否 | 否（594 MB 权重另行分发） |
| RenoDX（面板风格来源） | <https://github.com/clshortfuse/renodx> | MIT | 否 | 否 |
| MinHook | <https://github.com/TsudaKageyu/minhook> | MIT | 否（构建 `mochinr`/`amd-nr` 时引用） | — |
| Intel XeSS / `libxess*.dll` | Intel | Intel 自有许可（见 `INTEL-XESS-LICENSE.txt`） | 否 | 是 |
| AMD FidelityFX / `amd_fidelityfx_*.dll` | AMD | MIT（FidelityFX SDK） | 否 | 是 |
| NVIDIA DLSS 模型权重 | NVIDIA | NVIDIA 许可 | 否 | 否（由使用者自行提取） |

## 关于 Magpie 的 GPL-3.0

发布包内含 **Magpie Experimental** 的二进制，它是 **GPL-3.0** 程序。按 GPL-3 第 6 条，
分发二进制时必须同时提供「对应源码」的可获取途径：

- 源码：<https://github.com/SAOG0721/Magpie>，分支 `experimental`，对应 v0.6.11 的提交；
- 许可全文：[THIRD-PARTY-LICENSES/GPL-3.0.txt](THIRD-PARTY-LICENSES/GPL-3.0.txt)。

本仓库自身代码是 MIT，且**不与 Magpie 链接**（运行时通过 ReShade add-on API 加载、按文件
摆放），因此不与 GPL 部分构成衍生作品；两者的关系是同一发行包中的**聚合分发**。
若你要二次分发发布包，请一并保留 Magpie 的源码地址与 GPL 全文。

## 分发 `dist/` 与发布包时请注意

- `dist/amd-nr.addon64` 由 MIT 许可的 OpenNR 源码 + 本仓库补丁构建，可再分发，但须保留其
  版权与许可声明（见下）。
- `dist/overlay-input.addon64`、`dist/nr-router.exe`、`dist/nrtest.exe`、`dist/nrbench.exe`、
  `dist/mochinr.addon64` 是本仓库代码，随本仓库 MIT 许可分发。
- 模型权重（`dlssnr-amd\`、`DLSS5-AMD\`）与 Intel/AMD 的 DLL 的再分发条款由各自权利人决定。

## 上游版权声明

### Magpie Experimental（GPL-3.0）

```
Copyright (c) SAOG0721 及 Magpie 原作者 Blinue 等贡献者
License: GNU General Public License v3.0
全文: THIRD-PARTY-LICENSES/GPL-3.0.txt
源码: https://github.com/SAOG0721/Magpie
```

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

（条款同 OpenNR 一节；全文见上游仓库 LICENSE / HF 数据集根目录）
```

### lmxxf — dlss5-on-amd-9070xt-porting（MIT）

```
MIT License — Copyright (c) 2026 Kien
全文见 https://github.com/lmxxf/dlss5-on-amd-9070xt-porting
```

### RenoDX（MIT）

```
MIT License — Copyright (c) RenoDX contributors
全文见 https://github.com/clshortfuse/renodx
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

### MinHook（MIT）

```
Copyright (c) 2009-2017 Tsuda Kageyu — 全文见上游仓库。
```
