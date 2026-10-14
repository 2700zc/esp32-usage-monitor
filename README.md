# ESP32-S3 用量监视器

在 Waveshare ESP32-S3-Touch-AMOLED-2.16 开发板上显示 AI 编程服务用量的桌面小仪表盘。
左右滑动屏幕在 ZCode 状态和三个服务商之间切换。

## 功能

- **四页，滑动切换**（页面顺序 ZCode 状态 → OpenCode Go → CommandCode → DeepSeek）：
  - **ZCode 状态**：正在思考 / 执行工具 / 等待授权 / 思考完成 / 空闲；
    第二行是工具名，第三行是它此刻在做什么（Bash 的命令、Edit 的文件名），
    不在执行时显示会话所属的项目名
  - **OpenCode Go**：滚动用量 / 每周用量 / 每月用量（百分比 + 距重置倒计时）
  - **CommandCode**：滚动用量 / 每周用量 / 每月用量（同上）
  - **DeepSeek**：今日请求数、今日 Tokens（完整千分位）、账户余额（低于 5 元变红警示）
- 用量达到 100% 时数字变红
- **左右滑动**切页，到底循环；底部圆点指示当前页
- 用量每 60 秒刷新一轮；按 **PWR（中键）** 可立即刷新
- ZCode 状态每秒刷新一次，状态一变立即推送（亚秒级）
- 深色 AMOLED 界面：纯黑背景 + 深灰圆角卡片 + 各服务商品牌色
- WiFi 屏幕配置界面（扫描网络 + 软键盘输密码），首次配置后开机自动连接
- 自动连接超过 10 秒失败会回到 WiFi 配置界面重新选择
- 配置存储在 LittleFS 文件系统

只配置了其中一家的凭据时，只有那一页，滑动无效果。

## 硬件

- **开发板**: Waveshare ESP32-S3-Touch-AMOLED-2.16
- **屏幕**: 480×480 AMOLED (CO5300 驱动)
- **触摸**: CST9220
- **PSRAM**: 8MB | **Flash**: 16MB

## 环境搭建

1. 安装 [PlatformIO](https://platformio.org/)
2. 克隆仓库：
   ```bash
   git clone https://github.com/2700zc/esp32-usage-monitor.git
   cd esp32-usage-monitor
   ```

## 配置

复制配置模板并填入你的信息：

```bash
cp data/config.json.example data/config.json
```

`data/config.json` 的字段（模板里每个字段上方都写了中文注释，直接看文件即可）：

```json
{
  "ds_token": "DeepSeek Bearer token",
  "oc_api_key": "oc_sk_xxxxxxxxxxxx_xxxxxxxxxxxxxxxx",
  "oc_workspace_id": "wrk_XXXXXXXXXXXX",
  "oc_cookie": "__Host-console_session=st_xxxxxxxx-...",
  "cc_cookie": "CommandCode 会话 token",
  "agent_url": "http://192.168.1.10:8787/agent"
}
```

**任意一项留空（或只填了空白字符）就不显示对应的页面**，页面总数自动减少，滑动切页照常可用，也不影响其他页面：

- 只填 `ds_token` → 1 页
- 填 `ds_token` + `cc_cookie` → 2 页
- 三家的凭据都填齐 → 4 页（含 ZCode 状态页）

`agent_url` 需要在 PC 上跑一个桥接进程，见下文「ZCode 状态页」。

OpenCode Go 的凭据二选一，任意一条走通就生成页面：

- **`oc_api_key`（推荐）**：Go 套餐的 API key，形如 `oc_sk_xxxxxxxxxxxx_...`，长期有效。
  在 opencode 控制台生成后填这一项即可，不需要 workspace_id 和 cookie。
- **`oc_workspace_id` + `oc_cookie`**：浏览器会话，会过期，两项必须同时填。

### OpenCode Go：`oc_api_key`（推荐）

1. 登录 `opencode.ai` 控制台，进入 Go / API keys 页面生成一个 key（`oc_sk_` 开头）
2. 原样填进 `oc_api_key`

固件以 `Authorization: Bearer <key>` 请求用量接口，实测连 `x-org-id` 与 Referer 都不需要。

### OpenCode Go：`oc_workspace_id` + `oc_cookie`（备选）

1. 登录 `opencode.ai`，打开控制台页面，地址形如
   `https://opencode.ai/console/wrk_XXXXXXXXXXXX/models`，URL 里那段 `wrk_...` 就是 `oc_workspace_id`
2. F12 → 网络 (Network)，刷新页面，找 `/console/api/go/status` 的请求
3. 复制请求头 `cookie` 里 `__Host-console_session=` 后面那一段（形如 `st_xxxxxxxx-xxxx-...`），
   连名字带值一起填进 `oc_cookie` 也行——固件只认这个 cookie 名
4. 整串 cookie、或右键该请求「Copy as cURL」得到的整条命令直接粘进去同样可以，
   固件会自己把 session 挑出来

> 这条路的 session 会过期，届时该页显示"凭据已失效"，重新复制即可，设备上无法登录。
> 旧的 `auth=Fe26.2**`（配合 `/_server` 接口用的那个 cookie）已经失效，填了会一直报错——那个接口
> 现在只回一个跳登录页的 302。

### 关于配置文件里的注释

`data/config.json` 里带 `//` 中文注释便于手改。JSON 标准不支持注释，设备端是靠
`platformio.ini` 里的 `-DARDUINOJSON_ENABLE_COMMENTS=1` 才认识它们——**删掉这个编译选项
会导致整个配置解析失败、所有页面都消失**，别去掉它。（同一个原因：在头文件里局部
`#define` 不行，会让不同编译单元看到不一致的配置。）

### DeepSeek：`ds_token`

1. 登录 [platform.deepseek.com/usage](https://platform.deepseek.com/usage)
2. 打开浏览器开发者工具 (F12) → 网络 (Network)
3. 刷新页面，找任意 `api/v0/` 开头的请求
4. 复制请求头 `authorization` 中 `Bearer ` **后面**的部分（不要带 `Bearer ` 前缀）

### OpenCode Go：`oc_workspace_id` / `oc_cookie`

1. 登录 `opencode.ai`，打开控制台页面，地址形如
   `https://opencode.ai/console/wrk_XXXXXXXXXXXX/models`，URL 里那段 `wrk_...` 就是 `oc_workspace_id`
2. F12 → 网络 (Network)，刷新页面，找 `/console/api/go/status` 的请求
3. 复制请求头 `cookie` 里 `__Host-console_session=` 后面那一段（形如 `st_xxxxxxxx-xxxx-...`），
   连名字带值一起填进 `oc_cookie` 也行——固件只认这个 cookie 名
4. 整串 cookie、或右键该请求「Copy as cURL」得到的整条命令直接粘进去同样可以，
   固件会自己把 session 挑出来

> `oc_cookie` 是登录态凭据，会过期（表现为该页显示"cookie 已过期"）。届时重新复制即可，设备上无法登录。
> 旧的 `auth=Fe26.2**`（配合 `/_server` 接口用的那个 cookie）已经失效，填了会一直报错——那个接口
> 现在只回一个跳登录页的 302。

### CommandCode：`cc_cookie`

1. 登录 `commandcode.ai`，F12 → 应用 (Application) → Cookie
2. 找到 `__Secure-commandcode_prod_.session_token`，复制它的**值**（不要带 cookie 名）

## ZCode 状态页（`agent_url`）

让屏幕显示 ZCode 此刻在做什么。第一次配置需要两步（都在 PC 上，只需做一次）：

```bash
# 1. 装 hook：让 ZCode 把事件报给桥接（会先备份 ~/.zcode/cli/config.json）
python tools/agent-bridge/install-hooks.py
#    然后重启 ZCode —— hook 是启动时读取的，当前已开着的会话不会生效

# 2. 常驻跑桥接（也可以直接双击 tools/agent-bridge/start-bridge.bat）
python tools/agent-bridge/bridge.py
```

把桥接打印的地址填进 `data/config.json` 的 `agent_url`，重刷文件系统即可。
细节、接口定义和排查表见 [`tools/agent-bridge/README.md`](tools/agent-bridge/README.md)。

> **桥接必须在 ZCode 的进程树之外启动。** 从 ZCode 会话里拉起来的进程会随 ZCode
> 重启一起被杀，设备就显示「连不上桥接」。双击 `tools/agent-bridge/start-bridge.bat`
> 是对的；开机自启也已装好（启动文件夹里一条 `ZCode agent bridge.lnk` → 隐藏启动，
> 日志写 `tools/agent-bridge/bridge.log`）。不需要时删掉那个快捷方式即可。

**为什么需要一个 PC 端进程**：ZCode 没有向外发布状态的接口，最接近的扩展点是 hook
（7 个事件，覆盖"提交提示/调用工具/请求授权/本轮结束"）。hook 是**同步执行**的，
脚本里不能做网络请求，所以它只往本机发一个 UDP 包（约 1ms），由桥接进程汇聚成一份
状态，设备再去取。

**两条链路同时工作**：设备每秒轮询一次 `agent_url`（保底 1 秒内的延迟），桥接从轮询
请求的源地址学到设备位置后，状态一变就再直接 UDP 推一份过去（亚秒级）。所以设备侧
不需要固定 IP，桥接侧也不用配置设备地址。状态页"正在执行"卡片右侧会显示这一份是
「实时」（推送）还是「轮询」取的，排查链路时一眼能分辨。

**状态与来源事件的对应**：`UserPromptSubmit` → 正在思考；`PreToolUse` → 执行工具（带
工具名）；`PermissionRequest` → 等待授权；`Stop` → 思考完成。同时开多个会话或子智能体
时按"最新的优先、需要人管的优先"取一个显示。

**第三行显示什么**：执行中显示动作本体——Bash 是命令、Edit/Read 是文件名、Grep 是
pattern、等待授权时是它想执行的东西；不在执行时显示项目名（会话的工作目录名），这样
同时开几个窗口也能一眼看出这块屏幕在说哪个项目。命令里的长串无分隔字符（API key、
哈希）会被替换成 `…` 再上屏，也不会进那个局域网明文接口。

> 没有 token 级的流式事件，所以粒度就到"轮次/工具"这一层——显示的是"正在思考"，
> 不是逐字输出。桥接不在、PC 换网络导致 IP 变了的时候，该页显示「连不上桥接」，
> 其他三页不受影响。

## 编译与上传

```bash
# 编译
pio run -e esp32-usage-monitor

# 上传固件
pio run -e esp32-usage-monitor -t upload

# 上传文件系统（包含 config.json）
pio run -e esp32-usage-monitor -t uploadfs
```

> **别把 `-t upload` 和 `-t uploadfs` 写在一条命令里。** 实测（PlatformIO Core 6.1.19）
> 两个目标同时指定时文件系统上传会顶掉固件上传，日志里只有 `Uploading ...littlefs.bin`
> 而没有 `firmware.bin`，固件实际没刷进去且不会报错。请分两条命令依次执行，并确认日志里
> 出现了 `Wrote ... at 0x00010000`。

指定端口（板子插在电脑上时）加 `--upload-port COM5`。

## 使用说明

1. 首次启动进入 WiFi 配置界面（自动扫描附近网络）
2. 三个按键操作：
   - **PWR（中）**: 列表中选下一个
   - **IO18（左）**: 列表中选上一个
   - **BOOT（右）**: 确认进入密码输入
3. 软键盘输入密码：PWR/IO18 移动光标（长按连发），BOOT 输入字符；底行功能键为 模式切换 / 删除 / 空格 / OK
4. 连接成功后进入用量页面，自动刷新
5. **左右滑动**切换服务商页面，底部圆点指示当前位置
6. 之后开机自动连接已保存的 WiFi；超过 10 秒连不上会自动回到 WiFi 配置界面
7. 随时按 **BOOT** 可重新进入 WiFi 配置
8. 用量页面按 **PWR** 立即刷新一轮

### 触摸调试

滑动阈值（主方向 35px、时长 800ms 以内、主方向至少是次方向的 1.5 倍）都在画布坐标下，
与板型无关。每次识别出的滑动都会在串口打印位移，用来核对轴向与方向：

```
swipe: dx=12 dy=-58 -> next
```

**关于轴向**：屏幕经 MADCTL 旋转了 90°（`BOARD_CO5300_MADCTL` 的 MV 位），触控上报的
x/y 与画布逻辑坐标是**转置**的——`input.cpp` 里那句 `setMirrorXY(true, true)` 只等价于
180° 镜像，做不到转置。所以判定用的是上报的 y 位移。如果方向反了，把
`src/hw/gesture.cpp` 末尾 `dy < 0 ? SwipeDir::Left : SwipeDir::Right` 的两个分支对调即可。

要连未识别成滑动的触摸也看到，在 `platformio.ini` 的 `build_flags` 里打开
`-DTOUCH_DEBUG=1`，然后用 `pio device monitor` 观察每次触摸的画布坐标。

## 刷新机制

用量每 60 秒触发一轮刷新，把已配置的服务商各抓一遍。三家**分摊到连续的多帧**里完成，
每帧只抓一个，帧间照常重绘并响应触摸，因此最长无响应时间约等于单个服务商（1~1.5 秒），
而不是三家之和。某一家的请求失败只影响它自己那一页，其他页保留上一次的数据。

**ZCode 状态页不参与这个轮次**：它按秒轮询（桥接在局域网里，一次请求几十毫秒），
因为状态是"此刻"的事，塞进 60 秒轮次里等于最多滞后一分钟。桥接连不上时按失败次数
退避到最长 30 秒一次——否则每秒一次的连接超时会把滑动和重绘卡死。

## 项目结构

```
├── data/
│   ├── config.json          # 运行时配置（已 gitignore）
│   └── config.json.example  # 配置模板
├── src/
│   ├── main.cpp             # 主循环：WiFi 状态机 + 页面状态 + 分批刷新 + 状态轮询
│   ├── http_client.cpp/h    # HTTPS / 明文 HTTP GET（共用按 Content-Length 精确读正文的核心）
│   ├── usage_types.h        # 通用「用量窗口」结构 + 倒计时格式化
│   ├── json_util.h          # 共享：JSON 数字取值 / ISO-8601 → epoch / 重置倒计时
│   ├── agent_client.cpp/h   # ZCode 状态客户端（轮询 + UDP 推送共用解析）
│   ├── deepseek_client.cpp/h    # DeepSeek 用量/余额客户端
│   ├── opencode_client.cpp/h    # OpenCode Go 客户端（console/api/go/status）
│   ├── commandcode_client.cpp/h # CommandCode 客户端（credits + subscriptions）
│   ├── usage_display.cpp/h  # 四个页面的绘制 + 页码圆点
│   ├── wifi_config.cpp/h    # WiFi 配置界面（扫描 + 软键盘）
│   ├── config.h             # 配置结构体定义 + 加载
│   └── hw/                  # 硬件驱动层
│       ├── display.cpp/h    # AMOLED 显示驱动
│       ├── input.cpp/h      # 按键和触摸输入
│       ├── gesture.cpp/h    # 滑动手势识别
│       ├── power.cpp/h      # 电源管理 (AXP2101)
│       ├── imu.cpp/h        # 加速度计
│       ├── rtc.cpp/h        # 实时时钟
│       ├── expander.cpp/h   # IO 扩展器
│       ├── border.cpp/h     # 边框绘制
│       ├── net.cpp/h        # 网络工具
│       ├── pins.h           # 引脚定义
│       └── hw.cpp/h         # 硬件初始化
├── tools/
│   └── agent-bridge/        # PC 侧：ZCode 状态桥接（bridge.py + hook + 安装脚本）
└── platformio.ini           # PlatformIO 配置
```

## 注意事项

- `data/config.json` 包含认证信息，已被 `.gitignore` 排除，不会上传到仓库
- 每次修改 `config.json` 后需要重新上传文件系统：`pio run -e esp32-usage-monitor -t uploadfs`
- WiFi 凭据保存在 NVS 分区，与文件系统相互独立
- **OpenCode 与 CommandCode 用的是逆向出来的内部接口**（`/console/api/go/status`、
  `/internal/billing/*`），不是公开 API，对方调整格式后解析可能失效
- OpenCode 的会话 cookie（`__Host-console_session`）过期后，该页显示"cookie 已过期"；
  若填的 cookie 里根本没有这个名字，则显示"cookie 缺少 session"

## 友情链接
- Windows 上的用量显示悬浮窗：[TokenHub](https://github.com/2700zc/TokenHub)，本项目的数据来源与界面参考

## License

MIT
