# -*- coding: utf-8 -*-
"""
游戏专线节点极速测速工具
通过本地 Sing-box (127.0.0.1:2081) 测试连接游戏服务器 (141.95.207.63:8085) 的实际往返延迟
"""
import sys
import time
import socket

def test_socks5_latency(proxy_host="127.0.0.1", proxy_port=2081, target_ip="141.95.207.63", target_port=8085, timeout=5.0):
    ip_bytes = [int(x) for x in target_ip.split(".")]
    times = []

    for i in range(3):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        try:
            t0 = time.perf_counter()
            s.connect((proxy_host, proxy_port))
            # 1. 认证协商
            s.sendall(b"\x05\x01\x00")
            auth_resp = s.recv(2)
            if auth_resp != b"\x05\x00":
                s.close()
                return False, "Socks5 认证协商失败"

            # 2. 发起 CONNECT 请求（计时核心：测量经过专线建立到游戏服的握手延迟）
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
                s.close()
                return False, f"代理远端连接失败 (REP=0x{rep:02X})"
            s.close()
        except socket.timeout:
            return False, "连接超时 (节点无响应)"
        except Exception as e:
            return False, str(e)
        time.sleep(0.1)

    if not times:
        return False, "测速失败"

    avg_ms = sum(times) / len(times)
    min_ms = min(times)
    return True, avg_ms

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 2081
    print(f"[*] 正在向游戏服 (141.95.207.63:8085) 发起 3 次往返测速...")
    ok, res = test_socks5_latency(proxy_port=port)
    if ok:
        print(f"[+] 测速成功！平均延迟: {res:.1f} ms")
        if res < 100:
            print("[+] 网络质量评级: 极佳 (适合团队副本与高难度战斗)")
        elif res < 200:
            print("[+] 网络质量评级: 良好 (游戏体验流畅)")
        else:
            print("[!] 网络质量评级: 一般")
        sys.exit(0)
    else:
        print(f"[!] 测速失败: {res}")
        print("[!] 建议检查 Sing-box 核心是否运行，或点击底部重新拉取订阅！")
        sys.exit(1)
