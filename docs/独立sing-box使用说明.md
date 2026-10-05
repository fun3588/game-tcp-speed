# 独立 Sing-box 使用说明

> 结论：**可以，而且已经跑通验证。** 一份配置、一个 exe、**两个可同时运行的实例**，跟 Throne 完全不共用。

## 0. 当前两个实例（一主一备）

`start_singbox.py --profile <名字>` 可以起多个互不干扰的实例，各自随机端口：

| profile | 角色 | 默认节点 | 状态文件 | 日志 |
|---|---|---|---|---|
| `main` | **主实例** | `qiniu-0001`（Cloudflare 隧道） | `runtime.json` | `singbox.log` |
| `backup` | **备实例** | `hy2-va1`（直连 VPS，最稳） | `runtime.backup.json` | `singbox.backup.log` |

两个实例的 selector **都包含全部 4 个节点**，只是默认选中的不同：

- 主实例顺序：`qiniu-0001` → `qiniu-0709` → `hy2-va1` → `vl-reality-va1`
- 备实例顺序：`hy2-va1` → `vl-reality-va1` → `qiniu-0001` → `qiniu-0709`

所以实例内可以**热切换**（不断线），跨实例则要改加速器上游（会断一次，游戏一般自动重连）。

> **当前实况**（写文档时那次启动，重启会变，以 `portof.py list` 为准）：
>
> ```
> profile  入口    控制    节点         PID     存活
> main     55932   62462   qiniu-0001   41580   是
> backup   54619   58472   hy2-va1      41812   是
> ```
>
> 端口全部落在 `50000–65535` 的随机高位段，且都避开了系统预留块。

### 一键操作

**图形面板（推荐，配置 + 运行都在里面）**

```
gui\run-gui.cmd                   双击 → 自动提权 → 弹出窗口
```

面板里可以直接启停实例、下拉热切换节点、点按钮测延迟（走 clash_api，**不断线**），
还能**粘贴分享链接加节点** —— 改的是 `config.json`，手写注释会保留。
详见 `gui/README.md`。

> 面板需要一个**带 tkinter 的** Python —— tkinter 是编译期模块、没有 pip 包、
> 装不回来，项目自带的托管 Python 就没有。启动器会自动去挑一个带 tkinter 的，
> 挑不到会明确提示。

**命令行 / 批处理**

```
Sing-box\run-singbox-all.cmd       两个实例都起来（各开一个最小化窗口）
Sing-box\run-singbox.cmd           只起主实例
Sing-box\run-singbox.cmd backup    只起备实例
Sing-box\stop-singbox.cmd all      两个都停
Sing-box\stop-singbox.cmd backup   只停备实例
python Sing-box\portof.py list     看两个实例的端口/节点/PID/存活
```

### ⚠️ 七牛 0709 的隧道当前是死的

`controller-julie-alert-shakespeare.trycloudflare.com` 直连返回 **HTTP 530**
（Cloudflare 的"隧道未连接"错误，说明服务端 cloudflared 掉线了），
sing-box 报 `unexpected HTTP response status: 530`。

所以主实例的默认节点设成了 `qiniu-0001`（那个是通的）。
0709 仍留在 selector 里，隧道恢复后 `switch-node.cmd 0709` 一键切回。

> trycloudflare 的免费 quick tunnel 是**临时**的，域名会随服务端重启而变。
> 看到 530 基本就是服务端没在跑，不是你这边的配置问题。

### 实测性能（12 轮，取 p50）

| 实例 | 节点 | 完整 HTTPS 请求 p50 | 出口 IP |
|---|---|---|---|
| 主实例 | `qiniu-0001`（CF 隧道） | **1849 ms** | 154.41.93.160 |
| 备实例 | `hy2-va1`（直连） | **686 ms** | 66.154.126.18 |

**Cloudflare 隧道比直连慢约 2.7 倍** —— 多了一跳 CF 边缘 + 隧道回源。
隧道节点的价值不在低延迟，而在**换了一条独立链路**（不占直连 VPS 的账号流量、
且更难被针对性封锁）。要低延迟就打游戏走备实例。

## 1. 它和 Throne 什么关系

| | Throne | 独立 sing-box |
|---|---|---|
| 程序 | `Throne.exe` + `ThroneCore.exe` | `Sing-box\sing-box.exe` (v1.14.2) |
| 配置存放 | SQLite（`throne.db`） | 明文 `Sing-box\config.json` |
| 本地端口 | `127.0.0.1:2080`（固定） | 两个实例，各两个**随机**端口 |
| 控制接口 | 有（GUI） | clash_api（端口随机） |
| 上游节点 | `hy2-hy2.va1.kkip.vip` | 4 个可选（2 CF 隧道 + 2 直连） |

**不共用的含义**：进程各自独立。Throne 退出、切节点、开 TUN、
甚至卸载，都不影响独立实例；反过来也一样。加速器可以任选其中一个当出口。

⚠️ 唯一的耦合是**账号流量额度**：走直连节点（`hy2-va1` / `vl-reality-va1`）时
和 Throne 连的是同一台服务器（`66.154.126.18`）的同一个账号，会一起消耗套餐流量。
**走两个 Cloudflare 隧道节点则不占这份额度**。

## 2. 节点参数从哪来的

不是猜的，是从 Throne 自己的数据库里读出来的：

```
%LOCALAPPDATA%\Throne\config\throne.db  → 表 profiles → 列 outbound_json
```

当前使用的节点（原样抄进 config.json）：

```json
{
  "type": "hysteria2",
  "tag": "hy2-va1",
  "server": "66.154.126.18",
  "server_port": 42468,
  "password": "95012e97-e174-4ecf-8a88-a9c6dad2b513",
  "tls": { "enabled": true, "alpn": ["h3"], "insecure": true,
           "server_name": "www.bing.com" }
}
```

> ⚠️ 这些是**凭据**，别把 `config.json` 传到公开仓库。
> `config.runtime*.json` 同理会生成一份含凭据的副本，都已进 `.gitignore`。

同一台服务器 `66.154.126.18` 上还开了几个别的协议，可以互换（都在 config.json 里备用）：

| 协议 | 端口 | 说明 |
|---|---|---|
| hysteria2 | 42468 | UDP/QUIC 封装，自带前向纠错。**实测最稳，用这个** |
| vless + reality | 30752 | 纯 TCP。注意是 TCP over TCP，跨境丢包时两层重传叠加，实测抖动翻倍 |
| tuic v5 | 15107 | UDP/QUIC |
| anytls | — | 另一台机器（203.88.120.102） |

### 2.1 Cloudflare 隧道节点（WebSocket over TLS）

另外两个节点不是自己搭的服务器，而是 `cloudflared` quick tunnel 转发出来的：

| tag | 域名 | path | uuid | TLS |
|---|---|---|---|---|
| `qiniu-0709` | `controller-julie-alert-shakespeare.trycloudflare.com` | `/vless` | `c993eed9-…-667eec18e30c` | 校验证书 |
| `qiniu-0001` | `seekers-extraordinary-suppliers-goals.trycloudflare.com` | `/mywspath` | `d3b07384-…-2831f237f37a` | `insecure` |

sing-box 里对应写法（关键三点）：

```json
{
  "type": "vless",
  "tag": "qiniu-0001",
  "server": "seekers-extraordinary-suppliers-goals.trycloudflare.com",
  "server_port": 443,
  "uuid": "d3b07384-d113-42a6-a51f-2831f237f37a",
  "tls": {
    "enabled": true,
    "server_name": "seekers-extraordinary-suppliers-goals.trycloudflare.com",
    "alpn": ["http/1.1"],          // ← WS 必须走 h1，别给 h2
    "insecure": true,              // ← 原链接带 allowInsecure=true
    "utls": { "enabled": true, "fingerprint": "chrome" }
  },
  "transport": {                   // ← WS 传输层
    "type": "ws",
    "path": "/mywspath",
    "headers": { "Host": "seekers-extraordinary-suppliers-goals.trycloudflare.com" }
  }
}
```

- **不能写 `flow`**：`xtls-rprx-vision` 只适用于裸 TCP/REALITY，配 WS 会起不来。
- 原链接里的 `packetEncoding=xudp` 对应 `"packet_encoding": "xudp"`（sing-box 接受）。
- `allowInsecure=true` 映射成 `"insecure": true`。

### 2.2 别手写 JSON —— 用面板粘分享链接

上面那些映射规则，手写一次就够累了，再加个节点更累。所以做了个解析器：
**粘贴分享链接，自动生成正确的出站 JSON**。

```
gui\run-gui.cmd      →  「+ 添加节点」→ 粘贴链接 → 「解析链接」→ 「保存」
```

支持的 scheme：`vless://`、`hysteria2://` / `hy2://`、`trojan://`、`ss://`、`vmess://`。

解析时会**主动纠正链接里的坑**，并且把纠正内容显示给你看，比如：

| 链接里写的 | 面板的处理 |
|---|---|
| `type=ws` 却带 `flow=xtls-rprx-vision` | **丢掉 flow** 并警告（ws 上加 flow 会起不来） |
| `type=ws` 却写 `alpn=h2` | 强制改成 `http/1.1` 并警告 |
| `security=reality` 但没写 `fp` | 补 `fp=chrome`（reality 必须带 utls，否则报 `uTLS is required by reality client`） |
| `type=ws` 但 `host` 和 `sni` 只有后者 | 用 sni 补 `transport.headers.Host` |

保存时改的是 `Sing-box/config.json`，**手写注释原样保留**
（编辑器是按文本块改的，不是 `json.dump` 整体重写），改前会备份成 `config.json.bak`。

命名的 tag 取链接 `#` 后面的名字；重名会自动加 `-2`、`-3`。

## 3. 怎么跑

```
Sing-box\run-singbox-all.cmd        两个实例都起（一主一备）
Sing-box\run-singbox.cmd            只起主实例
Sing-box\run-singbox.cmd backup     只起备实例
Sing-box\stop-singbox.cmd           停主实例
Sing-box\stop-singbox.cmd backup    停备实例
Sing-box\stop-singbox.cmd all       两个都停
Sing-box\switch-node.cmd 0001       主实例切到七牛0001
Sing-box\switch-node.cmd hy2 backup 备实例切到 hysteria2
```

启动后会监听两个**随机**端口（见 3.1）。查当前值：

```
python Sing-box\portof.py list
```

### 3.1 端口是随机的

启动时**不写死端口**：`start_singbox.py` 每次都在 `50000–65535` 里挑两个
当前空闲的高位端口（**并避开另一个实例已占的**），分别给 SOCKS5/HTTP 入口和
control API，然后把结果写进对应的 runtime 文件：

```json
{ "profile": "main", "title": "主实例", "node": "qiniu-0001",
  "socks_port": 55932, "clash_port": 62462, "pid": 41580,
  "started_at": "2026-10-05 01:06:36", "config": "config.runtime.json" }
```

于是文件分成几份，各司其职：

| 文件 | 谁写 | 用途 |
|---|---|---|
| `config.json` | 人 | 带注释的**模板**（含全部 4 个节点），只给人看不直接跑 |
| `config.runtime.json` | `start_singbox.py` | 主实例**实际生效**的配置 |
| `config.runtime.backup.json` | 同上 | 备实例实际生效的配置 |
| `runtime.json` / `runtime.backup.json` | 同上 | 运行状态（端口 / PID / 节点） |

**为什么要随机**：固定 `2081/9091` 长期占用容易被本机其它代理软件、IDE 插件、
安全软件莫名撞车；随机 + 启动前 `bind` 实测，从根上避免"端口被占"这类事故。

**为什么偏要挑 50000 以上**：低位段藏着一堆常见服务的固定端口（数据库、开发
服务器、各类代理默认口），大数端口几乎不会被别人认领；而且排查时一眼就能看出
"这不是任何已知服务，是我们自己随机出来的"。整个高位段有 15536 个候选，够用。

**要避开系统预留段**：Windows 上 Hyper-V / WSL / Docker 会通过 `netsh` **整块
预留**端口，落在预留块里的端口 `bind` 会直接返回 access denied（**不是** in use，
很容易被误判成"随机不到端口"）。所以 `start_singbox.py` 启动时会先跑一次

```
python Sing-box\start_singbox.py --show-excluded
```

读系统预留段并自动跳开。本机当前被预留的段（含 TCP+UDP 并集）：

```
49821-49920   50000-50059   50060-50159   50160-50259   57317-57416
50452-50551   50552-50651   50652-50751   57082-57181
```

想换范围（比如避开某个软件）就用 `--port-min` / `--port-max`；高位段万一挑不到
会**自动退到** `20000–65535`，不用手改。

**唯一不能随机的**是副 IP 上的 `1119 / 8285 / 8286` —— 那是游戏客户端**写死**
要连的端口，必须保持原样。随机的只是本机内部约定，只要加速器知道就行。

### 3.2 别写死端口，要读

所有脚本/工具都用 `Sing-box/portof.py` 现读，不要在新代码里再写端口号：

```
python Sing-box\portof.py socks   [默认值] [profile]   # 入口端口（默认读 main）
python Sing-box\portof.py clash   [默认值] [profile]   # 控制接口端口
python Sing-box\portof.py node    [默认值] [profile]   # 当前默认节点 tag
python Sing-box\portof.py pid     [默认值] [profile]   # 进程 PID
python Sing-box\portof.py list                          # 两个实例一览（带存活判定）
```

> ⚠️ **绝不能跨 profile 回退**。早期版本在备实例的状态文件缺失时会退到
> `runtime.json`，结果备实例没在跑时把**主实例的端口**当成了自己的 —— 已经修掉，
> 现在每个 profile 只读自己的文件。

`.cmd` 里这样取（注意 usebackq 两端那对多余的 `""`，见 5.6）：

```bat
for /f "usebackq delims=" %%i in (`""%PY%" "%~dp0portof.py" clash "" backup"`) do set "CLASH_PORT=%%i"
```

### 3.3 让加速器指向它

```
py\run_accel_singbox.cmd        双击即可（自动读随机端口 + 自动提权）
py\switch_upstream.cmd          从 Throne 切到独立 sing-box（自动提权）
py\switch_upstream.cmd throne   切回 Throne
```

或手动：

```
python py\firestorm_accel.py setup --socks 127.0.0.1:<portof 输出的端口>
```

托盘工具（`py\firestorm_tray.cmd`）会自动读 `runtime.json`，菜单里直接显示
当前实际端口。

### 3.4 `start_singbox.py` 全部参数

| 参数 | 作用 |
|---|---|
| `--profile main\|backup` | 起哪个实例（默认 `main`） |
| `--node <tag>` | 覆盖该 profile 的默认节点（tag 不在本实例范围内会提示并回落） |
| `--keep-port` | 沿用该实例上次的端口，不重新随机 |
| `--port <n>` | 指定入口端口（控制接口仍随机；落在系统预留段会直接报错拒绝） |
| `--port-min <n>` / `--port-max <n>` | 自定义随机范围（默认 `50000` / `65535`） |
| `--show-excluded` | 只打印系统预留端口段然后退出，不启动 |

`stop_singbox.py` 侧：

| 参数 | 作用 |
|---|---|
| `--profile main\|backup` | 停哪个实例（默认 `main`） |
| `--all` | 两个都停 |

## 4. 实测数据

### 4.1 两个节点的对比（30 轮交替，每节点各 30 个样本）

| 出口 | min | p50 | p90 | avg | max | 抖动 σ |
|---|---|---|---|---|---|---|
| **hysteria2（独立入口）** | 384 | 391 | **616** | **467** | **897** | **141** |
| vless-reality（独立入口） | 378 | 397 | 859 | 514 | 1583 | 284 |
| Throne :2080（对照） | 384 | 391 | — | 460 | 1119 | 173 |

**读法**：两者的最低延迟几乎相同（378 / 384），因为**物理距离决定了下限**
（中国 ↔ 法国 ~390ms 光缆），任何代理都突破不了。真正的差别在**稳定性**。

- **用 hysteria2 —— 配置里的默认节点。** vless-reality 的抖动是它的两倍、
  最差单次几乎翻倍。原因是 **vless-reality 是 TCP over TCP**：外层隧道丢包会让
  内层游戏的 TCP 也跟着重传，两层互相放大；而 hysteria2 基于 QUIC/UDP，自带
  前向纠错和抗劣质链路的拥塞控制 —— 这正是它被设计出来要解决的问题。
- Throne 那行只作对照。它同时扛着游戏的实际流量，有背景负载，不完全公平。
- ⚠️ **这个结论经过修正。** 早期 18 轮小样本曾显示 vless-reality 抖动更小
  （σ 72 vs 105），据此写过"推荐 vless"；30 轮复测后 hysteria2 全面胜出。
  **跨境延迟测量噪声极大，别用一两轮结果下结论。**

### 4.2 热切换验证

```
GET  /proxies/node                              → {all: [...], now: qiniu-0001}
PUT  /proxies/node {"name":"qiniu-0709"}        → now = qiniu-0709
PUT  /proxies/node {"name":"qiniu-0001"}        → now = qiniu-0001
```

**切换功能本身完全有效**：即时生效、可逆、**不影响已建立的连接**（游戏不会断线）。
两个实例各自有独立的 selector，互不影响。

### 4.3 出口验证

经当前随机入口端口查 `api.ipify.org`，确认走的是各自的默认节点：

| 实例 | 节点 | 出口 IP |
|---|---|---|
| 主实例 | `qiniu-0001`（CF 隧道） | `154.41.93.160` |
| 备实例 | `hy2-va1`（直连） | `66.154.126.18` |

端口随机化不影响链路本身 —— 重启换端口后出口 IP 不变。

> 注意 Cloudflare 隧道的出口 IP 是 CF 那边的地址，**不是** `66.154.126.18`。
> 这是正常的，也是它"不占直连 VPS 流量额度"的原因。

## 5. 踩过的坑

### 5.1 `uTLS is required by reality client`

你用占位符模板时遇到的：

```
FATAL[0000] initialize outbound[1]: uTLS is required by reality client
```

VLESS + Reality **必须**带 `utls`，漏了就起不来：

```json
"tls": {
  "enabled": true,
  "server_name": "apple.com",
  "utls": { "enabled": true, "fingerprint": "chrome" },   // ← 少了这行就报错
  "reality": { "enabled": true, "public_key": "...", "short_id": "..." }
}
```

### 5.2 注释其实是支持的

标准 JSON 不允许 `//` 注释，但 **sing-box 的解析器接受**（内部用类似 hujson 的宽松解析）。
已实测：`sing-box run` 用带注释的 `config.json` 正常启动。所以注释可以放心留。

> 注意：**Python 的 `json` 不容忍注释**。所以 `start_singbox.py` 在生成
> `config.runtime.json` 前会先逐字符剥掉注释（不能简单 `replace` —— 否则
> `"http://..."` 这类字符串里的 `//` 会被误伤）。

### 5.3 别在加速器开着的时候重启 sing-box

切节点请用 `switch-node.cmd`（热切换，不断线）。
直接重启 sing-box 会让中继的上游连接全部断开 → 游戏掉线。

### 5.4 端口别撞车

| 端口 | 归属 |
|---|---|
| 2080 | Throne（固定） |
| 50000–65535 中随机 4 个 | 两个 sing-box 实例各占 2 个（入口 / 控制接口） |
| 1119 / 3724 / 8085 / 8086 / 8285 / 8286 | 加速器副 IP 监听（**固定，游戏写死的**） |

挑端口时会**避开另一个实例已占的**，也会**避开系统预留段**（见 §3.1），
所以两个实例不会撞车、也不会撞上 Hyper-V 那类预留块。
核对当前值跑 `python Sing-box\portof.py list`。

### 5.5 `hysteria2` 不要照抄 `up_mbps` / `down_mbps`

模板里常见 `"up_mbps": 50, "down_mbps": 100`。这两个是**客户端主动限速**，
填小了会把带宽卡死。不填则用 BBR 自适应，通常更好。本配置没有填。

### 5.6 `.cmd` 必须是 CRLF + GBK，而且不要 `chcp`

这条是**最坑的一个**，排查了很久。三个条件全部满足才不会出错：

| 存法 | 结果 |
|---|---|
| LF-only 行尾 | ✗ 解析错位，报 `The filename, directory name, or volume label syntax is incorrect.`，注释后半截被当命令执行 |
| UTF-8 + `chcp 65001` | ✗ cmd 中途换代码页后仍按旧代码页算字节偏移，中文注释被截断成 `'引号' 不是内部或外部命令` |
| **GBK(cp936) + CRLF + 不调 chcp** | ✓ 完全正常 |

实测对照（同一份内容，只改存法）：UTF-8+chcp65001 第 2 行注释就炸了，
GBK 版本三行注释 + 中文输出全部正常。

所以：**这些 `.cmd` 按 GBK 提交、按 GBK 编辑，别让编辑器"另存为 UTF-8"，
也不要加 `chcp`。** `.gitattributes` 里已用 `*.cmd -text` 关掉行尾归一化，
防止检出时被改动。

> ⚠️ 用工具/编辑器改完 `.cmd` 后**一定要复查编码** —— 有些写入通道会悄悄
> 存成 UTF-8。一条命令自检：
> `python -c "b=open('x.cmd','rb').read();print(b.count(b'\n')==b.count(b'\r\n'));b.decode('cp936')"`

另外 `for /f` 取命令输出时，`usebackq` 里的命令**两端要再包一层 `""`**：

```bat
rem ✓ 正确：cmd 会剥掉最外层引号，剩下 "exe路径" "脚本路径" 参数
for /f "usebackq delims=" %%i in (`""%PY%" "%SBX%\portof.py" socks"`) do set "SP=%%i"

rem ✗ 错误：剥完引号变成裸路径，报"文件名、目录名或卷标语法不正确"
for /f "usebackq delims=" %%i in (`"%PY%" "%SBX%\portof.py" socks`) do set "SP=%%i"
```

### 5.7 状态文件可能是残留的

`start_singbox.py` 是"主管进程"，它自己 `wait()` 着 sing-box。正常退出（Ctrl+C、
关窗口）时会顺带删掉状态文件；但被 `taskkill /F` 强杀就来不及删，
于是留下一个指向**已经不存在的端口**的 runtime 文件。

所以判断"实例在跑吗"**一律要探端口**，不能只看文件在不在：

- 托盘：`sb_running(profile)` = 读文件 + `connect` 探端口
- `portof.py list`：多一列"存活"，残留会标 `否(残留)`
- `start_singbox.py`：发现状态文件里的 PID 不通且端口没人听，会照常重新挑端口启动

### 5.8 云隧道的 `530` 是服务端掉线，不是你配错了

`trycloudflare.com` 的地址直连返回 **HTTP 530**，说明 CF 边缘收到了请求但
服务端的 `cloudflared` 没连上来。sing-box 侧报：

```
ERROR connection: open connection to xxx:443 using outbound/selector[node]:
      unexpected HTTP response status: 530
```

这是**对方的问题**，本地怎么调都没用。快速验证一个隧道死没死：

```bash
curl -s -o /dev/null -w "%{http_code}\n" https://<域名>/
```

- `404` → 隧道活着（只是根路径没有内容，正常）
- `530` → 隧道死了

### 5.9 随机挑端口撞上系统预留段会报 access denied

Windows 上 Hyper-V / WSL / Docker 会用 `netsh` **整块预留**端口，落在预留块里的
端口 `bind` 抛的是 **`WSAEACCES`（拒绝访问）**，**不是** `WSAEADDRINUSE`。
现象就是"随机挑端口偶尔莫名失败"，很难往"预留"上想。

用一条命令看当前预留段（TCP+UDP 都看一眼）：

```
python Sing-box\start_singbox.py --show-excluded
```

`start_singbox.py` 启动时已经自动读并跳开这些段，平时不用管；
只有你**手动** `--port <n>` 指定端口时才会因此报错。

## 6. 配置字段速查

| 字段 | 作用 |
|---|---|
| `inbounds[].listen_port` | 本地入口端口（模板里的值会被 `start_singbox.py` 覆盖掉） |
| `outbounds[].tag` | 节点名，`route.final` 和 selector 引用它 |
| `outbounds[].transport` | 传输层，WS 隧道节点用 `{"type":"ws","path":...,"headers":{"Host":...}}` |
| `route.rules[].ip_is_private` | 私网直连，别绕出去 |
| `route.final` | 默认走哪个出站（这里指向 selector） |
| `selector.outbounds` / `selector.default` | 可切换的节点列表 / 默认选中（两个都由 `start_singbox.py` 按 profile 重写） |
| `experimental.clash_api` | 控制接口，热切换用（端口同样被覆盖） |

## 7. 停止后检查

`stop-singbox.cmd` 会从对应 profile 的 runtime 文件里拿本次实际端口，
检查是否已释放，并删掉该状态文件。

一个容易踩的坑：按 PID 杀失败时**不要无脑 `taskkill /IM sing-box.exe`** ——
那会把另一个实例也杀掉。`stop_singbox.py` 的做法是先探端口，**只有确认
该 profile 的端口还在监听**才做进程名兜底。

彻底停干净：

```
Sing-box\stop-singbox.cmd all
python Sing-box\portof.py list      # 应该没有任何行
```
