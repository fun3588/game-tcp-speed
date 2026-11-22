# AI 协作与维护指南 v3.1.3 (AI Collaboration & Maintenance Guide v3.1.3)

[中文说明](#-中文说明) • [English Documentation](#-english-documentation)

---

## 🇨🇳 中文说明

> 本文档是给 AI 助手与开发者的交接规范文档，进行代码维护与二次开发时请严格遵循以下准则。

### 核心约束与架构规范 (v3.1.3)

1. **纯经典 Win32 API + GDI 架构准则**：
   - 官方唯一客户端全栈采用纯 C/C++ 编译，前端为 `c/gui/launcher_gui.cpp`（纯原生 Win32 API + GDI），底层为 `c/dll/hook_proxy.cpp`；
   - **严禁引入 WebView2、Electron、WPF 或重型第三方渲染库**，确保可执行文件单文件体积 ≤ 350 KB（静态链接 libstdc++/libgcc/winpthread 并链接期 `-s` 剥离符号后的体积上限），运行时内存常驻在 **3 MB ~ 5 MB**，启动即开；
   - 原 C# WPF 方案已封存归档于 `c#/GameTcpSpeed.Wpf` 目录作为历史参考，日常构建与发布全部基于 `bin/v3.1` 与 `build_all.bat`（构建链只用 `.bat`，避免执行策略与签名问题）；
   - **PowerShell 只在无法替代处保留**：当前仅 `tools/*.cmd` 的 UAC 自提权一行使用 `-Command` 形式（不受执行策略限制）。除此之外不得新增 `.ps1` 脚本文件，也不得把已改为 `.bat` 的构建链退回脚本形式。
2. **极简纯净 Hook 原则 (Minimal Pure Hook Invariant)**：
   - 底层 Hook 模块**仅挂钩 `connect` 与 `WSAConnect`**，严禁挂钩 `closesocket`、`ioctlsocket`、`WSAEventSelect`、`WSAAsyncSelect` 或 `WSAIoctl`；
   - 不在内存中维护任何 Socket 上下文映射表（`g_SocketMap`），消除锁争用。所有发送接收统一基于超时 `select()`；连接一旦建立，后续数据流与过图关闭 100% 走系统原生代码。
3. **严禁引入 TUN / TAP 虚拟网卡**：
   - 坚持“用户态 Winsock 行内 Hook 拦截”原则，严禁安装虚拟网卡或改写系统默认路由网关。
4. **严禁对 `mswsock.dll` / `ConnectEx` 执行机器码篡改**：
   - Windows 10/11 x64 下 `mswsock.dll` 内部指令高度经过编译器优化与 CFG 插桩，包含基于 `RIP` 的相对寻址指令。MinHook 搬迁至匿名跳板页后相对偏移失真，过图调用 IOCP 时会触发非法内存访问崩溃（`ERROR #132 ACCESS_VIOLATION`）；
   - 必须放行所有完成端口扩展查询直通 Windows 原生协议栈，绝对严禁对 `mswsock.dll` 函数体进行 MinHook Inline Patch。
5. **模块钉住与卸载排空原则（Unload Safety Invariant，v3.1.3）**：
   - 注入成功后必须对 `hook_proxy.dll` 执行**模块钉住**（固定模块句柄并持有引用），严禁允许游戏进程或第三方工具提前 `FreeLibrary` 造成代码页失效；
   - 卸载必须严格按「置停止标志 → Drain 排空飞行中的 `connect` → 还原原始字节 → 释放钉住」顺序执行，禁止直接还原字节后立刻返回；
   - Drain 实际耗时必须写入日志，便于现场定位滞留线程；
   - **已废弃**：`HookedWSAEventSelect` / `HookedWSAAsyncSelect` 之类的事件解绑清理逻辑自 v3.1.2 起已随钩子整体移除，新增代码严禁恢复此类辅助钩子。
6. **IPv6 泄露拦截与强制回退原则 (IPv6 Fallback Invariant)**：
   - 针对国内宽带分配公网 IPv6 导致魔兽直连暴露真实 IP 并触发防挂多 IP 封号机制的问题，DLL 必须主动拦截 `AF_INET6` 协议族连接并返回 `WSAEAFNOSUPPORT`；
   - 强制游戏客户端网络协议栈平滑回退至 IPv4，确保 100% 流量进入加速专线隧道。
7. **零副作用预连接状态探针 (Zero-Side-Effect Socket Probe)**：
   - 检测代理是否在线时，**严禁**直接拿游戏的主通信 Socket 去尝试连接代理（失败后 Windows Winsock 会将原 Socket 置为不可重用状态，导致后续直连产生 `WSAEINVAL 10022` 或 `WSAEISCONN 10056`）；
   - 必须使用短暂独立的探针套接字 (`CheckProxyPortAlive`) 确认端口就绪，保证游戏原始 Socket 零副作用回退。
8. **启动起飞就绪锁 (Pre-flight Readiness Lock)**：
   - 启动 Sing-box 后不可立即注入游戏进程，必须执行紧凑就绪探测（50ms 循环），验证代理入站端口处于完全监听状态后方可放行注入，杜绝“核心未就绪先注入导致游戏断线”的问题。
9. **AnyTLS 协议纯净度保障**：
   - 对外服网游（特别是使用 SRP 认证的魔兽世界私服），严禁在 AnyTLS 配置中开启 `padding_scheme`，否则二进制包长度被篡改将导致客户端在 10ms 内断开连接。
10. **中英双语（i18n）维护规范**：
    - 所有新增 UI 文本、按钮和诊断信息必须通过 `L(L"中文", L"English")` 辅助宏在 `ApplyLanguage()` 中同步维护，支持运行时右上角一键热切换。
11. **信息安全与去敏感化**：
    - 严禁将真实 VPS IP、WebUI 密码、订阅 Token 及真实私钥提交至公开仓库，所有示例统一使用占位符。
12. **平滑旁路降级与远程卸载保障**：
    - 底层拦截模块在本地代理未运行或 `Proxy.Enabled=0` 时必须自动平滑直连原始目标地址，杜绝因核心停止造成游戏断网；
    - 必须提供远程 `FreeLibrary` 卸载与界面一键卸载功能，确保加速器关闭或卸载后游戏 100% 恢复原生网络。
13. **版本号单一来源规范 (Version Sync Invariant)**：
    - 版本号共存在于 4 处，升级时必须同步递增：`c/app.rc` 的 `FILEVERSION`/`PRODUCTVERSION` 与 `FileVersion` 字符串、`c/gui/launcher_gui.cpp` 的窗口标题/关于框/初始化日志、`c/dll/hook_proxy.cpp` 的 `GameTcpSpeed_GetVersion()` 返回值、以及 `README.md` / `ai.md` / `llms.txt` / `c/README.md` 的文档标识；
    - 严禁出现「代码升版本、外壳标识未升」的错配（v3.1.3 曾出现 DLL 报 3.1.3 而 exe 属性仍为 3.1.2 的问题）。

---

## 🇺🇸 English Documentation

> This document serves as the maintenance handover guide for AI assistants and contributors for v3.1.3.

### Core Guidelines & Architectural Invariants (v3.1.3)

1. **Pure Win32 API + GDI Ultra-Lightweight Standard**:
   - The primary client is built purely with native Win32 API + GDI (`c/gui/launcher_gui.cpp`) and MinHook (`c/dll/hook_proxy.cpp`).
   - **Strictly No WebView2, Electron, or Heavy CLR Runtimes**. Memory footprint must remain within **3 ~ 5 MB RAM**, single executable ≤ 350 KB (static libstdc++/libgcc/winpthread, link-time `-s` stripped), instant launch.
   - Legacy C# WPF project is archived under `c#/GameTcpSpeed.Wpf`. Releases are packaged under `bin/v3.1`.
2. **Minimal Pure Hook Invariant**:
   - The hook DLL may hook **only** `connect` and `WSAConnect`. Never re-introduce hooks on `closesocket`, `ioctlsocket`, `WSAEventSelect`, `WSAAsyncSelect` or `WSAIoctl`, and never maintain a socket context map (`g_SocketMap`).
3. **Strictly No TUN/TAP Virtual Adapters**:
   - Operates purely through user-space Winsock API hooking. Never introduce virtual NICs or modify routing tables.
4. **Strictly No Machine Code Splicing on `mswsock.dll` / `ConnectEx` (Anti-Trampoline Invariant)**:
   - Modern Windows 11 x64 binaries utilize RIP-relative relocations and CFG instrumentation. MinHook relocations to arbitrary trampoline memory pages cause address displacement faults (`ERROR #132 ACCESS_VIOLATION`). IOCP extensions must flow directly through native Winsock without inline bytecode modification.
5. **Module Pinning & Drain Barrier Invariant (v3.1.3)**:
   - After injection the module must be pinned (handle fixed and reference held) so that neither the game nor third-party tools can `FreeLibrary` it prematurely.
   - Unload must follow the strict order: set stop flag → drain all in-flight `connect` calls → restore original bytes → release pin. Drain duration must be logged.
   - **Deprecated**: `WSAEventSelect` / `WSAAsyncSelect` unbind clean-up logic was removed together with those hooks in v3.1.2. Do not reinstate it.
6. **Non-Blocking Preservation**:
   - Intercept standard `connect` / `WSAConnect`. Sockets must be restored to non-blocking mode with `FD_CONNECT` / `SetEvent` signals to prevent game lockups.
7. **IPv6 Leak Defense & Forced IPv4 Fallback**:
   - For game clients attempting domestic IPv6 connections (which leak local IP and trigger security bans), the hook DLL must intercept `AF_INET6` and return `WSAEAFNOSUPPORT` to force game fallback to IPv4.
8. **Zero-Side-Effect Socket Health Probe**:
   - Never attempt connecting to the local proxy with the game's own primary socket during health validation, which would render the socket unusable (`WSAEINVAL (10022)`). Use isolated ephemeral probe sockets instead.
9. **Pre-flight Readiness Lock**:
   - When Sing-box starts, enforce an active 50ms polling loop until the local port accepts connections before unlocking process injection.
10. **Strict Transparency in AnyTLS Configuration**:
    - Never enable `padding_scheme` for games using binary SRP handshakes.
11. **Bilingual Localization Standards**:
    - All user-facing strings must support instant dynamic switching via `L(L"Chinese", L"English")` in `ApplyLanguage()`.
12. **Security & Redaction**:
    - Never commit private VPS IP addresses, passwords, or secret tokens.
13. **Clean Ejection & Auto-Bypass**:
    - The hook layer must degrade gracefully to direct connections if Sing-box is offline.
    - Remote `FreeLibrary` unhooking must safely restore unmodified Winsock states.
14. **Version Sync Invariant**:
    - The version string lives in four places and must always be bumped together: `c/app.rc` (`FILEVERSION` / `PRODUCTVERSION` / `FileVersion` string), `c/gui/launcher_gui.cpp` (window title, about box, init log), `c/dll/hook_proxy.cpp` (`GameTcpSpeed_GetVersion()` return value), and the docs (`README.md`, `ai.md`, `llms.txt`, `c/README.md`).
    - Never ship a build where the code version and the shell metadata disagree (v3.1.3 once reported 3.1.3 from the DLL while the exe properties still showed 3.1.2).
