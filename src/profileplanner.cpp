#include "profileplanner.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <climits>
#include <memory>

namespace {

QJsonArray strings(const QStringList &list) {
    return QJsonArray::fromStringList(list);
}

QStringList strings(const QJsonValue &value) {
    QStringList out;
    for (const QJsonValue &item : value.toArray())
        out << item.toString();
    return out;
}

bool readOk(const ReadResult &read) {
    return read.status == QLatin1String("ok");
}

}  // namespace

bool ProfilePlanItem::willRun() const {
    return status == QLatin1String("ready") || (status == QLatin1String("conflict") && resolution == QLatin1String("use-saved"));
}

QJsonObject ProfilePlanItem::toJson() const {
    QJsonObject o;
    o["id"] = id;
    o["kind"] = kind;
    o["target"] = target;
    o["label"] = label;
    o["status"] = status;
    if (!category.isEmpty())
        o["category"] = category;
    if (!mode.isEmpty())
        o["mode"] = mode;
    if (mode == QLatin1String("set") || kind == QLatin1String("package"))
        o["desired"] = desired;
    if (!currentStatus.isEmpty()) {
        o["current"] = current;
        o["currentStatus"] = currentStatus;
        o["currentOverride"] = currentOverride;
    }
    if (!reason.isEmpty())
        o["reason"] = reason;
    if (!resolution.isEmpty())
        o["resolution"] = resolution;
    if (!action.isEmpty())
        o["action"] = action;
    if (!transaction.isEmpty())
        o["transaction"] = strings(transaction);
    if (!needs.isEmpty())
        o["needs"] = strings(needs);
    if (!after.isEmpty())
        o["after"] = strings(after);
    if (!activation.isEmpty())
        o["activation"] = strings(activation);
    if (!effects.isEmpty())
        o["effects"] = strings(effects);
    if (!confirmText.isEmpty())
        o["confirmText"] = confirmText;
    if (!recovery.isEmpty())
        o["recovery"] = recovery;
    if (!restart.isEmpty())
        o["restart"] = restart;
    if (needsOwnership)
        o["needsOwnership"] = true;
    o["willRun"] = willRun();
    return o;
}

ProfilePlanItem ProfilePlanItem::fromJson(const QJsonObject &o) {
    ProfilePlanItem item;
    item.id = o.value("id").toString();
    item.kind = o.value("kind").toString();
    item.target = o.value("target").toString();
    item.label = o.value("label").toString();
    item.category = o.value("category").toString();
    item.mode = o.value("mode").toString();
    item.desired = o.value("desired").toString();
    item.current = o.value("current").toString();
    item.currentStatus = o.value("currentStatus").toString();
    item.currentOverride = o.value("currentOverride").toBool();
    item.status = o.value("status").toString();
    item.reason = o.value("reason").toString();
    item.resolution = o.value("resolution").toString();
    item.action = o.value("action").toString();
    item.transaction = strings(o.value("transaction"));
    item.needs = strings(o.value("needs"));
    item.after = strings(o.value("after"));
    item.activation = strings(o.value("activation"));
    item.effects = strings(o.value("effects"));
    item.confirmText = o.value("confirmText").toString();
    item.recovery = o.value("recovery").toString();
    item.restart = o.value("restart").toString();
    item.needsOwnership = o.value("needsOwnership").toBool();
    return item;
}

int ProfilePlan::count(const QString &status) const {
    int n = 0;
    for (const ProfilePlanItem &item : items)
        if (item.status == status)
            ++n;
    return n;
}

int ProfilePlan::leftOut() const {
    int n = 0;
    for (const ProfilePlanItem &item : items)
        if (item.status != QLatin1String("unchanged") && !item.willRun())
            ++n;
    return n;
}

QJsonObject ProfilePlan::toJson() const {
    QJsonObject o;
    o["id"] = id;
    o["createdAt"] = createdAt;
    o["directory"] = directory;
    o["valid"] = valid;
    if (!errors.isEmpty())
        o["errors"] = strings(errors);
    if (valid) {
        o["profileName"] = profileName;
        o["profileDigest"] = profileDigest;
    }
    o["destination"] = destination;
    QJsonArray rows;
    for (const ProfilePlanItem &item : items)
        rows.append(item.toJson());
    o["items"] = rows;
    QJsonObject summary;
    for (const char *status : {"ready", "unchanged", "conflict", "blocked", "manual"})
        summary[QString::fromLatin1(status)] = count(QString::fromLatin1(status));
    summary["leftOut"] = leftOut();
    o["summary"] = summary;
    return o;
}

ProfilePlan ProfilePlan::fromJson(const QJsonObject &o) {
    ProfilePlan plan;
    plan.id = o.value("id").toString();
    plan.createdAt = o.value("createdAt").toString();
    plan.directory = o.value("directory").toString();
    plan.valid = o.value("valid").toBool();
    plan.errors = strings(o.value("errors"));
    plan.profileName = o.value("profileName").toString();
    plan.profileDigest = o.value("profileDigest").toString();
    plan.destination = o.value("destination").toObject();
    for (const QJsonValue &value : o.value("items").toArray())
        plan.items << ProfilePlanItem::fromJson(value.toObject());
    return plan;
}

ProfilePlanner::ProfilePlanner(const Catalog *catalog, SettingsEngine *engine, Capabilities *capabilities, PackageProvider *packages,
                               const Env &env, QObject *parent)
    : QObject(parent), m_catalog(catalog), m_engine(engine), m_capabilities(capabilities), m_packages(packages), m_env(env) {}

QString ProfilePlanner::packageState(const PackageProvider::Inventory &inventory, const QString &name) {
    if (!inventory.isInstalled(name))
        return QStringLiteral("missing");
    return inventory.installed.value(name).explicitly ? QStringLiteral("explicit") : QStringLiteral("dependency");
}

bool ProfilePlanner::packageSatisfied(const ProfilePlanItem &item, const QString &state) {
    return item.mode == QLatin1String("present") ? state == QLatin1String("explicit") : state == QLatin1String("missing");
}

bool ProfilePlanner::satisfied(const CatalogEntry &entry, const ProfilePlanItem &item, const ReadResult &read) {
    if (!readOk(read))
        return false;
    if (item.mode == QLatin1String("inherit"))
        return !read.isOverride;
    return SettingsEngine::valuesMatch(entry, read.value, item.desired);
}

bool ProfilePlanner::unchangedSincePlan(const CatalogEntry &entry, const ProfilePlanItem &item, const ReadResult &read) {
    if (!readOk(read) || !SettingsEngine::valuesMatch(entry, read.value, item.current))
        return false;
    // Returning to the default removes an override, so whether there is one matters too.
    return item.mode != QLatin1String("inherit") || read.isOverride == item.currentOverride;
}

// "default" when this computer still has what a fresh installation has, "local" when it
// has something else, and "unknown" when nothing can tell.
QString ProfilePlanner::provenance(const CatalogEntry &entry, const ReadResult &read) const {
    const ProfileTraits traits = ProfileTraits::of(entry);
    if (traits.hasStock)
        return SettingsEngine::valuesMatch(entry, read.value, traits.stock) ? QStringLiteral("default") : QStringLiteral("local");
    const QString kind = entry.read.value("kind").toString();
    if (kind == QLatin1String("hypr"))
        return m_engine->hyprOrigin(entry) == QLatin1String("omarchy") ? QStringLiteral("default") : QStringLiteral("local");
    if (kind == QLatin1String("json") || kind == QLatin1String("toml"))
        return read.isOverride ? QStringLiteral("local") : QStringLiteral("default");
    return QStringLiteral("unknown");
}

void ProfilePlanner::plan(const QString &directory, const Options &options, Done done) {
    ProfilePlan plan;
    plan.id = ProfileStore::newId(QStringLiteral("plan"));
    plan.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    plan.directory = QFileInfo(directory).absoluteFilePath();
    const QVariantMap versions = m_capabilities->versions();
    plan.destination = QJsonObject{{"omarchy", versions.value("omarchy").toString()}, {"hyprland", versions.value("hyprland").toString()},
                                   {"control", QCoreApplication::applicationVersion()}};
    const ProfileModel::Parsed parsed = ProfileModel::load(directory);
    if (!parsed.ok) {
        plan.errors = parsed.errors;
        done(plan);
        return;
    }
    // A default app the profile asks for, whose app the same profile removes, cannot both
    // happen. The profile is refused rather than half of it guessed at.
    for (auto it = parsed.profile.settings.constBegin(); it != parsed.profile.settings.constEnd(); ++it) {
        const CatalogEntry *entry = m_catalog->find(it.key());
        if (!entry || it.value().mode != QLatin1String("set"))
            continue;
        const QString package = ProfileTraits::packageFor(*entry, it.value().value);
        if (!package.isEmpty() && parsed.profile.package(package).intent == QLatin1String("absent"))
            plan.errors << tr("This profile asks for %1 as %2 and also to remove %3.").arg(m_engine->displayValue(*entry, it.value().value), entry->label, package);
    }
    if (!plan.errors.isEmpty()) {
        done(plan);
        return;
    }
    plan.valid = true;
    plan.profileName = parsed.profile.name;
    plan.profileDigest = parsed.profile.digest();
    planSettings(parsed.profile, options, plan, done);
}

void ProfilePlanner::planPackages(const Profile &profile, const Options &options, std::function<void(const QList<ProfilePlanItem> &)> done) {
    const QList<ProfilePackage> packages = profile.packageList();
    if (packages.isEmpty() || !m_packages) {
        done({});
        return;
    }
    m_packages->readInventory([this, packages, options, done](const PackageProvider::Inventory &inventory) {
        auto items = std::make_shared<QList<ProfilePlanItem>>();
        for (const ProfilePackage &package : packages) {
            ProfilePlanItem item;
            item.id = "package:" + package.name;
            item.kind = QStringLiteral("package");
            item.target = package.name;
            item.label = package.name;
            item.category = QStringLiteral("apps");
            item.mode = package.intent;
            item.desired = package.source;
            if (!inventory.ok) {
                item.status = QStringLiteral("blocked");
                item.reason = tr("Installed apps could not be read: %1").arg(inventory.error);
                *items << item;
                continue;
            }
            item.currentStatus = packageState(inventory, package.name);
            item.current = inventory.installed.value(package.name).version;
            if (packageSatisfied(item, item.currentStatus)) {
                item.status = QStringLiteral("unchanged");
            } else if (package.intent == QLatin1String("present") && item.currentStatus == QLatin1String("dependency")) {
                // Already here only because another package needs it. Marking it as wanted
                // keeps it when that package goes, and changes nothing else.
                item.action = QStringLiteral("mark-explicit");
                item.status = QStringLiteral("ready");
                item.reason = tr("Installed only as another package's dependency; it becomes an app you want.");
            } else if (package.intent == QLatin1String("present")) {
                if (package.source == QLatin1String("manual")) {
                    item.status = QStringLiteral("manual");
                    item.reason = tr("Not from a configured repository, so install it yourself.");
                } else if (package.source == QLatin1String("aur")) {
                    // Nothing about an AUR package is known here without the network, so
                    // what it brings is reviewed in the terminal when it is built.
                    if (!m_packages->hasAurHelper()) {
                        item.status = QStringLiteral("blocked");
                        item.reason = tr("Building from the AUR needs yay, which is not installed.");
                    } else {
                        item.action = QStringLiteral("build");
                        item.status = QStringLiteral("ready");
                        item.reason = tr("Built from the AUR in the terminal, where yay offers its build files for review first.");
                    }
                } else {
                    item.action = QStringLiteral("install");
                }
            } else if (PackageProvider::isProtected(package.name)) {
                item.status = QStringLiteral("blocked");
                item.reason = tr("Part of the system or this desktop's session, so a restore never removes it.");
            } else if (PackageProvider::isSystemPackage(package.name) || inventory.hardware.contains(package.name)) {
                item.status = QStringLiteral("blocked");
                item.reason = tr("Omarchy installs this for particular hardware, so a restore never removes it.");
            } else {
                item.action = QStringLiteral("remove");
            }
            *items << item;
        }

        // pacman says what each install or removal would really do, one package at a time,
        // so one that cannot happen does not hide what the others would do.
        auto next = std::make_shared<std::function<void(int)>>();
        *next = [this, items, inventory, options, done, next](int index) {
            while (index < items->size() && (!(*items)[index].status.isEmpty() || (*items)[index].action.isEmpty()))
                ++index;
            if (index >= items->size()) {
                const QJsonObject baseline = ProfileStore::baseline(m_env);
                const auto chosen = [](const QStringList &list, const QString &name) {
                    return list.contains(name) || list.contains("package:" + name) || list.contains(QStringLiteral("all"));
                };
                for (ProfilePlanItem &item : *items) {
                    if (item.status != QLatin1String("ready") || item.action == QLatin1String("mark-explicit"))
                        continue;
                    // As with settings: what this computer last agreed on tells a choice
                    // made here since from a profile that moved.
                    const QString agreed = baseline.value("package:" + item.target).toObject().value("mode").toString();
                    if (item.action == QLatin1String("install") || item.action == QLatin1String("build")) {
                        if (agreed == QLatin1String("present")) {
                            item.status = QStringLiteral("conflict");
                            item.reason = tr("Removed from this computer since it was last saved or restored.");
                        }
                    } else if (agreed == QLatin1String("absent")) {
                        item.status = QStringLiteral("conflict");
                        item.reason = tr("Installed on this computer since it was last saved or restored.");
                    } else if (agreed.isEmpty() && inventory.hasBaseline && inventory.baseline.contains(item.target)) {
                        item.reason = tr("Removes an app Omarchy installs by default.");
                    } else if (agreed.isEmpty()) {
                        item.status = QStringLiteral("conflict");
                        item.reason = tr("Not one of Omarchy's defaults, so it may be an app you installed here yourself.");
                    }
                    if (item.status == QLatin1String("conflict")) {
                        if (chosen(options.useSaved, item.target))
                            item.resolution = QStringLiteral("use-saved");
                        else if (chosen(options.keepLocal, item.target))
                            item.resolution = QStringLiteral("keep-local");
                    }
                }
                done(*items);
                *next = nullptr;
                return;
            }
            ProfilePlanItem &item = (*items)[index];
            const auto finished = [items, index, next](const PackageProvider::Preview &preview) {
                ProfilePlanItem &planned = (*items)[index];
                if (preview.ok) {
                    planned.status = QStringLiteral("ready");
                    planned.transaction = preview.packages;
                    if (planned.action == QLatin1String("install") && preview.packages.size() > 1)
                        planned.reason = tr("Also installs %n package(s) it needs.", "", int(preview.packages.size() - 1));
                } else {
                    planned.status = QStringLiteral("blocked");
                    planned.reason = !preview.requiredBy.isEmpty() ? tr("Needed by %1, so it stays.").arg(preview.requiredBy.join(", "))
                                     : !preview.missing.isEmpty() ? tr("No configured repository offers it.")
                                                                  : preview.error;
                }
                (*next)(index + 1);
            };
            if (item.action == QLatin1String("install"))
                m_packages->previewInstall({item.target}, finished);
            else
                m_packages->previewRemoval({item.target}, finished);
        };
        (*next)(0);
    });
}

void ProfilePlanner::planSettings(const Profile &profile, const Options &options, ProfilePlan plan, Done done) {
    // Restore order: the catalog's reviewed order, adjusted so each setting comes after
    // the ones it declares. The order settings happen to have in a profile never counts.
    QHash<QString, int> catalogOrder;
    for (int i = 0; i < m_catalog->entries().size(); ++i)
        catalogOrder.insert(m_catalog->entries()[i].id, i);
    QStringList pending = profile.settings.keys();
    std::stable_sort(pending.begin(), pending.end(), [&catalogOrder](const QString &a, const QString &b) {
        return catalogOrder.value(a, INT_MAX) < catalogOrder.value(b, INT_MAX);
    });
    QStringList ordered;
    QSet<QString> placed;
    while (!pending.isEmpty()) {
        bool progressed = false;
        for (int i = 0; i < pending.size(); ++i) {
            const CatalogEntry *entry = m_catalog->find(pending[i]);
            bool ready = true;
            if (entry)
                for (const QString &before : ProfileTraits::of(*entry).after)
                    if (profile.settings.contains(before) && !placed.contains(before))
                        ready = false;
            if (ready) {
                ordered << pending[i];
                placed.insert(pending.takeAt(i));
                progressed = true;
                break;
            }
        }
        // A cycle in reviewed data is a bug; the rest still go in catalog order.
        if (!progressed) {
            ordered += pending;
            break;
        }
    }

    auto items = std::make_shared<QList<ProfilePlanItem>>();
    QStringList reads;    // Settings whose current value is needed.
    QStringList choices;  // Settings whose live list of choices is needed.
    for (const QString &id : ordered) {
        const ProfileSetting saved = profile.settings.value(id);
        ProfilePlanItem item;
        item.id = "setting:" + id;
        item.kind = QStringLiteral("setting");
        item.target = id;
        item.label = id;
        item.mode = saved.mode;
        item.desired = saved.value;
        const CatalogEntry *entry = m_catalog->find(id);
        if (!entry || !entry->isEditable() || id.contains('@')) {
            item.status = QStringLiteral("manual");
            item.reason = tr("This version of Omarchy Control does not know this setting.");
            *items << item;
            continue;
        }
        const ProfileTraits traits = ProfileTraits::of(*entry);
        item.label = entry->label;
        item.category = traits.category;
        item.activation = traits.activation;
        item.effects = entry->effects;
        item.recovery = entry->recovery;
        item.restart = entry->restart;
        for (const QString &before : traits.after)
            if (profile.settings.contains(before))
                item.after << "setting:" + before;
        const QString reason = QCoreApplication::translate("catalog", traits.reason.toUtf8().constData());
        if (!traits.portable()) {
            item.status = QStringLiteral("manual");
            item.reason = traits.portability == QLatin1String("later") ? tr("This version does not restore it yet: %1").arg(reason)
                          : traits.portability.isEmpty()                ? tr("This setting is not saved in profiles.")
                                                                        : tr("This stays with each computer: %1").arg(reason);
            *items << item;
            continue;
        }
        const auto availability = m_capabilities->availability(*entry);
        if (!availability.available) {
            item.status = QStringLiteral("blocked");
            item.reason = availability.reason;
            *items << item;
            continue;
        }
        if (item.mode == QLatin1String("inherit") && !traits.inherit) {
            item.status = QStringLiteral("blocked");
            item.reason = tr("This setting has no default it can reliably return to.");
            *items << item;
            continue;
        }
        reads << id;
        if (item.mode == QLatin1String("set"))
            choices << id;
        for (const ProfileTraits::Overlap &overlap : traits.overlaps)
            if (!profile.settings.contains(overlap.setting) && !reads.contains(overlap.setting))
                reads << overlap.setting;
        *items << item;
    }

    auto readResults = std::make_shared<QHash<QString, ReadResult>>();
    auto choiceResults = std::make_shared<QHash<QString, QVariantList>>();
    // Every value is read once, one after another, and nothing is decided until all of
    // them are in: what overrides one setting can be another one's value.
    auto finish = [this, profile, options, items, readResults, choiceResults, plan, done]() mutable {
        planPackages(profile, options, [this, profile, options, items, readResults, choiceResults, plan, done](const QList<ProfilePlanItem> &packageItems) mutable {
            const QJsonObject baseline = ProfileStore::baseline(m_env);
            const auto wanted = [](const QStringList &list, const QString &id) {
                return list.contains(id) || list.contains(QStringLiteral("all"));
            };
            const auto installing = [&packageItems](const QString &name) -> const ProfilePlanItem * {
                for (const ProfilePlanItem &package : packageItems)
                    if (package.target == name && (package.action == QLatin1String("install") || package.action == QLatin1String("build")) && package.willRun())
                        return &package;
                return nullptr;
            };
            for (ProfilePlanItem &item : *items) {
                if (!item.status.isEmpty())
                    continue;
                const CatalogEntry *entry = m_catalog->find(item.target);
                const ProfileTraits traits = ProfileTraits::of(*entry);
                const ReadResult read = readResults->value(item.target);
                item.currentStatus = read.status;
                item.current = read.value;
                item.currentOverride = read.isOverride;
                if (read.status == QLatin1String("custom")) {
                    item.status = QStringLiteral("blocked");
                    item.reason = read.reason;
                    continue;
                }
                if (!readOk(read)) {
                    item.status = QStringLiteral("blocked");
                    item.reason = tr("Its current value could not be read: %1").arg(read.reason.isEmpty() ? tr("no value was found.") : read.reason);
                    continue;
                }
                if (satisfied(*entry, item, read)) {
                    item.status = QStringLiteral("unchanged");
                    continue;
                }
                if (item.mode == QLatin1String("set")) {
                    // Two writers for one effect: restoring this would be masked, and its
                    // check would fail, so it is held back and the reason shown.
                    QString overridden;
                    for (const ProfileTraits::Overlap &overlap : traits.overlaps) {
                        const CatalogEntry *other = m_catalog->find(overlap.setting);
                        if (!other)
                            continue;
                        const ProfileSetting saved = profile.settings.value(overlap.setting);
                        QString value;
                        if (saved.mode == QLatin1String("set"))
                            value = saved.value;
                        else if (saved.mode.isEmpty() && readOk(readResults->value(overlap.setting)))
                            value = readResults->value(overlap.setting).value;
                        if (!value.isEmpty() && SettingsEngine::valuesMatch(*other, value, overlap.when))
                            overridden = tr("%1 is set to %2, which overrides this.").arg(other->label, m_engine->displayValue(*other, overlap.when));
                    }
                    if (!overridden.isEmpty()) {
                        item.status = QStringLiteral("blocked");
                        item.reason = overridden;
                        continue;
                    }
                    // A default app whose app this restore installs first: it is checked again,
                    // with the app in place, when its turn comes.
                    if (const ProfilePlanItem *provider = installing(ProfileTraits::packageFor(*entry, item.desired))) {
                        item.needs << provider->id;
                        item.after << provider->id;
                    } else {
                        const ChangePlan check = m_engine->plan(item.target, item.desired, choiceResults->value(item.target));
                        if (!check.ok) {
                            item.status = QStringLiteral("blocked");
                            item.reason = check.error;
                            continue;
                        }
                        item.confirmText = check.confirmText;
                        item.needsOwnership = check.needsOwnership;
                    }
                } else {
                    // Returning to the default is a reset, which plan() does not describe.
                    item.confirmText.clear();
                }
                const QJsonObject agreed = baseline.value(item.target).toObject();
                if (!agreed.isEmpty()) {
                    // Compared with the last value this computer agreed on: if it still has
                    // that, only the saved profile moved, and restoring is safe. If not,
                    // someone changed it here since, and that is not ours to overwrite.
                    const bool unchangedHere = agreed.value("mode").toString() == QLatin1String("inherit")
                                                   ? !read.isOverride
                                                   : SettingsEngine::valuesMatch(*entry, read.value, agreed.value("value").toString());
                    item.status = unchangedHere ? QStringLiteral("ready") : QStringLiteral("conflict");
                    if (!unchangedHere)
                        item.reason = tr("Changed on this computer since it was last saved or restored.");
                } else {
                    const QString origin = provenance(*entry, read);
                    item.status = origin == QLatin1String("default") ? QStringLiteral("ready") : QStringLiteral("conflict");
                    item.reason = origin == QLatin1String("default") ? tr("Replaces this computer's default.")
                                  : origin == QLatin1String("local") ? tr("This computer has its own value here, which may be your choice.")
                                                                     : tr("Nothing shows whether this computer's value is a default or your choice.");
                }
                if (item.status == QLatin1String("conflict")) {
                    if (wanted(options.useSaved, item.target))
                        item.resolution = QStringLiteral("use-saved");
                    else if (wanted(options.keepLocal, item.target))
                        item.resolution = QStringLiteral("keep-local");
                }
                if (!item.needs.isEmpty() && item.status == QLatin1String("ready"))
                    item.reason = tr("Once %1 is installed.").arg(item.needs.first().mid(8));
            }
            // Apps arrive before the settings that use them, and go only after the settings
            // that replaced them have been verified.
            QList<ProfilePlanItem> ordered;
            for (const ProfilePlanItem &package : packageItems)
                if (package.mode == QLatin1String("present"))
                    ordered << package;
            ordered += *items;
            for (const ProfilePlanItem &package : packageItems)
                if (package.mode == QLatin1String("absent"))
                    ordered << package;
            plan.items = ordered;
            // Sections a later version restores stay in view, never quietly dropped.
            const auto section = [&plan](const QString &target, const QString &label, int count, const QString &reason) {
                if (count == 0)
                    return;
                ProfilePlanItem item;
                item.id = target;
                item.kind = QStringLiteral("section");
                item.target = target;
                item.label = label;
                item.status = QStringLiteral("manual");
                item.reason = reason.arg(count);
                plan.items << item;
            };
            section(QStringLiteral("desktop"), tr("Desktop"), profile.desktop.size(), tr("%1 saved desktop choices, such as the dock. This version does not restore them yet."));
            section(QStringLiteral("files"), tr("Configuration files"), profile.files.size(), tr("%1 saved configuration files. This version does not restore them yet."));
            section(QStringLiteral("assets"), tr("Backgrounds and icons"), profile.assets.size(), tr("%1 saved files such as backgrounds. This version does not restore them yet."));
            done(plan);
        });
    };

    auto next = std::make_shared<std::function<void(int)>>();
    *next = [this, reads, choices, readResults, choiceResults, finish, next](int index) mutable {
        if (index >= reads.size() * 2) {
            finish();
            *next = nullptr;
            return;
        }
        const QString id = reads[index / 2];
        if (index % 2 == 0) {
            m_engine->read(id, [readResults, id, next, index](const ReadResult &read) {
                readResults->insert(id, read);
                (*next)(index + 1);
            });
        } else if (choices.contains(id)) {
            m_engine->readChoices(id, [choiceResults, id, next, index](const QVariantList &list) {
                choiceResults->insert(id, list);
                (*next)(index + 1);
            });
        } else {
            (*next)(index + 1);
        }
    };
    (*next)(0);
}

bool ProfilePlanner::save(const ProfilePlan &plan, QString *error) const {
    return ProfileStore::writeJson(ProfileStore::plansDir(m_env) + "/" + plan.id + ".json", plan.toJson(), error);
}

ProfilePlan ProfilePlanner::load(const QString &id, QString *error) const {
    if (!ProfileStore::isId(id, QStringLiteral("plan"))) {
        if (error)
            *error = tr("%1 is not a plan id.").arg(id);
        return {};
    }
    bool ok = false;
    const QJsonObject object = ProfileStore::readJson(ProfileStore::plansDir(m_env) + "/" + id + ".json", &ok);
    if (!ok) {
        if (error)
            *error = tr("There is no plan %1 on this computer.").arg(id);
        return {};
    }
    return ProfilePlan::fromJson(object);
}
