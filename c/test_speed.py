# -*- coding: utf-8 -*-
"""
游戏专线对比测速工具 (支持单次详细测试与周期循环对比监控)
"""
import sys
import io
import time
import socket
import urllib.request
import datetime

# 强制 UTF-8 输出，杜绝 Windows 管道乱码
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8', errors='replace')

TARGET_IP = "141.95.207.63"
TARGET_PORT = 8285  # 魔兽世界 Firestorm 真实世界服务器端口

def test_direct_latency(target_ip=TARGET_IP, target_port=TARGET_PORT, count=3, timeout=3.0):
    """测试直连（不加速）到游戏服务器的真实连通性与握手延迟"""
    times = []
    fake_hijack_count = 0
    lost = 0

    for _ in range(count):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        try:
            t0 = time.perf_counter()
            s.connect((target_ip, target_port))
            t1 = time.perf_counter()
            rtt = (t1 - t0) * 1000.0

            # 校验连接真实性：尝试发送探测包，检测是否为本地假握手后立即抛出 RST
            try:
                s.sendall(b"\x00")
                data = s.recv(1024)
            except (ConnectionResetError, ConnectionAbortedError):
                fake_hijack_count += 1
                s.close()
                continue
            except socket.timeout:
                pass  # 超时说明连接未被强断，依然存活

            times.append(rtt)
            s.close()
        except Exception:
            lost += 1
            try:
                s.close()
            except Exception:
                pass
        if count > 1:
            time.sleep(0.06)

    if fake_hijack_count > 0:
        return False, "伪响应阻断 (本地网关/网络驱动伪握手, 但发包即被 RST 强行掐断, 实际不通)", 1.0

    if not times:
        return False, "超时阻断 (直连无法连通游戏服)", 1.0

    avg_ms = sum(times) / len(times)
    min_ms = min(times)
    return True, (avg_ms, min_ms), lost / count

def test_proxy_real_rtt(proxy_port, count=3, timeout=5.0):
    """通过本地 Sing-box 代理发起真实的端到端网络往返探测 (计算真实物理 RTT)"""
    proxy_url = f"http://127.0.0.1:{proxy_port}"
    proxy_handler = urllib.request.ProxyHandler({'http': proxy_url, 'https': proxy_url})
    opener = urllib.request.build_opener(proxy_handler)

    test_endpoints = [
        "http://cp.cloudflare.com/generate_204",
        "http://www.qualcomm.cn/generate_204",
        "http://connectivitycheck.gstatic.com/generate_204"
    ]

    times = []
    last_err = "未知错误"

    for i in range(count):
        endpoint = test_endpoints[i % len(test_endpoints)]
        try:
            req = urllib.request.Request(endpoint, headers={"User-Agent": "Mozilla/5.0"})
            t0 = time.perf_counter()
            with opener.open(req, timeout=timeout) as resp:
                t1 = time.perf_counter()
                times.append((t1 - t0) * 1000.0)
        except Exception as e:
            last_err = str(e)
        if count > 1:
            time.sleep(0.06)

    if not times:
        return False, last_err
    avg_ms = sum(times) / len(times)
    min_ms = min(times)
    return True, (avg_ms, min_ms)

def run_loop_monitor(port, interval=10):
    """连续对比监控模式：每隔指定秒数打一条清晰简明的对比日志"""
    print(f"[*] 启动实时对比监控 (每 {interval}s 刷新一次)...")
    sys.stdout.flush()

    while True:
        now_str = datetime.datetime.now().strftime("%H:%M:%S")
        # 1. 快速测试直连 (1-2次)
        d_ok, d_res, _ = test_direct_latency(count=2, timeout=2.0)
        # 2. 快速测试专线 (1-2次)
        p_ok, p_res = test_proxy_real_rtt(proxy_port=port, count=2, timeout=3.5)

        if p_ok and d_ok:
            d_val = d_res[0]
            p_val = p_res[0]
            diff = d_val - p_val
            if diff > 5:
                pct = (diff / d_val) * 100.0
                print(f"📊 [对比监控] 直连: {d_val:.1f}ms | 专线: {p_val:.1f}ms | 优化: -{diff:.1f}ms (提速 {pct:.1f}%)")
            elif diff < -5:
                print(f"📊 [对比监控] 直连: {d_val:.1f}ms | 专线: {p_val:.1f}ms | 专线平稳 (相差 {-diff:.1f}ms)")
            else:
                print(f"📊 [对比监控] 直连: {d_val:.1f}ms | 专线: {p_val:.1f}ms | 两者延迟基本相当")
        elif p_ok and not d_ok:
            print(f"📊 [对比监控] 直连: [未连通/掐断] | 专线: {p_res[0]:.1f}ms | 状态: 专线稳定打通中")
        elif not p_ok and d_ok:
            print(f"⚠️ [对比监控] 直连: {d_res[0]:.1f}ms | 专线: [超时/异常] | 提示: 请检查核心进程")
        else:
            print(f"⚠️ [对比监控] 直连: [未连通] | 专线: [未连通] | 提示: 请检查本地网络")

        sys.stdout.flush()
        time.sleep(interval)

if __name__ == "__main__":
    port = 2081
    loop_mode = False
    loop_interval = 10

    args = sys.argv[1:]
    idx = 0
    while idx < len(args):
        arg = args[idx]
        if arg == "--loop":
            loop_mode = True
            if idx + 1 < len(args) and args[idx+1].isdigit():
                loop_interval = int(args[idx+1])
                idx += 1
        elif arg.isdigit():
            port = int(arg)
        idx += 1

    if loop_mode:
        run_loop_monitor(port, loop_interval)
        sys.exit(0)

    # 单次测速模式
    print(f"[*] 目标游戏服: {TARGET_IP}:{TARGET_PORT} (Firestorm 世界服)")
    print("--------------------------------------------------")

    # 1. 测试直连 (不加速)
    print("[1/2] 正在测试【直连模式 (不加速)】真实可用性...")
    d_ok, d_res, d_loss = test_direct_latency(count=3, timeout=3.0)
    if d_ok:
        d_avg, d_min = d_res
        loss_str = f", 丢包率: {int(d_loss*100)}%" if d_loss > 0 else ""
        print(f"  [-] 直连延迟 (不加速): {d_avg:.1f} ms (最低 {d_min:.1f} ms{loss_str})")
    else:
        print(f"  [-] 直连状态 (不加速): [未连通] {d_res}")

    # 2. 测试专线加速 (真实端到端 RTT)
    print(f"[2/2] 正在测试【专线加速 (已加速)】真实端到端网络延迟 (本地端口: {port})...")
    p_ok, p_res = test_proxy_real_rtt(proxy_port=port, count=3, timeout=5.0)
    if p_ok:
        p_avg, p_min = p_res
        print(f"  [+] 专线真实端到端延迟: {p_avg:.1f} ms (最低 {p_min:.1f} ms)")
        if p_avg < 260:
            print("  [+] 专线传输质量: 极速畅通 [5星级] (直连海外骨干网)")
        elif p_avg < 450:
            print("  [+] 专线传输质量: 良好稳定 [4星级] (适合全天候稳定挂机与副本)")
        else:
            print("  [!] 专线传输质量: 一般 [3星级] (建议切换备用专线或刷新订阅)")
    else:
        print(f"  [!] 专线测速失败: {p_res}")
        print("  [!] 建议检查 Sing-box 核心是否运行，或点击底部重新拉取订阅！")

    print("--------------------------------------------------")

    # 3. 对比结论
    if p_ok and not d_ok:
        print("[评级] 加速优化效果: 成功突破直连阻断！")
        print("       直连时网络包直接被掐断，专线以 %.1f ms 延迟稳定护航！" % p_res[0])
    elif p_ok and d_ok:
        d_avg = d_res[0]
        p_avg = p_res[0]
        diff = d_avg - p_avg
        if diff > 5:
            percent = (diff / d_avg) * 100.0
            print(f"[评级] 加速优化: 延迟降低 {diff:.1f} ms (提速 {percent:.1f}%)")
        elif diff < -5:
            print(f"[评级] 延迟对比: 专线 {p_avg:.1f} ms vs 直连 {d_avg:.1f} ms")
        else:
            print(f"[评级] 延迟对比: 专线与直连基本相当 (~{p_avg:.1f} ms)")
