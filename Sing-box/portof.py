#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
portof.py  ·  查某个 sing-box 实例的运行时端口 / 节点
======================================================
给 .cmd 脚本用的：因为端口是随机的，脚本里不能再写死。

用法：
    python portof.py socks   [默认值] [profile]   # SOCKS5/HTTP 入口端口
    python portof.py clash   [默认值] [profile]   # clash_api 控制接口端口
    python portof.py node    [默认值] [profile]   # 当前默认节点 tag
    python portof.py pid     [默认值] [profile]   # 进程 PID
    python portof.py list                          # 两个实例一行一个

profile 省略时为 main（主实例）。读不到状态文件时输出默认值（不报错），
这样调用方总能拿到一个可用的值。
"""

import json
import os
import socket
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

KEYS = {
    "socks": "socks_port",
    "port": "socks_port",
    "clash": "clash_port",
    "node": "node",
    "pid": "pid",
    "title": "title",
}
PROFILES = ["main", "backup"]


def state_path(profile):
    if profile == "main":
        return os.path.join(HERE, "runtime.json")
    return os.path.join(HERE, "runtime." + profile + ".json")


def read(profile):
    """只读该 profile 自己的状态文件 —— 不要跨 profile 回退，
    否则备实例没跑时会把主实例的端口当成自己的。"""
    try:
        with open(state_path(profile), "r", encoding="utf-8") as f:
            return json.load(f) or {}
    except Exception:
        return {}


def _alive(port):
    """真的有人在监听吗？状态文件可能是残留（主管进程被强杀时来不及清理）。"""
    if not port:
        return False
    try:
        s = socket.create_connection(("127.0.0.1", int(port)), timeout=0.4)
        s.close()
        return True
    except Exception:
        return False


def main():
    args = sys.argv[1:]
    which = args[0] if args else "socks"

    if which == "list":
        # 带存活判定：残留的状态文件要能一眼看出来，不然会以为实例还在跑
        sys.stdout.write("profile\t入口\t控制\t节点\tPID\t存活\n")
        for prof in PROFILES:
            d = read(prof)
            if d.get("socks_port"):
                alive = _alive(d.get("socks_port"))
                sys.stdout.write("%s\t%s\t%s\t%s\t%s\t%s\n" % (
                    prof, d.get("socks_port", ""), d.get("clash_port", ""),
                    d.get("node", ""), d.get("pid", ""),
                    "是" if alive else "否(残留)"))
        return

    default = args[1] if len(args) > 1 else ""
    profile = args[2] if len(args) > 2 else "main"
    key = KEYS.get(which, which)
    val = read(profile).get(key) or default
    sys.stdout.write(str(val))


if __name__ == "__main__":
    main()
