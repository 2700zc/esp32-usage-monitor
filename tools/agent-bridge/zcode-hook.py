#!/usr/bin/env python3
"""ZCode hook entry point: forward one hook event to the local agent bridge.

Hooks run inline in the session, so this must return in milliseconds and it
must never be able to fail a session:

  * stdin is read byte-wise until the payload parses, which does not depend
    on the writer closing the pipe (a read that waits for EOF -- or for a
    buffer to fill -- would hang the session if it does not);
  * only the handful of fields the bridge needs are forwarded, which keeps
    the datagram small and constant-size (a prompt can be huge);
  * the UDP send is fire-and-forget, so a stopped bridge costs nothing;
  * stdout stays empty and the exit code is always 0 -- hook stdout is
    parsed as JSON by ZCode and a malformed result is treated as a failure.
"""

import json
import os
import re
import socket
import sys
import threading
import time
import traceback

# ZCode hands hooks a Claude-shaped payload (snake_case) but its native event
# objects use camelCase; accept either spelling so the wiring cannot depend on
# which one arrives.
FIELDS = {
    "hook_event_name": ("hook_event_name", "hookEventName"),
    "tool_name": ("tool_name", "toolName"),
    "session_id": ("session_id", "sessionId"),
    "cwd": ("cwd",),
    "agent_type": ("agent_type", "agentName"),
    "permission_mode": ("permission_mode", "mode"),
}

MAX_PAYLOAD = 1 << 20
READ_DEADLINE_SEC = 1.5   # 必须远小于 ZCode 的 hook 超时（5 秒），它到点会杀进程
DETAIL_MAX = 60

FILE_TOOLS = ("Read", "Write", "Edit", "MultiEdit", "NotebookEdit")
# 抢救不完整载荷时要抠的工具字段（都是短字符串，且在 toolInput 里靠前）
TOOL_KEYS = ("command", "description", "file_path", "path", "pattern", "query", "url")

# 长串无分隔的字母数字（API key、哈希、base64）既不该上屏，也不该进这个
# 局域网明文接口——Bash 命令里 `curl -H "Authorization: Bearer sk-xxx"` 是
# 最常见的泄入口。
LONG_TOKEN = re.compile(r"[A-Za-z0-9_\-]{20,}")


def field(src: dict, *keys) -> str:
    for k in keys:
        v = src.get(k)
        if isinstance(v, str) and v:
            return v
    return ""


def tool_detail(name: str, ti: dict) -> str:
    """一句话说清"这个工具此刻在做什么"。

    优先取工具的目标物（文件名、pattern、命令本体），而不是模型写的
    description：描述是给人读的散文，截断到卡片宽度后只剩 "Show working tree…"，
    而命令 "git status --porcelain" 截断后仍是有效信息。取不到目标物时再退回
    描述——总比卡片空着强。
    """
    def s(k: str) -> str:
        v = ti.get(k)
        return v if isinstance(v, str) else ""

    if name == "Bash":
        cmd = s("command")
        if cmd:
            return LONG_TOKEN.sub("…", cmd)
    elif name in FILE_TOOLS:
        p = s("file_path") or s("path")
        if p:
            return os.path.basename(p.replace("\\", "/"))
    elif name in ("Grep", "Glob"):
        if s("pattern"):
            return s("pattern")
    elif name == "WebFetch":
        u = s("url")
        if u:
            return u.split("/")[2] if u.count("/") >= 2 else u
    elif name == "WebSearch":
        if s("query"):
            return s("query")
    elif name in ("Agent", "Task"):
        if s("description"):
            return s("description")

    for k in ("description", "file_path", "path", "pattern", "query", "command"):
        if s(k):
            return s(k)
    return ""


def build_message(src: dict) -> dict:
    """挑出桥接需要的字段，附上工具动作。纯函数，便于单测（见 _test_detail.py）。"""
    msg = {}
    for out_key, aliases in FIELDS.items():
        v = field(src, *aliases)
        if v:
            msg[out_key] = v
    # 工具动作只随 PreToolUse / PermissionRequest 到达；其余事件载荷里的
    # toolInput 是上一次调用的残留，带上会让卡片显示一个早已结束的动作。
    if msg.get("hook_event_name") in ("PreToolUse", "PermissionRequest"):
        ti = src.get("tool_input") or src.get("toolInput")
        if isinstance(ti, dict):
            detail = tool_detail(field(src, "tool_name", "toolName"), ti)
            if detail:
                msg["detail"] = detail[:DETAIL_MAX]
    return msg


def decode(buf) -> str:
    # errors="replace"：载荷里混进非法 UTF-8 时，宁可变出一个替换字符，也不要抛异常
    return bytes(buf).decode("utf-8", "replace").rstrip()


def grab(text: str, *keys: str) -> str:
    """从文本里抠一个 `"key": "value"` 字符串字段（value 按 JSON 转义解析）。"""
    for k in keys:
        m = re.search(r'"%s"\s*:\s*"((?:[^"\\]|\\.)*)"' % re.escape(k), text)
        if m:
            try:
                return json.loads('"%s"' % m.group(1))
            except Exception:
                return ""
    return ""


def salvage(text: str) -> dict:
    """载荷不完整（或 JSON 本身坏了）时的抢救：字段级抠值。

    载荷开头就是这些字段，所以哪怕被截断，需要的东西通常都在。丢掉整个事件
    的话，界面上就少一次状态更新。"""
    out = {}
    for out_key, aliases in FIELDS.items():
        v = grab(text, *aliases)
        if v:
            out[out_key] = v
    ti = {}
    for k in TOOL_KEYS:
        v = grab(text, k)
        if v:
            ti[k] = v
    if ti:
        out["tool_input"] = ti
    return out


def sufficient(text: str) -> bool:
    """值得立刻收工吗：事件名、会话号到手，工具类事件还要等到工具名。

    toolName 排在 toolInput 之后，而 Write 这类工具的 toolInput 是整篇文件
    内容——所以"够用"这个判断只在真正够用、且不必等长尾数据时才成立。
    """
    f = salvage(text)
    event = f.get("hook_event_name", "")
    if not event or not f.get("session_id"):
        return False
    if event in ("PreToolUse", "PermissionRequest") and not f.get("tool_name"):
        return False
    return True


def parse_payload(raw: bytes) -> dict:
    text = raw.decode("utf-8", "replace").strip()
    try:
        obj = json.loads(text)
        if isinstance(obj, dict):
            return obj
    except Exception:
        pass
    return salvage(text)


def note_error(where: str) -> None:
    """把意外异常记到脚本旁边的日志里。

    hook 必须"永不失败"（不能拖累会话），但"永不失败"很容易变成"静默空转"：
    文件里少一个 import，表现就是 hook 一路成功退出、桥接什么也收不到，而
    ZCode 日志里只字不提。所以异常一律落盘，别让它无声无息。"""
    try:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hook-errors.log")
        if os.path.exists(path) and os.path.getsize(path) > 64 * 1024:
            return                      # 不设轮转，但不让它无限长大
        with open(path, "a", encoding="utf-8") as f:
            f.write(f"--- {time.strftime('%Y-%m-%d %H:%M:%S')} {where}\n")
            f.write(traceback.format_exc())
    except Exception:
        pass


def read_payload() -> bytes:
    """读到"够用"为止，最多等 READ_DEADLINE_SEC。

    这里每一步都是在躲同一个坑：os.read() 会一直阻塞到有数据为止，而写入方
    什么时候写完、写不写得完，都不受我们控制。实测 ZCode 确实会发出不完整的
    载荷——那种情况下如果继续读，就会一路挂到 ZCode 的 5 秒 hook 超时，把用户
    的一次工具调用白等 5 秒（日志里表现为 hook.run.failed / durationMs≈5014）。
    所以：
      * 读到整份 JSON 收尾（`}`/`]`）就停——它要么能解析，要么本身就是坏载荷，
        再读一定是空等；
      * 需要的字段齐了就停——Write 这类工具会带上整篇文件内容，等它读完纯属浪费；
      * 上面两条都不成立时由守护线程兜底，到点直接返回已经读到的部分。
    """
    shared = bytearray()
    done = threading.Event()

    def reader() -> None:
        try:
            fd = sys.stdin.fileno()
            while len(shared) < MAX_PAYLOAD:
                chunk = os.read(fd, 4096)
                if not chunk:
                    break  # EOF：载荷到此为止
                # 必须用 extend：`shared += chunk` 会被当成对闭包变量的赋值，
                # 于是 shared 在这个函数里变成局部名，len(shared) 直接抛
                # UnboundLocalError（这个坑踩过一次，见 hook-errors.log）
                shared.extend(chunk)
                text = decode(shared)
                if text.endswith(("}", "]")):
                    break
                if sufficient(text):
                    break
        except Exception:
            note_error("read_payload/reader")
        finally:
            done.set()

    threading.Thread(target=reader, daemon=True).start()
    done.wait(READ_DEADLINE_SEC)
    return bytes(shared).strip()


def main() -> int:
    try:
        if sys.stdin.isatty():
            return 0
    except Exception:
        return 0

    raw = read_payload()
    if not raw:
        return 0

    # Set ZCODE_AGENT_BRIDGE_DEBUG=1 when wiring this up: the raw payload
    # ZCode hands the hook lands next to the script, which is the only way to
    # see what the client actually sends if the bridge reports nothing.
    if os.environ.get("ZCODE_AGENT_BRIDGE_DEBUG"):
        try:
            with open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                   "hook-debug.log"), "ab") as f:
                f.write(b"--- " + str(time.time()).encode() + b"\n" + raw[:4096] + b"\n")
        except OSError:
            pass

    try:
        src = parse_payload(raw)
        if not src.get("hook_event_name"):
            return 0            # 连事件名都没读到，没有可报的东西
        payload = json.dumps(build_message(src), ensure_ascii=False).encode("utf-8")
    except Exception:
        payload = raw[:2048]

    port = int(os.environ.get("ZCODE_AGENT_BRIDGE_PORT", "8788"))
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.sendto(payload, ("127.0.0.1", port))
        s.close()
    except OSError:
        # 桥接没在跑是常态，不该打扰会话；但也不能像上次那样——环境异常导致
        # socket 根本建不起来时，一眼看过去和"桥接没开"完全一样
        note_error("send")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        # 会话优先：hook 绝不能让 ZCode 报错，但异常必须留下来供排查
        note_error("main")
        sys.exit(0)
