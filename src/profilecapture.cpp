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
    return settings.isEmpty() && categories.isEmpty() && !suggested && inherit.isEmpty() && forget.isEmpty() && name.isEmpty();
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
        QJsonObject row{{"id", item.id}, {"label", item.label}, {"change", item.change}};
        if (!item.category.isEmpty())
            row["category"] = item.category;
        if (!item.before.mode.isEmpty())
            row["before"] = settingJson(item.before);
        if (!item.after.mode.isEmpty())
            row["after"] = settingJson(item.after);
        if (!item.reason.isEmpty())
            row["reason"] = item.reason;
        if (item.attention)
            row["attention"] = true;
        rows.append(row);
    }
    object["items"] = rows;
    return object;
}

ProfileCapture::ProfileCapture(const Catalog *catalog, SettingsEngine *engine, Capabilities *capabilities, const Env &env, QObject *parent)
    : QObject(parent), m_catalog(catalog), m_engine(engine), m_capabilities(capabilities), m_env(env) {}

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
    *next = [this, wants, result, saved, hadProfile = existing != nullptr, source, done, next](int index) {
        if (index >= wants.size()) {
            // Shown in the catalog's order, whatever order they were chosen in.
            QHash<QString, int> order;
            for (int i = 0; i < m_catalog->entries().size(); ++i)
                order.insert(m_catalog->entries()[i].id, i);
            std::stable_sort(result->items.begin(), result->items.end(), [&order](const Item &a, const Item &b) {
                return order.value(a.id, INT_MAX) < order.value(b.id, INT_MAX);
            });
            result->changed = !hadProfile || !result->profile.sameChoices(*saved);
            result->profile.source = result->changed ? source : saved->source;
            if (!hadProfile && result->profile.settings.isEmpty()) {
                result->error = tr("Nothing could be remembered, so no profile was written.");
                done(*result);
            } else {
                result->ok = true;
                done(*result);
            }
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
