#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
config_edit.py  ·  保留注释地读写 Sing-box/config.json
========================================================
`config.json` 里有一大堆手写注释（解释 WS 传输怎么映射、reality 为什么必须带
utls、flow 为什么不能加在 ws 上……）。这些注释是**资产**，绝不能被
`json.dump()` 一把梭洗掉。

所以这个模块**不做 JSON 整体序列化**，而是把文件当文本，用括号匹配定位结构，
做块级的增 / 改 / 删：

    · 定位顶层 "outbounds": [ ... ]
    · 把数组里的顶层元素切成"块"（每块含它前面的注释行）
    · 按 tag 找到对应块 → 替换 / 删除 / 在其前面插入
    · selector 的 outbounds 列表和 default 单独改

这样已有节点的注释原封不动，新加的节点会自动带上我们自己生成的注释。

写文件前会先存一份 `config.json.bak`（只保留最近一次）。
"""

import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SB_DIR = os.path.join(ROOT, "Sing-box")
CONFIG = os.path.join(SB_DIR, "config.json")
BACKUP = CONFIG + ".bak"

if SB_DIR not in sys.path:
    sys.path.insert(0, SB_DIR)
# 剥注释只保留一份实现，别抄第二遍
from start_singbox import strip_json_comments        # noqa: E402


class ConfigError(Exception):
    pass


# ----------------------------- 括号匹配 -----------------------------

def _match_bracket(text, i):
    """text[i] 必须是 [ 或 {，返回与之配对的闭合括号下标。
    会正确跳过字符串字面量和 // 、/* */ 注释。"""
    if text[i] not in "[{":
        raise ConfigError(f"位置 {i} 不是括号")
    open_c = text[i]
    close_c = "]" if open_c == "[" else "}"
    depth = 0
    in_str = False
    j = i
    n = len(text)
    while j < n:
        c = text[j]
        if in_str:
            if c == "\\":
                j += 2
                continue
            if c == '"':
                in_str = False
            j += 1
            continue
        if c == '"':
            in_str = True
            j += 1
            continue
        if c == "/" and j + 1 < n and text[j + 1] == "/":
            while j < n and text[j] != "\n":
                j += 1
            continue
        if c == "/" and j + 1 < n and text[j + 1] == "*":
            j += 2
            while j + 1 < n and not (text[j] == "*" and text[j + 1] == "/"):
                j += 1
            j += 2
            continue
        if c == open_c:
            depth += 1
        elif c == close_c:
            depth -= 1
            if depth == 0:
                return j
        j += 1
    raise ConfigError("括号不匹配，文件可能被改坏了")


def _array_span(text, key, within=None):
    """定位 "<key>": [ ... ] 的 [ 和 ] 下标。

    within=(start,end) 时只在该区间里找。同名的 key 可能出现在多层
    （顶层有 "outbounds"，selector 里也有 "outbounds"），所以取缩进最浅的那个。
    """
    lo = within[0] if within else 0
    hi = within[1] if within else len(text)
    pat = re.compile(r'"%s"\s*:\s*\[' % re.escape(key))
    best = None
    for m in pat.finditer(text, lo, hi):
        line_start = text.rfind("\n", lo, m.start()) + 1
        indent = len(text[line_start:m.start()]) - \
            len(text[line_start:m.start()].lstrip())
        if best is None or indent < best[0]:
            best = (indent, m.start())
    if best is None:
        return None
    open_i = text.index("[", best[1])
    return open_i, _match_bracket(text, open_i)


def _top_level_items(text, open_i, close_i):
    """把 [ ... ] 的顶层元素切成块，返回 [(start, end), ...]。
    start 含元素前面的注释/空行，end 含元素后面的逗号。"""
    items = []
    i = open_i + 1
    n = len(text)
    while i < close_i:
        start = i
        j = i
        # 跳过空白与注释，找到元素本体
        while j < close_i:
            c = text[j]
            if c in " \t\r\n":
                j += 1
            elif c == "/" and j + 1 < n and text[j + 1] == "/":
                while j < n and text[j] != "\n":
                    j += 1
            elif c == "/" and j + 1 < n and text[j + 1] == "*":
                j += 2
                while j + 1 < n and not (text[j] == "*" and text[j + 1] == "/"):
                    j += 1
                j += 2
            else:
                break
        if j >= close_i:
            break
        if text[j] not in "[{":
            raise ConfigError(f"出站数组里有非对象元素，位置 {j}")
        end = _match_bracket(text, j) + 1
        k = end
        while k < close_i and text[k] in " \t\r\n":
            k += 1
        if k < close_i and text[k] == ",":
            k += 1
        items.append((start, k))
        i = k
    return items


# ----------------------------- 读 -----------------------------

def load_raw():
    with open(CONFIG, "r", encoding="utf-8") as f:
        return f.read()


def _parse_item(block):
    """块文本 → dict；块里可能混着注释和前后逗号。"""
    body = block.strip()
    body = body.rstrip(",").strip()
    try:
        return json.loads(strip_json_comments(body))
    except Exception:
        return None


def outbounds(text=None):
    """列出所有出站：[{index, tag, type, span}]"""
    text = load_raw() if text is None else text
    span = _array_span(text, "outbounds")
    if not span:
        raise ConfigError('找不到顶层 "outbounds" 数组')
    out = []
    for idx, (a, b) in enumerate(_top_level_items(text, span[0], span[1])):
        obj = _parse_item(text[a:b])
        out.append({
            "index": idx,
            "start": a, "end": b,
            "obj": obj,
            "tag": (obj or {}).get("tag", ""),
            "type": (obj or {}).get("type", ""),
        })
    return out


def node_tags(text=None):
    """可作为节点的 tag（排除 selector / direct）"""
    skip = ("selector", "direct", "block", "dns")
    return [o["tag"] for o in outbounds(text)
            if o["tag"] and o["type"] not in skip]


def selector_span(text=None):
    """找到 selector 那个块，返回其字符区间。"""
    text = load_raw() if text is None else text
    for o in outbounds(text):
        if o["type"] == "selector":
            return o["start"], o["end"], o["obj"]
    return None


def summary(text=None):
    """给界面看的节点摘要列表。"""
    rows = []
    for o in outbounds(text):
        obj = o["obj"] or {}
        if o["type"] in ("selector", "direct") or not obj:
            continue
        rows.append({
            "tag": o["tag"],
            "type": obj.get("type", ""),
            "server": obj.get("server", ""),
            "port": obj.get("server_port", ""),
            "transport": (obj.get("transport") or {}).get("type", ""),
            "tls": ("reality" if (obj.get("tls") or {}).get("reality", {})
                    .get("enabled") else
                    ("tls" if (obj.get("tls") or {}).get("enabled") else "")),
            "flow": obj.get("flow", ""),
            "raw": obj,
        })
    return rows


# ----------------------------- 写 -----------------------------

def _dump_block(obj, indent="    ", comments=None):
    """按现有文件的风格生成一块。json.dumps(indent=2) 再整体右移 4 格，
    出来的就是文件里那种 4/6 空格缩进。"""
    body = _compact_string_arrays(
        json.dumps(obj, ensure_ascii=False, indent=2))
    lines = [indent + ln if ln.strip() else ln for ln in body.split("\n")]
    out = []
    for c in (comments or []):
        out.append(f"{indent}// {c}")
    out.append("\n".join(lines))
    return "\n".join(out)


# json.dumps(indent=2) 会把 ["a","b"] 这种字符串数组也拆成多行，
# 而原文件是写成一行的手写风格。这里把"全是字符串"的数组再压回一行，
# 只为了 diff 干净、不把整份文件重排。太长的保持多行（可读性优先）。
_STR_ARRAY = re.compile(r'\[((?:\s*"[^"\\]*"\s*,)*\s*"[^"\\]*"\s*)\]')


def _compact_string_arrays(s):
    def repl(m):
        items = re.findall(r'"[^"\\]*"', m.group(1))
        joined = ", ".join(items)
        return "[" + joined + "]" if len(joined) <= 72 else m.group(0)
    return _STR_ARRAY.sub(repl, s)


def _body_start(text, item):
    """item 里对象本体的起点（跳过前导空白与注释）。"""
    probe = item["start"]
    end = item["end"]
    while probe < end:
        c = text[probe]
        if c in " \t\r\n":
            probe += 1
        elif text[probe:probe + 2] == "//":
            while probe < end and text[probe] != "\n":
                probe += 1
        elif text[probe:probe + 2] == "/*":
            probe = text.index("*/", probe) + 2
        else:
            break
    return probe


def _insert_pos(text, items):
    """新节点插在哪 —— 插在第一个 selector/direct 之前，
    这样所有节点仍然聚在一起，选择器留在最后。"""
    for it in items:
        if it["type"] in ("selector", "direct", "block", "dns"):
            return it["start"]
    span = _array_span(text, "outbounds")
    pos = span[1]
    # 数组末尾：把前导空白吃掉，免得插入后多出空行
    while pos > 0 and text[pos - 1] in " \t\r\n":
        pos -= 1
    return pos


def _write(text):
    if os.path.exists(CONFIG):
        shutil.copy2(CONFIG, BACKUP)
    tmp = CONFIG + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.replace(tmp, CONFIG)


def add_outbound(obj, comments=None, text=None):
    """新增一个出站。tag 重复直接报错（调用方负责先去重）。"""
    text = load_raw() if text is None else text
    items = outbounds(text)
    tags = {i["tag"] for i in items}
    if obj.get("tag") in tags:
        raise ConfigError(f"节点 {obj['tag']} 已存在")

    pos = _insert_pos(text, items)
    block = _dump_block(obj, comments=comments)
    # 插入点紧跟在上一项的逗号后面，所以要自己补一个换行；
    # 末尾保留 text[pos:] 原有的换行，这样加完再删能还原成原样。
    new = text[:pos] + "\n" + block + "," + text[pos:]
    _write(new)
    return new


def update_outbound(tag, obj, comments=None, text=None):
    """按 tag 替换整个对象（前面的注释保留）。"""
    text = load_raw() if text is None else text
    for it in outbounds(text):
        if it["tag"] == tag:
            probe = _body_start(text, it)
            # 关键：从**行首**开始替换，行首到 probe 那段就是缩进。
            # 从 probe 开始替换的话，缩进会被"原地保留 + 新块再补一次"，
            # 结果每改一次缩进就翻一倍（踩过）。
            line_start = text.rfind("\n", 0, probe) + 1
            indent = text[line_start:probe]
            body_end = _match_bracket(text, probe) + 1
            new_block = _dump_block(obj, indent=indent, comments=comments)
            new = text[:line_start] + new_block + text[body_end:]
            _write(new)
            return new
    raise ConfigError(f"找不到节点 {tag}")


def delete_outbound(tag, text=None):
    """按 tag 删除（连同它前面的注释一起删）。"""
    text = load_raw() if text is None else text
    for it in outbounds(text):
        if it["tag"] == tag:
            if it["type"] in ("selector", "direct"):
                raise ConfigError("不能删除 selector/direct")
            # 块的区间是「上一项逗号之后」到「本项逗号之后」，
            # 整段删掉不会留下空行或多余逗号
            new = text[:it["start"]] + text[it["end"]:]
            _write(new)
            return new
    raise ConfigError(f"找不到节点 {tag}")


def set_selector(tags=None, default=None, text=None):
    """更新 selector 的 outbounds 列表 / 默认节点。"""
    text = load_raw() if text is None else text
    found = selector_span(text)
    if not found:
        raise ConfigError("找不到 selector 出站")
    s, e, obj = found
    new_obj = dict(obj)
    if tags is not None:
        new_obj["outbounds"] = list(tags)
    if default is not None:
        new_obj["default"] = default
    return update_outbound("node", new_obj, text=text)


def sync_selector_with_nodes(text=None, prefer=None):
    """把 selector 的列表同步成当前所有节点（去掉已删的，补上新加的）。"""
    text = load_raw() if text is None else text
    tags = node_tags(text)
    found = selector_span(text)
    if not found:
        return text
    cur = list(found[2].get("outbounds") or [])
    # 保留原顺序，剔掉不存在的，再把新节点追加进去
    merged = [t for t in cur if t in tags] + [t for t in tags if t not in cur]
    default = found[2].get("default")
    if default not in merged:
        default = prefer if prefer in merged else (merged[0] if merged else None)
    return set_selector(merged, default, text)


# ----------------------------- 自检 -----------------------------

if __name__ == "__main__":
    t = load_raw()
    print("=== 节点列表 ===")
    for r in summary(t):
        print("  %-18s %-12s %s:%s" % (r["tag"], r["type"], r["server"], r["port"]))
    print("=== selector ===")
    f = selector_span(t)
    print("  outbounds =", f[2].get("outbounds"))
    print("  default   =", f[2].get("default"))
    print("=== 括号自检 ===")
    span = _array_span(t, "outbounds")
    items = _top_level_items(t, span[0], span[1])
    print("  顶层元素 %d 个，切块成功" % len(items))
    for a, b in items:
        o = _parse_item(t[a:b])
        assert o is not None, "有块解析失败"
    print("  全部块可解析 OK")
    print("  strip_json_comments 可用:", len(strip_json_comments(t)) > 0)
