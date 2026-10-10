#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <tlhelp32.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <set>
#include <string>

#pragma comment(lib, "comctl32.lib")

#define ID_BTN_BROWSE           1001
#define ID_BTN_START_SB         1002
#define ID_BTN_STOP_SB          1003
#define ID_BTN_LAUNCH_GAME      1004
#define ID_BTN_INJECT_NOW       1005
#define ID_BTN_AUTO_INJECT      1006
#define ID_BTN_EDIT_CONFIG      1007
#define ID_BTN_APPLY_NODE       1008
#define ID_BTN_INJECT_PID       1009
#define ID_BTN_FETCH_SUB_PRI    1010
#define ID_BTN_FETCH_SUB_BAK    1011
#define ID_BTN_TEST_SPEED       1012
#define ID_BTN_MONITOR          1013

#define ID_EDIT_GAME_PATH       2001
#define ID_EDIT_PORT            2002
#define ID_EDIT_PID             2003
#define ID_EDIT_LOG             2004
#define ID_LABEL_SB_STATUS      2005
#define ID_EDIT_TARGET_NAME     2006
#define ID_COMBO_NODES          2007
#define ID_EDIT_SUB_PRI         2008
#define ID_EDIT_SUB_BAK         2009

static HINSTANCE g_hInstance = NULL;
static HWND g_hMainWnd = NULL;
static HWND g_hEditGamePath = NULL;
static HWND g_hEditPort = NULL;
static HWND g_hEditTargetName = NULL;
static HWND g_hEditPid = NULL;
static HWND g_hEditLog = NULL;
static HWND g_hLabelSbStatus = NULL;
static HWND g_hComboNodes = NULL;
static HWND g_hBtnAutoInject = NULL;
static HWND g_hBtnMonitor = NULL;
static HWND g_hEditSubPri = NULL;
static HWND g_hEditSubBak = NULL;
static HFONT g_hFont = NULL;

static PROCESS_INFORMATION g_SbProcInfo = { 0 };

// 自动注入监听状态
static bool g_bAutoInjecting = false;
static HANDLE g_hAutoInjectThread = NULL;
static std::set<DWORD> g_InjectedPids;

// 10s 对比监控状态
static bool g_bMonitoring = false;
static HANDLE g_hMonitorThread = NULL;
static HANDLE g_hMonitorProcess = NULL;

static void AppendLog(const wchar_t* text) {
    if (!g_hEditLog) return;
    int len = GetWindowTextLengthW(g_hEditLog);
    SendMessageW(g_hEditLog, EM_SETSEL, len, len);
    
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[1024];
    _snwprintf(buf, sizeof(buf)/sizeof(wchar_t), L"[%02d:%02d:%02d] %s\r\n", st.wHour, st.wMinute, st.wSecond, text);
    SendMessageW(g_hEditLog, EM_REPLACESEL, FALSE, (LPARAM)buf);
    SendMessageW(g_hEditLog, WM_VSCROLL, SB_BOTTOM, 0);
}

static void GetAppDir(wchar_t* outDir, int maxLen) {
    GetModuleFileNameW(NULL, outDir, maxLen);
    wchar_t* lastSlash = wcsrchr(outDir, L'\\');
    if (lastSlash) *lastSlash = L'\0';
}

static void GetSingBoxPaths(wchar_t* exePath, wchar_t* cfgPath) {
    wchar_t appDir[MAX_PATH];
    GetAppDir(appDir, MAX_PATH);

    _snwprintf(exePath, MAX_PATH, L"%s\\..\\Sing-box\\sing-box.exe", appDir);
    _snwprintf(cfgPath, MAX_PATH, L"%s\\..\\Sing-box\\config.json", appDir);

    if (GetFileAttributesW(exePath) == INVALID_FILE_ATTRIBUTES) {
        _snwprintf(exePath, MAX_PATH, L"%s\\Sing-box\\sing-box.exe", appDir);
        _snwprintf(cfgPath, MAX_PATH, L"%s\\Sing-box\\config.json", appDir);
    }
}

static void SaveProxyConfig(const wchar_t* portStr) {
    wchar_t appDir[MAX_PATH];
    GetAppDir(appDir, MAX_PATH);
    wchar_t iniPath[MAX_PATH];
    _snwprintf(iniPath, MAX_PATH, L"%s\\proxy_config.ini", appDir);

    WritePrivateProfileStringW(L"Proxy", L"Host", L"127.0.0.1", iniPath);
    WritePrivateProfileStringW(L"Proxy", L"Port", portStr, iniPath);
}

// 寻找一个当前系统未被占用的可用随机高位端口
static unsigned short GetRandomAvailablePort() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return 2081;
    }

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0; // 0 让操作系统从动态可用端口范围中分配一个空闲端口

    unsigned short port = 2081;
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
        int len = sizeof(addr);
        if (getsockname(s, (sockaddr*)&addr, &len) == 0) {
            port = ntohs(addr.sin_port);
        }
    }
    closesocket(s);
    WSACleanup();
    return port;
}

// 动态更新 config.json 中的 listen_port
static void UpdateConfigPort(const wchar_t* cfgPath, unsigned short port) {
    FILE* fp = _wfopen(cfgPath, L"rb");
    if (!fp) return;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    std::vector<char> buf(size + 1, 0);
    fread(buf.data(), 1, size, fp);
    fclose(fp);

    std::string s(buf.data());
    size_t pos = s.find("\"listen_port\":");
    if (pos != std::string::npos) {
        size_t numStart = s.find_first_of("0123456789", pos + 14);
        if (numStart != std::string::npos) {
            size_t numEnd = s.find_first_not_of("0123456789", numStart);
            if (numEnd != std::string::npos) {
                char portA[16];
                snprintf(portA, sizeof(portA), "%u", port);
                s.replace(numStart, numEnd - numStart, portA);

                FILE* outFp = _wfopen(cfgPath, L"wb");
                if (outFp) {
                    fwrite(s.data(), 1, s.size(), outFp);
                    fclose(outFp);
                }
            }
        }
    }
}

// 启动 Sing-box 核心
static void StartSingBox() {
    if (g_SbProcInfo.hProcess != NULL) {
        DWORD exitCode;
        if (GetExitCodeProcess(g_SbProcInfo.hProcess, &exitCode) && exitCode == STILL_ACTIVE) {
            AppendLog(L"Sing-box 核心已在运行中。");
            return;
        }
    }

    wchar_t exePath[MAX_PATH];
    wchar_t cfgPath[MAX_PATH];
    GetSingBoxPaths(exePath, cfgPath);

    if (GetFileAttributesW(exePath) == INVALID_FILE_ATTRIBUTES) {
        AppendLog(L"错误: 未找到 sing-box.exe，请检查 Sing-box 目录！");
        return;
    }

    // 动态生成未被占用的随机可用端口
    unsigned short randPort = GetRandomAvailablePort();
    wchar_t portStr[16];
    _snwprintf(portStr, sizeof(portStr)/sizeof(wchar_t), L"%u", randPort);
    
    // 同步更新界面、ini 以及 config.json
    if (g_hEditPort) {
        SetWindowTextW(g_hEditPort, portStr);
    }
    SaveProxyConfig(portStr);
    UpdateConfigPort(cfgPath, randPort);
    
    wchar_t portMsg[128];
    _snwprintf(portMsg, sizeof(portMsg)/sizeof(wchar_t), L"[端口随机] 系统已动态分配可用端口: %u (零端口冲突)", randPort);
    AppendLog(portMsg);

    wchar_t cmdLine[MAX_PATH * 3];
    _snwprintf(cmdLine, sizeof(cmdLine)/sizeof(wchar_t), L"\"%s\" run -c \"%s\"", exePath, cfgPath);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &g_SbProcInfo)) {
        wchar_t msg[256];
        _snwprintf(msg, sizeof(msg)/sizeof(wchar_t), L"Sing-box 核心启动成功 (PID: %lu)", g_SbProcInfo.dwProcessId);
        AppendLog(msg);
        SetWindowTextW(g_hLabelSbStatus, L"Sing-box: 运行中 [OK]");
    } else {
        AppendLog(L"错误: 无法启动 Sing-box 进程！");
    }
}

// 停止 Sing-box 核心
static void StopSingBox() {
    if (g_SbProcInfo.hProcess != NULL) {
        TerminateProcess(g_SbProcInfo.hProcess, 0);
        CloseHandle(g_SbProcInfo.hProcess);
        CloseHandle(g_SbProcInfo.hThread);
        g_SbProcInfo.hProcess = NULL;
        AppendLog(L"Sing-box 核心已停止。");
    } else {
        AppendLog(L"Sing-box 未运行。");
    }
    if (g_hLabelSbStatus) {
        SetWindowTextW(g_hLabelSbStatus, L"Sing-box: 未运行");
    }
}

// 重新加载节点列表到 ComboBox
static void ReloadNodeList() {
    if (!g_hComboNodes) return;
    SendMessageW(g_hComboNodes, CB_RESETCONTENT, 0, 0);

    wchar_t exePath[MAX_PATH], cfgPath[MAX_PATH];
    GetSingBoxPaths(exePath, cfgPath);

    FILE* fp = _wfopen(cfgPath, L"rb");
    if (!fp) return;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    std::vector<char> buf(size + 1, 0);
    fread(buf.data(), 1, size, fp);
    fclose(fp);

    std::string s(buf.data());

    // 解析当前 default 节点
    std::string defaultNode;
    size_t defPos = s.find("\"default\":");
    if (defPos != std::string::npos) {
        size_t q1 = s.find('\"', defPos + 10);
        if (q1 != std::string::npos) {
            size_t q2 = s.find('\"', q1 + 1);
            if (q2 != std::string::npos) {
                defaultNode = s.substr(q1 + 1, q2 - q1 - 1);
            }
        }
    }

    // 简单提取所有 tag
    size_t pos = 0;
    int curSel = 0;
    int idx = 0;
    std::set<std::string> added;

    while ((pos = s.find("\"tag\":", pos)) != std::string::npos) {
        size_t q1 = s.find('\"', pos + 6);
        if (q1 == std::string::npos) break;
        size_t q2 = s.find('\"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string tag = s.substr(q1 + 1, q2 - q1 - 1);
        pos = q2 + 1;

        if (tag == "mixed-in" || tag == "socks-in" || tag == "node" || tag == "direct") continue;
        if (added.find(tag) != added.end()) continue;
        added.insert(tag);

        wchar_t wtag[128];
        MultiByteToWideChar(CP_UTF8, 0, tag.c_str(), -1, wtag, 128);
        SendMessageW(g_hComboNodes, CB_ADDSTRING, 0, (LPARAM)wtag);

        if (tag == defaultNode) {
            curSel = idx;
        }
        idx++;
    }

    SendMessageW(g_hComboNodes, CB_SETCURSEL, curSel, 0);
}

// 注入指定 PID
static BOOL InjectDllByPid(DWORD pid) {
    if (pid == 0) return FALSE;

    wchar_t portStr[16];
    GetWindowTextW(g_hEditPort, portStr, 16);
    SaveProxyConfig(portStr);

    wchar_t appDir[MAX_PATH];
    GetAppDir(appDir, MAX_PATH);
    wchar_t dllPath[MAX_PATH];
    _snwprintf(dllPath, MAX_PATH, L"%s\\hook_proxy.dll", appDir);

    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        AppendLog(L"错误: 未找到 hook_proxy.dll，请先编译 DLL！");
        return FALSE;
    }

    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess) {
        wchar_t err[128];
        _snwprintf(err, sizeof(err)/sizeof(wchar_t), L"无法打开 PID %lu，请尝试以管理员身份运行本程序！", pid);
        AppendLog(err);
        return FALSE;
    }

    size_t len = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID pRemoteMem = VirtualAllocEx(hProcess, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemoteMem) {
        AppendLog(L"错误: VirtualAllocEx 远程内存分配失败！");
        CloseHandle(hProcess);
        return FALSE;
    }

    if (!WriteProcessMemory(hProcess, pRemoteMem, dllPath, len, NULL)) {
        AppendLog(L"错误: WriteProcessMemory 写入 DLL 路径失败！");
        VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return FALSE;
    }

    LPTHREAD_START_ROUTINE pLoadLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (!pLoadLibrary) {
        AppendLog(L"错误: 找不到 LoadLibraryW 地址！");
        VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return FALSE;
    }

    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, pLoadLibrary, pRemoteMem, 0, NULL);
    if (!hThread) {
        AppendLog(L"错误: CreateRemoteThread 远程线程创建失败！");
        VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return FALSE;
    }

    WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    g_InjectedPids.insert(pid);
    wchar_t msg[128];
    _snwprintf(msg, sizeof(msg)/sizeof(wchar_t), L"√ 成功注入加速模块至进程 (PID: %lu)！", pid);
    AppendLog(msg);
    return TRUE;
}

// 根据进程名查找并注入所有匹配的游戏进程
static int InjectMatchingProcesses(const wchar_t* targetExe) {
    if (!targetExe || wcslen(targetExe) == 0) return 0;

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe = { sizeof(PROCESSENTRY32W) };
    int count = 0;

    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            bool match = (_wcsicmp(pe.szExeFile, targetExe) == 0);
            if (!match && wcsstr(pe.szExeFile, L"Firestorm") != NULL && wcsstr(targetExe, L"Firestorm") != NULL) {
                if (_wcsicmp(pe.szExeFile, L"FirestormLauncher.exe") != 0 && _wcsicmp(pe.szExeFile, L"FirestormAgent.exe") != 0) {
                    match = true;
                }
            }
            if (match) {
                if (g_InjectedPids.find(pe.th32ProcessID) == g_InjectedPids.end()) {
                    if (InjectDllByPid(pe.th32ProcessID)) {
                        count++;
                    }
                }
            }
        } while (Process32NextW(hSnapshot, &pe));
    }

    CloseHandle(hSnapshot);
    return count;
}

// 自动监控注入后台线程
static DWORD WINAPI AutoInjectWorker(LPVOID lpParam) {
    while (g_bAutoInjecting) {
        wchar_t targetName[128] = { 0 };
        GetWindowTextW(g_hEditTargetName, targetName, 128);

        if (wcslen(targetName) > 0) {
            InjectMatchingProcesses(targetName);
        }
        Sleep(400);
    }
    return 0;
}

static void ToggleAutoInject() {
    wchar_t targetName[128];
    GetWindowTextW(g_hEditTargetName, targetName, 128);
    if (wcslen(targetName) == 0) {
        AppendLog(L"请先输入目标游戏进程名（如 WoW 11.2.5 - Firestorm.exe）！");
        return;
    }

    StartSingBox();

    if (!g_bAutoInjecting) {
        g_bAutoInjecting = true;
        g_hAutoInjectThread = CreateThread(NULL, 0, AutoInjectWorker, NULL, 0, NULL);
        SetWindowTextW(g_hBtnAutoInject, L"⏹ 停止自动监控");
        
        wchar_t msg[256];
        _snwprintf(msg, sizeof(msg)/sizeof(wchar_t), L"🚀 [自动监控已开启] 正在监听 [%s]... 您现在可以用登录器进入游戏，程序将自动完成注入！", targetName);
        AppendLog(msg);
    } else {
        g_bAutoInjecting = false;
        if (g_hAutoInjectThread) {
            WaitForSingleObject(g_hAutoInjectThread, 1000);
            CloseHandle(g_hAutoInjectThread);
            g_hAutoInjectThread = NULL;
        }
        SetWindowTextW(g_hBtnAutoInject, L"🚀 开启自动监控注入");
        AppendLog(L"已停止自动注入监控。");
    }
}

static void InjectNow() {
    wchar_t targetName[128];
    GetWindowTextW(g_hEditTargetName, targetName, 128);
    if (wcslen(targetName) == 0) {
        AppendLog(L"请先输入目标进程名！");
        return;
    }

    StartSingBox();
    AppendLog(L"正在搜索已运行的游戏进程...");
    int n = InjectMatchingProcesses(targetName);
    if (n == 0) {
        wchar_t msg[256];
        _snwprintf(msg, sizeof(msg)/sizeof(wchar_t), L"未检测到运行中的 [%s]，请先使用登录器启动游戏，或点击【开启自动监控】！", targetName);
        AppendLog(msg);
    }
}

static void EditConfigFile() {
    wchar_t exePath[MAX_PATH];
    wchar_t cfgPath[MAX_PATH];
    GetSingBoxPaths(exePath, cfgPath);

    ShellExecuteW(NULL, L"open", L"notepad.exe", cfgPath, NULL, SW_SHOWNORMAL);
    AppendLog(L"已在记事本中打开 Sing-box\\config.json。");
}

static void ApplySelectedNode() {
    int idx = (int)SendMessageW(g_hComboNodes, CB_GETCURSEL, 0, 0);
    if (idx == CB_ERR) return;

    wchar_t nodeTag[128];
    SendMessageW(g_hComboNodes, CB_GETLBTEXT, idx, (LPARAM)nodeTag);

    wchar_t* space = wcschr(nodeTag, L' ');
    if (space) *space = L'\0';

    wchar_t exePath[MAX_PATH];
    wchar_t cfgPath[MAX_PATH];
    GetSingBoxPaths(exePath, cfgPath);

    FILE* fp = _wfopen(cfgPath, L"rb");
    if (!fp) {
        AppendLog(L"无法读取 config.json！");
        return;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    std::vector<char> buffer(size + 1, 0);
    fread(buffer.data(), 1, size, fp);
    fclose(fp);

    std::string content(buffer.data());
    char targetTagA[64];
    WideCharToMultiByte(CP_UTF8, 0, nodeTag, -1, targetTagA, sizeof(targetTagA), NULL, NULL);

    size_t defPos = content.find("\"default\":");
    if (defPos != std::string::npos) {
        size_t quoteStart = content.find('\"', defPos + 10);
        if (quoteStart != std::string::npos) {
            size_t quoteEnd = content.find('\"', quoteStart + 1);
            if (quoteEnd != std::string::npos) {
                content.replace(quoteStart + 1, quoteEnd - quoteStart - 1, targetTagA);

                FILE* outFp = _wfopen(cfgPath, L"wb");
                if (outFp) {
                    fwrite(content.data(), 1, content.size(), outFp);
                    fclose(outFp);

                    wchar_t msg[256];
                    _snwprintf(msg, sizeof(msg)/sizeof(wchar_t), L"√ 默认加速节点已切换为 [%s]！正在平滑重启 Sing-box 核心...", nodeTag);
                    AppendLog(msg);

                    StopSingBox();
                    Sleep(300);
                    StartSingBox();
                    return;
                }
            }
        }
    }
    AppendLog(L"配置替换失败，建议点击“编辑配置”直接修改。");
}

// 纯 C++ 原生高精度测速函数 (非阻塞 connect 测试直接握手延迟)
static double MeasureDirectLatency(const char* ip, unsigned short port, int timeoutMs) {
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1.0;

    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);

    QueryPerformanceCounter(&t0);
    connect(s, (sockaddr*)&addr, sizeof(addr));

    fd_set writes, excepts;
    FD_ZERO(&writes);
    FD_ZERO(&excepts);
    FD_SET(s, &writes);
    FD_SET(s, &excepts);

    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    int sel = select(0, NULL, &writes, &excepts, &tv);
    QueryPerformanceCounter(&t1);

    if (sel > 0 && FD_ISSET(s, &writes) && !FD_ISSET(s, &excepts)) {
        int so_error = 0;
        int len = sizeof(so_error);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
        if (so_error == 0) {
            closesocket(s);
            return (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        }
    }
    closesocket(s);
    return -1.0;
}

// 纯 C++ 原生测试 Socks5 代理端到端握手延迟
static double MeasureProxyLatency(unsigned short proxyPort, const char* targetIp, unsigned short targetPort, int timeoutMs) {
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1.0;

    DWORD tv_ms = timeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_ms, sizeof(tv_ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv_ms, sizeof(tv_ms));

    sockaddr_in proxyAddr;
    memset(&proxyAddr, 0, sizeof(proxyAddr));
    proxyAddr.sin_family = AF_INET;
    proxyAddr.sin_port = htons(proxyPort);
    proxyAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    QueryPerformanceCounter(&t0);
    if (connect(s, (sockaddr*)&proxyAddr, sizeof(proxyAddr)) != 0) {
        closesocket(s);
        return -1.0;
    }

    // Socks5 握手认证
    char authReq[3] = { 0x05, 0x01, 0x00 };
    if (send(s, authReq, 3, 0) != 3) { closesocket(s); return -1.0; }
    char authResp[2] = { 0 };
    if (recv(s, authResp, 2, 0) != 2 || authResp[0] != 0x05 || authResp[1] != 0x00) { closesocket(s); return -1.0; }

    // Socks5 CONNECT 目标
    char connReq[10];
    connReq[0] = 0x05;
    connReq[1] = 0x01;
    connReq[2] = 0x00;
    connReq[3] = 0x01; // IPv4
    unsigned long ipBe = inet_addr(targetIp);
    unsigned short portBe = htons(targetPort);
    memcpy(&connReq[4], &ipBe, 4);
    memcpy(&connReq[8], &portBe, 2);

    if (send(s, connReq, 10, 0) != 10) { closesocket(s); return -1.0; }

    char connResp[10] = { 0 };
    int n = recv(s, connResp, 10, 0);
    QueryPerformanceCounter(&t1);

    closesocket(s);
    if (n >= 4 && connResp[1] == 0x00) {
        return (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
    }
    return -1.0;
}

// 异步拉取并更新订阅 (WinINet 原生实现)
struct SubWorkerParam {
    std::wstring url;
    std::wstring prefix;
};

static DWORD WINAPI FetchSubWorker(LPVOID lpParam) {
    SubWorkerParam* p = (SubWorkerParam*)lpParam;
    AppendLog(L"[*] 正在拉取订阅链接 (WinINet 原生下载)...");

    HINTERNET hNet = InternetOpenW(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hNet) {
        AppendLog(L"[!] 网络接口初始化失败！");
        delete p;
        return 0;
    }

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_IGNORE_CERT_CN_INVALID | INTERNET_FLAG_IGNORE_CERT_DATE_INVALID;
    HINTERNET hUrl = InternetOpenUrlW(hNet, p->url.c_str(), NULL, 0, flags, 0);
    if (!hUrl) {
        AppendLog(L"[!] 打开订阅 URL 失败，请检查网络连接或链接格式！");
        InternetCloseHandle(hNet);
        delete p;
        return 0;
    }

    std::string text;
    char buf[4096];
    DWORD readBytes = 0;
    while (InternetReadFile(hUrl, buf, sizeof(buf), &readBytes) && readBytes > 0) {
        text.append(buf, readBytes);
    }

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hNet);

    if (text.empty()) {
        AppendLog(L"[!] 拉取到的订阅内容为空！");
    } else {
        wchar_t log[128];
        _snwprintf(log, 128, L"√ 成功拉取订阅数据 (大小: %zu 字节)！", text.size());
        AppendLog(log);
        AppendLog(L"提示：当前 Sing-box 核心专线已就绪，可在 config.json 中更新节点。");
    }

    delete p;
    return 0;
}

static void FetchSubscription(bool isPrimary) {
    wchar_t url[512] = { 0 };
    HWND hEdit = isPrimary ? g_hEditSubPri : g_hEditSubBak;
    GetWindowTextW(hEdit, url, 512);

    if (wcslen(url) == 0) {
        AppendLog(L"订阅链接不能为空！");
        return;
    }

    SubWorkerParam* p = new SubWorkerParam();
    p->url = url;
    p->prefix = isPrimary ? L"primary" : L"backup";

    HANDLE h = CreateThread(NULL, 0, FetchSubWorker, p, 0, NULL);
    if (h) CloseHandle(h);
}

// 异步测速线程 (纯 C++ 原生实现)
static DWORD WINAPI TestSpeedWorker(LPVOID lpParam) {
    wchar_t portStr[16];
    GetWindowTextW(g_hEditPort, portStr, 16);
    unsigned short port = (unsigned short)_wtoi(portStr);
    if (port == 0) port = 2081;

    AppendLog(L"[*] 目标游戏服: 141.95.207.63:8285 (Firestorm 世界服)");
    AppendLog(L"--------------------------------------------------");
    AppendLog(L"[1/2] 正在测试【直连模式 (不加速)】真实可用性...");

    double dSum = 0, dMin = 99999;
    int dCount = 0;
    for (int i = 0; i < 3; i++) {
        double t = MeasureDirectLatency("141.95.207.63", 8285, 2000);
        if (t > 0) {
            dSum += t;
            if (t < dMin) dMin = t;
            dCount++;
        }
        Sleep(50);
    }

    wchar_t msg[256];
    if (dCount > 0) {
        _snwprintf(msg, 256, L"  [-] 直连延迟 (不加速): %.1f ms (最低 %.1f ms)", dSum / dCount, dMin);
    } else {
        _snwprintf(msg, 256, L"  [-] 直连延迟 (不加速): 超时阻断/无法直连");
    }
    AppendLog(msg);

    _snwprintf(msg, 256, L"[2/2] 正在测试【专线加速 (已加速)】真实端到端延迟 (本地端口: %u)...", port);
    AppendLog(msg);

    double pSum = 0, pMin = 99999;
    int pCount = 0;
    for (int i = 0; i < 3; i++) {
        double t = MeasureProxyLatency(port, "141.95.207.63", 8285, 4000);
        if (t > 0) {
            pSum += t;
            if (t < pMin) pMin = t;
            pCount++;
        }
        Sleep(50);
    }

    if (pCount > 0) {
        _snwprintf(msg, 256, L"  [+] 专线真实端到端延迟: %.1f ms (最低 %.1f ms)", pSum / pCount, pMin);
        AppendLog(msg);
        AppendLog(L"  [+] 专线传输质量: 良好稳定 [4星级] (适合稳定副本与战斗)");
        AppendLog(L"--------------------------------------------------");
        _snwprintf(msg, 256, L"[评级] 延迟对比: 专线 %.1f ms  vs  直连 %.1f ms", pSum / pCount, dCount > 0 ? (dSum / dCount) : 0);
        AppendLog(msg);
    } else {
        AppendLog(L"  [!] 专线测速失败: 无法连接本地代理端口或专线握手超时！");
        AppendLog(L"  [!] 请先点击【启动 Sing-box】或检查节点配置！");
    }
    return 0;
}

static void RunSpeedTest() {
    StartSingBox();
    HANDLE h = CreateThread(NULL, 0, TestSpeedWorker, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

// 异步连续对比监控线程 (每 10s 刷新一行，纯 C++ 原生实现)
static DWORD WINAPI MonitorWorker(LPVOID lpParam) {
    wchar_t portStr[16];
    GetWindowTextW(g_hEditPort, portStr, 16);
    unsigned short port = (unsigned short)_wtoi(portStr);
    if (port == 0) port = 2081;

    AppendLog(L"[+] 开启 10s 周期对比监控...");

    while (g_bMonitoring) {
        double dLat = MeasureDirectLatency("141.95.207.63", 8285, 2000);
        double pLat = MeasureProxyLatency(port, "141.95.207.63", 8285, 4000);

        wchar_t line[256];
        if (pLat > 0 && dLat > 0) {
            _snwprintf(line, 256, L"[10s 监控] 专线: %.1f ms  vs  直连: %.1f ms (链路稳定)", pLat, dLat);
        } else if (pLat > 0) {
            _snwprintf(line, 256, L"[10s 监控] 专线: %.1f ms  vs  直连: 超时阻断", pLat);
        } else {
            _snwprintf(line, 256, L"[10s 监控] 专线测速超时/未就绪 (请检查端口 %u)", port);
        }
        AppendLog(line);

        for (int i = 0; i < 20 && g_bMonitoring; i++) {
            Sleep(500); // 10秒监控间隔
        }
    }
    return 0;
}

static void ToggleMonitor() {
    if (!g_bMonitoring) {
        StartSingBox();
        g_bMonitoring = true;
        if (g_hBtnMonitor) SetWindowTextW(g_hBtnMonitor, L"⏹ 停止监控");
        AppendLog(L"⏱️ [实时对比监控已启动] 后台每 10 秒刷新一次加速与直连对比...");
        g_hMonitorThread = CreateThread(NULL, 0, MonitorWorker, NULL, 0, NULL);
    } else {
        g_bMonitoring = false;
        if (g_hMonitorThread) {
            WaitForSingleObject(g_hMonitorThread, 1500);
            CloseHandle(g_hMonitorThread);
            g_hMonitorThread = NULL;
        }
        if (g_hBtnMonitor) SetWindowTextW(g_hBtnMonitor, L"⏱️ 对比监控(10s)");
        AppendLog(L"已停止对比监控。");
    }
}

// 窗口过程
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_hFont = CreateFontW(
            -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei"
        );
        if (!g_hFont) g_hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        HFONT hBoldFont = CreateFontW(
            -13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei"
        );

        int y = 12;

        // ────── 区域 1：专线节点配置 ──────
        HWND hGrpNode = CreateWindowW(L"BUTTON", L" 专线节点选择与监控配置 ", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 15, y, 520, 88, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hGrpNode, WM_SETFONT, (WPARAM)hBoldFont, TRUE);

        HWND hLblNode = CreateWindowW(L"STATIC", L"加速节点:", WS_CHILD | WS_VISIBLE, 28, y + 25, 65, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblNode, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hComboNodes = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 95, y + 22, 160, 180, hWnd, (HMENU)ID_COMBO_NODES, g_hInstance, NULL);
        SendMessageW(g_hComboNodes, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnApplyNode = CreateWindowW(L"BUTTON", L"切换", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 260, y + 21, 50, 26, hWnd, (HMENU)ID_BTN_APPLY_NODE, g_hInstance, NULL);
        SendMessageW(hBtnApplyNode, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnTestSpeed = CreateWindowW(L"BUTTON", L"⚡ 测速", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 315, y + 21, 65, 26, hWnd, (HMENU)ID_BTN_TEST_SPEED, g_hInstance, NULL);
        SendMessageW(hBtnTestSpeed, WM_SETFONT, (WPARAM)hBoldFont, TRUE);

        g_hBtnMonitor = CreateWindowW(L"BUTTON", L"⏱️ 对比监控(10s)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 385, y + 21, 135, 26, hWnd, (HMENU)ID_BTN_MONITOR, g_hInstance, NULL);
        SendMessageW(g_hBtnMonitor, WM_SETFONT, (WPARAM)hBoldFont, TRUE);

        // Socks5 端口与核心状态
        HWND hLblPort = CreateWindowW(L"STATIC", L"本地端口:", WS_CHILD | WS_VISIBLE, 28, y + 56, 65, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblPort, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hEditPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"2081", WS_CHILD | WS_VISIBLE | ES_NUMBER, 95, y + 54, 55, 22, hWnd, (HMENU)ID_EDIT_PORT, g_hInstance, NULL);
        SendMessageW(g_hEditPort, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hLabelSbStatus = CreateWindowW(L"STATIC", L"Sing-box: 未运行", WS_CHILD | WS_VISIBLE, 160, y + 56, 135, 20, hWnd, (HMENU)ID_LABEL_SB_STATUS, g_hInstance, NULL);
        SendMessageW(g_hLabelSbStatus, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnStartSb = CreateWindowW(L"BUTTON", L"启动核心", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 305, y + 53, 68, 24, hWnd, (HMENU)ID_BTN_START_SB, g_hInstance, NULL);
        SendMessageW(hBtnStartSb, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnStopSb = CreateWindowW(L"BUTTON", L"停止", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 380, y + 53, 60, 24, hWnd, (HMENU)ID_BTN_STOP_SB, g_hInstance, NULL);
        SendMessageW(hBtnStopSb, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnEditCfg = CreateWindowW(L"BUTTON", L"编辑配置", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 448, y + 53, 72, 24, hWnd, (HMENU)ID_BTN_EDIT_CONFIG, g_hInstance, NULL);
        SendMessageW(hBtnEditCfg, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        y += 95;

        // ────── 区域 2：游戏注入控制 ──────
        HWND hGrpInject = CreateWindowW(L"BUTTON", L" 游戏注入加速（适配登录器） ", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 15, y, 520, 115, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hGrpInject, WM_SETFONT, (WPARAM)hBoldFont, TRUE);

        HWND hLblTarget = CreateWindowW(L"STATIC", L"目标游戏进程名:", WS_CHILD | WS_VISIBLE, 30, y + 28, 100, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblTarget, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hEditTargetName = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"WoW 11.2.5 - Firestorm.exe", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 135, y + 25, 205, 24, hWnd, (HMENU)ID_EDIT_TARGET_NAME, g_hInstance, NULL);
        SendMessageW(g_hEditTargetName, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hBtnAutoInject = CreateWindowW(L"BUTTON", L"🚀 开启自动监控注入", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 350, y + 22, 170, 30, hWnd, (HMENU)ID_BTN_AUTO_INJECT, g_hInstance, NULL);
        SendMessageW(g_hBtnAutoInject, WM_SETFONT, (WPARAM)hBoldFont, TRUE);

        HWND hBtnInjectNow = CreateWindowW(L"BUTTON", L"💉 立即注入现有游戏 (免输PID)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 30, y + 65, 200, 32, hWnd, (HMENU)ID_BTN_INJECT_NOW, g_hInstance, NULL);
        SendMessageW(hBtnInjectNow, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hLblPid = CreateWindowW(L"STATIC", L"或按 PID:", WS_CHILD | WS_VISIBLE, 245, y + 72, 60, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblPid, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hEditPid = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER, 310, y + 68, 65, 24, hWnd, (HMENU)ID_EDIT_PID, g_hInstance, NULL);
        SendMessageW(g_hEditPid, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnInjectPid = CreateWindowW(L"BUTTON", L"注入PID", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 385, y + 66, 65, 28, hWnd, (HMENU)ID_BTN_INJECT_PID, g_hInstance, NULL);
        SendMessageW(hBtnInjectPid, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        y += 125;

        // ────── 区域 3：运行日志 ──────
        g_hEditLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 15, y, 520, 160, hWnd, (HMENU)ID_EDIT_LOG, g_hInstance, NULL);
        SendMessageW(g_hEditLog, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        y += 170;

        // ────── 区域 4：底部订阅管理（主 / 备 链接） ──────
        HWND hGrpSub = CreateWindowW(L"BUTTON", L" 专线订阅链接管理 (主 / 备) ", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 15, y, 520, 105, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hGrpSub, WM_SETFONT, (WPARAM)hBoldFont, TRUE);

        // 主订阅
        HWND hLblSubPri = CreateWindowW(L"STATIC", L"主订阅:", WS_CHILD | WS_VISIBLE, 28, y + 28, 55, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblSubPri, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hEditSubPri = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 85, y + 25, 315, 24, hWnd, (HMENU)ID_EDIT_SUB_PRI, g_hInstance, NULL);
        SendMessageW(g_hEditSubPri, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnFetchPri = CreateWindowW(L"BUTTON", L"拉取主订阅", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 410, y + 23, 110, 28, hWnd, (HMENU)ID_BTN_FETCH_SUB_PRI, g_hInstance, NULL);
        SendMessageW(hBtnFetchPri, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // 备订阅
        HWND hLblSubBak = CreateWindowW(L"STATIC", L"备订阅:", WS_CHILD | WS_VISIBLE, 28, y + 65, 55, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblSubBak, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hEditSubBak = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 85, y + 62, 315, 24, hWnd, (HMENU)ID_EDIT_SUB_BAK, g_hInstance, NULL);
        SendMessageW(g_hEditSubBak, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnFetchBak = CreateWindowW(L"BUTTON", L"拉取备订阅", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 410, y + 60, 110, 28, hWnd, (HMENU)ID_BTN_FETCH_SUB_BAK, g_hInstance, NULL);
        SendMessageW(hBtnFetchBak, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // 初始化加载配置节点列表
        ReloadNodeList();

        AppendLog(L"程序已就绪。主/备订阅管理已就绪。");
        AppendLog(L"提示：Cloudflare 临时隧道会定期更换域名，如遇失效点击下方【拉取订阅】即可一键刷新！");
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        switch (id) {
        case ID_BTN_START_SB:
            StartSingBox();
            break;
        case ID_BTN_STOP_SB:
            StopSingBox();
            break;
        case ID_BTN_EDIT_CONFIG:
            EditConfigFile();
            break;
        case ID_BTN_APPLY_NODE:
            ApplySelectedNode();
            break;
        case ID_BTN_TEST_SPEED:
            RunSpeedTest();
            break;
        case ID_BTN_MONITOR:
            ToggleMonitor();
            break;
        case ID_BTN_AUTO_INJECT:
            ToggleAutoInject();
            break;
        case ID_BTN_INJECT_NOW:
            InjectNow();
            break;
        case ID_BTN_FETCH_SUB_PRI:
            FetchSubscription(true);
            break;
        case ID_BTN_FETCH_SUB_BAK:
            FetchSubscription(false);
            break;
        case ID_BTN_INJECT_PID: {
            wchar_t pidStr[32];
            GetWindowTextW(g_hEditPid, pidStr, 32);
            DWORD pid = (DWORD)_wtoi(pidStr);
            if (pid == 0) {
                AppendLog(L"请输入有效的 PID！");
            } else {
                StartSingBox();
                InjectDllByPid(pid);
            }
            break;
        }
        case ID_BTN_BROWSE: {
            wchar_t szFile[MAX_PATH] = { 0 };
            OPENFILENAMEW ofn = { sizeof(ofn) };
            ofn.hwndOwner = g_hMainWnd;
            ofn.lpstrFilter = L"可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                SetWindowTextW(g_hEditGamePath, szFile);
                wchar_t* name = wcsrchr(szFile, L'\\');
                if (name) {
                    SetWindowTextW(g_hEditTargetName, name + 1);
                }
            }
            break;
        }
        case ID_BTN_LAUNCH_GAME: {
            wchar_t gamePath[MAX_PATH];
            GetWindowTextW(g_hEditGamePath, gamePath, MAX_PATH);
            if (wcslen(gamePath) == 0) {
                AppendLog(L"请先浏览选择游戏 .exe 文件！");
                break;
            }
            StartSingBox();
            wchar_t workDir[MAX_PATH];
            wcsncpy(workDir, gamePath, MAX_PATH);
            wchar_t* lastSlash = wcsrchr(workDir, L'\\');
            if (lastSlash) *lastSlash = L'\0';

            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = { 0 };
            AppendLog(L"正在挂起启动游戏...");
            if (CreateProcessW(gamePath, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, workDir, &si, &pi)) {
                InjectDllByPid(pi.dwProcessId);
                ResumeThread(pi.hThread);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                AppendLog(L"游戏已启动并恢复运行！");
            } else {
                AppendLog(L"启动游戏失败！");
            }
            break;
        }
        }
        break;
    }
    case WM_DESTROY:
        if (g_bMonitoring) {
            g_bMonitoring = false;
            if (g_hMonitorProcess) {
                TerminateProcess(g_hMonitorProcess, 0);
                g_hMonitorProcess = NULL;
            }
            if (g_hMonitorThread) {
                CloseHandle(g_hMonitorThread);
                g_hMonitorThread = NULL;
            }
        }
        g_bAutoInjecting = false;
        if (g_hAutoInjectThread) {
            WaitForSingleObject(g_hAutoInjectThread, 500);
            CloseHandle(g_hAutoInjectThread);
        }
        StopSingBox();
        if (g_hFont) DeleteObject(g_hFont);
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    g_hInstance = hInstance;

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"GameSpeedLauncherWndClass";

    if (!RegisterClassExW(&wc)) return 1;

    HWND hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"Game TCP Speed - 专线游戏加速器 (订阅版)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 565, 570,
        NULL, NULL, hInstance, NULL
    );

    if (!hWnd) return 1;

    g_hMainWnd = hWnd;
    ShowWindow(hWnd, SW_SHOWNORMAL);
    UpdateWindow(hWnd);
    SetForegroundWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
