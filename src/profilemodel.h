#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

#include "catalog.h"
#include "env.h"

// One remembered setting. "set" keeps the chosen value; "inherit" asks for the
// destination's own default, only where the setting can really return to one. A
// setting the profile leaves out is not managed at all and is left alone.
struct ProfileSetting {
    QString mode;   // set, inherit
    QString value;  // The catalog's normalized string, for "set".
    bool operator==(const ProfileSetting &other) const { return mode == other.mode && value == other.value; }
    bool operator!=(const ProfileSetting &other) const { return !(*this == other); }
};

// One app a profile asks for, or explicitly does not want. Leaving an app out of a
// profile never removes it: only "absent" does.
struct ProfilePackage {
    QString name;
    QString source;  // repository, aur, manual
    QString intent;  // present, absent
    QString repository;
    bool operator==(const ProfilePackage &other) const {
        return name == other.name && source == other.source && intent == other.intent && repository == other.repository;
    }
    QJsonObject toJson() const;
    static ProfilePackage fromJson(const QJsonObject &object);
};

// profile.json: the choices that make an Omarchy machine someone's own. It records
// what is wanted; the live machine stays the authority on what is in effect. Sections
// this version cannot restore yet are still validated, and kept exactly as they are
// when the profile is saved again, so a save never drops them.
struct Profile {
    static constexpr int SchemaVersion = 1;
    int schemaVersion = SchemaVersion;
    QString name;
    QJsonObject source;  // Where it was saved. Diagnostic, never an instruction to downgrade.
    QMap<QString, ProfileSetting> settings;
    QJsonArray packages;
    QJsonObject desktop;
    QJsonArray files;
    QJsonArray assets;

    QList<ProfilePackage> packageList() const;
    ProfilePackage package(const QString &name) const;
    bool hasPackage(const QString &name) const { return !package(name).name.isEmpty(); }
    void setPackage(const ProfilePackage &package);
    void removePackage(const QString &name);

    QJsonObject toJson() const;
    // Canonical bytes: sorted keys, stable record order, one trailing newline. The same
    // choices always give the same bytes, so an unchanged capture changes nothing.
    QByteArray serialize() const;
    // The whole canonical file, which a restore plan is built against.
    QString digest() const;
    // The same choices, whatever the diagnostic `source` says.
    bool sameChoices(const Profile &other) const;
    bool hasUnsupportedSections() const;
};

// How a catalog setting travels in a profile, from its reviewed `profile` block.
struct ProfileTraits {
    QString portability;  // portable, later, machine, runtime; empty when never reviewed
    QString category;
    QString reason;       // Why it does not travel, for anything but portable.
    bool inherit = false; // "Use the default" can be restored reliably.
    bool hasStock = false;
    QString stock;        // The value a fresh installation has, where that is known.
    QStringList after;    // Settings restored before this one.
    QStringList activation;
    // Another setting whose value, when it is `when`, overrides this one's effect, as
    // turning window gaps off zeroes the gap sizes whatever they are set to.
    struct Overlap {
        QString setting, when;
    };
    QList<Overlap> overlaps;

    bool portable() const { return portability == QLatin1String("portable"); }
    static ProfileTraits of(const CatalogEntry &entry);
    // The categories a capture can select, in the order they are shown.
    static QStringList categories();
    static QString describeActivation(const QString &key);
    // The package a default-app choice opens, from the catalog: "" when it names none.
    static QString packageFor(const CatalogEntry &entry, const QString &value, QString *source = nullptr);
};

namespace ProfileModel {

constexpr qint64 MaxManifestBytes = 1024 * 1024;
constexpr qint64 MaxFileBytes = 25 * 1024 * 1024;
constexpr qint64 MaxProfileBytes = 100 * 1024 * 1024;

struct Parsed {
    bool ok = false;
    Profile profile;
    QStringList errors;      // Why it was refused. Nothing is restored from a refused profile.
    int originalSchema = 0;  // Before any migration.
    QStringList notes;       // What a migration changed in meaning.
};

// Validates everything before anything can use it: malformed JSON, duplicate keys or
// identities, unknown sections or file adapters, and newer schemas are all refused.
Parsed parse(const QByteArray &json);
// Reads <directory>/profile.json. `exists` tells a missing profile from a broken one.
Parsed load(const QString &directory, bool *exists = nullptr);
bool save(const QString &directory, const Profile &profile, QString *error);
QString manifestPath(const QString &directory);

// The reviewed adapters a profile's files may name. Profiles supply data, never code.
QStringList fileAdapters();
bool isSettingId(const QString &id);
bool isPackageName(const QString &name);

}  // namespace ProfileModel

// What this computer remembers about restores: plans, runs, and the last value it
// verified for each setting. Private to this machine, under the app's state directory,
// and never part of a profile.
namespace ProfileStore {

QString root(const Env &env);
QString plansDir(const Env &env);
QString runsDir(const Env &env);
QString baselinePath(const Env &env);

QString newId(const QString &prefix);
// Plan and run ids become file names, so only the shape newId() makes is accepted.
bool isId(const QString &id, const QString &prefix);

bool writeJson(const QString &path, const QJsonObject &object, QString *error = nullptr);
QJsonObject readJson(const QString &path, bool *ok = nullptr);

// setting id -> {mode, value, at, by}: what Control last saw agreed, through a capture
// or a verified restore. It tells a local edit from a value nobody has touched.
QJsonObject baseline(const Env &env);
bool recordBaseline(const Env &env, const QJsonObject &entries, QString *error = nullptr);

}  // namespace ProfileStore
