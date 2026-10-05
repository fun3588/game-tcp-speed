#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
stop_singbox.py  ·  停止独立 sing-box（支持多实例）
====================================================
按 runtime 文件里记录的 PID 精确停止；PID 不在了就按进程名兜底。
最后清理对应 profile 的 runtime 文件（它是"正在运行"的标志）。

用法：
    python stop_singbox.py                  停主实例（main）
    python stop_singbox.py --profile backup 停备实例
    python stop_singbox.py --all            两个实例都停
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CREATE_NO_WINDOW = 0x08000000

PROFILES = {"main": "主实例", "backup": "备实例"}


def state_path(profile):
    if profile == "main":
        return os.path.join(HERE, "runtime.json")
    return os.path.join(HERE, "runtime." + profile + ".json")


def read_state(profile):
    try:
        with open(state_path(profile), "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def port_open(port):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=0.5)
        s.close()
        return True
    except Exception:
        return False


def kill_pid(pid):
    p = subprocess.run(["taskkill", "/F", "/PID", str(pid)],
                       capture_output=True, creationflags=CREATE_NO_WINDOW)
    out = (p.stdout or b"").decode("gbk", "replace").strip()
    err = (p.stderr or b"").decode("gbk", "replace").strip()
    return p.returncode == 0, (out or err)


def stop_one(profile, fallback_by_name=True):
    title = PROFILES.get(profile, profile)
    STATE = state_path(profile)
    st = read_state(profile)

    print("-" * 62)
    print(f"  停止 {title}（profile={profile}）")
    print("-" * 62)

    stopped = False
    if st and st.get("pid"):
        pid = st["pid"]
        print(f"  runtime 记录的 PID = {pid}，节点 = {st.get('node', '?')}")
        ok, msg = kill_pid(pid)
        if ok:
            print(f"  [OK] {msg}")
            stopped = True
        else:
            print(f"  [!] 按 PID 停止失败（可能已退出）：{msg}")
    else:
        print(f"  没有 {os.path.basename(STATE)}（可能不是用 start_singbox.py 启动的）")

    # 只要不是"由本 profile 精确停掉"的，就按进程名兜底（可能杀掉别的实例，
    # 所以只在确实还监听时才做）
    if fallback_by_name and not stopped:
        ports = [st.get(k) for k in ("socks_port", "clash_port")] if st else []
        still = any(port_open(p) for p in ports if p)
        if still:
            print("  端口仍在监听，按进程名兜底 sing-box.exe")
            p = subprocess.run(["taskkill", "/F", "/IM", "sing-box.exe"],
                               capture_output=True, creationflags=CREATE_NO_WINDOW)
            out = (p.stdout or b"").decode("gbk", "replace").strip()
            print(f"  {out or '没有找到 sing-box.exe'}")

    time.sleep(0.8)

    if st:
        for label, key in (("入口", "socks_port"), ("控制接口", "clash_port")):
            port = st.get(key)
            if port:
                print(f"  127.0.0.1:{port} ({label}) "
                      f"{'仍在监听 [异常]' if port_open(port) else '已释放'}")

    try:
        if os.path.exists(STATE):
            os.remove(STATE)
            print(f"  已删除 {os.path.basename(STATE)}")
    except Exception as e:
        print(f"  [!] 删除状态文件失败：{e}")
    return 0


def main():
    ap = argparse.ArgumentParser(description="停止独立 sing-box")
    ap.add_argument("--profile", default="main", choices=sorted(PROFILES.keys()),
                    help="要停的实例（默认 main）")
    ap.add_argument("--all", action="store_true", help="两个实例都停")
    a = ap.parse_args()

    print("=" * 62)
    print("  停止独立 sing-box")
    print("=" * 62)

    if a.all:
        for p in PROFILES:
            stop_one(p)
    else:
        stop_one(a.profile)

    # 全局兜底：确认没有漏网的
    p = subprocess.run(["tasklist", "/IM", "sing-box.exe"],
                       capture_output=True, creationflags=CREATE_NO_WINDOW)
    if b"sing-box" in (p.stdout or b"").lower():
        print()
        print("  [!] 仍有 sing-box.exe 在运行（可能是另一个 profile），"
              "如需全清跑 stop_singbox.py --all")
    return 0


if __name__ == "__main__":
    sys.exit(main())
