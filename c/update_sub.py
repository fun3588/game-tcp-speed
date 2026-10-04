# -*- coding: utf-8 -*-
"""
从订阅链接动态拉取最新的 Sing-box 节点并更新到 config.json
"""
import sys
import os
import json
import urllib.request
import ssl
import re

def update_from_subscription(sub_url, tag_prefix="sub", set_as_default=True):
    print(f"[*] 正在从订阅拉取最新节点: {sub_url}")
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE

    req = urllib.request.Request(
        sub_url,
        headers={"User-Agent": "sing-box"}
    )

    try:
        with urllib.request.urlopen(req, context=ctx, timeout=15) as resp:
            data_bytes = resp.read()
            text = data_bytes.decode("utf-8", errors="ignore")
    except Exception as e:
        print(f"[!] 拉取订阅失败: {e}")
        return False

    try:
        sub_json = json.loads(text)
    except Exception as e:
        print(f"[!] 解析订阅 JSON 失败: {e}")
        return False

    new_outbounds = sub_json.get("outbounds", [])
    # 过滤掉 direct 等基础出口，只保留真实代理节点
    proxy_nodes = [ob for ob in new_outbounds if ob.get("type") in ("vless", "hysteria2", "shadowsocks", "trojan", "vmess")]
    if not proxy_nodes:
        print("[!] 订阅中未发现有效代理节点！")
        return False

    print(f"[+] 成功解析到 {len(proxy_nodes)} 个代理节点:")
    for node in proxy_nodes:
        print(f"    - [{node.get('type')}] {node.get('tag')} -> {node.get('server')}:{node.get('server_port')}")

    # 读取现有的 config.json
    script_dir = os.path.dirname(os.path.abspath(__file__))
    cfg_path = os.path.join(script_dir, "..", "Sing-box", "config.json")
    if not os.path.exists(cfg_path):
        cfg_path = os.path.join(script_dir, "Sing-box", "config.json")

    if not os.path.exists(cfg_path):
        print(f"[!] 未找到 config.json: {cfg_path}")
        return False

    with open(cfg_path, "r", encoding="utf-8") as f:
        content = f.read()

    # 移除 json 中的注释以便标准解析（保留模板结构）
    clean_json = re.sub(r'//.*', '', content)
    clean_json = re.sub(r'/\*.*?\*/', '', clean_json, flags=re.DOTALL)
    config = json.loads(clean_json)

    existing_outbounds = config.get("outbounds", [])
    
    # 替换或合并节点
    first_new_tag = None
    for new_node in proxy_nodes:
        tag = new_node.get("tag")
        if not first_new_tag:
            first_new_tag = tag
        
        # 查找是否存在同名 tag
        found = False
        for i, ex in enumerate(existing_outbounds):
            if ex.get("tag") == tag or (ex.get("type") == new_node.get("type") and tag_prefix in ex.get("tag", "")):
                existing_outbounds[i] = new_node
                found = True
                break
        if not found:
            # 插入在 selector 或 direct 之前
            insert_pos = 0
            for i, ex in enumerate(existing_outbounds):
                if ex.get("type") in ("selector", "direct"):
                    insert_pos = i
                    break
            existing_outbounds.insert(insert_pos, new_node)

    # 更新 selector 中的 outbounds 列表
    for ob in existing_outbounds:
        if ob.get("type") == "selector":
            sel_outbounds = ob.get("outbounds", [])
            for new_node in proxy_nodes:
                t = new_node.get("tag")
                if t not in sel_outbounds:
                    sel_outbounds.insert(0, t)
            if set_as_default and first_new_tag:
                ob["default"] = first_new_tag
                print(f"[+] 已将默认加速节点设为: {first_new_tag}")

    config["outbounds"] = existing_outbounds

    # 写回 config.json
    with open(cfg_path, "w", encoding="utf-8") as f:
        json.dump(config, f, indent=2, ensure_ascii=False)

    print(f"[√] 节点已成功更新并写入: {cfg_path}")
    return True

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python update_sub.py <sub_url> [tag_prefix]")
        sys.exit(1)
    url = sys.argv[1]
    prefix = sys.argv[2] if len(sys.argv) > 2 else "sub"
    success = update_from_subscription(url, prefix)
    sys.exit(0 if success else 1)
