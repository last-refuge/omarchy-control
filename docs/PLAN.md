# Omarchy Control — implementation plan

Status: P0–P5 implemented, with 60 of 76 writable settings, display recovery, and the real window verified on real Hyprland and the real shell in an isolated nested session. No write has been made on a real desktop session yet. See [LIVE-CHECKS.md](LIVE-CHECKS.md). Baseline: local machine, November 5, 2026.

## 1. Product direction

Build **Omarchy Control**, a native desktop application that becomes the main entry point for configuring and maintaining an Omarchy machine. Its job is to help someone find a setting, understand its current state, make a change, and verify the result. Specialist workflows remain accessible through clearly named helper launches.

The Windows Control Panel analogy describes the breadth and central entry point. The interface, terminology, keyboard behavior, packaging, and appearance should belong to Omarchy.

The working executable, package, desktop entry, icon name, and Wayland application ID are `omarchy-control`. The working directory is this workspace. `omarchy-settings` is already an installed package for defaults and support files, so that name should not be reused.

### What comprehensive means

Every supported user-facing Omarchy configuration surface must have a searchable destination and one of these outcomes:

1. **An integrated editor**, with current values, validation, application, and verification.
2. **An existing Omarchy panel or wizard**, opened at the appropriate destination.
3. **A specialist application**, launched with its availability and purpose explained.
4. **An advanced configuration entry**, identifying the relevant file, documentation, ownership, and reload procedure.

A missing implementation is an explicit backlog item, not a hidden omission. Missing hardware is a distinct state from an unsupported feature. An internal utility or developer-only operation can be excluded with a recorded reason.

Coverage is measured against settings and workflows, not the number of command buttons. One command can expose many settings; some commands expose none. Native-editing coverage and helper-based coverage are reported separately.

The release scope includes Omarchy's stock configuration, its shipped desktop services, supported setup and maintenance workflows, installed shell extensions, and common machine administration. Arbitrary application preferences and custom Lua programs can have advanced or helper access without a fabricated graphical editor. Filesystem snapshots are not a replacement for personal-file backups.

## 2. Local evidence and its implications

The [discovery inventory](inventory-2026-11-05.json) records the source data and its limitations.

| Observed locally | Consequence for this application |
| --- | --- |
| Omarchy and `omarchy-settings` 4.5.0-3; Hyprland 0.56.2-4 | Start with a tested 4.5 compatibility baseline. Do not assume older Omarchy config formats. |
| Quickshell 0.3.2.r7.g2b4501a-1; Qt 6.12 packages; qmake6, make, and g++ available | A native Qt Quick application fits the installed platform and build tools. |
| 433 public CLI entries across 74 groups; 565 entries including 132 hidden commands | Use the CLI catalog for discovery and drift detection, followed by curated adapters. Discovery does not authorize execution. |
| 425 static stock menu nodes, including submenus and dynamic providers | The menu supplies additional task vocabulary and workflows that a command list misses. Runtime provider rows also need coverage. |
| 40 packaged plugin/widget manifests; 13 user plugin/widget manifests | Include bar widgets as well as directory-level plugins. Preserve user clones and selected implementations. |
| Hyprland uses `hyprland.lua`, `monitors.lua`, `input.lua`, `bindings.lua`, `looknfeel.lua`, and `autostart.lua` | A legacy `hyprland.conf` editor would target the wrong configuration. Lua requires explicit ownership boundaries. |
| User configuration includes an additional Lua module and custom shell implementations | Treat externally authored logic as authoritative. Detect overrides instead of rewriting whole files. |
| Shell settings use `~/.config/omarchy/shell.json`; style overrides use `shell.toml` | Layout, plugin configuration, and visual style are related but distinct data sources. |
| Network, Bluetooth, audio, display, and power panels already exist | Reuse these immediately, and add integrated controls selectively. |
| Printer settings, GNOME Disks, nmtui, Fcitx settings, and Superwhisper are installed | A useful first release can expose working specialist workflows without adding dependencies. |
| Common alternatives such as pavucontrol, Blueman, and nwg-displays are absent | Never assume a familiar Linux helper exists. Detect it or use the installed Omarchy route. |
| Omarchy's locale registry contains 50 languages | Build translation, live language changes, number formatting, and RTL layout into the foundation. |

The audit inspected metadata and source files. It did not launch helpers, test their behavior, enumerate credentials, or exercise configuration changes. A binary being installed is not proof that its service, hardware, or operation works.

## 3. Experience and navigation

Use one normal Wayland application window, with a compact navigation column, persistent search, and a main content area. Settings pages use readable rows, concise descriptions, and grouped controls. Device pages can use diagrams where placement matters, such as display arrangement.

| Navigation destination | Main contents |
| --- | --- |
| Overview | Machine and Omarchy version, important problems, pending restart requirements, pinned settings, recent changes |
| Appearance | Theme, background, fonts, text size, window appearance, branding |
| Desktop | Bar, dock, widgets, plugins, workspaces, shortcuts, notifications |
| Displays | Monitor layout, modes, scaling, brightness, night light, screen sharing |
| Input & accessibility | Keyboard, pointer, touchpad, gestures, input methods, dictation, accessibility tools |
| Sound & devices | Output/input, application audio, Bluetooth, printers, cameras, other peripherals |
| Network & sharing | Wi-Fi, Ethernet, DNS, VPN, captive portals, Tailscale, local sharing |
| Power & session | Power profiles, battery, idle, lock, screensaver, suspend, hibernation |
| Apps & defaults | Installed apps, supported installs/removals, file associations, web apps, default tools |
| Accounts & security | User password, authentication devices, SSH, firewall, privacy-related settings |
| System & maintenance | Updates, storage, snapshots, startup/services, language/time, firmware, kernel/boot |
| Advanced | Config files, hooks, developer tools, diagnostics, compatibility, change history and recovery |

Cross-link settings that belong to several tasks. For example, text size appears in Appearance and is discoverable from Accessibility; microphone privacy links to Sound. Both links open the same setting and backend.

Search indexes setting names, descriptions, synonyms, device types, helper names, and advanced config filenames. Queries such as “stop sleeping,” “make text bigger,” “change microphone,” and “start an app at login” should land on the relevant control. Search does not execute actions. Unknown command metadata never becomes an executable search result automatically.

### Interaction rules

- Show the current effective value, loading/unknown state, and any reason editing is unavailable. Do not show a cached value as confirmed current state.
- Apply a low-impact toggle immediately when its operation is reliable and reversible. Stage related form fields behind a page-level Apply button. Avoid a global pending-changes queue in the first release.
- Label handoffs by destination: “Open Omarchy network panel,” “Open Printer Settings,” or “Run fingerprint setup.” Show “Opened” separately from “Changed successfully.”
- Refresh state after a helper closes or the control window regains focus. Query actual state to establish success.
- Preserve unfinished form edits when external state changes; offer reload or reconciliation rather than silently replacing the form.
- Keep technical file paths and command details in an expandable details area. Surface restart requirements, authentication, and real consequences beside the action.
- Use explicit confirmation for destructive resets, removals, loss of connectivity, or other consequential operations. Ordinary reversible changes should not require repeated dialogs.
- Provide Ctrl+K/Ctrl+F for search, Alt+Left for back, `?` for shortcuts, and Q/Ctrl+Q for quit. Single-letter shortcuts are inactive while typing. Space activates the focused control. Ctrl+Z/redo apply only when an undoable edit or transaction exists.
- Support complete keyboard navigation, visible focus, screen-reader names, larger text, and RTL. Do not communicate state through color alone.
- Keep optional hardware findable with a “No device detected” explanation; the default view can prioritize relevant controls.

### Example journeys

**Find and change text size:** search “bigger text,” view the current size, choose a value, apply through the Omarchy text-size adapter, and show which applications need reopening. Verify each affected setting; this command changes more than one file.

**Fix a microphone:** open Sound, inspect the available input and mute state, select an input, verify the active default, and open the existing audio panel for additional controls.

**Arrange monitors:** select detected outputs and supported modes, stage layout changes, preview under a recovery timer, and keep the new layout explicitly. An unconfirmed preview restores the previous configuration.

**Set up fingerprint login:** show support and enrollment status where available, explain the authentication contexts affected, launch Omarchy's existing setup flow, and refresh status on return.

## 4. Make it look and feel distributed with Omarchy

Use **C++ with Qt 6 Quick/QML**, built with **qmake6 and make**, following the local Omarchy app conventions. C++ owns state, filesystem work, services, and processes. QML owns layout and interaction. Use the Omarchy UI Qt Quick kit for the live palette, tokens, and shared components, with Qt Quick Controls in the Basic style for native input behavior. [Upstream integration findings](OMARCHY-UI-UPSTREAM.md)

The design must follow more than the accent color:

- Read the active palette from `~/.local/state/omarchy/current/theme/colors.toml`, including background, foreground, selection, and light/dark mode.
- Follow the relevant typography, spacing, and interaction tokens in the active theme's `shell.toml`, with user overrides from `~/.config/omarchy/shell.toml`. Implement and document the subset supported by the app.
- Follow the selected Omarchy font and effective Hyprland corner rounding. Support both sharp and rounded configurations. Match light themes as well as dark themes.
- Respect disabled animations; otherwise use short, restrained transitions and the existing shell's general timing conventions.
- Re-arm file watchers after atomic replacement and theme-directory changes. A theme or language change should update the open window.
- Keep palette parsing and the theme bridge independent of the running shell. Do not import `qs.*` modules into a normal Qt application or assume they form a stable public library.
- Use a small, consistent bundled icon set with proper attribution and accessible labels. Keep branding restrained and avoid claiming official distribution until accepted upstream.
- Follow `~/.config/omarchy/language`; translate whole strings with placeholders, support locale-aware numbers/dates and RTL, and fall back to English per string. Track actual translation coverage separately from language-switch support.

Use the installed shell's `Commons/Color.qml`, `Commons/Style.qml`, and UI components as local design references. The Omarchy app skill and [Hype's source project](https://github.com/omacom/hype) provide native app references; runtime integration must be verified against the installed version.

Package it as an Arch package with a desktop entry, icon, license, version, and only the Qt/system dependencies it uses. Multimedia dependencies from other app templates are unnecessary unless a feature needs them. Launcher keywords should include Settings, Control Panel, and Preferences. Add an Omarchy menu entry through the user extension mechanism, preserving existing entries. Global keybindings are optional and require collision detection.

A separate application process lets the control center remain available when the shell is restarted or unavailable. No always-running background daemon is needed for routine settings browsing.

## 5. Architecture

The main flow is:

`QML pages → typed settings model → provider → change coordinator → existing command/service or owned config patch → verification`

Use a small set of explicit components:

| Component | Responsibility |
| --- | --- |
| Capability scanner | Detect installed versions, command routes, tools, service interfaces, device support, and config format without making changes |
| Settings catalog | Stable setting IDs, translated labels, search terms, destination, type, supported values, provider, and coverage status |
| Providers | Discover/read/validate/plan/apply/verify and, when supported, undo one domain's operations |
| Process and service layer | Asynchronous QProcess/D-Bus access, validated arguments, output bounds, timeouts, permission handling, and cancellation rules |
| Change coordinator | Capture preconditions, serialize conflicting writes, journal operations, apply changes, verify effective state, and recover |
| Helper launcher | Resolve supported panels, desktop entries, and terminal workflows; track launch versus outcome |
| Theme and translation bridge | Live visual tokens, locale, layout direction, and safe fallback behavior |
| Search and history models | Fast local search, pinned destinations, redacted change history, and eligible undo operations |

### Provider contract

Each setting/action declares:

- Stable ID and data type; range/choices; whether its default is known or inherited.
- Capability requirements, supported versions, and its state reader.
- Effective value and, when knowable, persisted value and source. Use “custom configuration” when Lua prevents reliable provenance.
- Integration type: native control, Omarchy panel/wizard, external helper, advanced file access, or documented exclusion.
- Apply mechanism and complete side effects: files, services, package operations, session impact, and possible app launches.
- Scope: per-user, runtime-only, device-specific, or system-wide.
- Validation, verification, privilege path, timeout, cancellation policy, and restart requirement.
- Recovery class: automatically reversible, best-effort restoration, or no automatic undo.
- Compatibility evidence and meaningful tests.

Use built-in, reviewed providers initially. A declarative setting registry supplies metadata; it is not an arbitrary shell-command plugin system. A versioned provider SDK can follow once real integrations establish the contract.

### Integration order

1. **Use a supported Omarchy command** when it owns the behavior and preserves Omarchy's persistence/hooks.
2. **Use an existing service interface** for structured state and operations where appropriate, including NetworkManager, BlueZ, systemd, UPower, and portal APIs.
3. **Patch an owned user configuration surface** when there is no suitable command or service API.
4. **Launch the existing workflow** when it is complex, interactive, privileged, or not yet safely integrated.

Implementers must read each command's source/help. Names and metadata alone are insufficient. For example, `omarchy default agent` can install and launch a tool; it is not a harmless setter. `omarchy toggle bar on` hides the bar. The adapter must translate backend semantics into clear user-facing state.

Prefer `omarchy <group> <action>` routes. Invoke processes with an executable and argument vector; do not concatenate user values into shell commands. Some Omarchy terminal wrappers deliberately interpret a command string: restrict their use to reviewed static invocations, or use an application-owned terminal runner that receives validated operation IDs and arguments. Do not execute copied menu `action`, `when`, or `checked` strings during catalog discovery.

Signals and file watchers refresh state. Poll only when a backend has no event interface, and only while relevant. Deduplicate scans and avoid repeatedly spawning large command sets while the user types. Backend operations must never block the GUI thread.

### Proposed project shape

```text
omarchy-control.pro
bin/build
bin/test
bin/install
src/main.cpp
src/backend.h / backend.cpp
src/catalog.h / catalog.cpp
src/capabilities.h / capabilities.cpp
src/provider.h
src/theme.h / theme.cpp
src/translator.h / translator.cpp
src/commandrunner.h / commandrunner.cpp
src/changecoordinator.h / changecoordinator.cpp
src/helperlauncher.h / helperlauncher.cpp
src/<domain>provider.h / <domain>provider.cpp
src/Main.qml
src/<PageOrComponent>.qml
src/resources.qrc
src/i18n/<language>.json
tests/omarchy-control_tests.pro
tests/<behavior>_tests.cpp
tests/fixtures/
pkgbuild/PKGBUILD
pkgbuild/omarchy-control.desktop
pkgbuild/omarchy-control.svg
docs/README.md
docs/PLAN.md
docs/COVERAGE.md
docs/inventory-2026-11-05.json
branding/omarchy-control-overview.png
branding/overview-prompt.txt
```

Keep source files flat initially, as in the Omarchy app conventions. Use fixtures and injected filesystem/process/service interfaces so tests never touch the real desktop.

Expose the same reviewed backend through a proposed headless CLI: `catalog`, `get`, `plan`, `apply`, `history`, `undo`, and `doctor`, with `--json`. These are proposed `omarchy-control` subcommands, not existing Omarchy commands. `apply` rechecks plan preconditions and permissions; GUI and CLI share validation. Bundle a short agent skill through `omarchy-control skill install`. Keep GUI startup behind `omarchy-control open`.

## 6. Configuration ownership and recovery

Omarchy's files and services remain the source of truth. The application stores its preferences under the XDG config directory for `omarchy-control`, cache under its XDG cache directory, and restricted history/backups under its XDG state directory. It does not create a competing database of system settings.

### File editing

- Never write packaged sources under `/usr/share/omarchy/`. Use supported commands, user overrides, or appropriate privileged service interfaces.
- Preserve unrelated keys and comments. Use format-aware edits for JSON, JSONC, TOML, and simple supported formats. Do not apply generic regular-expression rewrites to arbitrary configuration.
- For Lua, prefer existing Omarchy setters. For missing controls, the P0 prototype must establish a safe generated module, such as `~/.config/hypr/omarchy-control.lua`, and a single clearly marked user-config import. Confirm its ordering relative to defaults, toggles, and custom imports. Never silently append a competing authoritative configuration layer.
- Store only explicitly selected overrides in that module. “Use default” removes the owned override and re-reads the inherited result. It does not regenerate the user's whole configuration.
- If custom Lua, symlinks, or another manager make editing ambiguous, show the effective value and offer advanced access or an explicit ownership/override decision. Never execute the user's Lua in the control application to discover settings.
- Fingerprint files and compare again before applying. Reconcile or stop on changes made by an editor, another app instance, the shell, or an update. Preserve approved symlink relationships and file permissions.
- Use atomic replacement for each owned file, keeping direct-write fallback disabled. Qt's [QSaveFile](https://doc.qt.io/qt-6/qsavefile.html) supports this pattern. A multi-file or service operation is not made atomic by using QSaveFile; it needs a journal and an explicit recovery sequence.

### Change lifecycle

1. Read capabilities, effective state, and file/service preconditions.
2. Validate the proposed values and form a change plan.
3. Present the relevant effects and obtain confirmation only where the operation warrants it.
4. Capture the required backup/pre-state, lock conflicting operations, and journal intent before mutation.
5. Apply the smallest supported operation and record each completed step.
6. Reload only the affected component and verify actual state. For Hyprland changes, run reload and check `hyprctl configerrors` against the pre-existing baseline, then query the intended values.
7. Mark success only after verification; otherwise restore eligible changes or report partial completion with a recovery action.

Backups have bounded retention and restrictive permissions. Redact secrets from logs, history, diffs, and diagnostic exports. Undo compares current state with the transaction's result before changing it; it must not overwrite later edits. Commands with third-party hooks can have side effects that are not automatically reversible, and their history must say so.

### Operations needing specialized treatment

| Operation | Required behavior |
| --- | --- |
| Display mode/layout changes | A separate temporary user-session watchdog restores pre-state if confirmation times out or the GUI dies. Register recovery before the first mutation; account for hot-unplug and keep at least one usable output. Block integrated editing until this is proven. |
| Network connection/profile changes | Use a supported NetworkManager checkpoint where possible, with a rollback timeout and deliberate keep action. Test the installed implementation's limits; the checkpoint does not cover arbitrary external DNS/firewall files. See the [NetworkManager API](https://networkmanager.dev/docs/api/latest/gdbus-org.freedesktop.NetworkManager.html). |
| Keyboard/shortcut changes | Detect collisions and retain a usable recovery path. Do not silently replace the user's existing binding. Validate persisted and runtime results. |
| Updates/install/removal/firmware | Launch the existing supported workflow first; show progress or handoff honestly. Serialize against package operations. Do not kill a package manager to implement Cancel, and do not promise generic undo. |
| Snapshot restoration, encryption/auth changes, factory reset | Use the existing guided flow, with its scope and consequences visible. Do not reproduce these as unguarded buttons or infer backup coverage. |
| Privileged system changes | Prefer existing polkit-enabled services or visible Omarchy terminal workflows. Keep the GUI unprivileged. Any future helper exposes narrowly defined operations, never a generic root shell or arbitrary root file writer. |
| Secrets and sign-in | Let established agents/keyrings/helpers collect credentials. Never place secrets in command arguments, debug logs, persistent plans, or exported state. |
| Shell unavailable | Keep diagnosis, config access, and supported restart actions available in the standalone app; explain unavailable panel handoffs. |

Temporary display recovery is an exception to the no-background-daemon design. It runs only for the preview transaction. Crash recovery for unfinished ordinary transactions runs on next launch and must be idempotent.

## 7. Delivery milestones

Starting at P1, each milestone produces an installable, reviewable application. These are dependency-ordered work packages, not calendar estimates. The complete control-center outcome includes P5; P1 is only the first useful increment.

### P0 — compatibility and ownership prototypes

Build small probes against fixtures and a disposable test environment for command discovery, shell handoffs, theme/language watching, Lua override ordering, and the detached display recovery mechanism. Read package metadata and available service interfaces. Establish adapters' exact command side effects and supported version range.

**Exit gate:** documented source-of-truth and write strategy for each initial provider; verified behavior for custom Lua, toggles, and user plugin clones; a tested recovery approach before direct display editing. A failed write prototype results in a helper/advanced route, not an unsafe editor.

### P1 — native application and broad navigation

Build the themed window, navigation, search, keyboard behavior, capability model, error states, helper launcher, and package. Populate destinations for all coverage domains. Provide live read-only summaries where verified, and reviewed launches into existing panels, setup wizards, config editors, and available helper apps. New/unclassified inventory entries remain non-executable.

**Exit gate:** a user can find every reviewed stock configuration workflow and reach its current implementation. Missing helpers and hardware are explained. Theme/language switching works without restarting the app. No launch is reported as a successful settings change.

### P2 — integrated everyday settings

Implement the change coordinator and shared provider tests, then integrate themes/backgrounds/fonts/text size, default apps, bar/dock layout and selected widget options, idle/lock timing, notifications, and power profiles. Respect active plugin clones; controls that cannot configure a clone explain that limitation.

**Exit gate:** values survive restart where expected, changes made outside the app appear, custom data is preserved, injected failures produce usable recovery, and undo is offered only for supported transactions. This is the first release that combines broad navigation with substantial native editing.

### P3 — input, displays, connectivity, and audio

Add display arrangement and scaling, keyboard/pointer controls, shortcut editing, audio routing, and common network/Bluetooth controls. Retain helpers for enterprise Wi-Fi, complex VPNs, printer setup, and other specialist cases. Reuse Omarchy persistence rules rather than storing runtime-only changes as if they were permanent.

**Exit gate:** test preview timeout, GUI crash, hotplug, connection loss, device removal, competing edits, and permission rejection. Protected editing ships only after recovery tests pass.

### P4 — administration and advanced coverage

Complete updates/package workflows, startup and service management, locale/time, firewall/remote access, snapshots, storage helpers, authentication, firmware/boot, developer tools, hooks, plugin maintenance, and diagnostic export. Add typed native controls only where the underlying contract and authorization are understood; the remaining supported workflows stay integrated through handoffs.

**Exit gate:** the app reaches supported administrative operations without generic privileged execution; long-running operations have accurate status and safe cancellation behavior; recovery records distinguish partial completion and irreversible steps.

### P5 — coverage closure and release

Reconcile the full catalog, menu/provider inventory, configs, plugins, service interfaces, and installed helper capabilities. Resolve every unclassified user-facing entry. Validate translations, accessibility, themes, package installation/removal, upgrades, and supported hardware profiles. Document the compatibility range and known limitations.

**Exit gate:** every in-scope setting/workflow has a verified route and a recorded integration mode; no unclassified entry is counted as supported. Publish coverage of native editors and handoffs separately. Advanced-only access remains clearly identified. Candidate upstream packaging or menu integration is a separate distribution decision after local acceptance.

### Where each milestone stands

| Milestone | State | Open against its exit gate |
| --- | --- | --- |
| P0 | Done, [findings recorded](P0-FINDINGS.md) | Nothing. Recovery is proven on real Hyprland in a nested session |
| P1 | Built, real window and sample helpers verified nested | Installer and removal routes are route-checked, deliberately not run |
| P2 | Built, verified nested | Power profiles and language await a real session |
| P3 | Built, display recovery verified nested | Real monitors, sound, Bluetooth, and Wi-Fi need real hardware; the Arrangement editor stays locked per this gate until `selftest display` passes on the machine |
| P4 | Built | Long-running work stays in Omarchy's terminal flows, which own their status and cancellation; this app waits for pacman's lock rather than tracking jobs |
| P5 | Built, except what needs a person or hardware | Zero unclassified entries, ledger, package lifecycle, upgrade quarantine, and accessibility tree are done. Open: first run on a real session, a screen reader speaking, other hardware profiles, native-speaker review, and translated strings for 46 languages |

### First implementation work package

Start with the smallest complete path through the architecture: capability detection → theme-aware window → searchable Power page → read idle timeout → stage a new value → scoped `shell.json` change → verify → history/undo. Use temporary fixtures first. Add a network-panel handoff alongside it to prove both integrated and delegated workflows. This establishes the reusable foundation before expanding the catalog.

## 8. Validation and release criteria

Use Qt Test for backend behaviors and offscreen QML interaction tests. Inject temporary XDG locations and fake processes/services. Meaningful tests cover preservation, failure, concurrency, ownership, and recovery rather than merely repeating implementation details.

| Test area | Required evidence |
| --- | --- |
| Read correctness | Effective versus persisted state; unknown values; unavailable tools/services; custom configuration; capability changes |
| Write correctness | Round trips, invalid inputs, unknown-key/comment preservation, symlinks, permissions, disk-full failures, atomic replacement, conflicting edits |
| Process/service handling | Argument boundaries, exit failures, malformed/bounded output, cancellation, timeouts, denied authentication, no GUI blocking |
| Change coordinator | Partial failures, crash at each journal phase, safe replay, conflict-aware undo, redacted history |
| Desktop behavior | External edits, updates during use, reload errors, shell restarts, theme replacement, locale changes, no duplicate mutation from a second instance |
| Hazardous controls | Monitor timeout/crash recovery; network checkpoint recovery; no package-manager termination during a critical operation |
| UX/accessibility | Keyboard-only use, focus order, screen-reader labels, text scaling, RTL, long translations, narrow tiled windows |
| Visual fit | Compare the real app with stock Omarchy UI under dark/light themes, sharp/rounded corners, and multiple scale factors |
| Packaging | Clean build/install, launcher/menu entry, correct application ID, dependency declarations, upgrade, uninstall preserving user configuration |
| Coverage | Trace each supported catalog item to its provider/handoff and acceptance evidence; newly discovered entries fail the coverage review |

Live tests should use a disposable account/VM for destructive and authentication workflows, plus physical hardware for display, audio, Bluetooth, battery, and peripheral behavior. Record what was actually tested; VM success does not establish hardware compatibility. Screenshot fixtures use synthetic content.

Initial performance budgets are targets to measure, not existing results: a usable window within one second on this machine's warm launch; search response within 100 ms for the catalog; state updates without UI stalls; and negligible idle activity once scans settle. Avoid background network queries during normal settings search.

Release acceptance requires both breadth and trust: a common setting is easy to find, its displayed state is accurate, applying it preserves unrelated customizations, and a failed change leaves a clear recovery path.

## 9. Decisions to validate during implementation

- Which current Omarchy CLI outputs are sufficiently structured and stable for direct integration? Record adapters against inspected versions.
- Can the generated Lua module coexist with arbitrary user imports and Omarchy toggles without ambiguous priority? If not, keep the affected controls behind explicit overrides or advanced access.
- Which active third-party plugins expose supported settings? Manifest discovery alone is not a settings schema.
- Which system operations already have suitable service authorization? Add a privileged helper only for a concrete unmet need.
- What exact files and side effects do theme, font, defaults, and text-size commands touch? Recovery scope must match that evidence.
- Which locale catalogs can be maintained at release quality? Framework support for all registered locales is separate from translated-string coverage.
- How should upstream changes be tracked? Re-run discovery after Omarchy upgrades; compare IDs/signatures and gate incompatible providers. Support the tested local release first, rather than claiming all historical Omarchy versions.

None of these questions prevents starting P0. They are explicit engineering investigations with fallback behavior and exit gates.
