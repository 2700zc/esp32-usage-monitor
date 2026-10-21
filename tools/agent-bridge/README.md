# ZCode 状态桥接

让 ESP32 屏幕显示 ZCode 此刻在做什么：**正在思考 / 执行工具 / 等待授权 / 思考完成 / 空闲**。

```
ZCode 进程 ──hook(stdio)──> zcode-hook.py ──UDP 本机──> bridge.py
                                                          │
                                          HTTP /agent 轮询 │  ← 设备每秒取一次（1 秒延迟保底）
                                          UDP 推送         │  ← 状态一变立刻推（亚秒级）
                                          UDP 发现应答      │  ← 设备喊一句"桥接在哪"，这里答
                                                          ▼
                                                   ESP32 Agent 状态页
```

三件事各自独立，任何一件没做都只是降级，不会互相拖累：

| 组件 | 没启动时的后果 |
|---|---|
| `zcode-hook.py`（装在 ZCode 配置里） | 桥接收不到事件，状态永远停在 `idle` |
| `bridge.py` | 设备显示"连不上桥接"，其他三页照常 |
| ESP32 或网络 | 桥接手上有状态，只是没人看 |

## 一次性安装

```bash
# 1. 装 hook（写 ~/.zcode/cli/config.json，会先备份）
python tools/agent-bridge/install-hooks.py

# 2. 重启 ZCode（hook 在启动时读取）
```

装完事件的记录方式：**ZCode 每触发一次事件就起一个 python 进程**（约 80ms），
只做"读 stdin → 发一个 UDP 包 → 立即退出"。它不等待桥接、不打印任何东西，
桥接没开也不会拖慢会话。

## 日常使用

```bash
python tools/agent-bridge/bridge.py
```

启动时打印设备要用的地址，直接填进 `data/config.json` 的 `agent_url`：

```
  device poll   http://192.168.1.10:8787/agent
  instant push  udp   -> device:8788 (after first poll)
  firmware config:  "agent_url": "http://192.168.1.10:8787/agent"
```

### 让桥接常驻（重要）

**从 ZCode 会话里启动的桥接会随 ZCode 一起被杀**——我在会话里用 `nohup`/后台方式
拉起过，结果 ZCode 一重启桥接就没了，设备显示「连不上桥接」。桥接必须在 ZCode 的
进程树之外启动：

| 方式 | 说明 |
|---|---|
| `start-bridge.bat` 双击 | 带控制台窗口，窗口里实时打印事件，适合边看边调 |
| `bridge-daemon.bat` | 无窗口，日志写 `bridge.log`。给自启/脚本用 |
| 开机自启（已装好） | 启动文件夹里有 `ZCode agent bridge.lnk` → `wscript bridge-hidden.vbs` → `bridge-daemon.bat`。登录即静默启动 |

开机自启的增删（不需要管理员权限，放 `shell:startup` 即可）：

```powershell
# 查看 / 删除
explorer "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Startup"
```

> `.vbs` 必须是纯 ASCII：Windows Script Host 按系统 ANSI 代码页（简中是 GBK）读它，
> 而不是 UTF-8，中文注释会被解码错乱、把语句结构弄坏（症状是注释行报
> `缺少对象: 'fso'`）。`.bat` 没这个问题。

第一次连不上时，Windows 防火墙可能拦了入站（设备是主动连过来的那一方）：

```
netsh advfirewall firewall add rule name="ZCode agent bridge" ^
      dir=in action=allow protocol=TCP localport=8787 profile=private
```

调试用：

```bash
curl http://127.0.0.1:8787/agent     # 看当前状态 JSON
python tools/agent-bridge/bridge.py --stale-after 60 --idle-after 600
```

## 局域网自动发现

板子不该知道桥接在哪台机器上——换电脑、DHCP 换 IP 都不应该重刷固件。于是它自己找：

```
设备 -> 广播或单播 :8789   "ZCODE-AGENT-V1?"
桥接 -> 单播回源端口        "ZCODE-AGENT-V1 8787"     (8787 = HTTP 端口)
```

设备拿到应答后，IP 取自报文源地址、端口取自报文内容，紧接着就开始每秒轮询。

**两条腿，缺一条也能用**：

| 方式 | 什么时候起作用 | 什么时候不行 |
|---|---|---|
| 广播（`255.255.255.255` + 本子网定向广播） | 设备和 PC 在同一个子网（家里路由器那种扁平网络） | 跨子网——广播不过路由器 |
| 扫段（按上次那台 PC 所在的 /24 逐个单播） | 跨子网也能用（单播走路由），**换电脑后新 PC 大概率还在同一段** | 从没连过任何 PC 时没有可扫的网段 |

扫段需要"上次的地址"当种子，来源有两个：配置里的 `agent_url`（优先），或者上一次成功发现的地址。
所以**建议 `agent_url` 填上当前 PC 的地址**——它让开机第一次就直连成功，同时充当扫段的种子；
填错了也不致命，连败五次后会重新发现并纠正过来。

什么时候触发重新发现：连接 WiFi 后、以及轮询连续失败 5 次时（PC 换了 IP / 换了机器）。
没找到时按 2/4/8/16/32 秒退避着喊，找到后每 60 秒确认一次。

防火墙要放行 UDP 8789（桥接启动横幅里会打印命令），否则设备喊破喉咙也没人应答。
不想用发现：`bridge.py --no-discovery`，然后在固件里写死 `agent_url`。

### 反过来：桥接主动喊设备

只有设备找桥接是不够的。把板子从一个地方搬到另一个地方（或者换过网络）时，它记的是
**上一个地方的桥接地址**，而它自己永远找不到这里——于是桥接得先开口：

- 桥接把每台来轮询过的设备记在 `devices.json`（最多 4 台，运行状态文件，已 gitignore）；
- 设备超过 45 秒没来轮询，桥接就朝记得的地址单播 `ZCODE-AGENT-V1 <端口>`，每 10 秒一次；
- 设备收到就切过去，不用重启也不用重刷配置。

实测（把设备复位成"记着另一个网段的桥接地址"的状态，全程不人工干预）：
45 秒后桥接喊话，设备**同一秒**切过来并恢复每秒轮询。日志带 `HH:MM:SS` 时间戳，
恢复过程一眼能对上账。

**第一次到新地方**需要引导一次（桥接还没有任何设备的记忆，也就不知道该喊谁）：

```bash
python bridge.py --announce-to 172.16.5.95     # 板子当前的 IP，喊一次之后就记住了
```

板子 IP 怎么看：路由器后台、或者桥接日志里上一条 `device at <ip>`。

## 接口

`GET /agent` → JSON（UDP 推送的是同一份内容）：

```json
{"state":"tool","tool":"Bash","detail":"pio run -e esp32-usage-monitor",
 "project":"esp32-usage-monitor","age":4.2,"dur":37.5,
 "active":1,"stale":false,"seq":12,"ts":1760000000}
```

- `state`：`idle` / `thinking` / `tool` / `waiting` / `done`
- `tool`：`state == tool` 时的工具名
- `detail`：此刻在做什么——Bash 的命令、Edit/Read 的文件名、Grep 的 pattern；
  只在 `tool` / `waiting` 状态参与显示。命令里的长串无分隔字符（API key、哈希、
  base64）会先被替换成 `…`：这个接口是局域网明文，命令里带凭据是常事
- `project`：会话工作目录的目录名，其余状态用它回答"这块屏幕在说哪个项目"
- `age`：距桥接收到的最后一个事件多久（谨慎度量的"新鲜度"）
- `dur`：在当前状态里待了多久（界面不再显示，留着便于 curl 排查）
- `active`：近 3 分钟内有活动的会话数
- `stale`：桥接自己认为这个状态已经不可信
- `seq`：每次事件自增，便于判断是否漏包

## 事件到状态的映射

| ZCode 事件 | 状态 |
|---|---|
| `UserPromptSubmit` | `thinking` 正在思考 |
| `PreToolUse` | `tool` 执行工具（带工具名） |
| `PostToolUse` / `PostToolUseFailure` | `thinking` 回到思考 |
| `PermissionRequest` | `waiting` 等待授权 |
| `Stop` | `done` 思考完成 |

`PostToolUse` 必须一起装：只装 `PreToolUse` 的话，一个会话跑完一条命令后会一直
钉在"执行工具"上直到这一轮结束。

## 多个会话 / 子智能体

ZCode 可以同时开多个窗口，还会起子智能体会话（子智能体不触发 `Stop`，因此它会
一直停在"执行工具"上）。桥接按两级规则决定显示哪个：

1. **先看新鲜度**：`--stale-after`（默认 120 秒）内有事件的会话优先，久不吭声的
   会话不会压过正在干活的那个；
2. **再看优先级**：`waiting` > `tool` > `thinking` > `done` > `idle`——同时都新鲜时
   显示最需要你处理的那个（等待授权 > 正在跑）。

超过 `--idle-after`（默认 900 秒）没有任何事件则回落成 `idle`，覆盖"ZCode 被直接
关掉、`Stop` 永远不会来"的情况。

## 排查

**不重启 ZCode 也能端到端自测**：ZCode 带一个无头 CLI，另起一个进程会重新读取配置
（也就带上了 hook），足以验证整条链路：

```bash
node "D:\ZCODE\resources\glm\zcode.cjs" -p "Run exactly this bash command: echo hi. Reply OK." \
     --cwd "C:\Users\admin\AppData\Local\Temp"
```

预期：桥接窗口依次出现 `UserPromptSubmit` / `PreToolUse` / `PostToolUse` / `Stop`
四行，设备串口对应 `agent: push thinking` / `push tool Bash` / `push thinking` / `push done`。

| 现象 | 原因 |
|---|---|
| 状态一直 `idle` | hook 没装 / ZCode 没重启 / 桥接日志里没有事件行 |
| 设备显示「连不上桥接」 | 桥接没在跑（**最常见：它是从 ZCode 会话里启动的，ZCode 一重启就被带走了**，见「让桥接常驻」）、防火墙、PC 和板子不在同一网段 |
| 设备显示「正在查找桥接」→「没找到桥接」 | 局域网里没人应答：桥接没跑、防火墙挡了 UDP 8789，或（跨子网时）扫段的种子地址不在 PC 所在网段 |
| 桥接日志有事件、设备不动 | `agent_url` 写错或 PC 换了网络（IP 变了）；页面右侧会显示"轮询"而不是"实时" |
| 状态卡在某个值不动 | 该会话已退出但没有 `Stop` 事件——等 `--stale-after` 到点会自动降级 |
| 桥接启动时报 `cannot bind udp` | 已经有实例在跑（两个实例会让事件只投给其中一个）——先杀掉旧的 |
| hook 有没有被调用 | `hook-errors.log`（脚本旁边）：hook 里的意外异常都记在这，这是"hook 成功退出但什么都不发"的唯一线索 |
| hook 改了怕改坏 | `python _selftest.py`：动作提取 + 事件分流 + 真起进程发一次报文的冒烟测试 |
| 想看 ZCode 到底传了什么 | 设 `ZCODE_AGENT_BRIDGE_DEBUG=1`（ZCode 侧的环境变量），原始载荷会写到 `hook-debug.log` |

> 顶层 `except` 保证 hook 永不拖累会话，代价是**异常会静默**。所以 hook 里每一处
> 兜底都接了 `note_error()` 落盘；排查任何"桥接收不到事件"的问题，第一个看的就是
> `hook-errors.log`。

卸载（只删本桥接装的条目，其他 hook 不动）：

```bash
python tools/agent-bridge/install-hooks.py --uninstall
```

## 已知边界

- **没有 token 级事件**：ZCode 只提供 7 个 hook 事件，粒度是"轮次/工具"。所以
  状态是"正在思考"而不是逐字输出；一轮内的细粒度变化只能到这个程度。
- **hook 是同步的**：`async: true` 在当前版本没有实际作用，所以脚本必须毫秒级返回。
  这也是 hook 只发 UDP、不做 HTTP 的原因。
- **hook 端口是 UDP**：桥接没开时事件直接丢弃，不会排队补发——设备上看到的是
  "最新的真实状态"，不是历史。
