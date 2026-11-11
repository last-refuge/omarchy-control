import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Window

ApplicationWindow {
    id: win
    width: 1240
    height: 800
    minimumWidth: 560
    minimumHeight: 460
    visible: true
    title: qsTr("Omarchy Control")
    color: theme.canvas
    font.family: theme.uiFont
    font.pixelSize: theme.fontSize

    Material.theme: theme.dark ? Material.Dark : Material.Light
    Material.accent: theme.accent
    Material.primary: theme.accent
    Material.background: theme.window
    Material.foreground: theme.foreground
    Material.roundedScale: theme.controlRadius

    property string currentPage: "overview"
    property string focusRequest: ""
    property var pageHistory: []
    property bool helpVisible: false
    readonly property bool narrow: width < theme.fontSize * 61
    readonly property int sidebarWidth: narrow ? 60 : Math.max(256, theme.fontSize * 18.5)
    readonly property bool typing: activeFocusItem !== null && activeFocusItem.hasOwnProperty("cursorPosition")

    // ---- Shared actions, used by every page -----------------------------------
    function go(page, focus) {
        if (page !== currentPage) {
            pageHistory = pageHistory.concat([currentPage])
            currentPage = page
        }
        focusRequest = focus || ""
        searchField.text = ""
        // Also covers clicking the tab already open, which changes no page.
        if (!focusRequest.length)
            scrollPageToTop()
    }
    // Tab never leaves the keyboard's place off-screen.
    onActiveFocusItemChanged: {
        var item = activeFocusItem
        var page = pageLoader.item
        if (!item || !page)
            return
        var view = page.contentItem
        var ancestor = item.parent
        while (ancestor && ancestor !== view)
            ancestor = ancestor.parent
        if (!ancestor)
            return
        var margin = 24
        var top = item.mapToItem(view.contentItem, 0, 0).y
        var bottom = top + item.height
        if (top - margin < view.contentY)
            view.contentY = Math.max(0, top - margin)
        else if (bottom + margin > view.contentY + view.height)
            view.contentY = Math.min(Math.max(0, view.contentHeight - view.height), bottom + margin - view.height)
    }
    function scrollPageToTop() {
        if (!pageLoader.item)
            return
        pageLoader.item.contentItem.cancelFlick()
        pageLoader.item.contentItem.contentY = 0
    }
    // Opening a result only navigates, even for an action: it lands on the row, and
    // running it stays one deliberate step further.
    function openResult(row) {
        if (row.launchKind === "page")
            go(row.linkPage, row.linkFocus)
        else
            go(row.page, row.id)
        searchField.focus = false
    }
    function back() {
        if (!pageHistory.length)
            return
        var list = pageHistory.slice()
        currentPage = list.pop()
        pageHistory = list
        focusRequest = ""
    }
    function requestChange(id, value) {
        var plan = backend.planChange(id, value)
        if (!plan.ok) {
            toast.show(plan.error, "error")
            backend.refresh(id)
            return
        }
        if (plan.confirmText.length) {
            confirm.open_(qsTranslate("catalog", backend.entry(id).label), qsTranslate("catalog", plan.confirmText), plan.effects, qsTr("Change"), false, function () {
                backend.commit(id, value)
            }, function () {
                backend.refresh(id)
            })
            return
        }
        backend.commit(id, value)
    }
    // The first thing to use the owned Lua file asks before adding its one line. Only
    // typed settings go through a plan, so displays and shortcuts ask here.
    function withOwnership(proceed) {
        if (backend.capabilities["hypr.import"]) {
            proceed()
            return
        }
        confirm.open_(qsTr("Save to your Hyprland config"), qsTr("To save this, Omarchy Control adds one marked line to hyprland.lua and keeps your choices in omarchy-control.lua. It loads after your own files and before Omarchy's toggles. Nothing else in your config changes."), [], qsTr("Continue"), false, proceed)
    }
    function activate(entry) {
        if (entry.confirm === "confirm" || entry.confirm === "destructive") {
            confirm.open_(qsTranslate("catalog", entry.label), qsTranslate("catalog", entry.confirmText || ""), [], qsTranslate("HelperLabels", entry.verb || "Open"), entry.confirm === "destructive", function () {
                backend.launch(entry.id)
            })
            return
        }
        backend.launch(entry.id)
    }
    function setWifi(on) {
        if (!on) {
            confirm.open_(qsTr("Turn off Wi-Fi"), qsTr("If Wi-Fi is your only connection, this computer goes offline."), [], qsTr("Turn off"), false, function () {
                backend.network.setWifiEnabled(false)
            })
            return
        }
        backend.network.setWifiEnabled(true)
    }
    function setDefaultVolume(percent) {
        var outputs = backend.audio.outputs
        for (var i = 0; i < outputs.length; ++i)
            if (outputs[i].isDefault)
                backend.audio.setVolume(outputs[i].name, percent)
    }
    function formatBytes(bytes) {
        var units = ["B", "KB", "MB", "GB", "TB", "PB"]
        var value = bytes
        var unit = 0
        while (value >= 1024 && unit < units.length - 1) {
            value /= 1024
            ++unit
        }
        return value.toLocaleString(Qt.locale(), "f", unit > 2 ? 1 : 0) + " " + units[unit]
    }

    // Pages share one scrolling view, so a new page would otherwise keep the last offset.
    onCurrentPageChanged: {
        backend.pageShown(currentPage)
        scrollPageToTop()
    }
    onActiveChanged: if (active) { backend.windowActivated(); theme.refreshRounding() }

    // ---- Tray: the mark in the bar's tray brings the window back ----------------
    // With a tray to wait in, closing only hides the window, so the app stays one click
    // away. Q, Ctrl+Q, and Quit in the tray's menu end it. Without a tray, closing quits.
    readonly property bool inTray: trayIcon.item !== null && trayIcon.item.available
    // Quitting asks the window to close first, so the close below has to know to let it.
    property bool quitting: false
    function quitApp() {
        quitting = true
        Qt.quit()
    }
    onClosing: function (close) {
        if (quitting)
            return
        if (!inTray) {
            quitApp()
            return
        }
        close.accepted = false
        hide()
    }
    // Shows the window where the person is now. Showing it again, rather than only
    // asking for focus, brings it to the workspace in view, which a request alone cannot.
    function summon() {
        if (visible && !active)
            hide()
        show()
        raise()
        requestActivate()
    }
    Loader {
        id: trayIcon
        active: trayHost.available
        source: "qrc:/TrayIcon.qml"
    }
    Connections {
        target: trayIcon.item
        function onOpenRequested() { win.summon() }
        function onQuitRequested() { win.quitApp() }
    }
    Component.onCompleted: {
        if (initialPage.length)
            currentPage = initialPage
        if (initialSetting.length) {
            var target = backend.entry(initialSetting)
            if (target.page)
                go(target.page, initialSetting)
        }
        backend.pageShown(currentPage)
        theme.refreshRounding()
    }

    Connections {
        target: backend
        function onToast(text, kind) { toast.show(text, kind) }
        function onNavigate(page, focus) { win.go(page, focus) }
        function onRaiseRequested(page, focus) {
            win.summon()
            if (focus.length) {
                var target = backend.entry(focus)
                if (target.page)
                    win.go(target.page, focus)
            } else if (page.length) {
                win.go(page, "")
            }
        }
    }

    // ---- Keys: single letters switch off while typing --------------------------
    Shortcut { sequences: ["Ctrl+K", "Ctrl+F"]; context: Qt.ApplicationShortcut; onActivated: { searchField.forceActiveFocus(); searchField.selectAll() } }
    Shortcut { sequence: "Alt+Left"; context: Qt.ApplicationShortcut; onActivated: win.back() }
    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.quitApp() }
    Shortcut { sequence: "Ctrl+Q"; context: Qt.ApplicationShortcut; onActivated: win.quitApp() }
    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        enabled: win.helpVisible || searchField.text.length > 0 || confirm.visible
        onActivated: {
            if (confirm.visible)
                confirm.close()
            else if (win.helpVisible)
                win.helpVisible = false
            else
                searchField.text = ""
        }
    }
    Shortcut {
        sequences: [StandardKey.Undo]
        context: Qt.ApplicationShortcut
        enabled: !win.typing
        onActivated: {
            var rows = backend.history
            for (var i = 0; i < rows.length; ++i)
                if (rows[i].undoable) {
                    backend.undo(rows[i].id)
                    return
                }
            toast.show(qsTr("Nothing to undo"), "ok")
        }
    }

    Shortcut {
        sequences: [StandardKey.Redo]
        context: Qt.ApplicationShortcut
        enabled: !win.typing
        onActivated: {
            // Redo is undoing the most recent undo.
            var rows = backend.history
            for (var i = 0; i < rows.length; ++i)
                if (rows[i].kind === "undo" && rows[i].undoable) {
                    backend.undo(rows[i].id)
                    return
                }
            toast.show(qsTr("Nothing to redo"), "ok")
        }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        id: shell
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Toolbar
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.round(theme.fontSize * 3.6)
                color: theme.canvas
                RowLayout {
                    anchors.fill: parent
                    spacing: 0
                    // The brand sits over the sidebar, and search starts where the content does.
                    Item {
                        Layout.preferredWidth: win.sidebarWidth
                        Layout.fillHeight: true
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 19
                            spacing: 10
                            AppIcon { name: "logo"; tint: theme.accentText; size: 22 }
                            AppText {
                                visible: !win.narrow
                                text: qsTr("Omarchy Control")
                                font.weight: Font.DemiBold
                                scale_: 1.15
                                Layout.fillWidth: true
                            }
                        }
                    }
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        AppField {
                            id: searchField
                            readonly property bool rtl: shell.LayoutMirroring.enabled
                            label: qsTr("Search settings, devices, and tools")
                            anchors.left: parent.left
                            anchors.leftMargin: 20
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.min(parent.width - 48, theme.fontSize * 46)
                            leftPadding: rtl ? 76 : 38
                            rightPadding: rtl ? 38 : 76
                            placeholderText: qsTr("Search settings, devices, and tools")
                            Keys.onDownPressed: results.currentIndex = Math.min(results.currentIndex + 1, results.count - 1)
                            Keys.onUpPressed: results.currentIndex = Math.max(results.currentIndex - 1, 0)
                            Keys.onReturnPressed: results.openCurrent()
                            Keys.onEnterPressed: results.openCurrent()
                            AppIcon { x: searchField.rtl ? searchField.width - width - 12 : 12; anchors.verticalCenter: parent.verticalCenter; name: "search"; tint: theme.muted }
                            Rectangle {
                                x: searchField.rtl ? 6 : searchField.width - width - 6
                                anchors.verticalCenter: parent.verticalCenter
                                width: keyHint.implicitWidth + 14
                                height: parent.height - 12
                                radius: theme.controlRadius
                                color: Qt.alpha(theme.foreground, 0.09)
                                visible: !searchField.activeFocus
                                AppText { id: keyHint; anchors.centerIn: parent; text: "Ctrl K"; secondary: true; mono: true; scale_: 0.88 }
                            }
                        }
                    }
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: theme.hairline }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                // Sidebar
                Rectangle {
                    Layout.fillHeight: true
                    Layout.preferredWidth: win.sidebarWidth
                    color: theme.rail
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 2
                        ListView {
                            id: navigation
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            model: backend.pages
                            spacing: 2
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            keyNavigationEnabled: true
                            delegate: Item {
                                // Thirteen items read as four groups: home, this desktop, the
                                // machine behind it, and this app.
                                readonly property int gap: ["appearance", "apps", "about"].indexOf(modelData.id) >= 0 ? 15 : 0
                                width: navigation.width
                                height: navButton.height + gap
                                // The line is what says these are groups, not uneven spacing.
                                Rectangle {
                                    visible: parent.gap > 0
                                    objectName: "navSeparator"
                                    x: 10
                                    y: Math.round((parent.gap - 1) / 2)
                                    width: parent.width - 20
                                    height: 1
                                    color: theme.hairline
                                }
                                AbstractButton {
                                    id: navButton
                                    objectName: "nav_" + modelData.id
                                    y: parent.gap
                                    width: parent.width
                                    height: Math.round(theme.fontSize * 2.7)
                                    hoverEnabled: true
                                    focusPolicy: Qt.StrongFocus
                                    readonly property bool selected: win.currentPage === modelData.id
                                    Accessible.role: Accessible.Button
                                    Accessible.name: qsTranslate("catalog", modelData.label)
                                    Accessible.selected: selected
                                    onClicked: win.go(modelData.id, "")
                                    ToolTip.visible: win.narrow && hovered
                                    ToolTip.text: qsTranslate("catalog", modelData.label)
                                    background: Rectangle {
                                        radius: theme.controlRadius
                                        color: navButton.selected ? "transparent" : navButton.down ? Qt.alpha(theme.foreground, 0.1) : navButton.hovered ? Qt.alpha(theme.foreground, 0.05) : "transparent"
                                        Behavior on color { enabled: theme.animate; ColorAnimation { duration: 100 } }
                                        // The accent, fading away from the reading edge.
                                        Rectangle {
                                            anchors.fill: parent
                                            visible: navButton.selected
                                            radius: parent.radius
                                            border.width: 1
                                            border.color: Qt.alpha(theme.accent, 0.28)
                                            gradient: Gradient {
                                                orientation: Gradient.Horizontal
                                                GradientStop { position: 0; color: Qt.alpha(theme.accent, navButton.mirrored ? 0.1 : 0.3) }
                                                GradientStop { position: 1; color: Qt.alpha(theme.accent, navButton.mirrored ? 0.3 : 0.1) }
                                            }
                                        }
                                        Rectangle {
                                            anchors.fill: parent
                                            visible: navButton.visualFocus
                                            radius: parent.radius
                                            color: "transparent"
                                            border.width: 2
                                            border.color: theme.accent
                                        }
                                    }
                                    contentItem: RowLayout {
                                        spacing: 12
                                        Item { Layout.preferredWidth: 4 }
                                        AppIcon {
                                            name: modelData.icon
                                            size: 18
                                            tint: navButton.selected ? theme.accentText : theme.foreground
                                            opacity: navButton.selected ? 1 : 0.8
                                        }
                                        AppText {
                                            visible: !win.narrow
                                            Layout.fillWidth: true
                                            text: qsTranslate("catalog", modelData.label)
                                            color: theme.foreground
                                        }
                                    }
                                }
                            }
                        }
                        // Side by side, or stacked when a long translation of
                        // "Shortcuts" would otherwise cut off the version.
                        GridLayout {
                            id: footer
                            readonly property bool stacked: !win.narrow && footerVersion.implicitWidth + footerShortcuts.implicitWidth + columnSpacing > width
                            Layout.fillWidth: true
                            Layout.margins: win.narrow ? 0 : 6
                            columns: stacked ? 1 : 2
                            rowSpacing: 2
                            AppText {
                                id: footerVersion
                                visible: !win.narrow
                                Layout.fillWidth: true
                                text: backend.omarchyVersion ? qsTr("Omarchy %1").arg(backend.omarchyVersion.split("-")[0]) : ""
                                secondary: true
                                mono: true
                                scale_: 0.88
                            }
                            AppButton {
                                id: footerShortcuts
                                style: "flat"
                                Layout.fillWidth: win.narrow
                                text: win.narrow ? "" : qsTr("Shortcuts")
                                leadingIcon: "help"
                                Accessible.name: qsTr("Shortcuts")
                                ToolTip.visible: win.narrow && hovered
                                ToolTip.text: qsTr("Shortcuts")
                                onClicked: win.helpVisible = true
                            }
                        }
                    }
                    Rectangle {
                        width: 1
                        height: parent.height
                        x: shell.LayoutMirroring.enabled ? 0 : parent.width - 1
                        color: theme.hairline
                    }
                }

                // Content
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true

                    Loader {
                        id: pageLoader
                        objectName: "pageLoader"
                        anchors.fill: parent
                        sourceComponent: win.currentPage === "overview" ? overviewComponent : pageComponent
                    }
                    Component { id: overviewComponent; OverviewPage {} }
                    Component {
                        id: pageComponent
                        GenericPage {
                            pageId: win.currentPage
                            page: {
                                var pages = backend.pages
                                for (var i = 0; i < pages.length; ++i)
                                    if (pages[i].id === win.currentPage)
                                        return pages[i]
                                return {}
                            }
                            focusId: win.focusRequest
                        }
                    }

                    // Search results replace the page while a query is present, and
                    // opening one only navigates: search never runs an action.
                    Rectangle {
                        anchors.fill: parent
                        visible: searchField.text.trim().length > 0
                        color: theme.canvas
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 20
                            spacing: 8
                            AppText {
                                Layout.fillWidth: true
                                secondary: true
                                text: results.count ? qsTr("%n result(s). Press Enter to go to the first.", "", results.count) : qsTr("Nothing matches. Try different words.")
                            }
                            ListView {
                                id: results
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                spacing: 2
                                currentIndex: 0
                                model: searchField.text.trim().length ? backend.search(searchField.text) : []
                                onModelChanged: currentIndex = 0
                                function openCurrent() {
                                    if (count > 0 && currentIndex >= 0 && currentIndex < model.length) {
                                        win.openResult(model[currentIndex])
                                    }
                                }
                                delegate: AbstractButton {
                                    id: hit
                                    width: results.width
                                    height: hitColumn.implicitHeight + 16
                                    hoverEnabled: true
                                    Accessible.role: Accessible.Button
                                    Accessible.name: qsTranslate("catalog", modelData.label)
                                    onClicked: win.openResult(modelData)
                                    background: Rectangle {
                                        radius: theme.controlRadius
                                        color: ListView.isCurrentItem || hit.ListView.isCurrentItem ? Qt.alpha(theme.accent, 0.18) : hit.hovered ? Qt.alpha(theme.foreground, 0.05) : "transparent"
                                    }
                                    contentItem: RowLayout {
                                        spacing: 12
                                        ColumnLayout {
                                            id: hitColumn
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 12
                                            spacing: 2
                                            AppText { Layout.fillWidth: true; text: qsTranslate("catalog", modelData.label) }
                                            AppText {
                                                Layout.fillWidth: true
                                                secondary: true
                                                scale_: 0.88
                                                text: modelData.available
                                                      ? qsTranslate("catalog", modelData.pageLabel) + " › " + qsTranslate("catalog", modelData.section) + (modelData.description ? " — " + qsTranslate("catalog", modelData.description) : "")
                                                      : modelData.reason
                                            }
                                        }
                                        AppText {
                                            Layout.rightMargin: 12
                                            secondary: true
                                            scale_: 0.88
                                            text: {
                                                switch (modelData.integration) {
                                                case "native": return qsTr("Setting")
                                                case "omarchy-panel": return qsTr("Omarchy panel")
                                                case "omarchy-wizard": return qsTr("Guided setup")
                                                case "external-helper": return qsTr("App")
                                                case "advanced-config": return qsTr("Config file")
                                                }
                                                return ""
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        ConfirmDialog { id: confirm; objectName: "confirmDialog" }
        ShortcutsOverlay { visible: win.helpVisible; onCloseRequested: win.helpVisible = false }
        ToastBar { id: toast }
    }
}
