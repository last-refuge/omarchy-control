import QtQuick
import QtQuick.Controls
import Omarchy.Theme as Theme

// Mirrors the Omarchy ScrollArea thumb on ListView and ScrollView's attached bars.
ScrollBar {
    id: root
    padding: Theme.Style.spacing.hairline
    minimumSize: Math.min(1, Theme.Style.space(24) / Math.max(1, vertical ? availableHeight : availableWidth))
    hoverEnabled: true
    contentItem: Rectangle {
        implicitWidth: Theme.Style.spacing.scrollbarThickness
        implicitHeight: Theme.Style.spacing.scrollbarThickness
        radius: Math.min(Theme.Style.space(4), Theme.Style.cornerRadius)
        color: root.pressed || root.hovered ? Theme.Color.textDim : Theme.Style.normalBorderColor
        opacity: root.size < 1 ? 1 : 0
    }
    background: Rectangle {
        visible: root.size < 1
        color: Theme.Color.track
        radius: Math.min(Theme.Style.space(4), Theme.Style.cornerRadius)
    }
}
