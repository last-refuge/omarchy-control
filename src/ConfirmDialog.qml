import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omarchy.Theme as Theme
import Omarchy.Ui as O

// Consequences, in words, for the changes that warrant asking. Ordinary reversible
// changes never come through here. Popup owns modality and focus restoration.
Popup {
    id: root
    property string heading: ""
    property string body: ""
    property var effects: []
    property string acceptLabel: ""
    property bool danger: false
    property var onAccept: null
    property var onDecline: null
    property var opener: null

    function accept_() {
        var accept = root.onAccept
        root.onDecline = null
        root.close()
        if (accept)
            accept()
    }

    function open_(title, text, effectList, label, isDanger, accept, decline) {
        heading = title
        body = text
        effects = effectList || []
        acceptLabel = label
        danger = isDanger
        onAccept = accept
        onDecline = decline || null
        opener = win.activeFocusItem
        open()
        // A stray Space must never confirm something that cannot be taken back.
        if (isDanger)
            cancelButton.forceActiveFocus(Qt.TabFocusReason)
        else
            acceptButton.forceActiveFocus(Qt.TabFocusReason)
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(520, parent ? parent.width - 40 : 520)
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    padding: Theme.Style.spacing.panelPadding
    Overlay.modal: Rectangle { color: Theme.Color.scrim }
    background: O.BorderSurface {
        radius: Theme.Style.cornerRadius
        color: theme.surfaceTop
        borderSpec: root.danger ? Theme.Border.flat(theme.dangerText, Theme.Style.normalBorderWidth)
                   : Theme.Border.surfaceSpec("popups", "border", Theme.Color.popups.border, Theme.Style.normalBorderWidth)
    }
    onClosed: {
        if (onDecline)
            onDecline()
        onDecline = null
        if (opener)
            opener.forceActiveFocus()
        opener = null
    }

    contentItem: ColumnLayout {
        Accessible.role: Accessible.Dialog
        Accessible.name: root.heading
        spacing: Theme.Style.spacing.xl
        AppText { Layout.fillWidth: true; text: root.heading; font.weight: Font.DemiBold; scale_: 1.1; wrapMode: Text.Wrap; Accessible.role: Accessible.Heading }
        AppText { Layout.fillWidth: true; text: root.body; wrapMode: Text.Wrap; visible: text.length > 0 }
        Repeater {
            model: root.effects
            delegate: AppText { Layout.fillWidth: true; text: "• " + qsTranslate("catalog", modelData); secondary: true; wrapMode: Text.Wrap }
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            Layout.topMargin: Theme.Style.spacing.lg
            spacing: Theme.Style.spacing.controlGap
            AppButton {
                id: cancelButton
                text: qsTr("Cancel")
                onClicked: root.close()
            }
            AppButton {
                id: acceptButton
                text: root.acceptLabel
                style: root.danger ? "danger" : "primary"
                onClicked: root.accept_()
            }
        }
    }
}
