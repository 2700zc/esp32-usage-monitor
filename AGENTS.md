# AGENTS.md

## 用户要“刷固件”时

先读 [FLASHING.md](FLASHING.md)，然后运行：

```bash
python tools/flash.py --yes
```

不要为了刷写安装 PlatformIO，也不要重新编译。脚本会自动找串口、校验镜像、
必要时安装 esptool。

## 开发

- 编译：`pio run -e esp32-usage-monitor`。冷编译约 29 分钟；`src/` 改动通常
  1–2 分钟。日志重定向到文件后 grep `[SUCCESS]`/`[FAILED]`，不要用 `| tail`
  判断退出码。
- 源码烧写：`pio run -t upload`；文件系统：`pio run -t uploadfs`。两条命令
  必须分开跑。
- 发布：`pio run` 后运行 `python tools/make-release.py`，只上传 `dist/` 下的
  `full.bin`、`full.bin.sha256`、`esp32-usage-monitor-flash.zip`。
- 绝不提交或上传 `data/config.json`、`tools/agent-bridge/devices.json`、
  `tools/agent-bridge/*.log`、`littlefs.bin`。发布镜像不包含文件系统。
