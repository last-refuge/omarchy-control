import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Hooks are executable code that belongs to the user, so this lists them and points
// at them; it does not guess at what they do. Closed until asked.
AppPanel {
    id: root
    readonly property var hooks: backend.system.hooks
    property bool expanded: false
    // Search for Hooks lands here and opens it.
    function takeFocus() {
        expanded = true
        header.forceActiveFocus(Qt.TabFocusReason)
    }

    AbstractButton {
        id: header
        objectName: "hooksHeader"
        width: parent.width
        implicitHeight: headerRow.implicitHeight + 28
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Hooks") + ", " + root.hooks.length
        Accessible.description: qsTr("Scripts for theme, font, and update events.")
        Accessible.expandable: true
        Accessible.expanded: root.expanded
        onClicked: root.expanded = !root.expanded
        background: Rectangle {
            anchors.fill: parent
            anchors.margins: 3
            radius: theme.controlRadius
            color: header.hovered ? Qt.alpha(theme.foreground, 0.04) : "transparent"
            border.width: header.visualFocus ? 2 : 0
            border.color: theme.accent
        }
        contentItem: Item {
            RowLayout {
                id: headerRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 18
                anchors.rightMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                spacing: 14
                AppIcon { name: "hook"; size: Math.round(theme.fontSize * 1.75); tint: theme.foreground }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    AppText { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Hooks"); font.weight: Font.DemiBold; scale_: 1.45 }
                    AppText { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Scripts for theme, font, and update events."); secondary: true; scale_: 0.9 }
                }
                CountPill { visible: root.hooks.length > 0; count: root.hooks.length }
                AppIcon {
                    name: "chevron"
                    size: 16
                    tint: theme.muted
                    rotation: root.expanded ? 180 : 0
                    Behavior on rotation { enabled: theme.animate; NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                }
            }
        }
    }
    Column {
        visible: root.expanded
        width: parent.width
        Rectangle { x: 16; width: parent.width - 32; height: 1; color: theme.hairline }
        Item {
            objectName: "hooksEmpty"
            visible: !root.hooks.length
            width: parent.width
            height: visible ? emptyText.implicitHeight + 26 : 0
            AppText {
                id: emptyText
                x: 18
                width: parent.width - 36
                anchors.verticalCenter: parent.verticalCenter
                wrapMode: Text.Wrap
                secondary: true
                readonly property var state: backend.system.factStates.hooks || {}
                text: state.status === "unavailable" || state.status === "partial" ? qsTr("Hooks could not be read completely.")
                    : qsTr("No active hooks. Omarchy's samples in ~/.config/omarchy/hooks stay inert until renamed.")
            }
        }
        Repeater {
            model: root.expanded ? root.hooks : []
            delegate: Item {
                objectName: "hookRow"
                width: parent ? parent.width : 400
                implicitHeight: Math.max(Math.round(theme.fontSize * 2.6), hookColumn.implicitHeight + 14)
                height: implicitHeight
                Accessible.role: Accessible.StaticText
                Accessible.name: modelData.name + ", " + modelData.path
                Rectangle {
                    visible: index < root.hooks.length - 1
                    anchors.bottom: parent.bottom
                    x: 16
                    width: parent.width - 32
                    height: 1
                    color: theme.hairline
                }
                RowLayout {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 18
                    anchors.rightMargin: 18
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 12
                    AppIcon { name: "terminal"; size: Math.round(theme.fontSize * 1.2); tint: theme.foreground }
                    ColumnLayout {
                        id: hookColumn
                        Layout.fillWidth: true
                        spacing: 1
                        AppText { Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; mono: true; text: modelData.name }
                    }
                }
            }
        }
        // Where they live, said once.
        Item {
            visible: root.hooks.length > 0
            width: parent.width
            height: visible ? whereText.implicitHeight + 22 : 0
            Rectangle { x: 16; width: parent.width - 32; height: 1; color: theme.hairline }
            AppText {
                id: whereText
                x: 18
                width: parent.width - 36
                anchors.verticalCenter: parent.verticalCenter
                wrapMode: Text.Wrap
                secondary: true
                scale_: 0.88
                text: qsTr("Executable files in ~/.config/omarchy/hooks run on their events.")
            }
        }
        Item { width: parent.width; height: 4 }
    }
}
