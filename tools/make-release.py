#!/usr/bin/env python3
"""生成可发布的刷机资产（不含文件系统镜像，绝不包含 config.json）。

产物：
    dist/full.bin                     bootloader + 分区表 + 应用，写到 0x0
    dist/full.bin.sha256
    dist/esp32-usage-monitor-flash.zip  客户双击即可刷的包

用法：python tools/make-release.py [--version v2026.10.21]
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / ".pio" / "build" / "esp32-usage-monitor"
DIST = ROOT / "dist"
APP_OFFSET = 0x10000
SPIFFS_OFFSET = 0x610000


class Die(Exception):
    pass


def say(msg: str = "") -> None:
    print(msg, flush=True)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def esptool_cmd() -> tuple[list[str], int]:
    try:
        import esptool  # noqa: F401

        base = [sys.executable, "-m", "esptool"]
    except ImportError:
        pio = Path.home() / ".platformio" / "packages" / "tool-esptoolpy" / "esptool.py"
        if not pio.exists():
            raise Die("没有 esptool。请先运行：python -m pip install esptool")
        base = [sys.executable, str(pio)]

    out = subprocess.run(base + ["version"], capture_output=True, text=True)
    text = (out.stdout or "") + (out.stderr or "")
    m = re.search(r"esptool\s+v?(\d+)\.", text)
    return base, int(m.group(1)) if m else 5


def newest_src_mtime() -> float:
    files = list((ROOT / "src").rglob("*"))
    return max((p.stat().st_mtime for p in files if p.is_file()), default=0)


def check_inputs() -> dict[str, Path]:
    parts = {
        "bootloader": BUILD / "bootloader.bin",
        "partitions": BUILD / "partitions.bin",
        "firmware": BUILD / "firmware.bin",
    }
    missing = [str(p) for p in parts.values() if not p.exists()]
    if missing:
        raise Die("缺少编译产物：\n  " + "\n  ".join(missing) +
                  "\n先运行：pio run -e esp32-usage-monitor")
    fw = parts["firmware"]
    if fw.stat().st_mtime < newest_src_mtime():
        raise Die("firmware.bin 比 src/ 旧，先重新编译：pio run -e esp32-usage-monitor")
    return parts


def merge(base: list[str], major: int, parts: dict[str, Path], out: Path) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    out.unlink(missing_ok=True)
    cmd = base + [
        "--chip", "esp32s3",
        "merge-bin" if major >= 5 else "merge_bin",
        "-o", str(out),
        "--flash-mode" if major >= 5 else "--flash_mode", "qio",
        "--flash-freq" if major >= 5 else "--flash_freq", "80m",
        "--flash-size" if major >= 5 else "--flash_size", "8MB",
        "0x0", str(parts["bootloader"]),
        "0x8000", str(parts["partitions"]),
        f"{APP_OFFSET:#x}", str(parts["firmware"]),
    ]
    say("> " + " ".join(cmd))
    if subprocess.run(cmd).returncode != 0:
        raise Die("merge-bin 失败。")


def validate(image: Path, fw_size: int) -> None:
    data = image.read_bytes()
    if len(data) != APP_OFFSET + fw_size:
        raise Die(f"镜像大小异常：{len(data)}，期望 {APP_OFFSET + fw_size}")
    if data[0] != 0xE9:
        raise Die("0x0 不是 bootloader 镜像（缺少 0xE9 头）")
    if data[APP_OFFSET] != 0xE9:
        raise Die("0x10000 不是应用镜像（缺少 0xE9 头）")
    if data[0x8000:0x8002] != b"\xaa\x50":
        raise Die("0x8000 不是分区表（缺少 0xAA50 魔数）")
    # 分区表条目 32 字节：magic(2) type(1) subtype(1) offset(4) size(4) label(16) flags(4)
    app_off = None
    spiffs_off = None
    for i in range(0x8000, 0x8000 + 0xC00, 0x20):
        entry = data[i:i + 0x20]
        if entry[:2] != b"\xaa\x50":
            break
        ptype, subtype = entry[2], entry[3]
        off = int.from_bytes(entry[4:8], "little")
        if ptype == 0x00 and subtype == 0x10 and app_off is None:  # app / ota_0
            app_off = off
        elif ptype == 0x01 and subtype == 0x82:  # data / spiffs
            spiffs_off = off
    if app_off != APP_OFFSET:
        raise Die(f"分区表 app0 偏移是 {app_off!r}，与本脚本的 {APP_OFFSET:#x} 不一致")
    if spiffs_off != SPIFFS_OFFSET:
        raise Die(f"分区表 spiffs 偏移是 {spiffs_off!r}，与本脚本的 {SPIFFS_OFFSET:#x} 不一致")


def customer_readme(version: str) -> str:
    return f"""ESP32-S3 AI 用量监视器 - 刷机包 {version}

适用板型：Waveshare ESP32-S3-Touch-AMOLED-2.16（ESP32-S3R8 / 16MB Flash）
不要刷到 1.8 寸、1.75 寸或 ESP32-C6 的版本上；刷错通常只是黑屏，重刷正确固件即可。

步骤：
1. 安装 Python 3（已装可跳过）：
   https://www.python.org/downloads/windows/
   安装时勾选 "Add python.exe to PATH"。
2. 用 USB-C 数据线连接板子，双击 flash.bat。
   第一次会自动下载 esptool（约 7MB），之后不再需要。
3. 看到 "刷入完成" 后，板子会自动重启并进入 WiFi 配置界面：
   PWR / IO18 选择，BOOT 确认。

日常使用：
- 左右滑动切换页面；PWR（中键）随时刷新用量。
- 双击 PWR（中键）进入页面开关：PWR / IO18 选行，BOOT 切换，
  选「完成」再按 BOOT 保存并返回。关掉的页面不再显示，也不再请求用量
  （过期凭据不会反复报错）。这一页里还有一行「WiFi 配置」，选中按 BOOT
  可以重新配网，退出后回到设置页。
- 单击 PWR 刷新用量（会先等 0.4 秒确认不是双击）。
- 短按 BOOT：按键页关闭时（默认）进入 WiFi 配置界面；开启按键页后改为
  发送电脑的右 Alt，WiFi 配置从设置页的「WiFi 配置」行进入。

这个包只包含固件，不包含任何账号凭据。
用量页（DeepSeek / OpenCode / CommandCode）需要另外填凭据，
见仓库 README 的"配置详解"。

如果双击后找不到串口：换一根支持数据传输的 USB-C 线；
仍然不行就按住 BOOT，点一下 RESET，松开 BOOT，再运行一次。
"""


def make_zip(image: Path, version: str) -> Path:
    zip_path = DIST / "esp32-usage-monitor-flash.zip"
    root = "esp32-usage-monitor-flash"
    files = {
        "flash.bat": ROOT / "tools" / "flash.bat",
        "flash.py": ROOT / "tools" / "flash.py",
        "full.bin": image,
    }
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, src in files.items():
            z.write(src, f"{root}/{name}")
        z.writestr(f"{root}/full.bin.sha256",
                   f"{sha256(image)}  full.bin\n")
        z.writestr(f"{root}/README.txt", customer_readme(version))
        names = z.namelist()
    if any("config.json" in n or "littlefs" in n for n in names):
        raise Die("发布包里出现了配置文件，已中止。")
    return zip_path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="v" + dt.date.today().isoformat())
    args = ap.parse_args()

    DIST.mkdir(exist_ok=True)
    base, major = esptool_cmd()
    parts = check_inputs()

    image = DIST / "full.bin"
    merge(base, major, parts, image)
    validate(image, parts["firmware"].stat().st_size)

    digest = sha256(image)
    (DIST / "full.bin.sha256").write_text(f"{digest}  full.bin\n", encoding="ascii")

    zip_path = make_zip(image, args.version)

    say()
    say(f"版本：{args.version}")
    say(f"镜像：{image}  {image.stat().st_size / 1024 / 1024:.2f} MB")
    say(f"SHA256：{digest}")
    say(f"客户包：{zip_path}  {zip_path.stat().st_size / 1024 / 1024:.2f} MB")
    say()
    say("发布（只需一次）：")
    say("  1. 打开 https://github.com/2700zc/esp32-usage-monitor/releases/new")
    say(f"  2. Tag 填 {args.version}，标题随便写")
    say("  3. 上传 dist/ 下的这三个文件：")
    say("       full.bin")
    say("       full.bin.sha256")
    say("       esp32-usage-monitor-flash.zip")
    say("  4. Publish release")
    say()
    say("客户只需要下载 esp32-usage-monitor-flash.zip。")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Die as e:
        say(f"失败：{e}")
        sys.exit(1)
