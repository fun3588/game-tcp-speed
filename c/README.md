# C 模块：TCP 游戏专线加速套件 (C Module: TCP Game Acceleration Suite)

[中文说明](#-中文说明) • [English Documentation](#-english-documentation)

---

## 🇨🇳 中文说明

本目录包含基于 **Winsock API Hook** 的进程级 TCP 拦截动态链接库（`hook_proxy.dll`）以及 Win32 控制台启动器（`launcher_gui.exe`）。

### 设计要点

1. **避免虚拟网卡冲突**：
   - TUN 方案通常需要改写系统全局网关，容易与系统上的 VPN、Docker 或其他代理软件发生路由冲突。
   - 本项目通过内存 API Hook 仅拦截目标游戏进程发起的 TCP 连接，不修改系统路由表，也不创建虚拟网卡。
2. **套接字非阻塞与事件恢复**：
   - 接管并记录 `ioctlsocket`、`WSAEventSelect` 和 `WSAAsyncSelect` 的调用状态。
   - 在临时阻塞模式下完成 Socks5 握手后，恢复套接字原本的非阻塞状态，并补发 `FD_CONNECT` 与事件信号，避免游戏主线程在握手后卡死。
3. **保持二进制流式直传**：
   - 配合去除 `padding_scheme` 填充的 AnyTLS 配置，避免破坏游戏 SRP 协议头的字节长度。
4. **纯 C++ 实现**：
   - 测速逻辑基于 Winsock 原生高精度计时，节点订阅下载使用 WinINet API，无需 Python 运行环境。

### 编译构建

打开 PowerShell 执行：
```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```
编译产物输出至当前目录：
- `hook_proxy.dll`：注入核心动态链接库。
- `launcher_gui.exe`：原生 Win32 加速器控制台。

---

## 🇺🇸 English Documentation

This directory contains the process-level TCP proxy DLL (`hook_proxy.dll`) based on **Winsock API Hooking** and the native Win32 console launcher (`launcher_gui.exe`).

### Design Highlights

1. **Avoiding Virtual Adapter Conflicts**:
   - TUN-based setups rewrite system routing tables, which can disrupt VPNs, Docker, and developer network interfaces.
   - This project hooks Winsock socket APIs strictly inside the target game process without altering routing tables or virtual network adapters.
2. **Socket Mode & Event Preservation**:
   - Records states across `ioctlsocket`, `WSAEventSelect`, and `WSAAsyncSelect`.
   - After completing the Socks5 handshake in temporary blocking mode, it restores the socket's original non-blocking state and signals `FD_CONNECT` / `SetEvent` to keep the game's message loop active.
3. **Transparent Stream Forwarding**:
   - Works with AnyTLS configurations that omit `padding_scheme` to avoid altering binary SRP packet sizes.
4. **Native C++ Codebase**:
   - Uses native Winsock RTT testing and WinINet for subscription retrieval with zero Python dependencies.

### Build Instructions

Run the PowerShell build script:
```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```
Build outputs:
- `hook_proxy.dll`: Core Winsock proxy hook library.
- `launcher_gui.exe`: Native Win32 launcher console.
