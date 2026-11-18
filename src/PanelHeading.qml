import QtQuick
import QtQuick.Layouts
import Omarchy.Theme as Theme
import Omarchy.Ui as O

// A section's heading above its card: a line icon, the name, a quiet count, and room at
// the end for a control that belongs to the whole section, such as its power switch.
RowLayout {
    id: root
    property string icon: ""
    property string title: ""
    property string detail: ""
    default property alias trailing: tail.data
    spacing: Theme.Style.spacing.xxl
    AppIcon {
        visible: root.icon.length > 0
        name: root.icon
        size: Theme.Style.font.iconLarge
        tint: theme.foreground
        Layout.alignment: Qt.AlignVCenter
    }
    O.Heading {
        text: root.title
        level: "heading"
        Layout.alignment: Qt.AlignVCenter
        Layout.fillWidth: root.detail.length === 0
    }
    AppText {
        visible: root.detail.length > 0
        text: root.detail
        secondary: true
        scale_: 0.95
        Layout.leftMargin: 2
        Layout.alignment: Qt.AlignVCenter
        Layout.fillWidth: true
    }
    Row {
        id: tail
        spacing: Theme.Style.spacing.controlGap
        Layout.alignment: Qt.AlignVCenter
    }
}
