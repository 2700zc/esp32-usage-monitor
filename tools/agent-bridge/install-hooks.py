#!/usr/bin/env python3
"""Install (or remove) the ZCode hooks that feed the agent-status bridge.

Targets the user-scope config (~/.zcode/cli/config.json) by default so every
workspace on this machine reports its status to the bridge; --workspace
installs into the current repo instead.

Idempotent and non-destructive: entries belonging to this bridge are replaced,
every other hook and top-level key is left alone, and the config is backed up
before it is rewritten.

    python install-hooks.py              # install / refresh
    python install-hooks.py --uninstall  # remove only this bridge's hooks
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import time
from pathlib import Path

# PreToolUse/PostToolUse are deliberately both installed: without the Post
# event a session that ran one command would stay pinned to "running Bash"
# for the whole remainder of the turn.
EVENTS = (
    "UserPromptSubmit",
    "PreToolUse",
    "PostToolUse",
    "PostToolUseFailure",
    "PermissionRequest",
    "Stop",
)

HOOK_SCRIPT = "zcode-hook.py"
TIMEOUT_MS = 5000


def config_path(workspace: bool) -> Path:
    if workspace:
        return Path.cwd() / ".zcode" / "config.json"
    return Path.home() / ".zcode" / "cli" / "config.json"


def is_ours(entry: dict) -> bool:
    """True for hook groups this installer created."""
    if not isinstance(entry, dict):
        return False
    for hook in entry.get("hooks", []) or []:
        if not isinstance(hook, dict):
            continue
        argv = [str(hook.get("command", ""))] + [str(a) for a in hook.get("args", []) or []]
        if any(HOOK_SCRIPT in a for a in argv):
            return True
    return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--uninstall", action="store_true")
    ap.add_argument("--workspace", action="store_true",
                    help="install into <repo>/.zcode/config.json instead of the user config")
    args = ap.parse_args()

    cfg_file = config_path(args.workspace)
    script = Path(__file__).resolve().parent / HOOK_SCRIPT
    if not args.uninstall and not script.exists():
        print(f"missing {script}", file=sys.stderr)
        return 1

    if cfg_file.exists():
        try:
            cfg = json.loads(cfg_file.read_text(encoding="utf-8"))
        except Exception as e:
            print(f"{cfg_file} is not valid JSON: {e}", file=sys.stderr)
            return 1
        if not isinstance(cfg, dict):
            print(f"{cfg_file}: expected a JSON object", file=sys.stderr)
            return 1
        backup = cfg_file.with_suffix(f".json.bak-{time.strftime('%Y%m%d-%H%M%S')}")
        shutil.copy2(cfg_file, backup)
        print(f"backed up  {backup.name}")
    else:
        cfg = {}
        cfg_file.parent.mkdir(parents=True, exist_ok=True)

    hooks = cfg.setdefault("hooks", {})
    events = hooks.setdefault("events", {})

    entry = {"hooks": [{
        "type": "process",                      # argv, no shell: works the same on any shell
        "command": sys.executable,
        "args": [str(script)],
        "timeoutMs": TIMEOUT_MS,
    }]}

    changed = 0
    for ev in EVENTS:
        groups = [g for g in events.get(ev, []) if not is_ours(g)]
        if not args.uninstall:
            groups.append(entry)
        if groups:
            events[ev] = groups
        else:
            events.pop(ev, None)
        changed += 1

    if not events:
        hooks.pop("events", None)
    if not hooks:
        cfg.pop("hooks", None)
    else:
        # Configuration-file hooks are disabled unless this is set; plugin
        # hooks are what normally enables the runner.
        if not args.uninstall:
            hooks["enabled"] = True

    cfg_file.write_text(json.dumps(cfg, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    action = "removed from" if args.uninstall else "installed into"
    print(f"{len(EVENTS)} hook events {action} {cfg_file}")
    if not args.uninstall:
        print(f"  interpreter {sys.executable}")
        print(f"  script      {script}")
        print(f"  timeout     {TIMEOUT_MS} ms per event")
        print("  takes effect in new sessions (restart ZCode to be sure)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
