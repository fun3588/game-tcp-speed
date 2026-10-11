# 独立 Sing-box 运行说明 (Standalone Sing-box User Manual)

[中文说明](#-中文说明) • [English Documentation](#-english-documentation)

---

## 🇨🇳 中文说明

本项目附带了用于流量转发的独立 Sing-box 核心及其配置。

### 核心特性

1. **进程隔离**：
   - 作为独立后台进程运行，不与其他本地代理软件共享端口或配置文件。
2. **AnyTLS + Reality 配置**：
   - 采用标准 TCP 出站，去除 `padding_scheme` 填充，避免修改游戏客户端的二进制握手包格式。
3. **动态端口监听**：
   - 监听类型为 `mixed`（兼容 HTTP 与 SOCKS5），由外层启动器在 `50000–65535` 范围内动态分配可用端口，避免固定端口被其他程序占用。

### 常用命令

- **检查配置文件语法**：
  ```cmd
  sing-box.exe check -c config.json
  ```
- **命令行启动**：
  ```cmd
  sing-box.exe run -c config.json
  ```

---

## 🇺🇸 English Documentation

This directory contains the standalone Sing-box core and its AnyTLS configuration for traffic forwarding.

### Core Features

1. **Process Isolation**:
   - Runs independently as a local process without sharing ports or configurations with third-party proxy clients.
2. **AnyTLS + Reality Configuration**:
   - Standard TCP transport without `padding_scheme` length padding, avoiding modifications to binary game packets.
3. **Dynamic Port Inbound**:
   - Uses `mixed` inbound (supporting HTTP and SOCKS5) bound to a high port (`50000–65535`) allocated dynamically by the launcher to prevent port conflicts.

### Common Commands

- **Validate Configuration**:
  ```cmd
  sing-box.exe check -c config.json
  ```
- **Run in Command Line**:
  ```cmd
  sing-box.exe run -c config.json
  ```
