#include "profilecapture.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <QSysInfo>
#include <algorithm>
#include <climits>
#include <memory>

namespace {

QJsonObject settingJson(const ProfileSetting &setting) {
    if (setting.mode.isEmpty())
        return {};
    QJsonObject object{{"mode", setting.mode}};
    if (setting.mode == QLatin1String("set"))
        object["value"] = setting.value;
    return object;
}

}  // namespace

bool ProfileCapture::Selection::isEmpty() const {
    return settings.isEmpty() && categories.isEmpty() && !suggested && inherit.isEmpty() && forget.isEmpty() && name.isEmpty() && apps.isEmpty()
           && excludeApps.isEmpty() && forgetApps.isEmpty() && !suggestedApps && !resolveAur;
}

QJsonObject ProfileCapture::Result::toJson() const {
    QJsonObject object;
    object["ok"] = ok;
    if (!error.isEmpty())
        object["error"] = error;
    object["name"] = profile.name;
    object["existed"] = existed;
    object["changed"] = changed;
    QJsonArray rows;
    for (const Item &item : items) {
        QJsonObject row{{"kind", item.kind}, {"id", item.id}, {"label", item.label}, {"change", item.change}};
        if (!item.category.isEmpty())
            row["category"] = item.category;
        if (!item.before.mode.isEmpty())
            row["before"] = settingJson(item.before);
        if (!item.after.mode.isEmpty())
            row["after"] = settingJson(item.after);
        if (!item.packageBefore.name.isEmpty())
            row["before"] = item.packageBefore.toJson();
        if (!item.packageAfter.name.isEmpty())
            row["after"] = item.packageAfter.toJson();
        if (!item.reason.isEmpty())
            row["reason"] = item.reason;
        if (item.attention)
            row["attention"] = true;
        rows.append(row);
    }
    object["items"] = rows;
    return object;
}

ProfileCapture::ProfileCapture(const Catalog *catalog, SettingsEngine *engine, Capabilities *capabilities, PackageProvider *packages,
                               const Env &env, QObject *parent)
    : QObject(parent), m_catalog(catalog), m_engine(engine), m_capabilities(capabilities), m_packages(packages), m_env(env) {}

QString ProfileCapture::capturedValue(const CatalogEntry &entry, const ReadResult &read) const {
    // This app's module holds the choice itself. What Hyprland reports can be masked by a
    // toggle that loads later, as window gaps turned off zero the gap sizes.
    if (entry.write.value("kind").toString() == QLatin1String("hypr") && read.isOverride) {
        const QString held = m_engine->hyprModuleValue(entry);
        if (!held.isEmpty())
            return held;
    }
    return read.value;
}

bool ProfileCapture::differsFromStock(const CatalogEntry &entry, const ReadResult &read) const {
    const ProfileTraits traits = ProfileTraits::of(entry);
    if (traits.hasStock)
        return !SettingsEngine::valuesMatch(entry, read.value, traits.stock);
    const QString kind = entry.read.value("kind").toString();
    if (kind == QLatin1String("hypr"))
        return m_engine->hyprOrigin(entry) != QLatin1String("omarchy");
    if (kind == QLatin1String("json") || kind == QLatin1String("toml"))
        return read.isOverride;
    // Nothing says what a fresh installation has here, so it is never suggested. It can
    // still be chosen by name or by category.
    return false;
}

void ProfileCapture::capture(const Profile *existing, const Selection &selection, Done done) {
    auto result = std::make_shared<Result>();
    result->existed = existing != nullptr;
    auto saved = std::make_shared<Profile>(existing ? *existing : Profile());
    result->profile = *saved;
    if (!existing)
        result->profile.name = QStringLiteral("My Omarchy");
    if (!selection.name.trimmed().isEmpty())
        result->profile.name = selection.name.trimmed();

    for (const QString &category : selection.categories) {
        if (!ProfileTraits::categories().contains(category)) {
            result->error = tr("There is no category called %1. Choose from: %2.").arg(category, ProfileTraits::categories().join(", "));
            done(*result);
            return;
        }
    }

    // What to read, and why: "keep" for what the profile remembers, "add" and "inherit"
    // for what was chosen now, "suggest" for what differs from a fresh installation.
    struct Want {
        QString id, how;
    };
    QList<Want> wants;
    QSet<QString> handled;
    const auto labelOf = [this](const QString &id) {
        const CatalogEntry *entry = m_catalog->find(id);
        return entry ? entry->label : id;
    };
    const auto exclude = [&](const QString &id, const QString &reason) {
        if (handled.contains(id))
            return;
        Item item;
        item.id = id;
        item.label = labelOf(id);
        if (const CatalogEntry *entry = m_catalog->find(id))
            item.category = ProfileTraits::of(*entry).category;
        item.change = QStringLiteral("excluded");
        item.reason = reason;
        result->items << item;
        handled.insert(id);
    };
    const auto eligible = [this](const QString &id, QString *why) -> const CatalogEntry * {
        const CatalogEntry *entry = m_catalog->find(id);
        if (!entry || !entry->isEditable() || id.contains('@')) {
            *why = tr("Omarchy Control has no setting by that name.");
            return nullptr;
        }
        const ProfileTraits traits = ProfileTraits::of(*entry);
        if (!traits.portable()) {
            *why = traits.reason.isEmpty() ? tr("This setting is not saved in profiles.") : QCoreApplication::translate("catalog", traits.reason.toUtf8().constData());
            return nullptr;
        }
        return entry;
    };
    const auto want = [&](const QString &id, const QString &how) {
        for (Want &existingWant : wants) {
            if (existingWant.id == id) {
                // An explicit choice outranks a suggestion or a plain re-read.
                if (how == QLatin1String("inherit") || existingWant.how == QLatin1String("suggest"))
                    existingWant.how = how;
                return;
            }
        }
        wants << Want{id, how};
    };

    for (const QString &id : selection.forget) {
        if (result->profile.settings.contains(id)) {
            Item item;
            item.id = id;
            item.label = labelOf(id);
            item.change = QStringLiteral("removed");
            item.before = result->profile.settings.value(id);
            result->items << item;
            result->profile.settings.remove(id);
            handled.insert(id);
        } else {
            exclude(id, tr("This profile does not remember it."));
        }
    }
    for (auto it = result->profile.settings.constBegin(); it != result->profile.settings.constEnd(); ++it)
        want(it.key(), QStringLiteral("keep"));
    for (const QString &id : selection.inherit) {
        QString why;
        const CatalogEntry *entry = eligible(id, &why);
        if (!entry)
            exclude(id, why);
        else if (!ProfileTraits::of(*entry).inherit)
            exclude(id, tr("This setting has no default it can reliably return to, so it can only be saved as a value."));
        else
            want(id, QStringLiteral("inherit"));
    }
    for (const QString &id : selection.settings) {
        QString why;
        if (handled.contains(id))
            continue;
        if (!eligible(id, &why))
            exclude(id, why);
        else
            want(id, QStringLiteral("add"));
    }
    for (const QString &category : selection.categories) {
        for (const CatalogEntry &entry : m_catalog->entries()) {
            const ProfileTraits traits = ProfileTraits::of(entry);
            if (!entry.isEditable() || traits.category != category || handled.contains(entry.id))
                continue;
            // What a category holds but a profile cannot carry is listed, never dropped quietly.
            if (traits.portable())
                want(entry.id, QStringLiteral("add"));
            else
                exclude(entry.id, traits.reason.isEmpty() ? tr("This setting is not saved in profiles.") : QCoreApplication::translate("catalog", traits.reason.toUtf8().constData()));
        }
    }
    if (selection.suggested) {
        for (const CatalogEntry &entry : m_catalog->entries())
            if (entry.isEditable() && ProfileTraits::of(entry).portable() && !handled.contains(entry.id))
                want(entry.id, QStringLiteral("suggest"));
    }

    const QJsonObject source = [this] {
        QJsonObject object;
        if (!QCoreApplication::applicationVersion().isEmpty())
            object["control"] = QCoreApplication::applicationVersion().left(64);
        const QVariantMap versions = m_capabilities->versions();
        if (!versions.value("omarchy").toString().isEmpty())
            object["omarchy"] = versions.value("omarchy").toString().left(64);
        if (!versions.value("hyprland").toString().isEmpty())
            object["hyprland"] = versions.value("hyprland").toString().left(64);
        object["architecture"] = QSysInfo::currentCpuArchitecture().left(64);
        return object;
    }();

    auto next = std::make_shared<std::function<void(int)>>();
    *next = [this, wants, result, saved, hadProfile = existing != nullptr, source, selection, done, next](int index) {
        if (index >= wants.size()) {
            capturePackages(result, selection, [this, result, saved, hadProfile, source, done] {
                // Shown in the catalog's order, whatever order they were chosen in, apps last.
                QHash<QString, int> order;
                for (int i = 0; i < m_catalog->entries().size(); ++i)
                    order.insert(m_catalog->entries()[i].id, i);
                std::stable_sort(result->items.begin(), result->items.end(), [&order](const Item &a, const Item &b) {
                    return order.value(a.id, INT_MAX) < order.value(b.id, INT_MAX);
                });
                result->changed = !hadProfile || !result->profile.sameChoices(*saved);
                result->profile.source = result->changed ? source : saved->source;
                if (!hadProfile && result->profile.settings.isEmpty() && result->profile.packages.isEmpty())
                    result->error = tr("Nothing could be remembered, so no profile was written.");
                else
                    result->ok = true;
                done(*result);
            });
            *next = nullptr;
            return;
        }
        const Want current = wants[index];
        const CatalogEntry *entry = m_catalog->find(current.id);
        const bool had = result->profile.settings.contains(current.id);
        // A setting from a newer Omarchy Control, or one no longer portable here, is kept
        // exactly as saved: this version cannot read it, which is no reason to forget it.
        if (!entry || !ProfileTraits::of(*entry).portable()) {
            Item item;
            item.id = current.id;
            item.label = entry ? entry->label : current.id;
            item.change = QStringLiteral("kept");
            item.before = item.after = result->profile.settings.value(current.id);
            item.reason = entry ? tr("This setting is not saved in profiles by this version, so it is kept as saved.")
                                : tr("This version of Omarchy Control does not know this setting, so it is kept as saved.");
            result->items << item;
            (*next)(index + 1);
            return;
        }
        m_engine->read(current.id, [this, current, entry, had, result, next, index](const ReadResult &read) {
            Item item;
            item.id = current.id;
            item.label = entry->label;
            item.category = ProfileTraits::of(*entry).category;
            if (had)
                item.before = result->profile.settings.value(current.id);
            if (read.status != QLatin1String("ok")) {
                const QString why = read.reason.isEmpty() ? tr("No current value was found.") : read.reason;
                if (current.how == QLatin1String("suggest")) {
                    (*next)(index + 1);
                    return;
                }
                if (had) {
                    item.change = QStringLiteral("kept");
                    item.after = item.before;
                    item.attention = true;
                    item.reason = tr("Its current value could not be read, so the saved choice stays: %1").arg(why);
                } else {
                    item.change = QStringLiteral("excluded");
                    item.reason = tr("Its current value could not be read: %1").arg(why);
                }
                result->items << item;
                (*next)(index + 1);
                return;
            }
            if (current.how == QLatin1String("suggest") && !differsFromStock(*entry, read)) {
                (*next)(index + 1);
                return;
            }
            ProfileSetting now{QStringLiteral("set"), capturedValue(*entry, read)};
            if (current.how == QLatin1String("inherit") || (current.how == QLatin1String("keep") && item.before.mode == QLatin1String("inherit") && !read.isOverride))
                now = ProfileSetting{QStringLiteral("inherit"), QString()};
            if (!had) {
                item.change = QStringLiteral("added");
            } else if (item.before.mode == now.mode && (now.mode == QLatin1String("inherit") || SettingsEngine::valuesMatch(*entry, item.before.value, now.value))) {
                item.change = QStringLiteral("unchanged");
                now = item.before;  // The saved spelling stays, so nothing changes for nothing.
            } else {
                item.change = QStringLiteral("modified");
            }
            item.after = now;
            result->profile.settings.insert(current.id, now);
            // What this computer has right now, as a restore will read it later.
            QJsonObject agreed{{"mode", now.mode}, {"value", read.value}};
            if (read.isOverride)
                agreed["override"] = true;
            result->agreed[current.id] = agreed;
            result->items << item;
            (*next)(index + 1);
        });
    };
    (*next)(0);
}

void ProfileCapture::capturePackages(std::shared_ptr<Result> result, const Selection &selection, std::function<void()> finished) {
    // A default app the profile sets brings the app it opens, so a restore can install it
    // before choosing it.
    // Where that app comes from is in the catalog, taken from Omarchy's own installers, so
    // an AUR browser is known as one without asking the AUR.
    QHash<QString, QString> neededBy, neededSource;
    for (auto it = result->profile.settings.constBegin(); it != result->profile.settings.constEnd(); ++it) {
        const CatalogEntry *entry = m_catalog->find(it.key());
        if (!entry || it.value().mode != QLatin1String("set"))
            continue;
        QString source;
        const QString package = ProfileTraits::packageFor(*entry, it.value().value, &source);
        if (!package.isEmpty()) {
            neededBy.insert(package, entry->label);
            neededSource.insert(package, source);
        }
    }
    const bool wanted = !result->profile.packages.isEmpty() || !selection.apps.isEmpty() || !selection.excludeApps.isEmpty()
                        || !selection.forgetApps.isEmpty() || selection.suggestedApps || !neededBy.isEmpty();
    if (!wanted || !m_packages) {
        finished();
        return;
    }
    m_packages->readInventory([this, result, selection, neededBy, neededSource, finished](const PackageProvider::Inventory &inventory) {
        Profile &profile = result->profile;
        QSet<QString> handled;
        const auto item = [](const QString &name) {
            Item made;
            made.kind = QStringLiteral("package");
            made.id = "package:" + name;
            made.label = name;
            made.category = QStringLiteral("apps");
            return made;
        };
        const auto agree = [&](const QString &name) {
            if (inventory.ok)
                result->agreed["package:" + name] =
                    QJsonObject{{"mode", inventory.isInstalled(name) ? QStringLiteral("present") : QStringLiteral("absent")},
                                {"explicit", inventory.installed.value(name).explicitly}};
        };
        const auto exclude = [&](const QString &name, const QString &reason) {
            Item excluded = item(name);
            excluded.change = QStringLiteral("excluded");
            excluded.reason = reason;
            result->items << excluded;
            handled.insert(name);
        };
        const auto record = [&](const ProfilePackage &package, const QString &reason) {
            Item changed = item(package.name);
            changed.packageBefore = profile.package(package.name);
            changed.packageAfter = package;
            changed.change = changed.packageBefore.name.isEmpty() ? QStringLiteral("added")
                             : changed.packageBefore == package   ? QStringLiteral("unchanged")
                                                                  : QStringLiteral("modified");
            changed.reason = reason;
            profile.setPackage(package);
            result->items << changed;
            handled.insert(package.name);
            agree(package.name);
        };
        const QString unreadable = tr("Installed apps could not be read: %1").arg(inventory.error);

        for (const QString &name : selection.forgetApps) {
            if (profile.hasPackage(name)) {
                Item removed = item(name);
                removed.change = QStringLiteral("removed");
                removed.packageBefore = profile.package(name);
                profile.removePackage(name);
                result->items << removed;
                handled.insert(name);
            } else {
                exclude(name, tr("This profile does not remember it."));
            }
        }

        // What the profile remembers stays as saved. An app missing here, or one wanted
        // gone that is installed here, is flagged, never forgotten or turned around.
        for (const ProfilePackage &package : profile.packageList()) {
            if (handled.contains(package.name) || selection.apps.contains(package.name) || selection.excludeApps.contains(package.name))
                continue;
            Item kept = item(package.name);
            kept.packageBefore = kept.packageAfter = package;
            handled.insert(package.name);
            if (!inventory.ok) {
                kept.change = QStringLiteral("kept");
                kept.attention = true;
                kept.reason = tr("Installed apps could not be read, so this stays as saved: %1").arg(inventory.error);
            } else if (package.intent == QLatin1String("present") && !inventory.isInstalled(package.name)) {
                kept.change = QStringLiteral("kept");
                kept.attention = true;
                kept.reason = tr("Not installed here now, so it stays as saved. Stop remembering it with --forget-app %1.").arg(package.name);
            } else if (package.intent == QLatin1String("absent") && inventory.isInstalled(package.name)) {
                kept.change = QStringLiteral("kept");
                kept.attention = true;
                kept.reason = tr("Installed here, though the profile keeps it as unwanted.");
            } else {
                kept.change = QStringLiteral("unchanged");
                agree(package.name);
            }
            result->items << kept;
        }

        QStringList wantedApps = selection.apps;
        for (auto it = neededBy.constBegin(); it != neededBy.constEnd(); ++it)
            if (!wantedApps.contains(it.key()) && !profile.hasPackage(it.key()) && inventory.isInstalled(it.key()))
                wantedApps << it.key();
        for (const QString &name : wantedApps) {
            if (handled.contains(name) && !selection.apps.contains(name))
                continue;
            if (!ProfileModel::isPackageName(name)) {
                exclude(name, tr("That is not a package name."));
            } else if (!inventory.ok) {
                exclude(name, unreadable);
            } else if (!inventory.isInstalled(name)) {
                exclude(name, tr("It is not installed here, so there is nothing to remember."));
            } else if (PackageProvider::isSystemPackage(name) || inventory.hardware.contains(name)) {
                exclude(name, tr("Part of this computer's system or hardware, so it is not saved as an app."));
            } else {
                const PackageInfo info = inventory.installed.value(name);
                // A foreign package may be from the AUR, a local build, or gone from its
                // repository. Nothing here can tell which without the network, unless the
                // catalog already knows the app.
                const QString foreignSource = neededSource.value(name) == QLatin1String("aur") ? QStringLiteral("aur") : QStringLiteral("manual");
                const ProfilePackage package{name, info.foreign ? foreignSource : QStringLiteral("repository"), QStringLiteral("present"),
                                             info.repository};
                record(package, neededBy.contains(name) && !selection.apps.contains(name) ? tr("Needed by %1.").arg(neededBy.value(name))
                                : info.foreign ? tr("Not from a configured repository, so a restore lists it for you to install.")
                                               : QString());
            }
        }

        for (const QString &name : selection.excludeApps) {
            if (!ProfileModel::isPackageName(name)) {
                exclude(name, tr("That is not a package name."));
            } else if (PackageProvider::isProtected(name)) {
                exclude(name, tr("A restore never removes this: it is part of the system or this desktop's session."));
            } else if (PackageProvider::isSystemPackage(name) || inventory.hardware.contains(name)) {
                exclude(name, tr("Omarchy installs this for particular hardware, so whether a computer needs it is not a profile's choice."));
            } else if (neededBy.contains(name)) {
                exclude(name, tr("The profile's %1 needs it.").arg(neededBy.value(name)));
            } else {
                const PackageInfo info = inventory.installed.value(name);
                const ProfilePackage package{name, info.foreign ? QStringLiteral("manual") : QStringLiteral("repository"), QStringLiteral("absent"),
                                             info.repository};
                record(package, inventory.isInstalled(name) ? tr("Still installed on this computer.") : QString());
            }
        }

        if (selection.suggestedApps) {
            if (!inventory.ok) {
                exclude(QStringLiteral("apps"), unreadable);
            } else if (!inventory.hasBaseline) {
                exclude(QStringLiteral("apps"), tr("Omarchy's list of default apps could not be read, so no apps were suggested."));
            } else {
                QStringList names = inventory.installed.keys();
                names.sort();
                for (const QString &name : names) {
                    const PackageInfo info = inventory.installed.value(name);
                    if (!info.explicitly || handled.contains(name) || profile.hasPackage(name) || inventory.baseline.contains(name)
                        || inventory.hardware.contains(name) || PackageProvider::isSystemPackage(name))
                        continue;
                    record(ProfilePackage{name, info.foreign ? QStringLiteral("manual") : QStringLiteral("repository"), QStringLiteral("present"),
                                          info.repository},
                           tr("Installed here beyond Omarchy's defaults."));
                }
                // Removing something is never inferred. Omarchy's defaults missing here are
                // shown, and remembered as unwanted only when chosen by name.
                for (const QString &name : inventory.missingDefaults) {
                    if (profile.hasPackage(name) || handled.contains(name) || PackageProvider::isProtected(name))
                        continue;
                    Item suggestion = item(name);
                    suggestion.change = QStringLiteral("suggested");
                    suggestion.reason = tr("Omarchy installs this, but it is not installed here. To remember it as unwanted: --exclude-app %1").arg(name);
                    result->items << suggestion;
                }
            }
        }

        QStringList unresolved;
        for (const ProfilePackage &package : profile.packageList())
            if (package.source == QLatin1String("manual") && package.intent == QLatin1String("present"))
                unresolved << package.name;
        if (!selection.resolveAur || unresolved.isEmpty()) {
            finished();
            return;
        }
        m_packages->lookupAur(unresolved, [this, result, unresolved, finished](const PackageProvider::AurLookup &lookup) {
            if (!lookup.ok) {
                Item failed;
                failed.kind = QStringLiteral("package");
                failed.id = QStringLiteral("package:aur");
                failed.label = QStringLiteral("AUR");
                failed.change = QStringLiteral("excluded");
                failed.reason = tr("The AUR could not be asked, so apps from no configured repository stay as installed-by-hand: %1").arg(lookup.error);
                result->items << failed;
                finished();
                return;
            }
            for (const QString &name : unresolved) {
                if (!lookup.found.contains(name))
                    continue;
                ProfilePackage package = result->profile.package(name);
                const ProfilePackage before = package;
                package.source = QStringLiteral("aur");
                result->profile.setPackage(package);
                bool shown = false;
                for (Item &item : result->items) {
                    if (item.id != "package:" + name)
                        continue;
                    item.packageAfter = package;
                    if (item.change == QLatin1String("unchanged") || item.change == QLatin1String("kept"))
                        item.change = QStringLiteral("modified");
                    item.reason = tr("Found in the AUR, so a restore builds it there.");
                    shown = true;
                }
                if (!shown) {
                    Item item;
                    item.kind = QStringLiteral("package");
                    item.id = "package:" + name;
                    item.label = name;
                    item.category = QStringLiteral("apps");
                    item.change = QStringLiteral("modified");
                    item.packageBefore = before;
                    item.packageAfter = package;
                    item.reason = tr("Found in the AUR, so a restore builds it there.");
                    result->items << item;
                }
            }
            finished();
        });
    });
}

bool ProfileCapture::save(const QString &directory, const Result &result, QString *error) {
    if (!result.ok) {
        if (error)
            *error = result.error;
        return false;
    }
    if (result.changed && !ProfileModel::save(directory, result.profile, error))
        return false;
    QJsonObject entries;
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    for (auto it = result.agreed.begin(); it != result.agreed.end(); ++it) {
        QJsonObject entry = it.value().toObject();
        entry["at"] = now;
        entry["by"] = QStringLiteral("capture");
        entries[it.key()] = entry;
    }
    return ProfileStore::recordBaseline(m_env, entries, error);
}
