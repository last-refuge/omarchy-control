# Everything except main(), shared with the test binary and screenshot harness.
INCLUDEPATH += $$PWD/src
# The tray icon's host is found on the session bus.
QT += dbus
# Icon pack archives are read with libarchive, which pacman itself depends on.
LIBS += -larchive

HEADERS += \
    $$PWD/src/env.h \
    $$PWD/src/commandrunner.h \
    $$PWD/src/fileops.h \
    $$PWD/src/theme.h \
    $$PWD/src/translator.h \
    $$PWD/src/iconprovider.h \
    $$PWD/src/iconthemes.h \
    $$PWD/src/iconthemeprovider.h \
    $$PWD/src/iconthemeimporter.h \
    $$PWD/src/iconlayer.h \
    $$PWD/src/iconarchive.h \
    $$PWD/src/launchereditor.h \
    $$PWD/src/catalog.h \
    $$PWD/src/capabilities.h \
    $$PWD/src/blur.h \
    $$PWD/src/hypromodule.h \
    $$PWD/src/changecoordinator.h \
    $$PWD/src/settingsengine.h \
    $$PWD/src/helperlauncher.h \
    $$PWD/src/audioprovider.h \
    $$PWD/src/bluetoothprovider.h \
    $$PWD/src/networkprovider.h \
    $$PWD/src/displayprovider.h \
    $$PWD/src/servicesprovider.h \
    $$PWD/src/shortcutsprovider.h \
    $$PWD/src/pluginsprovider.h \
    $$PWD/src/systemprovider.h \
    $$PWD/src/windowrulesprovider.h \
    $$PWD/src/updater.h \
    $$PWD/src/compat.h \
    $$PWD/src/packageprovider.h \
    $$PWD/src/profilemodel.h \
    $$PWD/src/profilecapture.h \
    $$PWD/src/profiledesktop.h \
    $$PWD/src/profilefiles.h \
    $$PWD/src/profileplanner.h \
    $$PWD/src/profileprovider.h \
    $$PWD/src/profilerepository.h \
    $$PWD/src/profilerunner.h \
    $$PWD/src/appcore.h \
    $$PWD/src/backend.h \
    $$PWD/src/coverage.h \
    $$PWD/src/cli.h \
    $$PWD/src/trayhost.h \
    $$PWD/src/gui.h

SOURCES += \
    $$PWD/src/env.cpp \
    $$PWD/src/commandrunner.cpp \
    $$PWD/src/fileops.cpp \
    $$PWD/src/theme.cpp \
    $$PWD/src/translator.cpp \
    $$PWD/src/iconprovider.cpp \
    $$PWD/src/iconthemes.cpp \
    $$PWD/src/iconthemeprovider.cpp \
    $$PWD/src/iconthemeimporter.cpp \
    $$PWD/src/iconlayer.cpp \
    $$PWD/src/iconarchive.cpp \
    $$PWD/src/launchereditor.cpp \
    $$PWD/src/catalog.cpp \
    $$PWD/src/capabilities.cpp \
    $$PWD/src/blur.cpp \
    $$PWD/src/hypromodule.cpp \
    $$PWD/src/changecoordinator.cpp \
    $$PWD/src/settingsengine.cpp \
    $$PWD/src/helperlauncher.cpp \
    $$PWD/src/audioprovider.cpp \
    $$PWD/src/bluetoothprovider.cpp \
    $$PWD/src/networkprovider.cpp \
    $$PWD/src/displayprovider.cpp \
    $$PWD/src/servicesprovider.cpp \
    $$PWD/src/shortcutsprovider.cpp \
    $$PWD/src/pluginsprovider.cpp \
    $$PWD/src/systemprovider.cpp \
    $$PWD/src/windowrulesprovider.cpp \
    $$PWD/src/updater.cpp \
    $$PWD/src/compat.cpp \
    $$PWD/src/packageprovider.cpp \
    $$PWD/src/profilemodel.cpp \
    $$PWD/src/profilecapture.cpp \
    $$PWD/src/profiledesktop.cpp \
    $$PWD/src/profilefiles.cpp \
    $$PWD/src/profileplanner.cpp \
    $$PWD/src/profileprovider.cpp \
    $$PWD/src/profilerepository.cpp \
    $$PWD/src/profilerunner.cpp \
    $$PWD/src/backend.cpp \
    $$PWD/src/coverage.cpp \
    $$PWD/src/cli.cpp \
    $$PWD/src/trayhost.cpp \
    $$PWD/src/gui.cpp
