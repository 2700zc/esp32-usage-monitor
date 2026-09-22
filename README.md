# ESP32-S3 DeepSeek 用量监视器

在 Waveshare ESP32-S3-Touch-AMOLED-2.16 开发板上显示 DeepSeek 平台用量的桌面小仪表盘。

## 功能

- 显示**今日请求数**、**今日 Tokens**（完整千分位数字）、**账户余额**（大字号，低于 5 元变红警示）
- 每 30 秒自动刷新，按 **PWR（中键）** 可手动立即刷新
- 深色 AMOLED 界面：纯黑背景 + 深灰圆角卡片 + 蓝/青品牌色
- WiFi 屏幕配置界面（扫描网络 + 软键盘输密码），首次配置后开机自动连接
- 自动连接超过 10 秒失败会回到 WiFi 配置界面重新选择
- 配置存储在 LittleFS 文件系统

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

编辑 `data/config.json`：

```json
{
  "ds_token": "你的 DeepSeek Bearer token"
}
```

### 获取 token

1. 登录 [platform.deepseek.com/usage](https://platform.deepseek.com/usage)
2. 打开浏览器开发者工具 (F12) → 网络 (Network)
3. 刷新页面，找任意 `api/v0/` 开头的请求
4. 复制请求头 `authorization` 中 `Bearer ` **后面**的部分（不要带 `Bearer ` 前缀）

> token 是登录态凭据，有效期有限，过期后页面数据会显示"获取失败"，重新获取并更新配置即可。

## 编译与上传

```bash
# 编译
pio run -e esp32-usage-monitor

# 上传固件
pio run -e esp32-usage-monitor -t upload

# 上传文件系统（包含 config.json）
pio run -e esp32-usage-monitor -t uploadfs

# 同时上传固件和文件系统
pio run -e esp32-usage-monitor -t upload -t uploadfs
```

## 使用说明

1. 首次启动进入 WiFi 配置界面（自动扫描附近网络）
2. 三个按键操作：
   - **PWR（中）**: 列表中选下一个
   - **IO18（左）**: 列表中选上一个
   - **BOOT（右）**: 确认进入密码输入
3. 软键盘输入密码：PWR/IO18 移动光标（长按连发），BOOT 输入字符；底行功能键为 模式切换 / 删除 / 空格 / OK
4. 连接成功后进入用量页面，自动每 30 秒刷新
5. 之后开机自动连接已保存的 WiFi；超过 10 秒连不上会自动回到 WiFi 配置界面
6. 随时按 **BOOT** 可重新进入 WiFi 配置
7. 用量页面按 **PWR** 手动刷新一次

## 项目结构

```
├── data/
│   ├── config.json          # 运行时配置（已 gitignore）
│   └── config.json.example  # 配置模板
├── src/
│   ├── main.cpp             # 主循环：WiFi 状态机 + 30 秒刷新
│   ├── deepseek_client.cpp/h# DeepSeek 用量/余额 API 客户端
│   ├── usage_display.cpp/h  # 用量仪表盘界面
│   ├── wifi_config.cpp/h    # WiFi 配置界面（扫描 + 软键盘）
│   ├── config.h             # 配置结构体定义
│   └── hw/                  # 硬件驱动层
│       ├── display.cpp/h    # AMOLED 显示驱动
│       ├── input.cpp/h      # 按键和触摸输入
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

## 友情链接
- Windows 上的用量显示悬浮窗：[TokenHub](https://github.com/2700zc/TokenHub)，配置好之后开箱即用

## License

MIT
