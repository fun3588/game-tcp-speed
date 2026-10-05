#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
firestorm_tray.py  ·  Firestorm 加速器托盘控制台
================================================
一个常驻系统托盘的迷你控制面板，用来一键管理：
  · 两个独立 sing-box 实例（Sing-box/sing-box.exe）
      main   主实例 —— 走 Cloudflare 隧道节点
      backup 备实例 —— 走直连 VPS 节点
  · 加速器中继（py/firestorm_accel.py）

独立 sing-box 的入口/控制端口是**随机的**（每次启动由 Sing-box/start_singbox.py
挑空闲高位端口），本工具不写死端口 —— 一律现读 Sing-box/runtime.json 与
runtime.backup.json。所以菜单里显示的端口号就是当前实际生效的那个。

零第三方依赖 —— 只用 Python 标准库 + ctypes 直调 Win32 API
（本机 pypi 不可达，装不了 pystray / Pillow，索性手写）。

托盘图标颜色即状态：
  绿色 = sing-box 和加速器都在跑
  黄色 = 只跑了一个
  灰色 = 都没跑
  红色 = 异常（副 IP 挂着但中继没起来 —— 此时游戏流量是黑洞）

用法：
    python firestorm_tray.py              正常启动（会自动请求管理员权限）
    python firestorm_tray.py --no-elevate 不提权（仅用于调试 UI）
    python firestorm_tray.py --console    保留控制台窗口看日志
"""

import ctypes
import ctypes.wintypes as wt
import json
import os
import socket
import subprocess
import sys
import time
import traceback

# ----------------------------- 路径与常量 -----------------------------

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                      # game-tcp-speed/
ACCEL_PY = os.path.join(HERE, "firestorm_accel.py")
SB_DIR = os.path.join(ROOT, "Sing-box")
SB_EXE = os.path.join(SB_DIR, "sing-box.exe")
SB_START = os.path.join(SB_DIR, "start_singbox.py")
SB_STOP = os.path.join(SB_DIR, "stop_singbox.py")
LOG_PATH = os.path.join(HERE, "tray.log")

TARGET_IP = "141.95.207.63"           # 被劫持的游戏服务器 IP
ACCEL_PORTS = [1119, 3724, 8085, 8086, 8285, 8286]
PORT_THRONE = 2080                    # 原上游：Throne（端口固定）
# 独立 sing-box 有两个实例（profile），端口每次启动都随机：
#   main   主实例 —— 走 Cloudflare 隧道节点
#   backup 备实例 —— 走直连 VPS 节点
# 端口从 Sing-box/runtime.json 与 runtime.backup.json 现读，写入者是
# Sing-box/start_singbox.py。托盘不缓存、不写死。
SB_PROFILES = [("main", "主实例"), ("backup", "备实例")]

APP_NAME = "Firestorm 加速器"
TRAY_ID = 1
WM_TRAY = 0x8000 + 1                  # WM_APP + 1
WM_TIMER = 0x0113
WM_DESTROY = 0x0002
TIMER_ID = 1
TIMER_MS = 4000

# 菜单命令 ID
ID_ACCEL_START = 1001
ID_ACCEL_STOP = 1002
ID_SB_MAIN = 1003                     # 主实例：按当前状态启动 / 停止
ID_SB_BACKUP = 1004                   # 备实例：按当前状态启动 / 停止
ID_UP_THRONE = 1005
ID_UP_MAIN = 1006
ID_UP_BACKUP = 1010
ID_OPEN_LOG = 1007
ID_OPEN_DIR = 1008
ID_AUTOSTART = 1009
ID_QUIT = 1100

MENU_NONE = 0
MENU_STRING = 0x00000000
MENU_GRAYED = 0x00000001
MENU_SEPARATOR = 0x00000800
MENU_CHECKED = 0x00000008
TPM_RIGHTBUTTON = 0x0002
TPM_RETURNCMD = 0x0100
TPM_NONOTIFY = 0x0080

CREATE_NO_WINDOW = 0x08000000


def LOG(msg):
    try:
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}\n")
    except Exception:
        pass


# ----------------------------- 权限 -----------------------------

def is_admin():
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def elevate():
    """以管理员身份重新启动自己（会弹 UAC）。"""
    try:
        ctypes.windll.shell32.ShellExecuteW(
            None, "runas", sys.executable,
            '"{0}"'.format(os.path.abspath(__file__)), None, 1)
        return True
    except Exception as e:
        LOG(f"提权失败: {e}")
        return False


# ----------------------------- 状态探测 -----------------------------

def sb_state_path(profile):
    """该实例的状态文件。main 沿用历史文件名 runtime.json（兼容旧脚本）。"""
    if profile == "main":
        return os.path.join(SB_DIR, "runtime.json")
    return os.path.join(SB_DIR, "runtime." + profile + ".json")


def sb_runtime(profile="main"):
    """读某个实例的运行时状态（随机端口 / PID / 当前节点）。
    读不到（没跑过 / 已被清理）返回空 dict，调用方按“未运行”处理。

    注意：**绝不能跨 profile 回退** —— 早期版本在读到空文件时会退到
    runtime.json，结果备实例没在跑时会把主实例的端口当成自己的。"""
    try:
        with open(sb_state_path(profile), "r", encoding="utf-8") as f:
            return json.load(f) or {}
    except Exception:
        return {}


def _as_int(v):
    try:
        return int(v) if v else None
    except (TypeError, ValueError):
        return None


def sb_socks_port(profile="main"):
    """该实例的入口端口（随机分配，未运行时返回 None）。"""
    return _as_int(sb_runtime(profile).get("socks_port"))


def sb_clash_port(profile="main"):
    """该实例的 clash_api 控制端口（随机），未运行时 None。"""
    return _as_int(sb_runtime(profile).get("clash_port"))


def sb_node(profile="main"):
    """该实例当前默认节点 tag。"""
    return sb_runtime(profile).get("node") or ""


def port_listening(port, host="127.0.0.1"):
    try:
        s = socket.create_connection((host, port), timeout=0.6)
        s.close()
        return True
    except Exception:
        return False


def alias_present(ip=TARGET_IP):
    """能否 bind 到该 IP —— 能就说明副 IP 挂在本机（与加速器同款判定）。"""
    s = socket.socket()
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((ip, 0))
        return True
    except OSError:
        return False
    finally:
        s.close()


def _netstat_lines():
    """跑一次 netstat（只看 TCP，输出小很多），给下面几个解析函数复用。"""
    try:
        p = subprocess.run(["netstat", "-ano", "-p", "TCP"], capture_output=True,
                           creationflags=CREATE_NO_WINDOW)
        raw = p.stdout.decode("utf-8", "replace")
        if "TCP" not in raw:
            raw = p.stdout.decode("gbk", "replace")
        return raw.splitlines()
    except Exception:
        return []


def accel_pid(lines=None):
    """反查正在监听 副IP:8285 的进程 PID —— 那就是中继。"""
    lines = lines if lines is not None else _netstat_lines()
    for ln in lines:
        parts = ln.split()
        if len(parts) >= 5 and parts[0] == "TCP" and "LISTENING" in ln:
            if parts[1] == f"{TARGET_IP}:8285":
                return int(parts[-1])
    return None


def accel_upstream(pid, lines=None):
    """看中继进程连到哪个本地上游端口，据此判断当前用的是谁。

    上游端口是**随机的**（独立 sing-box 每次启动重挑），所以不能只认固定值 ——
    直接取中继那条到 127.0.0.1 的 ESTABLISHED 连接的远端口即可。
    中继对外的连接目标是游戏服务器 IP（不是回环），不会误判。"""
    if not pid:
        return None
    lines = lines if lines is not None else _netstat_lines()
    found = []
    for ln in lines:
        parts = ln.split()
        if len(parts) >= 5 and parts[-1] == str(pid) and "ESTABLISHED" in ln:
            dst = parts[2]
            if dst.startswith("127.0.0.1:"):
                try:
                    found.append(int(dst.rsplit(":", 1)[1]))
                except ValueError:
                    pass
    if not found:
        return None
    known = [p for p in found if p in known_upstream_ports()]
    return known[0] if known else found[0]


def known_upstream_ports():
    """当前所有合法上游端口：Throne + 两个 sing-box 实例的入口端口。"""
    ports = {PORT_THRONE}
    for prof, _ in SB_PROFILES:
        p = sb_socks_port(prof)
        if p:
            ports.add(p)
    return ports


def upstream_name(port):
    """端口 → 可读名字（给菜单/气泡用）。"""
    if not port:
        return "未知"
    if port == PORT_THRONE:
        return "Throne"
    for prof, title in SB_PROFILES:
        if port == sb_socks_port(prof):
            return f"{title} sing-box"
    return f"未知端口 {port}"


def sb_running(profile="main"):
    """该实例真的在监听吗？（不能只看 runtime 文件，可能是残留）"""
    p = sb_socks_port(profile)
    return bool(p and port_listening(p))


def sb_title(profile):
    for p, t in SB_PROFILES:
        if p == profile:
            return t
    return profile


def sb_brief(profile):
    """给菜单用的一行摘要。"""
    title = sb_title(profile)
    if sb_running(profile):
        return f"{title} 运行中 {sb_socks_port(profile)}"
    return f"{title} 未运行"


def port_bound(ip, port):
    """有人在 ip:port 上监听吗？用 bind 反测：不带 SO_REUSEADDR 能 bind 成功
    就说明没人占。比 connect 快，也不会真的建起一条连接去惊动中继。

    **必须分两步**，不能只看第二次 bind 的成败：
    副 IP 没挂在本机时，bind((ip, port)) 一样会失败，但原因是**地址不存在**
    （Windows 返回 WSAEINVAL 10022），不是"被占用"。只判一次的话，副 IP 一摘
    就会谎报"中继在监听" —— 托盘图标显示绿色"运行中"、面板把「停止加速器」
    点亮而「启动加速器」置灰，正好全反了。
    """
    # 1) 该 IP 在本机吗？不在就谈不上监听（和 alias_present 同款判定）。
    s = socket.socket()
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((ip, 0))
    except OSError:
        return False
    finally:
        s.close()

    # 2) 端口被占了吗？
    s = socket.socket()
    try:
        s.bind((ip, port))
        return False
    except OSError:
        return True
    finally:
        s.close()


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
                ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p),
                ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD),
                ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", wt.WCHAR * 260)]


TH32CS_SNAPPROCESS = 0x00000002
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


def _enum_processes():
    """用 Toolhelp 快照枚举进程名，比 tasklist 快两个数量级。"""
    names = []
    kernel32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snap or snap == INVALID_HANDLE_VALUE:
        return names
    try:
        pe = PROCESSENTRY32W()
        pe.dwSize = ctypes.sizeof(PROCESSENTRY32W)
        ok = kernel32.Process32FirstW(ctypes.c_void_p(snap), ctypes.byref(pe))
        while ok:
            names.append(pe.szExeFile.lower())
            ok = kernel32.Process32NextW(ctypes.c_void_p(snap), ctypes.byref(pe))
    finally:
        kernel32.CloseHandle(ctypes.c_void_p(snap))
    return names


def game_running():
    """看有没有 WoW 客户端在跑（进程名以 wow 开头，如 'wow.exe'）。"""
    try:
        return any(n.startswith("wow") for n in _enum_processes())
    except Exception:
        return False


def sb_pid(profile="main", lines=None):
    """该实例 sing-box 的 PID：从当前随机入口端口反查。
    用 endswith(":端口") 匹配，这样 IPv4/IPv6 两种监听写法都能命中。"""
    sp = sb_socks_port(profile)
    if not sp:
        return None
    lines = lines if lines is not None else _netstat_lines()
    suffix = f":{sp}"
    for ln in lines:
        parts = ln.split()
        if len(parts) >= 5 and parts[0] == "TCP" and "LISTENING" in ln:
            if parts[1].endswith(suffix):
                return int(parts[-1])
    return None


def snapshot(full=False):
    """quick（默认）只做几次本地探测，可以放心高频刷；
    full 额外跑 netstat / 进程快照拿上游与游戏状态，只在开菜单时用。"""
    sb = {}
    for prof, title in SB_PROFILES:
        port = sb_socks_port(prof)
        sb[prof] = {
            "title": title,
            "running": bool(port and port_listening(port)),
            "port": port,
            "node": sb_node(prof),
            "pid": None,
        }
    st = {
        "alias": alias_present(),                 # 副 IP 挂在本机？
        "relay": port_bound(TARGET_IP, 8285),     # 中继在监听？
        "singbox": any(v["running"] for v in sb.values()),   # 有任一实例在跑？
        "throne": port_listening(PORT_THRONE),
        "sb": sb,
        "accel_pid": None, "upstream": None, "game": None,
    }
    st["accel"] = st["relay"]
    if full:
        lines = _netstat_lines()
        pid = accel_pid(lines)
        st["accel_pid"] = pid
        st["upstream"] = accel_upstream(pid, lines)
        for prof in sb:
            sb[prof]["pid"] = sb_pid(prof, lines)
        st["game"] = game_running()
    return st


def health(st):
    """返回 (颜色, 一句话状态)。颜色决定托盘图标。"""
    if st["alias"] and not st["relay"]:
        return (200, 60, 60), "异常：副 IP 在但中继未运行（游戏连不上）"
    if st["relay"]:
        return (40, 170, 90), "运行中"
    if st["singbox"]:
        return (230, 170, 40), "仅 sing-box 运行（加速器已停）"
    return (140, 140, 140), "已停止"


# ----------------------------- 动作 -----------------------------

def find_python():
    for p in (sys.executable,
              r"C:\Users\junjun\.workbuddy\binaries\python\versions\3.13.12\python.exe",
              r"D:\py3-12-1\python.exe"):
        if p and os.path.exists(p):
            return p
    return "python"


def _run(args, **kw):
    kw.setdefault("creationflags", CREATE_NO_WINDOW)
    kw.setdefault("capture_output", True)
    return subprocess.run(args, **kw)


def start_singbox(profile="main"):
    """随机端口启动某个实例 —— 全权交给 Sing-box/start_singbox.py 处理
    （挑端口、生成运行时配置、写状态文件），托盘只负责拉起来 + 等它就绪。"""
    title = sb_title(profile)
    if sb_running(profile):
        return f"{title} 已在运行（入口 {sb_socks_port(profile)}）"
    if not os.path.exists(SB_EXE):
        return f"找不到内核: {SB_EXE}"
    if not os.path.exists(SB_START):
        return f"找不到启动脚本: {SB_START}"
    # 注意：start_singbox.py 是“主管进程”，它自己 wait() 着 sing-box，
    # 被杀时会顺带清理状态文件 —— 所以这里不要用 run() 同步等它。
    subprocess.Popen([find_python(), SB_START, "--profile", profile], cwd=SB_DIR,
                     creationflags=CREATE_NO_WINDOW,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL)
    for _ in range(60):                       # 最多等 15s（含配置校验时间）
        time.sleep(0.25)
        if sb_running(profile):
            LOG(f"{title} 已启动，随机入口端口 {sb_socks_port(profile)}")
            return f"{title} 已启动（入口 {sb_socks_port(profile)}）"
    return f"{title} 启动失败，请看 Sing-box\\singbox{'（'+profile+'）' if profile!='main' else ''}.log"


def stop_singbox(profile="main"):
    """停止某个实例 —— 优先交给 stop_singbox.py（它会杀 PID 并清状态文件）。"""
    title = sb_title(profile)
    if not sb_running(profile) and not sb_socks_port(profile):
        return f"{title} 未在运行"
    if os.path.exists(SB_STOP):
        _run([find_python(), SB_STOP, "--profile", profile], cwd=SB_DIR)
    else:                                     # 脚本缺失时退化：直接按端口反查 PID 杀
        pid = sb_pid(profile)
        if pid:
            _run(["taskkill", "/F", "/PID", str(pid)])
    time.sleep(0.4)
    LOG(f"{title} 已停止")
    return f"{title} 已停止"


def preferred_upstream():
    """启动加速器时的默认上游：优先主实例，其次备实例，都没有就回退 Throne。"""
    for prof, _ in SB_PROFILES:
        p = sb_socks_port(prof)
        if p and port_listening(p):
            return p
    return PORT_THRONE


def start_accel(upstream_port):
    st = snapshot()
    if st["accel"]:
        return "加速器已在运行（请先停止再切换上游）"
    if not upstream_port:
        return "上游不可用：独立 sing-box 未运行"
    socks = f"127.0.0.1:{upstream_port}"
    subprocess.Popen([find_python(), ACCEL_PY, "setup", "--socks", socks],
                     cwd=HERE, creationflags=CREATE_NO_WINDOW,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL)
    for _ in range(40):
        time.sleep(0.25)
        if accel_pid():
            LOG(f"加速器已启动，上游 {socks}")
            return f"加速器已启动（上游 {upstream_name(upstream_port)}）"
    return "加速器启动失败，请看 firestorm_accel.log"


def stop_accel():
    pid = accel_pid()
    if not pid:
        # 中继不在，但副 IP 可能残留 —— 一样要清
        _run([find_python(), ACCEL_PY, "teardown"], cwd=HERE)
        return "加速器未在运行（已顺带清理副 IP）"
    _run(["taskkill", "/F", "/PID", str(pid)])
    time.sleep(0.8)
    # 强杀不会触发进程内的清理回调，副 IP 得自己摘
    _run([find_python(), ACCEL_PY, "teardown"], cwd=HERE)
    LOG(f"加速器已停止 (PID {pid})，副 IP 已清理")
    return "加速器已停止，副 IP 已清理"


def switch_upstream(port):
    if not port:
        return "该 sing-box 实例未在运行"
    st = snapshot(full=True)
    if not st["accel"]:
        return start_accel(port)
    if st["upstream"] == port:
        return f"上游已经是 {upstream_name(port)}"
    stop_accel()
    time.sleep(1.0)
    return start_accel(port)


def open_path(path):
    try:
        os.startfile(path)          # noqa: S606
    except Exception as e:
        LOG(f"打开失败 {path}: {e}")


_autostart_cache = {"t": 0.0, "v": False}


def autostart_enabled(max_age=15.0):
    """带缓存 —— schtasks 一次要 100~300ms，而右键菜单里要显示勾选状态，
    不能每次开菜单都真去跑一遍。"""
    now = time.time()
    if now - _autostart_cache["t"] < max_age:
        return _autostart_cache["v"]
    try:
        p = _run(["schtasks", "/Query", "/TN", "FirestormAccelTray"])
        val = p.returncode == 0
    except Exception:
        val = False
    _autostart_cache.update(t=now, v=val)
    return val


def toggle_autostart():
    _autostart_cache["t"] = 0.0            # 先失效缓存
    if autostart_enabled():
        _run(["schtasks", "/Delete", "/TN", "FirestormAccelTray", "/F"])
        _autostart_cache["t"] = 0.0
        LOG("已关闭开机自启")
        return "已关闭开机自启"
    cmd = f'"{sys.executable}" "{os.path.abspath(__file__)}"'
    p = _run(["schtasks", "/Create", "/TN", "FirestormAccelTray",
              "/TR", cmd, "/SC", "ONLOGON", "/RL", "HIGHEST", "/F"])
    ok = p.returncode == 0
    _autostart_cache["t"] = 0.0
    LOG(f"设置开机自启: {'成功' if ok else '失败'}")
    return "已开启开机自启（登录时以管理员身份运行）" if ok else "设置开机自启失败"


# ----------------------------- Win32 绑定 -----------------------------

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
shell32 = ctypes.windll.shell32
kernel32 = ctypes.windll.kernel32

LRESULT = ctypes.c_ssize_t
# 注意：不要用 wt.WPARAM / wt.LPARAM —— 在 64 位 Python 上它们会退化成 32 位，
# 导致 lParam 溢出（实测报 "argument 4: OverflowError: int too long to convert"）。
# 用 size_t / ssize_t 才和 Windows 的 UINT_PTR / LONG_PTR 对齐。
WPARAM = ctypes.c_size_t
LPARAM = ctypes.c_ssize_t
WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wt.HWND, wt.UINT, WPARAM, LPARAM)


class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", wt.UINT),
                ("lpfnWndProc", WNDPROC),
                ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE),
                ("hIcon", wt.HICON),
                ("hCursor", wt.HANDLE),
                ("hbrBackground", wt.HBRUSH),
                ("lpszMenuName", wt.LPCWSTR),
                ("lpszClassName", wt.LPCWSTR)]


class NOTIFYICONDATAW(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD),
                ("hWnd", wt.HWND),
                ("uID", wt.UINT),
                ("uFlags", wt.UINT),
                ("uCallbackMessage", wt.UINT),
                ("hIcon", wt.HICON),
                ("szTip", wt.WCHAR * 128),
                ("dwState", wt.DWORD),
                ("dwStateMask", wt.DWORD),
                ("szInfo", wt.WCHAR * 256),
                ("uVersion", wt.UINT),
                ("szInfoTitle", wt.WCHAR * 64),
                ("dwInfoFlags", wt.DWORD),
                ("guidItem", ctypes.c_byte * 16),
                ("hBalloonIcon", wt.HICON)]


class ICONINFO(ctypes.Structure):
    _fields_ = [("fIcon", wt.BOOL),
                ("xHotspot", wt.DWORD),
                ("yHotspot", wt.DWORD),
                ("hbmMask", wt.HBITMAP),
                ("hbmColor", wt.HBITMAP)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", wt.WORD),
                ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wt.DWORD * 3)]


NIM_ADD, NIM_MODIFY, NIM_DELETE = 0, 1, 2
NIF_MESSAGE, NIF_ICON, NIF_TIP = 0x01, 0x02, 0x04
IDI_APPLICATION = 32512

PV = ctypes.c_void_p


def _declare_signatures():
    """ctypes 默认把参数当 c_int、返回值当 32 位 int —— 在 64 位下会截断句柄，
    也会让 WPARAM/LPARAM 溢出。用到的函数逐个声明清楚（句柄统一用 c_void_p）。"""
    def sig(fn, res, *args):
        fn.restype = res
        fn.argtypes = list(args)

    sig(user32.DefWindowProcW, LRESULT, PV, wt.UINT, WPARAM, LPARAM)
    sig(user32.CreateWindowExW, PV, wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        PV, PV, PV, PV)
    sig(user32.GetMessageW, ctypes.c_int, ctypes.POINTER(wt.MSG), PV,
        wt.UINT, wt.UINT)
    sig(user32.TranslateMessage, wt.BOOL, ctypes.POINTER(wt.MSG))
    sig(user32.DispatchMessageW, LRESULT, ctypes.POINTER(wt.MSG))
    sig(user32.PostQuitMessage, None, ctypes.c_int)
    sig(user32.PostMessageW, wt.BOOL, PV, wt.UINT, WPARAM, LPARAM)
    sig(user32.CreatePopupMenu, PV)
    sig(user32.AppendMenuW, wt.BOOL, PV, wt.UINT, ctypes.c_size_t, wt.LPCWSTR)
    sig(user32.DestroyMenu, wt.BOOL, PV)
    sig(user32.TrackPopupMenu, wt.UINT, PV, wt.UINT, ctypes.c_int,
        ctypes.c_int, ctypes.c_int, PV, PV)
    sig(user32.SetTimer, ctypes.c_size_t, PV, ctypes.c_size_t, wt.UINT, PV)
    sig(user32.SetForegroundWindow, wt.BOOL, PV)
    sig(user32.GetCursorPos, wt.BOOL, ctypes.POINTER(wt.POINT))
    sig(user32.RegisterClassW, wt.WORD, PV)
    sig(user32.LoadCursorW, PV, PV, wt.LPCWSTR)
    sig(user32.DestroyIcon, wt.BOOL, PV)
    sig(user32.CreateIconIndirect, PV, ctypes.POINTER(ICONINFO))
    sig(user32.GetDC, PV, PV)
    sig(user32.ReleaseDC, ctypes.c_int, PV, PV)
    sig(user32.ShowWindow, wt.BOOL, PV, ctypes.c_int)
    sig(gdi32.CreateDIBSection, PV, PV, ctypes.POINTER(BITMAPINFO), wt.UINT,
        ctypes.POINTER(PV), PV, wt.DWORD)
    sig(gdi32.CreateBitmap, PV, ctypes.c_int, ctypes.c_int, wt.UINT, wt.UINT, PV)
    sig(gdi32.DeleteObject, wt.BOOL, PV)
    sig(shell32.Shell_NotifyIconW, wt.BOOL, wt.DWORD,
        ctypes.POINTER(NOTIFYICONDATAW))


_declare_signatures()


def make_dot_icon(rgb, size=32):
    """用 GDI 画一个实心圆点当托盘图标（零依赖，比带 png 资源省事）。"""
    W = H = size
    cx = cy = (size - 1) / 2.0
    rad = size / 2.0 - 2.0
    b, g, r = rgb

    bmi = BITMAPINFO()
    bmi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.bmiHeader.biWidth = W
    bmi.bmiHeader.biHeight = -H          # 负数 = top-down
    bmi.bmiHeader.biPlanes = 1
    bmi.bmiHeader.biBitCount = 32
    bmi.bmiHeader.biCompression = 0      # BI_RGB

    ppv = ctypes.c_void_p()
    hdc = user32.GetDC(None)
    hbm = gdi32.CreateDIBSection(hdc, ctypes.byref(bmi), 0,
                                 ctypes.byref(ppv), None, 0)
    if not hbm:
        user32.ReleaseDC(None, hdc)
        return None

    buf = (ctypes.c_ubyte * (W * H * 4)).from_address(ppv.value)
    for y in range(H):
        for x in range(W):
            i = (y * W + x) * 4
            inside = (x - cx) ** 2 + (y - cy) ** 2 <= rad * rad
            if inside:
                buf[i], buf[i + 1], buf[i + 2], buf[i + 3] = b, g, r, 255
            else:
                buf[i], buf[i + 1], buf[i + 2], buf[i + 3] = 255, 255, 255, 0

    # 掩码位图：圆外为 1（透明）。每行按 WORD 对齐，32 像素 = 4 字节正好对齐。
    stride = (W + 15) // 16 * 2
    mask = bytearray(stride * H)
    for y in range(H):
        for x in range(W):
            if (x - cx) ** 2 + (y - cy) ** 2 > rad * rad:
                mask[y * stride + x // 8] |= 0x80 >> (x % 8)
    mbuf = (ctypes.c_ubyte * len(mask)).from_buffer(mask)
    hmsk = gdi32.CreateBitmap(W, H, 1, 1, mbuf)

    ii = ICONINFO()
    ii.fIcon = 1
    ii.hbmColor = hbm
    ii.hbmMask = hmsk
    hicon = user32.CreateIconIndirect(ctypes.byref(ii))

    gdi32.DeleteObject(hbm)
    if hmsk:
        gdi32.DeleteObject(hmsk)
    user32.ReleaseDC(None, hdc)
    return hicon


# ----------------------------- 托盘主体 -----------------------------

class Tray(object):
    def __init__(self):
        self.hwnd = None
        self.hicon = None
        self.last_rgb = None
        self._wndproc = WNDPROC(self._on_message)   # 必须保持引用，否则被 GC
        self.class_name = "FirestormTrayWnd"
        self.notify = None
        self.stop_flag = False

    # ---- 窗口 ----
    def create_window(self):
        hinst = kernel32.GetModuleHandleW(None)
        wc = WNDCLASSW()
        wc.lpfnWndProc = self._wndproc
        wc.hInstance = hinst
        wc.lpszClassName = self.class_name
        wc.hCursor = user32.LoadCursorW(None, ctypes.c_wchar_p(IDI_APPLICATION))
        if not user32.RegisterClassW(ctypes.byref(wc)):
            err = kernel32.GetLastError()
            if err != 1410:                          # 1410 = 类已注册
                raise ctypes.WinError(err)
        self.hwnd = user32.CreateWindowExW(
            0, self.class_name, APP_NAME, 0, 0, 0, 0, 0,
            None, None, hinst, None)
        if not self.hwnd:
            raise ctypes.WinError(kernel32.GetLastError())

    def add_icon(self, rgb):
        self.hicon = make_dot_icon(rgb)
        nid = NOTIFYICONDATAW()
        nid.cbSize = ctypes.sizeof(NOTIFYICONDATAW)
        nid.hWnd = self.hwnd
        nid.uID = TRAY_ID
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP
        nid.uCallbackMessage = WM_TRAY
        nid.hIcon = self.hicon
        nid.szTip = APP_NAME
        self.notify = nid
        if not shell32.Shell_NotifyIconW(NIM_ADD, ctypes.byref(nid)):
            raise ctypes.WinError(kernel32.GetLastError())
        self.last_rgb = rgb

    def update_icon(self, rgb, tip):
        if not self.notify:
            return
        if rgb != self.last_rgb:
            new = make_dot_icon(rgb)
            if new:
                old = self.hicon
                self.hicon = new
                self.notify.hIcon = new
                if old:
                    user32.DestroyIcon(old)
                self.last_rgb = rgb
        self.notify.uFlags = NIF_ICON | NIF_TIP
        self.notify.szTip = tip[:127]
        shell32.Shell_NotifyIconW(NIM_MODIFY, ctypes.byref(self.notify))

    def remove_icon(self):
        if self.notify:
            shell32.Shell_NotifyIconW(NIM_DELETE, ctypes.byref(self.notify))
            self.notify = None
        if self.hicon:
            user32.DestroyIcon(self.hicon)
            self.hicon = None

    # ---- 菜单 ----
    def build_menu(self, st):
        hmenu = user32.CreatePopupMenu()
        rgb, txt = health(st)
        state = txt

        if st["accel"]:
            up = st["upstream"]
            state += f"  |  上游: {upstream_name(up)}"
        if st["game"]:
            state += "  |  游戏在线"
        user32.AppendMenuW(hmenu, MENU_STRING | MENU_GRAYED, 0, state)

        if st["alias"] and not st["accel"]:
            user32.AppendMenuW(hmenu, MENU_STRING | MENU_GRAYED, 0,
                               "⚠ 副 IP 残留，点“停止加速器”可清理")
        user32.AppendMenuW(hmenu, MENU_SEPARATOR, 0, None)

        if st["accel"]:
            user32.AppendMenuW(hmenu, MENU_STRING, ID_ACCEL_STOP, "停止加速器")
        else:
            user32.AppendMenuW(hmenu, MENU_STRING, ID_ACCEL_START, "启动加速器")

        # ---- sing-box 实例（两个，各自随机端口、可独立起停）----
        user32.AppendMenuW(hmenu, MENU_SEPARATOR, 0, None)
        user32.AppendMenuW(hmenu, MENU_STRING | MENU_GRAYED, 0, "sing-box 实例")
        for prof, title in SB_PROFILES:
            info = st["sb"].get(prof, {})
            running = info.get("running")
            port = info.get("port")
            mid = ID_SB_MAIN if prof == "main" else ID_SB_BACKUP
            if running:
                user32.AppendMenuW(hmenu, MENU_STRING, mid,
                                   f"  停止{title}  (入口 {port})")
            else:
                user32.AppendMenuW(hmenu, MENU_STRING, mid,
                                   f"  启动{title}（随机端口）")

        # ---- 上游节点 ----
        user32.AppendMenuW(hmenu, MENU_SEPARATOR, 0, None)
        user32.AppendMenuW(hmenu, MENU_STRING | MENU_GRAYED, 0, "上游节点")
        f = MENU_STRING | (MENU_CHECKED if st["upstream"] == PORT_THRONE else 0)
        user32.AppendMenuW(hmenu, f, ID_UP_THRONE, "  Throne  (2080)")
        for prof, title in SB_PROFILES:
            info = st["sb"].get(prof, {})
            mid = ID_UP_MAIN if prof == "main" else ID_UP_BACKUP
            node = info.get("node") or ""
            if info.get("running"):
                label = f"  {title} sing-box  ({info['port']}"
                label += f"，{node})" if node else ")"
                f = MENU_STRING | (MENU_CHECKED if st["upstream"] == info["port"] else 0)
                user32.AppendMenuW(hmenu, f, mid, label)
            else:
                user32.AppendMenuW(hmenu, MENU_STRING | MENU_GRAYED, 0,
                                   f"  {title} sing-box  (未运行)")

        user32.AppendMenuW(hmenu, MENU_SEPARATOR, 0, None)
        f = MENU_STRING | (MENU_CHECKED if autostart_enabled() else 0)
        user32.AppendMenuW(hmenu, f, ID_AUTOSTART, "开机自动启动")
        user32.AppendMenuW(hmenu, MENU_STRING, ID_OPEN_LOG, "查看日志")
        user32.AppendMenuW(hmenu, MENU_STRING, ID_OPEN_DIR, "打开程序目录")
        user32.AppendMenuW(hmenu, MENU_SEPARATOR, 0, None)
        user32.AppendMenuW(hmenu, MENU_STRING, ID_QUIT, "退出")
        return hmenu

    def show_menu(self):
        st = snapshot(full=True)
        hmenu = self.build_menu(st)

        pt = wt.POINT()
        user32.GetCursorPos(ctypes.byref(pt))
        # 托盘菜单必须先让本窗口到前台，否则点别处菜单不消失
        user32.SetForegroundWindow(self.hwnd)
        cmd = user32.TrackPopupMenu(
            hmenu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
            pt.x, pt.y, 0, self.hwnd, None)
        user32.PostMessageW(self.hwnd, 0, 0, 0)
        user32.DestroyMenu(hmenu)
        if cmd:
            self.handle_command(cmd)

    def handle_command(self, cmd):
        if cmd == ID_QUIT:
            self.stop_flag = True
            user32.PostQuitMessage(0)
            return
        try:
            if cmd == ID_ACCEL_START:
                msg = start_accel(preferred_upstream())
            elif cmd == ID_ACCEL_STOP:
                msg = stop_accel()
            elif cmd == ID_SB_MAIN:
                msg = (stop_singbox("main") if sb_running("main")
                       else start_singbox("main"))
            elif cmd == ID_SB_BACKUP:
                msg = (stop_singbox("backup") if sb_running("backup")
                       else start_singbox("backup"))
            elif cmd == ID_UP_THRONE:
                msg = switch_upstream(PORT_THRONE)
            elif cmd == ID_UP_MAIN:
                msg = switch_upstream(sb_socks_port("main"))
            elif cmd == ID_UP_BACKUP:
                msg = switch_upstream(sb_socks_port("backup"))
            elif cmd == ID_AUTOSTART:
                msg = toggle_autostart()
            elif cmd == ID_OPEN_LOG:
                open_path(LOG_PATH if os.path.exists(LOG_PATH) else HERE)
                msg = "已打开日志"
            elif cmd == ID_OPEN_DIR:
                open_path(HERE)
                msg = "已打开程序目录"
            else:
                return
            LOG(f"菜单 {cmd} -> {msg}")
            self.balloon(msg)
        except Exception as e:
            LOG(f"命令 {cmd} 异常: {e}\n{traceback.format_exc()}")
            self.balloon(f"操作失败: {e}")

    def balloon(self, text):
        if not self.notify:
            return
        nid = self.notify
        nid.uFlags = NIF_ICON | NIF_TIP
        shell32.Shell_NotifyIconW(NIM_MODIFY, ctypes.byref(nid))

    # ---- 消息 ----
    def _on_message(self, hwnd, msg, wparam, lparam):
        try:
            if msg == WM_TRAY:
                ev = lparam & 0xFFFF
                if ev in (0x0205, 0x0203):        # WM_RBUTTONUP / DBLCLK
                    self.show_menu()
                elif ev == 0x0202:                # WM_LBUTTONUP
                    self.balloon(health(snapshot())[1])
                return 0
            if msg == WM_TIMER:
                st = snapshot()                   # quick：只有 4 次本地探测
                rgb, txt = health(st)
                self.update_icon(rgb, f"{APP_NAME}\n{txt}")
                return 0
            if msg == WM_DESTROY:
                user32.PostQuitMessage(0)
                return 0
        except Exception as e:
            LOG(f"消息处理异常: {e}\n{traceback.format_exc()}")
        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    # ---- 主循环 ----
    def run(self):
        self.create_window()
        st = snapshot(full=True)
        rgb, txt = health(st)
        self.add_icon(rgb)
        self.update_icon(rgb, f"{APP_NAME}\n{txt}")
        user32.SetTimer(self.hwnd, TIMER_ID, TIMER_MS, None)

        msg = wt.MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        self.remove_icon()
        return 0


def main():
    args = sys.argv[1:]
    if "--no-elevate" not in args and not is_admin():
        LOG("未提权，请求管理员权限重新启动")
        if elevate():
            return 0
        LOG("提权被拒绝，继续以普通权限运行（启停功能会失败）")

    if "--console" not in args:
        # 把控制台窗口藏掉（双击 .cmd 时会弹一个黑框）
        try:
            hwnd = kernel32.GetConsoleWindow()
            if hwnd:
                user32.ShowWindow(hwnd, 0)
        except Exception:
            pass

    LOG(f"托盘启动 管理员={is_admin()}")
    try:
        return Tray().run()
    except Exception as e:
        LOG(f"启动失败: {e}\n{traceback.format_exc()}")
        ctypes.windll.user32.MessageBoxW(
            None, f"启动失败：\n{e}\n\n详情见 {LOG_PATH}", APP_NAME, 0x10)
        return 1


if __name__ == "__main__":
    sys.exit(main())
