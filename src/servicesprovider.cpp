#include "servicesprovider.h"

#include "iconthemes.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>

#include "fileops.h"

ServicesProvider::ServicesProvider(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *parent)
    : QObject(parent), m_runner(runner), m_coordinator(coordinator) {}

bool ServicesProvider::isSafeUnit(const QString &unit) {
    static const QRegularExpression safe(R"(^[A-Za-z0-9_][A-Za-z0-9:_.@\\-]*\.service$)");
    const auto match = safe.match(unit);
    return match.hasMatch() && match.capturedLength() == unit.size() && !unit.endsWith("@.service");
}

QVariantList ServicesProvider::mergeUnits(const QByteArray &units, const QByteArray &files) {
    QHash<QString, QString> enablement;
    for (const auto &value : QJsonDocument::fromJson(files).array()) {
        const auto file = value.toObject();
        enablement.insert(file.value("unit_file").toString(), file.value("state").toString());
    }
    QVariantList rows;
    QStringList seen;
    for (const auto &value : QJsonDocument::fromJson(units).array()) {
        const auto unit = value.toObject();
        const QString name = unit.value("unit").toString();
        if (!name.endsWith(".service"))
            continue;
        seen << name;
        QVariantMap row;
        row["unit"] = name;
        row["description"] = unit.value("description").toString();
        row["active"] = unit.value("active").toString();
        row["load"] = unit.value("load").toString();
        row["sub"] = unit.value("sub").toString();
        row["enabled"] = enablement.value(name);
        row["failed"] = unit.value("active").toString() == QLatin1String("failed");
        rows << row;
    }
    // Installed but never started this boot; it can still be enabled from here.
    for (auto it = enablement.constBegin(); it != enablement.constEnd(); ++it) {
        if (seen.contains(it.key()) || !isSafeUnit(it.key()))
            continue;
        QVariantMap row;
        row["unit"] = it.key();
        row["description"] = QString();
        row["active"] = QStringLiteral("inactive");
        row["load"] = QStringLiteral("not-loaded");
        row["sub"] = QStringLiteral("dead");
        row["enabled"] = it.value();
        row["failed"] = false;
        rows << row;
    }
    std::sort(rows.begin(), rows.end(), [](const QVariant &a, const QVariant &b) {
        const auto ma = a.toMap(), mb = b.toMap();
        if (ma.value("failed").toBool() != mb.value("failed").toBool())
            return ma.value("failed").toBool();
        const bool activeA = ma.value("active").toString() == QLatin1String("active");
        const bool activeB = mb.value("active").toString() == QLatin1String("active");
        if (activeA != activeB)
            return activeA;
        return ma.value("unit").toString() < mb.value("unit").toString();
    });
    return rows;
}

int ServicesProvider::failedCount() const {
    int count = 0;
    for (const auto *rows : {&m_user, &m_system})
        for (const auto &row : *rows)
            if (row.toMap().value("failed").toBool())
                ++count;
    return count;
}

static bool validServiceList(const CommandResult &result, bool files) {
    const auto document = QJsonDocument::fromJson(result.out);
    if (!result.ok() || result.truncated || !document.isArray())
        return false;
    for (const auto &value : document.array()) {
        const auto row = value.toObject();
        if (!value.isObject() || !row.value(files ? "unit_file" : "unit").isString()
            || !row.value(files ? "state" : "active").isString())
            return false;
    }
    return true;
}

void ServicesProvider::loadScope(bool system, std::function<void()> done) {
    const int generation = ++m_generation[system ? 1 : 0];
    (system ? m_systemStatus : m_userStatus) = "loading";
    const QString scope = system ? "--system" : "--user";
    m_runner->run({"systemctl", scope, "list-units", "--type=service", "--all", "-o", "json", "--no-pager"}, CommandOptions{6000}, this,
                  [this, system, scope, generation, done](const CommandResult &units) {
        m_runner->run({"systemctl", scope, "list-unit-files", "--type=service", "-o", "json", "--no-pager"}, CommandOptions{6000}, this,
                      [this, system, units, generation, done](const CommandResult &files) {
            if (generation != m_generation[system ? 1 : 0])
                return;
            const bool readable = validServiceList(units, false) && validServiceList(files, true);
            (system ? m_system : m_user) = readable ? mergeUnits(units.out, files.out) : QVariantList{};
            (system ? m_systemStatus : m_userStatus) = readable ? "ok" : "unavailable";
            m_status = m_userStatus == QLatin1String("unavailable") || m_systemStatus == QLatin1String("unavailable") ? "unavailable"
                : m_userStatus == QLatin1String("ok") && m_systemStatus == QLatin1String("ok") ? "ok" : "loading";
            emit changed();
            done();
        });
    });
}

void ServicesProvider::refreshServices(bool system) {
    loadScope(system, [this] { emit refreshed(); });
}

void ServicesProvider::refreshStartup() {
    QString error;
    m_startup = readStartup(m_runner->env(), &error);
    m_startupStatus = error.isEmpty() ? "ok" : "unavailable";
    emit changed();
    emit refreshed();
}

void ServicesProvider::refresh() {
    auto remaining = std::make_shared<int>(2);
    const auto done = [this, remaining] { if (--*remaining == 0) emit refreshed(); };
    loadScope(false, done);
    loadScope(true, done);
    QString error;
    m_startup = readStartup(m_runner->env(), &error);
    m_startupStatus = error.isEmpty() ? "ok" : "unavailable";
    emit changed();
}

void ServicesProvider::readService(const QString &unit, bool system, std::function<void(bool, QJsonObject)> done) {
    m_runner->run({"systemctl", system ? "--system" : "--user", "show", "--all", "--no-pager",
                  "--property=Id,LoadState,FragmentPath,ActiveState,SubState,UnitFileState,Type,Result,InvocationID,ExecMainStartTimestampMonotonic", unit},
                 CommandOptions{5000}, this, [done](const CommandResult &result) {
        QJsonObject fields;
        bool valid = result.ok() && !result.truncated;
        for (const QString &line : result.outText().split('\n', Qt::SkipEmptyParts)) {
            const int equals = line.indexOf('=');
            if (equals < 1 || fields.contains(line.left(equals))) {
                valid = false;
                continue;
            }
            fields[line.left(equals)] = line.mid(equals + 1);
        }
        for (const char *key : {"Id", "LoadState", "FragmentPath", "ActiveState", "SubState", "UnitFileState", "Type", "Result", "ExecMainStartTimestampMonotonic"})
            valid &= fields.contains(QLatin1String(key));
        bool numeric = false;
        fields.value("ExecMainStartTimestampMonotonic").toString().toULongLong(&numeric);
        valid &= numeric && isSafeUnit(fields.value("Id").toString());
        done(valid, fields);
    });
}

void ServicesProvider::planService(const QString &unit, const QString &action, bool system, std::function<void(ProviderPlan)> done) {
    ProviderPlan p;
    p.operation = "services.action";
    p.target = unit;
    if (!isSafeUnit(unit) || !QStringList{"start", "stop", "restart", "enable", "disable"}.contains(action)) {
        p.errorCode = "invalid_arguments";
        p.error = tr("That service action is not available.");
        done(p);
        return;
    }
    readService(unit, system, [p, unit, action, system, done](bool known, const QJsonObject &facts) mutable {
        const auto fail = [&](const QString &code, const QString &error) { p.errorCode = code; p.error = error; done(p); };
        if (!known) {
            fail("read_unavailable", tr("The service's state could not be confirmed."));
            return;
        }
        const QString load = facts.value("LoadState").toString(), active = facts.value("ActiveState").toString();
        if (load == QLatin1String("not-found")) {
            fail("stale_target", "This service is no longer installed in the selected scope.");
            return;
        }
        if ((load != QLatin1String("loaded") && !(action == QLatin1String("stop") && load == QLatin1String("masked")))
            || !QStringList{"active", "inactive", "failed"}.contains(active)) {
            fail("unsupported_configuration", "This service is masked, unavailable, or still changing state. Inspect it before changing it.");
            return;
        }
        const bool enablement = action == QLatin1String("enable") || action == QLatin1String("disable");
        const QString fileState = facts.value("UnitFileState").toString();
        if (enablement && !QStringList{"enabled", "disabled"}.contains(fileState)) {
            fail("unsupported_configuration", "Enable/disable requires a normal enabled or disabled unit file; static, linked, masked and runtime-only states need their owning tool.");
            return;
        }
        p.before = enablement ? fileState : active;
        p.after = enablement ? (action == QLatin1String("enable") ? "enabled" : "disabled")
            : action == QLatin1String("stop") ? "inactive" : "active-or-completed-oneshot";
        p.details = {{"scope", system ? "system" : "user"}, {"action", action}, {"beforeState", facts},
                     {"authorization", system ? "system-policy" : "user-manager"}, {"verificationScope", "selected-unit"}};
        p.effects << (enablement ? "Changes persistent unit enablement; it does not start or stop the service."
                                 : "Changes this service now. Its unit can start or stop dependencies and execute configured hooks.");
        if (system && QStringList{"stop", "restart", "disable"}.contains(action))
            p.confirmText = "This is a system service. Stopping, restarting or disabling it can disconnect or freeze this computer and affect dependent services.";
        p.ok = true;
        done(p);
    });
}

void ServicesProvider::reject(const ProviderPlan &plan) {
    Transaction tx;
    tx.phase = "failed";
    tx.message = plan.error;
    emit message(tx.message, true);
    emit operationFinished(tx);
}

void ServicesProvider::serviceAction(const QString &unit, const QString &action, bool system) {
    changeService(unit, action, system, true);
}

void ServicesProvider::changeService(const QString &unit, const QString &action, bool system, bool interactive) {
    planService(unit, action, system, [this, unit, action, system, interactive](const ProviderPlan &p) {
        if (!p.ok) {
            reject(p);
            return;
        }
        ChangeCoordinator::Request request;
        request.settingId = "services." + QString(system ? "system." : "user.") + unit;
        request.label = unit;
        request.before = p.before;
        request.after = p.after;
        request.extra = p.details;
        request.extra["target"] = unit;
        request.apply = [this, unit, action, system, interactive, p](ChangeCoordinator::Done done) {
            readService(unit, system, [this, unit, action, system, interactive, p, done](bool known, const QJsonObject &current) {
                if (!known || current != p.details.value("beforeState").toObject()) {
                    done(false, tr("The service changed or could not be read; refresh before changing it."));
                    return;
                }
                QStringList argv{"systemctl", system ? "--system" : "--user", action, unit};
                if (!interactive)
                    argv << "--no-ask-password";
                CommandOptions options;
                options.mutation = true;
                options.timeoutMs = 90000;
                m_runner->run(argv, options, this, [done](const CommandResult &result) { done(result.ok(), result.describe("systemctl")); });
            });
        };
        auto settled = std::make_shared<QString>(p.after);
        request.verify = [this, unit, action, system, p, settled](ChangeCoordinator::Done done) {
            readService(unit, system, [this, action, p, settled, done](bool known, const QJsonObject &current) {
                const auto before = p.details.value("beforeState").toObject();
                if (!known || current.value("Id") != before.value("Id") || current.value("FragmentPath") != before.value("FragmentPath")
                    || current.value("LoadState") != before.value("LoadState")) {
                    done(false, tr("The service's state could not be confirmed."));
                    return;
                }
                const QString active = current.value("ActiveState").toString();
                const auto started = current.value("ExecMainStartTimestampMonotonic").toString().toULongLong();
                const auto previousStart = before.value("ExecMainStartTimestampMonotonic").toString().toULongLong();
                const bool newRun = started > previousStart;
                const bool ranFine = active == QLatin1String("inactive") && current.value("Type").toString() == QLatin1String("oneshot")
                    && current.value("Result").toString() == QLatin1String("success") && newRun;
                bool verified = false;
                if (action == QLatin1String("enable") || action == QLatin1String("disable")) {
                    *settled = current.value("UnitFileState").toString();
                    verified = *settled == p.after;
                } else {
                    *settled = ranFine ? "completed-oneshot" : active;
                    verified = action == QLatin1String("stop") ? active == QLatin1String("inactive")
                        : action == QLatin1String("restart") ? (active == QLatin1String("active") && newRun) || ranFine
                        : active == QLatin1String("active") || ranFine;
                }
                done(verified, action == QLatin1String("start") || action == QLatin1String("restart") ? tr("The service did not start.")
                    : action == QLatin1String("stop") ? tr("The service is still running.") : tr("The service's enablement could not be confirmed."));
            });
        };
        request.settled = [settled] { return *settled; };
        m_coordinator->submit(std::move(request), [this, unit, action, system](const Transaction &tx) {
            emit message(tx.phase == QLatin1String("committed") ? tr("%1: %2").arg(unit, action) : tx.message, tx.phase != QLatin1String("committed"));
            emit operationFinished(tx);
            refreshServices(system);
        });
    });
}

static QHash<QString, QString> parseDesktop(const QByteArray &contents, bool *valid) {
    QHash<QString, QString> values;
    bool inEntry = false, duplicate = false;
    int entries = 0;
    for (const auto &raw : QString::fromUtf8(contents).split('\n')) {
        const QString line = raw.trimmed();
        if (line.startsWith('#') || line.isEmpty())
            continue;
        if (line.startsWith('[')) {
            inEntry = line == QLatin1String("[Desktop Entry]");
            entries += inEntry ? 1 : 0;
            continue;
        }
        const int equals = line.indexOf('=');
        if (inEntry && equals > 0) {
            const QString key = line.left(equals).trimmed();
            duplicate |= values.contains(key);
            values.insert(key, line.mid(equals + 1).trimmed());
        }
    }
    *valid = entries == 1 && !duplicate;
    for (const QString &key : {QStringLiteral("Hidden"), QStringLiteral("X-GNOME-Autostart-enabled")})
        if (values.contains(key) && values.value(key) != QLatin1String("true") && values.value(key) != QLatin1String("false"))
            *valid = false;
    return values;
}

static QString startupUserDirectory(const Env &env) {
    const QString config = env.process.value("XDG_CONFIG_HOME");
    return (QDir::isAbsolutePath(config) ? QDir::cleanPath(config) : env.configHome) + "/autostart";
}

static QStringList startupSystemDirectories(const Env &env) {
    QStringList directories;
    for (const QString &config : env.process.value("XDG_CONFIG_DIRS").split(':', Qt::SkipEmptyParts))
        if (QDir::isAbsolutePath(config) && !directories.contains(QDir::cleanPath(config) + "/autostart"))
            directories << QDir::cleanPath(config) + "/autostart";
    if (directories.isEmpty())
        directories << env.sys("/etc/xdg/autostart");
    return directories;
}

static QStringList startupDirectories(const Env &env) {
    QStringList directories = startupSystemDirectories(env);
    // Read from lowest to highest priority. A higher entry replaces the whole
    // lower entry, not just the keys it happens to mention.
    std::reverse(directories.begin(), directories.end());
    directories.removeAll(startupUserDirectory(env));
    directories << startupUserDirectory(env);
    return directories;
}

QVariantList ServicesProvider::readStartup(const Env &env, QString *error) {
    if (error)
        error->clear();
    QHash<QString, QVariantMap> rows;
    const QString userDirectory = startupUserDirectory(env);
    for (const QString &directory : startupDirectories(env)) {
        const QFileInfo directoryInfo(directory);
        if (directoryInfo.isSymLink() && !directoryInfo.exists()) {
            if (error) *error = "A startup directory link is broken.";
            continue;
        }
        if (!directoryInfo.exists())
            continue;
        if (!directoryInfo.isDir() || !directoryInfo.isReadable() || !directoryInfo.isExecutable()) {
            if (error) *error = "A startup directory could not be read.";
            continue;
        }
        const bool user = directory == userDirectory;
        for (const auto &file : QDir(directory).entryInfoList({"*.desktop"}, QDir::Files | QDir::System)) {
            QByteArray contents;
            const bool readable = file.isFile() && file.size() <= 1024 * 1024 && FileOps::readFile(file.absoluteFilePath(), &contents);
            bool valid = false;
            const auto values = parseDesktop(contents, &valid);
            QVariantMap row;
            const bool override = rows.contains(file.fileName());
            row["id"] = file.fileName();
            row["name"] = values.value("Name", file.baseName());
            row["comment"] = values.value("Comment");
            row["exec"] = values.value("Exec");
            row["iconName"] = values.value("Icon");
            row["enabled"] = readable && valid && values.value("Hidden") != QLatin1String("true") && values.value("X-GNOME-Autostart-enabled") != QLatin1String("false");
            row["status"] = !readable ? "unreadable" : valid ? "ok" : "unsupported_configuration";
            row["source"] = user ? (override ? "override" : "user") : "system";
            row["path"] = file.absoluteFilePath();
            row["fingerprint"] = readable ? FileOps::fingerprintBytes(contents) : QStringLiteral("unreadable");
            row["userFile"] = userDirectory + "/" + file.fileName();
            row["conditions"] = QVariantMap{{"onlyShowIn", values.value("OnlyShowIn")}, {"notShowIn", values.value("NotShowIn")},
                                           {"tryExec", values.value("TryExec")}, {"systemdSkip", values.value("X-systemd-skip")}};
            rows.insert(file.fileName(), row);
        }
    }
    QVariantList list;
    const QString theme = IconThemes::desktopDefault(env);
    QHash<QString, QString> icons;
    for (auto row : std::as_const(rows)) {
        const QString name = row.value("iconName").toString();
        if (!icons.contains(name))
            icons.insert(name, name.isEmpty() ? QString() : IconThemes::appIconUrl(env, theme, {name}));
        row["icon"] = icons.value(name);
        list << row;
    }
    std::sort(list.begin(), list.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("name").toString().localeAwareCompare(b.toMap().value("name").toString()) < 0;
    });
    return list;
}

QByteArray ServicesProvider::withHidden(const QByteArray &entry, bool hidden) {
    QList<QByteArray> lines = entry.split('\n');
    bool found = false, inEntry = false;
    int entryLine = -1;
    const bool crlf = entry.contains("\r\n");
    for (int i = 0; i < lines.size(); ++i) {
        const auto trimmed = lines[i].trimmed();
        if (trimmed.startsWith('[')) {
            inEntry = trimmed == "[Desktop Entry]";
            if (inEntry) entryLine = i;
            continue;
        }
        if (!inEntry || trimmed.startsWith('#'))
            continue;
        const int equals = lines[i].indexOf('=');
        const auto key = lines[i].left(equals).trimmed();
        if (equals < 1)
            continue;
        if (key == "Hidden") {
            lines[i] = lines[i].left(equals + 1) + (hidden ? "true" : "false") + (crlf ? "\r" : "");
            found = true;
        } else if (!hidden && key == "X-GNOME-Autostart-enabled") {
            lines[i] = lines[i].left(equals + 1) + "true" + (crlf ? "\r" : "");
        }
    }
    if (!found) {
        if (entryLine < 0) {
            lines.prepend(crlf ? "[Desktop Entry]\r" : "[Desktop Entry]");
            entryLine = 0;
        }
        lines.insert(entryLine + 1, QByteArray(hidden ? "Hidden=true" : "Hidden=false") + (crlf ? "\r" : ""));
    }
    return lines.join('\n');
}

ProviderPlan ServicesProvider::planStartup(const QString &id, bool enabled) const {
    ProviderPlan p;
    p.operation = "startup.enabled";
    p.target = id;
    p.after = enabled ? "true" : "false";
    const auto fail = [&](const QString &code, const QString &message) { p.errorCode = code; p.error = message; return p; };
    if (id.contains('/') || id.contains('\\') || !id.endsWith(".desktop") || id.size() <= 8 || id.contains(QRegularExpression("[\\x00-\\x1f]")))
        return fail("invalid_value", tr("That login item is not valid."));
    QString error;
    QVariantMap selected;
    for (const auto &row : readStartup(m_runner->env(), &error))
        if (row.toMap().value("id").toString() == id)
            selected = row.toMap();
    if (!error.isEmpty())
        return fail("read_unavailable", error);
    if (selected.isEmpty())
        return fail("stale_target", tr("This login item no longer exists."));
    const QString status = selected.value("status").toString();
    if (status != QLatin1String("ok"))
        return fail(status == QLatin1String("unreadable") ? "read_unavailable" : status, "The selected desktop entry could not be read or has ambiguous preference keys.");
    const QString userFile = selected.value("userFile").toString();
    if (QFileInfo(userFile).isSymLink() || QFileInfo(QFileInfo(userFile).absolutePath()).isSymLink())
        return fail("ownership_conflict", "The user startup entry or autostart directory is a link. Copy it to a regular user entry before changing it here.");
    const QString userDirectory = QFileInfo(QFileInfo(userFile).absolutePath()).canonicalFilePath();
    for (const QString &systemDirectory : startupSystemDirectories(m_runner->env()))
        if (!userDirectory.isEmpty() && userDirectory == QFileInfo(systemDirectory).canonicalFilePath())
            return fail("ownership_conflict", "The user startup directory resolves to a system directory. Choose a separate user configuration directory before changing it here.");
    p.before = selected.value("enabled").toBool() ? "true" : "false";
    p.details = {{"source", selected.value("source").toString()}, {"sourceFile", selected.value("path").toString()},
                 {"sourceFingerprint", selected.value("fingerprint").toString()}, {"userFile", userFile},
                 {"userFingerprint", FileOps::fingerprint(userFile)}, {"verificationScope", "startup-preference-only"},
                 {"conditions", QJsonObject::fromVariantMap(selected.value("conditions").toMap())}, {"failureRecovery", "restore-user-file"}};
    p.effects << "Changes the user's Hidden preference and, when enabling, the GNOME autostart flag. Package files and other launch conditions are preserved."
              << "Takes effect at a later login; this does not start or stop the app or prove it will launch. The user override is backed up for failure recovery; there is no automatic history undo.";
    p.ok = true;
    return p;
}

void ServicesProvider::setStartupEnabled(const QString &id, bool enabled) {
    const auto p = planStartup(id, enabled);
    if (!p.ok) {
        reject(p);
        return;
    }
    const QString userFile = p.details.value("userFile").toString();
    const QString sourceFile = p.details.value("sourceFile").toString();
    ChangeCoordinator::Request request;
    request.settingId = "startup." + id;
    request.label = id;
    request.before = p.before;
    request.after = p.after;
    request.files << userFile;
    request.extra = p.details;
    request.expectedFingerprints.insert(sourceFile, p.details.value("sourceFingerprint").toString());
    request.expectedFingerprints.insert(userFile, p.details.value("userFingerprint").toString());
    request.apply = [userFile, sourceFile, p, enabled](ChangeCoordinator::Done done) {
        QByteArray contents;
        QString error;
        if (QFileInfo(userFile).isSymLink() || QFileInfo(QFileInfo(userFile).absolutePath()).isSymLink()
            || FileOps::fingerprint(userFile) != p.details.value("userFingerprint").toString()
            || !FileOps::readFile(sourceFile, &contents) || FileOps::fingerprintBytes(contents) != p.details.value("sourceFingerprint").toString()) {
            done(false, QObject::tr("The login item changed or could not be read; refresh before changing it."));
            return;
        }
        done(FileOps::atomicWrite(userFile, withHidden(contents, !enabled), &error, 0644), error);
    };
    request.verify = [this, id, enabled, userFile](ChangeCoordinator::Done done) {
        QString error;
        for (const auto &row : readStartup(m_runner->env(), &error))
            if (error.isEmpty() && row.toMap().value("id").toString() == id && row.toMap().value("status").toString() == QLatin1String("ok")
                && row.toMap().value("path").toString() == userFile && row.toMap().value("enabled").toBool() == enabled) {
                done(true, {});
                return;
            }
        done(false, tr("The login item did not change."));
    };
    m_coordinator->submit(std::move(request), [this](const Transaction &tx) {
        if (tx.phase != QLatin1String("committed"))
            emit message(tx.message, true);
        emit operationFinished(tx);
        refreshStartup();
    });
}
