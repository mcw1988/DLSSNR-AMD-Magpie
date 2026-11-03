// 探针自测宿主：不启动 Magpie，直接用 Magpie 的方式调用 nvngx_dlssnr.dll，
// 验证四件事：导出可解析、参数对象能拿到、Set 调用能落进槽位、句柄生命周期通顺。
// 跑完看 nvngx-shim.log 就知道探针链路是否成立。
//
// 构建与用法见 src/nvngx-shim/build.ps1 -SelfTest

#include <windows.h>
#include <cstdio>
#include <cstdint>

// 按 NVIDIA NGX 头文件的典型布局声明 —— 只为了让调用走 vtable，
// 真正的"槽位是谁"由 DLL 里的日志回答（这正是探针要测的东西）。
class NGXParameter {
public:
	virtual ~NGXParameter() = default;
	virtual void Set(const char*, int) {}
	virtual void Set(const char*, unsigned int) {}
	virtual void Set(const char*, double) {}
	virtual void Set(const char*, unsigned long long) {}
	virtual void Set(const char*, float) {}
	virtual void Set(const char*, void*) {}
	virtual void Set(const char*, void* value, int /*tag*/) {}
	virtual void Get(const char*, int*) {}
	virtual void Get(const char*, unsigned int*) {}
	virtual void Get(const char*, double*) {}
	virtual void Get(const char*, unsigned long long*) {}
	virtual void Get(const char*, float*) {}
	virtual void Get(const char*, void**) {}
};

using InitExtFn = int(__cdecl*)(unsigned long long, const wchar_t*, void*, unsigned int, const NGXParameter*);
using AllocFn = int(__cdecl*)(NGXParameter**);
using DestroyFn = int(__cdecl*)(NGXParameter*);
using CreateFn = int(__cdecl*)(void*, int, NGXParameter*, void**);
using EvalFn = int(__cdecl*)(void*, const void*, const NGXParameter*, void*);
using ReleaseFn = int(__cdecl*)(void*);
using ShutdownFn = int(__cdecl*)(void*);

int wmain(int argc, wchar_t** argv) {
	const wchar_t* dllPath = argc > 1 ? argv[1] : L"nvngx_dlssnr.dll";
	HMODULE module = LoadLibraryW(dllPath);
	if (!module) {
		printf("LoadLibrary failed: %lu\n", GetLastError());
		return 1;
	}
	printf("loaded %ls at %p\n", dllPath, module);

	auto init = reinterpret_cast<InitExtFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_Init_Ext"));
	auto alloc = reinterpret_cast<AllocFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_AllocateParameters"));
	auto destroy = reinterpret_cast<DestroyFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_DestroyParameters"));
	auto create = reinterpret_cast<CreateFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature"));
	auto eval = reinterpret_cast<EvalFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature"));
	auto release = reinterpret_cast<ReleaseFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_ReleaseFeature"));
	auto shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_Shutdown1"));
	if (!init || !alloc || !destroy || !create || !eval || !release || !shutdown) {
		printf("missing export(s)\n");
		return 1;
	}
	printf("exports resolved\n");

	// 假指针：探针不解引用，只记录
	void* fakeDevice12 = (void*)0x1122334455667788ull;
	void* fakeCmdList = (void*)0x2233445566778899ull;

	printf("Init_Ext -> 0x%X\n", init(0, L"X:\\Magpie-SLOT-TEST", fakeDevice12, 0x00000107, nullptr));

	NGXParameter* params = nullptr;
	printf("AllocateParameters -> 0x%X\n", alloc(&params));
	if (!params) return 1;

	params->Set("DLSSNR.Width", 1920u);
	params->Set("DLSSNR.Height", 1080u);
	params->Set("DLSSNR.Color", (void*)0x3344556677889900ull);
	params->Set("DLSSNR.Output", (void*)0x4455667788990011ull);
	params->Set("DLSSNR.Style", 1);
	params->Set("DLSSNR.Intensity", 1.25f);

	void* handle = nullptr;
	printf("CreateFeature -> 0x%X\n", create(fakeCmdList, 18, params, &handle));
	printf("EvaluateFeature -> 0x%X\n", eval(fakeCmdList, handle, params, nullptr));
	printf("EvaluateFeature#2 -> 0x%X\n", eval(fakeCmdList, handle, params, nullptr));
	printf("ReleaseFeature -> 0x%X\n", release(handle));
	printf("DestroyParameters -> 0x%X\n", destroy(params));
	printf("Shutdown1 -> 0x%X\n", shutdown(fakeDevice12));

	FreeLibrary(module);
	printf("done\n");
	return 0;
}
