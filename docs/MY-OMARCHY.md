# My Omarchy: contract, audit, and progress

Status, November 12, 2026: P0 (contract and portability audit) is recorded here, and P1 (local settings round trip) is implemented and tested against fixtures. P2 (apps) is implemented: repository installs, reviewed AUR builds, explicit removals, and default apps that wait for their app, fixture-tested and checked against real pacman in a disposable root. P3 (desktop choices and files) is implemented and fixture-tested: themes installed from Git, the background with its file, the bar's layout and Omarchy's plugin switches, the dock, launcher changes with their icon files, file associations, the shortcuts and window rules this app owns, and Ghostty and Alacritty configuration files. P4 (versions and GitHub) is implemented and tested with real git against local repositories standing in for GitHub: saving versions, history and differences, connecting or creating a private repository, fetching, publishing, and restoring a chosen version. On this machine, a capture, save, and plan from the saved version ran with the app's own folders in a scratch location; nothing has been restored on a real desktop, and nothing has been published to github.com yet. The milestones P0 to P5 are those of the My Omarchy implementation plan.

Evidence base: a read-only audit of this machine. `pacman -Q` reports `omarchy` and `omarchy-settings` 4.5.0-3 and `hyprland` 0.56.2-4, on x86_64. `/usr/share/omarchy/version` reads `4.0.0.alpha` on this release, so it is never used; the version comes from `omarchy version`, as Control already does.

## 1. Profile contract

A profile is a directory:

```text
profile.json                 # Version, choices, package intent, adapter records
assets/sha256/<digest>       # Backgrounds and launcher icons, by content
files/<adapter>/<name>       # Reviewed configuration files: ghostty/config, alacritty/alacritty.toml
```

[profile.schema.json](profile.schema.json) defines version 1. The validator in `src/profilemodel.cpp` enforces the same rules, plus some that JSON Schema cannot express: duplicate keys at any depth, duplicate package names or record ids, and a 100 MiB total for files and assets. 48 invalid fixtures and 3 valid ones in `tests/fixtures/profiles/` pin the rules. Until the first public release, version 1 may still change; after it, a change in meaning needs version 2 and a migration.

- **Settings** map a catalog id to `{"mode": "set", "value": …}` or `{"mode": "inherit"}`. A setting the profile leaves out is unmanaged and left alone. Values are the catalog's normalized strings.
- **Refused before anything happens:** invalid JSON (comments and trailing commas included), duplicate keys, unknown sections or fields, a newer `schemaVersion`, ids that are not ids, control characters, package names pacman would read as options, unknown file adapters, path-like file names, malformed digests, files over 25 MiB, and profiles over 100 MiB. A profile is refused as a whole; nothing restores from a refused profile.
- **Kept and reported, never applied:** setting ids this version does not know, and settings the catalog does not consider portable. They stay in the profile on the next save.
- **Canonical bytes:** sorted keys, packages sorted by name, records sorted by id, the dock's order kept, one trailing newline. The same choices always produce the same file. `source` (Control, Omarchy, and Hyprland versions, and architecture) is diagnostic and never counts as a change, so an unchanged capture writes nothing.
- **No home paths travel.** `source` has a closed set of fields. A background or launcher icon that is a file travels as an asset named by its digest, and goes back only to the one place its adapter puts it. Configuration files and bar options that name a path in a home folder are not carried.

Machine-local state lives under `~/.local/state/omarchy-control/profiles` (directories 0700, files 0600): restore plans, run records, and the baseline, which is the last value this computer agreed on for each setting. The restore lease is `~/.local/state/omarchy-control/restore.lock`. None of this is ever part of a profile.

## 2. Which settings travel

Every editable catalog setting now carries a reviewed `profile` block in `src/data/settings.json`. Being editable does not make a setting portable, and a test fails if a new editable setting has no block.

| Portability | Count | Meaning |
| --- | ---: | --- |
| `portable` | 40 | Captured, planned, restored, and verified as settings |
| `later` | 20 | Not a setting a profile carries yet, or carried by a desktop record instead |
| `machine` | 12 | Belongs to this computer's hardware, network, or relationships |
| `runtime` | 4 | A passing state, not a preference |

Fields: `category`; `reason` for anything not portable; `inherit` where "use the default" is reliable (a JSON key removal, an owned Lua override removal, or a reset command); `after` for restore order; `activation` for effects to disclose; `stock` for the value a fresh installation has, where that is known; `overlaps` for another setting that masks this one.

### Portable settings

| Category | Settings | Adapter | Inherit |
| --- | --- | --- | --- |
| appearance | theme, font, CJK glyph forms, text size | Omarchy commands | text size only |
| windows | corners, transparency, gaps on/off, single-window aspect, animations, default layout | Omarchy commands | no |
| windows | inner and outer gaps, border, dim, dim strength | owned `omarchy-control.lua` | yes |
| input | repeat rate and delay, Num Lock, sensitivity, acceleration, natural scroll, focus follows mouse, six touchpad options | owned `omarchy-control.lua` | yes |
| idle | lock after, screensaver after | scoped `shell.json` edit | yes |
| idle | screensaver available | Omarchy toggle | no |
| desktop | bar position, background, and visibility; dock position and autohide | Omarchy commands | no |
| defaults | browser, terminal, editor | Omarchy commands | no |
| language | language | Omarchy command | no |

### Not portable yet, with the recorded reason

- **later:** icon packs (folders or packages) and the launcher icon layer that follows them; the 10 blur settings (they read through window transparency and Omarchy's glass, and a strength writes two values, so their order needs evidence); the 5 Superwhisper dictation settings (an app's own settings). Three more are carried by desktop records instead, so each has one writer: the background (the background record), and the clock's two formats (options of the clock in the bar's layout).
- **machine:** display brightness and scale, touchpad and touchscreen on/off, DNS and Wi-Fi band, power profiles on AC and battery, whether to offer suspend, crash capture, herdr theme sync, and the weather location (personal location data).
- **runtime:** do not disturb, night light, the active power profile, and stay awake.

## 3. Audit of the commands behind portable settings

Read from source on this machine. All 14 scripts match the hashes already pinned in `src/data/compat.json`, so a later change to any of them pauses its settings, and a restore plan shows them as blocked until reviewed.

| Command | What it does beyond its value | Consequence for restore |
| --- | --- | --- |
| `omarchy theme set` | Rebuilds the current theme, chooses that theme's background (remembered per theme), restarts terminals, btop, opencode, and helix, reloads Hyprland, and retints GTK, the browser policy, VS Code, Obsidian, the keyboard, and fcitx5 in the background. Runs `omarchy-hook theme-set`, and starts `omarchy-theme-set-herdr-machines`, which mirrors to herdr machines when theme sync is on. Refuses a theme that is not installed. | Restored first among settings. Discloses hooks and restarts. A theme installed from Git is installed first and the setting waits for it; one installed by hand is blocked on a computer that lacks it. The background comes after it. Control's own icon pack hook runs from here and takes the change lock, so each restore step first waits for the lock. |
| `omarchy font set` | Requires the family in `fc-list`. Rewrites the family in Alacritty, Kitty, Ghostty, and Foot configs, replaces `~/.config/fontconfig/fonts.conf`, restarts the shell, and runs `omarchy-hook font-set`. | Blocked unless the font is installed (P2 installs fonts). Runs after the terminal files, so it edits the restored file rather than being overwritten by it. |
| `omarchy font cjk` | Runs Omarchy's input-method `configure.py` and restarts the shell. | Discloses the restart. |
| `omarchy display text size` | Writes `[font] base-size` in `~/.config/omarchy/shell.toml`, the GTK text scaling factor, and terminal sizes with `sed`. `reset` removes the override. | Inherit supported. Runs after the terminal files, as the font does. |
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

P2 does not route through these scripts. A restore needs pacman to show each transaction and ask, exact names with nothing else removed, and a check of each package afterwards, so it calls pacman itself: `pacman -S --needed`, `pacman -R`, and `pacman -D --asexplicit`, never with `--noconfirm`, `-s`, `-c`, or `-n`. The dry runs `pacman -S --print` and `pacman -R --print`, and the dependency test `pacman -T`, need neither root nor the network, and were checked against this machine's pacman. These scripts' fingerprints, as SHA-256 prefixes, for any later use: `omarchy-pkg-add` f39314028454ced3, `omarchy-pkg-aur-add` 91704da97df33ce1, `omarchy-pkg-remove` b591764abec88fa7, `omarchy-pkg-drop` 0e2ec756ceb3d4a1, `omarchy-pkg-present` 3d6b2d498a3ebdf7, `omarchy-pkg-missing` 2cfa64af694cc58d, `omarchy-pkg-defaults` 6a71de3dcc410381.

**Application baseline (as built in P2):** read live from the installed Omarchy, not checked in: `omarchy pkg defaults` gives what Omarchy installs on this platform, and its own `install/omarchy-other.packages` lists what it installs for particular hardware or from its installer image, such as drivers, firmware, and the audio stack. Explicitly installed packages outside both are capture suggestions. A default missing here is shown as a possible exclusion only when `pacman -T` says nothing installed provides it, since an alternative such as `ex-vi-compat` stands in for `vi`; it is never recorded unless chosen by name. On this machine that took the suggestions from 51 to 41 genuine apps, and the missing defaults from 4 to 2.

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

## 7. Apps (P2)

**Capture.** `--app <package>` remembers an installed package as wanted, command-line tools included. `--exclude-app <package>` remembers one as unwanted, so a restore removes it. `--forget-app` stops remembering either way. `--suggested-apps` takes what is installed beyond Omarchy's defaults. A package from no configured repository is saved with source `manual`, since nothing can tell an AUR package from a local build without the network. `--resolve-aur` asks the AUR's public lookup which of those it has, sending only their names, and saves the ones it knows with source `aur`; it is the only part of a capture that uses the network. A default app's own package needs no lookup: the catalog knows Chrome comes from the AUR. A default app the profile sets brings the package that provides it, from a mapping taken from Omarchy's own installers. Kernels, firmware, drivers, and what Omarchy installs for hardware are never saved as apps. A remembered app that is missing here, or an unwanted one installed here, stays as saved and is flagged.

**Plan.** Each app is one item, checked by pacman's dry run:

| Saved | Here | Status |
| --- | --- | --- |
| wanted | installed as wanted | `unchanged` |
| wanted | installed only as a dependency | `ready`: marked as wanted |
| wanted | missing, in a configured repository | `ready`: install, listing what it brings |
| wanted | missing, in no repository | `blocked` |
| wanted, from the AUR | missing | `ready`: built in the terminal, or `blocked` without `yay` |
| wanted, `manual` | missing | `manual` |
| unwanted | missing | `unchanged` |
| unwanted | installed, one of Omarchy's defaults | `ready`: remove |
| unwanted | installed, not one of Omarchy's | `conflict`: possibly installed here on purpose |
| unwanted | needed by another installed package | `blocked`, naming it |
| unwanted | protected or hardware | `blocked`, always |

Protected: the base system, kernels and firmware, the bootloader and snapshots, pacman and sudo, Omarchy, Omarchy Control, Hyprland and the session it runs in, NetworkManager and the audio stack, and `yay` and `git`, which restoring relies on. As with settings, the record of what this computer last agreed on turns a change made here since into a conflict. A profile whose default app needs a package it also removes is refused.

**Order.** Apps to install come first, then settings, then removals, so a removed default is only removed after whatever replaces it has been verified. A default app whose app the restore installs waits for that install, and is blocked if it fails.

**Apply.** All repository installs go in one transaction, all removals in another, so sudo asks once and pacman shows each whole transaction before changing anything. Each AUR app is built on its own with `yay -S --needed --aur --diffmenu --noanswerdiff`: yay offers its build files for review whatever answer its own settings keep, and a build that fails or is cancelled blocks only what needs that app. Before each, pacman's dry run is repeated: if it now differs from the plan, because packages updated or a dependency appeared, nothing starts and the plan has to be made again. If another package operation holds pacman's lock, the restore waits a minute and then pauses, resumable; the lock is never removed. The items are recorded as underway before pacman starts, and afterwards each app is settled by reading pacman, whatever its exit status: a refused password changes nothing, and that is what is reported. Each transaction is one journal entry under the run, marked as not undoable. Packages change only in a terminal, where sudo and pacman can ask, so only the command line does it; elsewhere app items are blocked with that reason.

**Resume.** An app that was underway is read. Installed as wanted, it is done; still as the plan saw it, it runs again; anything else is left for a person.

**Checked against real pacman.** `tools/profile-package-check.sh` builds a few small packages into a local repository, then has the real `omarchy-control` plan and restore a profile into a disposable pacman root under fakeroot, with this machine's database untouched. It confirmed the dry runs, dependency listing, install reasons, the refused removal, partial results, and that a second restore runs no transaction. It also found two things the fakes had hidden: pacman prints the dependencies a removal would break on stdout, not stderr, and its output went into `--json` results; both are fixed. And it settled what happens when nobody can answer: pacman takes end of input, or "n", as no, and nothing changes, so a restore run from a script never confirms on its own.

## 8. Desktop choices and files (P3)

| Record | Saved as | Restored with | Verified by |
| --- | --- | --- | --- |
| Themes from Git | Each theme's folder and the https address it was cloned from | `git clone` into Omarchy's themes folder, as `omarchy-theme-install` does, without switching to it, and only where no theme of that name is installed | The folder's own record of where it came from |
| Background | The theme it belongs to and its file name; one of the user's own also has its file, as an asset | One of the user's goes into their backgrounds for that theme, then `omarchy theme bg set` | The current background link |
| Bar layout | Omarchy's own widgets in each section, in order, with the options that are only data | The layout written into `shell.json`, as Omarchy's own bar commands commit it, then the shell asked to reload it | `shell.json` read back |
| Plugins switched off | Which of Omarchy's own plugins, other than bar widgets, are off | `omarchy plugin disable` and `enable` | The shell's plugin list |
| Dock | The whole ordered list of pinned app ids, as Omarchy keeps them | `omarchy dock pinned <ids...>`, which sets the list in one step and keeps every other key | `omarchy dock pinned` reading the same list |
| Launchers | Only the fields changed here: name, icon, hidden. An icon that is a file is an asset | The launcher editor, as the window does: a marked copy, never the package's own file; an icon file goes through the editor's own import | Reading the launcher back |
| File associations | The app for each of the window's everyday file types | `xdg-mime default` | `xdg-mime query default` |
| Shortcuts this app owns | Keys, description, and command, under an id made from the keys | Rewriting `omarchy-control.lua`, then reloading Hyprland | Hyprland's own list of binds, and no new config error |
| Window rules this app owns | Match, action, and value, under an id made from the action and match | The same | No new config error after the reload, as Hyprland has no query for a rule |
| Ghostty and Alacritty configuration | The whole file, at `files/<adapter>/<name>` | Written atomically to the one place the adapter knows | Its digest |

**Capture.** `--themes`, `--background`, `--bar`, `--plugins`, `--dock`, `--launchers`, `--associations`, `--shortcuts`, `--window-rules`, or `--desktop` for all nine; `--file ghostty|alacritty` or `--terminal-configs` for the files. A later capture reads each remembered record again: a launcher change undone here, a theme removed here, or an owned shortcut gone leaves the profile, while an app missing here, an association or bar that cannot be read, or a hand-edited `omarchy-control.lua` keeps the saved record and flags it. Remembering the theme setting brings the theme's Git address along, unasked, since restoring the setting needs it.

**What does not travel, and says so.** A theme installed by hand, or from an address that is not https: SSH addresses on GitHub, GitLab, Codeberg, Bitbucket, and SourceHut are carried in their https form, since a fresh computer has no keys yet, and a user name or password in an address never leaves the computer. A background that is neither its theme's nor in the user's backgrounds folder. Bar widgets from plugins installed separately, which are code. Any bar option that is not on the list of data options read from Omarchy's own widgets: an entry with `exec`, `source`, or `type` becomes a module that runs a command or loads a file, and the click handlers run commands. A configuration file that includes anything but Omarchy's own theme file, runs a program, loads a shader or image, or names a path in a home folder; and one identical to Omarchy's own, which carries nothing of the user's.

**Plan.** Each record is treated as a setting is: unchanged when this computer already has it, a conflict when it may be someone's choice here, and ready otherwise. A dock, bar, or file that differs from Omarchy's own, a launcher changed here, plugins switched off here, one of the user's own backgrounds, and an existing shortcut or rule of the same id are possible choices. A shortcut whose keys already do something is always a decision, and restoring it with `--use-saved` releases the old binding rather than leave two. Every shortcut's command and every theme's address is shown before anything happens. Installing a theme overwrites nothing, so it is ready or blocked, never a conflict: a theme of that name already here, from anywhere else, is left alone with the reason, and one Omarchy ships counts as there. The theme setting waits for a theme the same restore installs, and a background waits for its theme to be the one in use. A launcher or association for an app the restore installs waits for that install. A bar widget or plugin this Omarchy does not have, a hand-edited `omarchy-control.lua`, and a profile's file or asset that is missing or does not match its digest are blocked, never forced. Each adapter is paused, like a setting, when the Omarchy command it follows changed since review: the dock and background commands, `omarchy-theme-install`, `omarchy-bar` and `omarchy-shell-config`, and the plugin commands.

**Order.** Apps to install, then themes, then configuration files, then settings with the theme first, then the other desktop records, then app removals. Files come before settings so that a font or text size restored with them is set on top of the file rather than overwritten by it.

**Writing the bar.** The saved widgets go in each section the profile names, in their saved order. One that is already on the bar keeps whatever options of its own a profile does not carry, and moves rather than appear twice. Widgets from plugins installed separately keep their section, after the saved ones. A section the profile does not name is left alone, as a setting it leaves out is; a capture always names all six. Everything else in `shell.json` is left byte for byte. A shell that is not running reads the file when it starts.

After the run, what each restored record reads as is kept as this computer's agreed value, for later plans to compare with.

## 9. Versions and GitHub (P4)

**Where things live.** This computer's own profile is a plain folder, `~/.local/share/omarchy-control/profiles/my-omarchy/`, and its history a bare Git repository beside it, `my-omarchy.git`. The connection is recorded in `~/.config/omarchy-control/profile-repository.json`. A version read back for planning goes to a private folder under `~/.local/state/omarchy-control/profiles/revisions/`. Plans, runs, and journals stay on this computer and never enter the repository. A capture with no folder named uses this computer's profile, and starts from the newest version saved here, or published, so it never begins again from nothing.

**Saved and published are separate.** `profile save` makes a version from the profile's reviewed files alone: `profile.json`, the assets it names, and its configuration files, each checked against its digest first. Nothing is checked out and nothing runs: the files are hashed into a temporary index of their own, written as a tree, and committed. Saving the same profile again makes no version, and saving never uses the network. `profile publish` fetches first, then pushes only when GitHub's newest version is behind this computer's, which is a fast-forward; nothing is ever forced. When the push fails, offline or refused, the versions stay saved here, to publish later.

**When both sides changed.** Publishing stops, and nothing is rewritten, merged, or forced. `profile resolve --keep-mine` saves this computer's files as a new version with both histories behind it, which publishing then adds. `--use-published` takes the published version here; this computer's newest, with any unsaved changes, is first kept as a version of its own under `refs/omarchy-control/kept`, never thrown away. Fetching never moves this computer's versions on its own, so a capture can never silently drop what another computer published.

**A downloaded version is input, not code.** Git runs with none of the user's configuration (no global or system config), no hooks, no attributes or filters, only https, objects checked as they arrive, no submodules, and no prompts. A version is read blob by blob, never checked out. Before anything is read, every entry is checked, and the version is refused whole if it holds any path but `profile.json`, `assets/sha256/<digest>`, and `files/<adapter>/<name>`; any link, program, or submodule; or anything over the size limits. What is read is then validated as any profile is, digests included, before it can be planned, and a refused version leaves nothing behind.

**GitHub.** `profile connect owner/name`, or its github.com address, records the repository and branch. With GitHub CLI signed in, it also reads the repository's visibility and the account's no-reply address, which versions carry instead of a real one; without it, they carry a placeholder name and address. A public repository is refused unless `--allow-public` says so. `profile connect --create <name>` creates a private repository with `gh repo create --private`. Signing in stays with GitHub CLI: the managed repository asks `gh auth git-credential` for github.com, as `gh auth setup-git` would, without touching the user's own Git setup, and Control never asks for or stores a token. GitHub CLI keeps its own token, in plain text where no keyring is available; nothing here claims it is encrypted. Only connect, fetch, and publish use the network.

**Restoring a version.** `profile plan --revision published`, `saved`, or a commit from `profile history` reads that version back and plans it like any profile; the plan records that folder, so applying it restores exactly what was reviewed. `profile diff <version> [<version>]` lists what one changes from another, setting by setting, app by app, and record by record.

## 10. Command line

```sh
omarchy-control profile capture ~/my-omarchy --category appearance --category idle
omarchy-control profile capture ~/my-omarchy            # Later: re-reads what it remembers
omarchy-control profile inspect ~/my-omarchy
omarchy-control profile plan ~/my-omarchy [--use-saved <id|all>] [--keep-local <id|all>]
omarchy-control profile apply <plan-id> --yes [--partial]
omarchy-control profile resume <run-id>
omarchy-control profile status

omarchy-control profile capture --category appearance       # This computer's own profile
omarchy-control profile save --message "New laptop"
omarchy-control profile connect --create my-omarchy          # Or: connect owner/name
omarchy-control profile publish
omarchy-control profile history [--published]
omarchy-control profile diff published working

# On another computer
omarchy-control profile connect owner/my-omarchy
omarchy-control profile fetch
omarchy-control profile plan --revision published
```

Apps take `--app`, `--exclude-app`, `--forget-app`, `--suggested-apps`, and `--resolve-aur`, and desktop records and files the options in section 8. Settings: `--category` takes appearance, windows, desktop, input, idle, defaults, or language; `--setting <id>` names one setting; `--suggested` takes the portable settings that differ from a fresh installation by the rules above; `--inherit <id>` saves "use the default"; `--forget <id>` stops remembering one; `--dry-run` shows the changes without writing. A first capture needs a selection; nothing is captured in bulk by default. What a category holds but a profile cannot carry is listed with its reason. A saved setting whose current value cannot be read stays as saved and is flagged. Each command takes `--json`, and exits 0 when the request was fulfilled and 1 otherwise.

## 11. Fixture matrix

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
| P2: install a selected app and a CLI tool, switch a default to the new app, remove an excluded optional app, preserve unrelated apps | `profileRestoresApps`, `profileCliRestoresApps` |
| P2: refuse a required removal, and protected or hardware ones | `profileRefusesUnsafeAppRemovals` |
| P2: resume after an interrupted worker | `profileResumesAfterAnInterruptedPackageWorker` |
| P2: auth denial, failed transactions, changed package plans, missing packages | `profileAppFailuresAreReportedAsTheyAre`, `profileRefusesUnsafeAppRemovals` |
| P2: pacman busy | `profilePausesWhilePacmanIsBusy` |
| P2: inventory and dry runs change nothing | `packageInventoryReadsPacmanWithoutChangingAnything` |
| P2: capture, suggestions, hardware, and a stand-in for a default | `profileCapturesApps` |
| P2: AUR builds, and a failed build blocking only what needs it | `profileBuildsAppsFromTheAur`, `profileFailedAurBuildBlocksOnlyWhatNeedsIt` |
| P2: AUR lookup only when asked | `profileResolvesAurAppsOnlyWhenAsked` |
| P2: real pacman, declining and confirming | `tools/profile-package-check.sh` |
| P3: capture the dock, a launcher change, an association, and an owned shortcut and rule; an undone change leaves the profile; an unreadable one stays | `profileCapturesDesktopChoices` |
| P3: restore them on another computer, each verified, and find nothing to do the second time | `profileRestoresDesktopChoices` |
| P3: a dock of one's own, keys already in use, a missing app, an icon file, and a hand-edited module | `profileDesktopConflictsAndBlocks` |
| P3: a background of one's own and a launcher icon file travel as assets, and a damaged copy is refused | `profileCarriesBackgroundsAndIconFiles` |
| P3: a theme's own background waits for its theme | `profileRestoresAThemesOwnBackgroundForItsThemeOnly` |
| P3: Ghostty and Alacritty files restore before settings; a local file is a conflict; a damaged or edited copy is refused | `profileCarriesTerminalConfigs`, `profileLeavesOutConfigsItCannotCarry` |
| P3: theme addresses are https only, with no password | `profileThemeSourcesAreHttpsOnly`, `profileCarriesThemeSources` |
| P3: a theme is installed before the setting that selects it and the background that belongs to it | `profileInstallsAThemeBeforeSelectingIt` |
| P3: a theme already here is never replaced; a failed clone is taken back and blocks what needs it; a changed installer pauses | `profileNeverReplacesAThemeAlreadyHere`, `profileThemeInstallFailuresAreContained` |
| P3: the bar's layout and plugin switches, keeping widgets installed separately and options of one's own | `profileCarriesTheBarLayout` |
| P3: a widget or plugin this Omarchy lacks, and a shell that is not running | `profileBarAndPluginsRefuseWhatIsNotHere` |
| P4: versions saved without the network, a no-op save, history, differences, and a version read back | `profileRepositorySavesVersionsWithoutTheNetwork` |
| P4: save on A, publish to a private repository, fetch on a fresh B, restore the same preferences, and add to the history from B | `profileRepositoryRoundTripsThroughGitHub` |
| P4: remote divergence stops publishing; keeping either side loses nothing; an offline or failed publish keeps the local versions | `profileRepositoryNeverLosesAVersion` |
| P4: disallowed Git content, oversized files, unknown schemas, and asset integrity failures | `profileRepositoryRefusesWhatAProfileNeverHolds` |
| P4: only GitHub; a public repository only when allowed; a created one is private | `profileRepositoryConnectsOnlyToGitHub` |
| P4: command line end to end | `profileCliSavesAndPublishes` |

The tests use fake commands, including a stateful fake pacman, and `tools/profile-package-check.sh` repeats the package side with real pacman in a disposable root. Every test machine is a separate sandbox with its own home folder, so each restore lands under a different home path than its capture, as the P3 gate asks. The P1 gate also asks for real settings behavior: extending `tools/nested-check.py` to restore a profile into the nested session is the next verification step there. On this machine, a dry-run capture of apps and a plan of a package-only profile also ran against the system's own pacman without root, changing nothing.

## 12. Coverage by adapter

| Adapter | Capture | Plan | Apply | Verify | Recovery | State |
| --- | --- | --- | --- | --- | --- | --- |
| Settings: scoped `shell.json` | yes | yes | yes | read-back | per-change undo | P1, fixture-tested |
| Settings: owned `omarchy-control.lua` | the module's value | yes | yes | read-back and config errors | per-change undo | P1, fixture-tested |
| Settings: Omarchy commands | yes | yes | yes | read-back | per catalog: undo or best effort | P1, fixture-tested |
| Applications from configured repositories | yes | pacman's dry run | pacman, in a terminal | reads pacman afterwards | reported, not undone | P2, fixture-tested and checked with real pacman |
| Applications from the AUR | with `--resolve-aur`, or from the catalog | needs `yay` | yay, in a terminal, with review | reads pacman afterwards | reported, not undone | P2, fixture-tested |
| Dock | yes | yes | Omarchy's dock command | read back | no automatic undo; the file is backed up | P3, fixture-tested |
| Launchers | changed fields, icon files as assets | yes | the launcher editor | read back | the editor's own undo | P3, fixture-tested |
| File associations | everyday types | yes | `xdg-mime` | read back | no automatic undo; the file is backed up | P3, fixture-tested |
| Owned shortcuts and window rules | yes | yes, with collisions | `omarchy-control.lua` | binds list, config errors | no automatic undo; files backed up | P3, fixture-tested |
| Themes from Git | the https address | never replaces one | `git clone`, without switching | the folder's origin | a failed clone is removed; no undo after | P3, fixture-tested |
| Background | with its file when it is the user's | waits for its theme | `omarchy theme bg set` | the background link | no automatic undo | P3, fixture-tested |
| Bar layout | Omarchy's own widgets, data options | yes | `shell.json`, then reload | read back | the file is backed up | P3, fixture-tested |
| Plugins switched off | from the shell | yes | Omarchy's plugin commands | the shell's list | the file is backed up | P3, fixture-tested |
| Ghostty and Alacritty files | reviewed contents | yes | atomic write | digest | the file is backed up | P3, fixture-tested |
| Versions on this computer | the reviewed files only | any version, read back and validated | commit, never a checkout | the version read back | nothing lost: set-aside versions are kept | P4, tested with real git |
| GitHub | connect, fetch | fetch first | fast-forward push only | the published head | a failed publish keeps local versions | P4, tested with local repositories and a fake GitHub CLI |
| Window | none | none | none | none | none | P5 |

### Test strategy for the remaining categories

| Category | Adapter | Test strategy |
| --- | --- | --- |
| Launchers, associations, dock | LauncherEditor, SystemProvider associations, Omarchy's dock commands | Fixtures, then the nested session |
| Shortcuts and window rules | Owned Lua module, merged by record id | Fixtures, then nested Hyprland |
| Themes, bar layout, and plugins | ProfileDesktop over Omarchy's own commands and `shell.json` | Fixtures with a fake shell and fake git, then the nested session |
| Backgrounds and icon assets | profilefiles | Fixtures for digests, containment, links, and oversized files |
| Ghostty and Alacritty configs | profilefiles adapters | Fixtures for includes, executable directives, and the order against font and text size |
| GitHub | ProfileRepository over `git` and `gh` | Local bare repositories and a fake `gh`, done; one private test repository for the release gate, still to do |
| Whole restore | All of the above | A disposable Omarchy VM |

## 13. Known limits so far

- There is no window yet (P5). The window's own controls already respect a restore running elsewhere, because every change goes through the coordinator.
- Font, default apps, and language have no evidenced stock value, so without a baseline they need `--use-saved` on a fresh installation.
- Hyprland provenance recognizes numeric and true/false assignments in the user's own Lua. A string value there, such as an acceleration profile, reads as Omarchy's default.
- Font and text size edit the terminal files a profile can also carry. They are ordered after the files, so the setting lands on top; a profile whose file and font disagree ends with the font.
- After the first write, a failure to update the run record does not stop the run. The coordinator's journal still records every change with its run id.
- Apps install with pacman alone. Omarchy's own installers for some apps also write their configs, desktop entries, or browser policies; a restore does not run them, and no file adapter carries that setup yet.
- An AUR build is checked with a fake yay only. A real one needs the network and a real AUR package, which belongs in the disposable-VM check of P5.
- Launchers that are the user's own files, such as web apps, are not carried yet; a launcher's icon file is.
- A theme comes from its address as it is now, not as it was when saved: no commit is recorded. Omarchy's own staging of a Git theme leaves out anything that runs code.
- Icon packs, third-party plugins and their widgets, and a bar chosen from a plugin are not carried. The bar's options are an allowlist, so an option a newer Omarchy adds is left out, with a note, until it is reviewed.
- On a widget already on the bar, options a profile does not carry stay as they are here, while the ones it carries are set to the saved ones, which removes one the saved widget did not have.
- Capture records file associations for the window's seven everyday types only.
- The window cannot restore apps itself. Launching the command-line restore in a terminal, which then holds the lease and survives the window closing, is the P5 route.
- GitHub itself is not exercised yet: the tests use local bare repositories and a fake GitHub CLI. The release gate's round trip through a private test repository on github.com is still to do, and needs a GitHub account to run it with.
- A refused sign-in is recognized from git's own message, which covers GitHub's answers over https, though perhaps not every proxy's.
- One profile per repository, on one branch.
- Versions read back for planning stay under the state folder until read again; nothing prunes them yet.
