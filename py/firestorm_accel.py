#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
firestorm_accel.py  ·  自制「加速器」原型
=========================================
把游戏发往指定服务器 IP 的 TCP 流量，悄悄改道走本地 SOCKS5 代理。
不需要 TUN、不需要 Proxifier、不需要注入 DLL、不需要自写驱动。

原理
----
  1) setup   : 把目标 IP 作为「副 IP」挂到本机（优先回环伪接口，失败则挂物理网卡
               并带 skipassource）。此后游戏发往该 IP 的连接会被操作系统投递到本机。
  2) run     : 在这些副 IP 的端口上监听。每个进来的连接，都经由 SOCKS5
               (默认 127.0.0.1:2080) 去连真正的 <目标IP>:<端口>（由代理节点侧发起），
               然后双向转发字节流。
  3) teardown: 摘掉副 IP，系统完全复原。

  游戏以为自己直连了 141.95.207.63，实际连接从你的代理节点出去 ——
  这条路更稳（Hysteria2 自带抗丢包）。实测同主机：直连 avg 577ms/抖动 344，
  经代理 avg 455ms/抖动 91。

用法（**必须以管理员身份运行**）
------------------------------
  python firestorm_accel.py status      # 看副 IP 在不在
  python firestorm_accel.py setup       # 加副 IP 并开始中继 (Ctrl+C 自动清理)
  python firestorm_accel.py run         # 只跑中继（副 IP 已存在时）
  python firestorm_accel.py teardown    # 摘掉副 IP
  python firestorm_accel.py selftest    # 无需管理员：用 127.0.0.1 本地端到端自测
"""

import argparse
import os
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import time

# ----------------------------- 可调参数 -----------------------------

TARGET_IP = "141.95.207.63"          # 游戏服务器(世界服+门户)公网 IP
PORTS = [1119, 8285, 8286, 3724, 8085, 8086]   # 需要接管的端口
SOCKS_HOST = "127.0.0.1"             # 本地 SOCKS5 (Throne)
SOCKS_PORT = 2080
SOCKS_USER = None                    # 如需认证再填
SOCKS_PASS = None
IFACE = None                         # 挂副 IP 的网卡索引；None=自动探测
USED_IFACE = None                    # 实际挂上的网卡索引（供清理用）
LOOPBACK_IFACE = 1                   # Windows 回环伪接口索引，通常是 1

LOG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "firestorm_accel.log")


def LOG(*a):
    """同时打到控制台和日志文件（提权窗口的输出我们也要能查）。"""
    line = " ".join(str(x) for x in a)
    print(line, flush=True)
    try:
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {line}\n")
    except Exception:
        pass

# ----------------------------- 小工具 -----------------------------

def run(cmd):
    """跑系统命令，返回 (rc, 合并输出文本)。中文 Windows 下 netsh 输出是 GBK。"""
    p = subprocess.run(cmd, shell=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    raw = p.stdout or b""
    for enc in ("utf-8", "gbk", "mbcs", "latin1"):
        try:
            return p.returncode, raw.decode(enc)
        except Exception:
            continue
    return p.returncode, raw.decode("utf-8", "replace")


def is_admin():
    rc, _ = run("net session")
    return rc == 0


def detect_iface():
    """自动探测默认路由所在网卡的 ifIndex。失败返回 None。"""
    rc, out = run('powershell -NoProfile -Command '
                  '"(Get-NetRoute -DestinationPrefix 0.0.0.0/0 | '
                  'Sort-Object RouteMetric | Select-Object -First 1).ifIndex"')
    for line in out.splitlines():
        line = line.strip()
        if line.isdigit():
            return int(line)
    return None


def alias_present(ip, port=0):
    """能否在本机 bind 到该 IP —— 能就说明副 IP 已挂上。"""
    s = socket.socket()
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((ip, port))
        return True
    except OSError:
        return False
    finally:
        s.close()

# ----------------------------- SOCKS5 -----------------------------

def _recvn(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise IOError("连接被对端关闭")
        buf += chunk
    return buf


def socks5_connect(dst_host, dst_port, timeout=15):
    s = socket.create_connection((SOCKS_HOST, SOCKS_PORT), timeout=timeout)
    s.settimeout(timeout)
    try:
        s.sendall(b"\x05\x02\x00\x02" if SOCKS_USER else b"\x05\x01\x00")
        ver, method = _recvn(s, 2)
        if ver != 5:
            raise IOError("SOCKS 版本错误")
        if method == 0x02:
            u = (SOCKS_USER or "").encode()
            p = (SOCKS_PASS or "").encode()
            s.sendall(b"\x01" + bytes([len(u)]) + u + bytes([len(p)]) + p)
            if _recvn(s, 2)[1] != 0:
                raise IOError("SOCKS 认证失败")
        elif method != 0x00:
            raise IOError("SOCKS 无可接受认证方式: %d" % method)

        try:
            req = (b"\x05\x01\x00\x01" + socket.inet_aton(dst_host)
                   + struct.pack(">H", dst_port))
        except OSError:
            h = dst_host.encode()
            req = (b"\x05\x01\x00\x03" + bytes([len(h)]) + h
                   + struct.pack(">H", dst_port))
        s.sendall(req)

        rep = _recvn(s, 4)
        if rep[1] != 0:
            raise IOError("SOCKS CONNECT 被拒, code=%d" % rep[1])
        atyp = rep[3]
        if atyp == 1:
            _recvn(s, 4)
        elif atyp == 3:
            _recvn(s, _recvn(s, 1)[0])
        elif atyp == 4:
            _recvn(s, 16)
        _recvn(s, 2)
        s.settimeout(None)
        return s
    except Exception:
        s.close()
        raise

# ----------------------------- 中继 -----------------------------

def _pipe(src, dst):
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            dst.sendall(data)
    except Exception:
        pass
    finally:
        try:
            dst.shutdown(socket.SHUT_WR)
        except Exception:
            pass


class Relay(socketserver.BaseRequestHandler):
    def handle(self):
        port = self.server.server_address[1]
        client = self.request
        try:
            upstream = socks5_connect(TARGET_IP, port)
        except Exception as e:
            LOG(f"  [x] :{port} 经代理建连失败: {e}")
            return
        LOG(f"  [>] :{port} 已接入 {client.getpeername()[0]} -> 代理 -> {TARGET_IP}:{port}")
        t = threading.Thread(target=_pipe, args=(client, upstream), daemon=True)
        t.start()
        _pipe(upstream, client)
        try:
            upstream.close()
        except Exception:
            pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def start_relay(bind_ip, ports):
    servers = []
    for p in ports:
        try:
            srv = Server((bind_ip, p), Relay)
        except OSError as e:
            LOG(f"  [!] 无法监听 {bind_ip}:{p} -> {e}")
            continue
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        servers.append(srv)
        LOG(f"  监听 {bind_ip}:{p}  (-> SOCKS5 {SOCKS_HOST}:{SOCKS_PORT} -> {TARGET_IP}:{p})")
    return servers

# ----------------------------- 子命令 -----------------------------

def add_alias(ip):
    global USED_IFACE
    if alias_present(ip):
        LOG(f"[=] 副 IP {ip} 已存在，跳过添加")
        return True

    targets = []
    if IFACE is not None:
        targets.append((IFACE, False))
    else:
        targets.append((LOOPBACK_IFACE, False))
        idx = detect_iface()
        if idx and idx != LOOPBACK_IFACE:
            targets.append((idx, True))

    for idx, need_skip in targets:
        cmd = f"netsh interface ipv4 add address {idx} {ip} 255.255.255.255"
        if need_skip:
            cmd += " skipassource=true"
        rc, out = run(cmd)
        time.sleep(0.4)
        if alias_present(ip):
            USED_IFACE = idx
            LOG(f"[+] 副 IP {ip} 已挂到接口 {idx}" + (" (skipassource)" if need_skip else ""))
            return True
        LOG(f"[!] 接口 {idx} 添加失败: {out.strip()}")
    return False


_CONSOLE_HANDLER = None


def install_console_handler(ip):
    """窗口被 X 关闭 / Ctrl+C / 注销时也摘掉副 IP，避免残留把游戏连挂。"""
    global _CONSOLE_HANDLER
    if os.name != "nt":
        return
    try:
        import ctypes
        proto = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_uint)

        def _h(evt):
            if evt in (0, 1, 2, 5, 6):
                for idx in {USED_IFACE, LOOPBACK_IFACE} - {None}:
                    try:
                        run(f"netsh interface ipv4 delete address {idx} {ip}")
                    except Exception:
                        pass
            return False

        _CONSOLE_HANDLER = proto(_h)
        ctypes.windll.kernel32.SetConsoleCtrlHandler(_CONSOLE_HANDLER, True)
    except Exception:
        pass


def del_alias(ip):
    removed = False
    for idx in {LOOPBACK_IFACE, IFACE} | {detect_iface()}:
        if idx is None:
            continue
        run(f"netsh interface ipv4 delete address {idx} {ip}")
        if not alias_present(ip):
            removed = True
    # 兜底：按接口名逐个删
    rc, out = run("netsh interface ipv4 show interfaces")
    for line in out.splitlines():
        parts = line.split()
        if parts and parts[0].isdigit():
            run(f"netsh interface ipv4 delete address {parts[0]} {ip}")
    LOG("[+] 已清理副 IP" if not alias_present(ip) else "[!] 仍检测到副 IP，请手动检查")
    return removed


def cmd_status(_):
    LOG(f"目标: {TARGET_IP}  端口: {PORTS}")
    LOG(f"代理: SOCKS5 {SOCKS_HOST}:{SOCKS_PORT}")
    if not alias_present(TARGET_IP):
        LOG(f"[OFF] 副 IP {TARGET_IP} 不在本机 —— 当前是直连")
        return
    LOG(f"[ON ] 副 IP {TARGET_IP} 已挂在本机 —— 游戏流量会被中转")

    # netsh 的输出是按接口分段的：先出现 '接口 "xxx" 的配置'，后面跟着该接口的属性行。
    # 我们只关心「哪一段里出现了目标 IP」，其余接口一概不打印（否则全是噪音），
    # 同时也不必依赖任何本地化字符串的细节匹配。
    rc, out = run("netsh interface ipv4 show addresses")
    blocks, cur = [], None
    for line in out.splitlines():
        s = line.strip()
        if not s:
            continue
        if s.startswith("接口") and s.endswith("的配置"):
            cur = [s, []]
            blocks.append(cur)
            continue
        if cur is not None:
            cur[1].append(s)

    shown = False
    for name, body in blocks:
        if any(TARGET_IP in b for b in body):
            shown = True
            LOG("     " + name)
            for b in body:
                LOG("       " + b)
    if not shown:                      # 地址挂上了但 netsh 没列出来（极少见）
        LOG("     （netsh 未列出该地址，请手动核对）")


def cmd_setup(_):
    if not is_admin():
        LOG("[x] 需要管理员权限。请以管理员身份重开终端后再运行。")
        return 2
    if not add_alias(TARGET_IP):
        LOG("[x] 副 IP 添加失败，退出。")
        return 3
    install_console_handler(TARGET_IP)
    if not socks5_ok():
        LOG(f"[!] 警告：SOCKS5 {SOCKS_HOST}:{SOCKS_PORT} 连不上，先打开 Throne。")
    LOG("[*] 开始中继，Ctrl+C 退出（退出时自动清理副 IP）")
    start_relay(TARGET_IP, PORTS)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        LOG("\n[*] 退出，清理副 IP...")
    finally:
        del_alias(TARGET_IP)
    return 0


def cmd_run(_):
    if not alias_present(TARGET_IP):
        LOG(f"[x] 副 IP {TARGET_IP} 还没挂上，先跑 setup（需管理员）。")
        return 2
    install_console_handler(TARGET_IP)
    start_relay(TARGET_IP, PORTS)
    LOG("[*] 中继运行中，Ctrl+C 退出")
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
    return 0


def cmd_teardown(_):
    if not is_admin():
        LOG("[x] 需要管理员权限。")
        return 2
    del_alias(TARGET_IP)
    return 0


def socks5_ok():
    try:
        s = socks5_connect(TARGET_IP, 1119, timeout=8)
        s.close()
        return True
    except Exception:
        return False


def cmd_selftest(_):
    """无需管理员：把中继绑在 127.0.0.1 上做端到端验证。"""
    port = PORTS[0]                      # 用真实端口(1119)，保证映射到真实服务
    LOG("[*] 本地自测：127.0.0.1:%d -> SOCKS5 -> %s:%d" % (port, TARGET_IP, port))
    start_relay("127.0.0.1", [port])
    time.sleep(0.5)
    try:
        c = socket.create_connection(("127.0.0.1", port), timeout=10)
        LOG("[+] 本地接入成功，链路：本进程 -> 中继 -> SOCKS5 -> 真实服务器")
        c.settimeout(3)
        try:
            data = c.recv(16)
            if data:
                LOG("[+] 服务器首包: %r" % data)
            else:
                LOG("[!] 对端立即关闭（端口可能不对，或代理侧连不上）")
        except socket.timeout:
            LOG("[+] 已建立 TCP，服务器暂未主动发数据（logon 端口正常表现）")
        c.close()
    except Exception as e:
        LOG("[x] 自测失败: %s" % e)
    return 0


# ----------------------------- 入口 -----------------------------

def main():
    global TARGET_IP, PORTS, SOCKS_HOST, SOCKS_PORT, IFACE

    ap = argparse.ArgumentParser(description="Firestorm 自制加速器（副 IP + SOCKS5 中继）")
    ap.add_argument("cmd", choices=["setup", "run", "teardown", "status", "selftest"])
    ap.add_argument("--target", default=TARGET_IP, help="目标服务器 IP")
    ap.add_argument("--ports", default=",".join(map(str, PORTS)), help="逗号分隔端口")
    ap.add_argument("--socks", default=f"{SOCKS_HOST}:{SOCKS_PORT}", help="SOCKS5 地址")
    ap.add_argument("--iface", type=int, default=None, help="挂副 IP 的网卡索引")
    a = ap.parse_args()

    TARGET_IP = a.target
    PORTS = [int(x) for x in a.ports.split(",") if x.strip()]
    SOCKS_HOST, _, sp = a.socks.partition(":")
    SOCKS_PORT = int(sp or SOCKS_PORT)
    IFACE = a.iface

    return {"setup": cmd_setup, "run": cmd_run, "teardown": cmd_teardown,
            "status": cmd_status, "selftest": cmd_selftest}[a.cmd](a) or 0


if __name__ == "__main__":
    sys.exit(main())
