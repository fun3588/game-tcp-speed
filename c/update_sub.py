# -*- coding: utf-8 -*-
"""
纯净更新主/备专线节点到 config.json
"""
import sys
import io
import os
import json
import urllib.request
import ssl
import re

# 强制 UTF-8 输出
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8', errors='replace')

def update_node(sub_url, is_primary=True):
    node_tag = "主节点-0001 (新加坡)" if is_primary else "备节点-0709 (新加坡)"
    print(f"[*] 正在拉取【{'主' if is_primary else '备'}订阅】: {sub_url}")

    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE

    req = urllib.request.Request(sub_url, headers={"User-Agent": "sing-box"})
    try:
        with urllib.request.urlopen(req, context=ctx, timeout=15) as resp:
            text = resp.read().decode("utf-8", errors="ignore")
    except Exception as e:
        print(f"[!] 拉取失败: {e}")
        return False

    try:
        sub_json = json.loads(text)
    except Exception as e:
        print(f"[!] 解析 JSON 失败: {e}")
        return False

    outbounds = sub_json.get("outbounds", [])
    valid_proxies = [ob for ob in outbounds if ob.get("type") in ("vless", "hysteria2", "shadowsocks", "trojan", "vmess")]
    if not valid_proxies:
        print("[!] 订阅中未找到有效节点！")
        return False

    new_node = valid_proxies[0]
    new_node["tag"] = node_tag
    print(f"[+] 提取到最新节点: {new_node.get('server')}:{new_node.get('server_port')}")

    # 定位 config.json
    script_dir = os.path.dirname(os.path.abspath(__file__))
    cfg_path = os.path.join(script_dir, "..", "Sing-box", "config.json")
    if not os.path.exists(cfg_path):
        cfg_path = os.path.join(script_dir, "Sing-box", "config.json")

    with open(cfg_path, "r", encoding="utf-8") as f:
        content = f.read()

    clean_json = re.sub(r'//.*', '', content)
    clean_json = re.sub(r'/\*.*?\*/', '', clean_json, flags=re.DOTALL)
    config = json.loads(clean_json)

    # 仅保留主节点、备节点、selector 和 direct
    existing = config.get("outbounds", [])
    updated = False
    new_outbounds = []

    for ob in existing:
        if ob.get("tag") == node_tag:
            new_outbounds.append(new_node)
            updated = True
        elif ob.get("tag") in ("主节点-0001 (新加坡)", "备节点-0709 (新加坡)", "node", "direct"):
            new_outbounds.append(ob)

    if not updated:
        # 插入在 selector 之前
        insert_idx = 0
        for i, ob in enumerate(new_outbounds):
            if ob.get("type") in ("selector", "direct"):
                insert_idx = i
                break
        new_outbounds.insert(insert_idx, new_node)

    # 规范化 selector
    for ob in new_outbounds:
        if ob.get("type") == "selector":
            ob["outbounds"] = ["主节点-0001 (新加坡)", "备节点-0709 (新加坡)"]
            ob["default"] = node_tag

    config["outbounds"] = new_outbounds

    with open(cfg_path, "w", encoding="utf-8") as f:
        json.dump(config, f, indent=2, ensure_ascii=False)

    print(f"[√] 已纯净更新【{node_tag}】并设为默认加速节点！")
    return True

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python update_sub.py <url> [primary/backup]")
        sys.exit(1)
    url = sys.argv[1]
    is_pri = True
    if len(sys.argv) > 2 and sys.argv[2].lower() == "backup":
        is_pri = False
    ok = update_node(url, is_pri)
    sys.exit(0 if ok else 1)
