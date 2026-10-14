"""zcode-hook.py 的自测：动作提取、事件分流、以及"真的发出去了吗"。

跑法（在 tools/agent-bridge 下）：python _selftest.py
只依赖标准库；不碰桥接、不碰设备，用一个临时 UDP 端口收报文。

冒烟测试这一段是必要的：hook 的顶层 except 会吞掉一切异常并退出 0，
所以"少一个 import"这种错误的表现是——ZCode 不报错、hook 不报错、桥接
什么都收不到。只有真起进程发一次才照得出来。
"""
import importlib.util
import json
import os
import socket
import subprocess
import sys
import time

spec = importlib.util.spec_from_file_location("hook", "zcode-hook.py")
hook = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hook)

BASH = {"command": "git status --porcelain",
        "description": "Show working tree status in porcelain format"}
CWD = r"C:\Users\admin\AppData\Local\Temp\zhooktest2"

DETAIL_CASES = [
    ("Bash 短命令", "Bash", BASH, "git status --porcelain"),
    ("Bash 长命令", "Bash",
     {"command": "pio run -e esp32-usage-monitor -t upload --upload-port COM5"},
     "pio run -e esp32-usage-monitor -t upload --upload-port COM5"),
    ("Bash 带密钥", "Bash",
     {"command": 'curl -H "Authorization: Bearer sk-abcdefghijklmnopqrstuvwxyz012345" '
                 'https://api.example.com'},
     'curl -H "Authorization: Bearer …" https://api.example.com'),
    ("Bash 无命令", "Bash", {"description": "List files in the repo"}, "List files in the repo"),
    ("Edit 文件", "Edit", {"file_path": r"D:\esp32\esp32-usage-monitor\src\main.cpp"}, "main.cpp"),
    ("Write 新文件", "Write", {"file_path": "/tmp/foo/bar.txt"}, "bar.txt"),
    ("Grep", "Grep", {"pattern": "cfgAgentReady"}, "cfgAgentReady"),
    ("WebFetch", "WebFetch", {"url": "https://platform.deepseek.com/usage"}, "platform.deepseek.com"),
    ("WebSearch", "WebSearch", {"query": "u8g2 font metrics"}, "u8g2 font metrics"),
    ("Agent", "Agent", {"description": "Explore firmware data flow"}, "Explore firmware data flow"),
    ("未知工具退回描述", "Read", {"description": "Read the config"}, "Read the config"),
    ("完全取不到", "MysteryTool", {}, ""),
]

fails = 0


def check(label: str, got, want) -> None:
    global fails
    if got != want:
        fails += 1
        print(f"  [DIFF] {label:<22} -> {got!r}   期望 {want!r}")
    else:
        print(f"  [ OK ] {label:<22} -> {got!r}")


print("1) 动作提取")
for label, tool, ti, want in DETAIL_CASES:
    check(label, hook.tool_detail(tool, ti), want)

print()
print("2) 事件分流（只有工具类事件该带 detail）")
for event, want_has in [("PreToolUse", True), ("PermissionRequest", True),
                        ("PostToolUse", False), ("Stop", False),
                        ("UserPromptSubmit", False)]:
    msg = hook.build_message({"hook_event_name": event, "session_id": "s", "cwd": CWD,
                              "tool_name": "Bash", "tool_input": BASH})
    check(event, "detail" in msg, want_has)

print()
print("3) 端到端冒烟：真起 hook 进程，看报文有没有发出来")

sink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sink.bind(("127.0.0.1", 0))
port = sink.getsockname()[1]
sink.settimeout(3)


def smoke(label, stdin_bytes, want_event, close=True, deadline_ms=2500):
    global fails
    # 注意必须继承完整环境：剥空环境（只留 ZCODE_AGENT_BRIDGE_PORT）时 Windows
    # 连 Winsock 都初始化不了（WinError 10106），子进程的 socket 建不起来，
    # 看起来就像"hook 不发报文"。
    env = {**os.environ, "ZCODE_AGENT_BRIDGE_PORT": str(port)}
    p = subprocess.Popen([sys.executable, "zcode-hook.py"], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, env=env)
    if stdin_bytes:
        try:
            p.stdin.write(stdin_bytes)
            p.stdin.flush()
        except OSError:
            pass          # hook 提前收工时写入方会拿到 EPIPE，这是设计内的
    if close:
        try:
            p.stdin.close()
        except OSError:
            pass
    t0 = time.perf_counter()
    try:
        p.wait(timeout=6)
    except subprocess.TimeoutExpired:
        p.kill()
        fails += 1
        print(f"  [卡死] {label}")
        return
    ms = (time.perf_counter() - t0) * 1000
    try:
        data, _ = sink.recvfrom(65536)
        got = json.loads(data)
        event = got.get("hook_event_name")
    except Exception:
        event = None
    slow = ms > deadline_ms
    bad = slow or event != want_event
    fails += 0 if not bad else 1
    print(f"  [{'OK ' if not bad else 'BAD'}] {label:<26} {ms:>6.0f}ms  "
          f"收到 event={event!r}")


def payload(event="PreToolUse", **kw):
    return json.dumps({"hook_event_name": event, "session_id": "smoke",
                       "cwd": r"D:\esp32\esp32-usage-monitor", "tool_name": "Bash",
                       "tool_input": BASH, **kw}).encode()


smoke("小载荷（正常路径）", payload(), "PreToolUse")
smoke("大载荷 9KB（提前收工）", json.dumps(
    {"hook_event_name": "PreToolUse", "session_id": "smoke", "cwd": CWD,
     "tool_name": "Bash", "tool_input": {"command": "echo " + "x" * 9000}}).encode(),
    "PreToolUse", close=False)
smoke("截断载荷（抢救）", payload()[:70], "PreToolUse", close=False)
smoke("空 stdin", b"", None, close=False)
smoke("非法 UTF-8", b'{"hook_event_name":"Stop","session_id":"s3","x":"\xff\xfe',
      "Stop", close=False)

sink.close()
print()
print("全部通过" if not fails else f"{fails} 项未通过")
sys.exit(1 if fails else 0)
