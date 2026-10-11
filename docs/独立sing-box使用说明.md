# 独立 Sing-box 运行手册 (Standalone Sing-box User Manual)

[中文说明](#-中文说明) • [English Documentation](#-english-documentation)

---

## 🇨🇳 中文说明

本目录包含了用于游戏流量转发的独立 Sing-box 核心及其 AnyTLS 纯净专线配置。

### 核心特性

1. **独立运行隔离**：
   - 与本地任何其他第三方代理软件（如 Throne、Clash 等）完全隔离，不共享端口、不共用进程。
2. **AnyTLS + Reality 纯净配置**：
   - 采用标准 TCP 封装出站，彻底移除 `padding_scheme`，确保魔兽世界等网游 SRP 二进制握手数据包零修改、零篡改透明直达目标海外服务器。
3. **随机动态端口入站**：
   - 入站类型为 `mixed`（同时支持 HTTP 与 SOCKS5 代理），每次由外层启动器在 `50000–65535` 范围内动态分配可用端口，彻底杜绝端口冲突（`10061`）。

### 常用命令

- **语法检查配置**：
  ```cmd
  sing-box.exe check -c config.json
  ```
- **命令行测试运行**：
  ```cmd
  sing-box.exe run -c config.json
  ```

---

## 🇺🇸 English Documentation

This directory contains the standalone Sing-box core and its clean AnyTLS dedicated configuration.

### Core Features

1. **Process & Network Isolation**:
   - Completely independent from third-party proxies (e.g., Throne, Clash). No shared ports or process interference.
2. **Clean AnyTLS + Reality Configuration**:
   - Uses standard TCP transport without `padding_scheme`, ensuring zero payload manipulation for World of Warcraft binary SRP handshakes.
3. **Dynamic Port Mixed Inbound**:
   - Supports both HTTP and SOCKS5 on dynamically assigned ports (`50000–65535`), avoiding `10061` port conflicts.

### Common Commands

- **Validate Configuration**:
  ```cmd
  sing-box.exe check -c config.json
  ```
- **Run in Command Line**:
  ```cmd
  sing-box.exe run -c config.json
  ```
