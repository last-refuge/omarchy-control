import QtQuick
import Omarchy.Theme as Theme
import Omarchy.Ui as O

// Outcomes in words: "Opened" and "changed" are different messages, because opening
// a helper says nothing about whether anything changed.
O.Banner {
    id: root
    property string kind: "ok"
    property string message: ""

    function show(text, type) {
        if (!text || !text.length)
            return
        message = text
        kind = type
        hide.interval = type === "error" || type === "warning" ? 9000 : 3500
        hide.restart()
        opacity = 1
    }

    anchors.bottom: parent.bottom
    anchors.horizontalCenter: parent.horizontalCenter
    anchors.bottomMargin: Theme.Style.spacing.panelPadding
    width: parent ? Math.min(implicitWidth, Math.max(0, parent.width - 2 * Theme.Style.spacing.panelPadding))
                  : implicitWidth
    height: implicitHeight
    // The banner floats over the page, so use a solid surface behind its text.
    color: Theme.Color.background
    text: message
    tone: kind === "error" || kind === "warning" ? kind : kind === "ok" ? "success" : "info"
    // Standard characters can fall back through Qt when the user's font lacks Nerd Font glyphs.
    glyph: kind === "error" ? "×" : kind === "warning" ? "⚠" : kind === "ok" ? "✓" : "↗"
    opacity: 0
    visible: opacity > 0
    Behavior on opacity { enabled: theme.animate; NumberAnimation { duration: 140 } }

    Timer { id: hide; onTriggered: root.opacity = 0 }
    TapHandler { onTapped: root.opacity = 0 }
}
