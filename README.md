# game-tcp-speed

针对特定进程的 TCP 游戏加速器。

## 架构说明
- `c/`：底层注入与 Winsock Hook DLL（劫持目标进程 TCP 流量重定向到本地 Socks5 隧道）。
- `py/`：管理端 / 启动器 / 配置生成与进程监控脚本。
