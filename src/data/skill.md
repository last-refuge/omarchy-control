---
name: omarchy-control
description: Read and change Omarchy desktop settings through the verified `omarchy-control` CLI. Use for audio, paired Bluetooth devices, services, startup apps, appearance, displays, input, power, default apps, and other machine settings, and to find which Omarchy tool handles a task.
---

# Omarchy Control

`omarchy-control` is a reviewed interface to Omarchy's settings. Use its plans,
journal, and readback results to change settings and confirm what happened.
Prefer it to editing Omarchy's files.

If skill routing needs repair, `omarchy-control skill status` reports filesystem
registration separately from harness loading. `skill plan` is read-only;
`skill reconcile` repairs owned registrations without overwriting custom files.
Registration never refreshes this conversation's cached instructions. Use the
current upstream guides only for tasks outside Control's supported scope, not
to bypass ownership/compatibility refusals or failed verification.

Discover IDs, value limits, supported operations, and remaining CLI gaps from the
installed binary. API v1 is opt-in and returns one JSON response for successes
and failures. Do not add its version flag to a command it does not advertise:

```bash
omarchy-control commands --json --api-version 1
omarchy-control search "stop sleeping" --json --api-version 1
omarchy-control describe power.idle.lock --live --json --api-version 1
omarchy-control coverage --cli --json --api-version 1
```

Workflow descriptions may point to a `nativeSetting` or `nativeOperations`.
Inspect that setting or operation for a planned, verified change. A workflow's
static `launch` remains a handoff and does not verify its effects. For example,
background introductions use the boolean `appearance.background.intros` setting;
its explicit on/off writer skips unchanged values. It verifies only the owned
empty marker, not animation playback. Unsafe/custom marker paths are refused;
unconfirmed writes retain backups without automatic restoration. Committed
changes support guarded history undo.

`preferences crash-mutes` lists per-program crash-notification markers.
`preferences crash-mute <program-basename>` reads one; add `true|false` and
`--dry-run` to plan, then omit `--dry-run` to set it explicitly. Names are plain
basenames, not paths. Custom/linked marker paths are refused. Readback confirms
only the preference, not watcher behavior. Unconfirmed changes retain backups;
there is no automatic history undo. This does not switch crash capture itself.

API v1 supports discovery, typed-setting `get`/`plan`/`apply`/`reset`, journal
`history`/`undo`, and `audio`,
`bluetooth`, `services`, `startup`, installed-app/MIME, system-fact/diagnostics,
shortcut/window-rule and shell plugin/bar/dock operations, network reads, scan
requests, Wi-Fi switch changes and icon discovery. Discover targets
with `audio status`, `bluetooth status`, `services <user|system> status`,
`startup list`, or `apps list`;
`describe <operation-id>` lists parameter grammar and options. Use `--dry-run`
on provider mutations to inspect the plan. Audio stream moves take the current
`streams[].selector`, including its identity, so a recycled index cannot silently
select another app. Powering Bluetooth off or forgetting a device requires
acknowledging the plan's consequences with `--yes`. Pairing stays in Omarchy's
panel. Runtime device operations do not promise undo.

Use `network status` for NetworkManager state, devices, saved profiles and cached
Wi-Fi access points. `network general|devices|saved|wifi` reads only one section.
Inspect `states` and `complete`: failed/unrequested sections are null, while a
known empty list is `[]`. Unknown device connection state is null; an unknown
connectivity report does not establish internet access. Profiles retain UUIDs
even when names repeat, and access points retain device/BSSID identities even
when SSIDs repeat. Names are display strings, not raw SSID bytes. These calls
request no scan, connectivity check or secrets; cached AP age is unknown.
For an explicit scan, use `network rescan [interface] --dry-run`, then omit
`--dry-run` to request it. Discover the current interface with `network devices`;
omitting it selects all eligible managed Wi-Fi adapters. `scan_requested` means
NetworkManager accepted the request, with `verified: false`: scan completion and
fresh AP data remain unverified. Inspect per-interface results on `partial` or
`unknown`; do not automatically retry an unconfirmed request. Read `network wifi`
later for cached results. The history receipt covers request acceptance only and
has no undo. Use `network radio true|false --dry-run` for the global Wi-Fi switch;
omit `--dry-run` to apply, adding `--yes` when turning it off. Off may disconnect
this remote session; on may automatically reconnect saved networks. Enabling is
refused when the hardware switch is blocked. `verified: true` covers only the
NetworkManager switch readback, with hardware state reported separately. It does
not prove connection usability or internet access. Partial or unreadable results
are not retried or rolled back automatically. Do not infer a usable connection
from a radio switch, scan or cached read.

`network activate <uuid> <interface> --dry-run` and
`network disconnect <interface> --dry-run` plan protected previews; execution needs `--yes`. Discover UUIDs with
`network saved` and interfaces with `network devices`. Activation currently
supports ordinary Ethernet and open Wi-Fi; secured/802.1X activation hands off to
the network panel. A successful start returns `outcome: preview` and
`data.receipt.token`, with a 60-second NetworkManager rollback timer. Use
`network preview status [token]`, `network preview revert <token>`, or
`network preview keep <token> --yes` from a later process. Keep only after the
user confirms the connection is usable; never immediately keep based on daemon
readback. Missing checkpoints or changed daemons are unknown outcomes, not proof
of rollback. Inspect status before retrying. A token's historical receipt does
not verify current connectivity. Control never uses an unprotected CLI fallback.

For displays, `display status` reports outputs, modes, geometry and the recovery
gate. `display layout <json-file> --dry-run` accepts an array with one row per
output; retain `output` and change the desired mode, coordinates, scale, transform,
enablement, mirror or VRR fields. Omitted fields inherit fresh reported values.
If the version's recovery check has not passed, explain its brief nudge and run
`display check --yes`. Starting a layout preview also needs `--yes`; its independent
15-second timer starts before applying. Use `data.receipt.token` with `display
preview status [token]`, `revert <token>`, or `keep <token> --yes` in a later process.
Keep only after the person confirms the display is visible and usable. The watchdog
persists Keep and guards recovery of interrupted file writes. `restore_failed`
retains the plan for retry/review; reported geometry never proves physical usability.
Unresolved display previews exclude other coordinated changes and profile restores.

`preferences pins`, `preferences pin <catalog-id>` and `preferences unpin
<catalog-id>` manage Control search pins; mutations support `--dry-run`. They do
not change the desktop. `profile cancel <run-id>` requests cooperative cancellation
of a running restore. It finishes the current operation, then stops before the
next; inspect `profile status --json` for the final state. Request acceptance is
not completed cancellation. Other profile commands retain their unversioned API.

`history --api-version 1` supplies transaction IDs for `undo <id> --api-version 1`.
Undo refuses nonundoable/already-undone changes and later edits. `reset <id>
--api-version 1` returns a setting to its supported inherited default, which can
differ from its previous value. Inspect `describe` and `get` first; reset/undo
have no dry-run and consequential changes require `--yes`. Runtime provider
actions generally have no automatic undo.

Service actions take a scope, action and concrete `.service` unit. Enable/disable
changes persistent enablement; start/stop/restart changes the running service.
System stop/restart/disable requires `--yes`, which grants no administrator
privilege. CLI changes do not ask for credentials. Startup enable/disable takes
the discovered desktop ID and changes only a future-login preference; remaining
launch conditions still apply, and success does not prove that the app ran.
These operations have no automatic history undo. A failed service readback can
follow a partially applied action; inspect its transaction and current state.

Use `mime list` for common file types, or `mime get <type>` for another concrete
type. `mime set <type> <desktop-id> --dry-run` reports the effective user override
and planned fallback list; omit `--dry-run` to apply. Candidate lists include only
declared MIME support and are not complete. Empty query output is unresolved;
failed reads are unavailable. Successful readback confirms the default desktop ID,
not that the app can open every file. MIME changes have file rollback on failure
and no automatic history undo. `apps launch <desktop-id>` is a handoff with
`outcome: launched` and `verified: false`; do not report a visible window as verified.
Existing `apps launcher ...` calls retain their unversioned interface.

For icon discovery, use `icons list --api-version 1`, then `icons samples <pack-id>`
or `icons search <pack-id> [query] --limit 30`. Search matches icon names in the
selected pack, its inherited packs and hicolor; it covers application/device SVG
and PNG icons. Rows expose file paths and originating packs. Inspect `complete`
and `diagnostics`: unreadable or excessive sources can leave useful partial rows.
`truncated` limits returned search rows; it is separate from incomplete source
reads. These are file lookups, not proof that images decode or desktop apps
render them. `appearance.icons` remains the typed setting for choosing a pack;
icon/launcher mutations otherwise retain their advertised unversioned calls.

For troubleshooting, `system status` reads machine, battery, storage and hooks;
`system <machine|battery|storage|hooks>` isolates one section. Inspect `states`,
`complete` and field-known flags: unknown capacity/free space is not an empty
battery/disk, and an unreadable hook directory is not an empty inventory.
`diagnostics export --dry-run` describes a private local report; export without
that flag waits until collection and saving finish. `exported` verifies the saved
artifact. `diagnostics_incomplete` can still include a saved partial report; inspect
`reportSaved` and `complete`. Nothing is uploaded. Redaction is best effort; review
reports before sharing. The stock debug helper writes a shared raw log; Control
isolates a fingerprint-checked copy in private temporary staging. Report any
compatibility/isolation refusal and use the available system reads for facts.
Ordinary settings changes do not require diagnostics export.

Use `shortcut list` to inspect reported effective bindings and Control's owned
entries separately; `shortcut check "SUPER + SHIFT + F"` checks a chord. Inspect
source states, `complete`, matches/submaps and `collisionState`. `unknown` leaves
keycode or multi-key bindings unresolved; `no_named_match` does not establish
that a chord is globally free. Do not drop unknown modifiers to make input valid.
`rule types` describes supported actions and value constraints; `rule validate
<match> <action> [value]` checks input without reading files or reloading Hyprland.
`validated` is input acceptance only, not a verified change or compositor test.
`rule list --api-version 1` lists owned rules and their current indices, with
explicit unavailable/ownership states. No Lua is executed. Unversioned `rule list`
keeps its JSON array.

Use API v1 explicitly for shortcut/rule writes. `shortcut add <keys> <description>
<command> --api-version 1 --dry-run` and `rule add <match> <action> [value]
--api-version 1 --dry-run` return shared plans; omit `--dry-run` to apply. A shortcut
replacement needs both `--replace` and consequence acknowledgement with `--yes`;
`--yes` alone does not select replacement in API v1. Matching submaps/catch-all
bindings and unresolved keycodes are refused. For removal, take the current
`selector` from owned bindings or rules, then call `shortcut remove <selector>
--api-version 1` or `rule remove <selector> --api-version 1`. Never substitute a bare
index. Any owned-module change invalidates the selector. Critical shortcut
removal needs `--yes`.

Inspect each plan's verification scope. Shortcut adds verify the command in the
owned file and a matching live Lua binding, whose opaque reference does not expose
the command. Rules/removal verify owned entries and reload acceptance. Do not
claim command execution, effective app behavior or a globally unbound chord.
Failures attempt file rollback and a checked recovery reload; `partial` can retain
a concurrent edit or failed recovery. These writes have no automatic history undo.
Unversioned mutation calls retain their legacy output and selection conventions.

Use `plugin list` for the live registry, `bar status` for live/persisted layouts
and widget selectors, and `dock status` for ordered persisted pins. `plugin status`
collects all three. Inspect `complete` and source states; offline manifest hints
have unknown enabled state and are not a complete registry. `plugin enable|disable
<id> --api-version 1 --dry-run` plans a state change. A bar needs another bar as its
successor; it cannot simply be disabled. Plugin code may load/unload, and widget
enabled state means placement rather than component loadability.

Move the current `bar.widgets[].selector` with `widget move <selector> <section>
<index> --api-version 1 --dry-run`. Indices are measured after source removal.
Keep duplicate widget instances distinct; their options are preserved. Selectors
become stale when the bar changes. `dock pin <desktop-id>` and `dock unpin
<desktop-id>` support `--dry-run`; app IDs come from `apps list`, while missing
apps can still be unpinned. Dock success confirms the persisted preference only.

For a first-party Power widget, `bar status` reports the `showPercentage` option.
Use its current instance selector with `widget option <selector> showPercentage
<true|false> --api-version 1 --dry-run`, then omit `--dry-run` to apply. An omitted
option inherits false. Bare-ID entries, custom option types and replacement
plugins are refused. Readback covers the option and preserved bar configuration,
not visible battery text. Arbitrary third-party widget options have no schema.

These shell writers create backups and verify scoped readback, but cannot safely
attribute exact bytes written by the separate shell process. An unverified write
reports `partial` without automatic file restoration; inspect current state and
captured backups before retrying. There is no automatic history undo. Successful
registry/layout readback does not prove plugin health, rendering or app launch.
`plugin clone <built-in-id> --api-version 1 --dry-run` and `plugin remove
<user-id> --api-version 1 --dry-run` plan directory lifecycle changes. Execution
requires `--yes`. Use the returned `transaction.extra.recoveryToken` with `plugin
recover <token> --dry-run`, then `--yes` to restore captured files and reload the
shell. Recovery refuses later edits and preserves displaced content. Inspect
reported capture limits; large, hard-linked or special-file trees, linked clone
sources, and interrupted writers without post-write snapshots need manual review.

`describe --live` and `get` return current choices. A `cli-gap` has no callable
adapter yet, and opening a helper does not confirm a change. Static descriptions
label availability as `not_checked`; inspect the live state before changing it.
Read `error.code` and `outcome` even when a command exits successfully; only a
verified committed change confirms a write. Keep using the existing unversioned
commands below for operations not yet supported by API v1.

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

Add `--json` for machine-readable output. Commands exit 0 or 1. API v1 returns
errors in its JSON response on stdout; older commands can also diagnose errors on
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
