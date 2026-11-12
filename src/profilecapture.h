#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <functional>

#include "capabilities.h"
#include "catalog.h"
#include "packageprovider.h"
#include "profiledesktop.h"
#include "profilemodel.h"
#include "settingsengine.h"

// Reads what a profile remembers, plus whatever is newly chosen, from the live machine,
// and says what changed against the saved profile before anything is written. Reading
// never changes a setting, starts an install, or touches the network.
class ProfileCapture : public QObject {
    Q_OBJECT

public:
    struct Selection {
        QStringList settings;    // Remember these, at their current values.
        QStringList categories;  // Remember every portable setting in these.
        bool suggested = false;  // Remember settings that differ from a fresh installation.
        QStringList inherit;     // Remember these as "use this computer's default".
        QStringList forget;      // Stop remembering these.
        QStringList apps;        // Remember these installed packages as wanted.
        QStringList excludeApps; // Remember these as unwanted: a restore removes them.
        QStringList forgetApps;  // Stop remembering these packages either way.
        bool suggestedApps = false;  // Remember what is installed beyond Omarchy's defaults.
        // Ask the AUR which of the saved apps from no configured repository it has, so a
        // restore can build them. The only part of a capture that uses the network.
        bool resolveAur = false;
        // Desktop records: the dock's pins, launchers changed here, the everyday file
        // associations, and the shortcuts and window rules this app owns.
        bool dock = false;
        bool launchers = false;
        bool associations = false;
        bool shortcuts = false;
        bool windowRules = false;
        bool background = false;  // The background, with its file when it is one of the user's.
        // Where every theme installed from Git came from. The theme the profile selects comes
        // along whenever it is one, chosen or not, since restoring the setting needs it.
        bool themes = false;
        bool bar = false;      // The bar's layout: Omarchy's own widgets, in order, with their options.
        bool plugins = false;  // Which of Omarchy's own plugins are switched off.
        QStringList configFiles;  // Configuration files by adapter: ghostty, alacritty.
        QString name;            // Rename the profile.
        bool isEmpty() const;
    };

    struct Item {
        QString kind = QStringLiteral("setting");  // setting, package, desktop
        QString id, label, category;
        // added, modified, unchanged, removed, kept, excluded, and for apps "suggested":
        // shown as a possible exclusion, never recorded on its own.
        QString change;
        ProfileSetting before, after;
        ProfilePackage packageBefore, packageAfter;
        QString reason;
        // A saved choice whose current value could not be read. It stays as saved: a
        // failed read is never a reason to forget something.
        bool attention = false;
    };

    struct Result {
        bool ok = false;
        QString error;
        Profile profile;
        bool existed = false;
        bool changed = false;  // The choices differ from what was saved.
        QList<Item> items;
        // setting -> what was read just now, for the agreed-value record.
        QJsonObject agreed;
        // digest -> the local file a newly carried asset is copied from when saving.
        QHash<QString, QString> assetSources;
        // "<adapter>/<name>" -> the configuration file's contents as read, written when saving.
        QHash<QString, QByteArray> configContents;
        QJsonObject toJson() const;
    };

    using Done = std::function<void(const Result &)>;

    ProfileCapture(const Catalog *catalog, SettingsEngine *engine, Capabilities *capabilities, PackageProvider *packages, const Env &env,
                   QObject *parent = nullptr);

    // `existing` is the profile saved so far, or null for a first save.
    void capture(const Profile *existing, const Selection &selection, Done done);
    // Writes the profile when its choices changed, and remembers the values just read as
    // agreed on this computer either way. An unchanged capture leaves the file alone.
    bool save(const QString &directory, const Result &result, QString *error);

    // Whether a fresh installation would read differently, from the setting's provenance.
    bool differsFromStock(const CatalogEntry &entry, const ReadResult &read) const;

    // The dock, launchers, file associations, and owned shortcuts and rules.
    void setDesktop(ProfileDesktop *desktop) { m_desktop = desktop; }

private:
    QString capturedValue(const CatalogEntry &entry, const ReadResult &read) const;
    void capturePackages(std::shared_ptr<Result> result, const Selection &selection, std::function<void()> finished);
    void captureDesktop(std::shared_ptr<Result> result, const Selection &selection, std::function<void()> finished);
    void captureFiles(std::shared_ptr<Result> result, const Selection &selection);
    void captureThemes(std::shared_ptr<Result> result, const Selection &selection);

    const Catalog *m_catalog;
    SettingsEngine *m_engine;
    Capabilities *m_capabilities;
    PackageProvider *m_packages;
    Env m_env;
    ProfileDesktop *m_desktop = nullptr;
};
