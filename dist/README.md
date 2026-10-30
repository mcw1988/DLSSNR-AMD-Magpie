# dist —— 预编译产物

这些文件由本仓库源码构建，可以直接使用；也可以在目标机上用 `scripts/build-all.ps1` 重新生成。

| 文件 | 来源 | 构建命令 |
|---|---|---|
| `overlay-input.addon64` | `src/overlay-input/overlay_input.cpp` | `cl /O2 /MT /LD ... /link /DLL`，依赖 ReShade 头文件 |
| `nr-router.exe` | `src/nr-router/nr_router.cpp` | `cl /O2 /MT ... /link /SUBSYSTEM:CONSOLE` |
| `nrtest.exe` | `src/nrtest/main.cpp` | `cl /O2 /MT ... /link` + `d3d12.lib dxgi.lib` |
| `amd-nr.addon64` | **上游** OpenNR 源码（`zmodelerlover/dlss5-neural-amd`，MIT） | 该工程的 `build.ps1 -Target neural` |

构建环境：MSVC 14.44.35207（VS 2022 Build Tools 17.14）+ Windows SDK 10.0.26100，`/MT` 静态 CRT。
三个自有产物只依赖系统 DLL（`USER32` / `bcrypt` / `SHELL32` / `ole32` / `KERNEL32`），
导出只有自注册 add-on 需要的 `NAME`、`DESCRIPTION`。

## 校验值

```
79d58f27ca0629c45198206ef6bdba29158a1a6045d7f7d7a075200dcb8f917a  nr-router.exe
fb0315565d9cbfcdd85376efd6a57f8b438d6bafb5c9d06540889ae7127543ad  nrtest.exe
c7f17e370af1ebfaf6c6e71efc816958d3ca04ea1dd8b910b9428fb1b84f3960  overlay-input.addon64
c9cef21554721bccd3434aedda0e591b24b7fc73253d4196e401e8b2416b0571  amd-nr.addon64
```

## 许可

- `overlay-input.addon64`、`nr-router.exe`、`nrtest.exe`：本仓库 MIT。
- `amd-nr.addon64`：上游 OpenNR（MIT）的构建产物，再分发时须保留其版权与许可声明，
  见 [../THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md)。

`dist/` 里**没有** ReShade、Magpie、Intel/AMD 的 DLL，也没有模型权重 —— 那些需按
[../docs/upstream.md](../docs/upstream.md) 自行获取。
