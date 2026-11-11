import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Choosing an icon pack by seeing its icons. Each tile draws samples from its own pack
// and nothing else, so looking around changes nothing. Picking one applies it, and it is
// only shown as picked once the engine has read the result back.
Column {
    id: root
    property var choices: []
    property string current: ""
    property var details: ({})
    property bool enabled_: true
    property bool busy: false
    property bool loading: false
    signal picked(string value)

    // A pack the user installed can go, unless something needs it, which the app checks
    // first. It moves to the Trash, and undo in History brings it back.
    function requestRemoval(id) {
        var plan = backend.planIconRemoval(id)
        if (!plan.ok) {
            toast.show(plan.reason, "warning")
            return
        }
        confirm.open_(qsTr("Remove %1").arg(plan.name),
                      qsTr("%1 moves to the Trash. Undo in History brings it back.").arg(plan.name),
                      [plan.path], qsTr("Move to Trash"), true, function () { backend.removeIconPack(id) })
    }

    // Refreshing hands over a fresh list every time. Rebuilding the tiles for one with the
    // same contents would throw away keyboard focus, so it only changes when they do.
    property var packs: []
    onChoicesChanged: {
        var next = JSON.stringify(choices)
        if (next !== signature) {
            signature = next
            packs = choices
        }
    }
    property string signature: ""

    readonly property var importer: backend.iconImporter
    readonly property string desktopPack: details.desktopDefault || ""
    // A deliberate pick of the current pack still means something when things disagree.
    readonly property bool reconcile: !!details.reconcile

    property string query: ""
    property bool showAll: false
    readonly property int columns: Math.max(2, Math.floor((width + tiles.spacing) / (theme.fontSize * 13 + tiles.spacing)))
    readonly property real tileWidth: (width - tiles.spacing * (columns - 1)) / columns
    readonly property int limit: columns * 2

    readonly property var filtered: {
        var text = query.trim().toLowerCase()
        var active = current
        return packs.filter(function (choice) {
            // What is in use stays visible while searching, so it is never lost track of.
            return choice.value === "follow-theme" || choice.value === active || !text.length
                || choice.label.toLowerCase().indexOf(text) >= 0 || (choice.id || "").toLowerCase().indexOf(text) >= 0
        })
    }
    readonly property var wanted: {
        if (showAll || query.trim().length || filtered.length <= limit)
            return filtered
        var list = filtered.slice(0, limit)
        var active = current
        if (!list.some(function (choice) { return choice.value === active })) {
            var match = filtered.filter(function (choice) { return choice.value === active })
            if (match.length)
                list[limit - 1] = match[0]
        }
        return list
    }
    // Rebuilt only when what is listed really changes, and with the keyboard put back on the
    // same pack afterwards, since the tiles it was on have just been replaced.
    property var shown: []
    property string shownSignature: ""
    onWantedChanged: {
        var next = JSON.stringify(wanted)
        if (next === shownSignature)
            return
        var focused = ""
        for (var i = 0; i < tileRepeater.count; ++i) {
            var item = tileRepeater.itemAt(i)
            if (item && item.activeFocus)
                focused = item.value
        }
        shownSignature = next
        shown = wanted
        if (focused.length)
            Qt.callLater(root.refocus, focused)
    }
    function refocus(value) {
        for (var i = 0; i < tileRepeater.count; ++i) {
            var item = tileRepeater.itemAt(i)
            if (item && item.value === value)
                item.forceActiveFocus(Qt.TabFocusReason)
        }
    }
    // The first entry is always Follow desktop theme, so this means no pack to switch to.
    readonly property bool noOtherPacks: !loading && packs.length > 0 && packs.length <= 2

    // Search lands on the pack in use, or on the first one if none of them is.
    function takeFocus() {
        var target = tileRepeater.count ? tileRepeater.itemAt(0) : null
        for (var i = 0; i < tileRepeater.count; ++i)
            if (tileRepeater.itemAt(i) && tileRepeater.itemAt(i).selected)
                target = tileRepeater.itemAt(i)
        if (target)
            target.forceActiveFocus(Qt.TabFocusReason)
    }

    spacing: 8

    AppField {
        id: filter
        objectName: "iconPackSearch"
        // Stays while there is a query, even if the window has grown enough not to need it,
        // or there would be no way to clear what it is hiding.
        visible: root.packs.length > root.limit || root.query.length > 0
        width: Math.min(root.width, theme.fontSize * 22)
        label: qsTr("Search icon packs")
        placeholderText: label
        onTextChanged: root.query = text
    }

    Item {
        id: dropZone
        width: root.width
        height: tiles.implicitHeight

        DropArea {
            id: drop
            anchors.fill: parent
            onEntered: function (drag) { drag.accepted = drag.hasUrls && !root.importer.busy }
            onDropped: function (event) {
                if (root.importer.busy)
                    return
                if (event.urls.length !== 1)
                    root.importer.reject(qsTr("Drop one folder or archive at a time."))
                else
                    root.importer.choose(event.urls[0])
                event.accept(Qt.CopyAction)
            }
        }

        Flow {
            id: tiles
            width: parent.width
            spacing: 10

            Repeater {
                id: tileRepeater
                model: root.shown
                delegate: AbstractButton {
                    id: tile
                    objectName: "iconPackTile"
                    readonly property string value: modelData.value
                    readonly property bool follows: modelData.value === "follow-theme"
                    readonly property string packId: follows ? root.desktopPack : (modelData.id || "")
                    readonly property bool selected: modelData.value === root.current
                    readonly property bool removable: !!modelData.removable && !follows && !selected
                    readonly property var preview: packId.length ? backend.iconSamples(packId) : ({ samples: [], borrowedFrom: "" })
                    width: root.tileWidth
                    height: frame.height + caption.implicitHeight + subcaption.implicitHeight + 8
                    // Stays enabled during a change, because disabling the focused tile would
                    // throw the keyboard out of the list. Presses just wait until it is done.
                    enabled: root.enabled_
                    hoverEnabled: true
                    focusPolicy: Qt.StrongFocus
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: caption.text + (subcaption.text.length ? ", " + subcaption.text : "")
                    Accessible.checked: selected
                    ToolTip.visible: hovered && caption.truncated
                    ToolTip.text: caption.text
                    ToolTip.delay: 500
                    onClicked: if (!root.busy && (!selected || root.reconcile)) root.picked(modelData.value)
                    Keys.onDeletePressed: if (removable && !root.busy) root.requestRemoval(packId)
                    background: Item {}
                    contentItem: Column {
                        spacing: 4
                        Rectangle {
                            id: frame
                            width: tile.width
                            height: Math.round(theme.fontSize * 5.4)
                            radius: theme.panelRadius
                            color: theme.deep
                            Row {
                                anchors.centerIn: parent
                                spacing: Math.round(theme.fontSize * 0.4)
                                Repeater {
                                    // Only what it really has, so a sparse pack is still centered.
                                    model: tile.preview.samples.filter(function (sample) { return sample.found })
                                    delegate: Image {
                                        readonly property int side: Math.round(theme.fontSize * 2.1)
                                        width: side
                                        height: side
                                        objectName: "iconPackSample"
                                        source: modelData.url
                                        asynchronous: true
                                        cache: true
                                        fillMode: Image.PreserveAspectFit
                                        sourceSize.width: side * 2
                                        sourceSize.height: side * 2
                                    }
                                }
                            }
                            Rectangle {
                                anchors.fill: parent
                                radius: parent.radius
                                color: "transparent"
                                border.width: tile.selected || tile.visualFocus ? 2 : 1
                                border.color: tile.selected || tile.visualFocus ? theme.accent
                                            : tile.hovered ? Qt.alpha(theme.foreground, 0.5)
                                            : theme.divider
                            }
                            // Only on packs the user installed, and only where a pointer or the
                            // keyboard is, so the gallery stays calm.
                            AbstractButton {
                                objectName: "iconPackRemove"
                                visible: tile.removable && (tile.hovered || hovered || tile.visualFocus)
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.margins: 6
                                width: 24
                                height: 24
                                hoverEnabled: true
                                focusPolicy: Qt.NoFocus
                                Accessible.name: qsTr("Remove %1").arg(caption.text)
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Remove (Delete)")
                                ToolTip.delay: 400
                                onClicked: root.requestRemoval(tile.packId)
                                background: Rectangle {
                                    radius: Math.min(12, Math.max(3, theme.radius))
                                    color: parent.hovered ? Qt.alpha(theme.dangerText, 0.22) : Qt.alpha(theme.window, 0.85)
                                    border.width: 1
                                    border.color: theme.divider
                                }
                                contentItem: Item {
                                    AppIcon { anchors.centerIn: parent; name: "trash"; size: 13; tint: parent.parent.hovered ? theme.dangerText : theme.muted }
                                }
                            }
                            Rectangle {
                                visible: tile.selected
                                // Faded while being read again: what it says is not confirmed yet.
                                opacity: root.loading || root.busy ? 0.5 : 1
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 6
                                width: 20
                                height: 20
                                radius: Math.min(10, Math.max(3, theme.radius))
                                color: theme.accent
                                AppIcon { anchors.centerIn: parent; name: "check"; size: 13; tint: theme.accentForeground }
                            }
                        }
                        AppText {
                            id: caption
                            width: tile.width
                            elide: Text.ElideRight
                            text: tile.follows ? qsTranslate("catalog", modelData.label) : modelData.label
                            secondary: !tile.selected
                            scale_: 0.88
                        }
                        AppText {
                            id: subcaption
                            width: tile.width
                            elide: Text.ElideRight
                            // Every tile keeps this line, so a row of them stays aligned.
                            text: tile.follows ? root.desktopPack
                                : tile.preview.borrowedFrom ? qsTr("Shows icons from %1").arg(tile.preview.borrowedFrom) : ""
                            secondary: true
                            scale_: 0.8
                            height: Math.max(implicitHeight, Math.round(theme.fontSize * 1.2))
                        }
                    }
                }
            }
        }

        // Over the tiles only while something is being dragged, so it never takes a click.
        Rectangle {
            anchors.fill: parent
            visible: drop.containsDrag
            radius: theme.panelRadius
            color: Qt.alpha(theme.window, 0.88)
            border.width: 2
            border.color: theme.accent
            AppText {
                anchors.centerIn: parent
                text: qsTr("Drop an icon pack folder or archive to import it")
                font.weight: Font.DemiBold
            }
        }
    }

    AppText {
        width: root.width
        // What is kept on screen regardless is not a match.
        visible: root.query.trim().length > 0 && !root.filtered.some(function (choice) {
            var text = root.query.trim().toLowerCase()
            return choice.value !== "follow-theme" && (choice.label.toLowerCase().indexOf(text) >= 0 || (choice.id || "").toLowerCase().indexOf(text) >= 0)
        })
        text: qsTr("No icon packs match “%1”.").arg(root.query.trim())
        secondary: true
        wrapMode: Text.Wrap
    }
    AppText {
        width: root.width
        visible: root.noOtherPacks
        text: qsTr("No other icon packs are installed. Import a pack's folder or archive here, or put it in %1.").arg(backend.iconsFolder())
        secondary: true
        scale_: 0.88
        wrapMode: Text.Wrap
    }

    RowLayout {
        width: root.width
        spacing: 8
        AppButton {
            id: importButton
            objectName: "iconImportButton"
            text: qsTr("Import folder…")
            iconName: "plus"
            enabled: !root.importer.busy
            onClicked: {
                root.importer.dismiss()
                folderDialog.active = true
                folderDialog.item.open()
            }
        }
        AppButton {
            objectName: "iconImportArchiveButton"
            text: qsTr("Import archive…")
            iconName: "download"
            enabled: !root.importer.busy
            onClicked: {
                root.importer.dismiss()
                archiveDialog.active = true
                archiveDialog.item.open()
            }
        }
        AppButton {
            visible: root.packs.length > root.limit && !root.query.trim().length
            style: "flat"
            text: root.showAll ? qsTr("Show fewer") : qsTr("Show all %n", "", root.packs.length)
            iconName: "chevron"
            iconRotation: root.showAll ? 180 : 0
            onClicked: root.showAll = !root.showAll
        }
        Item { Layout.fillWidth: true }
    }

    Loader {
        id: folderDialog
        active: false
        source: "qrc:/IconFolderDialog.qml"
    }
    Loader {
        id: archiveDialog
        active: false
        source: "qrc:/IconArchiveDialog.qml"
    }

    // Where an import reports back. Importing and using a pack are separate decisions, so
    // even a successful one changes nothing until Use this pack is pressed.
    RowLayout {
        id: importStatus
        objectName: "iconImportStatus"
        readonly property string state_: root.importer.state
        readonly property bool working: state_ === "copying" || state_ === "extracting"
        visible: state_ !== "idle" || root.importer.message.length > 0
        width: root.width
        spacing: 8
        AppIcon {
            visible: !importStatus.working
            name: importStatus.state_ === "failed" ? "warning" : importStatus.state_ === "imported" ? "check" : "info"
            tint: importStatus.state_ === "failed" ? theme.dangerText : theme.muted
            size: 14
        }
        AppText {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            scale_: 0.88
            color: importStatus.state_ === "failed" ? theme.dangerText : theme.muted
            text: root.importer.message
        }
        Rectangle {
            visible: importStatus.working
            Layout.preferredWidth: theme.fontSize * 8
            Layout.preferredHeight: 4
            radius: theme.rounded ? 2 : 0
            color: theme.divider
            Rectangle {
                width: parent.width * root.importer.progress
                height: parent.height
                radius: parent.radius
                color: theme.accent
            }
            Accessible.role: Accessible.ProgressBar
            Accessible.name: qsTr("Import progress")
        }
        AppButton {
            visible: importStatus.state_ === "confirm"
            text: qsTr("Import")
            onClicked: root.importer.confirm()
        }
        AppButton {
            visible: root.importer.usable
            objectName: "iconUseImported"
            style: "primary"
            enabled: !root.busy
            text: qsTr("Use this pack")
            onClicked: {
                root.picked("pack:" + root.importer.themeId)
                // This button goes away next, and the keyboard should not go with it.
                importButton.forceActiveFocus(Qt.TabFocusReason)
                root.importer.dismiss()
            }
        }
        AppButton {
            visible: importStatus.working || importStatus.state_ === "confirm"
            style: "flat"
            text: qsTr("Cancel")
            onClicked: root.importer.cancel()
        }
        AppButton {
            visible: importStatus.state_ === "failed" || importStatus.state_ === "imported" || (importStatus.state_ === "idle" && root.importer.message.length > 0)
            style: "flat"
            text: ""
            iconName: "close"
            implicitWidth: 28
            Accessible.name: qsTr("Dismiss")
            onClicked: root.importer.dismiss()
        }
    }
}
