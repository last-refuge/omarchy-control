import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The arrangement: every connected display drawn to scale beside the selected one's
// settings. Edits are staged in the provider and reach Hyprland only on Preview, which
// undoes itself unless it is kept. The selection is this window's own and never follows
// the desktop's keyboard focus.
ColumnLayout {
    id: root
    spacing: 10
    readonly property var displays: backend.displays
    readonly property var rows: displays.staged
    readonly property int count: displays.monitors.length
    readonly property bool multi: count > 1
    readonly property bool unlocked: displays.editingUnlocked
    readonly property bool checking: displays.checkState === "running"
    readonly property bool canEdit: unlocked && !displays.busy
    readonly property bool narrow: width < theme.fontSize * 50
    readonly property var outcome: displays.outcome
    readonly property bool checkFailed: !unlocked && !checking
                                        && (outcome.scope === "check" || (!!displays.lastCheck.current && displays.lastCheck.passed === false))
    readonly property var selectedRow: rowFor(displays.selected)

    // Delegates are keyed by name, so a staged edit or a drag never rebuilds them.
    property var names: []
    function syncNames() {
        var next = rows.map(function (row) { return row.output })
        if (JSON.stringify(next) !== JSON.stringify(names))
            names = next
    }
    onRowsChanged: syncNames()
    Component.onCompleted: syncNames()

    function rowFor(output) {
        for (var i = 0; i < rows.length; ++i)
            if (rows[i].output === output)
                return rows[i]
        return null
    }
    function takeFocus() {
        canvas.focusSelected()
    }
    // "2880x1800@120.00" as a person would say it.
    function modeLabel(mode) {
        var parts = /^(\d+)x(\d+)@([\d.]+)/.exec(mode)
        return parts ? parts[1] + " × " + parts[2] + " · " + Math.round(parseFloat(parts[3]) * 100) / 100 + " Hz" : mode
    }
    function percent(value) {
        return Math.round(value * 100) + "%"
    }
    function kindOf(row) {
        return row.internal ? qsTr("Built-in display") : qsTr("External display")
    }
    function pending(row, field) {
        return !!row && (row.pending || []).indexOf(field) >= 0
    }

    BlockHeading {
        Layout.fillWidth: true
        title: qsTr("Arrangement")
        AppText {
            visible: root.count > 0
            height: Math.round(theme.fontSize * 2)
            text: qsTr("%n display(s) connected", "", root.count)
            secondary: true
            scale_: 0.95
        }
    }
    AppText {
        visible: displays.status === "unavailable"
        text: qsTr("Hyprland is not reachable, so displays cannot be shown.")
        color: theme.dangerText
        Layout.fillWidth: true
        wrapMode: Text.Wrap
    }

    // Where the safety check stands: needed, running, failed, or passed.
    AppPanel {
        id: notice
        objectName: "displaySafety"
        Layout.fillWidth: true
        visible: displays.status === "ok" && root.count > 0
        readonly property bool compact: root.unlocked && !root.checking
        padding: compact ? 7 : 11
        RowLayout {
            width: parent.width
            spacing: 12
            AppIcon {
                Layout.leftMargin: 6
                Layout.alignment: notice.compact ? Qt.AlignVCenter : Qt.AlignTop
                Layout.topMargin: notice.compact ? 0 : 2
                name: root.checkFailed ? "warning" : notice.compact ? "shieldcheck" : "shield"
                tint: root.checkFailed ? theme.dangerText : notice.compact ? theme.goodText : theme.foreground
                size: Math.round(theme.fontSize * 1.45)
            }
            GridLayout {
                Layout.fillWidth: true
                columns: notice.compact && !root.narrow ? 2 : 1
                columnSpacing: 14
                rowSpacing: 2
                AppText {
                    objectName: "displaySafetyTitle"
                    font.weight: Font.Medium
                    scale_: notice.compact ? 1 : 1.07
                    Accessible.role: Accessible.StaticText
                    text: root.checking ? qsTr("Running the recovery check")
                        : root.checkFailed ? (root.outcome.scope === "check" ? root.outcome.title : qsTr("The recovery check failed"))
                        : root.unlocked ? qsTr("Safety check passed")
                        : qsTr("Unlock display arrangement")
                }
                AppText {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    secondary: true
                    scale_: 0.93
                    text: root.checking ? (displays.previewSeconds > 0
                                           ? qsTr("One display moves a few pixels, then the safety timer has to put it back. %n second(s) left.", "", displays.previewSeconds)
                                           : qsTr("Confirming that the display is back where it was…"))
                        : root.checkFailed ? (root.outcome.scope === "check" ? root.outcome.detail : displays.lastCheck.reason || "")
                                             + " " + qsTr("Arranging stays locked until the check passes.")
                        : root.unlocked ? (displays.previewing ? qsTr("A layout preview is running. Keep it or restore the previous one below.")
                                           : root.multi ? qsTr("Select a display to edit. Drag displays to arrange.")
                                           : qsTr("Change the display's settings, then preview them."))
                        : qsTr("Run a 3-second recovery check before changing the layout. It moves one display a few pixels, then restores it. Windows may shift slightly.")
                }
                // How far the check has to go.
                Rectangle {
                    visible: root.checking
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    Layout.preferredHeight: 3
                    radius: 1.5
                    color: Qt.alpha(theme.foreground, 0.12)
                    Rectangle {
                        height: parent.height
                        radius: parent.radius
                        color: theme.accent
                        width: parent.width * (displays.previewTotal > 0 ? 1 - displays.previewSeconds / displays.previewTotal : 1)
                        Behavior on width { enabled: theme.animate; NumberAnimation { duration: 900 } }
                    }
                }
            }
            AppButton {
                objectName: "runCheckButton"
                visible: !root.unlocked
                Layout.alignment: Qt.AlignVCenter
                style: "primary"
                enabled: !displays.busy
                busy: root.checking
                text: root.checking ? qsTr("Checking…") : root.checkFailed ? qsTr("Run check again") : qsTr("Run check")
                onClicked: displays.runRecoveryCheck()
            }
        }
    }

    GridLayout {
        id: workspace
        visible: displays.status === "ok" && root.count > 0
        Layout.fillWidth: true
        columns: root.narrow ? 1 : 2
        columnSpacing: 14
        rowSpacing: 10

        // The displays at their real proportions. The bar a display draws on has a
        // physical left and right, so this never mirrors with the language.
        Rectangle {
            id: canvas
            objectName: "displayCanvas"
            LayoutMirroring.enabled: false
            LayoutMirroring.childrenInherit: true
            Layout.fillWidth: true
            Layout.fillHeight: !root.narrow
            Layout.minimumHeight: Math.round(theme.fontSize * (root.narrow ? 14 : 17))
            Layout.preferredHeight: root.narrow ? Math.round(theme.fontSize * 15) : Math.max(inspector.implicitHeight, Math.round(theme.fontSize * 17))
            color: theme.deep
            radius: theme.panelRadius
            border.width: 1
            border.color: theme.hairline
            clip: true

            readonly property var placed: root.rows.filter(function (row) { return row.enabled })
            readonly property real pad: Math.round(theme.fontSize * 1.7)
            readonly property real footerSpace: footer.visible ? footer.height + 10 : 0
            readonly property var liveBounds: {
                var minX = 0, minY = 0, maxX = 1, maxY = 1
                for (var i = 0; i < placed.length; ++i) {
                    var r = placed[i]
                    if (i === 0) {
                        minX = r.x; minY = r.y; maxX = r.x + r.logicalWidth; maxY = r.y + r.logicalHeight
                        continue
                    }
                    minX = Math.min(minX, r.x); minY = Math.min(minY, r.y)
                    maxX = Math.max(maxX, r.x + r.logicalWidth); maxY = Math.max(maxY, r.y + r.logicalHeight)
                }
                return { minX: minX, minY: minY, width: Math.max(1, maxX - minX), height: Math.max(1, maxY - minY) }
            }
            readonly property real areaWidth: Math.max(10, width - pad * 2)
            readonly property real areaHeight: Math.max(10, height - pad * 2 - footerSpace)
            // A map no bigger than a large display's tile would be: one lone display
            // should look like a display, not fill the well edge to edge.
            readonly property real liveUnit: Math.min(areaWidth / liveBounds.width, areaHeight / liveBounds.height,
                                                      placed.length === 1 ? Math.min(areaWidth * 0.55 / liveBounds.width, (height - pad * 2) / liveBounds.height) : 1e9)
            // While a display is dragged the map holds still, or it would slide under the pointer.
            property var frozen: null
            readonly property var bounds: frozen ? frozen.bounds : liveBounds
            readonly property real unit: frozen ? frozen.unit : liveUnit
            readonly property real originX: frozen ? frozen.originX : pad + (areaWidth - bounds.width * unit) / 2
            readonly property real originY: frozen ? frozen.originY : pad + (areaHeight - bounds.height * unit) / 2

            function focusSelected() {
                for (var i = 0; i < tiles.count; ++i) {
                    var tile = tiles.itemAt(i)
                    if (tile && tile.output === displays.selected && tile.visible) {
                        tile.forceActiveFocus(Qt.TabFocusReason)
                        return
                    }
                }
            }
            function selectBy(from, step) {
                var list = root.names
                var at = list.indexOf(from)
                if (!list.length)
                    return
                displays.select(list[(at + step + list.length) % list.length])
                focusSelected()
            }
            // Edges within a few pixels of another display's edge meet it exactly.
            function snap(output, x, y, w, h) {
                var reach = 12 / Math.max(unit, 0.0001)
                var bestX = x, bestY = y, dx = reach, dy = reach
                for (var i = 0; i < placed.length; ++i) {
                    var o = placed[i]
                    if (o.output === output)
                        continue
                    var xs = [o.x - w, o.x + o.logicalWidth, o.x, o.x + o.logicalWidth - w]
                    var ys = [o.y - h, o.y + o.logicalHeight, o.y, o.y + o.logicalHeight - h]
                    for (var a = 0; a < 4; ++a) {
                        if (Math.abs(xs[a] - x) < dx) { dx = Math.abs(xs[a] - x); bestX = xs[a] }
                        if (Math.abs(ys[a] - y) < dy) { dy = Math.abs(ys[a] - y); bestY = ys[a] }
                    }
                }
                return Qt.point(Math.round(bestX), Math.round(bestY))
            }

            Repeater {
                id: tiles
                model: root.names
                delegate: Item {
                    id: tile
                    objectName: "displayTile_" + modelData
                    readonly property string output: modelData
                    readonly property var row: root.rowFor(modelData)
                    // A lone display is simply the display; selection only means something among several.
                    readonly property bool isSelected: displays.selected === modelData && root.multi
                    readonly property bool movable: root.canEdit && root.multi
                    readonly property real inset: 3
                    readonly property real lineHeight: theme.fontSize * 1.55
                    visible: !!row && row.enabled
                    x: row ? canvas.originX + (row.x - canvas.bounds.minX) * canvas.unit + inset : 0
                    y: row ? canvas.originY + (row.y - canvas.bounds.minY) * canvas.unit + inset : 0
                    width: row ? Math.max(8, row.logicalWidth * canvas.unit - inset * 2) : 0
                    height: row ? Math.max(8, row.logicalHeight * canvas.unit - inset * 2) : 0
                    z: drag.active ? 2 : isSelected ? 1 : 0
                    activeFocusOnTab: visible
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: row ? qsTr("%1, %2 by %3").arg(row.output).arg(row.logicalWidth).arg(row.logicalHeight) : ""
                    Accessible.description: movable ? qsTr("Arrow keys move it. Shift moves it further.") : ""
                    Accessible.checkable: true
                    Accessible.checked: isSelected
                    Accessible.onPressAction: displays.select(modelData)

                    Keys.onPressed: function (event) {
                        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                            displays.select(modelData)
                            event.accepted = true
                            return
                        }
                        var horizontal = event.key === Qt.Key_Left ? -1 : event.key === Qt.Key_Right ? 1 : 0
                        var vertical = event.key === Qt.Key_Up ? -1 : event.key === Qt.Key_Down ? 1 : 0
                        if (!horizontal && !vertical)
                            return
                        event.accepted = true
                        // Arrows walk between displays until one can be moved, then move it.
                        if (!movable) {
                            canvas.selectBy(modelData, horizontal + vertical)
                            return
                        }
                        if (displays.selected !== modelData)
                            displays.select(modelData)
                        var step = event.modifiers & Qt.ShiftModifier ? 100 : 10
                        displays.stagePosition(modelData, row.x + horizontal * step, row.y + vertical * step)
                    }

                    Rectangle {
                        anchors.fill: parent
                        radius: theme.controlRadius
                        color: tile.isSelected ? Qt.alpha(theme.accent, drag.active ? 0.22 : 0.13)
                             : hover.hovered ? Qt.alpha(theme.foreground, 0.07) : Qt.alpha(theme.foreground, 0.045)
                        border.width: tile.isSelected ? 2 : 1
                        border.color: tile.isSelected ? theme.accent : Qt.alpha(theme.foreground, 0.2)
                        Behavior on color { enabled: theme.animate; ColorAnimation { duration: 120 } }
                    }
                    // Keyboard focus is its own ring, outside the selection's.
                    Rectangle {
                        visible: tile.activeFocus
                        anchors.fill: parent
                        anchors.margins: -4
                        radius: theme.controlRadius + 3
                        color: "transparent"
                        border.width: 2
                        border.color: theme.foreground
                        opacity: 0.8
                    }
                    // Something about this display is staged and not applied yet.
                    Rectangle {
                        visible: !!tile.row && tile.row.pending.length > 0
                        width: 8
                        height: 8
                        radius: 4
                        x: parent.width - width - 8
                        y: 8
                        color: theme.accent
                    }
                    Column {
                        anchors.centerIn: parent
                        width: parent.width - 12
                        spacing: 2
                        readonly property int lines: Math.floor((tile.height - 10) / tile.lineHeight)
                        AppText {
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            text: tile.output
                            fontSizeMode: Text.HorizontalFit
                            minimumPixelSize: Math.round(theme.fontSize * 0.7)
                            font.weight: Font.DemiBold
                            scale_: parent.lines >= 3 && tile.width > theme.fontSize * 7 ? 1.25 : 1
                        }
                        AppText {
                            visible: parent.lines >= 2
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            // A narrow portrait tile shrinks the numbers rather than cutting them off.
                            fontSizeMode: Text.HorizontalFit
                            minimumPixelSize: Math.round(theme.fontSize * 0.7)
                            text: tile.row ? tile.row.logicalWidth + " × " + tile.row.logicalHeight : ""
                            secondary: !tile.isSelected
                            scale_: 0.95
                        }
                        Item {
                            visible: parent.lines >= 4
                            width: parent.width
                            height: Math.round(theme.fontSize * 2)
                            // A selected display says so in words as well as color.
                            Rectangle {
                                visible: tile.isSelected && root.multi
                                anchors.centerIn: parent
                                height: Math.round(theme.fontSize * 1.65)
                                width: chosen.implicitWidth + 22
                                radius: height / 2
                                color: Qt.alpha(theme.accent, 0.2)
                                border.width: 1
                                border.color: Qt.alpha(theme.accent, 0.55)
                                Row {
                                    id: chosen
                                    anchors.centerIn: parent
                                    spacing: 6
                                    AppIcon { name: "checkcircle"; size: Math.round(theme.fontSize * 1.05); tint: theme.foreground; anchors.verticalCenter: parent.verticalCenter }
                                    AppText { text: qsTr("Selected"); scale_: 0.88; anchors.verticalCenter: parent.verticalCenter }
                                }
                            }
                            Row {
                                visible: !(tile.isSelected && root.multi) && !!tile.row && tile.row.internal
                                anchors.centerIn: parent
                                spacing: 8
                                AppIcon { name: "displays"; size: Math.round(theme.fontSize * 1.1); tint: theme.foreground; anchors.verticalCenter: parent.verticalCenter }
                                AppText { text: qsTr("Built-in display"); scale_: 0.95; anchors.verticalCenter: parent.verticalCenter }
                            }
                        }
                    }
                    HoverHandler {
                        id: hover
                        cursorShape: tile.movable ? (drag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor) : Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: displays.select(modelData)
                    }
                    DragHandler {
                        id: drag
                        enabled: tile.movable
                        target: null
                        property point origin
                        onActiveChanged: {
                            if (active) {
                                displays.select(modelData)
                                origin = Qt.point(tile.row.x, tile.row.y)
                                canvas.frozen = { bounds: canvas.liveBounds, unit: canvas.liveUnit, originX: canvas.originX, originY: canvas.originY }
                            } else {
                                canvas.frozen = null
                            }
                        }
                        onTranslationChanged: {
                            if (!active || canvas.unit <= 0)
                                return
                            var point = canvas.snap(modelData, origin.x + translation.x / canvas.unit, origin.y + translation.y / canvas.unit,
                                                    tile.row.logicalWidth, tile.row.logicalHeight)
                            displays.stagePosition(modelData, point.x, point.y)
                        }
                    }
                }
            }

            // Along the bottom: whether moving is allowed yet, and the displays that are
            // off, so they can still be found and turned back on.
            Row {
                id: footer
                visible: !root.unlocked || offRepeater.count > 0
                x: 14
                y: parent.height - height - 10
                width: parent.width - 28
                spacing: 16
                Row {
                    visible: !root.unlocked
                    spacing: 8
                    anchors.verticalCenter: parent.verticalCenter
                    AppIcon { name: "lock"; size: Math.round(theme.fontSize * 1.05); tint: theme.muted; anchors.verticalCenter: parent.verticalCenter }
                    AppText { text: qsTr("Arrangement locked"); secondary: true; scale_: 0.92; anchors.verticalCenter: parent.verticalCenter }
                }
                Repeater {
                    id: offRepeater
                    model: root.rows.filter(function (row) { return !row.enabled })
                    delegate: AbstractButton {
                        id: offTile
                        objectName: "displayOff_" + modelData.output
                        readonly property bool isSelected: displays.selected === modelData.output
                        implicitHeight: Math.round(theme.fontSize * 2.1)
                        implicitWidth: offLabel.implicitWidth + 24
                        focusPolicy: Qt.StrongFocus
                        hoverEnabled: true
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: qsTr("%1, off").arg(modelData.output)
                        Accessible.checkable: true
                        Accessible.checked: isSelected
                        onClicked: displays.select(modelData.output)
                        background: Rectangle {
                            radius: theme.controlRadius
                            color: offTile.isSelected ? Qt.alpha(theme.accent, 0.13) : offTile.hovered ? Qt.alpha(theme.foreground, 0.06) : "transparent"
                            border.width: offTile.visualFocus || offTile.isSelected ? 2 : 1
                            border.color: offTile.visualFocus ? theme.foreground : offTile.isSelected ? theme.accent : Qt.alpha(theme.foreground, 0.22)
                        }
                        contentItem: AppText {
                            id: offLabel
                            horizontalAlignment: Text.AlignHCenter
                            text: qsTr("%1 · Off").arg(modelData.output)
                            secondary: !offTile.isSelected
                            scale_: 0.92
                        }
                    }
                }
            }
        }

        // The selected display, as it will be once the staged layout is previewed.
        AppPanel {
            id: inspector
            objectName: "displayInspector"
            Layout.fillWidth: root.narrow
            Layout.fillHeight: !root.narrow
            Layout.preferredWidth: root.narrow ? -1 : Math.max(Math.round(theme.fontSize * 20), Math.round(workspace.width * 0.36))
            Layout.alignment: Qt.AlignTop
            padding: 16
            readonly property var row: root.selectedRow
            readonly property var scaleInfo: displays.selectedScale
            ColumnLayout {
                visible: !!inspector.row
                width: parent.width
                spacing: 4
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        AppText {
                            objectName: "inspectorOutput"
                            Layout.fillWidth: true
                            text: inspector.row ? inspector.row.output : ""
                            font.weight: Font.DemiBold
                            scale_: 1.3
                            Accessible.role: Accessible.Heading
                        }
                        AppText {
                            Layout.fillWidth: true
                            text: inspector.row ? (inspector.row.description || root.kindOf(inspector.row)) : ""
                            secondary: true
                            scale_: 0.95
                        }
                        Rectangle {
                            visible: root.multi
                            Layout.topMargin: 5
                            implicitHeight: Math.round(theme.fontSize * 1.6)
                            implicitWidth: selectedLabel.implicitWidth + 22
                            radius: height / 2
                            color: Qt.alpha(theme.accent, 0.16)
                            border.width: 1
                            border.color: Qt.alpha(theme.accent, 0.5)
                            AppText {
                                id: selectedLabel
                                anchors.centerIn: parent
                                text: qsTr("Selected display")
                                scale_: 0.85
                            }
                        }
                    }
                    AppSwitch {
                        objectName: "inspectorEnabled"
                        Layout.alignment: Qt.AlignTop
                        label: inspector.row ? qsTr("Use %1").arg(inspector.row.output) : ""
                        on: !!inspector.row && inspector.row.enabled
                        enabled: root.canEdit
                        onClicked: displays.stageEdit(inspector.row.output, "enabled", !on)
                    }
                }

                AppText {
                    visible: !!inspector.row && !inspector.row.enabled
                    Layout.fillWidth: true
                    Layout.topMargin: 10
                    wrapMode: Text.Wrap
                    secondary: true
                    text: inspector.row ? (root.unlocked ? qsTr("%1 is off. Turn it on to place it in the layout.").arg(inspector.row.output)
                                                         : qsTr("%1 is off. Run the recovery check to turn it back on here.").arg(inspector.row.output)) : ""
                }

                ColumnLayout {
                    visible: !!inspector.row && inspector.row.enabled
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    spacing: 4
                    FieldLabel { text: qsTr("Resolution & refresh"); changed: root.pending(inspector.row, "mode") }
                    AppCombo {
                        objectName: "inspectorMode"
                        Layout.fillWidth: true
                        proportional: true
                        label: inspector.row ? qsTr("Resolution of %1").arg(inspector.row.output) : ""
                        enabled: root.canEdit
                        readonly property var modes: {
                            if (!inspector.row)
                                return []
                            var list = inspector.row.modes.slice()
                            // A mode in use but not listed, as nested outputs have, still shows.
                            if (list.indexOf(inspector.row.mode) < 0)
                                list.unshift(inspector.row.mode)
                            return list
                        }
                        model: modes.map(root.modeLabel)
                        currentIndex: inspector.row ? modes.indexOf(inspector.row.mode) : -1
                        onActivated: function (i) { displays.stageEdit(inspector.row.output, "mode", modes[i]) }
                    }
                    FieldLabel { text: qsTr("Scale"); changed: root.pending(inspector.row, "scale"); Layout.topMargin: 6 }
                    AppCombo {
                        objectName: "inspectorScale"
                        Layout.fillWidth: true
                        proportional: true
                        label: inspector.row ? qsTr("Scale of %1").arg(inspector.row.output) : ""
                        enabled: root.canEdit
                        readonly property var choices: inspector.scaleInfo.choices || []
                        model: choices.map(function (choice) { return choice.label })
                        currentIndex: {
                            var best = -1
                            for (var i = 0; i < choices.length; ++i)
                                if (best < 0 || Math.abs(choices[i].value - inspector.row.scale) < Math.abs(choices[best].value - inspector.row.scale))
                                    best = i
                            return best
                        }
                        onActivated: function (i) { displays.stageEdit(inspector.row.output, "scale", choices[i].value) }
                    }
                    FieldLabel { text: qsTr("Rotation"); changed: root.pending(inspector.row, "transform"); Layout.topMargin: 6 }
                    AppCombo {
                        objectName: "inspectorRotation"
                        Layout.fillWidth: true
                        proportional: true
                        label: inspector.row ? qsTr("Rotation of %1").arg(inspector.row.output) : ""
                        enabled: root.canEdit
                        readonly property var names: [qsTr("Normal"), qsTr("90°"), qsTr("180°"), qsTr("270°"),
                                                      qsTr("Flipped"), qsTr("Flipped, 90°"), qsTr("Flipped, 180°"), qsTr("Flipped, 270°")]
                        // Flipped turns are only offered to a display already using one.
                        readonly property int offered: inspector.row && inspector.row.transform >= 4 ? 8 : 4
                        model: names.slice(0, offered)
                        currentIndex: inspector.row ? Math.min(7, inspector.row.transform) : 0
                        onActivated: function (i) { displays.stageEdit(inspector.row.output, "transform", i) }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: 12
                        Layout.preferredHeight: 1
                        color: theme.hairline
                    }
                    AppText {
                        objectName: "inspectorWorkspace"
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        wrapMode: Text.Wrap
                        secondary: true
                        scale_: 0.9
                        text: inspector.row ? qsTr("%1 × %2 workspace at %3").arg(inspector.row.logicalWidth).arg(inspector.row.logicalHeight).arg(root.percent(inspector.row.scale)) : ""
                    }
                }
            }
        }
    }

    // What just happened, said once and kept until dismissed: an ended preview, a
    // refused layout, a display unplugged with edits pending.
    AppPanel {
        id: result
        objectName: "displayOutcome"
        Layout.fillWidth: true
        visible: !!root.outcome.kind && root.outcome.scope !== "check" && displays.status === "ok"
        padding: 8
        RowLayout {
            width: parent.width
            spacing: 12
            AppIcon {
                Layout.leftMargin: 8
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 3
                name: root.outcome.kind === "info" ? "info" : "warning"
                tint: root.outcome.kind === "error" ? theme.dangerText : root.outcome.kind === "warning" ? theme.warningText : theme.muted
                size: Math.round(theme.fontSize * 1.2)
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                AppText {
                    objectName: "displayOutcomeTitle"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.weight: Font.Medium
                    text: root.outcome.title || ""
                }
                AppText {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    secondary: true
                    scale_: 0.93
                    text: root.outcome.detail || ""
                }
            }
            AppButton {
                Layout.alignment: Qt.AlignVCenter
                style: "flat"
                text: qsTr("Dismiss")
                onClicked: displays.dismissOutcome()
            }
        }
    }

    // What is staged or being tried, with what to do next.
    AppPanel {
        id: changes
        objectName: "displayChanges"
        Layout.fillWidth: true
        readonly property string mode: displays.previewing && !root.checking ? "preview" : displays.dirty ? "pending" : ""
        visible: mode.length > 0 && displays.status === "ok"
        padding: 8
        GridLayout {
            width: parent.width
            columns: root.narrow ? 1 : 2
            columnSpacing: 12
            rowSpacing: 8
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 8
                spacing: 12
                GridLayout {
                    Layout.fillWidth: true
                    columns: root.narrow ? 1 : 3
                    columnSpacing: 14
                    rowSpacing: 2
                    AppText {
                        objectName: "displayChangesTitle"
                        font.weight: Font.Medium
                        text: changes.mode === "preview" ? (displays.previewCanKeep ? qsTr("Keep this layout?") : qsTr("Display recovery is pending")) : qsTr("Changes not applied")
                    }
                    Rectangle {
                        visible: !root.narrow
                        Layout.preferredWidth: 1
                        Layout.preferredHeight: Math.round(theme.fontSize * 1.5)
                        color: theme.divider
                    }
                    AppText {
                        objectName: "displayChangesDetail"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        scale_: 0.93
                        readonly property bool problem: changes.mode === "pending" && displays.stageProblem.length > 0
                        color: problem ? theme.dangerText : theme.muted
                        text: changes.mode === "preview" ? (displays.previewCanKeep ? qsTr("The previous layout comes back in %n second(s) unless you keep this one.", "", displays.previewSeconds)
                            : displays.previewState === "keeping" ? qsTr("Saving the confirmed layout…") : qsTr("Restoration could not be confirmed. Inspect your displays or retry Restore."))
                            : problem ? displays.stageProblem : qsTr("Unconfirmed changes restore automatically.")
                    }
                }
            }
            RowLayout {
                Layout.alignment: root.narrow ? Qt.AlignRight : Qt.AlignVCenter
                spacing: 8
                AppButton {
                    visible: changes.mode === "pending"
                    text: qsTr("Discard")
                    onClicked: displays.discardStaged()
                }
                AppButton {
                    objectName: "previewLayoutButton"
                    visible: changes.mode === "pending"
                    style: "primary"
                    text: qsTr("Preview layout")
                    enabled: root.canEdit && displays.stageProblem.length === 0
                    onClicked: win.withOwnership(function () { displays.previewStaged() })
                }
                AppButton {
                    visible: changes.mode === "preview"
                    text: qsTr("Restore")
                    enabled: displays.previewState !== "keeping"
                    onClicked: displays.revert()
                }
                AppButton {
                    objectName: "keepLayoutButton"
                    visible: changes.mode === "preview"
                    style: "primary"
                    text: qsTr("Keep")
                    enabled: displays.previewCanKeep
                    onClicked: displays.keep()
                }
            }
        }
        // Time left before the preview undoes itself.
        Rectangle {
            visible: changes.mode === "preview"
            x: 8
            width: (parent.width - 16) * (displays.previewTotal > 0 ? displays.previewSeconds / displays.previewTotal : 0)
            height: 2
            color: theme.accent
            Behavior on width { enabled: theme.animate; NumberAnimation { duration: 900 } }
        }
    }

    component FieldLabel: RowLayout {
        property alias text: label.text
        property bool changed: false
        Layout.fillWidth: true
        spacing: 8
        AppText {
            id: label
            font.weight: Font.Medium
            scale_: 0.93
        }
        // Staged and not applied yet, in words as well as color.
        Rectangle {
            visible: parent.changed
            implicitHeight: Math.round(theme.fontSize * 1.35)
            implicitWidth: changedLabel.implicitWidth + 14
            radius: theme.controlRadius
            color: Qt.alpha(theme.accent, 0.16)
            AppText {
                id: changedLabel
                anchors.centerIn: parent
                text: qsTr("Changed")
                color: theme.accentText
                scale_: 0.8
            }
        }
        Item { Layout.fillWidth: true }
    }
}
