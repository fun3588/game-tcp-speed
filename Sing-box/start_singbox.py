#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
start_singbox.py  ·  以随机端口启动独立 sing-box（支持多实例）
==============================================================
两个实例（profile），可同时运行、互不干扰：

    main    主实例 —— 走 Cloudflare 隧道节点（低延迟、抗封锁）
            状态文件 runtime.json
    backup  备实例 —— 走直连 VPS 节点（hy2 / vless-reality，最稳）
            状态文件 runtime.backup.json

每次启动都从系统里挑两个**空闲的高位端口**（默认 50000-65535），分别给：

    · SOCKS5/HTTP 入口  （原来是固定的 2081）
    · clash_api 控制接口（原来是固定的 9091）

挑端口时会自动避开 Windows 的**预留端口段**（Hyper-V / WSL / Docker 会通过
netsh 占掉一些块，落在这些块里的端口 bind 会直接 access denied）。
高位段挑不到就自动退到 20000-65535。

然后把实际端口写进对应 profile 的 runtime 文件，供加速器和托盘工具读取。

配置文件分工：
    config.json                  带注释的模板（含全部节点），人看的，不直接跑
    config.runtime.json          主实例本次实际生效的配置（端口/节点已替换）
    config.runtime.<profile>.json 其它实例的运行时配置
    runtime.json / runtime.<profile>.json  本次运行状态（端口 / PID / 节点）

用法：
    python start_singbox.py                    主实例，随机端口
    python start_singbox.py --profile backup    备实例，随机端口
    python start_singbox.py --keep-port         沿用上次的端口
    python start_singbox.py --port 2081         指定入口端口（控制接口仍随机）
    python start_singbox.py --node hy2-va1      指定默认节点（覆盖 profile 默认）
    python start_singbox.py --port-min 50000 --port-max 65535   自定义随机范围
    python start_singbox.py --show-excluded     只打印系统预留端口段，然后退出
"""

import argparse
import json
import os
import random
import re
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SB_EXE = os.path.join(HERE, "sing-box.exe")
BASE_CFG = os.path.join(HERE, "config.json")

# 默认随机范围：高位段。数大一些既不容易撞到常用软件的固定端口，
# 也不容易被手动排查时误认成"某个已知服务"。
PORT_MIN, PORT_MAX = 50000, 65535
# 高位段全被占/被预留时的兜底范围（正常用不到）
FALLBACK_MIN, FALLBACK_MAX = 20000, 65535

# 固定占用/易冲突的端口，避开
AVOID = {2080}          # Throne 占着
# 已被别的实例占用的端口，运行时动态追加
_port_blacklist = set()
# Windows 预留端口段缓存
_excl_cache = None

# profile 定义：默认节点 + 中文名
PROFILES = {
    "main":   {"title": "主实例", "node": "qiniu-0001"},
    "backup": {"title": "备实例", "node": "hy2-va1"},
}


# ----------------------------- 路径 -----------------------------

def state_path(profile):
    """该 profile 的状态文件（端口 / PID）。main 沿用历史文件名以兼容旧脚本。"""
    if profile == "main":
        return os.path.join(HERE, "runtime.json")
    return os.path.join(HERE, "runtime." + profile + ".json")


def runtime_cfg_path(profile):
    if profile == "main":
        return os.path.join(HERE, "config.runtime.json")
    return os.path.join(HERE, "config.runtime." + profile + ".json")


def log_path(profile):
    if profile == "main":
        return "singbox.log"
    return "singbox." + profile + ".log"


# ----------------------------- 小工具 -----------------------------

def strip_json_comments(text):
    """去掉 JSON 里的 // 和 /* */ 注释。

    sing-box 自己的解析器容忍注释，但 Python 的 json 不容忍，所以生成运行时
    配置前得先剥掉。必须逐字符扫描 —— 不能简单 replace，否则
    "http://..." 这种字符串里的 // 会被误伤。"""
    out = []
    i, n = 0, len(text)
    in_str = False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def excluded_ranges():
    """Windows 预留的端口段（Hyper-V / WSL / Docker 会占掉整块）。

    落在这些段里的端口，bind 会直接返回 access denied（**不是** in use），
    很容易被误判成"随机不到端口"。只查一次并缓存。
    非 Windows 平台返回空列表。"""
    global _excl_cache
    if _excl_cache is not None:
        return _excl_cache
    _excl_cache = []
    if os.name != "nt":
        return _excl_cache
    seen = set()
    for proto in ("tcp", "udp"):
        try:
            r = subprocess.run(
                ["netsh", "int", "ipv4", "show", "excludedportrange",
                 "protocol=" + proto],
                capture_output=True, timeout=10)
        except Exception:
            continue
        # netsh 的输出是控制台代码页（简中是 936）。只取数字，
        # 用 replace 容错，乱码不影响解析。
        text = r.stdout.decode("cp936", "replace")
        for line in text.splitlines():
            # 数据行长这样（前面有缩进）: "     50000       50059      "
            # 末尾可能跟一个 "*"（表示"管理的端口排除"，是管理员永久预留的，
            # 更要避开），所以第二列后面允许有任意非数字结尾。
            m = re.match(r"\s*(\d{1,5})\s+(\d{1,5})\s*\S*\s*$", line)
            if not m:
                continue
            a, b = int(m.group(1)), int(m.group(2))
            if 1 <= a <= b <= 65535 and (a, b) not in seen:
                seen.add((a, b))
                _excl_cache.append((a, b))
    return _excl_cache


def is_excluded(port):
    for a, b in excluded_ranges():
        if a <= port <= b:
            return True
    return False


def port_free(port):
    """不带 SO_REUSEADDR：能 bind 上才算真空闲（避免撞到已有监听）。"""
    if port in _port_blacklist:
        return False
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def pick_port(used, lo=None, hi=None, tries=600):
    """在 [lo, hi] 里随机挑一个真正能用的端口；挑不到返回 None。"""
    lo = PORT_MIN if lo is None else lo
    hi = PORT_MAX if hi is None else hi
    for _ in range(tries):
        p = random.randint(lo, hi)
        if p in used or p in AVOID or is_excluded(p):
            continue
        if port_free(p):
            used.add(p)
            return p
    return None


def take_port(used, what="端口"):
    """挑一个端口，高位段失败就退到兜底段。"""
    p = pick_port(used)
    if p is None:
        print(f"[!] {PORT_MIN}-{PORT_MAX} 里没挑到空闲{what}，"
              f"退到 {FALLBACK_MIN}-{FALLBACK_MAX}")
        p = pick_port(used, FALLBACK_MIN, FALLBACK_MAX)
    if p is None:
        raise RuntimeError(
            f"{FALLBACK_MIN}-{FALLBACK_MAX} 里也找不到空闲{what}")
    return p


def load_base_config():
    with open(BASE_CFG, "r", encoding="utf-8") as f:
        return json.loads(strip_json_comments(f.read()))


def read_state(profile):
    try:
        with open(state_path(profile), "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def listening(port):
    if not port:
        return False
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=0.5)
        s.close()
        return True
    except Exception:
        return False


def other_ports(profile):
    """其它 profile 已经占掉的端口 —— 挑端口时要避开，否则删了状态文件会撞车。"""
    used = set()
    for p in PROFILES:
        if p == profile:
            continue
        st = read_state(p)
        if not st:
            continue
        for k in ("socks_port", "clash_port"):
            if st.get(k):
                used.add(int(st[k]))
    return used


def all_outbound_tags(cfg):
    return [o["tag"] for o in cfg.get("outbounds", [])
            if o.get("type") in ("vless", "hysteria2", "shadowsocks", "trojan",
                                 "vmess", "tuic", "anytls", "http")]


def profile_outbounds(cfg, profile):
    """每个 profile 实际启用哪些节点 —— 写进 selector 的 outbounds 列表。

    注意是按 tag 过滤（不是遍历 config 的顶层键，早期就栽在这儿：
    list(cfg) 拿到的是 log/inbounds/outbounds/... 这种键名）。"""
    available = [o.get("tag") for o in cfg.get("outbounds", [])]
    if profile == "main":
        # 主实例：走 Cloudflare 隧道（两个节点互为热备），老节点留作兜底
        want = ["qiniu-0001", "qiniu-0709", "hy2-va1", "vl-reality-va1"]
    elif profile == "backup":
        # 备实例：走直连 VPS（最稳），隧道节点也留着
        want = ["hy2-va1", "vl-reality-va1", "qiniu-0001", "qiniu-0709"]
    else:
        want = available
    picked = [t for t in want if t in available]
    return picked or available


# ----------------------------- 主流程 -----------------------------

def main():
    # --port-min / --port-max 要改的是模块级的随机范围，
    # global 必须写在函数最前面（提前引用 PORT_MIN 会直接 SyntaxError）。
    global PORT_MIN, PORT_MAX

    ap = argparse.ArgumentParser(description="随机端口启动独立 sing-box（多实例）")
    ap.add_argument("--profile", default="main",
                    choices=sorted(PROFILES.keys()),
                    help="实例名：main=主（隧道节点）/ backup=备（直连节点）")
    ap.add_argument("--keep-port", action="store_true",
                    help="沿用该实例上次的端口")
    ap.add_argument("--port", type=int, default=None,
                    help="指定 SOCKS5 入口端口（控制接口仍随机）")
    ap.add_argument("--node", default=None,
                    help="指定默认节点 tag（覆盖 profile 默认）")
    ap.add_argument("--port-min", type=int, default=PORT_MIN,
                    help=f"随机端口下限（默认 {PORT_MIN}）")
    ap.add_argument("--port-max", type=int, default=PORT_MAX,
                    help=f"随机端口上限（默认 {PORT_MAX}）")
    ap.add_argument("--show-excluded", action="store_true",
                    help="只打印系统预留端口段，然后退出")
    a = ap.parse_args()

    if a.show_excluded:
        rng = excluded_ranges()
        if not rng:
            print("（没有查到预留端口段，或不是 Windows）")
        for lo, hi in rng:
            print(f"{lo}-{hi}")
        return 0

    if not (1 <= a.port_min < a.port_max <= 65535):
        print(f"[x] 端口范围不合法：{a.port_min}-{a.port_max}")
        return 2
    PORT_MIN, PORT_MAX = a.port_min, a.port_max

    profile = a.profile
    meta = PROFILES[profile]
    STATE = state_path(profile)
    RUNTIME_CFG = runtime_cfg_path(profile)

    old = read_state(profile) or {}
    if old.get("pid") and listening(old.get("socks_port")):
        print(f"[i] {meta['title']}（{profile}）似乎已在运行："
              f"PID {old['pid']}，入口端口 {old['socks_port']}")
        print(f"    如需重启用 stop_singbox.py --profile {profile}")
        return 0

    used = set(AVOID) | other_ports(profile)

    if a.port:
        socks_port = a.port
        if is_excluded(socks_port):
            print(f"[x] 指定端口 {socks_port} 落在系统预留段里，bind 会失败")
            return 2
        if not port_free(socks_port):
            print(f"[x] 指定端口 {socks_port} 已被占用")
            return 2
    elif a.keep_port and old.get("socks_port"):
        socks_port = old["socks_port"]
        if not port_free(socks_port):
            print(f"[!] 上次的端口 {socks_port} 已被占用，改为随机")
            socks_port = take_port(used, "入口端口")
    else:
        socks_port = take_port(used, "入口端口")

    used.add(socks_port)
    clash_port = take_port(used, "控制端口")

    cfg = load_base_config()
    cfg["log"]["output"] = log_path(profile)
    cfg["inbounds"][0]["listen_port"] = socks_port
    cfg["experimental"]["clash_api"]["external_controller"] = \
        f"127.0.0.1:{clash_port}"

    # selector：限定本 profile 的节点范围，并设置默认节点
    tags = profile_outbounds(cfg, profile)
    node = a.node or meta["node"]
    if node not in tags:
        print(f"[!] 节点 {node} 不在本实例范围内，改用 {tags[0]}")
        node = tags[0]
    for o in cfg["outbounds"]:
        if o.get("type") == "selector":
            o["outbounds"] = tags
            o["default"] = node

    with open(RUNTIME_CFG, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)

    print("=" * 62)
    print(f"  独立 sing-box · {meta['title']}（profile={profile}）")
    print("=" * 62)
    print(f"  入口 (SOCKS5+HTTP) : 127.0.0.1:{socks_port}")
    print(f"  控制接口 (clash)   : 127.0.0.1:{clash_port}")
    print(f"  默认节点           : {node}")
    print(f"  可选节点           : {', '.join(tags)}")
    print(f"  运行时配置         : {os.path.basename(RUNTIME_CFG)}")
    print(f"  状态文件           : {os.path.basename(STATE)}")
    print("-" * 62)

    check = subprocess.run([SB_EXE, "check", "-c", RUNTIME_CFG],
                           cwd=HERE, capture_output=True)
    if check.returncode != 0:
        print("[x] 配置校验失败：")
        print("   " + check.stderr.decode("utf-8", "replace").strip())
        return 3

    proc = subprocess.Popen([SB_EXE, "run", "-D", HERE, "-c", RUNTIME_CFG],
                            cwd=HERE)

    ready = False
    crashed = False
    for _ in range(60):
        time.sleep(0.25)
        if listening(socks_port):
            ready = True
            break
        if proc.poll() is not None:      # 进程已退出，别再白等 15 秒
            crashed = True
            break

    if not ready:
        # 失败时不写状态文件 —— 否则别的工具会读到一个"看起来在跑"的假端口
        print(f"[x] {'sing-box 启动即退出' if crashed else '启动后端口未就绪'}，"
              f"请看 {log_path(profile)}")
        try:
            rc = proc.wait(timeout=2)
            print(f"    退出码 = {rc}")
        except Exception:
            proc.kill()
        return 4

    state = {
        "profile": profile,
        "title": meta["title"],
        "node": node,
        "socks_port": socks_port,
        "clash_port": clash_port,
        "pid": proc.pid,
        "started_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "config": os.path.basename(RUNTIME_CFG),
    }
    with open(STATE, "w", encoding="utf-8") as f:
        json.dump(state, f, ensure_ascii=False, indent=2)

    print(f"  [OK] 已启动  PID = {proc.pid}")
    print()
    print("  让加速器指向它：")
    print(f"      python py\\firestorm_accel.py setup --socks 127.0.0.1:{socks_port}")
    print(f"  或双击 py\\switch_upstream.cmd（默认读主实例）")
    print()
    print(f"  停止：Ctrl+C，或另开窗口跑 stop_singbox.py --profile {profile}")

    try:
        proc.wait()
    except KeyboardInterrupt:
        print("\n[*] 收到 Ctrl+C，停止 sing-box...")
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
    finally:
        try:
            if os.path.exists(STATE):
                os.remove(STATE)
        except Exception:
            pass
        print("[*] 已停止")
    return 0


if __name__ == "__main__":
    sys.exit(main())
