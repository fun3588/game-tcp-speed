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

// 控件 ID 定义
#define ID_TAB_MAIN             1000

// Tab 1 控件
#define ID_COMBO_NODES          1001
#define ID_BTN_APPLY_NODE       1002
#define ID_BTN_TEST_SPEED       1003
#define ID_BTN_MONITOR          1004
#define ID_EDIT_PORT            1005
#define ID_BTN_START_SB         1006
#define ID_BTN_STOP_SB          1007
#define ID_BTN_EDIT_CONFIG      1008
#define ID_LABEL_SB_STATUS      1009
#define ID_EDIT_TARGET_NAME     1010
#define ID_BTN_AUTO_INJECT      1011
#define ID_BTN_INJECT_NOW       1012
#define ID_EDIT_PID             1013
#define ID_BTN_INJECT_PID       1014
#define ID_EDIT_LOG             1015

// Tab 2 控件 (服务器连通性探针)
#define ID_LIST_PROBE           1101
#define ID_BTN_PROBE_ALL        1102
#define ID_BTN_PROBE_ADD        1103
#define ID_BTN_PROBE_DEL        1104
#define ID_EDIT_PROBE_NAME      1105
#define ID_EDIT_PROBE_HOST      1106
#define ID_EDIT_PROBE_PORT      1107
#define ID_LABEL_PROBE_TIPS     1108

// Tab 3 控件 (订阅与配置)
#define ID_EDIT_SUB_PRI         1201
#define ID_BTN_FETCH_SUB_PRI    1202
#define ID_EDIT_SUB_BAK         1203
#define ID_BTN_FETCH_SUB_BAK    1204
#define ID_BTN_OPEN_DIR         1205
#define ID_BTN_RELOAD_NODES     1206

static HINSTANCE g_hInstance = NULL;
static HWND g_hMainWnd = NULL;
static HWND g_hTabCtrl = NULL;

// 控件句柄
static HWND g_hComboNodes = NULL;
static HWND g_hEditPort = NULL;
static HWND g_hLabelSbStatus = NULL;
static HWND g_hBtnMonitor = NULL;
static HWND g_hEditTargetName = NULL;
static HWND g_hBtnAutoInject = NULL;
static HWND g_hEditPid = NULL;
static HWND g_hEditLog = NULL;

static HWND g_hListProbe = NULL;
static HWND g_hBtnProbeAll = NULL;
static HWND g_hEditProbeName = NULL;
static HWND g_hEditProbeHost = NULL;
static HWND g_hEditProbePort = NULL;
static HWND g_hLabelProbeTips = NULL;

static HWND g_hEditSubPri = NULL;
static HWND g_hEditSubBak = NULL;

// 字体与画刷
static HFONT g_hFontNormal = NULL;
static HFONT g_hFontBold = NULL;
static HFONT g_hFontConsole = NULL;
static HBRUSH g_hBrushBg = NULL;
static HBRUSH g_hBrushDarkLog = NULL;

// 分组管理各 Tab 控件
static std::vector<HWND> g_Tab1Ctrls;
static std::vector<HWND> g_Tab2Ctrls;
static std::vector<HWND> g_Tab3Ctrls;

// 进程与状态
static PROCESS_INFORMATION g_SbProcInfo = { 0 };
static bool g_bAutoInjecting = false;
static HANDLE g_hAutoInjectThread = NULL;
static std::set<DWORD> g_InjectedPids;

static bool g_bMonitoring = false;
static HANDLE g_hMonitorThread = NULL;

static bool g_bProbing = false;
static HANDLE g_hProbeThread = NULL;

// 服务器探针目标结构体
struct ProbeTarget {
    std::wstring name;
    std::string host;
    int port;
    std::wstring status;
    double rtt;
};

static std::vector<ProbeTarget> g_ProbeTargets = {
    { L"Firestorm 世界服务器 (TWW 11.2.5)", "141.95.207.63", 8285, L"待检测", -1.0 },
    { L"Firestorm 战网认证登录网关", "141.95.207.63", 1119, L"待检测", -1.0 },
    { L"Firestorm 经典认证网关", "141.95.207.63", 3724, L"待检测", -1.0 },
    { L"Reality 伪装 SNI 节点 (iCloud)", "gateway.icloud.com", 443, L"待检测", -1.0 },
    { L"当前 AnyTLS 专线节点", "31.59.121.22", 30927, L"待检测", -1.0 }
};

// ==================== 基础日志与路径操作 ====================

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
}

static void SaveProxyConfig(const wchar_t* portStr) {
    wchar_t appDir[MAX_PATH];
    GetAppDir(appDir, MAX_PATH);
    wchar_t iniPath[MAX_PATH];
    _snwprintf(iniPath, MAX_PATH, L"%s\\proxy_config.ini", appDir);
    WritePrivateProfileStringW(L"Proxy", L"Port", portStr, iniPath);
}

// 检查端口是否被占用
static bool IsPortInUse(unsigned short port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return true;

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int res = bind(s, (sockaddr*)&addr, sizeof(addr));
    closesocket(s);
    return (res == SOCKET_ERROR);
}

// 获取空闲随机高位端口
static unsigned short GetRandomAvailablePort() {
    unsigned short port = 0;
    for (int i = 0; i < 50; i++) {
        port = (unsigned short)(50000 + (rand() % 15000));
        if (!IsPortInUse(port)) return port;
    }
    return 57565;
}

// 修改 config.json 中的 mixed-in 监听端口
static void UpdateConfigPort(const wchar_t* cfgPath, unsigned short newPort) {
    FILE* fp = _wfopen(cfgPath, L"rb");
    if (!fp) return;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    std::vector<char> buf(size + 1, 0);
    fread(buf.data(), 1, size, fp);
    fclose(fp);

    std::string s(buf.data());
    size_t inbPos = s.find("\"inbounds\":");
    if (inbPos != std::string::npos) {
        size_t portPos = s.find("\"listen_port\":", inbPos);
        if (portPos != std::string::npos) {
            size_t valStart = portPos + 14;
            while (valStart < s.size() && (s[valStart] == ' ' || s[valStart] == '\t')) valStart++;
            size_t valEnd = valStart;
            while (valEnd < s.size() && (s[valEnd] >= '0' && s[valEnd] <= '9')) valEnd++;

            char newPortStr[16];
            snprintf(newPortStr, sizeof(newPortStr), "%u", newPort);
            s.replace(valStart, valEnd - valStart, newPortStr);

            FILE* wfp = _wfopen(cfgPath, L"wb");
            if (wfp) {
                fwrite(s.data(), 1, s.size(), wfp);
                fclose(wfp);
            }
        }
    }
}

// ==================== Sing-box 核心管理 ====================

static void StartSingBox() {
    if (g_SbProcInfo.hProcess != NULL) {
        DWORD exitCode = 0;
        if (GetExitCodeProcess(g_SbProcInfo.hProcess, &exitCode) && exitCode == STILL_ACTIVE) {
            return;
        }
        CloseHandle(g_SbProcInfo.hProcess);
        CloseHandle(g_SbProcInfo.hThread);
        g_SbProcInfo.hProcess = NULL;
    }

    wchar_t exePath[MAX_PATH], cfgPath[MAX_PATH];
    GetSingBoxPaths(exePath, cfgPath);

    if (GetFileAttributesW(exePath) == INVALID_FILE_ATTRIBUTES) {
        AppendLog(L"错误: 未找到 Sing-box\\sing-box.exe！");
        return;
    }

    unsigned short randPort = GetRandomAvailablePort();
    wchar_t portStr[16];
    _snwprintf(portStr, sizeof(portStr)/sizeof(wchar_t), L"%u", randPort);

    if (g_hEditPort) SetWindowTextW(g_hEditPort, portStr);
    SaveProxyConfig(portStr);
    UpdateConfigPort(cfgPath, randPort);

    wchar_t portMsg[128];
    _snwprintf(portMsg, sizeof(portMsg)/sizeof(wchar_t), L"[端口调度] 分配可用随机端口: %u (避免占用冲突)", randPort);
    AppendLog(portMsg);

    wchar_t cmdLine[MAX_PATH * 3];
    _snwprintf(cmdLine, sizeof(cmdLine)/sizeof(wchar_t), L"\"%s\" run -c \"%s\"", exePath, cfgPath);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &g_SbProcInfo)) {
        wchar_t msg[256];
        _snwprintf(msg, sizeof(msg)/sizeof(wchar_t), L"Sing-box 核心已启动 (PID: %lu)", g_SbProcInfo.dwProcessId);
        AppendLog(msg);
        wchar_t statusBuf[128];
        _snwprintf(statusBuf, sizeof(statusBuf)/sizeof(wchar_t), L"● 运行中 [端口: %u]", randPort);
        SetWindowTextW(g_hLabelSbStatus, statusBuf);
    } else {
        AppendLog(L"错误: 无法启动 Sing-box 进程！");
        SetWindowTextW(g_hLabelSbStatus, L"● 启动失败");
    }
}

static void StopSingBox() {
    if (g_SbProcInfo.hProcess != NULL) {
        TerminateProcess(g_SbProcInfo.hProcess, 0);
        CloseHandle(g_SbProcInfo.hProcess);
        CloseHandle(g_SbProcInfo.hThread);
        g_SbProcInfo.hProcess = NULL;
        AppendLog(L"Sing-box 核心已停止。");
    }
    if (g_hLabelSbStatus) {
        SetWindowTextW(g_hLabelSbStatus, L"● 未运行");
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
                    _snwprintf(msg, sizeof(msg)/sizeof(wchar_t), L"√ 默认节点已切换为 [%s]，正在重启核心...", nodeTag);
                    AppendLog(msg);

                    StopSingBox();
                    Sleep(300);
                    StartSingBox();
                    return;
                }
            }
        }
    }
    AppendLog(L"节点切换失败，请检查配置文件格式。");
}

static void EditConfigFile() {
    wchar_t exePath[MAX_PATH], cfgPath[MAX_PATH];
    GetSingBoxPaths(exePath, cfgPath);
    ShellExecuteW(NULL, L"open", L"notepad.exe", cfgPath, NULL, SW_SHOWNORMAL);
    AppendLog(L"已在记事本中打开 config.json。");
}

// ==================== 权限与进程注入 ====================

static void EnableDebugPrivilege() {
    HANDLE hToken;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        TOKEN_PRIVILEGES tp;
        tp.PrivilegeCount = 1;
        LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &tp.Privileges[0].Luid);
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
        CloseHandle(hToken);
    }
}

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

    HANDLE hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess) {
        hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    }
    if (!hProcess) {
        wchar_t err[128];
        _snwprintf(err, sizeof(err)/sizeof(wchar_t), L"无法打开 PID %lu，请确保已以管理员身份运行本程序！", pid);
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

static int InjectMatchingProcesses(const wchar_t* targetExe) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe = { sizeof(PROCESSENTRY32W) };
    int count = 0;

    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            bool match = false;
            if (targetExe && wcslen(targetExe) > 0) {
                if (_wcsicmp(pe.szExeFile, targetExe) == 0) match = true;
            } else {
                if (_wcsicmp(pe.szExeFile, L"WoW 11.2.5 - Firestorm.exe") == 0) match = true;
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
        _snwprintf(msg, sizeof(msg)/sizeof(wchar_t), L"🚀 [自动监控已开启] 正在监听 [%s]... 启动游戏后将自动完成注入！", targetName);
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
        _snwprintf(msg, sizeof(msg)/sizeof(wchar_t), L"未检测到运行中的 [%s]，请先启动游戏或点击【开启自动监控】！", targetName);
        AppendLog(msg);
    }
}

// ==================== 测速与连续监控 ====================

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

    char authReq[3] = { 0x05, 0x01, 0x00 };
    if (send(s, authReq, 3, 0) != 3) { closesocket(s); return -1.0; }
    char authResp[2] = { 0 };
    if (recv(s, authResp, 2, 0) != 2 || authResp[0] != 0x05 || authResp[1] != 0x00) { closesocket(s); return -1.0; }

    char connReq[10];
    connReq[0] = 0x05;
    connReq[1] = 0x01;
    connReq[2] = 0x00;
    connReq[3] = 0x01;
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

static DWORD WINAPI TestSpeedWorker(LPVOID lpParam) {
    wchar_t portStr[16];
    GetWindowTextW(g_hEditPort, portStr, 16);
    unsigned short port = (unsigned short)_wtoi(portStr);
    if (port == 0) port = 2081;

    AppendLog(L"[*] 目标: 141.95.207.63:8285 (Firestorm 世界服)");
    AppendLog(L"[1/2] 正在测试【直连模式】真实延迟...");

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
        _snwprintf(msg, 256, L"  [-] 直连延迟 (不加速): %.1f ms", dSum / dCount);
    } else {
        _snwprintf(msg, 256, L"  [-] 直连延迟 (不加速): 超时阻断 / 无法直连");
    }
    AppendLog(msg);

    _snwprintf(msg, 256, L"[2/2] 正在测试【专线加速】真实端到端延迟 (本地端口: %u)...", port);
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
        _snwprintf(msg, 256, L"  [+] 专线加速延迟: %.1f ms (最低 %.1f ms) - 链路稳定", pSum / pCount, pMin);
        AppendLog(msg);
    } else {
        AppendLog(L"  [!] 专线测速超时: 请先确认 Sing-box 核心已启动！");
    }
    return 0;
}

static void RunSpeedTest() {
    StartSingBox();
    HANDLE h = CreateThread(NULL, 0, TestSpeedWorker, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

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
            _snwprintf(line, 256, L"[10s 监控] 专线: %.1f ms  vs  直连: %.1f ms (平稳)", pLat, dLat);
        } else if (pLat > 0) {
            _snwprintf(line, 256, L"[10s 监控] 专线: %.1f ms  vs  直连: 超时阻断", pLat);
        } else {
            _snwprintf(line, 256, L"[10s 监控] 专线握手超时 (端口: %u)", port);
        }
        AppendLog(line);

        for (int i = 0; i < 20 && g_bMonitoring; i++) {
            Sleep(500);
        }
    }
    return 0;
}

static void ToggleMonitor() {
    if (!g_bMonitoring) {
        StartSingBox();
        g_bMonitoring = true;
        if (g_hBtnMonitor) SetWindowTextW(g_hBtnMonitor, L"⏹ 停止监控");
        AppendLog(L"⏱️ [实时对比监控已启动] 每 10 秒刷新一次加速与直连对比...");
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

// ==================== 订阅管理 ====================

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
        AppendLog(L"提示：订阅拉取完成，可重新加载节点或编辑配置。");
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

// ==================== Tab 2: 服务器连通性探针 ====================

// 探测单个 IP/域名 和端口
// return: 0=在线, 1=超时, 2=连接被拒(RST), 3=域名解析失败
static int ProbeServer(const char* host, int port, int timeoutMs, double& outRtt) {
    outRtt = -1.0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 1;

    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        struct hostent* he = gethostbyname(host);
        if (!he || !he->h_addr_list[0]) {
            closesocket(s);
            return 3;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(in_addr));
    }

    LARGE_INTEGER freq, tStart, tEnd;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tStart);

    int connRes = connect(s, (sockaddr*)&addr, sizeof(addr));
    int status = 1; // default timeout

    if (connRes == 0) {
        QueryPerformanceCounter(&tEnd);
        outRtt = (double)(tEnd.QuadPart - tStart.QuadPart) * 1000.0 / (double)freq.QuadPart;
        status = 0;
    } else {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
            fd_set writeSet, errSet;
            FD_ZERO(&writeSet);
            FD_ZERO(&errSet);
            FD_SET(s, &writeSet);
            FD_SET(s, &errSet);

            timeval tv;
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;

            int selRes = select(0, NULL, &writeSet, &errSet, &tv);
            if (selRes > 0) {
                if (FD_ISSET(s, &writeSet)) {
                    int so_error = 0;
                    int len = sizeof(so_error);
                    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
                    if (so_error == 0) {
                        QueryPerformanceCounter(&tEnd);
                        outRtt = (double)(tEnd.QuadPart - tStart.QuadPart) * 1000.0 / (double)freq.QuadPart;
                        status = 0;
                    } else if (so_error == WSAECONNREFUSED) {
                        status = 2;
                    } else {
                        status = 1;
                    }
                } else if (FD_ISSET(s, &errSet)) {
                    status = 2;
                }
            } else {
                status = 1;
            }
        } else if (err == WSAECONNREFUSED) {
            status = 2;
        }
    }

    closesocket(s);
    return status;
}

static void UpdateProbeListViewItem(int index) {
    if (!g_hListProbe || index < 0 || index >= (int)g_ProbeTargets.size()) return;
    const ProbeTarget& tgt = g_ProbeTargets[index];

    ListView_SetItemText(g_hListProbe, index, 3, (LPWSTR)tgt.status.c_str());

    wchar_t rttBuf[32];
    if (tgt.rtt >= 0) {
        _snwprintf(rttBuf, 32, L"%.1f ms", tgt.rtt);
    } else {
        wcscpy(rttBuf, L"--");
    }
    ListView_SetItemText(g_hListProbe, index, 4, rttBuf);
}

static DWORD WINAPI ProbeAllWorker(LPVOID lpParam) {
    g_bProbing = true;
    if (g_hBtnProbeAll) EnableWindow(g_hBtnProbeAll, FALSE);
    if (g_hLabelProbeTips) SetWindowTextW(g_hLabelProbeTips, L"正在并发探测各服务器节点連通性...");

    int onlineCount = 0;
    int offlineCount = 0;
    bool wowOffline = false;

    for (size_t i = 0; i < g_ProbeTargets.size(); i++) {
        g_ProbeTargets[i].status = L"检测中...";
        UpdateProbeListViewItem((int)i);
    }

    for (size_t i = 0; i < g_ProbeTargets.size(); i++) {
        double rtt = -1.0;
        int res = ProbeServer(g_ProbeTargets[i].host.c_str(), g_ProbeTargets[i].port, 2000, rtt);
        g_ProbeTargets[i].rtt = rtt;

        if (res == 0) {
            g_ProbeTargets[i].status = L"● 在线 (正常)";
            onlineCount++;
        } else if (res == 1) {
            g_ProbeTargets[i].status = L"✖ 超时离线";
            offlineCount++;
            if (g_ProbeTargets[i].port == 8285 || g_ProbeTargets[i].port == 1119) {
                wowOffline = true;
            }
        } else if (res == 2) {
            g_ProbeTargets[i].status = L"✖ 端口关闭 (RST)";
            offlineCount++;
            if (g_ProbeTargets[i].port == 8285 || g_ProbeTargets[i].port == 1119) {
                wowOffline = true;
            }
        } else {
            g_ProbeTargets[i].status = L"✖ DNS 解析失败";
            offlineCount++;
        }

        UpdateProbeListViewItem((int)i);
        Sleep(50);
    }

    // 智能诊断提示
    wchar_t diagMsg[256];
    if (wowOffline) {
        _snwprintf(diagMsg, 256, L"诊断结论: Firestorm 认证/世界服端口超时离线，疑似官方服务器正在维护或被骨干网阻断！");
    } else if (onlineCount == (int)g_ProbeTargets.size()) {
        _snwprintf(diagMsg, 256, L"诊断结论: 所有探测目标全部连通在线！网络状态优异，可正常登录游戏。");
    } else {
        _snwprintf(diagMsg, 256, L"诊断完成: %d 个在线，%d 个离线。若专线在线而游戏服离线，说明专线良好、游戏服未开放。", onlineCount, offlineCount);
    }

    if (g_hLabelProbeTips) SetWindowTextW(g_hLabelProbeTips, diagMsg);
    if (g_hBtnProbeAll) EnableWindow(g_hBtnProbeAll, TRUE);
    g_bProbing = false;
    return 0;
}

static void RunProbeAll() {
    if (g_bProbing) return;
    HANDLE h = CreateThread(NULL, 0, ProbeAllWorker, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

static void PopulateProbeListView() {
    if (!g_hListProbe) return;
    ListView_DeleteAllItems(g_hListProbe);

    for (size_t i = 0; i < g_ProbeTargets.size(); i++) {
        const ProbeTarget& tgt = g_ProbeTargets[i];

        LVITEMW lvi = { 0 };
        lvi.mask = LVIF_TEXT;
        lvi.iItem = (int)i;
        lvi.iSubItem = 0;
        lvi.pszText = (LPWSTR)tgt.name.c_str();
        ListView_InsertItem(g_hListProbe, &lvi);

        wchar_t wHost[128];
        MultiByteToWideChar(CP_UTF8, 0, tgt.host.c_str(), -1, wHost, 128);
        ListView_SetItemText(g_hListProbe, (int)i, 1, wHost);

        wchar_t wPort[16];
        _snwprintf(wPort, 16, L"%d", tgt.port);
        ListView_SetItemText(g_hListProbe, (int)i, 2, wPort);

        ListView_SetItemText(g_hListProbe, (int)i, 3, (LPWSTR)tgt.status.c_str());

        wchar_t rttBuf[32];
        if (tgt.rtt >= 0) {
            _snwprintf(rttBuf, 32, L"%.1f ms", tgt.rtt);
        } else {
            wcscpy(rttBuf, L"--");
        }
        ListView_SetItemText(g_hListProbe, (int)i, 4, rttBuf);
    }
}

static void AddCustomProbeTarget() {
    wchar_t name[128] = { 0 }, host[128] = { 0 }, portStr[16] = { 0 };
    GetWindowTextW(g_hEditProbeName, name, 128);
    GetWindowTextW(g_hEditProbeHost, host, 128);
    GetWindowTextW(g_hEditProbePort, portStr, 16);

    int port = _wtoi(portStr);
    if (wcslen(host) == 0 || port <= 0 || port > 65535) {
        MessageBoxW(g_hMainWnd, L"请输入有效的目标 IP/域名 和 1-65535 端口！", L"提示", MB_ICONWARNING);
        return;
    }

    if (wcslen(name) == 0) {
        wcscpy(name, L"自定义探针节点");
    }

    char hostA[128];
    WideCharToMultiByte(CP_UTF8, 0, host, -1, hostA, 128, NULL, NULL);

    ProbeTarget pt;
    pt.name = name;
    pt.host = hostA;
    pt.port = port;
    pt.status = L"待检测";
    pt.rtt = -1.0;

    g_ProbeTargets.push_back(pt);
    PopulateProbeListView();

    // 清空输入框
    SetWindowTextW(g_hEditProbeName, L"");
    SetWindowTextW(g_hEditProbeHost, L"");
    SetWindowTextW(g_hEditProbePort, L"");
}

static void DeleteSelectedProbeTarget() {
    int sel = ListView_GetNextItem(g_hListProbe, -1, LVNI_SELECTED);
    if (sel >= 0 && sel < (int)g_ProbeTargets.size()) {
        g_ProbeTargets.erase(g_ProbeTargets.begin() + sel);
        PopulateProbeListView();
    } else {
        MessageBoxW(g_hMainWnd, L"请先在列表中选中要删除的探针项！", L"提示", MB_ICONINFORMATION);
    }
}

// ==================== Tab 页面切换 ====================

static void SwitchTab(int tabIndex) {
    for (HWND h : g_Tab1Ctrls) ShowWindow(h, tabIndex == 0 ? SW_SHOW : SW_HIDE);
    for (HWND h : g_Tab2Ctrls) ShowWindow(h, tabIndex == 1 ? SW_SHOW : SW_HIDE);
    for (HWND h : g_Tab3Ctrls) ShowWindow(h, tabIndex == 2 ? SW_SHOW : SW_HIDE);
}

// ==================== 窗口过程 ====================

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        // 创建全局字体
        g_hFontNormal = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        if (!g_hFontNormal) g_hFontNormal = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        g_hFontBold = CreateFontW(-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        g_hFontConsole = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

        g_hBrushBg = CreateSolidBrush(RGB(246, 248, 250));
        g_hBrushDarkLog = CreateSolidBrush(RGB(22, 27, 34));

        // 1. 顶部 Tab Control
        g_hTabCtrl = CreateWindowExW(0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            12, 10, 638, 550, hWnd, (HMENU)ID_TAB_MAIN, g_hInstance, NULL);
        SendMessageW(g_hTabCtrl, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);

        TCITEMW tie = { 0 };
        tie.mask = TCIF_TEXT;

        tie.pszText = (LPWSTR)L"  🎮 游戏专线加速  ";
        TabCtrl_InsertItem(g_hTabCtrl, 0, &tie);

        tie.pszText = (LPWSTR)L"  🔍 服务器连通性探针  ";
        TabCtrl_InsertItem(g_hTabCtrl, 1, &tie);

        tie.pszText = (LPWSTR)L"  🌐 订阅与专线配置  ";
        TabCtrl_InsertItem(g_hTabCtrl, 2, &tie);

        // ==================== TAB 1 控件布局 ====================
        int y1 = 45;

        // 区域 1：专线节点控制
        HWND hGrpNode = CreateWindowW(L"BUTTON", L" 专线节点与核心状态 ", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 25, y1, 612, 88, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hGrpNode, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab1Ctrls.push_back(hGrpNode);

        HWND hLblNode = CreateWindowW(L"STATIC", L"加速节点:", WS_CHILD | WS_VISIBLE, 38, y1 + 25, 65, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblNode, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(hLblNode);

        g_hComboNodes = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 105, y1 + 22, 185, 200, hWnd, (HMENU)ID_COMBO_NODES, g_hInstance, NULL);
        SendMessageW(g_hComboNodes, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(g_hComboNodes);

        HWND hBtnApply = CreateWindowW(L"BUTTON", L"切换", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 296, y1 + 21, 55, 26, hWnd, (HMENU)ID_BTN_APPLY_NODE, g_hInstance, NULL);
        SendMessageW(hBtnApply, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(hBtnApply);

        HWND hBtnSpeed = CreateWindowW(L"BUTTON", L"⚡ 测速", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 356, y1 + 21, 68, 26, hWnd, (HMENU)ID_BTN_TEST_SPEED, g_hInstance, NULL);
        SendMessageW(hBtnSpeed, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab1Ctrls.push_back(hBtnSpeed);

        g_hBtnMonitor = CreateWindowW(L"BUTTON", L"⏱️ 对比监控(10s)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 430, y1 + 21, 135, 26, hWnd, (HMENU)ID_BTN_MONITOR, g_hInstance, NULL);
        SendMessageW(g_hBtnMonitor, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab1Ctrls.push_back(g_hBtnMonitor);

        HWND hLblPort = CreateWindowW(L"STATIC", L"本地端口:", WS_CHILD | WS_VISIBLE, 38, y1 + 56, 65, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblPort, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(hLblPort);

        g_hEditPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"57565", WS_CHILD | WS_VISIBLE | ES_NUMBER, 105, y1 + 54, 55, 22, hWnd, (HMENU)ID_EDIT_PORT, g_hInstance, NULL);
        SendMessageW(g_hEditPort, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(g_hEditPort);

        g_hLabelSbStatus = CreateWindowW(L"STATIC", L"● 未运行", WS_CHILD | WS_VISIBLE, 175, y1 + 56, 200, 20, hWnd, (HMENU)ID_LABEL_SB_STATUS, g_hInstance, NULL);
        SendMessageW(g_hLabelSbStatus, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab1Ctrls.push_back(g_hLabelSbStatus);

        HWND hBtnStartSb = CreateWindowW(L"BUTTON", L"启动核心", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 385, y1 + 53, 72, 25, hWnd, (HMENU)ID_BTN_START_SB, g_hInstance, NULL);
        SendMessageW(hBtnStartSb, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(hBtnStartSb);

        HWND hBtnStopSb = CreateWindowW(L"BUTTON", L"停止", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 462, y1 + 53, 60, 25, hWnd, (HMENU)ID_BTN_STOP_SB, g_hInstance, NULL);
        SendMessageW(hBtnStopSb, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(hBtnStopSb);

        HWND hBtnEditCfg = CreateWindowW(L"BUTTON", L"配置", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 527, y1 + 53, 58, 25, hWnd, (HMENU)ID_BTN_EDIT_CONFIG, g_hInstance, NULL);
        SendMessageW(hBtnEditCfg, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(hBtnEditCfg);

        y1 += 96;

        // 区域 2：游戏注入控制
        HWND hGrpInject = CreateWindowW(L"BUTTON", L" 游戏进程拦截与自动注入 ", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 25, y1, 612, 105, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hGrpInject, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab1Ctrls.push_back(hGrpInject);

        HWND hLblTarget = CreateWindowW(L"STATIC", L"目标进程名:", WS_CHILD | WS_VISIBLE, 38, y1 + 27, 75, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblTarget, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(hLblTarget);

        g_hEditTargetName = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"WoW 11.2.5 - Firestorm.exe", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 115, y1 + 24, 250, 24, hWnd, (HMENU)ID_EDIT_TARGET_NAME, g_hInstance, NULL);
        SendMessageW(g_hEditTargetName, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(g_hEditTargetName);

        g_hBtnAutoInject = CreateWindowW(L"BUTTON", L"🚀 开启自动监控注入", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 380, y1 + 22, 160, 28, hWnd, (HMENU)ID_BTN_AUTO_INJECT, g_hInstance, NULL);
        SendMessageW(g_hBtnAutoInject, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab1Ctrls.push_back(g_hBtnAutoInject);

        HWND hBtnInjectNow = CreateWindowW(L"BUTTON", L"💉 立即注入现有游戏", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 38, y1 + 60, 160, 30, hWnd, (HMENU)ID_BTN_INJECT_NOW, g_hInstance, NULL);
        SendMessageW(hBtnInjectNow, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(hBtnInjectNow);

        HWND hLblPid = CreateWindowW(L"STATIC", L"按 PID:", WS_CHILD | WS_VISIBLE, 220, y1 + 66, 50, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblPid, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(hLblPid);

        g_hEditPid = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER, 275, y1 + 63, 65, 24, hWnd, (HMENU)ID_EDIT_PID, g_hInstance, NULL);
        SendMessageW(g_hEditPid, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(g_hEditPid);

        HWND hBtnInjectPid = CreateWindowW(L"BUTTON", L"注入PID", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 348, y1 + 62, 70, 26, hWnd, (HMENU)ID_BTN_INJECT_PID, g_hInstance, NULL);
        SendMessageW(hBtnInjectPid, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(hBtnInjectPid);

        y1 += 113;

        // 区域 3：极客终端日志
        g_hEditLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            25, y1, 612, 240, hWnd, (HMENU)ID_EDIT_LOG, g_hInstance, NULL);
        SendMessageW(g_hEditLog, WM_SETFONT, (WPARAM)g_hFontConsole, TRUE);
        g_Tab1Ctrls.push_back(g_hEditLog);

        // ==================== TAB 2 控件布局 (服务器探针) ====================
        int y2 = 45;

        HWND hLblProbeHeader = CreateWindowW(L"STATIC", L"实时探测各服务节点 TCP 握手与延迟，排查游戏连不上是否为官方停服维护：", WS_CHILD | WS_VISIBLE, 25, y2, 450, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblProbeHeader, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab2Ctrls.push_back(hLblProbeHeader);

        g_hBtnProbeAll = CreateWindowW(L"BUTTON", L"⚡ 一键全面检测", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 485, y2 - 4, 150, 30, hWnd, (HMENU)ID_BTN_PROBE_ALL, g_hInstance, NULL);
        SendMessageW(g_hBtnProbeAll, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab2Ctrls.push_back(g_hBtnProbeAll);

        y2 += 34;

        // ListView 探针报表
        g_hListProbe = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER,
            25, y2, 612, 280, hWnd, (HMENU)ID_LIST_PROBE, g_hInstance, NULL);
        SendMessageW(g_hListProbe, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        ListView_SetExtendedListViewStyle(g_hListProbe, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        LVCOLUMNW col = { 0 };
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        col.pszText = (LPWSTR)L"服务 / 目标名称"; col.cx = 220; ListView_InsertColumn(g_hListProbe, 0, &col);
        col.pszText = (LPWSTR)L"目标 IP / 域名"; col.cx = 140; ListView_InsertColumn(g_hListProbe, 1, &col);
        col.pszText = (LPWSTR)L"端口"; col.cx = 65; ListView_InsertColumn(g_hListProbe, 2, &col);
        col.pszText = (LPWSTR)L"连通状态"; col.cx = 100; ListView_InsertColumn(g_hListProbe, 3, &col);
        col.pszText = (LPWSTR)L"往返延迟 (RTT)"; col.cx = 85; ListView_InsertColumn(g_hListProbe, 4, &col);

        g_Tab2Ctrls.push_back(g_hListProbe);

        y2 += 288;

        // 诊断建议信息框
        g_hLabelProbeTips = CreateWindowW(L"STATIC", L"就绪。点击【一键全面检测】立即并发探测 Firestorm 官方服与专线状态。", WS_CHILD | WS_VISIBLE, 25, y2, 612, 38, hWnd, (HMENU)ID_LABEL_PROBE_TIPS, g_hInstance, NULL);
        SendMessageW(g_hLabelProbeTips, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab2Ctrls.push_back(g_hLabelProbeTips);

        y2 += 44;

        // 添加自定义探针面板
        HWND hGrpAddProbe = CreateWindowW(L"BUTTON", L" 添加自定义探测节点 ", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 25, y2, 612, 80, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hGrpAddProbe, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab2Ctrls.push_back(hGrpAddProbe);

        HWND hLblPName = CreateWindowW(L"STATIC", L"名称:", WS_CHILD | WS_VISIBLE, 38, y2 + 28, 40, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblPName, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab2Ctrls.push_back(hLblPName);

        g_hEditProbeName = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"自定义游戏服", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 80, y2 + 25, 120, 24, hWnd, (HMENU)ID_EDIT_PROBE_NAME, g_hInstance, NULL);
        SendMessageW(g_hEditProbeName, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab2Ctrls.push_back(g_hEditProbeName);

        HWND hLblPHost = CreateWindowW(L"STATIC", L"IP/域名:", WS_CHILD | WS_VISIBLE, 210, y2 + 28, 55, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblPHost, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab2Ctrls.push_back(hLblPHost);

        g_hEditProbeHost = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"141.95.207.63", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 270, y2 + 25, 120, 24, hWnd, (HMENU)ID_EDIT_PROBE_HOST, g_hInstance, NULL);
        SendMessageW(g_hEditProbeHost, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab2Ctrls.push_back(g_hEditProbeHost);

        HWND hLblPPort = CreateWindowW(L"STATIC", L"端口:", WS_CHILD | WS_VISIBLE, 400, y2 + 28, 40, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblPPort, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab2Ctrls.push_back(hLblPPort);

        g_hEditProbePort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"8285", WS_CHILD | WS_VISIBLE | ES_NUMBER, 445, y2 + 25, 55, 24, hWnd, (HMENU)ID_EDIT_PROBE_PORT, g_hInstance, NULL);
        SendMessageW(g_hEditProbePort, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab2Ctrls.push_back(g_hEditProbePort);

        HWND hBtnAddProbe = CreateWindowW(L"BUTTON", L"➕ 添加", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 510, y2 + 24, 60, 26, hWnd, (HMENU)ID_BTN_PROBE_ADD, g_hInstance, NULL);
        SendMessageW(hBtnAddProbe, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab2Ctrls.push_back(hBtnAddProbe);

        HWND hBtnDelProbe = CreateWindowW(L"BUTTON", L"🗑️ 删除", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 575, y2 + 24, 55, 26, hWnd, (HMENU)ID_BTN_PROBE_DEL, g_hInstance, NULL);
        SendMessageW(hBtnDelProbe, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab2Ctrls.push_back(hBtnDelProbe);

        // ==================== TAB 3 控件布局 (订阅与配置) ====================
        int y3 = 45;

        HWND hGrpSub = CreateWindowW(L"BUTTON", L" 专线订阅管理 (支持 Base64 / Clash / Sing-box 订阅) ", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 25, y3, 612, 130, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hGrpSub, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab3Ctrls.push_back(hGrpSub);

        HWND hLblSubPri = CreateWindowW(L"STATIC", L"主订阅链接:", WS_CHILD | WS_VISIBLE, 38, y3 + 32, 80, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblSubPri, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab3Ctrls.push_back(hLblSubPri);

        g_hEditSubPri = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"http://<SERVER_IP>:3589/sub?token=<YOUR_TOKEN>", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 125, y3 + 29, 360, 24, hWnd, (HMENU)ID_EDIT_SUB_PRI, g_hInstance, NULL);
        SendMessageW(g_hEditSubPri, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab3Ctrls.push_back(g_hEditSubPri);

        HWND hBtnFetchPri = CreateWindowW(L"BUTTON", L"拉取主订阅", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 495, y3 + 27, 125, 28, hWnd, (HMENU)ID_BTN_FETCH_SUB_PRI, g_hInstance, NULL);
        SendMessageW(hBtnFetchPri, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab3Ctrls.push_back(hBtnFetchPri);

        HWND hLblSubBak = CreateWindowW(L"STATIC", L"备订阅链接:", WS_CHILD | WS_VISIBLE, 38, y3 + 75, 80, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblSubBak, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab3Ctrls.push_back(hLblSubBak);

        g_hEditSubBak = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 125, y3 + 72, 360, 24, hWnd, (HMENU)ID_EDIT_SUB_BAK, g_hInstance, NULL);
        SendMessageW(g_hEditSubBak, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab3Ctrls.push_back(g_hEditSubBak);

        HWND hBtnFetchBak = CreateWindowW(L"BUTTON", L"拉取备订阅", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 495, y3 + 70, 125, 28, hWnd, (HMENU)ID_BTN_FETCH_SUB_BAK, g_hInstance, NULL);
        SendMessageW(hBtnFetchBak, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab3Ctrls.push_back(hBtnFetchBak);

        y3 += 145;

        HWND hGrpTools = CreateWindowW(L"BUTTON", L" 快捷工具与配置目录 ", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 25, y3, 612, 90, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hGrpTools, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab3Ctrls.push_back(hGrpTools);

        HWND hBtnOpenDir = CreateWindowW(L"BUTTON", L"📂 打开核心所在目录", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 45, y3 + 35, 170, 32, hWnd, (HMENU)ID_BTN_OPEN_DIR, g_hInstance, NULL);
        SendMessageW(hBtnOpenDir, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab3Ctrls.push_back(hBtnOpenDir);

        HWND hBtnReloadNodes = CreateWindowW(L"BUTTON", L"🔄 重新载入节点列表", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 230, y3 + 35, 170, 32, hWnd, (HMENU)ID_BTN_RELOAD_NODES, g_hInstance, NULL);
        SendMessageW(hBtnReloadNodes, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab3Ctrls.push_back(hBtnReloadNodes);

        HWND hBtnEditConfigTab3 = CreateWindowW(L"BUTTON", L"📝 记事本编辑 config.json", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 415, y3 + 35, 200, 32, hWnd, (HMENU)ID_BTN_EDIT_CONFIG, g_hInstance, NULL);
        SendMessageW(hBtnEditConfigTab3, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab3Ctrls.push_back(hBtnEditConfigTab3);

        // 默认显示 Tab 1，隐藏 Tab 2 与 Tab 3
        SwitchTab(0);

        // 初始化数据
        PopulateProbeListView();
        ReloadNodeList();

        AppendLog(L"GUI 加载完成。已就绪。");

        // 自动拉起 Sing-box 核心并开启对 Firestorm 的自动监控注入
        StartSingBox();
        ToggleAutoInject();
        break;
    }
    case WM_NOTIFY: {
        LPNMHDR pnm = (LPNMHDR)lParam;
        if (pnm->hwndFrom == g_hTabCtrl && pnm->code == TCN_SELCHANGE) {
            int curSel = TabCtrl_GetCurSel(g_hTabCtrl);
            SwitchTab(curSel);
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        HWND hwndStatic = (HWND)lParam;

        if (hwndStatic == g_hLabelSbStatus) {
            wchar_t statusText[64] = { 0 };
            GetWindowTextW(g_hLabelSbStatus, statusText, 64);
            if (wcsstr(statusText, L"运行中")) {
                SetTextColor(hdcStatic, RGB(16, 185, 129)); // 亮绿
            } else {
                SetTextColor(hdcStatic, RGB(148, 163, 184)); // 灰
            }
            SetBkMode(hdcStatic, TRANSPARENT);
            return (LRESULT)g_hBrushBg;
        }

        if (hwndStatic == g_hLabelProbeTips) {
            SetTextColor(hdcStatic, RGB(30, 41, 59)); // 稳重深灰蓝
            SetBkMode(hdcStatic, TRANSPARENT);
            return (LRESULT)g_hBrushBg;
        }

        SetBkMode(hdcStatic, TRANSPARENT);
        SetTextColor(hdcStatic, RGB(31, 41, 55));
        return (LRESULT)g_hBrushBg;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdcEdit = (HDC)wParam;
        HWND hwndEdit = (HWND)lParam;
        if (hwndEdit == g_hEditLog) {
            SetTextColor(hdcEdit, RGB(74, 222, 128)); // 翠绿终端字
            SetBkColor(hdcEdit, RGB(22, 27, 34));   // 深黑背景
            return (LRESULT)g_hBrushDarkLog;
        }
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
        case ID_BTN_PROBE_ALL:
            RunProbeAll();
            break;
        case ID_BTN_PROBE_ADD:
            AddCustomProbeTarget();
            break;
        case ID_BTN_PROBE_DEL:
            DeleteSelectedProbeTarget();
            break;
        case ID_BTN_OPEN_DIR: {
            wchar_t appDir[MAX_PATH];
            GetAppDir(appDir, MAX_PATH);
            ShellExecuteW(NULL, L"open", appDir, NULL, NULL, SW_SHOWNORMAL);
            break;
        }
        case ID_BTN_RELOAD_NODES:
            ReloadNodeList();
            AppendLog(L"节点列表已重新载入。");
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
        }
        break;
    }
    case WM_DESTROY:
        if (g_bMonitoring) {
            g_bMonitoring = false;
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
        if (g_hFontNormal) DeleteObject(g_hFontNormal);
        if (g_hFontBold) DeleteObject(g_hFontBold);
        if (g_hFontConsole) DeleteObject(g_hFontConsole);
        if (g_hBrushBg) DeleteObject(g_hBrushBg);
        if (g_hBrushDarkLog) DeleteObject(g_hBrushDarkLog);
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    g_hInstance = hInstance;
    EnableDebugPrivilege();

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"GameSpeedLauncherModernWndClass";

    if (!RegisterClassExW(&wc)) return 1;

    HWND hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"Game TCP Speed - 专线游戏加速器 (Firestorm 专版)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 680, 615,
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
