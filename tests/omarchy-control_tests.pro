QT += core gui qml quick quickcontrols2 svg network testlib
CONFIG += c++17 testcase console
CONFIG -= app_bundle
TARGET = omarchy-control_tests
TEMPLATE = app
DEFINES += APP_VERSION=\\\"0.5.0\\\" PROJECT_ROOT=\\\"$$PWD/..\\\"

include(../omarchy-control-sources.pri)

HEADERS += fake_scripts.h
SOURCES += omarchy-control_tests.cpp
RESOURCES += ../src/resources.qrc ../src/qml.qrc
