# Comprehensive CLI and Omarchy agent integration

Status: implementation started November 13, 2026. Planned November 12 against Control 0.4.0; implementation is based on Control 0.5.0 and Omarchy 4.5.0. The current source includes the CLI and packaged skill-integration implementation. See the [current readiness report](CLI-AGENT-READINESS.md) for final evidence and remaining desktop/publication gates. The implementation slices below retain the history of earlier gaps; they are not the current status.

## 1. Intended result

After Control is installed and its user integration is active, a newly opened, supported agent that loads the `omarchy` skill should discover Control, inspect what this machine supports, and use its CLI to complete the user's requested settings change. The agent should not need to know the setting ID, configuration path, device ID, or command arguments beforehand.

Every setting and operation supported by Control's window must have a discoverable CLI equivalent or an explicit explanation of the remaining interactive step. Every catalogued Omarchy workflow must have a discoverable destination. These are separate coverage claims: opening a panel or editor is not an automated settings change.

Audit the workflows themselves as well as GUI parity. A route labelled “wizard” today may wrap a command that can safely accept structured parameters. Routine changes with a supported noninteractive backend should become shared CLI operations even if the window currently launches a helper. Classify genuinely interactive requirements separately from missing implementation; a helper button must not conceal an automation gap.

“Any change” means complete coverage within Control's reviewed scope. Arbitrary third-party preferences, unsupported hardware, custom Lua programs, and authentication that requires a person remain explicit boundaries. The CLI must explain those boundaries and identify a useful next step.

An agent already running may cache skill instructions. Report whether a refresh or a new session is needed; do not promise that changing skill files updates an existing conversation.

## 2. Current implementation and confirmed gaps

The following source-audit baseline predates the implementation slices below. Counts come from the current JSON catalogs, not the older totals in the product roadmap. Runtime evidence is recorded separately.

- `src/data/settings.json` has 89 setting/status entries, including 76 with writers. `src/data/workflows.json` now has 425 workflow entries, including the My Omarchy profile page added in 0.5.0. Provider operations need separate accounting because a native page can contain many actions.
- `src/main.cpp` already runs the CLI through `QCoreApplication`, without starting the window. Shared typed settings use `AppCore`, `SettingsEngine`, and `ChangeCoordinator`.
- The CLI already has `catalog`, `search`, `get`, `plan`, `apply`, `reset`, `history`, `undo`, `doctor`, coverage/compatibility tools, and selected provider commands.
- Unversioned `catalog --json` omits much of the existing metadata: value limits, choices, ownership, side effects, recovery, and exact callable operations. Unversioned `get` resolves choices for display but does not generally return the choice list. The new opt-in API addresses discovery and typed-setting access below.
- Provider access was incomplete. Audio, Bluetooth, services, startup, installed-app discovery/launch and MIME defaults now have dedicated handlers as recorded below. Network state discovery is implemented; its mutations still need adapters. Other provider families expose only a subset of their GUI behavior.
- JSON responses have several shapes. Some failures still emit only human text even with `--json`. Several provider handlers report a message rather than a structured transaction/result. Argument parsing also needs strict arity, option, and numeric validation.
- Display preview currently moves one output's position. `--keep` keeps it immediately when the preview starts; this is not a user-confirmed preview across separate agent calls.
- The original `skill install` wrote only `~/.claude/skills/omarchy-control/SKILL.md`. Explicit guarded registration and stock forwarding are now implemented below; harness loading and automatic activation remain open.
- The existing package checks explicitly expect no install scriptlet and run package operations with `--noscriptlet`. They do not prove automatic per-user skill activation.

### First implementation slice: delivered and remaining

Implemented on `feat/cli-agent-discovery`:

- Opt-in API v1 for commands/catalog/search/describe, operation coverage, and typed-setting get/plan/apply. Existing unversioned output is preserved.
- Static descriptions with types, limits, static choices, requirements, known file locations, effects, confirmation, and recovery information; live descriptions/reads with current choices and source state.
- Structured success/error responses, strict option/arity parsing, shared planning error codes, and verified transaction results. Device-qualified setting IDs survive reads and descriptions.
- A reviewed inventory of 86 operation groups covering all 118 current `Q_INVOKABLE` methods, including My Omarchy profiles. The initial inventory recorded forty CLI gaps; the audio/Bluetooth slice reduces that count below. Helper destinations remain marked for source/automation review. These are accounting results, not complete parity or live verification.
- An independent source-to-inventory check in CI, tests for new/stale/duplicate mappings, and an updated bundled skill that explains the supported API and remaining gaps.

The [API reference](CLI-API.md) documents the installed command contract. Verification in a disposable x86_64 Arch container on this macOS host:

- The production binary builds successfully. Focused API tests pass: 20 C++ cases, plus four Python operation-accounting tests. The bundled skill passes the skill validator, and the translation-report command completes.
- The full C++ suite reports 538 passes and one failure: `appearanceShowsPacksAndChoosingOneWorks`, waiting for keyboard focus on the Follow desktop theme tile. This is the focus failure observed during earlier release work; the test body is unchanged from `origin/main`. The full suite is therefore not green.
- No writes were made to a real Omarchy desktop, no package lifecycle was exercised, and no agent harness was registered. These results establish fixture/API behavior only.

Still open: auditing the actual automation requirements behind helper routes, dynamic target enumeration for remaining domains, complete descriptions and adapters for the remaining provider gaps, versioned reset/undo, durable previews, harness discovery, stock-skill forwarding, and the package/session activation/removal prototypes. CLI-0 and CLI-1 are in progress; CLI-2 has begun with audio, Bluetooth, services and startup.

### Audio and Bluetooth implementation

- All 13 audio/Bluetooth operation groups have API command adapters and parameter/target discovery metadata. At completion of this slice the inventory covered 118 GUI-invokable methods and 86 groups: 13 `api-cli`, 38 `legacy-cli`, eight `ui-only`, and 27 `cli-gap`. This is accounting, not complete automation or live-verification evidence.
- `audio status` exposes outputs, microphones, and application streams. Changes cover defaults, volume, mute, and individual stream routing, with shared provider plans. Volume/mute and stream moves now use the coordinator's journal and readback verification from both the CLI and GUI.
- DSP resolution completes before CLI planning. Device identity and the effective route are checked before writes. Stream selectors contain the PipeWire identity so a recycled index is rejected across separate calls and during execution. Default verification covers the default device, not every stream Omarchy attempts to move.
- Bluetooth reads every controller and known device, supports radio power and paired-device connect/disconnect/forget, and preserves an interactive pairing handoff. Failed or empty reads cannot prove disconnection, removal, or power-off. Power-on verification requires every controller on. Plans expose persisted radio effects and consequential disconnect/removal acknowledgements.
- Reviewed Omarchy 4.5 helper sources have compatibility fingerprints; plans refuse changed helpers. Runtime operations declare no automatic undo and cannot bypass a profile restore lease.

Validation in the disposable Arch environment:

- The production binary builds. The first provider-focused run passed 34 C++ cases, including existing GUI-facing provider regressions, against stateful fake services.
- The full C++ suite reported 572 passes and the same `appearanceShowsPacksAndChoosingOneWorks` keyboard-focus failure recorded above. After tightening multi-controller power verification, all 16 affected CLI/provider/overview cases passed, including the added partial-power-on regression. The suite is not fully green.
- The operation-accounting guard and its four Python tests pass. The translation consistency check and bundled-skill validator pass; untranslated new diagnostics still use their English fallback.
- All 22 production-binary smoke checks passed without display/session services, using an isolated home: discovery and parameter metadata, DSP resolution, audio changes and stale stream rejection, paired Bluetooth actions, consequence acknowledgement, multi-controller power verification, false-success refusal, versioned GUI-launch refusal, and byte-identical bundled-skill export.

No real desktop devices or agent registrations have been changed. Package/session activation and real harness discovery remain untested.

### Services and startup implementation

- Added `services <user|system> status`, scoped start/stop/restart/enable/disable, and `startup list|enable|disable`. Mutations expose shared dry-run plans and the same coordinator/readback path as the GUI. At completion of this slice the inventory had 16 `api-cli`, 38 `legacy-cli`, eight `ui-only`, and 24 `cli-gap` groups, still covering 118 GUI methods across 86 groups.
- User/system discovery failures remain scoped, and both unit and unit-file lists must be readable. Static/masked installed services are visible; template/pattern actions and unsupported enablement states are refused. Plans and writes inspect actual unit identity/state. Verification requires settled state, persisted enablement, or fresh process-start evidence for restarts and completed one-shots. Old successful one-shot results cannot prove a new run.
- System stop/restart/disable requires consequence acknowledgement. CLI actions use existing policy without asking for passwords; the GUI retains desktop authorization. Service hooks/dependencies remain outside selected-unit verification. No automatic service undo is advertised.
- Startup discovery honors XDG configuration paths and whole-entry precedence, exposing source files and remaining launch conditions. Edits preserve other groups, comments and line endings, guard queued changes with fingerprints, back up the user override for failure recovery, and refuse linked or system-resolving user paths. Only the startup preference is verified; future launch is not claimed, and history undo is not advertised.
- The bundled skill and API reference explain these calls, their effect boundaries and recovery limitations. Startup operations work without systemd and both families respect a profile restore lease.

Validation in the disposable Arch environment:

- All 61 focused grammar/provider/GUI regression cases passed. A fresh full build and suite reported 605 passes and one failure, the same `appearanceShowsPacksAndChoosingOneWorks` keyboard-focus failure recorded above. The suite is not fully green.
- The production binary builds and passes 24 service/startup smoke checks without display/session services, including scoped actions, fresh one-shot/restart verification, authorization refusal, XDG overrides, symlink refusal, and byte-identical bundled-skill export into a temporary home.
- The operation-accounting guard and its four Python tests pass. Translation consistency, the bundled-skill validator, documentation links/fences and whitespace checks pass. New untranslated diagnostics retain their English fallback.

No real services, startup entries or agent registrations have been changed. Real session/authorization behavior, package lifecycle and agent harness discovery remain open verification gates.

### Installed apps and MIME defaults implementation

- Added `apps list`, planned `apps launch <desktop-id>`, `mime list`, `mime get <type>`, and planned `mime set <type> <desktop-id>`. At completion of this slice the inventory had 19 `api-cli`, 38 `legacy-cli`, eight `ui-only`, and 21 `cli-gap` groups, still accounting for all 118 GUI methods across 86 groups. Legacy `apps launcher ...` routing/output is preserved.
- Shared app discovery honors XDG data roots and nested desktop IDs, preserves high-priority masking, and rejects unavailable or ambiguous launch/association targets. `NoDisplay` remains distinct from deletion. Launch reports an unverified handoff, with no claim that a window appeared and no settings transaction.
- MIME reads distinguish failed/malformed output, unresolved empty results and stale handler IDs. Common-type discovery retains the GUI's seven rows; individual reads/changes support other valid MIME types. Declared-type candidate lists are explicitly incomplete. GUI choices no longer present failed reads as an unset handler.
- Shared GUI/CLI changes select a declaring current-desktop-specific user override when present, otherwise the generic user file. Format-aware editing preserves fallback IDs, comments, line endings and unrelated groups. Plans expose the actual path and fallback list; app/input fingerprints and fresh effective-state checks guard apply. File backup/rollback and the restore lease are honored; history undo and functional file-opening verification are not promised.
- Profile restoration retains its separate backend/interface; its association verification now also requires a successful, untruncated query. The bundled skill and API reference explain current discovery, file ownership, result and recovery boundaries.

Validation in the disposable Arch environment:

- The fixture/grammar/GUI checks report 45 passing cases, and both real `xdg-mime` integration cases pass with isolated generic and desktop-specific overrides. A fresh full build and suite report 622 passes and one failure, the same `appearanceShowsPacksAndChoosingOneWorks` keyboard-focus failure recorded above. The suite is not fully green.
- The production binary builds and passes 20 app/MIME smoke checks using real `xdg-mime` without desktop services, covering default readback, fallback preservation, current-desktop overrides, masking/conflicts, launch handoffs, unchanged legacy launcher output and byte-identical skill export into a temporary home.
- Operation accounting and its four Python tests, translation consistency, skill validation, documentation links/fences and whitespace checks pass. New untranslated UI/diagnostic text retains its English fallback. CI installs `xdg-utils` for the real-query integration checks.

No real desktop associations, applications or agent registrations have been changed. Real session behavior, automatic skill registration/forwarding, package lifecycle and agent task evaluations remain open gates.

### System facts and diagnostics implementation

- Added scoped `system status|machine|battery|storage|hooks` snapshots and `diagnostics export [--dry-run]`. At completion of this slice the inventory had 21 `api-cli`, 38 `legacy-cli`, eight `ui-only`, and 19 `cli-gap` groups, accounting for 118 GUI methods across 86 groups.
- Reads complete before CLI output and retain section-level sources/states. Battery supply presence, capacity and cycle data distinguish unknown from absent; all detected supplies are listed while the GUI summary uses the first. Storage retains unknown metrics as null, detects malformed/failed reads, preserves mount spaces and negative availability, and exposes existing shared/temporary mount annotations. Hook inventory includes inert samples and metadata without running code; inaccessible parents/roots are not empty inventories. GUI storage/hook messages reflect unavailable reads.
- Diagnostics refreshes capability readings and awaits collection, private saving and path/byte/fingerprint/permission verification. Complete artifact success is distinct from an explicitly incomplete saved report. Common quoted/plain credentials, authorization values and private-key blocks are redacted heuristically; sharing still requires review. Exports have no settings transaction or history undo.
- Reviewing the actual Omarchy 4.5 helper revealed its shared raw `/tmp/omarchy-debug.log`, even in print mode. A fingerprinted adapter now stages the installed helper privately, changes only its log destination, runs non-sudo print mode with output/time/file-size limits and no core dumps, and removes staging. Installed helper/shared log remain untouched. The helper's compatibility fingerprint and required binding guard refuse unreviewed/unrecognized layouts.
- Same-provider duplicate collection and invalid ownership are refused. Destroyed providers clean unfinished exports/staging. Async capability scans now have completion callbacks, failed/truncated reads do not supply trusted versions/device facts, and the command runner records final/stdout/stderr truncation.

Validation in the disposable Arch environment:

- The initial focused run passed 57 cases, and the private-adapter follow-up passed 13. The final build and full suite, including timeout/raw-file-limit cases and the last permission guard, report 640 passes and the same `appearanceShowsPacksAndChoosingOneWorks` keyboard-focus failure recorded above. The suite is not fully green.
- Production builds pass 15 system/diagnostics checks using the actual reviewed helper with isolated read-command fixtures, including untouched shared raw log, private staging cleanup, no sudo/upload/menu calls, compatibility refusal and byte-identical skill export.
- Operation accounting and its four Python tests, translation consistency, skill validation, documentation links/fences, Python syntax and whitespace checks pass. New untranslated text retains its English fallback.

No real desktop settings or agent registrations have been changed. Real hardware/session behavior, stock-skill routing, automatic registration/package lifecycle and agent task evaluations remain open verification gates.

### Shortcut discovery and rule validation implementation

- Added versioned `shortcut list`, `shortcut check <keys>`, `rule types`, `rule validate <match> <action> [value]`, and opt-in versioned `rule list`. Two GUI gaps were closed; a new read-only rule-inventory group was recorded explicitly. At completion of this slice the inventory had 24 `api-cli`, 38 `legacy-cli`, eight `ui-only`, and 17 `cli-gap` groups, accounting for all 118 GUI methods across 87 groups.
- Shortcut discovery awaits bounded compositor reads and independently reports owned-module state. It preserves all reported bindings, including submaps, flags, raw masks, keycodes and catch-all entries. Failed/malformed/truncated reads return unavailable/null data. Named-key checks retain matched binding details and distinguish unresolved keycode/multi-key comparisons; they do not establish keyboard-layout equivalence or global freedom.
- Shared chord parsing rejects unknown/repeated modifiers, empty tokens and control characters instead of silently dropping tokens. Adds refresh collision data and refuse failed discovery. The GUI uses the same checks and avoids claiming unresolved comparisons are free; obsolete free-chord translations were removed and new strings retain English fallback.
- Rule constraints and validation share the provider implementation. No-value actions reject extra values, value patterns reject trailing controls, and validation explicitly establishes only input acceptance. Owned-rule reads never execute Lua, retain current indices, and distinguish absent, modified, unreadable, oversized and dangling-link sources. The module read cap is 4 MiB.
- Existing unversioned mutation/output routes remain. Invalid text, overflow, negative rule indices and excess positional arguments are refused; invalid text can no longer remove index zero. Versioned mutation plans, identity-guarded removal, stronger mutation/readback parity and complete replacement planning remain open; the new read operations do not close those legacy gaps.
- The bundled skill and API reference teach discovery, result limits and legacy routing. Automatic stock-skill forwarding and per-user registration are still unimplemented.

Validation in the disposable Arch environment:

- The initial focused run passed all 96 cases. The final build/full suite, including the last bounded-read/dangling-link/unreadable-file checks, report 678 passes and one failure: the same `appearanceShowsPacksAndChoosingOneWorks` keyboard-focus failure recorded before this slice. The suite is not fully green.
- The production binary builds and passes 31 shortcut/rule smoke checks without display/session services, including retained raw binding details, scoped collisions, failed discovery refusal, legacy add/remove behavior and rule-list output, index rejection, ownership conflicts, read-only checks, and byte-identical skill export into a temporary home.
- Operation accounting and its four Python tests, translation consistency, bundled-skill validation, documentation links/fences, Python syntax and whitespace checks pass. Evidence is retained in `build/cli-agent-tests/shortcuts-focused.log`, `shortcuts-full.log`, `shortcuts-coverage.json`, and the isolated production script `build/cli-agent-app/shortcuts-smoke.py`.

No real desktop settings or agent registrations have been changed. Real-session/hardware behavior, stock-skill routing, automatic registration/package lifecycle and end-to-end agent tasks remain open verification gates.

### Versioned shortcut and rule mutations implementation

- Added API v1 shortcut/rule add/remove with shared GUI/CLI plans, `--dry-run`, structured transactions and explicit verification scopes. At completion of this slice four legacy groups gained versioned adapters: the inventory had 28 `api-cli`, 34 `legacy-cli`, eight `ui-only` and 17 `cli-gap` groups across the same 118 GUI methods/87 groups. This improved result/planning parity without claiming another reduction in the gap count.
- Owned entries expose `index@owned-module-sha256` selectors. Versioned removal and the GUI use them; stale module contents, ordering and duplicate-rule indices cannot silently select another entry. Legacy key/index entry points and JSON output remain available with strict argument parsing.
- Shortcut plans refresh reported collisions. API replacements require `--replace` plus `--yes`; `--yes` alone is not replacement intent. Matching submaps/catch-all entries and unresolved keycodes are refused. Critical shortcut removal requires consequence acknowledgement, with the GUI displaying its existing launcher warning. Command/description bounds and control-character validation are declared.
- The shared transaction adapter checks file ownership, links, bounded reads, active import layout, runtime version compatibility and complete config-error arrays. A coordinator preflight hook runs bounded reads under the change lock before capture/intent, then rechecks file fingerprints. Queued collision/file changes are refused before mutation, and a restore lease still excludes these actions.
- Writes verify owned entries/imports, a successful reload and no additional config errors against preflight. Source review established that Omarchy's `o.bind` uses Hyprland's opaque `__lua` reference. Shortcut adds therefore verify persisted command bytes and a matching live Lua binding/description/flags, not live command text or execution. Rules/removal verify owned persistence/reload, not app matching, effective rules or a globally unbound chord.
- Failed verification restores captured files and checks the recovery reload; failed recovery or retained concurrent edits report `partial`. The coordinator can record exact written fingerprints so edits during reload are not mistaken for Control's own bytes and overwritten. No automatic history undo is advertised. The bundled skill and API reference explain selectors, opt-in mutation grammar, confirmation, scope and recovery.

Validation in the disposable Arch environment:

- The initial regression run passed 74 cases, and the focused mutation/coordinator pass passed 24. The broad build/full suite reported 696 passes and the same existing `appearanceShowsPacksAndChoosingOneWorks` keyboard-focus failure; it is not fully green.
- After that full run, the last GUI confirmation fix retains the displayed selector by value, failure recovery refreshes the GUI's live binding cache, and an additional stale-GUI-row regression was added. A fresh build and final follow-up passed all 68 selected grammar/provider/GUI cases, including page loading, input navigation, recovery scenarios and queued guards. The full suite was not repeated after these small final changes.
- The rebuilt production binary passed all 38 isolated shortcut/rule action checks again, including versioned plans/results, preserved legacy output, stale selectors, explicit replacement/acknowledgement, compatibility refusal, rollback/partial recovery and byte-identical skill export. The compositor fixture reports opaque Lua binding references; no configured shortcut command was executed.
- Operation accounting and its four Python tests, translation consistency, bundled-skill validation, documentation links/fences, smoke-script syntax and whitespace checks passed. New English strings retain fallback behavior. Evidence is retained in `build/cli-agent-tests/actions-initial-focused.log`, `actions-focused.log`, `actions-full.log`, `actions-final-followup.log`, `actions-coverage.json`, and `build/cli-agent-app/actions-smoke.log` / `actions-smoke.py`.

No real desktop settings or agent registrations have changed; stock-skill forwarding, automatic registration/package lifecycle, real sessions and end-to-end agent tasks remain open gates.

### Shell discovery, plugin state, widget movement and dock pins

- Added scoped `plugin list|status`, `bar status`, and `dock status`; API v1 plugin enable/disable, fingerprinted widget moves and dock pin/unpin share provider plans with the GUI. The inventory now has 32 `api-cli`, 32 `legacy-cli`, eight `ui-only` and 15 `cli-gap` groups across all 118 GUI methods/87 groups. Two gaps are closed and two legacy groups gain versioned planning/results.
- Reviewed the actual Omarchy 4.5 scripts and shell registry/configuration contracts. Live bar reads use `listShellConfig`, preserving the reported implementation and full instance options; persisted source files remain distinct. Failed/malformed/truncated/duplicate registry data is unavailable, while offline manifests are bounded, explicitly incomplete hints with unknown enabled state. IDs outside the writer grammar remain visible in valid discovery data.
- File reads are bounded, versioned JSON. Missing/zero-byte user configuration selects defaults, but invalid nonempty files are not silently replaced. Dock pin fallback is resolved independently and has a preference-only scope; a live shell is not required. Pin candidates honor the existing app discovery/masking policy, and missing apps can still be unpinned.
- Widget selectors bind the section/index to the full live bar fingerprint, keeping duplicate IDs/options distinct. Destination indices are measured after source removal and strictly validated. Tray clones are checked through registry metadata. Plans require relevant live/persisted agreement, safe user paths, reviewed helper fingerprints and recognizable configuration shape. A locked preflight repeats the plan and file guards before capture/intent; restore leases exclude writes. The GUI keeps planning busy and rejects a changed displayed file snapshot.
- Readback verifies registry state and its persisted meaning, the full live/persisted moved layout with options, or the exact ordered dock preference. It does not establish plugin health, visible rendering or app launch. Valid legacy plugin/widget output is retained; legacy index text can no longer silently coerce to zero.
- These external writers do not expose exact written bytes. The coordinator can retain captured backups without automatically restoring an un-attributable file after failure. Unverified writes report `partial`; concurrent edits survive, and automatic history undo is not advertised. Other operations retain their existing recovery policies.
- Clone/removal are confirmed parameterized, noninteractive helper workflows, not inherently interactive boundaries. Their versioned directory capture/recovery and lifecycle verification remain gaps. The existing GUI removal call also still needs to pass the helper's `--yes` after its already-displayed confirmation; that belongs with the lifecycle adapter and its directory tests.
- Updated operation metadata, compatibility dependencies/generator, bundled skill and API reference. All seven helper sources used for integration checks match the recorded fingerprints; actual shell/session behavior remains a separate gate.

Validation in the disposable Arch environment is complete for this slice:

- Focused shell coverage passed 89 tests; the GUI/follow-up run passed 10, including keyboard movement, dragging and dock removal. The full suite finished with 729 passing tests and the previously known `appearanceShowsPacksAndChoosingOneWorks` focus assertion failure; it is not a green full-suite result.
- The rebuilt production binary passed 27 isolated shell checks using seven fingerprint-matched Omarchy 4.5 helpers and simulated `qs` IPC. These checks cover scoped discovery, plans/results, stale-state refusal, option-preserving moves, preference-only dock changes, compatibility blocking, preserved legacy output, partial recovery with retained backups and byte-identical skill export. No plugin code or app was launched; this is not live desktop verification.
- Operation accounting and its four Python tests, translation consistency, bundled-skill validation, documentation links/fences, smoke-script syntax and whitespace checks passed. Evidence is retained in `build/cli-agent-tests/shell-focused.log`, `shell-gui-focused.log`, `shell-full.log`, `shell-coverage.json`, `shell-i18n.log`, and `build/cli-agent-app/shell-smoke.log` / `shell-smoke.py`.

No real desktop settings, plugin directories or agent registrations have changed. Stock-skill forwarding, automatic registration/package lifecycle, live sessions and end-to-end agent tasks remain open gates.

### Explicit per-user skill registration and stock forwarding

- Added structured `skill status|plan|install|reconcile|remove`, persistent `opt-out|opt-in`, repeated explicit `--target` directories and mutation dry-runs. The original bare `skill install` retains human output and the Claude default. Reads/plans create no registration files or directories and start no agent.
- Generated per-user companion/forwarding skills link to the current `$OMARCHY_PATH` stock skill, retaining topic-guide relative references and development relinks. The stock-named skill includes Control discovery without depending on initial sibling selection; missing/incompatible Control can fall back to current stock guidance. A stock reference to Control conservatively skips an additional wrapper without claiming routing is verified.
- Registration replaces only missing entries, verified stock links or matching manifest-owned entries. Bounded, versioned private state stores original raw link text, payload fingerprints, configured targets, opt-out and write-ahead intent. Atomic writes/links and a separate user lock guard installation and removal. Reconciliation recognizes Omarchy relinks and pending old/new states. Conflicts preserve user content and retain fallback guidance/references; safe targets can complete with an explicit partial result.
- The observed Omarchy 4.5 provisioning paths are documented separately from actual harness loading. Directory aliases are deduplicated; custom locations need explicit targets, and private data/state paths are limited to the invoking user's home in this initial adapter. Root/elevated registration is refused. Filesystem verification never claims harness refresh or automatic activation.
- The [registration contract](AGENT-SKILLS.md) lists exact paths, compatibility/migration limits and repair/removal behavior. The operation inventory remains 32 API groups and 15 CLI gaps; this slice adds integration commands rather than closing a GUI-provider gap.

Validation in the disposable Arch environment:

- All 29 focused registration cases passed, including read-only plans, relative stock-link restoration, custom-content preservation, aliases/profiles, development/upstream relinks, persistent opt-out with custom targets, recorded bundle upgrades, scoped removal, pending install/removal intent, lock refusal, malformed state, dangling links and hard-linked payloads. The full suite reported 755 passes and the same previously known `appearanceShowsPacksAndChoosingOneWorks` focus assertion failure; it is not a green full-suite result.
- The rebuilt production binary passed 31 skill checks in an isolated home using the actual Omarchy 4.5 skill and all six topic guides, including preserved edited upstream references. An additional root-context check confirmed refusal before registration data/state creation. The prior 27 shell CLI checks also passed again using fingerprint-matched helpers with simulated IPC. No agent harness, plugin code or app was launched.
- Both skill entry points passed skill validation. Operation accounting and its four Python tests, translation consistency, documentation links/fences, QA-script syntax and whitespace checks passed. The final follow-up passed all 30 focused cases after embedding the updated skill prose and exposing conflicting upstream references in status/plans; the full suite was not repeated for these final changes. Evidence is retained in `build/cli-agent-tests/skill-focused.log`, `skill-full.log`, `skill-final-focused.log`, `skill-coverage.json`, `skill-i18n.log`, and `build/cli-agent-app/skill-smoke.py` / `skill-smoke.log` / `skill-shell-regression.log`.

No real agent registrations or package/session hooks have changed. Real harness stock-only loading, direct-install activation for active users, inactive-user cleanup, upgrade/downgrade and end-to-end tasks remain release gates.

### Actual harness discovery and user-manager activation prototype

- Added credential-free, no-model-turn probes for actual Codex CLI 0.167.1, Pi 0.64.0 and Claude Code 2.1.322 on macOS. Sixteen scenarios cover default/generic roots, custom homes, multiple roots, aliases, conflicts, opt-out/opt-in and removal. Fifteen discover the registered skills; Claude's generic `.agents/skills` case records non-discovery, so its own root remains required. These are metadata/control-interface results, not stock-only model selection or task execution.
- Actual Codex discovery exposed duplicate `omarchy` identities from the visible nested stock skill. The guide reference now uses `.upstream`; reconciliation creates that reference before publishing updated instructions and removes only a recorded, unmodified legacy visible reference. Custom references remain conflicts. Existing manifests are accepted for guarded migration/removal.
- Added an unshipped package-to-user-manager prototype: bounded root notifications contact only already-running regular-user managers, while user one-shot units call the existing CLI. A global user-target dependency handles the next manager start; example pacman hooks show post-install/upgrade notification and pre-removal cleanup. Root never guesses a desktop home or runs a write loop over user directories. Neither package recipe, installer nor updater activates these prototype files.
- A production ARM build with real Debian 13/systemd 257.13 managers passed 18 lifecycle checks, including active and next-manager activation, user ownership, opt-out, conflicts, upstream relinks, active-user restoration and inactive-user fallback after staged payload removal. Deferred inactive-user unregistration is not implemented. The x86 Arch systemd manager could not boot under translation on this Mac, so the native Linux prototype does not close Arch/pacman or graphical-session gates.
- The lab now masks host-wide setup services and mounts `/proc/sys` read-only before systemd starts. An initial privileged test reset Docker's translation registrations; Docker Desktop was restarted to restore them. The protected lab was rerun successfully and x86 execution rechecked. No other user's containers were running during the restart.
- Added eight Python identity/notification guard tests to CI and documented exact support evidence and reproducible checks in the [lifecycle prototype](AGENT-LIFECYCLE.md). The operation ledger remains 118 GUI methods/87 groups, with 32 API groups and 15 explicit CLI gaps.

Validation: the initial 32 focused C++ registration/migration cases, eight Python lifecycle tests, four accounting tests, sixteen actual harness scenarios and 31 production registration checks pass. The full C++ run finished with 758 passes and the same known appearance-focus assertion failure; it is not a green full-suite result. A final follow-up passed all 33 focused cases and repeated the 31 production checks after adding a recovery guard/test for both guide references being journaled at the 32-target limit; the full suite was not repeated for that bounded follow-up. Both skills, translation consistency, documentation links/fences, Python/shell syntax and whitespace checks pass. Final follow-up evidence is in `build/cli-agent-tests/activation-final-focused.log` and `build/cli-agent-app/activation-smoke.log`. No real-user skill registrations were changed, and no model turn was started. Package transactions on Arch, actual Omarchy graphical sessions, inactive-user unregistration, Linux harness discovery and stock-only model tasks remain release gates.

### Prepared-user cleanup after package-payload loss

- Added an unshipped per-user recovery controller that captures a private, fingerprinted copy of Control plus its cleanup helper and user service/timer. Registration removal continues to use the existing CLI engine and its ownership/journal rules; Python manages only the private lifecycle artifacts. Captured context contains paths, not credentials.
- Prototype activation prepares recovery before reconciling skills, preserves opt-out, and requires verified complete stock forwarding before reporting activation. The package's pre-removal user service now retires owned runtime files after active-user cleanup. The public `skill install` command and both package recipes remain unchanged.
- The retained timer runs at the next user-manager start after payload loss. Package presence and the pacman lock defer removal; altered files, partial/unverified CLI results, timeouts and missing runtime libraries retain fallback and recovery state. A durable registration-removal phase permits retirement to resume without the copied binary. Owned files and scheduling links are removed on success; private receipts/locks remain, and late self-removal interruption can still need explicit repair/reinstall.
- Expanded real-systemd scenarios cover inactive-user link restoration, companion/runtime deletion, homes with spaces/percent/dollar signs, an unavailable Qt library, an edited registration, transaction-lock deferral and retirement interrupted after binary deletion. Dependency restoration, conflict resolution and a user-manager restart each demonstrate the appropriate retry. These remain native Debian/systemd tests with staged payloads, not Arch package transactions or graphical sessions.
- The retained binary still needs its Linux libraries and Python. Unprepared legacy/manual registrations, dependency pruning/ABI changes, downgrade, package-boundary races and late self-removal are explicit remaining lifecycle gates. Automatic activation remains unshipped. See the updated [lifecycle contract](AGENT-LIFECYCLE.md).

Verification: all 28 Python notification/recovery cases pass on macOS and Linux. The final expanded native Debian/systemd run passed 41 checks, followed by four actual reactivation/reinstall checks with the current controller, including a second deferred cleanup. Operation accounting and its four Python tests, Python/shell syntax, documentation links/fences and whitespace checks pass. No C++ registration code changed, so its full suite was not repeated; the previously recorded appearance-focus failure remains unchanged. No real-user registrations or package recipes were changed. Evidence is retained in `build/deferred-agent-cleanup/unit-tests.log`, `linux-unit-tests.log`, `session-verified.log`, `reinstall-check.log`, `coverage.json` and `build.log`. Arch transactions, unprepared legacy/manual users, full dependency pruning, downgrade/ABI changes, package-boundary races and model-task execution remain open gates.

### Real fixture package lifecycle

- Added a reusable staging helper and a guarded disposable-lab check that builds fixture packages with actual makepkg and executes pacman install, upgrade, same-API downgrade, reinstall and removal. The package contains the production binary, canonical companion skill and prototype hook/user-unit/recovery payload. Neither production recipe, installer nor updater has changed.
- The package's own new post-transaction hook executes on first installation. Already-running users activate through their own managers; the global dependency activates another user at next manager start. Actual pacman removal runs the pre-removal service while helpers exist, and a prepared inactive user subsequently restores stock links and retires recovery at next manager start.
- Seven users cover opt-out, custom skills, modified retained binaries, modified registrations and unprepared manual registrations. Conflicts remain visible without aborting other users or overwriting edits; resolving owned-file conflicts permits cleanup to resume. Hook failure and package-command success are recorded separately. Pacman's physical output chunking is handled when checking the hook's JSON result.
- Fixed the recovery wrapper's failure accounting: attempting a child writer can already change registrations, so a failed, timed-out or invalid response reports possibly `partial` effects. A real edited-registration removal demonstrates this path, alongside the focused regression test.
- All 36 real package checks pass with pacman 7.0.0/libalpm 15.0.0, Debian 13 libraries and systemd 257.13 on native ARM. All 33 Python lifecycle/staging/output tests pass on macOS and Linux. Same-API fixture versions have inert binary stamps; this does not establish compatibility across API/ABI changes or validate Arch dependency metadata/pruning. Automatic activation remains unshipped.

Evidence and reproduction are in the [lifecycle contract](AGENT-LIFECYCLE.md) and `build/agent-package-lifecycle/verified/report.json`, with actual transaction logs alongside it. No C++ engine code changed; the full C++ suite was not repeated and its previously recorded appearance-focus failure remains open. Remaining gates include actual Arch recipes/Omarchy desktop sessions, dependency policy, Omarchy relink/new-profile events, incompatible downgrade/races, Linux harness discovery and stock-only model tasks. The operation ledger remains 118 GUI methods/87 groups, with 15 CLI gaps.

### Icon sample and picker discovery

- Added API v1 `icons list`, `icons samples <pack-id>` and `icons search <pack-id> [query] [--limit <1..300>]`. Samples/search select the API implicitly; existing icon-list output and icon/launcher mutation commands retain their unversioned interfaces. Discovery advertises pack IDs, parameter limits and file-lookup verification scopes.
- Shared sample and picker lookup now exposes originating packs, inherited files, usable file paths/URLs and explicit incomplete-read diagnostics. Search selects a pack explicitly, while the GUI picker uses its current choice. XDG precedence, nearest-size directory lookup, SVG preference, application/device contexts and hicolor fallback remain shared behavior. Search rows have stable ordering and a separate truncation flag.
- Bounded directory enumeration, descriptors and inheritance prevent a small result limit from hiding an unlimited scan. Broken links, inaccessible directories, malformed/oversized descriptors and missing declared parents cannot masquerade as an empty verified read. Missing optional roots and absent hicolor remain ordinary. Read completeness does not establish image decoding or desktop rendering.
- Updated the bundled skill and API reference with the discovery commands, source/result limits and existing mutation boundaries. The inventory now covers the same 118 GUI methods across 88 operation groups: 35 `api-cli`, 32 `legacy-cli`, eight `ui-only` and 13 `cli-gap`. Two GUI discovery gaps are closed; one additional read-only pack inventory group is now accounted for.

Verification: all 161 final icon/launcher/API regression cases pass on Arch with Qt 6.12.0. The production Linux CLI passes 33 isolated smoke checks, including unchanged legacy JSON, encoded file URLs, uppercase extensions, read-only behavior and byte-identical skill registration. The final full Arch suite reports 791 passes and the same known `appearanceShowsPacksAndChoosingOneWorks` keyboard-focus assertion failure; the full suite is not green. Four accounting tests and 33 lifecycle guards pass; skill validation, translation consistency, documentation links/fences and whitespace checks pass. A native Debian Qt 6.8 GUI run could not load the existing QML accessibility property, so broad GUI validation uses the matching Arch Qt environment.

Evidence is retained under `build/icon-cli-tests/`: `verification.json`, `arch-final-followup.log`, `arch-verified-full.log`, `smoke.log`, `coverage.json` and build/check logs. No real desktop or user skill registrations were changed. Automatic package activation, remaining legacy mutation parity, display/network previews and real desktop/model-task gates remain open.

### NetworkManager state and cached access-point discovery

- Added API v1 `network status|general|devices|saved|wifi`, selected implicitly. `status` awaits all four sources; scoped reads request only their own source. These calls do not scan, check connectivity, ask for secrets or change connections. CLI discovery describes the read contract and remaining mutation gaps.
- Shared provider reads now record per-source errors, completion times and nullable values. Malformed/truncated/failed output is not an empty verified inventory or an off radio. Every snapshot-read callback is guarded by its refresh generation, and completion waits for all requested reads. Snapshots remain explicitly non-atomic, with unknown AP age and process-local generations rather than preview tokens.
- The CLI retains all reported device/profile types, distinct UUIDs sharing names, and AP identities by interface/BSSID, including duplicate SSIDs and empty display SSIDs. Unknown device connection states remain null. The GUI keeps its existing device filter and active/strongest-per-name grouping. Source failures clear stale lists and remain visible; pending refreshes preserve row identities and disable affected actions. Overview/Wi-Fi controls distinguish unavailable radio/connection state from off/disconnected.
- Actual nmcli testing exposed Unicode transliteration under plain `C` and empty separators between multiple interfaces' AP tables. Reads now use `C.UTF-8` with English messages, forced plain output and escaped fields, preserving accented display names, colons and backslashes. Descriptions distinguish display SSIDs from arbitrary raw SSID bytes. Reads are bounded to four seconds, 1 MiB per stream and 4,096 rows.
- Added a guarded reproducible integration check using actual nmcli 1.52.1 against an unmanaged private NetworkManager and Wi-Fi D-Bus fixtures. It checks a temporary inactive unsaved profile, multiple adapters, secured/open APs, Unicode/escaping, and the absence of scan, connectivity-check and secret requests. This establishes client/protocol behavior, not real Wi-Fi hardware or desktop behavior. It adds no production Python/mock dependency.

Verification: 141 focused network/API/UI cases, 27 production-process smoke checks and 23 real-client integration assertions pass. The final full Arch/Qt 6.12 run reports 839 passes and the same known `appearanceShowsPacksAndChoosingOneWorks` keyboard-focus failure; the full suite is not green. Four accounting tests, 33 lifecycle guards, skill validation, translation consistency, documentation and whitespace checks pass. Evidence is retained under `build/network-cli-tests/`, including `verification.json`, `focused.log`, `full.log`, `smoke.log`, `integration.json`, coverage and build/check logs. No real user registrations or desktop network settings were changed.

At this slice the ledger covered 118 GUI methods/88 groups: 36 `api-cli`, 32 `legacy-cli`, eight `ui-only` and 12 `cli-gap`. One discovery gap was closed. Explicit scan requests are addressed below; radio changes, activation/disconnection and cross-process keep/revert remain open.

### Explicit Wi-Fi scan requests

- Added API v1 `network rescan [interface] [--dry-run]` and routed the GUI Scan action through the same provider. Plans discover eligible managed/available Wi-Fi adapters; an omitted interface selects all eligible adapters, up to 16. Invalid, absent, non-Wi-Fi, external, unmanaged, unavailable and unknown-state targets cannot silently become accepted requests.
- Execution honors restore leases and the global change lock, refreshes preconditions under the lock, and resolves all selected NetworkManager object paths before sending any request. Noninteractive D-Bus `RequestScan` calls use bounded per-process and batch deadlines. Plans send no scan and write no journal receipt. No radio preferences, profiles or credentials are changed.
- Results distinguish `scan_requested`, `blocked`, `unknown` and `partial`, retaining per-interface attempted/accepted evidence and unsent deadline-exhausted targets. `verified`, `scanCompletionVerified` and `accessPointAgeKnown` remain false. A successful void reply establishes acceptance only. The non-undoable action journal's `committed` phase is scoped to request acceptance, never scan completion, cache freshness or persistent settings. Unconfirmed requests are not retried automatically.
- The bundled skill, README and API reference explain planning, target discovery, deferred cached reads, acceptance-only history and failure handling. The guarded integration tool optionally exercises actual nmcli/busctl against private Wi-Fi D-Bus mocks, including denied/partial batches; the unmanaged real-daemon case refuses a scan plan. This adds no production Python/mock dependency and establishes no real Wi-Fi hardware behavior.

Verification: 165 focused network/API/UI cases, 38 production-process checks and 40 real-client integration assertions pass. The original discovery-only integration mode still passes its 23 assertions. The full Arch/Qt 6.12 suite reports 863 passes and the same known `appearanceShowsPacksAndChoosingOneWorks` keyboard-focus failure; the full suite is not green. A final test-only change removes an assumption about coarse timer scheduling from the batch-deadline assertion; its rebuilt follow-up passes with unchanged production sources/binary. Four accounting tests, 33 lifecycle guards, skill validation, translation consistency, documentation and whitespace checks pass.

Evidence is retained under `build/network-scan-tests/`, including `verification.json`, `focused.log`, `full.log`, `deadline-followup.log`, `smoke.log`, `integration.json`, coverage and build/source fingerprints. No real user registrations or desktop network settings were changed. At this slice the ledger covered 118 GUI methods/88 groups: 37 `api-cli`, 32 `legacy-cli`, eight `ui-only` and 11 `cli-gap`. Radio changes are addressed below; activation/disconnection and durable status/keep/revert remain open.

### Wi-Fi radio control

- Added API v1 `network radio true|false [--dry-run] [--yes]`, preserving `network wifi` as an AP-cache read. Plans report NetworkManager's global switch and independent hardware flag, the current Wi-Fi interface-name inventory, consequences and the narrow verification scope. Off always requires `--yes`; hardware-blocked enabling, absent Wi-Fi adapters and unavailable reads/tools are refused.
- The GUI and CLI share planning and execution. Restore leases and the global change lock guard writes; a fresh preflight rejects changed switch/hardware state or interface names. Same-provider pending scans, connection operations and previews block radio changes, and active radio changes disable/refuse connection and scan actions. Cross-process connection-preview ownership remains part of the unfinished durable-preview work.
- Writes use a noninteractive D-Bus `Properties.Set` call with bounded process/output limits, followed by strict readback. Success verifies NetworkManager's switch only. Failed writers, malformed/truncated/failed readback and no-op writers cannot report a committed verified change. No checkpoint, automatic rollback or history undo is promised. Enabling may trigger NetworkManager's normal saved-profile auto-connect; connection usability, internet reachability and reboot persistence remain unverified.
- Updated the bundled skill, API reference and README with syntax, acknowledgement, hardware refusal, partial-result handling and readback limits. Extended the guarded integration tool with optional radio changes on private Wi-Fi mocks only, using actual nmcli/busctl and checking denial, no-op writes and preservation of another radio type. The unmanaged real daemon is never used for radio writes; there is no new production mock dependency.

Verification: 198 focused network/API/UI cases and final concurrency guard checks pass. The production Linux CLI passes 52 process checks, including byte-identical skill registration; actual nmcli/busctl pass 61 combined discovery/scan/radio integration assertions. Default discovery-only mode still passes its original 23 assertions, and the GUI command fakes pass four state-consistency checks. The final full Arch/Qt 6.12 suite reports 897 passes and the same known `appearanceShowsPacksAndChoosingOneWorks` keyboard-focus failure; the full suite is not green. Four accounting tests, 33 lifecycle guards, skill validation, translation consistency, documentation and whitespace checks pass.

Evidence is retained under `build/network-radio-tests/`, including `verification.json`, `focused.log`, `guard-focused.log`, `full.log`, `smoke.log`, `integration.json`, coverage and build/source fingerprints. No real user registrations or desktop network settings were changed; real Wi-Fi hardware behavior remains unverified. At this slice the ledger covered 118 GUI methods/88 groups: 38 `api-cli`, 32 `legacy-cli`, eight `ui-only` and 10 `cli-gap`. Durable network previews for a reviewed subset are addressed below; remaining parity and live-session gates stay open.

### Durable CLI network previews

- Added API v1 `network activate <uuid> <interface>`, `network disconnect <interface>`, and separate `network preview status|keep|revert` calls. Starts support `--dry-run` and require connectivity acknowledgement with `--yes`. Activation covers ordinary managed Ethernet and open Wi-Fi profiles; secured Wi-Fi/802.1X has a concrete panel handoff. Generated/volatile/external, unmanaged, composite/VPN/port/controller, secondary and unstable targets are refused. These are explicit configuration limits, not full GUI parity.
- A pinned Qt D-Bus connection binds each token to its bus and daemon owner, device object/interface/UDI/type, baseline, checkpoint identity/timeout and acknowledged active connection path. The 60-second checkpoint is mandatory; there is no unprotected CLI fallback. Public profile and device preconditions are checked again after checkpoint creation before the connection action. The GUI compatibility route retains its existing behavior and is fenced against CLI-owned previews.
- Private durable intent precedes checkpoint creation; writer acknowledgement is separate from target readback. A later status may verify an acknowledged applied request, but it cannot infer success from an interrupted writer. Keep requires user usability confirmation, unchanged owned state and remaining rollback margin. Revert validates every per-device result and observes the baseline before finalizing the owned checkpoint. Missing checkpoints, daemon replacement and incomplete creation records remain uncertain; no automatic retry or successful rollback is invented.
- Receipts/index files enforce owner-only permissions, ownership, link/schema/token validation and bounded enumeration/terminal retention. Network coordinator requests (including typed network settings), GUI network writers and restore leases honor unresolved tokens. The GUI's own local connection path also holds the shared change lock through its preview. Other users/external network tools remain outside these per-user guards. Generic settings history/undo does not settle a network preview.
- Updated the bundled skill and added the [network lifecycle reference](NETWORK-PREVIEWS.md). The reproducible private D-Bus tool exercises production Qt clients across separate processes, process death, incomplete receipts, modified checkpoints, failed per-device rollback, delayed readback, explicit keep, timer expiry and daemon replacement. A separate real unmanaged NetworkManager check validates checkpoint creation/properties, survival of the creating bus peer, and explicit removal; it performs no connection change or real device rollback.

Verification: 217 focused network/API/UI cases pass. The production CLI passes 61 process checks, including structured errors and byte-identical skill registration. All 85 private D-Bus client assertions pass, including real-time 60-second fixture expiry, late keep refusal, bounded retention, process death, post-write readback recovery, stale/altered identities, explicit rollback and actual daemon replacement. Three real unmanaged-daemon protocol checks confirm checkpoint properties, survival of the creating bus peer and explicit removal; no real connection, device rollback or real timer expiry was exercised. The final full Arch/Qt 6.12 suite reports 915 passes and the same known `appearanceShowsPacksAndChoosingOneWorks` keyboard-focus failure; the full suite is not green. Four accounting tests, 33 lifecycle guards, skill validation, translation consistency, documentation and whitespace checks pass.

Evidence is retained under `build/network-preview-tests/`, including `verification.json`, `focused.log`, `full.log`, `smoke.log`, `integration.json`, `checkpoint-protocol.json`, coverage and final build/source fingerprints. No real user skill registration or desktop network setting was changed. The ledger accounts for 118 GUI methods/89 groups: 43 `api-cli`, 32 `legacy-cli`, eight `ui-only` and six `cli-gap`. The four coarse network lifecycle gaps now have reviewed CLI routes and a status group is added. Secured activation, full GUI token adoption/behavior convergence, unidentified creation recovery, driver recovery and real-session usability remain explicit limits/gates. Display discovery/layout, plugin clone/remove recovery, profile cancellation and search-pin CLI gaps remain, as do automatic skill activation and real desktop/model-task gates.

### Initial CLI parity inventory

The implementation must refine this table into an operation-level ledger. A provider method is evidence of existing behavior, not proof that it is suitable for automation unchanged.

| Surface | Current CLI access | Work required |
| --- | --- | --- |
| Typed settings | Generic get/plan/apply/reset | Full schema, choices, target discovery, consistent errors, per-setting parity evidence |
| Audio | API status, plans and verified changes | Device/session verification and end-to-end agent evidence remain |
| Bluetooth | API status, plans and verified known-device/radio changes; pairing handoff | Real controller/pairing-agent and end-to-end evidence remain |
| Networking | API state/devices/profiles/cached APs, scans, Wi-Fi radio and protected Ethernet/open-Wi-Fi previews with durable status/keep/revert; typed DNS/band; panel routes | Secured activation, GUI token adoption/behavior convergence, unidentified creation recovery and real adapter/session verification |
| Displays | Scale/brightness settings; position preview; recovery selftest | Output/mode discovery, complete layout input, persistent preview status/keep/revert |
| Plugins/bar/dock | API registry/live/persisted discovery; planned plugin state, instance moves and dock pins; legacy routes preserved | Clone/remove directory recovery and verification; bulk pin ordering/custom option helper audits; real shell and agent evidence |
| Shortcuts | API effective/owned inventory, scoped checks, replacement plans and writes, fingerprinted removal; legacy routes preserved | Opaque live command references, unreported binding options and keyboard aliases remain explicit limits; real-session and agent evidence |
| Window rules | API constraints, owned inventory, plans/writes and fingerprinted removal; strict legacy indices | Verification establishes persistence/reload, not effective window behavior; real-session and agent evidence |
| Installed apps/MIME | API app discovery/launch and MIME reads/plans/changes; legacy launcher editing | Complete candidate enumeration and real-session/end-to-end evidence remain; package workflows stay explicit |
| Startup/services | API discovery, dry-run plans and verified scoped changes | Real session/authorization and end-to-end agent evidence remain; startup verifies preference only |
| Machine/battery/storage/hooks | Scoped API facts and awaited private diagnostics export | Real hardware/service and agent end-to-end evidence remain; redaction is heuristic |
| Icons/launchers/updates | API icon-pack list, samples and picker search; existing mutation/launcher/update commands | Versioned mutation plans/results and remaining capability boundaries; preserve existing behavior |
| Other Omarchy workflows | Catalog/search/launch | Inspect actual command behavior; promote safe parameterized changes to shared operations, and describe unavoidable handoffs and their verification limits |
| Control preferences/history | History/undo/doctor/compat | Audit persistent user-facing preferences and reset/recovery operations; exclude visual-only interaction with a recorded reason |

## 3. Decisions for the implementation

1. **CLI coverage is a product contract.** New GUI functionality cannot silently create an automation gap. Existing providers stay the owners of behavior; CLI adapters must not reimplement their shell commands.
2. **Discovery is authoritative.** A concise skill teaches agents to query the installed CLI. Do not copy hundreds of settings and volatile choices into skill prose.
3. **Integrate with both skill-selection paths.** Ship a companion `omarchy-control` skill and make the stock `omarchy` selection route to it. A sibling skill alone is insufficient.
4. **Keep Omarchy's packaged skill intact.** Prefer an upstream-supported optional integration reference. Until available, use a reversible per-user forwarding skill, preserving access to the current stock guides. Do not patch `/usr/share/omarchy` or maintain a frozen fork of its instructions.
5. **Automatic integration is the intended installation default.** Provide an explicit opt-out and a repair/status command. An opt-out or user-owned conflict must be visible, not silently counted as successful activation.
6. **Preserve actual outcomes.** Distinguish a verified change, an unchanged value, an opened helper, a pending preview, and a failed or partially recovered operation. Never equate an exit code or a panel launch with the requested setting being changed.
7. **Respect the user's authorization.** Routine reversible actions already requested by the user should not acquire an extra permission ritual. Expose meaningful consequences and use existing confirmation requirements where they apply. `--yes` acknowledges those consequences; it does not authorize unrelated actions or bypass compatibility/ownership checks.

## 4. CLI contract

All command names and additional options in this section are proposals to settle in CLI-1. Existing commands are not a claim that the proposed API already exists.

### Discovery and schema

Extend `catalog` and add focused command/entry descriptions, rather than making the agent read source files. A candidate surface is:

```sh
omarchy-control commands --json --api-version 1
omarchy-control catalog --json --api-version 1
omarchy-control search "make my microphone quieter" --json --api-version 1
omarchy-control describe <setting-or-operation-id> --json --api-version 1
omarchy-control get <setting-id> --json --api-version 1
```

The description must supply enough information to construct a valid call:

- Stable setting/operation IDs, descriptions, synonyms, integration mode, and exact argument/option grammar.
- Input types, units, ranges, patterns, nullable/default semantics, and static or discoverable dynamic choices. Resolve choices against current installed devices/apps, not a list in the skill.
- Supported reads, mutations, reset, undo, dry-run, and preview behavior. A status row is not a writable setting.
- Effective and persisted values where knowable, their source, and the difference between inherited defaults and an explicit override.
- Stable target identity and current target enumeration, including `<setting>@<device>` addressing. Handle missing/ambiguous/stale targets explicitly.
- Capability requirements, availability reasons, display/session/bus/authorization requirements, and whether an interaction is unavoidable.
- Configuration ownership and affected file locations, reload/restart behavior, side effects, expected duration, verification, and recovery class. Distinguish known owned paths from dynamically determined effects.
- The callable next step for helper-only or unsupported cases, without exposing arbitrary executable command strings from untrusted metadata.

Keep metadata in the existing catalogs and a reviewed operation registry for provider commands. Help, descriptions, coverage checks, and skill examples should use those definitions. Runtime capability checks remain fresh; generating static help must not probe or mutate the desktop.

The coverage ledger records whether each operation is a native provider, a reviewed parameterized Omarchy adapter, necessarily interactive, an advanced configuration boundary, unsupported, excluded, or a missing CLI implementation. Include the reason and verification evidence. Do not assign “necessarily interactive” solely because the current catalog calls it a wizard. An advanced file route is a useful location reference, not a claim to support every possible expression or key in that file.

### Structured output and compatibility

Define a versioned JSON envelope for the new API, including application/API versions, operation, outcome, data, structured error, transaction/reference IDs, verification state, warnings, and any next step. Use stable error codes for invalid input, unavailable capability, interaction/authorization needed, conflict, timeout, compatibility block, and recovery failure.

- In versioned JSON mode, stdout contains exactly one parseable response for successes, validation failures, missing arguments, and unknown commands. Human diagnostics belong on stderr. Long operations may support a separately specified progress channel later.
- Keep existing 0/1 exit behavior unless a documented migration changes it. An accepted pending preview or opened helper must state that it is incomplete even if the request exits 0.
- Preserve current unversioned scripts while introducing an explicit API-version selection. Record a compatibility policy; do not silently replace existing JSON arrays/fields with a new envelope.
- Reject unknown options, conflicting options, extra positional arguments, missing option values, and failed numeric conversions before mutation. `--help` and discovery must never execute an operation.
- Return the actual normalized value and confirmed state. Distinguish transient runtime objects from persistent preferences. Reject retries that would duplicate a pending operation.
- Bound operation time and output size. On timeout or interruption, report what is known and how to inspect/recover; a timeout does not prove that a backend made no change.

### Planning, changes, and recovery

Typed settings retain `plan`/`apply`. Provider mutations gain an equivalent planning path, using the same validators and effect metadata as execution. Choose consistent grammar in CLI-1, such as operation-specific `--dry-run`; avoid a generic arbitrary-command runner.

Planning does not change configuration, restart processes, install software, or create a preview. Execution refreshes relevant preconditions, checks ownership/compatibility, serializes with the GUI, journals the operation where applicable, and verifies the result. Describe whether an operation can be undone and implement the shared recovery policy before advertising it.

Display and network previews need a result that survives the originating CLI process: a user/session-scoped token, expiry, inspectable state, and separate status/keep/revert calls. Retain the detached display watchdog and NetworkManager checkpoint protections. Test process death, expiry, a stale token, changed hardware, and inability to create a checkpoint. Do not let an agent's immediate keep substitute for the user's confirmation that connectivity or a display is usable.

Use guarded structured input for related edits such as a complete display layout. An arbitrary multi-domain transaction engine is outside the first implementation; do not imply atomic rollback across unrelated providers.

Authentication, Bluetooth pairing, and secured-network enrollment stay with the owning agents/helpers unless a supported noninteractive interface already exists. Never accept passwords in command arguments or persist them in plans/history. A headless CLI may still require the user's desktop services; report that distinction clearly.

## 5. Skill contents and routing

### Companion skill

Expand `src/data/skill.md` into a maintained skill bundle with a short entry point and only the references needed for substantial special cases. Package a canonical copy under `/usr/share/omarchy-control/agent-skills/omarchy-control/`; keep the bundled/exported copy consistent for supported installation methods.

The skill should teach this decision flow:

1. Check the installed CLI/API and find the setting or operation using discovery. Resolve ambiguity from current choices and the user's intent.
2. Inspect current state, valid targets/values, effects, availability, and whether the operation is native or a handoff.
3. Plan the change. Apply within the user's authorized request, acknowledging only the consequences actually required by that operation.
4. Inspect the structured outcome and verified state. Report what changed, what remains pending, and any restart or user interaction needed.
5. Use transaction history/undo where supported. If a write fails, inspect its result rather than switching to raw files to defeat a refusal.

Read-only discovery and existing authorization should keep ordinary tasks short. Do not require a full `doctor` scan or repeat permission questions before every volume, font, or timeout change.

Use focused references for API/result semantics, display/network previews and handoffs, and configuration ownership. Include the main/blur module load order, managed-file hand-edit behavior, icon consumers/hooks, and user versus package-owned locations. Query current paths and choices from the CLI whenever possible.

An unsupported task can return to the stock Omarchy instructions for a supported command or deliberate advanced edit. Explain that this is outside Control's managed operation. A compatibility block, ownership conflict, or failed verification must not trigger that fallback automatically.

### Bridge from the stock `omarchy` skill

Preferred long-term integration: propose a small optional routing reference upstream. If Control is present and compatible, the stock skill directs the agent to the installed Control skill/CLI; otherwise its normal guidance remains usable. Upstream acceptance is tracked separately and must not block a working local integration.

For stock versions without that extension mechanism:

- Generate a per-user forwarding `omarchy/SKILL.md` under an application-owned data directory. It contains enough Control routing guidance to work even when the harness has loaded no sibling skills.
- Link to the current stock skill and topic guides through an `upstream` reference. Preserve their relative references. Resolve the installed `$OMARCHY_PATH`, including supported development links, rather than copying the entire stock skill.
- Register the forwarding skill at discovery locations that previously referred to the stock Omarchy skill, and register the companion skill for direct selection.
- Automatically replace only a verified stock Omarchy symlink or a known unmodified Control-owned entry. Preserve custom directories, unrelated links, and user modifications; return a conflict and a concrete integration plan.
- Record original targets and fingerprints. Removal restores them only when Control still owns the current entry; it must preserve edits made since registration.
- If Control is missing, the forwarding skill must still lead to usable stock Omarchy guidance. Package removal must not leave the primary `omarchy` skill pointing only at a deleted file.

Do not alter `omarchy-app` or `diagnose-crash` merely to steer ordinary settings work. Detect an upstream routing integration and avoid stacking another wrapper over it.

### Harness support

Build a versioned support matrix based on actual harness discovery, not just directories that can be created. Cover Omarchy's generic `~/.agents/skills` path and its Claude, Codex, Pi, Antigravity, and Hermes/profile integrations, verifying the supported versions and paths during implementation. Account for documented custom-home/environment overrides; provide explicit target registration where automatic discovery cannot know them.

Deduplicate paths that resolve to the same directory. Avoid advertising duplicate copies where a harness reads several skill locations. A launcher's model name is not proof of skill loading: test the agent harness that runs it. Other Omarchy agents are either verified through these paths or listed as unsupported/pending with a reason.

Proposed lifecycle commands: `skill status`, `skill plan`, `skill install`, `skill reconcile`, and `skill remove`, with structured output and per-target results. Status distinguishes registered, conflicting, stale, opted-out, unsupported, and refresh-required states. Preserve the existing `skill install` entry point.

## 6. Installation, update, and removal

Package payload and per-user registration are separate responsibilities. The root package transaction installs canonical resources and lifecycle helpers; user-home changes run in the actual user's context. Never use root's `HOME`, guess a desktop owner from a directory name, or write every user's skills from an unrestricted root loop.

Automatic registration is an intended behavior change. Update the installer/README/package expectations to explain that Control adds reversible agent integration, with an opt-out. Do not retain the old blanket claim that installation changes no user configuration.

| Lifecycle | Required behavior and evidence |
| --- | --- |
| Curl installation by a desktop user | After successful pacman installation, register for that invoking user; report active/conflict/opt-out state. A root invocation must not register for root and claim the desktop user is ready. |
| Direct package/repository installation | Prove a session-owned activation mechanism for already logged-in users and next-login activation for inactive users. Prototype a user-service/session helper before choosing package hooks. First GUI launch is a repair path, not the only installation mechanism. |
| Control upgrade | Atomically update canonical resources; reconcile owned registrations and check binary/skill API compatibility. Preserve opt-outs and user changes. |
| Omarchy upgrade or development relink | Account for Omarchy re-pointing its skill symlinks. Evaluate the supported per-user `post-update.d` hook and session-start reconciliation; preserve current upstream guides and avoid wrapper loops. |
| Another agent/profile added later | Reconcile verified discovery targets on supported setup/session events and explicit repair. Do not create credentials or sign the user into an agent. |
| Removal | Restore owned stock-skill registrations, remove only owned companion links/hooks, preserve user content, and keep stock guidance reachable for inactive users too. Prove how deferred cleanup works when their session is absent. |
| Downgrade, interruption, conflict | Detect incompatible skill/API versions, retain usable stock guidance, and expose a repair action without replacing someone else's files. |

CLI-0 must resolve immediate activation for direct package installs and cleanup for inactive users with an Arch session prototype. Until those are proven, label them pending and do not advertise “install and every agent is ready.” Merely placing a skill file on disk or adding an autostart entry does not satisfy the already-logged-in-user gate.

Registration needs atomic writes, a lock, ownership/fingerprint checks, and a per-user manifest under Control's XDG state directory. Package updates cannot reset an opt-out. Symlink targets and registration paths must be checked before replacement; a path existing is not proof that Control owns it.

Extend both package recipes, `install.sh`, the updater path, and package/installer checks as needed. Existing `--noscriptlet` package checks can remain useful for payload verification, but a separate real lifecycle test must execute activation/removal behavior and use realistic users/session services.

## 7. Phases and exit gates

The completion pass closes all six remaining coarse CLI gaps, adds reset/undo/history API responses and audits the legacy contracts. The CLI-2 follow-up promotes background introductions, Power widget percentage and per-program crash muting. All 344 script destinations now identify native alternatives or specific handoff reasons; the independent check rejects missing reviews. The workflow audit records compound lifecycle, content, productivity, administration and interaction boundaries explicitly. CLI-3 has durable display/network lifecycles and GUI token adoption; physical recovery remains an acceptance gate, and GUI-started legacy network connections retain a documented compatibility path. CLI-4 has stock forwarding, 16 real discovery scenarios on macOS and Linux, and five passing stock-only Codex model tasks. Pi/Claude model execution and other unverified harnesses carry no readiness claim. CLI-5 packages activation with 38 production-recipe checks, six real pacman transactions, real Arch ARM user managers, graphical target ordering and installer activation reporting. CLI-6 has the helper follow-up evidence recorded in the readiness report; actual Omarchy desktop acceptance and publication remain open.

| Phase | Deliverable | Exit gate |
| --- | --- | --- |
| CLI-0: inventory and integration prototypes | Operation-level CLI coverage ledger; harness support matrix; stock-skill bridge and Arch lifecycle prototypes in disposable environments | Every current GUI operation/catalog route is classified; exact provider gaps are recorded; direct-install activation and inactive-user cleanup have a tested design or explicit unresolved dependency |
| CLI-1: discovery and stable API | Shared operation descriptions, complete setting schemas/choices/targets, versioned responses, strict parsing, compatibility policy | An agent can construct valid calls using CLI output alone; all versioned JSON success/error paths parse; discovery does not mutate desktop/skill configuration; existing scripts retain documented behavior |
| CLI-2: ordinary operations | Audio, known Bluetooth devices, plugins/bar/dock, shortcuts/rules, apps/MIME, startup/services, facts/diagnostics; typed-setting closure; reviewed parameterized adapters for routine helper-only changes | Each supported GUI operation and automatable routine workflow has a CLI read/plan/action path, meaningful readback, and declared recovery/interaction behavior; residual gaps have specific reasons |
| CLI-3: previews and interactive boundaries | Full display layout and network lifecycle APIs; durable status/keep/revert; explicit helper-only operations | Process-death/timeout/revert cases pass; a preview is never confused with a committed change; pairing/secrets/privileged workflows do not hang unattended or claim unverified completion |
| CLI-4: skills and registration | Companion skill, stock-skill bridge, harness registration, status/repair/remove, ownership manifest | Loading only `omarchy` leads to Control discovery; real harness tests prove the promised discovery paths; custom skills survive; upstream docs remain reachable |
| CLI-5: installation lifecycle | Both package payloads, installer/updater integration, per-user activation, upgrade/relink/removal handling | Fresh install, direct package install, upgrade, Omarchy update/relink, multi-user, opt-out, conflict, and removal scenarios pass in a disposable Arch desktop environment |
| CLI-6: end-to-end release evidence | Natural-language task evaluation, generated CLI coverage report, public docs and release checklist | Zero unclassified operations in the supported scope; no unreported native CLI gaps; agents complete representative tasks without supplied IDs; remaining interaction and harness limitations are published |

CLI-0's package/harness prototypes and the CLI inventory can proceed independently. CLI-1's foundational routing/result contract precedes provider expansion; richer discovery continues alongside provider work. CLI-4's final instructions depend on the stable contract and preview semantics. Ship the automatic integration claim only after CLI-5 and CLI-6. Do not infer delivery dates from phase numbering.

### First implementation slice

Start with CLI-0 and the discovery portion of CLI-1:

1. Create the machine-readable operation ledger, adding every provider action, the true automation/interaction requirements of existing helper routes, and a reason for UI-only exclusions. Keep it distinct from Omarchy command/menu route coverage.
2. Design and implement descriptions for typed settings and provider command grammar, including dynamic choices and unsupported reasons.
3. Add versioned JSON error handling and strict parsing for the existing get/plan/apply path; demonstrate one valid change, one invalid value, and one ownership/compatibility refusal.
4. Prototype registration and stock-only skill routing with temporary homes, then validate the package/session lifecycle on a disposable Arch desktop. Freeze the activation design before changing shipped package behavior.

This slice should produce reviewable discovery output and a tested installation design before broadening mutations or auto-registering anything on a user's machine.

## 8. Verification and ongoing coverage

### Automated checks

- Derive operation inventory from catalog/provider definitions and assert each entry has a reviewed CLI mapping or a precise exclusion. Reconcile a curated GUI action inventory too: deriving both sides solely from the CLI registry would miss GUI-only features.
- Test versioned JSON content and real resulting state, not only return codes or exact help wording. Cover errors, dynamic choices, targeted settings, stable identifiers, invalid flags/numbers, absent tools/hardware, denied authorization, and disconnected sessions.
- Use fixture commands and temporary homes to prove that discovery/planning do not mutate settings, that CLI/GUI share validation, and that concurrent changes or external edits are not overwritten.
- Exercise existing journal/undo/recovery behavior through the actual CLI process. Cover asynchronous timeout, partial failure, process death, preview expiry, and unknown verification state.
- Test registration ownership with stock symlinks, pre-existing custom skills, edited generated entries, duplicate discovery paths, custom homes, multiple users/profiles, read-only directories, opt-out, repeated installation, interrupted writes, upstream relinking, upgrade/downgrade, and removal.
- Validate skill frontmatter/references and execute documented examples against fixtures. Generate/validate command references from the operation contract so examples cannot quietly drift.
- Extend real package tests to verify canonical resources, per-user activation, root-versus-user ownership, session timing, and cleanup. A file-presence assertion is insufficient.

### Real-session and agent checks

Use a disposable Omarchy 4.5 desktop for hardware/session-bound checks; record versions and outcomes separately from local fixture results. Verify actual effective state and recovery. The macOS workspace alone cannot prove these gates.

Start fresh supported harness sessions without giving them Control command examples or setting IDs. Include tasks such as:

- “Lock my screen after ten minutes.”
- “Make my microphone quieter and use my headset for sound.”
- “Put the clock on the right and pin this app to the dock.”
- “Open PDF files with this installed application.”
- “Disable this startup app and restart my user service.”
- “Put the second screen on the left,” including rejection/timeout recovery.
- “Switch to this saved network,” including lost connectivity and checkpoint refusal.
- “Undo the change you just made.”
- A task requiring a pairing prompt, an unavailable device, a custom skill conflict, and a compatibility-blocked write.

Run the routing cases with only the stock-named `omarchy` skill initially selected. Verify actual tool calls and resulting state, including truthful helper/pending outcomes and absence of raw-file bypass after a refusal. Record discovery, valid selection, execution, verification, and explanation separately; merely mentioning Control does not pass.

Include different model families where available, but distinguish model behavior from harness file discovery. Tests need not expose credentials or alter the user's real desktop. Live account-dependent evaluations require an available authorized test harness; their absence is an unfinished verification gate.

### Release criteria

- [x] Every supported GUI method and catalog destination has a classified agent route.
- [x] Native CLI gaps are closed; external workflow and interaction boundaries are explicit.
- [x] Versioned JSON and documented legacy compatibility checks pass.
- [x] Stock-only Codex model turns route through Control; supported Pi/Claude paths have metadata-discovery evidence.
- [x] Active/inactive-user activation, opt-out, conflicts, removal and same-API downgrade pass real Arch ARM package/user-manager checks.
- [x] Fixture relinks retain current stock instructions and preserve ownership.
- [x] Coverage, support matrix and public docs distinguish local evidence from acceptance gates.
- [ ] Run the new integration and representative device tasks on an actual disposable Omarchy desktop.
- [ ] Verify physical display usability and actual network-driver recovery; complete the pre-existing fresh-machine profile restore acceptance.
- [ ] Review and publish the release, then update the external recipe to its published source/checksum.

## 9. Source references and implementation touchpoints

Local sources: [CLI](../src/cli.cpp), [shared core](../src/appcore.h), [catalog](../src/catalog.h), [settings engine](../src/settingsengine.h), [GUI backend](../src/backend.h), [change coordinator](../src/changecoordinator.h), [bundled skill](../src/data/skill.md), [package checks](../tools/package-check.sh), [installer](../install.sh), and the provider headers under `src/`.

Upstream baseline: [stock Omarchy skill](https://github.com/omacom/omarchy/blob/v4.5.0/default/agents/skills/omarchy/SKILL.md), [user provisioning and skill links](https://github.com/omacom/omarchy/blob/v4.5.0/bin/omarchy-provision-user), and [user hook support](https://github.com/omacom/omarchy/blob/v4.5.0/default/agents/skills/omarchy/hooks.md). Reverify installed versions and harness discovery behavior during CLI-0; these links establish the audited baseline, not a promise about future releases.

### Completion pass: ordinary gaps and durable displays

The GUI inventory now maps 118 methods into 90 reviewed operation groups: 58
`api-cli`, 24 `legacy-cli`, eight `ui-only`, and zero coarse `cli-gap` groups.
Legacy contracts, catalog destination scope, activation and actual harness tasks
are now audited/tested as recorded in [the readiness report](CLI-AGENT-READINESS.md).
Physical desktop acceptance and publication remain separate release gates.

- `display status` awaits strict monitor/version discovery. Full JSON layouts,
  recovery checks and persistent preview status/keep/revert are callable through
  API v1. The detached owner saves Keep, captures guarded persistence recovery,
  and verifies reported restoration; another CLI process or GUI can adopt its token.
- Plugin clone/removal share GUI/CLI planning, private directory/Git/symlink capture,
  noninteractive helpers and registry/file/live readback. Explicit `plugin recover`
  guards post-write state, preserves displaced content and reloads the shell.
- Search pins have an idempotent shared preference writer. Restore cancellation
  uses a private request consumed between operations; acceptance is not completion.
- The existing icon-picker focus failure is fixed by retaining navigation's focus
  request until choices load. Selecting a just-imported pack plans against current
  installed descriptors rather than an unfinished asynchronous choice refresh.

Evidence for this pass is under `build/cli-completion/`. The full Qt suite passes
938 cases. Display and plugin checks include real detached processes against
fixture services, and package activation has separate production-recipe evidence.
These results do not establish physical display usability or real-shell plugin
health. Final verification details and remaining acceptance steps are in the
readiness report.

### Helper audit follow-up

The [workflow review](CLI-WORKFLOWS.md) distinguishes routine native preferences
from remaining upstream lifecycle/content/administration handoffs. Background
introductions use explicit on/off operations and guarded empty-marker recovery;
Power percentage uses an exact widget selector and explicit JSON boolean;
per-program crash muting has bounded discovery, reads, plans and idempotent sets.
Search/describe/coverage expose native alternatives, handoff reasons and known
file locations. The forwarding and companion skills explain how to use them.

The inventory now has 118 GUI methods, 92 operation groups (60 API v1, 24 legacy,
eight UI-only), 90 typed entries and 425 destinations. The full Arch ARM/Qt 6.12
suite passes 966 cases. A final rebuilt parser/helper/updater follow-up passes 43 cases;
38 production CLI checks per architecture use eight pinned Omarchy helpers with simulated IPC.
The current source-built Arch ARM package passes 38 checks and six real pacman
transactions. All 40 Python lifecycle/accounting checks pass. Current evidence
is under `build/cli-workflows/`; earlier `cli-completion` artifacts remain the
baseline rather than evidence for these newly added routes. Physical desktop
acceptance and publication remain open.

The release follow-up fixes Qt's `arm64` architecture alias to select Arch's
`aarch64` package filenames. Release fixtures use the actual packaging spelling;
negative origin/checksum/name checks remain intact. Both final package binaries
are checked directly, including natural search terms and implicit widget-option
API selection. Local release files/checksums remain unpublished.

### Release version preparation

GitHub already publishes v0.5.0, which predates this CLI/skill integration. The
next source release is 0.6.0; application, test/screenshot builds and the primary
package recipe agree. The external recipe stays pinned to its published source
until a real 0.6.0 tarball and checksum exist. Package lifecycle QA derives its
version from the chosen source recipe, so upgrading the application version does
not silently keep testing a hard-coded old package. Physical acceptance and
release publication are still open gates.

The 0.6.0 verification pass has a clean 966-test Arch ARM/Qt 6.12 suite and
41 Python checks. Both final package binaries pass 39 helper/discovery checks,
including their reported 0.6.0 version and an explicit blocked search-pin error.
Four focused tests cover that final error-normalization correction. The packaged
CLI/watchdog passes 34 display checks and 92 private NetworkManager checks with
real expiry. The primary 0.6.0 recipe passes 38 lifecycle checks/six pacman
transactions. A checksum-verified published 0.5.0 package installs/runs and
upgrades to 0.6.0 in the separate 14-check package-root test; hooks are masked
there, with activation covered separately. The curl installer passes nine checks.
Artifacts are under `build/cli-release-0.6.0/`; publication and physical acceptance
remain open. Source review proceeds through a draft PR rather than a release tag.
