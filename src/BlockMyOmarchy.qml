import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// My Omarchy: save this computer's choices, publish them to a private GitHub repository, and
// restore them on another computer. Every step shows what it will do first: a save shows
// what changes in the profile, a restore shows every change, and nothing reaches the network
// unless a button here says so. The work itself is the same the command line does.
AppPanel {
    id: root
    readonly property var profiles: backend.profiles
    readonly property var status: profiles.status || ({})
    readonly property var review: profiles.review || ({})
    readonly property var plan: profiles.plan || ({})
    readonly property var run: profiles.run || ({})
    // home, capture, review, connect, versions, restore, progress
    property string view: "home"
    readonly property string shown: run.state ? "progress" : view
    // What a save remembers, chosen in the capture view.
    property var chosen: ({})
    property var chosenApps: ({})
    property bool confirmPartial: false

    function takeFocus() {
        if (primaryHome.visible)
            primaryHome.forceActiveFocus(Qt.TabFocusReason)
    }
    function pick(key, on) {
        var next = Object.assign({}, chosen)
        next[key] = on
        chosen = next
    }
    function selection() {
        var categories = []
        for (var i = 0; i < profiles.categories.length; ++i)
            if (chosen["category." + profiles.categories[i].id])
                categories.push(profiles.categories[i].id)
        var files = []
        if (chosen.ghostty) files.push("ghostty")
        if (chosen.alacritty) files.push("alacritty")
        return {
            categories: categories, suggested: !!chosen.suggested, suggestedApps: !!chosen.suggestedApps,
            themes: !!chosen.themes, background: !!chosen.background, bar: !!chosen.bar, plugins: !!chosen.plugins,
            dock: !!chosen.dock, launchers: !!chosen.launchers, associations: !!chosen.associations,
            shortcuts: !!chosen.shortcuts, windowRules: !!chosen.windowRules, configFiles: files
        }
    }
    function when(version) {
        if (!version || !version.time)
            return ""
        return new Date(version.time).toLocaleString(Qt.locale(), Locale.ShortFormat)
    }
    function relationText() {
        switch (status.relation) {
        case "same": return qsTr("Up to date: GitHub has the newest version.")
        case "ahead": return status.ahead === 1 ? qsTr("One version is not published yet.") : qsTr("%1 versions are not published yet.").arg(status.ahead)
        case "behind":
        case "published-only": return qsTr("GitHub has a newer version than this computer.")
        case "diverged": return qsTr("Both this computer and GitHub have versions the other does not. Choose which to keep.")
        }
        return ""
    }
    function changeMark(change) {
        switch (change) {
        case "added": return "+"
        case "modified": return "~"
        case "removed": return "−"
        case "excluded": return "×"
        case "kept": return "!"
        }
        return "·"
    }
    function stateText(state) {
        switch (state) {
        case "verified": return qsTr("Done")
        case "running": return qsTr("Underway")
        case "pending": return qsTr("Waiting")
        case "failed": return qsTr("Failed")
        case "blocked": return qsTr("Not possible")
        case "skipped": return qsTr("Left as it is")
        case "interrupted": return qsTr("Interrupted")
        }
        return state
    }
    function runHeading() {
        switch (run.state) {
        case "starting":
        case "running": return qsTr("Restoring…")
        case "completed": return qsTr("Restored and verified")
        case "partial": return qsTr("Restored, with some items left out")
        case "incomplete": return qsTr("Restored in part: some items need attention")
        case "cancelled": return qsTr("Stopped")
        case "paused": return qsTr("Paused while another package operation runs")
        case "interrupted": return qsTr("Interrupted")
        case "not-started": return qsTr("The restore did not start")
        }
        return qsTr("Restore")
    }
    function listOf(value) { return value && value.length !== undefined ? value : [] }

    // ---- Shared pieces ----------------------------------------------------------------
    component Heading: Item {
        id: heading
        property string title
        property string icon
        width: parent ? parent.width : 400
        implicitHeight: headingRow.implicitHeight + 30
        height: implicitHeight
        RowLayout {
            id: headingRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            spacing: 14
            AppIcon { name: heading.icon; size: Math.round(theme.fontSize * 1.75); tint: theme.foreground }
            AppText {
                Layout.fillWidth: true
                text: heading.title
                font.weight: Font.DemiBold
                scale_: 1.45
                wrapMode: Text.Wrap
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }
        }
    }
    component Note: RowLayout {
        property string text
        property string icon: "info"
        property color tint: theme.muted
        property color textColor: theme.foreground
        Layout.fillWidth: true
        spacing: 10
        visible: text.length > 0
        AppIcon { Layout.alignment: Qt.AlignTop; Layout.topMargin: 2; name: parent.icon; size: Math.round(theme.fontSize * 1.1); tint: parent.tint }
        AppText {
            Layout.fillWidth: true
            text: parent.text
            wrapMode: Text.Wrap
            color: parent.textColor
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }
    component Subheading: AppText {
        Layout.fillWidth: true
        Layout.topMargin: 6
        font.weight: Font.Medium
        wrapMode: Text.Wrap
        Accessible.role: Accessible.Heading
        Accessible.name: text
    }
    Heading {
        title: root.shown === "capture" ? qsTr("What to remember")
             : root.shown === "review" ? qsTr("Review the save")
             : root.shown === "connect" ? qsTr("Connect GitHub")
             : root.shown === "versions" ? qsTr("Restore a version")
             : root.shown === "restore" ? qsTr("Review the restore")
             : root.shown === "progress" ? root.runHeading()
             : qsTr("Save and restore")
        icon: root.shown === "progress" ? (root.run.state === "completed" ? "checkcircle" : "refresh") : root.shown === "connect" ? "git" : "layers"
    }
    Rectangle { x: 16; width: parent.width - 32; height: 1; color: theme.hairline }

    Item {
        width: parent.width
        implicitHeight: body.implicitHeight + 40
        height: implicitHeight
        ColumnLayout {
            id: body
            x: 24
            y: 20
            width: parent.width - 48
            spacing: 14

            // What is happening now, and how the last step went.
            Note {
                objectName: "profileBusy"
                text: root.profiles.busy ? root.profiles.busyText : ""
                icon: "refresh"
                tint: theme.accentText
            }
            Note {
                objectName: "profileError"
                text: root.profiles.busy ? "" : root.profiles.error
                icon: "warning"
                tint: theme.dangerText
                textColor: theme.dangerText
            }

            // ---- Home -------------------------------------------------------------------
            ColumnLayout {
                visible: root.shown === "home"
                Layout.fillWidth: true
                spacing: 14
                AppText {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("Save this computer's settings, apps, and desktop choices, publish them to a private GitHub repository, and restore them on another Omarchy computer. You review every change before it happens.")
                }
                Note {
                    visible: root.status.git === false
                    text: root.status.git === false ? qsTr("Saving versions needs git, which is not installed.") : ""
                    icon: "warning"
                    tint: theme.warningText
                }
                // Saved on this computer, and published to GitHub, side by side.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: places.implicitHeight + 4
                    radius: theme.controlRadius
                    color: "transparent"
                    border.width: 1
                    border.color: theme.hairline
                    Column {
                        id: places
                        width: parent.width
                        y: 2
                        Repeater {
                            model: [
                                { name: "profileSaved", icon: "database", label: qsTr("Saved on this computer"),
                                  value: root.status.saved ? root.when(root.status.saved) + " · " + root.status.saved.message
                                       : root.status.hasProfile ? qsTr("Not saved as a version yet") : qsTr("Nothing saved yet"),
                                  note: root.status.unsavedChanges && root.status.saved ? qsTr("This computer's profile has changes since then.") : "" },
                                { name: "profilePublished", icon: "git", label: qsTr("Published to GitHub"),
                                  value: !root.status.connected ? qsTr("Not connected")
                                       : (root.status.repositoryName + (root.status.repository && root.status.repository.visibility ? " · " + root.status.repository.visibility : "")
                                          + (root.status.published ? " · " + root.when(root.status.published) : " · " + qsTr("nothing published yet"))),
                                  note: root.status.connected ? root.relationText() : "" }
                            ]
                            delegate: Item {
                                width: places.width
                                implicitHeight: placeRow.implicitHeight + 20
                                height: implicitHeight
                                Accessible.role: Accessible.StaticText
                                Accessible.name: modelData.label + ", " + modelData.value + (modelData.note ? ". " + modelData.note : "")
                                Rectangle { visible: index === 0; anchors.bottom: parent.bottom; x: 12; width: parent.width - 24; height: 1; color: theme.hairline }
                                RowLayout {
                                    id: placeRow
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.leftMargin: 14
                                    anchors.rightMargin: 14
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 12
                                    AppIcon { Layout.alignment: Qt.AlignTop; Layout.topMargin: 2; name: modelData.icon; size: Math.round(theme.fontSize * 1.2); tint: theme.muted }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        AppText { Layout.fillWidth: true; text: modelData.label; secondary: true; scale_: 0.9 }
                                        AppText { objectName: modelData.name; Layout.fillWidth: true; text: modelData.value; wrapMode: Text.Wrap }
                                        AppText { Layout.fillWidth: true; visible: modelData.note.length > 0; text: modelData.note; wrapMode: Text.Wrap; scale_: 0.9; color: theme.accentText }
                                    }
                                }
                            }
                        }
                    }
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 10
                    AppButton {
                        id: primaryHome
                        objectName: "profileStartSave"
                        style: "primary"
                        leadingIcon: "upload"
                        text: qsTr("Save this computer…")
                        enabled: !root.profiles.busy && root.status.git !== false
                        onClicked: root.view = "capture"
                    }
                    AppButton {
                        objectName: "profileStartRestore"
                        leadingIcon: "download"
                        text: qsTr("Restore…")
                        enabled: !root.profiles.busy
                        onClicked: { root.profiles.loadVersions(); root.view = "versions" }
                    }
                    AppButton {
                        objectName: "profileConnect"
                        visible: !root.status.connected
                        leadingIcon: "git"
                        text: qsTr("Connect GitHub…")
                        enabled: !root.profiles.busy
                        onClicked: root.view = "connect"
                    }
                    AppButton {
                        objectName: "profilePublish"
                        visible: root.status.connected === true && root.status.relation === "ahead"
                        leadingIcon: "upload"
                        text: qsTr("Publish")
                        enabled: !root.profiles.busy
                        onClicked: root.profiles.publish()
                    }
                    AppButton {
                        objectName: "profileFetch"
                        visible: root.status.connected === true
                        leadingIcon: "refresh"
                        text: qsTr("Check GitHub")
                        enabled: !root.profiles.busy
                        onClicked: root.profiles.fetch()
                    }
                }
                // Both sides changed: one of two explicit choices, neither losing anything.
                ColumnLayout {
                    visible: root.status.relation === "diverged"
                    Layout.fillWidth: true
                    spacing: 10
                    Note {
                        text: root.status.relation === "diverged" ? qsTr("Keeping this computer's saves it as a new version on top of GitHub's, so publishing adds to the history. Using GitHub's sets this computer's aside as a version of its own; nothing is lost either way.") : ""
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 10
                        AppButton { objectName: "profileKeepMine"; text: qsTr("Keep this computer's"); enabled: !root.profiles.busy; onClicked: root.profiles.resolve("mine") }
                        AppButton { objectName: "profileUsePublished"; text: qsTr("Use GitHub's"); enabled: !root.profiles.busy; onClicked: root.profiles.resolve("published") }
                    }
                }
                AppText {
                    visible: root.status.connected === true
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    secondary: true
                    scale_: 0.88
                    text: qsTr("Only Check GitHub and Publish use the network. Signing in stays with GitHub CLI.")
                }
            }

            // ---- What to remember --------------------------------------------------------
            ColumnLayout {
                visible: root.shown === "capture"
                Layout.fillWidth: true
                spacing: 12
                AppText {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: root.status.hasProfile
                          ? qsTr("Saving reads again everything the profile already remembers. Choose anything to add; nothing is saved until you have reviewed it.")
                          : qsTr("Choose what to remember. Nothing is saved until you have reviewed it.")
                }
                Repeater {
                    model: [
                        { heading: qsTr("Settings"), choices: root.profiles.categories.map(function (c) { return { key: "category." + c.id, label: c.label, description: "" } }).concat([
                            { key: "suggested", label: qsTr("Anything else changed from a fresh installation"), description: qsTr("Settings that differ from what Omarchy starts with.") }]) },
                        { heading: qsTr("Desktop"), choices: [
                            { key: "themes", label: qsTr("Themes installed from Git"), description: qsTr("Where each came from, so a restore can install it.") },
                            { key: "background", label: qsTr("Background"), description: qsTr("With the image, when it is one of your own.") },
                            { key: "bar", label: qsTr("Bar layout"), description: qsTr("Omarchy's own widgets, in order, with their options.") },
                            { key: "plugins", label: qsTr("Omarchy plugins switched off"), description: "" },
                            { key: "dock", label: qsTr("Dock"), description: "" },
                            { key: "launchers", label: qsTr("Launcher changes"), description: qsTr("Names, icons, and hidden apps.") },
                            { key: "associations", label: qsTr("Apps for file types"), description: "" },
                            { key: "shortcuts", label: qsTr("Shortcuts made here"), description: "" },
                            { key: "windowRules", label: qsTr("Window rules made here"), description: "" }] },
                        { heading: qsTr("Files and apps"), choices: [
                            { key: "ghostty", label: qsTr("Ghostty settings"), description: "" },
                            { key: "alacritty", label: qsTr("Alacritty settings"), description: "" },
                            { key: "suggestedApps", label: qsTr("Apps you installed"), description: qsTr("Shown in the review for you to pick from.") }] }
                    ]
                    delegate: ColumnLayout {
                        id: group
                        readonly property var choices: modelData.choices
                        Layout.fillWidth: true
                        spacing: 10
                        Subheading { text: modelData.heading }
                        Repeater {
                            model: group.choices
                            delegate: RowLayout {
                                Layout.fillWidth: true
                                spacing: 12
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    AppText { Layout.fillWidth: true; text: modelData.label; wrapMode: Text.Wrap }
                                    AppText { Layout.fillWidth: true; visible: modelData.description.length > 0; text: modelData.description; secondary: true; scale_: 0.88; wrapMode: Text.Wrap }
                                }
                                AppSwitch {
                                    objectName: "profileChoice_" + modelData.key
                                    label: modelData.label
                                    on: !!root.chosen[modelData.key]
                                    onClicked: root.pick(modelData.key, !on)
                                }
                            }
                        }
                    }
                }
                Flow {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    spacing: 10
                    AppButton {
                        objectName: "profileReviewSave"
                        style: "primary"
                        text: qsTr("Review")
                        enabled: !root.profiles.busy
                        onClicked: { root.chosenApps = ({}); root.profiles.startCapture(root.selection()); root.view = "review" }
                    }
                    AppButton { objectName: "profileCaptureBack"; text: qsTr("Back"); onClicked: root.view = "home" }
                }
            }

            // ---- Review the save ---------------------------------------------------------
            ColumnLayout {
                id: reviewView
                visible: root.shown === "review"
                Layout.fillWidth: true
                spacing: 12
                readonly property var items: root.listOf(root.review.items)
                readonly property var changes: items.filter(function (item) { return item.change !== "excluded" })
                readonly property var notCarried: items.filter(function (item) { return item.change === "excluded" })
                readonly property var suggestions: root.listOf(root.review.suggestedApps)
                property bool showNotCarried: false
                AppText {
                    visible: root.review.ok === true
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: reviewView.changes.length === 0
                          ? qsTr("Nothing changes in the profile. Saving records that this computer still agrees with it.")
                          : qsTr("Saving changes the profile like this.")
                }
                Repeater {
                    model: reviewView.changes.concat(reviewView.showNotCarried ? reviewView.notCarried : [])
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Accessible.role: Accessible.StaticText
                        Accessible.name: modelData.label + ": " + (modelData.after || modelData.before || "") + (modelData.reason ? ". " + modelData.reason : "")
                        AppText {
                            Layout.alignment: Qt.AlignTop
                            Layout.minimumWidth: Math.round(theme.fontSize * 1.2)
                            text: root.changeMark(modelData.change)
                            mono: true
                            color: modelData.change === "excluded" || modelData.attention ? theme.warningText : theme.accentText
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            AppText {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: modelData.change === "modified" && modelData.before
                                      ? qsTr("%1: %2 → %3").arg(modelData.label).arg(modelData.before).arg(modelData.after)
                                      : modelData.after ? qsTr("%1: %2").arg(modelData.label).arg(modelData.after)
                                      : modelData.label
                            }
                            AppText { Layout.fillWidth: true; visible: !!modelData.reason; text: modelData.reason || ""; secondary: true; scale_: 0.88; wrapMode: Text.Wrap }
                        }
                    }
                }
                // What a profile cannot carry is never dropped quietly, but it waits behind one line.
                AppButton {
                    objectName: "profileNotCarried"
                    visible: reviewView.notCarried.length > 0
                    style: "link"
                    text: reviewView.showNotCarried ? qsTr("Hide what is not carried")
                                                    : qsTr("%n thing(s) not carried, with the reasons", "", reviewView.notCarried.length)
                    onClicked: reviewView.showNotCarried = !reviewView.showNotCarried
                }
                AppText {
                    visible: (root.review.unchanged || 0) > 0
                    Layout.fillWidth: true
                    secondary: true
                    wrapMode: Text.Wrap
                    text: qsTr("%n already as saved.", "", root.review.unchanged || 0)
                }
                // Apps installed beyond Omarchy's own, to pick from; none is remembered unasked.
                Subheading { visible: reviewView.suggestions.length > 0; text: qsTr("Apps you installed") }
                Repeater {
                    model: reviewView.suggestions
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        AppText { Layout.fillWidth: true; text: modelData.label; wrapMode: Text.Wrap }
                        AppSwitch {
                            label: modelData.label
                            on: !!root.chosenApps[modelData.name]
                            onClicked: {
                                var next = Object.assign({}, root.chosenApps)
                                next[modelData.name] = !on
                                root.chosenApps = next
                            }
                        }
                    }
                }
                AppButton {
                    visible: reviewView.suggestions.length > 0
                    objectName: "profileIncludeApps"
                    text: qsTr("Add the chosen apps")
                    enabled: !root.profiles.busy && Object.keys(root.chosenApps).some(function (name) { return root.chosenApps[name] })
                    onClicked: root.profiles.includeApps(Object.keys(root.chosenApps).filter(function (name) { return root.chosenApps[name] }))
                }
                AppField {
                    id: saveMessage
                    visible: root.review.ok === true
                    objectName: "profileSaveMessage"
                    Layout.fillWidth: true
                    label: qsTr("Note for this version")
                    placeholderText: qsTr("Note for this version (optional)")
                }
                Flow {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    spacing: 10
                    AppButton {
                        objectName: "profileSaveHere"
                        visible: root.review.ok === true
                        style: "primary"
                        text: qsTr("Save on this computer")
                        enabled: !root.profiles.busy
                        onClicked: { root.profiles.saveReview(saveMessage.text, false); saveMessage.text = ""; root.view = "home" }
                    }
                    AppButton {
                        objectName: "profileSaveAndPublish"
                        visible: root.review.ok === true && root.review.connected === true
                        leadingIcon: "upload"
                        text: qsTr("Save and publish")
                        enabled: !root.profiles.busy
                        onClicked: { root.profiles.saveReview(saveMessage.text, true); saveMessage.text = ""; root.view = "home" }
                    }
                    AppButton { objectName: "profileReviewBack"; text: qsTr("Back"); onClicked: { root.profiles.clearReview(); root.view = "capture" } }
                }
            }

            // ---- Connect GitHub ----------------------------------------------------------
            ColumnLayout {
                visible: root.shown === "connect"
                Layout.fillWidth: true
                spacing: 12
                AppText {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("Versions are published to a private repository on your GitHub account. Signing in stays with GitHub CLI: Omarchy Control never sees a password or token.")
                }
                Note {
                    visible: root.status.githubCli === false
                    text: root.status.githubCli === false ? qsTr("GitHub CLI is not installed. Install github-cli, then run gh auth login in a terminal.") : ""
                    icon: "warning"
                    tint: theme.warningText
                }
                Subheading { text: qsTr("A new private repository") }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    AppField { id: newName; objectName: "profileNewRepository"; Layout.fillWidth: true; label: qsTr("New repository name"); text: "my-omarchy" }
                    AppButton {
                        objectName: "profileCreateRepository"
                        style: "primary"
                        text: qsTr("Create")
                        enabled: !root.profiles.busy && newName.text.trim().length > 0 && root.status.githubCli !== false
                        onClicked: root.profiles.createRepository(newName.text.trim())
                    }
                }
                Subheading { text: qsTr("A repository you already have") }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    AppField { id: existing; objectName: "profileExistingRepository"; Layout.fillWidth: true; label: qsTr("Repository"); placeholderText: qsTr("owner/name") }
                    AppButton {
                        objectName: "profileConnectRepository"
                        text: qsTr("Connect")
                        enabled: !root.profiles.busy && existing.text.trim().length > 0
                        onClicked: root.profiles.connectRepository(existing.text.trim(), allowPublic.on)
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    AppText { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("It is public, and I want my profile there anyway") }
                    AppSwitch { id: allowPublic; objectName: "profileAllowPublic"; label: qsTr("Allow a public repository"); onClicked: on = !on }
                }
                Note { text: root.status.connected ? qsTr("Connected to %1.").arg(root.status.repositoryName) : ""; icon: "check"; tint: theme.goodText }
                AppButton { objectName: "profileConnectBack"; text: qsTr("Back"); onClicked: root.view = "home" }
            }

            // ---- Restore a version -------------------------------------------------------
            ColumnLayout {
                visible: root.shown === "versions"
                Layout.fillWidth: true
                spacing: 12
                AppText {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("Choose a version. You see every change it would make before anything happens.")
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 10
                    AppButton {
                        objectName: "profileRestorePublished"
                        visible: !!root.status.published
                        style: "primary"
                        text: qsTr("Review the published version")
                        enabled: !root.profiles.busy
                        onClicked: { root.confirmPartial = false; root.profiles.planRestore("published"); root.view = "restore" }
                    }
                    AppButton {
                        objectName: "profileRestoreSaved"
                        visible: !!root.status.saved
                        style: root.status.published ? "outline" : "primary"
                        text: qsTr("Review the newest saved version")
                        enabled: !root.profiles.busy
                        onClicked: { root.confirmPartial = false; root.profiles.planRestore("saved"); root.view = "restore" }
                    }
                    AppButton {
                        visible: root.status.connected === true
                        leadingIcon: "refresh"
                        text: qsTr("Check GitHub")
                        enabled: !root.profiles.busy
                        onClicked: root.profiles.fetch()
                    }
                }
                Note {
                    text: !root.status.saved && !root.status.published
                          ? (root.status.connected ? qsTr("No versions yet. Check GitHub to get the published ones.") : qsTr("No versions yet. Save this computer, or connect GitHub to restore one published elsewhere."))
                          : ""
                }
                Subheading { visible: root.profiles.versions.length > 0; text: qsTr("Saved on this computer") }
                Repeater {
                    model: root.profiles.versions
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            AppText { Layout.fillWidth: true; text: modelData.message; wrapMode: Text.Wrap }
                            AppText { Layout.fillWidth: true; text: root.when(modelData) + " · " + modelData.short; secondary: true; scale_: 0.88 }
                        }
                        AppButton {
                            text: qsTr("Review")
                            Accessible.name: qsTr("Review version %1").arg(modelData.short)
                            enabled: !root.profiles.busy
                            onClicked: { root.confirmPartial = false; root.profiles.planRestore(modelData.commit); root.view = "restore" }
                        }
                    }
                }
                AppButton { objectName: "profileVersionsBack"; text: qsTr("Back"); onClicked: root.view = "home" }
            }

            // ---- Review the restore ------------------------------------------------------
            ColumnLayout {
                id: restoreView
                visible: root.shown === "restore"
                Layout.fillWidth: true
                spacing: 12
                readonly property var items: root.listOf(root.plan.items)
                readonly property var counts: root.plan.counts || ({})
                function group(filter) { return items.filter(filter) }
                readonly property var changes: group(function (item) { return item.willRun })
                readonly property var decisions: group(function (item) { return item.status === "conflict" })
                readonly property var blocked: group(function (item) { return item.status === "blocked" || item.status === "manual" })

                Repeater {
                    model: root.plan.valid === false ? root.listOf(root.plan.errors) : []
                    delegate: Note { text: modelData; icon: "warning"; tint: theme.dangerText }
                }
                AppText {
                    visible: root.plan.valid === true
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.weight: Font.Medium
                    text: restoreView.changes.length > 0 ? qsTr("%n change(s) would be made.", "", restoreView.changes.length)
                        : restoreView.decisions.length > 0 ? qsTr("Nothing changes until you decide below.")
                        : restoreView.blocked.length > 0 ? qsTr("Nothing here can be restored on this computer.")
                        : qsTr("Nothing would change: this computer already matches.")
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
                AppText {
                    visible: (restoreView.counts.unchanged || 0) > 0
                    Layout.fillWidth: true
                    secondary: true
                    text: qsTr("%n already as saved.", "", restoreView.counts.unchanged || 0)
                }
                Note {
                    text: root.plan.needsTerminal ? qsTr("Apps are installed and removed in a terminal, where sudo and pacman show the change and ask before anything happens. The restore keeps going there if this window closes.") : ""
                    icon: "terminal"
                    tint: theme.accentText
                }
                Subheading { visible: restoreView.changes.length > 0; text: qsTr("What changes") }
                Repeater {
                    model: restoreView.changes
                    delegate: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Accessible.role: Accessible.StaticText
                        Accessible.name: modelData.label + ": " + modelData.desired
                        AppText {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: modelData.current ? qsTr("%1: %2 → %3").arg(modelData.label).arg(modelData.current).arg(modelData.desired)
                                                    : qsTr("%1: %2").arg(modelData.label).arg(modelData.desired)
                        }
                        Repeater {
                            model: root.listOf(modelData.effects)
                            delegate: AppText { Layout.fillWidth: true; text: modelData; secondary: true; scale_: 0.88; wrapMode: Text.Wrap }
                        }
                        AppText { Layout.fillWidth: true; visible: !!modelData.reason && modelData.status === "ready"; text: modelData.reason || ""; secondary: true; scale_: 0.88; wrapMode: Text.Wrap }
                    }
                }
                // Choices someone may have made here: never overwritten without a decision.
                Subheading { visible: restoreView.decisions.length > 0; text: qsTr("Your decision") }
                Repeater {
                    model: restoreView.decisions
                    delegate: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        AppText {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: qsTr("%1: this computer has %2, the profile %3").arg(modelData.label).arg(modelData.current || qsTr("nothing")).arg(modelData.desired)
                        }
                        AppText { Layout.fillWidth: true; visible: !!modelData.reason; text: modelData.reason || ""; secondary: true; scale_: 0.88; wrapMode: Text.Wrap }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 8
                            AppButton {
                                objectName: "profileUseSaved"
                                text: qsTr("Use the profile's")
                                Accessible.name: qsTr("Use the profile's %1").arg(modelData.label)
                                style: modelData.resolution === "use-saved" ? "primary" : "outline"
                                enabled: !root.profiles.busy
                                onClicked: root.profiles.choose(modelData.id, modelData.resolution === "use-saved" ? "" : "use-saved")
                            }
                            AppButton {
                                objectName: "profileKeepLocal"
                                text: qsTr("Keep this computer's")
                                Accessible.name: qsTr("Keep this computer's %1").arg(modelData.label)
                                style: modelData.resolution === "keep-local" ? "primary" : "outline"
                                enabled: !root.profiles.busy
                                onClicked: root.profiles.choose(modelData.id, modelData.resolution === "keep-local" ? "" : "keep-local")
                            }
                        }
                    }
                }
                Subheading { visible: restoreView.blocked.length > 0; text: qsTr("Not restored here") }
                Repeater {
                    model: restoreView.blocked
                    delegate: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        AppText { Layout.fillWidth: true; text: modelData.label; wrapMode: Text.Wrap }
                        AppText { Layout.fillWidth: true; text: modelData.reason || ""; secondary: true; scale_: 0.88; wrapMode: Text.Wrap }
                    }
                }
                // Restoring what is ready, with anything left out said first.
                Note {
                    text: root.plan.valid === true && (root.plan.leftOut || 0) > 0 && restoreView.changes.length > 0
                          ? qsTr("%n item(s) are left out of this restore. The rest can still be restored.", "", root.plan.leftOut) : ""
                    icon: "warning"
                    tint: theme.warningText
                }
                Flow {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    spacing: 10
                    AppButton {
                        objectName: "profileRestoreStart"
                        visible: root.plan.valid === true && restoreView.changes.length > 0
                        style: "primary"
                        leadingIcon: "download"
                        text: (root.plan.leftOut || 0) > 0 ? qsTr("Restore the rest") : qsTr("Restore")
                        enabled: !root.profiles.busy
                        onClicked: { root.profiles.startRestore((root.plan.leftOut || 0) > 0) }
                    }
                    AppButton { objectName: "profileRestoreBack"; text: qsTr("Back"); onClicked: { root.profiles.clearPlan(); root.view = "versions" } }
                }
            }

            // ---- Progress and result -----------------------------------------------------
            ColumnLayout {
                visible: root.shown === "progress"
                Layout.fillWidth: true
                spacing: 12
                readonly property var steps: root.listOf(root.run.items)
                Note { objectName: "profileRunMessage"; text: root.run.message || ""; icon: root.run.state === "completed" ? "check" : "info"; tint: root.run.state === "completed" ? theme.goodText : theme.accentText }
                AppText {
                    visible: (root.run.total || 0) > 0
                    Layout.fillWidth: true
                    secondary: true
                    text: qsTr("%1 of %2 done").arg((root.run.counts || {}).verified || 0).arg(root.run.total || 0)
                }
                Repeater {
                    model: parent.steps
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Accessible.role: Accessible.StaticText
                        Accessible.name: modelData.label + ": " + root.stateText(modelData.state) + (modelData.message ? ". " + modelData.message : "")
                        AppIcon {
                            Layout.alignment: Qt.AlignTop
                            Layout.topMargin: 2
                            name: modelData.state === "verified" ? "check" : modelData.state === "failed" || modelData.state === "blocked" || modelData.state === "interrupted" ? "warning" : modelData.state === "running" ? "refresh" : "minus"
                            size: Math.round(theme.fontSize * 1.05)
                            tint: modelData.state === "verified" ? theme.goodText : modelData.state === "failed" || modelData.state === "interrupted" ? theme.dangerText : modelData.state === "blocked" ? theme.warningText : theme.muted
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            AppText { Layout.fillWidth: true; text: modelData.label + " · " + root.stateText(modelData.state); wrapMode: Text.Wrap }
                            AppText { Layout.fillWidth: true; visible: !!modelData.message; text: modelData.message || ""; secondary: true; scale_: 0.88; wrapMode: Text.Wrap }
                        }
                    }
                }
                Flow {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    spacing: 10
                    AppButton {
                        objectName: "profileRestoreStop"
                        visible: root.run.here === true
                        text: qsTr("Stop after this step")
                        onClicked: root.profiles.cancelRestore()
                    }
                    AppButton {
                        objectName: "profileRestoreResume"
                        visible: root.run.resumable === true
                        style: "primary"
                        text: qsTr("Resume")
                        enabled: !root.profiles.busy
                        onClicked: root.profiles.resumeRestore()
                    }
                    AppButton {
                        objectName: "profileRestoreDone"
                        visible: root.run.state !== "running" && root.run.state !== "starting" && root.run.here !== true
                        text: qsTr("Done")
                        onClicked: { root.profiles.clearRun(); root.profiles.clearPlan(); root.view = "home"; root.profiles.refresh() }
                    }
                }
            }
        }
    }
}
