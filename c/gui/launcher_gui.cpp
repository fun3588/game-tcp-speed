#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "comctl32.lib")

#define ID_BTN_BROWSE       1001
#define ID_BTN_START_SB     1002
#define ID_BTN_STOP_SB      1003
#define ID_BTN_LAUNCH_GAME  1004
#define ID_BTN_INJECT_PID   1005

#define ID_EDIT_GAME_PATH   2001
#define ID_EDIT_PORT        2002
#define ID_EDIT_PID         2003
#define ID_EDIT_LOG         2004
#define ID_LABEL_SB_STATUS  2005

static HINSTANCE g_hInstance = NULL;
static HWND g_hMainWnd = NULL;
static HWND g_hEditGamePath = NULL;
static HWND g_hEditPort = NULL;
static HWND g_hEditPid = NULL;
static HWND g_hEditLog = NULL;
static HWND g_hLabelSbStatus = NULL;
static HFONT g_hFont = NULL;

static PROCESS_INFORMATION g_SbProcInfo = { 0 };

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

// 获取可执行文件所在目录 (Unicode)
static void GetAppDir(wchar_t* outDir, int maxLen) {
    GetModuleFileNameW(NULL, outDir, maxLen);
    wchar_t* lastSlash = wcsrchr(outDir, L'\\');
    if (lastSlash) *lastSlash = L'\0';
}

// 写入 proxy_config.ini
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

    wchar_t appDir[MAX_PATH];
    GetAppDir(appDir, MAX_PATH);

    wchar_t exePath[MAX_PATH];
    _snwprintf(exePath, MAX_PATH, L"%s\\..\\Sing-box\\sing-box.exe", appDir);
    wchar_t cfgPath[MAX_PATH];
    _snwprintf(cfgPath, MAX_PATH, L"%s\\..\\Sing-box\\config.json", appDir);

    // 检查是否存在
    if (GetFileAttributesW(exePath) == INVALID_FILE_ATTRIBUTES) {
        _snwprintf(exePath, MAX_PATH, L"%s\\Sing-box\\sing-box.exe", appDir);
        _snwprintf(cfgPath, MAX_PATH, L"%s\\Sing-box\\config.json", appDir);
    }

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

// 向目标进程注入 DLL (宽字符版，支持中文路径)
static BOOL InjectDllByHandle(HANDLE hProcess, const wchar_t* dllPath) {
    size_t len = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID pRemoteMem = VirtualAllocEx(hProcess, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemoteMem) {
        AppendLog(L"错误: VirtualAllocEx 远程内存分配失败！");
        return FALSE;
    }

    if (!WriteProcessMemory(hProcess, pRemoteMem, dllPath, len, NULL)) {
        AppendLog(L"错误: WriteProcessMemory 写入 DLL 路径失败！");
        VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
        return FALSE;
    }

    LPTHREAD_START_ROUTINE pLoadLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (!pLoadLibrary) {
        AppendLog(L"错误: 找不到 LoadLibraryW 地址！");
        VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
        return FALSE;
    }

    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, pLoadLibrary, pRemoteMem, 0, NULL);
    if (!hThread) {
        AppendLog(L"错误: CreateRemoteThread 远程线程创建失败！");
        VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
        return FALSE;
    }

    WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
    return TRUE;
}

// 启动游戏并注入（挂起启动方式）
static void LaunchGameAndInject() {
    wchar_t gamePath[MAX_PATH];
    GetWindowTextW(g_hEditGamePath, gamePath, MAX_PATH);
    if (wcslen(gamePath) == 0) {
        AppendLog(L"请先选择游戏主程序 (.exe)！");
        return;
    }

    wchar_t portStr[16];
    GetWindowTextW(g_hEditPort, portStr, 16);
    SaveProxyConfig(portStr);

    wchar_t appDir[MAX_PATH];
    GetAppDir(appDir, MAX_PATH);
    wchar_t dllPath[MAX_PATH];
    _snwprintf(dllPath, MAX_PATH, L"%s\\hook_proxy.dll", appDir);

    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        AppendLog(L"错误: 未找到 hook_proxy.dll，请先编译 DLL！");
        return;
    }

    StartSingBox();

    wchar_t workDir[MAX_PATH];
    wcsncpy(workDir, gamePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(workDir, L'\\');
    if (lastSlash) *lastSlash = L'\0';

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };

    AppendLog(L"正在以挂起模式启动游戏...");
    if (!CreateProcessW(gamePath, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, workDir, &si, &pi)) {
        wchar_t errBuf[256];
        _snwprintf(errBuf, sizeof(errBuf)/sizeof(wchar_t), L"启动游戏失败！错误代码: %lu", GetLastError());
        AppendLog(errBuf);
        return;
    }

    wchar_t msg[256];
    _snwprintf(msg, sizeof(msg)/sizeof(wchar_t), L"游戏已创建挂起 (PID: %lu)，正在注入加速模块...", pi.dwProcessId);
    AppendLog(msg);

    if (InjectDllByHandle(pi.hProcess, dllPath)) {
        AppendLog(L"加速模块注入成功！恢复游戏主线程运行...");
        ResumeThread(pi.hThread);
        AppendLog(L">>> 游戏已成功启动并已开启专线加速！ <<<");
    } else {
        AppendLog(L"注入失败，终止游戏进程以防流量直连！");
        TerminateProcess(pi.hProcess, 0);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

// 注入已在运行的进程 PID
static void InjectExistingPid() {
    wchar_t pidStr[32];
    GetWindowTextW(g_hEditPid, pidStr, 32);
    DWORD pid = (DWORD)_wtoi(pidStr);
    if (pid == 0) {
        AppendLog(L"请输入有效的游戏进程 PID！");
        return;
    }

    wchar_t portStr[16];
    GetWindowTextW(g_hEditPort, portStr, 16);
    SaveProxyConfig(portStr);

    wchar_t appDir[MAX_PATH];
    GetAppDir(appDir, MAX_PATH);
    wchar_t dllPath[MAX_PATH];
    _snwprintf(dllPath, MAX_PATH, L"%s\\hook_proxy.dll", appDir);

    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess) {
        AppendLog(L"错误: 无法打开该进程 (OpenProcess 失败)，请尝试以管理员权限运行！");
        return;
    }

    AppendLog(L"正在注入现有进程...");
    if (InjectDllByHandle(hProcess, dllPath)) {
        AppendLog(L"注入成功！该进程的 TCP 流量已接入加速通道。");
    } else {
        AppendLog(L"注入现有进程失败！");
    }
    CloseHandle(hProcess);
}

// 选择游戏可执行文件
static void BrowseGameExe() {
    wchar_t szFile[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameW(&ofn)) {
        SetWindowTextW(g_hEditGamePath, szFile);
    }
}

// 窗口过程
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        // 创建高质量微软雅黑字体
        g_hFont = CreateFontW(
            -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei"
        );
        if (!g_hFont) {
            g_hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        }

        // 标签：游戏路径
        HWND hLblGame = CreateWindowW(L"STATIC", L"游戏主程序 (.exe):", WS_CHILD | WS_VISIBLE, 20, 20, 150, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblGame, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hEditGamePath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 20, 45, 360, 26, hWnd, (HMENU)ID_EDIT_GAME_PATH, g_hInstance, NULL);
        SendMessageW(g_hEditGamePath, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnBrowse = CreateWindowW(L"BUTTON", L"浏览...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 390, 44, 75, 28, hWnd, (HMENU)ID_BTN_BROWSE, g_hInstance, NULL);
        SendMessageW(hBtnBrowse, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // 标签：本地代理端口
        HWND hLblPort = CreateWindowW(L"STATIC", L"Socks5 本地端口:", WS_CHILD | WS_VISIBLE, 20, 85, 120, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblPort, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hEditPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"10808", WS_CHILD | WS_VISIBLE | ES_NUMBER, 145, 82, 75, 26, hWnd, (HMENU)ID_EDIT_PORT, g_hInstance, NULL);
        SendMessageW(g_hEditPort, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Sing-box 控制
        g_hLabelSbStatus = CreateWindowW(L"STATIC", L"Sing-box: 未运行", WS_CHILD | WS_VISIBLE, 235, 85, 145, 20, hWnd, (HMENU)ID_LABEL_SB_STATUS, g_hInstance, NULL);
        SendMessageW(g_hLabelSbStatus, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnStartSb = CreateWindowW(L"BUTTON", L"启动核心", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 390, 81, 75, 28, hWnd, (HMENU)ID_BTN_START_SB, g_hInstance, NULL);
        SendMessageW(hBtnStartSb, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // 启动并加速按钮
        HWND hBtnLaunch = CreateWindowW(L"BUTTON", L"【启动游戏并加速】", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 20, 120, 240, 38, hWnd, (HMENU)ID_BTN_LAUNCH_GAME, g_hInstance, NULL);
        SendMessageW(hBtnLaunch, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // 或按现有 PID 注入
        HWND hLblPid = CreateWindowW(L"STATIC", L"已有 PID:", WS_CHILD | WS_VISIBLE, 280, 130, 60, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessageW(hLblPid, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hEditPid = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER, 345, 126, 60, 26, hWnd, (HMENU)ID_EDIT_PID, g_hInstance, NULL);
        SendMessageW(g_hEditPid, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnInjectPid = CreateWindowW(L"BUTTON", L"注入PID", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 415, 125, 52, 28, hWnd, (HMENU)ID_BTN_INJECT_PID, g_hInstance, NULL);
        SendMessageW(hBtnInjectPid, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // 日志显示框
        g_hEditLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 20, 175, 445, 180, hWnd, (HMENU)ID_EDIT_LOG, g_hInstance, NULL);
        SendMessageW(g_hEditLog, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        AppendLog(L"程序就绪。未开启 TUN 模式，零网卡/路由冲突。");
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        switch (id) {
        case ID_BTN_BROWSE:
            BrowseGameExe();
            break;
        case ID_BTN_START_SB:
            StartSingBox();
            break;
        case ID_BTN_LAUNCH_GAME:
            LaunchGameAndInject();
            break;
        case ID_BTN_INJECT_PID:
            InjectExistingPid();
            break;
        }
        break;
    }
    case WM_DESTROY:
        StopSingBox();
        if (g_hFont) {
            DeleteObject(g_hFont);
            g_hFont = NULL;
        }
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

    if (!RegisterClassExW(&wc)) {
        return 1;
    }

    HWND hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"Game TCP Speed - 专线游戏加速器",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 420,
        NULL, NULL, hInstance, NULL
    );

    if (!hWnd) {
        return 1;
    }

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
