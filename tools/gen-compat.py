#!/usr/bin/env python3
"""Records which Omarchy command each typed setting's write was reviewed against.

Writes src/data/compat.json from the catalog, the discovery inventory, and the
commands installed here. Run this only after re-reading a changed command's source
and confirming its adapter still fits. It is the reviewer's signature.
"""
import hashlib, json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
inventory = json.load(open(ROOT + "/docs/inventory-2026-11-05.json"))
routes = sorted(((c["route"], c["binary"]) for c in inventory["commands"]), key=lambda r: -len(r[0]))
settings = json.load(open(ROOT + "/src/data/settings.json"))

out = {"omarchy": subprocess.run(["omarchy", "version"], capture_output=True, text=True).stdout.strip(), "hyprland": "0.56", "settings": {}, "commands": {}}
for entry in settings:
    write = entry.get("write") or {}
    if write.get("kind") != "cmd":
        continue
    text = " ".join(part for part in write["argv"] if "{" not in part)
    binary = next((b for route, b in routes if (text + " ").startswith(route + " ")), None)
    if not binary:
        continue
    out["settings"][entry["id"]] = binary
    with open("/usr/bin/" + binary, "rb") as f:
        out["commands"][binary] = hashlib.sha256(f.read()).hexdigest()
# Settings resting on how several commands behave together, not on one setter. The icon
# pack relies on what omarchy-theme-set-gnome writes, on omarchy-theme-set running hooks
# after that, and on omarchy-hook running the owned hook.
for setting, binaries in {"appearance.icons": ["omarchy-hook", "omarchy-theme-set", "omarchy-theme-set-gnome"]}.items():
    out["settings"][setting] = binaries
    for binary in binaries:
        with open("/usr/bin/" + binary, "rb") as f:
            out["commands"][binary] = hashlib.sha256(f.read()).hexdigest()
# Profile restores that are not settings: themes installed from Git the way
# omarchy-theme-install does, into the same folder under the same name; the bar's layout
# written the way omarchy-bar commits shell.json; and plugins switched with Omarchy's own
# commands (docs/MY-OMARCHY.md).
out["adapters"] = {}
for adapter, binaries in {"profile.themes": ["omarchy-theme-install"],
                          "profile.bar": ["omarchy-bar", "omarchy-shell-config"],
                          "profile.plugins": ["omarchy-plugin-disable", "omarchy-plugin-enable", "omarchy-plugin-list"]}.items():
    out["adapters"][adapter] = binaries
    for binary in binaries:
        with open("/usr/bin/" + binary, "rb") as f:
            out["commands"][binary] = hashlib.sha256(f.read()).hexdigest()
json.dump(out, open(ROOT + "/src/data/compat.json", "w"), indent=1, sort_keys=True)
open(ROOT + "/src/data/compat.json", "a").write("\n")
print(f"{len(out['settings'])} settings pinned to {len(out['commands'])} commands, on Omarchy {out['omarchy']}")
