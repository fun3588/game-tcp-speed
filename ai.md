# AI 协作与维护指南 v3.0 (AI Collaboration & Maintenance Guide v3.0)

[中文说明](#-中文说明) • [English Documentation](#-english-documentation)

---

## 🇨🇳 中文说明

> 本文档是给 AI 助手与开发者的交接规范文档，进行代码维护与二次开发时请严格遵循以下准则。

### 核心约束与架构规范 (v3.0)

1. **纯经典 Win32 API + GDI 架构准则**：
   - 官方唯一客户端全栈采用纯 C/C++ 编译，前端为 `c/gui/launcher_gui.cpp`（纯原生 Win32 API + GDI），底层为 `c/dll/hook_proxy.cpp`；
   - **严禁引入 WebView2、Electron、WPF 或重型第三方渲染库**，确保可执行文件单文件体积 ≤ 200 KB，运行时内存常驻在 **3 MB ~ 5 MB**，启动即开；
   - 原 C# WPF 方案已封存归档于 `c#/GameTcpSpeed.Wpf` 目录作为历史参考，日常构建与发布全部基于 `bin/v3.0` 与 `build_all.ps1`。
2. **严禁引入 TUN / TAP 虚拟网卡**：
   - 坚持“用户态 Winsock 行内 Hook 拦截”原则，严禁安装虚拟网卡或改写系统默认路由网关。
3. **套接字非阻塞保护与 ConnectEx 支持**：
   - 必须同时拦截标准 `connect`、`WSAConnect` 与异步完成端口 `ConnectEx`（以及 `WSAIoctl` 查询），保障已在运行中的游戏可免重启平滑接管；
   - 握手完成后必须把 Socket 还原为游戏期望的非阻塞状态，并补发 `FD_CONNECT` / `SetEvent` 信号，避免游戏主线程死锁。
4. **AnyTLS 协议纯净度保障**：
   - 对外服网游（特别是使用 SRP 认证的魔兽世界私服），严禁在 AnyTLS 配置中开启 `padding_scheme`，否则二进制包长度被篡改将导致客户端在 10ms 内断开连接。
5. **中英双语（i18n）维护规范**：
   - 所有新增 UI 文本、按钮和诊断信息必须通过 `L(L"中文", L"English")` 辅助宏在 `ApplyLanguage()` 中同步维护，支持运行时右上角一键热切换。
6. **信息安全与去敏感化**：
   - 严禁将真实 VPS IP、WebUI 密码、订阅 Token 及真实私钥提交至公开仓库，所有示例统一使用占位符。
7. **平滑旁路降级与远程卸载保障**：
   - 底层拦截模块在本地代理未运行或 `Proxy.Enabled=0` 时必须自动平滑直连原始目标地址，杜绝因核心停止造成游戏断网；
   - 必须提供远程 `FreeLibrary` 卸载与界面一键卸载功能，确保加速器关闭或卸载后游戏 100% 恢复原生网络。
8. **游戏目录探测与真实直连测试原则**：
   - 优先扫描 `E:\gamel\The War Within` 等实机安装路径，解析 `WTF\Config.wtf` 提取真实服务器 IP；
   - 探针测试必须基于本机直连执行原始 Socket 测量，反映真实物理 RTT，严禁伪造测试结果。

---

## 🇺🇸 English Documentation

> This document serves as the maintenance handover guide for AI assistants and contributors for v3.0.

### Core Guidelines & Architectural Invariants (v3.0)

1. **Pure Win32 API + GDI Ultra-Lightweight Standard**:
   - The primary client is built purely with native Win32 API + GDI (`c/gui/launcher_gui.cpp`) and MinHook (`c/dll/hook_proxy.cpp`).
   - **Strictly No WebView2, Electron, or Heavy CLR Runtimes**. Memory footprint must remain within **3 ~ 5 MB RAM**, single executable ≤ 200 KB, instant launch.
   - Legacy C# WPF project is archived under `c#/GameTcpSpeed.Wpf`.
2. **Strictly No TUN/TAP Virtual Adapters**:
   - Operates purely through user-space Winsock API hooking. Never introduce virtual NICs or modify routing tables.
3. **Non-Blocking Preservation & Asynchronous ConnectEx**:
   - Intercept both standard `connect` / `WSAConnect` and asynchronous IOCP `ConnectEx` (plus `WSAIoctl`).
   - Sockets must be restored to non-blocking mode with `FD_CONNECT` / `SetEvent` signals to prevent game lockups.
4. **Strict Transparency in AnyTLS Configuration**:
   - Never enable `padding_scheme` for games using binary SRP handshakes.
5. **Bilingual Localization Standards**:
   - All user-facing strings must support instant dynamic switching via `L(L"Chinese", L"English")` in `ApplyLanguage()`.
6. **Security & Redaction**:
   - Never commit private VPS IP addresses, passwords, or secret tokens.
7. **Clean Ejection & Auto-Bypass**:
   - The hook layer must degrade gracefully to direct connections if Sing-box is offline.
   - Remote `FreeLibrary` unhooking must safely restore unmodified Winsock states.
8. **Game Directory Endpoint Inspection**:
   - Endpoints should be derived from local game directories (`WTF\Config.wtf`) with direct physical RTT benchmarks.
