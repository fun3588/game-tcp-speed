# C 模块：纯经典 Win32 API + GDI 极速客户端与 Winsock Hook 核心 (v3.0)

[中文说明](#-中文说明) • [English Documentation](#-english-documentation)

---

## 🇨🇳 中文说明

本目录包含 **Winsock API Hook** 进程级 TCP 拦截动态链接库（`hook_proxy.dll`）以及 v3.0 纯经典 Win32 旗舰图形客户端（`GameTcpSpeed.exe`）。

### v3.0 核心特性

1. **纯经典 Win32 API + GDI (零外部依赖)**：
   - 彻底摒弃一切 WebView2、Electron 与 .NET 托管运行时的数百兆内存开销；
   - 运行时内存仅 **3 ~ 5 MB**，启动毫秒级秒开，单文件仅 **174 KB**；
   - 纯原生 Windows 控件与 GDI 绘制，零 DirectX 依赖，在任何纯净精简系统上均可直接运行。
2. **中英双语即时一键热切换**：
   - 窗口右上角集成 `[ 🇨🇳 简体中文 / 🇺🇸 English ]` 原生按钮，点击瞬间重载界面所有文本。
3. **异步完成端口 `ConnectEx` 挂钩**：
   - 不仅拦截传统 `connect` 与 `WSAConnect`，更主动挂钩大型 3D 游戏（如魔兽世界 11.2.5）大量采用的异步 IOCP `ConnectEx`（及 `WSAIoctl` 扩展查询）；
   - 实现**已运行中游戏免重启平滑接管**。
4. **智能直连旁路与远程热卸载（Clean Eject）**：
   - 动态监控代理核心状态，当本地代理未开启或不可用时，自动平滑退回原生直连，游戏绝不断网；
   - 支持通过远程线程安全执行 `FreeLibrary` 卸载注入模块并解除所有 MinHook Winsock 钩子，可在不重启游戏的前提下立即恢复原生直连。

### 编译构建

打开 PowerShell 执行：
```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```
产物输出至当前目录：
- `GameTcpSpeed.exe`：纯经典 Win32 API + GDI 双语版主程序 (174 KB)。
- `hook_proxy.dll`：注入核心动态链接库。

---

## 🇺🇸 English Documentation

This directory contains the core process-level TCP proxy DLL (`hook_proxy.dll`) and the v3.0 native Win32 GUI client (`GameTcpSpeed.exe`).

### Key v3.0 Features

1. **Pure Win32 API + GDI (Zero Dependencies)**:
   - Eliminates WebView2/.NET overhead, reducing memory footprint to **3 ~ 5 MB RAM**. Starts in `< 0.01s`, single binary only **174 KB**.
   - Zero DirectX or GPU requirement, runs out-of-the-box on any stripped Windows system.
2. **Instant Bilingual Toggle**:
   - Integrated top-right toggle button to switch between Chinese and English in <1ms.
3. **Asynchronous IOCP `ConnectEx` Interception**:
   - Hooks both synchronous `connect`/`WSAConnect` and asynchronous IOCP `ConnectEx`, enabling **live process takeover without restarting games**.
4. **Clean Remote Ejection & Fallback Bypass**:
   - Safely unhooks via `FreeLibrary` to restore native direct connectivity on demand; automatic bypass if Sing-box stops.

### Build Instructions

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```
Outputs:
- `GameTcpSpeed.exe`: Pure Win32 API + GDI bilingual client.
- `hook_proxy.dll`: Winsock proxy hook module.
