import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// What this app found when it checked the system, each from its own source, and a report
// that stays on this computer. Commands that changed since this app was tested pause
// their changes, said here with the way to resume them.
AppPanel {
    id: root
    readonly property var machine: backend.system.machine
    readonly property var caps: backend.capabilities
    property bool showDetails: false
    property bool exporting: false
    property string exportText: ""
    property bool exportFailed: false
    readonly property bool checking: caps.hyprland === undefined || caps.shell === undefined
    readonly property bool paused: backend.compatChanges.length > 0
    // One word for the whole card, with its tone.
    readonly property string overall: checking ? "checking"
        : !caps.hyprland || !caps.shell ? "attention"
        : paused || !backend.compatible ? "paused" : "good"
    function takeFocus() {
        (resumeButton.visible ? resumeButton : detailsButton).forceActiveFocus(Qt.TabFocusReason)
    }

    Connections {
        target: backend.system
        function onDiagnosticsFinished(result) {
            if (!root.exporting)
                return
            root.exporting = false
            root.exportText = result.message
            root.exportFailed = !result.ok
        }
    }
    Timer {
        // The report always answers; this only frees the button if it never does.
        running: root.exporting
        interval: 45000
        onTriggered: root.exporting = false
    }

    Item {
        width: parent.width
        implicitHeight: heading.implicitHeight + 30
        height: implicitHeight
        RowLayout {
            id: heading
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            spacing: 14
            AppIcon { name: "pulse"; size: Math.round(theme.fontSize * 1.75); tint: theme.foreground }
            AppText {
                Layout.fillWidth: true
                text: qsTr("Diagnostics")
                font.weight: Font.DemiBold
                scale_: 1.45
                wrapMode: Text.Wrap
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }
            AppIcon {
                visible: root.overall !== "checking"
                name: root.overall === "good" ? "check" : "warning"
                size: 14
                tint: root.overall === "good" ? theme.goodText : theme.warningText
            }
            AppText {
                objectName: "diagnosticsOverall"
                Layout.maximumWidth: root.width * 0.4
                horizontalAlignment: Text.AlignRight
                wrapMode: Text.Wrap
                scale_: 0.9
                color: root.overall === "good" || root.overall === "checking" ? theme.muted : theme.warningText
                text: root.overall === "checking" ? qsTr("Checking…")
                    : root.overall === "attention" ? qsTr("Needs attention")
                    : root.overall === "paused" ? (root.paused ? qsTr("Changes paused") : qsTr("Newer than tested"))
                    : qsTr("All good")
            }
        }
    }

    // A reading: what it is on one side, what was found on the other.
    component Reading: Item {
        id: reading
        property string label: ""
        property string value: ""
        property string tone: ""
        property bool mono: false
        objectName: "diagnosticsReading"
        width: parent ? parent.width : 400
        implicitHeight: Math.max(Math.round(theme.fontSize * 2.4), readingRow.implicitHeight + 12)
        height: implicitHeight
        Accessible.role: Accessible.StaticText
        Accessible.name: label + ", " + value
        Rectangle { anchors.bottom: parent.bottom; x: 16; width: parent.width - 32; height: 1; color: theme.hairline }
        RowLayout {
            id: readingRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 16
            AppText { text: reading.label; Layout.alignment: Qt.AlignTop }
            AppText {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                wrapMode: reading.mono ? Text.WrapAnywhere : Text.Wrap
                mono: reading.mono
                scale_: reading.mono ? 0.9 : 1
                color: reading.tone === "danger" ? theme.dangerText : reading.tone === "warning" ? theme.warningText : theme.foreground
                text: reading.value
            }
        }
    }

    Rectangle { x: 16; width: parent.width - 32; height: 1; color: theme.hairline }
    Reading {
        label: qsTr("Omarchy")
        value: (backend.omarchyVersion.length ? backend.omarchyVersion : qsTr("Unknown")) + " · " + (backend.compatible ? qsTr("Tested") : qsTr("Newer than tested"))
        tone: backend.compatible ? "" : "warning"
    }
    Reading {
        label: qsTr("Hyprland")
        value: root.caps.hyprland === undefined ? qsTr("Checking…")
             : root.caps.hyprland ? (root.machine.hyprland ? root.machine.hyprland + " · " : "") + qsTr("Running") : qsTr("Not reachable")
        tone: root.caps.hyprland === false ? "danger" : ""
    }
    Reading {
        label: qsTr("Omarchy shell")
        value: root.caps.shell === undefined ? qsTr("Checking…") : root.caps.shell ? qsTr("Responding") : qsTr("Not responding")
        tone: root.caps.shell === false ? "danger" : ""
    }
    Reading {
        label: qsTr("Hyprland overrides")
        value: root.caps["hypr.import"] ? qsTr("omarchy-control.lua is loaded") : qsTr("Not in use")
    }

    // The computer itself, one step in.
    AbstractButton {
        id: detailsButton
        objectName: "diagnosticsDetails"
        width: parent.width
        implicitHeight: Math.max(Math.round(theme.fontSize * 2.6), detailsRow.implicitHeight + 14)
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Computer and kernel details")
        Accessible.expandable: true
        Accessible.expanded: root.showDetails
        onClicked: root.showDetails = !root.showDetails
        background: Rectangle {
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            anchors.topMargin: 2
            anchors.bottomMargin: 2
            radius: theme.controlRadius
            color: detailsButton.hovered ? Qt.alpha(theme.foreground, 0.04) : "transparent"
            border.width: detailsButton.visualFocus ? 2 : 0
            border.color: theme.accent
        }
        contentItem: Item {
            RowLayout {
                id: detailsRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                spacing: 12
                AppIcon {
                    name: "chevronright"
                    size: 14
                    tint: theme.muted
                    rotation: root.showDetails ? 90 : (root.LayoutMirroring.enabled ? 180 : 0)
                    Behavior on rotation { enabled: theme.animate; NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                }
                AppText { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Computer and kernel details") }
            }
        }
    }
    Column {
        visible: root.showDetails
        width: parent.width
        Rectangle { x: 16; width: parent.width - 32; height: 1; color: theme.hairline }
        Reading {
            label: qsTr("Computer")
            value: [root.machine.vendor || "", root.machine.model || ""].filter(function (part) { return part.length > 0 }).join(" ") || qsTr("Unknown")
        }
        Reading {
            label: qsTr("Kernel")
            value: root.machine.kernel || qsTr("Unknown")
            mono: !!root.machine.kernel
        }
    }

    // Commands that changed since this app was tested: their changes wait for a yes.
    Item {
        objectName: "compatWarning"
        visible: root.paused
        width: parent.width
        height: visible ? compatBox.height + 20 : 0
        Rectangle {
            id: compatBox
            x: 14
            y: 10
            width: parent.width - 28
            height: compatColumn.implicitHeight + 24
            radius: theme.controlRadius
            color: Qt.alpha(theme.warningText, 0.07)
            border.width: 1
            border.color: Qt.alpha(theme.warningText, 0.45)
            ColumnLayout {
                id: compatColumn
                x: 14
                y: 12
                width: parent.width - 28
                spacing: 8
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    AppIcon { Layout.alignment: Qt.AlignTop; Layout.topMargin: 2; name: "warning"; size: Math.round(theme.fontSize * 1.15); tint: theme.warningText }
                    AppText {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: theme.warningText
                        text: qsTr("These Omarchy commands changed since this app was tested. Their settings read normally, but changes are paused.")
                    }
                }
                Repeater {
                    model: backend.compatChanges
                    delegate: AppText {
                        objectName: "compatCommand"
                        Layout.fillWidth: true
                        Layout.leftMargin: Math.round(theme.fontSize * 1.15) + 10
                        wrapMode: Text.WrapAnywhere
                        mono: true
                        scale_: 0.9
                        secondary: true
                        text: modelData
                    }
                }
                AppButton {
                    id: resumeButton
                    objectName: "compatResume"
                    Layout.leftMargin: Math.round(theme.fontSize * 1.15) + 10
                    text: qsTr("Resume changes")
                    onClicked: confirm.open_(qsTr("Resume changes"), qsTr("These settings may behave differently from what was tested. Every change is still verified by reading it back."), [], qsTr("Resume"), false, function () { backend.acceptCompat() })
                }
            }
        }
    }

    Rectangle { x: 16; width: parent.width - 32; height: 1; color: theme.hairline }
    // A report kept on this computer, with secrets taken out.
    Item {
        width: parent.width
        height: saveColumn.implicitHeight + 24
        ColumnLayout {
            id: saveColumn
            x: 18
            width: parent.width - 32
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                AppText {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    secondary: true
                    scale_: 0.9
                    text: qsTr("Save a local report with secrets removed.")
                }
                AppButton {
                    id: saveButton
                    objectName: "diagnosticsSave"
                    text: qsTr("Save report")
                    leadingIcon: "download"
                    Accessible.description: qsTr("Saves a report on this computer with secrets removed. Nothing is uploaded.")
                    busy: root.exporting
                    enabled: !root.exporting
                    onClicked: {
                        root.exportText = ""
                        root.exporting = true
                        backend.system.exportDiagnostics()
                    }
                }
            }
            RowLayout {
                visible: root.exportText.length > 0
                Layout.fillWidth: true
                spacing: 8
                AppIcon { Layout.alignment: Qt.AlignTop; Layout.topMargin: 2; name: root.exportFailed ? "warning" : "check"; size: 14; tint: root.exportFailed ? theme.dangerText : theme.goodText }
                AppText {
                    objectName: "diagnosticsSaved"
                    Layout.fillWidth: true
                    wrapMode: Text.WrapAnywhere
                    scale_: 0.88
                    color: root.exportFailed ? theme.dangerText : theme.muted
                    text: root.exportText
                }
            }
        }
    }
}
