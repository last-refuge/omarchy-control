import QtQuick
import QtQuick.Layouts

// The interfaces NetworkManager knows about, with the state it reports. Ones something
// else set up, such as Tailscale, are shown and never driven from here. Whether the
// internet is reachable is a separate fact, said in the heading only when it is not.
ColumnLayout {
    id: root
    readonly property var net: backend.network
    readonly property string devicesState: net.states.devices.status
    property var devices: []
    spacing: 12

    function syncDevices() {
        var next = net.devices.map(function (d) { return d.device })
        if (JSON.stringify(next) !== JSON.stringify(devices))
            devices = next
    }
    Connections {
        target: root.net
        function onChanged() { root.syncDevices() }
    }
    Component.onCompleted: syncDevices()
    function deviceFor(name) {
        for (var i = 0; i < net.devices.length; ++i)
            if (net.devices[i].device === name)
                return net.devices[i]
        return ({ device: name })
    }
    function iconFor(device) {
        return device.type === "wifi" ? "wifi" : device.type === "ethernet" ? "ethernet" : "network"
    }
    function takeFocus() {
        if (keep.visible)
            keepButton.forceActiveFocus(Qt.TabFocusReason)
        else if (rows.count)
            rows.itemAt(0).takeFocus()
    }

    PanelHeading {
        Layout.fillWidth: true
        icon: "network"
        title: qsTr("Connections")
        detail: root.net.states.general.status !== "ok" ? ""
              : root.net.connectivity === "limited" ? qsTr("Limited internet")
              : root.net.connectivity === "portal" ? qsTr("Sign-in needed for internet")
              : root.net.connectivity === "none" ? qsTr("No internet")
              : ""
    }

    // The deliberate keep step. Doing nothing restores the previous connection, which
    // is what you want if this window just lost its network.
    AppPanel {
        id: keep
        objectName: "networkKeep"
        Layout.fillWidth: true
        visible: root.net.awaitingKeep
        border.color: theme.accent
        padding: 12
        ColumnLayout {
            width: parent.width
            spacing: 8
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    AppText { Layout.fillWidth: true; text: qsTr("Keep this connection?"); font.weight: Font.Medium; wrapMode: Text.Wrap }
                    AppText {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        secondary: true
                        scale_: 0.93
                        text: root.net.canKeep ? qsTr("Going back to the previous one in %n second(s).", "", root.net.keepSeconds)
                            : qsTr("Preview status is unconfirmed. Check this connection or go back.")
                    }
                }
                AppButton { text: qsTr("Go back"); enabled: !root.net.busy; onClicked: root.net.revertConnection() }
                AppButton { id: keepButton; text: qsTr("Keep"); style: "primary"; enabled: root.net.canKeep && !root.net.busy; onClicked: root.net.keepConnection() }
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 2
                color: Qt.alpha(theme.foreground, 0.1)
                Rectangle {
                    height: parent.height
                    width: parent.width * (root.net.keepTotal > 0 ? root.net.keepSeconds / root.net.keepTotal : 0)
                    color: theme.accent
                    Behavior on width { enabled: theme.animate; NumberAnimation { duration: 900 } }
                }
            }
        }
    }

    AppPanel {
        Layout.fillWidth: true
        Item {
            visible: message.text.length > 0
            width: parent.width
            height: visible ? message.implicitHeight + 28 : 0
            AppText {
                id: message
                objectName: "connectionReadMessage"
                x: 16
                width: parent.width - 32
                anchors.verticalCenter: parent.verticalCenter
                wrapMode: Text.Wrap
                color: root.devicesState === "read_unavailable" ? theme.dangerText : theme.muted
                text: root.net.status === "loading" || root.devicesState === "not_requested" ? qsTr("Looking for connections…")
                    : root.devicesState !== "ok" ? qsTr("NetworkManager is not responding, so connections cannot be shown.")
                    : root.devices.length ? "" : qsTr("No network interfaces found.")
            }
        }
        Repeater {
            id: rows
            model: root.devicesState === "ok" || root.devicesState === "loading" ? root.devices : []
            delegate: Item {
                id: row
                objectName: "connectionRow"
                readonly property var device: root.deviceFor(modelData)
                readonly property string action: root.net.pending[modelData] || ""
                readonly property string failure: root.net.failures[modelData] || ""
                // Only connections NetworkManager itself runs can be switched off here.
                readonly property bool managed: !device.external && (device.type === "wifi" || device.type === "ethernet")
                function takeFocus() { (disconnect.visible ? disconnect : row).forceActiveFocus(Qt.TabFocusReason) }
                width: parent ? parent.width : 400
                implicitHeight: Math.max(theme.rowHeight, names.implicitHeight + 20)
                height: implicitHeight
                activeFocusOnTab: !disconnect.visible
                Accessible.role: Accessible.StaticText
                Accessible.name: (device.connection || device.device) + ", " + device.state
                Rectangle {
                    visible: index < rows.count - 1
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    height: 1
                    color: theme.hairline
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 14
                    spacing: 12
                    AppIcon {
                        name: root.iconFor(row.device)
                        size: Math.round(theme.fontSize * 1.45)
                        tint: row.device.connected ? theme.accentText : theme.muted
                    }
                    ColumnLayout {
                        id: names
                        Layout.fillWidth: true
                        spacing: 1
                        AppText {
                            objectName: "connectionName"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: row.device.connection || row.device.device
                        }
                        AppText {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            mono: !row.action.length && !row.failure.length
                            scale_: 0.88
                            color: row.failure.length ? theme.dangerText : theme.muted
                            text: row.action === "disconnect" ? qsTr("Disconnecting…")
                                : row.failure.length ? row.failure
                                : row.device.device + " · " + (row.device.state || "")
                        }
                    }
                    StatePill {
                        visible: row.device.connected && !row.device.external
                        text: qsTr("Connected")
                    }
                    StatePill {
                        visible: !!row.device.external
                        text: qsTr("External")
                    }
                    AppButton {
                        id: disconnect
                        visible: !!row.device.connected && row.managed
                        enabled: root.net.status !== "loading" && root.devicesState === "ok" && !root.net.radioBusy && !row.action.length
                        text: qsTr("Disconnect")
                        Accessible.name: qsTr("Disconnect %1").arg(row.device.connection || row.device.device)
                        onClicked: confirm.open_(qsTr("Disconnect %1").arg(row.device.connection), qsTr("If this is your only connection, this computer goes offline."), [], qsTr("Disconnect"), false, function () { root.net.deactivate(row.device.device) })
                    }
                }
            }
        }
    }

    component StatePill: Rectangle {
        property alias text: pillLabel.text
        implicitHeight: Math.round(theme.fontSize * 1.75)
        implicitWidth: pillLabel.implicitWidth + 20
        radius: height / 2
        color: Qt.alpha(theme.foreground, 0.06)
        border.width: 1
        border.color: theme.divider
        AppText {
            id: pillLabel
            anchors.centerIn: parent
            scale_: 0.88
        }
    }
}
