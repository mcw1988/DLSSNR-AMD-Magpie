# dist —— 预编译产物

这些文件由本仓库源码构建，可以直接使用；也可以在目标机上用 `scripts/build-all.ps1` 重新生成。

| 文件 | 来源 | 构建命令 |
|---|---|---|
| `overlay-input.addon64` | `src/overlay-input/overlay_input.cpp` | `cl /O2 /MT /LD ... /link /DLL`，依赖 ReShade 头文件 |
| `nr-router.exe` | `src/nr-router/nr_router.cpp` | `cl /O2 /MT ... /link /SUBSYSTEM:CONSOLE` |
| `nrtest.exe` | `src/nrtest/main.cpp` | `cl /O2 /MT ... /link` + `d3d12.lib dxgi.lib` |
| `nrbench.exe` | `src/nrtest/nrbench.cpp` | 同上；跑 N 帧并统计各阶段墙钟 |
| `amd-nr.addon64` | **上游** OpenNR 源码 + 本仓库补丁（`src/opennr-patches/`，MIT） | 该工程的 `build.ps1 -Target neural` |
| `mochinr.addon64` | `src/mochinr/mochinr.cpp`（v0.4 起：NR 接进 FSR 槽位） | `src/mochinr/build.ps1`，依赖 MinHook + ReShade 6.8 头 |

构建环境：MSVC 14.44.35207（VS 2022 Build Tools 17.14）+ Windows SDK 10.0.26100，`/MT` 静态 CRT。
四个自有产物只依赖系统 DLL（`USER32` / `bcrypt` / `SHELL32` / `ole32` / `KERNEL32`），
导出只有自注册 add-on 需要的 `NAME`、`DESCRIPTION`。

## 校验值

```
fc9dcff0cd00f61f20f35c0657bb9801d257456fa545592023ce5f863a247e1b  nr-router.exe
48a857598c63c5a71c5a21a65aa28363106df31093df50e42a8c845ab83c134c  nrtest.exe
acc426d9f9992c990f9c608aa9288318b758cb3869077ffdb2ecf70789989781  nrbench.exe
e27b6dede96f95a73c439bb7705ed45b54432eed805c348574c4086108385f55  overlay-input.addon64
8643e87362bc575a2c202933b046a412e20e75c2ebd9d227c63916666c02021d  amd-nr.addon64
b48c129f1db5f152b70c6a1ab833d792deb3589e463eb28cd06f24746a615069  mochinr.addon64
```

## 许可

- `overlay-input.addon64`、`nr-router.exe`、`nrtest.exe`、`nrbench.exe`：本仓库 MIT。
- `amd-nr.addon64`：上游 OpenNR（MIT）+ 本仓库补丁的构建产物，再分发时须保留其版权与许可声明，
  见 [../THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md)。
- `mochinr.addon64`：本仓库 MIT；提交契约移植自 lmxxf/dlss5-on-amd-9070xt-porting（MIT），
  扩展参数 ABI 头取自 OpenNR（MIT），见 [../THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md)。

`dist/` 里**没有** ReShade、Magpie、Intel/AMD 的 DLL，也没有模型权重 —— 那些需按
[../docs/upstream.md](../docs/upstream.md) 自行获取。
