#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include "../minhook/include/MinHook.h"

#pragma comment(lib, "ws2_32.lib")

// 全局配置
static char g_ProxyIp[64] = "127.0.0.1";
static unsigned short g_ProxyPort = 10808;
static char g_LogPath[MAX_PATH] = "proxy_hook.log";
static HMODULE g_hModule = NULL;
static DWORD g_lastConfigCheck = 0;

// 原函数指针
typedef int (WSAAPI* PFN_CONNECT)(SOCKET s, const sockaddr* name, int namelen);
typedef int (WSAAPI* PFN_WSACONNECT)(SOCKET s, const sockaddr* name, int namelen, LPWSABUF lpCallerData, LPWSABUF lpCalleeData, LPQOS lpSQOS, LPQOS lpGQOS);

static PFN_CONNECT pOriginalConnect = NULL;
static PFN_WSACONNECT pOriginalWSAConnect = NULL;

// 线程内部重入标记
static thread_local BOOL g_InHook = FALSE;

static void WriteLog(const char* format, ...) {
    FILE* fp = fopen(g_LogPath, "a");
    if (!fp) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(fp, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, format);
    vfprintf(fp, format, args);
    va_end(args);

    fprintf(fp, "\n");
    fclose(fp);
}

// 加载同目录下的 proxy_config.ini
static void LoadConfig(HMODULE hModule) {
    if (hModule) g_hModule = hModule;
    char modulePath[MAX_PATH];
    if (GetModuleFileNameA(g_hModule, modulePath, MAX_PATH)) {
        char* lastSlash = strrchr(modulePath, '\\');
        if (lastSlash) {
            *lastSlash = '\0';
            snprintf(g_LogPath, sizeof(g_LogPath), "%s\\proxy_hook.log", modulePath);
            
            char iniPath[MAX_PATH];
            snprintf(iniPath, sizeof(iniPath), "%s\\proxy_config.ini", modulePath);
            
            GetPrivateProfileStringA("Proxy", "Host", "127.0.0.1", g_ProxyIp, sizeof(g_ProxyIp), iniPath);
            unsigned short p = (unsigned short)GetPrivateProfileIntA("Proxy", "Port", 0, iniPath);
            if (p != 0) g_ProxyPort = p;
        }
    }
}

static void RefreshConfigIfNeeded() {
    DWORD now = GetTickCount();
    if (now - g_lastConfigCheck > 1000) { // 至少 1 秒检查一次
        g_lastConfigCheck = now;
        unsigned short oldPort = g_ProxyPort;
        LoadConfig(NULL);
        if (oldPort != g_ProxyPort) {
            WriteLog("[配置热更新] 代理端口从 %u 动态更新为 %u", oldPort, g_ProxyPort);
        }
    }
}

// 确保在阻塞模式下发送完所有字节
static BOOL SendAll(SOCKET s, const char* buf, int len) {
    int total = 0;
    while (total < len) {
        int n = send(s, buf + total, len - total, 0);
        if (n <= 0) return FALSE;
        total += n;
    }
    return TRUE;
}

// 确保在阻塞模式下接收完指定字节
static BOOL RecvAll(SOCKET s, char* buf, int len) {
    int total = 0;
    while (total < len) {
        int n = recv(s, buf + total, len - total, 0);
        if (n <= 0) return FALSE;
        total += n;
    }
    return TRUE;
}

// 执行 Socks5 TCP 握手 (IPv4)
static BOOL DoSocks5Handshake(SOCKET s, unsigned long target_ip_be, unsigned short target_port_be) {
    // 1. 发送版本与认证协商：\x05 \x01 \x00 (Socks5, 1种认证, 无认证)
    char authReq[3] = { 0x05, 0x01, 0x00 };
    if (!SendAll(s, authReq, 3)) {
        WriteLog("[Socks5] 发送认证请求失败 (WSAGetLastError=%d)", WSAGetLastError());
        return FALSE;
    }

    char authResp[2];
    if (!RecvAll(s, authResp, 2)) {
        WriteLog("[Socks5] 接收认证应答失败");
        return FALSE;
    }

    if (authResp[0] != 0x05 || authResp[1] != 0x00) {
        WriteLog("[Socks5] 认证协商不被支持: 0x%02X 0x%02X", (unsigned char)authResp[0], (unsigned char)authResp[1]);
        return FALSE;
    }

    // 2. 发送 CONNECT 命令：\x05 \x01 \x00 \x01 + 4字节IP + 2字节Port
    char connReq[10];
    connReq[0] = 0x05; // VER
    connReq[1] = 0x01; // CMD: CONNECT
    connReq[2] = 0x00; // RSV
    connReq[3] = 0x01; // ATYP: IPv4
    memcpy(&connReq[4], &target_ip_be, 4);
    memcpy(&connReq[8], &target_port_be, 2);

    if (!SendAll(s, connReq, 10)) {
        WriteLog("[Socks5] 发送 CONNECT 请求失败");
        return FALSE;
    }

    // 3. 读取 CONNECT 响应
    char connRespHead[4];
    if (!RecvAll(s, connRespHead, 4)) {
        WriteLog("[Socks5] 读取 CONNECT 响应头失败");
        return FALSE;
    }

    if (connRespHead[1] != 0x00) {
        WriteLog("[Socks5] 代理连接目标失败，REP 错误码: 0x%02X", (unsigned char)connRespHead[1]);
        return FALSE;
    }

    // 读取剩余的绑定地址信息（根据 ATYP 确定长度）
    int remainingLen = 0;
    if (connRespHead[3] == 0x01) { // IPv4 (4字节 IP + 2字节 Port)
        remainingLen = 4 + 2;
    } else if (connRespHead[3] == 0x03) { // 域名 (1字节长度 + N + 2字节 Port)
        char domainLen = 0;
        if (!RecvAll(s, &domainLen, 1)) return FALSE;
        remainingLen = (unsigned char)domainLen + 2;
    } else if (connRespHead[3] == 0x04) { // IPv6 (16字节 + 2字节 Port)
        remainingLen = 16 + 2;
    }

    if (remainingLen > 0) {
        char dummy[260];
        if (!RecvAll(s, dummy, remainingLen)) return FALSE;
    }

    return TRUE;
}

// 统一的连接处理逻辑
static int HandleRedirectConnect(SOCKET s, const sockaddr* name, int namelen, BOOL isWSA, LPWSABUF lpCallerData, LPWSABUF lpCalleeData, LPQOS lpSQOS, LPQOS lpGQOS) {
    RefreshConfigIfNeeded();

    if (g_InHook) {
        if (isWSA) return pOriginalWSAConnect(s, name, namelen, lpCallerData, lpCalleeData, lpSQOS, lpGQOS);
        return pOriginalConnect(s, name, namelen);
    }

    if (name && name->sa_family == AF_INET) {
        sockaddr_in* target = (sockaddr_in*)name;
        unsigned long targetIp = target->sin_addr.s_addr;
        unsigned short targetPort = target->sin_port;

        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(target->sin_addr), ipStr, INET_ADDRSTRLEN);
        int portNum = ntohs(targetPort);

        // 如果是本机环回 (127.0.0.1)，不进行重定向
        if (targetIp == inet_addr("127.0.0.1")) {
            if (isWSA) return pOriginalWSAConnect(s, name, namelen, lpCallerData, lpCalleeData, lpSQOS, lpGQOS);
            return pOriginalConnect(s, name, namelen);
        }

        WriteLog("[TCP 拦截] 检测到目标连接: %s:%d，准备重定向至本地代理 %s:%d", ipStr, portNum, g_ProxyIp, g_ProxyPort);

        // 构造本地代理地址
        sockaddr_in proxyAddr;
        memset(&proxyAddr, 0, sizeof(proxyAddr));
        proxyAddr.sin_family = AF_INET;
        proxyAddr.sin_port = htons(g_ProxyPort);
        proxyAddr.sin_addr.s_addr = inet_addr(g_ProxyIp);

        // 临时切换至阻塞模式完成 Socks5 握手
        u_long nonBlocking = 0;
        ioctlsocket(s, FIONBIO, &nonBlocking);

        // 设置重入保护
        g_InHook = TRUE;
        int connectRes = pOriginalConnect(s, (sockaddr*)&proxyAddr, sizeof(proxyAddr));
        g_InHook = FALSE;

        if (connectRes == 0) {
            // 连接本地代理成功，立刻执行 Socks5 握手
            if (DoSocks5Handshake(s, targetIp, targetPort)) {
                WriteLog("[TCP 成功] 已成功打通 Socks5 隧道并连接目标: %s:%d", ipStr, portNum);
                return 0; // 返回成功骗过游戏
            } else {
                WriteLog("[TCP 错误] Socks5 握手失败: %s:%d", ipStr, portNum);
            }
        } else {
            WriteLog("[TCP 错误] 连接本地代理 %s:%d 失败 (WSAGetLastError=%d)", g_ProxyIp, g_ProxyPort, WSAGetLastError());
        }

        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }

    if (isWSA) return pOriginalWSAConnect(s, name, namelen, lpCallerData, lpCalleeData, lpSQOS, lpGQOS);
    return pOriginalConnect(s, name, namelen);
}

// Hook 包装函数
int WSAAPI HookedConnect(SOCKET s, const sockaddr* name, int namelen) {
    return HandleRedirectConnect(s, name, namelen, FALSE, NULL, NULL, NULL, NULL);
}

int WSAAPI HookedWSAConnect(SOCKET s, const sockaddr* name, int namelen, LPWSABUF lpCallerData, LPWSABUF lpCalleeData, LPQOS lpSQOS, LPQOS lpGQOS) {
    return HandleRedirectConnect(s, name, namelen, TRUE, lpCallerData, lpCalleeData, lpSQOS, lpGQOS);
}

// DLL 入口点
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        LoadConfig(hModule);
        WriteLog("=== Hook DLL 载入进程 (PID: %lu) ===", GetCurrentProcessId());

        if (MH_Initialize() == MH_OK) {
            MH_CreateHookApi(L"ws2_32.dll", "connect", (LPVOID)&HookedConnect, (LPVOID*)&pOriginalConnect);
            MH_CreateHookApi(L"ws2_32.dll", "WSAConnect", (LPVOID)&HookedWSAConnect, (LPVOID*)&pOriginalWSAConnect);
            MH_EnableHook(MH_ALL_HOOKS);
            WriteLog("Winsock Hook 已成功激活！");
        } else {
            WriteLog("MinHook 初始化失败！");
        }
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        WriteLog("=== Hook DLL 卸载 ===");
    }
    return TRUE;
}
