// 探针自测宿主：不启动 Magpie，直接用 Magpie 的方式调用 amdgx_dlssnr.dll，
// 验证四件事：导出可解析、参数对象能拿到、Set 调用能落进槽位、句柄生命周期通顺。
//
// 本程序只负责「按宿主的方式驱动」。参数通道是否真的把浮点值接到了，由调用方
// （build-p2.ps1 -SelfTest）在本进程退出后读 nvngx-shim.log 断言 ——
// 不要在这个进程里读那份日志：DLL 的日志句柄要到进程退出才真正释放。

#include <windows.h>
#include <cstdio>
#include <cstdint>

// 真实宿主的 vtable 顺序（由 P2 实机日志反推，见 docs/2026-11-04-v06-panel-wiring.md §2）：
//   虚析构占前两格 → void* / ull / int / uint / double / float
//
// 注意：这里**故意不用同名重载**。MSVC 会把同一名字的重载在 vtable 里按反序摆放，
// 于是"看起来声明成 void*,ull,int,uint,double,float"的类，实际槽位会整体反过来
// （2026-11-04 实测：float 落到 slot0、void* 落到 slot5，正是这个原因）。
// 用不同名字就能让声明顺序 = vtable 顺序，测的才是真实布局。
class NGXParameter {
public:
	virtual ~NGXParameter() = default;
	virtual void SetPtr(const char*, void*) {}              // 槽位 0
	virtual void SetUll(const char*, unsigned long long) {} // 槽位 1
	virtual void SetInt(const char*, int) {}                // 槽位 2
	virtual void SetUint(const char*, unsigned int) {}      // 槽位 3
	virtual void SetDouble(const char*, double) {}          // 槽位 4
	virtual void SetFloat(const char*, float) {}            // 槽位 5
};

using InitExtFn = int(__cdecl*)(unsigned long long, const wchar_t*, void*, unsigned int, const NGXParameter*);
using AllocFn = int(__cdecl*)(NGXParameter**);
using DestroyFn = int(__cdecl*)(NGXParameter*);
using CreateFn = int(__cdecl*)(void*, int, NGXParameter*, void**);
using EvalFn = int(__cdecl*)(void*, const void*, const NGXParameter*, void*);
using ReleaseFn = int(__cdecl*)(void*);
using ShutdownFn = int(__cdecl*)(void*);

int wmain(int argc, wchar_t** argv) {
	const wchar_t* dllPath = argc > 1 ? argv[1] : L"amdgx_dlssnr.dll";
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

	// 四类槽位各来一个；浮点两项用能被日志精确断言的值（1.25 / 0.75）。
	params->SetUint("DLSSNR.Width", 1920u);
	params->SetUint("DLSSNR.Height", 1080u);
	params->SetPtr("DLSSNR.Color", (void*)0x3344556677889900ull);
	params->SetPtr("DLSSNR.Output", (void*)0x4455667788990011ull);
	params->SetInt("DLSSNR.Style", 1);
	params->SetFloat("DLSSNR.Intensity", 1.25f);
	params->SetFloat("DLSSNR.LocalToneStrength", 0.75f);
	params->SetInt("DLSSNR.UseAutoMask", 1);

	// 自测不提供真 D3D12 资源：Eval 前把两个资源键置空，走 shim 的 "missing" 早退分支
	// （否则 shim 会对假指针调 GetDesc()，必然访问违例）。
	params->SetPtr("DLSSNR.Color", nullptr);
	params->SetPtr("DLSSNR.Output", nullptr);

	void* handle = nullptr;
	printf("CreateFeature -> 0x%X\n", create(fakeCmdList, 18, params, &handle));
	printf("EvaluateFeature -> 0x%X\n", eval(fakeCmdList, handle, params, nullptr));
	printf("EvaluateFeature#2 -> 0x%X\n", eval(fakeCmdList, handle, params, nullptr));
	printf("ReleaseFeature -> 0x%X\n", release(handle));
	printf("DestroyParameters -> 0x%X\n", destroy(params));
	printf("Shutdown1 -> 0x%X\n", shutdown(fakeDevice12));

	FreeLibrary(module);
	printf("done（参数落地请在 nvngx-shim.log 里核对）\n");
	return 0;
}
