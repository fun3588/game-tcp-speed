// 手动映射（Manual Mapping）注入器
// ---------------------------------------------------------------------------
// 为什么需要它：
//   WoW 12.0.7 起，主 exe 变成薄壳，唯一导入的 Wow_loader.dll 是商业保护壳，
//   它在进程初始化阶段就接管模块加载，只允许「微软签名」的模块进入。
//   实测：未签名 DLL 无论放在哪个目录、哪怕只有 KERNEL32 依赖，LoadLibrary 一律返回 NULL；
//         连暴雪自家 DigiCert EV 签名的 DLL 也被拒。自签证书（方案 A）无效。
//
//   本模块不调用 LoadLibrary，而是自己把 PE 映射进目标进程：
//   解析节表 → 应用基址重定位 → 解析并填写导入表 → 一次性写入 → 远线程调 GTS_Init。
//   整个过程不经过 Windows 加载器，因此碰不到那层签名校验。
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <stdint.h>

// 传给远端 GTS_Init 的上下文（本结构必须与 hook_proxy.cpp 中的定义完全一致）
#pragma pack(push, 8)
struct GTSInitCtx {
    uintptr_t     imageBase;   // 本镜像在目标进程中的基址
    const char*   dllPath;     // DLL 在磁盘上的完整路径（ANSI），用于定位 proxy_config.ini
    volatile LONG done;        // GTS_Init 完成后写 1；注入方据此确认 APC 是否真的执行了
};
#pragma pack(pop)

struct ManualMapResult {
    uintptr_t imageBase;      // 映射后的远端基址
    DWORD     initExitCode;   // GTS_Init 的返回值（非 0 视为成功）
    DWORD     initRva;        // GTS_Init 的 RVA（便于复用镜像）
    DWORD     shutdownRva;    // GTS_Shutdown 的 RVA（卸载时远线程调用）
    char      detail[256];    // 失败原因（可直接写进 GUI 日志）
};

// 把 dllPath 手动映射进 hProcess（需 PROCESS_VM_* / PROCESS_CREATE_THREAD 权限）。
// dllPathAnsi 用于让 DLL 自己定位同目录的 proxy_config.ini，不能为空。
bool ManualMapDll(HANDLE hProcess,
                  const wchar_t* dllPath,
                  const char* dllPathAnsi,
                  ManualMapResult* out);

// 在目标进程里远线程调用 imageBase + funcRva，arg 作为唯一参数（可为 0）。
bool CallRemoteRva(HANDLE hProcess, uintptr_t imageBase, DWORD funcRva,
                   uintptr_t arg, DWORD* exitCode);

// 从磁盘上的 PE 文件读取某个导出函数的 RVA（用于手动映射后的卸载）。
bool GetExportRvaFromFile(const wchar_t* dllPath, const char* exportName, DWORD* rva);
