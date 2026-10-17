#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <stdio.h>
#include <stdlib.h>
#include <unordered_map>
#include <mutex>
#include "../minhook/include/MinHook.h"

#pragma comment(lib, "ws2_32.lib")

// 全局配置
static char g_ProxyIp[64] = "127.0.0.1";
static unsigned short g_ProxyPort = 10808;
static BOOL g_ProxyEnabled = TRUE;
static char g_LogPath[MAX_PATH] = "proxy_hook.log";
static HMODULE g_hModule = NULL;
static DWORD g_lastConfigCheck = 0;

// 原函数指针
typedef int (WSAAPI* PFN_CONNECT)(SOCKET s, const sockaddr* name, int namelen);
typedef int (WSAAPI* PFN_WSACONNECT)(SOCKET s, const sockaddr* name, int namelen, LPWSABUF lpCallerData, LPWSABUF lpCalleeData, LPQOS lpSQOS, LPQOS lpGQOS);
typedef int (WSAAPI* PFN_IOCTLSOCKET)(SOCKET s, long cmd, u_long* argp);
typedef int (WSAAPI* PFN_WSAEVENTSELECT)(SOCKET s, WSAEVENT hEventObject, long lNetworkEvents);
typedef int (WSAAPI* PFN_WSAASYNCSELECT)(SOCKET s, HWND hWnd, u_int wMsg, long lEvent);
typedef BOOL (PASCAL FAR * LPFN_CONNECTEX)(SOCKET s, const struct sockaddr FAR *name, int namelen, PVOID lpSendBuffer, DWORD dwSendDataLength, LPDWORD lpdwBytesSent, LPOVERLAPPED lpOverlapped);
typedef int (WSAAPI* PFN_WSAIOCTL)(SOCKET s, DWORD dwIoControlCode, LPVOID lpvInBuffer, DWORD cbInBuffer, LPVOID lpvOutBuffer, DWORD cbOutBuffer, LPDWORD lpcbBytesReturned, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);

static PFN_CONNECT pOriginalConnect = NULL;
static PFN_WSACONNECT pOriginalWSAConnect = NULL;
static PFN_IOCTLSOCKET pOriginalIoctlsocket = NULL;
static PFN_WSAEVENTSELECT pOriginalWSAEventSelect = NULL;
static PFN_WSAASYNCSELECT pOriginalWSAAsyncSelect = NULL;
static LPFN_CONNECTEX pOriginalConnectEx = NULL;
static PFN_WSAIOCTL pOriginalWSAIoctl = NULL;

// 跟踪每个 Socket 的非阻塞状态
struct SocketContext {
    bool isNonBlocking;
    WSAEVENT hEvent;
    long networkEvents;
    HWND hWnd;
    u_int wMsg;
    long asyncEvents;
};

static std::mutex g_SocketMutex;
static std::unordered_map<SOCKET, SocketContext> g_SocketMap;

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
            g_ProxyEnabled = (GetPrivateProfileIntA("Proxy", "Enabled", 1, iniPath) != 0);
            unsigned short p = (unsigned short)GetPrivateProfileIntA("Proxy", "Port", 0, iniPath);
            if (p != 0) g_ProxyPort = p;
        }
    }
}

static void RefreshConfigIfNeeded() {
    DWORD now = GetTickCount();
    if (now - g_lastConfigCheck > 1000) {
        g_lastConfigCheck = now;
        unsigned short oldPort = g_ProxyPort;
        LoadConfig(NULL);
        if (oldPort != g_ProxyPort) {
            WriteLog("[配置热更新] 代理端口从 %u 动态更新为 %u", oldPort, g_ProxyPort);
        }
    }
}

// 确保跨平台兼容阻塞与非阻塞套接字的精准超时发送
static BOOL SendAllTimed(SOCKET s, const char* buf, int len, int timeoutMs = 2500) {
    int total = 0;
    while (total < len) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        int sel = select(0, NULL, &wfds, NULL, &tv);
        if (sel <= 0) return FALSE;

        int n = send(s, buf + total, len - total, 0);
        if (n <= 0) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                Sleep(1);
                continue;
            }
            return FALSE;
        }
        total += n;
    }
    return TRUE;
}

// 确保跨平台兼容阻塞与非阻塞套接字的精准超时接收
static BOOL RecvAllTimed(SOCKET s, char* buf, int len, int timeoutMs = 2500) {
    int total = 0;
    while (total < len) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        int sel = select(0, &rfds, NULL, NULL, &tv);
        if (sel <= 0) return FALSE;

        int n = recv(s, buf + total, len - total, 0);
        if (n <= 0) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                Sleep(1);
                continue;
            }
            return FALSE;
        }
        total += n;
    }
    return TRUE;
}

// 执行 Socks5 TCP 握手 (IPv4，基于 select 超时驱动，完全无损兼容非阻塞套接字)
static BOOL DoSocks5Handshake(SOCKET s, unsigned long target_ip_be, unsigned short target_port_be) {
    // 1. 发送版本与认证协商：\x05 \x01 \x00 (Socks5, 1种认证, 无认证)
    char authReq[3] = { 0x05, 0x01, 0x00 };
    if (!SendAllTimed(s, authReq, 3)) {
        WriteLog("[Socks5] 发送认证请求失败 (WSAGetLastError=%d)", WSAGetLastError());
        return FALSE;
    }

    char authResp[2];
    if (!RecvAllTimed(s, authResp, 2)) {
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

    if (!SendAllTimed(s, connReq, 10)) {
        WriteLog("[Socks5] 发送 CONNECT 请求失败");
        return FALSE;
    }

    // 3. 读取 CONNECT 响应
    char connRespHead[4];
    if (!RecvAllTimed(s, connRespHead, 4)) {
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
        if (!RecvAllTimed(s, &domainLen, 1)) return FALSE;
        remainingLen = (unsigned char)domainLen + 2;
    } else if (connRespHead[3] == 0x04) { // IPv6 (16字节 + 2字节 Port)
        remainingLen = 16 + 2;
    }

    if (remainingLen > 0) {
        char dummy[260];
        if (!RecvAllTimed(s, dummy, remainingLen)) return FALSE;
    }

    return TRUE;
}

// 智能识别是否为游戏核心网络流量
static bool ShouldAccelerate(const sockaddr_in* target) {
    if (!target) return false;
    unsigned long ip = target->sin_addr.s_addr;
    if (ip == inet_addr("127.0.0.1")) return false; // 本地环回永不加速

    int port = ntohs(target->sin_port);

    // 绝对放行 HTTP / HTTPS Web 资源、CDN 贴图流式下载 (例如 146.59.238.42:80)
    // 彻底防止 cURL 状态紊乱导致 CDN hosts exhausted 与 funcs.cpp(100) 贴图解码崩溃
    if (port == 80 || port == 443 || port == 8080) {
        return false;
    }

    // 核心加速目标 1：魔兽世界专有端口
    // 1119: 战网认证登录 (Logon Portal: eu.sl.logon.firestorm-servers.com)
    // 3724: 经典魔兽认证/选服端口
    // 8285, 8286: Firestorm 核心世界服务器
    // 8085, 8086: 常用私服世界服端口
    if (port == 1119 || port == 3724 || port == 8285 || port == 8286 || port == 8085 || port == 8086) {
        return true;
    }

    // 核心加速目标 2：Firestorm 官方世界服 IP (141.95.207.63) 的游戏服务
    if (ip == inet_addr("141.95.207.63")) {
        return true;
    }

    // 核心加速目标 3：魔兽私服世界服务器高位端口区间 (8000 ~ 9999 且非 8080)
    if (port >= 8000 && port <= 9999) {
        return true;
    }

    return false;
}

// 统一的连接处理逻辑
static int HandleRedirectConnect(SOCKET s, const sockaddr* name, int namelen, BOOL isWSA, LPWSABUF lpCallerData, LPWSABUF lpCalleeData, LPQOS lpSQOS, LPQOS lpGQOS) {
    RefreshConfigIfNeeded();

    if (g_InHook || !g_ProxyEnabled || g_ProxyPort == 0) {
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

        // 如果不是游戏核心端口（如 CDN/Web/遥测/本机环回），直接放行原生直连，确保 100% 顺畅
        if (!ShouldAccelerate(target)) {
            if (isWSA) return pOriginalWSAConnect(s, name, namelen, lpCallerData, lpCalleeData, lpSQOS, lpGQOS);
            return pOriginalConnect(s, name, namelen);
        }

        WriteLog("[TCP 拦截] 检测到游戏核心连接: %s:%d，准备重定向至本地专线 %s:%d", ipStr, portNum, g_ProxyIp, g_ProxyPort);

        // 构造本地代理地址
        sockaddr_in proxyAddr;
        memset(&proxyAddr, 0, sizeof(proxyAddr));
        proxyAddr.sin_family = AF_INET;
        proxyAddr.sin_port = htons(g_ProxyPort);
        proxyAddr.sin_addr.s_addr = inet_addr(g_ProxyIp);

        // 设置重入保护，连接本地环回代理 (127.0.0.1)
        g_InHook = TRUE;
        int connectRes = pOriginalConnect(s, (sockaddr*)&proxyAddr, sizeof(proxyAddr));
        int lastErr = WSAGetLastError();
        g_InHook = FALSE;

        // 如果原套接字配置为非阻塞模式，connect 会返回 WSAEWOULDBLOCK，用 select 等待本地环回建立连接 (通常 < 1ms)
        if (connectRes != 0 && lastErr == WSAEWOULDBLOCK) {
            fd_set wfds, efds;
            FD_ZERO(&wfds);
            FD_ZERO(&efds);
            FD_SET(s, &wfds);
            FD_SET(s, &efds);
            timeval tv = { 1, 500000 };
            int sel = select(0, NULL, &wfds, &efds, &tv);
            if (sel > 0 && FD_ISSET(s, &wfds)) {
                int so_error = 0;
                int errLen = sizeof(so_error);
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&so_error, &errLen);
                if (so_error == 0) {
                    connectRes = 0;
                }
            }
        }

        if (connectRes == 0) {
            // 启用 TCP_NODELAY，禁用 Nagle 算法，消除世界服心跳包 40~200ms 的积压延迟
            BOOL bNoDelay = TRUE;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&bNoDelay, sizeof(bNoDelay));

            // 执行 Socks5 握手 (由 select 驱动，完全无损兼容非阻塞套接字，绝不触碰和破坏 WSAEventSelect 内部记录！)
            if (DoSocks5Handshake(s, targetIp, targetPort)) {
                WriteLog("[TCP 成功] 已打通 Socks5 隧道并连接目标: %s:%d", ipStr, portNum);

                // 唤醒游戏可能关联的事件句柄，但绝不调用 WSAEventSelect 清空内部事件！
                {
                    std::lock_guard<std::mutex> lock(g_SocketMutex);
                    auto it = g_SocketMap.find(s);
                    if (it != g_SocketMap.end()) {
                        if (it->second.hEvent != NULL) {
                            SetEvent(it->second.hEvent);
                        }
                        if (it->second.hWnd != NULL && it->second.wMsg != 0) {
                            PostMessageW(it->second.hWnd, it->second.wMsg, (WPARAM)s, WSAMAKESELECTREPLY(FD_CONNECT, 0));
                            PostMessageW(it->second.hWnd, it->second.wMsg, (WPARAM)s, WSAMAKESELECTREPLY(FD_WRITE, 0));
                        }
                    }
                }

                WSASetLastError(0);
                return 0; // 返回成功骗过游戏
            } else {
                WriteLog("[TCP 错误] Socks5 握手失败: %s:%d，主动重置连接防止脏状态", ipStr, portNum);
                closesocket(s);
                WSASetLastError(WSAECONNREFUSED);
                return SOCKET_ERROR;
            }
        } else {
            WriteLog("[自动直连旁路] 连接本地代理 %s:%d 失败，自动平滑直连原始目标: %s:%d", g_ProxyIp, g_ProxyPort, ipStr, portNum);
            if (isWSA) return pOriginalWSAConnect(s, name, namelen, lpCallerData, lpCalleeData, lpSQOS, lpGQOS);
            return pOriginalConnect(s, name, namelen);
        }
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

// Hook ioctlsocket 捕获游戏设置非阻塞
int WSAAPI HookedIoctlsocket(SOCKET s, long cmd, u_long* argp) {
    if (cmd == FIONBIO && argp) {
        std::lock_guard<std::mutex> lock(g_SocketMutex);
        g_SocketMap[s].isNonBlocking = (*argp != 0);
    }
    return pOriginalIoctlsocket(s, cmd, argp);
}

// Hook WSAEventSelect 捕获游戏事件通知绑定
int WSAAPI HookedWSAEventSelect(SOCKET s, WSAEVENT hEventObject, long lNetworkEvents) {
    if (hEventObject != NULL) {
        std::lock_guard<std::mutex> lock(g_SocketMutex);
        g_SocketMap[s].isNonBlocking = true;
        g_SocketMap[s].hEvent = hEventObject;
        g_SocketMap[s].networkEvents = lNetworkEvents;
    }
    return pOriginalWSAEventSelect(s, hEventObject, lNetworkEvents);
}

// Hook WSAAsyncSelect 捕获消息窗口通知绑定
int WSAAPI HookedWSAAsyncSelect(SOCKET s, HWND hWnd, u_int wMsg, long lEvent) {
    if (hWnd != NULL) {
        std::lock_guard<std::mutex> lock(g_SocketMutex);
        g_SocketMap[s].isNonBlocking = true;
        g_SocketMap[s].hWnd = hWnd;
        g_SocketMap[s].wMsg = wMsg;
        g_SocketMap[s].asyncEvents = lEvent;
    }
    return pOriginalWSAAsyncSelect(s, hWnd, wMsg, lEvent);
}

// Hook ConnectEx (异步 IOCP 连接拦截)
BOOL PASCAL FAR HookedConnectEx(
    SOCKET s,
    const struct sockaddr FAR *name,
    int namelen,
    PVOID lpSendBuffer,
    DWORD dwSendDataLength,
    LPDWORD lpdwBytesSent,
    LPOVERLAPPED lpOverlapped
) {
    // 异步 ConnectEx 在没有完整的非阻塞 Socks5 Overlapped 状态机的情况下，
    // 若强行改写到 127.0.0.1 会导致完成端口在无握手情况下收到业务流而断开。
    // 因此对于异步 IOCP ConnectEx 调用，必须优先放行原始直连，确保 IOCP 链路完全健康！
    if (pOriginalConnectEx) {
        return pOriginalConnectEx(s, name, namelen, lpSendBuffer, dwSendDataLength, lpdwBytesSent, lpOverlapped);
    }
    return FALSE;
}

// Hook WSAIoctl 捕获扩展函数指针查询
int WSAAPI HookedWSAIoctl(
    SOCKET s,
    DWORD dwIoControlCode,
    LPVOID lpvInBuffer,
    DWORD cbInBuffer,
    LPVOID lpvOutBuffer,
    DWORD cbOutBuffer,
    LPDWORD lpcbBytesReturned,
    LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine
) {
    int res = pOriginalWSAIoctl(s, dwIoControlCode, lpvInBuffer, cbInBuffer, lpvOutBuffer, cbOutBuffer, lpcbBytesReturned, lpOverlapped, lpCompletionRoutine);
    if (res == 0 && dwIoControlCode == SIO_GET_EXTENSION_FUNCTION_POINTER && lpvInBuffer && lpvOutBuffer) {
        GUID* pGuid = (GUID*)lpvInBuffer;
        GUID guidConnectEx = WSAID_CONNECTEX;
        if (memcmp(pGuid, &guidConnectEx, sizeof(GUID)) == 0) {
            LPFN_CONNECTEX* ppConnectEx = (LPFN_CONNECTEX*)lpvOutBuffer;
            if (*ppConnectEx && *ppConnectEx != HookedConnectEx) {
                if (!pOriginalConnectEx) {
                    pOriginalConnectEx = *ppConnectEx;
                }
                *ppConnectEx = HookedConnectEx;
                WriteLog("[WSAIoctl 拦截] 拦截到 WSAID_CONNECTEX 函数指针获取，已成功替换为 HookedConnectEx！");
            }
        }
    }
    return res;
}

// 主动获取并 Hook ConnectEx
static void TryHookConnectExDirectly() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) {
        GUID guidConnectEx = WSAID_CONNECTEX;
        LPFN_CONNECTEX pConnectEx = NULL;
        DWORD dwBytes = 0;
        int rc = WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER,
            &guidConnectEx, sizeof(guidConnectEx),
            &pConnectEx, sizeof(pConnectEx),
            &dwBytes, NULL, NULL);
        closesocket(s);

        if (rc == 0 && pConnectEx) {
            if (MH_CreateHook((LPVOID)pConnectEx, (LPVOID)&HookedConnectEx, (LPVOID*)&pOriginalConnectEx) == MH_OK) {
                MH_EnableHook((LPVOID)pConnectEx);
                WriteLog("[主动 Hook] 成功对 mswsock!ConnectEx 挂接行内钩子！地址: %p", pConnectEx);
            }
        }
    }
}

// 导出版本标识供外部注入检查（防二次注入）
extern "C" __declspec(dllexport) DWORD WINAPI GameTcpSpeed_GetVersion() {
    return 0x020000; // v2.0.0
}

extern "C" __declspec(dllexport) void WINAPI GameTcpSpeed_Disable() {
    g_ProxyEnabled = FALSE;
    WriteLog("[主动旁路] 已手动禁用专线代理重定向，恢复原生直连");
}

extern "C" __declspec(dllexport) void WINAPI GameTcpSpeed_Enable() {
    g_ProxyEnabled = TRUE;
    WriteLog("[主动启用] 已启用专线代理重定向");
}

// DLL 入口点
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        LoadConfig(hModule);
        WriteLog("=== Hook DLL v2.0 载入进程 (PID: %lu) ===", GetCurrentProcessId());

        if (MH_Initialize() == MH_OK) {
            MH_CreateHookApi(L"ws2_32.dll", "connect", (LPVOID)&HookedConnect, (LPVOID*)&pOriginalConnect);
            MH_CreateHookApi(L"ws2_32.dll", "WSAConnect", (LPVOID)&HookedWSAConnect, (LPVOID*)&pOriginalWSAConnect);
            MH_CreateHookApi(L"ws2_32.dll", "ioctlsocket", (LPVOID)&HookedIoctlsocket, (LPVOID*)&pOriginalIoctlsocket);
            MH_CreateHookApi(L"ws2_32.dll", "WSAEventSelect", (LPVOID)&HookedWSAEventSelect, (LPVOID*)&pOriginalWSAEventSelect);
            MH_CreateHookApi(L"ws2_32.dll", "WSAAsyncSelect", (LPVOID)&HookedWSAAsyncSelect, (LPVOID*)&pOriginalWSAAsyncSelect);
            MH_CreateHookApi(L"ws2_32.dll", "WSAIoctl", (LPVOID)&HookedWSAIoctl, (LPVOID*)&pOriginalWSAIoctl);
            MH_EnableHook(MH_ALL_HOOKS);

            // 主动拦截异步 IOCP ConnectEx (针对魔兽世界等大型 3D 游戏已运行状态免重启接管)
            TryHookConnectExDirectly();

            WriteLog("Winsock Hook v2.0 (全套 TCP connect + 异步 ConnectEx + WSAIoctl) 已成功激活！");
        } else {
            WriteLog("MinHook 初始化失败！");
        }
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        WriteLog("=== Hook DLL v2.0 卸载 ===");
    }
    return TRUE;
}
