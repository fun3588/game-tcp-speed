<div align="center">

# ⚡ Game-TCP-Speed

### 针对 Windows 进程级 TCP 专线游戏加速方案（零 TUN 网卡 / 零网关抢占）
*(以魔兽世界 Firestorm 11.2.5 客户端及专用登录器深度适配为例)*

[![C++11](https://img.shields.io/badge/Language-C%2B%2B11-00599C?style=flat-square&logo=c%2B%2B)](c/)
[![MinHook](https://img.shields.io/badge/Hook-MinHook%20x64-brightgreen?style=flat-square)](c/minhook/)
[![Sing-box](https://img.shields.io/badge/Core-Sing--box%20v1.14.2-E65C00?style=flat-square&logo=box)](Sing-box/)
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011%20x64-blue?style=flat-square&logo=windows)](README.md)
[![Status](https://img.shields.io/badge/Status-Production%20Ready-success?style=flat-square)](README.md)
[![License](https://img.shields.io/badge/License-MIT-yellow?style=flat-square)](LICENSE)

[核心原理](#-核心原理与设计哲学) •
[功能特性](#-功能特性) •
[快速开始](#-快速开始指南) •
[技术内幕](#-深度技术内幕与踩坑实录) •
[项目目录](#-项目目录结构) •
[English Version](#-english-version)

</div>

---

## 🇨🇳 中文说明 (Chinese Documentation)

### 📖 项目背景与愿景

在跨国网络游戏（如魔兽世界海外私服、最终幻想14、暗黑破坏神等）场景中，游戏服务器通常部署在欧洲（如法国 OVH 机房）或北美。国内家庭宽带直连跨国公网出口时，常常遭遇剧烈抖动、20%~30% 高丢包率，甚至因骨干网策略导致 TCP RST 阻断，引发游戏读条卡死、秒断或掉线。

市面上传统的网络加速器大多采用 **TUN / TAP 虚拟网卡模式**，强行改写 Windows 系统的默认网关：
- ❌ **严重抢占网络**：导致开发环境（Docker、WSL2、Hyper-V）、公司内网 VPN、日常代理工具（如 Clash、Sing-box）产生全局路由死锁与冲突；
- ❌ **不适配第三方登录器**：魔兽私服普遍依赖定制登录器（如 `FirestormLauncher.exe`）进行动态补丁与验证，商业加速器往往难以精准拦截；
- ❌ **网络代答虚假延迟**：部分驱动工具会在本地内核虚假代答 SYN，制造 2ms 延迟假象，实际游戏依然严重卡顿。

**本项目打破以上痛点**：不创建任何虚拟网卡、不修改系统默认路由、不干扰现有代理。通过**用户态 Winsock API Hook 注入**与**随机端口隔离**，实现应用级精准 TCP 进程加速，结合 **AnyTLS + Reality** 纯净直传专线，打造纯原生、零冲突、秒级自愈的加速方案。

---

### 🏗️ 核心原理与设计哲学

```mermaid
flowchart TD
    subgraph UserSpace["用户桌面与操作区"]
        Launcher["第三方登录器\n(FirestormLauncher.exe)"]
        Game["游戏主程序\n(WoW 11.2.5 - Firestorm.exe)"]
        GUI["Win32 原生控制台\n(launcher_gui.exe)"]
    end

    subgraph CoreEngine["本地加速引擎 (零驱动 / 零网关修改)"]
        HookDLL["hook_proxy.dll\n(MinHook 拦截 connect/WSAConnect)"]
        SingBox["Sing-box 独立核心\n(动态随机端口监听)"]
        Config["proxy_config.ini\n(端口热更新调度)"]
    end

    subgraph UplinkTunnel["跨国加密专线链路"]
        AnyTLS["极速专线: AnyTLS + Reality\n(TCP 隧道直连 / 零 Padding 长度混淆 / 完美过白)"]
    end

    subgraph RemoteTarget["远端海外游戏服务器"]
        Server["Firestorm 世界服务器\n(141.95.207.63 : 8285 / 1119)"]
    end

    Launcher -->|拉起| Game
    GUI -->|1. 协商分配高位空闲端口| SingBox
    GUI -->|2. 写入配置| Config
    GUI -->|3. 后台监听并自动注入| HookDLL
    HookDLL -.->|注入宿主| Game

    Game -->|TCP connect 目标 141.95.207.63| HookDLL
    HookDLL -->|拦截并重定向 (Socks5 握手)| SingBox
    Config -.->|热读取最新端口| HookDLL

    SingBox -->|透明代理直连| AnyTLS
    AnyTLS -->|稳定交付| Server
```

#### 关键架构原则：
1. **纯应用层透明重定向**：在游戏调用 `connect()` / `WSAConnect()` 瞬间，将目标 IP 与端口透明重定向至本地 Sing-box 代理端口，完成 Socks5 握手后欺骗游戏返回成功，游戏流量全量走专线。
2. **非阻塞套接字与异步事件无缝还原**：拦截握手期间临时切入阻塞模式并在 2.5 秒内完成 Socks5 协商，握手成功后**100% 恢复套接字原始非阻塞状态**并主动补发 `WSAEVENT` / `FD_CONNECT` 信号，彻底根治游戏主线程卡死无响应。
3. **端口动态安全协商**：每次启动 Sing-box 时动态绑定 0 端口向系统申请空闲高位端口（`50000–65535`），避开系统保留段，杜绝端口冲突（`10061`）。
4. **DLL 运行时配置热更新**：`hook_proxy.dll` 拦截时动态检查 `proxy_config.ini` 修改，GUI 无论何时更换端口，DLL 毫秒级自动热同步，无需重启游戏。

---

### ✨ 功能特性

| 功能模块 | 说明与优势 |
| :--- | :--- |
| **🚀 零 TUN 进程级加速** | 基于 MinHook 拦截 Winsock，完全在用户态实现流量改道，不装虚拟网卡、不改系统路由表。 |
| **🛡️ 零阻塞防卡死引擎** | 智能记录套接字模式，握手后秒级还原非阻塞状态并补发事件信号，游戏主线程永久流畅响应。 |
| **⚡ AnyTLS-Reality 纯净专线** | 彻底去除 `padding_scheme` 混淆长度填充，保证魔兽世界 SRP 认证与世界服数据包 100% 原始透明转发。 |
| **🔒 端口冲突免疫** | 启动时自动申请未占用高位随机端口，自动写入配置，彻底消除端口占用冲突。 |
| **💉 登录器全自动监控注入** | 内置高效的后台快照检测线程，命中 `WoW 11.2.5 - Firestorm.exe` 瞬间自动注入，支持先开登录器或先开加速器。 |
| **📦 纯 C++ 原生免安装** | 彻底剥离 Python 依赖，单文件 C++ 静态编译，包含 Winsock 高精度端到端测速与 WinINet 订阅拉取。 |

---

### 🚀 快速开始指南

#### 1. 运行加速器
直接右键以**管理员身份运行**：
```text
c\launcher_gui.exe
```

#### 2. 自动启动加速
- 启动器界面打开后，会自动随机分配可用端口、启动 Sing-box 核心并开启对 `WoW 11.2.5 - Firestorm.exe` 的后台监听。
- 此时直接使用第三方登录器启动游戏（或直接运行游戏主程序），控制台将自动打印：
  ```text
  √ 成功注入加速模块至魔兽进程 (PID: xxx)！
  ```
- 游戏主界面将秒级加载完成并建立专线加速连接。

---

### 🔬 深度技术内幕与踩坑实录

#### 1. 魔兽世界主线程卡死无响应根因与解决
- **现象**：游戏进程被注入后，窗口黑屏或无响应卡死在登录界面。
- **排查**：魔兽世界网络引擎深度依赖非阻塞 Socket 与 Windows 异步事件（`WSAEventSelect`）。先前版本在 Hook `connect` 时临时切入阻塞模式执行 Socks5 握手，但完成后未还原非阻塞标志，导致游戏主线程在调用 `recv` 时陷入死锁。
- **解决**：在 [`hook_proxy.cpp`](file:///E:/code/game-tcp-speed/c/dll/hook_proxy.cpp) 中全量 Hook `ioctlsocket`、`WSAEventSelect` 和 `WSAAsyncSelect`，记录 Socket 原始状态，握手完成后立即恢复非阻塞，并主动触发 `SetEvent` 及 `FD_CONNECT` 消息通知，彻底消除卡死。

#### 2. 魔兽世界连接时报错“已断开连接”根因
- **现象**：游戏能进入登录界面，但连接世界服（`8285` 端口）瞬间弹出“已断开连接”。
- **排查**：服务端节点启用了针对 Web 的 `padding_scheme`（长度混淆填充方案）。魔兽世界的 SRP 认证握手包对二进制长度极其敏感，强行填充导致数据格式被破坏，游戏客户端主动中断连接。
- **解决**：服务端与客户端配置中彻底移除 `padding_scheme`，恢复原始透明流式直传；同时在服务端内核禁用 `tcp_fastopen` 与 `tcp_tw_reuse`，保证游戏长连接绝对平稳。

---

### 📁 项目目录结构

```text
game-tcp-speed/
├── ai.md                             # AI 协作与维护规范文档
├── README.md                         # 中英文全景技术文档与使用说明
├── setupAnytls.md                    # AnyTLS 纯净服务端与看门狗部署手册
├── LICENSE                           # MIT 开源许可证
│
├── c/                                # 【核心生产级】纯 C++ Winsock Hook 与原生 GUI
│   ├── CMakeLists.txt                # CMake 构建脚本 (Ninja 构建系统)
│   ├── build.ps1 / build.bat         # 自动化一键编译脚本
│   ├── hook_proxy.dll                # Winsock 动态拦截核心 (支持非阻塞还原与端口热更新)
│   ├── launcher_gui.exe              # 原生 Win32 控制台界面 (零依赖)
│   ├── proxy_config.ini              # 运行时随机端口与配置
│   ├── dll/hook_proxy.cpp            # MinHook 拦截实现源码
│   ├── gui/launcher_gui.cpp          # Win32 原生界面与注入引擎源码
│   └── minhook/                      # MinHook x64 开源底层库
│
├── Sing-box/                         # 独立 Sing-box 核心环境
│   ├── sing-box.exe                  # Sing-box 稳定版核心
│   └── config.json                   # AnyTLS-Reality 纯净专线配置 (零 Padding)
│
├── tools/                            # 系统协议栈与底层驱动维护工具
│   ├── set_proxifier_demand.cmd      # 将 ProxifierDrv 驱动改为按需启动 (demand)
│   ├── disable_proxifier_driver.cmd  # 彻底停止并禁用 Proxifier 驱动
│   └── enable_proxifier_driver.cmd   # 恢复 Proxifier 驱动运行
│
└── docs/                             # 技术设计与效果验证报告
    ├── 加速器效果验证.md             # 延迟与丢包率对比报告
    └── 独立sing-box使用说明.md        # 独立核心架构设计手册
```

---

## 🇺🇸 English Version

### 📖 Overview & Vision

In cross-border gaming scenarios (e.g., World of Warcraft overseas private/retail servers, FFXIV, Diablo), servers are typically hosted in Europe (such as OVH in France) or North America. Direct connection over public ISP lines often suffers from **severe packet loss (20%–30%), jitter, and TCP RST drops**, causing disconnects and high latency.

Traditional commercial game accelerators rely on **TUN/TAP virtual network adapters**, rewriting default routing tables:
- ❌ **Routing Conflicts**: Clashes with VPNs, Docker, WSL2, and corporate proxies;
- ❌ **Poor Custom Launcher Compatibility**: Ineffective for custom launchers (e.g. `FirestormLauncher.exe`);
- ❌ **Fake Latency Acknowledgement**: Driver-based tools often fake 2ms SYN-ACKs in kernel space while the real game connection lags.

**Game-TCP-Speed solves these problems**: Zero TUN adapters, zero gateway modifications, zero proxy conflicts. Utilizing **user-mode Winsock API Hooking** and **dynamic port negotiation**, it provides precise process-level TCP acceleration powered by **AnyTLS + Reality** transparent tunneling.

---

### 🏗️ Architecture & Principles

1. **User-Mode Transparent Redirection**: When the target game calls `connect()` or `WSAConnect()`, the destination IP/port is seamlessly hijacked to the local Sing-box proxy port, performing a quick Socks5 handshake before returning control to the game.
2. **Non-Blocking Socket State Restoration**: Hooks track `ioctlsocket`, `WSAEventSelect`, and `WSAAsyncSelect`. Sockets are temporarily set to blocking mode during the handshake (with a 2.5s safety timeout) and **100% restored to their original non-blocking state** with manual `FD_CONNECT` signaling. This prevents game main-thread freezes.
3. **Zero Port Collisions**: Dynamically allocates random unreserved high ports (`50000–65535`) on each run.
4. **Runtime Config Hot-Reload**: `hook_proxy.dll` dynamically reads `proxy_config.ini` upon connection, allowing port sync without restarting the game.

---

### ✨ Features

| Feature | Description |
| :--- | :--- |
| **🚀 Zero-TUN Process Acceleration** | Hijacks Winsock traffic entirely in user space via MinHook without installing virtual network adapters. |
| **🛡️ Anti-Freeze Socket Engine** | Full non-blocking socket mode tracking and asynchronous event signaling to prevent UI lockups. |
| **⚡ AnyTLS-Reality Dedicated Tunnel** | Zero `padding_scheme` length obfuscation, ensuring 100% transparent binary forwarding for WoW SRP authentication. |
| **🔒 Port Conflict Immunity** | Dynamic high-port binding eliminates `10061` connection refused errors. |
| **💉 Automated Launcher Hooking** | Background process watcher immediately injects into `WoW 11.2.5 - Firestorm.exe`. |
| **📦 Pure Native C++ Implementation** | Zero Python dependencies. Compact Win32 UI with built-in Winsock RTT latency measurement and WinINet subscription downloader. |

---

### 🚀 Quick Start

1. Right-click and **Run as Administrator**:
   ```text
   c\launcher_gui.exe
   ```
2. The launcher will automatically start the Sing-box core on a random port and begin monitoring for `WoW 11.2.5 - Firestorm.exe`.
3. Launch your game via the Firestorm launcher. The console will display:
   ```text
   √ 成功注入加速模块至魔兽进程 (PID: xxx)！
   ```
   Enjoy ultra-stable, accelerated gameplay!

---

### 📄 License

Distributed under the [MIT License](LICENSE).
Built with [MinHook](https://github.com/TsudaKageyu/minhook) & [Sing-box](https://github.com/SagerNet/sing-box).
