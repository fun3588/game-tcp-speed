# gui · 加速器图形面板

把「改节点 + 起停实例 + 切上游 + 看日志」全放进一个原生桌面窗口（tkinter）。

```
双击 gui\run-gui.cmd   →  自动提权  →  弹出窗口
```

关掉窗口 = 关掉面板。**sing-box 实例和加速器都不受影响**（面板只是个控制台，
不像托盘那样驻留）。

---

## 为什么要单独挑解释器

**tkinter 是 CPython 的编译期模块，没有 pip 包可装。**
项目平时用的托管 Python 是精简构建，没编译它，装不回来：

```
C:\Users\junjun\.workbuddy\binaries\python\versions\3.13.12\python.exe
>>> import tkinter
ModuleNotFoundError: No module named 'tkinter'
```

而系统 Python `D:\py3-12-1\python.exe` 自带 tkinter 8.6 + ttk。

所以**不换项目的解释器**，只让 GUI 单独用一个带 tkinter 的：

- **没有引入任何第三方依赖** —— `config_edit.py`、`share_link.py`、
  `py/firestorm_tray.py` 全是纯标准库，两个解释器都能 import。
- **业务逻辑一份都没重复** —— 状态探测/启停转手给 `firestorm_tray`，
  改配置用 `config_edit`，分享链接解析用 `share_link`。

---

## 界面能干什么

### 状态条（顶部）
副 IP 有没有挂上、中继在不在、游戏是否在线、加速器当前挂在哪个上游、当前是否管理员。
2.5 秒自动刷新。

### 两个实例卡片
| 显示 | 说明 |
|---|---|
| 运行中 / 已停止 | 真探端口，不看状态文件（状态文件可能是残留） |
| 入口 / 控制 | 当前随机端口；**已停止时显示 `—`** |
| PID | 该实例的 sing-box 进程；已停止时显示 `—` |
| 节点 | 从 clash_api 读的**实时**节点，不是配置文件里的默认值 |

> 已停止时残留的端口不丢弃，降级成一行浅色小字：
> 「上次端口 55932 / 控制 62462（已停止，重启会换新端口）」。
> 直接当"当前端口"显示会让人以为还在监听，而重启本来就会换新的。

操作：**启动 / 停止 / 测速**，以及**下拉直接换节点**（热切换，不断线）。

### 加速器
启动 / 停止；上游选 Throne 或任一实例，点「切换」。当前上游会标 `✓`。
切换会短暂断线（停旧中继 → 清副 IP → 重启）。**改副 IP 需要管理员权限**，
所以 `run-gui.cmd` 会先提权一次；未提权时点这类按钮会先问你要不要就地提权。

### 节点配置
表格列出 `Sing-box/config.json` 里的所有节点：tag、类型、服务器、传输、加密（tls/reality）、
是否上架（在 selector 里）、实时延迟。操作：

- **+ 添加节点** —— 粘一条分享链接就行
- **编辑**（双击行也行）—— 改 tag 靠删除+新增，其余可直接改
- **复制链接** —— 反向生成分享链接，方便备份/分享
- **删除**
- **全部测速** —— 用任一在跑的实例把所有节点测一遍

支持解析：`vless://`、`hysteria2://` / `hy2://`、`trojan://`、`ss://`、`vmess://`。

### 日志
切换看主实例 / 备实例 / 加速器的日志尾部。

---

## 关键设计

### 1. 配置改动**保留手写注释**

`config.json` 里有一大堆有价值的注释（解释 WS 怎么映射、reality 为什么必须带 utls、
flow 为什么不能加在 ws 上……）。`config_edit.py` **不做 `json.dump()` 整体序列化**，
而是把文件当文本，用括号匹配定位结构，做块级增删改 —— 已有节点的注释原封不动。

改之前会先备份成 `config.json.bak`。

自检（增 → 改 → 删一整圈后文件**字节一致**）：

```bash
python -c "
import sys; sys.path.insert(0, r'gui')
import config_edit as C
o = open(C.CONFIG,'rb').read()
n = {'type':'vless','tag':'__t__','server':'a.b','server_port':443,'uuid':'x'}
C.add_outbound(n, comments=['临时']); C.sync_selector_with_nodes()
C.update_outbound('__t__', dict(n, server='c.d')); C.delete_outbound('__t__')
C.set_selector(['qiniu-0709','qiniu-0001','hy2-va1','vl-reality-va1'], 'qiniu-0709')
print('字节一致:', open(C.CONFIG,'rb').read() == o)
"
```

### 2. 测速用 clash_api，**不切节点**

```
GET /proxies/<tag>/delay?timeout=8000&url=http://www.gstatic.com/generate_204
```

sing-box 的 clash_api 自带这个端点，所以能一次把选择器里所有节点都测一遍，
**完全不影响当前连接** —— 不会因为"想测一下"把正在玩的游戏踢下线。

### 3. 启停**不自己拼 sing-box 命令行**

一律转手调 `Sing-box/start_singbox.py --profile X` / `stop_singbox.py --profile X`
（直接复用 `py/firestorm_tray.py` 里那几个函数）。挑端口、剥注释、生成运行时配置
的逻辑只存在一份 —— 否则面板、托盘、脚本三处迟早不一致。

### 4. tkinter 不是线程安全的 ★

工作线程**绝对不能**碰 widget，**也不能调 `root.after()`** —— `after()` 内部会
`createcommand`，非主线程调用会抛：

```
RuntimeError: main thread is not in main loop
```

所以走**队列**：后台线程只把结果 `put` 进 `queue.Queue`，主线程用 `_drain()`
（每 80ms）排空并落到界面上。所有 `root.after` 都只在主线程调用。

> 这个问题在正常 `mainloop()` 下常常"看起来没事"（GIL 帮忙），属于**未定义行为**，
> 只在自检/单元测试/启动早期炸，特别容易误判成"偶发"。别图省事直接在工作线程里 `after`。

### 5. 端口探活要防"IP 不在本机"的假阳性

判断"中继在不在监听"不能用一次 `bind()` 反测 —— 副 IP 摘掉后 `bind` 失败的原因是
**地址不存在**（`WSAEINVAL` 10022），不是被占用。只判一次的话，加速器一停就会
显示"监听中"，按钮状态全反。

`py/firestorm_tray.py` 的 `port_bound()` 现在分两步：先 `bind((ip, 0))` 确认 IP
在本机，再探端口。详见 skill `windows-free-port`。

---

## 文件

```
gui/
├─ run-gui.cmd       双击入口（自提权，自动挑带 tkinter 的 Python）
├─ firestorm_gui.py  原生窗口（tkinter + ttk，单文件）
├─ config_edit.py    保留注释的 config.json 编辑器
├─ share_link.py     分享链接 ↔ sing-box 出站
└─ README.md
```

> `run-gui.cmd` 必须是 **GBK(cp936) + CRLF**，且**不要加 `chcp`**。
> 用编辑器"另存为 UTF-8"会让里面的中文提示全变乱码。
> 要改它请做**字节级替换**（`open(p,'rb').read().decode('cp936')` → 改 → 编码写回），
> 别用会写 UTF-8 的编辑器直接保存。

---

## 启动器在挑什么

`run-gui.cmd` 按顺序试这些，**第一个能 `import tkinter` 的**就用它：

```
D:\py3-12-1\pythonw.exe
%LOCALAPPDATA%\Programs\Python\Python312\pythonw.exe
%LOCALAPPDATA%\Programs\Python\Python313\pythonw.exe
%ProgramFiles%\Python312\pythonw.exe
%ProgramFiles%\Python313\pythonw.exe
```

- 用 **`pythonw.exe`** 起，不留黑框；探测 tkinter 时用同目录的 `python.exe`。
  （`pythonw` 启动失败是**静默**的，所以下面那步自检不能省。）
- 起飞前先 `import firestorm_gui` 跑一遍（不建窗口），有错当场显示而不是静默退出。
- 一个都挑不到时给明确提示，并告诉你去 python.org 装官方 CPython。

自测（构建界面 + 渲染一轮后退出，不进事件循环）：

```bash
python gui\firestorm_gui.py --self-test
```

```
self-test OK：窗口已构建，主题 vista
  实例  : main=已停止, backup=已停止
  节点表: 4 行
  胶囊  : alias=未挂载, relay=未监听, game=未运行, accel=已停止
  加速器: 加速器未运行
  权限  : 未提权 · 加速器启停不可用
```

---

## 和托盘（`py\firestorm_tray.cmd`）什么关系

两者**功能重叠**，但定位不同：

| | 托盘 | 这个面板 |
|---|---|---|
| 形态 | 系统托盘图标 + 右键菜单 | 桌面窗口 |
| 常驻 | 是，一直挂在托盘 | 否，关窗口就没了 |
| 适合 | 日常一键起停、看状态颜色 | 配置节点、看表格、批量测速 |

可以同时开，**不冲突** —— 它们读同一批 runtime 文件、调同一批脚本，
谁先谁后起停也没关系（实例的存活由端口探测判定，不靠"谁启动的"）。

---

## ⚠️ 一个关于"谁启动的实例"的提醒

面板启动实例时，真正跑起来的是两层进程：

```
面板 (firestorm_gui.py)
  └─ 主管 (start_singbox.py --profile X)   ← 它自己 wait() 着 sing-box
       └─ sing-box.exe
```

正常关掉面板**不会**带走 sing-box —— 主管进程是独立起来的（`CREATE_NO_WINDOW`，
没有控制台绑定），Windows 也不会因为父进程退出就级联杀子进程。

但有一种情况会连带清掉：**面板自己处在一个"作业对象(job object)"里**
（比如被某个自动化工具/沙箱托管，或者任务管理器里对整棵进程树"结束任务"）。
那种情况下作业被拆除时会连带杀掉里面的所有进程。

所以如果你希望实例**长期稳定地跑**（尤其是打算挂机），更稳的做法是：

```
Sing-box\run-singbox-all.cmd     一次性把两个实例起在独立窗口里
```

然后面板只用来**配置节点、看状态、切节点、切上游** —— 这些操作跟实例是谁启动的无关。

---

## 踩过的坑

**① tkinter 会假装自己是线程安全的。**

工作线程里调 `root.after(0, ...)` 在 `mainloop()` 下往往"能跑"，实际是未定义行为，
一旦主线程不在 `mainloop` 里（自检、单元测试、启动早期）就立刻炸
`RuntimeError: main thread is not in main loop`。必须用队列 + 主线程轮询。

**② 自检不能只 `update()` 一次。**

状态快照是在后台线程跑完再回主线程的，`update()` 一次拿不到数据，
会误报"节点表 0 行"。自检要**一边推事件循环一边等**（轮询 `app.state`，给个超时），
并把关键读数打出来 —— 否则"自检通过"是假的。

**③ 只用一次 `bind()` 判断端口占用会假阳性。**

见上面「关键设计 5」。

**④ 解析 `ss://`：`urlsplit()` 的 `.username` 在没有 `@` 时返回 `None`，
`.hostname` 会把内容转小写。**

老式 `ss://base64(method:password@host:port)` 整串 base64 里没有字面 `@`，
所以 `.username` 是 `None`、`.hostname` 还把它**全部小写**（base64 是大小写敏感的，
直接毁掉数据）。必须用 `url.netloc` 原文。

**⑤ 替换 JSON 对象时要**从行首**开始替换，不是从 `{` 开始。**

否则行首缩进被原地保留、新块的缩进又补一次，**每改一次缩进翻一倍**。

**⑥ 删掉新增的节点要靠"块区间"还原。**

块区间定义成「上一项逗号之后 → 本项逗号之后」，插入时补 `"\n" + block + ","`，
这样加完再删能**字节还原**。（早期版本插的是 `block + ",\n"`，会多留一个空行。）
