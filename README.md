<div align="center">

# Game-TCP-Speed

### Windows 进程级 TCP 游戏加速工具（无需 TUN 虚拟网卡 / 不改写系统网关）
*(以魔兽世界 Firestorm 11.2.5 客户端及专用登录器适配为例)*

[![C++11](https://img.shields.io/badge/Language-C%2B%2B11-00599C?style=flat-square&logo=c%2B%2B)](c/)
[![MinHook](https://img.shields.io/badge/Hook-MinHook%20x64-brightgreen?style=flat-square)](c/minhook/)
[![Sing-box](https://img.shields.io/badge/Core-Sing--box%20v1.14.2-E65C00?style=flat-square&logo=box)](Sing-box/)
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011%20x64-blue?style=flat-square&logo=windows)](README.md)
[![Status](https://img.shields.io/badge/Status-Production%20Ready-success?style=flat-square)](README.md)
[![License](https://img.shields.io/badge/License-MIT-yellow?style=flat-square)](LICENSE)

[工作原理](#-工作原理与架构) •
[功能特性](#-功能特性) •
[快速开始](#-快速开始) •
[技术要点](#-技术要点与排查记录) •
[目录结构](#-项目目录结构) •
[English Documentation](#-english-documentation)

</div>

---

## 🇨🇳 中文说明

### 项目背景

跨国网络游戏（如魔兽世界海外私服/官服、最终幻想14、暗黑破坏神等）的服务器多位于欧洲或北美。国内宽带直连时容易遇到延迟抖动、丢包或骨干网 TCP RST 阻断，导致登录失败或频繁掉线。

市面上常见的加速方案多依赖 TUN/TAP 虚拟网卡改写全局网关，这在实际使用中有几个典型痛点：
- 改写系统路由表，容易与本地开发环境（Docker、WSL2、Hyper-V）、公司 VPN 或现有代理工具冲突；
- 部分私服依赖第三方定制登录器（如 `FirestormLauncher.exe`）启动游戏主程序，商业加速器容易漏掉目标进程；
- 某些底层驱动会在本地代答 TCP SYN，显示虚假低延迟，但实际游戏依然丢包。

本项目采用用户态 Winsock API Hook，只针对指定游戏进程（如 `WoW 11.2.5 - Firestorm.exe`）拦截并重定向 TCP 连接，不安装虚拟网卡，不修改系统路由表。配合 AnyTLS + Reality 纯净流式传输，在不影响系统其他网络应用的前提下改善游戏连接质量。

---

### 架构与工作原理

```mermaid
flowchart TD
    subgraph UserSpace["用户桌面环境"]
        Launcher["第三方登录器\n(FirestormLauncher.exe)"]
        Game["游戏主程序\n(WoW 11.2.5 - Firestorm.exe)"]
        GUI["控制台界面\n(launcher_gui.exe)"]
    end

    subgraph CoreEngine["本地代理核心 (无驱动 / 不改网关)"]
        HookDLL["hook_proxy.dll\n(MinHook 拦截 connect/WSAConnect)"]
        SingBox["Sing-box 核心\n(动态随机端口监听)"]
        Config["proxy_config.ini\n(端口热更新)"]
    end

    subgraph UplinkTunnel["网络传输链路"]
        AnyTLS["AnyTLS + Reality\n(标准 TCP 隧道直连 / 去除 Padding 填充)"]
    end

    subgraph RemoteTarget["远端游戏服务器"]
        Server["Firestorm 世界服务器\n(141.95.207.63 : 8285 / 1119)"]
    end

    Launcher -->|拉起| Game
    GUI -->|1. 协商分配高位空闲端口| SingBox
    GUI -->|2. 写入配置| Config
    GUI -->|3. 后台监听并自动注入| HookDLL
    HookDLL -.->|注入宿主| Game

    Game -->|TCP connect 目标 141.95.207.63| HookDLL
    HookDLL -->|拦截并重定向 (Socks5 握手)| SingBox
    Config -.->|读取最新端口| HookDLL

    SingBox -->|透明直连| AnyTLS
    AnyTLS -->|交付数据| Server
```

#### 实现要点：
1. **应用层连接重定向**：在游戏调用 `connect()` 或 `WSAConnect()` 时，将目标地址与端口重定向至本地 Sing-box 监听端口，完成 Socks5 握手后再通知游戏连接成功。
2. **套接字非阻塞状态与事件还原**：游戏引擎通常使用非阻塞套接字和事件通知机制（如 `WSAEventSelect`）。Hook 层在临时阻塞模式下完成 Socks5 协商后，会恢复套接字原始的非阻塞模式，并补发 `FD_CONNECT` 事件，避免游戏主线程挂起。
3. **动态高位端口**：Sing-box 启动时申请系统空闲的高位端口（`50000–65535`），避免固定端口被占用导致启动失败。
4. **配置热同步**：`hook_proxy.dll` 拦截连接时会检查 `proxy_config.ini` 的端口配置，GUI 更换端口后动态生效，无需重启游戏。

---

### 功能特性

| 模块 | 说明 |
| :--- | :--- |
| **进程级拦截** | 基于 MinHook 拦截 Winsock API，仅在用户态接管目标进程网络流量，不安装虚拟网卡，不修改系统路由表。 |
| **套接字状态保护** | 记录套接字原有模式，Socks5 协商完成后恢复非阻塞状态并补发事件信号，避免界面挂起。 |
| **透明流式传输** | 配置中去除 `padding_scheme` 填充，避免破坏游戏 SRP 认证包长度导致断连。 |
| **动态端口分配** | 运行时自动申请空闲高位端口并写入配置文件，避免固定端口冲突。 |
| **进程自动检测注入** | 后台循环检测目标游戏进程，进程启动后自动注入 DLL，无需手动附加。 |
| **纯 C++ 实现** | 采用 C++ 静态编译，内置基于 Winsock 的延迟测试与 WinINet 订阅获取，无需外部 Python 运行环境。 |

---

### 快速开始

#### 1. 运行加速器
右键以**管理员身份运行**：
```text
c\launcher_gui.exe
```

#### 2. 启动游戏
- 启动器界面打开后，会自动分配可用端口、启动 Sing-box 核心并开启对 `WoW 11.2.5 - Firestorm.exe` 的后台监听。
- 此时直接使用第三方登录器启动游戏（或直接运行游戏主程序），控制台将显示注入成功提示：
  ```text
  √ 成功注入加速模块至魔兽进程 (PID: xxx)！
  ```
- 游戏主界面启动后即可正常通过代理隧道建立连接。

---

### 技术要点与排查记录

#### 1. 注入后游戏窗口卡死无响应
- **原因**：魔兽世界客户端使用非阻塞套接字与异步事件通知机制（`WSAEventSelect`）。如果在 Hook `connect` 期间临时转为阻塞模式执行 Socks5 握手，但在握手后未恢复非阻塞状态，会导致游戏主线程在后续等待网络数据时陷入死锁。
- **处理**：在 `hook_proxy.cpp` 中 Hook `ioctlsocket`、`WSAEventSelect` 和 `WSAAsyncSelect`，保存套接字的原始模式与事件句柄。握手成功后立即恢复非阻塞模式，并调用 `SetEvent` 触发 `FD_CONNECT` 通知，使游戏事件循环继续运转。

#### 2. 登录或进入世界服时提示“已断开连接”
- **原因**：服务端启用了 `padding_scheme` 长度填充。魔兽世界的 SRP 认证协议对数据包长度敏感，填充改变了包体长度导致客户端校验失败断开。此外，服务端如果开启 TCP FastOpen 或开启连接复用（`tcp_tw_reuse`），在经过家庭宽带 NAT 后也容易触发重置或丢包。
- **处理**：移除服务端与客户端配置中的 `padding_scheme`，恢复原生流式传输；在 Linux 服务端内核配置中将 `net.ipv4.tcp_fastopen` 与 `net.ipv4.tcp_tw_reuse` 均设为 0。

---

### 项目目录结构

```text
game-tcp-speed/
├── ai.md                             # AI 协作与维护规范文档
├── README.md                         # 中英文技术文档与使用说明
├── setupAnytls.md                    # AnyTLS 服务端与守护进程配置参考
├── LICENSE                           # MIT 开源许可证
│
├── c/                                # 纯 C++ Winsock Hook 与原生 GUI
│   ├── CMakeLists.txt                # CMake 构建脚本 (Ninja 构建系统)
│   ├── build.ps1 / build.bat         # 自动化编译脚本
│   ├── hook_proxy.dll                # Winsock 动态拦截核心 (支持非阻塞还原与端口热更新)
│   ├── launcher_gui.exe              # 原生 Win32 控制台界面 (零依赖)
│   ├── proxy_config.ini              # 运行时随机端口与配置
│   ├── dll/hook_proxy.cpp            # MinHook 拦截实现源码
│   ├── gui/launcher_gui.cpp          # Win32 原生界面与注入引擎源码
│   └── minhook/                      # MinHook x64 底层库
│
├── Sing-box/                         # 独立 Sing-box 核心环境
│   ├── sing-box.exe                  # Sing-box 运行核心
│   └── config.json                   # AnyTLS-Reality 专线配置 (去除 Padding)
│
├── tools/                            # 系统协议栈与底层驱动维护工具
│   ├── set_proxifier_demand.cmd      # 将 ProxifierDrv 驱动改为按需启动 (demand)
│   ├── disable_proxifier_driver.cmd  # 停止并禁用 Proxifier 驱动
│   └── enable_proxifier_driver.cmd   # 恢复 Proxifier 驱动运行
│
└── docs/                             # 技术设计与效果验证报告
    ├── 加速器效果验证.md             # 延迟与丢包率对比测试
    └── 独立sing-box使用说明.md        # 独立核心运行手册
```

---

## 🇺🇸 English Documentation

### Overview

Cross-border online gaming often involves routing traffic to servers in Europe or North America. Direct connections from residential ISPs frequently encounter packet loss, jitter, or TCP RST drops from backbone gateways, leading to login failures or unexpected disconnections.

Most traditional accelerators rely on TUN/TAP virtual network adapters to modify the system default gateway:
- Modifying the routing table can conflict with local development environments (Docker, WSL2, Hyper-V), corporate VPNs, or existing proxy tools;
- Private gaming servers often rely on custom launchers (such as `FirestormLauncher.exe`), which driver-based tools may fail to capture;
- Certain low-level drivers generate synthetic TCP SYN acknowledgments locally, presenting misleadingly low latency figures while the actual connection still suffers from packet loss.

Game-TCP-Speed uses user-mode Winsock API hooking to redirect network connections solely for the target game process (e.g., `WoW 11.2.5 - Firestorm.exe`). It operates without virtual network adapters or routing table modifications. Combined with AnyTLS + Reality streaming, it stabilizes game connectivity without affecting other desktop applications.

---

### Implementation Details

1. **Application-Level Redirection**: Captures calls to `connect()` and `WSAConnect()`, redirecting target destinations to a local Sing-box listener and executing the Socks5 handshake before returning control to the game.
2. **Socket Non-Blocking Restoration**: Tracks socket state via `ioctlsocket`, `WSAEventSelect`, and `WSAAsyncSelect`. After completing the handshake in temporary blocking mode, it restores the socket's original non-blocking state and signals `FD_CONNECT` to keep the game's event loop moving.
3. **Dynamic Port Binding**: Obtains an unreserved high port (`50000–65535`) from the operating system upon starting Sing-box, preventing port collision errors.
4. **Configuration Hot-Reload**: `hook_proxy.dll` re-reads `proxy_config.ini` during connection attempts, allowing GUI port updates to take effect without restarting the game.

---

### Features

| Feature | Description |
| :--- | :--- |
| **Process-Level Interception** | Uses MinHook to intercept Winsock calls in user mode. No virtual network adapters or routing table modifications required. |
| **Socket Mode Preservation** | Tracks socket modes and restores non-blocking states and event signals after Socks5 handshakes, preventing main-thread lockups. |
| **Transparent Streaming** | Disables `padding_scheme` length obfuscation to protect binary SRP packet lengths from corruption. |
| **Dynamic Port Allocation** | Requests available high ports at runtime and updates configuration dynamically to avoid conflicts. |
| **Automated Injection** | Periodically checks for the target process in the background and injects the DLL once detected. |
| **Native C++ Implementation** | Compiled as pure C++ without Python runtime dependencies, featuring built-in Winsock RTT testing and WinINet subscription retrieval. |

---

### Quick Start

1. Right-click and **Run as Administrator**:
   ```text
   c\launcher_gui.exe
   ```
2. The launcher automatically allocates an available port, starts the Sing-box core, and monitors for `WoW 11.2.5 - Firestorm.exe`.
3. Launch the game client or custom launcher. The console will report successful injection:
   ```text
   √ 成功注入加速模块至魔兽进程 (PID: xxx)！
   ```
4. The game will establish its network connection through the local tunnel.

---

### License

Distributed under the [MIT License](LICENSE).
Uses [MinHook](https://github.com/TsudaKageyu/minhook) and [Sing-box](https://github.com/SagerNet/sing-box).
