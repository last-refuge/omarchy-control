# Durable display previews

API v1 provides complete output discovery and full-layout previews with an
independent watchdog. The existing single-output `display preview <output> <x>
<y>` interface remains available for old scripts.

```sh
omarchy-control display status
omarchy-control display check --dry-run
omarchy-control display check --yes
omarchy-control display layout /absolute/layout.json --dry-run
omarchy-control display layout /absolute/layout.json --yes
omarchy-control display preview status [token]
omarchy-control display preview revert <token>
omarchy-control display preview keep <token> --yes
```

Discovery awaits bounded monitor and compositor-version reads. A failed source
remains unknown; an empty output array is a known empty inventory. No command
above proves physical visibility. The recovery check briefly moves one enabled
output eight pixels, observes it, then verifies the watchdog's timed restoration.
Its pass applies only to the reported Hyprland version.

The layout file is a UTF-8 JSON array, one row per reported output. `output` is
required. Omitted mutable fields inherit fresh reported state. Supported fields
are `mode`, `x`, `y`, `scale`, `transform`, `enabled`, `mirrorOf` and `vrr`.
Coordinates are integers in -131072..131072, transform is 0..7, and enabled scales
are 0.25..8. Modes must be offered or currently in use. Scales normalize to valid
mode divisions. At least one output stays enabled. Unknown keys, duplicate outputs,
missing outputs, invalid types and stale supplied identity fields are refused.
Full rows from `display status` can be reused; descriptive fields are read-only.

```json
[
  {"output":"eDP-1","x":0,"y":0,"scale":1.25,"enabled":true},
  {"output":"DP-1","x":1536,"y":0,"scale":1,"enabled":true}
]
```

A real start requires `--yes` and a passed recovery check. The timer is armed
before the first output change, with an absolute receipt deadline and a monotonic
watchdog window of at most 15 seconds. Start returns `outcome: preview` only after
reported geometry matches. The calling process can exit. A later CLI process or
the GUI can adopt the same token. The preview countdown uses the original deadline.

Keep requires the person's confirmation that the preview is visible and usable.
An agent must not infer that from a successful geometry read. Keep's `--yes`
acknowledges that confirmation and persistence of the owned module/import. The
watchdog checks reported hardware identity, session/version, geometry and original
file fingerprints before saving. It performs persistence itself, then verifies
file bytes and reported geometry before recording `kept`. Another caller cannot
reuse a reverted or expired token to keep a different layout.

Receipts live in the private `display-previews` directory below Control's state
root. Runtime plans, control files and results live under
`$XDG_RUNTIME_DIR/omarchy-control`. Tokens bind the Hyprland instance signature,
reported identity, original layout, requested layout, deadline and owned-file
fingerprints. A shared change lock excludes competing writers; unresolved previews
also exclude coordinated changes and profile restore leases. Unrelated read-only
discovery remains available. Terminal receipts/results are retained, with 100
historical terminal records and a 256-receipt enumeration limit.

Timeout or explicit revert restores original-identity outputs that remain present,
then verifies reported geometry and an enabled output. If Keep began writing,
private before-file captures and pre-recorded after hashes allow guarded recovery
of interrupted persistence too. Later edits are preserved. Capture accepts regular,
owned, singly linked persistence files of at most 256 KiB each. An unavailable read,
changed output, failed write or inability to restore safely yields `restore_failed`
and retains the plan for retry/review. A fallback enabled output is never called a
verified restoration of the original arrangement. A different session cannot keep
or revert an old token. Historical receipts describe recorded outcomes, not current
physical appearance or a guarantee that files have remained unchanged.

`tools/display-preview-check.py` runs the production CLI and real detached
watchdog against a stateful fixture compositor in an explicitly enabled Docker
lab. Qt tests also cover caller exit, GUI adoption, expiry, partial persistence
recovery, ignored writes, hand edits and legacy watchdog behavior. These checks
exercise protocol and filesystem behavior. Real Hyprland, driver recovery,
hot-plugging and physical usability remain separate release evidence.
