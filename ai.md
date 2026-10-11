# AI 协作与维护指南 (AI Collaboration & Maintenance Guide)

[中文说明](#-中文说明) • [English Documentation](#-english-documentation)

---

## 🇨🇳 中文说明

> 本文档是给 AI 助手与开发者的交接规范文档，进行代码维护与二次开发时请严格遵循以下准则。

### 核心约束与注意事项

1. **严禁引入 TUN / TAP 虚拟网卡**：
   - 本项目的核心原则是“纯应用层 Winsock Hook 拦截”，绝不允许创建虚拟网卡或修改系统默认路由网关，确保与开发者机器上的现有网络、VPN 及系统代理共存。
2. **纯 C++ 原生实现，严禁引入 Python 依赖**：
   - 客户端已全面完成纯 C++ 原生迁移，内置 Winsock 原生测速与 WinINet 原生订阅下载，不可再回退添加 `.py` 脚本依赖。
3. **套接字非阻塞保护机制**：
   - 修改 `hook_proxy.cpp` 时必须维护对 `ioctlsocket`、`WSAEventSelect` 和 `WSAAsyncSelect` 的状态还原机制。在握手完成后必须把 Socket 还原为非阻塞模式，否则会导致魔兽世界主线程卡死无响应。
4. **AnyTLS 协议纯净度保障**：
   - 对外服网游（特别是使用 SRP 认证的魔兽世界私服），严禁在 AnyTLS 配置中开启 `padding_scheme`，否则二进制包长度被篡改将导致客户端在 10ms 内断开连接。
5. **动态高位随机端口与配置热同步**：
   - 核心启动时必须动态分配随机高位端口（`50000–65535`），并通过 `proxy_config.ini` 实现毫秒级热更新，杜绝端口冲突（`10061`）。
6. **信息安全与去敏感化**：
   - 严禁将真实 VPS IP、WebUI 密码、订阅 Token 及真实公钥提交至公开仓库，所有示例统一使用占位符（如 `<SERVER_IP>`、`<YOUR_TOKEN>`）。

---

## 🇺🇸 English Documentation

> This document serves as the maintenance handover guide for AI assistants and contributors.

### Core Guidelines & Architectural Invariants

1. **Strictly No TUN/TAP Virtual Adapters**:
   - The fundamental architecture is user-mode Winsock API hooking. Do not introduce virtual network adapters or modify system routing tables, preventing conflicts with existing VPNs or developer network interfaces.
2. **Pure Native C++ Without Python Dependencies**:
   - The client is fully compiled in pure C++ Win32 with native Winsock RTT testing and WinINet subscription management. Do not reintroduce Python runtime scripts.
3. **Non-Blocking Socket Mode Preservation**:
   - When modifying `hook_proxy.cpp`, preserve the state tracking for `ioctlsocket`, `WSAEventSelect`, and `WSAAsyncSelect`. The socket must be restored to non-blocking mode after Socks5 handshakes to prevent main-thread game lockups.
4. **Strict Transparency in AnyTLS Configuration**:
   - Never enable `padding_scheme` for games using binary SRP handshakes (like World of Warcraft). Packet length tampering causes immediate client disconnects.
5. **Dynamic High-Port Negotiation**:
   - Always allocate unreserved random high ports (`50000–65535`) on launch and synchronize via `proxy_config.ini` to avoid `10061` connection refused errors.
6. **Security & Redaction**:
   - Never commit sensitive VPS credentials, IP addresses, or tokens. Always use standardized placeholders (`<SERVER_IP>`, `<YOUR_TOKEN>`).
