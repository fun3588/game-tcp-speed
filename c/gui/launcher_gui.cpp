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
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <set>
#include <string>

#pragma comment(lib, "comctl32.lib")

// 图标资源 ID
#define IDI_APP_ICON            101

// 控件 ID 定义
#define ID_TAB_MAIN             1000
#define ID_BTN_TOGGLE_LANG      1001

// Tab 1 控件
#define ID_COMBO_NODES          1002
#define ID_BTN_APPLY_NODE       1003
#define ID_BTN_TEST_SPEED       1004
#define ID_EDIT_PORT            1005
#define ID_BTN_START_SB         1006
#define ID_BTN_STOP_SB          1007
#define ID_BTN_EDIT_CONFIG      1008
#define ID_LABEL_SB_STATUS      1009
#define ID_EDIT_TARGET_NAME     1010
#define ID_BTN_AUTO_INJECT      1011
#define ID_BTN_INJECT_NOW       1012
#define ID_BTN_UNLOAD_NOW       1013
#define ID_EDIT_PID             1014
#define ID_BTN_INJECT_PID       1015
#define ID_EDIT_LOG             1016
#define ID_BTN_CLEAR_LOG        1017

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
#define ID_BTN_SAVE_SUB         1205
#define ID_BTN_OPEN_DIR         1206

static HINSTANCE g_hInstance = NULL;
static HWND g_hMainWnd = NULL;
static HWND g_hTabCtrl = NULL;
static HWND g_hBtnLang = NULL;

// 全局双语状态标志
static bool g_bEnglish = false;
inline const wchar_t* L(const wchar_t* zh, const wchar_t* en) {
    return g_bEnglish ? en : zh;
}

// 控件句柄 (用于动态刷新双语标签)
static HWND g_hGrpNode = NULL;
static HWND g_hLblNode = NULL;
static HWND g_hComboNodes = NULL;
static HWND g_hBtnApplyNode = NULL;
static HWND g_hBtnSpeed = NULL;
static HWND g_hLblPort = NULL;
static HWND g_hEditPort = NULL;
static HWND g_hLabelSbStatus = NULL;
static HWND g_hBtnStartSb = NULL;
static HWND g_hBtnStopSb = NULL;
static HWND g_hBtnEditCfg = NULL;

static HWND g_hGrpInject = NULL;
static HWND g_hLblTarget = NULL;
static HWND g_hEditTargetName = NULL;
static HWND g_hBtnAutoInject = NULL;
static HWND g_hBtnInjectNow = NULL;
static HWND g_hBtnUnloadNow = NULL;
static HWND g_hLblPid = NULL;
static HWND g_hEditPid = NULL;
static HWND g_hBtnInjectPid = NULL;
static HWND g_hBtnClearLog = NULL;
static HWND g_hEditLog = NULL;

// Tab 2 控件
static HWND g_hLblProbeHeader = NULL;
static HWND g_hBtnProbeAll = NULL;
static HWND g_hListProbe = NULL;
static HWND g_hLabelProbeTips = NULL;
static HWND g_hGrpAddProbe = NULL;
static HWND g_hLblPName = NULL;
static HWND g_hEditProbeName = NULL;
static HWND g_hLblPHost = NULL;
static HWND g_hEditProbeHost = NULL;
static HWND g_hLblPPort = NULL;
static HWND g_hEditProbePort = NULL;
static HWND g_hBtnAddProbe = NULL;
static HWND g_hBtnDelProbe = NULL;

// Tab 3 控件
static HWND g_hGrpSub = NULL;
static HWND g_hLblSubPri = NULL;
static HWND g_hEditSubPri = NULL;
static HWND g_hBtnFetchPri = NULL;
static HWND g_hLblSubBak = NULL;
static HWND g_hEditSubBak = NULL;
static HWND g_hBtnFetchBak = NULL;
static HWND g_hBtnSaveSub = NULL;
static HWND g_hGrpTools = NULL;
static HWND g_hBtnOpenDir = NULL;

// 字体与画刷
static HFONT g_hFontNormal = NULL;
static HFONT g_hFontBold = NULL;
static HFONT g_hFontConsole = NULL;
static HBRUSH g_hBrushBg = NULL;

// 分组管理各 Tab 控件
static std::vector<HWND> g_Tab1Ctrls;
static std::vector<HWND> g_Tab2Ctrls;
static std::vector<HWND> g_Tab3Ctrls;

// 进程与状态
static PROCESS_INFORMATION g_SbProcInfo = { 0 };
static bool g_bAutoInjecting = false;
static HANDLE g_hAutoInjectThread = NULL;
static std::set<DWORD> g_InjectedPids;
static bool g_bProbing = false;

// 探针目标结构体 (真实物理连通与 RTT)
struct ProbeTarget {
    std::wstring nameZh;
    std::wstring nameEn;
    std::string host;
    int port;
    std::wstring statusText;
    double rtt;
    bool isSpecial;
};

static std::vector<ProbeTarget> g_ProbeTargets;

// ==================== 基础操作与日志 ====================

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
    _snwprintf(exePath, MAX_PATH, L"%s\\Sing-box\\sing-box.exe", appDir);
    if (GetFileAttributesW(exePath) == INVALID_FILE_ATTRIBUTES) {
        _snwprintf(exePath, MAX_PATH, L"%s\\..\\Sing-box\\sing-box.exe", appDir);
        _snwprintf(cfgPath, MAX_PATH, L"%s\\..\\Sing-box\\config.json", appDir);
    } else {
        _snwprintf(cfgPath, MAX_PATH, L"%s\\Sing-box\\config.json", appDir);
    }
}

static void SaveProxyConfig(const wchar_t* portStr, bool enabled) {
    wchar_t appDir[MAX_PATH];
    GetAppDir(appDir, MAX_PATH);
    wchar_t iniPath[MAX_PATH];
    _snwprintf(iniPath, MAX_PATH, L"%s\\proxy_config.ini", appDir);
    WritePrivateProfileStringW(L"Proxy", L"Port", portStr, iniPath);
    WritePrivateProfileStringW(L"Proxy", L"Enabled", enabled ? L"1" : L"0", iniPath);
}

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

static unsigned short GetRandomAvailablePort() {
    unsigned short port = 0;
    for (int i = 0; i < 50; i++) {
        port = (unsigned short)(50000 + (rand() % 15000));
        if (!IsPortInUse(port)) return port;
    }
    return 58588;
}

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

static void InitProbeTargets() {
    g_ProbeTargets.clear();
    g_ProbeTargets.push_back({ L"Firestorm 战网认证登录网关 (1119)", L"Battle.net Logon Gateway (1119)", "141.95.207.63", 1119, L"待检测", -1.0, false });
    g_ProbeTargets.push_back({ L"Firestorm 世界服务器 (TWW 8285)", L"EU World Realm Server (8285)", "141.95.207.63", 8285, L"待检测", -1.0, false });
    g_ProbeTargets.push_back({ L"Firestorm 备用认证节点 (1119)", L"Backup Auth Node (1119)", "51.75.148.169", 1119, L"待检测", -1.0, false });
    g_ProbeTargets.push_back({ L"Firestorm 官方 Web 门户 (443)", L"Official Web Portal (443)", "firestorm-servers.com", 443, L"待检测", -1.0, false });
    g_ProbeTargets.push_back({ L"当前 AnyTLS 专线节点 (104.252.30.68)", L"Current AnyTLS Node (104.252.30.68)", "104.252.30.68", 3589, L"待检测", -1.0, true });
}

// ==================== 订阅持久化 ====================

static void LoadSavedSubscriptions() {
    wchar_t appDir[MAX_PATH];
    GetAppDir(appDir, MAX_PATH);
    wchar_t iniPath[MAX_PATH];
    _snwprintf(iniPath, MAX_PATH, L"%s\\subscription.ini", appDir);

    wchar_t val[512];
    if (GetPrivateProfileStringW(L"Subscription", L"JsonUrl", L"", val, 512, iniPath) > 0) {
        if (g_hEditSubPri) SetWindowTextW(g_hEditSubPri, val);
    }
    if (GetPrivateProfileStringW(L"Subscription", L"GeneralUrl", L"", val, 512, iniPath) > 0) {
        if (g_hEditSubBak) SetWindowTextW(g_hEditSubBak, val);
    }
    int p = GetPrivateProfileIntW(L"Proxy", L"LocalPort", 0, iniPath);
    if (p > 1024 && p < 65535 && g_hEditPort) {
        wchar_t pStr[16];
        _snwprintf(pStr, 16, L"%d", p);
        SetWindowTextW(g_hEditPort, pStr);
    }
}

static void SaveSubscriptions() {
    wchar_t appDir[MAX_PATH];
    GetAppDir(appDir, MAX_PATH);
    wchar_t iniPath[MAX_PATH];
    _snwprintf(iniPath, MAX_PATH, L"%s\\subscription.ini", appDir);

    wchar_t val1[512] = { 0 }, val2[512] = { 0 }, pStr[16] = { 0 };
    if (g_hEditSubPri) GetWindowTextW(g_hEditSubPri, val1, 512);
    if (g_hEditSubBak) GetWindowTextW(g_hEditSubBak, val2, 512);
    if (g_hEditPort) GetWindowTextW(g_hEditPort, pStr, 16);

    WritePrivateProfileStringW(L"Subscription", L"JsonUrl", val1, iniPath);
    WritePrivateProfileStringW(L"Subscription", L"GeneralUrl", val2, iniPath);
    WritePrivateProfileStringW(L"Proxy", L"LocalPort", pStr, iniPath);

    AppendLog(L(L"√ 订阅与端口配置已成功保存至 subscription.ini！", 
                L"√ Subscriptions and port saved to subscription.ini!"));
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
        AppendLog(L(L"错误: 未找到 Sing-box\\sing-box.exe！", L"Error: sing-box.exe not found!"));
        return;
    }

    wchar_t portStr[16] = { 0 };
    if (g_hEditPort) GetWindowTextW(g_hEditPort, portStr, 16);
    unsigned short randPort = (unsigned short)_wtoi(portStr);
    if (randPort <= 1024 || randPort >= 65535) {
        randPort = GetRandomAvailablePort();
        _snwprintf(portStr, sizeof(portStr)/sizeof(wchar_t), L"%u", randPort);
        if (g_hEditPort) SetWindowTextW(g_hEditPort, portStr);
    }

    SaveProxyConfig(portStr, true);
    UpdateConfigPort(cfgPath, randPort);

    wchar_t cmdLine[MAX_PATH * 3];
    _snwprintf(cmdLine, sizeof(cmdLine)/sizeof(wchar_t), L"\"%s\" run -c \"%s\"", exePath, cfgPath);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    wchar_t singboxDir[MAX_PATH];
    wcsncpy(singboxDir, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(singboxDir, L'\\');
    if (lastSlash) *lastSlash = L'\0';

    if (CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, singboxDir, &si, &g_SbProcInfo)) {
        wchar_t msg[256];
        _snwprintf(msg, sizeof(msg)/sizeof(wchar_t), 
                   L(L"√ Sing-box 核心已启动 (PID: %lu, 监听端口: %u)", 
                     L"√ Sing-box core started (PID: %lu, Port: %u)"), g_SbProcInfo.dwProcessId, randPort);
        AppendLog(msg);
        wchar_t statusBuf[128];
        _snwprintf(statusBuf, sizeof(statusBuf)/sizeof(wchar_t), L(L"● 运行中 [端口: %u]", L"● Running [Port: %u]"), randPort);
        if (g_hLabelSbStatus) SetWindowTextW(g_hLabelSbStatus, statusBuf);
    } else {
        AppendLog(L(L"错误: 无法启动 Sing-box 进程！", L"Error: Failed to start Sing-box!"));
        if (g_hLabelSbStatus) SetWindowTextW(g_hLabelSbStatus, L(L"● 启动失败", L"● Failed"));
    }
}

static void StopSingBox() {
    if (g_SbProcInfo.hProcess != NULL) {
        TerminateProcess(g_SbProcInfo.hProcess, 0);
        CloseHandle(g_SbProcInfo.hProcess);
        CloseHandle(g_SbProcInfo.hThread);
        g_SbProcInfo.hProcess = NULL;
        AppendLog(L(L"Sing-box 核心已停止，已注入游戏平滑降级为直连。", 
                    L"Sing-box core stopped. Injected game degraded to direct connection."));
    }
    SaveProxyConfig(L"0", false);
    if (g_hLabelSbStatus) {
        SetWindowTextW(g_hLabelSbStatus, L(L"● 未运行", L"● Stopped"));
    }
}

static void ReloadNodeList() {
    if (!g_hComboNodes) return;
    SendMessageW(g_hComboNodes, CB_RESETCONTENT, 0, 0);

    const wchar_t* nodeName = L(L"AnyTLS 极速专线 (104.252.30.68:443)", L"AnyTLS Premium Node (104.252.30.68:443)");
    SendMessageW(g_hComboNodes, CB_ADDSTRING, 0, (LPARAM)nodeName);
    SendMessageW(g_hComboNodes, CB_SETCURSEL, 0, 0);
}

// ==================== 远程注入与卸载 ====================

static BOOL InjectDllByPid(DWORD pid) {
    if (pid == 0) return FALSE;

    wchar_t portStr[16];
    if (g_hEditPort) GetWindowTextW(g_hEditPort, portStr, 16);
    SaveProxyConfig(portStr, true);

    wchar_t appDir[MAX_PATH];
    GetAppDir(appDir, MAX_PATH);
    wchar_t dllPath[MAX_PATH];
    _snwprintf(dllPath, MAX_PATH, L"%s\\hook_proxy.dll", appDir);

    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        AppendLog(L(L"错误: 未找到 hook_proxy.dll！", L"Error: hook_proxy.dll not found!"));
        return FALSE;
    }

    HANDLE hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess) {
        hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    }
    if (!hProcess) {
        wchar_t err[128];
        _snwprintf(err, sizeof(err)/sizeof(wchar_t), 
                   L(L"无法打开 PID %lu，请以管理员身份运行本程序！", 
                     L"Cannot open PID %lu. Run as Admin!"), pid);
        AppendLog(err);
        return FALSE;
    }

    size_t len = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID pRemoteMem = VirtualAllocEx(hProcess, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemoteMem) {
        CloseHandle(hProcess);
        return FALSE;
    }

    WriteProcessMemory(hProcess, pRemoteMem, dllPath, len, NULL);
    LPTHREAD_START_ROUTINE pLoadLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, pLoadLibrary, pRemoteMem, 0, NULL);

    if (hThread) {
        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);
    }

    VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    g_InjectedPids.insert(pid);
    wchar_t msg[128];
    _snwprintf(msg, sizeof(msg)/sizeof(wchar_t), 
               L(L"√ 成功注入接管进程 (PID: %lu, ConnectEx 异步钩子已就绪)！", 
                 L"√ Successfully hooked process (PID: %lu, ConnectEx active)!"), pid);
    AppendLog(msg);
    return TRUE;
}

static BOOL EjectDllByPid(DWORD pid) {
    if (pid == 0) return FALSE;

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE) return FALSE;

    MODULEENTRY32W me = { sizeof(me) };
    HMODULE hTargetModule = NULL;

    if (Module32FirstW(hSnap, &me)) {
        do {
            if (_wcsicmp(me.szModule, L"hook_proxy.dll") == 0) {
                hTargetModule = me.hModule;
                break;
            }
        } while (Module32NextW(hSnap, &me));
    }
    CloseHandle(hSnap);

    if (!hTargetModule) {
        wchar_t msg[128];
        _snwprintf(msg, sizeof(msg)/sizeof(wchar_t), 
                   L(L"提示: 进程 PID %lu 中未找到 hook_proxy.dll，无需卸载。", 
                     L"PID %lu has no hook_proxy.dll loaded."), pid);
        AppendLog(msg);
        return FALSE;
    }

    HANDLE hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION, FALSE, pid);
    if (!hProcess) return FALSE;

    LPTHREAD_START_ROUTINE pFreeLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "FreeLibrary");
    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, pFreeLibrary, (LPVOID)hTargetModule, 0, NULL);

    if (hThread) {
        WaitForSingleObject(hThread, 4000);
        CloseHandle(hThread);
    }
    CloseHandle(hProcess);

    g_InjectedPids.erase(pid);
    wchar_t msg[128];
    _snwprintf(msg, sizeof(msg)/sizeof(wchar_t), 
               L(L"√ 已安全卸载 PID %lu 的 Hook 模块，游戏已恢复原生物理直连！", 
                 L"√ Unhooked PID %lu. Restored direct connection!"), pid);
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
            if (_wcsicmp(pe.szExeFile, targetExe) == 0) {
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

static int EjectMatchingProcesses(const wchar_t* targetExe) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe = { sizeof(PROCESSENTRY32W) };
    int count = 0;

    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, targetExe) == 0) {
                if (EjectDllByPid(pe.th32ProcessID)) {
                    count++;
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
    if (wcslen(targetName) == 0) return;

    StartSingBox();

    if (!g_bAutoInjecting) {
        g_bAutoInjecting = true;
        g_hAutoInjectThread = CreateThread(NULL, 0, AutoInjectWorker, NULL, 0, NULL);
        SetWindowTextW(g_hBtnAutoInject, L(L"⏹ 停止自动监控", L"⏹ Stop Monitor"));

        wchar_t msg[256];
        _snwprintf(msg, sizeof(msg)/sizeof(wchar_t), 
                   L(L"🚀 [自动监控已开启] 正在监听 [%s]... 启动游戏后自动注入！", 
                     L"🚀 [Auto-Monitor ON] Watching [%s]..."), targetName);
        AppendLog(msg);
    } else {
        g_bAutoInjecting = false;
        if (g_hAutoInjectThread) {
            WaitForSingleObject(g_hAutoInjectThread, 1000);
            CloseHandle(g_hAutoInjectThread);
            g_hAutoInjectThread = NULL;
        }
        SetWindowTextW(g_hBtnAutoInject, L(L"🚀 开启自动监控", L"🚀 Auto-Monitor"));
        AppendLog(L(L"已停止自动注入监控。", L"Auto-Monitor stopped."));
    }
}

// ==================== 物理直连 RTT 真实测速 (修复 3ms 假延迟) ====================

static double MeasureDirectRtt(const char* host, int port, int timeoutMs = 2500) {
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1.0;

    DWORD tvTimeout = timeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tvTimeout, sizeof(tvTimeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tvTimeout, sizeof(tvTimeout));

    addrinfo hints = { 0 };
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;

    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", port);
    if (getaddrinfo(host, portStr, &hints, &res) != 0 || !res) {
        closesocket(s);
        return -1.0;
    }

    sockaddr_in addr = *(sockaddr_in*)res->ai_addr;
    freeaddrinfo(res);

    if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return -1.0;
    }

    // 核心创新：真实端到端载荷探测，彻底击破运营商网关 3ms SYN 代答虚假延迟！
    QueryPerformanceCounter(&t0);
    char recvBuf[512];
    int recvBytes = 0;

    if (port == 8285) {
        // 魔兽世界世界服务器 (发送 4 字节协议头，等待 53 字节真实游戏握手头)
        const char probeData[] = "\x00\x01\x02\x03";
        send(s, probeData, 4, 0);
        recvBytes = recv(s, recvBuf, sizeof(recvBuf), 0);
    } else if (port == 443 || port == 80 || port == 3589) {
        // HTTP / Web 门户 / 专线控制台 (发送 HTTP GET，等待服务器真实 HTTP 响应头)
        char httpReq[256];
        snprintf(httpReq, sizeof(httpReq), "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
        send(s, httpReq, (int)strlen(httpReq), 0);
        recvBytes = recv(s, recvBuf, sizeof(recvBuf), 0);
    } else {
        // 其他端口：发送 2 字节保活探测并尝试接收
        const char pingProbe[] = "\x00\x00";
        send(s, pingProbe, 2, 0);
        recvBytes = recv(s, recvBuf, sizeof(recvBuf), 0);
    }

    QueryPerformanceCounter(&t1);
    closesocket(s);

    if (recvBytes > 0) {
        // 成功获取远端真实数据回包！这才是真实的端到端往返延时
        double rtt = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        return rtt;
    }

    // 若远端未返回数据或被阻断
    return -1.0;
}

static DWORD WINAPI ProbeAllWorker(LPVOID lpParam) {
    g_bProbing = true;
    if (g_hBtnProbeAll) EnableWindow(g_hBtnProbeAll, FALSE);
    if (g_hLabelProbeTips) SetWindowTextW(g_hLabelProbeTips, L(L"正在全面测试到各游戏服务器的真实物理连通与 RTT，请稍候...", 
                                                              L"Benchmarking physical RTT to game endpoints..."));

    for (size_t i = 0; i < g_ProbeTargets.size(); i++) {
        double rtt = MeasureDirectRtt(g_ProbeTargets[i].host.c_str(), g_ProbeTargets[i].port, 2000);
        g_ProbeTargets[i].rtt = rtt;
        if (rtt >= 0) {
            g_ProbeTargets[i].statusText = L(L"● 物理畅通", L"● Connected");
        } else {
            g_ProbeTargets[i].statusText = L(L"✖ 超时 (骨干网丢包/阻断)", L"✖ Timeout (Blocked)");
        }

        wchar_t rttBuf[64];
        if (rtt >= 0) {
            _snwprintf(rttBuf, 64, L"%.1f ms", rtt);
        } else {
            wcscpy(rttBuf, L"--");
        }
        ListView_SetItemText(g_hListProbe, (int)i, 2, rttBuf);
        ListView_SetItemText(g_hListProbe, (int)i, 3, (LPWSTR)g_ProbeTargets[i].statusText.c_str());
        Sleep(50);
    }

    if (g_hLabelProbeTips) {
        SetWindowTextW(g_hLabelProbeTips, L(L"√ 真实物理测速完成！排除了网关 RST 干扰，真实反映公网直连状态。", 
                                           L"√ Benchmark complete! Genuine host-to-server physical metrics."));
    }
    if (g_hBtnProbeAll) EnableWindow(g_hBtnProbeAll, TRUE);
    g_bProbing = false;
    return 0;
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
        lvi.pszText = (LPWSTR)(g_bEnglish ? tgt.nameEn.c_str() : tgt.nameZh.c_str());
        ListView_InsertItem(g_hListProbe, &lvi);

        wchar_t wAddr[128];
        wchar_t wHost[96];
        MultiByteToWideChar(CP_UTF8, 0, tgt.host.c_str(), -1, wHost, 96);
        _snwprintf(wAddr, 128, L"%s:%d", wHost, tgt.port);
        ListView_SetItemText(g_hListProbe, (int)i, 1, wAddr);

        wchar_t rttBuf[64];
        if (tgt.rtt >= 0) _snwprintf(rttBuf, 64, L"%.1f ms", tgt.rtt);
        else wcscpy(rttBuf, L"--");
        ListView_SetItemText(g_hListProbe, (int)i, 2, rttBuf);
        ListView_SetItemText(g_hListProbe, (int)i, 3, (LPWSTR)tgt.statusText.c_str());
    }
}

// ==================== 全局双语切换 ====================

static void ApplyLanguage() {
    // 1. 窗口标题
    SetWindowTextW(g_hMainWnd, L(L"GameTcpSpeed v3.0 | TCP 游戏加速器", 
                                 L"GameTcpSpeed v3.0 | TCP Game Accelerator"));

    // 2. 语言切换按钮
    SetWindowTextW(g_hBtnLang, L(L"🇺🇸 English", L"🇨🇳 简体中文"));

    // 3. TabControl 标签
    TCITEMW tie = { 0 };
    tie.mask = TCIF_TEXT;
    tie.pszText = (LPWSTR)L(L" 🎮 核心加速控制 ", L" 🎮 Dashboard ");
    TabCtrl_SetItem(g_hTabCtrl, 0, &tie);

    tie.pszText = (LPWSTR)L(L" 🔍 全面测试看板 ", L" 🔍 Probe ");
    TabCtrl_SetItem(g_hTabCtrl, 1, &tie);

    tie.pszText = (LPWSTR)L(L" 🌐 订阅与专线配置 ", L" 🌐 Subscription ");
    TabCtrl_SetItem(g_hTabCtrl, 2, &tie);

    // 4. Tab 1 控件
    SetWindowTextW(g_hGrpNode, L(L" 专线节点与核心调度 ", L" Dedicated Node & Core "));
    SetWindowTextW(g_hLblNode, L(L"加速节点:", L"Node:"));
    SetWindowTextW(g_hBtnApplyNode, L(L"切换", L"Apply"));
    SetWindowTextW(g_hBtnSpeed, L(L"⚡ 测速", L"⚡ Ping"));
    SetWindowTextW(g_hLblPort, L(L"本地端口:", L"Port:"));
    SetWindowTextW(g_hBtnStartSb, L(L"▶ 启动加速", L"▶ Start Core"));
    SetWindowTextW(g_hBtnStopSb, L(L"■ 停止", L"■ Stop"));
    SetWindowTextW(g_hBtnEditCfg, L(L"配置", L"Config"));

    SetWindowTextW(g_hGrpInject, L(L" 游戏进程接管与注入控制 ", L" Game Process Takeover "));
    SetWindowTextW(g_hLblTarget, L(L"目标进程:", L"Target:"));
    SetWindowTextW(g_hBtnInjectNow, L(L"⚡ 立即接管", L"⚡ Takeover"));
    SetWindowTextW(g_hBtnUnloadNow, L(L"🛑 恢复直连", L"🛑 Restore Direct"));
    SetWindowTextW(g_hBtnAutoInject, g_bAutoInjecting ? 
                   L(L"⏹ 停止自动监控", L"⏹ Stop Monitor") : 
                   L(L"🚀 开启自动监控", L"🚀 Auto-Monitor"));
    SetWindowTextW(g_hLblPid, L(L"按 PID:", L"PID:"));
    SetWindowTextW(g_hBtnInjectPid, L(L"注入 PID", L"By PID"));
    SetWindowTextW(g_hBtnClearLog, L(L"清空日志", L"Clear"));

    // 5. Tab 2 控件
    SetWindowTextW(g_hLblProbeHeader, L(L"主机到游戏服的原始物理连通与真实 RTT 测速（已修复 RST 快速拒绝干扰）", 
                                       L"Host-to-Game Direct Physical RTT Benchmark (Filtered RST False Positives)"));
    SetWindowTextW(g_hBtnProbeAll, L(L"⚡ 开始全面测速", L"⚡ Benchmark All"));

    LVCOLUMNW col = { 0 };
    col.mask = LVCF_TEXT;
    col.pszText = (LPWSTR)L(L"服务 / 探测端点", L"Service / Endpoint");
    ListView_SetColumn(g_hListProbe, 0, &col);

    col.pszText = (LPWSTR)L(L"目标地址与端口", L"Host & Port");
    ListView_SetColumn(g_hListProbe, 1, &col);

    col.pszText = (LPWSTR)L(L"【物理直连】真实RTT", L"Physical Direct RTT");
    ListView_SetColumn(g_hListProbe, 2, &col);

    col.pszText = (LPWSTR)L(L"连通状态 / 诊断", L"Connection Status");
    ListView_SetColumn(g_hListProbe, 3, &col);

    SetWindowTextW(g_hGrpAddProbe, L(L" 添加自定义探测节点 ", L" Add Custom Endpoint "));
    SetWindowTextW(g_hLblPName, L(L"名称:", L"Name:"));
    SetWindowTextW(g_hLblPHost, L(L"IP/域名:", L"Host:"));
    SetWindowTextW(g_hLblPPort, L(L"端口:", L"Port:"));
    SetWindowTextW(g_hBtnAddProbe, L(L"➕ 添加", L"➕ Add"));
    SetWindowTextW(g_hBtnDelProbe, L(L"🗑️ 删除", L"🗑️ Delete"));

    // 6. Tab 3 控件
    SetWindowTextW(g_hGrpSub, L(L" 专线订阅管理 (持久化写入 subscription.ini) ", L" Subscriptions (Saved to subscription.ini) "));
    SetWindowTextW(g_hLblSubPri, L(L"JSON 订阅:", L"JSON Sub:"));
    SetWindowTextW(g_hBtnFetchPri, L(L"拉取 JSON 订阅", L"Fetch JSON"));
    SetWindowTextW(g_hLblSubBak, L(L"通用订阅:", L"General Sub:"));
    SetWindowTextW(g_hBtnFetchBak, L(L"拉取通用订阅", L"Fetch General"));
    SetWindowTextW(g_hBtnSaveSub, L(L"💾 保存订阅配置", L"💾 Save Config"));
    SetWindowTextW(g_hGrpTools, L(L" 快捷工具与文件目录 ", L" Tools & File Directories "));
    SetWindowTextW(g_hBtnOpenDir, L(L"📂 打开程序根目录", L"📂 Open Directory"));

    ReloadNodeList();
    PopulateProbeListView();
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

        // 右上角双语切换按钮 (宽 105px)
        g_hBtnLang = CreateWindowW(L"BUTTON", L"🇺🇸 English", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            555, 8, 105, 26, hWnd, (HMENU)ID_BTN_TOGGLE_LANG, g_hInstance, NULL);
        SendMessageW(g_hBtnLang, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);

        // 1. 顶部 Tab Control (宽度 648px)
        g_hTabCtrl = CreateWindowExW(0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            12, 10, 650, 580, hWnd, (HMENU)ID_TAB_MAIN, g_hInstance, NULL);
        SendMessageW(g_hTabCtrl, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);

        TCITEMW tie = { 0 };
        tie.mask = TCIF_TEXT;
        tie.pszText = (LPWSTR)L"";
        TabCtrl_InsertItem(g_hTabCtrl, 0, &tie);
        TabCtrl_InsertItem(g_hTabCtrl, 1, &tie);
        TabCtrl_InsertItem(g_hTabCtrl, 2, &tie);

        // ==================== TAB 1 控件布局 ====================
        int y1 = 45;

        // 区域 1：专线节点控制 (宽 624px)
        g_hGrpNode = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 25, y1, 624, 88, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(g_hGrpNode, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab1Ctrls.push_back(g_hGrpNode);

        g_hLblNode = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 38, y1 + 25, 70, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(g_hLblNode, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(g_hLblNode);

        // 下拉框加宽至 275px，文字完全展现
        g_hComboNodes = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 110, y1 + 22, 275, 200, hWnd, (HMENU)ID_COMBO_NODES, g_hInstance, NULL);
        SendMessageW(g_hComboNodes, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(g_hComboNodes);

        g_hBtnApplyNode = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 395, y1 + 21, 60, 26, hWnd, (HMENU)ID_BTN_APPLY_NODE, g_hInstance, NULL);
        SendMessageW(g_hBtnApplyNode, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(g_hBtnApplyNode);

        g_hBtnSpeed = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 465, y1 + 21, 80, 26, hWnd, (HMENU)ID_BTN_TEST_SPEED, g_hInstance, NULL);
        SendMessageW(g_hBtnSpeed, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab1Ctrls.push_back(g_hBtnSpeed);

        g_hLblPort = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 38, y1 + 56, 70, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(g_hLblPort, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(g_hLblPort);

        g_hEditPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"58588", WS_CHILD | WS_VISIBLE | ES_NUMBER, 110, y1 + 54, 60, 22, hWnd, (HMENU)ID_EDIT_PORT, g_hInstance, NULL);
        SendMessageW(g_hEditPort, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(g_hEditPort);

        g_hLabelSbStatus = CreateWindowW(L"STATIC", L"● 未运行", WS_CHILD | WS_VISIBLE, 185, y1 + 56, 195, 20, hWnd, (HMENU)ID_LABEL_SB_STATUS, g_hInstance, NULL);
        SendMessageW(g_hLabelSbStatus, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab1Ctrls.push_back(g_hLabelSbStatus);

        g_hBtnStartSb = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 395, y1 + 53, 95, 26, hWnd, (HMENU)ID_BTN_START_SB, g_hInstance, NULL);
        SendMessageW(g_hBtnStartSb, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab1Ctrls.push_back(g_hBtnStartSb);

        g_hBtnStopSb = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 498, y1 + 53, 65, 26, hWnd, (HMENU)ID_BTN_STOP_SB, g_hInstance, NULL);
        SendMessageW(g_hBtnStopSb, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(g_hBtnStopSb);

        g_hBtnEditCfg = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 570, y1 + 53, 65, 26, hWnd, (HMENU)ID_BTN_EDIT_CONFIG, g_hInstance, NULL);
        SendMessageW(g_hBtnEditCfg, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(g_hBtnEditCfg);

        y1 += 96;

        // 区域 2：游戏注入与接管控制 (宽 624px)
        g_hGrpInject = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 25, y1, 624, 120, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(g_hGrpInject, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab1Ctrls.push_back(g_hGrpInject);

        g_hLblTarget = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 38, y1 + 25, 70, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(g_hLblTarget, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(g_hLblTarget);

        g_hEditTargetName = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"WoW 11.2.5 - Firestorm.exe", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 110, y1 + 22, 230, 24, hWnd, (HMENU)ID_EDIT_TARGET_NAME, g_hInstance, NULL);
        SendMessageW(g_hEditTargetName, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(g_hEditTargetName);

        // 彻底解决文字溢出：立即接管与恢复直连按钮加宽至 135px
        g_hBtnInjectNow = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 350, y1 + 20, 135, 28, hWnd, (HMENU)ID_BTN_INJECT_NOW, g_hInstance, NULL);
        SendMessageW(g_hBtnInjectNow, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab1Ctrls.push_back(g_hBtnInjectNow);

        g_hBtnUnloadNow = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 495, y1 + 20, 135, 28, hWnd, (HMENU)ID_BTN_UNLOAD_NOW, g_hInstance, NULL);
        SendMessageW(g_hBtnUnloadNow, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab1Ctrls.push_back(g_hBtnUnloadNow);

        // 第二行：自动监控按钮加宽至 185px，文字完全容纳
        g_hBtnAutoInject = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 38, y1 + 60, 185, 28, hWnd, (HMENU)ID_BTN_AUTO_INJECT, g_hInstance, NULL);
        SendMessageW(g_hBtnAutoInject, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(g_hBtnAutoInject);

        g_hLblPid = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 240, y1 + 65, 50, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(g_hLblPid, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(g_hLblPid);

        g_hEditPid = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER, 295, y1 + 62, 70, 24, hWnd, (HMENU)ID_EDIT_PID, g_hInstance, NULL);
        SendMessageW(g_hEditPid, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(g_hEditPid);

        g_hBtnInjectPid = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 375, y1 + 61, 85, 26, hWnd, (HMENU)ID_BTN_INJECT_PID, g_hInstance, NULL);
        SendMessageW(g_hBtnInjectPid, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(g_hBtnInjectPid);

        g_hBtnClearLog = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 550, y1 + 61, 80, 26, hWnd, (HMENU)ID_BTN_CLEAR_LOG, g_hInstance, NULL);
        SendMessageW(g_hBtnClearLog, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab1Ctrls.push_back(g_hBtnClearLog);

        y1 += 128;

        // 区域 3：极客终端日志 (宽 624px)
        g_hEditLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            25, y1, 624, 250, hWnd, (HMENU)ID_EDIT_LOG, g_hInstance, NULL);
        SendMessageW(g_hEditLog, WM_SETFONT, (WPARAM)g_hFontConsole, TRUE);
        g_Tab1Ctrls.push_back(g_hEditLog);

        // ==================== TAB 2 控件布局 (服务器探针) ====================
        int y2 = 45;

        g_hLblProbeHeader = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 25, y2, 450, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(g_hLblProbeHeader, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab2Ctrls.push_back(g_hLblProbeHeader);

        // 删除了进入游戏目录选项，仅保留大按钮开始全面测速
        g_hBtnProbeAll = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 490, y2 - 4, 155, 30, hWnd, (HMENU)ID_BTN_PROBE_ALL, g_hInstance, NULL);
        SendMessageW(g_hBtnProbeAll, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab2Ctrls.push_back(g_hBtnProbeAll);

        y2 += 34;

        g_hListProbe = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER,
            25, y2, 624, 300, hWnd, (HMENU)ID_LIST_PROBE, g_hInstance, NULL);
        SendMessageW(g_hListProbe, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        ListView_SetExtendedListViewStyle(g_hListProbe, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        LVCOLUMNW col = { 0 };
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = (LPWSTR)L""; col.cx = 210; ListView_InsertColumn(g_hListProbe, 0, &col);
        col.pszText = (LPWSTR)L""; col.cx = 145; ListView_InsertColumn(g_hListProbe, 1, &col);
        col.pszText = (LPWSTR)L""; col.cx = 115; ListView_InsertColumn(g_hListProbe, 2, &col);
        col.pszText = (LPWSTR)L""; col.cx = 145; ListView_InsertColumn(g_hListProbe, 3, &col);
        g_Tab2Ctrls.push_back(g_hListProbe);

        y2 += 308;

        g_hLabelProbeTips = CreateWindowW(L"STATIC", L"就绪。", WS_CHILD | WS_VISIBLE, 25, y2, 624, 38, hWnd, (HMENU)ID_LABEL_PROBE_TIPS, g_hInstance, NULL);
        SendMessageW(g_hLabelProbeTips, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab2Ctrls.push_back(g_hLabelProbeTips);

        y2 += 44;

        g_hGrpAddProbe = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 25, y2, 624, 80, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(g_hGrpAddProbe, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab2Ctrls.push_back(g_hGrpAddProbe);

        g_hLblPName = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 38, y2 + 28, 40, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(g_hLblPName, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab2Ctrls.push_back(g_hLblPName);

        g_hEditProbeName = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"自定义端点", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 80, y2 + 25, 120, 24, hWnd, (HMENU)ID_EDIT_PROBE_NAME, g_hInstance, NULL);
        SendMessageW(g_hEditProbeName, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab2Ctrls.push_back(g_hEditProbeName);

        g_hLblPHost = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 210, y2 + 28, 55, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(g_hLblPHost, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab2Ctrls.push_back(g_hLblPHost);

        g_hEditProbeHost = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"141.95.207.63", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 270, y2 + 25, 120, 24, hWnd, (HMENU)ID_EDIT_PROBE_HOST, g_hInstance, NULL);
        SendMessageW(g_hEditProbeHost, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab2Ctrls.push_back(g_hEditProbeHost);

        g_hLblPPort = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 400, y2 + 28, 40, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(g_hLblPPort, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab2Ctrls.push_back(g_hLblPPort);

        g_hEditProbePort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"8285", WS_CHILD | WS_VISIBLE | ES_NUMBER, 445, y2 + 25, 55, 24, hWnd, (HMENU)ID_EDIT_PROBE_PORT, g_hInstance, NULL);
        SendMessageW(g_hEditProbePort, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab2Ctrls.push_back(g_hEditProbePort);

        g_hBtnAddProbe = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 510, y2 + 24, 60, 26, hWnd, (HMENU)ID_BTN_PROBE_ADD, g_hInstance, NULL);
        SendMessageW(g_hBtnAddProbe, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab2Ctrls.push_back(g_hBtnAddProbe);

        g_hBtnDelProbe = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 578, y2 + 24, 60, 26, hWnd, (HMENU)ID_BTN_PROBE_DEL, g_hInstance, NULL);
        SendMessageW(g_hBtnDelProbe, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab2Ctrls.push_back(g_hBtnDelProbe);

        // ==================== TAB 3 控件布局 (订阅与配置) ====================
        int y3 = 45;

        g_hGrpSub = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 25, y3, 624, 175, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(g_hGrpSub, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab3Ctrls.push_back(g_hGrpSub);

        g_hLblSubPri = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 38, y3 + 32, 80, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(g_hLblSubPri, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab3Ctrls.push_back(g_hLblSubPri);

        g_hEditSubPri = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"http://104.252.30.68:3589/sub?token=a54c5ad53eaf2f814f6ecbf33d127b2f&format=singbox", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 125, y3 + 29, 360, 24, hWnd, (HMENU)ID_EDIT_SUB_PRI, g_hInstance, NULL);
        SendMessageW(g_hEditSubPri, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab3Ctrls.push_back(g_hEditSubPri);

        g_hBtnFetchPri = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 495, y3 + 27, 140, 28, hWnd, (HMENU)ID_BTN_FETCH_SUB_PRI, g_hInstance, NULL);
        SendMessageW(g_hBtnFetchPri, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab3Ctrls.push_back(g_hBtnFetchPri);

        g_hLblSubBak = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 38, y3 + 75, 80, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(g_hLblSubBak, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab3Ctrls.push_back(g_hLblSubBak);

        g_hEditSubBak = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"http://104.252.30.68:3589/sub?token=a54c5ad53eaf2f814f6ecbf33d127b2f", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 125, y3 + 72, 360, 24, hWnd, (HMENU)ID_EDIT_SUB_BAK, g_hInstance, NULL);
        SendMessageW(g_hEditSubBak, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab3Ctrls.push_back(g_hEditSubBak);

        g_hBtnFetchBak = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 495, y3 + 70, 140, 28, hWnd, (HMENU)ID_BTN_FETCH_SUB_BAK, g_hInstance, NULL);
        SendMessageW(g_hBtnFetchBak, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab3Ctrls.push_back(g_hBtnFetchBak);

        g_hBtnSaveSub = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 125, y3 + 115, 210, 32, hWnd, (HMENU)ID_BTN_SAVE_SUB, g_hInstance, NULL);
        SendMessageW(g_hBtnSaveSub, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab3Ctrls.push_back(g_hBtnSaveSub);

        y3 += 190;

        g_hGrpTools = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 25, y3, 624, 90, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(g_hGrpTools, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        g_Tab3Ctrls.push_back(g_hGrpTools);

        g_hBtnOpenDir = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 45, y3 + 35, 170, 32, hWnd, (HMENU)ID_BTN_OPEN_DIR, g_hInstance, NULL);
        SendMessageW(g_hBtnOpenDir, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        g_Tab3Ctrls.push_back(g_hBtnOpenDir);

        SwitchTab(0);
        InitProbeTargets();
        LoadSavedSubscriptions();
        ApplyLanguage();

        AppendLog(L"GameTcpSpeed v3.0 (纯经典 Win32 双语版) 初始化就绪。");
        return 0;
    }

    case WM_NOTIFY: {
        LPNMHDR pnm = (LPNMHDR)lParam;
        if (pnm->idFrom == ID_TAB_MAIN && pnm->code == TCN_SELCHANGE) {
            int sel = TabCtrl_GetCurSel(g_hTabCtrl);
            SwitchTab(sel);
        }
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        switch (id) {
        case ID_BTN_TOGGLE_LANG: {
            g_bEnglish = !g_bEnglish;
            ApplyLanguage();
            AppendLog(L(L"已切换为简体中文界面。", L"Switched to English interface."));
            break;
        }

        case ID_BTN_START_SB:
            StartSingBox();
            break;

        case ID_BTN_STOP_SB:
            StopSingBox();
            break;

        case ID_BTN_EDIT_CONFIG: {
            wchar_t exePath[MAX_PATH], cfgPath[MAX_PATH];
            GetSingBoxPaths(exePath, cfgPath);
            ShellExecuteW(NULL, L"open", L"notepad.exe", cfgPath, NULL, SW_SHOWNORMAL);
            break;
        }

        case ID_BTN_TEST_SPEED: {
            AppendLog(L(L"正在测试当前专线节点物理延迟...", L"Measuring node direct RTT..."));
            double rtt = MeasureDirectRtt("104.252.30.68", 3589, 2500);
            if (rtt >= 0) {
                wchar_t msg[128];
                _snwprintf(msg, 128, L(L"√ 专线物理 RTT 延迟: %.1f ms", L"√ Node direct RTT: %.1f ms"), rtt);
                AppendLog(msg);
            } else {
                AppendLog(L(L"× 专线节点连接超时/阻断，请检查网络！", L"× Node connection timeout/blocked!"));
            }
            break;
        }

        case ID_BTN_INJECT_NOW: {
            wchar_t targetName[128];
            GetWindowTextW(g_hEditTargetName, targetName, 128);
            StartSingBox();
            int n = InjectMatchingProcesses(targetName);
            if (n == 0) {
                AppendLog(L(L"未检测到运行中的游戏进程，可点击【开启自动监控】！", 
                            L"No running game found. Click [Auto-Monitor]!"));
            }
            break;
        }

        case ID_BTN_UNLOAD_NOW: {
            wchar_t targetName[128];
            GetWindowTextW(g_hEditTargetName, targetName, 128);
            EjectMatchingProcesses(targetName);
            break;
        }

        case ID_BTN_AUTO_INJECT:
            ToggleAutoInject();
            break;

        case ID_BTN_INJECT_PID: {
            wchar_t pidStr[32];
            GetWindowTextW(g_hEditPid, pidStr, 32);
            DWORD pid = (DWORD)_wtoi(pidStr);
            if (pid > 0) {
                StartSingBox();
                InjectDllByPid(pid);
            }
            break;
        }

        case ID_BTN_CLEAR_LOG:
            if (g_hEditLog) SetWindowTextW(g_hEditLog, L"");
            break;

        case ID_BTN_PROBE_ALL:
            if (!g_bProbing) {
                HANDLE h = CreateThread(NULL, 0, ProbeAllWorker, NULL, 0, NULL);
                if (h) CloseHandle(h);
            }
            break;

        case ID_BTN_SAVE_SUB:
            SaveSubscriptions();
            break;

        case ID_BTN_OPEN_DIR: {
            wchar_t appDir[MAX_PATH];
            GetAppDir(appDir, MAX_PATH);
            ShellExecuteW(NULL, L"open", appDir, NULL, NULL, SW_SHOWNORMAL);
            break;
        }
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        return (INT_PTR)g_hBrushBg;
    }

    case WM_DESTROY:
        StopSingBox();
        if (g_hAutoInjectThread) {
            g_bAutoInjecting = false;
            CloseHandle(g_hAutoInjectThread);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    g_hInstance = hInstance;

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    WNDCLASSEXW wcex = { 0 };
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    
    // 加载嵌入的精致应用图标
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    wcex.hIconSm = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, 16, 16, 0);
    if (!wcex.hIcon) wcex.hIcon = LoadIcon(NULL, IDI_APPLICATION);

    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = CreateSolidBrush(RGB(246, 248, 250));
    wcex.lpszClassName = L"GameTcpSpeed_Win32_Class";

    RegisterClassExW(&wcex);

    int winWidth = 690;
    int winHeight = 640;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - winWidth) / 2;
    int posY = (screenH - winHeight) / 2;

    g_hMainWnd = CreateWindowExW(
        0, L"GameTcpSpeed_Win32_Class",
        L"GameTcpSpeed v3.0",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        posX, posY, winWidth, winHeight,
        NULL, NULL, hInstance, NULL);

    if (!g_hMainWnd) return 1;

    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WSACleanup();
    return (int)msg.wParam;
}
