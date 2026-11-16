#include "shortcutsprovider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

#include "hypromodule.h"
#include "hyprchange.h"

ShortcutsProvider::ShortcutsProvider(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *parent)
    : QObject(parent), m_runner(runner), m_coordinator(coordinator) {}

QString ShortcutsProvider::modsToText(int mask) {
    QStringList mods;
    if (mask & 64) mods << "SUPER";
    if (mask & 4) mods << "CTRL";
    if (mask & 8) mods << "ALT";
    if (mask & 1) mods << "SHIFT";
    if (mask & 2) mods << "CAPS";
    if (mask & 16) mods << "MOD2";
    if (mask & 32) mods << "MOD3";
    if (mask & 128) mods << "MOD5";
    return mods.join(" + ");
}

QVariantList ShortcutsProvider::parseBinds(const QByteArray &json) {
    QVariantList rows;
    for (const auto &value : QJsonDocument::fromJson(json).array()) {
        const auto bind = value.toObject();
        const QString key = bind.value("key").toString();
        if (key.isEmpty() || bind.value("catch_all").toBool())
            continue;
        const QString mods = modsToText(bind.value("modmask").toInt());
        QVariantMap row;
        row["keys"] = mods.isEmpty() ? key.toUpper() : mods + " + " + key.toUpper();
        row["description"] = bind.value("description").toString();
        row["submap"] = bind.value("submap").toString();
        row["mouse"] = bind.value("mouse").toBool();
        rows << row;
    }
    std::stable_sort(rows.begin(), rows.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("description").toString().localeAwareCompare(b.toMap().value("description").toString()) < 0;
    });
    return rows;
}

QString ShortcutsProvider::canonicalKeys(const QString &keys) {
    for (const QChar c : keys)
        if (c.category() == QChar::Other_Control) return {};
    QStringList parts;
    for (const auto &part : keys.split('+'))
        parts << part.trimmed().toUpper();
    if (parts.size() < 2 || parts.contains(QString()))
        return {};
    const QString key = parts.takeLast();
    static const QStringList supported{"SUPER", "CTRL", "ALT", "SHIFT"};
    QSet<QString> seen;
    for (const auto &part : parts) {
        if (!supported.contains(part) || seen.contains(part)) return {};
        seen.insert(part);
    }
    if (supported.contains(key)) return {};
    QStringList ordered;
    for (const char *mod : {"SUPER", "CTRL", "ALT", "SHIFT"})
        if (parts.contains(QLatin1String(mod)))
            ordered << QLatin1String(mod);
    ordered << key;
    return ordered.join(" + ");
}

bool ShortcutsProvider::isValidKeys(const QString &keys) {
    // A chord needs a modifier, or an ordinary letter would stop typing anywhere.
    static const QRegularExpression valid(R"(\A((SUPER|CTRL|ALT|SHIFT) \+ )+[A-Z0-9_]{1,24}\z)");
    return valid.match(keys).hasMatch() && canonicalKeys(keys) == keys;
}

QVariantList ShortcutsProvider::ownedBinds() const {
    return HyprChange::selectable(HyprModule(m_runner->env()).inspect(), "binds").toVariantList();
}

void ShortcutsProvider::refresh() {
    refresh({});
}

void ShortcutsProvider::refresh(std::function<void()> done) {
    m_runner->run(m_runner->env().hyprctl({"-j", "binds"}), CommandOptions{4000}, this, [this, done](const CommandResult &result) {
        const auto document = QJsonDocument::fromJson(result.out);
        bool valid = document.isArray();
        for (const auto &value : document.array()) {
            const auto bind = value.toObject();
            const double mask = bind.value("modmask").toDouble(-1);
            valid &= value.isObject() && mask >= 0 && mask <= 255 && mask == int(mask) && bind.value("key").isString();
            for (const char *field : {"description", "submap", "dispatcher", "arg"})
                valid &= !bind.contains(field) || bind.value(field).isString();
            for (const char *field : {"catch_all", "mouse", "release", "repeat", "locked", "non_consuming", "auto_consuming", "longPress", "allow_input_capture"})
                valid &= !bind.contains(field) || bind.value(field).isBool();
            if (bind.contains("keycode")) {
                const double code = bind.value("keycode").toDouble(-1);
                valid &= code >= 0 && code <= 65535 && code == int(code);
            }
        }
        m_status = result.ok() && !result.truncated && valid ? "ok" : "unavailable";
        m_reason = m_status == QLatin1String("ok") ? QString() : tr("Hyprland's shortcut list could not be read completely as valid JSON.");
        m_reportedBinds = m_status == QLatin1String("ok") ? document.array() : QJsonArray{};
        m_binds = m_status == QLatin1String("ok") ? parseBinds(result.out) : QVariantList{};
        emit changed();
        if (done) done();
    });
}

QJsonObject ShortcutsProvider::snapshot() const {
    auto owned = HyprModule(m_runner->env()).inspect();
    if (owned.value("binds").isArray()) owned["binds"] = HyprChange::selectable(owned, "binds");
    const QString state = owned.value("status").toString();
    return {{"effective", QJsonObject{{"status", m_status}, {"reason", m_reason},
                                     {"source", QJsonArray{"hyprctl", "-j", "binds"}},
                                     {"binds", m_status == QLatin1String("ok") ? QJsonValue(m_reportedBinds) : QJsonValue(QJsonValue::Null)}}},
            {"owned", owned}, {"complete", m_status == QLatin1String("ok") && (state == QLatin1String("ok") || state == QLatin1String("absent"))}};
}

QVariantMap ShortcutsProvider::checkKeys(const QString &keys) const {
    QVariantMap result;
    const QString canonical = canonicalKeys(keys);
    result["keys"] = canonical;
    result["valid"] = isValidKeys(canonical);
    QStringList conflicts;
    QVariantList matches, unresolved;
    int mask = 0;
    const auto parts = canonical.split(" + ");
    if (parts.contains("SUPER")) mask |= 64;
    if (parts.contains("CTRL")) mask |= 4;
    if (parts.contains("ALT")) mask |= 8;
    if (parts.contains("SHIFT")) mask |= 1;
    for (const auto &bind : m_reportedBinds) {
        auto map = bind.toObject().toVariantMap();
        if (map.value("modmask").toInt() != mask || !isValidKeys(canonical)) continue;
        const QString key = map.value("key").toString().toUpper();
        if (key == parts.last() || map.value("catch_all").toBool()) {
            map["matchKind"] = map.value("catch_all").toBool() ? "catch-all" : "named-key";
            matches << map;
            QString description = map.value("description").toString();
            if (description.isEmpty()) description = map.value("dispatcher").toString() + " " + map.value("arg").toString();
            conflicts << (description.trimmed().isEmpty() ? tr("Unnamed shortcut") : description.trimmed());
        } else if (map.value("keycode").toInt() != 0 || key.isEmpty() || key.contains('&')) {
            unresolved << map;
        }
    }
    result["conflicts"] = conflicts;
    result["matches"] = matches;
    result["unresolved"] = unresolved;
    result["available"] = m_status == QLatin1String("ok");
    result["reason"] = m_reason;
    result["collisionState"] = !isValidKeys(canonical) ? "invalid" : m_status != QLatin1String("ok") ? "unavailable"
        : !matches.isEmpty() ? "conflict" : !unresolved.isEmpty() ? "unknown" : "no_named_match";
    result["scope"] = "Reported modifier masks and named keys across all submaps; keycode/multi-key aliases and keyboard layout equivalence are not resolved.";
    // Without these there is no keyboard path back to any window or menu.
    static const QStringList critical{"SUPER + SPACE", "SUPER + RETURN", "SUPER + ALT + SPACE"};
    result["critical"] = critical.contains(canonical);
    return result;
}


void ShortcutsProvider::reject(const ProviderPlan &plan) {
    Transaction tx; tx.settingId = "shortcuts"; tx.phase = "failed"; tx.message = plan.error;
    emit message(plan.error, true);
    emit operationFinished(tx);
}

void ShortcutsProvider::planAdd(const QString &keys, const QString &description, const QString &command, bool replace, std::function<void(ProviderPlan)> done) {
    const QString canonical = canonicalKeys(keys);
    ProviderPlan invalid; invalid.operation = "shortcuts.add"; invalid.target = canonical;
    auto ordinary = [](const QString &text) {
        for (const QChar c : text) if (c.category() == QChar::Other_Control) return false;
        return true;
    };
    const bool numericCode = QRegularExpression("\\A[0-9]{2,}\\z").match(canonical.section(" + ", -1)).hasMatch();
    if (!isValidKeys(canonical) || numericCode || command.trimmed().isEmpty() || command.size() > 8192 || description.size() > 256 || !ordinary(command) || !ordinary(description)) {
        invalid.error = command.trimmed().isEmpty() ? tr("Enter the command this shortcut should run.")
            : tr("Use a valid modified key, a description up to 256 characters and a command up to 8192 characters, without control characters.");
        done(invalid); return;
    }
    auto plan = HyprChange::base(m_runner->env(), "shortcuts.add", canonical, "binds");
    if (!plan.ok) { done(plan); return; }
    refresh([this, plan, description, command, replace, done]() mutable {
        const auto check = checkKeys(plan.target);
        plan.details["check"] = QJsonObject::fromVariantMap(check);
        auto refuse = [&](const QString &code, const QString &reason) { plan.ok = false; plan.errorCode = code; plan.error = reason; done(plan); };
        if (m_status != QLatin1String("ok")) { refuse("read_unavailable", m_reason); return; }
        if (!check.value("unresolved").toList().isEmpty()) { refuse("unsupported_configuration", tr("Keycode or multi-key shortcuts cannot be replaced by a named-key plan.")); return; }
        for (const auto &item : check.value("matches").toList()) {
            const auto row = item.toMap();
            if (!row.value("submap").toString().isEmpty() || row.value("catch_all").toBool()) {
                refuse("unsupported_configuration", tr("This chord matches a submap or catch-all binding. Manage it in the source configuration.")); return;
            }
        }
        auto rows = plan.details.value("entriesBefore").toArray();
        QJsonArray after;
        int owned = 0; bool preservesUnbind = false;
        for (const auto &item : rows) {
            const auto record = item.toObject();
            if (canonicalKeys(record.value("keys").toString()) == plan.target) {
                ++owned; preservesUnbind |= record.value("replaces").toBool();
            } else after.append(item);
        }
        if (owned > 1) { refuse("unsupported_configuration", tr("The owned module contains duplicate shortcut keys.")); return; }
        const bool collision = owned || !check.value("matches").toList().isEmpty();
        if (collision && !replace) { refuse("conflict", tr("%1 is already in use. Choose to replace it, or pick other keys.").arg(plan.target)); return; }
        const QJsonObject record{{"keys", plan.target}, {"description", description.trimmed()}, {"command", command.trimmed()},
                                 {"replaces", preservesUnbind || (replace && collision)}};
        after.append(record);
        plan.details["record"] = record;
        plan.details["entriesAfter"] = after;
        plan.details["verificationScope"] = "owned-command-file-reload-and-live-lua-binding";
        plan.details["liveCommandKnown"] = false;
        plan.effects << tr("Hyprland reports an opaque Lua reference. The command text is verified in the owned file, not through the live dispatcher or by executing it.");
        plan.before = QString::number(rows.size()) + " owned shortcuts";
        plan.after = plan.target;
        if (collision) plan.confirmText = check.value("critical").toBool()
            ? tr("Replacing this main launcher/terminal shortcut can remove a way back into the desktop. Keep another route available.")
            : tr("Replace the current binding for %1. Its existing action will stop using these keys.").arg(plan.target);
        HyprChange::runtime(m_runner, this, plan, done);
    });
}

void ShortcutsProvider::planRemove(const QString &selector, std::function<void(ProviderPlan)> done) {
    auto plan = HyprChange::base(m_runner->env(), "shortcuts.remove", selector, "binds");
    if (!plan.ok) { done(plan); return; }
    const int index = HyprChange::selected(plan, selector);
    if (index < 0) { plan.ok = false; plan.errorCode = "stale_target"; plan.error = tr("That shortcut selector is invalid or stale. Read shortcut list again."); done(plan); return; }
    auto rows = plan.details.value("entriesBefore").toArray();
    const auto record = rows.at(index).toObject();
    if (!isValidKeys(canonicalKeys(record.value("keys").toString()))) {
        plan.ok = false; plan.errorCode = "unsupported_configuration"; plan.error = tr("The owned shortcut has invalid keys."); done(plan); return;
    }
    plan.details["record"] = record;
    rows.removeAt(index); plan.details["entriesAfter"] = rows;
    plan.before = record.value("keys").toString(); plan.after = "removed";
    plan.effects << tr("Remove only the owned entry. Inherited bindings may reappear; this does not globally unbind the chord.");
    if (checkKeys(record.value("keys").toString()).value("critical").toBool())
        plan.confirmText = tr("Removing this owned launcher/terminal shortcut may remove a way back into the desktop. Keep another route available.");
    HyprChange::runtime(m_runner, this, plan, done);
}

void ShortcutsProvider::applyPlan(const ProviderPlan &plan) {
    if (!plan.ok) { reject(plan); return; }
    ChangeCoordinator::Step preflight;
    if (plan.operation == QLatin1String("shortcuts.add")) {
        preflight = [this, plan](ChangeCoordinator::Done done) {
            refresh([this, plan, done] {
                const auto fresh = QJsonObject::fromVariantMap(checkKeys(plan.target));
                done(m_status == QLatin1String("ok") && fresh == plan.details.value("check").toObject(),
                     tr("Shortcut collisions changed after planning. Read the current bindings and plan again."));
            });
        };
    }
    const auto record = plan.details.value("record").toObject();
    ChangeCoordinator::Step verify = [this, plan, record](ChangeCoordinator::Done done) {
        refresh([this, plan, record, done] {
            if (m_status != QLatin1String("ok")) { done(false, m_reason); return; }
            if (plan.operation == QLatin1String("shortcuts.remove")) { done(true, {}); return; }
            const auto check = checkKeys(record.value("keys").toString());
            const auto matches = check.value("matches").toList();
            bool confirmed = matches.size() == 1 && check.value("unresolved").toList().isEmpty();
            if (confirmed) {
                const auto bind = matches.first().toMap();
                confirmed = bind.value("submap").toString().isEmpty() && bind.value("dispatcher").toString() == QLatin1String("__lua")
                    && QRegularExpression("\\A[1-9][0-9]*\\z").match(bind.value("arg").toString()).hasMatch()
                    && bind.value("description").toString() == record.value("description").toString() && bind.value("keycode").toInt() == 0;
                for (const char *flag : {"catch_all", "mouse", "release", "repeat", "locked", "non_consuming", "auto_consuming", "longPress", "allow_input_capture"})
                    confirmed &= !bind.value(flag).toBool();
            }
            done(confirmed, tr("Hyprland did not report the expected Lua shortcut and description."));
        });
    };
    HyprChange::submit(m_runner, m_coordinator, this, plan, preflight, verify, [this, plan](const Transaction &tx) {
        emit message(tx.phase == QLatin1String("committed") ? (plan.operation == QLatin1String("shortcuts.add") ? tr("Shortcut saved") : tr("Shortcut removed")) : tx.message,
                     tx.phase != QLatin1String("committed"));
        emit operationFinished(tx);
        if (tx.phase == QLatin1String("committed")) emit changed();
        else refresh();  // Failure recovery may have restored different live bindings.
    });
}

void ShortcutsProvider::addBinding(const QString &keys, const QString &description, const QString &command, bool replaceExisting) {
    planAdd(keys, description, command, replaceExisting, [this](const ProviderPlan &plan) { applyPlan(plan); });
}

void ShortcutsProvider::removeBinding(const QString &keys) {
    if (HyprChange::validSelector(keys)) {
        planRemove(keys, [this](const ProviderPlan &plan) { applyPlan(plan); });
        return;
    }
    const QString canonical = canonicalKeys(keys);
    if (!isValidKeys(canonical)) {
        ProviderPlan invalid; invalid.error = tr("A shortcut needs at least one modifier and one key."); reject(invalid); return;
    }
    const auto owned = HyprModule(m_runner->env()).inspect();
    const auto rows = HyprChange::selectable(owned, "binds");
    QString selector;
    for (const auto &item : rows)
        if (canonicalKeys(item.toObject().value("keys").toString()) == canonical) selector = item.toObject().value("selector").toString();
    planRemove(selector, [this](const ProviderPlan &plan) { applyPlan(plan); });
}
