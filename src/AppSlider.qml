import QtQuick
import QtQuick.Controls
import Omarchy.Theme as Theme
import Omarchy.Ui as O

// Native keyboard handling and snapping; signed ranges fill from zero.
Slider {
    id: root
    property string label: ""
    readonly property real knobSize: Math.max(14, Math.round(Theme.Style.spacing.controlHeight * 0.38))
    implicitWidth: Theme.Style.space(200)
    implicitHeight: Math.max(Theme.Style.space(22), knobSize + Theme.Style.spacing.md)
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    Accessible.name: label
    opacity: enabled ? 1 : 0.5

    // A range either side of zero fills from zero, so its middle reads as neutral.
    readonly property real origin: from < 0 && to > 0 ? -from / (to - from) : 0

    background: Rectangle {
        x: root.leftPadding + root.knobSize / 2
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width: Math.max(0, root.availableWidth - root.knobSize)
        height: Math.max(4, Math.round(Theme.Style.spacing.controlHeight * 0.11))
        radius: Theme.Style.cornerRadius > 0 ? height / 2 : 0
        color: Theme.Color.track
        Rectangle {
            readonly property real low: Math.min(root.origin, root.position)
            readonly property real high: Math.max(root.origin, root.position)
            width: (high - low) * parent.width
            height: parent.height
            radius: parent.radius
            color: theme.accent
            x: root.mirrored ? parent.width * (1 - high) : parent.width * low
        }
    }
    handle: Item {
        x: root.leftPadding + root.visualPosition * (root.availableWidth - width)
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width: root.knobSize
        height: width

        O.BorderSurface {
            anchors.fill: parent
            radius: Theme.Style.cornerRadius > 0 ? height / 2 : 0
            color: theme.foreground
            scale: root.pressed ? 1.15 : root.hovered || root.visualFocus ? 1.07 : 1
            borderSpec: root.visualFocus ? Theme.Border.controlSpec("focus", theme.foreground, theme.accent)
                       : Theme.Border.flat(theme.surfaceTop, Theme.Style.spacing.hairline)
            Behavior on scale { enabled: theme.animate; NumberAnimation { duration: Theme.Style.duration(110) } }
        }
    }
}
