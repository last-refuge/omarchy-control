import QtQuick
import QtQuick.Layouts

// Nearby networks, the power switch, and the Wi-Fi settings the page's catalog gives this
// panel, all in one card. Saved networks connect through their profile; a new one is
// joined in Omarchy's panel, which handles passwords, so this window never collects one.
ColumnLayout {
    id: root
    readonly property var net: backend.network
    readonly property bool hasWifi: !!backend.capabilities.wifi
    // The catalog rows this panel claims, such as the band, drawn under the networks.
    property var panelRows: []
    property string focusId: ""
    property var ssids: []
    spacing: 12

    function syncNetworks() {
        var next = net.networks.slice(0, 12).map(function (n) { return n.ssid })
        if (JSON.stringify(next) !== JSON.stringify(ssids))
            ssids = next
    }
    Connections {
        target: root.net
        function onChanged() { root.syncNetworks() }
    }
    Component.onCompleted: syncNetworks()
    function networkFor(ssid) {
        for (var i = 0; i < net.networks.length; ++i)
            if (net.networks[i].ssid === ssid)
                return net.networks[i]
        return ({ ssid: ssid })
    }
    function savedUuid(ssid) {
        var saved = net.saved
        for (var i = 0; i < saved.length; ++i)
            if (saved[i].name === ssid)
                return saved[i].uuid
        return ""
    }
    function rowItem(id) {
        for (var i = 0; i < settings.count; ++i)
            if (settings.itemAt(i) && settings.itemAt(i).entryId === id)
                return settings.itemAt(i)
        return null
    }
    function takeFocus() {
        if (power.visible && power.enabled)
            power.forceActiveFocus(Qt.TabFocusReason)
        else if (networks.count)
            networks.itemAt(0).takeFocus()
    }

    PanelHeading {
        Layout.fillWidth: true
        icon: "wifi"
        title: qsTr("Wi-Fi")
        AppButton {
            visible: root.hasWifi
            style: "flat"
            implicitHeight: Math.round(theme.fontSize * 2)
            text: root.net.scanning ? qsTr("Scanning…") : qsTr("Scan")
            enabled: !root.net.scanning && root.net.wifiEnabled && root.net.status === "ok"
            onClicked: root.net.rescan()
        }
        AppSwitch {
            id: power
            objectName: "wifiPower"
            visible: root.hasWifi
            label: qsTr("Wi-Fi")
            on: root.net.wifiEnabled
            unknown: root.net.status === "loading"
            enabled: root.net.status === "ok"
            onClicked: win.setWifi(!on)
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
                x: 16
                width: parent.width - 32
                anchors.verticalCenter: parent.verticalCenter
                wrapMode: Text.Wrap
                color: root.hasWifi && root.net.status === "unavailable" ? theme.dangerText : theme.muted
                text: !root.hasWifi ? qsTr("No Wi-Fi adapter detected.")
                    : root.net.status === "unavailable" ? qsTr("NetworkManager is not responding, so networks cannot be shown.")
                    : root.net.status === "loading" ? qsTr("Looking for networks…")
                    : !root.net.wifiEnabled ? qsTr("Wi-Fi is off. Turn it on to see networks nearby.")
                    : root.ssids.length ? ""
                    : root.net.scanning ? qsTr("Scanning for networks…")
                    : qsTr("No networks found. Scan to look again.")
            }
        }
        Repeater {
            id: networks
            model: root.hasWifi && root.net.wifiEnabled && root.net.status === "ok" ? root.ssids : []
            delegate: Item {
                id: row
                objectName: "wifiNetwork"
                readonly property var network: root.networkFor(modelData)
                readonly property string uuid: root.savedUuid(modelData)
                readonly property string action: root.net.pending[modelData] || ""
                readonly property string failure: root.net.failures[modelData] || ""
                function takeFocus() { (join.visible ? join : row).forceActiveFocus(Qt.TabFocusReason) }
                width: parent ? parent.width : 400
                implicitHeight: Math.max(theme.rowHeight, names.implicitHeight + 20)
                height: implicitHeight
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("%1, signal %2%").arg(modelData).arg(network.signal || 0)
                Rectangle {
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
                        name: "wifi"
                        size: Math.round(theme.fontSize * 1.45)
                        tint: row.network.active ? theme.accentText : theme.muted
                    }
                    ColumnLayout {
                        id: names
                        Layout.fillWidth: true
                        spacing: 1
                        AppText { Layout.fillWidth: true; wrapMode: Text.Wrap; text: modelData }
                        AppText {
                            visible: text.length > 0
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            scale_: 0.88
                            color: row.failure.length ? theme.dangerText : theme.muted
                            text: row.action === "connect" ? qsTr("Connecting…") : row.failure
                        }
                    }
                    AppText {
                        secondary: true
                        mono: true
                        scale_: 0.88
                        text: qsTr("%1%").arg(row.network.signal || 0) + " · " + (row.network.secured ? qsTr("Secured") : qsTr("Unsecured"))
                    }
                    Rectangle {
                        visible: !!row.network.active
                        implicitHeight: Math.round(theme.fontSize * 1.75)
                        implicitWidth: connectedLabel.implicitWidth + 20
                        radius: height / 2
                        color: Qt.alpha(theme.foreground, 0.06)
                        border.width: 1
                        border.color: theme.divider
                        AppText { id: connectedLabel; anchors.centerIn: parent; scale_: 0.88; text: qsTr("Connected") }
                    }
                    AppButton {
                        id: join
                        visible: !row.network.active
                        enabled: !root.net.busy && !root.net.awaitingKeep
                        text: row.uuid.length ? qsTr("Connect") : qsTr("Join in panel")
                        iconName: row.uuid.length ? "" : "external"
                        Accessible.name: (row.uuid.length ? qsTr("Connect to %1") : qsTr("Join %1 in the Omarchy panel")).arg(modelData)
                        // Passwords belong to Omarchy's panel, never to this window.
                        onClicked: row.uuid.length ? root.net.activate(row.uuid, modelData) : win.activate(backend.entry("panel.network"))
                    }
                }
            }
        }
        Repeater {
            id: settings
            model: root.panelRows
            delegate: SettingRow {
                width: parent ? parent.width : 400
                entry: modelData
                rich: true
                last: index === root.panelRows.length - 1
                highlighted: root.focusId === modelData.id
            }
        }
    }
}
