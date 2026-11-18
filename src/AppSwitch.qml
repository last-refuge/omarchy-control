import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omarchy.Theme as Theme
import Omarchy.Ui as O

// Shows the state that was last read back. Clicking asks for a change; the knob
// only moves once the new value has been confirmed. State is also spelled out in
// words, so it is never communicated by color alone.
AbstractButton {
    id: root
    property bool on: false
    property bool busy: false
    // Not read yet: saying "Off" here would be a claim nothing has established.
    property bool unknown: false
    property string label: ""

    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    padding: 0
    Accessible.role: Accessible.CheckBox
    Accessible.name: label
    Accessible.checkable: !unknown
    Accessible.checked: !unknown && on

    contentItem: RowLayout {
        spacing: Theme.Style.spacing.controlGap
        AppText {
            text: root.unknown ? "…" : root.on ? qsTr("On") : qsTr("Off")
            secondary: true
            mono: true
            horizontalAlignment: Text.AlignRight
            Layout.minimumWidth: Math.round(theme.fontSize * 2)
        }
        Item {
            opacity: root.enabled ? (root.busy ? 0.6 : 1) : 0.45
            implicitWidth: track.implicitWidth
            implicitHeight: track.implicitHeight

            O.ToggleSwitch {
                id: track
                anchors.fill: parent
                visible: !root.unknown
                interactive: false
                checked: root.on
                foreground: theme.foreground
                accent: theme.accent
            }
            O.BorderSurface {
                anchors.fill: parent
                visible: root.unknown
                radius: track.rounded ? height / 2 : 0
                color: Theme.Style.normalFillFor(theme.foreground, theme.accent)
                borderSpec: Theme.Border.controlSpec("normal", theme.foreground, theme.accent)
                Rectangle {
                    anchors.centerIn: parent
                    width: track.knobSize
                    height: width
                    radius: track.rounded ? height / 2 : 0
                    color: theme.foreground
                }
            }
            O.BorderSurface {
                visible: root.visualFocus || root.hovered
                anchors.fill: parent
                anchors.margins: -Theme.Style.spacing.xxs
                radius: track.rounded ? height / 2 : 0
                color: "transparent"
                borderSpec: Theme.Border.controlSpec(root.visualFocus ? "focus" : "hover-cursor", theme.foreground, theme.accent)
            }
        }
    }
}
