# ESP32-S3 用量监视器

在 Waveshare ESP32-S3-Touch-AMOLED-2.16 开发板上显示 AI 编程服务用量的桌面小仪表盘。
左右滑动屏幕在三个服务商之间切换。

## 功能

- **三个服务商，滑动切换**（页面顺序 OpenCode Go → CommandCode → DeepSeek）：
  - **OpenCode Go**：滚动用量 / 每周用量 / 每月用量（百分比 + 距重置倒计时）
  - **CommandCode**：滚动用量 / 每周用量 / 每月用量（同上）
  - **DeepSeek**：今日请求数、今日 Tokens（完整千分位）、账户余额（低于 5 元变红警示）
- 用量达到 100% 时数字变红
- **左右滑动**切页，到底循环；底部圆点指示当前页
- 每 60 秒刷新一轮；按 **PWR（中键）** 可立即刷新
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
  "oc_workspace_id": "wrk_XXXXXXXXXXXX",
  "oc_server_id": "OpenCode server-fn id",
  "oc_cookie": "oc_locale=zh; auth=Fe26.2**...",
  "cc_cookie": "CommandCode 会话 token"
}
```

**任意一项留空（或只填了空白字符）就不显示该服务商的页面**，页面总数自动减少，滑动切页照常可用，也不影响其他服务商。留空只是界面层面的事，不需要改动代码：

- 只填 `ds_token` → 1 页
- 填 `ds_token` + `cc_cookie` → 2 页
- 三家都填齐 → 3 页

OpenCode Go 是唯一的例外：它的 `oc_workspace_id` / `oc_server_id` / `oc_cookie` **三项必须同时填齐**才会生成页面，缺任意一项整页隐藏（少任何一项都发不出请求）。

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

### OpenCode Go：`oc_workspace_id` / `oc_server_id` / `oc_cookie`

1. 登录 `opencode.ai`，打开你的 workspace 的 Go 页面，地址形如
   `https://opencode.ai/workspace/wrk_XXXXXXXXXXXX/go`，URL 里那段 `wrk_...` 就是 `oc_workspace_id`
2. F12 → 网络，刷新页面，找 `_server?id=...` 的请求
3. 请求 URL 里 `id=` 后面那串 64 位十六进制就是 `oc_server_id`
4. 复制请求头 `cookie` 的**完整值**，作为 `oc_cookie`

> cookie 是登录态凭据，会过期（表现为该页显示"cookie 已过期"）。届时重新复制即可，设备上无法登录。

### CommandCode：`cc_cookie`

1. 登录 `commandcode.ai`，F12 → 应用 (Application) → Cookie
2. 找到 `__Secure-commandcode_prod_.session_token`，复制它的**值**（不要带 cookie 名）

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

每 60 秒触发一轮刷新，把已配置的服务商各抓一遍。三家**分摊到连续的多帧**里完成，
每帧只抓一个，帧间照常重绘并响应触摸，因此最长无响应时间约等于单个服务商（1~1.5 秒），
而不是三家之和。某一家的请求失败只影响它自己那一页，其他页保留上一次的数据。

## 项目结构

```
├── data/
│   ├── config.json          # 运行时配置（已 gitignore）
│   └── config.json.example  # 配置模板
├── src/
│   ├── main.cpp             # 主循环：WiFi 状态机 + 页面状态 + 分批刷新
│   ├── http_client.cpp/h    # 共享的 HTTPS GET（按 Content-Length 精确读正文）
│   ├── usage_types.h        # 通用「用量窗口」结构 + 倒计时格式化
│   ├── deepseek_client.cpp/h    # DeepSeek 用量/余额客户端
│   ├── opencode_client.cpp/h    # OpenCode Go 客户端（响应是 JS，用手写扫描器）
│   ├── commandcode_client.cpp/h # CommandCode 客户端（credits + subscriptions）
│   ├── usage_display.cpp/h  # 三个页面的绘制 + 页码圆点
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
└── platformio.ini           # PlatformIO 配置
```

## 注意事项

- `data/config.json` 包含认证信息，已被 `.gitignore` 排除，不会上传到仓库
- 每次修改 `config.json` 后需要重新上传文件系统：`pio run -e esp32-usage-monitor -t uploadfs`
- WiFi 凭据保存在 NVS 分区，与文件系统相互独立
- **OpenCode 与 CommandCode 用的是逆向出来的内部接口**（`/_server`、`/internal/billing/*`），
  不是公开 API，对方调整格式后解析可能失效
- OpenCode 返回的不是 JSON，而是 SolidStart 的服务端函数文本，解析器据此手写

## 友情链接
- Windows 上的用量显示悬浮窗：[TokenHub](https://github.com/2700zc/TokenHub)，本项目的数据来源与界面参考

## License

MIT
