#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <stdio.h>
#include <stdlib.h>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include "../minhook/include/MinHook.h"

#pragma comment(lib, "ws2_32.lib")

// 全局配置
static char g_ProxyIp[64] = "127.0.0.1";
static unsigned short g_ProxyPort = 58588;
static BOOL g_ProxyEnabled = TRUE;
static char g_LogPath[MAX_PATH] = "proxy_hook.log";
static HMODULE g_hModule = NULL;
static DWORD g_lastConfigCheck = 0;

// 活动 Hook 调用计数屏障，防止卸载/禁用时并发线程崩溃
static std::atomic<int> g_ActiveHookCount(0);
static std::atomic<bool> g_IsDetaching(false);

// 保护 g_ProxyIp / g_ProxyPort / g_ProxyEnabled 的并发访问。
// 多开时同一进程内会有多个游戏线程同时调用 connect，而配置刷新是写操作，
// 无锁会导致读到半更新的 IP 字符串。
static std::mutex g_ConfigMutex;

// 原函数指针 (仅拦截 connect 与 WSAConnect，实现极简纯净加速，零干扰过图与 Socket 生命周期)
typedef int (WSAAPI* PFN_CONNECT)(SOCKET s, const sockaddr* name, int namelen);
typedef int (WSAAPI* PFN_WSACONNECT)(SOCKET s, const sockaddr* name, int namelen, LPWSABUF lpCallerData, LPWSABUF lpCalleeData, LPQOS lpSQOS, LPQOS lpGQOS);

static PFN_CONNECT pOriginalConnect = NULL;
static PFN_WSACONNECT pOriginalWSAConnect = NULL;

// 线程内部重入标记。
// 原实现是 static thread_local：手动映射注入时镜像不经过 Windows 加载器，
// 静态 TLS 目录没有任何人初始化（_tls_index 无值、TEB TLS 槽为空），
// 游戏线程一进 HookedConnect 就会写到空槽触发崩溃。
// 改为 TlsAlloc 动态槽位：进程级索引，任意线程 TlsGetValue/TlsSetValue 都安全，
// 槽位未设置时 TlsGetValue 只会返回 NULL，不会解引用野指针。
static DWORD g_TlsInHook = TLS_OUT_OF_INDEXES;

static inline BOOL InHookGet() {
    if (g_TlsInHook == TLS_OUT_OF_INDEXES) return FALSE;
    return TlsGetValue(g_TlsInHook) != NULL;
}

static inline void InHookSet(BOOL v) {
    if (g_TlsInHook != TLS_OUT_OF_INDEXES)
        TlsSetValue(g_TlsInHook, v ? (LPVOID)(ULONG_PTR)1 : NULL);
}

static void WriteLog(const char* format, ...) {
    // 超过 5MB 自动截断清理，防止多开长时间运行导致磁盘膨胀
    static int s_logCount = 0;
    if (++s_logCount % 100 == 0) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(g_LogPath, GetFileExInfoStandard, &fad)) {
            if (fad.nFileSizeLow > 5 * 1024 * 1024) {
                FILE* fpTrunc = fopen(g_LogPath, "w");
                if (fpTrunc) {
                    fprintf(fpTrunc, "=== 日志达到 5MB 上限，已自动滚动清空重置 ===\n");
                    fclose(fpTrunc);
                }
            }
        }
    }

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

// 从「DLL 在磁盘上的完整路径」推导日志与配置路径。
// 普通 LoadLibrary 注入时由 GetModuleFileNameA 得到该路径；
// 手动映射注入时镜像不在模块表里，GetModuleFileName 拿不到，必须由注入方经 GTS_Init 传入。
static void LoadConfigFromPath(const char* dllDiskPath) {
    if (!dllDiskPath || !*dllDiskPath) return;

    char dir[MAX_PATH];
    strncpy(dir, dllDiskPath, MAX_PATH - 1);
    dir[MAX_PATH - 1] = '\0';

    char* lastSlash = strrchr(dir, '\\');
    if (!lastSlash) lastSlash = strrchr(dir, '/');
    if (!lastSlash) return;
    *lastSlash = '\0';

    snprintf(g_LogPath, sizeof(g_LogPath), "%s\\proxy_hook.log", dir);

    char iniPath[MAX_PATH];
    snprintf(iniPath, sizeof(iniPath), "%s\\proxy_config.ini", dir);

    GetPrivateProfileStringA("Proxy", "Host", "127.0.0.1", g_ProxyIp, sizeof(g_ProxyIp), iniPath);
    g_ProxyEnabled = (GetPrivateProfileIntA("Proxy", "Enabled", 1, iniPath) != 0);
    unsigned short p = (unsigned short)GetPrivateProfileIntA("Proxy", "Port", 0, iniPath);
    if (p != 0) g_ProxyPort = p;
}

// 手动映射时由 GTS_Init 预置的 DLL 磁盘路径（普通注入时为 NULL）
// 必须拷贝到本模块自己的静态缓冲区：注入方在 GTS_Init 返回后就会释放那块远端内存，
// 而钩子内部每秒都会 LoadConfig(NULL) 热重载配置，直接持有外部指针必然变成野指针。
static char g_ExternalDllPathBuf[MAX_PATH] = {0};
static const char* g_ExternalDllPath = NULL;

// 加载同目录下的 proxy_config.ini（普通注入路径）
static void LoadConfig(HMODULE hModule) {
    if (hModule) g_hModule = hModule;

    // 手动映射的镜像不在模块表里，GetModuleFileNameA 必然失败，改用注入方传入的路径
    if (g_ExternalDllPath && *g_ExternalDllPath) {
        LoadConfigFromPath(g_ExternalDllPath);
        return;
    }

    char modulePath[MAX_PATH];
    if (GetModuleFileNameA(g_hModule, modulePath, MAX_PATH))
        LoadConfigFromPath(modulePath);
}

static BOOL g_ProxyAlive = TRUE;

// 快速检测本地代理端口是否实际开放监听
static bool CheckProxyPortAlive(const char* ip, unsigned short port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);

    int rc = pOriginalConnect ? pOriginalConnect(s, (sockaddr*)&addr, sizeof(addr)) : connect(s, (sockaddr*)&addr, sizeof(addr));
    bool alive = false;
    if (rc == 0) {
        alive = true;
    } else {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(s, &wfds);
            timeval tv = { 0, 50000 }; // 50ms 快速探针
            if (select(0, NULL, &wfds, NULL, &tv) > 0) {
                int so_error = 0;
                int len = sizeof(so_error);
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
                if (so_error == 0) alive = true;
            }
        }
    }
    closesocket(s);
    return alive;
}

// ==================== 目标级失败记忆（自愈降级） ====================
// 用途：某个 目标:端口 在隧道内握手超时/失败后，60 秒内该目标改走原生直连。
// 背景：世界服 8285 偶发隧道内握手超时，若每次重连都继续走隧道，游戏会一直卡在
//       "连接失败" 回不到游戏；改为短期直连可立刻恢复可玩，到期后自动恢复加速。
static std::mutex g_TargetFailMutex;
static std::unordered_map<unsigned long long, unsigned long long> g_TargetFailUntil;

static inline unsigned long long TargetFailKey(unsigned long ip, unsigned short port) {
    return ((unsigned long long)ip << 16) | (unsigned long long)port;
}

static bool IsTargetFailedRecently(unsigned long ip, unsigned short port) {
    std::lock_guard<std::mutex> lock(g_TargetFailMutex);
    auto it = g_TargetFailUntil.find(TargetFailKey(ip, port));
    if (it == g_TargetFailUntil.end()) return false;
    if (GetTickCount64() >= it->second) {
        g_TargetFailUntil.erase(it);
        return false;
    }
    return true;
}

static void MarkTargetFailed(unsigned long ip, unsigned short port) {
    std::lock_guard<std::mutex> lock(g_TargetFailMutex);
    unsigned long long now = GetTickCount64();
    if (g_TargetFailUntil.size() > 64) {
        for (auto it = g_TargetFailUntil.begin(); it != g_TargetFailUntil.end();) {
            if (now >= it->second) it = g_TargetFailUntil.erase(it);
            else ++it;
        }
    }
    g_TargetFailUntil[TargetFailKey(ip, port)] = now + 60000ULL;
}

static void RefreshConfigIfNeeded() {    DWORD now = GetTickCount();
    if (now - g_lastConfigCheck > 1000) {
        g_lastConfigCheck = now;
        unsigned short oldPort = g_ProxyPort;
        {
            std::lock_guard<std::mutex> lock(g_ConfigMutex);
            LoadConfig(NULL);
        }
        if (oldPort != g_ProxyPort) {
            WriteLog("[配置热更新] 代理端口从 %u 动态更新为 %u", oldPort, g_ProxyPort);
        }
        if (g_ProxyEnabled && g_ProxyPort > 0) {
            g_ProxyAlive = CheckProxyPortAlive(g_ProxyIp, g_ProxyPort);
        } else {
            g_ProxyAlive = FALSE;
        }
    }
}

// 加锁取一份配置快照，供本次连接使用。
// 直接读全局变量会在配置刷新的瞬间读到半更新的字符串。
static void SnapshotConfig(char* ipOut, size_t ipLen, unsigned short* portOut, BOOL* enabledOut, BOOL* aliveOut) {
    std::lock_guard<std::mutex> lock(g_ConfigMutex);
    strncpy(ipOut, g_ProxyIp, ipLen - 1);
    ipOut[ipLen - 1] = '\0';
    *portOut = g_ProxyPort;
    *enabledOut = g_ProxyEnabled;
    *aliveOut = g_ProxyAlive;
}

static int RemainingMs(ULONGLONG deadline) {
    ULONGLONG now = GetTickCount64();
    if (now >= deadline) return 0;
    ULONGLONG rem = deadline - now;
    return rem > 60000 ? 60000 : (int)rem;
}

// 确保跨平台兼容阻塞与非阻塞套接字的精准超时发送。
// 超时按绝对截止时间计算：早期实现每次循环都重置一次完整超时，
// 慢速传输下总耗时可能被放大数十倍，表现为游戏读条卡死。
static BOOL SendAllTimed(SOCKET s, const char* buf, int len, int timeoutMs = 2500) {
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeoutMs;
    int total = 0;
    while (total < len) {
        int remain = RemainingMs(deadline);
        if (remain <= 0) return FALSE;

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        timeval tv;
        tv.tv_sec = remain / 1000;
        tv.tv_usec = (remain % 1000) * 1000;
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

// 确保跨平台兼容阻塞与非阻塞套接字的精准超时接收（按绝对截止时间）
static BOOL RecvAllTimed(SOCKET s, char* buf, int len, int timeoutMs = 2500) {
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeoutMs;
    int total = 0;
    while (total < len) {
        int remain = RemainingMs(deadline);
        if (remain <= 0) return FALSE;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        timeval tv;
        tv.tv_sec = remain / 1000;
        tv.tv_usec = (remain % 1000) * 1000;
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
//
// 分阶段超时：认证协商在本地 sing-box 完成，毫秒级，2.5s 足够；
// CONNECT 响应对应「本地 sing-box 向上游节点拨号 + 上游连到游戏服」的完整链路，
// 跨国链路在正常负载下就可能到 1.5s 以上。双开时两条连接争抢同一上游节点会排队，
// 沿用 2.5s 会误判超时 —— 这正是多开卡死的直接原因。故 CONNECT 阶段放宽到 6s。
static BOOL DoSocks5Handshake(SOCKET s, unsigned long target_ip_be, unsigned short target_port_be,
                              int authTimeoutMs = 2500, int connectTimeoutMs = 6000) {
    // 1. 发送版本与认证协商：\x05 \x01 \x00 (Socks5, 1种认证, 无认证)
    char authReq[3] = { 0x05, 0x01, 0x00 };
    if (!SendAllTimed(s, authReq, 3, authTimeoutMs)) {
        WriteLog("[Socks5] 发送认证请求失败 (WSAGetLastError=%d)", WSAGetLastError());
        return FALSE;
    }

    char authResp[2];
    if (!RecvAllTimed(s, authResp, 2, authTimeoutMs)) {
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

    if (!SendAllTimed(s, connReq, 10, authTimeoutMs)) {
        WriteLog("[Socks5] 发送 CONNECT 请求失败");
        return FALSE;
    }

    // 3. 读取 CONNECT 响应（此阶段耗时最长，用放宽后的超时）
    char connRespHead[4];
    if (!RecvAllTimed(s, connRespHead, 4, connectTimeoutMs)) {
        WriteLog("[Socks5] 读取 CONNECT 响应头失败 (等待 %d ms 超时，可能是上游节点排队)", connectTimeoutMs);
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
        if (!RecvAllTimed(s, &domainLen, 1, connectTimeoutMs)) return FALSE;
        remainingLen = (unsigned char)domainLen + 2;
    } else if (connRespHead[3] == 0x04) { // IPv6 (16字节 + 2字节 Port)
        remainingLen = 16 + 2;
    }

    if (remainingLen > 0) {
        char dummy[260];
        if (!RecvAllTimed(s, dummy, remainingLen, connectTimeoutMs)) return FALSE;
    }

    return TRUE;
}

// 智能识别是否为游戏核心网络流量 (针对纯注入游戏进程环境)
static bool ShouldAccelerate(const sockaddr_in* target) {
    if (!target) return false;
    unsigned long ip = target->sin_addr.s_addr;
    if (ip == inet_addr("127.0.0.1")) return false; // 本地环回永不加速

    int port = ntohs(target->sin_port);

    // 绝对放行已知 CDN 贴图流式下载 (例如 146.59.238.42:80) 以及标准 Web HTTP 80
    // 彻底防止 cURL 状态紊乱导致 CDN hosts exhausted 与 funcs.cpp(100) 贴图解码崩溃
    if (ip == inet_addr("146.59.238.42") && port == 80) {
        return false;
    }
    if (port == 80) {
        return false;
    }

    // 核心加速目标 1：魔兽世界专有端口与私服常用登录/世界服端口
    // 1119: 战网认证登录 (Logon Portal: eu.sl.logon.firestorm-servers.com)
    // 3724: 经典魔兽认证/选服端口
    // 8285, 8286: Firestorm 核心世界服务器
    // 8085, 8086: 常用私服世界服端口
    // 8080: 私服登录/世界服非标端口 (解除误杀，保障各类私服连接)
    if (port == 1119 || port == 3724 || port == 8285 || port == 8286 || port == 8085 || port == 8086 || port == 8080) {
        return true;
    }

    // 核心加速目标 2：Firestorm 官方主服务器 IP (141.95.207.63) 的全部游戏通信
    if (ip == inet_addr("141.95.207.63")) {
        return true;
    }

    // 核心加速目标 3：魔兽私服世界服务器通用高位端口区间 (8000 ~ 9999)
    if (port >= 8000 && port <= 9999) {
        return true;
    }

    return false;
}

// 统一的连接处理逻辑
static int HandleRedirectConnect(SOCKET s, const sockaddr* name, int namelen, BOOL isWSA, LPWSABUF lpCallerData, LPWSABUF lpCalleeData, LPQOS lpSQOS, LPQOS lpGQOS) {
    RefreshConfigIfNeeded();

    // 1. 【核心防御】拦截 IPv6 流量：强制拒绝 AF_INET6，迫使魔兽世界客户端平滑回退 IPv4 专线！
    // 杜绝国内 IPv6 宽带下游戏走原生 IPv6 直连漏包，防止登录 IP 与游戏 IP 不一致被私服踢下线。
    if (name && name->sa_family == AF_INET6) {
        WriteLog("[IPv6 拦截] 捕获到游戏发起的 AF_INET6 连接请求，主动拒绝以迫使游戏回退 IPv4 专线！");
        WSASetLastError(WSAEAFNOSUPPORT);
        return SOCKET_ERROR;
    }

    // 取配置快照，避免本次连接期间被其它线程的配置刷新打断
    char proxyIp[64] = { 0 };
    unsigned short proxyPort = 0;
    BOOL proxyEnabled = FALSE;
    BOOL proxyAlive = FALSE;
    SnapshotConfig(proxyIp, sizeof(proxyIp), &proxyPort, &proxyEnabled, &proxyAlive);

    // 2. 【平滑降级直连】若未启用专线，或本地 Sing-box 核心尚未运行/未监听，套接字在发起连接前直接平滑走原生直连
    // 彻底杜绝调用 pOriginalConnect(s, 127.0.0.1) 失败后再次调用引发的 Winsock WSAEINVAL (10022) 崩溃！
    if (InHookGet() || !proxyEnabled || proxyPort == 0 || !proxyAlive) {
        if (!proxyAlive && proxyEnabled && name && name->sa_family == AF_INET) {
            sockaddr_in* target = (sockaddr_in*)name;
            if (ShouldAccelerate(target)) {
                int pNum = ntohs(target->sin_port);
                char tIp[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(target->sin_addr), tIp, INET_ADDRSTRLEN);
                WriteLog("[平滑直连降级] 本地代理核心未响应 (端口: %u)，目标 %s:%d 安全降级直连！", proxyPort, tIp, pNum);
            }
        }
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

        // 该目标近期在隧道内握手超时过 → 本次直接走原生直连。
        // 否则游戏每次重连都会被同一不可用隧道目标卡死，表现为"断开后再也连不上"。
        if (IsTargetFailedRecently(targetIp, targetPort)) {
            WriteLog("[自愈降级] %s:%d 近期隧道握手失败，本次改走原生直连", ipStr, portNum);
            if (isWSA) return pOriginalWSAConnect(s, name, namelen, lpCallerData, lpCalleeData, lpSQOS, lpGQOS);
            return pOriginalConnect(s, name, namelen);
        }

        WriteLog("[TCP 拦截] 检测到游戏核心连接: %s:%d，准备重定向至本地专线 %s:%d", ipStr, portNum, proxyIp, proxyPort);

        // 构造本地代理地址
        sockaddr_in proxyAddr;
        memset(&proxyAddr, 0, sizeof(proxyAddr));
        proxyAddr.sin_family = AF_INET;
        proxyAddr.sin_port = htons(proxyPort);
        proxyAddr.sin_addr.s_addr = inet_addr(proxyIp);

        // 设置重入保护，连接本地环回代理 (127.0.0.1)
        InHookSet(TRUE);
        int connectRes = pOriginalConnect(s, (sockaddr*)&proxyAddr, sizeof(proxyAddr));
        int lastErr = WSAGetLastError();
        InHookSet(FALSE);

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

                WSASetLastError(0);
                return 0; // 返回成功骗过游戏
            } else {
                // 握手失败不主动关闭：句柄归游戏所有，由游戏在收到错误后自行关闭。
                // 早期实现此处 closesocket(s)，游戏随后再关一次，句柄回收复用后会误杀
                // 另一条正常连接 —— 多开时表现为随机掉线。
                WriteLog("[TCP 错误] Socks5 握手失败: %s:%d，返回错误由游戏侧回收套接字（60 秒内该目标改走直连）", ipStr, portNum);
                MarkTargetFailed(targetIp, targetPort);
                WSASetLastError(WSAECONNREFUSED);
                return SOCKET_ERROR;
            }
        } else {
            WriteLog("[TCP 错误] 连接本地代理 %s:%d 失败 (WSAGetLastError=%d)，返回错误由游戏侧回收重连", proxyIp, proxyPort, WSAGetLastError());
            MarkTargetFailed(targetIp, targetPort);
            WSASetLastError(WSAECONNREFUSED);
            return SOCKET_ERROR;
        }
    }

    if (isWSA) return pOriginalWSAConnect(s, name, namelen, lpCallerData, lpCalleeData, lpSQOS, lpGQOS);
    return pOriginalConnect(s, name, namelen);
}

// Hook 包装函数
int WSAAPI HookedConnect(SOCKET s, const sockaddr* name, int namelen) {
    g_ActiveHookCount.fetch_add(1, std::memory_order_relaxed);
    int res = HandleRedirectConnect(s, name, namelen, FALSE, NULL, NULL, NULL, NULL);
    g_ActiveHookCount.fetch_sub(1, std::memory_order_relaxed);
    return res;
}

int WSAAPI HookedWSAConnect(SOCKET s, const sockaddr* name, int namelen, LPWSABUF lpCallerData, LPWSABUF lpCalleeData, LPQOS lpSQOS, LPQOS lpGQOS) {
    g_ActiveHookCount.fetch_add(1, std::memory_order_relaxed);
    int res = HandleRedirectConnect(s, name, namelen, TRUE, lpCallerData, lpCalleeData, lpSQOS, lpGQOS);
    g_ActiveHookCount.fetch_sub(1, std::memory_order_relaxed);
    return res;
}

// 导出版本标识供外部注入检查（防二次注入）
extern "C" __declspec(dllexport) DWORD WINAPI GameTcpSpeed_GetVersion() {
    return 0x030104; // v3.1.5
}

extern "C" __declspec(dllexport) void WINAPI GameTcpSpeed_Disable() {
    g_ProxyEnabled = FALSE;
    WriteLog("[主动旁路] 已手动禁用专线代理重定向，恢复原生直连");
}

extern "C" __declspec(dllexport) void WINAPI GameTcpSpeed_Enable() {
    g_ProxyEnabled = TRUE;
    WriteLog("[主动启用] 已启用专线代理重定向");
}

// ==================== 运行时初始化 / 卸载（两条注入路径共用） ====================

// 启动探针开关：手动映射初始化失败时置 1，可定位「入口点在哪一步返回失败」。
// 正常运行必须为 0，否则每次注入都会往临时目录写文件。
#define GTS_BOOTLOG 1   // 临时开启定位 MinHook 卡点

static void BootLog(const char* tag, unsigned long extra) {
#if !GTS_BOOTLOG
    (void)tag; (void)extra;
    return;
#else
    HANDLE h = CreateFileA("C:/tmp/gts_boot.log", FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[160];
    int n = wsprintfA(buf, "[%lu] %s extra=%lu\r\n", GetCurrentProcessId(), tag, extra);
    DWORD wrote = 0;
    WriteFile(h, buf, (DWORD)n, &wrote, NULL);
    CloseHandle(h);
#endif
}

// 普通 LoadLibrary 注入（DllMain 触发）与手动映射注入（GTS_Init 触发）都走这里。
static void InitRuntime() {
    // 幂等：手动映射路径下入口点与 GTS_Init 都可能触发，重复初始化会让 MinHook 二次挂钩
    static LONG s_inited = 0;
    if (InterlockedExchange(&s_inited, 1) != 0) return;

    if (g_TlsInHook == TLS_OUT_OF_INDEXES)
        g_TlsInHook = TlsAlloc();

    WriteLog("=== Hook DLL v3.1.5 (常驻安全防崩版) 载入进程 (PID: %lu) ===", GetCurrentProcessId());

    BootLog("MH_Initialize begin", 0);
    MH_STATUS st = MH_Initialize();
    BootLog("MH_Initialize ret", (unsigned long)st);
    if (st == MH_OK) {
        BootLog("hook connect begin", 0);
        MH_STATUS s1 = MH_CreateHookApi(L"ws2_32.dll", "connect", (LPVOID)&HookedConnect, (LPVOID*)&pOriginalConnect);
        BootLog("hook connect ret", (unsigned long)s1);
        MH_STATUS s2 = MH_CreateHookApi(L"ws2_32.dll", "WSAConnect", (LPVOID)&HookedWSAConnect, (LPVOID*)&pOriginalWSAConnect);
        BootLog("hook wsaconnect ret", (unsigned long)s2);
        BootLog("EnableHook begin", 0);
        MH_EnableHook(MH_ALL_HOOKS);
        BootLog("EnableHook done", 0);

        WriteLog("Winsock Hook v3.1.5 (常驻防崩 / 极简纯净 / 仅挂钩connect / 零触碰Socket生命周期) 已成功激活！");
    } else {
        WriteLog("MinHook 初始化失败！");
    }
}

static void ShutdownRuntime() {
    // 幂等：APC 与远线程可能各投递一次
    static LONG s_down = 0;
    if (InterlockedExchange(&s_down, 1) != 0) return;

    g_IsDetaching.store(true, std::memory_order_release);
    g_ProxyEnabled = FALSE;

    // 【核心防御 2: 退出屏障与优雅等待 (Drain Barrier)】
    // 必须等待所有正在执行 HookedConnect 的工作线程退出 Detour 区域
    int spinWait = 0;
    while (g_ActiveHookCount.load(std::memory_order_acquire) > 0 && spinWait < 100) {
        Sleep(10);
        spinWait++;
    }

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    WriteLog("=== Hook DLL v3.1.5 安全卸载 (Drain耗时: %dms) ===", spinWait * 10);

    // 钩子已全部拆除，此后不会再有线程进入 Detour，释放 TLS 槽位是安全的
    if (g_TlsInHook != TLS_OUT_OF_INDEXES) {
        TlsFree(g_TlsInHook);
        g_TlsInHook = TLS_OUT_OF_INDEXES;
    }
}

// DLL 入口点 (极简纯净Hook：仅挂钩 connect / WSAConnect，零触碰 Socket 生命周期与事件机制)
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        BootLog("DllMain attach enter", 0);
        g_hModule = hModule;

        // 手动映射（g_ExternalDllPath 已预置）时镜像不在模块表里：
        // DisableThreadLibraryCalls / GetModuleHandleEx 都是针对「真实已加载模块」的 API，
        // 对无模块记录的镜像调用没有意义，直接跳过，避免任何未定义行为。
        const BOOL bManualMapped = (g_ExternalDllPath != NULL);

        if (!bManualMapped) {
            DisableThreadLibraryCalls(hModule);

            // 【核心防御 1: 模块常驻锁定 (Module Pinning)】
            // 游戏网络多线程极其密集，若外部执行 FreeLibrary 或远线程卸载，
            // 会导致运行中的游戏网络线程 RIP 落在被释放的内存页而触发 ACCESS_VIOLATION。
            // 通过 GET_MODULE_HANDLE_EX_FLAG_PIN 锁定模块常驻内存。
            HMODULE hPinned = NULL;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               (LPCWSTR)hModule, &hPinned);
        }

        LoadConfig(hModule);
        BootLog("after LoadConfig", 0);
        InitRuntime();
        BootLog("after InitRuntime", 0);
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        ShutdownRuntime();
    }
    return TRUE;
}

// ==================== 手动映射入口（绕过目标进程的模块签名策略） ====================
// 被保护的宿主进程（如 WoW 12.0.7 的 Wow_loader.dll）只允许加载微软签名模块，
// 未签名 DLL 走 LoadLibrary 一律被拒。手动映射不经过加载器，因此碰不到该校验。
// 此时镜像不在模块表中：GetModuleFileName 拿不到路径，TLS 回调也没有加载器代为调用，
// 所以两件事都由注入方经本入口补齐（ctx 结构与 gui/manual_map.h 中的定义保持一致）。
#pragma pack(push, 8)
struct GTSInitCtx {
    uintptr_t     imageBase;   // 本镜像在宿主进程中的基址
    const char*   dllPath;     // DLL 在磁盘上的完整路径，用于定位同目录的 proxy_config.ini
    volatile LONG done;        // 完成后写 1，注入方据此确认 APC 是否真的被执行
};
#pragma pack(pop)

// 手动映射时自己走一遍本镜像的 TLS 回调（正常情况下由 Windows 加载器调用）。
static void RunOwnTlsCallbacks(HMODULE hBase) {
    if (!hBase) return;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hBase;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)hBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!dir.VirtualAddress || !dir.Size) return;

    PIMAGE_TLS_DIRECTORY tls = (PIMAGE_TLS_DIRECTORY)((BYTE*)hBase + dir.VirtualAddress);
    if (!tls->AddressOfCallBacks) return;

    PIMAGE_TLS_CALLBACK* cb = (PIMAGE_TLS_CALLBACK*)tls->AddressOfCallBacks;
    for (; *cb; ++cb) {
        (*cb)((PVOID)hBase, DLL_PROCESS_ATTACH, NULL);
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI GTS_Init(GTSInitCtx* ctx) {
    // 【必须最先做】APC 会同时投递到目标进程的很多线程，多个线程可能同时进这里。
    // _CRT_INIT 与 DllMain 都不是并发安全的，两个线程同时跑会把宿主进程直接打死。
    // 用原子抢占保证只有一个线程真正执行初始化，其余立即返回。
    static LONG s_initClaimed = 0;
    if (InterlockedExchange(&s_initClaimed, 1) != 0) {
        // 只有「尚无终态」时才写 3，否则会把已完成的 1/2 覆盖掉
        if (ctx && ctx->done == 0) ctx->done = 3;
        return TRUE;
    }

    BootLog("GTS_Init enter", 0);
    if (!ctx || !ctx->imageBase) { BootLog("bad ctx", 0); return FALSE; }
    BootLog("ctx ok, base", (unsigned long)(ctx->imageBase & 0xFFFFFFFF));

    HMODULE hBase = (HMODULE)ctx->imageBase;
    g_hModule = hBase;

    // 先给 DllMain 备好路径（它拿不到模块文件名）。
    // 复制而不是保存指针：注入方随后会释放远端上下文内存。
    if (ctx->dllPath) {
        strncpy(g_ExternalDllPathBuf, ctx->dllPath, MAX_PATH - 1);
        g_ExternalDllPathBuf[MAX_PATH - 1] = '\0';
        g_ExternalDllPath = g_ExternalDllPathBuf;
    }

    // 【关键】手动映射不会有加载器替我们调用 PE 入口点。
    // 不调它则 CRT（_CRT_INIT）与 C++ 静态初始化都不执行，
    // 表现为注入后宿主进程在首次 connect 时以 0xC0000094（整数除零）等陷阱崩溃。
    // 入口点内部会先跑 CRT 初始化，再转入本文件的 DllMain。
    RunOwnTlsCallbacks(hBase);

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hBase;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)hBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return FALSE;

    typedef BOOL (WINAPI *PFN_ENTRY)(HINSTANCE, DWORD, LPVOID);
    PFN_ENTRY entry = (PFN_ENTRY)((BYTE*)hBase + nt->OptionalHeader.AddressOfEntryPoint);
    BootLog("before entry", (unsigned long)(nt->OptionalHeader.AddressOfEntryPoint));
    BOOL r = entry((HINSTANCE)hBase, DLL_PROCESS_ATTACH, NULL);
    BootLog("after entry, result", r);

    // 通知注入方「初始化确实在这个线程上跑完了」——APC 可能排队很久才被投递
    ctx->done = r ? 1 : 2;
    return r;
}

// 手动映射的镜像无法用 FreeLibrary 卸载：这里只拆钩子并置旁路，内存交由宿主进程退出回收。
extern "C" __declspec(dllexport) BOOL WINAPI GTS_Shutdown(void) {
    ShutdownRuntime();
    return TRUE;
}
