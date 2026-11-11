# C 模块：纯经典 Win32 API + GDI 极速客户端与 Winsock Hook 核心 (v3.1)

[中文说明](#-中文说明) • [English Documentation](#-english-documentation)

---

## 🇨🇳 中文说明

本目录包含 **Winsock API Hook** 进程级 TCP 拦截动态链接库（`hook_proxy.dll`）以及 v3.1 纯经典 Win32 旗舰图形客户端（`GameTcpSpeed.exe`）。

### v3.1 核心特性

1. **纯经典 Win32 API + GDI (零外部依赖)**：
   - 彻底摒弃一切 WebView2、Electron 与 .NET 托管运行时的数百兆内存开销；
   - 运行时内存仅 **3 ~ 5 MB**，启动毫秒级秒开，单文件仅 **174 KB**；
   - 纯原生 Windows 控件与 GDI 绘制，零 DirectX 依赖，在任何纯净精简系统上均可直接运行。
2. **中英双语即时一键热切换**：
   - 窗口右上角集成 `[ 🇨🇳 简体中文 / 🇺🇸 English ]` 原生按钮，点击瞬间重载界面所有文本。
3. **异步完成端口 `ConnectEx` 挂钩与已运行游戏接管**：
   - 不仅拦截传统 `connect` 与 `WSAConnect`，更主动挂钩大型 3D 游戏（如魔兽世界 11.2.5）大量采用的异步 IOCP `ConnectEx`（及 `WSAIoctl` 扩展查询）；
   - 实现**已运行中游戏免重启平滑接管**。
4. **IPv6 泄露拦截与双栈强制回退 (Anti-Leak Fallback)**：
   - 针对国内宽带下发公网 IPv6 导致魔兽直连暴露真实 IP 并触发防挂多 IP 封号机制的问题，DLL 主动拦截 `AF_INET6` 协议族连接并返回 `WSAEAFNOSUPPORT`；
   - 强制游戏客户端网络协议栈平滑回退至 IPv4，确保 100% 流量进入加速专线隧道。
5. **零副作用预连接状态探针 (Zero-Side-Effect Health Check)**：
   - 摒弃以往在游戏原 Socket 上探测代理可能诱发 Winsock `WSAEINVAL (10022)` 或 `WSAEISCONN (10056)` 的隐患；
   - 采用独立瞬时探测套接字探测本地核心，不可用时游戏原生套接字 100% 完好无损地退回直连。
6. **启动起飞就绪锁 (Pre-flight Readiness Lock) 与多开极速捕获**：
   - 启动 Sing-box 时采用 50ms 紧凑就绪探针，必须确实验证本地监听就绪才放行注入，杜绝“服务未起先注入导致的断网”；
   - 自动注入扫描周期优化为 200ms，多开游戏启动瞬间毫秒级自动捕获接管。

### 编译构建

打开 PowerShell 执行：
```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```
产物输出至当前目录：
- `GameTcpSpeed.exe`：纯经典 Win32 API + GDI 双语版主程序。
- `hook_proxy.dll`：注入核心动态链接库。

---

## 🇺🇸 English Documentation

This directory contains the core process-level TCP proxy DLL (`hook_proxy.dll`) and the v3.1 native Win32 GUI client (`GameTcpSpeed.exe`).

### Key v3.1 Features

1. **Pure Win32 API + GDI (Zero Dependencies)**:
   - Eliminates WebView2/.NET overhead, reducing memory footprint to **3 ~ 5 MB RAM**. Starts in `< 0.01s`, single binary only **174 KB**.
   - Zero DirectX or GPU requirement, runs out-of-the-box on any stripped Windows system.
2. **Instant Bilingual Toggle**:
   - Integrated top-right toggle button to switch between Chinese and English in <1ms.
3. **Asynchronous IOCP `ConnectEx` Interception**:
   - Hooks both synchronous `connect`/`WSAConnect` and asynchronous IOCP `ConnectEx`, enabling **live process takeover without restarting games**.
4. **IPv6 Leak Defense & Dual-Stack Fallback**:
   - Intercepts `AF_INET6` socket connections with `WSAEAFNOSUPPORT`, forcing game network engines to fall back to IPv4, preventing domestic ISP IPv6 leakage and multi-IP bans.
5. **Zero-Side-Effect Pre-Connect Health Probe**:
   - Uses an ephemeral probe socket rather than touching the game's actual socket, completely eliminating Winsock `WSAEINVAL (10022)` or `WSAEISCONN (10056)` errors upon direct fallback.
6. **Pre-flight Readiness Lock & Multi-boxing Polling Boost**:
   - Enforces a 50ms verification loop ensuring Sing-box local inbound is active before injecting; boosts auto-injection cycle to 200ms for seamless multi-client game launches.

### Build Instructions

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```
Outputs:
- `GameTcpSpeed.exe`: Pure Win32 API + GDI bilingual client (v3.1).
- `hook_proxy.dll`: Winsock proxy hook module (v3.1).
