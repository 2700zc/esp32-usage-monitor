# ESP32-S3 AI 用量监视器

一块放在桌上的小屏幕，抬眼就能看到两件事：**你的 AI 编程服务还剩多少额度**，以及
**你的 Agent 此刻在干什么**（正在思考 / 执行工具 / 等待授权 / 思考完成）。左右滑动切页。

硬件是 Waveshare ESP32-S3-Touch-AMOLED-2.16（480×480 AMOLED + 触摸），固件用
PlatformIO + Arduino 框架，配置放在板子的文件系统里，凭据不经过任何第三方服务器。

```
┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
│ ● Agent     14:32│  │ ● OpenCode Go    │  │ ● CommandCode    │  │ ● DeepSeek       │
│   状态·3秒前      │  │   用量窗口·刚刚   │  │   用量窗口·刚刚   │  │   今日用量·刚刚   │
│ ┌──────────────┐ │  │ ┌──────────────┐ │  │ ┌──────────────┐ │  │ ┌──────────────┐ │
│ │ 当前状态   1个│ │  │ │ 滚动用量  2时  │ │  │ │ 滚动用量  3时  │ │  │ │ 今日请求数    │ │
│ │ 正在思考     │ │  │ │ 0.54%        │ │  │ │ 3.01%        │ │  │ │ 128 次       │ │
│ └──────────────┘ │  │ └──────────────┘ │  │ └──────────────┘ │  │ └──────────────┘ │
│ ┌──────────────┐ │  │ ┌──────────────┐ │  │ ┌──────────────┐ │  │ ┌──────────────┐ │
│ │ 当前工具     │ │  │ │ 每周用量  5天  │ │  │ │ 每周用量  5天  │ │  │ │ 今日 Tokens   │ │
│ │ Bash         │ │  │ │ 18.11%       │ │  │ │ 41.2%        │ │  │ │ 1,234,567    │ │
│ └──────────────┘ │  │ └──────────────┘ │  │ └──────────────┘ │  │ └──────────────┘ │
│ ┌──────────────┐ │  │ ┌──────────────┐ │  │ ┌──────────────┐ │  │ ┌──────────────┐ │
│ │ 当前动作     │ │  │ │ 每月用量  2周  │ │  │ │ 每月用量  2周  │ │  │ │ 账户余额  CNY │ │
│ │ pio run -e … │ │  │ │ 14.43%       │ │  │ │ 22.8%        │ │  │ │ ¥ 42.15      │ │
│ └──────────────┘ │  │ └──────────────┘ │  │ └──────────────┘ │  │ └──────────────┘ │
│    ·  ○  ○  ○    │  │    ○  ●  ○  ○    │  │    ○  ○  ●  ○    │  │    ○  ○  ○  ●    │
└──────────────────┘  └──────────────────┘  └──────────────────┘  └──────────────────┘
```

## 目录

- [功能一览](#功能一览)
- [硬件需求](#硬件需求)
- [快速开始](#快速开始)：装环境 → 取代码 → 填配置 → 烧写 → 首次开机
- [刷入固件](FLASHING.md)：预编译包（不用编译）、AI/命令行刷写、故障排查
- [配置详解](#配置详解)：每个凭据从哪里拿
- [ZCode 状态页完整教程](#zcode-状态页完整教程)：PC 端装 hook 与桥接
- [日常使用](#日常使用)：按键、滑动、刷新
- [换电脑 / 换网络](#换电脑--换网络)：不用重刷固件的那些办法
- [常见问题与排查](#常见问题与排查)
- [项目结构](#项目结构)
- [开发](#开发)：构建、串口、调试开关
- [安全与隐私](#安全与隐私)：凭据都在哪、什么东西不会被提交

## 功能一览

- **四页滑动切换**，顺序：Agent 状态 → OpenCode Go → CommandCode → DeepSeek
  - **Agent 状态**：正在思考 / 执行工具 / 等待授权 / 思考完成 / 空闲；第二行是工具名，
    第三行是**它此刻在做什么**（Bash 的执行命令、Edit 的文件名），不在执行时显示会话
    所属的项目名。状态变化在**亚秒级**推到屏幕上
  - **OpenCode Go / CommandCode**：滚动 / 每周 / 每月用量（百分比 + 距重置倒计时），
    到 100% 数字变红
  - **DeepSeek**：今日请求数、今日 Tokens（千分位）、账户余额（低于 5 元变红）
- 用量**每 60 秒**刷新一轮；**PWR 键**随时手动刷新
- **左右滑动**切页，到底循环，底部圆点指示当前位置
- **屏幕随重力自动旋转**：竖放 / 横放 / 倒置时界面自动保持正立；平放在桌面上原地
  转 90° 由陀螺仪识别。2.16 是方形屏，旋转只改扫描方向，不重排、不拉伸布局
- **WiFi 配置界面直接做在屏幕上**（扫描 + 软键盘输密码），配好之后开机自动连接
- **页面开关做在设备上**：长按 BOOT 进设置页，四页可各自开关；关掉的页面不再显示，
  也不再抓取用量（过期的凭据不会再被每分钟请求一次）。开关存在 NVS，重刷固件
  或文件系统都不丢
- 深色 AMOLED 界面：纯黑背景 + 深灰圆角卡片 + 各家品牌色
- 刷新失败**不会清空页面**：继续显示上一次的数据，页头用琥珀色标出失败原因与数据年龄

只配了其中一家的凭据时，就只有那一页 + 状态页。状态页默认存在（不填
`agent_url` 也能靠局域网发现找到桥接），也可以在页面开关里关掉；四页全关时
屏幕显示「所有页面已关闭」，长按 BOOT 即可重新打开。

## 硬件需求

| 项目 | 要求 |
|---|---|
| 开发板 | Waveshare **ESP32-S3-Touch-AMOLED-2.16**（ESP32-S3R8 / 16MB Flash / 8MB OPI PSRAM） |
| 屏幕 | 480×480 AMOLED（CO5300），电容触摸（CST9220） |
| IMU | 板载 QMI8658A（加速度计 + 陀螺仪），用于屏幕自动旋转 |
| 数据线 | USB-C，**要能传数据**（不少线只能供电，插上找不到串口就是这类） |
| 网络 | 2.4GHz WiFi，且与跑桥接的电脑互通（见[换电脑 / 换网络](#换电脑--换网络)） |
| 电脑 | Windows（脚本按 Windows 写的；固件本身跨平台） |

## 快速开始

### 0. 最省事：直接刷预编译固件（不用编译）

到 [Releases](https://github.com/2700zc/esp32-usage-monitor/releases/latest) 下载
`esp32-usage-monitor-flash.zip`（约 1.1MB），解压后双击 `flash.bat`。只需要
**Python 3**；脚本会自动安装 esptool（约 7MB），不需要 PlatformIO，也不需要
编译工具链。详细说明和故障排查见 [FLASHING.md](FLASHING.md)。

下面的 1–5 步是给要改代码的人看的。

### 1. 装开发环境

- [PlatformIO](https://platformio.org/)：VS Code 装 PlatformIO 插件，或直接 `pip install platformio`
- 首次编译要下载 ESP32 工具链，**冷编译大约十几分钟**，之后增量编译一分钟左右

### 2. 取代码

```bash
git clone https://github.com/2700zc/esp32-usage-monitor.git
cd esp32-usage-monitor
```

### 3. 填配置

```bash
cp data/config.json.example data/config.json
```

然后编辑 `data/config.json`，按[配置详解](#配置详解)逐项填入。**任意一项留空 = 不显示
那一页**，留空不影响其他页。

### 4. 编译与烧写

```bash
# 编译
pio run -e esp32-usage-monitor

# 烧固件（板子插在哪就写哪个口）
pio run -e esp32-usage-monitor -t upload --upload-port COM5

# 烧文件系统（config.json 在这里面）
pio run -e esp32-usage-monitor -t uploadfs --upload-port COM5
```

> ⚠️ **不要把 `-t upload` 和 `-t uploadfs` 写在同一条命令里。** 实测（PlatformIO Core 6.1.19）
> 两个目标同时指定时文件系统上传会顶掉固件上传，日志里只有 `Uploading ...littlefs.bin`
> 而没有 `firmware.bin`，**固件实际没刷进去且不报错**。请分两条命令依次执行，并确认日志里
> 出现过 `Wrote ... at 0x00010000`。

> 💡 串口不确定是哪个：Windows 设备管理器里认 **VID:PID = 303A** 那个。蓝牙虚拟串口
> （常见 COM3/COM4）长得像但不能烧写。

### 5. 首次开机

1. 上电后自动进入 **WiFi 配置界面**（扫描附近网络）
2. 三个实体键操作：
   - **PWR（中）**：列表中选下一个
   - **IO18（左）**：列表中选上一个
   - **BOOT（右）**：确认，进入密码输入
3. 软键盘输密码：**PWR / IO18** 移动光标（长按连发），**BOOT** 输入字符；
   底行功能键依次是 模式切换 / 删除 / 空格 / OK
4. 连上后进入页面并开始刷新；之后开机自动连接已保存的 WiFi
5. 连不上超过 10 秒会自动退回 WiFi 配置界面重新选

> 刚上电那几十秒 DeepSeek 页可能显示"时间未同步"：取值要用到东八区的当天边界，而 NTP 还没
> 同步完。等一分钟（自动重试一轮）或按 PWR 立刻重试即可，不用重启。

## 配置详解

`data/config.json` 是带 `//` 注释的 JSON（板子靠 `-DARDUINOJSON_ENABLE_COMMENTS=1` 才认，
见 `platformio.ini`——**别去掉那个编译选项**，否则整份配置解析失败、所有页面消失）。

| 字段 | 用途 | 留空时 |
|---|---|---|
| `ds_token` | DeepSeek 用量/余额 | 不显示 DeepSeek 页 |
| `oc_api_key` | OpenCode Go（推荐，长期有效） | 见下（可改用 cookie 方式） |
| `oc_workspace_id` + `oc_cookie` | OpenCode Go 的备选（浏览器会话） | 不显示 OpenCode Go 页 |
| `cc_cookie` | CommandCode 用量 | 不显示 CommandCode 页 |
| `agent_url` | Agent 状态页的桥接地址（**可选**，只是个种子） | 靠自动发现找桥接 |

### DeepSeek：`ds_token`

1. 登录 <https://platform.deepseek.com/usage>
2. F12 → 网络 → 刷新页面 → 点任意 `api/v0/` 开头的请求
3. 复制请求头 `authorization` 里 **`Bearer ` 后面**的部分（不要带前缀）

> 这是网页登录态 token，会过期；过期后该页显示"时间未同步"或获取失败。

### OpenCode Go：`oc_api_key`（推荐）

在 opencode.ai 控制台 → Go / API keys 生成一个 `oc_sk_` 开头的 key，原样填入。
实测它长期有效，且请求时连 `x-org-id`、Referer 都不需要。

### OpenCode Go：`oc_workspace_id` + `oc_cookie`（备选）

1. `oc_workspace_id`：控制台地址 `https://opencode.ai/console/wrk_XXXXXXXXXXXX/models`
   里那段 `wrk_...`
2. `oc_cookie`：F12 → 网络 → 找 `/console/api/go/status` 请求 → 复制请求头 `cookie` 里
   `__Host-console_session=` 那一段（连名字带值一起填也行，固件只认这个名字）
3. 整串 cookie、或右键该请求「Copy as cURL」得到的整条命令直接粘进去也可以，固件会自己挑

> 这条路是浏览器会话，会过期，届时该页显示"凭据已失效"，重新复制即可。
> 旧的 `auth=Fe26.2**` 已经失效（`/_server` 接口现在只回 302 跳登录页），别填那个。

### CommandCode：`cc_cookie`

登录 commandcode.ai → F12 → 应用(Application) → Cookie → 找到
`__Secure-commandcode_prod_.session_token`，复制它的**值**（不带 cookie 名）。
浏览器复制出来是 URL 编码的（含 `%2F`、`%3D`），原样填入即可。

### Agent 状态：`agent_url`

**可以留空**——板子会在局域网里自己找桥接。想填就填桥接启动时打印的那一行，好处是
开机第一次就直连成功、不用等发现过程。详见下一节。

## ZCode 状态页完整教程

### 一句话原理

ZCode 本身不向外发布状态，但支持 **hook**（事件回调）。PC 上跑一个小小的桥接进程：
hook 把事件用 UDP 报给它，它汇总成"当前状态"，设备每秒来取一次，另外状态一变还会
**主动推**给设备（亚秒级）。设备只管显示。

```
ZCode ──hook──> zcode-hook.py ──UDP 本机──> bridge.py
                                              │ HTTP /agent  ← 设备每秒取（保底 1 秒）
                                              │ UDP 推送     ← 状态一变立刻推（亚秒级）
                                              │ UDP 发现     ← 设备找桥接 / 桥接喊设备
                                              ▼
                                        ESP32 状态页
```

### 第一步：PC 上装 hook（一次性）

```bash
python tools/agent-bridge/install-hooks.py
```

它会往 `~/.zcode/cli/config.json` 写入 6 个事件的 hook（先自动备份成
`config.json.bak-<时间戳>`），然后**重启 ZCode** —— hook 是启动时读取的。

> 代价要知道：hook 是同步执行的，于是**每个 ZCode 会话的每次工具调用会多起一个
> python 进程（约 80ms）**。不想要了就 `python tools/agent-bridge/install-hooks.py --uninstall`。

### 第二步：PC 上常驻桥接

```bash
python tools/agent-bridge/bridge.py
```

也可以双击 `tools/agent-bridge/start-bridge.bat`（带窗口，窗口里实时打印事件，调试用），
或者 `bridge-daemon.bat`（无窗口，日志写 `bridge.log`）。

启动后会打印设备需要的地址、以及需要放行的防火墙命令：

```
ZCode agent bridge up
  device poll   http://192.168.1.10:8787/agent
  instant push  udp   -> device:8788 (after first poll)
  discovery     the device finds this bridge by itself; agent_url may be left empty
  if the device cannot reach it, allow the ports once (admin cmd):
    netsh advfirewall firewall add rule name="ZCode agent bridge" dir=in action=allow protocol=TCP localport=8787 profile=private
    netsh advfirewall firewall add rule name="ZCode agent discovery" dir=in action=allow protocol=UDP localport=8789 profile=private
```

首次连不上就把那两条 `netsh` 命令用**管理员** cmd 跑一遍（设备是主动连过来的那一方，
入站被拦是最常见的原因）。

> ⚠️ **桥接必须在 ZCode 的进程树之外启动**。从 ZCode 会话里拉起来的进程会随 ZCode
> 重启一起被杀，设备就显示"连不上桥接"（这个坑我们踩过）。

### 第三步：让桥接开机自启（推荐）

双击一次 `start-bridge.bat` 能跑，但重启电脑就没了。二选一：

- **启动文件夹**：`Win+R` → `shell:startup` → 把 `bridge-hidden.vbs` 的快捷方式丢进去
  （无窗口启动，日志仍然写 `bridge.log`）；
- **手动**：每次开机双击 `start-bridge.bat`。

### 第四步：确认它工作

重启 ZCode 后随便发一句话，桥接窗口里应该立刻出现：

```
14:32:05   UserPromptSubmit   sess_xxxxxxxx   thinking   esp32-usage-monitor   sessions=1
14:32:09   PreToolUse         sess_xxxxxxxx   Bash       pio run -e esp32-…    sessions=1
14:32:34   Stop               sess_xxxxxxxx   done       esp32-usage-monitor   sessions=1
```

同时屏幕上应该看到 正在思考 → 执行工具(Bash) → 思考完成。状态页"当前工具"那行右侧
会写 **「实时」**（这份状态来自 UDP 推送）或 **「轮询」**（来自每秒的 HTTP 轮询）——
排查链路时看这两个字就知道走的是哪条路。

### 事件到状态的映射

| ZCode 事件 | 屏幕状态 |
|---|---|
| `UserPromptSubmit` | 正在思考 |
| `PreToolUse` | 执行工具（带工具名） |
| `PostToolUse` / `PostToolUseFailure` | 回到 正在思考 |
| `PermissionRequest` | 等待授权（这个状态不会随时间衰减） |
| `Stop` | 思考完成 |

同时开多个会话或子智能体时，按"最新的优先、需要人管的优先"取一个显示（等待授权 >
执行工具 > 正在思考 > 完成）。子智能体不发 `Stop`，所以它的状态靠超时降级。

### 端口一览

| 端口 | 方向 | 用途 |
|---|---|---|
| 8787/TCP | 设备 → PC | 设备每秒轮询状态 |
| 8788/UDP | hook → PC（本机回环）；PC → 设备 | hook 上报事件；桥接即时推状态 |
| 8789/UDP | 双向 | 局域网发现（设备找桥接 / 桥接喊设备） |

## 日常使用

| 操作 | 效果 |
|---|---|
| 左右滑动 | 切页，到底循环 |
| **PWR（中键）** | 立刻刷新用量；同时让状态页立刻重试（不用等退避） |
| **BOOT（右键）短按** | 重新进入 WiFi 配置界面 |
| **BOOT（右键）长按 0.7 秒** | 进入页面开关设置页 |
| 断电重开 | 自动连 WiFi，并从上次那台 PC 恢复状态页 |

### 页面开关

长按 BOOT 进入设置页，屏幕列出四页（状态页 / OpenCode Go / CommandCode /
DeepSeek）。PWR、IO18 上下选行，BOOT 切换开关；选到「完成」按 BOOT 保存并返回。

- 关掉的页面从滑动序列里消失，页码圆点自动减去一页。
- 关掉同时**停止抓取**：该服务商不再进入 60 秒轮次，过期的 token / cookie 不会
  每分钟再吃一次 401；状态页关掉后也不再轮询桥接、不再扫局域网。
- 批量抓取只在后台任务里进行，改开关不影响其他页面。重新打开的页面会立刻请求
  一次，不用等下一轮。
- 开关存在 NVS（命名空间 `pages`），换 WiFi、重刷固件、重刷文件系统都不会重置。
  想要恢复默认全开，目前重新刷一次整片 Flash 即可。
- 没填凭据的服务商行里标着「未配置」：开关可以拨，但在凭据填好之前那一页不会
  出现。

用量每 60 秒刷新一轮：Agent 状态轮询和三家用量抓取各跑在一个 **core 0 的后台
FreeRTOS 任务**里，UI 循环只做触摸采样和绘制，从不等待网络。所以 TLS 握手、接
口超时、桥接不在都不会再让滑动或画面卡住。

主循环也不会每帧重画：翻页/数据更新/网络状态变化时立即画，其余按 1 秒（状态页）
或 10 秒（用量页）兜底刷新。空闲时循环只有几毫秒，触摸采样约 100 Hz；滑动在位移
越过阈值的那一帧就切页，不等抬手。

## 换电脑 / 换网络

**结论：一般不需要重刷固件。** 设备找桥接有三条路，从快到慢：

1. **NVS 记忆**：设备把最后连上的桥接地址存在自己的闪存里，重启后直接照它连 —— 同一个
   地方重启，立刻恢复。
2. **桥接主动喊**：桥接把来轮询过的设备记在 `tools/agent-bridge/devices.json`，设备
   超过 45 秒没来就朝它单播一条"桥接在这儿"，每 10 秒一次。实测把板子复位成"记着另一个
   网段的地址"的状态，45 秒后被自动找回。**这条覆盖"换电脑/搬到新地方"**。
3. **设备自己找**：广播（扁平局域网有效）+ 按种子地址所在 /24 逐个单播（能跨网段）。
   轮询连续失败 5 次触发。

真正需要动手的只有两种情况：

- **第一次到一个全新的地方**：桥接那边还没有任何设备记忆，也就不知道该喊谁。用
  `python tools/agent-bridge/bridge.py --announce-to <板子IP>` 引导一次，之后它就记住了。
- **板子的 WiFi 也换了**（换了路由器/换了网段）：那要在板子上重配 WiFi——按 **BOOT** 键
  进配置界面，扫描选网络、输密码。这跟桥接是两件独立的事。

同一个局域网里两台电脑都跑桥接也可以，设备只会认它当前连上的那台。

## 常见问题与排查

| 现象 | 原因 / 处理 |
|---|---|
| 插上后找不到串口 | 数据线只能供电；或需要装 CH343/USB-JTAG 驱动。认 VID:PID `303A` |
| 烧写完页面还是旧的 | `uploadfs` 和 `upload` 写在了同一条命令里，文件系统顶掉了固件（见上） |
| 页面显示"未配置服务商" | `data/config.json` 没填 / 没 `uploadfs` / JSON 注释被关掉了 |
| 某页显示"凭据已失效" | 该服务的 token/cookie 过期，重新复制填上再 `uploadfs` |
| DeepSeek 页显示"时间未同步" | 刚上电还没同步 NTP，等一分钟或按 PWR 刷新 |
| 用量不更新 | 看页头副标题：琥珀色的"失败原因 · 数据年龄"就是上一次刷新失败的原因 |
| 状态页显示"正在查找桥接" | 正常过程：设备在广播找桥接（2→32 秒退避） |
| 状态页显示"没找到桥接" | 一分钟没找到。看 PC 上桥接是否在跑、防火墙是否放行 UDP 8789；第一次到新地方用 `--announce-to` |
| 状态页显示"连不上桥接" | 桥接不在（**最常见：它是从 ZCode 会话里启的，ZCode 一重启就被带走了**）、PC 换了 IP、防火墙 |
| 桥接日志里没有事件 | hook 没装 / ZCode 没重启；看 `tools/agent-bridge/hook-errors.log` |
| 桥接启动报 `cannot bind udp` | 已经有一个实例在跑，先杀掉旧的（两个实例会让事件只投给其中一个） |
| 状态停在某个值不动 | 那个会话退出了但没发 `Stop`（子智能体就这样）——等 2 分钟会自动降级 |
| 屏幕方向不跟手 | 看串口 `rot:` 日志：倾斜时以重力为准，平放时以陀螺仪为准；平放没有绝对方位，把设备竖起一下再放平即可重新定方向 |
| 旋转 90° 后界面上下颠倒 | `src/hw/display.cpp` 的 `ROT_MADCTL` 表里 `R90` / `R270` 两项对调 |
| 触摸左右滑方向反了 | `src/hw/gesture.cpp` 里对应旋转角的 `next` 条件取反 |

桥接自己的排查表更细，见 [`tools/agent-bridge/README.md`](tools/agent-bridge/README.md)。

## 项目结构

```
├── data/
│   ├── config.json          # 运行时配置（含凭据，已 gitignore）
│   └── config.json.example  # 配置模板
├── src/
│   ├── main.cpp             # 主循环 + 两个 core 0 网络任务（用量抓取 / Agent 状态）
│   ├── http_client.cpp/h    # HTTPS / 明文 HTTP GET（共用精确读正文的核心）
│   ├── agent_client.cpp/h   # Agent 状态：轮询、推送解析、局域网发现、地址记忆
│   ├── deepseek_client.cpp/h    # DeepSeek 用量/余额
│   ├── opencode_client.cpp/h    # OpenCode Go
│   ├── commandcode_client.cpp/h # CommandCode
│   ├── json_util.h          # 共享的 JSON 数字取值 / ISO-8601 / 天数换算
│   ├── usage_types.h        # 通用"用量窗口"结构 + 倒计时格式化
│   ├── usage_display.cpp/h  # 四个页面的绘制
│   ├── page_settings.cpp/h  # 设备上的页面开关（NVS 持久化 + 设置页）
│   ├── wifi_config.cpp/h    # WiFi 配置界面（扫描 + 软键盘）
│   ├── config.h             # 配置结构体 + 加载
│   └── hw/                  # 硬件层：显示 / 输入 / 手势 / 自动旋转 / 电源 / IMU / RTC / 扩展器
├── tools/
│   └── agent-bridge/        # PC 侧：Agent 状态桥接（详见其中的 README）
└── platformio.ini           # PlatformIO 配置
```

## 开发

```bash
pio run -e esp32-usage-monitor                      # 编译
pio run -e esp32-usage-monitor -t upload --upload-port COM5
pio run -e esp32-usage-monitor -t uploadfs --upload-port COM5
pio device monitor -p COM5 -b 115200                # 串口日志
```

- **数据源接口**都是逆向来的内部接口（`/console/api/go/status`、`/internal/billing/*`、
  `platform.deepseek.com/api/v0/*`），对方改格式就可能失效——失效时页面会显示解析失败。
- **触摸调试**：`platformio.ini` 里打开 `-DTOUCH_DEBUG=1`，串口会打印每次触摸的画布坐标。
- **串口有缓冲假象**：ESP32-S3 原生 USB CDC 会积压十几秒再成串吐出来，**别据串口静默
  判断设备卡死**；用网络侧行为（桥接日志、轮询）验证更可靠。
- 改完 `src/` 要重编译并 `upload`；改完 `data/config.json` 只要 `uploadfs`。

## 安全与隐私

- **`data/config.json` 含凭据，已在 `.gitignore` 里**，不会被提交。仓库里只有
  `data/config.json.example`（占位符）。
- 桥接的运行产物也不会被提交：`tools/agent-bridge/*.log`、`devices.json`（设备 IP）、
  `__pycache__/`。
- **给设备的是网页登录态凭据**（DeepSeek token、CommandCode/OpenCode 会话 cookie），
  它们存在板子的文件系统里。设备丢了就等于这些凭据落在别人手里 —— 请把它们当成
  "一次性、可随时吊销"的东西，丢了就去各家后台重新登录（旧凭据自然失效）。
- **桥接的 HTTP 接口是局域网明文**（`http://<PC>:8787/agent`），局域网里任何人访问
  都能看到当前状态、项目名，以及**正在执行的命令**。所以命令里的长串无分隔字符
  （API key、哈希、base64）在进这个接口**之前**就会被替换成 `…`。
- hook 只在**本机回环**（`127.0.0.1:8788`）接收事件，不对局域网开放。
- 不想用发现：`bridge.py --no-discovery`（同时关掉广播应答与主动通告），
  并在固件里写死 `agent_url`。

## 友情链接

- [TokenHub](https://github.com/2700zc/TokenHub)：Windows 上的用量悬浮窗，本项目的数据
  来源与界面参考

## License

MIT
