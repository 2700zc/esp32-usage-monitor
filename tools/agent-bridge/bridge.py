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
import json
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

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

class Bridge:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.lock = threading.Lock()
        self.sessions: dict[str, dict] = {}
        self.seq = 0
        self.device_ip: str | None = None
        self.device_seen = 0.0
        self.last_push_seq = -1
        self.udp_out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

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
            else:
                self._send(b"not found", "text/plain", 404)

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
    ap.add_argument("--no-push", action="store_true",
                    help="poll only; disable the instant UDP push to the device")
    args = ap.parse_args()

    # 输出重定向到文件时 Python 默认块缓冲：横幅（设备要填的地址、防火墙命令）
    # 会卡在缓冲区里，tail 日志的人看不到。整个脚本就是给人看的，改行缓冲。
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except Exception:
        pass

    if args.no_push:
        args.udp_port = 0

    bridge = Bridge(args)

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
