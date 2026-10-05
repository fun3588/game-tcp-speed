#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
share_link.py  ·  分享链接 → sing-box 出站 JSON
================================================
把常见的代理分享链接解析成可以直接塞进 config.json 的 outbounds 对象。

支持：
    vless://    （tcp / ws / grpc / http，none / tls / reality）
    hysteria2:// 与 hy2://
    trojan://
    ss://       （SIP002 与老式 base64 两种写法）
    vmess://    （base64(JSON)）

设计约定：
    · parse() 返回 (outbound, warnings)，**不抛异常表示成功**；
      参数不合法就 raise ValueError，消息直接给用户看。
    · warnings 是"我们替你改了什么"，必须显示给用户 —— 因为
      链接里的某些参数在 sing-box 里是无效甚至有害的（见下面 flow 的注释）。

踩坑备忘（都是实测过的）：
    · **flow 只能用在裸 TCP 上**。`xtls-rprx-vision` 是 REALITY/纯 TLS 的
      TCP 直连特性，挂在 websocket / grpc / http 传输上 sing-box 直接起不来。
      所以这里只在 net==tcp 且没有 headerType 时才保留 flow，否则丢掉并警告。
    · **ws 的 alpn 只能是 http/1.1**。链接里写 h2 的话必然握手失败，这里强制纠正。
    · **reality 必须带 utls**，否则报 "uTLS is required by reality client"。
    · alpn / host 这类参数在链接里可能是逗号分隔，要拆成数组。
"""

import base64
import binascii
import json
import re
import urllib.parse


# ----------------------------- 小工具 -----------------------------

def _q(url):
    """取 query，值全部按 str 返回；同名参数保留第一个。"""
    out = {}
    for k, v in urllib.parse.parse_qsl(url.query, keep_blank_values=True):
        if k not in out:
            out[k] = v
    return out


def _truthy(v):
    return str(v).strip().lower() in ("1", "true", "yes", "on")


def _alpn(v, fallback=None):
    """alpn 可能是 'h2,http/1.1' 或重复参数，统一成数组。"""
    if not v:
        return list(fallback) if fallback else None
    if isinstance(v, (list, tuple)):
        items = list(v)
    else:
        items = [x for x in re.split(r"[,\s]+", str(v)) if x]
    return items or (list(fallback) if fallback else None)


def _name(url, default):
    """链接 #fragment 里的名字，URL 解码后当 tag。"""
    frag = url.fragment or ""
    frag = urllib.parse.unquote(frag).strip()
    return frag or default


def _safe_tag(tag, taken=None):
    """tag 去空白；重名就加 -2 -3。sing-box 的 tag 可以是任意字符串，
    但留着空格/井号在 .cmd 和 URL 里很麻烦，统一清掉。"""
    t = re.sub(r"\s+", "_", (tag or "").strip())
    t = t.replace("#", "").replace("/", "_").replace("\\", "_")
    t = t or "node"
    if not taken:
        return t
    if t not in taken:
        return t
    i = 2
    while f"{t}-{i}" in taken:
        i += 1
    return f"{t}-{i}"


def _b64pad(s):
    """分享链接里的 base64 经常没补齐 padding，也可能用了 URL-safe 字母表。"""
    s = s.strip().replace("-", "+").replace("_", "/")
    return s + "=" * (-len(s) % 4)


def _b64decode(s):
    raw = base64.b64decode(_b64pad(s))
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("utf-8", "replace")


# ----------------------------- 各协议 -----------------------------

def _parse_vless(url):
    warn = []
    out = {"type": "vless", "tag": ""}
    uuid = urllib.parse.unquote(url.username or "")
    if not uuid:
        raise ValueError("vless 链接里没有 uuid")
    if not url.hostname:
        raise ValueError("vless 链接里没有服务器地址")

    q = _q(url)
    net = (q.get("type") or q.get("net") or "tcp").lower()
    security = (q.get("security") or "none").lower()
    sni = q.get("sni") or q.get("peer") or ""
    host = q.get("host") or ""
    path = q.get("path") or ""
    fp = q.get("fp") or "chrome"
    flow = q.get("flow") or ""
    hdr = (q.get("headerType") or "").lower()

    out["server"] = url.hostname
    out["server_port"] = url.port or (443 if security in ("tls", "reality") else 80)
    out["uuid"] = uuid

    # packetEncoding=xudp —— 影响 UDP 走法，带上更稳
    if (q.get("packetEncoding") or "").lower() == "xudp":
        out["packet_encoding"] = "xudp"

    # ---------- 传输层 ----------
    if net == "ws":
        tr = {"type": "ws"}
        if path:
            tr["path"] = path
        if host:
            tr["headers"] = {"Host": host}
        elif security == "tls" and sni:
            # 隧道类（cloudflare 等）经常靠 Host 路由，缺了就补 sni
            tr["headers"] = {"Host": sni}
        out["transport"] = tr
    elif net in ("grpc", "gun"):
        svc = q.get("serviceName") or q.get("servicename") or path.lstrip("/")
        out["transport"] = {"type": "grpc", "service_name": svc}
    elif net in ("http", "h2", "h2c") or hdr == "http":
        tr = {"type": "http"}
        if host:
            tr["host"] = [h for h in re.split(r"[,\s]+", host) if h]
        if path:
            tr["path"] = path
        out["transport"] = tr
    elif net in ("tcp", "raw", ""):
        pass
    else:
        warn.append(f"传输类型 {net} 不常见，按 tcp 处理")

    # ---------- TLS ----------
    if security == "reality":
        pbk = q.get("pbk") or ""
        if not pbk:
            raise ValueError("reality 链接缺少 pbk（public key）")
        tls = {
            "enabled": True,
            "server_name": sni or host or url.hostname,
            "utls": {"enabled": True, "fingerprint": fp},
            "reality": {"enabled": True, "public_key": pbk},
        }
        sid = q.get("sid") or ""
        if sid:
            tls["reality"]["short_id"] = sid
        if _truthy(q.get("allowInsecure") or q.get("insecure")):
            tls["insecure"] = True
        a = _alpn(q.get("alpn"))
        if a:
            tls["alpn"] = a
        out["tls"] = tls
    elif security in ("tls", "xtls"):
        tls = {"enabled": True}
        if sni or host:
            tls["server_name"] = sni or host
        # ws 必须 http/1.1 —— 链接里写 h2 也得纠正，否则握手必挂
        if net == "ws":
            a = _alpn(q.get("alpn"), fallback=["http/1.1"])
            if a != ["http/1.1"]:
                warn.append("ws 传输只支持 http/1.1，已把 alpn 纠正为 http/1.1")
            tls["alpn"] = ["http/1.1"]
        else:
            a = _alpn(q.get("alpn"))
            if a:
                tls["alpn"] = a
        if _truthy(q.get("allowInsecure") or q.get("insecure")):
            tls["insecure"] = True
        # reality 之外的 tls 也常带 fp，带上能过指纹检测
        if fp:
            tls["utls"] = {"enabled": True, "fingerprint": fp}
        out["tls"] = tls

    # ---------- flow ----------
    if flow:
        if net in ("tcp", "raw", "") and not hdr:
            out["flow"] = flow
        else:
            warn.append(
                f"链接带 flow={flow}，但它只对裸 TCP 有效；"
                f"{net} 传输上会被 sing-box 拒绝，已忽略")

    return out, warn


def _parse_hysteria2(url):
    warn = []
    out = {"type": "hysteria2", "tag": ""}
    if not url.hostname:
        raise ValueError("hysteria2 链接里没有服务器地址")
    q = _q(url)
    password = urllib.parse.unquote(url.password or url.username or "")
    if not password:
        raise ValueError("hysteria2 链接里没有密码")

    out["server"] = url.hostname
    out["server_port"] = url.port or 443
    out["password"] = password

    sni = q.get("sni") or q.get("peer") or ""
    tls = {"enabled": True}
    if sni:
        tls["server_name"] = sni
    a = _alpn(q.get("alpn"), fallback=["h3"])
    tls["alpn"] = a
    if _truthy(q.get("insecure") or q.get("allowInsecure")):
        tls["insecure"] = True
    out["tls"] = tls

    # obfs（salamander）—— 服务端开了才要填
    obfs = q.get("obfs")
    if obfs and obfs.lower() != "none":
        ob = {"type": obfs.lower()}
        op = q.get("obfs-password") or q.get("obfs_password") or ""
        if op:
            ob["password"] = op
        out["obfs"] = ob
    return out, warn


def _parse_trojan(url):
    warn = []
    out = {"type": "trojan", "tag": ""}
    if not url.hostname:
        raise ValueError("trojan 链接里没有服务器地址")
    q = _q(url)
    password = urllib.parse.unquote(url.username or "")
    if not password:
        raise ValueError("trojan 链接里没有密码")

    out["server"] = url.hostname
    out["server_port"] = url.port or 443
    out["password"] = password

    net = (q.get("type") or "tcp").lower()
    host, path = q.get("host") or "", q.get("path") or ""
    if net == "ws":
        tr = {"type": "ws"}
        if path:
            tr["path"] = path
        if host:
            tr["headers"] = {"Host": host}
        out["transport"] = tr
    elif net == "grpc":
        out["transport"] = {"type": "grpc",
                            "service_name": q.get("serviceName") or ""}

    sni = q.get("sni") or q.get("peer") or ""
    tls = {"enabled": True}
    if sni:
        tls["server_name"] = sni
    a = _alpn(q.get("alpn"), fallback=["http/1.1"] if net == "ws" else None)
    if a:
        tls["alpn"] = a
    if _truthy(q.get("allowInsecure") or q.get("insecure")):
        tls["insecure"] = True
    fp = q.get("fp")
    if fp:
        tls["utls"] = {"enabled": True, "fingerprint": fp}
    out["tls"] = tls
    return out, warn


def _parse_ss(url):
    warn = []
    out = {"type": "shadowsocks", "tag": ""}

    # 两种写法，靠"有没有独立的主机:端口"区分：
    #   SIP002 : ss://base64(method:password)@host:port#tag
    #            ss://method:password@host:port#tag        （明文也允许）
    #   老式   : ss://base64(method:password@host:port)#tag  （整段编码，
    #            里面的 @ 是编码后的，所以 urlsplit 看不到主机端口）
    # 注意：urlsplit 在 netloc 里**没有 @** 时 username 返回 None，
    # 而且 hostname 会把整串 base64 转成小写（直接毁掉数据）——
    # 所以老式写法必须拿 netloc 原文，不能用 hostname。
    raw_user = url.username if url.username is not None else url.netloc
    userinfo = urllib.parse.unquote(raw_user or "")
    if url.hostname and url.port:
        if url.password is not None:
            # urlsplit 已经在第一个冒号处拆开了 → 明文 method:password
            method, password = userinfo, urllib.parse.unquote(url.password)
        else:
            # 用户名里没有可拆的冒号 → 期望是 base64(method:password)
            try:
                plain = _b64decode(userinfo)
            except (binascii.Error, ValueError):
                raise ValueError("ss 链接的 base64 解不开")
            if ":" not in plain:
                raise ValueError("ss 链接缺少 加密方式:密码")
            method, password = plain.split(":", 1)
    else:
        if not userinfo:
            raise ValueError("ss 链接格式无法识别")
        try:
            plain = _b64decode(userinfo)
        except (binascii.Error, ValueError):
            raise ValueError("ss 链接的 base64 解不开")
        if "@" not in plain:
            raise ValueError("ss 链接缺少 服务器:端口")
        cred, hostport = plain.rsplit("@", 1)
        if ":" not in cred:
            raise ValueError("ss 链接缺少 加密方式:密码")
        method, password = cred.split(":", 1)
        if ":" not in hostport:
            raise ValueError("ss 链接缺少端口")
        host, port = hostport.rsplit(":", 1)
        url = url._replace(netloc=hostport)      # 让下面的 hostname/port 生效

    if not url.hostname:
        raise ValueError("ss 链接里没有服务器地址")
    if not url.port:
        raise ValueError("ss 链接里没有端口")
    out["server"] = url.hostname
    out["server_port"] = int(url.port)
    out["method"] = method
    out["password"] = password

    q = _q(url)
    plugin = q.get("plugin")
    if plugin:
        # 只做最常见的两种；其余原样警告，让用户自己确认
        parts = plugin.split(";")
        name = parts[0]
        opts = dict(p.split("=", 1) for p in parts[1:] if "=" in p)
        if name == "obfs-local" or name == "simple-obfs":
            out["plugin"] = "obfs-local"
            out["plugin_opts"] = \
                f"obfs={opts.get('obfs','http')};obfs-host={opts.get('obfs-host','')}"
        elif name == "v2ray-plugin":
            out["plugin"] = "v2ray-plugin"
            out["plugin_opts"] = plugin[len(name) + 1:]
        else:
            warn.append(f"插件 {name} 未做映射，已忽略：sing-box 侧可能不支持")
    return out, warn


def _parse_vmess(url):
    warn = []
    body = (url.netloc or "") + (url.path or "")
    body = body.split("#")[0].strip("/")
    if not body:
        raise ValueError("vmess 链接是空的")
    try:
        cfg = json.loads(_b64decode(body))
    except Exception:
        raise ValueError("vmess 链接的 base64 解不开或不是合法 JSON")

    out = {"type": "vmess", "tag": ""}
    if not cfg.get("add"):
        raise ValueError("vmess 配置里没有 add（服务器地址）")
    out["server"] = cfg["add"]
    out["server_port"] = int(cfg.get("port") or 443)
    out["uuid"] = cfg.get("id") or ""
    if not out["uuid"]:
        raise ValueError("vmess 配置里没有 id（uuid）")
    out["security"] = cfg.get("scy") or cfg.get("security") or "auto"
    if str(cfg.get("aid", 0)) not in ("0", "", None):
        out["alter_id"] = int(cfg["aid"])

    net = (cfg.get("net") or "tcp").lower()
    host = cfg.get("host") or ""
    path = cfg.get("path") or ""
    if net == "ws":
        tr = {"type": "ws"}
        if path:
            tr["path"] = path
        if host:
            tr["headers"] = {"Host": host}
        out["transport"] = tr
    elif net == "grpc":
        out["transport"] = {"type": "grpc",
                            "service_name": cfg.get("path") or ""}
    elif net in ("http", "h2") or (cfg.get("type") or "") == "http":
        tr = {"type": "http"}
        if host:
            tr["host"] = [h for h in re.split(r"[,\s]+", host) if h]
        if path:
            tr["path"] = path
        out["transport"] = tr

    if (cfg.get("tls") or "").lower() in ("tls", "true", "1"):
        tls = {"enabled": True}
        if cfg.get("sni"):
            tls["server_name"] = cfg["sni"]
        elif host:
            tls["server_name"] = host
        a = _alpn(cfg.get("alpn"), fallback=["http/1.1"] if net == "ws" else None)
        if a:
            tls["alpn"] = a
        fp = cfg.get("fp")
        if fp:
            tls["utls"] = {"enabled": True, "fingerprint": fp}
        out["tls"] = tls
    return out, warn


# ----------------------------- 入口 -----------------------------

_SCHEMES = {
    "vless": _parse_vless,
    "hysteria2": _parse_hysteria2,
    "hy2": _parse_hysteria2,
    "trojan": _parse_trojan,
    "ss": _parse_ss,
    "vmess": _parse_vmess,
}


def parse(link, taken_tags=None):
    """分享链接 → (sing-box outbound dict, warnings list)。

    taken_tags 传当前已有的 tag 集合，用来避免重名。
    """
    link = (link or "").strip()
    if not link:
        raise ValueError("链接是空的")
    if "://" not in link:
        raise ValueError("不是分享链接：缺少 scheme://（比如 vless://）")

    scheme = link.split("://", 1)[0].lower()
    fn = _SCHEMES.get(scheme)
    if not fn:
        raise ValueError(
            f"暂不支持 {scheme}://（支持：" +
            "、".join(sorted(set(_SCHEMES))) + "）")

    url = urllib.parse.urlsplit(link)
    out, warn = fn(url)

    default_name = f"{out.get('type','node')}-{out.get('server','')}".strip("-")
    out["tag"] = _safe_tag(_name(url, default_name), taken_tags)
    return out, warn


def describe(obj):
    """给界面用的一行摘要。"""
    t = obj.get("type", "?")
    server = obj.get("server", "")
    port = obj.get("server_port", "")
    bits = [f"{server}:{port}"]
    tr = (obj.get("transport") or {}).get("type")
    if tr:
        bits.append(tr)
    tls = obj.get("tls") or {}
    if (tls.get("reality") or {}).get("enabled"):
        bits.append("reality")
    elif tls.get("enabled"):
        bits.append("tls")
    if obj.get("flow"):
        bits.append(obj["flow"])
    return f"{t} · " + " · ".join(bits)


def link_for(obj):
    """反向：把出站转回分享链接，方便复制备份。只做能无损还原的几种。"""
    t = obj.get("type")
    tag = urllib.parse.quote(obj.get("tag", ""), safe="")
    tls = obj.get("tls") or {}
    tr = obj.get("transport") or {}
    q = []

    if t == "vless":
        q.append(("encryption", "none"))
        if (tls.get("reality") or {}).get("enabled"):
            q.append(("security", "reality"))
            q.append(("pbk", tls["reality"].get("public_key", "")))
            if tls["reality"].get("short_id"):
                q.append(("sid", tls["reality"]["short_id"]))
        elif tls.get("enabled"):
            q.append(("security", "tls"))
        if tls.get("server_name"):
            q.append(("sni", tls["server_name"]))
        if (tls.get("utls") or {}).get("fingerprint"):
            q.append(("fp", tls["utls"]["fingerprint"]))
        if tls.get("insecure"):
            q.append(("allowInsecure", "true"))
        if tr.get("type") == "ws":
            q.append(("type", "ws"))
            q.append(("path", tr.get("path", "/")))
            h = (tr.get("headers") or {}).get("Host")
            if h:
                q.append(("host", h))
        elif tr.get("type") == "grpc":
            q.append(("type", "grpc"))
            q.append(("serviceName", tr.get("service_name", "")))
        else:
            q.append(("type", "tcp"))
        if obj.get("flow"):
            q.append(("flow", obj["flow"]))
        if obj.get("packet_encoding"):
            q.append(("packetEncoding", obj["packet_encoding"]))
        user = obj.get("uuid", "")
        scheme = "vless"

    elif t == "hysteria2":
        if tls.get("server_name"):
            q.append(("sni", tls["server_name"]))
        if tls.get("insecure"):
            q.append(("insecure", "1"))
        if (obj.get("obfs") or {}).get("password"):
            q.append(("obfs", obj["obfs"].get("type", "salamander")))
            q.append(("obfs-password", obj["obfs"]["password"]))
        user = obj.get("password", "")
        scheme = "hysteria2"

    elif t == "trojan":
        if tls.get("server_name"):
            q.append(("sni", tls["server_name"]))
        if tr.get("type") == "ws":
            q.append(("type", "ws"))
            q.append(("path", tr.get("path", "/")))
            h = (tr.get("headers") or {}).get("Host")
            if h:
                q.append(("host", h))
        user = obj.get("password", "")
        scheme = "trojan"

    else:
        return None

    qs = urllib.parse.urlencode(q)
    return (f"{scheme}://{urllib.parse.quote(user, safe='')}"
            f"@{obj.get('server')}:{obj.get('server_port')}?{qs}#{tag}")


if __name__ == "__main__":
    import sys
    for a in sys.argv[1:]:
        try:
            o, w = parse(a)
            print(json.dumps(o, ensure_ascii=False, indent=2))
            for x in w:
                print("[!]", x)
        except ValueError as e:
            print("[x]", e)
