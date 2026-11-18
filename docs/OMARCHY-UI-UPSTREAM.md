# Omarchy UI Qt Quick findings

Control embeds the Qt Quick kit at `2f35eaef42d333c59bbfde146e4139ad4ff034c9`. The findings below use that revision. Runtime probes used Arch ARM, Qt 6.12.0, and the Basic controls style. [Pinned source](https://github.com/omacom/omarchy-ui/tree/2f35eaef42d333c59bbfde146e4139ad4ff034c9) · [Qt integration guide](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/docs/using-qt.md)

For a standalone QML repro, use the upstream `examples/qt-vendored` host with the probe as its `Main.qml`. Import `Omarchy.Ui as O` and `Omarchy.Theme as Theme`. For live theme probes, launch with a fresh `HOME`, an empty `XDG_RUNTIME_DIR`, and `OMARCHY_UI_THEME_DIR` unset. Set `OMARCHY_UI_REDUCE_MOTION=1` to observe changes without waiting for a fade.

## Upstream fix order

Each row can be reviewed independently. The palette fix comes first because it affects Control's live theme directly. The dropdown click fix comes next because it can apply a different value from the one the user chose.

| Order | Fix | Regression check |
| --- | --- | --- |
| 1 | [Resolve partial themes from defaults](#a-partial-theme-inherits-colors-from-the-previously-loaded-theme) | A live full-to-partial switch publishes and settles on the same palette as a cold load. Cover both motion settings. |
| 2 | [Select the clicked row in both dropdowns](#dropdown-can-activate-the-highlighted-row-in-place-of-the-clicked-row) | A stationary click selects its row. Enter still selects the keyboard's highlighted row. |
| 3 | [Preserve caller-owned values in both dropdowns](#dropdown-selection-breaks-a-binding-to-the-model) | A selection request keeps the caller's binding and displays the confirmed value. Existing callers still get automatic selection. |
| 4 | [Watch the first theme being created](#creating-the-first-theme-after-startup-goes-unnoticed) | Populate the directories of an initially empty home in stages. The running app picks up the first palette and override. |
| 5 | [Refresh options after a Hyprland reconnect](#reconnecting-to-hyprland-leaves-old-appearance-options-in-use) | A later event-socket connection refreshes rounding even if Hyprland sends no configuration event. |
| 6 | [Give body-only toasts an accessible name and layout](#a-body-only-toast-has-an-empty-accessible-name-and-reserves-space-for-a-title) | A body-only toast exposes its message to assistive technology and takes one text row. Titled toasts retain their layout. |
| 7 | [Mirror Heading actions in RTL](#heading-elides-its-subtitle-and-fixes-trailing-content-on-the-right) | Trailing actions sit on the right in LTR and on the left in RTL. Both layouts give the labels their remaining space. |
| 8 | [Ship the MIT notice in the Arch package](#the-standalone-omarchy-ui-recipe-omits-its-mit-notice) | Inspect the built package for `usr/share/licenses/omarchy-ui/LICENSE`. |

The destructive-confirmation default is a behavior proposal. Native input and accessibility, model and content slots, an attachable scrollbar, unread switches, Sidebar overflow, opt-in subtitle wrapping, and Toast tones need their own API decisions. Start Pane with a layout example. Configurable live theme roots and asynchronous compositor reads merit separate runtime changes with focused tests.

## Theme and runtime

### A partial theme inherits colors from the previously loaded theme

Start with a theme whose `colors.toml` specifies `accent = "#ff00ff"`, `background = "#141414"` and `foreground = "#e4e4e4"`. In the same process, replace that file with one specifying `mode = "light"`, `background = "#fafafa"`, and `foreground = "#232323"` only. The background and foreground update, and the accent stays `#ff00ff`. Starting the second theme in a fresh process gives accent `#cacccc`, as the token spec describes when both `accent` and `color4` are absent. The same code path can retain foreground, background and urgent when their inputs are missing. Resolve a complete palette from defaults on each reload, and test a full-to-partial switch against a cold load of the partial theme. Control uses the SDK palette, so this also affects its live chrome. [Color.loadColors](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Theme/Color.qml#L163-L204) · [Palette fallbacks](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/spec/tokens.md#palette)

### Creating the first theme after startup goes unnoticed

Start the host with an empty home and observe the default `Theme.Color.accent`. While it is open, create `~/.local/state/omarchy/current/theme/colors.toml` with `accent = "#123456"`. The accent stays `#cacccc` until a reload or restart. `armWatcher()` watches existing target directories and files. A missing directory tree leaves it without an ancestor to observe. Watch the closest existing parent and re-arm as the child directories appear. The same approach would cover an initial creation of the configuration tree. [ThemeSource.armWatcher](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/src/themesource.cpp#L155-L183)

### A Qt host cannot inject a live theme environment

The QML native singleton is process-wide. It derives theme, override, language, and fontconfig paths from the process home, and compositor paths from process environment variables. `OMARCHY_UI_THEME_DIR` selects a fixture but also disables user overrides and Hyprland events. To reproduce that coupling, point it at a fixture with `[font] base-size = 15` and put `base-size = 16` in the user's `shell.toml`. The pinned theme stays at 15. A live theme source with configurable roots and runtime paths would let an embedding host isolate each engine and still exercise the real merge and watch behavior. Control's screenshot tool starts with a synthetic process home, and the Qt suite shares one isolated theme home across engines. [Native singleton registration](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/src/omarchyui.cpp#L10-L25) · [Path and pin behavior](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/src/themesource.cpp#L15-L46) · [Override read](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/src/themesource.cpp#L137-L143)

### Slow compositor replies delay QML startup and block theme reload

Run the host with `XDG_RUNTIME_DIR` pointing to a fixture that listens on `hypr/demo/.socket.sock` and `hypr/demo/.socket2.sock`. Reply to each of the four separate `j/getoption` requests after 140 ms and close the request socket after each response. The first QML completion callback took about 626 ms in this synthetic startup test. Construction and reload both use the same blocking socket and `fc-match` reads. A worker or asynchronous read can preserve the batched theme update. Verify GUI responsiveness during a delayed reload, one notification per resolved change, and reduced-motion behavior. [ThemeSource startup and reload](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/src/themesource.cpp#L83-L134) · [Blocking reads](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/src/themesource.cpp#L199-L247)

### Reconnecting to Hyprland leaves old appearance options in use

Start the host against an empty `XDG_RUNTIME_DIR`. Then bring up both Hyprland fixture sockets with `decoration:rounding` returning `{"int":8}`. The Qt source connects to the new event socket, sends zero option requests, and keeps `Theme.Style.cornerRadius` at 0. Emitting `configreloaded>>\n` changes it to 8. Re-read options after a successful event-socket connection so a compositor restart can refresh the app even if its initial config reload happened before the app reconnected. [Event setup and reconnect](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/src/themesource.cpp#L55-L69) · [ThemeSource.connectEvents](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/src/themesource.cpp#L249-L259)

## Controls and navigation

### Dropdown selection breaks a binding to the model

Bind the dropdown's `value` to `model.active` and give it First, Second and Third. Select Second, then update `model.active` to Third. The dropdown stays at Second because its handler assigns to `root.value`, removing the binding. Both `Dropdown` and `SearchableDropdown` do this. An opt-in caller-owned mode should emit a selection request and continue displaying the backend's confirmed value, including if the backend declines the request. Existing callers can keep the automatic update. [Dropdown selection handler](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Dropdown.qml#L207-L213) · [SearchableDropdown selection handler](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/SearchableDropdown.qml#L275-L281)

### Dropdown can activate the highlighted row in place of the clicked row

Open with First highlighted. Send a pointer press and release on Second without sending a pointer-move event first. The dropdown can emit First. In both dropdowns, pointer movement updates the cursor and clicking invokes the keyboard selection handler. Use the clicked delegate's index for pointer activation and the cursor index for Enter. Include a click or tap on a different row with no preceding pointer motion. [Dropdown pointer handler](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Dropdown.qml#L238-L244) · [SearchableDropdown pointer handler](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/SearchableDropdown.qml#L344-L350)

### Dropdown needs model roles and visual slots

`O.Dropdown` accepts strings and `{ value, label }` objects. Options `{ value: "one", label: "Terminal" }` and `{ value: "two", label: "Terminal" }` have distinct values. The gaps for Control's application selector are arbitrary model roles and slots for trigger and option content, including an image per option. The stock chevron relies on a Nerd Font glyph and can render as a box with DejaVu Sans Mono. Control uses a native ComboBox with SDK popup, focus, spacing, and state tokens. [Dropdown options](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Dropdown.qml#L54-L65) · [Glyph troubleshooting](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/docs/using-qt.md#glyphs-show-as-boxes)

### Button activates on key press and exposes no content slot

Tab to `O.Button { focusable: true }` and press and hold Space. It emits `clicked` on key down. Qt's AbstractButton activates on release and can cancel a pending activation. The SDK button also lacks a native accessible button action and a callable `click()`. It supports rotation and spinning for one font glyph. Control needs custom content, including a bundled SVG on either side of the label, and uses Qt's AbstractButton with SDK chrome. Treat native activation and custom content as separate proposals. [Button keyboard handlers](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Button.qml#L62-L69) · [Glyph animation](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Button.qml#L182-L200)

### ValueSlider needs standalone input and signed-range options

Use `O.ValueSlider { minimum: -1; maximum: 1; step: 0.25 }`. The wheel uses `step`, while pointer input remains continuous unless integer mode is set. At zero the fill extends from the minimum to the midpoint. Control's balance slider needs configurable pointer snapping and a neutral zero fill origin. Treat those opt-in options separately from adding a Tab stop, keyboard adjustment, and accessible slider actions. Control uses Qt's Slider with SDK colors and metrics. [Pointer mapping](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/PanelSlider.qml#L119-L126) · [Wheel handling](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/PanelSlider.qml#L150-L157)

### ToggleSwitch represents only a binary value

`O.ToggleSwitch` paints checked or unchecked and has no Tab stop, keyboard activation, or named accessible action. The labeled `O.Toggle` adds keyboard handling but still has no unread or mixed state. A newly opened settings page displays unknown until the first readback. Control uses the SDK switch for known values and a neutral track with a named native button for the unread state. Expose an indeterminate state and native semantics on a compact switch variant. [ToggleSwitch](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/ToggleSwitch.qml) · [Toggle](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Toggle.qml)

### Destructive dialogs select Confirm by default

Open `O.Dialog { destructive: true; opened: true }` and press Return immediately. The default `selectedIndex: 1` confirms. Both dialog types expose `selectedIndex`, so a host can set or reset it to 0 for Cancel. Outside-click rejection is documented. Propose a Cancel default for destructive dialogs, an explicit initial-selection override, and a test that reopens the dialog after Confirm was selected. Pointer hover currently also changes what Return activates. Window-modal focus containment, restoration to the opener, configurable scrim dismissal, and translated button widths can be reviewed separately. Control uses its native Qt Popup with SDK styling and focuses Cancel for destructive changes. [Dialog keyboard handling](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Dialog.qml#L20-L61) · [ConfirmDialog host key handler](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/ConfirmDialog.qml#L8-L48) · [Dialog spec](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/spec/components/forms.md#dialog)

### Scrollbar styling is bound to the kit's own Flickable

`O.ScrollArea` owns a Flickable and a Column. Its scrollbar implementation is inside that wrapper. An existing ListView, or the popup delegate model in a native ComboBox, already owns its scrolling and needs an attachable Qt ScrollBar. Exposing the themed scrollbar on its own would also make the shared hover and drag treatment available there. Control uses SDK thickness, colors and motion on the native attached ScrollBar. [ScrollArea](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/ScrollArea.qml#L20-L108)

### ListRow and Sidebar need accessible and extensible row content

`O.ListRow` already has leading, trailing, and expansion component slots. A standalone row still needs a host cursor for keyboard navigation and supplies no accessible activation action. `O.Sidebar` has its own group keyboard cursor. Its rows need accessible names and actions, and an application navigation sidebar can also benefit from custom icon sources, delegates, wrapping, and scrolling. Constrain the sidebar height to see the Column overflow, and narrow the sidebar to see long labels elide. Control keeps accessible expandable headers and a scrollable navigation list with SDK state and border styling. [ListRow slots](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/ListRow.qml#L19-L61) · [Sidebar](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Sidebar.qml)

## Layout and feedback

### Pane's default body requires layout-aware children

Put two `Item { width: parent.width; height: 38 }` children in a bordered `O.Pane` with `width: 220` and `contentPadding: 10`. The items measure zero width under the pane's ColumnLayout. Show how children can use layout hints such as `Layout.fillWidth: true` and `implicitHeight: 38`. A plain content slot is a separate proposal for positioner-based groups and existing delegates. Control uses `O.BorderSurface` with its own Column. [Pane body and sizing](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Pane.qml#L53-L78)

### Heading elides its subtitle and fixes trailing content on the right

Place an `O.Heading` at width 230 with a long subtitle and an `O.Button` in its trailing slot. Under `LayoutMirroring.enabled: true` and `childrenInherit: true`, the trailing button stays on the right. The layout fix should make the existing slot follow the reading direction. Elision is documented, so optional subtitle wrapping is a separate feature. Control uses the kit heading for titles and supplies a wrapping subtitle and RTL-aware composition around it. [Heading placement and typography](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Heading.qml#L22-L80)

### A body-only Toast has an empty accessible name and reserves space for a title

`O.Toast { width: 280; text: "Body-only message" }` reports an empty accessible name and the same implicit height as the toast with a title. Hide the unused title row and use the body as the accessible name when the title is empty. Severity tones are a separate API proposal because the kit already offers them on `O.Banner`. Control uses Banner with app-managed timing and dismissal, and standard Unicode tone icons that can use Qt's font fallback. [Toast content and accessibility](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Toast.qml#L34-L99) · [Banner glyphs](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Banner.qml#L29-L39)

## Arch packaging

### The standalone Omarchy UI recipe omits its MIT notice

In the upstream package recipe, `license=('MIT')` is declared and `package()` installs the binding, docs, and fixtures. It has no install step for the repository's MIT license. This is a source review. Build the recipe and inspect the archive for a license under `usr/share/licenses/omarchy-ui/` to verify the packaging fix. Both Control recipes already include an `Omarchy-UI-LICENSE` file. [Upstream recipe](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/packaging/arch/omarchy-ui/PKGBUILD)
