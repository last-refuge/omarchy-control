import QtQuick
import QtQuick.Layouts
import Omarchy.Theme as Theme
import Omarchy.Ui as O

// A titled group of rows. Long lists, like everything installable, start closed so
// a page stays readable; search still reaches every row.
ColumnLayout {
    id: root
    property string title: ""
    property var rows: []
    property string focusId: ""
    // A quiet note at the heading's end, such as which display the rows act on.
    property string aside: ""
    readonly property int actionCount: rows.filter(function (row) { return row.type === "action" }).length
    readonly property bool collapsible: actionCount > 12 && actionCount === rows.length
    readonly property bool containsFocus: {
        for (var i = 0; i < rows.length; ++i)
            if (rows[i].id === focusId)
                return true
        return false
    }
    property bool expanded: !collapsible
    spacing: 8
    onContainsFocusChanged: if (containsFocus) expanded = true

    // Rows another row keeps behind it, by the id of the row that holds them. They
    // show once it is opened, or when search lands on one of them.
    property var disclosedBy: ({})
    readonly property var held: {
        var map = {}
        for (var i = 0; i < rows.length; ++i)
            for (var j = 0; j < (rows[i].discloses || []).length; ++j)
                map[rows[i].discloses[j]] = rows[i].id
        return map
    }
    readonly property var shownRows: rows.filter(function (row) {
        var holder = held[row.id]
        return !holder || !!disclosedBy[holder] || row.id === focusId
    })
    function toggleDisclosure(id) {
        var next = Object.assign({}, disclosedBy)
        if (next[id])
            delete next[id]
        else
            next[id] = true
        disclosedBy = next
    }

    function rowItem(id) {
        for (var i = 0; i < repeater.count; ++i)
            if (repeater.itemAt(i) && repeater.itemAt(i).entryId === id)
                return repeater.itemAt(i)
        return null
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: Math.max(implicitHeight, Math.round(theme.fontSize * 2))
        spacing: Theme.Style.spacing.controlGap
        O.Heading {
            text: qsTranslate("catalog", root.title)
            Layout.fillWidth: true
        }
        AppText {
            objectName: "sectionAside"
            visible: root.aside.length > 0
            text: root.aside
            secondary: true
            scale_: 0.95
        }
        AppButton {
            visible: root.collapsible
            style: "flat"
            implicitHeight: Math.round(theme.fontSize * 1.75)
            text: root.expanded ? qsTr("Hide") : qsTr("Show %n", "", root.rows.length)
            iconName: "chevron"
            iconRotation: root.expanded ? 180 : 0
            onClicked: root.expanded = !root.expanded
        }
    }
    AppPanel {
        visible: root.expanded
        Layout.fillWidth: true
        Repeater {
            id: repeater
            model: root.expanded ? root.shownRows : []
            delegate: SettingRow {
                width: parent ? parent.width : 400
                entry: modelData
                last: index === root.shownRows.length - 1
                highlighted: root.focusId === modelData.id
                rich: true
                disclosed: !!root.disclosedBy[modelData.id] || (modelData.discloses || []).indexOf(root.focusId) >= 0
                onDisclosureToggled: root.toggleDisclosure(modelData.id)
            }
        }
    }
}
