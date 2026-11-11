import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

ComboBox {
    id: root
    property string label: ""
    // For a name, not a value, and for sitting inside a control that draws its own edge.
    property bool proportional: false
    property bool flush: false
    property string leadingIcon: ""
    property color leadingTint: theme.foreground
    // An app's own picture before the text, from a file URL rather than the bundled set.
    property string leadingImage: ""
    // The model role holding each option's picture, for the list as well.
    property string iconRole: ""
    readonly property bool hasLeading: leadingIcon.length > 0 || leadingImage.length > 0
    implicitHeight: Math.round(theme.fontSize * 2.2) + 2
    implicitWidth: 220
    font.family: proportional || translator.cursiveScript ? theme.uiFont : "monospace"
    font.pixelSize: theme.fontSize
    Accessible.name: label
    Material.roundedScale: theme.controlRadius
    Material.foreground: theme.foreground
    Material.background: theme.raised
    Material.accent: theme.accent
    leftPadding: 0
    rightPadding: 0
    opacity: enabled ? 1 : 0.55

    background: Rectangle {
        radius: theme.controlRadius
        color: root.down ? Qt.alpha(theme.foreground, 0.1) : root.hovered ? Qt.alpha(theme.foreground, 0.06) : "transparent"
        border.width: root.visualFocus ? 2 : root.flush ? 0 : 1
        border.color: root.visualFocus ? theme.accent : theme.divider
        Behavior on color { enabled: theme.animate; ColorAnimation { duration: 100 } }
    }
    Image {
        visible: root.leadingImage.length > 0 && status === Image.Ready
        source: root.leadingImage
        width: Math.round(theme.fontSize * 1.25)
        height: width
        sourceSize.width: width * 2
        sourceSize.height: height * 2
        fillMode: Image.PreserveAspectFit
        smooth: true
        x: root.mirrored ? root.width - width - 8 : 8
        anchors.verticalCenter: parent.verticalCenter
    }
    AppIcon {
        visible: root.leadingIcon.length > 0 && !root.leadingImage.length
        name: root.leadingIcon
        tint: root.leadingTint
        size: Math.round(theme.fontSize * 1.15)
        x: root.mirrored ? root.width - width - 4 : 4
        anchors.verticalCenter: parent.verticalCenter
    }
    contentItem: AppText {
        readonly property int iconSpace: root.leadingImage.length ? Math.round(theme.fontSize * 1.25) + 16 : root.leadingIcon.length ? Math.round(theme.fontSize * 1.15) + 10 : 12
        readonly property int chevronSpace: root.flush ? 24 : 30
        mono: !root.proportional
        leftPadding: root.mirrored ? chevronSpace : iconSpace
        rightPadding: root.mirrored ? iconSpace : chevronSpace
        verticalAlignment: Text.AlignVCenter
        // An empty list says so instead of drawing a blank box.
        text: root.displayText.length ? root.displayText : qsTr("Not set")
        secondary: root.displayText.length === 0
    }
    indicator: AppIcon {
        name: "chevron"
        size: 14
        readonly property int inset: root.flush ? 7 : 10
        x: root.mirrored ? inset : root.width - width - inset
        anchors.verticalCenter: parent.verticalCenter
        tint: theme.muted
        rotation: root.popup.visible ? 180 : 0
        Behavior on rotation { enabled: theme.animate; NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
    }
    delegate: ItemDelegate {
        id: option
        required property int index
        width: ListView.view ? ListView.view.width : root.width
        height: Math.round(theme.fontSize * 2.5)
        padding: 0
        highlighted: root.highlightedIndex === index
        hoverEnabled: true
        Accessible.name: root.textAt(index)
        background: Rectangle {
            color: option.highlighted ? Qt.alpha(theme.accent, 0.18) : option.hovered ? Qt.alpha(theme.foreground, 0.06) : "transparent"
        }
        contentItem: Item {
            Image {
                id: optionIcon
                readonly property var entry: root.iconRole.length && Array.isArray(root.model) ? root.model[option.index] : null
                visible: !!entry && !!entry[root.iconRole] && status === Image.Ready
                source: entry && entry[root.iconRole] ? entry[root.iconRole] : ""
                width: Math.round(theme.fontSize * 1.2)
                height: width
                sourceSize.width: width * 2
                sourceSize.height: height * 2
                fillMode: Image.PreserveAspectFit
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
            }
            AppText {
                anchors.left: parent.left
                anchors.right: mark.left
                anchors.leftMargin: root.iconRole.length ? Math.round(theme.fontSize * 1.2) + 18 : 12
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                mono: !root.proportional
                text: root.textAt(option.index)
            }
            AppIcon {
                id: mark
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                size: 14
                name: "check"
                tint: theme.accentText
                visible: root.currentIndex === option.index
            }
        }
    }
    popup: Popup {
        y: root.height + 2
        width: Math.max(root.width, 200)
        implicitHeight: Math.min(contentItem.implicitHeight + 2, Math.round(theme.fontSize * 2.5) * 10 + 2)
        padding: 1
        transformOrigin: Item.Top
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: root.popup.visible ? root.delegateModel : null
            currentIndex: root.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: AppScrollBar {}
        }
        background: Rectangle {
            radius: theme.controlRadius
            color: theme.surfaceTop
            border.width: 1
            border.color: theme.controlBorder
        }
    }
}
