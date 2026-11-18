import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omarchy.Theme as Theme
import Omarchy.Ui as O

// Long tools folded into one quiet panel: a row each, with what is inside counted. Each
// one's content is only built when opened, and stays where it was until it is closed.
AppPanel {
    id: root
    property var blocks: []

    // Opens a row for search to land on, and hands it back.
    function reveal(name) {
        for (var i = 0; i < repeater.count; ++i) {
            var row = repeater.itemAt(i)
            if (row && row.blockName === name) {
                row.expanded = true
                return row
            }
        }
        return null
    }

    function details(name) {
        switch (name) {
        case "plugins":
            return { icon: "plugin", title: qsTr("Plugins"), count: backend.plugins.plugins.length,
                     note: backend.plugins.status === "shell-down" ? qsTr("The Omarchy shell is not responding, so which plugins are active is unknown. Installed plugins are listed from disk.") : "" }
        case "shortcuts":
            return { icon: "input", title: qsTr("Keyboard shortcuts"), count: backend.shortcuts.binds.length,
                     note: backend.shortcuts.status === "unavailable" ? qsTr("Hyprland is not reachable, so shortcuts cannot be shown.") : "" }
        case "rules":
            return { icon: "windows", title: qsTr("Window rules"), count: backend.windowRules.rules.length, note: "" }
        }
        return { icon: "list", title: name, count: 0, note: "" }
    }

    Repeater {
        id: repeater
        model: root.blocks
        delegate: Column {
            id: row
            readonly property string blockName: modelData
            readonly property var info: root.details(modelData)
            property bool expanded: false
            width: parent ? parent.width : 400

            function takeFocus() {
                header.forceActiveFocus(Qt.TabFocusReason)
            }

            AbstractButton {
                id: header
                objectName: "disclosure_" + row.blockName
                width: parent.width
                implicitHeight: Math.max(Math.round(theme.fontSize * 2.95), content.implicitHeight + 16)
                focusPolicy: Qt.StrongFocus
                hoverEnabled: true
                Accessible.role: Accessible.Button
                Accessible.name: row.info.title + (row.info.count > 0 ? ", " + row.info.count : "")
                Accessible.expandable: true
                Accessible.expanded: row.expanded
                onClicked: row.expanded = !row.expanded
                background: O.BorderSurface {
                    anchors.fill: parent
                    anchors.margins: 2
                    radius: theme.controlRadius
                    color: header.down ? Theme.Style.pressedFill
                         : header.visualFocus ? Theme.Style.focusFillColor
                         : header.hovered ? Theme.Style.hoverFill : "transparent"
                    borderSpec: header.visualFocus
                                ? Theme.Border.controlSpec("focus", Theme.Color.foreground, Theme.Color.accent, Theme.Color.urgent)
                                : Theme.Border.none()
                }
                contentItem: RowLayout {
                    id: content
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    spacing: 12
                    AppIcon {
                        name: row.info.icon
                        size: Math.round(theme.fontSize * 1.5)
                        tint: theme.foreground
                        Layout.alignment: Qt.AlignVCenter
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 1
                        AppText {
                            text: row.info.title
                            font.weight: Font.Medium
                            Layout.fillWidth: true
                        }
                        // What is wrong stays readable with the row closed.
                        AppText {
                            visible: text.length > 0
                            text: row.info.note
                            color: theme.warningText
                            scale_: 0.88
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                    }
                    AppText {
                        // An empty list has nothing worth counting.
                        visible: row.info.count > 0
                        objectName: "disclosureCount"
                        text: row.info.count.toLocaleString(Qt.locale(), "f", 0)
                        secondary: true
                        Layout.alignment: Qt.AlignVCenter
                    }
                    AppIcon {
                        name: "chevron"
                        size: 16
                        tint: theme.muted
                        rotation: row.expanded ? 180 : 0
                        Layout.alignment: Qt.AlignVCenter
                        Behavior on rotation { enabled: theme.animate; NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                    }
                }
            }
            Item {
                visible: row.expanded
                width: parent.width
                height: visible ? loader.height + 16 : 0
                Loader {
                    id: loader
                    active: row.expanded
                    x: 14
                    y: 4
                    width: parent.width - 28
                    source: "qrc:/Block" + row.blockName.charAt(0).toUpperCase() + row.blockName.slice(1) + ".qml"
                }
            }
            O.PanelSeparator {
                visible: index < repeater.count - 1
                x: 14
                width: parent.width - 28
                color: theme.hairline
            }
        }
    }
}
