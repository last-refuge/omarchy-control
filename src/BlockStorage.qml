import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Disk space, one row per mount. An app running from an image mounts itself while it runs;
// those are kept together below, apart from the disks. System snapshots protect the system,
// not personal files, so this is never presented as a backup.
AppPanel {
    id: root
    readonly property var disks: backend.system.storage
    readonly property var mounts: disks.filter(function (d) { return !d.temporary })
    readonly property var temporary: disks.filter(function (d) { return !!d.temporary })
    property bool showTemporary: false
    function takeFocus() {
        (temporaryButton.visible ? temporaryButton : focusTarget).forceActiveFocus(Qt.TabFocusReason)
    }

    Item {
        id: focusTarget
        width: parent.width
        implicitHeight: heading.implicitHeight + 30
        height: implicitHeight
        activeFocusOnTab: !temporaryButton.visible
        Accessible.role: Accessible.Heading
        Accessible.name: qsTr("Storage")
        RowLayout {
            id: heading
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            spacing: 14
            AppIcon { name: "disk"; size: Math.round(theme.fontSize * 1.75); tint: theme.foreground }
            AppText { Layout.fillWidth: true; text: qsTr("Storage"); font.weight: Font.DemiBold; scale_: 1.45; wrapMode: Text.Wrap }
        }
    }
    Rectangle { x: 16; width: parent.width - 32; height: 1; color: theme.hairline }
    Item {
        visible: !root.disks.length
        width: parent.width
        height: visible ? note1.implicitHeight + 24 : 0
        AppText {
            id: note1
            x: 18
            width: parent.width - 36
            anchors.verticalCenter: parent.verticalCenter
            wrapMode: Text.Wrap
            secondary: true
            readonly property var state: backend.system.factStates.storage || {}
            text: state.status === "loading" ? qsTr("Reading storage…")
                : state.status === "unavailable" ? qsTr("Storage could not be read.") : qsTr("No disks found.")
        }
    }
    Repeater {
        model: root.mounts
        delegate: Item {
            objectName: "storageMount"
            width: parent ? parent.width : 400
            implicitHeight: mountColumn.implicitHeight + 22
            height: implicitHeight
            Accessible.role: Accessible.StaticText
            Accessible.name: modelData.mount + ", " + modelData.fstype + ", " + modelData.device + ", " + space.text
                             + (modelData.sharedWith ? ", " + qsTr("Shares its space with %1").arg(modelData.sharedWith) : "")
            Rectangle {
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                height: 1
                color: theme.hairline
            }
            ColumnLayout {
                id: mountColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 18
                anchors.rightMargin: 18
                spacing: 7
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    AppText { Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; text: modelData.mount + " (" + modelData.fstype + ")" }
                    AppText {
                        id: space
                        secondary: true
                        scale_: 0.93
                        text: modelData.known ? qsTr("%1 free of %2").arg(win.formatBytes(modelData.available)).arg(win.formatBytes(modelData.size))
                                              : qsTr("Size unknown")
                    }
                }
                AppText {
                    Layout.fillWidth: true
                    visible: !!modelData.sharedWith
                    wrapMode: Text.Wrap
                    secondary: true
                    scale_: 0.85
                    text: qsTr("Shares its space with %1").arg(modelData.sharedWith || "")
                }
                // How full, drawn only from a reading that makes sense.
                Rectangle {
                    Layout.fillWidth: true
                    visible: modelData.known
                    height: 5
                    radius: 2.5
                    color: Qt.alpha(theme.foreground, 0.12)
                    Rectangle {
                        readonly property real share: Math.max(0, Math.min(100, modelData.percent)) / 100
                        width: parent.width * share
                        height: parent.height
                        radius: 2.5
                        color: modelData.percent >= 90 ? theme.danger : theme.accent
                        x: root.LayoutMirroring.enabled ? parent.width - width : 0
                    }
                }
            }
        }
    }

    // Apps running from images, together and closed until asked.
    Item {
        visible: root.temporary.length > 0
        width: parent.width
        height: visible ? temporaryBox.height + 24 : 0
        Rectangle {
            id: temporaryBox
            x: 14
            y: 12
            width: parent.width - 28
            height: temporaryColumn.implicitHeight
            radius: theme.panelRadius
            color: Qt.alpha(theme.foreground, 0.025)
            border.width: 1
            border.color: theme.hairline
            Column {
                id: temporaryColumn
                width: parent.width
                AbstractButton {
                    id: temporaryButton
                    objectName: "temporaryMounts"
                    width: parent.width
                    implicitHeight: temporaryRow.implicitHeight + 20
                    focusPolicy: Qt.StrongFocus
                    hoverEnabled: true
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Temporary app mounts") + ", " + root.temporary.length
                    Accessible.expandable: true
                    Accessible.expanded: root.showTemporary
                    onClicked: root.showTemporary = !root.showTemporary
                    background: Rectangle {
                        radius: theme.panelRadius
                        color: temporaryButton.hovered ? Qt.alpha(theme.foreground, 0.04) : "transparent"
                        border.width: temporaryButton.visualFocus ? 2 : 0
                        border.color: theme.accent
                    }
                    contentItem: Item {
                        RowLayout {
                            id: temporaryRow
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: 14
                            anchors.rightMargin: 14
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 12
                            AppIcon { name: "layers"; size: Math.round(theme.fontSize * 1.35); tint: theme.foreground }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                AppText { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Temporary app mounts") }
                                AppText {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    secondary: true
                                    scale_: 0.88
                                    text: root.temporary.map(function (d) { return d.device + " · " + win.formatBytes(d.size) }).join(", ")
                                }
                            }
                            CountPill { count: root.temporary.length }
                            AppIcon {
                                name: "chevron"
                                size: 16
                                tint: theme.muted
                                rotation: root.showTemporary ? 180 : 0
                                Behavior on rotation { enabled: theme.animate; NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                            }
                        }
                    }
                }
                Repeater {
                    model: root.showTemporary ? root.temporary : []
                    delegate: Item {
                        width: parent ? parent.width : 400
                        implicitHeight: detail.implicitHeight + 16
                        height: implicitHeight
                        Rectangle { x: 14; width: parent.width - 28; height: 1; color: theme.hairline }
                        ColumnLayout {
                            id: detail
                            x: 14
                            width: parent.width - 28
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 1
                            AppText { Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; text: modelData.mount; scale_: 0.93 }
                            AppText {
                                Layout.fillWidth: true
                                wrapMode: Text.WrapAnywhere
                                secondary: true
                                mono: true
                                scale_: 0.85
                                text: modelData.fstype + " · " + win.formatBytes(modelData.size)
                            }
                        }
                    }
                }
            }
        }
    }
    Item {
        width: parent.width
        height: note2.implicitHeight + 22
        AppText {
            id: note2
            x: 16
            width: parent.width - 32
            anchors.verticalCenter: parent.verticalCenter
            wrapMode: Text.Wrap
            secondary: true
            scale_: 0.9
            text: qsTr("System snapshots cover system files. Back up personal files separately.")
        }
    }
}
