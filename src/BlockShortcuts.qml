import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: root
    spacing: 8
    readonly property var shortcuts: backend.shortcuts
    property string filter: ""
    property string captured: ""
    readonly property var check: {
        // Re-evaluate a captured chord when discovery finishes, too.
        const state = shortcuts.status
        return captured.length ? shortcuts.checkKeys(captured) : ({ valid: false, available: false, conflicts: [], critical: false })
    }
    readonly property var visibleRows: shortcuts.binds.filter(function (b) {
        return !filter.length || (b.keys + " " + b.description).toLowerCase().indexOf(filter.toLowerCase()) >= 0
    })

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 8

        AppPanel {
            Layout.fillWidth: true
            padding: 12
            ColumnLayout {
                width: parent.width
                spacing: 8
                AppText { text: qsTr("Add a shortcut") }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    // Captures the chord itself, so nobody has to know key names.
                    AppField {
                        id: capture
                        Layout.preferredWidth: 220
                        label: qsTr("Keys")
                        readOnly: true
                        placeholderText: qsTr("Press keys…")
                        text: root.captured
                        Keys.onPressed: function (event) {
                            if ([Qt.Key_Control, Qt.Key_Shift, Qt.Key_Alt, Qt.Key_Meta, Qt.Key_Super_L, Qt.Key_Super_R, Qt.Key_Tab, Qt.Key_Backtab].indexOf(event.key) >= 0)
                                return
                            var mods = []
                            if (event.modifiers & Qt.MetaModifier) mods.push("SUPER")
                            if (event.modifiers & Qt.ControlModifier) mods.push("CTRL")
                            if (event.modifiers & Qt.AltModifier) mods.push("ALT")
                            if (event.modifiers & Qt.ShiftModifier) mods.push("SHIFT")
                            var names = {}
                            names[Qt.Key_Return] = "RETURN"; names[Qt.Key_Space] = "SPACE"; names[Qt.Key_Escape] = "ESCAPE"
                            names[Qt.Key_Left] = "LEFT"; names[Qt.Key_Right] = "RIGHT"; names[Qt.Key_Up] = "UP"; names[Qt.Key_Down] = "DOWN"
                            names[Qt.Key_Backspace] = "BACKSPACE"; names[Qt.Key_Delete] = "DELETE"; names[Qt.Key_Home] = "HOME"; names[Qt.Key_End] = "END"
                            var key = names[event.key]
                            if (!key && event.key >= Qt.Key_F1 && event.key <= Qt.Key_F24) key = "F" + (event.key - Qt.Key_F1 + 1)
                            if (!key && event.text.length === 1) key = event.text.toUpperCase()
                            if (!key) return
                            root.captured = mods.concat([key]).join(" + ")
                            event.accepted = true
                        }
                    }
                    AppField { id: descriptionField; Layout.preferredWidth: 220; label: qsTr("Description"); placeholderText: qsTr("Description") }
                    AppField { id: commandField; Layout.fillWidth: true; label: qsTr("Command"); placeholderText: qsTr("Command to run") }
                    AppButton {
                        text: root.check.conflicts.length ? qsTr("Replace") : qsTr("Add")
                        style: root.check.conflicts.length ? "danger" : "primary"
                        enabled: root.check.valid && root.check.available && commandField.text.trim().length > 0
                        onClicked: {
                            var save = function () {
                                root.shortcuts.addBinding(root.captured, descriptionField.text, commandField.text, root.check.conflicts.length > 0)
                                root.captured = ""
                                commandField.text = ""
                                descriptionField.text = ""
                            }
                            if (!root.check.conflicts.length) {
                                win.withOwnership(save)
                            } else {
                                confirm.open_(qsTr("Replace %1").arg(root.check.keys),
                                              root.check.critical ? qsTr("This shortcut is one of the main ways back into the desktop. Make sure you keep another way to open the launcher.")
                                                                  : qsTr("It currently does: %1").arg(root.check.conflicts.join(", ")),
                                              [], qsTr("Replace"), true, function () { win.withOwnership(save) })
                            }
                        }
                    }
                }
                AppText {
                    visible: root.captured.length > 0
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: !root.check.valid || root.check.conflicts.length ? theme.warning : theme.muted
                    text: !root.check.valid ? qsTr("A shortcut needs at least one modifier and one key.")
                        : !root.check.available ? qsTr("Shortcuts could not be read. Try again after the desktop is available.")
                        : root.check.conflicts.length ? qsTr("%1 is already used by: %2").arg(root.check.keys).arg(root.check.conflicts.join(", "))
                        : root.check.collisionState === "unknown" ? qsTr("Some shortcuts use keycodes or multiple keys and cannot be compared by name.")
                        : qsTr("No matching named shortcut was reported for %1.").arg(root.check.keys)
                }
            }
        }

        Repeater {
            model: root.shortcuts.ownedBinds
            delegate: RowLayout {
                Layout.fillWidth: true
                spacing: 10
                AppText { text: modelData.keys; color: theme.accentText; Layout.preferredWidth: 260 }
                AppText { text: modelData.description || modelData.command; Layout.fillWidth: true }
                AppButton {
                    style: "flat"; text: qsTr("Remove")
                    onClicked: {
                        var selected = modelData.selector
                        var chord = modelData.keys
                        var remove = function () { root.shortcuts.removeBinding(selected) }
                        if (root.shortcuts.checkKeys(chord).critical)
                            confirm.open_(qsTr("Remove %1").arg(chord),
                                          qsTr("This shortcut is one of the main ways back into the desktop. Make sure you keep another way to open the launcher."),
                                          [], qsTr("Remove"), true, remove)
                        else remove()
                    }
                }
            }
        }

        AppField { Layout.fillWidth: true; label: qsTr("Filter shortcuts"); placeholderText: qsTr("Filter shortcuts"); onTextChanged: root.filter = text }
        AppPanel {
            Layout.fillWidth: true
            Repeater {
                model: root.visibleRows.slice(0, 80)
                delegate: Item {
                    width: parent ? parent.width : 400
                    height: 32
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 14
                        spacing: 10
                        AppText { text: modelData.keys; color: theme.accentText; Layout.preferredWidth: 280 }
                        AppText { text: modelData.description; Layout.fillWidth: true }
                    }
                }
            }
        }
    }
}
