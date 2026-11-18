import QtQuick
import Omarchy.Theme as Kit

QtObject {
    readonly property color foreground: Kit.Color.foreground
    readonly property color window: Kit.Color.background
    readonly property color accent: Kit.Color.accent
    readonly property color danger: Kit.Color.urgent
    readonly property color good: Kit.Color.success
    readonly property color warning: Kit.Color.warning
    readonly property bool dark: Kit.Color.dark
    readonly property bool animate: !Kit.Style.reduceMotion
    readonly property int radius: Kit.Style.cornerRadius
    readonly property int panelRadius: radius
    readonly property int controlRadius: radius
    readonly property bool rounded: radius > 0
    readonly property int fontSize: Kit.Style.font.body
    readonly property int rowHeight: Math.max(Kit.Style.spacing.controlHeight, Math.round(fontSize * 3.65))

    // Keep a proportional fallback for languages whose text needs cursive shaping.
    readonly property FontLoader scriptFont: FontLoader {
        source: translator.cursiveScript ? "qrc:/fonts/InterVariable.ttf" : ""
    }
    readonly property string uiFont: translator.cursiveScript ? (scriptFont.name || "sans-serif") : Kit.Style.font.family

    readonly property color canvas: window
    readonly property color sidebar: Kit.Color.mixColor(window, foreground, 0.035)
    readonly property color rail: sidebar
    readonly property color deep: window
    readonly property color raised: Qt.alpha(Kit.Color.popups.background, 1)
    readonly property color surfaceTop: Kit.Color.mixColor(raised, foreground, 0.045)
    readonly property color surfaceBottom: raised
    readonly property color hairline: Kit.Color.track
    readonly property color divider: hairline
    readonly property color controlBorder: Kit.Border.color(Kit.Border.controlSpec("normal", foreground, accent, danger))

    readonly property color accentForeground: appearance.accentForeground(accent)
    readonly property color muted: readable(Kit.Color.textDim)
    readonly property color accentText: readable(accent)
    readonly property color dangerText: readable(danger)
    readonly property color goodText: readable(good)
    readonly property color warningText: readable(warning)

    function readable(color) {
        return appearance.readableColor(color, foreground, canvas, surfaceTop)
    }
}
