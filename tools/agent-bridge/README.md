# ZCode 状态桥接

让 ESP32 屏幕显示 ZCode 此刻在做什么：**正在思考 / 执行工具 / 等待授权 / 思考完成 / 空闲**。

```
ZCode 进程 ──hook(stdio)──> zcode-hook.py ──UDP 本机──> bridge.py
                                                          │
                                          HTTP /agent 轮询 │  ← 设备每秒取一次（1 秒延迟保底）
                                          UDP 推送         │  ← 状态一变立刻推（亚秒级）
                                          UDP 发现应答      │  ← 设备喊一句"桥接在哪"，这里答
                                          POST /asr (PCM)  │  ← BOOT 录音 → 火山识别 → 粘贴
                                                          ▼
                                                   ESP32 Agent 状态页
```

三件事各自独立，任何一件没做都只是降级，不会互相拖累：

| 组件 | 没启动时的后果 |
|---|---|
| `zcode-hook.py`（装在 ZCode 配置里） | 桥接收不到事件，状态永远停在 `idle` |
| `bridge.py` | 设备显示"连不上桥接"，其他页面照常 |
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
python bridge.py --announce-to 192.168.1.50     # 板子当前的 IP，喊一次之后就记住了
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

### 语音接口 `POST /asr`

设备把 16 kHz / 16 bit / 单声道 PCM 直接放在请求体里：

```text
POST /asr?tok=<act_token>
Content-Type: application/octet-stream
X-Sample-Rate: 16000
X-Channels: 1
X-Bits: 16
```

桥接补 WAV 头 → 火山识别 → 剪贴板 + Ctrl+V。成功返回
`{"ok":true,"text":"..."}`；识别失败返回 502，粘贴失败返回 409。
令牌与 `/act` 共用 `act.token`。

### 动作接口 `/act`（按键页，实验性）

固件里有一页「按键」（默认隐藏），用物理键触发：**IO18 短按 = 回车，IO18 长按
0.7 秒 = Esc**。BOOT 现在固定用于语音输入（见下一节），不再发右 Alt；`/act`
仍保留 `ralt` 动作给旧固件或手动调用。

它是会让 PC **产生动作**的接口，所以和只读的 `/agent` 分开设计：

- **token 必填**，只接受预定义动作 `name=enter` / `name=esc` / `name=ralt`；
- **默认只允许前台窗口属于 `zcode.exe`**——按键触发的那一刻焦点不可控，
  回车打进终端会直接执行已经敲好但没回车的命令；
- **回车注入前先用 UIA 把输入框聚焦**，并确认 `HasKeyboardFocus` 为真才发键。
  ZCode 的 Enter 提交处理挂在编辑器上，光标不在输入框时按键等于按在空白处；
  更糟的是确认弹窗是全局 Enter，盲发可能替用户点掉授权框。聚焦失败就不发键（422）。
- **Esc / 右 Alt 不需要聚焦**：它们是窗口级按键，直接注入。右 Alt 用
  `VK_RMENU` + `KEYEVENTF_EXTENDEDKEY`（扫描码 0x38），否则应用会把它当成左 Alt；
- 桥接关掉 `/act` 时接口返回 403，设备屏幕上会写「令牌不对/未启用」。

配置分两步：

```bash
# 1) PC：生成令牌（32 位十六进制；或在启动参数里加 --act-token）
python -c "import secrets;print(secrets.token_hex(16))" > tools/agent-bridge/act.token

# 2) 设备：把同一个值写进 data/config.json 的 "act_token"，然后重刷文件系统
pio run -t uploadfs
```

`act.token` 与 `data/config.json` 都已进 `.gitignore`，不会被提交。重启桥接后
启动横幅会打印 `act enabled for zcode.exe`；没打印说明没找到令牌，`/act` 会拒绝
所有请求。

设备上的页面要手动打开：**双击 PWR →「按键页」→ BOOT 切换 →「完成」**。
打开后 IO18 的按键动作在任意页面都生效（设置页 / WiFi 配置页里它要选行，
不触发动作）。BOOT 不参与按键页：它在主界面固定是语音输入，在设置页 / WiFi
配置页里仍用于确认和输入。按键页默认关闭，不影响其他页和用量抓取。

`GET /act?name=enter|esc|ralt&tok=<token>` 的返回：

| 状态码 | 含义 |
|---|---|
| 200 | 已注入，或 dry-run 通过 |
| 400 | `name` 不在白名单里 |
| 403 | 令牌不对，或桥接没有启用 `/act` |
| 409 | 前台窗口不是允许的进程（响应里带 `app` / `title`，桥接日志也有一行） |
| 422 | 仅 `enter`：前台窗口里找不到可聚焦的输入框，或聚焦后没有拿到键盘焦点——**没有发键** |
| 500 | `SendInput` 失败 |

**前提（只影响回车）：ZCode 的无障碍树要可见。** 平时跑过 Computer Use 之后它会被
打开并保持；冷启动的 ZCode 可能一个 Edit 都查不到，这时按键页会显示「找不到输入框」。
解决办法是给 ZCode 的启动快捷方式加 Chromium 标准开关
`--force-renderer-accessibility`（不要开调试端口）。Esc 和右 Alt 不依赖它。

前台程序默认只认 `zcode.exe`。要临时允许别的程序（例如先在记事本里试），用
`--act-app notepad.exe`；可以重复给多次。**不建议长期放开终端类窗口。**

安全的调试顺序（不聚焦、不发按键）：

```bash
# 只校验 token 和前台窗口
python bridge.py --act-dry-run
curl "http://127.0.0.1:8787/act?name=enter&tok=$(cat act.token)"
```

已知限制（与 Windows 的注入机制有关，不是桥接的 bug）：桥接以普通权限运行时，
按键进不了管理员窗口；UAC 弹窗和锁屏是安全桌面，永远到不了。焦点在浏览器、
编辑器里时，回车就是那个程序收到回车——这正是默认白名单要挡住的情况。

## 语音输入（BOOT）

按下设备右侧的 **BOOT** 开始录音，再按一次结束。设备把音频 POST 到桥接的
`/asr`，桥接调用火山「大模型录音文件识别」，识别结果用剪贴板 + Ctrl+V 粘贴到
**PC 当前前台窗口**的光标处。

> 完整的控制台开通步骤、标准版/极速版区别、请求字段和错误码见
> [docs/volcengine-asr.md](../../docs/volcengine-asr.md)。

```text
BOOT 按下 ──> ES7210 采 16 kHz / 16 bit 单声道 ──> PSRAM
BOOT 再按 ──> POST /asr（PCM）──> bridge.py ──> 火山 ASR ──> 剪贴板 + Ctrl+V
```

设备端一次最长 60 秒；录音短于 0.1 秒会提示「录音太短」。录音/识别期间屏幕
会被语音界面接管，结束后回到原来的页面。

### 配置火山

在 `tools/agent-bridge/` 下创建 `volc.key`，一行放火山语音识别的 API Key：

```bash
# 从示例复制，再把第一行替换成真实 Key
cp tools/agent-bridge/volc.key.example tools/agent-bridge/volc.key
# 或者直接写
printf '你的火山APIKey\n' > tools/agent-bridge/volc.key
```

也可以改用环境变量 `VOLC_API_KEY`，或启动参数 `--volc-api-key`。旧版控制台的
App Key + Access Key 也支持：`--volc-app-key` + `--volc-access-key`。

资源 ID 决定走哪套接口：

| 资源 ID | 接口 | 说明 |
|---|---|---|
| `volc.seedasr.auc` | 标准版 2.0：提交 + 轮询查询 | **默认**；短音频通常 1~3 秒，语言自动识别 |
| `volc.bigasr.auc` | 标准版 1.0：提交 + 轮询查询 | 老账号用；同样异步 |
| `volc.bigasr.auc_turbo` | 极速版 flash：同步返回 | 延迟最低；需要单独开通极速版 |

标准版是异步任务：桥接先 `submit` 再每 0.5 秒 `query`，最多等 25 秒。短句一般
1~3 秒出结果；60 秒长录音可能更久。极速版一次请求直接返回，延迟更低。

标准版不传语言，模型自动识别中文/英文/方言；极速版用 `--volc-language` 指定，
默认 `zh-CN`。改资源 ID 或语言需要重启桥接；只换 Key 不需要。

`volc.key` 已加入 `.gitignore`，不会被提交。密钥只在 PC 上，固件里没有。

### 使用

1. 桥接正常运行（`python bridge.py`，端口 8787）。
2. 在 PC 上点一下目标输入框，让光标停在那里。
3. 按 BOOT 开始说话，屏幕显示「正在录音」和电平条；再按 BOOT 结束。
4. 屏幕显示「正在识别」→「已输入到电脑」，文字出现在输入框里。

设备 `data/config.json` 里的 `act_token` 必须和桥接的 `act.token` 一致
（语音接口和 `/act` 共用同一个令牌）。

### 不接火山也能测

没有 API Key 时，可以用 mock 文本验证设备 → 桥接 → 返回这条链路：

```bash
python bridge.py --asr-mock-text "语音测试成功" --asr-dry-run
```

`--asr-dry-run` 只识别不粘贴，不会动你的前台窗口。去掉它就会真的粘贴。

### 相关参数

| 参数 | 作用 |
|---|---|
| `--asr-app notepad.exe` | 只在前台是该程序时粘贴（可重复；默认任意窗口） |
| `--no-restore-clipboard` | 粘贴后不恢复原来的剪贴板（默认恢复） |
| `--asr-dry-run` | 识别但不粘贴 |
| `--asr-mock-text "..."` | 跳过火山，直接返回这段文字 |
| `--asr-save-wav out.wav` | 把最近一次收到的音频存成 WAV（排查麦克风用） |

### 排查

| 现象 | 原因 |
|---|---|
| 设备显示「没找到电脑桥接」 | 桥接没跑 / 防火墙 / 不在同一网段（和 `/agent` 同一套发现） |
| 设备显示「未配置 act_token」 | 设备 `data/config.json` 里没有 `act_token`，或值为空 |
| 设备显示「没有配置火山 API Key」 | `volc.key` / `VOLC_API_KEY` 没读到；改完要重启桥接 |
| 设备显示「火山未授权 45000030…」 | Key 有效，但账号没开通对应资源：标准版 2.0 用 `volc.seedasr.auc`，极速版用 `volc.bigasr.auc_turbo`；去语音技术控制台开通/授权 |
| 设备显示「没有识别到文字」 | 录到的是静音，或火山返回空结果 |
| 文字没进输入框 | 按 BOOT 前光标不在输入框；桥接日志有 `asr paste failed` 和前台程序名 |
| 桥接日志 502 | 火山 Key、资源 ID 或语言配置不对，错误信息带 `X-Api-Status-Code` |

> 局域网里的 `/asr` 和 `/agent` 都是明文（只有令牌校验，没有加密）。不要在
> 不可信的网络里长期开着；云 API 密钥只在 PC 上，设备端拿不到它。

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
| 按键页显示「令牌不对/未启用」 | 桥接没读到 `act.token`（或 `--act-token`），或与设备 `act_token` 不一致；改完要重启桥接 |
| 按键页显示「前台不是 ZCode」 | 按键时 ZCode 不在前台。这是默认白名单在拦，不是故障；桥接日志里有实际的前台程序名 |
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
