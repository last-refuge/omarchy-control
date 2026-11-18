import QtQuick
import Omarchy.Ui as O

O.TextField {
    id: root
    property string label: ""

    font.family: theme.uiFont
    font.pixelSize: theme.fontSize
    foreground: theme.foreground
    accent: theme.accent
    selectionTint: theme.accent
    selectedTextColor: theme.accentForeground
    placeholderTextColor: theme.muted
    verticalAlignment: Text.AlignVCenter
    Accessible.name: label
}
