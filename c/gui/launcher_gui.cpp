#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <set>
#include <string>

#pragma comment(lib, "comctl32.lib")

#define ID_BTN_BROWSE       1001
#define ID_BTN_START_SB     1002
#define ID_BTN_STOP_SB      1003
#define ID_BTN_LAUNCH_GAME  1004
#define ID_BTN_INJECT_NOW   1005
#define ID_BTN_AUTO_INJECT  1006
#define ID_BTN_EDIT_CONFIG  1007
#define ID_BTN_APPLY_NODE   1008
#define ID_BTN_INJECT_PID   1009

#define ID_EDIT_GAME_PATH   2001
#define ID_EDIT_PORT        2002
#define ID_EDIT_PID         2003
#define ID_EDIT_LOG         2004
#define ID_LABEL_SB_STATUS  2005
#define ID_EDIT_TARGET_NAME 2006
#define ID_COMBO_NODES      2007

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
static HFONT g_hFont = NULL;

static PROCESS_INFORMATION g_SbProcInfo = { 0 };

// 自动注入监听状态
static bool g_bAutoInjecting = false;
static HANDLE g_hAutoInjectThread = NULL;
static std::set<DWORD> g_InjectedPids;

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

// 写入 proxy_config.ini 给 hook_proxy.dll 使用
static void SaveProxyConfig(const wchar_t* portStr) {
    wchar_t appDir[MAX_PATH];
    GetAppDir(appDir, MAX_PATH);
    wchar_t iniPath[MAX_PATH];
    _snwprintf(iniPath, MAX_PATH, L"%s\\proxy_config.ini", appDir);

    WritePrivateProfileStringW(L"Proxy", L"Host", L"127.0.0.1", iniPath);
    WritePrivateProfileStringW(L"Proxy", L"Port", portStr, iniPath);
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
                // 排除登录器本身与 Agent 后台进程
                if (_wcsicmp(pe.szExeFile, L"FirestormLauncher.exe") != 0 && _wcsicmp(pe.szExeFile, L"FirestormAgent.exe") != 0) {
                    match = true;
                }
            }
            if (match) {
                // 如果尚未注入过
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
        Sleep(400); // 400ms 扫描一次，低 CPU 开销
    }
    return 0;
}

// 切换自动监控状态
static void ToggleAutoInject() {
    wchar_t targetName[128];
    GetWindowTextW(g_hEditTargetName, targetName, 128);
    if (wcslen(targetName) == 0) {
        AppendLog(L"请先输入目标游戏进程名（如 Wow.exe）！");
        return;
    }

    StartSingBox(); // 保证核心处于运行状态

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
        SetWindowTextW(g_hBtnAutoInject, L"🚀 开启自动监控注入 (登录器推荐)");
        AppendLog(L"已停止自动注入监控。");
    }
}

// 立即注入当前已运行的游戏
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

// 打开 config.json 编辑
static void EditConfigFile() {
    wchar_t exePath[MAX_PATH];
    wchar_t cfgPath[MAX_PATH];
    GetSingBoxPaths(exePath, cfgPath);

    ShellExecuteW(NULL, L"open", L"notepad.exe", cfgPath, NULL, SW_SHOWNORMAL);
    AppendLog(L"已在记事本中打开 Sing-box\\config.json，修改保存后可点击“重启核心”生效。");
}

// 应用选中的出站节点
static void ApplySelectedNode() {
    int idx = (int)SendMessageW(g_hComboNodes, CB_GETCURSEL, 0, 0);
    if (idx == CB_ERR) return;

    wchar_t nodeTag[128];
    SendMessageW(g_hComboNodes, CB_GETLBTEXT, idx, (LPARAM)nodeTag);

    // 截取空格前的 tag 标识
    wchar_t* space = wcschr(nodeTag, L' ');
    if (space) *space = L'\0';

    wchar_t exePath[MAX_PATH];
    wchar_t cfgPath[MAX_PATH];
    GetSingBoxPaths(exePath, cfgPath);

    // 读取 config.json 内容并替换 default 节点
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
    // 替换 "default": "..."
    char targetTagA[64];
    WideCharToMultiByte(CP_UTF8, 0, nodeTag, -1, targetTagA, sizeof(targetTagA), NULL, NULL);

    size_t defPos = content.find("\"default\":");
    if (defPos != std::string::npos) {
        size_t quoteStart = content.find('\"', defPos + 10);
        if (quoteStart != std::string::npos) {
            size_t quoteEnd = content.find('\"', quoteStart + 1);
            if (quoteEnd != std::string::npos) {
                content.replace(quoteStart + 1, quoteEnd - quoteStart - 1, targetTagA);

                // 写回文件
                FILE* outFp = _wfopen(cfgPath, L"wb");
                if (outFp) {
                    fwrite(content.data(), 1, content.size(), outFp);
                    fclose(outFp);

                    wchar_t msg[256];
                    _snwprintf(msg, sizeof(msg)/sizeof(wchar_t), L"√ 默认加速节点已切换为 [%s]！正在平滑重启 Sing-box 核心...", nodeTag);
                    AppendLog(msg);

                    // 重启 Sing-box
                    StopSingBox();
                    Sleep(300);
                    StartSingBox();
                    return;
                }
            }
        }
    }
    AppendLog(L"配置替换失败，建议点击“编辑节点文件”直接修改。");
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

        int y = 15;

        // ────── 区域 1：专线节点配置 ──────
        HWND hGrpNode = CreateWindowW(L"BUTTON", L" 专线节点选择与配置 ", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 15, y, 510, 85, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hGrpNode, WM_SETFONT, (WPARAM)hBoldFont, TRUE);

        HWND hLblNode = CreateWindowW(L"STATIC", L"当前加速节点:", WS_CHILD | WS_VISIBLE, 30, y + 25, 90, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblNode, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hComboNodes = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 125, y + 22, 230, 150, hWnd, (HMENU)ID_COMBO_NODES, g_hInstance, NULL);
        SendMessageW(g_hComboNodes, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(g_hComboNodes, CB_ADDSTRING, 0, (LPARAM)L"qiniu-0709 (新加坡主节点 - VLESS)");
        SendMessageW(g_hComboNodes, CB_ADDSTRING, 0, (LPARAM)L"qiniu-0001 (新加坡备节点 - VLESS)");
        SendMessageW(g_hComboNodes, CB_ADDSTRING, 0, (LPARAM)L"hy2-va1 (Hysteria 2 专线 - 66.154)");
        SendMessageW(g_hComboNodes, CB_ADDSTRING, 0, (LPARAM)L"vl-reality-va1 (VLESS Reality - 66.154)");
        SendMessageW(g_hComboNodes, CB_SETCURSEL, 0, 0);

        HWND hBtnApplyNode = CreateWindowW(L"BUTTON", L"应用切换", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 365, y + 21, 75, 26, hWnd, (HMENU)ID_BTN_APPLY_NODE, g_hInstance, NULL);
        SendMessageW(hBtnApplyNode, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnEditCfg = CreateWindowW(L"BUTTON", L"编辑配置", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 445, y + 21, 68, 26, hWnd, (HMENU)ID_BTN_EDIT_CONFIG, g_hInstance, NULL);
        SendMessageW(hBtnEditCfg, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Socks5 端口与核心状态
        HWND hLblPort = CreateWindowW(L"STATIC", L"本地 Socks 端口:", WS_CHILD | WS_VISIBLE, 30, y + 54, 100, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblPort, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hEditPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"2081", WS_CHILD | WS_VISIBLE | ES_NUMBER, 135, y + 52, 60, 22, hWnd, (HMENU)ID_EDIT_PORT, g_hInstance, NULL);
        SendMessageW(g_hEditPort, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hLabelSbStatus = CreateWindowW(L"STATIC", L"Sing-box: 未运行", WS_CHILD | WS_VISIBLE, 215, y + 54, 150, 20, hWnd, (HMENU)ID_LABEL_SB_STATUS, g_hInstance, NULL);
        SendMessageW(g_hLabelSbStatus, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnStartSb = CreateWindowW(L"BUTTON", L"启动核心", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 365, y + 51, 75, 24, hWnd, (HMENU)ID_BTN_START_SB, g_hInstance, NULL);
        SendMessageW(hBtnStartSb, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnStopSb = CreateWindowW(L"BUTTON", L"停止", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 445, y + 51, 68, 24, hWnd, (HMENU)ID_BTN_STOP_SB, g_hInstance, NULL);
        SendMessageW(hBtnStopSb, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        y += 95;

        // ────── 区域 2：游戏注入控制（专为登录器优化） ──────
        HWND hGrpInject = CreateWindowW(L"BUTTON", L" 游戏注入加速（适配登录器） ", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 15, y, 510, 115, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hGrpInject, WM_SETFONT, (WPARAM)hBoldFont, TRUE);

        HWND hLblTarget = CreateWindowW(L"STATIC", L"目标游戏进程名:", WS_CHILD | WS_VISIBLE, 30, y + 28, 105, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblTarget, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hEditTargetName = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"WoW 11.2.5 - Firestorm.exe", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 140, y + 25, 200, 24, hWnd, (HMENU)ID_EDIT_TARGET_NAME, g_hInstance, NULL);
        SendMessageW(g_hEditTargetName, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // 重点按钮：开启自动监控注入
        g_hBtnAutoInject = CreateWindowW(L"BUTTON", L"🚀 开启自动监控注入", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 350, y + 22, 165, 30, hWnd, (HMENU)ID_BTN_AUTO_INJECT, g_hInstance, NULL);
        SendMessageW(g_hBtnAutoInject, WM_SETFONT, (WPARAM)hBoldFont, TRUE);

        // 按钮：立即注入现有进程
        HWND hBtnInjectNow = CreateWindowW(L"BUTTON", L"💉 立即注入现有游戏", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 30, y + 65, 160, 32, hWnd, (HMENU)ID_BTN_INJECT_NOW, g_hInstance, NULL);
        SendMessageW(hBtnInjectNow, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // 按指定 PID 注入
        HWND hLblPid = CreateWindowW(L"STATIC", L"或按 PID:", WS_CHILD | WS_VISIBLE, 210, y + 72, 60, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblPid, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hEditPid = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER, 275, y + 68, 65, 24, hWnd, (HMENU)ID_EDIT_PID, g_hInstance, NULL);
        SendMessageW(g_hEditPid, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnInjectPid = CreateWindowW(L"BUTTON", L"注入PID", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 345, y + 66, 65, 28, hWnd, (HMENU)ID_BTN_INJECT_PID, g_hInstance, NULL);
        SendMessageW(hBtnInjectPid, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        y += 125;

        // ────── 区域 3：直接启动游戏（备用） ──────
        HWND hGrpLaunch = CreateWindowW(L"BUTTON", L" 备用：不走登录器直接启动 ", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 15, y, 510, 65, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hGrpLaunch, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hEditGamePath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"E:\\gamel\\The War Within\\WoW 11.2.5 - Firestorm.exe", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 30, y + 25, 260, 24, hWnd, (HMENU)ID_EDIT_GAME_PATH, g_hInstance, NULL);
        SendMessageW(g_hEditGamePath, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnBrowse = CreateWindowW(L"BUTTON", L"浏览...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 295, y + 24, 65, 26, hWnd, (HMENU)ID_BTN_BROWSE, g_hInstance, NULL);
        SendMessageW(hBtnBrowse, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnLaunch = CreateWindowW(L"BUTTON", L"直接启动并注入", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 370, y + 23, 140, 28, hWnd, (HMENU)ID_BTN_LAUNCH_GAME, g_hInstance, NULL);
        SendMessageW(hBtnLaunch, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        y += 75;

        // ────── 区域 4：运行日志 ──────
        g_hEditLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 15, y, 510, 160, hWnd, (HMENU)ID_EDIT_LOG, g_hInstance, NULL);
        SendMessageW(g_hEditLog, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        AppendLog(L"程序已就绪。适配登录器模式，无 TUN 模式与网卡冲突。");
        AppendLog(L"推荐用法：点击【开启自动监控注入】，然后正常用您的登录器打开游戏即可！");
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
        case ID_BTN_AUTO_INJECT:
            ToggleAutoInject();
            break;
        case ID_BTN_INJECT_NOW:
            InjectNow();
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
        L"Game TCP Speed - 专线游戏加速器 (登录器伴侣版)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 555, 520,
        NULL, NULL, hInstance, NULL
    );

    if (!hWnd) return 1;

    g_hMainWnd = hWnd;
    ShowWindow(hWnd, SW_SHOWNORMAL);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
