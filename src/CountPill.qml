import QtQuick
import Omarchy.Ui as O

// How many things are behind a closed heading. The number is also in
// the heading's accessible name, so this one is not read twice.
O.Badge {
    property int count: 0
    text: count.toLocaleString(Qt.locale(), "f", 0)
    Accessible.ignored: true
}
