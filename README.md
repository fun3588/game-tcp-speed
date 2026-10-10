<div align="center">

# ⚡ Game-TCP-Speed

### 针对 Windows 进程级 TCP 专线游戏加速方案（零 TUN 网卡 / 零网关抢占）
*(以魔兽世界 Firestorm 11.2.5 客户端及第三方专用登录器深度定制为例)*

[![C++20](https://img.shields.io/badge/Language-C%2B%2B20-00599C?style=flat-square&logo=c%2B%2B)](c/)
[![Python](https://img.shields.io/badge/Language-Python%203.10+-3776AB?style=flat-square&logo=python)](py/)
[![MinHook](https://img.shields.io/badge/Hook-MinHook%20x64-brightgreen?style=flat-square)](c/minhook/)
[![Sing-box](https://img.shields.io/badge/Core-Sing--box%20v1.14.2-E65C00?style=flat-square&logo=box)](Sing-box/)
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011%20x64-blue?style=flat-square&logo=windows)](README.md)
[![Status](https://img.shields.io/badge/Status-Production%20Archive-success?style=flat-square)](README.md)
[![License](https://img.shields.io/badge/License-MIT-yellow?style=flat-square)](LICENSE)

[核心原理](#-核心原理与设计哲学) •
[功能特性](#-功能特性) •
[双方案对比](#-双加速方案全景对比) •
[快速开始](#-快速开始指南) •
[技术内幕](#-深度技术内幕与踩坑实录) •
[项目归档目录](#-项目完整归档目录)

</div>

---

## 📖 项目背景与愿景

在跨国网络游戏（如魔兽世界海外私服、最终幻想14、暗黑破坏神等）场景中，游戏服务器通常部署在欧洲（如法国 OVH 机房）或北美。国内家庭宽带直连跨国公网出口时，常常遭遇**剧烈抖动、20%~30% 高丢包率**，甚至因骨干网策略导致 **TCP RST 阻断**，引发游戏读条卡死、秒断或团本跳 ping。

然而，市面上传统的网络加速器大多采用 **TUN / TAP 虚拟网卡模式**，强行改写 Windows 系统的默认网关：
- ❌ **严重抢占网络**：导致开发环境（Docker、WSL2、Hyper-V）、公司内网 VPN、日常代理工具（如 Throne、Clash）产生全局路由死锁与冲突；
- ❌ **不适配第三方登录器**：魔兽私服普遍依赖定制登录器（如 `FirestormLauncher.exe`）进行动态补丁与验证，商业加速器往往难以精准拦截；
- ❌ **网络代答虚假延迟**：部分驱动工具（如 Proxifier）会在本地内核虚假代答 SYN，制造 2ms 延迟假象，实际游戏依然严重卡顿。

**本项目彻底打破以上痛点**：不创建任何虚拟网卡、不修改系统默认路由、不干扰现有代理。通过**用户态 Winsock API Hook 注入**与**随机端口隔离**，实现毫米级的精准 TCP 进程加速，并结合 **Hysteria 2 UDP QUIC 专线** 与 **Cloudflare 穿透双冗余**，打造高可用、零冲突、可归档的进程级加速方案。

---

## 🏗️ 核心原理与设计哲学

```mermaid
flowchart TD
    subgraph UserSpace["用户桌面与操作区"]
        Launcher["第三方登录器\n(FirestormLauncher.exe)"]
        Game["游戏主程序\n(WoW 11.2.5 - Firestorm.exe)"]
        GUI["Win32 原生控制台\n(launcher_gui.exe)"]
    end

    subgraph CoreEngine["本地加速引擎 (零驱动 / 零网关修改)"]
        HookDLL["hook_proxy.dll\n(MinHook 挂钩 connect/WSAConnect)"]
        SingBox["Sing-box 独立核心\n(动态随机端口监听, 如 52471)"]
        Config["proxy_config.ini\n(端口热更新调度)"]
    end

    subgraph UplinkTunnel["跨国加密专线链路"]
        HY2["极速专线: Hysteria 2\n(独立 VPS 直连 / UDP QUIC 暴力抗丢包)"]
        CF_Pri["主订阅专线: VLESS + WS\n(Cloudflare Anycast 穿透)"]
        CF_Bak["备订阅专线: VLESS + WS\n(Cloudflare 穿透冗余)"]
    end

    subgraph RemoteTarget["远端欧洲游戏服务器"]
        Server["Firestorm 世界服务器\n(141.95.207.63 : 8285 / 8286 / 1119)"]
    end

    Launcher -->|拉起| Game
    GUI -->|1. 协商分配高位空闲端口| SingBox
    GUI -->|2. 写入配置| Config
    GUI -->|3. 后台监听并自动注入| HookDLL
    HookDLL -.->|注入宿主| Game

    Game -->|TCP connect 目标 141.95.207.63| HookDLL
    HookDLL -->|拦截并重定向 (Socks5 握手)| SingBox
    Config -.->|热读取最新端口| HookDLL

    SingBox -->|首选直连 (RTT ~230ms)| HY2
    SingBox -.->|备用切换 (RTT ~900ms)| CF_Pri
    SingBox -.->|备用切换| CF_Bak

    HY2 -->|稳定交付| Server
    CF_Pri -.->|交付| Server
```

### 关键架构原则：
1. **纯应用层透明重定向**：在游戏调用 `connect()` / `WSAConnect()` 的瞬间，将目标 IP `141.95.207.63` 掉包为 `127.0.0.1:<随机端口>`，并在完成 Socks5 握手后返回 `0` 欺骗游戏进程，游戏无感走专线通信。
2. **端口动态安全协商**：每次启动 Sing-box 时，动态探测系统可用高位端口（`50000–65535`），避开系统保留段，杜绝多实例或与其他软件端口冲突。
3. **DLL 运行时配置热更新**：`hook_proxy.dll` 在拦截连接时动态检查 `proxy_config.ini` 修改，GUI 无论何时更换端口，DLL 毫秒级自动热同步，无需重启游戏。
4. **真实物理 RTT 测量**：放弃不可靠的纯 TCP 握手测量，通过本地混合端口发起真实的端到端应用层 HTTP 204 网络往返探测，反映 100% 真实的物理延迟。

---

## ✨ 功能特性

| 功能模块 | 说明与优势 |
| :--- | :--- |
| **🚀 零 TUN 进程级加速** | 基于 MinHook 挂钩 Winsock，完全在用户态实现流量改道，不装虚拟网卡、不改路由表。 |
| **⚡ Hysteria 2 极速专线** | 原生集成基于 UDP QUIC 的 Hysteria 2 独立 IP 节点，实测端到端延迟仅 **230~260 ms**，强抗 30% 丢包。 |
| **🛡️ 端口冲突免疫** | 启动时自动绑定 0 端口向系统申请空闲高位端口，自动写入配置，100% 杜绝 `10061` 冲突。 |
| **🔄 主/备订阅一键拉取** | 界面底部直接配置主订阅与备订阅 URL，一键从远端自动拉取最新节点并清洗写入 `config.json`。 |
| **💉 登录器全自动监控注入** | 内置高效的后台快照检测线程，命中游戏进程瞬间自动注入，支持先开登录器或先开加速器。 |
| **📊 纯净无乱码测速对比** | 全面修复 Windows 管道编码错位问题，提供【直连可用性诊断】与【专线真实 RTT】直观双向对比。 |
| **⏱️ 10秒实时对比监控** | 支持前台一键开启后台持续监控，每隔 10 秒向日志区刷新一条加速 vs 未加速的端到端对比走势。 |
| **🛠️ Proxifier 深度兼容** | 彻底探明内核驱动 `ProxifierDrv` 本地代答原理，提供一键 `demand`（按需启动）配置工具。 |

---

## ⚖️ 双加速方案全景对比

本项目沉淀了两种成熟的加速实现，用户可根据环境与权限需求自由选择：

| 维度 | 【推荐】C++ Winsock Hook 方案 (`c/`) | 【备用】Python 副 IP 方案 (`py/`) |
| :--- | :--- | :--- |
| **技术实现** | DLL 注入 + MinHook 拦截 `connect()` | Windows IP Alias（副 IP 绑定）+ 本地中继 |
| **权限要求** | 普通用户权限即可使用（注入已运行进程） | **必须管理员权限**（需要执行 `netsh` 绑副 IP） |
| **游戏登录器适配** | **完美适配**（支持自动监控，游戏随后启动自动挂载） | 适中（需先挂副 IP，再由登录器启动） |
| **系统副作用** | **无任何系统副作用**，进程退出即自动销毁 | 需确保退出时摘除副 IP，异常退出需清理 |
| **适用场景** | 日常游戏、多开客户端、不想频繁弹出 UAC 提权 | 无法或不允许使用 DLL 注入的环境 |

---

## 🚀 快速开始指南

### 方式一：C++ 原生控制台（日常推荐，最省心）

1. **直接启动控制台**：
   双击运行根目录的快捷入口：
   ```cmd
   启动加速器.bat
   ```
   *(或直接双击运行 `c\launcher_gui.exe`)*。

2. **界面操作步骤**：
   - **步骤 1：查看或测速**：点击 **【⚡ 测速】**，日志框将打印当前直连连通性与专线真实 RTT。
   - **步骤 2：选择节点**：下拉菜单支持切换 `极速专线-HY2`、`主节点-0001 (新加坡)` 或 `备节点-0709 (新加坡)`。
   - **步骤 3：开启加速**：
     - **如果游戏尚未运行**：点击 **【🚀 开启自动监控注入】**，随后打开您的游戏登录器（`FirestormLauncher.exe`）启动游戏，程序将自动完成挂载加速！
     - **如果游戏已经运行**：点击 **【💉 立即注入当前运行的游戏】**，即可瞬间无感挂载加速。

---

### 方式二：Python 托盘 / 副 IP 中继（免注入场景备选）

如果您在某些特殊环境下不希望注入 DLL，可使用 Python 副 IP 方案：

```cmd
py\firestorm_tray.cmd        # 启动零依赖 Win32 原生托盘控制台
```
- 点击托盘菜单中的【启动加速】即可一键挂载副 IP 并启动用户态中继；
- 退出托盘时会自动安全摘除副 IP，恢复系统原状。

---

## 🛠️ 编译与构建指南

本项目源码结构清晰，预置了一键自动化编译脚本，支持任何主流 64 位 C++ 工具链。

### 环境要求
- **操作系统**：Windows 10 / 11 x64
- **构建工具**：CMake >= 3.20，Ninja 构建系统
- **编译器**：MSVC (Visual Studio 2022 / Build Tools) 或 LLVM Clang / MinGW-w64
- **脚本解释器**：Python >= 3.10

### 一键编译命令
打开 PowerShell 执行：
```powershell
cd E:\code\game-tcp-speed\c
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

编译产物将自动生成并复制到 `c/` 根目录：
- `c/hook_proxy.dll`：Winsock 拦截核心动态链接库（约 50KB）
- `c/launcher_gui.exe`：纯 Win32 原生控制台程序（约 120KB，零外部 GUI 运行时依赖）

---

## 🔬 深度技术内幕与踩坑实录

本项目在研发过程中解决了一系列底层网络协议、Windows 内核驱动与编码兼容的典型疑难杂症，特记录如下以备归档查阅：

### 1. MinHook 拦截 connect 与 127.0.0.1 保护
- **挑战**：当 Hook `connect` 和 `WSAConnect` 时，如果无脑重定向所有流量，会导致本地内部进程通信（IPC）死锁，甚至导致后续连接本地 Sing-box 代理自身时陷入死循环递归。
- **解决**：在 [`c/dll/hook_proxy.cpp`](file:///E:/code/game-tcp-speed/c/dll/hook_proxy.cpp) 中维护线程局部变量 `g_InHook`，显式跳过 `127.0.0.1` 环回流量，并在重定向建立本地 Socket 时直接调用原始函数指针 `pOriginalConnect`。

### 2. 避免 10061 错误：DLL 运行时配置热更新机制
- **挑战**：由于 Sing-box 启动时采用动态随机端口（如 53126），早期的 DLL 仅在 `DllMain` 加载时读取一次配置。如果游戏早于配置写入前被注入，DLL 内存中保留旧端口（2081），发包直接被操作系统拒绝（`WSAECONNREFUSED / 10061`）。
- **解决**：在 DLL 中引入 `RefreshConfigIfNeeded()`，在每次触发拦截时按时间戳（1 秒节流）动态检查 `proxy_config.ini`，当端口变更时直接在内存中无缝热更新，彻底解决端口脱节。

### 3. 探明 Proxifier 内核代答伪握手（2ms 假象）
- **现象**：在测速时，直连游戏服无论如何都显示 `2.9ms`，但实际游戏根本打不开。
- **真相**：系统后台驻留了 `ProxifierDrv.sys`（内核 WFP 过滤驱动，`STATE: 4 RUNNING`）。该驱动在内核层截获了所有 TCP 连接并在本地直接伪造 `SYN-ACK`，造成 2ms 假象，随后发包被掐断抛出 `10054`。
- **治理**：
  1. 在 `Default.ppx` 规则最顶层为游戏与 Python 注入 `Direct` 放行规则；
  2. 研发 [`tools/set_proxifier_demand.cmd`](file:///E:/code/game-tcp-speed/tools/set_proxifier_demand.cmd)，将驱动启动模式调整为 `demand`（按需启动）。平时不加载，保持系统协议栈纯净；启动 Proxifier 时自动按需唤醒。

### 4. Windows 管道与 CMD 编码乱码治理
- **现象**：Python 测速输出重定向到 GUI 管道时出现 `\uFFFD`（菱形问号），批处理脚本运行输出乱码。
- **成因**：
  - Windows CMD 双击运行批处理默认按系统代码页 **GBK (CP936)** 解析，若批处理为 UTF-8 无 BOM 会造成汉字错位；
  - Python 在 Windows 管道模式下默认回退至系统 ANSI 编码，导致 UTF-8 宽字符转换截断。
- **方案**：
  - 将所有辅助批处理脚本文件保存为纯正的 **ANSI / GBK** 编码；
  - GUI 启动 Python 命令时强制指定 `python -X utf8`；
  - C++ 端转换宽字符采用 `MB_ERR_INVALID_CHARS`，解码失败自动无缝降级到 `CP_ACP`，杜绝任何乱码。

---

## 📁 项目完整归档目录

```text
game-tcp-speed/
├── ai.md                             # AI 工作交接规范文档 (遵循 https://ai.drx.ac.cn/aimd 规范)
├── README.md                         # 本项目全景技术文档与使用说明
├── 启动加速器.bat                     # 根目录一键启动快捷方式 (纯 GBK 编码)
│
├── c/                                # 【核心生产级】C++ Winsock Hook 与原生 GUI
│   ├── CMakeLists.txt                # CMake 构建脚本 (Ninja 构建系统)
│   ├── build.ps1 / build.bat         # 自动化一键编译脚本
│   ├── hook_proxy.dll                # Winsock 动态拦截核心 (支持端口热更新)
│   ├── launcher_gui.exe              # 原生 Win32 控制台界面
│   ├── proxy_config.ini              # 运行时随机端口与配置
│   ├── test_speed.py                 # 真实端到端网络测速脚本 (真实 RTT 测试)
│   ├── update_sub.py                 # 纯净版主/备订阅拉取与清洗更新脚本
│   ├── dll/hook_proxy.cpp            # MinHook 拦截实现源码
│   ├── gui/launcher_gui.cpp          # Win32 原生界面与注入引擎源码
│   └── minhook/                      # MinHook x64 开源底层库
│
├── py/                               # 【备选方案】Python 副 IP 劫持与用户态中继
│   ├── firestorm_accel.py            # 副 IP 挂载与中继核心
│   ├── firestorm_tray.py             # 纯 Win32 零依赖托盘控制台
│   ├── run_accel.cmd                 # 启动脚本 (走本地代理)
│   ├── run_accel_singbox.cmd         # 启动脚本 (走独立 Sing-box)
│   └── stop_accel.cmd                # 停止并摘除副 IP 脚本
│
├── Sing-box/                         # 独立 Sing-box 核心环境
│   ├── sing-box.exe                  # Sing-box v1.14.2 稳定版核心
│   ├── config.json                   # 核心配置 (HY2 专线 + 主备 CF 隧道)
│   ├── start_singbox.py              # 高位安全随机端口启动脚本
│   ├── stop_singbox.py               # 核心安全停止脚本
│   └── portof.py                     # 端口查询辅助工具
│
├── tools/                            # 系统协议栈与底层驱动维护工具 (纯 GBK)
│   ├── set_proxifier_demand.cmd      # 将 ProxifierDrv 驱动改为按需启动 (demand)
│   ├── disable_proxifier_driver.cmd  # 彻底停止并禁用 Proxifier 驱动
│   └── enable_proxifier_driver.cmd   # 恢复 Proxifier 驱动运行
│
└── docs/                             # 技术设计与效果验证文档
    ├── 加速器效果验证.md             # 延迟与丢包率对比报告
    └── 独立sing-box使用说明.md        # 独立核心架构设计手册
```

---

## 📄 许可证与归档声明

- 本项目核心代码基于 **[MIT License](LICENSE)** 开源。
- 依赖项目致谢：[MinHook](https://github.com/TsudaKageyu/minhook)（Tsuda Kageyu）、[Sing-box](https://github.com/SagerNet/sing-box)（SagerNet）。
- **归档声明**：本项目所有核心业务逻辑、底层 Hook 拦截、多协议支持、驱动冲突治理及自动化构建工具链均已全部经过实机验证并封存，可作为进程级网络加速与 Winsock 流量重定向的工业级参考实现。
