#include "profilecapture.h"
#include "fileops.h"
#include "profilefiles.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
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
           && excludeApps.isEmpty() && forgetApps.isEmpty() && !suggestedApps && !resolveAur && !dock && !launchers && !associations && !shortcuts
           && !windowRules && !background && !themes && !bar && !plugins && configFiles.isEmpty();
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
            capturePackages(result, selection, [this, result, saved, hadProfile, source, selection, done] {
                captureDesktop(result, selection, [this, result, saved, hadProfile, source, done] {
                    // Shown in the catalog's order, whatever order they were chosen in, apps last.
                    QHash<QString, int> order;
                    for (int i = 0; i < m_catalog->entries().size(); ++i)
                        order.insert(m_catalog->entries()[i].id, i);
                    std::stable_sort(result->items.begin(), result->items.end(), [&order](const Item &a, const Item &b) {
                        return order.value(a.id, INT_MAX) < order.value(b.id, INT_MAX);
                    });
                    result->changed = !hadProfile || !result->profile.sameChoices(*saved);
                    result->profile.source = result->changed ? source : saved->source;
                    if (!hadProfile && result->profile.settings.isEmpty() && result->profile.packages.isEmpty() && result->profile.desktop.isEmpty()
                        && result->profile.files.isEmpty())
                        result->error = tr("Nothing could be remembered, so no profile was written.");
                    else
                        result->ok = true;
                    done(*result);
                });
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

void ProfileCapture::captureDesktop(std::shared_ptr<Result> result, const Selection &selection, std::function<void()> finished) {
    const QJsonObject saved = result->profile.desktop;
    const bool wanted = !saved.isEmpty() || selection.dock || selection.launchers || selection.associations || selection.shortcuts || selection.windowRules
                        || selection.background || selection.bar || selection.plugins;
    if (!wanted || !m_desktop) {
        captureThemes(result, selection);
        captureFiles(result, selection);
        finished();
        return;
    }
    m_desktop->read(saved.value("associations").toObject().keys(), [this, result, selection, finished](const ProfileDesktop::Snapshot &snapshot) {
        QJsonObject desktop = result->profile.desktop;
        const auto show = [&](const QString &target, const QString &before, const QString &after, const QString &change, const QString &reason,
                              bool attention = false) {
            Item item;
            item.kind = QStringLiteral("desktop");
            item.id = "desktop:" + target;
            item.label = ProfileDesktop::label(target, after.isEmpty() ? before : after);
            item.category = QStringLiteral("desktop");
            item.change = change;
            item.before = ProfileSetting{before.isEmpty() ? QString() : QStringLiteral("set"), ProfileDesktop::summary(target, before)};
            item.after = ProfileSetting{after.isEmpty() ? QString() : QStringLiteral("set"), ProfileDesktop::summary(target, after)};
            item.reason = reason;
            item.attention = attention;
            result->items << item;
        };
        const auto changeOf = [](const QString &before, const QString &after) {
            return before.isEmpty() ? QStringLiteral("added") : before == after ? QStringLiteral("unchanged") : QStringLiteral("modified");
        };
        const auto agree = [&](const QString &target) {
            result->agreed["desktop:" + target] = QJsonObject{{"value", ProfileDesktop::current(snapshot, target)}};
        };
        // A file the profile is to carry, recorded by its digest and copied in when saved.
        const auto carry = [&](const QString &path, const QString &sha256, const QString &kind) {
            QString digest;
            qint64 size = 0;
            if (!ProfileFiles::describe(path, &digest, &size) || digest != sha256)
                return false;
            result->profile.setAsset(QJsonObject{{"sha256", sha256}, {"size", double(size)}, {"kind", kind}, {"name", QFileInfo(path).fileName()}});
            result->assetSources.insert(sha256, path);
            return true;
        };
        // A launcher's icon that is a file travels as one of the profile's files.
        const auto portableEdit = [&](const QString &id, QJsonObject edit) -> QJsonObject {
            if (!QDir::isAbsolutePath(edit.value("icon").toString()))
                return edit;
            const QString sha256 = snapshot.launcherIconDigests.value(id);
            if (sha256.isEmpty() || !carry(snapshot.launcherIconFiles.value(id), sha256, QStringLiteral("icon")))
                return {};
            edit["icon"] = "sha256:" + sha256;
            return edit;
        };

        // The background, for the theme it belongs to.
        if (selection.background || desktop.contains("background")) {
            const QString before = desktop.contains("background") ? ProfileDesktop::desired(desktop, "background") : QString();
            QJsonObject record = snapshot.background;
            if (record.contains("sha256") && !carry(snapshot.backgroundPath, record.value("sha256").toString(), QStringLiteral("background")))
                record = QJsonObject();
            if (record.isEmpty()) {
                show("background", before, before, before.isEmpty() ? QStringLiteral("excluded") : QStringLiteral("kept"),
                     tr("This background is neither one of its theme's nor in your own backgrounds folder for it, so a profile does not carry it."),
                     !before.isEmpty());
            } else {
                desktop["background"] = record;
                const QString after = ProfileDesktop::desired(desktop, "background");
                show("background", before, after, changeOf(before, after), QString());
                agree("background");
            }
        }

        // The bar: Omarchy's own widgets, in order, with the options a profile carries. What
        // does not travel is listed: widgets from plugins installed separately, and options
        // that run something or name a path on this computer.
        if (selection.bar || desktop.contains("bar")) {
            const QString before = desktop.contains("bar") ? ProfileDesktop::desired(desktop, "bar") : QString();
            if (!snapshot.barOk) {
                show("bar", before, before, before.isEmpty() ? QStringLiteral("excluded") : QStringLiteral("kept"),
                     tr("The bar's layout could not be read, so nothing changed: %1").arg(snapshot.barError), !before.isEmpty());
            } else {
                QStringList separate, notes;
                QJsonObject layout;
                for (const QString &section : ProfileModel::barSections()) {
                    QJsonArray widgets;
                    for (const QJsonValue &value : snapshot.barLayout.value(section).toArray()) {
                        const QString id = value.isString() ? value.toString() : value.toObject().value("id").toString();
                        if (!ProfileModel::isOmarchyPluginId(id) || !snapshot.barWidgets.contains(id)) {
                            if (!id.isEmpty() && !separate.contains(id))
                                separate << id;
                            continue;
                        }
                        QJsonObject entry{{"id", id}};
                        const QJsonObject options = value.toObject();
                        for (auto it = options.begin(); it != options.end(); ++it) {
                            if (it.key() == QLatin1String("id"))
                                continue;
                            const QString problem = ProfileModel::barOptionProblem(it.key(), it.value());
                            if (problem.isEmpty())
                                entry.insert(it.key(), it.value());
                            else
                                notes << tr("%1: %2").arg(id.mid(8), problem);
                        }
                        widgets.append(entry);
                    }
                    layout.insert(section, widgets);
                }
                if (!separate.isEmpty())
                    notes.prepend(tr("Left out, since a profile does not install plugins: %1.").arg(separate.join(", ")));
                desktop["bar"] = QJsonObject{{"layout", layout}};
                const QString after = ProfileDesktop::desired(desktop, "bar");
                show("bar", before, after, changeOf(before, after), notes.join(" "));
                agree("bar");
            }
        }

        // Omarchy's own plugins someone switched off, as the running shell reports them.
        if (selection.plugins || desktop.contains("plugins")) {
            const QString before = desktop.contains("plugins") ? ProfileDesktop::desired(desktop, "plugins") : QString();
            if (!snapshot.pluginsOk) {
                show("plugins", before, before, before.isEmpty() ? QStringLiteral("excluded") : QStringLiteral("kept"),
                     tr("Omarchy's plugins could not be read, so nothing changed: %1").arg(snapshot.pluginsError), !before.isEmpty());
            } else {
                desktop["plugins"] = QJsonObject{{"disabled", QJsonArray::fromStringList(snapshot.disabledPlugins)}};
                const QString after = ProfileDesktop::desired(desktop, "plugins");
                show("plugins", before, after, changeOf(before, after), QString());
                agree("plugins");
            }
        }

        // The dock, as one ordered list.
        if (selection.dock || desktop.contains("dockPins")) {
            const QString before = desktop.contains("dockPins") ? ProfileDesktop::desired(desktop, "dock") : QString();
            if (!snapshot.dockOk) {
                show("dock", before, before, before.isEmpty() ? QStringLiteral("excluded") : QStringLiteral("kept"),
                     tr("The dock's pins could not be read, so nothing changed: %1").arg(snapshot.dockError), !before.isEmpty());
            } else {
                QJsonArray pins;
                for (const QString &pin : snapshot.dockPins)
                    pins.append(pin);
                desktop["dockPins"] = pins;
                const QString after = ProfileDesktop::desired(desktop, "dock");
                show("dock", before, after, changeOf(before, after), QString());
                agree("dock");
            }
        }

        // Launchers: what is remembered is read again, and the ones changed here are added
        // when chosen. A change undone here leaves the profile too; an app missing here
        // keeps its saved change.
        QJsonObject launchers = desktop.value("launchers").toObject();
        const QStringList remembered = launchers.keys();
        for (const QString &id : remembered) {
            const QString target = "launcher:" + id;
            const QString before = ProfileDesktop::desired(desktop, target);
            if (!snapshot.launchers.contains(id)) {
                show(target, before, before, QStringLiteral("kept"), tr("Its app is not installed here, so the saved change stays."), true);
            } else if (snapshot.launcherEdits.contains(id) && portableEdit(id, snapshot.launcherEdits.value(id)).isEmpty()) {
                show(target, before, before, QStringLiteral("kept"), tr("Its icon file could not be read, so the saved change stays."), true);
            } else if (snapshot.launcherEdits.contains(id)) {
                launchers[id] = portableEdit(id, snapshot.launcherEdits.value(id));
                QJsonObject probe;
                probe["launchers"] = launchers;
                show(target, before, ProfileDesktop::desired(probe, target), changeOf(before, ProfileDesktop::desired(probe, target)), QString());
                agree(target);
            } else {
                launchers.remove(id);
                show(target, before, QString(), QStringLiteral("removed"), tr("No longer changed on this computer."));
            }
        }
        if (selection.launchers) {
            for (auto it = snapshot.launcherEdits.constBegin(); it != snapshot.launcherEdits.constEnd(); ++it) {
                if (remembered.contains(it.key()))
                    continue;
                const QString target = "launcher:" + it.key();
                const QJsonObject edit = portableEdit(it.key(), it.value());
                if (edit.isEmpty()) {
                    show(target, QString(), QString(), QStringLiteral("excluded"), tr("Its icon file could not be read."));
                    continue;
                }
                launchers[it.key()] = edit;
                show(target, QString(), QString::fromUtf8(QJsonDocument(edit).toJson(QJsonDocument::Compact)), QStringLiteral("added"), QString());
                agree(target);
            }
        }
        if (launchers.isEmpty())
            desktop.remove("launchers");
        else
            desktop["launchers"] = launchers;

        // File associations: the everyday types, by the app that opens each now.
        QJsonObject associations = desktop.value("associations").toObject();
        for (const QString &mime : associations.keys()) {
            const QString target = "association:" + mime;
            const QString before = associations.value(mime).toString();
            const QString now = snapshot.associations.value(mime);
            if (now.isEmpty()) {
                show(target, before, before, QStringLiteral("kept"), tr("Which app opens this could not be read, so the saved choice stays."), true);
                continue;
            }
            associations[mime] = now;
            show(target, before, now, changeOf(before, now), QString());
            agree(target);
        }
        if (selection.associations) {
            for (const QString &mime : ProfileDesktop::commonMimes()) {
                const QString now = snapshot.associations.value(mime);
                if (associations.contains(mime) || now.isEmpty() || !now.endsWith(QLatin1String(".desktop")))
                    continue;
                associations[mime] = now;
                show("association:" + mime, QString(), now, QStringLiteral("added"), QString());
                agree("association:" + mime);
            }
        }
        if (associations.isEmpty())
            desktop.remove("associations");
        else
            desktop["associations"] = associations;

        // Shortcuts and window rules this app owns, merged by their record ids.
        const auto ownedRecords = [&](const QString &section, const QString &kind, const QHash<QString, QJsonObject> &here, bool chosen) {
            QJsonArray kept;
            QSet<QString> seen;
            for (const QJsonValue &value : desktop.value(section).toArray()) {
                const QJsonObject record = value.toObject();
                const QString id = record.value("id").toString();
                const QString target = kind + ":" + id;
                QJsonObject probe;
                probe[section] = QJsonArray{record};
                const QString before = ProfileDesktop::desired(probe, target);
                seen.insert(id);
                if (snapshot.moduleEditedByHand) {
                    kept.append(record);
                    show(target, before, before, QStringLiteral("kept"), tr("omarchy-control.lua was edited by hand, so the saved one stays."), true);
                } else if (here.contains(id)) {
                    QJsonObject now = here.value(id);
                    now["id"] = id;
                    kept.append(now);
                    probe[section] = QJsonArray{now};
                    show(target, before, ProfileDesktop::desired(probe, target), changeOf(before, ProfileDesktop::desired(probe, target)), QString());
                    agree(target);
                } else {
                    show(target, before, QString(), QStringLiteral("removed"), tr("No longer on this computer."));
                }
            }
            if (chosen && snapshot.moduleEditedByHand) {
                show(kind, QString(), QString(), QStringLiteral("excluded"), tr("omarchy-control.lua was edited by hand, so nothing in it was read."));
            } else if (chosen) {
                for (auto it = here.constBegin(); it != here.constEnd(); ++it) {
                    if (seen.contains(it.key()) || it.key().isEmpty())
                        continue;
                    QJsonObject record = it.value();
                    record["id"] = it.key();
                    kept.append(record);
                    QJsonObject probe;
                    probe[section] = QJsonArray{record};
                    show(kind + ":" + it.key(), QString(), ProfileDesktop::desired(probe, kind + ":" + it.key()), QStringLiteral("added"), QString());
                    agree(kind + ":" + it.key());
                }
            }
            if (kept.isEmpty())
                desktop.remove(section);
            else
                desktop[section] = kept;
        };
        ownedRecords(QStringLiteral("shortcuts"), QStringLiteral("shortcut"), snapshot.shortcuts, selection.shortcuts);
        ownedRecords(QStringLiteral("windowRules"), QStringLiteral("rule"), snapshot.rules, selection.windowRules);
        result->profile.desktop = desktop;
        result->profile.dropUnreferencedAssets();
        captureThemes(result, selection);
        captureFiles(result, selection);
        finished();
    });
}

void ProfileCapture::captureThemes(std::shared_ptr<Result> result, const Selection &selection) {
    Profile &profile = result->profile;
    const QJsonObject saved = profile.desktop.value("themes").toObject();
    const ProfileSetting chosen = profile.settings.value(QStringLiteral("appearance.theme"));
    const QString selected = chosen.mode == QLatin1String("set") ? ProfileDesktop::themeFolder(chosen.value) : QString();
    if (saved.isEmpty() && !selection.themes && selected.isEmpty())
        return;
    const QHash<QString, ProfileDesktop::ThemeState> here = ProfileDesktop::readThemes(m_env);
    const auto show = [&](const QString &folder, const QString &before, const QString &after, const QString &change, const QString &reason,
                          bool attention = false) {
        const QString target = "theme:" + folder;
        const auto value = [](const QString &url) { return url.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(QJsonObject{{"url", url}}).toJson(QJsonDocument::Compact)); };
        Item item;
        item.kind = QStringLiteral("desktop");
        item.id = "desktop:" + target;
        item.label = ProfileDesktop::label(target, QString());
        item.category = QStringLiteral("desktop");
        item.change = change;
        item.before = ProfileSetting{before.isEmpty() ? QString() : QStringLiteral("set"), before.isEmpty() ? QString() : ProfileDesktop::summary(target, value(before))};
        item.after = ProfileSetting{after.isEmpty() ? QString() : QStringLiteral("set"), after.isEmpty() ? QString() : ProfileDesktop::summary(target, value(after))};
        item.reason = reason;
        item.attention = attention;
        result->items << item;
    };
    QJsonObject themes;
    // What the profile remembers is read again. A theme removed here leaves the profile too,
    // as a shortcut removed here does; one that is no longer a Git copy keeps its address.
    for (auto it = saved.begin(); it != saved.end(); ++it) {
        const QString before = it.value().toObject().value("url").toString();
        const ProfileDesktop::ThemeState state = here.value(it.key());
        if (!state.installed) {
            show(it.key(), before, QString(), QStringLiteral("removed"), tr("No longer installed on this computer."));
        } else if (state.url.isEmpty()) {
            themes[it.key()] = it.value();
            show(it.key(), before, before, QStringLiteral("kept"), tr("%1 The saved address stays.").arg(state.problem), true);
        } else {
            const QString after = ProfileDesktop::sameSource(before, state.url) ? before : state.url;
            themes[it.key()] = QJsonObject{{"url", after}};
            show(it.key(), before, after, before == after ? QStringLiteral("unchanged") : QStringLiteral("modified"), QString());
        }
    }
    QStringList wanted;
    if (selection.themes) {
        for (auto it = here.begin(); it != here.end(); ++it)
            if (it.value().installed)
                wanted << it.key();
        wanted.sort();
    }
    if (!selected.isEmpty() && here.value(selected).installed && !wanted.contains(selected))
        wanted << selected;
    for (const QString &folder : wanted) {
        if (saved.contains(folder))
            continue;
        const ProfileDesktop::ThemeState state = here.value(folder);
        if (state.url.isEmpty()) {
            show(folder, QString(), QString(), QStringLiteral("excluded"), state.problem);
            continue;
        }
        themes[folder] = QJsonObject{{"url", state.url}};
        show(folder, QString(), state.url, QStringLiteral("added"),
             folder == selected && !selection.themes ? tr("The theme this profile selects, so a restore can install it.") : QString());
    }
    if (themes.isEmpty())
        profile.desktop.remove("themes");
    else
        profile.desktop["themes"] = themes;
}

void ProfileCapture::captureFiles(std::shared_ptr<Result> result, const Selection &selection) {
    Profile &profile = result->profile;
    if (profile.files.isEmpty() && selection.configFiles.isEmpty())
        return;
    const auto show = [&](const QString &adapter, const QString &name, const QString &before, const QString &after, const QString &change,
                          const QString &reason, bool attention = false) {
        Item item;
        item.kind = QStringLiteral("desktop");
        item.id = "desktop:file:" + adapter + "/" + name;
        item.label = ProfileDesktop::label("file:" + adapter + "/" + name, QString());
        item.category = QStringLiteral("files");
        item.change = change;
        item.before = ProfileSetting{before.isEmpty() ? QString() : QStringLiteral("set"), before.left(12)};
        item.after = ProfileSetting{after.isEmpty() ? QString() : QStringLiteral("set"), after.left(12)};
        item.reason = reason;
        item.attention = attention;
        result->items << item;
    };
    QJsonArray files;
    QStringList wanted = selection.configFiles;
    for (const QJsonValue &value : profile.files) {
        const QString adapter = value.toObject().value("adapter").toString();
        if (!wanted.contains(adapter))
            wanted << adapter;
    }
    for (const QString &id : wanted) {
        ProfileFiles::ConfigAdapter adapter;
        QJsonObject saved;
        for (const QJsonValue &value : profile.files)
            if (value.toObject().value("adapter").toString() == id)
                saved = value.toObject();
        if (!ProfileFiles::configAdapter(m_env, id, &adapter)) {
            if (!saved.isEmpty())
                files.append(saved);
            show(id, id, saved.value("sha256").toString(), saved.value("sha256").toString(), saved.isEmpty() ? QStringLiteral("excluded") : QStringLiteral("kept"),
                 tr("Omarchy Control has no adapter for this file."), !saved.isEmpty());
            continue;
        }
        const QString before = saved.value("sha256").toString();
        // Kept as saved when it cannot be carried as it is here, never dropped quietly.
        const auto keep = [&](const QString &reason) {
            if (!saved.isEmpty())
                files.append(saved);
            show(id, adapter.name, before, before, saved.isEmpty() ? QStringLiteral("excluded") : QStringLiteral("kept"), reason, !saved.isEmpty());
        };
        QString sha256;
        qint64 size = 0;
        QByteArray contents;
        if (!ProfileFiles::describe(adapter.destination, &sha256, &size) || !FileOps::readFile(adapter.destination, &contents)) {
            keep(tr("There is no %1 here.").arg(adapter.destination.section('/', -2)));
            continue;
        }
        const QString problem = ProfileFiles::reviewConfig(id, contents);
        if (!problem.isEmpty()) {
            keep(problem);
            continue;
        }
        QString defaults;
        qint64 defaultSize = 0;
        if (saved.isEmpty() && ProfileFiles::describe(adapter.omarchyDefault, &defaults, &defaultSize) && defaults == sha256) {
            keep(tr("It is Omarchy's own version, so there is nothing of yours in it to carry."));
            continue;
        }
        files.append(QJsonObject{{"adapter", id}, {"name", adapter.name}, {"sha256", sha256}, {"size", double(size)}});
        result->configContents.insert(id + "/" + adapter.name, contents);
        result->agreed["desktop:file:" + id + "/" + adapter.name] = QJsonObject{{"value", sha256}};
        show(id, adapter.name, before, sha256, before.isEmpty() ? QStringLiteral("added") : before == sha256 ? QStringLiteral("unchanged") : QStringLiteral("modified"),
             QString());
    }
    profile.files = files;
}

bool ProfileCapture::save(const QString &directory, const Result &result, QString *error) {
    if (!result.ok) {
        if (error)
            *error = result.error;
        return false;
    }
    if (result.changed) {
        // The files first, so profile.json never names one that is not there yet.
        for (const QString &sha256 : result.profile.referencedAssets()) {
            if (result.assetSources.contains(sha256)) {
                if (!ProfileFiles::store(directory, result.assetSources.value(sha256), sha256, error))
                    return false;
            } else if (!QFileInfo::exists(ProfileFiles::assetPath(directory, sha256))) {
                if (error)
                    *error = tr("The profile's copy of a file it names is missing, so nothing was written.");
                return false;
            }
        }
        for (const QJsonValue &value : result.profile.files) {
            const QString adapter = value.toObject().value("adapter").toString(), name = value.toObject().value("name").toString();
            if (result.configContents.contains(adapter + "/" + name)) {
                if (!ProfileFiles::storeConfig(directory, adapter, name, result.configContents.value(adapter + "/" + name), error))
                    return false;
            } else if (!QFileInfo::exists(ProfileFiles::configPath(directory, adapter, name))) {
                if (error)
                    *error = tr("The profile's copy of a file it names is missing, so nothing was written.");
                return false;
            }
        }
        if (!ProfileModel::save(directory, result.profile, error))
            return false;
        ProfileFiles::prune(directory, result.profile);
        ProfileFiles::pruneConfigs(directory, result.profile);
    }
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
