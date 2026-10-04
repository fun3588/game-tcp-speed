#define WIN32_LEAN_AND_MEAN
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

static PROCESS_INFORMATION g_SbProcInfo = { 0 };

static void AppendLog(const char* text) {
    if (!g_hEditLog) return;
    int len = GetWindowTextLengthA(g_hEditLog);
    SendMessageA(g_hEditLog, EM_SETSEL, len, len);
    
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[1024];
    snprintf(buf, sizeof(buf), "[%02d:%02d:%02d] %s\r\n", st.wHour, st.wMinute, st.wSecond, text);
    SendMessageA(g_hEditLog, EM_REPLACESEL, FALSE, (LPARAM)buf);
    SendMessageA(g_hEditLog, WM_VSCROLL, SB_BOTTOM, 0);
}

// 获取可执行文件所在目录
static void GetAppDir(char* outDir, int maxLen) {
    GetModuleFileNameA(NULL, outDir, maxLen);
    char* lastSlash = strrchr(outDir, '\\');
    if (lastSlash) *lastSlash = '\0';
}

// 写入 proxy_config.ini
static void SaveProxyConfig(const char* portStr) {
    char appDir[MAX_PATH];
    GetAppDir(appDir, MAX_PATH);
    char iniPath[MAX_PATH];
    snprintf(iniPath, sizeof(iniPath), "%s\\proxy_config.ini", appDir);

    WritePrivateProfileStringA("Proxy", "Host", "127.0.0.1", iniPath);
    WritePrivateProfileStringA("Proxy", "Port", portStr, iniPath);
}

// 启动 Sing-box 核心
static void StartSingBox() {
    if (g_SbProcInfo.hProcess != NULL) {
        DWORD exitCode;
        if (GetExitCodeProcess(g_SbProcInfo.hProcess, &exitCode) && exitCode == STILL_ACTIVE) {
            AppendLog("Sing-box 核心已在运行中。");
            return;
        }
    }

    char appDir[MAX_PATH];
    GetAppDir(appDir, MAX_PATH);

    // Sing-box 位于同级或者上层目录
    char exePath[MAX_PATH];
    snprintf(exePath, sizeof(exePath), "%s\\..\\Sing-box\\sing-box.exe", appDir);
    char cfgPath[MAX_PATH];
    snprintf(cfgPath, sizeof(cfgPath), "%s\\..\\Sing-box\\config.json", appDir);

    // 检查文件是否存在，如果不存在则检查同目录
    if (GetFileAttributesA(exePath) == INVALID_FILE_ATTRIBUTES) {
        snprintf(exePath, sizeof(exePath), "%s\\Sing-box\\sing-box.exe", appDir);
        snprintf(cfgPath, sizeof(cfgPath), "%s\\Sing-box\\config.json", appDir);
    }

    if (GetFileAttributesA(exePath) == INVALID_FILE_ATTRIBUTES) {
        AppendLog("错误: 未找到 sing-box.exe，请检查 Sing-box 目录！");
        return;
    }

    char cmdLine[MAX_PATH * 3];
    snprintf(cmdLine, sizeof(cmdLine), "\"%s\" run -c \"%s\"", exePath, cfgPath);

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // 后台静默运行

    if (CreateProcessA(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &g_SbProcInfo)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Sing-box 核心启动成功 (PID: %lu)", g_SbProcInfo.dwProcessId);
        AppendLog(msg);
        SetWindowTextA(g_hLabelSbStatus, "Sing-box: 运行中 [OK]");
    } else {
        AppendLog("错误: 无法启动 Sing-box 进程！");
    }
}

// 停止 Sing-box 核心
static void StopSingBox() {
    if (g_SbProcInfo.hProcess != NULL) {
        TerminateProcess(g_SbProcInfo.hProcess, 0);
        CloseHandle(g_SbProcInfo.hProcess);
        CloseHandle(g_SbProcInfo.hThread);
        g_SbProcInfo.hProcess = NULL;
        AppendLog("Sing-box 核心已停止。");
    } else {
        AppendLog("Sing-box 未运行。");
    }
    SetWindowTextA(g_hLabelSbStatus, "Sing-box: 未运行");
}

// 向目标进程注入 DLL
static BOOL InjectDllByHandle(HANDLE hProcess, const char* dllPath) {
    size_t len = strlen(dllPath) + 1;
    LPVOID pRemoteMem = VirtualAllocEx(hProcess, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemoteMem) {
        AppendLog("错误: VirtualAllocEx 分配内存失败！");
        return FALSE;
    }

    if (!WriteProcessMemory(hProcess, pRemoteMem, dllPath, len, NULL)) {
        AppendLog("错误: WriteProcessMemory 写入失败！");
        VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
        return FALSE;
    }

    LPTHREAD_START_ROUTINE pLoadLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    if (!pLoadLibrary) {
        AppendLog("错误: 找不到 LoadLibraryA 地址！");
        VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
        return FALSE;
    }

    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, pLoadLibrary, pRemoteMem, 0, NULL);
    if (!hThread) {
        AppendLog("错误: CreateRemoteThread 创建远程线程失败！");
        VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
        return FALSE;
    }

    WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
    return TRUE;
}

// 启动游戏并注入（挂起启动方式，拦截第一个包）
static void LaunchGameAndInject() {
    char gamePath[MAX_PATH];
    GetWindowTextA(g_hEditGamePath, gamePath, MAX_PATH);
    if (strlen(gamePath) == 0) {
        AppendLog("请先选择游戏可执行文件 (.exe)！");
        return;
    }

    char portStr[16];
    GetWindowTextA(g_hEditPort, portStr, 16);
    SaveProxyConfig(portStr);

    char appDir[MAX_PATH];
    GetAppDir(appDir, MAX_PATH);
    char dllPath[MAX_PATH];
    snprintf(dllPath, sizeof(dllPath), "%s\\hook_proxy.dll", appDir);

    if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES) {
        AppendLog("错误: 未找到 hook_proxy.dll，请先编译 DLL！");
        return;
    }

    // 确保 Sing-box 正在运行
    StartSingBox();

    // 解析工作目录
    char workDir[MAX_PATH];
    strncpy(workDir, gamePath, sizeof(workDir));
    char* lastSlash = strrchr(workDir, '\\');
    if (lastSlash) *lastSlash = '\0';

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };

    AppendLog("正在以挂起模式启动游戏...");
    if (!CreateProcessA(gamePath, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, workDir, &si, &pi)) {
        char errBuf[256];
        snprintf(errBuf, sizeof(errBuf), "启动游戏失败！错误代码: %lu", GetLastError());
        AppendLog(errBuf);
        return;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "游戏已创建挂起 (PID: %lu)，正在注入加速模块...", pi.dwProcessId);
    AppendLog(msg);

    if (InjectDllByHandle(pi.hProcess, dllPath)) {
        AppendLog("加速模块注入成功！恢复游戏主线程运行...");
        ResumeThread(pi.hThread);
        AppendLog(">>> 游戏已成功启动并已开启专线加速！ <<<");
    } else {
        AppendLog("注入失败，终止游戏进程以防流量直连！");
        TerminateProcess(pi.hProcess, 0);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

// 注入已在运行的进程 PID
static void InjectExistingPid() {
    char pidStr[32];
    GetWindowTextA(g_hEditPid, pidStr, sizeof(pidStr));
    DWORD pid = (DWORD)atoi(pidStr);
    if (pid == 0) {
        AppendLog("请输入有效的游戏进程 PID！");
        return;
    }

    char portStr[16];
    GetWindowTextA(g_hEditPort, portStr, 16);
    SaveProxyConfig(portStr);

    char appDir[MAX_PATH];
    GetAppDir(appDir, MAX_PATH);
    char dllPath[MAX_PATH];
    snprintf(dllPath, sizeof(dllPath), "%s\\hook_proxy.dll", appDir);

    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess) {
        AppendLog("错误: 无法打开该进程 (OpenProcess 失败)，请尝试以管理员权限运行！");
        return;
    }

    AppendLog("正在注入现有进程...");
    if (InjectDllByHandle(hProcess, dllPath)) {
        AppendLog("注入成功！该进程的网络流量已接入加速通道。");
    } else {
        AppendLog("注入现有进程失败！");
    }
    CloseHandle(hProcess);
}

// 选择游戏可执行文件
static void BrowseGameExe() {
    char szFile[MAX_PATH] = { 0 };
    OPENFILENAMEA ofn = { sizeof(ofn) };
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = "可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameA(&ofn)) {
        SetWindowTextA(g_hEditGamePath, szFile);
    }
}

// 窗口过程
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        // 标签：游戏路径
        HWND hLblGame = CreateWindowA("STATIC", "游戏主程序 (.exe):", WS_CHILD | WS_VISIBLE, 20, 20, 150, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessage(hLblGame, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hEditGamePath = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 20, 45, 360, 24, hWnd, (HMENU)ID_EDIT_GAME_PATH, g_hInstance, NULL);
        SendMessage(g_hEditGamePath, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hBtnBrowse = CreateWindowA("BUTTON", "浏览...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 390, 44, 75, 26, hWnd, (HMENU)ID_BTN_BROWSE, g_hInstance, NULL);
        SendMessage(hBtnBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);

        // 标签：本地代理端口
        HWND hLblPort = CreateWindowA("STATIC", "Socks5 本地端口:", WS_CHILD | WS_VISIBLE, 20, 85, 120, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessage(hLblPort, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hEditPort = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "10808", WS_CHILD | WS_VISIBLE | ES_NUMBER, 140, 82, 80, 24, hWnd, (HMENU)ID_EDIT_PORT, g_hInstance, NULL);
        SendMessage(g_hEditPort, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Sing-box 控制
        g_hLabelSbStatus = CreateWindowA("STATIC", "Sing-box: 未运行", WS_CHILD | WS_VISIBLE, 240, 85, 140, 20, hWnd, (HMENU)ID_LABEL_SB_STATUS, g_hInstance, NULL);
        SendMessage(g_hLabelSbStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hBtnStartSb = CreateWindowA("BUTTON", "启动核心", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 390, 82, 75, 24, hWnd, (HMENU)ID_BTN_START_SB, g_hInstance, NULL);
        SendMessage(hBtnStartSb, WM_SETFONT, (WPARAM)hFont, TRUE);

        // 分割线效果按钮组
        HWND hBtnLaunch = CreateWindowA("BUTTON", "【启动游戏并加速】", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 20, 120, 240, 36, hWnd, (HMENU)ID_BTN_LAUNCH_GAME, g_hInstance, NULL);
        SendMessage(hBtnLaunch, WM_SETFONT, (WPARAM)hFont, TRUE);

        // 或按现有 PID 注入
        HWND hLblPid = CreateWindowA("STATIC", "已有 PID:", WS_CHILD | WS_VISIBLE, 280, 130, 60, 20, hWnd, NULL, g_hInstance, NULL);
        SendMessage(hLblPid, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hEditPid = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_NUMBER, 340, 126, 60, 24, hWnd, (HMENU)ID_EDIT_PID, g_hInstance, NULL);
        SendMessage(g_hEditPid, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hBtnInjectPid = CreateWindowA("BUTTON", "注入PID", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 410, 125, 55, 26, hWnd, (HMENU)ID_BTN_INJECT_PID, g_hInstance, NULL);
        SendMessage(hBtnInjectPid, WM_SETFONT, (WPARAM)hFont, TRUE);

        // 日志显示框
        g_hEditLog = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 20, 175, 445, 170, hWnd, (HMENU)ID_EDIT_LOG, g_hInstance, NULL);
        SendMessage(g_hEditLog, WM_SETFONT, (WPARAM)hFont, TRUE);

        AppendLog("程序就绪。未开启 TUN 模式，零网卡冲突。");
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
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcA(hWnd, message, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    g_hInstance = hInstance;
    InitCommonControls();

    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "GameSpeedLauncherWndClass";

    RegisterClassExA(&wc);

    HWND hWnd = CreateWindowExA(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        "Game TCP Speed - 专线游戏加速器",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 400,
        NULL, NULL, hInstance, NULL
    );

    g_hMainWnd = hWnd;
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
