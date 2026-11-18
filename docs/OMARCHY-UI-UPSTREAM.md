# Omarchy UI Qt Quick findings

Control embeds the Qt Quick kit at `2f35eaef42d333c59bbfde146e4139ad4ff034c9`. The findings below use that revision. Runtime probes used Arch ARM, Qt 6.12.0, and the Basic controls style. [Pinned source](https://github.com/omacom/omarchy-ui/tree/2f35eaef42d333c59bbfde146e4139ad4ff034c9) · [Qt integration guide](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/docs/using-qt.md)

For a standalone QML repro, use the upstream `examples/qt-vendored` host with the probe as its `Main.qml`. Import `Omarchy.Ui as O` and `Omarchy.Theme as Theme`. For live theme probes, launch with a fresh `HOME`, an empty `XDG_RUNTIME_DIR`, and `OMARCHY_UI_THEME_DIR` unset. Set `OMARCHY_UI_REDUCE_MOTION=1` to observe changes without waiting for a fade.

## Theme and runtime

### A partial theme inherits colors from the previously loaded theme

Start with a theme whose `colors.toml` specifies `accent = "#ff00ff"`, `background = "#141414"` and `foreground = "#e4e4e4"`. In the same process, replace that file with one specifying `mode = "light"`, `background = "#fafafa"`, and `foreground = "#232323"` only. The background and foreground update, and the accent stays `#ff00ff`. Starting the second theme in a fresh process gives accent `#cacccc`, as the token spec describes when both `accent` and `color4` are absent. The same code path can retain foreground, background and urgent when their inputs are missing. Resolve a complete palette from defaults on each reload, and test a full-to-partial switch against a cold load of the partial theme. Control uses the SDK palette, so this also affects its live chrome. [Color.loadColors](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Theme/Color.qml#L163-L204) · [Palette fallbacks](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/spec/tokens.md#palette)

### Creating the first theme after startup goes unnoticed

Start the host with an empty home and observe the default `Theme.Color.accent`. While it is open, create `~/.local/state/omarchy/current/theme/colors.toml` with `accent = "#123456"`. The accent stays `#cacccc` until a reload or restart. `armWatcher()` watches existing target directories and files. A missing directory tree leaves it without an ancestor to observe. Watch the closest existing parent and re-arm as the child directories appear. The same approach would cover an initial creation of the configuration tree. [ThemeSource.armWatcher](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/src/themesource.cpp#L155-L183)

### A Qt host cannot inject a live theme environment

The QML native singleton is process-wide. It derives theme, override, language, and fontconfig paths from the process home, and compositor paths from process environment variables. `OMARCHY_UI_THEME_DIR` selects a fixture but also disables user overrides and Hyprland events. To reproduce that coupling, point it at a fixture with `[font] base-size = 15` and put `base-size = 16` in the user's `shell.toml`. The pinned theme stays at 15. A live theme source with configurable roots and runtime paths would let an embedding host isolate each engine and still exercise the real merge and watch behavior. Control's screenshot tool starts with a synthetic process home, and the Qt suite shares one isolated theme home across engines. [Native singleton registration](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/src/omarchyui.cpp#L10-L25) · [Path and pin behavior](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/src/themesource.cpp#L15-L46) · [Override read](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/src/themesource.cpp#L137-L143)

### Slow compositor replies delay QML startup and block theme reload

Run the host with `XDG_RUNTIME_DIR` pointing to a fixture that listens on `hypr/demo/.socket.sock` and `hypr/demo/.socket2.sock`. Reply to each of the four separate `j/getoption` requests after 140 ms and close the request socket after each response. The first QML completion callback took about 626 ms under the fixture. Native theme construction and reload use blocking socket waits in series and also wait for `fc-match`. Reading these on a worker or asynchronously would keep the UI responsive and still permit one batched update for the resolved snapshot. [ThemeSource startup and reload](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/src/themesource.cpp#L83-L134) · [Blocking reads](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/src/themesource.cpp#L199-L247)

### Reconnecting to Hyprland leaves old appearance options in use

Start the host against an empty `XDG_RUNTIME_DIR`. Then bring up both Hyprland fixture sockets with `decoration:rounding` returning `{"int":8}`. The Qt source connects to the new event socket, sends zero option requests, and keeps `Theme.Style.cornerRadius` at 0. Emitting `configreloaded>>\n` changes it to 8. Re-read options after a successful event-socket connection so a compositor restart can refresh the app even if its initial config reload happened before the app reconnected. [Event setup and reconnect](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/src/themesource.cpp#L55-L69) · [ThemeSource.connectEvents](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/src/themesource.cpp#L249-L259)

## Controls and navigation

### Dropdown selection breaks a binding to the model

Bind `O.Dropdown.value` to `model.active` and give it the string options First, Second and Third. Select Second with the mouse, then update `model.active` to Third. The dropdown stays at Second because its selection handler assigns to `root.value`. A request signal with an externally owned value, or an opt-in controlled mode, would let an app display only the value its backend has confirmed. [Dropdown selection handler](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Dropdown.qml#L204-L211)

### Dropdown can activate the highlighted row in place of the clicked row

Open with First selected. Send a pointer press and release on Second without sending a pointer-move event first. The dropdown selects First. Its cursor is updated by pointer movement and its click handler uses the cursor rather than finding the row at the click coordinates. Resolve the row on click, including touch taps, and test that a click on Second emits Second even while the keyboard cursor is on First. [Dropdown pointer handler](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Dropdown.qml#L230-L248)

### Dropdown needs model roles and visual slots

`O.Dropdown` takes a list of strings. An application selector can show two identical labels for different desktop IDs, with each option carrying an independent icon. Try binding `[{ id: "one", label: "Terminal" }, { id: "two", label: "Terminal" }]` and selecting the second ID. There is no `textRole`, `valueRole`, or option delegate to express the distinction. Its trigger also draws a Nerd Font chevron, which a font such as DejaVu Sans Mono cannot supply. Role-aware options, trigger and row content slots, and a vector or reliable font fallback would let Control replace its native ComboBox adapter. It currently uses SDK popup, focus, spacing, and state tokens on the Qt control. [Dropdown API and trigger](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Dropdown.qml) · [Glyph troubleshooting](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/docs/using-qt.md#glyphs-show-as-boxes)

### Button activates on key press and exposes no content slot

Tab to `O.Button { focusable: true }` and press and hold Space. It emits `clicked` on key down. Qt's AbstractButton activates on release and can cancel a pending activation. The SDK type also lacks a native accessible button action and a callable `click()`. Its glyph property covers one font icon, while Control has buttons with an SVG on each side of the label or a rotating progress icon. Keep the current pressed appearance and add native activation semantics and optional content slots. Control uses Qt's AbstractButton with SDK chrome for these paths. [Button API and input handlers](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Button.qml#L24-L60) · [Button keys](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Button.qml#L186-L234)

### ValueSlider has no keyboard or accessible slider action and pointer changes ignore step

Use `O.ValueSlider { minimum: -1; maximum: 1; step: 0.25 }`. Tab and arrow keys cannot adjust the slider. Clicking a non-step position emits an unsnapped fraction, and at value zero the entire left half reads as filled. The balance control needs quarter-step or finer snapping and neutral zero, plus keyboard and assistive-technology adjustment. Preserve the continuous option and expose snapping and a fill origin. Control uses Qt's Slider with SDK colors and metrics. [ValueSlider](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/ValueSlider.qml) · [PanelSlider](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/PanelSlider.qml)

### ToggleSwitch represents only a binary value

`O.ToggleSwitch` paints checked or unchecked and has no Tab stop, keyboard activation, or named accessible action. The labeled `O.Toggle` adds keyboard handling but still has no unread or mixed state. A newly opened settings page displays unknown until the first readback. Control uses the SDK switch for known values and a neutral track with a named native button for the unread state. Expose an indeterminate state and native semantics on a compact switch variant. [ToggleSwitch](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/ToggleSwitch.qml) · [Toggle](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Toggle.qml)

### Dialog needs configurable default action and modality

Open `O.Dialog { destructive: true; opened: true }` and press Return immediately. It accepts because the default selected index is 1. Clicking outside the card rejects. `O.ConfirmDialog` shares that default action and scrim behavior. Both use an in-tree Item, so an embedding app must manage window-modal focus containment and restore focus to the opener. Their fixed-width buttons also constrain translated labels. A native Popup or Dialog foundation with configurable dismissal and a safe default for destructive actions would cover Control's confirmation flow. Control uses themed chrome on its existing Qt Popup, with Cancel focused for destructive changes. [Dialog](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Dialog.qml) · [ConfirmDialog](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/ConfirmDialog.qml)

### Scrollbar styling is bound to the kit's own Flickable

`O.ScrollArea` owns a Flickable and a Column. Its scrollbar implementation is inside that wrapper. An existing ListView, or the popup delegate model in a native ComboBox, already owns its scrolling and needs an attachable Qt ScrollBar. Exposing the themed scrollbar on its own would also make the shared hover and drag treatment available there. Control uses SDK thickness, colors and motion on the native attached ScrollBar. [ScrollArea](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/ScrollArea.qml#L20-L108)

### ListRow and Sidebar need accessible and extensible row content

A standalone `O.ListRow` reacts to a pointer click and expects an external cursor model for keyboard navigation. Tab, Space, and Return do not activate it on their own, and it supplies no accessible row action. `O.Sidebar` has a keyboard cursor for the whole group, but no accessible row names/actions, icon-source or delegate slot, or scrolling container for tall navigation. Give `O.Sidebar` a constrained height and enough items to overflow, or use a long label in a narrow sidebar. The Column overflows and the label elides. Control retains its accessible expandable headers and scrollable navigation list, with SDK state and border styling. A focusable row API, injected row content, and optional wrapping and scrolling would cover those layouts. [ListRow](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/ListRow.qml) · [Sidebar](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Sidebar.qml)

## Layout and feedback

### Pane's default body requires layout-aware children

Put two `Item { width: parent.width; height: 38 }` children in a bordered `O.Pane` with `width: 220` and `contentPadding: 10`. The two items measure zero width. Their parent is the kit's ColumnLayout, which needs children to supply layout hints such as `Layout.fillWidth`. A plain content slot would support positioner-based groups and existing delegates, or the Qt guide could show how to adapt them. Control uses `O.BorderSurface` with its own Column. [Pane body and sizing](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Pane.qml#L53-L78)

### Heading elides its subtitle and fixes trailing content on the right

Place an `O.Heading` at width 230 with a long subtitle and an `O.Button` in its trailing slot. Under `LayoutMirroring.enabled: true` and `childrenInherit: true`, the trailing button stays on the right. The subtitle still elides. A wrapping subtitle and logical leading and trailing slots would let Control use the full heading. It uses the kit heading for titles, with a wrapping subtitle and RTL-aware composition around it. [Heading placement and typography](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Heading.qml#L22-L80)

### A body-only Toast has an empty accessible name and reserves space for a title

`O.Toast { width: 280; text: "Body-only message" }` reports an empty accessible name and the same implicit height as the toast with a title. It also lacks severity styling. Hide the unused title row, use the body as the accessible name when the title is empty, and offer warning and error tones. Control uses `O.Banner` for the wrapping message and severity, with app-managed timing and dismissal. It overrides Banner's default Nerd Font tone icons with standard Unicode characters so Qt can use a fallback font. [Toast content and accessibility](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Toast.qml#L34-L99) · [Banner glyphs](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/qt/qml/Omarchy/Ui/Banner.qml#L29-L39)

## Arch packaging

### The standalone Omarchy UI recipe omits its MIT notice

In the upstream package recipe, `license=('MIT')` is declared and `package()` installs the binding, docs, and fixtures. It has no install step for the repository's MIT license. This is a source review. Build the recipe and inspect the archive for a license under `usr/share/licenses/omarchy-ui/` to verify the packaging fix. Both Control recipes already include an `Omarchy-UI-LICENSE` file. [Upstream recipe](https://github.com/omacom/omarchy-ui/blob/2f35eaef42d333c59bbfde146e4139ad4ff034c9/packaging/arch/omarchy-ui/PKGBUILD)
