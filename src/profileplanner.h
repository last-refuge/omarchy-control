#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <functional>

#include "capabilities.h"
#include "catalog.h"
#include "profilemodel.h"
#include "settingsengine.h"

// One thing a restore would do, or a reason it will not.
struct ProfilePlanItem {
    QString id;      // "setting:<id>", "packages", "desktop", "files", "assets"
    QString kind;    // setting, section
    QString target;  // The setting id, or the section name.
    QString label;
    QString category;
    QString mode;     // set, inherit
    QString desired;  // The saved value, for "set".
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

    ProfilePlanner(const Catalog *catalog, SettingsEngine *engine, Capabilities *capabilities, const Env &env, QObject *parent = nullptr);

    void plan(const QString &directory, const Options &options, Done done);
    bool save(const ProfilePlan &plan, QString *error) const;
    ProfilePlan load(const QString &id, QString *error) const;

    // Whether what was read is already what the item wants.
    static bool satisfied(const CatalogEntry &entry, const ProfilePlanItem &item, const ReadResult &read);
    // Whether what was read is still what the plan saw.
    static bool unchangedSincePlan(const CatalogEntry &entry, const ProfilePlanItem &item, const ReadResult &read);

private:
    void planSettings(const Profile &profile, const Options &options, ProfilePlan plan, Done done);
    QString provenance(const CatalogEntry &entry, const ReadResult &read) const;

    const Catalog *m_catalog;
    SettingsEngine *m_engine;
    Capabilities *m_capabilities;
    Env m_env;
};
