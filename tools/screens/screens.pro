# Offscreen screenshot harness: runs the real QML against a synthetic machine, so
# images never contain anyone's personal data.
QT += core gui qml quick quickcontrols2 svg network
CONFIG += c++17
TARGET = screens
TEMPLATE = app
DEFINES += APP_VERSION=\\\"0.4.0\\\"
include(../../omarchy-control-sources.pri)
SOURCES += main.cpp
RESOURCES += ../../src/resources.qrc ../../src/qml.qrc
