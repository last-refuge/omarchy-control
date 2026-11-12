---
name: omarchy-control
description: Read and change Omarchy desktop settings through the verified `omarchy-control` CLI. Use for appearance, displays, input, power, default apps, and other machine settings, and to find which Omarchy tool handles a task.
---

# Omarchy Control

`omarchy-control` is a reviewed interface to Omarchy's settings. Each change is
validated, journaled, and verified by reading the value back. Prefer it to
editing Omarchy's files.

```bash
omarchy-control search "stop sleeping"   # Find the setting or tool; runs nothing
omarchy-control get power.idle.lock      # Current value
omarchy-control plan power.idle.lock 600 # Validate without changing anything
omarchy-control apply power.idle.lock 600
omarchy-control history                  # What changed, with transaction ids
omarchy-control undo <transaction>       # Refuses if something edited it since
omarchy-control doctor                   # Compatibility and read checks
omarchy-control update check             # Is a newer version of this app out?
omarchy-control icons list                # Installed icon packs, and any problems reading them
omarchy-control apply appearance.icons <id or follow-theme>  # Choose one; `get --json` has the details
```

Add `--json` for machine-readable output. Commands exit 0 or 1, with errors on
stderr. `apply` exits 1 and explains itself when a change needs `--yes`: show that
consequence to the user before passing `--yes`.

My Omarchy profiles save this computer's choices and restore them elsewhere:

```bash
omarchy-control profile status                      # Saved here, published, and recent restores
omarchy-control profile capture --category idle --dry-run  # What a save would change
omarchy-control profile save --message "Before reinstalling"
omarchy-control profile plan --revision published   # Review a restore; changes nothing
omarchy-control profile apply <plan-id> --yes       # Only after the user approves the plan
```

Show the user a restore plan, conflicts included, before applying it. Apps install
and remove in the user's terminal, where pacman and sudo ask; publishing and
connecting GitHub are the user's decisions.

A nonzero `apply` means the value was not confirmed. Report the message rather
than retrying with a different mechanism. Workflows such as installs, fingerprint
setup, and updates are interactive helpers: tell the user which one to open.
