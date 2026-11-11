import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One section of a composed page: a heading with a line icon, inside its card or above
// it, or a setting standing in as the heading, then the section's rows. Settings come
// first and actions after a small gap, so what runs something sits apart from what
// changes something. A panel can instead hold one of the page's own blocks, which then
// draws its heading and card itself.
Item {
    id: root
    objectName: "sectionPanel"
    property var spec: ({})
    property string focusId: ""
    readonly property string section: spec.section || ""
    readonly property string title: qsTranslate("catalog", spec.title || section)
    readonly property string block: spec.block || ""
    readonly property var header: spec.header || null
    readonly property var allRows: spec.rows || []
    // A first look at a long list: the catalog's chosen few first, the rest on request.
    readonly property var previewIds: spec.preview || []
    property bool showAll: false
    readonly property bool previewing: previewIds.length > 0 && !showAll && allRows.length > previewIds.length
    readonly property var rows: {
        if (!previewIds.length)
            return allRows
        var first = [], rest = []
        for (var p = 0; p < previewIds.length; ++p)
            for (var i = 0; i < allRows.length; ++i)
                if (allRows[i].id === previewIds[p])
                    first.push(allRows[i])
        for (var j = 0; j < allRows.length; ++j)
            if (previewIds.indexOf(allRows[j].id) < 0)
                rest.push(allRows[j])
        return previewing ? first : first.concat(rest)
    }
    // Rows in runs: settings first, then actions, each split by the group it belongs
    // to, in the order the groups first appear, with a gap between runs.
    readonly property var runs: {
        var settingKeys = [], actionKeys = [], byKey = {}
        // A library draws its rows inside its own groups, and a strip side by side.
        var rows = library.length || root.spec.strip ? [] : root.rows
        for (var i = 0; i < rows.length; ++i) {
            var action = rows[i].type === "action"
            var key = (action ? "a:" : "s:") + (rows[i].group || "")
            if (!byKey[key]) {
                byKey[key] = []
                ;(action ? actionKeys : settingKeys).push(key)
            }
            byKey[key].push(rows[i])
        }
        return settingKeys.concat(actionKeys).map(function (key) { return byKey[key] })
    }
    readonly property bool inset: !!spec.inset
    // A long section can start closed behind its heading, with its size said there.
    readonly property bool collapsible: !!spec.collapsible
    // Some start open, still able to close, with their size said at the heading.
    property bool expanded: !collapsible || !!spec.open
    readonly property var library: spec.library || []
    // Which library groups are open, by section name.
    property var open: ({})
    function setOpen(section, value) {
        var next = Object.assign({}, open)
        if (value)
            next[section] = true
        else
            delete next[section]
        open = next
    }
    // Opens whatever holds this entry. True when something had to open, so the caller
    // can wait for the rows to be laid out before scrolling to one.
    function reveal(id) {
        var opened = false
        if (collapsible && !expanded && allRows.some(function (row) { return row.id === id })) {
            expanded = true
            opened = true
        }
        if (previewing && allRows.some(function (row) { return row.id === id }) && !rows.some(function (row) { return row.id === id })) {
            showAll = true
            opened = true
        }
        for (var g = 0; g < library.length; ++g)
            for (var i = 0; i < library[g].items.length; ++i) {
                var item = library[g].items[i]
                if (!open[item.section] && item.rows.some(function (row) { return row.id === id })) {
                    setOpen(item.section, true)
                    opened = true
                }
            }
        return opened
    }
    readonly property bool outside: !!spec.outside
    readonly property bool headed: spec.heading !== false && !root.header
    // The jump link that leads here, if any: its own label, or the section's name.
    readonly property string jumpLabel: spec.jump === false ? "" : typeof spec.jump === "string" ? spec.jump : section
    readonly property bool jumpTarget: jumpLabel.length > 0

    // From the parts' natural sizes, never from the card's stretched one.
    // A block can leave the page entirely, as Battery does on a computer without one.
    // Its own wish, apart from whether its row is on screen.
    readonly property bool wanted: !blockLoader.item || blockLoader.item.shown === undefined || blockLoader.item.shown
    visible: wanted
    implicitHeight: block.length ? blockLoader.implicitHeight
                  : (outsideHeading.visible ? outsideHeading.implicitHeight + layout.spacing : 0) + card.implicitHeight

    function rowItem(id) {
        if (blockLoader.item)
            return blockLoader.item.rowItem ? blockLoader.item.rowItem(id) : null
        if (headerRow.item && headerRow.item.entryId === id)
            return headerRow.item
        for (var t = 0; t < stripRepeater.count; ++t)
            if (stripRepeater.itemAt(t) && stripRepeater.itemAt(t).entryId === id)
                return stripRepeater.itemAt(t)
        for (var g = 0; g < libraryRepeater.count; ++g) {
            var group = libraryRepeater.itemAt(g)
            for (var k = 0; group && k < group.items.count; ++k) {
                var found = group.items.itemAt(k) ? group.items.itemAt(k).rowItem(id) : null
                if (found)
                    return found
            }
        }
        for (var r = 0; r < runRepeater.count; ++r) {
            var run = runRepeater.itemAt(r)
            if (run && run.tiled) {
                var tile = run.tiles.tileFor(id)
                if (tile)
                    return tile
            }
            for (var i = 0; run && i < run.rows.count; ++i)
                if (run.rows.itemAt(i) && run.rows.itemAt(i).entryId === id)
                    return run.rows.itemAt(i)
        }
        return null
    }
    // A jump link lands on the first thing in the panel that can take the keyboard.
    function takeFocus() {
        if (blockLoader.item) {
            if (blockLoader.item.takeFocus)
                blockLoader.item.takeFocus()
            return
        }
        if (collapsible) {
            disclose.forceActiveFocus(Qt.TabFocusReason)
            return
        }
        if (root.spec.strip && stripRepeater.count) {
            stripRepeater.itemAt(0).takeFocus()
            return
        }
        if (library.length) {
            var firstGroup = libraryRepeater.count ? libraryRepeater.itemAt(0) : null
            if (firstGroup && firstGroup.items.count)
                firstGroup.items.itemAt(0).takeFocus()
            return
        }
        var firstRun = runRepeater.count ? runRepeater.itemAt(0) : null
        var first = headerRow.item || (firstRun && firstRun.rows.count ? firstRun.rows.itemAt(0) : firstRun && firstRun.tiled ? firstRun.tiles : null)
        if (first)
            first.takeFocus()
    }

    Accessible.role: Accessible.Grouping
    Accessible.name: title

    Column {
        id: layout
        width: parent.width
        spacing: 12

        Loader {
            id: blockLoader
            width: parent.width
            active: root.block.length > 0
            visible: active
            source: active ? "qrc:/Block" + root.block.charAt(0).toUpperCase() + root.block.slice(1) + ".qml" : ""
            // Side by side, a block can ask to reach its neighbor's line, as a card does.
            Binding on height {
                when: !!root.spec.fill
                value: Math.max(blockLoader.implicitHeight, root.height)
            }
            // A block can take the catalog rows its panel claims and draw them in its own card.
            onLoaded: {
                if (item.panelRows !== undefined)
                    item.panelRows = Qt.binding(function () { return root.rows })
                if (item.focusId !== undefined)
                    item.focusId = Qt.binding(function () { return root.focusId })
            }
        }

        PanelHeading {
            id: outsideHeading
            visible: !root.block.length && root.outside && root.headed
            width: parent.width
            icon: root.spec.icon || ""
            title: root.title
        }

        AppPanel {
            id: card
            visible: !root.block.length
            width: parent.width
            // Side by side, the last panel of a column stretches to its neighbor's line.
            height: Math.max(implicitHeight, root.height - y)

            // A setting as the heading, as a device's on/off switch is.
            Loader {
                id: headerRow
                width: parent.width
                active: !!root.header
                visible: active
                sourceComponent: SettingRow {
                    width: card.width
                    entry: root.header
                    rich: true
                    last: true
                    headingTitle: root.title
                    headingIcon: root.spec.icon || ""
                    highlighted: root.focusId === root.header.id
                }
            }
            Item {
                visible: root.headed && !root.outside
                width: parent.width
                implicitHeight: heading.implicitHeight + 30
                height: visible ? implicitHeight : 0
                RowLayout {
                    id: heading
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 18
                    anchors.rightMargin: 18
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 14
                    AppIcon {
                        visible: name.length > 0
                        name: root.spec.icon || ""
                        size: Math.round(theme.fontSize * 1.75)
                        tint: theme.foreground
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        AppText {
                            Layout.fillWidth: true
                            text: root.title
                            font.weight: Font.DemiBold
                            scale_: 1.45
                            wrapMode: Text.Wrap
                            Accessible.role: Accessible.Heading
                            Accessible.name: text
                        }
                        AppText {
                            visible: text.length > 0
                            Layout.fillWidth: true
                            text: qsTranslate("catalog", root.spec.description || "")
                            secondary: true
                            scale_: 0.9
                            wrapMode: Text.Wrap
                        }
                    }
                    CountPill {
                        visible: root.collapsible
                        count: root.allRows.length
                    }
                    AppIcon {
                        visible: root.collapsible
                        name: "chevron"
                        size: 16
                        tint: theme.muted
                        rotation: root.expanded ? 180 : 0
                        Behavior on rotation { enabled: theme.animate; NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                    }
                }
                // The whole heading opens and closes a collapsible panel.
                AbstractButton {
                    id: disclose
                    objectName: "panelDisclosure"
                    anchors.fill: parent
                    anchors.margins: 3
                    visible: root.collapsible
                    focusPolicy: Qt.StrongFocus
                    hoverEnabled: true
                    Accessible.role: Accessible.Button
                    Accessible.name: root.title + ", " + root.allRows.length
                    Accessible.expandable: true
                    Accessible.expanded: root.expanded
                    onClicked: root.expanded = !root.expanded
                    background: Rectangle {
                        radius: theme.controlRadius
                        color: disclose.hovered ? Qt.alpha(theme.foreground, 0.04) : "transparent"
                        border.width: disclose.visualFocus ? 2 : 0
                        border.color: theme.accent
                    }
                }
            }
            Rectangle {
                visible: ((root.headed && !root.outside) || !!root.header) && root.expanded
                x: 16
                width: parent.width - 32
                height: 1
                color: theme.hairline
            }

            // An engine's own set of rows reads as one card within the section.
            Item {
                visible: root.expanded
                width: parent.width
                implicitHeight: body.implicitHeight + (root.inset ? 26 : root.headed && !root.outside ? 4 : 0)
                height: implicitHeight
                Rectangle {
                    id: inner
                    x: root.inset ? 14 : 0
                    y: root.inset ? 14 : 0
                    width: parent.width - (root.inset ? 28 : 0)
                    implicitHeight: body.implicitHeight
                    height: implicitHeight
                    radius: theme.panelRadius
                    color: root.inset ? Qt.alpha(theme.foreground, 0.025) : "transparent"
                    border.width: root.inset ? 1 : 0
                    border.color: theme.hairline
                    Column {
                        id: body
                        width: parent.width
                        Item {
                            visible: root.inset
                            width: parent.width
                            height: visible ? insetTitle.implicitHeight + 22 : 0
                            AppText {
                                id: insetTitle
                                x: 16
                                width: parent.width - 32
                                anchors.verticalCenter: parent.verticalCenter
                                text: root.spec.inset || ""
                                font.weight: Font.DemiBold
                                scale_: 1.2
                                Accessible.role: Accessible.Heading
                                Accessible.name: text
                            }
                        }
                        Rectangle {
                            visible: root.inset
                            x: 14
                            width: parent.width - 28
                            height: 1
                            color: theme.hairline
                        }
                        Repeater {
                            id: runRepeater
                            model: root.runs
                            delegate: Column {
                                readonly property alias rows: runRows
                                readonly property alias tiles: runTiles
                                readonly property int run: index
                                // The panel's tile group is drawn as tiles, not rows.
                                readonly property bool tiled: !!root.spec.tiles && modelData.length > 0 && modelData[0].group === root.spec.tiles
                                width: body.width
                                Item {
                                    visible: parent.run > 0
                                    width: parent.width
                                    height: visible ? (parent.tiled ? 16 : 10) : 0
                                }
                                ActionTiles {
                                    id: runTiles
                                    visible: parent.tiled
                                    x: 16
                                    width: parent.width - 32
                                    height: visible ? implicitHeight + 16 : 0
                                    entries: parent.tiled ? modelData : []
                                    focusId: root.focusId
                                }
                                Repeater {
                                    id: runRows
                                    model: parent.tiled ? [] : modelData
                                    delegate: SettingRow {
                                        width: body.width
                                        entry: modelData
                                        rich: true
                                        file: !!root.spec.files
                                        prominent: !!root.spec.prominent
                                        navigation: !!root.spec.navigation && modelData.type === "action"
                                        last: parent.run === root.runs.length - 1 && index === runRows.count - 1 && !root.previewIds.length
                                        highlighted: root.focusId === modelData.id
                                    }
                                }
                            }
                        }
                        // The rest of a previewed list, in place.
                        Item {
                            visible: root.previewIds.length > 0 && root.allRows.length > root.previewIds.length
                            width: parent.width
                            height: visible ? moreButton.implicitHeight + 22 : 0
                            AbstractButton {
                                id: moreButton
                                objectName: "panelShowAll"
                                x: 14
                                y: 10
                                width: parent.width - 28
                                implicitHeight: Math.max(Math.round(theme.fontSize * 2.8), moreRow.implicitHeight + 16)
                                focusPolicy: Qt.StrongFocus
                                hoverEnabled: true
                                Accessible.role: Accessible.Button
                                Accessible.name: moreLabel.text
                                Accessible.expandable: true
                                Accessible.expanded: root.showAll
                                onClicked: root.showAll = !root.showAll
                                background: Rectangle {
                                    radius: theme.controlRadius
                                    color: moreButton.hovered ? Qt.alpha(theme.foreground, 0.06) : Qt.alpha(theme.foreground, 0.025)
                                    border.width: moreButton.visualFocus ? 2 : 1
                                    border.color: moreButton.visualFocus ? theme.accent : theme.hairline
                                }
                                contentItem: Item {
                                    RowLayout {
                                        id: moreRow
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.leftMargin: 16
                                        anchors.rightMargin: 14
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 12
                                        AppText {
                                            id: moreLabel
                                            Layout.fillWidth: true
                                            wrapMode: Text.Wrap
                                            text: root.spec.files
                                                  ? (root.showAll ? qsTr("Show fewer files") : qsTr("Show all %n files", "", root.allRows.length))
                                                  : (root.showAll ? qsTr("Show fewer") : qsTr("Show all %n", "", root.allRows.length))
                                        }
                                        AppIcon {
                                            name: "chevron"
                                            size: 16
                                            tint: theme.muted
                                            rotation: root.showAll ? 180 : 0
                                            Behavior on rotation { enabled: theme.animate; NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                                        }
                                    }
                                }
                            }
                        }
                        // A strip: a few readings side by side, stacked when there is no room.
                        GridLayout {
                            id: strip
                            visible: !!root.spec.strip
                            width: body.width
                            columns: width > theme.fontSize * 50 ? Math.max(1, stripRepeater.count) : 1
                            columnSpacing: 0
                            rowSpacing: 0
                            Repeater {
                                id: stripRepeater
                                model: root.spec.strip ? root.rows : []
                                delegate: SettingRow {
                                    // Equal shares of the line, whatever each one has to say.
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: strip.width / strip.columns
                                    Layout.maximumWidth: strip.width / strip.columns
                                    entry: modelData
                                    rich: true
                                    // Stacked, the readings get the usual rule between them.
                                    last: strip.columns > 1 || index === stripRepeater.count - 1
                                    highlighted: root.focusId === modelData.id
                                    // Between readings on one line, a hairline instead of a rule below.
                                    Rectangle {
                                        visible: index > 0 && strip.columns > 1
                                        width: 1
                                        anchors.top: parent.top
                                        anchors.bottom: parent.bottom
                                        anchors.topMargin: 14
                                        anchors.bottomMargin: 14
                                        color: theme.divider
                                    }
                                }
                            }
                        }
                        // The library: groups under quiet headings, each closed until asked.
                        Repeater {
                            id: libraryRepeater
                            model: root.library
                            delegate: Column {
                                readonly property alias items: itemRepeater
                                width: body.width
                                AppText {
                                    visible: text.length > 0
                                    x: 18
                                    width: parent.width - 36
                                    height: visible ? implicitHeight + 16 : 0
                                    verticalAlignment: Text.AlignBottom
                                    bottomPadding: 4
                                    text: qsTranslate("catalog", modelData.heading || "")
                                    font.weight: Font.Medium
                                    secondary: true
                                    Accessible.role: Accessible.Heading
                                    Accessible.name: text
                                }
                                Repeater {
                                    id: itemRepeater
                                    model: modelData.items
                                    delegate: Column {
                                        id: group
                                        readonly property var item: modelData
                                        readonly property bool isOpen: !!root.open[item.section]
                                        function takeFocus() { groupButton.forceActiveFocus(Qt.TabFocusReason) }
                                        function rowItem(id) {
                                            for (var i = 0; i < groupRows.count; ++i)
                                                if (groupRows.itemAt(i) && groupRows.itemAt(i).entryId === id)
                                                    return groupRows.itemAt(i)
                                            return null
                                        }
                                        width: body.width
                                        AbstractButton {
                                            id: groupButton
                                            objectName: "libraryGroup_" + group.item.section
                                            width: parent.width
                                            implicitHeight: Math.max(Math.round(theme.fontSize * 2.4), groupLabel.implicitHeight + 12)
                                            focusPolicy: Qt.StrongFocus
                                            hoverEnabled: true
                                            Accessible.role: Accessible.Button
                                            Accessible.name: groupLabel.text + ", " + group.item.rows.length
                                            Accessible.expandable: true
                                            Accessible.expanded: group.isOpen
                                            onClicked: root.setOpen(group.item.section, !group.isOpen)
                                            background: Item {
                                                Rectangle {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 6
                                                    anchors.rightMargin: 6
                                                    radius: theme.controlRadius
                                                    // A group whose actions undo your setup reads with caution.
                                                    color: group.item.tone === "caution" ? Qt.alpha(theme.warningText, groupButton.hovered ? 0.14 : 0.09)
                                                         : groupButton.hovered ? Qt.alpha(theme.foreground, 0.04) : "transparent"
                                                    border.width: groupButton.visualFocus ? 2 : 0
                                                    border.color: theme.accent
                                                }
                                                Rectangle {
                                                    anchors.bottom: parent.bottom
                                                    x: 16
                                                    width: parent.width - 32
                                                    height: 1
                                                    color: theme.hairline
                                                }
                                            }
                                            contentItem: RowLayout {
                                                spacing: 12
                                                AppIcon {
                                                    Layout.leftMargin: 12
                                                    name: group.item.icon || "list"
                                                    size: Math.round(theme.fontSize * 1.3)
                                                    tint: group.item.tone === "caution" ? theme.warningText : theme.foreground
                                                }
                                                AppText {
                                                    id: groupLabel
                                                    Layout.fillWidth: true
                                                    wrapMode: Text.Wrap
                                                    text: qsTranslate("catalog", group.item.label || group.item.section)
                                                }
                                                CountPill { count: group.item.rows.length }
                                                AppIcon {
                                                    Layout.rightMargin: 12
                                                    name: "chevron"
                                                    size: 16
                                                    tint: theme.muted
                                                    rotation: group.isOpen ? 180 : 0
                                                    Behavior on rotation { enabled: theme.animate; NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                                                }
                                            }
                                        }
                                        Repeater {
                                            id: groupRows
                                            model: group.isOpen ? group.item.rows : []
                                            delegate: SettingRow {
                                                width: body.width
                                                entry: modelData
                                                rich: true
                                                last: index === group.item.rows.length - 1
                                                highlighted: root.focusId === modelData.id
                                            }
                                        }
                                        Item {
                                            visible: group.isOpen
                                            width: parent.width
                                            height: visible ? 8 : 0
                                        }
                                    }
                                }
                            }
                        }
                        // A state that changes what the rows above mean, said where they are.
                        Item {
                            readonly property var notice: root.spec.notice || null
                            readonly property var st: notice ? (backend.states[backend.stateKey(notice.setting)] || ({})) : ({})
                            visible: !!notice && st.value === notice.value
                            width: parent.width
                            height: visible ? noticeBox.height + 22 : 0
                            Rectangle {
                                id: noticeBox
                                objectName: "panelNotice"
                                x: 14
                                y: 8
                                width: parent.width - 28
                                height: noticeRow.implicitHeight + 18
                                radius: theme.controlRadius
                                color: Qt.alpha(theme.foreground, 0.03)
                                border.width: 1
                                border.color: theme.divider
                                RowLayout {
                                    id: noticeRow
                                    x: 14
                                    width: parent.width - 28
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 10
                                    AppIcon { name: root.spec.notice ? root.spec.notice.icon || "info" : "info"; size: Math.round(theme.fontSize * 1.2); tint: theme.muted }
                                    AppText {
                                        Layout.fillWidth: true
                                        wrapMode: Text.Wrap
                                        scale_: 0.93
                                        text: root.spec.notice ? qsTranslate("catalog", root.spec.notice.text || "") : ""
                                    }
                                }
                            }
                        }
                        // What the actions above do beyond their names.
                        Item {
                            visible: !!root.spec.footnote
                            width: parent.width
                            height: visible ? footRow.implicitHeight + 30 : 0
                            Rectangle { x: 16; width: parent.width - 32; height: 1; color: theme.hairline }
                            RowLayout {
                                id: footRow
                                x: 16
                                y: 15
                                width: parent.width - 32
                                spacing: 8
                                AppIcon { name: "info"; size: 14; tint: theme.muted; Layout.alignment: Qt.AlignTop; Layout.topMargin: 2 }
                                AppText {
                                    objectName: "panelFootnote"
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    secondary: true
                                    scale_: 0.9
                                    text: qsTranslate("catalog", root.spec.footnote || "")
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
