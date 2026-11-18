import QtQuick
import Omarchy.Theme as Theme
import Omarchy.Ui as O

// A flat group with positioner-based rows. Their widths and heights belong to the
// rows themselves, including delegates and blocks that stretch beside another panel.
O.BorderSurface {
    id: root
    default property alias content: inner.data
    implicitHeight: inner.implicitHeight + padding * 2
    radius: Theme.Style.cornerRadius
    color: Theme.Style.normalFill
    borderSpec: Theme.Border.flat(theme.hairline, Theme.Style.spacing.hairline)

    Column {
        id: inner
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: root.padding
    }
}
