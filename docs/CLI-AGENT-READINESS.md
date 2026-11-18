# CLI and agent integration readiness

The 0.6.0 source implements the reviewed CLI and skill integration.
The helper audit added explicit background-introduction, Power widget percentage
and per-program crash-mute operations, with the passing evidence below. Physical
Omarchy desktop acceptance for the CLI and skill integration remains open.

## Supported contract

The independent inventory accounts for 119 GUI methods in 92 groups: 60 API v1
groups, 24 documented legacy CLI groups, eight UI-only groups and no native CLI
gaps. The catalog has 91 typed entries and 425 destinations. New/unclassified GUI
methods, unknown catalog backends, duplicate IDs and stale operation mappings
fail checks.

API v1 exposes setting schemas, current choices, provider selectors, plans,
structured outcomes and scoped verification. It includes reset, undo/history,
durable display/network previews, plugin lifecycle recovery, pins and restore
cancellation. Native-page descriptions identify their provider operations. Helper descriptions
point to native alternatives and state specific handoff reasons; see the
[workflow audit](CLI-WORKFLOWS.md).
Launcher/icon maintenance, updates and most profile operations retain their
existing unversioned interfaces; their descriptions state effects, recovery and
output limits. See the [CLI contract](CLI-API.md).

Catalog destinations have explicit scope: 30 native pages, 21 config editors,
13 documentation links, 17 app/panel handoffs and 344 external workflows.
`launch <id>` opens the catalog's static invocation. It does not accept arbitrary
upstream helper parameters or establish that a helper completed its changes.
Eleven script destinations advertise native alternatives; 333 retain explicit
content, productivity, lifecycle, administration or interaction boundaries. Some
upstream helpers accept parameters; their static launch is not a native
read/plan/change/readback adapter. Pairing, secrets and administration that needs
an interactive terminal retain a concrete handoff. No refusal authorizes a raw
file bypass.

## Verified evidence

Current 0.6.0 evidence and checksummed packages are under
`build/cli-release-0.6.0/`. Earlier helper, systemd and harness artifacts remain
under `build/cli-workflows/` and `build/cli-completion/`. These are generated local
artifacts, not committed test results. Published 0.5.0 predates this integration.
The current source snapshot is recorded in
`build/cli-release-0.6.0/verification.json`.

| Check | Result | Practical scope |
| --- | --- | --- |
| Full Qt suite, Arch ARM / Qt 6.12 | 966 passed, zero failed | Clean 0.6.0 build; includes the helper audit, updater and GUI/provider fault cases in offscreen fixtures |
| Tagged 0.6.0 release, Arch x86_64 | 977 Qt tests passed; package and installer checks passed | GitHub release workflow; downloaded package and SHA256SUMS independently verified after publication |
| Final search-pin error follow-up | Four tests passed | Explicit blocked outcome for invalid preferences; malformed content remains untouched |
| Real Omarchy helper integration | 39 checks passed per architecture | Actual x86_64 and ARM package binaries, eight fingerprint-matched helpers and simulated shell IPC; no rendering/playback claim |
| Baseline full Qt suite, Arch x86_64 / Qt 6.12 | 938 passed, zero failed | Before the helper follow-up; includes GUI regressions and provider/CLI fault cases in offscreen fixtures |
| Final skill-resource follow-up | Passed | Bundled export, stock forwarding, ownership, opt-out and development relinks after the guidance correction |
| Python lifecycle/accounting | 36 + 5 tests passed | Ownership, pending intent, package payload, context migration and inventory drift |
| Invoking user without a manager | Five checks passed | Actual helper/production CLI registration after durable next-session cleanup preparation; manager activation reported deferred |
| Production display processes | 34 checks passed | Actual CLI and detached watchdog against a stateful compositor fixture |
| Production NetworkManager client | 92 checks passed | Actual Qt D-Bus client on private mocks, including full expiry and process death |
| Native systemd user sessions | 41 checks passed on Arch ARM; 41 on Debian 13 | Real user managers, deferred cleanup and guarded fault cases |
| Production 0.6.0 Arch recipe | 38 checks, six real pacman transactions passed | Declared dependencies, active/inactive users, conflicts, opt-out, graphical target ordering and late environment import |
| Published 0.5.0 to 0.6.0 upgrade | 14 checks passed on x86_64 | Checksum-verified published package installs/runs, upgrades and preserves preferences; activation hooks masked in the disposable root |
| Source-built 0.6.0 x86_64 package and curl installer | Nine checks passed | Checksummed local release mirror, repeat/pinned installs, tampering, truncated downloads and invoking-user activation outcomes |
| Translation consistency and skill frontmatter | Passed | New untranslated messages retain English fallback |

The production 0.6.0 recipe report is
`build/cli-release-0.6.0/package-final/report.json`.
It exercises install, upgrade, same-API downgrade,
reinstall and removal. A separate fixture-package run passed 36 checks.

[Release 0.6.0](https://github.com/last-refuge/omarchy-control/releases/tag/v0.6.0)
publishes the x86_64 Arch package and SHA256SUMS. The bundled external Omarchy
recipe pins the actual 0.6.0 tag tarball and its verified SHA256;
`makepkg --verifysource` passed.

## Agent discovery and task execution

Fresh Codex 0.167.1, Pi 0.64.0 and Claude Code 2.1.322 processes passed 16 discovery
scenarios on both macOS and disposable Arch ARM. These cover defaults, generic
roots, custom homes, custom-skill conflicts and multiple roots; Codex additionally
covers directory aliases. Claude did not discover the generic `.agents/skills`
root and needs its own skill directory. See [supported paths](AGENT-SKILLS.md).

Baseline Codex model turns, with only the stock-named `omarchy` skill enabled,
passed five tasks without supplied setting IDs or CLI examples:

1. Set automatic locking to ten minutes, then read it back.
2. Pin that setting and verify the preference.
3. Discover the latest lock-timeout transaction and undo it through API v1.
4. Respect a hand-edited module refusal and report recovery without bypassing it.
5. Discover a display, pass its recovery check, preview a move and verify timed
   restoration without Keep or a physical-usability claim.

The evaluator used a disposable container, synthetic desktop services and a
read-only existing authentication mount. No real desktop settings were changed.
Its default harness model was used without a model override. The container supplied
the execution sandbox because nested user namespaces were unavailable. The
credential mount was removed with that container after testing. Traces and grades
are in `model-eval/`; failed fixture/setup attempts remain distinguishable from
the passing cases. The execution traces also prompted a guidance correction:
provider descriptions use static `describe`, then their advertised discovery and
dry-run commands; `describe --live` is for typed settings.

These model turns predate the helper follow-up; the updated skill text passed
frontmatter, export/registration and canonical package checks, with no additional
model-task claim. Pi and Claude evidence establishes metadata discovery, not model task execution.
Hermes/profile and Antigravity roots have filesystem support but no harness-loading
claim. Registration cannot refresh instructions already cached in a conversation.

## Remaining acceptance gates

- Run the new integration on a disposable Omarchy 4.5 / Hyprland 0.56 desktop:
  install as a logged-in user, start a fresh supported harness, update/relink
  upstream skills, and verify removal/reinstall with actual session environment.
- Verify display recovery and usability on physical outputs, including timeout,
  originating-process death and the human-confirmed Keep path. Fixtures establish
  reported-state recovery, not a usable picture, hotplug behavior or driver health.
- Verify NetworkManager authorization and driver rollback on actual Ethernet/open
  Wi-Fi. Secured activation and Bluetooth pairing retain the secret-owning panel
  handoff. GUI-started legacy network connections retain their separate checkpoint
  compatibility path; GUI adoption of CLI tokens uses the durable CLI protocol.
- Complete the pre-existing fresh-machine My Omarchy restore acceptance in
  [its checklist](MY-OMARCHY.md), and expand account-dependent model tasks if those
  harnesses are to be advertised beyond metadata discovery.

The attempted software-only desktop here reached Weston but could not start
Hyprland: its client requested `wl_compositor` version 6 while that parent accepted
at most 5. Logs are in `archarm/desktop-probe*.log`. This does not close the desktop
gate. Deferred cleanup also depends on its retained runtime's Python/Qt libraries;
missing dependencies preserve fallback guidance for review. Unprepared legacy
manual registrations require explicit cleanup or reinstall, as documented in
the [lifecycle contract](AGENT-LIFECYCLE.md).
