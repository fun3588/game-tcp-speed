// 手动映射注入器实现（详见 manual_map.h 的说明）
#include "manual_map.h"

#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// 基础工具
// ---------------------------------------------------------------------------

bool ReadWholeFile(const wchar_t* path, std::vector<BYTE>* out) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (64LL << 20)) {
        CloseHandle(h);
        return false;
    }
    out->resize((size_t)size.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(h, out->data(), (DWORD)out->size(), &read, NULL);
    CloseHandle(h);
    return ok && read == out->size();
}

// 把磁盘 PE 展开成「内存映像」布局（RVA 即偏移），后续所有修补都在这个缓冲区上做，
// 最后一次性写进目标进程。
bool BuildLocalImage(const std::vector<BYTE>& raw, std::vector<BYTE>* img,
                     PIMAGE_NT_HEADERS64* ntOut) {
    if (raw.size() < sizeof(IMAGE_DOS_HEADER)) return false;

    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)raw.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dos->e_lfanew <= 0 || (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > raw.size())
        return false;

    PIMAGE_NT_HEADERS64 nt = (PIMAGE_NT_HEADERS64)(raw.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;  // 只支持 x64

    const DWORD sizeOfImage = nt->OptionalHeader.SizeOfImage;
    const DWORD sizeOfHeaders = nt->OptionalHeader.SizeOfHeaders;
    if (sizeOfImage < sizeOfHeaders || sizeOfImage > (256 << 20)) return false;

    img->assign(sizeOfImage, 0);

    DWORD copyHeaders = sizeOfHeaders < raw.size() ? sizeOfHeaders : (DWORD)raw.size();
    memcpy(img->data(), raw.data(), copyHeaders);

    PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!sec->SizeOfRawData || !sec->PointerToRawData) continue;  // .bss 之类无需拷贝
        if (sec->VirtualAddress + sec->SizeOfRawData > sizeOfImage) return false;
        if ((size_t)sec->PointerToRawData + sec->SizeOfRawData > raw.size()) return false;
        memcpy(img->data() + sec->VirtualAddress,
               raw.data() + sec->PointerToRawData,
               sec->SizeOfRawData);
    }

    *ntOut = (PIMAGE_NT_HEADERS64)(img->data() + dos->e_lfanew);
    return true;
}

std::string ToAnsi(const wchar_t* w) {
    char buf[512] = {0};
    WideCharToMultiByte(CP_ACP, 0, w, -1, buf, sizeof(buf) - 1, NULL, NULL);
    return std::string(buf);
}

std::wstring ToWide(const char* a) {
    wchar_t buf[512] = {0};
    MultiByteToWideChar(CP_ACP, 0, a, -1, buf, 511);
    return std::wstring(buf);
}

// api-ms-win-crt-* API Set 实际由 ucrtbase.dll 承载。
// 手动映射必须自己完成这层转发，否则导入解析会失败（GetModuleHandle 拿不到 API Set）。
std::string ApiSetToRealModule(const std::string& name) {
    std::string lower = name;
    for (size_t i = 0; i < lower.size(); ++i) {
        if (lower[i] >= 'A' && lower[i] <= 'Z') lower[i] = (char)(lower[i] - 'A' + 'a');
    }
    if (lower.compare(0, 14, "api-ms-win-crt") == 0) return "ucrtbase.dll";
    if (lower.compare(0, 18, "api-ms-win-core-") == 0) return "kernel32.dll";
    return name;
}

// ---------------------------------------------------------------------------
// 远端操作
// ---------------------------------------------------------------------------

uintptr_t RemoteModuleBase(HANDLE hProcess, const char* moduleName) {
    DWORD pid = GetProcessId(hProcess);
    if (!pid) return 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    uintptr_t found = 0;
    MODULEENTRY32W me = { sizeof(me) };
    if (Module32FirstW(snap, &me)) {
        do {
            char name[MAX_MODULE_NAME32 + 1] = {0};
            WideCharToMultiByte(CP_ACP, 0, me.szModule, -1, name, MAX_MODULE_NAME32, NULL, NULL);
            if (_stricmp(name, moduleName) == 0) {
                found = (uintptr_t)me.modBaseAddr;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// 在目标进程内远线程调用一个「单参数或零参数」的系统 API，返回其返回值（退出码）。
DWORD RemoteCall1(HANDLE hProcess, const wchar_t* moduleName, const char* funcName,
                  uintptr_t arg, bool* threadOk) {
    if (threadOk) *threadOk = false;

    HMODULE local = GetModuleHandleW(moduleName);
    if (!local) local = LoadLibraryW(moduleName);
    if (!local) return 0;
    FARPROC fn = GetProcAddress(local, funcName);
    if (!fn) return 0;

    HANDLE th = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)fn,
                                   (LPVOID)arg, 0, NULL);
    if (!th) return 0;

    DWORD wr = WaitForSingleObject(th, 15000);
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    if (threadOk) *threadOk = (wr == WAIT_OBJECT_0);
    return code;
}

// 确保目标进程已加载某模块，返回其在目标进程中的基址（0 表示失败）。
uintptr_t EnsureRemoteModule(HANDLE hProcess, const std::string& moduleName) {
    uintptr_t base = RemoteModuleBase(hProcess, moduleName.c_str());
    if (base) return base;

    // 未加载则远线程 LoadLibrary 一次。这些都是系统签名模块，不受宿主的签名策略影响。
    std::wstring wide = ToWide(moduleName.c_str());
    size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(hProcess, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return 0;

    if (!WriteProcessMemory(hProcess, remote, wide.c_str(), bytes, NULL)) {
        VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
        return 0;
    }

    bool ok = false;
    DWORD modHandle = RemoteCall1(hProcess, L"kernel32.dll", "LoadLibraryW",
                                  (uintptr_t)remote, &ok);
    VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
    if (!ok || !modHandle) return 0;

    return RemoteModuleBase(hProcess, moduleName.c_str());
}

// 注意：导出目录里的 RVA 都是相对于「映像基址」的，必须加到 imgBase 上。
// 早期实现误加到 PIMAGE_NT_HEADERS 指针上（偏移了 e_lfanew 字节），会读到垃圾指针。
DWORD FindExportRva(BYTE* imgBase, PIMAGE_NT_HEADERS64 nt, const char* exportName) {
    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return 0;

    PIMAGE_EXPORT_DIRECTORY exp = (PIMAGE_EXPORT_DIRECTORY)(imgBase + dir.VirtualAddress);
    DWORD* names = (DWORD*)(imgBase + exp->AddressOfNames);
    WORD* ords = (WORD*)(imgBase + exp->AddressOfNameOrdinals);
    DWORD* funcs = (DWORD*)(imgBase + exp->AddressOfFunctions);

    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* n = (const char*)(imgBase + names[i]);
        if (strcmp(n, exportName) == 0) return funcs[ords[i]];
    }
    return 0;
}

// GTS_Init 完成后会把 ctx->done 置 1/2，注入方据此判断「代码到底有没有被执行」。
// 这一步必不可少：被反注入拦掉时 CreateRemoteThread 依然返回有效句柄、退出码 0，
// 光看线程返回值会把失败误判成成功。
DWORD ReadDoneFlag(HANDLE hProcess, void* remoteCtx) {
    if (!remoteCtx) return 0;
    DWORD v = 0;
    BYTE* p = (BYTE*)remoteCtx + offsetof(GTSInitCtx, done);
    if (!ReadProcessMemory(hProcess, p, &v, sizeof(v), NULL)) return 0;
    return v;
}

DWORD WaitDoneFlag(HANDLE hProcess, void* remoteCtx, DWORD timeoutMs) {
    DWORD waited = 0;
    for (;;) {
        DWORD v = ReadDoneFlag(hProcess, remoteCtx);
        // 只认终态：1=成功、2=失败；3 表示「已有别的线程在初始化」，继续等
        if (v == 1 || v == 2) return v;
        if (waited >= timeoutMs) return 0;
        Sleep(100);
        waited += 100;
    }
}

// 把目标进程内所有线程都挂一个 APC。
// 用途：某些受保护进程会拦截「起始地址不在已加载模块内」的远线程（CreateRemoteThread 静默失败），
// 但把同一地址作为 APC 投递给目标自己的线程可以正常执行。
DWORD QueueApcToAllThreads(HANDLE hProcess, uintptr_t funcAddr, uintptr_t arg) {
    DWORD pid = GetProcessId(hProcess);
    if (!pid) return 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    DWORD queued = 0;
    THREADENTRY32 te = { sizeof(te) };
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE ht = OpenThread(THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE,
                                   te.th32ThreadID);
            if (!ht) continue;
            if (QueueUserAPC((PAPCFUNC)funcAddr, ht, arg)) ++queued;
            CloseHandle(ht);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return queued;
}

void SetDetail(ManualMapResult* out, const char* fmt, ...) {
    if (!out) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->detail, sizeof(out->detail) - 1, fmt, ap);
    va_end(ap);
}

}  // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

bool ManualMapDll(HANDLE hProcess, const wchar_t* dllPath, const char* dllPathAnsi,
                  ManualMapResult* out) {
    if (!hProcess || !dllPath || !dllPathAnsi || !out) return false;
    memset(out, 0, sizeof(*out));

    std::vector<BYTE> raw;
    if (!ReadWholeFile(dllPath, &raw)) {
        SetDetail(out, "读取 DLL 失败 (err=%lu)", GetLastError());
        return false;
    }

    std::vector<BYTE> img;
    PIMAGE_NT_HEADERS64 nt = NULL;
    if (!BuildLocalImage(raw, &img, &nt)) {
        SetDetail(out, "不是有效的 x64 PE 文件");
        return false;
    }

    const DWORD sizeOfImage = nt->OptionalHeader.SizeOfImage;
    const uintptr_t preferred = (uintptr_t)nt->OptionalHeader.ImageBase;

    // 1. 在目标进程预留镜像空间（优先原始 ImageBase，失败则任意地址）
    void* remoteBase = VirtualAllocEx(hProcess, (void*)preferred, sizeOfImage,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!remoteBase) {
        remoteBase = VirtualAllocEx(hProcess, NULL, sizeOfImage,
                                    MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }
    if (!remoteBase) {
        SetDetail(out, "VirtualAllocEx 失败 (err=%lu)，目标可能拒绝了可执行内存",
                  GetLastError());
        return false;
    }
    const uintptr_t base = (uintptr_t)remoteBase;
    const ptrdiff_t delta = (ptrdiff_t)(base - preferred);

    // 2. 基址重定位（.reloc）
    if (delta != 0) {
        const IMAGE_DATA_DIRECTORY& relDir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (relDir.VirtualAddress && relDir.Size) {
            BYTE* p = img.data() + relDir.VirtualAddress;
            BYTE* end = p + relDir.Size;
            while (p + sizeof(IMAGE_BASE_RELOCATION) <= end) {
                PIMAGE_BASE_RELOCATION block = (PIMAGE_BASE_RELOCATION)p;
                if (!block->SizeOfBlock) break;
                const DWORD count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
                WORD* entries = (WORD*)(block + 1);
                for (DWORD i = 0; i < count; ++i) {
                    const WORD type = (WORD)(entries[i] >> 12);
                    const WORD off = (WORD)(entries[i] & 0x0FFF);
                    BYTE* target = img.data() + block->VirtualAddress + off;
                    if (target + sizeof(ULONGLONG) > img.data() + sizeOfImage) continue;
                    if (type == IMAGE_REL_BASED_DIR64) {
                        *(ULONGLONG*)target += (ULONGLONG)delta;
                    } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                        *(DWORD*)target += (DWORD)delta;
                    }
                }
                p += block->SizeOfBlock;
            }
        }
    }

    // 3. 解析导入表并填入目标进程中的真实地址
    //    IAT 里必须写目标进程的地址：同一份系统 DLL 在两个进程里基址可能不同（ASLR），
    //    所以用「目标模块基址 + 本进程算出的导出 RVA」来换算，而不是直接搬本进程的指针。
    const IMAGE_DATA_DIRECTORY& impDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.VirtualAddress && impDir.Size) {
        PIMAGE_IMPORT_DESCRIPTOR desc =
            (PIMAGE_IMPORT_DESCRIPTOR)(img.data() + impDir.VirtualAddress);

        for (; desc->Name; ++desc) {
            const char* rawName = (const char*)(img.data() + desc->Name);
            const std::string realName = ApiSetToRealModule(rawName);

            uintptr_t tgtBase = EnsureRemoteModule(hProcess, realName);
            if (!tgtBase) {
                SetDetail(out, "目标进程加载依赖模块失败: %s", realName.c_str());
                VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                return false;
            }

            HMODULE localMod = GetModuleHandleA(realName.c_str());
            if (!localMod) localMod = LoadLibraryA(realName.c_str());
            if (!localMod) {
                SetDetail(out, "本进程无法打开依赖模块: %s", realName.c_str());
                VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                return false;
            }
            const uintptr_t localBase = (uintptr_t)localMod;

            PIMAGE_THUNK_DATA64 oft = (PIMAGE_THUNK_DATA64)(img.data() +
                (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
            PIMAGE_THUNK_DATA64 ft = (PIMAGE_THUNK_DATA64)(img.data() + desc->FirstThunk);

            for (; oft->u1.AddressOfData; ++oft, ++ft) {
                FARPROC proc = NULL;
                if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG64) {
                    proc = GetProcAddress(localMod, (LPCSTR)(oft->u1.Ordinal & 0xFFFF));
                } else {
                    PIMAGE_IMPORT_BY_NAME ibn =
                        (PIMAGE_IMPORT_BY_NAME)(img.data() + oft->u1.AddressOfData);
                    proc = GetProcAddress(localMod, (LPCSTR)ibn->Name);
                }
                if (!proc) {
                    SetDetail(out, "解析导入失败: %s!%s", realName.c_str(),
                              (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG64) ? "(ordinal)" : "(name)");
                    VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                    return false;
                }
                ft->u1.Function = (ULONGLONG)(tgtBase + ((uintptr_t)proc - localBase));
            }
        }
    }

    // 4. TLS：静态 TLS 目录必须给 AddressOfIndex 一个有意义的槽位号，
    //    否则镜像内访问 _tls_index 会读到 0 而误用系统槽位。
    //    TLS 回调由 DLL 自己在 GTS_Init 里补调用（手动映射不会有加载器替它调用）。
    const IMAGE_DATA_DIRECTORY& tlsDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tlsDir.VirtualAddress && tlsDir.Size) {
        PIMAGE_TLS_DIRECTORY64 tls = (PIMAGE_TLS_DIRECTORY64)(img.data() + tlsDir.VirtualAddress);
        if (tls->AddressOfIndex) {
            // 注意：此时重定位已应用，AddressOfIndex 字段本身已被改成 base + RVA，
            // 因此必须减去实际基址 base（而不是 preferred），否则会写到错误位置。
            const uintptr_t idxRva = (uintptr_t)(tls->AddressOfIndex - (ULONGLONG)base);
            if (idxRva + sizeof(DWORD) <= sizeOfImage) {
                bool ok = false;
                DWORD idx = RemoteCall1(hProcess, L"kernel32.dll", "TlsAlloc", 0, &ok);
                if (ok && idx != TLS_OUT_OF_INDEXES) {
                    *(DWORD*)(img.data() + idxRva) = idx;
                }
            }
        }
    }

    // 5. 一次性写入完整镜像
    if (!WriteProcessMemory(hProcess, remoteBase, img.data(), sizeOfImage, NULL)) {
        SetDetail(out, "WriteProcessMemory 失败 (err=%lu)", GetLastError());
        VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(hProcess, remoteBase, sizeOfImage);

    const DWORD initRva = FindExportRva(img.data(), nt, "GTS_Init");
    const DWORD shutdownRva = FindExportRva(img.data(), nt, "GTS_Shutdown");
    if (!initRva) {
        SetDetail(out, "DLL 未导出 GTS_Init，无法初始化");
        VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
        return false;
    }

    // 6. 构造远端上下文并调用 GTS_Init
    const size_t pathLen = strlen(dllPathAnsi) + 1;
    const size_t ctxSize = sizeof(GTSInitCtx) + pathLen;
    void* remoteCtx = VirtualAllocEx(hProcess, NULL, ctxSize, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_READWRITE);
    if (!remoteCtx) {
        SetDetail(out, "为初始化上下文分配内存失败 (err=%lu)", GetLastError());
        VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
        return false;
    }

    GTSInitCtx ctx = {};
    ctx.imageBase = base;
    ctx.dllPath = (const char*)remoteCtx + sizeof(GTSInitCtx);
    ctx.done = 0;

    bool ok = false;
    bool apcQueued = false;
    if (WriteProcessMemory(hProcess, remoteCtx, &ctx, sizeof(ctx), NULL) &&
        WriteProcessMemory(hProcess, (void*)ctx.dllPath, dllPathAnsi, pathLen, NULL)) {

        // 测试开关：GTS_FORCE_APC=1 时跳过远线程，直接走 APC 路径（供沙箱复现用）
        const bool forceApc = GetEnvironmentVariableA("GTS_FORCE_APC", NULL, 0) > 0;

        // 6a. 先试标准远线程（普通进程走这条，最干净）
        if (!forceApc) {
            HANDLE th = CreateRemoteThread(hProcess, NULL, 0,
                                           (LPTHREAD_START_ROUTINE)(base + initRva),
                                           remoteCtx, 0, NULL);
            if (th) {
                DWORD wr = WaitForSingleObject(th, 20000);
                DWORD code = 0;
                GetExitCodeThread(th, &code);
                CloseHandle(th);
                out->initExitCode = code;
                ok = (wr == WAIT_OBJECT_0) && ReadDoneFlag(hProcess, remoteCtx) == 1;
            }
        }

        // 6b. 远线程被反注入机制拦掉时（线程返回 0、代码一条都没执行），改用 APC。
        //     实测 WoW 12.0.7 的 Wow_loader.dll 只放行「起始地址落在已加载模块内」的线程，
        //     手动映射的代码不在模块表里，因此 CreateRemoteThread 会被静默吞掉；
        //     而把同一地址挂成 APC 投递给目标自己的线程则能正常执行。
        if (!ok) {
            const DWORD queued = QueueApcToAllThreads(hProcess, base + initRva, (uintptr_t)remoteCtx);
            apcQueued = (queued > 0);
            ok = WaitDoneFlag(hProcess, remoteCtx, 20000) == 1;
            if (!ok) {
                DWORD state = ReadDoneFlag(hProcess, remoteCtx);
                SetDetail(out, "GTS_Init 未完成 (远线程无效, APC 投递 %lu 个线程, done=%lu)",
                          queued, state);
            }
        }
    } else {
        SetDetail(out, "写入初始化上下文失败 (err=%lu)", GetLastError());
    }

    // 注意：只要 APC 已经投递出去，就绝不能释放镜像与上下文——
    // 迟到的 APC 仍会跳进这块内存，释放等于给宿主埋一颗定时炸弹。
    if (!ok && !apcQueued) {
        VirtualFreeEx(hProcess, remoteCtx, 0, MEM_RELEASE);
        VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
        return false;
    }

    out->imageBase = base;
    out->initRva = initRva;
    out->shutdownRva = shutdownRva;
    SetDetail(out, "手动映射成功 base=0x%llX", (unsigned long long)base);
    return true;
}

bool CallRemoteRva(HANDLE hProcess, uintptr_t imageBase, DWORD funcRva,
                   uintptr_t arg, DWORD* exitCode) {
    if (!hProcess || !imageBase || !funcRva) return false;

    const uintptr_t addr = imageBase + funcRva;

    // 先走标准远线程；再补一次 APC 投递。
    // 受保护进程会把「起始地址不在模块内」的远线程静默吞掉（线程返回 0、代码不执行），
    // 此时只有 APC 能真正跑起来。被调用函数（GTS_Init / GTS_Shutdown）都做了幂等保护，
    // 两条路都生效也不会重复执行。
    HANDLE th = CreateRemoteThread(hProcess, NULL, 0,
                                   (LPTHREAD_START_ROUTINE)addr, (LPVOID)arg, 0, NULL);
    if (th) {
        WaitForSingleObject(th, 5000);
        DWORD code = 0;
        GetExitCodeThread(th, &code);
        CloseHandle(th);
        if (exitCode) *exitCode = code;
    }

    QueueApcToAllThreads(hProcess, addr, arg);
    return true;
}

bool GetExportRvaFromFile(const wchar_t* dllPath, const char* exportName, DWORD* rva) {
    if (!dllPath || !exportName || !rva) return false;

    std::vector<BYTE> raw;
    if (!ReadWholeFile(dllPath, &raw)) return false;

    std::vector<BYTE> img;
    PIMAGE_NT_HEADERS64 nt = NULL;
    if (!BuildLocalImage(raw, &img, &nt)) return false;

    *rva = FindExportRva(img.data(), nt, exportName);
    return *rva != 0;
}
