#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>
#include <functional>

#include "commandrunner.h"

// One package as pacman knows it here.
struct PackageInfo {
    QString name, version;
    QString repository;      // The sync repository that offers it; empty for a foreign package.
    bool explicitly = false; // Installed as wanted, not only as another package's dependency.
    bool foreign = false;    // In no configured repository: the AUR, a local build, or gone upstream.
};

// What is installed and what installing or removing something would do, read from pacman
// without root and without the network. Changes go through pacman itself, attached to a
// terminal so sudo and pacman can ask their own questions there. Nothing here ever deletes
// pacman's lock, forces a dependency, or removes more than the packages it is given.
class PackageProvider : public QObject {
    Q_OBJECT

public:
    explicit PackageProvider(CommandRunner *runner, QObject *parent = nullptr);

    struct Inventory {
        bool ok = false;
        QString error;
        QHash<QString, PackageInfo> installed;
        // What Omarchy installs on this platform, from `omarchy pkg defaults`. Empty when
        // that cannot be read, and then nothing is suggested from it.
        QStringList baseline;
        bool hasBaseline = false;
        // What Omarchy installs for particular hardware or from its installer image, from its
        // own omarchy-other.packages: drivers, firmware, the audio stack. None of it is an app
        // choice, so none of it is suggested, saved, or removed.
        QStringList hardware;
        // Omarchy's defaults that nothing installed here provides, as pacman's own dependency
        // test says: an alternative such as ex-vi-compat stands in for vi.
        QStringList missingDefaults;
        bool isInstalled(const QString &name) const { return installed.contains(name); }
    };
    void readInventory(std::function<void(const Inventory &)> done);

    // A dry run by pacman: every package a transaction would install or remove, as
    // "name version repository" or "name version". A refusal keeps pacman's reason, and for
    // a removal names the installed packages that still need it.
    struct Preview {
        bool ok = false;
        QString error;
        QStringList packages;
        QStringList missing;      // Install: names no configured repository offers.
        QStringList requiredBy;   // Removal: installed packages that depend on it.
    };
    void previewInstall(const QStringList &names, std::function<void(const Preview &)> done);
    void previewRemoval(const QStringList &names, std::function<void(const Preview &)> done);

    // Which of these names the AUR has, from its public lookup. This is the one call here that
    // reaches the network, so it only runs when someone asks for it. Nothing is downloaded or
    // built.
    struct AurLookup {
        bool ok = false;
        QString error;
        QStringList found;
    };
    void lookupAur(const QStringList &names, std::function<void(const AurLookup &)> done);
    static QString aurInfoUrl(const QStringList &names);
    bool hasAurHelper() const;

    // Never removed by a restore, whatever a profile asks: the system itself, the session
    // this desktop runs in, and what restoring depends on.
    static bool isProtected(const QString &name);
    // Hardware and system packages: kernels, firmware, microcode, and drivers. They belong
    // to a machine, not to someone's choice of apps, so they are never suggested.
    static bool isSystemPackage(const QString &name);
    bool pacmanBusy() const;

    // Attached to this terminal and waiting with no time limit, as only the CLI may. Each
    // returns pacman's exit code, or -1 with `error` set when it never started.
    int install(const QStringList &names, QString *error = nullptr);
    int markExplicit(const QStringList &names, QString *error = nullptr);
    int remove(const QStringList &names, QString *error = nullptr);
    // One AUR package, built by yay. yay offers the build files for review before anything is
    // built, whatever answer its own settings keep, and pacman asks before installing it.
    int buildFromAur(const QString &name, QString *error = nullptr);

    static QStringList parseNames(const QByteArray &output);
    static QHash<QString, QString> parseRepositories(const QByteArray &syncList);

private:
    QStringList pacman(const QStringList &arguments) const;

    CommandRunner *m_runner;
};
