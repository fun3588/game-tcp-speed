#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Firestorm 加速器 —— 原生桌面面板（tkinter）

为什么必须换解释器：
  tkinter 是 CPython 的**编译期**模块，没有 pip 包可装 —— 项目平时用的托管 Python
  （~/.workbuddy/binaries/python/...）是精简构建，**没编译它**，装不回来。
  而系统 Python `D:\\py3-12-1\\python.exe` 自带 tkinter 8.6 + ttk。
  所以本文件要挑一个带 tkinter 的解释器跑 —— `run-gui.cmd` 会自动挑，挑不到会提示。

  业务逻辑一份都没重复：状态探测/启停转手给 py/firestorm_tray.py，
  改配置用 config_edit.py，分享链接解析用 share_link.py，三者都是纯标准库。

启动：
  python firestorm_gui.py             # 正常打开窗口
  python firestorm_gui.py --self-test # 只构建界面 + 渲染一轮，不进事件循环（自检用）
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import queue
import sys
import threading
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                       # game-tcp-speed/
SB_DIR = os.path.join(ROOT, "Sing-box")
PY_DIR = os.path.join(ROOT, "py")

# 复用现成模块：config_edit（改配置）、share_link（分享链接）、
# firestorm_tray（状态探测 + 启停动作）。三者都是纯标准库。
for _p in (HERE, PY_DIR, SB_DIR):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import tkinter as tk                              # noqa: E402
from tkinter import messagebox, ttk               # noqa: E402

import config_edit as CE                          # noqa: E402
import firestorm_tray as T                        # noqa: E402
import share_link as SL                           # noqa: E402

APP_TITLE = "Firestorm 加速器 — 配置与运行"

# ----------------------------- 配色（浅色主题） -----------------------------

BG = "#f4f6f8"
PANEL = "#ffffff"
PANEL2 = "#f8fafc"
BORDER = "#e3e7ed"
FG = "#1d2430"
MUTED = "#6b7583"
FAINT = "#98a1ae"
ACCENT = "#2563eb"
ACCENT_BG = "#e9f0fe"
OK = "#0f9d58"
OK_BG = "#e5f6ec"
WARN = "#b26a00"
WARN_BG = "#fdf3e2"
ERR = "#c8382f"
ERR_BG = "#fdeceb"

FONT = ("Microsoft YaHei UI", 9)
FONT_B = ("Microsoft YaHei UI", 9, "bold")
FONT_H = ("Microsoft YaHei UI", 11, "bold")
FONT_S = ("Microsoft YaHei UI", 8)
MONO = ("Consolas", 9)
MONO_S = ("Consolas", 8)

# ----------------------------- 常量 -----------------------------

PROFILE_LOG = {
    "main": os.path.join(SB_DIR, "singbox.log"),
    "backup": os.path.join(SB_DIR, "singbox.backup.log"),
}
ACCEL_LOG = os.path.join(PY_DIR, "firestorm_accel.log")

NODE_URL = "http://www.gstatic.com/generate_204"
TIMEOUT_MS = 8000

REFRESH_MS = 2500          # 状态轮询间隔
LOG_REFRESH_MS = 4000      # 日志刷新间隔


# ============================= 小工具 =============================

def clash_get(port, path, timeout=4):
    """读 sing-box 的 clash API。失败返回 {'_error': ...}，不抛。"""
    url = f"http://127.0.0.1:{port}{path}"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace") or "{}")
    except Exception as e:
        return {"_error": str(e)}


def clash_put(port, path, payload, timeout=4):
    url = f"http://127.0.0.1:{port}{path}"
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, method="PUT",
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return {"ok": True, "body": r.read().decode("utf-8", "replace")}
    except urllib.error.HTTPError as e:
        return {"ok": False,
                "error": e.read().decode("utf-8", "replace") or str(e)}
    except Exception as e:
        return {"ok": False, "error": str(e)}


def clash_delay(clash_port, tag, timeout_ms=TIMEOUT_MS):
    """测某个节点的延迟 —— **不切换**到它，这样不会打断正在玩的游戏。

    clash API 的 /proxies/<tag>/delay 会真的发一次探测请求再返回毫秒数；
    节点不通时返回 4xx + {"message": "An error occurred in the delay test"}。
    """
    q = urllib.parse.quote(tag, safe="")
    url = (f"http://127.0.0.1:{clash_port}/proxies/{q}/delay"
           f"?timeout={timeout_ms}&url={NODE_URL}")
    try:
        with urllib.request.urlopen(url, timeout=timeout_ms / 1000.0 + 2) as r:
            d = json.loads(r.read().decode("utf-8", "replace") or "{}")
        if "delay" in d:
            return int(d["delay"]), None
        return None, d.get("message") or "测速失败"
    except urllib.error.HTTPError as e:
        try:
            d = json.loads(e.read().decode("utf-8", "replace") or "{}")
        except Exception:
            d = {}
        return None, d.get("message") or f"HTTP {e.code}"
    except Exception as e:
        return None, str(e)


def read_tail(path, lines=300):
    if not os.path.exists(path):
        return f"（日志文件不存在：{os.path.basename(path)}）"
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            data = f.readlines()
        return "".join(data[-lines:])
    except Exception as e:
        return f"（读日志失败：{e}）"


def is_admin():
    return T.is_admin()


def elevate_self():
    """以管理员重启本面板（会弹 UAC）。优先用 pythonw，免得多一个黑框。"""
    exe = sys.executable
    cand = os.path.join(os.path.dirname(exe), "pythonw.exe")
    if os.path.exists(cand):
        exe = cand
    script = os.path.abspath(__file__)
    try:
        rc = ctypes.windll.shell32.ShellExecuteW(
            None, "runas", exe, f'"{script}"', os.path.dirname(script), 1)
        return int(rc) > 32
    except Exception:
        return False


def dot_icon(size=32, rgb="#0f9d58"):
    """画一个纯色圆点当窗口图标（tkinter 没有内置 ico）。"""
    img = tk.PhotoImage(width=size, height=size)
    cx = (size - 1) / 2.0
    r = size / 2.0 - 0.5
    for y in range(size):
        for x in range(size):
            if (x - cx) ** 2 + (y - cx) ** 2 <= r * r:
                img.put(rgb, (x, y))
    return img


def set_enabled(widget, on):
    try:
        widget.state(["!disabled"] if on else ["disabled"])
    except Exception:
        try:
            widget.configure(state="normal" if on else "disabled")
        except Exception:
            pass


# ============================= 主界面 =============================

class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.state = {}
        self.busy_actions = 0          # 有写操作在跑时暂停轮询，避免打架
        self.latency = {}              # tag -> ms / -1 表示不通
        self._refreshing = False
        self._closed = False
        # tkinter 不是线程安全的：工作线程**绝对不能**碰 widget，也不能调
        # root.after()（after 内部会 createcommand，非主线程调用会抛
        # "main thread is not in main loop"）。所以后台线程只往这个队列里塞
        # 结果，主线程用 _drain() 定时排空并落到界面上。
        self.q = queue.Queue()

        root.title(APP_TITLE)
        root.geometry("1000x860")
        root.minsize(880, 700)
        root.configure(bg=BG)
        try:
            root.iconphoto(True, dot_icon())
        except Exception:
            pass

        self._init_style()
        self._build()
        self._bind_events()

        root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.refresh()
        self.tick_log()
        self._drain()

    # ---------- 主线程侧的结果队列 ----------

    def _drain(self):
        """把后台线程塞进来的结果落到界面上。**只在主线程跑。**"""
        if self._closed:
            return
        while True:
            try:
                item = self.q.get_nowait()
            except queue.Empty:
                break
            kind = item[0]
            try:
                if kind == "state":
                    self._apply_state(item[1])
                elif kind == "log":
                    self._set_log(item[1], item[2])
                elif kind == "lat":
                    self._set_latency(item[1], item[2], item[3])
                elif kind == "done":
                    self._finish(item[1], item[2])
                elif kind == "toast":
                    self.toast(item[1])
            except Exception:
                traceback.print_exc()
        self.root.after(80, self._drain)

    # ---------- 样式 ----------

    def _init_style(self):
        s = ttk.Style()
        try:
            s.theme_use("vista")
        except Exception:
            pass
        s.configure(".", font=FONT, background=BG, foreground=FG)
        s.configure("TFrame", background=BG)
        s.configure("Panel.TFrame", background=PANEL)
        s.configure("TLabel", background=BG, foreground=FG, font=FONT)
        s.configure("Panel.TLabel", background=PANEL, foreground=FG)
        s.configure("Muted.TLabel", background=BG, foreground=MUTED, font=FONT_S)
        s.configure("PanelMuted.TLabel", background=PANEL, foreground=MUTED,
                    font=FONT_S)
        s.configure("H.TLabel", background=BG, foreground=FG, font=FONT_H)
        s.configure("CardH.TLabel", background=PANEL, foreground=FG, font=FONT_B)
        s.configure("KV.TLabel", background=PANEL, foreground=MUTED, font=FONT_S)
        s.configure("KVV.TLabel", background=PANEL, foreground=FG, font=MONO)
        s.configure("TLabelframe", background=BG, bordercolor=BORDER)
        s.configure("TLabelframe.Label", background=BG, foreground=MUTED,
                    font=FONT_S)
        s.configure("TButton", padding=(9, 4))
        s.configure("Mini.TButton", padding=(8, 3), font=FONT_S)
        s.configure("Toolbutton", padding=(10, 4), font=FONT_S)
        s.configure("TEntry", fieldbackground=PANEL)
        s.configure("TCombobox", padding=(4, 2))
        s.configure("Treeview", background=PANEL, fieldbackground=PANEL,
                    rowheight=26, borderwidth=1, font=FONT)
        s.configure("Treeview.Heading", font=FONT_S, padding=(6, 4))
        s.map("Treeview", background=[("selected", ACCENT_BG)],
              foreground=[("selected", FG)])
        s.configure("TNotebook", background=BG, borderwidth=0)
        s.configure("TNotebook.Tab", padding=(12, 6), font=FONT_S)

    # ---------- 骨架 ----------

    def _build(self):
        outer = ttk.Frame(self.root, padding=(12, 10, 12, 8))
        outer.pack(fill="both", expand=True)
        outer.columnconfigure(0, weight=1)
        outer.rowconfigure(3, weight=1)
        outer.rowconfigure(5, weight=1)

        self._build_header(outer)
        self._build_status_chips(outer)
        self._build_instances(outer)
        self._build_accel(outer)
        self._build_nodes(outer)
        self._build_logs(outer)
        self._build_statusbar()

    # ---------- 顶部标题 ----------

    def _build_header(self, parent):
        bar = ttk.Frame(parent)
        bar.grid(row=0, column=0, sticky="ew")
        bar.columnconfigure(2, weight=1)

        left = ttk.Frame(bar)
        left.grid(row=0, column=0, sticky="w")
        tk.Label(left, text="\u25cf", fg=OK, bg=BG, font=("Segoe UI", 13)
                 ).pack(side="left")
        tk.Label(left, text=" Firestorm 加速器", bg=BG, fg=FG, font=FONT_H
                 ).pack(side="left")
        tk.Label(left, text="  配置与运行", bg=BG, fg=FAINT, font=FONT_S
                 ).pack(side="left")

        self.admin_chip = tk.Label(bar, text="", bg=WARN_BG, fg=WARN,
                                   font=FONT_S, padx=8, pady=2)
        self.admin_chip.grid(row=0, column=3, sticky="e", padx=(0, 8))

        btns = ttk.Frame(bar)
        btns.grid(row=0, column=4, sticky="e")
        ttk.Button(btns, text="打开目录", style="Mini.TButton",
                   command=lambda: T.open_path(ROOT)).pack(side="left")
        ttk.Button(btns, text="关闭面板", style="Mini.TButton",
                   command=self.on_close).pack(side="left", padx=(6, 0))

    # ---------- 状态胶囊 ----------

    def _build_status_chips(self, parent):
        wrap = tk.Frame(parent, bg=PANEL, highlightthickness=1,
                        highlightbackground=BORDER)
        wrap.grid(row=1, column=0, sticky="ew", pady=(8, 8))
        inner = tk.Frame(wrap, bg=PANEL)
        inner.pack(fill="x", padx=10, pady=7)

        self.chips = {}
        for key, label in (("alias", "副 IP"), ("relay", "中继"),
                           ("game", "游戏"), ("accel", "加速器")):
            f = tk.Frame(inner, bg=PANEL)
            f.pack(side="left", padx=(0, 18))
            dot = tk.Label(f, text="\u25cf", bg=PANEL, fg=FAINT,
                           font=("Segoe UI", 10))
            dot.pack(side="left")
            tk.Label(f, text=f" {label}", bg=PANEL, fg=MUTED,
                     font=FONT_S).pack(side="left")
            val = tk.Label(f, text="—", bg=PANEL, fg=FG, font=FONT_B)
            val.pack(side="left", padx=(4, 0))
            self.chips[key] = (dot, val)

    # ---------- 两个实例卡片 ----------

    def _build_instances(self, parent):
        box = tk.Frame(parent, bg=BG)
        box.grid(row=2, column=0, sticky="ew")
        box.columnconfigure(0, weight=1, uniform="inst")
        box.columnconfigure(1, weight=1, uniform="inst")

        self.inst = {}
        for col, (prof, title) in enumerate(T.SB_PROFILES):
            card = tk.Frame(box, bg=PANEL, highlightthickness=1,
                            highlightbackground=BORDER)
            card.grid(row=0, column=col, sticky="nsew",
                      padx=(0, 6) if col == 0 else (6, 0))
            card.columnconfigure(0, weight=1)

            head = tk.Frame(card, bg=PANEL)
            head.grid(row=0, column=0, sticky="ew", padx=11, pady=(10, 0))
            h = tk.Label(head, text=title, bg=PANEL, fg=FG, font=FONT_B)
            h.pack(side="left")
            st = tk.Label(head, text="已停止", bg="#eef0f3", fg=FAINT,
                          font=FONT_S, padx=7, pady=1)
            st.pack(side="left", padx=(8, 0))

            role = tk.Label(card, text="", bg=PANEL, fg=FAINT, font=FONT_S,
                            anchor="w", justify="left")
            role.grid(row=1, column=0, sticky="ew", padx=11, pady=(2, 8))

            kv = tk.Frame(card, bg=PANEL)
            kv.grid(row=2, column=0, sticky="ew", padx=11)
            kv.columnconfigure(1, weight=1)
            cells = {}
            for i, key in enumerate(("入口", "控制", "PID", "节点")):
                tk.Label(kv, text=key, bg=PANEL, fg=MUTED, font=FONT_S,
                         anchor="w", width=4).grid(row=i, column=0, sticky="w",
                                                   pady=1)
                v = tk.Label(kv, text="—", bg=PANEL, fg=FG, font=MONO,
                             anchor="w")
                v.grid(row=i, column=1, sticky="w", padx=(8, 0), pady=1)
                cells[key] = v

            stale = tk.Label(card, text="", bg=PANEL, fg=FAINT, font=FONT_S,
                             anchor="w")
            stale.grid(row=3, column=0, sticky="ew", padx=11, pady=(3, 0))

            row = tk.Frame(card, bg=PANEL)
            row.grid(row=4, column=0, sticky="ew", padx=11, pady=(8, 11))
            var = tk.StringVar()
            combo = ttk.Combobox(row, textvariable=var, state="readonly",
                                 width=16, font=MONO)
            combo.pack(side="left")
            b_start = ttk.Button(row, text="启动", style="Mini.TButton",
                                 command=lambda p=prof: self.act_instance(p, "start"))
            b_start.pack(side="left", padx=(6, 0))
            b_stop = ttk.Button(row, text="停止", style="Mini.TButton",
                                command=lambda p=prof: self.act_instance(p, "stop"))
            b_stop.pack(side="left", padx=(4, 0))
            b_test = ttk.Button(row, text="测速", style="Mini.TButton",
                                command=lambda p=prof: self.act_instance(p, "test"))
            b_test.pack(side="left", padx=(4, 0))

            self.inst[prof] = {
                "card": card, "state": st, "role": role, "cells": cells,
                "stale": stale, "var": var, "combo": combo,
                "start": b_start, "stop": b_stop, "test": b_test,
                "title": title,
            }

    # ---------- 加速器条 ----------

    def _build_accel(self, parent):
        wrap = tk.Frame(parent, bg=PANEL, highlightthickness=1,
                        highlightbackground=BORDER)
        wrap.grid(row=4, column=0, sticky="ew", pady=(10, 0))
        wrap.columnconfigure(0, weight=1)

        head = tk.Frame(wrap, bg=PANEL)
        head.grid(row=0, column=0, sticky="ew", padx=11, pady=(9, 0))
        tk.Label(head, text="加速器", bg=PANEL, fg=FG, font=FONT_B
                 ).pack(side="left")
        self.accel_hint = tk.Label(head, text="加速器未运行", bg=PANEL,
                                   fg=MUTED, font=FONT_S)
        self.accel_hint.pack(side="left", padx=(10, 0))

        row = tk.Frame(wrap, bg=PANEL)
        row.grid(row=1, column=0, sticky="ew", padx=11, pady=(8, 10))

        self.accel_start = ttk.Button(row, text="启动加速器",
                                      command=self.act_accel_start)
        self.accel_start.pack(side="left")
        self.accel_stop = ttk.Button(row, text="停止加速器",
                                     command=self.act_accel_stop)
        self.accel_stop.pack(side="left", padx=(6, 0))

        tk.Label(row, text="上游", bg=PANEL, fg=MUTED, font=FONT_S
                 ).pack(side="left", padx=(18, 6))

        self.up_var = tk.IntVar(value=T.PORT_THRONE)
        self.up_btns = {}
        for port, label in ((T.PORT_THRONE, "Throne 2080"),
                            ("main", "主实例"), ("backup", "备实例")):
            b = ttk.Radiobutton(row, text=label, value=port,
                                variable=self.up_var, style="Toolbutton")
            b.pack(side="left", padx=(0, 4))
            self.up_btns[port] = b

        ttk.Button(row, text="切换", style="Mini.TButton",
                   command=self.act_switch).pack(side="left", padx=(10, 0))

        tk.Label(wrap, text="切换会短暂断连（停旧中继 → 起新中继），游戏一般会自动重连。",
                 bg=PANEL, fg=FAINT, font=FONT_S
                 ).grid(row=2, column=0, sticky="w", padx=11, pady=(0, 9))

    # ---------- 节点配置 ----------

    def _build_nodes(self, parent):
        wrap = tk.Frame(parent, bg=PANEL, highlightthickness=1,
                        highlightbackground=BORDER)
        wrap.grid(row=3, column=0, sticky="nsew", pady=(10, 0))
        wrap.columnconfigure(0, weight=1)
        wrap.rowconfigure(1, weight=1)

        head = tk.Frame(wrap, bg=PANEL)
        head.grid(row=0, column=0, sticky="ew", padx=11, pady=(9, 6))
        tk.Label(head, text="节点配置", bg=PANEL, fg=FG, font=FONT_B
                 ).pack(side="left")
        tk.Label(head, text="  改的是 Sing-box\\config.json，手写注释会保留",
                 bg=PANEL, fg=FAINT, font=FONT_S).pack(side="left")

        btns = tk.Frame(head, bg=PANEL)
        btns.pack(side="right")
        ttk.Button(btns, text="全部测速", style="Mini.TButton",
                   command=self.act_test_all).pack(side="left", padx=(0, 6))
        ttk.Button(btns, text="+ 添加节点", style="Mini.TButton",
                   command=lambda: self.node_dialog("add")).pack(side="left")

        cols = ("tag", "type", "server", "transport", "tls", "up", "lat")
        heads = ("tag", "类型", "服务器", "传输", "加密", "上架", "延迟")
        widths = (150, 74, 250, 60, 62, 48, 76)
        tv = ttk.Treeview(wrap, columns=cols, show="headings", height=6)
        for c, h, w in zip(cols, heads, widths):
            tv.heading(c, text=h)
            tv.column(c, width=w, anchor="w" if c in
                      ("tag", "server", "type") else "center",
                      stretch=(c == "server"))
        tv.grid(row=1, column=0, sticky="nsew", padx=(11, 0))
        tv.tag_configure("off", foreground=FAINT)

        sb = ttk.Scrollbar(wrap, orient="vertical", command=tv.yview)
        sb.grid(row=1, column=1, sticky="ns", padx=(0, 4))
        tv.configure(yscrollcommand=sb.set)
        self.tree = tv

        ops = tk.Frame(wrap, bg=PANEL)
        ops.grid(row=2, column=0, sticky="ew", padx=11, pady=(6, 10))
        ttk.Button(ops, text="复制链接", style="Mini.TButton",
                   command=self.act_copy_link).pack(side="left")
        ttk.Button(ops, text="编辑", style="Mini.TButton",
                   command=lambda: self.act_edit_node()).pack(side="left",
                                                              padx=(6, 0))
        ttk.Button(ops, text="删除", style="Mini.TButton",
                   command=self.act_delete_node).pack(side="left", padx=(6, 0))
        tk.Label(ops, text="  双击一行可编辑", bg=PANEL, fg=FAINT, font=FONT_S
                 ).pack(side="left", padx=(8, 0))

    # ---------- 日志 ----------

    def _build_logs(self, parent):
        wrap = tk.Frame(parent, bg=PANEL, highlightthickness=1,
                        highlightbackground=BORDER)
        wrap.grid(row=5, column=0, sticky="nsew", pady=(10, 0))
        wrap.columnconfigure(0, weight=1)
        wrap.rowconfigure(0, weight=1)

        nb = ttk.Notebook(wrap)
        nb.grid(row=0, column=0, sticky="nsew", padx=6, pady=6)
        self.logs = {}
        self.log_tabs = []
        for key, label in (("main", "主实例"), ("backup", "备实例"),
                           ("accel", "加速器")):
            fr = tk.Frame(nb, bg=PANEL)
            fr.columnconfigure(0, weight=1)
            fr.rowconfigure(0, weight=1)
            txt = tk.Text(fr, bg="#1b1f27", fg="#d7dce4", font=MONO_S,
                          wrap="none", relief="flat", padx=8, pady=6,
                          insertbackground="#d7dce4")
            txt.grid(row=0, column=0, sticky="nsew")
            vs = ttk.Scrollbar(fr, orient="vertical", command=txt.yview)
            vs.grid(row=0, column=1, sticky="ns")
            hs = ttk.Scrollbar(fr, orient="horizontal", command=txt.xview)
            hs.grid(row=1, column=0, sticky="ew")
            txt.configure(yscrollcommand=vs.set, xscrollcommand=hs.set,
                          state="disabled")
            nb.add(fr, text=label)
            self.logs[key] = txt
            self.log_tabs.append(key)
        self.notebook = nb

        bar = tk.Frame(wrap, bg=PANEL)
        bar.grid(row=1, column=0, sticky="ew", padx=6, pady=(0, 6))
        ttk.Button(bar, text="刷新日志", style="Mini.TButton",
                   command=self.tick_log).pack(side="right")

    def _build_statusbar(self):
        self.toast_var = tk.StringVar(value="就绪")
        bar = tk.Frame(self.root, bg="#eceff3", height=24)
        bar.pack(fill="x", side="bottom")
        bar.pack_propagate(False)
        tk.Label(bar, textvariable=self.toast_var, bg="#eceff3", fg=MUTED,
                 font=FONT_S, anchor="w").pack(side="left", padx=12)
        self.who_var = tk.StringVar(value="")
        tk.Label(bar, textvariable=self.who_var, bg="#eceff3", fg=FAINT,
                 font=FONT_S, anchor="e").pack(side="right", padx=12)

    # ---------- 事件绑定 ----------

    def _bind_events(self):
        for prof, w in self.inst.items():
            w["combo"].bind("<<ComboboxSelected>>",
                            lambda e, p=prof: self.on_node_pick(p))
        self.tree.bind("<Double-1>", lambda e: self.act_edit_node())

    # ============================= 状态刷新 =============================

    def refresh(self):
        """后台跑 snapshot（要开 netstat / 枚举进程，别卡住 UI 主线程）。"""
        if self._closed:
            return
        if self.busy_actions == 0 and not self._refreshing:
            self._refreshing = True
            threading.Thread(target=self._snapshot_worker, daemon=True).start()
        self.root.after(REFRESH_MS, self.refresh)

    def _snapshot_worker(self):
        try:
            st = self._collect_state()
        except Exception:
            st = {"_error": traceback.format_exc()}
        self.q.put(("state", st))

    def _collect_state(self):
        """和 Web 版 build_state() 同一套语义：
        已停止的实例，端口一律给 None，残留值放进 stale 当"上次是 xxx"提示。"""
        st = T.snapshot(full=True)
        profs = {}
        for prof, title in T.SB_PROFILES:
            port = T.sb_socks_port(prof)
            clash = T.sb_clash_port(prof)
            running = bool(port and T.port_listening(port))
            st_node = T.sb_node(prof)
            entry = {
                "profile": prof, "title": title, "running": running,
                "socks": port if running else None,
                "clash": clash if running else None,
                "stale": None if running else (
                    {"socks": port, "clash": clash} if port else None),
                "node": st_node,
                "pid": (T.sb_pid(prof) if running else None),
                "live_node": None, "selectable": [],
            }
            if running and clash:
                info = clash_get(clash, "/proxies/node", timeout=1.5)
                if not info.get("_error"):
                    entry["live_node"] = info.get("now")
                    entry["selectable"] = info.get("all") or []
            profs[prof] = entry
        st["instances"] = profs
        st["accel_upstream_name"] = T.upstream_name(st.get("upstream"))
        st["throne_up"] = T.port_listening(T.PORT_THRONE)
        st["admin"] = is_admin()
        try:
            text = CE.load_raw()
            rows = CE.summary(text)
            sel = CE.selector_span(text)
            sel_tags = list(((sel[2] or {}).get("outbounds") or [])) if sel else []
            for r in rows:
                r["in_selector"] = r["tag"] in sel_tags
                r["desc"] = SL.describe(r["raw"])
            st["nodes"] = {"nodes": rows, "selector": sel_tags,
                           "default": (sel[2] or {}).get("default") if sel else None}
        except Exception as e:
            st["nodes"] = {"nodes": [], "selector": [], "default": None,
                           "error": str(e)}
        return st

    def _apply_state(self, st):
        self._refreshing = False
        if self._closed:
            return
        self.state = st
        if st.get("_error"):
            self.toast("状态读取失败：" + st["_error"].strip().splitlines()[-1])
            return
        self.render_chips(st)
        self.render_instances(st)
        self.render_accel(st)
        self.render_nodes(st)
        self.who_var.set(f"面板 PID {os.getpid()}"
                         + ("  · 管理员" if st.get("admin") else "  · 普通权限"))

    # ---------- 渲染：胶囊 ----------

    def render_chips(self, st):
        spec = {
            "alias": ("已挂载" if st.get("alias") else "未挂载",
                      bool(st.get("alias"))),
            "relay": ("监听中" if st.get("relay") else "未监听",
                      bool(st.get("relay"))),
        }
        game = st.get("game")
        spec["game"] = (("在运行" if game else "未运行") if game is not None
                        else "—", bool(game))
        if st.get("accel"):
            spec["accel"] = (f"{st.get('accel_upstream_name')} "
                             f"({st.get('upstream')})", True)
        else:
            spec["accel"] = ("已停止", False)

        for key, (text, on) in spec.items():
            dot, val = self.chips[key]
            dot.configure(fg=OK if on else FAINT)
            val.configure(text=text, fg=FG if on else MUTED)

        if st.get("admin"):
            self.admin_chip.configure(text="管理员", bg=OK_BG, fg=OK)
        else:
            self.admin_chip.configure(text="未提权 · 加速器启停不可用",
                                      bg=WARN_BG, fg=WARN)

    # ---------- 渲染：实例卡片 ----------

    def render_instances(self, st):
        for prof, w in self.inst.items():
            it = st["instances"].get(prof)
            if not it:
                continue
            running = it["running"]

            w["state"].configure(
                text="运行中" if running else "已停止",
                bg=OK_BG if running else "#eef0f3",
                fg=OK if running else FAINT)

            if running:
                w["role"].configure(
                    text=f"入口 127.0.0.1:{it['socks']}   控制 127.0.0.1:{it['clash']}")
            else:
                w["role"].configure(text=ROLE_TEXT.get(prof, ""))

            w["cells"]["入口"].configure(
                text=f"127.0.0.1:{it['socks']}" if it["socks"] else "—")
            w["cells"]["控制"].configure(
                text=f"127.0.0.1:{it['clash']}" if it["clash"] else "—")
            w["cells"]["PID"].configure(text=str(it["pid"]) if it["pid"] else "—")

            live = it.get("live_node")
            cur = live or it.get("node") or ""
            w["cells"]["节点"].configure(text=cur or "—")

            if not running and it.get("stale"):
                sk = it["stale"]
                w["stale"].configure(
                    text=f"上次端口 {sk['socks']} / 控制 {sk['clash']}"
                         f"（已停止，重启会换新端口）")
            else:
                w["stale"].configure(text="")

            opts = it.get("selectable") or ([cur] if cur else [])
            w["combo"].configure(values=opts)
            if w["var"].get() != cur:
                w["var"].set(cur)
            set_enabled(w["combo"], running and bool(opts))
            set_enabled(w["start"], not running)
            set_enabled(w["stop"], running)
            set_enabled(w["test"], running)

    # ---------- 渲染：加速器 ----------

    def render_accel(self, st):
        accel_on = bool(st.get("accel"))
        now = st.get("upstream")
        self.accel_hint.configure(
            text=(f"当前上游：{st.get('accel_upstream_name')} ({now})"
                  if accel_on else "加速器未运行"))

        set_enabled(self.accel_start, not accel_on)
        set_enabled(self.accel_stop, accel_on)

        # 把"当前上游"标在对应的单选按钮上（✓ 前缀），选项本身仍是用户的目标选择，
        # 所以不能直接改 var（否则用户刚点完就被刷新覆盖）。
        for port, btn in self.up_btns.items():
            label = {T.PORT_THRONE: "Throne 2080", "main": "主实例",
                     "backup": "备实例"}[port]
            on = False
            if accel_on and now:
                if port == T.PORT_THRONE:
                    on = (now == T.PORT_THRONE)
                else:
                    p = st["instances"].get(port) or {}
                    on = bool(p.get("socks")) and now == p.get("socks")
            btn.configure(text=("\u2713 " if on else "") + label)

    # ---------- 渲染：节点表 ----------

    def render_nodes(self, st):
        data = st.get("nodes") or {}
        rows = data.get("nodes") or []
        self.tree.delete(*self.tree.get_children())
        for r in rows:
            lat = self.latency.get(r["tag"])
            if lat is None:
                latxt = "未测"
                tags = ()
            elif lat < 0:
                latxt = "不通"
                tags = ("off",)
            else:
                latxt = f"{lat} ms"
                tags = ()
            self.tree.insert(
                "", "end", iid=r["tag"], tags=tags,
                values=(r["tag"], r["type"],
                        f"{r['server']}:{r['port']}" if r["server"] else "—",
                        r["transport"] or "—", r["tls"] or "—",
                        "\u2713" if r.get("in_selector") else "—", latxt))

    # ---------- 渲染：日志 ----------

    def tick_log(self):
        if self._closed:
            return
        try:
            idx = self.notebook.index(self.notebook.select())
            key = self.log_tabs[idx]
        except Exception:
            key = "main"
        threading.Thread(target=self._log_worker, args=(key,),
                         daemon=True).start()
        self.root.after(LOG_REFRESH_MS, self.tick_log)

    def _log_worker(self, key):
        path = ACCEL_LOG if key == "accel" else PROFILE_LOG.get(key)
        text = read_tail(path or "", 300)
        self.q.put(("log", key, text))

    def _set_log(self, key, text):
        w = self.logs.get(key)
        if not w:
            return
        yview = w.yview()          # 保住滚动位置：用户往上翻时别被拽回底部
        at_bottom = yview[1] >= 0.999
        w.configure(state="normal")
        w.delete("1.0", "end")
        w.insert("1.0", text)
        w.configure(state="disabled")
        if at_bottom:
            w.see("end")

    # ============================= 动作 =============================

    def busy(self, fn, done=None, note=None):
        """在后台线程跑 fn()，跑完回主线程调 done(text)。
        期间暂停轮询，免得状态刷新和写操作互相打架。"""
        self.busy_actions += 1
        if note:
            self.toast(note)

        def work():
            try:
                r = fn()
            except CE.ConfigError as e:
                r = f"配置出错：{e}"
            except Exception as e:
                r = f"出错：{type(e).__name__}: {e}"
            self.q.put(("done", r, done))

        threading.Thread(target=work, daemon=True).start()

    def _finish(self, r, done):
        self.busy_actions = max(0, self.busy_actions - 1)
        self.toast(str(r))
        if done:
            done(r)
        if self.busy_actions == 0:
            self.root.after(60, self._refresh_now)

    def _refresh_now(self):
        if self._closed:
            return
        if not self._refreshing:
            self._refreshing = True
            threading.Thread(target=self._snapshot_worker, daemon=True).start()

    def toast(self, msg):
        self.toast_var.set(msg)

    def _need_admin(self, what):
        if is_admin():
            return False
        ok = messagebox.askyesno(
            "需要管理员权限",
            f"{what}需要管理员权限（要改 IP 别名 / 路由）。\n\n"
            f"现在以管理员身份重新打开面板吗？\n"
            f"（会弹 UAC；当前窗口会留在原地，可以手动关掉）",
            parent=self.root)
        if ok:
            if elevate_self():
                self.toast("已请求提权，请在弹出的新窗口里操作")
            else:
                self.toast("提权失败（可能被拒绝）")
        return True

    # ---------- 实例动作 ----------

    def act_instance(self, prof, action):
        title = self.inst[prof]["title"]

        if action == "start":
            self.busy(lambda: T.start_singbox(prof),
                      note=f"{title} 启动中（挑端口 + 校验配置，最多 15 秒）…")
        elif action == "stop":
            self.busy(lambda: T.stop_singbox(prof), note=f"{title} 停止中…")
        elif action == "test":
            self.busy(lambda: self._test_instance(prof), note=f"{title} 测速中…")

    def _test_instance(self, prof):
        """测该实例 selector 里所有节点 —— 不切换，不会打断游戏。"""
        st = self._collect_state()["instances"][prof]
        if not st["running"]:
            return f"{st['title']}未在运行"
        clash, tags = st["clash"], (st.get("selectable") or [])
        if not tags:
            return "拿不到节点列表（clash API 没响应？）"
        ok = 0
        for tag in tags:
            ms, err = clash_delay(clash, tag)
            self.q.put(("lat", tag, ms, err))
            ok += 1 if ms else 0
        return f"{st['title']} 测速完成：{ok}/{len(tags)} 个节点可用"

    def _set_latency(self, tag, ms, err):
        self.latency[tag] = ms if ms else -1
        if self.tree.exists(tag):
            self.tree.set(tag, "lat", f"{ms} ms" if ms else "不通")
            self.tree.item(tag, tags=() if ms else ("off",))
        if err and not ms:
            self.toast(f"{tag} 不通：{err}")

    def act_test_all(self):
        self.busy(self._test_all, note="全部测速中…")

    def _test_all(self):
        st = self._collect_state()
        clash = None
        for prof, _ in T.SB_PROFILES:
            e = st["instances"][prof]
            if e["running"] and e["clash"]:
                clash = e["clash"]
                break
        if not clash:
            return "没有运行中的实例，先启动一个再测速"
        rows = (st.get("nodes") or {}).get("nodes") or []
        if not rows:
            return "config.json 里没有节点"
        ok = 0
        for r in rows:
            tag = r["tag"]
            ms, err = clash_delay(clash, tag)
            self.q.put(("lat", tag, ms, err))
            ok += 1 if ms else 0
        return f"测速完成：{ok}/{len(rows)} 个节点可用"

    def on_node_pick(self, prof):
        it = (self.state.get("instances") or {}).get(prof)
        if not it or not it.get("running"):
            return
        tag = self.inst[prof]["var"].get()
        if not tag or tag == it.get("live_node"):
            return
        self.busy(lambda: self._switch_node(prof, tag),
                  note=f"{it['title']} 切到 {tag} …")

    def _switch_node(self, prof, tag):
        it = self._collect_state()["instances"][prof]
        if not it["running"]:
            return f"{it['title']}未在运行"
        r = clash_put(it["clash"], "/proxies/node", {"name": tag})
        if not r["ok"]:
            return f"切换失败：{r['error']}"
        time.sleep(0.2)
        now = clash_get(it["clash"], "/proxies/node").get("now")
        return f"{it['title']} 已切到 {now}"

    # ---------- 加速器动作 ----------

    def act_accel_start(self):
        if self._need_admin("启动加速器"):
            return
        up = T.preferred_upstream()
        self.busy(lambda: T.start_accel(up),
                  note=f"加速器启动中（上游 {T.upstream_name(up)}）…")

    def act_accel_stop(self):
        if self._need_admin("停止加速器"):
            return
        self.busy(T.stop_accel, note="加速器停止中…")

    def act_switch(self):
        port = self.up_var.get()
        if port != T.PORT_THRONE:
            it = (self.state.get("instances") or {}).get(port) or {}
            if not it.get("running"):
                messagebox.showwarning(
                    "上游不可用",
                    f"{it.get('title', port)} 没在运行，先在左边把它启动起来。",
                    parent=self.root)
                return
        if self._need_admin("切换上游"):
            return
        self.busy(lambda: T.switch_upstream(port),
                  note=f"切换到 {T.upstream_name(port)} …")

    # ---------- 节点动作 ----------

    def _selected_tag(self):
        sel = self.tree.selection()
        if not sel:
            messagebox.showinfo("先选一行", "请先在表格里选中一个节点。",
                                parent=self.root)
            return None
        return sel[0]

    def act_copy_link(self):
        tag = self._selected_tag()
        if not tag:
            return
        for r in (self.state.get("nodes") or {}).get("nodes", []):
            if r["tag"] != tag:
                continue
            link = SL.link_for(r["raw"])
            if not link:
                self.toast(f"{r['type']} 类型暂不支持导出为分享链接")
                return
            self.root.clipboard_clear()
            self.root.clipboard_append(link)
            self.toast(f"已复制 {tag} 的分享链接")
            return
        self.toast(f"找不到节点 {tag}")

    def act_edit_node(self):
        tag = self._selected_tag()
        if not tag:
            return
        for r in (self.state.get("nodes") or {}).get("nodes", []):
            if r["tag"] == tag:
                self.node_dialog("edit", r)
                return

    def act_delete_node(self):
        tag = self._selected_tag()
        if not tag:
            return
        if not messagebox.askyesno(
                "确认删除",
                f"删除节点 {tag}？\n\n会同步从 selector 的节点列表里摘掉。\n"
                f"（写前会自动备份成 config.json.bak）", parent=self.root):
            return

        def job():
            CE.delete_outbound(tag)
            CE.sync_selector_with_nodes()
            return f"已删除 {tag}"
        self.busy(job)

    # ---------- 添加 / 编辑对话框 ----------

    def node_dialog(self, mode, row=None):
        top = tk.Toplevel(self.root)
        top.title("添加节点" if mode == "add" else f"编辑节点 — {row['tag']}")
        top.configure(bg=PANEL)
        top.transient(self.root)
        top.resizable(False, False)

        pad = {"padx": 14}

        tk.Label(top, text="粘贴分享链接", bg=PANEL, fg=MUTED, font=FONT_S
                 ).grid(row=0, column=0, sticky="w", pady=(14, 2), **pad)

        link_var = tk.StringVar()
        ent = ttk.Entry(top, textvariable=link_var, width=68, font=MONO)
        ent.grid(row=1, column=0, sticky="ew", **pad)

        tk.Label(top, text="支持 vless / hysteria2 / trojan / ss / vmess",
                 bg=PANEL, fg=FAINT, font=FONT_S
                 ).grid(row=2, column=0, sticky="w", pady=(2, 10), **pad)

        mid = tk.Frame(top, bg=PANEL)
        mid.grid(row=3, column=0, sticky="ew", **pad)
        mid.columnconfigure(1, weight=1)
        tk.Label(mid, text="tag", bg=PANEL, fg=MUTED, font=FONT_S
                 ).grid(row=0, column=0, sticky="w")
        tag_var = tk.StringVar(value=(row["tag"] if row else ""))
        tag_ent = ttk.Entry(mid, textvariable=tag_var, width=30, font=MONO)
        tag_ent.grid(row=0, column=1, sticky="w", padx=(10, 0))
        if mode == "edit":
            tag_ent.configure(state="readonly")

        tk.Label(top, text="解析结果", bg=PANEL, fg=MUTED, font=FONT_S
                 ).grid(row=4, column=0, sticky="w", pady=(12, 2), **pad)
        prev = tk.Text(top, width=68, height=7, font=MONO_S, bg=PANEL2,
                       fg=FG, relief="solid", borderwidth=1, wrap="word",
                       padx=8, pady=6, state="disabled")
        prev.grid(row=5, column=0, sticky="ew", **pad)

        warn = tk.Label(top, text="", bg=PANEL, fg=WARN, font=FONT_S,
                        wraplength=520, justify="left", anchor="w")
        warn.grid(row=6, column=0, sticky="ew", pady=(6, 0), **pad)

        # 编辑模式下先不回显链接（vmess 之类往返转换可能丢字段），
        # 保留原始出站，用户没重新解析就直接原样提交。
        box = {"obj": (row["raw"] if row else None)}
        if row:
            link = SL.link_for(row["raw"])
            if link:
                link_var.set(link)
            self._preview(prev, row["raw"], SL.describe(row["raw"]))

        def do_parse():
            link = link_var.get().strip()
            if not link:
                warn.configure(text="先粘一条分享链接")
                return
            taken = {r["tag"] for r in
                     ((self.state.get("nodes") or {}).get("nodes") or [])
                     if not row or r["tag"] != row["tag"]}
            try:
                obj, warnings = SL.parse(link, taken_tags=taken)
            except Exception as e:
                box["obj"] = None
                warn.configure(text=f"解析失败：{e}", fg=ERR)
                self._preview(prev, None, "")
                return
            if not obj.get("tag"):
                obj["tag"] = (row["tag"] if row else tag_var.get().strip())
            if row:                      # 编辑不允许改 tag（靠删除+新增）
                obj["tag"] = row["tag"]
            box["obj"] = obj
            tag_var.set(obj["tag"])
            warn.configure(text=("；".join(warnings) if warnings
                                 else "已自动补齐必要字段"),
                           fg=WARN if warnings else OK)
            self._preview(prev, obj, SL.describe(obj))

        def do_save():
            obj = box["obj"]
            if not obj:
                warn.configure(text="还没解析出可用的出站配置", fg=ERR)
                return
            tag = (row["tag"] if row else tag_var.get().strip())
            if not tag:
                warn.configure(text="请给节点起个 tag", fg=ERR)
                return
            obj = dict(obj)
            obj["tag"] = tag

            def job():
                if mode == "add":
                    CE.add_outbound(obj, comments=[])
                    CE.sync_selector_with_nodes(prefer=tag)
                    return f"已添加 {tag}"
                CE.update_outbound(tag, obj)
                return f"已更新 {tag}"
            top.destroy()
            self.busy(job)

        btns = tk.Frame(top, bg=PANEL)
        btns.grid(row=7, column=0, sticky="e", pady=(12, 14), **pad)
        ttk.Button(btns, text="解析链接", style="Mini.TButton",
                   command=do_parse).pack(side="left")
        ttk.Button(btns, text="取消", style="Mini.TButton",
                   command=top.destroy).pack(side="left", padx=(8, 0))
        ttk.Button(btns, text="保存", command=do_save).pack(side="left",
                                                            padx=(8, 0))

        top.columnconfigure(0, weight=1)
        top.grab_set()
        ent.focus_set()
        top.bind("<Return>", lambda e: do_parse())
        top.bind("<Escape>", lambda e: top.destroy())

    @staticmethod
    def _preview(widget, obj, desc):
        widget.configure(state="normal")
        widget.delete("1.0", "end")
        if obj:
            widget.insert("1.0", desc + "\n\n"
                          + json.dumps(obj, ensure_ascii=False, indent=2))
        widget.configure(state="disabled")

    # ---------- 关闭 ----------

    def on_close(self):
        if self.busy_actions:
            if not messagebox.askyesno(
                    "还有操作在跑",
                    "有后台操作正在进行，现在关闭可能让它半途中断。\n确定关闭吗？",
                    parent=self.root):
                return
        self._closed = True
        try:
            self.root.destroy()
        except Exception:
            pass


ROLE_TEXT = {
    "main": "Cloudflare 隧道 · 不占直连 VPS 流量额度，代价是延迟高",
    "backup": "直连 VPS · 延迟最低最稳，与 Throne 共用账号流量",
}


# ============================= 入口 =============================

def main():
    ap = argparse.ArgumentParser(description="Firestorm 加速器桌面面板")
    ap.add_argument("--self-test", action="store_true",
                    help="只构建界面 + 跑一次渲染，不进事件循环（自检用）")
    a = ap.parse_args()

    try:
        import tkinter  # noqa: F401
    except ImportError:
        sys.stderr.write(
            "这个 Python 没编译 tkinter：\n  {0}\n"
            "请改用带 tkinter 的 Python 运行，或双击 gui\\run-gui.cmd。\n"
            .format(sys.executable))
        return 2

    root = tk.Tk()
    app = App(root)

    if a.self_test:
        # 快照是在后台线程跑完再 root.after 回主线程的，所以要一边推事件循环
        # 一边等，光 update() 一次拿不到数据（会误报"节点表 0 行"）。
        import time as _t
        deadline = _t.time() + 12
        while _t.time() < deadline:
            root.update()
            if app.state.get("instances"):
                break
            _t.sleep(0.05)
        root.update()

        ok = bool(app.state.get("instances"))
        print("self-test {0}：窗口已构建，主题 {1}".format(
            "OK" if ok else "FAIL（12s 内没拿到状态）", ttk.Style().theme_use()))
        print("  实例  :", ", ".join(
            f"{p}={('运行中' if v['running'] else '已停止')}"
            for p, v in (app.state.get("instances") or {}).items()) or "—")
        print("  节点表:", len(app.tree.get_children()), "行")
        print("  胶囊  :", ", ".join(
            f"{k}={v[1]['text']}" for k, v in app.chips.items()))
        print("  加速器:", app.accel_hint.cget("text"))
        print("  权限  :", app.admin_chip.cget("text"))
        root.destroy()
        return 0 if ok else 1

    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
