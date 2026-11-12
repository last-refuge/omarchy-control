QT += core gui qml quick quickcontrols2 svg network

CONFIG += c++17 release
TARGET = omarchy-control
TEMPLATE = app

VERSION = 0.5.0
# version.h, from version.h.in. qmake rewrites it only when the version changes, so a new
# version rebuilds main.cpp, where a define would leave an old object in place.
QMAKE_SUBSTITUTES += src/version.h.in
INCLUDEPATH += $$OUT_PWD/src

include(omarchy-control-sources.pri)

SOURCES += src/main.cpp

RESOURCES += src/resources.qrc src/qml.qrc
