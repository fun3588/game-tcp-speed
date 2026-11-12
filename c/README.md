# C 模块：纯经典 Win32 API + GDI 极速客户端与 Winsock Hook 核心 (v3.1.1)

[中文说明](#-中文说明) • [English Documentation](#-english-documentation)

---

## 🇨🇳 中文说明

本目录包含 **Winsock API Hook** 进程级 TCP 拦截动态链接库（`hook_proxy.dll`）以及 v3.1.1 纯经典 Win32 旗舰图形客户端（`GameTcpSpeed.exe`）。

### v3.1.1 核心特性

1. **纯经典 Win32 API + GDI (零外部依赖)**：
   - 彻底摒弃一切 WebView2、Electron 与 .NET 托管运行时的数百兆内存开销；
   - 运行时内存仅 **3 ~ 5 MB**，启动毫秒级秒开，单文件仅 **174 KB**；
   - 纯原生 Windows 控件与 GDI 绘制，零 DirectX 依赖，在任何纯净精简系统上均可直接运行。
2. **过图防崩根治与原生 IOCP 放行 (World Transfer Anti-Crash)**：
   - 彻底废除使用 MinHook 对 `mswsock!ConnectEx` 的汇编行内篡改，根除了 Win11 下 **RIP 相对寻址指令搬迁失真** 导致的跳板内存访问越界（`0x000001f843ea7ef5`）与 `ERROR #132` 崩溃；
   - 魔兽世界进出副本地图传送门（`World transfer pending...`）过图重连 100% 稳定运行。
3. **套接字事件生命周期清理**：
   - 修复 `WSAEventSelect` / `WSAAsyncSelect` 取消绑定时的句柄残留，杜绝句柄复用引发悬空信号操作。
4. **中英双语即时一键热切换**：
   - 窗口右上角集成 `[ 🇨🇳 简体中文 / 🇺🇸 English ]` 原生按钮，点击瞬间重载界面所有文本。
5. **IPv6 泄露拦截与双栈强制回退 (Anti-Leak Fallback)**：
   - 针对国内宽带下发公网 IPv6 导致魔兽直连暴露真实 IP 并触发防挂多 IP 封号机制的问题，DLL 主动拦截 `AF_INET6` 协议族连接并返回 `WSAEAFNOSUPPORT`；
   - 强制游戏客户端网络协议栈平滑回退至 IPv4，确保 100% 流量进入加速专线隧道。
6. **零副作用预连接状态探针 (Zero-Side-Effect Health Check)**：
   - 摒弃以往在游戏原 Socket 上探测代理可能诱发 Winsock `WSAEINVAL (10022)` 或 `WSAEISCONN (10056)` 的隐患；
   - 采用独立瞬时探测套接字探测本地核心，不可用时游戏原生套接字 100% 完好无损地退回直连。
7. **启动起飞就绪锁 (Pre-flight Readiness Lock) 与多开极速捕获**：
   - 启动 Sing-box 时采用 50ms 紧凑就绪探针，必须确实验证本地监听就绪才放行注入，杜绝“服务未起先注入导致的断网”；
   - 自动注入扫描周期优化为 200ms，多开游戏启动瞬间毫秒级自动捕获接管。

### 编译构建

打开 PowerShell 执行：
```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```
产物输出至当前目录：
- `GameTcpSpeed.exe`：纯经典 Win32 API + GDI 双语版主程序 (v3.1.1)。
- `hook_proxy.dll`：注入核心动态链接库 (v3.1.1)。

---

## 🇺🇸 English Documentation

This directory contains the core process-level TCP proxy DLL (`hook_proxy.dll`) and the v3.1.1 native Win32 GUI client (`GameTcpSpeed.exe`).

### Key v3.1.1 Features

1. **Pure Win32 API + GDI (Zero Dependencies)**:
   - Eliminates WebView2/.NET overhead, reducing memory footprint to **3 ~ 5 MB RAM**. Starts in `< 0.01s`, single binary only **174 KB**.
   - Zero DirectX or GPU requirement, runs out-of-the-box on any stripped Windows system.
2. **World Transfer Crash Elimination**:
   - Completely removed MinHook inline splicing of `mswsock!ConnectEx`. Eliminates Windows 11 RIP-relative instruction displacement faults (`ERROR #132 ACCESS_VIOLATION` in anonymous trampoline memory).
3. **Socket Event Clean-up**:
   - Properly purges closed event handles on `WSAEventSelect` unbinds to eliminate stale handle reuse bugs.
4. **Instant Bilingual Toggle**:
   - Integrated top-right toggle button to switch between Chinese and English in <1ms.
5. **IPv6 Leak Defense & Dual-Stack Fallback**:
   - Intercepts `AF_INET6` socket connections with `WSAEAFNOSUPPORT`, forcing game network engines to fall back to IPv4, preventing domestic ISP IPv6 leakage and multi-IP bans.
6. **Zero-Side-Effect Pre-Connect Health Probe**:
   - Uses an ephemeral probe socket rather than touching the game's actual socket, completely eliminating Winsock `WSAEINVAL (10022)` or `WSAEISCONN (10056)` errors upon direct fallback.
7. **Pre-flight Readiness Lock & Multi-boxing Polling Boost**:
   - Enforces a 50ms verification loop ensuring Sing-box local inbound is active before injecting; boosts auto-injection cycle to 200ms for seamless multi-client game launches.

### Build Instructions

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```
Outputs:
- `GameTcpSpeed.exe`: Pure Win32 API + GDI bilingual client (v3.1.1).
- `hook_proxy.dll`: Winsock proxy hook module (v3.1.1).
