import QtQuick
import QtQuick.Controls
import QtQuick.Window

// Choosing a look by looking at it. Each tile is a real choice from the catalog: the
// picture comes from `source`, and anything without one draws its own colors.
Column {
    id: root
    property var choices: []
    property string current: ""
    property bool enabled_: true
    // Returns { image, background, surface, foreground, accent, line } for a value.
    property var source: function (value) { return ({}) }
    signal picked(string value)

    // The same size however many there are, and always filling whole columns.
    readonly property int columns: Math.max(2, Math.floor((width + tiles.spacing) / (theme.fontSize * 13 + tiles.spacing)))
    readonly property real tileWidth: (width - tiles.spacing * (columns - 1)) / columns
    // Two rows to start with, always including the one in use. The rest is one click away,
    // so a long list of themes never buries the settings below it.
    readonly property int limit: columns * 2
    property bool showAll: false
    readonly property var shown: {
        if (showAll || choices.length <= limit)
            return choices
        var list = choices.slice(0, limit)
        var inList = list.some(function (choice) { return choice.value === current })
        if (!inList) {
            var active = choices.filter(function (choice) { return choice.value === current })
            if (active.length)
                list[limit - 1] = active[0]
        }
        return list
    }
    spacing: 8

    Flow {
    id: tiles
    width: root.width
    spacing: 10

    Repeater {
        model: root.shown
        delegate: AbstractButton {
            id: tile
            readonly property bool selected: modelData.value === root.current
            readonly property var look: root.source(modelData.value)
            width: root.tileWidth
            height: frame.height + caption.implicitHeight + 6
            enabled: root.enabled_
            hoverEnabled: true
            focusPolicy: Qt.StrongFocus
            Accessible.role: Accessible.RadioButton
            Accessible.name: modelData.label
            Accessible.checked: selected
            onClicked: if (!selected) root.picked(modelData.value)
            background: Item {}
            contentItem: Column {
                spacing: 6
                Rectangle {
                    id: frame
                    width: tile.width
                    height: Math.round(tile.width * 10 / 16)
                    radius: theme.panelRadius
                    color: tile.look.background || theme.deep

                    // Drawn underneath, so a theme without a picture still shows itself
                    // and a slow one is never a blank box.
                    Rectangle {
                        width: parent.width
                        height: Math.round(parent.height * 0.14)
                        topLeftRadius: frame.radius
                        topRightRadius: frame.radius
                        color: tile.look.surface || theme.raised
                        Rectangle {
                            anchors.left: parent.left
                            anchors.leftMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.height * 1.6
                            height: Math.max(3, parent.height * 0.4)
                            radius: height / 2
                            color: tile.look.accent || theme.muted
                        }
                    }
                    Rectangle {
                        x: parent.width * 0.1
                        y: parent.height * 0.28
                        width: parent.width * 0.8
                        height: parent.height * 0.6
                        radius: theme.controlRadius
                        color: tile.look.surface || theme.raised
                        border.width: 1
                        border.color: tile.look.line || theme.divider
                        Column {
                            x: 8
                            y: 8
                            spacing: 5
                            Repeater {
                                model: [0.62, 0.42, 0.52]
                                delegate: Rectangle {
                                    width: tile.width * 0.8 * modelData
                                    height: 3
                                    radius: 1.5
                                    color: tile.look.foreground || theme.muted
                                    opacity: index === 0 ? 0.9 : 0.45
                                }
                            }
                        }
                        Rectangle {
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: 7
                            width: parent.width * 0.26
                            height: Math.max(5, parent.height * 0.14)
                            radius: theme.controlRadius
                            color: tile.look.accent || theme.muted
                        }
                    }
                    Image {
                        anchors.fill: parent
                        visible: status === Image.Ready
                        source: theme.thumbnail(tile.look.image || "", frame.radius * Screen.devicePixelRatio)
                        asynchronous: true
                        cache: true
                        sourceSize.width: Math.round(frame.width * Screen.devicePixelRatio)
                        sourceSize.height: Math.round(frame.height * Screen.devicePixelRatio)
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: parent.radius
                        color: "transparent"
                        border.width: tile.selected || tile.visualFocus ? 2 : 1
                        border.color: tile.selected || tile.visualFocus ? theme.accent
                                    : tile.hovered ? Qt.alpha(theme.foreground, 0.5)
                                    : theme.divider
                    }
                    Rectangle {
                        visible: tile.selected
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 7
                        width: 22
                        height: 22
                        radius: Math.min(11, Math.max(3, theme.radius))
                        color: theme.accent
                        AppIcon { anchors.centerIn: parent; name: "check"; size: 14; tint: theme.accentForeground }
                    }
                }
                AppText {
                    id: caption
                    width: tile.width
                    text: modelData.label
                    secondary: !tile.selected
                    scale_: 0.88
                }
            }
        }
    }
    }
    AppButton {
        visible: root.choices.length > root.limit
        style: "flat"
        text: root.showAll ? qsTr("Show fewer") : qsTr("Show all %n", "", root.choices.length)
        iconName: "chevron"
        iconRotation: root.showAll ? 180 : 0
        onClicked: root.showAll = !root.showAll
    }
}
