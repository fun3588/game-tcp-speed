# -*- coding: utf-8 -*-
"""
游戏专线对比测速工具
同时测量【直连 (不加速)】与【专线 (加速)】到游戏服 (141.95.207.63:8085) 的连接延迟与质量对比
"""
import sys
import io
import time
import socket

# 强制 UTF-8 输出，杜绝 Windows 管道乱码
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8', errors='replace')

TARGET_IP = "141.95.207.63"
TARGET_PORT = 8085

def test_direct_latency(target_ip=TARGET_IP, target_port=TARGET_PORT, count=3, timeout=3.0):
    """测试直连（不加速）到游戏服的 TCP 延迟"""
    times = []
    lost = 0
    for _ in range(count):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        try:
            t0 = time.perf_counter()
            s.connect((target_ip, target_port))
            t1 = time.perf_counter()
            times.append((t1 - t0) * 1000.0)
            s.close()
        except Exception:
            lost += 1
            try:
                s.close()
            except Exception:
                pass
        time.sleep(0.08)

    if not times:
        return False, None, lost / count
    avg_ms = sum(times) / len(times)
    min_ms = min(times)
    return True, (avg_ms, min_ms), lost / count

def test_socks5_latency(proxy_host="127.0.0.1", proxy_port=2081, target_ip=TARGET_IP, target_port=TARGET_PORT, count=3, timeout=5.0):
    """测试通过专线（已加速）到游戏服的握手往返延迟"""
    ip_bytes = [int(x) for x in target_ip.split(".")]
    times = []
    last_err = "未知错误"

    for _ in range(count):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        try:
            t0 = time.perf_counter()
            s.connect((proxy_host, proxy_port))
            # 1. Socks5 认证协商
            s.sendall(b"\x05\x01\x00")
            auth_resp = s.recv(2)
            if auth_resp != b"\x05\x00":
                s.close()
                last_err = "Socks5 认证协商失败"
                continue

            # 2. Socks5 CONNECT 请求（测量专线建立至游戏服耗时）
            t_req = time.perf_counter()
            req = b"\x05\x01\x00\x01" + bytes(ip_bytes) + target_port.to_bytes(2, "big")
            s.sendall(req)
            conn_resp = s.recv(10)
            t_resp = time.perf_counter()

            if len(conn_resp) >= 2 and conn_resp[1] == 0:
                latency_ms = (t_resp - t_req) * 1000.0
                times.append(latency_ms)
            else:
                rep = conn_resp[1] if len(conn_resp) >= 2 else 0xFF
                last_err = f"远端连接失败 (REP=0x{rep:02X})"
            s.close()
        except socket.timeout:
            last_err = "连接超时 (节点未响应)"
            try:
                s.close()
            except Exception:
                pass
        except Exception as e:
            last_err = str(e)
            try:
                s.close()
            except Exception:
                pass
        time.sleep(0.08)

    if not times:
        return False, last_err, None
    avg_ms = sum(times) / len(times)
    min_ms = min(times)
    return True, (avg_ms, min_ms), None

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 2081

    print(f"[*] 目标游戏服务器: {TARGET_IP}:{TARGET_PORT}")
    print("--------------------------------------------------")

    # 1. 测试不加速（直连）
    print("[1/2] 正在测试【直连模式 (不加速)】...")
    d_ok, d_res, d_loss = test_direct_latency(count=3, timeout=3.0)
    if d_ok:
        d_avg, d_min = d_res
        loss_str = f", 丢包率: {int(d_loss*100)}%" if d_loss > 0 else ""
        print(f"  [-] 直连延迟 (不加速): {d_avg:.1f} ms (最低 {d_min:.1f} ms{loss_str})")
    else:
        print(f"  [-] 直连延迟 (不加速): 超时/无法直连 (丢包率 100%)")

    # 2. 测试专线加速
    print("[2/2] 正在测试【专线模式 (已加速)】(端口: %d)..." % port)
    p_ok, p_res, _ = test_socks5_latency(proxy_port=port, count=3, timeout=5.0)
    if p_ok:
        p_avg, p_min = p_res
        print(f"  [+] 专线延迟 (已加速): {p_avg:.1f} ms (最低 {p_min:.1f} ms)")
    else:
        print(f"  [!] 专线测速失败: {p_res}")
        print("  [!] 提示: 请确认 Sing-box 核心已启动，或点击底部重新拉取订阅！")

    print("--------------------------------------------------")

    # 3. 对比与评级总结
    if p_ok and d_ok:
        d_avg = d_res[0]
        p_avg = p_res[0]
        diff = d_avg - p_avg
        if diff > 5:
            percent = (diff / d_avg) * 100.0
            print(f"[★] 加速优化: 延迟降低 {diff:.1f} ms (游戏提速 {percent:.1f}%)")
        elif diff < -5:
            print(f"[★] 延迟对比: 专线 {p_avg:.1f} ms vs 直连 {d_avg:.1f} ms")
        else:
            print(f"[★] 延迟对比: 专线与直连延迟基本相当 (~{p_avg:.1f} ms)")

        if p_avg < 100:
            print("[★] 专线质量: 极佳 ★★★★★ (非常适合团本与高难秘境)")
        elif p_avg < 200:
            print("[★] 专线质量: 良好 ★★★★☆ (游戏体验顺畅稳定)")
        else:
            print("[★] 专线质量: 一般 ★★★☆☆ (跨国节点稳定护航)")
    elif p_ok and not d_ok:
        print("[★] 加速优化: 突破直连阻断！专线成功连通游戏服 (延迟: %.1f ms)" % p_res[0])
        print("[★] 专线质量: 专线有效救活游戏连接！")
    elif not p_ok:
        sys.exit(1)
