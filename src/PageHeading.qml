import QtQuick
import QtQuick.Layouts
import Omarchy.Theme as Theme
import Omarchy.Ui as O

ColumnLayout {
    id: root
    property string title: ""
    property string subtitle: ""
    spacing: Theme.Style.spacing.labelGap
    O.Heading {
        text: root.title
        level: "display"
        Layout.fillWidth: true
    }
    AppText {
        text: root.subtitle
        secondary: true
        Layout.fillWidth: true
        wrapMode: Text.Wrap
    }
}
