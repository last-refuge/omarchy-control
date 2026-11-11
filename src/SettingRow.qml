import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One row of the settings list, for every type in the catalog. It shows the
// effective value as last read, why it cannot be edited if it cannot, and keeps
// file paths and commands behind an expandable details line.
Item {
    id: root
    objectName: "settingRow"
    property var entry: ({})
    property bool compact: false
    property bool last: false
    property bool highlighted: false
    // Where a row has the room to show its subject rather than only name it.
    property bool rich: false
    // Drawn as a panel's heading: the panel's own title and icon, with this setting's
    // control beside it, as a device's on/off switch is.
    property string headingTitle: ""
    property string headingIcon: ""
    // A row that only leads elsewhere: the whole row goes there, marked with a chevron.
    property bool navigation: false
    // A config file, shown as its file: the name, the folder it is in, then what it is for.
    property bool file: false
    // A short list of destinations given more room, as a page's few links are.
    property bool prominent: false
    // Whether the rows this one keeps behind it are showing; the section holding them decides.
    property bool disclosed: false
    signal disclosureToggled()
    readonly property bool discloses: rich && (entry.discloses || []).length > 0
    readonly property bool blurStrength: rich && entryId === "appearance.blur.strength"
    readonly property string filePath: file ? (entry.file || "") : ""
    readonly property string fileName: filePath.replace(/\/+$/, "").split("/").pop()
    readonly property string fileFolder: filePath.slice(0, filePath.length - fileName.length)
    readonly property bool fileNamed: {
        var label = (entry.label || "").toLowerCase(), name = fileName.toLowerCase()
        return label === name || label === name.replace(/^\./, "")
    }

    readonly property string entryId: entry.id || ""
    // A setting for one display of several reads and writes that display: the one
    // selected on the Displays page. Search and pins still know it by its plain id.
    readonly property string target: rich && entry.target === "display" ? backend.displays.selected : ""
    readonly property string stateId: target.length ? entryId + "@" + target : entryId
    readonly property bool multiDisplay: rich && backend.displays.monitors.length > 1
    readonly property string labelText: qsTranslate("catalog", multiDisplay && entry.multiLabel ? entry.multiLabel : entry.label || "")
    readonly property string descriptionText: st.detail ? st.detail
        : qsTranslate("catalog", multiDisplay && entry.multiDescription ? entry.multiDescription : entry.description || "").replace("{target}", backend.displays.selected)
    readonly property var st: backend.states[backend.stateKey(stateId)] || ({})
    readonly property string status: st.status || "loading"
    readonly property bool busy: !!st.busy
    readonly property bool unavailable: status === "unavailable"
    readonly property string kind: entry.type || "status"
    readonly property bool editable: !!entry.editable && !unavailable && status !== "custom" && !busy
    property bool showDetails: false
    readonly property bool pinned: backend.pins.indexOf(entryId) >= 0
    readonly property bool hasDetails: (entry.details || []).length > 0
    readonly property bool actionsRevealed: hover.hovered || pinButton.activeFocus || detailsButton.activeFocus
    readonly property bool packTiles: rich && entryId === "appearance.icons"
    // The selected display's scale goes where the Displays page says it can: applied now,
    // or staged with the arrangement, or not at all and why.
    readonly property bool displayScale: rich && entryId === "displays.scale"
    readonly property var scaleInfo: displayScale ? backend.displays.selectedScale : ({})
    onStateIdChanged: if (target.length) backend.refresh(stateId)
    readonly property bool tiles: rich && (entryId === "appearance.theme" || entryId === "appearance.background" || packTiles)
    readonly property string unitSuffix: !entry.unit ? "" : entry.unit === "%" ? "%" : " " + entry.unit

    // Search lands on a row and leaves the keyboard there, ready to change it.
    function takeFocus() {
        if (root.packTiles && packLoader.item) {
            packLoader.item.takeFocus()
            return
        }
        if (control.item && control.item.enabled && root.kind !== "slider" && root.kind !== "float" && root.kind !== "int")
            control.item.forceActiveFocus(Qt.TabFocusReason)
        else if (actionButton.visible && actionButton.enabled)
            actionButton.forceActiveFocus(Qt.TabFocusReason)
        else
            pinButton.forceActiveFocus(Qt.TabFocusReason)
    }

    implicitHeight: Math.max(body.implicitHeight + (root.prominent ? 30 : 18), root.compact ? Math.round(theme.fontSize * 3.2) : theme.rowHeight) + (last ? 0 : 1)
    width: parent ? parent.width : 400

    // Inset, so it never pokes past the corners of a rounded panel.
    Rectangle {
        anchors.fill: parent
        anchors.margins: 3
        radius: theme.controlRadius
        color: root.highlighted ? Qt.alpha(theme.accent, 0.14) : hover.hovered ? Qt.alpha(theme.foreground, 0.04) : "transparent"
        Behavior on color { enabled: theme.animate; ColorAnimation { duration: 200 } }
    }
    // For sizing a dropdown to its longest choice, in the font dropdowns use for values.
    FontMetrics {
        id: choiceMetrics
        font.family: translator.cursiveScript ? theme.uiFont : "monospace"
        font.pixelSize: theme.fontSize
    }
    HoverHandler {
        id: hover
        cursorShape: root.navigation && !root.unavailable ? Qt.PointingHandCursor : Qt.ArrowCursor
    }
    TapHandler {
        enabled: root.navigation && !root.unavailable
        onTapped: win.activate(root.entry)
    }
    // How a status reads at a glance, where the catalog says so per value.
    readonly property string tone: root.entry.tones && root.st.value !== undefined ? (root.entry.tones[root.st.value] || "") : ""
    AppIcon {
        id: icon
        visible: root.rich && name.length > 0
        name: root.headingIcon || (root.file ? "file" : root.entry.icon || "")
        size: Math.round(theme.fontSize * (root.headingTitle.length ? 1.75 : root.file ? 1.3 : root.prominent ? 2 : 1.55))
        tint: root.unavailable ? theme.muted : root.tone === "good" ? theme.goodText : root.tone === "danger" ? theme.dangerText : theme.foreground
        // Something that takes away, marked on its icon as well as its button.
        AppIcon {
            visible: root.entry.confirm === "destructive" && root.kind === "action"
            name: "close"
            size: Math.round(parent.size * 0.5)
            tint: theme.dangerText
            x: parent.width - width * 0.55
            y: parent.height - height * 0.55
        }
        anchors.left: parent.left
        anchors.leftMargin: root.prominent ? 24 : 18
        anchors.verticalCenter: parent.verticalCenter
    }
    Rectangle {
        visible: !root.last
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        height: 1
        color: theme.hairline
    }

    ColumnLayout {
        id: body
        anchors.left: icon.visible ? icon.right : parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: icon.visible ? (root.prominent ? 26 : 16) : 14
        anchors.rightMargin: root.prominent ? 18 : 14
        spacing: 3

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 120
                spacing: 2
                // The label with its quiet actions beside it. They sit in the label's slack
                // and only fade, so hovering moves nothing and every control can line up on
                // the panel's edge. A tap has no hover to reveal them, hence the opacity
                // checks: a touch must not change something it cannot see.
                Item {
                    Layout.fillWidth: true
                    // Under a section of the same name, the tiles say it already.
                    visible: !(root.tiles && root.entryId === "appearance.theme")
                    implicitHeight: title.implicitHeight
                    RowLayout {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        AppText {
                            id: title
                            Layout.maximumWidth: parent.width - quietActions.implicitWidth - 6
                            text: root.headingTitle.length ? root.headingTitle : root.file && root.fileName.length ? root.fileName : root.labelText
                            mono: root.file && root.fileName.length > 0
                            // A long or translated name gets a second line rather than losing its end.
                            wrapMode: root.file ? Text.WrapAnywhere : Text.Wrap
                            scale_: root.headingTitle.length ? 1.45 : root.prominent ? 1.1 : 1
                            font.weight: root.headingTitle.length ? Font.DemiBold : root.prominent ? Font.DemiBold : Font.Normal
                            Accessible.role: root.headingTitle.length ? Accessible.Heading : Accessible.StaticText
                            opacity: root.unavailable ? 0.6 : 1
                        }
                        Row {
                            id: quietActions
                            visible: !root.compact
                            spacing: 2
                            AppButton {
                                objectName: "resetButton"
                                visible: !!root.st.isOverride && !!root.entry.hasReset && root.kind !== "action" && !root.unavailable
                                enabled: !root.busy
                                style: "flat"
                                text: ""
                                implicitWidth: 24
                                implicitHeight: 24
                                iconName: "undo"
                                Accessible.name: qsTr("Use default")
                                ToolTip.visible: hovered
                                ToolTip.text: Accessible.name
                                ToolTip.delay: 500
                                onClicked: backend.resetValue(root.stateId)
                            }
                            AppButton {
                                id: pinButton
                                objectName: "pinButton"
                                style: "flat"
                                text: ""
                                implicitWidth: 24
                                implicitHeight: 24
                                iconName: "pin"
                                opacity: root.pinned ? 1 : root.actionsRevealed ? 0.6 : 0
                                Behavior on opacity { enabled: theme.animate; NumberAnimation { duration: 120 } }
                                Accessible.name: root.pinned ? qsTr("Unpin from Overview") : qsTr("Pin to Overview")
                                ToolTip.visible: hovered
                                ToolTip.text: Accessible.name
                                ToolTip.delay: 500
                                onClicked: if (opacity > 0) backend.togglePin(root.entryId)
                            }
                            AppButton {
                                id: detailsButton
                                objectName: "detailsButton"
                                visible: root.hasDetails
                                style: "flat"
                                text: ""
                                implicitWidth: 24
                                implicitHeight: 24
                                iconName: "info"
                                opacity: root.actionsRevealed || root.showDetails ? 0.7 : 0
                                Behavior on opacity { enabled: theme.animate; NumberAnimation { duration: 120 } }
                                Accessible.name: qsTr("Technical details")
                                ToolTip.visible: hovered
                                ToolTip.text: Accessible.name
                                ToolTip.delay: 500
                                onClicked: if (opacity > 0) root.showDetails = !root.showDetails
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
                AppText {
                    objectName: "fileFolder"
                    Layout.fillWidth: true
                    visible: root.file && root.fileFolder.length > 0
                    text: root.fileFolder
                    mono: true
                    secondary: true
                    scale_: 0.85
                    wrapMode: Text.WrapAnywhere
                }
                AppText {
                    Layout.fillWidth: true
                    visible: !root.compact && text.length > 0 && !root.unavailable
                    // A file whose name does not say what it is keeps its name from the catalog.
                    text: root.file ? [root.fileNamed ? "" : root.labelText, root.descriptionText].filter(function (part) { return part.length > 0 }).join(" · ")
                                    : root.descriptionText
                    secondary: true
                    scale_: root.prominent ? 0.98 : 0.88
                    wrapMode: Text.Wrap
                }
                // State that needs words: why it is unavailable, what it is set by, what failed.
                RowLayout {
                    visible: warningText.text.length > 0
                    Layout.fillWidth: true
                    spacing: 6
                    AppIcon {
                        name: root.st.error ? "warning" : "info"
                        tint: root.st.error ? theme.dangerText : theme.muted
                        size: 14
                    }
                    AppText {
                        id: warningText
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        scale_: 0.88
                        secondary: !root.st.error
                        color: root.st.error ? theme.dangerText : theme.muted
                        text: root.st.error || (root.displayScale && root.scaleInfo.route !== "immediate" ? root.scaleInfo.reason || "" : "") || root.st.reason || root.st.note || ""
                    }
                }
            }

            AppText {
                visible: root.kind === "status" && (root.status === "loading" || !!root.st.loading)
                text: "…"
                secondary: true
                Accessible.name: qsTr("Loading")
            }
            // Control
            Loader {
                id: control
                objectName: "settingControl"
                Layout.alignment: Qt.AlignVCenter
                active: root.kind !== "action" && !root.unavailable && !root.tiles
                visible: active
                sourceComponent: {
                    if (root.displayScale)
                        return displayScaleControl
                    if (root.blurStrength)
                        return blurStrengthControl
                    switch (root.kind) {
                    case "toggle": return toggleControl
                    case "choice": return choiceControl
                    case "int": return stepperControl
                    case "slider": return sliderControl
                    case "float": return sliderControl
                    case "text": return textControl
                    default: return statusControl
                    }
                }
            }
            AppButton {
                id: actionButton
                objectName: "actionButton"
                visible: root.kind === "action"
                enabled: !root.unavailable
                text: root.navigation ? "" : qsTranslate("HelperLabels", root.entry.verb || "Open")
                iconName: root.navigation ? "chevronright" : root.actionIcon()
                implicitWidth: root.navigation ? 34 : implicitContentWidth + 24
                style: root.navigation ? "flat" : root.entry.confirm === "destructive" ? "danger" : "outline"
                Accessible.name: qsTranslate("catalog", root.entry.label || "")
                onClicked: win.activate(root.entry)
            }
        }

        Loader {
            Layout.fillWidth: true
            Layout.topMargin: 8
            Layout.bottomMargin: 4
            active: root.tiles && !root.packTiles
            visible: active
            sourceComponent: ChoiceTiles {
                enabled_: root.editable
                current: root.st.value === undefined ? "" : root.st.value
                choices: root.st.choices || []
                source: function (value) {
                    if (root.entryId === "appearance.theme")
                        return theme.preview(value)
                    // Videos have no still to show, so they fall back to the drawn tile.
                    return /\.(png|jpe?g|webp|gif)$/i.test(value) ? ({ image: "file://" + value }) : ({})
                }
                onPicked: function (value) { win.requestChange(root.stateId, value) }
            }
        }

        Loader {
            id: packLoader
            Layout.fillWidth: true
            Layout.topMargin: 8
            Layout.bottomMargin: 4
            active: root.packTiles
            visible: active
            sourceComponent: IconPackTiles {
                enabled_: !!root.entry.editable && !root.unavailable && root.status !== "custom"
                busy: root.busy
                loading: root.status === "loading" || !!root.st.loading
                current: root.st.value === undefined ? "" : root.st.value
                choices: root.st.choices || []
                details: root.st.extra || ({})
                onPicked: function (value) { win.requestChange(root.stateId, value) }
            }
        }

        // The finer settings this one stands for, such as a strength's radius and passes.
        AppButton {
            objectName: "disclosureButton"
            visible: root.discloses && !root.compact
            style: "link"
            text: root.disclosed ? qsTr("Hide fine-tuning") : qsTr("Fine-tune")
            iconName: "chevron"
            iconRotation: root.disclosed ? 180 : 0
            Accessible.expandable: true
            Accessible.expanded: root.disclosed
            onClicked: root.disclosureToggled()
        }

        // Under the blur status: one way back to Omarchy's glass for every blur row set here.
        Loader {
            Layout.fillWidth: true
            active: root.rich && root.entryId === "appearance.blur.status" && root.status === "ok"
            visible: active && item && item.overrides.length > 0
            sourceComponent: RowLayout {
                readonly property var overrides: {
                    var ids = [], states = backend.states
                    for (var key in states)
                        if (key.indexOf("appearance__blur__") === 0 && states[key] && states[key].isOverride)
                            ids.push(key.replace(/__/g, "."))
                    return ids
                }
                AppButton {
                    objectName: "blurResetAll"
                    style: "link"
                    iconName: "undo"
                    text: qsTr("Reset blur to Omarchy's")
                    onClicked: {
                        for (var i = 0; i < parent.overrides.length; ++i)
                            backend.resetValue(parent.overrides[i])
                    }
                }
                Item { Layout.fillWidth: true }
            }
        }

        // Under the launcher icons switch: what the layer covers, and Iconic if it is here too.
        Loader {
            Layout.fillWidth: true
            Layout.topMargin: 4
            Layout.bottomMargin: 2
            active: root.rich && root.entryId === "appearance.icons.launcher" && root.status === "ok"
            visible: active
            sourceComponent: IconLayerStatus { details: root.st.extra || ({}) }
        }

        AppText {
            // Only worth a line when it explains something: an inherited value is the
            // normal case, and saying so on every row is noise.
            visible: !root.compact && !!root.st.isOverride && root.entry.owned === true && root.status === "ok" && !root.packTiles
            text: root.st.source || ""
            secondary: true
            scale_: 0.88
        }
        Column {
            visible: root.showDetails
            Layout.fillWidth: true
            spacing: 2
            Repeater {
                model: root.showDetails ? (root.entry.details || []) : []
                delegate: AppText {
                    width: parent.width
                    text: modelData
                    secondary: true
                    scale_: 0.88
                    wrapMode: Text.WrapAnywhere
                }
            }
            AppText {
                visible: !!root.entry.domain
                text: qsTr("Scope: %1 · Undo: %2").arg(root.entry.scope || "—").arg(root.entry.recovery || "—")
                secondary: true
                scale_: 0.88
            }
            Repeater {
                model: root.showDetails ? (root.entry.effects || []) : []
                delegate: AppText {
                    width: parent.width
                    text: qsTranslate("catalog", modelData)
                    secondary: true
                    scale_: 0.88
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    function actionIcon() {
        // Where pressing it takes you; something that just happens needs no sign at all.
        switch (root.entry.launchKind) {
        case "page": return "arrow"
        case "terminal": return "terminal"
        case "exec": return ""
        default: return "external"
        }
    }

    Component {
        id: displayScaleControl
        AppCombo {
            label: root.labelText
            proportional: true
            implicitWidth: Math.min(320, Math.max(200, root.width * 0.32))
            readonly property string route: root.scaleInfo.route || ""
            enabled: (route === "immediate" && root.editable) || route === "stage"
            readonly property var choices: root.scaleInfo.choices || []
            model: choices.map(function (choice) { return choice.label })
            currentIndex: {
                var best = -1
                for (var i = 0; i < choices.length; ++i)
                    if (best < 0 || Math.abs(choices[i].value - root.scaleInfo.value) < Math.abs(choices[best].value - root.scaleInfo.value))
                        best = i
                return best
            }
            onActivated: function (i) {
                var value = choices[i].value
                if (route === "immediate")
                    win.requestChange(root.entryId, String(Math.round(value * 100000) / 100000))
                else if (route === "stage")
                    backend.displays.stageEdit(root.scaleInfo.output, "scale", value)
            }
        }
    }
    Component {
        id: blurStrengthControl
        BlurStrength {
            entry: root.entry
            st: root.st
            editable: root.editable
            stateId: root.stateId
        }
    }
    Component {
        id: toggleControl
        AppSwitch {
            on: root.st.value === "true"
            enabled: root.editable
            busy: root.busy
            unknown: root.status === "loading"
            label: qsTranslate("catalog", root.entry.label || "")
            onClicked: win.requestChange(root.stateId, on ? "false" : "true")
        }
    }
    Component {
        id: choiceControl
        AppCombo {
            id: combo
            label: qsTranslate("catalog", root.entry.label || "")
            enabled: root.editable
            textRole: "label"
            valueRole: "value"
            // The chosen app's own icon, where the choices name apps.
            iconRole: "icon"
            leadingImage: currentIndex >= 0 && model[currentIndex] && model[currentIndex].icon ? model[currentIndex].icon : ""
            // As wide as its longest choice needs, within reason, so a short list leaves
            // the description its room and a long label is not cut off.
            readonly property real longest: {
                var widest = 0, list = model || []
                for (var i = 0; i < list.length; ++i)
                    widest = Math.max(widest, choiceMetrics.advanceWidth(list[i].label))
                return widest
            }
            implicitWidth: root.compact ? 220 : Math.min(320, root.width * 0.55, Math.max(170, longest + 60 + (leadingImage.length ? theme.fontSize * 1.5 : 0)))
            readonly property string current: root.st.value === undefined ? "" : root.st.value
            // A value set outside the app may not be in the list; show it as it is
            // rather than pretending it is one of the choices.
            model: {
                var list = []
                var source = root.st.choices || []
                var found = false
                for (var i = 0; i < source.length; ++i) {
                    list.push({ value: source[i].value, label: qsTranslate("catalog", source[i].label), icon: source[i].icon || "" })
                    if (source[i].value === current)
                        found = true
                }
                if (!found && root.status !== "loading" && current.length)
                    list.unshift({ value: current, label: qsTr("Custom: %1").arg(current.replace(/\n/g, " ")) })
                return list
            }
            function sync() {
                // Called while the row is still being built, before the list exists.
                if (!model)
                    return
                for (var i = 0; i < model.length; ++i)
                    if (model[i].value === current) {
                        currentIndex = i
                        return
                    }
                currentIndex = -1
            }
            onModelChanged: sync()
            onCurrentChanged: sync()
            Connections {
                target: root
                function onStChanged() { combo.sync() }
            }
            onActivated: function (index) {
                var value = model[index].value
                if (value !== current)
                    win.requestChange(root.stateId, value)
            }
        }
    }
    Component {
        id: stepperControl
        // One control, not three: the value sits between its own minus and plus.
        Rectangle {
            id: stepper
            property int pending: parseInt(root.st.value) || 0
            implicitWidth: steps.implicitWidth + 2
            implicitHeight: Math.round(theme.fontSize * 2.2) + 2
            radius: theme.controlRadius
            color: "transparent"
            border.width: 1
            border.color: theme.divider
            opacity: root.editable ? 1 : 0.55
            Timer {
                id: settle
                interval: 500
                onTriggered: win.requestChange(root.stateId, String(stepper.pending))
            }
            RowLayout {
                id: steps
                anchors.centerIn: parent
                spacing: 0
                AppButton {
                    style: "flat"
                    text: ""
                    iconName: "minus"
                    implicitWidth: Math.round(theme.fontSize * 2.4)
                    implicitHeight: stepper.height - 2
                    enabled: root.editable && stepper.pending > root.entry.min
                    Accessible.name: qsTr("Decrease %1").arg(qsTranslate("catalog", root.entry.label || ""))
                    onClicked: { stepper.pending -= root.entry.step; settle.restart() }
                }
                Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: theme.divider }
                AppText {
                    Layout.minimumWidth: Math.round(theme.fontSize * 5.4)
                    horizontalAlignment: Text.AlignHCenter
                    mono: true
                    text: stepper.pending.toLocaleString(Qt.locale(), "f", 0) + root.unitSuffix
                }
                Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: theme.divider }
                AppButton {
                    style: "flat"
                    text: ""
                    iconName: "plus"
                    implicitWidth: Math.round(theme.fontSize * 2.4)
                    implicitHeight: stepper.height - 2
                    enabled: root.editable && stepper.pending < root.entry.max
                    Accessible.name: qsTr("Increase %1").arg(qsTranslate("catalog", root.entry.label || ""))
                    onClicked: { stepper.pending += root.entry.step; settle.restart() }
                }
            }
        }
    }
    Component {
        id: sliderControl
        RowLayout {
            spacing: 10
            AppSlider {
                id: slider
                label: qsTranslate("catalog", root.entry.label || "")
                Layout.preferredWidth: root.compact ? 150 : 200
                from: root.entry.min
                to: root.entry.max
                stepSize: root.entry.step
                snapMode: Slider.SnapAlways
                enabled: root.editable
                value: parseFloat(root.st.value) || 0
                onPressedChanged: if (!pressed) win.requestChange(root.stateId, String(root.kind === "float" ? Math.round(value * 1000) / 1000 : Math.round(value)))
                Keys.onReleased: function (event) {
                    if (event.key === Qt.Key_Left || event.key === Qt.Key_Right)
                        win.requestChange(root.stateId, String(root.kind === "float" ? Math.round(value * 1000) / 1000 : Math.round(value)))
                }
            }
            AppText {
                Layout.minimumWidth: 50
                horizontalAlignment: Text.AlignRight
                mono: true
                text: slider.value.toLocaleString(Qt.locale(), "f", root.kind === "float" ? 2 : 0) + root.unitSuffix
            }
        }
    }
    Component {
        id: textControl
        AppField {
            label: qsTranslate("catalog", root.entry.label || "")
            implicitWidth: 240
            enabled: root.editable
            placeholderText: qsTranslate("catalog", root.entry.placeholder || "")
            // An unfinished edit stays put when the file changes underneath it.
            text: activeFocus ? text : (root.st.value || "")
            onEditingFinished: if (text !== (root.st.value || "")) win.requestChange(root.stateId, text)
        }
    }
    Component {
        id: statusControl
        ColumnLayout {
            spacing: 2
        // A toned status reads as a small pill, its word in the tone's color.
        Rectangle {
            visible: root.rich && root.tone.length > 0
            Layout.alignment: Qt.AlignRight
            implicitHeight: Math.round(theme.fontSize * 1.85)
            implicitWidth: pillText.implicitWidth + 22
            radius: theme.controlRadius
            readonly property color ink: root.tone === "good" ? theme.goodText : root.tone === "danger" ? theme.dangerText : theme.muted
            color: root.tone === "neutral" || root.tone === "muted" ? Qt.alpha(theme.foreground, 0.06) : Qt.alpha(ink, 0.12)
            border.width: 1
            border.color: root.tone === "neutral" || root.tone === "muted" ? theme.divider : Qt.alpha(ink, 0.5)
            AppText {
                id: pillText
                anchors.centerIn: parent
                text: root.status === "loading" ? "…" : root.st.display || ""
                color: root.tone === "neutral" ? theme.foreground : parent.ink
                mono: root.tone === "neutral"
                scale_: 0.93
            }
        }
        RowLayout {
            visible: !(root.rich && root.tone.length > 0)
            Layout.alignment: Qt.AlignRight
            spacing: 8
            // A yes-or-no status says yes with a mark as well as a word.
            AppIcon {
                visible: root.rich && root.st.value === "true"
                name: "checkcircle"
                size: Math.round(theme.fontSize * 1.3)
                tint: theme.goodText
            }
            AppText {
                text: root.status === "unknown" ? qsTr("Unknown") : (root.st.display || "")
                secondary: !(root.rich && root.st.value === "true")
                mono: !(root.rich && (root.st.value === "true" || root.st.value === "false"))
                horizontalAlignment: Text.AlignRight
                Layout.maximumWidth: root.width * 0.5
            }
        }
        // Where this is managed, when that is somewhere else.
        AppButton {
            visible: root.rich && !!root.entry.link && !!root.entry.link.page
            Layout.alignment: Qt.AlignRight
            style: "link"
            text: root.entry.link ? qsTranslate("catalog", root.entry.link.label || "") : ""
            iconName: "external"
            Accessible.name: qsTr("Manage in %1").arg(text)
            onClicked: win.go(root.entry.link.page, root.entry.link.focus || "")
        }
        }
    }
}
