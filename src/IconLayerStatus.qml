import QtQuick
import QtQuick.Layouts

// What the launcher icon layer covers, under its switch. The row's own line says why it may
// be idle; this says how much of the launcher follows the pack, which apps keep their own
// icon, and offers to take over from Iconic when both are here.
ColumnLayout {
    id: root
    property var details: ({})
    property bool showApps: false
    readonly property bool built: !!details.built && !!details.enabled
    readonly property var gaps: details.gaps || []
    readonly property var iconic: details.iconic || []
    spacing: 8

    AppText {
        objectName: "iconLayerCoverage"
        visible: root.built
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        scale_: 0.93
        text: {
            var total = root.details.total || 0
            var line = qsTr("%1 gives %2 of %3 apps their icon.").arg(root.details.pack || "").arg(root.details.fromPack || 0).arg(total)
            if ((root.details.own || 0) > 0)
                line += " " + qsTr("%n apps use their own icon, since the pack has none for them.", "", root.details.own)
            if ((root.details.missing || 0) > 0)
                line += " " + qsTr("%n apps have no icon anywhere.", "", root.details.missing)
            return line
        }
    }

    AppButton {
        objectName: "iconLayerApps"
        visible: root.built && root.gaps.length > 0
        style: "link"
        text: root.showApps ? qsTr("Hide these apps") : qsTr("Show these apps")
        onClicked: root.showApps = !root.showApps
    }

    Column {
        visible: root.showApps && root.built
        Layout.fillWidth: true
        spacing: 2
        Repeater {
            model: root.showApps ? root.gaps : []
            delegate: RowLayout {
                width: parent ? parent.width : 0
                spacing: 10
                Accessible.role: Accessible.StaticText
                Accessible.name: tag.text + ", " + names.text
                AppText {
                    id: names
                    Layout.fillWidth: true
                    text: (modelData.apps || []).join(", ") || modelData.name
                    elide: Text.ElideRight
                    scale_: 0.88
                }
                AppText {
                    id: tag
                    text: modelData.kind === "missing" ? qsTr("No icon") : qsTr("Own icon")
                    secondary: true
                    scale_: 0.82
                }
            }
        }
    }

    // Iconic keeps a layer and a theme hook of its own, and its hook runs after this app's.
    RowLayout {
        visible: root.iconic.length > 0
        Layout.fillWidth: true
        spacing: 10
        AppButton {
            objectName: "retireIconic"
            style: "outline"
            leadingIcon: "arrow"
            text: qsTr("Hand icons over to Omarchy Control")
            onClicked: confirm.open_(qsTr("Hand icons over to Omarchy Control"),
                                     qsTr("Iconic's launcher icons, theme hook, and settings move into Omarchy Control's own folder, where they can be restored at any time. Iconic itself and any launchers it changed stay as they are."),
                                     root.iconic, qsTr("Hand over"), false, function () { backend.retireIconic() })
        }
        Item { Layout.fillWidth: true }
    }

    RowLayout {
        visible: root.iconic.length === 0 && !!root.details.iconicRetired
        Layout.fillWidth: true
        spacing: 10
        AppText {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            secondary: true
            scale_: 0.88
            text: qsTr("Iconic's files are kept aside from an earlier hand-over.")
        }
        AppButton {
            objectName: "restoreIconic"
            style: "link"
            text: qsTr("Restore them")
            onClicked: backend.restoreIconic()
        }
    }
}
