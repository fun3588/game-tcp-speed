# C 模块：TCP 游戏专线加速套件 (C Module: TCP Game Acceleration Suite)

[中文说明](#-中文说明) • [English Documentation](#-english-documentation)

---

## 🇨🇳 中文说明

本目录包含了基于 **Winsock API Hook** 的进程级 TCP 流量拦截动态库（`hook_proxy.dll`）以及轻量级 Win32 原生 GUI 注入启动器（`launcher_gui.exe`）。

### 架构优势与设计思想

1. **零网卡冲突（非 TUN 方案）**：
   - 系统开启 Clash、VPN 或办公虚拟网卡时，再开 TUN 会抢占默认网关 `0.0.0.0/0` 导致网络瘫痪。
   - 本项目通过 **进程内存级 API Hook** 仅拦截目标游戏进程的 TCP 连接，**完全不碰系统路由表与虚拟网卡**，与系统上其他任何网络软件完美共存。
2. **非阻塞套接字与事件状态还原**：
   - 彻底修复常规 Hook 导致游戏主线程卡死无响应的问题。Hook 全面接管并跟踪 `ioctlsocket`、`WSAEventSelect` 与 `WSAAsyncSelect`。
   - 在完成 Socks5 握手后，自动还原 Socket 的非阻塞属性并主动补发 `FD_CONNECT` 与事件触发，确保游戏主循环流畅运转。
3. **AnyTLS + Reality 原生直传**：
   - 去除所有 `padding_scheme` 混淆填充，杜绝报文变形引起的“已断开连接”错误，实现游戏认证与世界服数据 100% 透明转发。
4. **纯 C++ 工业级免安装**：
   - 测速模块重构为纯 Winsock 高精度计时，订阅下载使用 Windows 原生 WinINet API，无任何 Python 或第三方运行库依赖。

### 编译构建

打开 PowerShell 执行：
```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```
产物将输出至当前目录：
- `hook_proxy.dll`：注入核心动态链接库。
- `launcher_gui.exe`：原生 Win32 加速器控制台。

---

## 🇺🇸 English Documentation

This directory contains the process-level TCP interception dynamic link library (`hook_proxy.dll`) based on **Winsock API Hooking** and the lightweight native Win32 GUI launcher (`launcher_gui.exe`).

### Architectural Highlights

1. **Zero Virtual Adapter Conflicts (Non-TUN Approach)**:
   - Typical TUN software rewrites `0.0.0.0/0` routing table entries, disrupting system VPNs, Docker, and developer environments.
   - This project hooks Winsock socket APIs specifically within the target game process, touching neither routing tables nor virtual network adapters.
2. **Non-Blocking Socket & Asynchronous Event Preservation**:
   - Resolves freezing bugs during game loading. The hook records socket modes via `ioctlsocket`, `WSAEventSelect`, and `WSAAsyncSelect`.
   - After establishing the Socks5 tunnel, it restores the socket's original non-blocking state and signals `FD_CONNECT` / `SetEvent` to keep the game UI responsive.
3. **Pure AnyTLS + Reality Passthrough**:
   - Strips packet padding schemes to ensure binary-level transparency required by game SRP authentication handshakes.
4. **Standalone Native C++ Implementation**:
   - Includes native Winsock RTT measurement and WinINet subscription downloading with zero Python dependencies.

### Build Instructions

Run the PowerShell build script:
```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```
Generated artifacts:
- `hook_proxy.dll`: Core Winsock proxy hook library.
- `launcher_gui.exe`: Native Win32 game accelerator GUI.
