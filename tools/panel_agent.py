#!/usr/bin/env python3
"""Dev tool: send one command to the LookMatch panel's test agent and print the result.

The panel (dev build) polls <data>/agent/cmd.json while <data>/agent/enabled exists.
  tools/panel_agent.py ping
  tools/panel_agent.py applyReference '{"scope":"selection","path":"/path/ref.png"}'
  tools/panel_agent.py applyParams '{"scope":"selection","params":{"Grain":40}}'
"""
import json, os, sys, time

DATA = os.path.expanduser("~/Library/Application Support/PRO PLUGS/LookMatch/agent")

def send(name, args=None, timeout=30.0):
    os.makedirs(DATA, exist_ok=True)
    if not os.path.exists(os.path.join(DATA, "enabled")):
        raise SystemExit("agent disabled: touch " + os.path.join(DATA, "enabled"))
    cid = f"{time.time():.6f}"
    tmp = os.path.join(DATA, "cmd.json.tmp")
    with open(tmp, "w") as f:
        json.dump({"id": cid, "name": name, "args": args or {}}, f)
    os.replace(tmp, os.path.join(DATA, "cmd.json"))
    end = time.time() + timeout
    while time.time() < end:
        try:
            with open(os.path.join(DATA, "result.json")) as f:
                res = json.load(f)
            if res.get("id") == cid:
                return res
        except (OSError, ValueError):
            pass
        time.sleep(0.25)
    return {"id": cid, "ok": False, "error": f"no answer in {timeout:.0f}s (is the LookMatch panel open?)"}

if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    res = send(sys.argv[1], json.loads(sys.argv[2]) if len(sys.argv) > 2 else None)
    print(json.dumps(res, indent=2))
    sys.exit(0 if res.get("ok") else 1)
