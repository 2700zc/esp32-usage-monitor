#!/usr/bin/env python3
"""ZCode -> ESP32 agent status bridge.

Two transports, one state machine:

  * hook events arrive over UDP on 127.0.0.1 (fire-and-forget from
    zcode-hook.py, so a hook never waits on the network);
  * the ESP32 polls GET /agent over HTTP and is also pushed to by UDP the
    moment the state changes -- the bridge learns the device IP from the
    polling request, so no address has to be configured on this side.

Only the standard library is used, so the script runs on a bare Python
install (the packaged Python 3.14 that ships with the dev box is fine).
"""

from __future__ import annotations

import argparse
import base64
import ctypes
import hmac
import io
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid
import wave
from ctypes import wintypes
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

# ── state model ──────────────────────────────────────────────────────────
# waiting > tool > thinking > done > idle: when several ZCode sessions are
# live at once the most "blocking" one wins, because that is the one the
# user needs to look at (a session waiting for approval outranks a session
# that is merely generating).
PRIORITY = {"waiting": 4, "tool": 3, "thinking": 2, "done": 1, "idle": 0}
BUSY = ("thinking", "tool", "waiting")

EVENT_STATE = {
    "SessionStart": "idle",
    "UserPromptSubmit": "thinking",
    "PreToolUse": "tool",          # tool_name carries which one
    "PermissionRequest": "waiting",
    "PostToolUse": "thinking",
    "PostToolUseFailure": "thinking",
    "Stop": "done",
}

# ── 局域网发现 ────────────────────────────────────────────────────────────
# 设备不知道桥接在哪台机器上（换电脑 / DHCP 换 IP 都不该重刷固件），所以它
# 广播一句问话、或者按上次记住的网段逐个单播，谁在跑桥接谁应答。应答里带
# HTTP 端口，设备从报文源地址拿到 IP，于是不再需要任何手工配置。
#
#   设备 -> 广播/单播 :8789   "ZCODE-AGENT-V1?"
#   桥接 -> 单播回源端口       "ZCODE-AGENT-V1 8787"
AGENT_PROBE = b"ZCODE-AGENT-V1?"
AGENT_REPLY = b"ZCODE-AGENT-V1 %d"

# 设备地址存在桥接旁边，跨重启有效：设备可能记的是别的网络里的桥接地址
# （换过地方、换过电脑），这时它自己找不到；由记得它的这一侧主动喊一声，
# 它就会切过来。双向记忆——哪边记得，哪边就负责搭桥。
STATE_FILE = "devices.json"
DEVICE_MEMORY = 4          # 记最近几台设备（万一有两块屏）
ANNOUNCE_IDLE_SEC = 10.0   # 设备多久没来轮询就再喊一次
ANNOUNCE_POLL_SEC = 5.0    # 多久检查一次"设备还在不在"
DEVICE_FRESH_SEC = 45.0    # 设备多近来过才算"连着"

# ── 动作接口（设备 -> PC 注入按键）──────────────────────────────────────
# /agent 是局域网明文、无鉴权的只读接口；/act 会让 PC 真的动起来，安全模型
# 必须变：token 必填、只接受白名单动作、默认还要求前台窗口属于 ZCode。
# 按下设备按钮的那一刻前台是谁不受控，回车打进终端会直接执行已经敲好的命令。
ACT_TOKEN_FILE = "act.token"
# 动作名 -> (虚拟键, 扫描码, 额外标志)。右 Alt 必须带扩展键标志，否则应用
# 会把它当成左 Alt / 普通 Alt。
VK_RETURN = 0x0D
VK_ESCAPE = 0x1B
VK_RMENU = 0xA5
KEYEVENTF_EXTENDEDKEY = 0x0001
ACT_KEYS = {
    "enter": (VK_RETURN, 0, 0),
    "esc":   (VK_ESCAPE, 0, 0),
    "ralt":  (VK_RMENU, 0x38, KEYEVENTF_EXTENDEDKEY),
}
ACT_NAMES = tuple(ACT_KEYS)
DEFAULT_ACT_APPS = ("zcode.exe",)
# 设备把 token 拼进 URL 查询串，所以字符集收窄到不用转义的范围
ACT_TOKEN_RE = re.compile(r"[A-Za-z0-9_-]{8,64}")
KEYEVENTF_KEYUP = 0x0002
INPUT_KEYBOARD = 1
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

# ── 语音输入（设备 -> 火山 ASR -> 剪贴板粘贴）──────────────────────────
# 设备把 16 kHz / 16 bit 单声道 PCM 直接放进 POST body，桥接补 WAV 头后
# 交给火山「大模型录音文件识别」。识别结果用剪贴板 + Ctrl+V 注入当前前台
# 窗口：SendInput 的普通按键事件打不出中文，而剪贴板对任意输入框都有效。
VOLC_FLASH_URL = "https://openspeech.bytedance.com/api/v3/auc/bigmodel/recognize/flash"
VOLC_SUBMIT_URL = "https://openspeech.bytedance.com/api/v3/auc/bigmodel/submit"
VOLC_QUERY_URL = "https://openspeech.bytedance.com/api/v3/auc/bigmodel/query"
# 标准版（异步提交 + 查询）：2.0 用 volc.seedasr.auc，1.0 用 volc.bigasr.auc。
# 极速版（同步 flash）用 volc.bigasr.auc_turbo。默认走标准版 2.0，因为
# 大多数新账号只开通了标准版。
VOLC_RESOURCE_DEFAULT = "volc.seedasr.auc"
VOLC_STANDARD_RESOURCES = ("volc.seedasr.auc", "volc.bigasr.auc")
VOLC_KEY_FILE = "volc.key"
ASR_MAX_BYTES = 8 * 1024 * 1024
CF_UNICODETEXT = 13
GMEM_MOVEABLE = 0x0002
GMEM_ZEROINIT = 0x0040
VK_CONTROL = 0x11
VK_V = 0x56
VK_BACK = 0x08
# 长按 BOOT 清空只对"刚刚"粘贴的文字生效：光标可能已经被用户挪走，太久
# 之后再回删会删到别的内容。2 分钟足够看完一句话再决定重说。
CLEAR_TTL_SEC = 120
# 单次最多回删这么多字符：设备一次录音上限 60 秒，正常转录远小于此值；
# 设上限只是防止异常数据让桥接连发几万次按键。
CLEAR_MAX_CHARS = 1000


def load_act_token(cli_value: str | None) -> str:
    """CLI 优先，其次读脚本旁边的 act.token。非法值当作没配置。"""
    if cli_value is not None:
        tok = cli_value.strip()
        source = "--act-token"
    else:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), ACT_TOKEN_FILE)
        try:
            with open(path, encoding="utf-8") as f:
                tok = f.readline().strip()
            source = ACT_TOKEN_FILE
        except OSError:
            return ""
    if not ACT_TOKEN_RE.fullmatch(tok):
        print(f"!! {source} ignored: a token must be 8-64 chars of [A-Za-z0-9_-]",
              file=sys.stderr, flush=True)
        return ""
    return tok


class VolcError(RuntimeError):
    pass


def load_volc_key(cli_value: str | None) -> str:
    """CLI 优先，其次环境变量 VOLC_API_KEY，最后读脚本旁边的 volc.key。"""
    if cli_value:
        return cli_value.strip()
    env = os.environ.get("VOLC_API_KEY", "").strip()
    if env:
        return env
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), VOLC_KEY_FILE)
    try:
        with open(path, encoding="utf-8") as f:
            return f.readline().strip()
    except OSError:
        return ""


class _KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD),
                ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD),
                # ULONG_PTR：64 位下必须是指针大小，否则结构体布局对不上
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class _MOUSEINPUT(ctypes.Structure):
    # INPUT 的 union 要按"最大的成员"算大小；只放 KEYBDINPUT 会让 sizeof(INPUT)
    # 变成 32，而 Windows x64 期望 40，SendInput 会报参数错误、一次也发不出去。
    _fields_ = [("dx", wintypes.LONG),
                ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD),
                ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class _INPUT(ctypes.Structure):
    class _Union(ctypes.Union):
        _fields_ = [("ki", _KEYBDINPUT), ("mi", _MOUSEINPUT)]

    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _Union)]


def foreground_app() -> tuple[int, str, str]:
    """(hwnd, exe 文件名小写, 窗口标题)。取不到 exe 时留空。"""
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    user32.GetForegroundWindow.restype = ctypes.c_void_p
    hwnd = user32.GetForegroundWindow()
    if not hwnd:
        return 0, "", ""
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(pid))
    title = ctypes.create_unicode_buffer(512)
    n = user32.GetWindowTextW(ctypes.c_void_p(hwnd), title, len(title))
    title_s = title.value if n > 0 else ""

    exe = ""
    kernel32.OpenProcess.restype = ctypes.c_void_p
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid.value)
    if h:
        path = ctypes.create_unicode_buffer(1024)
        size = wintypes.DWORD(len(path))
        if kernel32.QueryFullProcessImageNameW(h, 0, path, ctypes.byref(size)):
            exe = os.path.basename(path.value).lower()
        kernel32.CloseHandle(ctypes.c_void_p(h))
    return int(hwnd), exe, title_s


def send_key(vk: int, scan: int = 0, extra: int = 0, hold_ms: int = 0) -> bool:
    """向当前前台窗口注入一次按键。返回值是按下/抬起是否都投递成功。

    hold_ms > 0 时在按下和抬起之间等一会儿：真人按键通常有 50-150ms 的持续时间，
    有些输入法的低级键盘钩子会过滤掉“同一批 SendInput 里按下又抬起”的瞬时事件。
    """
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.SendInput.argtypes = (wintypes.UINT, ctypes.POINTER(_INPUT), ctypes.c_int)
    user32.SendInput.restype = wintypes.UINT

    def one(flags: int) -> bool:
        inp = _INPUT()
        inp.type = INPUT_KEYBOARD
        inp.ki.wVk = vk
        inp.ki.wScan = scan
        inp.ki.dwFlags = extra | flags
        return user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(_INPUT)) == 1

    if not one(0):
        return False
    if hold_ms > 0:
        time.sleep(hold_ms / 1000.0)
    return one(KEYEVENTF_KEYUP)


# ZCode 的 Enter 提交处理挂在编辑器 root 上，光标不在输入框里时按键等于按在
# 空白处；全前端唯一一个全局 Enter 还是确认弹窗。所以注入前必须先聚焦输入框，
# 而且是"确认 HasKeyboardFocus 为真"才发键。这里用系统自带的 PowerShell + UIA，
# 不引入任何第三方包（刷机包承诺客户只装 Python 3）。
# 选最大面积的可见 Edit：ZCode 的 composer 是 Lexical contenteditable，UIA 里
# 表现为 ControlType.Edit，Name 是占位符文案（会变），不能按名字匹配。
_FOCUS_PS = r"""
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
$h = [IntPtr]__HWND__
$root = [System.Windows.Automation.AutomationElement]::FromHandle($h)
if ($null -eq $root) { 'NO_WINDOW'; exit 2 }
$best = $null
# 快路径：当前焦点本身就是输入框时直接用它。ZCode 的聊天记录很长时，
# FindAll(Descendants) 会遍历几千个节点，冷启动能超过 2 秒；焦点查询只要几十毫秒。
$f = [System.Windows.Automation.AutomationElement]::FocusedElement
if ($null -ne $f -and $f.Current.ControlType -eq [System.Windows.Automation.ControlType]::Edit) {
  $best = $f
}
if ($null -eq $best) {
  # 慢路径：用 FindFirst 而不是 FindAll，找到一个就停，不收集整棵树。
  $cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::Edit)
  $e = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
  if ($null -ne $e) {
    $r = $e.Current.BoundingRectangle
    if ($r.Width -gt 100 -and $r.Height -gt 16 -and -not $e.Current.IsOffscreen) { $best = $e }
  }
}
if ($null -eq $best) { 'NO_EDIT'; exit 2 }
try { $best.SetFocus() } catch { 'FOCUS_FAIL'; exit 3 }
Start-Sleep -Milliseconds 80
if ($best.Current.HasKeyboardFocus) { 'OK' } else { 'NO_FOCUS'; exit 4 }
"""


def focus_foreground_edit(hwnd: int) -> str:
    """把前台窗口里的输入框聚焦，返回 OK / NO_EDIT / NO_FOCUS / ...。"""
    script = _FOCUS_PS.replace("__HWND__", str(int(hwnd)))
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-STA", "-Command", script],
            capture_output=True, text=True, timeout=6.0,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except (OSError, subprocess.TimeoutExpired):
        return "PS_FAIL"
    lines = [ln.strip() for ln in (r.stdout or "").splitlines() if ln.strip()]
    return lines[-1] if lines else "PS_FAIL"


def warm_uia() -> None:
    """启动时预热 PowerShell + .NET UIA 程序集。

    冷启动实测要 2.6-2.9 秒，第一次按键会明显变慢；这里只查询焦点元素，
    不移动焦点、不发键。放到后台线程里做，不拖慢桥接启动。
    """
    try:
        subprocess.run(
            ["powershell", "-NoProfile", "-STA", "-Command",
             "Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes; "
             "$null = [System.Windows.Automation.AutomationElement]::FocusedElement"],
            capture_output=True, text=True, timeout=20,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except (OSError, subprocess.TimeoutExpired):
        pass


# ── 剪贴板 + Ctrl+V ──────────────────────────────────────────────────────
# 中文没法用 SendInput 的普通按键事件打出来（那只能发虚拟键码），所以识别
# 结果先放进剪贴板，再向前台窗口发一次 Ctrl+V。粘贴完把用户原来的剪贴板
# 内容放回去（默认开启；--no-restore-clipboard 可关）。
def _clipboard_api():
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GlobalAlloc.restype = ctypes.c_void_p
    kernel32.GlobalAlloc.argtypes = (wintypes.UINT, ctypes.c_size_t)
    kernel32.GlobalLock.restype = ctypes.c_void_p
    kernel32.GlobalLock.argtypes = (ctypes.c_void_p,)
    kernel32.GlobalUnlock.argtypes = (ctypes.c_void_p,)
    kernel32.GlobalFree.argtypes = (ctypes.c_void_p,)
    user32.OpenClipboard.argtypes = (ctypes.c_void_p,)
    user32.SetClipboardData.argtypes = (wintypes.UINT, ctypes.c_void_p)
    user32.SetClipboardData.restype = ctypes.c_void_p
    user32.GetClipboardData.argtypes = (wintypes.UINT,)
    user32.GetClipboardData.restype = ctypes.c_void_p
    return user32, kernel32


def clipboard_get_text() -> str | None:
    if os.name != "nt":
        return None
    user32, kernel32 = _clipboard_api()
    for _ in range(5):
        if user32.OpenClipboard(None):
            break
        time.sleep(0.02)
    else:
        return None
    try:
        h = user32.GetClipboardData(CF_UNICODETEXT)
        if not h:
            return None
        p = kernel32.GlobalLock(ctypes.c_void_p(h))
        if not p:
            return None
        try:
            return ctypes.wstring_at(p)
        finally:
            kernel32.GlobalUnlock(ctypes.c_void_p(h))
    finally:
        user32.CloseClipboard()


def clipboard_set_text(text: str) -> bool:
    if os.name != "nt":
        return False
    user32, kernel32 = _clipboard_api()
    for _ in range(5):
        if user32.OpenClipboard(None):
            break
        time.sleep(0.02)
    else:
        return False
    try:
        user32.EmptyClipboard()
        data = text.encode("utf-16-le") + b"\x00\x00"
        h = kernel32.GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, len(data))
        if not h:
            return False
        p = kernel32.GlobalLock(ctypes.c_void_p(h))
        if not p:
            kernel32.GlobalFree(ctypes.c_void_p(h))
            return False
        ctypes.memmove(p, data, len(data))
        kernel32.GlobalUnlock(ctypes.c_void_p(h))
        # SetClipboardData 成功后句柄归系统所有，不能再 GlobalFree。
        if not user32.SetClipboardData(CF_UNICODETEXT, ctypes.c_void_p(h)):
            kernel32.GlobalFree(ctypes.c_void_p(h))
            return False
        return True
    finally:
        user32.CloseClipboard()


def send_ctrl_v() -> bool:
    """向前台窗口发 Ctrl+V；按下/抬起之间留一点时间，避开输入法钩子。"""
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.SendInput.argtypes = (wintypes.UINT, ctypes.POINTER(_INPUT), ctypes.c_int)
    user32.SendInput.restype = wintypes.UINT

    def event(vk: int, up: bool) -> _INPUT:
        inp = _INPUT()
        inp.type = INPUT_KEYBOARD
        inp.ki.wVk = vk
        inp.ki.dwFlags = KEYEVENTF_KEYUP if up else 0
        return inp

    seq = ((VK_CONTROL, False), (VK_V, False), (VK_V, True), (VK_CONTROL, True))
    for i, (vk, up) in enumerate(seq):
        inp = event(vk, up)
        if user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(_INPUT)) != 1:
            return False
        if i == 1:
            time.sleep(0.03)
        else:
            time.sleep(0.01)
    return True


# ── 火山「大模型录音文件识别」flash 接口 ─────────────────────────────────
def _volc_auth_headers(args: argparse.Namespace, request_id: str) -> dict:
    # CLI/环境变量优先；否则每次请求都读一次 volc.key，这样填完密钥
    # 不用重启桥接就能生效。
    api_key = args.volc_api_key or load_volc_key(None)
    if not api_key and not (args.volc_app_key and args.volc_access_key):
        raise VolcError(f"没有配置火山 API Key：创建 {VOLC_KEY_FILE} 或设置 VOLC_API_KEY")
    headers = {
        "Content-Type": "application/json",
        "X-Api-Resource-Id": args.volc_resource_id,
        "X-Api-Request-Id": request_id,
        "X-Api-Sequence": "-1",
    }
    if api_key:
        headers["X-Api-Key"] = api_key
    else:
        headers["X-Api-App-Key"] = args.volc_app_key
        headers["X-Api-Access-Key"] = args.volc_access_key
    return headers


def _volc_post(url: str, headers: dict, payload: dict):
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode("utf-8"),
        headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=45) as resp:
            return resp.status, resp.headers, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read()
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise VolcError(f"连不上火山 ASR: {e}") from e


def _volc_resource_hint(resource_id: str) -> str:
    if resource_id in VOLC_STANDARD_RESOURCES:
        return "火山未授权 45000030：请开通录音文件识别标准版 2.0"
    return "火山未授权 45000030：请开通录音文件识别极速版"


def _volc_check(http_code: int, headers, body: bytes, resource_id: str) -> None:
    status = headers.get("X-Api-Status-Code", "") if headers else ""
    if http_code == 200 and status == "20000000":
        return
    detail = body.decode("utf-8", "replace")[:300]
    # 原始 body 写日志：里面的 reqid / logid 找火山支持时要提供。
    print(f"  asr: volc raw http={http_code} api={status} {detail[:240]}", flush=True)
    if status == "45000030":
        raise VolcError(_volc_resource_hint(resource_id))
    raise VolcError(f"火山识别失败 http={http_code} api={status}: {detail}")


def volc_transcribe_flash(wav_bytes: bytes, args: argparse.Namespace) -> str:
    """极速版：一次请求直接返回结果。"""
    request_id = str(uuid.uuid4())
    headers = _volc_auth_headers(args, request_id)
    payload = {
        "user": {"uid": args.volc_app_key or "esp32-voice"},
        "audio": {
            "data": base64.b64encode(wav_bytes).decode("ascii"),
            "format": "wav",
            "language": args.volc_language,
        },
        "request": {
            "model_name": "bigmodel",
            "enable_itn": True,
            "enable_punc": True,
            "show_utterances": False,
        },
    }
    http_code, resp_headers, body = _volc_post(VOLC_FLASH_URL, headers, payload)
    _volc_check(http_code, resp_headers, body, args.volc_resource_id)
    try:
        data = json.loads(body or b"{}")
    except Exception as e:
        raise VolcError(f"火山返回不是 JSON: {body[:200]!r}") from e
    return str((data.get("result") or {}).get("text") or "").strip()


def volc_transcribe_standard(wav_bytes: bytes, args: argparse.Namespace) -> str:
    """标准版：提交任务后轮询查询。短音频通常 1~3 秒出结果。"""
    task_id = str(uuid.uuid4())
    headers = _volc_auth_headers(args, task_id)
    payload = {
        "audio": {
            "data": base64.b64encode(wav_bytes).decode("ascii"),
            "format": "wav",
            "codec": "raw",
            "rate": 16000,
            "bits": 16,
            "channel": 1,
        },
        "request": {
            "model_name": "bigmodel",
            "enable_itn": True,
            "enable_punc": True,
            "show_utterances": False,
        },
    }
    http_code, resp_headers, body = _volc_post(VOLC_SUBMIT_URL, headers, payload)
    _volc_check(http_code, resp_headers, body, args.volc_resource_id)
    print(f"  asr: submitted task {task_id}", flush=True)

    deadline = time.time() + 25.0
    while time.time() < deadline:
        time.sleep(0.5)
        q_headers = _volc_auth_headers(args, task_id)
        q_http, q_headers_resp, q_body = _volc_post(VOLC_QUERY_URL, q_headers, {})
        q_status = q_headers_resp.get("X-Api-Status-Code", "") if q_headers_resp else ""
        if q_status in ("20000001", "20000002"):   # processing / queued
            continue
        if q_status == "20000003":   # 任务完成，但没有检测到有效语音
            return ""
        _volc_check(q_http, q_headers_resp, q_body, args.volc_resource_id)
        try:
            data = json.loads(q_body or b"{}")
        except Exception as e:
            raise VolcError(f"火山查询返回不是 JSON: {q_body[:200]!r}") from e
        return str((data.get("result") or {}).get("text") or "").strip()
    raise VolcError("火山识别超时（25 秒），请重试或缩短录音")


def volc_transcribe(wav_bytes: bytes, args: argparse.Namespace) -> str:
    if args.volc_resource_id in VOLC_STANDARD_RESOURCES:
        return volc_transcribe_standard(wav_bytes, args)
    return volc_transcribe_flash(wav_bytes, args)


def send_backspaces(count: int) -> bool:
    """向当前前台窗口连发 count 次 Backspace。

    逐次注入而不是拼成一批：目标程序每收到一个字符都会更新光标/行高，
    留 8ms 让它处理完再发下一个，长文本时比一次性塞进去更稳。
    """
    for _ in range(count):
        if not send_key(VK_BACK):
            return False
        time.sleep(0.008)
    return True


def paste_text(text: str, args: argparse.Namespace) -> tuple[bool, str, str, int]:
    """把文字粘贴到当前前台窗口。返回 (ok, exe, 说明, hwnd)。"""
    if os.name != "nt":
        return False, "", "paste is Windows-only", 0
    hwnd, exe, title = foreground_app()
    if not hwnd:
        return False, exe, "no foreground window", 0
    if args.asr_app:
        allow = [a.lower() for a in args.asr_app]
        ok = any(exe == a or (not a.endswith(".exe") and exe == a + ".exe")
                 for a in allow)
        if not ok:
            return False, exe, f"foreground {exe or '?'} not in --asr-app", int(hwnd)

    prev = clipboard_get_text() if args.restore_clipboard else None
    if not clipboard_set_text(text):
        return False, exe, "clipboard write failed", int(hwnd)
    time.sleep(0.05)
    if not send_ctrl_v():
        return False, exe, "SendInput Ctrl+V failed", int(hwnd)

    if args.restore_clipboard and prev is not None:
        def restore() -> None:
            time.sleep(0.6)
            clipboard_set_text(prev)
        threading.Thread(target=restore, daemon=True).start()
    return True, exe, title, int(hwnd)


def load_devices(path) -> list[str]:
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        return [str(x) for x in data if isinstance(x, str)][:DEVICE_MEMORY]
    except Exception:
        return []


def remember_device(path, devices: list[str], ip: str) -> list[str]:
    """把 ip 提到最前面；发生变化时返回新列表（调用方负责落盘）。"""
    if devices and devices[0] == ip:
        return devices
    devices = [ip] + [d for d in devices if d != ip]
    return devices[:DEVICE_MEMORY]

class Bridge:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.lock = threading.Lock()
        self.sessions: dict[str, dict] = {}
        self.seq = 0
        self.device_ip: str | None = None
        self.device_seen = 0.0
        self.last_push_seq = -1
        self.act_lock = threading.Lock()   # 两次按键注入不能交错
        # 最近一次成功粘贴：清空接口要知道删多少字符、删在哪个窗口。只在
        # act_lock 内读写；桥接重启后为空，此时长按 BOOT 会提示没有可清空的内容。
        self.last_paste: dict | None = None
        self.udp_out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.state_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), STATE_FILE)
        self.known_devices = load_devices(self.state_path)
        if self.known_devices:
            print(f"remembered device(s): {', '.join(self.known_devices)}", flush=True)

    # ── 动作（唯一的写接口）──
    def act(self, raw_path: str) -> tuple[int, dict]:
        """校验 token 与前台窗口，再注入一个预定义按键。"""
        if not self.args.act_token:
            return 403, {"ok": False, "error": "act disabled on the bridge"}
        q = parse_qs(urlsplit(raw_path).query)
        tok = (q.get("tok") or [""])[0]
        # compare_digest 不接受非 ASCII 的 str；用 bytes 比较，顺带让恶意
        # 查询串走正常的 403 而不是在 handler 里抛异常
        if not hmac.compare_digest(tok.encode("utf-8", "replace"),
                                   self.args.act_token.encode("utf-8")):
            return 403, {"ok": False, "error": "bad token"}
        name = (q.get("name") or [""])[0]
        if name not in ACT_NAMES:
            return 400, {"ok": False, "error": "unknown action"}
        if os.name != "nt":
            return 501, {"ok": False, "error": "key injection is Windows-only"}

        with self.act_lock:
            hwnd, exe, title = foreground_app()
            if not hwnd:
                return 409, {"ok": False, "error": "no foreground window"}
            allow = [a.lower() for a in self.args.act_app]
            ok = any(exe == a or (not a.endswith(".exe") and exe == a + ".exe")
                     for a in allow)
            if not ok:
                print(f"  act rejected: foreground={exe or '?'} title={title[:40]!r}",
                      flush=True)
                return 409, {"ok": False, "error": "foreground not allowed",
                             "app": exe, "title": title[:80]}
            if self.args.act_dry_run:
                print(f"  act dry-run: would send {name} to {exe} ({title[:40]!r})",
                      flush=True)
                return 200, {"ok": True, "dry_run": True, "action": name,
                             "app": exe, "title": title[:80]}
            if name in ("enter", "ralt"):
                # 豆包的语音唤起要求“当前有聚焦的输入框”（它的日志里就写着
                # 「请点击输入框后再唤起语音」），所以右 Alt 也要先把光标送回
                # 输入框。enter 更是必须聚焦：光标不在 composer 时按键无效，
                # 而确认弹窗会吃全局 Enter，盲发可能替用户点掉授权框。
                focus = focus_foreground_edit(hwnd)
                if focus != "OK":
                    print(f"  act focus failed: {focus} (foreground={exe})", flush=True)
                    return 422, {"ok": False, "error": "no focused input", "detail": focus}
            vk, scan, extra = ACT_KEYS[name]
            # 右 Alt 是给输入法语音用的：给一个接近真人按键的持续时间，
            # 免得被低级键盘钩子当成“瞬时注入”过滤掉。
            hold = 150 if name == "ralt" else 0
            if not send_key(vk, scan, extra, hold):
                print(f"  act {name} failed: SendInput returned an error "
                      f"(foreground={exe})", flush=True)
                return 500, {"ok": False, "error": "SendInput failed"}
            print(f"  act {name} -> {exe} ({title[:40]!r})", flush=True)
            return 200, {"ok": True, "action": name, "app": exe, "title": title[:80]}

    # ── 语音输入 ──
    def asr(self, body: bytes, headers, raw_path: str) -> tuple[int, dict]:
        """校验令牌，PCM -> WAV，火山识别，再把结果粘贴到前台窗口。"""
        if not self.args.act_token:
            return 403, {"ok": False, "error": "asr disabled: no act_token"}
        q = parse_qs(urlsplit(raw_path).query)
        tok = (q.get("tok") or [""])[0]
        if not hmac.compare_digest(tok.encode("utf-8", "replace"),
                                   self.args.act_token.encode("utf-8")):
            return 403, {"ok": False, "error": "bad token"}
        if not body:
            return 400, {"ok": False, "error": "empty audio"}

        try:
            rate = int(headers.get("X-Sample-Rate", "16000"))
            channels = int(headers.get("X-Channels", "1"))
            bits = int(headers.get("X-Bits", "16"))
        except (TypeError, ValueError):
            return 400, {"ok": False, "error": "bad audio headers"}
        if not (8000 <= rate <= 48000) or channels not in (1, 2) or bits != 16:
            return 400, {"ok": False, "error": "unsupported audio format"}
        if len(body) % (channels * 2) != 0:
            return 400, {"ok": False, "error": "partial audio frame"}

        buf = io.BytesIO()
        with wave.open(buf, "wb") as w:
            w.setnchannels(channels)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(body)
        wav_bytes = buf.getvalue()
        if self.args.asr_save_wav:
            try:
                with open(self.args.asr_save_wav, "wb") as f:
                    f.write(wav_bytes)
                print(f"  asr: saved {len(wav_bytes)}B -> {self.args.asr_save_wav}",
                      flush=True)
            except OSError as e:
                print(f"  asr: cannot save wav: {e}", flush=True)

        print(f"  asr: {len(body)}B pcm {rate}Hz/{channels}ch -> volcengine", flush=True)
        try:
            if self.args.asr_mock_text:
                text = self.args.asr_mock_text
            else:
                text = volc_transcribe(wav_bytes, self.args)
        except VolcError as e:
            print(f"  asr failed: {e}", flush=True)
            return 502, {"ok": False, "error": str(e)[:240]}
        if not text:
            return 200, {"ok": False, "error": "empty transcript"}

        if self.args.asr_dry_run:
            print(f"  asr dry-run: {len(text)} chars: {text[:60]!r}", flush=True)
            return 200, {"ok": True, "text": text, "dry_run": True}

        with self.act_lock:
            ok, exe, title, hwnd = paste_text(text, self.args)
            if ok:
                self.last_paste = {
                    "chars": len(text),
                    "hwnd": hwnd,
                    "exe": exe,
                    "title": title,
                    "at": time.time(),
                }
        if not ok:
            print(f"  asr paste failed: {title}", flush=True)
            return 409, {"ok": False, "error": title, "text": text}
        print(f"  asr: {len(text)} chars -> {exe} ({title[:40]!r})", flush=True)
        return 200, {"ok": True, "text": text, "app": exe}

    # ── 清空最近一次粘贴 ──
    def clear_last(self, raw_path: str) -> tuple[int, dict]:
        """把最近一次语音粘贴的文字删掉（按字符数逐字发 Backspace）。

        只在"同一个前台窗口"里生效：用户如果已经切走，回删就落到别的程序
        里了，所以宁可拒绝。清空成功后忘掉这次粘贴，再长按不会继续删更早
        的文字。
        """
        if not self.args.act_token:
            return 403, {"ok": False, "error": "clear disabled: no act_token"}
        q = parse_qs(urlsplit(raw_path).query)
        tok = (q.get("tok") or [""])[0]
        if not hmac.compare_digest(tok.encode("utf-8", "replace"),
                                   self.args.act_token.encode("utf-8")):
            return 403, {"ok": False, "error": "bad token"}
        if os.name != "nt":
            return 501, {"ok": False, "error": "key injection is Windows-only"}

        with self.act_lock:
            lp = self.last_paste
            if not lp:
                return 409, {"ok": False, "error": "没有可清空的输入"}
            if time.time() - lp["at"] > CLEAR_TTL_SEC:
                self.last_paste = None
                return 409, {"ok": False,
                             "error": f"上次输入已超过 {CLEAR_TTL_SEC // 60} 分钟，无法清空"}
            hwnd, exe, title = foreground_app()
            if not hwnd:
                return 409, {"ok": False, "error": "没有前台窗口"}
            if int(hwnd) != lp["hwnd"]:
                return 409, {"ok": False,
                             "error": "前台窗口已切换，请切回刚才的窗口"}
            n = int(lp["chars"])
            if n <= 0 or n > CLEAR_MAX_CHARS:
                return 409, {"ok": False, "error": "上次输入长度异常，无法清空"}

            if self.args.asr_dry_run:
                print(f"  clear dry-run: would delete {n} chars "
                      f"from {exe} ({title[:40]!r})", flush=True)
                return 200, {"ok": True, "chars": n, "dry_run": True}
            if not send_backspaces(n):
                print(f"  clear failed: SendInput returned an error "
                      f"(foreground={exe})", flush=True)
                return 500, {"ok": False, "error": "SendInput failed"}
            self.last_paste = None
            print(f"  clear: {n} chars <- {exe} ({title[:40]!r})", flush=True)
            return 200, {"ok": True, "chars": n, "app": exe, "title": title[:80]}

    # ── ingress ──
    def ingest(self, payload: bytes) -> None:
        try:
            msg = json.loads(payload)
            if not isinstance(msg, dict):
                raise ValueError("not an object")
        except Exception:
            print(f"  !! unparsable event ({len(payload)}B)", flush=True)
            return

        event = str(msg.get("hook_event_name") or msg.get("hookEventName") or "")
        state = EVENT_STATE.get(event)
        if state is None:
            print(f"  !! unknown event {event!r}", flush=True)
            return

        sid = str(msg.get("session_id") or msg.get("sessionId") or "unknown")
        tool = str(msg.get("tool_name") or msg.get("toolName") or "") if state == "tool" else ""
        # 工具细节（Bash 跑的是哪条命令、Edit 动的是哪个文件）只随
        # PreToolUse / PermissionRequest 到达；不继承上一次的，免得界面上
        # 挂着一个早已结束的动作。
        detail = str(msg.get("detail") or "")[:60]
        cwd = str(msg.get("cwd") or "").replace("\\", "/").rstrip("/")
        project = cwd.rsplit("/", 1)[-1] if cwd else ""
        now = time.time()

        with self.lock:
            prev = self.sessions.get(sid)
            self.sessions[sid] = {
                "state": state,
                "tool": tool,
                "detail": detail,
                "project": project or (prev["project"] if prev else ""),
                "since": now if not prev or prev["state"] != state or prev["tool"] != tool
                         else prev["since"],
                "last": now,
            }
            # 超过 idle_after 的会话永远不会被选中（上面的分支已经过滤掉了），
            # 留着只会占内存
            for k in [k for k, v in self.sessions.items()
                      if now - v["last"] > self.args.idle_after]:
                del self.sessions[k]
            self.seq += 1
            status = self._status_locked(now)

        # 日志打成"事件 + 设备上会显示成什么"，排查时不用两头对照
        shown = status["tool"] if status["state"] == "tool" else status["state"]
        extra = status["detail"] or status["project"]
        print(f"  {event:<18} {sid[:14]:<14} {shown:<12} {extra[:40]:<40}"
              f" sessions={len(self.sessions)}", flush=True)
        self.push(status)

    # ── state ──
    def _status_locked(self, now: float) -> dict:
        """Pick the one session worth showing.

        Three rules, in order:

        1. A session waiting for approval wins outright and never decays --
           ZCode is parked on that prompt and will emit no further events
           until a human answers, so age carries no information there.
        2. Otherwise a session that is busy *and* currently talking (an event
           within busy_grace) wins, by priority: running a tool outranks
           merely generating.
        3. Otherwise the most recent event wins regardless of priority. This
           is what keeps an abandoned session from pinning the display: a
           subagent never fires Stop (SubagentStop is not a supported event),
           so its stale "running Bash" would otherwise outrank the main
           session's fresh "done" for a full stale_after window.
        """
        all_sessions = list(self.sessions.values())
        fresh: list[dict] = []

        waiting = [s for s in all_sessions if s["state"] == "waiting"]
        if waiting:
            best = max(waiting, key=lambda s: s["last"])
        else:
            fresh = [s for s in all_sessions if now - s["last"] <= self.args.stale_after]
            talking = [s for s in fresh
                       if s["state"] in ("thinking", "tool")
                       and now - s["last"] <= self.args.busy_grace]
            if talking:
                best = max(talking, key=lambda s: (PRIORITY[s["state"]], s["last"]))
            elif fresh:
                best = max(fresh, key=lambda s: s["last"])
            else:
                # 连一个新鲜会话都没有：显示最后看到的那一个，这样一轮结束后
                # "思考完成"会一直挂在屏幕上（正是这一页存在的意义），直到
                # idle_after 把它清成空闲。
                best = max(all_sessions, key=lambda s: s["last"]) if all_sessions else None

        if best is None:
            state, tool, detail, project, age, dur = "idle", "", "", "", 0.0, 0.0
        else:
            state, tool = best["state"], best["tool"]
            detail, project = best["detail"], best["project"]
            age, dur = now - best["last"], now - best["since"]
            # Nothing heard for this long while supposedly busy: ZCode was
            # probably closed mid-turn (no Stop event ever fires then).
            if state in BUSY and age > self.args.idle_after:
                state, tool, detail = "idle", "", ""

        return {
            "state": state,
            "tool": tool,
            # 设备只在 tool / waiting 两个状态下显示 detail，其余状态用它显示项目名
            "detail": detail,
            "project": project,
            "age": round(age, 1),
            "dur": round(dur, 1),
            "active": 0 if waiting else len(fresh),
            # 等待授权是一个稳定的状态（人没答复之前它不会变），不该被标成过期
            "stale": state in ("thinking", "tool") and age > self.args.stale_after,            "seq": self.seq,
            "ts": int(now),
        }

    def status(self) -> dict:
        with self.lock:
            return self._status_locked(time.time())

    # ── egress ──
    def push(self, status: dict) -> None:
        """Fire-and-forget the new state to the device, if we know where it is."""
        with self.lock:
            ip = self.device_ip
            fresh = time.time() - self.device_seen < 30.0
            self.last_push_seq = self.seq
        if not ip or not fresh:
            return
        try:
            self.udp_out.sendto(json.dumps(status).encode(), (ip, self.args.udp_port))
        except OSError:
            pass

    def note_device(self, ip: str) -> None:
        # A curl from this very machine is not the device: treating it as one
        # would push status datagrams straight back into our own hook port.
        if ip.startswith("127.") or ip in ("::1", "localhost"):
            return
        with self.lock:
            if self.device_ip != ip:
                print(f"  device at {ip} (udp push -> :{self.args.udp_port})", flush=True)
            self.device_ip = ip
            self.device_seen = time.time()
            new = remember_device(self.state_path, self.known_devices, ip)
            if new is not self.known_devices:
                self.known_devices = new
                try:
                    with open(self.state_path, "w", encoding="utf-8") as f:
                        json.dump(new, f, ensure_ascii=False)
                except OSError as e:
                    print(f"  !! cannot save {self.state_path}: {e}", flush=True)

    def announcer(self) -> None:
        """设备不来轮询时，主动朝记得的地址喊一声"桥接在这儿"。

        设备侧只需要收到这条报文就会切换过去（它不用先发问），所以这招能救回
        "设备里存的是另一个网络的地址"这种自己永远找不到的情况——实测有效。
        """
        out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        last_announce = 0.0
        while True:
            time.sleep(ANNOUNCE_POLL_SEC)
            with self.lock:
                targets = list(self.known_devices)
                fresh = time.time() - self.device_seen < DEVICE_FRESH_SEC
            if not targets or fresh:
                last_announce = 0.0
                continue
            if time.time() - last_announce < ANNOUNCE_IDLE_SEC:
                continue
            last_announce = time.time()
            pkt = AGENT_REPLY % self.args.http_port
            for ip in targets:
                try:
                    out.sendto(pkt, (ip, self.args.discovery_port))
                    print(f"  announce -> {ip}:{self.args.discovery_port}", flush=True)
                except OSError:
                    pass

    def udp_listener(self, sock: socket.socket) -> None:
        print(f"hook ingress  udp 127.0.0.1:{self.args.hook_port}", flush=True)
        while True:
            try:
                data, _ = sock.recvfrom(65535)
            except OSError:
                continue
            try:
                self.ingest(data)
            except Exception as e:
                # 一个畸形事件不该让整个监听线程静默退出
                print(f"  !! ingest failed: {type(e).__name__}: {e}", flush=True)

    def discovery_listener(self, sock: socket.socket) -> None:
        print(f"discovery     udp 0.0.0.0:{self.args.discovery_port} "
              f"(replies with http port {self.args.http_port})", flush=True)
        while True:
            try:
                data, addr = sock.recvfrom(512)
            except OSError:
                continue
            if data.strip() != AGENT_PROBE:
                continue
            try:
                sock.sendto(AGENT_REPLY % self.args.http_port, addr)
                print(f"  discovery reply -> {addr[0]}:{addr[1]}", flush=True)
            except OSError as e:
                print(f"  !! discovery reply to {addr} failed: {e}", flush=True)


class _StampWriter:
    """给每一行输出加 HH:MM:SS。

    排查"设备多久才被找回""事件是不是漏了一拍"这类问题时，没有时间戳的日志
    基本没用——每次都要靠外部时间反推。包一层省得在几十处 print 里手工加。
    """
    def __init__(self, raw):
        self._raw = raw

    def write(self, s: str) -> None:
        self._raw.write((time.strftime("%H:%M:%S ") + s) if s.strip() else s)

    def flush(self) -> None:
        self._raw.flush()

    def reconfigure(self, **kw) -> None:
        if hasattr(self._raw, "reconfigure"):
            self._raw.reconfigure(**kw)


class Server(ThreadingHTTPServer):
    # 默认的 allow_reuse_address=1 会让两个实例绑同一个端口都成功，于是出现
    # "HTTP 有响应、事件却收不到"的半死状态（UDP 那边只能绑一个）。端口被占
    # 就直接失败，让人一眼看出是重复启动了。
    allow_reuse_address = False


def make_handler(bridge: Bridge):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def _send(self, body: bytes, ctype: str, code: int = 200) -> None:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:  # noqa: N802
            path = self.path.split("?")[0]
            if path in ("/agent", "/"):
                # The polling request itself is how the bridge discovers the
                # device -- no device address in any config on this side.
                bridge.note_device(self.client_address[0])
                status = bridge.status()
                self._send(json.dumps(status).encode(), "application/json")
            elif path == "/health":
                self._send(b"ok", "text/plain")
            elif path == "/act":
                code, payload = bridge.act(self.path)
                self._send(json.dumps(payload, ensure_ascii=False).encode(),
                           "application/json", code)
            elif path == "/clear":
                code, payload = bridge.clear_last(self.path)
                self._send(json.dumps(payload, ensure_ascii=False).encode(),
                           "application/json", code)
            else:
                self._send(b"not found", "text/plain", 404)

        def do_POST(self) -> None:  # noqa: N802
            path = urlsplit(self.path).path
            if path != "/asr":
                self._send(b"not found", "text/plain", 404)
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except (TypeError, ValueError):
                length = 0
            if length <= 0 or length > ASR_MAX_BYTES:
                self._send(json.dumps({"ok": False, "error": "bad length"}).encode(),
                           "application/json", 413)
                return
            body = self.rfile.read(length)
            code, payload = bridge.asr(body, self.headers, self.path)
            self._send(json.dumps(payload, ensure_ascii=False).encode(),
                       "application/json", code)

        def log_message(self, fmt: str, *args) -> None:
            pass  # the device polls every second; keep the console for events

    return Handler


def lan_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def main() -> int:
    ap = argparse.ArgumentParser(description="ZCode -> ESP32 agent status bridge")
    ap.add_argument("--http-port", type=int, default=8787,
                    help="HTTP port the ESP32 polls (default 8787)")
    ap.add_argument("--hook-port", type=int, default=8788,
                    help="UDP port the ZCode hook posts to (default 8788)")
    ap.add_argument("--udp-port", type=int, default=8788,
                    help="UDP port on the ESP32 that receives instant pushes "
                         "(default 8788, must match the firmware)")
    ap.add_argument("--stale-after", type=float, default=120.0,
                    help="seconds without events before a busy state is flagged stale")
    ap.add_argument("--busy-grace", type=float, default=20.0,
                    help="seconds a busy session keeps outranking a more recent "
                         "finished one (default 20)")
    ap.add_argument("--idle-after", type=float, default=900.0,
                    help="seconds without events before a busy state falls back to idle")
    ap.add_argument("--discovery-port", type=int, default=8789,
                    help="UDP port the ESP32 broadcasts its discovery probe to "
                         "(default 8789, must match the firmware)")
    ap.add_argument("--no-discovery", action="store_true",
                    help="do not answer discovery probes; the device must be "
                         "given this address in agent_url")
    ap.add_argument("--announce-to", action="append", default=[], metavar="IP",
                    help="announce to this device IP (repeatable). Only needed the "
                         "first time at a new site: the device may be holding another "
                         "network's bridge address and would never come to poll here, "
                         "so this side has to speak first. Remembered afterwards.")
    ap.add_argument("--no-push", action="store_true",
                    help="poll only; disable the instant UDP push to the device")
    ap.add_argument("--act-token", default=None, metavar="TOKEN",
                    help="enable the write endpoint /act with this token; it must "
                         "match act_token in the device's config.json. Defaults to "
                         f"the {ACT_TOKEN_FILE} file next to this script")
    ap.add_argument("--act-app", action="append", default=None, metavar="EXE",
                    help="foreground executable allowed to receive injected keys "
                         "(repeatable). Default: zcode.exe")
    ap.add_argument("--act-dry-run", action="store_true",
                    help="authenticate and check the foreground window, but do not "
                         "send any key (safe way to test the setup)")
    ap.add_argument("--volc-api-key", default=None, metavar="KEY",
                    help="Volcengine ASR API key (new console). Defaults to "
                         f"VOLC_API_KEY or the {VOLC_KEY_FILE} file next to this script")
    ap.add_argument("--volc-app-key", default=os.environ.get("VOLC_APP_KEY", ""),
                    help="legacy Volcengine App Key (used with --volc-access-key)")
    ap.add_argument("--volc-access-key", default=os.environ.get("VOLC_ACCESS_KEY", ""),
                    help="legacy Volcengine Access Key")
    ap.add_argument("--volc-resource-id",
                    default=os.environ.get("VOLC_RESOURCE_ID", VOLC_RESOURCE_DEFAULT),
                    help="Volcengine resource id (default standard 2.0 "
                         f"{VOLC_RESOURCE_DEFAULT}; standard 1.0 volc.bigasr.auc; "
                         "flash volc.bigasr.auc_turbo)")
    ap.add_argument("--volc-language", default=os.environ.get("VOLC_LANGUAGE", "zh-CN"),
                    help="ASR language for flash mode (default zh-CN); "
                         "standard mode auto-detects")
    ap.add_argument("--asr-app", action="append", default=[], metavar="EXE",
                    help="only paste when the foreground exe matches (repeatable; "
                         "default: any window)")
    ap.add_argument("--asr-dry-run", action="store_true",
                    help="recognize but do not paste (test the ASR path)")
    ap.add_argument("--asr-mock-text", default="", metavar="TEXT",
                    help="skip Volcengine and return this text (setup testing)")
    ap.add_argument("--asr-save-wav", default="", metavar="PATH",
                    help="save the last received WAV to this path (debug)")
    ap.add_argument("--no-restore-clipboard", dest="restore_clipboard",
                    action="store_false",
                    help="leave the transcript in the clipboard after pasting")
    ap.set_defaults(restore_clipboard=True)
    args = ap.parse_args()

    if args.act_app is None:
        args.act_app = list(DEFAULT_ACT_APPS)
    args.act_token = load_act_token(args.act_token)
    args.volc_api_key = load_volc_key(args.volc_api_key)

    # 输出重定向到文件时 Python 默认块缓冲：横幅（设备要填的地址、防火墙命令）
    # 会卡在缓冲区里，tail 日志的人看不到。整个脚本就是给人看的，改行缓冲。
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except Exception:
        pass
    sys.stdout = _StampWriter(sys.stdout)

    if args.no_push:
        args.udp_port = 0

    bridge = Bridge(args)
    if args.act_token:
        threading.Thread(target=warm_uia, daemon=True).start()

    # 命令行指定的设备地址并进记忆：这是"设备存着别的网络的桥接地址、自己永远
    # 找不到这里"时的引导办法（设备不会来轮询，桥接也就无从知道它在哪）
    for ip in args.announce_to:
        new = remember_device(bridge.state_path, bridge.known_devices, ip)
        if new is not bridge.known_devices:
            bridge.known_devices = new
            print(f"announce target added: {ip}", flush=True)
    if args.announce_to:
        try:
            with open(bridge.state_path, "w", encoding="utf-8") as f:
                json.dump(bridge.known_devices, f, ensure_ascii=False)
        except OSError as e:
            print(f"cannot save {bridge.state_path}: {e}", file=sys.stderr, flush=True)

    # hook 入口先绑定：绑不上就说明已经有实例在跑，直接退出。绝不能落到
    # "HTTP 端口绑上了、事件收不到"的半死状态——那种状态下设备一切正常，
    # 界面却永远不动，排查会从最不该怀疑的地方开始。
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        udp.bind(("127.0.0.1", args.hook_port))
    except OSError as e:
        print(f"cannot bind udp 127.0.0.1:{args.hook_port} -- {e}\n"
              f"another bridge is probably running (its port can be seen with "
              f"netstat -ano | findstr {args.hook_port})", file=sys.stderr)
        return 1
    threading.Thread(target=bridge.udp_listener, args=(udp,), daemon=True).start()

    # 发现端口绑在 0.0.0.0 上（要收局域网广播）；绑不上只警告不退出——
    # 老固件靠 agent_url 也照常能用，不值得因此拒绝启动。
    if not args.no_discovery:
        disc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            disc.bind(("0.0.0.0", args.discovery_port))
        except OSError as e:
            print(f"!! cannot bind udp 0.0.0.0:{args.discovery_port} -- {e}\n"
                  f"   discovery disabled; the device needs a static agent_url",
                  file=sys.stderr, flush=True)
            disc = None
        if disc is not None:
            threading.Thread(target=bridge.discovery_listener, args=(disc,),
                             daemon=True).start()
            # 设备自己找不到桥接时（它记的是别的网络的地址），由这一侧主动喊
            threading.Thread(target=bridge.announcer, daemon=True).start()

    ip = lan_ip()
    print(f"ZCode agent bridge up")
    print(f"  device poll   http://{ip}:{args.http_port}/agent")
    print(f"  instant push  udp   -> device:{args.udp_port} "
          f"({'disabled' if args.no_push else 'after first poll'})")
    if args.no_discovery:
        print(f"  discovery     disabled (firmware config: "
              f"\"agent_url\": \"http://{ip}:{args.http_port}/agent\")")
    else:
        print(f"  discovery     the device finds this bridge by itself; "
              f"agent_url may be left empty")
    if args.act_token:
        print(f"  act           enabled for {', '.join(args.act_app)}"
              f"{' (dry run)' if args.act_dry_run else ''}")
    else:
        print(f"  act           disabled (create {ACT_TOKEN_FILE} or pass --act-token)")

    asr_ready = bool(args.volc_api_key or (args.volc_app_key and args.volc_access_key))
    asr_mode = ("standard" if args.volc_resource_id in VOLC_STANDARD_RESOURCES
                else "flash")
    asr_lang = args.volc_language if asr_mode == "flash" else "auto"
    if not args.act_token:
        print(f"  asr           disabled (device auth needs {ACT_TOKEN_FILE})")
    elif args.asr_mock_text:
        print(f"  asr           MOCK -> {args.asr_mock_text!r}"
              f"{' (dry run)' if args.asr_dry_run else ''}")
    elif not asr_ready:
        print(f"  asr           waiting for {VOLC_KEY_FILE} / VOLC_API_KEY")
    else:
        print(f"  asr           volcengine {asr_mode} {args.volc_resource_id} "
              f"({asr_lang})"
              f"{' (dry run)' if args.asr_dry_run else ''}")
    print(f"  if the device cannot reach it, allow the ports once (admin cmd):")
    print(f"    netsh advfirewall firewall add rule name=\"ZCode agent bridge\" "
          f"dir=in action=allow protocol=TCP localport={args.http_port} profile=private")
    if not args.no_discovery:
        print(f"    netsh advfirewall firewall add rule name=\"ZCode agent discovery\" "
              f"dir=in action=allow protocol=UDP localport={args.discovery_port} profile=private")
    print()

    try:
        Server(("0.0.0.0", args.http_port), make_handler(bridge)).serve_forever()
    except OSError as e:
        print(f"cannot listen on :{args.http_port} -- {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
