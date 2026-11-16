import QtQuick
import QtQuick.Layouts

// Which app opens each everyday kind of file. A handler set some other way is shown as it
// is, even when it is not among the apps that list the type, and nothing else in
// mimeapps.list is touched by a change here.
AppPanel {
    id: root
    readonly property var system: backend.system
    readonly property var rows: system.associations
    function takeFocus() {
        if (list.count)
            list.itemAt(0).takeFocus()
    }
    function iconFor(mime) {
        switch (mime) {
        case "text/plain": return "file"
        case "application/pdf": return "file"
        case "image/png": return "image"
        case "video/mp4": return "video"
        case "audio/mpeg": return "music"
        case "inode/directory": return "folder"
        case "x-scheme-handler/mailto": return "mail"
        }
        return "file"
    }

    Item {
        width: parent.width
        implicitHeight: heading.implicitHeight + 30
        height: implicitHeight
        RowLayout {
            id: heading
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            spacing: 14
            AppIcon { name: "file"; size: Math.round(theme.fontSize * 1.75); tint: theme.foreground }
            AppText {
                Layout.fillWidth: true
                text: qsTr("File associations")
                font.weight: Font.DemiBold
                scale_: 1.45
                wrapMode: Text.Wrap
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }
        }
    }
    Rectangle { x: 16; width: parent.width - 32; height: 1; color: theme.hairline }
    Item {
        visible: !root.rows.length
        width: parent.width
        height: visible ? empty.implicitHeight + 28 : 0
        AppText {
            id: empty
            x: 18
            width: parent.width - 36
            anchors.verticalCenter: parent.verticalCenter
            wrapMode: Text.Wrap
            secondary: true
            text: qsTr("Looking up which apps open your files…")
        }
    }
    Repeater {
        id: list
        model: root.rows
        delegate: Item {
            id: row
            objectName: "associationRow"
            readonly property var association: modelData
            // The handlers that list this type, and the current one where it is not among them.
            readonly property var options: {
                var list = association.candidates.slice()
                var known = false
                for (var i = 0; i < list.length; ++i)
                    if (list[i].value === association.current)
                        known = true
                if (!known)
                    list.unshift({ value: association.current, label: association.status === "read_unavailable" ? qsTr("Could not read current app") : association.status === "unresolved" ? qsTr("Not resolved") : association.current.length ? association.currentLabel : qsTr("Not set"), icon: association.currentIcon || "" })
                return list
            }
            function takeFocus() { choice.forceActiveFocus(Qt.TabFocusReason) }
            width: parent ? parent.width : 400
            implicitHeight: Math.max(Math.round(theme.fontSize * 2.9), label.implicitHeight + 16)
            height: implicitHeight
            Rectangle {
                visible: index < list.count - 1
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                height: 1
                color: theme.hairline
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 18
                anchors.rightMargin: 14
                spacing: 14
                AppIcon { name: root.iconFor(row.association.mime); size: Math.round(theme.fontSize * 1.35); tint: theme.foreground }
                AppText { id: label; Layout.fillWidth: true; wrapMode: Text.Wrap; text: row.association.label }
                AppCombo {
                    id: choice
                    objectName: "associationChoice"
                    enabled: row.association.status !== "read_unavailable" && row.association.appsStatus !== "read_unavailable"
                    label: row.association.label
                    implicitWidth: Math.max(170, Math.min(250, root.width * 0.46))
                    model: row.options
                    textRole: "label"
                    valueRole: "value"
                    iconRole: "icon"
                    leadingImage: currentIndex >= 0 && row.options[currentIndex] ? row.options[currentIndex].icon || "" : ""
                    // The handler xdg-mime last reported. A ComboBox jumps to its first entry
                    // when its list fills, so the selection is put back each time.
                    readonly property int verified: {
                        for (var i = 0; i < row.options.length; ++i)
                            if (row.options[i].value === row.association.current)
                                return i
                        return -1
                    }
                    function sync() {
                        if (currentIndex !== verified)
                            currentIndex = verified
                    }
                    onVerifiedChanged: sync()
                    onCountChanged: Qt.callLater(sync)
                    Component.onCompleted: sync()
                    onActivated: function (i) {
                        var value = row.options[i].value
                        if (value.length && value !== row.association.current)
                            root.system.setAssociation(row.association.mime, value)
                        Qt.callLater(sync)
                    }
                }
            }
        }
    }
    Item { width: parent.width; height: 6 }
}
