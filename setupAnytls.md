# AnyTLS + Reality 服务端配置与守护部署参考

> **适用场景**：跨国网络游戏加速节点搭建、低延迟 TCP 代理链路。  
> **说明**：去除 Padding 长度填充保持流式传输，调整 Linux 网络参数降低 NAT 丢包与异常重置，配合 Go 编写的守护程序实现 sing-box 进程监控与订阅下发。

---

## 目录
- [一、 架构概览与设计思想](#一-架构概览与设计思想)
- [二、 核心避坑指南（网游断连的致命原因）](#二-核心避坑指南网游断连的致命原因)
- [三、 新 VPS 一键自动化部署脚本（推荐）](#三-新-vps-一键自动化部署脚本推荐)

---

## 一、 架构概览与设计思想

```
┌────────────────────────────────────────────────────────────────────────┐
│                        本地电脑 (Windows / macOS)                       │
│                                                                        │
│   魔兽世界 / 游戏主程序 (TCP 1119/8285/3724)                           │
│        │                                                               │
│        ▼ (自研注入 EXE 进程级 Hook 拦截 / 零 TUN 网卡)                  │
│   客户端核心 (自研加速内核 / AnyTLS 客户端隧道)                        │
└────────────────────────────────────┬───────────────────────────────────┘
                                     │ AnyTLS + Reality (标准 TCP 隧道)
                                     ▼ (无 Padding 填充，原生流式直传)
┌────────────────────────────────────────────────────────────────────────┐
│                        海外 VPS 服务器 (Linux)                          │
│                                                                        │
│   系统层: BBR + FQ 拥塞控制 | 禁用 FastOpen / TW复用 (防游戏 RST/丢包)  │
│                                                                        │
│   管理层: proxy_manager (Go 语言单二进制)                              │
│       ├─ WebUI 控制台 (端口 3589，Basic Auth: admin / 自定义密码)       │
│       ├─ 聚合订阅 API (/sub，Token 鉴权免密直接拉取)                   │
│       ├─ 流量与速率监控 (定时轮询 127.0.0.1:9090 clash_api)            │
│       └─ 进程看门狗 (cmd.Wait 秒级心跳守护，异常崩溃 1 秒内自动拉起)     │
│                                                                        │
│   核心层: sing-box (端口 30927/tcp)                                    │
│       └─ inbounds: anytls + reality (SNI: gateway.icloud.com)          │
│       └─ outbounds: direct (直连出站)                                  │
└────────────────────────────────────┬───────────────────────────────────┘
                                     │ 骨干内网极速直通 (~5ms)
                                     ▼
┌────────────────────────────────────────────────────────────────────────┐
│              目标游戏服务器 (如海外魔兽私服/官服 8285/1119/3724)         │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 二、 核心避坑指南（网游断连的致命原因）

在实际网络游戏加速场景中，常规的翻墙/代理配置往往会导致网游登录报错或进游戏秒弹“已从服务器断开连接”。以下为必须遵守的核心原则：

| 错误配置项 | 为什么会导致网游断连？ | 正确做法 |
| :--- | :--- | :--- |
| **`padding_scheme`（长度混淆填充）** | AnyTLS 的长度填充方案会强制改变数据包尺寸。魔兽世界私服或官方核心的 SRP 二进制认证包对字节长度极度敏感，客户端收到填充包后判定校验失败，在 **10毫秒内主动调用断开连接**。 | **彻底删除 `padding_scheme`**，保持纯净流式直传。 |
| **`net.ipv4.tcp_fastopen = 3`** | 暴雪战网和私服的 Realm 登录服务器不兼容 TCP Fast Open（带有早期数据的 SYN 包）。遇到 TFO 数据段时，暴雪服务器会直接回送 **TCP RST（强制重置）**。 | **必须设为 `0` 彻底禁用**。 |
| **`net.ipv4.tcp_tw_reuse = 1`** | 国内家庭宽带均处于光猫/路由器 NAT 之后。开启连接复用后，NAT 设备重写的时间戳出现微小抖动，会触发内核 **PAWS（防回绕）丢包机制**，战网连续握手包被内核静默丢弃。 | **必须设为 `0` 彻底禁用**。 |
| **`tcp_notsent_lowat = 16384`** | 该参数限制本地未发送队列，虽然对 Web 吞吐有利，但会导致游戏长连接套接字频繁报 `EAGAIN` 写入阻塞，触发超时断线。 | **恢复系统默认（不限制）**。 |
| **固定端口规则代理（遗漏非标端口）** | 常规代理工具或写死端口的规则只转发了官方端口（如 `1119` 和 `3724`），而网游私服的世界服常运行在非标端口（如 `8285`）。一旦非标端口漏包走本地直连宽带，服务器检测到认证 IP（国外节点）与进入世界的游戏 IP（国内直连）不一致，会直接判定异常踢出并断开连接。 | **必须使用自研注入 EXE（进程级 Hook 拦截）**：直接注入游戏主程序进程，将该进程发起的**所有 TCP 出站流量（全端口无遗漏，包括 1119/8285/3724 等）100% 完整劫持**并重定向至专线隧道，零 TUN 网卡、杜绝漏包。 |
| **Reality 密钥不匹配** | 服务端私钥更新后，若客户端未同步更新公钥，Reality 握手会被作为非法连接直接拦截（日志报错 `processed invalid connection`）。 | 服务端与订阅下发公钥需保持**严格一致**，修改配置后客户端需重新更新订阅。 |

---

## 三、 新 VPS 一键自动化部署脚本（推荐）

在新 VPS 上（支持 Debian 11/12/13、Ubuntu 20/22/24），只需以 root 身份运行以下自动化 Python 脚本（可在本地执行远程连接，也可直接在 VPS 本地执行）：

```python
#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
AnyTLS Gaming + Watchdog WebUI 一键自动化部署脚本
可在本地 Windows/Linux 运行（通过 SSH 远程部署），也可在 VPS 本机运行。
"""

import paramiko
import sys
import time

# ==================== VPS 配置信息 ====================
VPS_HOST = "你的VPS_IP"
VPS_PORT = 22
VPS_USER = "root"
VPS_PASS = "你的ROOT密码"

# 控制台登录凭据（部署前请修改为你自己的强密码）
WEB_USER = "admin"
WEB_PASS = "YourSecurePassword123"
WEB_PORT = 3589
# ====================================================

INSTALL_SHELL = f"""#!/bin/bash
set -e

echo "=== [1/6] 系统环境检查与安装基础依赖 ==="
apt-get update -qq && apt-get install -y -qq curl wget jq openssl golang-go iproute2 iptables

# 获取公网 IPv4
PUBLIC_IP=$(curl -s4 https://api64.ipify.org || curl -s4 https://ifconfig.me)
echo "检测到公网 IP: $PUBLIC_IP"

# 抽取 10000~60000 未占用随机端口
get_random_port() {{
    local port
    while true; do
        port=$((RANDOM % 50000 + 10000))
        if ! ss -tuln | grep -q ":$port "; then
            echo "$port"
            break
        fi
    done
}}
ANYTLS_PORT=$(get_random_port)
echo "分配 AnyTLS 端口: $ANYTLS_PORT"

echo "=== [2/6] 应用网游黄金 TCP 网络栈优化 ==="
cat << 'EOF' > /etc/sysctl.d/99-gaming-tcp.conf
# 网游专线黄金 TCP 参数
net.ipv4.ip_forward = 1
net.ipv6.conf.all.forwarding = 1
net.core.default_qdisc = fq
net.ipv4.tcp_congestion_control = bbr

# 关键：禁用 Fast Open 与 TW 复用，杜绝游戏 RST 和 NAT 丢包
net.ipv4.tcp_fastopen = 0
net.ipv4.tcp_tw_reuse = 0

# 滑动窗口与选择性确认 (保留 64MB 极限 BDP)
net.ipv4.tcp_window_scaling = 1
net.ipv4.tcp_sack = 1
net.ipv4.tcp_dsack = 1
net.ipv4.tcp_timestamps = 1

# 保活与大并发队列
net.ipv4.tcp_keepalive_time = 300
net.ipv4.tcp_keepalive_intvl = 30
net.ipv4.tcp_keepalive_probes = 5
net.core.somaxconn = 65535
net.core.netdev_max_backlog = 65535
net.ipv4.tcp_max_syn_backlog = 65535

# 读写滑动窗口
net.core.rmem_max = 67108864
net.core.wmem_max = 67108864
net.core.rmem_default = 16777216
net.core.wmem_default = 16777216
net.ipv4.tcp_rmem = 4096 87380 67108864
net.ipv4.tcp_wmem = 4096 65536 67108864
fs.file-max = 2097152
EOF
sysctl -p /etc/sysctl.d/99-gaming-tcp.conf

echo "=== [3/6] 安装 sing-box 最新内核 ==="
ARCH=$(uname -m)
case "$ARCH" in
  x86_64) SB_ARCH="amd64" ;;
  aarch64|arm64) SB_ARCH="arm64" ;;
  *) echo "不支持的架构: $ARCH"; exit 1 ;;
esac

LATEST_TAG=$(curl -s https://api.github.com/repos/SagerNet/sing-box/releases/latest | grep '"tag_name":' | sed -E 's/.*"([^"]+)".*/\\1/')
TAR_NAME="sing-box-${{LATEST_TAG#v}}-linux-${{SB_ARCH}}.tar.gz"
DOWNLOAD_URL="https://github.com/SagerNet/sing-box/releases/download/${{LATEST_TAG}}/${{TAR_NAME}}"

cd /tmp
rm -rf sing-box*
curl -Lo sing-box.tar.gz "$DOWNLOAD_URL"
tar -xzf sing-box.tar.gz
install -m 755 sing-box-*/sing-box /usr/local/bin/sing-box
rm -rf sing-box*

echo "=== [4/6] 生成凭证与写入纯净 sing-box 配置 ==="
WORKDIR="/root/anytls_hy2"
mkdir -p ${{WORKDIR}}/bin /etc/sing-box
cp /usr/local/bin/sing-box ${{WORKDIR}}/bin/sing-box

KEYPAIR=$(/usr/local/bin/sing-box generate reality-keypair)
PRIV=$(echo "$KEYPAIR" | grep "PrivateKey" | awk '{{print $2}}')
PUB=$(echo "$KEYPAIR" | grep "PublicKey" | awk '{{print $2}}')
SID=$(openssl rand -hex 8)
ANYTLS_PASS=$(openssl rand -base64 16 | tr -dc 'a-zA-Z0-9')
SUB_TOKEN=$(openssl rand -hex 16)
CLASH_SECRET=$(openssl rand -hex 16)

# 写入纯净 sing-box 配置文件 (无 padding_scheme，保证游戏封包透明直通)
cat << EOF > /etc/sing-box/config.json
{{
  "log": {{
    "level": "info",
    "timestamp": true
  }},
  "experimental": {{
    "clash_api": {{
      "external_controller": "127.0.0.1:9090",
      "secret": "${{CLASH_SECRET}}"
    }}
  }},
  "inbounds": [
    {{
      "type": "anytls",
      "tag": "anytls-in",
      "listen": "::",
      "listen_port": ${{ANYTLS_PORT}},
      "users": [
        {{
          "name": "user1",
          "password": "${{ANYTLS_PASS}}"
        }}
      ],
      "tls": {{
        "enabled": true,
        "server_name": "gateway.icloud.com",
        "reality": {{
          "enabled": true,
          "handshake": {{
            "server": "gateway.icloud.com",
            "server_port": 443
          }},
          "private_key": "${{PRIV}}",
          "short_id": ["${{SID}}"]
        }}
      }}
    }}
  ],
  "outbounds": [
    {{
      "type": "direct",
      "tag": "direct"
    }}
  ]
}}
EOF

/usr/local/bin/sing-box check -c /etc/sing-box/config.json

# 写入节点元数据
cat << EOF > ${{WORKDIR}}/anytls_hy2.json
{{
  "publicIp": "${{PUBLIC_IP}}",
  "anytlsPort": ${{ANYTLS_PORT}},
  "anytlsPassword": "${{ANYTLS_PASS}}",
  "realityPublicKey": "${{PUB}}",
  "realityShortId": "${{SID}}",
  "realitySni": "gateway.icloud.com",
  "webPort": {WEB_PORT},
  "subToken": "${{SUB_TOKEN}}",
  "clashApiSecret": "${{CLASH_SECRET}}",
  "subUrl": "http://${{PUBLIC_IP}}:{WEB_PORT}/sub?token=${{SUB_TOKEN}}",
  "singboxSubUrl": "http://${{PUBLIC_IP}}:{WEB_PORT}/sub?token=${{SUB_TOKEN}}&format=singbox",
  "createdAt": "$(date -Iseconds)"
}}
EOF
chmod 600 ${{WORKDIR}}/anytls_hy2.json

echo "=== [5/6] 编写并编译 Go 看门狗与 Web 控制台 ==="
cd ${{WORKDIR}}
cat << 'EOFGO' > ${{WORKDIR}}/main.go
package main

import (
	"bufio"
	"bytes"
	"encoding/base64"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

type ConfigMeta struct {
	PublicIP         string `json:"publicIp"`
	AnytlsPort       int    `json:"anytlsPort"`
	AnytlsPassword   string `json:"anytlsPassword"`
	RealityPublicKey string `json:"realityPublicKey"`
	RealityShortId   string `json:"realityShortId"`
	RealitySni       string `json:"realitySni"`
	WebPort          int    `json:"webPort"`
	SubToken         string `json:"subToken"`
	ClashApiSecret   string `json:"clashApiSecret"`
	SubUrl           string `json:"subUrl"`
	SingboxSubUrl    string `json:"singboxSubUrl"`
	CreatedAt        string `json:"createdAt"`
}

type ResourceSample struct {
	Timestamp int64   `json:"timestamp"`
	CPU       float64 `json:"cpu"`
	MemoryMB  float64 `json:"memory_mb"`
}

type AppState struct {
	mu          sync.RWMutex
	meta        ConfigMeta
	cmd         *exec.Cmd
	pid         int
	status      string
	restarts    int
	upSpeed     int64
	downSpeed   int64
	totalUp     int64
	totalDown   int64
	history     []ResourceSample
	webUser     string
	webPassword string
	webPort     int
}

var state AppState

func getCPUTime(pid int) (int64, int64, error) {
	data, err := os.ReadFile(fmt.Sprintf("/proc/%d/stat", pid))
	if err != nil {
		return 0, 0, err
	}
	fields := strings.Fields(string(data))
	if len(fields) < 15 {
		return 0, 0, fmt.Errorf("stat format error")
	}
	utime, _ := strconv.ParseInt(fields[13], 10, 64)
	stime, _ := strconv.ParseInt(fields[14], 10, 64)

	sysData, err := os.ReadFile("/proc/stat")
	if err != nil {
		return 0, 0, err
	}
	lines := strings.Split(string(sysData), "\n")
	var totalSys int64
	for _, l := range lines {
		if strings.HasPrefix(l, "cpu ") {
			f := strings.Fields(l)
			for i := 1; i < len(f); i++ {
				v, _ := strconv.ParseInt(f[i], 10, 64)
				totalSys += v
			}
			break
		}
	}
	return utime + stime, totalSys, nil
}

func getMemoryRSS(pid int) float64 {
	data, err := os.ReadFile(fmt.Sprintf("/proc/%d/status", pid))
	if err != nil {
		return 0
	}
	scanner := bufio.NewScanner(bytes.NewReader(data))
	for scanner.Scan() {
		line := scanner.Text()
		if strings.HasPrefix(line, "VmRSS:") {
			parts := strings.Fields(line)
			if len(parts) >= 2 {
				kb, _ := strconv.ParseFloat(parts[1], 64)
				return kb / 1024.0
			}
		}
	}
	return 0
}

func startSingBox() {
	state.mu.Lock()
	defer state.mu.Unlock()

	cmd := exec.Command("/usr/local/bin/sing-box", "run", "-c", "/etc/sing-box/config.json")
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	err := cmd.Start()
	if err != nil {
		log.Printf("[Watchdog] 启动 sing-box 失败: %v", err)
		state.status = "Failed"
		return
	}
	state.cmd = cmd
	state.pid = cmd.Process.Pid
	state.status = "Running"
	log.Printf("[Watchdog] 看门狗已锁定守护 sing-box, PID: %d", state.pid)

	go func(c *exec.Cmd, p int) {
		_ = c.Wait()
		log.Printf("[Watchdog] 检测到 sing-box (PID %d) 退出，看门狗将在 1 秒内自动拉起...", p)
		state.mu.Lock()
		state.restarts++
		state.status = "Restarting"
		state.mu.Unlock()
		time.Sleep(1 * time.Second)
		startSingBox()
	}(cmd, state.pid)
}

func resourceMonitorLoop() {
	var lastProcTime, lastSysTime int64

	for {
		time.Sleep(3 * time.Second)
		state.mu.RLock()
		pid := state.pid
		state.mu.RUnlock()

		if pid <= 0 {
			continue
		}

		pTime, sTime, err := getCPUTime(pid)
		var cpuPct float64
		if err == nil && lastProcTime > 0 && sTime > lastSysTime {
			deltaProc := float64(pTime - lastProcTime)
			deltaSys := float64(sTime - lastSysTime)
			cpuPct = (deltaProc / deltaSys) * 100.0
			if cpuPct < 0 {
				cpuPct = 0
			}
		}
		if err == nil {
			lastProcTime = pTime
			lastSysTime = sTime
		}

		rssMB := getMemoryRSS(pid)

		state.mu.Lock()
		sample := ResourceSample{
			Timestamp: time.Now().Unix(),
			CPU:       cpuPct,
			MemoryMB:  rssMB,
		}
		state.history = append(state.history, sample)
		if len(state.history) > 60 {
			state.history = state.history[len(state.history)-60:]
		}
		state.mu.Unlock()
	}
}

func clashStatsLoop() {
	for {
		time.Sleep(2 * time.Second)
		url := "http://127.0.0.1:9090/traffic"
		req, err := http.NewRequest("GET", url, nil)
		if err != nil {
			continue
		}
		state.mu.RLock()
		sec := state.meta.ClashApiSecret
		state.mu.RUnlock()
		if sec != "" {
			req.Header.Set("Authorization", "Bearer "+sec)
		}

		client := &http.Client{Timeout: 3 * time.Second}
		resp, err := client.Do(req)
		if err != nil {
			continue
		}

		reader := bufio.NewReader(resp.Body)
		for {
			line, err := reader.ReadBytes('\n')
			if err != nil {
				resp.Body.Close()
				break
			}
			var m struct {
				Up   int64 `json:"up"`
				Down int64 `json:"down"`
			}
			if err := json.Unmarshal(line, &m); err == nil {
				state.mu.Lock()
				state.upSpeed = m.Up
				state.downSpeed = m.Down
				state.totalUp += m.Up * 2
				state.totalDown += m.Down * 2
				state.mu.Unlock()
			}
		}
	}
}

func basicAuth(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		user, pass, ok := r.BasicAuth()
		state.mu.RLock()
		expectedUser := state.webUser
		expectedPass := state.webPassword
		state.mu.RUnlock()

		if !ok || user != expectedUser || pass != expectedPass {
			w.Header().Set("WWW-Authenticate", `Basic realm="AnyTLS Manager"`)
			http.Error(w, "Unauthorized", http.StatusUnauthorized)
			return
		}
		next(w, r)
	}
}

func handleStatus(w http.ResponseWriter, r *http.Request) {
	state.mu.RLock()
	defer state.mu.RUnlock()
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{
		"status":     state.status,
		"pid":        state.pid,
		"restarts":   state.restarts,
		"publicIp":   state.meta.PublicIP,
		"anytlsPort": state.meta.AnytlsPort,
		"webPort":    state.webPort,
	})
}

func handleStats(w http.ResponseWriter, r *http.Request) {
	state.mu.RLock()
	defer state.mu.RUnlock()
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{
		"upSpeed":   state.upSpeed,
		"downSpeed": state.downSpeed,
		"totalUp":   state.totalUp,
		"totalDown": state.totalDown,
	})
}

func handleResources(w http.ResponseWriter, r *http.Request) {
	state.mu.RLock()
	defer state.mu.RUnlock()
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(state.history)
}

func handleSub(w http.ResponseWriter, r *http.Request) {
	token := r.URL.Query().Get("token")
	state.mu.RLock()
	meta := state.meta
	state.mu.RUnlock()

	if token == "" || token != meta.SubToken {
		http.Error(w, "Invalid Token", http.StatusForbidden)
		return
	}

	format := r.URL.Query().Get("format")

	anytlsLink := fmt.Sprintf("anytls://%s@%s:%d?security=reality&sni=%s&pbk=%s&sid=%s&fp=chrome&type=tcp&insecure=1&allowInsecure=1#AnyTLS-Gaming",
		meta.AnytlsPassword, meta.PublicIP, meta.AnytlsPort, meta.RealitySni, meta.RealityPublicKey, meta.RealityShortId)

	if format == "singbox" {
		w.Header().Set("Content-Type", "application/json; charset=utf-8")
		sbConfig := fmt.Sprintf(`{
  "outbounds": [
    {
      "type": "selector",
      "tag": "select",
      "outbounds": ["anytls-out"]
    },
    {
      "type": "anytls",
      "tag": "anytls-out",
      "server": "%s",
      "server_port": %d,
      "password": "%s",
      "tls": {
        "enabled": true,
        "server_name": "%s",
        "insecure": true,
        "utls": { "enabled": true, "fingerprint": "chrome" },
        "reality": {
          "enabled": true,
          "public_key": "%s",
          "short_id": "%s"
        }
      }
    },
    { "type": "direct", "tag": "direct" }
  ]
}`, meta.PublicIP, meta.AnytlsPort, meta.AnytlsPassword, meta.RealitySni, meta.RealityPublicKey, meta.RealityShortId)
		w.Write([]byte(sbConfig))
		return
	}

	raw := fmt.Sprintf("%s\n", anytlsLink)
	encoded := base64.StdEncoding.EncodeToString([]byte(raw))
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.Write([]byte(encoded))
}

const htmlTemplate = `<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>AnyTLS Gaming 游戏节点控制台</title>
  <style>
    :root {
      --bg: #0b1120;
      --card: #1e293b;
      --border: #334155;
      --accent: #f59e0b;
      --accent2: #38bdf8;
      --accent3: #10b981;
      --text: #f8fafc;
      --muted: #94a3b8;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background: var(--bg);
      color: var(--text);
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "PingFang SC", sans-serif;
      padding: 1.5rem;
      line-height: 1.6;
    }
    .container { max-width: 960px; margin: 0 auto; }
    header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 1.5rem;
      padding-bottom: 1rem;
      border-bottom: 1px solid var(--border);
    }
    h1 { font-size: 1.35rem; color: var(--accent); }
    .badge {
      padding: 0.3rem 0.75rem;
      border-radius: 9999px;
      font-size: 0.82rem;
      font-weight: bold;
      background: #064e3b;
      color: #6ee7b7;
      display: inline-flex;
      align-items: center;
      gap: 6px;
    }
    .badge::before {
      content: "";
      display: inline-block;
      width: 8px;
      height: 8px;
      border-radius: 50%;
      background: #10b981;
      animation: pulse 2s infinite;
    }
    @keyframes pulse {
      0% { transform: scale(0.95); opacity: 0.8; }
      50% { transform: scale(1.2); opacity: 1; }
      100% { transform: scale(0.95); opacity: 0.8; }
    }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 1.2rem; margin-bottom: 1.5rem; }
    .card {
      background: var(--card);
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 1.25rem;
      box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.2);
    }
    .card-title { font-size: 1rem; font-weight: 600; color: var(--accent2); margin-bottom: 0.85rem; display: flex; justify-content: space-between; }
    .stat-row { display: flex; justify-content: space-between; margin: 0.45rem 0; font-size: 0.9rem; border-bottom: 1px dashed rgba(255,255,255,0.06); padding-bottom: 4px; }
    .stat-val { font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace; color: var(--text); font-weight: bold; }
    .input-group {
      display: flex;
      margin-top: 0.65rem;
      gap: 0.5rem;
    }
    input[type="text"] {
      flex: 1;
      background: #0f172a;
      border: 1px solid var(--border);
      color: #38bdf8;
      padding: 0.5rem 0.7rem;
      border-radius: 6px;
      font-family: monospace;
      font-size: 0.85rem;
    }
    button {
      background: #2563eb;
      color: white;
      border: none;
      padding: 0.5rem 0.9rem;
      border-radius: 6px;
      cursor: pointer;
      font-size: 0.85rem;
      font-weight: 500;
      transition: background 0.2s;
      white-space: nowrap;
    }
    button:hover { background: #1d4ed8; }
    button.copied { background: #059669; }
  </style>
</head>
<body>
  <div class="container">
    <header>
      <div>
        <h1>AnyTLS Gaming 游戏节点控制台</h1>
        <p style="color:var(--muted);font-size:0.85rem;">内核: sing-box &middot; 看门狗心跳守护 &middot; 魔兽世界私服 5ms 内网直达</p>
      </div>
      <div>
        <span class="badge" id="statusBadge">看门狗守护中 (PID: <span id="appPid">--</span>)</span>
      </div>
    </header>

    <div class="grid">
      <div class="card">
        <div class="card-title">
          <span>📊 实时流量与网络速率</span>
          <span style="font-size:0.75rem;color:var(--muted);font-weight:normal;">2秒自动刷新</span>
        </div>
        <div class="stat-row"><span>实时下行速率:</span><span class="stat-val" style="color:#38bdf8;" id="dispDownSpeed">↓ 0.0 KB/s</span></div>
        <div class="stat-row"><span>实时上行速率:</span><span class="stat-val" style="color:#f59e0b;" id="dispUpSpeed">↑ 0.0 KB/s</span></div>
        <div class="stat-row"><span>累计下载流量:</span><span class="stat-val" id="dispTotalDown">0 MB</span></div>
        <div class="stat-row"><span>累计上传流量:</span><span class="stat-val" id="dispTotalUp">0 MB</span></div>
        <div class="stat-row"><span>网络连接状态:</span><span class="stat-val" style="color:#10b981;" id="dispConnStatus">正常运行</span></div>
      </div>

      <div class="card">
        <div class="card-title">
          <span>🛡️ 进程看门狗守护状态</span>
          <span style="font-size:0.75rem;color:var(--muted);font-weight:normal;">心跳保护</span>
        </div>
        <div class="stat-row"><span>节点公网 IP:</span><span class="stat-val" id="dispIp">--</span></div>
        <div class="stat-row"><span>AnyTLS 端口:</span><span class="stat-val" id="dispPort">--</span></div>
        <div class="stat-row"><span>内核 PID:</span><span class="stat-val" id="dispPid">--</span></div>
        <div class="stat-row"><span>故障自动重启:</span><span class="stat-val" style="color:#10b981;" id="dispRestarts">0 次</span></div>
        <div class="stat-row"><span>内核 CPU / 内存:</span><span class="stat-val" id="dispRes">--</span></div>
      </div>
    </div>

    <div class="card" style="margin-bottom: 1.2rem;">
      <div class="card-title">🔗 专属聚合订阅（已集成跳过 TLS 校验）</div>
      <p style="color:var(--muted);font-size:0.8rem;">支持客户端直接导入与定时更新：</p>
      
      <div style="margin-top:0.6rem;">
        <span style="font-size:0.8rem;color:var(--muted);">通用 Base64 订阅:</span>
        <div class="input-group">
          <input type="text" id="subUrlInput" readonly>
          <button onclick="copyField('subUrlInput', this)">复制通用订阅</button>
        </div>
      </div>
    </div>

    <div class="card">
      <div class="card-title" style="color:var(--accent);">🎮 AnyTLS + Reality 极简节点链接</div>
      <p style="color:var(--muted);font-size:0.8rem;">SNI: gateway.icloud.com &middot; 零 Padding 干扰 &middot; 魔兽世界极低延迟</p>
      <div class="input-group">
        <input type="text" id="anytlsLink" readonly>
        <button onclick="copyField('anytlsLink', this)">复制节点链接</button>
      </div>
    </div>
  </div>

  <script>
    function copyField(id, btn) {
      const el = document.getElementById(id);
      el.select();
      navigator.clipboard.writeText(el.value).then(() => {
        const orig = btn.innerText;
        btn.innerText = "已复制✓";
        btn.classList.add("copied");
        setTimeout(() => {
          btn.innerText = orig;
          btn.classList.remove("copied");
        }, 1500);
      });
    }

    function formatBytes(bytes) {
      if (!bytes || bytes === 0) return '0.0 B';
      const k = 1024, dm = 1;
      const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
      const i = Math.floor(Math.log(bytes) / Math.log(k));
      return parseFloat((bytes / Math.pow(k, i)).toFixed(dm)) + ' ' + sizes[i];
    }

    async function updateData() {
      try {
        const sRes = await fetch('/api/status');
        const sData = await sRes.json();
        document.getElementById('dispIp').innerText = sData.publicIp;
        document.getElementById('dispPort').innerText = sData.anytlsPort + ' (TCP)';
        document.getElementById('dispPid').innerText = sData.pid;
        document.getElementById('appPid').innerText = sData.pid;
        document.getElementById('dispRestarts').innerText = sData.restarts + ' 次';

        const stRes = await fetch('/api/stats');
        const stData = await stRes.json();
        document.getElementById('dispDownSpeed').innerText = '↓ ' + formatBytes(stData.downSpeed) + '/s';
        document.getElementById('dispUpSpeed').innerText = '↑ ' + formatBytes(stData.upSpeed) + '/s';
        document.getElementById('dispTotalDown').innerText = formatBytes(stData.totalDown);
        document.getElementById('dispTotalUp').innerText = formatBytes(stData.totalUp);

        const rRes = await fetch('/api/resources');
        const rData = await rRes.json();
        if (rData && rData.length > 0) {
          const last = rData[rData.length - 1];
          document.getElementById('dispRes').innerText = last.cpu.toFixed(1) + '% / ' + last.memory_mb.toFixed(1) + ' MB';
        }
      } catch(e) {
        console.error(e);
      }
    }

    window.onload = function() {
      fetch('/anytls_hy2.json').then(r=>r.json()).then(meta=>{
        document.getElementById('subUrlInput').value = meta.subUrl;
        
        const anytlsUrl = "anytls://" + meta.anytlsPassword + "@" + meta.publicIp + ":" + meta.anytlsPort + "?security=reality&sni=" + meta.realitySni + "&pbk=" + meta.realityPublicKey + "&sid=" + meta.realityShortId + "&fp=chrome&type=tcp&insecure=1&allowInsecure=1#AnyTLS-Gaming";
        document.getElementById('anytlsLink').value = anytlsUrl;
      });
      updateData();
      setInterval(updateData, 2000);
    };
  </script>
</body>
</html>
`

func handleIndex(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write([]byte(htmlTemplate))
}

func handleMetaJson(w http.ResponseWriter, r *http.Request) {
	state.mu.RLock()
	defer state.mu.RUnlock()
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(state.meta)
}

func main() {
	portFlag := flag.Int("port", {WEB_PORT}, "Web 控制台端口")
	userFlag := flag.String("user", "{WEB_USER}", "Web 控制台用户名")
	passFlag := flag.String("password", "{WEB_PASS}", "Web 控制台登录密码")
	flag.Parse()

	state.webPort = *portFlag
	state.webUser = *userFlag
	state.webPassword = *passFlag

	metaBytes, err := os.ReadFile("/root/anytls_hy2/anytls_hy2.json")
	if err != nil {
		log.Fatalf("无法读取 anytls_hy2.json: %v", err)
	}
	if err := json.Unmarshal(metaBytes, &state.meta); err != nil {
		log.Fatalf("解析 json 失败: %v", err)
	}

	startSingBox()
	go resourceMonitorLoop()
	go clashStatsLoop()

	http.HandleFunc("/", basicAuth(handleIndex))
	http.HandleFunc("/anytls_hy2.json", basicAuth(handleMetaJson))
	http.HandleFunc("/api/status", basicAuth(handleStatus))
	http.HandleFunc("/api/stats", basicAuth(handleStats))
	http.HandleFunc("/api/resources", basicAuth(handleResources))
	http.HandleFunc("/sub", handleSub)

	server := &http.Server{{Addr: fmt.Sprintf(":%d", state.webPort)}}
	stopChan := make(chan os.Signal, 1)
	signal.Notify(stopChan, os.Interrupt, syscall.SIGTERM)
	go func() {{
		<-stopChan
		state.mu.Lock()
		if state.cmd != nil && state.cmd.Process != nil {{
			state.cmd.Process.Kill()
		}}
		state.mu.Unlock()
		server.Close()
		os.Exit(0)
	}}()

	log.Printf("[Manager] 启动在 :%d", state.webPort)
	server.ListenAndServe()
}}
EOFGO

go mod init anytls_hy2 2>/dev/null || true
go build -o ${{WORKDIR}}/proxy_manager ${{WORKDIR}}/main.go
echo "proxy_manager 编译成功！"

echo "=== [6/6] 放行防火墙与配置 systemd 开机自启 ==="
iptables -I INPUT -p tcp --dport ${{ANYTLS_PORT}} -j ACCEPT || true
iptables -I INPUT -p tcp --dport {WEB_PORT} -j ACCEPT || true

cat << EOF > /etc/systemd/system/anytls_hy2.service
[Unit]
Description=AnyTLS Gaming Sing-box Watchdog & WebUI Manager
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/root/anytls_hy2
ExecStart=/root/anytls_hy2/proxy_manager -port {WEB_PORT} -user {WEB_USER} -password {WEB_PASS}
Restart=always
RestartSec=3
LimitNOFILE=1048576

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable anytls_hy2
systemctl restart anytls_hy2
sleep 3
systemctl status anytls_hy2 --no-pager

echo ""
echo "=========================================================="
echo "[+] 部署成功！"
echo "WebUI 控制台: http://${{PUBLIC_IP}}:{WEB_PORT}"
echo "用户名: {WEB_USER} | 密码: {WEB_PASS}"
echo "通用订阅地址: http://${{PUBLIC_IP}}:{WEB_PORT}/sub?token=${{SUB_TOKEN}}"
echo "=========================================================="
"""

def main():
    print(f"[*] 正在连接 VPS: {VPS_HOST} ...")
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        client.connect(VPS_HOST, VPS_PORT, VPS_USER, VPS_PASS, timeout=15)
        print("[+] SSH 连接成功，开始执行部署...")
    except Exception as e:
        print(f"[-] 连接失败: {e}")
        sys.exit(1)

    sftp = client.open_sftp()
    with sftp.file("/tmp/install_anytls_gaming.sh", "w") as f:
        f.write(INSTALL_SHELL)
    sftp.close()

    stdin, stdout, stderr = client.exec_command("chmod +x /tmp/install_anytls_gaming.sh && /tmp/install_anytls_gaming.sh")
    for line in iter(stdout.readline, ""):
        print(line, end="")
    err = stderr.read().decode('utf-8', errors='ignore')
    if err:
        print("\n[STDERR]:\n", err)

    client.close()

if __name__ == "__main__":
    main()
```
