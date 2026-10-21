# 刷入固件

适用硬件：**Waveshare ESP32-S3-Touch-AMOLED-2.16**（ESP32-S3R8 / 16MB Flash /
8MB OPI PSRAM）。同系列的 1.8 寸、1.75 寸和 ESP32-C6 版本不能刷这个包。

## 最终用户：三步

1. 从 [Releases](https://github.com/2700zc/esp32-usage-monitor/releases/latest)
   下载 `esp32-usage-monitor-flash.zip`（约 1.1MB）
2. 解压，双击 `flash.bat`
3. 看到“刷入完成”后，设备自动重启并进入 WiFi 配置界面

需要 **Python 3**。脚本第一次运行会自动 `pip install esptool`（约 7MB），之后不再
下载。不需要 PlatformIO、VS Code、ESP-IDF，也不需要装串口驱动（Windows 10/11 自带）。

macOS / Linux：

```bash
python3 flash.py --file full.bin
```

> 为什么不是浏览器一键刷：这块板子是原生 USB CDC，WebSerial 在复位后会重新枚举，
> 浏览器方案不稳定；脚本复用客户已有的 Python，额外下载只有 7MB，成本最低。

## 这个包写了什么

| 地址 | 内容 |
|---|---|
| `0x0` | bootloader |
| `0x8000` | 分区表 |
| `0x10000` | 应用（约 1.6MB） |

**不包含 littlefs 文件系统**，也不包含 `data/config.json` 或任何 token。首次开机时
固件发现文件系统为空会自动格式化，然后进入 WiFi 配置界面。

整包写入会用 `0xFF` 填满 `0x0`–`0x10000` 的空隙，因此 **WiFi 配网信息会被清掉**，
需要重新配网；`0x610000` 起的 littlefs 不在写入范围内，已经填过的用量凭据会保留。
要彻底恢复出厂状态（连凭据一起清）用 `--erase`。

## 命令行 / 给 AI 用

```bash
python tools/flash.py --dry-run           # 只找串口、读芯片信息，不写入
python tools/flash.py --yes               # 自动找口、预检、写入
python tools/flash.py --file dist/full.bin --yes
python tools/flash.py --erase --yes       # 整片擦除后写入（清 WiFi + 凭据）
```

脚本按这个顺序找镜像：`--file` → 脚本旁边的 `full.bin` → 仓库的 `dist/full.bin` →
从 latest release 下载并校验 `.sha256`。串口按 **VID 303A** 自动识别，蓝牙虚拟串口
（COM3/COM4 之类）不会被选中。

给 AI 的硬约束：

- **不要为了刷写安装 PlatformIO**，也不要重新编译；直接跑上面的脚本。
- **不要改 flash size**。芯片是 16MB，但镜像和分区表按 8MB 构建；`flash-id` 读到
  16MB 是正常的，改成 16MB 反而与分区表不符。
- **不要上传或发布 `littlefs.bin`、`data/config.json`**。前者会把真实 token 打进
  公开镜像。
- 源码路径下 **不要把 `-t upload` 和 `-t uploadfs` 写在同一条命令里**：文件系统会
  顶掉固件且不报错。
- 找不到串口时先换数据线；仍然不行就按住 **BOOT**、点一下 **RESET**、松开 **BOOT**
  再试。这一步只是让芯片进入下载模式，不会改数据。

## 故障排查

| 现象 | 处理 |
|---|---|
| 双击后提示没有 Python | 按提示装 Python 3，安装时勾选 `Add python.exe to PATH` |
| 找不到串口 | 换支持数据传输的 USB-C 线；确认板子已上电；BOOT+RESET 进下载模式 |
| 列出 COM3/COM4 | 那是蓝牙虚拟串口，不能刷；脚本只认 VID 303A |
| `连不上芯片` | 串口被串口监视器占用；或按 BOOT+RESET 后重试 |
| `目标不是 ESP32-S3` | 插错了设备，脚本已停止，未写入任何数据 |
| 检测到 Flash < 8MB | 不是这块板子，脚本已停止 |
| 写入中途失败 | 脚本会自动降到 115200 重试；仍失败就换线/换口 |
| 刷完黑屏 | 确认是 2.16 寸版本；1.8/1.75 的固件不通用 |
| 刷完没有 WiFi 配置界面 | 断电重开；文件系统为空会自动格式化，首次启动多等几秒 |
| 想保留设置升级 | 脚本默认不整片擦除，只覆盖固件区；WiFi 仍需重配 |

## 维护者：生成与发布资产

```bash
pio run -e esp32-usage-monitor          # 先编译（冷编译约 29 分钟）
python tools/make-release.py            # 生成 dist/ 三个文件
```

`make-release.py` 会合并 `bootloader.bin` + `partitions.bin` + `firmware.bin`，检查
分区表偏移和应用镜像头，生成 SHA256，并打一个只含 `flash.bat`、`flash.py`、
`full.bin`、`README.txt` 的客户 zip；脚本会拒绝把 `config.json` 或 littlefs 打进包。

发布时在 GitHub Releases 新建 tag（例如 `v2026.10.21`），上传 `dist/` 下的：

- `full.bin`
- `full.bin.sha256`
- `esp32-usage-monitor-flash.zip`

`python tools/flash.py` 默认从 `releases/latest/download/full.bin` 下载，所以这三个
文件名不要改。
