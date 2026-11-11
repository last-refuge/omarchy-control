import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Background services, closed until asked, or opened by itself when one has failed. Your own
// services change without asking; a system one asks for administrator approval, and
// stopping, restarting or disabling it is confirmed first.
AppPanel {
    id: root
    readonly property var services: backend.services
    property bool systemScope: false
    property bool expanded: services.failedCount > 0
    property string filter: ""
    readonly property int limit: 120
    readonly property var scoped: systemScope ? services.systemServices : services.userServices
    readonly property var rows: scoped.filter(function (s) {
        return !filter.length || (s.unit + " " + s.description).toLowerCase().indexOf(filter.toLowerCase()) >= 0
    })
    // Search for Services lands here, open and ready to type.
    function takeFocus() {
        expanded = true
        Qt.callLater(function () { field.forceActiveFocus(Qt.TabFocusReason) })
    }
    function verb(action) {
        switch (action) {
        case "start": return qsTr("Start")
        case "stop": return qsTr("Stop")
        case "restart": return qsTr("Restart")
        case "enable": return qsTr("Enable")
        case "disable": return qsTr("Disable")
        }
        return action
    }
    function consequence(action) {
        switch (action) {
        case "stop": return qsTr("It stops now and starts again at the next boot if it is enabled.")
        case "restart": return qsTr("It stops and starts again right away.")
        case "disable": return qsTr("It no longer starts at boot. It keeps running until it is stopped.")
        }
        return ""
    }
    function act(service, action) {
        if (systemScope && (action === "stop" || action === "disable" || action === "restart")) {
            // A whole question per action, so each language can order it naturally.
            var question = action === "stop" ? qsTr("Stop %1?") : action === "restart" ? qsTr("Restart %1?") : qsTr("Disable %1?")
            confirm.open_(question.arg(service.unit),
                          qsTr("This is a system service, and other things may depend on it. Stopping the wrong one can disconnect or freeze this computer.") + " " + consequence(action),
                          [], verb(action), true, function () {
                root.services.serviceAction(service.unit, action, true)
            })
            return
        }
        services.serviceAction(service.unit, action, systemScope)
    }

    AbstractButton {
        id: header
        objectName: "servicesHeader"
        width: parent.width
        implicitHeight: headerRow.implicitHeight + 28
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Services") + (root.services.failedCount > 0 ? ", " + qsTr("%n failed", "", root.services.failedCount) : "")
        Accessible.description: qsTr("Start, stop, enable, and diagnose background services.")
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
                AppIcon { name: "gear"; size: Math.round(theme.fontSize * 1.75); tint: theme.foreground }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    AppText { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Services"); font.weight: Font.DemiBold; scale_: 1.45 }
                    AppText { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Start, stop, enable, and diagnose background services."); secondary: true; scale_: 0.9 }
                    AppText { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("User and system services"); secondary: true; scale_: 0.85 }
                }
                // A failure is worth seeing from outside.
                AppText {
                    objectName: "servicesFailed"
                    visible: root.services.failedCount > 0
                    scale_: 0.88
                    color: theme.warningText
                    text: qsTr("%n failed", "", root.services.failedCount)
                }
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
        RowLayout {
            x: 16
            width: parent.width - 32
            height: implicitHeight + 20
            spacing: 8
            AppButton {
                objectName: "servicesMine"
                text: qsTr("Mine")
                style: root.systemScope ? "outline" : "primary"
                Accessible.checkable: true
                Accessible.checked: !root.systemScope
                onClicked: root.systemScope = false
            }
            AppButton {
                objectName: "servicesSystem"
                text: qsTr("System")
                style: root.systemScope ? "primary" : "outline"
                Accessible.checkable: true
                Accessible.checked: root.systemScope
                onClicked: root.systemScope = true
            }
            AppField {
                id: field
                objectName: "servicesFilter"
                Layout.fillWidth: true
                Layout.minimumWidth: 80
                label: qsTr("Filter services")
                placeholderText: qsTr("Filter services")
                onTextChanged: root.filter = text
            }
        }
        // What a system change costs, said before any is made.
        RowLayout {
            visible: root.systemScope
            x: 16
            width: parent.width - 32
            height: visible ? implicitHeight + 8 : 0
            spacing: 10
            AppIcon { Layout.alignment: Qt.AlignTop; name: "shield"; size: Math.round(theme.fontSize * 1.1); tint: theme.warningText }
            AppText {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                secondary: true
                scale_: 0.88
                text: qsTr("Changing a system service asks for administrator approval. Stopping the wrong one can disconnect or freeze this computer.")
            }
        }
        Item {
            visible: root.services.status === "unavailable" || !root.rows.length
            width: parent.width
            height: visible ? note1.implicitHeight + 20 : 0
            AppText {
                id: note1
                x: 18
                width: parent.width - 36
                anchors.verticalCenter: parent.verticalCenter
                wrapMode: Text.Wrap
                secondary: true
                text: root.services.status === "unavailable" ? qsTr("Services can't be read right now.")
                    : root.scoped.length ? qsTr("No service matches that.") : qsTr("No services found.")
            }
        }
        Repeater {
            model: root.expanded ? root.rows.slice(0, root.limit) : []
            delegate: Item {
                id: serviceRow
                objectName: "serviceRow"
                readonly property string unit: modelData.unit
                readonly property bool running: modelData.active === "active"
                width: parent ? parent.width : 400
                implicitHeight: Math.max(theme.rowHeight, serviceLayout.implicitHeight + 14)
                height: implicitHeight
                RowDivider {}
                GridLayout {
                    id: serviceLayout
                    // Buttons drop below the name when there is no room beside it.
                    readonly property bool stacked: root.width < Math.round(theme.fontSize * 34)
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 16
                    anchors.rightMargin: 14
                    columns: stacked ? 1 : 2
                    columnSpacing: 8
                    rowSpacing: 6
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        AppText { Layout.fillWidth: true; wrapMode: Text.Wrap; text: modelData.description || modelData.unit }
                        AppText {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            scale_: 0.88
                            color: modelData.failed ? theme.dangerText : theme.muted
                            text: modelData.unit + " · " + (modelData.failed ? qsTr("failed") : modelData.active) + (modelData.enabled ? " · " + modelData.enabled : "")
                        }
                    }
                    RowLayout {
                        spacing: 4
                        AppButton {
                            objectName: "serviceRestart"
                            style: "flat"
                            visible: serviceRow.running || modelData.failed
                            text: qsTr("Restart")
                            Accessible.name: qsTr("Restart %1").arg(modelData.unit)
                            onClicked: root.act(modelData, "restart")
                        }
                        AppButton {
                            objectName: "serviceStartStop"
                            style: "flat"
                            Layout.minimumWidth: Math.round(theme.fontSize * 5.5)
                            text: serviceRow.running ? qsTr("Stop") : qsTr("Start")
                            Accessible.name: (serviceRow.running ? qsTr("Stop %1") : qsTr("Start %1")).arg(modelData.unit)
                            onClicked: root.act(modelData, serviceRow.running ? "stop" : "start")
                        }
                        AppButton {
                            objectName: "serviceEnable"
                            style: "flat"
                            Layout.minimumWidth: Math.round(theme.fontSize * 6)
                            visible: modelData.enabled === "enabled" || modelData.enabled === "disabled"
                            text: modelData.enabled === "enabled" ? qsTr("Disable") : qsTr("Enable")
                            Accessible.name: (modelData.enabled === "enabled" ? qsTr("Disable %1") : qsTr("Enable %1")).arg(modelData.unit)
                            onClicked: root.act(modelData, modelData.enabled === "enabled" ? "disable" : "enable")
                        }
                    }
                }
            }
        }
        Item {
            visible: root.rows.length > root.limit
            width: parent.width
            height: visible ? note2.implicitHeight + 20 : 0
            AppText {
                id: note2
                x: 18
                width: parent.width - 36
                anchors.verticalCenter: parent.verticalCenter
                wrapMode: Text.Wrap
                secondary: true
                scale_: 0.88
                text: qsTr("Showing %1 of %2. Filter to find the rest.").arg(root.limit).arg(root.rows.length)
            }
        }
        Item { width: parent.width; height: 6 }
    }
}
