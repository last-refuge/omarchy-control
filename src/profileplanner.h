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

// One thing a restore would do, or a reason it will not.
struct ProfilePlanItem {
    QString id;      // "setting:<id>", "package:<name>", "desktop", "files", "assets"
    QString kind;    // setting, package, section
    QString target;  // The setting id, the package name, or the section name.
    QString label;
    QString category;
    QString mode;     // set or inherit for a setting; present or absent for a package
    QString desired;  // The saved value, for "set"; a package's source.
    // What a package item does: install, build (from the AUR), mark-explicit, or remove.
    // Empty for settings.
    QString action;
    // Every package pacman said the transaction would install or remove. A restore checks
    // this again before it starts, and stops if the transaction has changed.
    QStringList transaction;
    // Items that must succeed first, as a default app needs its app installed.
    QStringList needs;
    // What this computer had when the plan was made. A restore starts only while that is
    // still true, so it never overwrites something that changed after the review.
    QString current;
    QString currentStatus;
    bool currentOverride = false;
    // ready, unchanged, conflict, blocked, manual
    QString status;
    QString reason;
    // For a conflict, the choice made: "use-saved" restores the saved value, "keep-local"
    // leaves this computer's. Without one, a conflict is never restored.
    QString resolution;
    QStringList after;  // Item ids restored first.
    QStringList activation;
    QStringList effects;
    QString confirmText;
    QString recovery;
    QString restart;
    bool needsOwnership = false;

    bool willRun() const;
    QJsonObject toJson() const;
    static ProfilePlanItem fromJson(const QJsonObject &object);
};

// A read-only restore plan, saved under the app's state directory. Making one changes no
// setting. Applying it later checks that neither the profile nor anything it touches has
// changed since, and refuses rather than act on a stale review.
struct ProfilePlan {
    QString id;
    QString createdAt;
    QString directory;
    QString profileName;
    QString profileDigest;
    bool valid = false;
    QStringList errors;       // Why the profile was refused. Nothing is planned then.
    QJsonObject destination;  // Versions here, for the record.
    QList<ProfilePlanItem> items;  // In restore order.

    int count(const QString &status) const;
    // Items with nothing to do (unchanged) aside, what will not be restored.
    int leftOut() const;
    QJsonObject toJson() const;
    static ProfilePlan fromJson(const QJsonObject &object);
};

class ProfilePlanner : public QObject {
    Q_OBJECT

public:
    struct Options {
        QStringList useSaved;   // Conflicts to resolve by restoring the saved value; "all" for every one.
        QStringList keepLocal;  // Conflicts to resolve by keeping this computer's value.
    };
    using Done = std::function<void(const ProfilePlan &)>;

    ProfilePlanner(const Catalog *catalog, SettingsEngine *engine, Capabilities *capabilities, PackageProvider *packages, const Env &env,
                   QObject *parent = nullptr);

    void plan(const QString &directory, const Options &options, Done done);
    bool save(const ProfilePlan &plan, QString *error) const;
    ProfilePlan load(const QString &id, QString *error) const;

    // Whether what was read is already what the item wants.
    static bool satisfied(const CatalogEntry &entry, const ProfilePlanItem &item, const ReadResult &read);
    // Whether what was read is still what the plan saw.
    static bool unchangedSincePlan(const CatalogEntry &entry, const ProfilePlanItem &item, const ReadResult &read);

    // A package's state here: "explicit", "dependency", or "missing".
    static QString packageState(const PackageProvider::Inventory &inventory, const QString &name);
    static bool packageSatisfied(const ProfilePlanItem &item, const QString &state);

    // The dock, launchers, file associations, and owned shortcuts and rules.
    void setDesktop(ProfileDesktop *desktop) { m_desktop = desktop; }

private:
    void planSettings(const Profile &profile, const Options &options, ProfilePlan plan, Done done);
    QString m_directory;  // The profile being planned, which holds its files.
    void planPackages(const Profile &profile, const Options &options, std::function<void(const QList<ProfilePlanItem> &)> done);
    void planDesktop(const Profile &profile, const Options &options, const ProfileDesktop::Context &context,
                     std::function<void(const QList<ProfilePlanItem> &)> done);
    // Themes installed from Git, read from their files alone, so the settings that select
    // them can wait for them as default apps wait for their apps.
    QList<ProfilePlanItem> planThemes(const Profile &profile) const;
    QString provenance(const CatalogEntry &entry, const ReadResult &read) const;

    const Catalog *m_catalog;
    SettingsEngine *m_engine;
    Capabilities *m_capabilities;
    PackageProvider *m_packages;
    Env m_env;
    ProfileDesktop *m_desktop = nullptr;
};
