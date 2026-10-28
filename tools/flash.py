#!/usr/bin/env python3
"""把预编译固件刷进 Waveshare ESP32-S3-Touch-AMOLED-2.16。

只依赖 Python 3 和 esptool（缺失时自动 pip 安装，约 7MB）。
不需要 PlatformIO、VS Code、ESP-IDF 或任何驱动。

用法：
    python tools/flash.py                 # 自动找串口、自动找镜像
    python tools/flash.py --erase         # 先整片擦除（会清 WiFi 和所有设置）
    python tools/flash.py --dry-run       # 只预检并打印命令，不写入
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

REPO = "2700zc/esp32-usage-monitor"
API_LATEST = f"https://api.github.com/repos/{REPO}/releases/latest"
# github.com 在国内经常连不上，而 api.github.com 通常可用；资产下载统一走 API。
USER_AGENT = "esp32-usage-monitor-flash"
HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent
DEFAULT_LOCAL = REPO_ROOT / "dist" / "full.bin"
ESP_VID = 0x303A


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


def _fetch(url: str, timeout: int, accept: str = "application/octet-stream") -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": accept})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def release_assets(names: list[str]) -> dict[str, str]:
    """一次取回 latest release 的资产地址；公开仓库不需要 token。"""
    try:
        release = json.loads(_fetch(API_LATEST, 30, "application/vnd.github+json"))
    except Exception as e:  # noqa: BLE001
        raise Die(f"查询 GitHub Release 失败：{e}\n请手动下载 full.bin，或用 --file 指定路径。") from e

    by_name = {a.get("name"): a["url"] for a in release.get("assets", [])}
    missing = [n for n in names if n not in by_name]
    tag = release.get("tag_name", "")
    if missing:
        raise Die(f"Release {tag} 里没有 {', '.join(missing)}，请手动下载或用 --file 指定路径。")
    return {n: by_name[n] for n in names}


def esptool_cmd(no_install: bool) -> tuple[list[str], int]:
    """返回 (命令行前缀, 主版本号)。优先用已安装的模块，其次 PlatformIO 自带的。"""
    try:
        import esptool  # noqa: F401

        base = [sys.executable, "-m", "esptool"]
    except ImportError:
        pio = Path.home() / ".platformio" / "packages" / "tool-esptoolpy" / "esptool.py"
        if pio.exists():
            base = [sys.executable, str(pio)]
        else:
            if no_install:
                raise Die("没有找到 esptool。请先运行：python -m pip install esptool")
            say("没有找到 esptool，正在安装（约 7MB，只需一次）...")
            for extra in ([], ["--user"]):
                r = subprocess.run(
                    [sys.executable, "-m", "pip", "install",
                     "--disable-pip-version-check", "esptool", *extra]
                )
                if r.returncode == 0:
                    break
            else:
                raise Die("esptool 安装失败。请手动运行：python -m pip install esptool")
            base = [sys.executable, "-m", "esptool"]

    out = subprocess.run(base + ["version"], capture_output=True, text=True)
    text = (out.stdout or "") + (out.stderr or "")
    m = re.search(r"esptool\s+v?(\d+)\.", text)
    major = int(m.group(1)) if m else 5
    return base, major


def find_image(explicit: str | None, no_install: bool) -> tuple[Path, bool]:
    """返回 (镜像路径, 是否来自网络)。本地优先，避免每次刷写都下载。"""
    if explicit:
        p = Path(explicit)
        if not p.exists():
            raise Die(f"找不到镜像文件：{p}")
        return p, False

    for p in (HERE / "full.bin", DEFAULT_LOCAL):
        if p.exists():
            return p, False

    say("本地没有 full.bin，从 GitHub Release 下载...")
    dest = DEFAULT_LOCAL
    dest.parent.mkdir(parents=True, exist_ok=True)
    try:
        urls = release_assets(["full.bin", "full.bin.sha256"])
        dest.write_bytes(_fetch(urls["full.bin"], 120))
        expected = _fetch(urls["full.bin.sha256"], 30).decode("utf-8", "replace").split()[0].lower()
    except Die:
        dest.unlink(missing_ok=True)
        raise
    except Exception as e:  # noqa: BLE001
        dest.unlink(missing_ok=True)
        raise Die(f"下载失败：{e}\n请手动下载 full.bin，或用 --file 指定路径。") from e

    actual = sha256(dest)
    if actual != expected:
        dest.unlink(missing_ok=True)
        raise Die(f"SHA256 不匹配（期望 {expected[:12]}…，实际 {actual[:12]}…），已删除下载文件。")
    say(f"SHA256 校验通过：{actual[:16]}…")
    return dest, True


def serial_ports() -> list:
    try:
        from serial.tools import list_ports
    except ImportError as e:
        raise Die("缺少 pyserial（通常随 esptool 一起安装）。请重新运行本脚本。") from e
    return list(list_ports.comports())


def pick_port(explicit: str | None, assume_yes: bool) -> str:
    if explicit:
        return explicit

    ports = serial_ports()
    esp = [p for p in ports if (p.vid or 0) == ESP_VID]

    if len(esp) == 1:
        p = esp[0]
        say(f"找到设备：{p.device}  {p.description}")
        return p.device

    if len(esp) > 1:
        say("找到多个 Espressif 串口：")
        for i, p in enumerate(esp, 1):
            say(f"  {i}) {p.device}  {p.description}")
        if assume_yes or not sys.stdin.isatty():
            raise Die("请用 --port 指定要刷写的串口。")
        i = input(f"选择 1-{len(esp)} [1]: ").strip() or "1"
        return esp[int(i) - 1].device

    say("没有找到 Espressif 设备。当前串口：")
    for p in ports or []:
        vid = f"{p.vid:04X}" if p.vid else "----"
        say(f"  {p.device}  VID={vid}  {p.description}")
    raise Die(
        "请检查：\n"
        "  1) USB-C 线是否支持数据传输（只能供电的线不会出现串口）；\n"
        "  2) 板子是否已上电；\n"
        "  3) 按住 BOOT，点一下 RESET，松开 BOOT，再重新运行本脚本。\n"
        "蓝牙虚拟串口（COM3/COM4 之类）不能刷写。"
    )


def esptool_run(cmd: list[str]) -> subprocess.CompletedProcess:
    say("> " + " ".join(cmd))
    return subprocess.run(cmd)


def preflight(base: list[str], major: int, port: str) -> str:
    cmd = base + ["--chip", "esp32s3", "--port", port,
                  "flash-id" if major >= 5 else "flash_id"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")
    say(out.strip())
    if r.returncode != 0:
        raise Die(
            "连不上芯片。请按住 BOOT，点一下 RESET，松开 BOOT，然后重新运行。\n"
            "（这一步只是读取芯片信息，不会改动设备。）"
        )
    if "ESP32-S3" not in out:
        raise Die("目标不是 ESP32-S3，已停止，未写入任何数据。")
    m = re.search(r"(?:Detected flash size|Flash size)[: ]+(\d+)MB", out, re.I)
    size = int(m.group(1)) if m else 0
    if size and size < 8:
        raise Die(f"检测到 {size}MB Flash，镜像需要至少 8MB；可能不是这块板子，已停止。")
    if size != 16:
        say(f"注意：检测到 {size}MB Flash（这块板子通常是 16MB），镜像仍按 8MB 分区表构建，可以继续。")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="刷入 ESP32-S3 用量监视器固件")
    ap.add_argument("--port", help="串口，例如 COM5；默认自动查找 VID 303A")
    ap.add_argument("--file", help="要写入的 full.bin；默认找脚本旁边、dist/，再下载")
    ap.add_argument("--baud", default="460800", help="烧写波特率，默认 460800")
    ap.add_argument("--erase", action="store_true", help="先整片擦除（会清 WiFi、凭据和所有设置）")
    ap.add_argument("--verify", action="store_true", help="写完后回读校验（多约 30 秒）")
    ap.add_argument("--dry-run", action="store_true", help="只预检并打印命令，不写入")
    ap.add_argument("--yes", "-y", action="store_true", help="不再询问确认")
    ap.add_argument("--no-install", action="store_true", help="缺少 esptool 时不自动安装")
    args = ap.parse_args()

    base, major = esptool_cmd(args.no_install)
    image, downloaded = find_image(args.file, args.no_install)
    size_mb = image.stat().st_size / 1024 / 1024
    digest = sha256(image)
    say(f"镜像：{image}  ({size_mb:.2f} MB)")
    say(f"SHA256：{digest}")

    if downloaded:
        pass  # find_image 已经校验过
    else:
        sidecar = image.with_suffix(image.suffix + ".sha256")
        if sidecar.exists():
            expected = sidecar.read_text(encoding="utf-8").split()[0].lower()
            if digest != expected:
                raise Die("镜像的 SHA256 与 .sha256 文件不一致，已停止。")
            say("SHA256 与 .sha256 文件一致。")

    port = pick_port(args.port, args.yes)
    preflight(base, major, port)

    write = "write-flash" if major >= 5 else "write_flash"
    cmd = base + [
        "--chip", "esp32s3", "--port", port, "--baud", args.baud,
        write,
        "--flash-mode" if major >= 5 else "--flash_mode", "qio",
        "--flash-size" if major >= 5 else "--flash_size", "8MB",
    ]
    if args.erase:
        cmd.append("--erase-all")
    cmd += ["0x0", str(image)]

    say()
    say(f"将把 {size_mb:.2f} MB 写入 {port} 的 0x0。")
    if args.erase:
        say("--erase 已开启：WiFi、凭据和所有设置都会被清除。")
    else:
        say("未整片擦除：文件系统里的配置会保留，但 WiFi 配网信息会被覆盖（需重新配网）。")
    if not args.yes and sys.stdin.isatty():
        if input("继续？[y/N] ").strip().lower() not in ("y", "yes"):
            say("已取消。")
            return 1

    if args.dry_run:
        say("dry-run：未写入。")
        return 0

    r = esptool_run(cmd)
    if r.returncode != 0 and args.baud != "115200":
        say(f"\n{args.baud} 波特率失败，降速到 115200 重试...")
        cmd[cmd.index(args.baud)] = "115200"
        r = esptool_run(cmd)
    if r.returncode != 0:
        raise Die("写入失败。请确认串口没有被串口监视器占用，然后重试。")

    if args.verify:
        say("\n回读校验...")
        vcmd = base + ["--chip", "esp32s3", "--port", port,
                       "verify-flash" if major >= 5 else "verify_flash",
                       "0x0", str(image)]
        if esptool_run(vcmd).returncode != 0:
            raise Die("回读校验失败，请重新刷写。")

    say()
    say("刷入完成，设备会自动重启。")
    say("首次使用：屏幕进入 WiFi 配置界面；PWR/IO18 选择，BOOT 确认。")
    say("用量页的凭据需要另刷文件系统（见 FLASHING.md 或 README）。")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Die as e:
        say(f"\n失败：{e}")
        sys.exit(1)
    except KeyboardInterrupt:
        say("\n已中断。")
        sys.exit(130)
