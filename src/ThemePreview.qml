import QtQuick
import QtQuick.Window

// The desktop as it looks right now: its wallpaper, a window in the theme's colors, and
// the palette, drawn from the live theme rather than from a stored picture.
Rectangle {
    id: root
    readonly property var look: appearance.preview(appearance.themeName)
    readonly property string picture: appearance.wallpaper || look.image || ""
    implicitHeight: Math.round(theme.fontSize * 10)
    radius: theme.controlRadius
    gradient: Gradient {
        GradientStop { position: 0; color: Qt.alpha(theme.accent, 0.35) }
        GradientStop { position: 1; color: theme.deep }
    }
    Accessible.role: Accessible.Graphic
    Accessible.name: qsTr("Preview of the %1 theme").arg(appearance.themeName)

    Image {
        anchors.fill: parent
        source: appearance.thumbnail(root.picture, root.radius * Screen.devicePixelRatio)
        visible: status === Image.Ready
        asynchronous: true
        sourceSize.width: Math.round(root.width * Screen.devicePixelRatio)
        sourceSize.height: Math.round(root.height * Screen.devicePixelRatio)
    }
    Rectangle {
        anchors.fill: parent
        radius: root.radius
        color: "transparent"
        border.width: 1
        border.color: theme.hairline
    }
    // A window in the theme's colors.
    Rectangle {
        anchors.right: parent.right
        anchors.rightMargin: Math.round(parent.width * 0.1)
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: -Math.round(parent.height * 0.06)
        width: Math.round(parent.width * 0.38)
        height: Math.round(parent.height * 0.62)
        radius: theme.controlRadius
        color: Qt.alpha(theme.sidebar, 0.92)
        border.width: 1
        border.color: theme.hairline
        Row {
            x: 9
            y: 8
            spacing: 4
            Repeater {
                model: [theme.accent, theme.warning, theme.muted]
                delegate: Rectangle { width: 6; height: 6; radius: theme.rounded ? 3 : 0; color: modelData }
            }
        }
        Column {
            x: 9
            y: 24
            spacing: 6
            Repeater {
                model: [0.2, 0.14, 0.2, 0.12]
                delegate: Rectangle { width: root.width * 0.38 * modelData; height: 3; radius: theme.rounded ? 1.5 : 0; color: theme.foreground; opacity: 0.45 }
            }
        }
        Rectangle {
            x: parent.width * 0.38
            y: 22
            width: parent.width * 0.62 - 8
            height: parent.height - 30
            radius: Math.max(0, theme.controlRadius - 2)
            color: Qt.alpha(theme.foreground, 0.08)
            Column {
                x: 8
                y: 8
                spacing: 6
                Repeater {
                    model: [0.8, 0.56, 0.7]
                    delegate: Rectangle { width: (parent.parent.width - 16) * modelData; height: 3; radius: theme.rounded ? 1.5 : 0; color: theme.foreground; opacity: index === 0 ? 0.7 : 0.35 }
                }
            }
        }
    }
    // The palette.
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 9
        width: palette.implicitWidth + 12
        height: palette.implicitHeight + 10
        radius: theme.controlRadius
        color: Qt.alpha(theme.deep, 0.72)
        border.width: 1
        border.color: theme.hairline
        Row {
            id: palette
            anchors.centerIn: parent
            spacing: 5
            Repeater {
                model: [theme.accent, theme.foreground, theme.warning, theme.divider, theme.raised]
                delegate: Rectangle {
                    width: 14
                    height: 14
                    radius: Math.min(3, theme.controlRadius)
                    color: modelData
                    border.width: 1
                    border.color: theme.hairline
                }
            }
        }
    }
}
