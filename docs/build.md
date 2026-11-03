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
| `nrtest.exe` | `src/nrtest/main.cpp` | 需 `src/nrtest/MochiNrApi.h` |
| `nrbench.exe` | `src/nrtest/nrbench.cpp` | 同上；跑 N 帧并拆开计时 |
| `mochinr.addon64` | `src/mochinr/mochinr.cpp` | v0.4 主力；需 MinHook 与 ReShade 6.8 头文件（`src/mochinr/build.ps1` 自动找 `upstream\`，也可 `-MinHookDir` / `-ReshadeIncludeDir` 指定；`-OutFile` 可直接输出到部署目录） |
| `amd-nr.addon64` | 上游 OpenNR 源码 + `src/opennr-patches/` | 由 `build-all.ps1` 调上游自带的 `build.ps1 -Target neural`（v0.4 起不随发布包分发） |

所有目标都用 **`/MT` 静态 CRT**：游戏目录里常躺着旧版 `msvcp140.dll`，`/MD` 会让私有副本覆盖
运行库，`DllMain` 可能在 ReShade 注册之后失败。

## 给宿主打补丁

`src/magpie-patches/` 里的两个补丁改的是**宿主 Magpie**（`src/Magpie.Core/FSR3Upscaler.cpp`），
不是本仓库的组件，所以**不走 `build-all.ps1`**，要用宿主的 MSBuild：

```powershell
# 1) 应用补丁（-Stage 1 只打阶段 1；-Stage 2 按序打两个；默认 -SourceDir 是 ..\magpie）
powershell -ExecutionPolicy Bypass -File scripts/apply-magpie-patch.ps1 -Stage 1 -CheckOnly   # 干跑
powershell -ExecutionPolicy Bypass -File scripts/apply-magpie-patch.ps1 -Stage 1              # 实打

# 2) 打开 FSR3 编译开关（不是改 BuildOptions.props！它要求走 .user 覆盖）
#    <Magpie 源码>\src\BuildOptions.props.user:
#      <Project><PropertyGroup><EnableFSR3ZeroMV>true</EnableFSR3ZeroMV></PropertyGroup></Project>

# 3) 出 Release
powershell -ExecutionPolicy Bypass -File <Magpie 源码>\scripts\Build-Release.ps1
```

脚本会先校验 `git rev-parse HEAD` 是不是基准提交 `6940f84`，再 `git apply --check` 干跑，
最后实打；不匹配会直接报错并提示"补丁只对基准提交有效"，**不会** `--3way` 硬合。
`-CheckOnly` 结束时会把宿主工作区还原干净。

运行期配置走环境变量（`Initialize` 里读一次，不支持运行期切换）：

| 变量 | 默认 | 作用 |
|---|---|---|
| `MAGPIE_FSR_SLOTS` | 3 | 环形槽数 1–4；`1` = 完全回到上游行为 |
| `MAGPIE_FSR_OUTPUT_LAG` | 0 | `1` = 输出晚一帧（阶段 2） |

细节（基准提交、补丁顺序、显存代价、与 `mochinr` 的 `inflight` 联动、回退方式）见
[`src/magpie-patches/README.md`](../src/magpie-patches/README.md)，设计与验收判据见
[docs/multislot.md](multislot.md)。

> 本机没有 C++ 编译器，这两个补丁**只在静态层面自检过，没有编译过**；
> 首次构建时如果 MSVC 报错，请按 [multislot.md](multislot.md) §3 的伪代码核对，不要把错改到文档里。

## 手工编译（自有组件）

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

`mochinr.addon64` 不导出 `NAME`/`DESCRIPTION`：它用 ReShade 的自注册（`reshade::register_addon`，
启动时无导出表），依赖只有
`USER32/bcrypt/SHELL32/ole32/KERNEL32` 这类系统 DLL。

`src/hooks/*.hlsl`（`AMDNR-ACO` / `AMDNR-HIP`）不是本仓库编译的，它们是 Magpie 的效果器源码，
由 Magpie 在启动时用 `d3dcompiler` 编译；改完直接重启 Magpie 即可。

## 关于 `dist/` 里的二进制

`dist/` 中的文件由本仓库源码（以及 MIT 许可的上游 OpenNR 源码）构建，可以直接用。
`mochinr.addon64` 随本仓库 MIT；`amd-nr.addon64` 是上游 OpenNR 的构建产物，再分发条件见
[THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md)。
