import QtQuick
import QtQuick.Layouts
import Omarchy.Theme as Theme
import Omarchy.Ui as O

RowLayout {
    id: root
    property string title: ""
    default property alias actions: tail.data
    spacing: Theme.Style.spacing.controlGap
    Layout.preferredHeight: Math.max(implicitHeight, Math.round(theme.fontSize * 2))
    O.Heading {
        text: root.title
        Layout.fillWidth: true
    }
    Row {
        id: tail
        spacing: Theme.Style.spacing.controlGap
    }
}
