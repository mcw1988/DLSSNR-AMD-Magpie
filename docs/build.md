# 构建

## 需要什么

| | 版本（本仓库发布时的实测值） |
|---|---|
| VS 2022 Build Tools | 17.14.37806.7，含 `Microsoft.VisualStudio.Workload.VCTools` |
| MSVC 工具集 | 14.44.35207（`cl.exe` 在 `VC\Tools\MSVC\<ver>\bin\Hostx64\x64`） |
| Windows SDK | 10.0.26100.0（10.0.22621 亦可） |
| LLVM（可选） | 23.1.4，只在想用 `clang-cl`/`lld-link` 时用 |

一次性安装（管理员 PowerShell）：

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools -e `
  --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --includeRecommended"
winget install --id LLVM.LLVM -e
```

## 一键构建

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-all.ps1
```

产物落在 `dist\`：

| 产物 | 来源 | 备注 |
|---|---|---|
| `overlay-input.addon64` | `src/overlay-input/overlay_input.cpp` | 需 ReShade 头文件（`scripts/fetch-upstream.ps1` 会拉） |
| `nr-router.exe` | `src/nr-router/nr_router.cpp` | 纯 Win32，无第三方依赖 |
| `nrtest.exe` | `src/nrtest/main.cpp` | 需 `src/nrtest/LmxxfNrApi.h` |
| `amd-nr.addon64` | 上游 OpenNR 源码 | 由 `scripts/build-upstream-addon.ps1` 驱动上游自带的 `build.ps1 -Target neural` |

所有目标都用 **`/MT` 静态 CRT**：游戏目录里常躺着旧版 `msvcp140.dll`，`/MD` 会让私有副本覆盖
运行库，`DllMain` 可能在 ReShade 注册之后失败。

## 手工编译（三个自有组件）

`cl.exe` 需要 `INCLUDE` / `LIB` 指向 MSVC + SDK，脚本里就是这么设的，等价命令：

```powershell
# overlay-input：ReShade add-on（/LD 出 DLL，扩展名换成 .addon64）
cl /nologo /utf-8 /std:c++20 /EHsc /O2 /MT /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN /LD `
   overlay_input.cpp /link /DLL /OUT:overlay-input.addon64 user32.lib

# nr-router：控制台程序
cl /nologo /utf-8 /std:c++20 /EHsc /O2 /MT /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN `
   nr_router.cpp /link /SUBSYSTEM:CONSOLE /OUT:nr-router.exe user32.lib

# nrtest：控制台程序，直连 D3D12
cl /nologo /utf-8 /std:c++20 /EHsc /O2 /MT /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN `
   main.cpp /link /OUT:nrtest.exe d3d12.lib dxgi.lib user32.lib
```

`overlay_input.cpp` 用 `#include <reshade.hpp>`，需要把 ReShade 的 `include/` 目录加进
`INCLUDE`（`reshade.hpp`、`reshade_api*.hpp`、`reshade_events.hpp` 等），运行时通过
`GetProcAddress` 解析导出的 `ReShade*` 函数，所以**不需要**链接任何 ReShade 的 lib。

## 校验产物

```powershell
dumpbin /exports dist\overlay-input.addon64   # 期望只有 NAME / DESCRIPTION（自注册 add-on）
dumpbin /dependents dist\overlay-input.addon64 # 期望只有系统 DLL，无 msvcp140/vcruntime
```

`amd-nr.addon64` 同理：导出 `NAME`、`DESCRIPTION`，依赖只有 `USER32/bcrypt/SHELL32/ole32/KERNEL32`。

## 关于 `dist/` 里的二进制

`dist/` 中的文件由本仓库源码（以及 MIT 许可的上游 OpenNR 源码）构建，可以直接用；
`amd-nr.addon64` 的再分发条件见 [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md)。
