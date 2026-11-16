import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: root
    spacing: 8
    readonly property var provider: backend.windowRules
    readonly property var actions: [
        { value: "float", label: qsTr("Always float") },
        { value: "tile", label: qsTr("Always tile") },
        { value: "workspace", label: qsTr("Open on workspace") },
        { value: "opacity", label: qsTr("Set opacity") },
        { value: "noblur", label: qsTr("Don't blur") }
    ]
    function actionLabel(rule) {
        for (var i = 0; i < actions.length; ++i)
            if (actions[i].value === rule.action)
                return actions[i].label + (rule.value ? " " + rule.value : "")
        return rule.action
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 8
        AppText {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            secondary: true
            scale_: 0.88
            text: qsTr("Rules in your own Hyprland files stay yours and are not listed here. See a window's class with “hyprctl clients”.")
        }
        AppPanel {
            Layout.fillWidth: true
            padding: 12
            RowLayout {
                width: parent.width
                spacing: 8
                AppField { id: matchField; Layout.fillWidth: true; label: qsTr("Window class"); placeholderText: qsTr("Window class, such as firefox") }
                AppCombo {
                    id: actionCombo
                    label: qsTr("Rule")
                    implicitWidth: 230
                    textRole: "label"
                    valueRole: "value"
                    model: root.actions
                }
                AppField {
                    id: valueField
                    visible: actionCombo.currentValue === "workspace" || actionCombo.currentValue === "opacity"
                    Layout.preferredWidth: 110
                    label: qsTr("Value")
                    placeholderText: actionCombo.currentValue === "opacity" ? "0.9 0.85" : "3"
                }
                AppButton {
                    text: qsTr("Add")
                    style: "primary"
                    enabled: matchField.text.trim().length > 0
                    onClicked: {
                        var value = valueField.visible ? valueField.text.trim() : ""
                        var problem = root.provider.validate(matchField.text, actionCombo.currentValue, value)
                        if (problem.length) {
                            toast.show(problem, "error")
                            return
                        }
                        win.withOwnership(function () {
                            root.provider.addRule(matchField.text, actionCombo.currentValue, value)
                            matchField.text = ""
                            valueField.text = ""
                        })
                    }
                }
            }
        }
        AppPanel {
            Layout.fillWidth: true
            visible: root.provider.rules.length > 0
            Repeater {
            model: root.provider.selectableRules
                delegate: Item {
                    width: parent ? parent.width : 400
                    height: theme.rowHeight
                    RowDivider {}
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 14
                        spacing: 10
                        AppText { text: modelData.match; Layout.preferredWidth: 260 }
                        AppText { text: root.actionLabel(modelData); secondary: true; Layout.fillWidth: true }
                AppButton { style: "flat"; text: qsTr("Remove"); onClicked: root.provider.removeRule(modelData.selector) }
                    }
                }
            }
        }
    }
}
