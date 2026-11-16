#!/usr/bin/python3
"""Packaged bridge from package transactions to existing user managers.

Root only contacts already-running regular-user managers. The service runs the
existing CLI as that user; this helper never opens or edits a user's home.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import time
import os

SYSTEMCTL = "/usr/bin/systemctl"
MAX_MANAGERS = 256
MAX_OUTPUT = 1024 * 1024


def regular_uid_min(text):
    match = re.search(r"(?m)^\s*UID_MIN\s+([0-9]+)\s*(?:#.*)?$", text)
    if not match or int(match[1]) == 0:
        raise ValueError("UID_MIN is unavailable; refusing to guess regular-user accounts")
    return int(match[1])


def manager_uids(output, uid_min):
    if uid_min <= 0:
        raise ValueError("invalid regular-user UID boundary")
    if len(output.encode()) > MAX_OUTPUT:
        raise ValueError("user-manager listing exceeds the limit")
    rows = json.loads(output)
    if not isinstance(rows, list) or len(rows) > MAX_MANAGERS:
        raise ValueError("invalid or oversized user-manager listing")
    uids = set()
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError("invalid user-manager row")
        unit = row.get("unit", "")
        if not isinstance(unit, str):
            raise ValueError("invalid user-manager unit name")
        match = re.fullmatch(r"user@(0|[1-9][0-9]*)\.service", unit)
        if not match or row.get("active") != "active" or row.get("sub") != "running":
            raise ValueError("unexpected user-manager identity/state")
        uid = int(match[1])
        if uid >= uid_min:
            uids.add(uid)
    return sorted(uids)


def invoke(arguments, timeout):
    completed = subprocess.run(arguments, capture_output=True, text=True, timeout=timeout, check=False)
    if len(completed.stdout.encode()) > MAX_OUTPUT or len(completed.stderr.encode()) > MAX_OUTPUT:
        raise ValueError("systemctl output exceeds the limit")
    return completed


def notify(action, dry_run=False, runner=invoke, uid_min=None):
    if action not in ("install", "upgrade", "remove"):
        raise ValueError("unsupported lifecycle action")
    if uid_min is None:
        uid_min = regular_uid_min(Path("/etc/login.defs").read_text())
    listing = runner([SYSTEMCTL, "--system", "--no-pager", "--output=json", "list-units",
                      "--state=running", "user@*.service"], 10)
    if listing.returncode:
        raise RuntimeError("could not enumerate running user managers: " + listing.stderr.strip())
    uids = manager_uids(listing.stdout, uid_min)
    service = "omarchy-control-agent-" + ("remove" if action == "remove" else "reconcile") + ".service"
    results = []
    deadline = time.monotonic() + 120
    for uid in uids:
        prefix = [SYSTEMCTL, "--user", f"--machine={uid}@.host", "--no-ask-password", "--no-pager"]
        commands = [prefix + ["daemon-reload"], prefix + ["start", service]]
        row = {"uid": uid, "commands": commands, "state": "planned"}
        if not dry_run:
            for command in commands:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    row.update(state="unresolved", error="overall notification deadline exceeded")
                    break
                try:
                    result = runner(command, min(65, remaining))
                    if result.returncode:
                        row.update(state="unresolved", error=result.stderr.strip() or "user service failed")
                        break
                except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                    row.update(state="unresolved", error=str(error))
                    break
            else:
                row["state"] = "service_completed"
        results.append(row)
    return {"protocolVersion": 1, "prototype": False, "action": action, "users": results,
            "ok": all(row["state"] in ("planned", "service_completed") for row in results),
            "scope": "running regular-user managers only; no root writes to user homes",
            "verificationScope": "systemctl job completion; user CLI results and harness loading require separate checks",
            "inactiveUsers": ("a prepared retained runtime cleans owned registrations at the next user-manager start; unprepared legacy users retain stock fallback"
                              if action == "remove" else "global user-unit dependency activates reconciliation at next manager start"),
            "harnessDiscoveryVerified": False, "automaticActivationPackaged": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["install", "upgrade", "remove"])
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if os.getuid() != 0 or os.geteuid() != 0:
        print(json.dumps({"ok": False, "error": "package notifications require the system context"}))
        return 1
    try:
        result = notify(args.action, args.dry_run)
    except (ValueError, OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(json.dumps({"ok": False, "protocolVersion": 1, "prototype": False, "error": str(error)}))
        return 1
    print(json.dumps(result))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
