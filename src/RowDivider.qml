import QtQuick
import Omarchy.Theme as Theme
import Omarchy.Ui as O

// The hairline between rows of a panel, gone after the last one.
O.PanelSeparator {
    anchors.bottom: parent.bottom
    anchors.left: parent.left
    anchors.leftMargin: 14
    width: parent ? Math.max(0, parent.width - 28) : 0
    height: Theme.Style.spacing.hairline
    color: theme.hairline
    visible: !!parent && !!parent.parent && parent.y + parent.height < parent.parent.height - height
}
