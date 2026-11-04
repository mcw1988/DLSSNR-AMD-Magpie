// ABI 校对探针：**不改写**接口，直接用上游宿主编译时用的那份
// `nvsdk_ngx_params.h`（D:\_build\DLSS-SDK\include）声明 NVSDK_NGX_Parameter，
// 然后把 shim 分配出来的参数对象强转成该类型并调用 Set。
//
// 目的：验证 shim 的 SlotN 是否真的对得上宿主的 vtable 布局 ——
// 这是"amdgx 有没有写错"的唯一硬证据（日志会打印每个调用落在哪个 Slot）。
//
// 构建：cl /nologo /utf-8 /EHsc /I"D:\_build\DLSS-SDK\include" abi_probe.cpp

#include <windows.h>
#include <d3d12.h>
#include <cstdio>

#include "nvsdk_ngx_params.h"

using AllocFn = int(__cdecl*)(NVSDK_NGX_Parameter**);
using DestroyFn = int(__cdecl*)(NVSDK_NGX_Parameter*);

int wmain(int argc, wchar_t** argv) {
	const wchar_t* dllPath = argc > 1 ? argv[1] : L"amdgx_dlssnr.dll";
	HMODULE module = LoadLibraryW(dllPath);
	if (!module) {
		printf("LoadLibrary failed: %lu\n", GetLastError());
		return 1;
	}
	auto alloc = reinterpret_cast<AllocFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_AllocateParameters"));
	auto destroy = reinterpret_cast<DestroyFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_DestroyParameters"));
	if (!alloc || !destroy) {
		printf("missing export(s)\n");
		return 1;
	}

	NVSDK_NGX_Parameter* params = nullptr;
	if (alloc(&params) != 1 || params == nullptr) {
		printf("AllocateParameters failed\n");
		return 1;
	}

	// 四类实参各一次，名字带 ABI_ 前缀方便在日志里定位。
	params->Set("ABI.Float", 1.25f);
	params->Set("ABI.Double", 2.5);
	params->Set("ABI.Int", 7);
	params->Set("ABI.UInt", 1920u);
	params->Set("ABI.ULL", 1234567890123ull);
	params->Set("ABI.Resource12", reinterpret_cast<ID3D12Resource*>(0x1234));
	params->Set("ABI.VoidPtr", reinterpret_cast<void*>(0x5678));

	destroy(params);
	FreeLibrary(module);
	printf("done —— 落点见 nvngx-shim.log 的 ABI.* 行\n");
	return 0;
}
