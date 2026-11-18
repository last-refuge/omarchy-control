import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omarchy.Theme as Theme
import Omarchy.Ui as O

// Native button input and accessibility, with Omarchy chrome and Control's SVG icons.
AbstractButton {
    id: root
    property string iconName: ""
    property string leadingIcon: ""
    property real iconRotation: 0
    property string style: "outline" // outline, flat, primary, danger, link
    property bool busy: false
    // A leading icon that turns while the work it starts is under way.
    property bool spinning: false
    // A larger label for the one action a panel is about.
    property real textScale: 1

    readonly property bool primary: style === "primary"
    readonly property bool outlined: style === "outline" || style === "danger"
    readonly property color accent: style === "danger" && enabled ? theme.dangerText : theme.accent

    implicitHeight: Math.max(Theme.Style.spacing.controlHeight, row.implicitHeight + 2 * Theme.Style.spacing.controlPaddingY)
    implicitWidth: row.implicitWidth + 2 * (style === "link" ? Theme.Style.spacing.hairline : Theme.Style.spacing.controlPaddingX)
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    Accessible.name: text
    Accessible.role: Accessible.Button
    opacity: enabled ? 1 : 0.5

    // A disabled destructive button is not a warning, so it stops looking like one.
    readonly property color tint: !enabled && style === "danger" ? theme.muted
                                : style === "primary" ? theme.accentForeground
                                : style === "danger" ? theme.dangerText
                                : style === "link" ? theme.accentText
                                : theme.foreground

    background: O.BorderSurface {
        radius: Theme.Style.cornerRadius
        color: root.primary ? (root.down ? Qt.darker(theme.accent, 1.2) : root.hovered ? Qt.darker(theme.accent, 1.08) : theme.accent)
             : root.style === "link" ? "transparent"
             : root.down ? Theme.Style.pressedFillFor(theme.foreground, root.accent)
             : root.visualFocus ? Theme.Style.focusFillFor(theme.foreground, theme.accent)
             : root.hovered ? Theme.Style.hoverFillFor(theme.foreground, root.accent)
             : root.outlined ? Theme.Style.normalFillFor(theme.foreground, root.accent)
             : "transparent"
        borderSpec: root.primary ? Theme.Border.none()
                  : root.visualFocus ? Theme.Border.controlSpec("focus", theme.foreground, theme.accent)
                  : root.style === "danger" && root.enabled ? Theme.Border.flat(root.accent, Theme.Style.normalBorderWidth)
                  : root.hovered && root.style !== "link" ? Theme.Border.controlSpec("hover-cursor", theme.foreground, root.accent)
                  : root.outlined ? Theme.Border.controlSpec("normal", theme.foreground, root.accent)
                  : Theme.Border.none()
        Behavior on color { enabled: theme.animate; ColorAnimation { duration: Theme.Style.duration(120) } }
        O.BorderSurface {
            visible: root.visualFocus && root.primary
            anchors.fill: parent
            anchors.margins: -Theme.Style.spacing.sm
            radius: parent.radius > 0 ? parent.radius + Theme.Style.spacing.sm : 0
            color: "transparent"
            borderSpec: Theme.Border.controlSpec("focus", theme.foreground, theme.accent)
        }
    }
    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight
        RowLayout {
            id: row
            anchors.centerIn: parent
            spacing: Theme.Style.spacing.controlGap
            AppIcon {
                id: leading
                visible: root.leadingIcon.length > 0
                name: root.leadingIcon
                tint: root.tint
                size: Math.round(theme.fontSize * 1.1 * root.textScale)
                rotation: 0
                RotationAnimator on rotation {
                    running: root.spinning && theme.animate
                    from: 0
                    to: 360
                    duration: 1200
                    loops: Animation.Infinite
                    onRunningChanged: if (!running) leading.rotation = 0
                }
            }
            AppText {
                visible: root.text.length > 0
                text: root.text
                scale_: root.textScale
                color: root.tint
                font.weight: root.textScale > 1 ? Font.DemiBold : root.style === "primary" || root.style === "link" ? Font.Medium : Font.Normal
                font.underline: root.style === "link" && (root.hovered || root.visualFocus)
            }
            AppIcon {
                visible: root.iconName.length > 0
                mirror: root.mirrored && (root.iconName === "arrow" || root.iconName === "chevronright")
                name: root.iconName
                tint: root.tint
                size: Math.round(theme.fontSize * 1.1)
                rotation: root.iconRotation
                Behavior on rotation { enabled: theme.animate; NumberAnimation { duration: Theme.Style.duration(160); easing.type: Easing.OutCubic } }
            }
        }
    }
}
