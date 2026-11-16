# CLI automation API

API v1 supports discovery, typed-setting reads/plans/changes, audio, Bluetooth,
scoped services, startup-app preferences, installed apps, MIME defaults, system
facts, local diagnostics export, shortcuts/window rules, plugin/bar/dock operations,
icon-pack discovery, durable display/network previews, search pins, restore
cancellation, reset/undo/history and per-user skill registration. The inventory
accounts for every GUI method. Selected launcher, icon maintenance, updater and
profile commands retain their documented unversioned interface. Source-built 0.5
packages also include [automatic user activation](AGENT-LIFECYCLE.md).

`skill status|plan|install|reconcile|remove` and `skill opt-out|opt-in` manage
filesystem registration with scoped plans, an ownership manifest and preserved
custom content. `--target <absolute-skills-directory>` selects custom locations;
install/reconcile/remove support `--dry-run`. Read the [registration contract and
support evidence](AGENT-SKILLS.md) before advertising harness or installation
readiness. Bare `skill install` keeps its human output; its JSON/version flags
select API v1. Other skill actions select API v1 implicitly.

## Discover the installed interface

```sh
omarchy-control commands --json --api-version 1
omarchy-control catalog --json --api-version 1
omarchy-control search "stop sleeping" --json --api-version 1
omarchy-control describe power.idle.lock --json --api-version 1
omarchy-control describe power.idle.lock --live --json --api-version 1
omarchy-control coverage --cli --json --api-version 1
```

`commands` lists the operations supported by API v1 and the reviewed GUI-operation
inventory. `catalog` returns every setting and workflow with its schema and
invocations. `search` returns at most 40 matching entries; quote the search query
as one argument. `describe` accepts a setting/workflow ID or an operation ID from
`commands` (for example, `audio.setVolume`). `commands` and `describe` also select
this API when the version option is omitted; their output is always JSON.

Static discovery does not execute desktop commands. It labels availability as
`not_checked`. Schema fields include choices, ranges, accepted toggle values,
requirements, effects, known files, confirmation rules, and recovery behavior.
The file list is explicitly incomplete: a backend can affect more files than
the static metadata identifies. Audio/Bluetooth and service targets come from
their status commands; startup IDs come from `startup list`, and app IDs from
`apps list`. Display targets come from `display status`, network UUIDs/interfaces
from `network saved|devices`, and plugin/bar/dock targets from their status commands.
Provider descriptions name the exact current selector source. Machine-specific
unavailability is separate from a missing CLI route.
Schema types are the catalog's control types (`choice`, `toggle`, `int`, and so
on), not JSON Schema type names. Pass the selected value as one CLI argument.

`describe --live` reads a typed setting, resolves its current choices, and checks
service availability. Read `current.status` as well as availability: a reachable
service does not prove that a value or its choices were successfully read.
Native-page descriptions expose `operationIds`; inspect those operations instead
of opening a window. External workflows expose the catalog destination, without
accepting arbitrary helper arguments or verifying the helper's effects.
Descriptions and coverage rows also expose `nativeSetting` and `nativeOperations`
where a helper has a native alternative. Inspect those IDs for parameterized
changes. External script destinations carry an `automation.status` and specific
`handoffReason`; `handoff` does not mean the upstream helper is inherently
interactive. See the [workflow audit](CLI-WORKFLOWS.md) for scope decisions.

`appearance.background.intros` is a boolean setting with an explicit enable/disable
helper. Its readback verifies the owned empty marker
`~/.local/state/omarchy/toggles/background-intros-off`, not animation playback.
Missing means enabled. Unchanged values skip the helper. Custom, linked, shared or
hard-linked paths are refused. Committed changes support guarded history undo;
unconfirmed external writes retain backups without automatic file restoration.

## Audio and Bluetooth

These new command families always return API v1 JSON, including when the version
flag is omitted. Descriptions list parameter grammar and target discovery sources.
`--dry-run` refreshes state and returns the shared provider's plan without changing
devices or writing a transaction.

```sh
omarchy-control audio status --json --api-version 1
omarchy-control audio output default <name> --dry-run
omarchy-control audio input default <name>
omarchy-control audio output volume <name> 55
omarchy-control audio input volume <name> 25
omarchy-control audio output mute <name> true
omarchy-control audio input mute <name> false
omarchy-control audio stream move <index@identity> <output-name>

omarchy-control bluetooth status --json --api-version 1
omarchy-control bluetooth power false --dry-run
omarchy-control bluetooth power false --yes
omarchy-control bluetooth connect <address>
omarchy-control bluetooth disconnect <address>
omarchy-control bluetooth forget <address> --dry-run
omarchy-control bluetooth forget <address> --yes
```

Audio names come from `outputs[].name` and `inputs[].name`; volume is a whole
percentage from 0 to 150, and mute accepts `true` or `false`. The current default
output's volume and mute follow the device returned by Omarchy's DSP resolver,
reported in the plan. The resolver can return the DSP sink itself when it cannot
follow a downstream link; Control preserves that upstream behavior. A failed
resolver call is refused rather than replaced with a guessed target. Reads
can report `outputRoutingKnown: false` while microphone controls remain usable.
Stream moves take `streams[].selector`, including its index and PipeWire identity.
A reused index is rejected across calls and checked again before writing. These
selectors belong to the current audio session; discover them again after a server
restart. Device identities are also checked before a write. The same providers
now journal and read back these changes from the GUI.

Default-device setters ask Omarchy to move active application streams too. Their
verification scope is explicitly `default-device-only`; they do not claim that
every app's stream moved. Individual moves verify that live stream's route.
Runtime audio operations have no automatic undo.

Bluetooth discovery reads every controller and known device. `powered` means at
least one controller is on; `allRadiosPowered` distinguishes a mixed state from
all controllers being on. Power changes verify every controller. A failed controller,
list, or device read is not treated as an off radio or absent device. Power-off
requires `--yes` because input/audio devices can be disconnected. Omarchy changes
all radios and remembers the rfkill preference across reboots. Connect can turn
Bluetooth on and trusts an already-paired device; forget can also turn it on,
and requires `--yes` because pairing is removed. These effects follow the reviewed
[power](https://github.com/omacom/omarchy/blob/v4.5.0/bin/omarchy-bluetooth-power)
and [device](https://github.com/omacom/omarchy/blob/v4.5.0/bin/omarchy-bluetooth-device)
commands. Neither operation promises automatic undo. Pairing stays with Omarchy's
panel: `bluetooth pair <address>` and connecting an unpaired device return
`interaction_required`, collecting no PIN or password.

Successful mutations return a provider plan and recorded transaction in `data`,
with `outcome: committed` and `verified: true`. Failed readback returns the actual
transaction phase, often `partial`: a backend may have changed state even though
verification failed. Inspect state/history instead of retrying blindly. Operations
that depend on Omarchy helpers honor compatibility fingerprints. A profile
restore's exclusive change lease also blocks these setters.

## Services and startup apps

These command families always return API v1 JSON. Mutations support `--dry-run`;
discovery runs no service actions and writes no startup preferences.

```sh
omarchy-control services user status
omarchy-control services system status
omarchy-control services user start <unit> --dry-run
omarchy-control services user start <unit>
omarchy-control services user restart <unit>
omarchy-control services user stop <unit>
omarchy-control services user enable <unit>
omarchy-control services user disable <unit>
omarchy-control services system stop <unit> --dry-run
omarchy-control services system stop <unit> --yes

omarchy-control startup list
omarchy-control startup disable <desktop-id> --dry-run
omarchy-control startup disable <desktop-id>
omarchy-control startup enable <desktop-id>
```

Service targets come from `services[].unit` in the chosen scope. A failed user
manager does not make system discovery fail, or vice versa. Both the units and
unit-file lists must be readable; failed or malformed output is not an empty list.
Installed static and masked services are listed too. Actions require a readable,
concrete `.service` unit; patterns, options and uninstantiated templates are refused.
The selected unit is read again before writing; changed identity/state is refused.

Start/stop/restart change the running service. Enable/disable change persistent
enablement without starting or stopping it. Enable/disable currently accepts only
normal `enabled`/`disabled` unit-file states; masked, static, linked and runtime-only
states need their owning tool. Readback verifies enablement, a settled active or
inactive state, or a newly completed successful one-shot. Restart and completed
one-shot verification require a newer process-start timestamp; old `Result=success`
or a service still `activating` cannot prove completion. The transaction records
the observed result, including `completed-oneshot`.

System stop/restart/disable requires `--yes` to acknowledge consequences. CLI
mutations pass `--no-ask-password`, so an authorization refusal returns a failed
transaction rather than collecting credentials. `--yes` grants no system privilege.
The GUI retains its desktop authorization flow. See the official
[systemctl authentication option](https://www.freedesktop.org/software/systemd/man/latest/systemctl.html#--no-ask-password)
and [service process timestamps](https://www.freedesktop.org/software/systemd/man/latest/org.freedesktop.systemd1.html).
Selected-unit verification does not prove the health of dependencies or all effects
of a service's configured hooks. Runtime service actions have no automatic undo.

Startup IDs come from `apps[].id`. Discovery reports the selected file, source,
preference state, and unevaluated `OnlyShowIn`, `NotShowIn`, `TryExec` and systemd
conditions. The highest-priority file replaces the entire lower-priority entry,
following [XDG autostart precedence](https://specifications.freedesktop.org/autostart/latest/#autostart-directories).
Absolute `XDG_CONFIG_HOME` and `XDG_CONFIG_DIRS` overrides are honored; defaults are
`~/.config/autostart` and `/etc/xdg/autostart`.

Startup changes set `Hidden` in the user entry and, when enabling, clear the GNOME
autostart disable flag. They preserve other desktop-entry groups, line endings,
comments and launch conditions. Package files stay unchanged. Unreadable or
ambiguous entries, linked user entries/directories, and user paths resolving to a
system startup directory are refused. Fingerprints guard edits made while a change
is queued; the coordinator backs up the user file for failure recovery. History
undo is not implemented for these operations.

Startup verification is explicitly `startup-preference-only`. A committed enable
does not start the app, override remaining launch conditions, or prove it will run
at the next login. Startup discovery and changes do not require systemd. Both
families respect the coordinator's journal, change lock and profile restore lease.

## Installed apps and MIME defaults

```sh
omarchy-control apps list
omarchy-control apps launch <desktop-id> --dry-run
omarchy-control apps launch <desktop-id>
omarchy-control mime list
omarchy-control mime get application/pdf
omarchy-control mime set application/pdf <desktop-id> --dry-run
omarchy-control mime set application/pdf <desktop-id>
```

These calls return API v1 JSON even without the version flag. Existing
`apps launcher ...` calls keep their legacy output and refuse API v1.
`apps list` reports IDs, names, declared MIME types, source files, fingerprints,
and availability. It follows XDG data-directory precedence and converts nested
paths to desktop IDs. Higher-priority hidden, invalid, or unreadable entries mask
lower copies; ambiguous IDs in the same root are unavailable. `NoDisplay` hides
an app from the launcher, but does not delete it or disqualify it as a handler.
These identity rules follow the
[desktop-entry specification](https://specifications.freedesktop.org/desktop-entry/latest-single/#desktop-file-id).

Launching validates the installed entry and delegates to `uwsm-app` or `gtk-launch`.
Its result is `outcome: launched`, `verified: false`: the launcher accepted a
request, which does not prove a window appeared or the app stayed running. A launch
is a handoff without a settings transaction or automatic undo. `--dry-run` launches
nothing.

`mime list` reads the seven everyday GUI types; `mime get/set` also accepts other
concrete MIME and `x-scheme-handler` types. Current handlers come from a bounded
`xdg-mime query default` call. Failed, truncated or malformed reads are unavailable;
empty successful output is `unresolved`, not proof of an unset default. A stale
handler ID remains visible with `currentInstalled: false`. Candidate lists describe
installed apps' declared `MimeType` support only. They do not enumerate every added
association or inherited type, so `candidatesComplete` is false.

A plan selects the first current-desktop-specific user file that declares this
default, otherwise `$XDG_CONFIG_HOME/mimeapps.list` (normally
`~/.config/mimeapps.list`). It reports the selected path and planned fallback list.
Selection follows the user portion of the
[MIME configuration lookup order](https://specifications.freedesktop.org/mime-apps/latest-single/#file-name-and-location).
Direct format-aware editing changes one `[Default Applications]` key, promoting
the chosen ID and preserving the other fallback IDs, comments, line endings and
groups. It does not invoke desktop-specific writer helpers or alter package files.
Linked selected files, user paths resolving to system configuration, ambiguous
defaults and unreadable inputs are refused.

Apply rechecks the effective handler, app identity and file fingerprints. The
coordinator journals the change, backs up the selected user file for failure
recovery, and respects the change lock and profile restore lease. A successful
fresh query must return the requested desktop ID; matching output from a failed
query cannot verify success. Failed readback attempts file rollback and reports
the actual transaction phase. No automatic history undo is advertised.
Verification covers the effective default ID, not the app's ability to open every
file of that type. Profiles retain their separate existing restore interface.

## System facts and local diagnostics

```sh
omarchy-control system status
omarchy-control system machine
omarchy-control system battery
omarchy-control system storage
omarchy-control system hooks
omarchy-control diagnostics export --dry-run
omarchy-control diagnostics export
```

System reads return completed shared-provider snapshots. `data.states` identifies
an `ok`, `partial`, or `unavailable` state, source and reason for each requested
section. Optional machine/session/version facts can remain unknown while other
fields are readable. `complete` and envelope `verified` are false for partial
reads. An unavailable requested section returns `read_unavailable` with the healthy
sections still present. Scoped reads do not require unrelated sections to succeed.

Battery data lists all detected battery supplies in `devices`; the legacy summary
uses the first supply, not an average. `presenceKnown` distinguishes a missing or
unreadable kernel source from a readable empty supply directory. Capacity outside
0–100 or unreadable capacity is `percent: -1`, `capacityKnown: false`; optional
cycle-count/status fields also carry known flags. These values follow the
[kernel power-supply interface](https://github.com/torvalds/linux/blob/master/Documentation/ABI/testing/sysfs-class-power).

Storage uses bounded [GNU df fields](https://www.gnu.org/software/coreutils/manual/html_node/df-invocation.html)
in bytes under the C locale. Unknown metrics are JSON null, not zero free space.
Failed/truncated commands are unavailable; unsupported rows or unknown sizes make
the section partial. The shared parser preserves mount-name spaces, negative
available space, shared filesystem markers and temporary AppImage mounts, and
excludes the existing pseudo-filesystem types. Storage is a mount-usage view, not
a full hardware inventory.

Hooks are metadata under `~/.config/omarchy/hooks`, with executable eligibility,
sample and symlink flags. Reading never runs a hook. Samples and non-executable
files remain visible in CLI inventory; the GUI keeps its active-candidate list.
Unreadable directories and untraversed linked directories cannot establish an
empty inventory. Eligibility does not prove that an event ran a script.

Diagnostics dry-run reports the owned export directory, privacy, bounds and effect
metadata without collecting or creating a report. Export refreshes capability
readings, then runs the reviewed Omarchy debug helper in non-sudo print mode
with a 30-second timeout and 1 MiB limits for each output stream. Stdout and stderr go into a local report with
available versions, capabilities and recent history. Nothing is uploaded.

The stock [Omarchy 4.5 helper](https://github.com/omacom/omarchy/blob/v4.5.0/bin/omarchy-debug)
writes a raw shared `/tmp/omarchy-debug.log` even in print mode. Control checks the
installed helper fingerprint and requires the reviewed log-destination binding.
It runs a temporary private copy with only that binding redirected to private
staging, caps child file sizes at 2 MiB, disables core dumps, and removes staging
after completion or cancellation. The installed helper and shared log stay
untouched. Changed or unrecognized helpers are refused by compatibility/isolation
guards; the reviewed helper fingerprint is recorded in the compatibility catalog.

The shared collector reserves a unique file under Control's XDG state
`omarchy-control/exports` directory. Directories are private (0700), reports are
0600, and linked/unowned export locations are refused. It flushes and reads back
the saved bytes and verifies the path, ownership, permissions and SHA-256 before
returning `outcome: exported`, `verified: true`. That verifies the local artifact,
not healthy desktop services. No settings transaction or history undo is created.
The GUI also waits for the export's own completion signal; unrelated messages
cannot complete it.

A failed, timed-out, empty or truncated debug collection can still save a useful
report. It exits 1 with `diagnostics_incomplete`, `outcome: partial`,
`reportSaved: true`, `complete: false`, and its path. A save/ownership failure does
not advertise a saved file. Duplicate collection on the same provider is refused,
and cancelled provider work removes its unfinished private file.

Common quoted/plain credentials, authorization values and private-key blocks are
redacted before saving. Redaction is explicitly `best-effort`: review a report
before sharing it. A successful export does not guarantee removal of every custom
secret format.

## Shortcuts and window rules

```sh
omarchy-control shortcut list
omarchy-control shortcut check "SUPER + SHIFT + F"
omarchy-control rule types
omarchy-control rule validate '^(firefox)$' workspace 3
omarchy-control rule validate 'org.gnome.Calculator' float
omarchy-control rule list --api-version 1
```

`shortcut list/check` and `rule types/validate` select API v1 implicitly. Read-only
operations reject mutation options. `rule list` needs the version flag to return
an envelope; its original unversioned JSON array is preserved. Use the version
flag on shortcut/rule mutations to select their structured interface:

```sh
omarchy-control shortcut add "SUPER + SHIFT + F" "Files" "nautilus" --api-version 1 --dry-run
omarchy-control shortcut add "SUPER + SHIFT + F" "Files" "nautilus" --api-version 1
omarchy-control shortcut add "SUPER + SPACE" "Launcher" "my-launcher" --api-version 1 --replace --dry-run
omarchy-control shortcut add "SUPER + SPACE" "Launcher" "my-launcher" --api-version 1 --replace --yes
omarchy-control shortcut remove '<selector>' --api-version 1 --dry-run
omarchy-control rule add '^(firefox)$' workspace 3 --api-version 1 --dry-run
omarchy-control rule add '^(firefox)$' workspace 3 --api-version 1
omarchy-control rule remove '<selector>' --api-version 1
```

A dry-run also selects API v1 without the version flag. Unversioned add/remove
calls retain their legacy output and argument conventions, using the shared
provider plans and guarded transaction implementation. For API v1, `--yes` alone
never selects replacement: request it with `--replace`, review its consequence,
then acknowledge with `--yes`. The legacy shortcut add `--yes` convention still
selects replacement. Rule writes have no `--yes` option.

Shortcut reads await `hyprctl -j binds`. `effective.binds` retains every reported
binding, including submaps, modifier masks, dispatcher/argument fields, keycodes,
catch-all bindings and flags. The field layout follows the reviewed
[Hyprland JSON output](https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/debug/HyprCtl.cpp#L946).
`owned` independently inspects the text of Control's module. A modified or
unreadable module does not erase usable compositor data. Inspect both source
states and `complete`; failed, timed-out, malformed or truncated reads return
`read_unavailable` with unavailable collections as null, not empty arrays.

Checks accept one named key with unique `SUPER`, `CTRL`, `ALT` or `SHIFT`
modifiers, separated by `+`. Case/spacing/order are normalized. Unknown or
repeated modifiers, empty tokens and control characters are rejected. Key names
are limited to 1–24 ASCII letters, digits or underscores; acceptance does not
prove the compositor recognizes a key name. `check.matches` includes matching
named/catch-all bindings from every submap, retaining flags and the submap name.
`conflicts` supplies readable labels even for bindings without descriptions.
`critical` marks the provider's three main launcher/terminal chords.

Read `check.collisionState`: `conflict` means a reported name or catch-all
matched; `unknown` means same-mask keycode/multi-key bindings remain unresolved;
`no_named_match` only means no match under that named-key comparison. Keyboard
layout and keysym aliases are not resolved. This is not proof that a chord is
globally free. The GUI uses the same checks and no longer labels an unresolved
comparison as free. Mutation plans refresh collision data. A reported collision
requires `--replace`; matching submaps/catch-all entries or unresolved keycodes
are refused. This is comparison of reported names/masks, not a complete keyboard
layout or hidden binding-option analysis. Shortcut descriptions are limited to
256 characters and commands to 8192, without control characters. Multi-digit
numeric keys are refused because Hyprland interprets them as keycodes.

`rule types` returns constraints from the same provider that validates GUI and
CLI input. Supported actions are `float`, `tile`, `noblur`, `workspace` and
`opacity`. The first three take no value; workspace takes a one-to-three-digit
number or `special[:name]`; opacity takes one or two space-separated values from
zero to one with up to three decimal places. Inspect `valuePattern`/examples
rather than guessing. Unknown actions, ignored extra values and trailing control
characters are rejected. Match strings are at most 120 characters before
trimming; Qt regular-expression syntax is checked on the trimmed string.

Validation reads no configuration and contacts no compositor. `validated` has
`verified: false` and `verificationScope: input-validation-only`; it does not
establish compositor regex acceptance, ownership, reload success or app behavior.
Versioned rule listing reads only owned rules, includes current zero-based
indices, and distinguishes absent, inaccessible and externally edited modules.
It never executes Lua or enumerates rules from other files. Module reads are
capped at 4 MiB. Both versioned lists add `selector` to owned entries, in the form
`index@owned-module-sha256`. Use the exact selector for versioned removal. Changes
to any part of the owned module invalidate it, including reordered or identical
rules and unrelated owned options. Legacy rule removal retains strict numeric
indices without a cross-call identity guard. Invalid text, overflow, negative
indices and excess positional arguments are refused. Re-read before removal.

Shared plans include actual files, entry changes, fingerprints, confirmation,
verification scope and failure recovery. Mutations refuse linked/unowned paths,
hard-linked files, externally edited modules and unrecognized import layouts.
They check the current Hyprland version through the existing compatibility
policy, read config errors as a complete JSON array, and recheck under the change
lock before backups/intent. Queued file/collision changes are refused before
writing. Dry-runs change no desktop configuration and create no transaction.

All four writes verify the persisted entries and active import, a successful
reload, and no additional config errors relative to their preflight baseline.
Shortcut adds additionally require a single matching live Lua binding with the
expected description and ordinary flags. Omarchy's
[`o.bind`](https://github.com/omacom/omarchy/blob/v4.5.0/default/hypr/helpers.lua#L130)
uses a Lua dispatcher; Hyprland exposes its
[opaque reference](https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/config/lua/bindings/LuaBindingsToplevel.cpp#L122)
as `dispatcher: __lua`. The command text is verified in the owned file, not in
that opaque live reference, and is never executed to test it. Plans report
`liveCommandKnown: false` and `owned-command-file-reload-and-live-lua-binding`.

Rule writes and shortcut removal report `owned-file-and-reload`: they verify
persistence and reload acceptance, not app matching, effective rule behavior,
command execution or a globally unbound chord. Removing an owned shortcut can
restore an inherited binding. Critical owned launcher/terminal removals require
`--yes`; the GUI also shows that consequence.

Failed verification restores captured files and checks a recovery reload. A
failed recovery or a concurrent edit that must be retained returns `partial`,
not success. The coordinator uses known written fingerprints so an edit during
reload is not attributed to Control and overwritten on failure. Automatic
history undo is unavailable; file backups and failure rollback are distinct
from a supported `undo` command.

## Shell plugins, bar instances and dock pins

```sh
omarchy-control plugin list
omarchy-control plugin status
omarchy-control bar status
omarchy-control dock status
omarchy-control plugin disable <id> --api-version 1 --dry-run
omarchy-control plugin enable <id> --api-version 1
omarchy-control widget move '<selector>' right 0 --api-version 1 --dry-run
omarchy-control widget move '<selector>' right 0 --api-version 1
omarchy-control dock pin <desktop-id> --dry-run
omarchy-control dock unpin <desktop-id>
```

`widget option <selector> showPercentage <true|false> [--dry-run]` sets the built-in
Power widget's battery percentage. Use a fresh `bar.widgets[].selector`; status
exposes the current value and whether it inherits the reviewed false default.
Only an enabled first-party `omarchy.power` object entry is supported. Bare IDs,
custom value types and replacement plugins are refused. An explicit JSON boolean
is written and the complete preserved bar configuration is verified against live
and persisted readback. Visible rendering remains unverified. Unconfirmed writes
retain backups; automatic rollback/history undo are unavailable.

Discovery and the new `dock` family select API v1 implicitly. Plugin/widget writes
use the version flag (or `--dry-run`); existing unversioned enable/disable and
positional widget moves retain their JSON output. Read-only commands reject
mutation options. These four writers have no `--yes` option.

`plugin list` queries only the running registry; `plugin status` collects the
registry, live bar configuration and persisted dock preferences. `bar status`
returns the registry and live/persisted bar sources. It uses the shell's
[`listShellConfig`](https://github.com/omacom/omarchy/blob/v4.5.0/shell/shell.qml#L1766)
query instead of reconstructing its state from disk. Inspect each source's
`status` and the top-level `complete`. A failed, malformed, duplicate-ID or
truncated registry read is unavailable with `items: null`; offline manifest hints
are bounded, unverified and have unknown enabled state/completeness. Reads never
evaluate plugin code. Reported IDs outside the writer's supported grammar remain
visible, while enable/disable refuses unsupported IDs.

The user configuration and Omarchy defaults are read independently, with their
paths and fingerprints. Files must be readable, bounded to 4 MiB, plain JSON
objects with version 1. Invalid nonempty user files are unavailable; they are not
silently replaced with defaults. A missing or zero-byte user file selects the
defaults. Dock pins fall back separately when the selected source has no pin
array. `dock status` needs no live shell: it reports the ordered persisted
preference, its source and `persisted-preference-only`. Pin IDs come from available
`apps list` entries; a `.desktop` suffix is optional and removed. Missing apps can
be unpinned. No visible dock or launched app is claimed.

`bar.widgets[]` includes each raw `entry`, retaining object options, plus its
section, index, ID and `selector`. Use the exact selector for movement; its format
is `section:index@live-bar-sha256`. Any change to the live bar configuration makes
it stale. Duplicate IDs remain distinct instances. The destination index is
measured **after removing the source instance**, from zero through the destination
length; out-of-range, fractional and overflowing values are refused rather than
coerced or clamped. The six sections are returned by discovery. The tray and its
clones cannot move into their own drawer; that check requires a readable registry.

Shared GUI/CLI plans check file ownership/links, configuration shape, current
helper fingerprints, target state and live/persisted agreement before changing
the bar or a plugin. They refuse layouts whose normalization could discard
unrelated choices. A locked preflight repeats the plan before backups/intent;
queued target/file changes and a profile restore lease prevent the write.
Dry-runs change no desktop configuration and create no transaction. The GUI
keeps moves busy while planning and rejects a changed displayed file snapshot.

Plugin writes verify the requested registry state and its persisted meaning,
with the shell and file agreeing. As the
[registry contract](https://github.com/omacom/omarchy/blob/v4.5.0/shell/shell.qml#L1728)
defines it, widget enabled state is placement, not component loadability. Selecting
a bar replaces the active bar; source/clone selection can also change. The helper
can load or unload plugin code. A disable request for a bar is refused because it
needs a successor. This verifies registry/configuration state, not plugin health
or visible rendering.

Moves verify the complete expected layout and preserved instance options in both
live and persisted state. Dock writes verify the exact ordered pin preference.
Readback allows bounded settling time; a zero exit alone never confirms these
changes. The reviewed
[bar](https://github.com/omacom/omarchy/blob/v4.5.0/bin/omarchy-bar) and
[dock](https://github.com/omacom/omarchy/blob/v4.5.0/bin/omarchy-dock) helpers retain
their own persistence/normalization behavior.

External shell writers do not expose exact written bytes. For these operations,
Control captures private file backups, but does **not** automatically restore the
whole file after an unverified write: that could overwrite a concurrent choice.
Such a write returns `partial`, `verified: false`, with the transaction and backup
paths. Inspect current state/backups before retrying. Automatic history undo is
unavailable. A rejected preflight starts no mutation.

Clone/removal have versioned, guarded directory capture and explicit recovery;
see [Plugin clone, removal and recovery](#plugin-clone-removal-and-recovery).

## Read, plan, apply

```sh
omarchy-control get power.idle.lock --json --api-version 1
omarchy-control plan power.idle.lock 600 --json --api-version 1
omarchy-control apply power.idle.lock 600 --json --api-version 1
```

The CLI uses the same settings engine, validation, compatibility checks, journal,
and verification as the window. `get` includes current choices and source data.
Targeted IDs such as `displays.brightness@DP-1` preserve the target throughout
reading and applying. `plan` changes no settings. `apply` checks again and returns
the recorded transaction after completion or failure.

When a plan reports an actual confirmation consequence, `apply` returns
`confirmation_required` until the authorized call includes `--yes`. That flag
does not bypass compatibility or managed-file ownership checks. An invalid value
or rejected plan never starts the change. Use `--` before a literal argument
beginning with a dash; put API options before that delimiter.

## Network discovery and scan requests

```sh
omarchy-control network status
omarchy-control network general
omarchy-control network devices
omarchy-control network saved
omarchy-control network wifi
```

These commands select API v1 implicitly and accept its optional version/JSON
flags. They perform reads only. `status` requests all four sections; the other
commands request just one. Dry-run and consequence flags are not supported.
Protected activation/disconnection and preview controls are described below;
their supported profile/device scope is narrower than the GUI compatibility path.

The response includes `requestedSections`, `states`, `complete`, `status`,
`startedAt` and a process-local refresh `generation`. Each requested source has
its own result, error, command and completion time. Failed or unrequested values
are null. A successfully read empty inventory is `[]`. One unavailable source
does not discard other sections; the overall request fails with
`read_unavailable`, `verified: false` and retained available data. Scoped reads
do not probe unrelated sources. All reads from an older refresh generation are
ignored; completion waits for every requested source.

- `general` exposes `wifiEnabled`, `connectivity` and `connectivityKnown`.
  Wi-Fi is NetworkManager's reported switch preference, not proof of usable
  hardware. `connectivity: unknown` remains explicit and is not a failed parse.
- `devices` includes every reported type, including loopback, bridge and future
  types. Rows retain interface names and raw state/connection text. `connected`
  is null when the device state is unknown/unrecognized; `stateKnown` records
  that distinction. `external` reflects NetworkManager's external-state label,
  not a complete ownership/management test. `shownInGui` records the window's
  narrower device filter.
- `saved` includes all profiles visible to the caller, with name, UUID, type and
  active flag. Different UUIDs can share a name. This is not a secret/configuration
  export or a promise that every system profile is visible to an ordinary user.
- `wifi` exposes `accessPoints`, with interface, BSSID, display SSID, signal,
  security and in-use state. Separate APs and interfaces are retained even when
  names/BSSIDs repeat. An empty display SSID remains a record (`ssidEmpty: true`).
  The GUI keeps its existing active/strongest-per-name grouping separately.

The client reads cached APs with `--rescan no`, does not request a connectivity
check and never asks for secrets. These semantics follow the
[nmcli manual](https://networkmanager.dev/docs/api/latest/nmcli.html). A complete
read confirms only NetworkManager-reported state; it does not prove internet
reachability or a recent scan. `scanRequested`, `connectivityCheckRequested` and
`accessPointAgeKnown` make those limits explicit. `atomic: false` means the
independent source reads can observe different moments during a transition.
These are not durable preview IDs or confirmation tokens.

Reads use English messages in a UTF-8 C locale, forced plain output and escaped
terse fields. A plain `C` locale can replace accented names with `?`; the actual
nmcli integration test verifies Unicode, colons and backslashes. Display SSIDs
are not a lossless representation of arbitrary 802.11 bytes. Malformed escapes,
field counts, identities, booleans/signals, duplicate identities, invalid UTF-8,
failed commands, timeouts and truncated output make that source unavailable.
Each command is limited to four seconds, 1 MiB per output stream and 4,096 rows.
Empty separators between multiple interfaces' AP tables are ordinary.

### Explicit Wi-Fi scan requests

```sh
omarchy-control network devices
omarchy-control network rescan wlan0 --dry-run
omarchy-control network rescan wlan0
omarchy-control network wifi
```

`network rescan [interface] [--dry-run]` selects API v1 implicitly. Omit the
interface to select all eligible Wi-Fi adapters, up to 16, in sorted name order.
The plan reads current radio/device state and lists selected and excluded Wi-Fi
interfaces. Radio-off, unavailable, unmanaged, external and unknown device states
are refused or excluded. The selected interface must still exist and be Wi-Fi.
Names are current NetworkManager interface identities, not durable hardware
tokens; they cannot start with a dash, contain whitespace/control characters,
slashes/colons, or exceed 15 UTF-8 bytes. Dry-run sends no scan and writes no
activity receipt. It does not reserve an adapter for a later call.

Execution uses the shared GUI/provider path. It holds the global change lock,
honors profile-restore leases, refreshes radio/device preconditions under the
lock, and resolves every selected NetworkManager object path before sending any
request. Resolution has a ten-second overall deadline (four seconds per lookup,
64 KiB output limit). It calls the wireless D-Bus `RequestScan` method through
`busctl --allow-interactive-authorization=no`; no credential prompt is requested.
The request stage has a twenty-second overall deadline, a ten-second D-Bus
timeout and a fifteen-second process limit per adapter, with 64 KiB output limits.
Authorization denial and driver throttling remain possible. These are bounded
stages, not a promise of scan duration. No connection profiles or radio
preferences are changed, no secrets are requested, and no connection is activated.

Success returns `outcome: scan_requested`, `requestAccepted: true` and
`verified: false`. Only a successful void D-Bus reply establishes request
acceptance. `scanCompletionVerified` and `accessPointAgeKnown` remain false.
`results` records each interface's attempt and acceptance. A failed, truncated,
unexpected or timed-out reply leaves attempted acceptance null. Mixed confirmed
and unconfirmed results produce `partial`; no confirmed reply after an attempt
produces `unknown`. A refusal before sending requests is `blocked`, with
`requestAttempted: false`. Unsent deadline-exhausted targets are explicitly
recorded. There is no automatic retry or rollback of a scan request.

The coordinator's non-undoable `action` history entry is an activity receipt with
`verificationScope: scan-request-acceptance-only`. Its `committed` phase records
accepted requests only; it does not establish a persistent settings change,
completed scan or fresh AP cache. Inspect per-interface evidence before deciding
whether to retry an unconfirmed request. A later `network wifi` still reads the
cache without scanning and cannot prove that this particular request completed.

### Global Wi-Fi switch

```sh
omarchy-control network radio true --dry-run
omarchy-control network radio true
omarchy-control network radio false --dry-run
omarchy-control network radio false --yes
```

`network radio true|false [--dry-run] [--yes]` controls NetworkManager's global
Wi-Fi switch. Boolean values must be exactly `true` or `false`. This is separate
from `network wifi`, which reads cached access points. A plan reads the current
Wi-Fi adapter inventory and the independent `WirelessEnabled` and
`WirelessHardwareEnabled` D-Bus properties. Their meanings are defined by
[NetworkManager's property API](https://networkmanager.dev/docs/api/latest/gdbus-org.freedesktop.NetworkManager.html).
No Wi-Fi adapter, unavailable tools, or failed/malformed/truncated reads block
the operation. Hardware-blocked enabling reports `hardware_blocked`; disabling
is still allowed with a readable switch. The adapter list describes the global
scope, including externally managed/unmanaged adapters; it does not restrict the
switch to a single managed connection.

Turning Wi-Fi off always requires `--yes`, including when it already reports
off. It can disconnect Wi-Fi connections and end this remote session. There is
no checkpoint, automatic rollback or history undo. Turning it on may cause
NetworkManager to reconnect saved profiles according to their preferences.
No connection profile, credentials, other radio type or overall networking
switch is written. NetworkManager owns persistence; reboot behavior is not
verified. A dry run performs reads only and writes no journal entry.

Execution shares the GUI provider. Under the global change lock and restore
lease guard, it re-reads the switch, hardware state and sorted Wi-Fi interface
names. A changed switch, hardware flag or adapter-name inventory refuses the
reviewed plan before writing. Current names are not durable hardware identities;
these independent reads are not an atomic hotplug snapshot. Pending scan
requests and connection previews block radio changes in the same provider.
The write uses an explicit D-Bus `Properties.Set` call through
[`busctl --allow-interactive-authorization=no`](https://www.freedesktop.org/software/systemd/man/latest/busctl.html),
with a ten-second D-Bus timeout, fifteen-second process limit and 64 KiB stream
limits. Each read has a four-second process limit; property reads have 64 KiB
stream limits and inventory reads retain the limits described above. `--yes`
acknowledges consequences and grants no administrative authority.

Only a successful void writer reply followed by a valid matching switch
readback returns `committed` and `verified: true`, with
`verificationScope: NetworkManager-radio-switch-only`. `readback` includes
`enabled` and `hardwareEnabled` separately. `connectionVerified` and
`internetReachabilityVerified` remain false, including after enabling. A failed
writer, timeout, unexpected output, unchanged switch or unavailable readback
returns an unverified `partial` transaction. A failed readback keeps its values
null; if readback was not attempted, `readback` itself is null. Inspect current
state before retrying; no write is retried or automatically reversed. An
execution refusal before the writer produces a failed transaction without a
radio write.

[`network-discovery-check.py`](../tools/network-discovery-check.py) exercises the
production CLI with actual nmcli against a private unmanaged NetworkManager and
private Wi-Fi D-Bus fixtures. In an explicitly enabled disposable Docker lab,
install `network-manager`, `python3-dbus` and `python3-dbusmock`, then run:

```sh
OC_NETWORK_DISCOVERY_LAB=1 python3 tools/network-discovery-check.py \
  --control /absolute/path/to/omarchy-control \
  --output build/network-cli-tests/integration.json
```

The tool refuses a normal host invocation. It creates fresh private D-Bus
sessions, keeps real daemon interfaces unmanaged, disables connectivity probes,
and creates then deletes an inactive unsaved fixture profile. The Wi-Fi case uses the
actual nmcli binary against D-Bus fixtures and checks for scan, connectivity-check
and secret requests. Add `--include-scan-requests` to also test real `busctl`
request acceptance, per-interface selection and denied/partial results against
the private Wi-Fi mocks only. This does not verify real Wi-Fi hardware or an Omarchy
desktop session. The CLI itself has no new Python/D-Bus-mock dependency.
Add `--include-radio-changes` to test switch writes/readback, hardware refusal,
consequence acknowledgement, denied writes and no-op writers. Those changes
affect only the private Wi-Fi mocks; the real unmanaged daemon is never used for
radio writes. Both optional modes may be combined.

### Durable connection previews

```sh
omarchy-control network activate <uuid> <interface> --dry-run
omarchy-control network activate <uuid> <interface> --yes
omarchy-control network disconnect <interface> --dry-run
omarchy-control network disconnect <interface> --yes
omarchy-control network preview status [token]
omarchy-control network preview keep <token> --yes
omarchy-control network preview revert <token>
```

Activation currently supports ordinary managed Ethernet and open Wi-Fi profiles;
secured/802.1X activation returns a concrete network-panel handoff. Starts require
`--yes` and a mandatory 60-second checkpoint. They return `outcome: preview`,
`verified: false` and `data.receipt.token`. Keep only after the user confirms
usability, using a separate call; daemon readback does not supply that confirmation.
Missing checkpoints and changed daemons remain unknown outcomes. See
[network preview lifecycle](NETWORK-PREVIEWS.md) for ownership, expiry, failure
handling, state paths, supported scope and verification limits.

## Icon-pack discovery

```sh
omarchy-control icons list --api-version 1
omarchy-control icons samples <pack-id>
omarchy-control icons search <pack-id> [query] --limit 30
```

`samples` and `search` select API v1 implicitly. `icons list` needs the version
flag for the new envelope; its existing human/JSON output remains unchanged.
These reads perform no desktop commands, rendering, downloads or settings writes.
Use `appearance.icons` through typed `get`/`plan`/`apply` to choose a pack. Existing
icon and launcher mutations retain their unversioned interfaces.

`list` returns `packs`, ordered XDG `searchRoots`, `complete` and `diagnostics`.
Each pack exposes its ID, name, comment, inherited IDs and locations; one pack can
span several roots, with the first descriptor taking precedence. Hidden packs and
cursor-only themes are omitted from this list. Explicit reads can inspect a hidden
pack when its ID is already known. A malformed or unreadable higher-priority
descriptor does not silently select a lower-priority descriptor.

`samples` returns four keys: folder, document, terminal and browser. A found row
includes `path`, `fileUrl`, originating pack `from` and `inherited`. `url` remains
the GUI's image-provider URL; CLI consumers should use `path`/`fileUrl`. A missing
sample has `found: false`. Inheritance and hicolor fallback are explicit.

`search` accepts an optional case-insensitive substring of an icon name, limited
to 512 characters without controls. It uses the same picker lookup as the GUI;
the CLI selects an explicit pack instead of the GUI's current choice. Only
application/device directories and SVG/PNG names are included. Nearest-size
directories, lookup-root order and SVG-before-PNG order within each directory
match the existing layer lookup. The result includes `icons`, `matches`, `limit`
and `truncated`. Rows expose `name`, `path`, file `url`, `from` and `inherited`.
`--limit` accepts an integer from 1 to 300 and defaults to 300. It is supported
only by `icons search`. Use `--` before a literal query beginning with a dash,
after any options. Ordering is case-insensitive with a case-sensitive tie break.

`complete` concerns bounded filesystem discovery. A result can be complete and
truncated because `--limit` restricts returned rows. Failed/inaccessible sources
and resource limits return `read_unavailable`, `verified: false`, `complete: false`
and available rows with diagnostics. An unknown or unreadable requested pack is
refused before substituting hicolor. An empty complete search is a valid no-match
result. A complete sample lookup can still contain absent sample files.

Descriptors are limited to 1 MiB and 5,000 directory declarations. Pack listing
reads at most 10,000 root entries and inspects at most 256 candidate themes;
inheritance follows at most 256 identities and depth 12. Search visits at most
100,000 directory visits/entries combined, and each sample lookup checks at most 100,000 candidate
files. Diagnostics are deduplicated and capped at 100 returned messages. Missing
optional roots and absent hicolor are ordinary; broken links, permission failures
and declared missing parents are reported. These are bounded snapshots, not an
atomic filesystem view. `verified: true` confirms the scoped file lookup only;
image decoding and launcher/desktop rendering are not tested.

## Response contract

Every API v1 invocation returns one JSON object on stdout and exits 0 or 1:

```json
{
  "apiVersion": 1,
  "appVersion": "0.5.0",
  "operation": "plan",
  "ok": true,
  "outcome": "planned",
  "verified": false,
  "data": {},
  "error": null
}
```

The example omits the plan details inside `data`. A failure has `ok: false` and
`error: {"code": "...", "message": "..."}`; its `data` may contain the rejected
plan, failed read, or transaction. Unknown commands, unsupported versions, invalid
options, and missing/extra arguments also return this envelope before probing or
changing the desktop. API mode is JSON even without the optional `--json` flag.

`completed` is a discovery result; `read` is a value read; `planned` is a validated
proposal; `launched` is an unverified app handoff. Only `committed` with
`verified: true` confirms a settings write. `exported` with `verified: true`
confirms a saved diagnostics artifact. A custom value
can be read successfully with `verified: false`. A failed write returns its actual
transaction phase and `change_not_confirmed`; inspect the transaction rather
than assuming that a failure or timeout means nothing changed.

Error codes include `invalid_arguments`, `unsupported_api_version`,
`unsupported_api_operation`, `unknown_command`, `unknown_id`, `not_a_setting`,
`read_only`, `invalid_value`, `unavailable`, `choice_unavailable`,
`choices_unavailable`, `read_unavailable`, `confirmation_required`,
`compatibility_blocked`, `ownership_conflict`, `unsupported_configuration`,
`configuration_conflict`, `stale_target`, `interaction_required`, `read_timeout`,
`operation_timeout`, `launch_failed`, `write_unavailable`, `diagnostics_incomplete`,
and `change_not_confirmed`.

## Compatibility and remaining work

`history --api-version 1` reads journal transactions. `reset <setting-id>
--api-version 1` and `undo <transaction-id> --api-version 1` use the shared
engine and return the same v1 outcome envelope as apply. They preserve the
existing unversioned interfaces. Inspect `describe` and current state before a
reset; it restores an inherited default rather than necessarily the previous
value. Consequential resets/undo require `--yes`. These commands have no
`--dry-run`. Unknown, already-undone or nonundoable transactions are refused;
later file/value edits remain guarded. Only a committed readback is verified.

Existing calls without `--api-version` keep their prior output, including the
array returned by `catalog --json`. Launcher/icon maintenance, updater, most profile
commands and helper launches retain their unversioned interfaces. Versioned calls
to those commands refuse with `unsupported_api_operation`; continue using their
documented unversioned interface where appropriate. Do not bypass a failed
typed-setting write by switching to direct file edits.

`coverage --cli` inventories all catalog destinations and separately accounts for
GUI-callable methods. It reports `complete: false`. `legacy-cli` records an
existing command but does not claim API/result parity; `cli-gap` records a missing
adapter; `api-cli` records an implemented API operation; `ui-only` records an excluded visual interaction with a reason.
Catalog classifications distinguish `typed-setting`, `native-page`,
`advanced-config`, `documentation`, `interactive-handoff` and `external-workflow`.
An external workflow can itself accept parameters; Control exposes its catalog
destination, not a versioned adapter to every upstream administration tool.
`inventoryComplete` describes accounting only. `complete` remains false because
physical desktop and harness/model readiness require separate evidence.

Run `python3 -I tools/cli-coverage.py --json` to check that every GUI-callable method
has exactly one reviewed accounting entry. CI fails on a new unclassified method,
a stale mapping, or an operation that advertises a command despite being a gap.
Runtime capabilities and device-specific behavior require separate tests.

### Display layouts, search pins and restore cancellation

`display status`, `display layout <json-file> [--dry-run] [--yes]`, `display check
[--dry-run] [--yes]`, and `display preview status|keep|revert` use API v1. Full-layout
input, session-bound tokens, the user-confirmed Keep boundary and interrupted-file
recovery are documented in [Display previews](DISPLAY-PREVIEWS.md). A preview is a
runtime change under a timer; `kept` records persistence. Physical usability is
never inferred from compositor geometry. The old single-output preview stays
unversioned.

`preferences pins` returns ordered Control search-pin IDs, including stale IDs
marked unknown. `preferences pin|unpin <catalog-id> [--dry-run]` uses a shared,
locked GUI/CLI writer, preserves unrelated preference keys and makes retries
idempotent. New pins need a known catalog ID; stale IDs can be unpinned. Malformed,
linked or unowned preference files are left alone. These preferences have no
history undo and do not change desktop settings.

`preferences crash-mutes` lists bounded per-program crash-notification markers.
`preferences crash-mute <program>` reads one; an optional `true|false` sets it and
supports `--dry-run`. The program is an executable basename matching
`[A-Za-z0-9._-]{1,255}`, excluding `.` and `..`, rather than a path. Missing means
unmuted. The explicit helper receives `-- <program> on|off`, avoiding toggle
retries. Reads/plans do not create markers. Linked, hard-linked, shared or custom
nonempty paths are refused; discovery marks custom entries without guessing their
value. Changes use coordinator locking, helper compatibility checks and readback
of `~/.local/state/omarchy/toggles/crash-ignore/<program>`. They confirm only that
preference, not watcher behavior or crash capture. There is no automatic history
undo or failure rollback; an unconfirmed helper retains captured backups.

`profile cancel <run-id> [--dry-run]` records a private cooperative cancellation
request. `outcome: cancellation_requested`, `verified: false` confirms acceptance
only. The worker completes and verifies its current operation, then stops before
the next. Read `profile status --json` for `cancelled`, completion or another final
state. Resume explicitly clears an old cancellation request. The remaining profile
commands retain their documented unversioned interface.

### Plugin clone, removal and recovery

`plugin clone <built-in-id> --api-version 1` and `plugin remove <user-id>
--api-version 1` support `--dry-run`; execution requires `--yes`. The GUI uses the
same plans. Clone binds the helper's username namespace and reviewed source tree,
then verifies the clone manifest, enabled registry state and persisted/live shell
configuration. Removal passes the helper's noninteractive `--yes` after Control's
confirmation. It verifies target absence, registry absence and applicable source
restoration. Code can load/unload; these checks do not prove rendered plugin health.

A private recovery capture retains the target's directories, file bytes, permissions,
Git metadata, opaque symlinks and original shell configuration. Captures do not
follow links. Trees over 64 MiB, 4096 entries or 32 levels, hard-linked files and
special files need manual review. Clone sources with links are refused before
Omarchy could dereference them. Omarchy's own hidden plain-directory backups remain.

Transactions return `extra.recoveryToken`. Use `plugin recover <token> --dry-run`,
then `plugin recover <token> --yes` to restore captured files and reload/verify the
shell. Recovery checks post-write fingerprints, refuses later edits, preserves
any displaced content in a hidden sibling and can resume guarded partial recovery.
Private backup corruption is refused. A writer interrupted before recording its
post-write state requires manual review of the retained capture. No automatic
history undo or unconditional file rollback is promised. Recoverable partial shell
configuration may be invalid JSON or disagree with live state; byte guards, rather
than an assumption of source/live agreement, govern recovery.
