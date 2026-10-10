# game-tcp-speed 交接

> 本文件是给 AI 的工作交接文档，请先阅读此文档再继续对本仓库进行操作。

## 一、还没做完的事 & 动手前注意

### 尚未完成 / 待办
- [ ] **多进程同时加速支持**：当前 `launcher_gui.cpp` 已支持基于 PID 集合的多进程注入，但如果游戏存在 32 位和 64 位混合子进程，需确保分别注入 32/64 位版本的 DLL（目前主游戏 `WoW 11.2.5 - Firestorm.exe` 为纯 64 位）。
- [ ] **节点测速历史与丢包统计持久化**：目前测速在内存与日志框实时展示，可考虑将测速记录（RTT、丢包率、时间戳）持久化到本地 SQLite 或 JSON 文件，便于长时间挂机网络质量分析。
- [ ] **GUI 托盘最小化**：目前 `c/launcher_gui.exe` 拥有任务栏窗口，后续可引入 `Shell_NotifyIconW` 支持关闭或最小化至系统托盘，减少桌面占用。
- [ ] **订阅自动定时刷新**：目前主备订阅支持手动点击【拉取主订阅】/【拉取备订阅】，后续可扩展为每 12 小时后台自动检测 Cloudflare 临时隧道域名是否失效并静默拉取。

### 动手前注意事项
1. **严禁引入 TUN / TAP 虚拟网卡**：本项目的根本设计原则是“纯应用层 TCP 拦截与进程重定向”，绝对不能引入虚拟网卡或修改系统默认网关，以避免与用户机器现有的系统代理、Throne、VPN 或虚拟局域网产生冲突。
2. **编码规则严格分离**：
   - Windows 批处理（`.cmd` / `.bat`）必须保存为 **ANSI / GBK (CP936)** 编码，绝对不能加 `chcp 65001`，否则在中文 Windows CMD 控制台与系统工具（如 `sc.exe`）交互时必产生严重乱码。
   - C++ 源码、Python 脚本必须保存为 **UTF-8（无 BOM）** 编码。
   - GUI 调用 Python 子进程时，命令行**必须显式附带 `-X utf8`** 参数（例如 `python -X utf8 test_speed.py <port>`），且 C++ 管道接收端必须使用带 `MB_ERR_INVALID_CHARS` 的宽字符转换并备用降级到 `CP_ACP`。
3. **端口分配为启动期随机协商**：Sing-box 入站端口并非写死在某固定端口，而是每次启动时调用 `GetRandomAvailablePort()` 绑定 0 端口获取未占用高位端口（如 50000+）。写文件或调用测试脚本前，必须通过 `c/proxy_config.ini` 获取当前运行端口。
4. **DLL 必须支持动态端口热更新**：`hook_proxy.cpp` 已实现每秒自动检查 `proxy_config.ini` 修改。严禁在代码中写死代理端口，以确保核心重启或换端口时已注入的游戏无需重启。
5. **内核驱动代答假值防范**：本机如安装了 Proxifier 等软件，其后台驱动 `ProxifierDrv` 默认常驻内核并代答 SYN（造成本地 2~3ms 假握手）。测速脚本已采用端到端真实数据探测，若需要还原物理网络，需使用 `tools/set_proxifier_demand.cmd` 将驱动设为 `demand` 模式。
6. **Git 提交约束**：本项目全流程采用简体中文编写提交信息与文档，提交代码时务必注意排查编译产物（`bin/`、`c/build/`、`*.log`）是否被 `.gitignore` 正常忽略。

---

## 二、历史记录（我们做了什么、怎么做的）

- **2026-10-04（阶段 1：Python 原型与副 IP 劫持方案探索）**：
  - 设计并编写了 `py/firestorm_accel.py`，采用 Windows IP Alias（副 IP 绑定）+ 用户态 Socks5 转发的方式加速魔兽世界。
  - 完成了 `py/firestorm_tray.py`（无三方依赖的纯 Win32 原生托盘控制台）。
  - 解决了管理员提权、动态网卡索引检测以及退出时自动卸载副 IP 的资源回收机制。
  - 交付文件：`py/firestorm_accel.py`、`py/run_accel.cmd`、`py/stop_accel.cmd`。

- **2026-10-04（阶段 2：独立 Sing-box 运行架构搭建）**：
  - 为实现与机器上既有代理（Throne: 2080）彻底隔离，引入独立内核 `Sing-box/sing-box.exe`（v1.14.2）。
  - 编写了双实例（主实例 CF 隧道 + 备实例 直连 VPS）脚本与动态端口解析工具 `Sing-box/portof.py`。
  - 实测排查出原订阅节点 `qiniu-0709` 隧道失效（HTTP 530），将默认节点平滑切换至有效线路。
  - 交付文件：`Sing-box/config.json`、`Sing-box/start_singbox.py`、`Sing-box/portof.py`。

- **2026-10-05 01:00~02:15（阶段 3：C++ 底层 Winsock Hook 与原生 Win32 GUI 研发）**：
  - 为支持第三方登录器（FirestormLauncher），彻底摆脱副 IP 绑定的局限性，采用 MinHook 研发了用户态 Winsock 拦截 DLL：`c/dll/hook_proxy.cpp`。
  - 拦截 `connect` / `WSAConnect`，过滤 127.0.0.1 回环流量，将发往远端游戏服的 TCP 连接无感重定向至本地 Sing-box 并执行 Socks5 握手。
  - 研发纯 Win32 C++ 原生控制台 `c/gui/launcher_gui.cpp`，提供进程挂起启动注入、运行中进程即刻注入以及后台自动监控注入线程。
  - 编写自动化编译工具链：`c/CMakeLists.txt`、`c/build.ps1`，成功编译出 `c/hook_proxy.dll` 与 `c/launcher_gui.exe`。

- **2026-10-05 02:15~02:35（阶段 4：主备双订阅集成、纯净节点过滤与随机高位端口机制）**：
  - 应用户需求彻底精简节点列表，只保留用户给定的主订阅与备订阅两条专线。
  - 编写 `c/update_sub.py`，支持一键动态拉取主/备订阅并更新写入 `Sing-box/config.json`。
  - 实现系统可用端口动态探测分配算法（`GetRandomAvailablePort()`），彻底杜绝端口冲突。
  - 研发专线测速脚本 `c/test_speed.py`，通过本地 Socks5 隧道测试到达游戏服的端到端握手延迟。
  - 排查并解决了管道输出编码导致 GUI 日志框中文乱码的 BUG（注入 `-X utf8` 并在 C++ 增加 `MB_ERR_INVALID_CHARS` 容错解码）。

- **2026-10-05 02:35~03:05（阶段 5：深度排查直连 3ms 假象、集成 HY2 极速专线、DLL 端口热更新）**：
  - **破解 3ms 伪握手真相**：经网络诊断发现游戏服公网 IP `141.95.207.63` 位于欧洲法国 OVH 机房，物理上国内到欧洲不可能只要 3ms。深入排查发现是因为本地安装了 Proxifier，其底层过滤驱动 `ProxifierDrv.sys`（内核服务）处于常驻 RUNNING 状态并在本地秒回 SYN。
  - **排查 10061 连接被拒与时序 BUG**：发现 `hook_proxy.dll` 之前只在 `DllMain` 加载时读取一次 ini 配置，后来 GUI 分配随机端口后 DLL 未更新，导致发往旧端口 2081 报 10061。在 DLL 中新增 `RefreshConfigIfNeeded()`，实现运行期 1 秒内无缝动态同步最新端口！
  - **集成 Hysteria 2 节点**：将用户提供的独立 IP 高性能节点（`<专线节点IP>:42468`）作为【极速专线-HY2】集成进系统。实测握手延迟从 CF 隧道的 1040ms 直降至 439ms（提速 58%），且基于 UDP QUIC 抗丢包。
  - **升级真实测速**：修正游戏目标端口为真实世界服端口 `8285`，并采用真正的端到端应用层 RTT 测量（实测 HY2 端到端延迟稳定在 235~260ms）。

- **2026-10-05 03:05~03:25（阶段 6：Proxifier 驱动按需启动改造与批处理编码净化）**：
  - 在 `C:\Users\junjun\AppData\Roaming\Proxifier4\Profiles\Default.ppx` 中为魔兽世界和测速脚本注入最高优先级 `Direct` 直连放行规则。
  - 编写 `tools/set_proxifier_demand.cmd`，将 `ProxifierDrv` 驱动服务启动模式从 `auto` 调整为 `demand`（按需启动），平时不常驻代答伪握手，打开 Proxifier 时自动加载。
  - 将所有辅助批处理脚本编码重构为原生 ANSI/GBK，清除 Unicode 特殊符号，杜绝控制台乱码。

- **2026-10-05 17:15~17:25（阶段 7：实装 10s 实时对比监控功能）**：
  - 在 `c/test_speed.py` 中增加 `--loop [interval]` 周期采样模式，后台每 10 秒快速完成直连与专线 RTT 采样。
  - 在 Win32 原生控制台 `c/gui/launcher_gui.cpp` 中新增【⏱️ 对比监控(10s)】/【⏹ 停止监控】开关按钮与异步管道监听线程。
  - 实时将加速与未加速的对比数据（延迟、优化毫秒、提速百分比）打入日志框并自动滚动，退出时安全回收监控进程。

### 当前仓库文件结构树

```text
game-tcp-speed/
├── ai.md                             # AI 工作交接规范文档（本项目核心交接入口）
├── README.md                         # 归档级高颜值全量项目技术文档
├── .gitignore                        # Git 忽略配置
├── .gitattributes                    # Git 属性配置
│
├── c/                                # 【推荐主方案】C++ Winsock Hook + Win32 原生加速器
│   ├── CMakeLists.txt                # CMake 构建定义 (Ninja + MSVC/Clang)
│   ├── build.ps1                     # 一键编译脚本 (构建 hook_proxy.dll & launcher_gui.exe)
│   ├── build.bat                     # 批处理构建包装
│   ├── hook_proxy.dll                # 编译生成的 Winsock 拦截动态库 (支持端口热更新)
│   ├── launcher_gui.exe              # 编译生成的 Win32 原生控制台 GUI
│   ├── proxy_config.ini              # 运行时代理配置 (由 GUI 动态更新)
│   ├── proxy_hook.log                # DLL 运行时注入与拦截日志
│   ├── singbox.log                   # Sing-box 核心运行日志
│   ├── test_speed.py                 # 真实端到端网络测速工具 (纯净 UTF-8)
│   ├── update_sub.py                 # 订阅拉取解析与节点纯净更新工具
│   ├── README.md                     # C++ 组件架构时序与原理说明
│   ├── dll/
│   │   └── hook_proxy.cpp            # MinHook 挂钩 connect/WSAConnect 核心实现
│   ├── gui/
│   │   └── launcher_gui.cpp          # Win32 原生 GUI 控制台与进程注入器源码
│   └── minhook/                      # MinHook 轻量级 x86/x64 API Hook 开源库
│
├── py/                               # 【备用方案】Python 副 IP (IP Alias) + 用户态中继
│   ├── firestorm_accel.py            # 核心中继与副 IP 挂载脚本
│   ├── firestorm_tray.py             # 零依赖纯 Win32 托盘控制台
│   ├── firestorm_tray.cmd            # 托盘启动批处理
│   ├── run_accel.cmd                 # 走本地 Throne (2080) 启动
│   ├── run_accel_singbox.cmd         # 走独立 Sing-box 随机端口启动
│   ├── stop_accel.cmd                # 停止中继并安全摘除副 IP
│   ├── switch_upstream.cmd           # 上游节点切换工具
│   ├── proxifier-driver.cmd          # Proxifier 驱动辅助
│   └── README.md                     # Python 方案说明与原理
│
├── Sing-box/                         # 独立 Sing-box 核心与配置文件
│   ├── sing-box.exe                  # Sing-box v1.14.2 独立核心程序
│   ├── config.json                   # 核心配置 (HY2 极速专线 + 主备 CF 订阅专线)
│   ├── config.template.json          # 空白节点模板
│   ├── start_singbox.py              # 随机端口高位安全启动脚本
│   ├── stop_singbox.py               # 进程安全停止脚本
│   ├── portof.py                     # 运行时端口与状态查询脚本
│   └── switch-node.cmd               # 节点热切换脚本
│
├── tools/                            # 系统网络与底层驱动调试维护工具 (全部纯正 GBK)
│   ├── set_proxifier_demand.cmd      # 将 ProxifierDrv 设为按需启动 (demand) 并停止常驻
│   ├── disable_proxifier_driver.cmd  # 彻底停止、禁用并清理 ProxifierDrv 驱动
│   └── enable_proxifier_driver.cmd   # 重新注册并启动 ProxifierDrv 驱动
│
├── gui/                              # 【早期方案】基于 Tkinter 的 Python 辅助配置面板
│   ├── firestorm_gui.py              # Tkinter GUI 源码
│   ├── run-gui.cmd                   # 启动包装
│   ├── config_edit.py                # 带注释保留的 JSON 解析器
│   └── share_link.py                 # 分享链接转换工具
│
└── docs/                             # 技术设计与效果验证文档
    ├── 加速器效果验证.md             # 实测加速对比数据
    ├── 独立sing-box使用说明.md        # 独立核心架构设计原理
    └── img/
        ├── accel-arch.svg            # 双链路架构设计矢量图
        └── accel-latency.svg         # 延迟测试对比图表
```

---

## 三、为什么做这件事

### 目标与痛点
- **痛点 1（传统网卡级 VPN / TUN 冲突）**：市面上的主流网络加速器和代理工具（如 Clash TUN、WireGuard、各大商业游戏加速器）通常会创建虚拟网卡并强行修改 Windows 默认网关。这不仅极其笨重，而且会与开发环境（如 Docker、WSL2、Hyper-V）、公司内网 VPN、日常代理工具（如 Throne）产生严重的路由抢占与死锁冲突。
- **痛点 2（私服魔兽特定 IP 跨境丢包与阻断）**：目标场景魔兽世界私服（Firestorm 11.2.5 客户端）服务器位于欧洲法国（`141.95.207.63`）。国内家庭宽带直连跨国光缆出口时，经常遭遇剧烈抖动、高达 30% 丢包甚至因防火墙策略产生的 TCP RST 阻断，导致玩家读条卡死、登入秒断或大招跳 ping。
- **痛点 3（第三方专用登录器难以拦截）**：游戏使用官方及第三方定制启动器（`FirestormLauncher.exe`），若使用商业加速器的“启动游戏”往往无法正常唤起专属更新与校验机制。

### 带来的价值
1. **纯应用层 TCP 重定向，零驱动/零网卡冲突**：
   - 方案采用 **用户态 MinHook 挂钩 Winsock**（`connect`/`WSAConnect`），直接在游戏进程内部将目标流量透明导向本地随机端口。
   - 操作系统完全感知不到任何路由改动，不创建任何虚拟网卡，与其他代理软件 100% 和谐共存。
2. **专线级加速体验（Hysteria 2 + VLESS 双引擎）**：
   - 提供基于 UDP QUIC 的 **Hysteria 2 极速专线**（实测端到端仅 230~260ms，即使物理网络丢包 30% 也能靠主动拥塞控制跑满，杜绝团本卡顿跳 ping）。
   - 保留 **主/备 Cloudflare 穿透专线** 作为兜底冗余，支持从订阅链接一键更新。
3. **完美适配第三方登录器**：
   - 原生控制台提供【开启自动监控注入】后台线程，玩家可随意用 Firestorm 登录器更新、进游戏，程序在毫秒级自动挂载加速，体验极其平滑。
4. **彻底查清并解决 Proxifier 内核假握手**：
   - 深度排查了本地 `ProxifierDrv` 驱动代答产生的 2ms 虚假延迟，提供 `demand` 按需加载模式，恢复物理真实网络测量。

### 复用方式
- **代码高度解耦与通用化**：
  - 核心模块 [`c/dll/hook_proxy.cpp`](file:///E:/code/game-tcp-speed/c/dll/hook_proxy.cpp) 是通用的 Winsock 代理重定向 DLL，只需要修改 `proxy_config.ini`，即可直接复用于任何其他 TCP 游戏（如魔兽世界其他版本、最终幻想14、暗黑破坏神、Steam/外服网游）或需要针对特定 IP 走代理的业务进程。
  - 控制台 [`c/gui/launcher_gui.cpp`](file:///E:/code/game-tcp-speed/c/gui/launcher_gui.cpp) 提供了通用的 Win32 原生进程注入、随机端口调度与订阅管理架构，可作为任何进程级加速软件的参考脚手架。
