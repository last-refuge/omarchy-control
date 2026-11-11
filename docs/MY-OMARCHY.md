# My Omarchy: contract, audit, and progress

Status, November 11, 2026: P0 (contract and portability audit) is recorded here, and P1 (local settings round trip) is implemented and tested against fixtures. Nothing has been restored on a real desktop yet. The milestones P0 to P5 are those of the My Omarchy implementation plan.

Evidence base: a read-only audit of this machine. `pacman -Q` reports `omarchy` and `omarchy-settings` 4.5.0-3 and `hyprland` 0.56.2-4, on x86_64. `/usr/share/omarchy/version` reads `4.0.0.alpha` on this release, so it is never used; the version comes from `omarchy version`, as Control already does.

## 1. Profile contract

A profile is a directory:

```text
profile.json                 # Version, choices, package intent, adapter records
assets/sha256/<digest>       # From P3: backgrounds, icons, theme files
files/<adapter>/<name>       # From P3: reviewed configuration files
```

[profile.schema.json](profile.schema.json) defines version 1. The validator in `src/profilemodel.cpp` enforces the same rules, plus some that JSON Schema cannot express: duplicate keys at any depth, duplicate package names or record ids, and a 100 MiB total for files and assets. 40 invalid fixtures and 3 valid ones in `tests/fixtures/profiles/` pin the rules. Until the first public release, version 1 may still change; after it, a change in meaning needs version 2 and a migration.

- **Settings** map a catalog id to `{"mode": "set", "value": …}` or `{"mode": "inherit"}`. A setting the profile leaves out is unmanaged and left alone. Values are the catalog's normalized strings.
- **Refused before anything happens:** invalid JSON (comments and trailing commas included), duplicate keys, unknown sections or fields, a newer `schemaVersion`, ids that are not ids, control characters, package names pacman would read as options, unknown file adapters, path-like file names, malformed digests, files over 25 MiB, and profiles over 100 MiB. A profile is refused as a whole; nothing restores from a refused profile.
- **Kept and reported, never applied:** setting ids this version does not know, and settings the catalog does not consider portable. They stay in the profile on the next save.
- **Canonical bytes:** sorted keys, packages sorted by name, records sorted by id, the dock's order kept, one trailing newline. The same choices always produce the same file. `source` (Control, Omarchy, and Hyprland versions, and architecture) is diagnostic and never counts as a change, so an unchanged capture writes nothing.
- **No home paths travel.** `source` has a closed set of fields, and settings whose values are paths (backgrounds) are not portable until P3 stores them as assets.

Machine-local state lives under `~/.local/state/omarchy-control/profiles` (directories 0700, files 0600): restore plans, run records, and the baseline, which is the last value this computer agreed on for each setting. The restore lease is `~/.local/state/omarchy-control/restore.lock`. None of this is ever part of a profile.

## 2. Which settings travel

Every editable catalog setting now carries a reviewed `profile` block in `src/data/settings.json`. Being editable does not make a setting portable, and a test fails if a new editable setting has no block.

| Portability | Count | Meaning |
| --- | ---: | --- |
| `portable` | 35 | Captured, planned, restored, and verified by P1 |
| `later` | 25 | Portable in principle, waiting for a later milestone |
| `machine` | 12 | Belongs to this computer's hardware, network, or relationships |
| `runtime` | 4 | A passing state, not a preference |

Fields: `category`; `reason` for anything not portable; `inherit` where "use the default" is reliable (a JSON key removal, an owned Lua override removal, or a reset command); `after` for restore order; `activation` for effects to disclose; `stock` for the value a fresh installation has, where that is known; `overlaps` for another setting that masks this one.

### Portable in P1

| Category | Settings | Adapter | Inherit |
| --- | --- | --- | --- |
| appearance | theme, font, CJK glyph forms, text size | Omarchy commands | text size only |
| windows | corners, transparency, gaps on/off, single-window aspect, animations, default layout | Omarchy commands | no |
| windows | inner and outer gaps, border, dim, dim strength | owned `omarchy-control.lua` | yes |
| input | repeat rate and delay, Num Lock, sensitivity, acceleration, natural scroll, focus follows mouse, six touchpad options | owned `omarchy-control.lua` | yes |
| idle | lock after, screensaver after | scoped `shell.json` edit | yes |
| idle | screensaver available | Omarchy toggle | no |
| defaults | browser, terminal, editor | Omarchy commands | no |
| language | language | Omarchy command | no |

### Not portable yet, with the recorded reason

- **later:** background and icon packs (files, so assets in P3); the launcher icon layer (follows the icon pack); the 10 blur settings (they read through window transparency and Omarchy's glass, and a strength writes two values, so their order needs evidence); bar and clock settings (restored with the bar layout, P3); dock position and autohide (with the dock, P3); the 5 Superwhisper dictation settings (an app's own settings).
- **machine:** display brightness and scale, touchpad and touchscreen on/off, DNS and Wi-Fi band, power profiles on AC and battery, whether to offer suspend, crash capture, herdr theme sync, and the weather location (personal location data).
- **runtime:** do not disturb, night light, the active power profile, and stay awake.

## 3. Audit of the commands behind portable settings

Read from source on this machine. All 14 scripts match the hashes already pinned in `src/data/compat.json`, so a later change to any of them pauses its settings, and a restore plan shows them as blocked until reviewed.

| Command | What it does beyond its value | Consequence for restore |
| --- | --- | --- |
| `omarchy theme set` | Rebuilds the current theme, chooses that theme's background (remembered per theme), restarts terminals, btop, opencode, and helix, reloads Hyprland, and retints GTK, the browser policy, VS Code, Obsidian, the keyboard, and fcitx5 in the background. Runs `omarchy-hook theme-set`, and starts `omarchy-theme-set-herdr-machines`, which mirrors to herdr machines when theme sync is on. Refuses a theme that is not installed. | Restored first. Discloses hooks and restarts. A theme installed in the home folder is blocked on a computer that lacks it, until P3 brings theme assets. A wallpaper restore (P3) must come after it. Control's own icon pack hook runs from here and takes the change lock, so each restore step first waits for the lock. |
| `omarchy font set` | Requires the family in `fc-list`. Rewrites the family in Alacritty, Kitty, Ghostty, and Foot configs, replaces `~/.config/fontconfig/fonts.conf`, restarts the shell, and runs `omarchy-hook font-set`. | Blocked unless the font is installed (P2 installs fonts). Overlaps P3's terminal file adapters. |
| `omarchy font cjk` | Runs Omarchy's input-method `configure.py` and restarts the shell. | Discloses the restart. |
| `omarchy display text size` | Writes `[font] base-size` in `~/.config/omarchy/shell.toml`, the GTK text scaling factor, and terminal sizes with `sed`. `reset` removes the override. | Inherit supported. Overlaps P3's terminal file adapters. |
| `omarchy hyprland corners`, `window transparency`, `hyprland toggle`, `toggle animations` | Copy or remove toggle files in `~/.local/state/omarchy/toggles/hypr`, then reload Hyprland. Transparency rewrites the theme's glass under the theme lock and restarts terminals. Theme changes keep these choices. | After the theme. |
| `window-no-gaps.lua` (the gaps toggle) | Sets inner and outer gaps, border size, and rounding to 0, and loads after `omarchy-control.lua`. | A profile that turns gaps off blocks its own gap and border sizes, with the reason, rather than fail verification. |
| `no-animations.lua` | Also turns off blur and shadows. | One reason blur waits. |
| `omarchy hyprland workspace layout default` | Busy workspaces keep their current layout; empty ones take the new default. | Discloses it. |
| `omarchy language` | Generates the locale as root: a passwordless grant, else `sudo` in a terminal, else `pkexec` on screen. Writes the language and the uwsm session locale, restarts the shell, and seeds a CJK input method. | Discloses that it may ask for a password. |
| `omarchy default browser`, `terminal`, `editor` | Launch an installer when the app is missing; otherwise write `mimeapps.list`, `xdg-terminals.list`, or Omarchy's editor state. | A missing app is blocked by the existing plan check, never installed. P2 installs apps first. |

Idle timers and owned Lua overrides use Control's own reviewed adapters, described in [P0 findings](P0-FINDINGS.md).

## 4. Audit of the package routes, for P2

| Route | Behavior | Fit |
| --- | --- | --- |
| `omarchy pkg add <names>` | `sudo pacman -S --noconfirm --needed`, then `pacman -Q` per name. No database refresh. | Usable for named repository installs only after the plan has shown the transaction, since `--noconfirm` answers pacman's own questions with defaults. |
| `omarchy pkg aur add <names>` | `yay -S --noconfirm --needed`. | Unsuitable: no build review. P2 needs a terminal flow that shows the PKGBUILD. |
| `omarchy pkg remove` | fzf picker, then `pacman -Rns --noconfirm`. | Unsuitable: interactive. |
| `omarchy pkg drop <names>` | `sudo pacman -Rns --noconfirm` on the installed ones. | Unsuitable: `-s` removes dependencies too, and `-n` removes config backups, without review. P2 uses a narrow `pacman -R` adapter with a printed dry run. |
| `omarchy pkg present`, `missing` | `pacman -Q` checks. | Verification reads. |
| `omarchy pkg defaults [platform]` (hidden) | The base list plus architecture and platform additions from `$OMARCHY_PATH/install/*.packages`. 162 names on this x86 machine. | Source of the application baseline. |
| `omarchy pkg install`, `aur install` | Interactive pickers. | Not used. |

Fingerprints to pin when the P2 adapter lands, as SHA-256 prefixes: `omarchy-pkg-add` f39314028454ced3, `omarchy-pkg-aur-add` 91704da97df33ce1, `omarchy-pkg-remove` b591764abec88fa7, `omarchy-pkg-drop` 0e2ec756ceb3d4a1, `omarchy-pkg-present` 3d6b2d498a3ebdf7, `omarchy-pkg-missing` 2cfa64af694cc58d, `omarchy-pkg-defaults` 6a71de3dcc410381.

**Application baseline format (proposed for P2):** one reviewed file per supported Omarchy version and platform, generated from `omarchy pkg defaults <platform>` and checked in: the Omarchy version, the platform, the package names, and digests of the package lists it came from. Explicitly installed packages outside the baseline become capture suggestions. A baseline package missing on the source machine is a proposed exclusion only when that baseline applies to the source's version and platform.

## 5. Destination support policy

- Supported destination: Omarchy 4.5.x on x86_64 with Hyprland 0.56, read from `omarchy version` and `hyprctl version`.
- On another version, settings still restore through the catalog. A command whose script changed since review pauses its settings through Compat, and the plan shows them as blocked. A profile never imports accepted fingerprints or bypasses that review.
- The versions in `source` are diagnostic. Nothing is ever downgraded to match them.
- P2 will not refresh package databases on their own, which would be a partial upgrade. When a full update is needed, it offers the normal update workflow, then plans again.

## 6. How a restore decides (P1)

A plan reads every setting once, then decides, and changes nothing. Each item is one of:

| Status | Meaning |
| --- | --- |
| `ready` | Will be restored |
| `unchanged` | Already as saved |
| `conflict` | This computer may have its own choice here. Restored only with `--use-saved`; `--keep-local` records the decision to leave it |
| `blocked` | Cannot be restored here, with the reason: not installed, no such hardware, masked by another setting, unreadable, or paused by Compat |
| `manual` | This version does not restore it: unknown, not portable, or a section a later milestone handles |

**Telling a default from a choice.** When this computer has a baseline for the setting, from an earlier capture or verified restore, a live value that still matches it means only the profile moved, so the item is `ready`; anything else changed here since and is a `conflict`. Without a baseline, the value is treated as a default when it equals the recorded `stock` value, when an owned Lua setting is set by neither this app nor the user's own files, or when a JSON or TOML key holds no override. Anything else is a `conflict`. Without a stock value (font, default apps, language), a fresh installation therefore asks with `--use-saved`. A local edit is never taken as permission to overwrite it.

**Order.** The catalog's reviewed order, adjusted for `after`. The theme always comes first. The order of entries in a profile never counts.

**Applying.** `apply` takes the restore lease, settles interrupted changes, waits for a change already underway, and reads every item again. If the profile changed since planning, or any item no longer reads as the plan saw it, nothing starts. The run record reaches the disk before the first change. Each item then goes through the normal engine: journaled, backed up, verified by reading back, and undoable on its own where the setting supports undo. Every transaction records its run id before it starts.

**The lease.** While a restore holds it, the coordinator refuses every other change, from this process, another window, or the CLI. It is a lock file with no age limit, so a run whose process dies releases it, and the run is then resumable.

**Resuming.** Verified items are checked again and left alone. An item that was underway is read: if it already has the saved value it is done; if it still has the planned value it runs again; anything else is reported as interrupted and left for a person. Nothing is replayed blindly. A run that changed profile since it began is not resumed.

**Results.** `completed` means every item in the plan was verified or already matched. `partial` means the run did everything it was asked, and the plan left items out, which `apply` only accepts with `--partial`. `incomplete` means something failed or needs attention. `cancelled` and `interrupted` runs can be resumed.

## 7. Command line

```sh
omarchy-control profile capture ~/my-omarchy --category appearance --category idle
omarchy-control profile capture ~/my-omarchy            # Later: re-reads what it remembers
omarchy-control profile inspect ~/my-omarchy
omarchy-control profile plan ~/my-omarchy [--use-saved <id|all>] [--keep-local <id|all>]
omarchy-control profile apply <plan-id> --yes [--partial]
omarchy-control profile resume <run-id>
omarchy-control profile status
```

Capture selection: `--category` takes appearance, windows, input, idle, defaults, or language; `--setting <id>` names one setting; `--suggested` takes the portable settings that differ from a fresh installation by the rules above; `--inherit <id>` saves "use the default"; `--forget <id>` stops remembering one; `--dry-run` shows the changes without writing. A first capture needs a selection; nothing is captured in bulk by default. What a category holds but a profile cannot carry is listed with its reason. A saved setting whose current value cannot be read stays as saved and is flagged. Each command takes `--json`, and exits 0 when the request was fulfilled and 1 otherwise.

## 8. Fixture matrix for the P1 gate

| Gate requirement | Test |
| --- | --- |
| Capture on A, restore to B, verify effective values, preserve unrelated config, second restore is a no-op | `profileRoundTripsBetweenMachines` |
| External-edit conflict detection | `profilePlanFindsLocalEdits`, `profileApplyRefusesAStalePlan` |
| Failed-read preservation | `profileCaptureKeepsWhatItCannotRead` |
| Crash recovery | `profileRunResumesAfterACrash` |
| GUI and CLI exclusion while a restore runs | `profileLeaseExcludesOtherChanges` |
| Invalid input refused before any change | `profileRefusesInvalidFixtures`, `profileRefusesABrokenProfileBeforeAnything` |
| Unsupported items stay visible | `profileCaptureListsWhatItCannotCarry`, `profilePlanHoldsBackMaskedAndUnsupportedItems` |
| Inherit returns to the default | `profileInheritRemovesAnOverride` |
| Every editable setting reviewed | `profileMetadataCoversEveryEditableSetting` |
| CLI end to end | `profileCliRoundTrip` |

These are fixture tests with fake commands. The P1 gate also asks for real settings behavior: extending `tools/nested-check.py` to restore a profile into the nested session is the next verification step.

## 9. Coverage by adapter

| Adapter | Capture | Plan | Apply | Verify | Recovery | State |
| --- | --- | --- | --- | --- | --- | --- |
| Settings: scoped `shell.json` | yes | yes | yes | read-back | per-change undo | P1, fixture-tested |
| Settings: owned `omarchy-control.lua` | the module's value | yes | yes | read-back and config errors | per-change undo | P1, fixture-tested |
| Settings: Omarchy commands | yes | yes | yes | read-back | per catalog: undo or best effort | P1, fixture-tested |
| Applications | none | listed as manual | none | none | none | P2 |
| Dock, launchers, associations, shortcuts, window rules | none | listed as manual | none | none | none | P3 |
| Ghostty and Alacritty files | none | listed as manual | none | none | none | P3 |
| Backgrounds, icons, theme files | none | listed as manual | none | none | none | P3 |
| Git and GitHub | none | none | none | none | none | P4 |
| Window | none | none | none | none | none | P5 |

### Test strategy for the remaining categories

| Category | Adapter | Test strategy |
| --- | --- | --- |
| Applications | PackageProvider: narrow pacman adapter, reviewed AUR terminal worker | Fake pacman and yay for denial, busy lock, and failed builds; a disposable pacman root, as in `tools/package-check.sh`, for real transactions |
| Launchers, associations, dock | LauncherEditor, SystemProvider associations, Omarchy's dock commands | Fixtures, then the nested session |
| Shortcuts and window rules | Owned Lua module, merged by record id | Fixtures, then nested Hyprland |
| Bar layout and plugins | PluginsProvider | Fixtures with a fake shell |
| Backgrounds, theme and icon assets | profilefiles | Fixtures for digests, containment, links, and oversized files |
| Ghostty and Alacritty configs | profilefiles adapters | Fixtures for includes, executable directives, and overlap with font and text size |
| GitHub | ProfileRepository over `git` and `gh` | Local bare repositories and a fake `gh`; one private test repository for the release gate |
| Whole restore | All of the above | A disposable Omarchy VM |

## 10. Known limits of P1

- There is no window yet (P5). The window's own controls already respect a restore running elsewhere, because every change goes through the coordinator.
- Font, default apps, and language have no evidenced stock value, so without a baseline they need `--use-saved` on a fresh installation.
- Hyprland provenance recognizes numeric and true/false assignments in the user's own Lua. A string value there, such as an acceleration profile, reads as Omarchy's default.
- Font and text size edit terminal configs that P3's file adapters will own. P3 must order them, or refuse the overlap.
- After the first write, a failure to update the run record does not stop the run. The coordinator's journal still records every change with its run id.
