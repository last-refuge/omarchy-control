# Helper workflow scope

GUI-method parity and a catalog destination are insufficient evidence of routine
automation. The helper follow-up reviewed the Omarchy 4.5.0 source contracts and
added these native alternatives. Each preserves its original static
`launch <workflow-id>` invocation for existing callers.

| Workflow | Native alternative | Verification |
| --- | --- | --- |
| Background introductions | `appearance.background.intros` get/plan/apply | Owned empty on/off marker; no animation-playback claim |
| Battery percentage | `widgets.option`: `widget option <selector> showPercentage true\|false` | First-party Power object instance and full preserved live/persisted bar configuration; no rendering claim |
| Per-program crash muting | `preferences.crash-mutes`: list/read/explicit boolean write | Owned empty program marker; no watcher-health claim |
| Theme/font/background pickers and next background | `appearance.theme`, `appearance.font`, `appearance.background` | Existing typed-setting plans and scoped readback; choose from current live choices |
| Plugin menu, built-in clone and removal | `plugins.inspect`, `plugins.clone`, `plugins.remove` | Existing registry, guarded directory capture and explicit recovery contracts |
| Internal-display and mirror toggles | `display.layout` | Existing full-layout protected preview; human usability confirmation still required before Keep |

External exec/terminal/shell destinations now carry `automationStatus` and a
specific `handoffReason` in the source catalog. API descriptions expose that
status under `automation.status`, plus `nativeSetting`/`nativeOperations` and
the reason. Coverage includes the alternatives and reason separately from its
destination classification. The independent accounting check fails when a new
external destination lacks this review or names a nonexistent alternative.
Currently 11 of the 344 script destinations identify native alternatives and
333 retain handoffs. The theme picker is an additional panel alternative.

Omarchy 4.5's stock bar uses object entries, including Power. Bare-ID entries
remain readable, but the reviewed upstream per-widget setter refuses them;
Control reports that custom-layout limitation rather than silently rewriting
the entry format.

The remaining handoffs have these boundaries. A noninteractive upstream command
is not described as inherently interactive merely because the GUI opens a terminal.

| Family | Source behavior and retained boundary |
| --- | --- |
| Default agent | `omarchy-default-agent` can install dependencies, writes `~/.config/omarchy/defaults/agent`, and launches the selected agent. The compound install/launch remains a confirmed handoff; `apps.default.agent` reads the reported preference. |
| Default dictation | `omarchy-default-dictation <backend>` launches its installer. `apps.default.dictation` reads `~/.config/omarchy/defaults/dictation`; native `input.dictation.*` settings cover available supported backends, not installation. |
| Keyboard/input setup | `omarchy-setup-keyboard` invokes the interactive Python typing selector, coordinating `~/.config/omarchy/keyboard-layouts` and Fcitx settings. Input-method presets can be passed noninteractively but include privileged package/setup work. A raw Hyprland layout edit would not preserve this workflow's semantics. |
| Branding | About/screensaver helpers select/transcode images, open editors, or copy defaults then open previews. Known paths are `~/.config/omarchy/branding/about.txt` and `screensaver.txt`. Content authoring/import remains with the editor/file picker; it is not a scalar machine preference. |
| Background import | The helper opens `~/.config/omarchy/backgrounds/<current-theme>` in the file manager. Existing image selection has a native setting; file import remains a content workflow. |
| External plugin add/update | Both helpers accept noninteractive arguments. They fetch/update external Git code and may enable it. Repository trust, whole-tree upgrades and recovery remain outside the reviewed built-in clone/remove adapter. Plugin validation is a separate read-only folder tool. |
| Packages, themes, fonts, web apps and developer tools | Installation/removal changes resource inventories and may run package managers, Git or application-specific setup. These guided lifecycle jobs retain static destinations, not a native package/resource transaction API. Existing native selection/preferences apply after resources are installed. |
| Updates, refresh/reset, firmware, boot, snapshots and firewall | System administration can replace multiple files, require privileges or alter boot/filesystem state. The upstream tool owns its sequence and recovery. Native concrete service operations and diagnostics remain available where advertised. |
| Audio tuning | `omarchy-audio-tuning` detects specific models and manages PipeWire fragments and a user service. The catalog currently opens its status report. Hardware tuning installation is separate from native volume/default/routing operations. |
| Keyboard backlight | The source chooses the first supported LED and exposes relative up/down/cycle plus off/restore. The catalog's static cycle remains callable; Control does not advertise an absolute persisted brightness setting or verified LED output. |
| Reminders, captures, sharing, notes, weather and transcoding | These are productivity/content tasks. For example `omarchy-reminder` already accepts minutes/message, `show --json`, and `clear`; Control does not wrap a timer/content API or claim notification/capture delivery. |
| Agent accounts, application configuration and onboarding | Credential/account lifecycle and application-specific schemas stay with the owning tools. Some account-policy helpers accept parameters; no native account/credential adapter is advertised. |
| Session/power actions and launch/reference tools | Lock/reboot/shutdown, app launch and reference panels are actions rather than persistent preferences. Acceptance of their static launch does not establish the eventual session/application state. |
| Network enrollment and device recovery | Authentication/captive portals stay with the owning panel/browser. Supported Ethernet/open-Wi-Fi changes use the native protected preview protocol; display recovery and physical usability still require desktop acceptance. |

The native contract covers the declared settings/provider scope, with explicit
handoffs for these additional catalogued tasks. It is not an API for every
parameter of every upstream administration or productivity command.
Ownership/compatibility refusals never authorize another writer or a raw-file
bypass. New routine preference helpers should be promoted through the shared
planner/writer/readback path, rather than added to a broad handoff category.

Reproducible preference checks use `tools/workflow-check.py` in a private,
unprivileged Docker lab. It verifies source fingerprints before running the real
Omarchy scripts and production CLI, with a simulated `qs` endpoint. This tests
argument/protocol integration; real shell rendering, playback and device behavior
remain separate acceptance evidence.
