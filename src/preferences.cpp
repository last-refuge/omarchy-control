#include "preferences.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSet>
#include <QDirIterator>
#include <QRegularExpression>
#include <memory>
#include <unistd.h>
#include "catalog.h"
#include "fileops.h"
#include "commandrunner.h"
#include "changecoordinator.h"
#include "compat.h"

void Preferences::crashMutes(CommandRunner *runner, ChangeCoordinator *coordinator, const QString &program,
                              const QString &value, bool dryRun, std::function<void(QJsonObject)> done) {
    const Env env = runner->env();
    const QString directory = env.home + "/.local/state/omarchy/toggles/crash-ignore";
    auto fail = [done](const QString &code, const QString &error) {
        done({{"ok", false}, {"outcome", "blocked"}, {"errorCode", code}, {"error", error}});
    };
    static const QRegularExpression plain("^[A-Za-z0-9._-]{1,255}$");
    if ((!program.isEmpty() && (!plain.match(program).hasMatch() || program == "." || program == ".."))
        || (!value.isEmpty() && (program.isEmpty() || (value != "true" && value != "false")))
        || (dryRun && value.isEmpty())) {
        fail("invalid_arguments", "Use preferences crash-mutes or crash-mute <program-basename> [true|false] [--dry-run]."); return;
    }
    const QString path = directory + '/' + (program.isEmpty() ? ".control-inspection" : program);
    const QString guard = FileOps::emptyMarkerError(env.home, path);
    if (!guard.isEmpty()) { fail("ownership_conflict", guard); return; }
    const bool before = QFileInfo::exists(path), desired = value == "true";
    QJsonObject result{{"ok", true}, {"directory", directory}, {"program", program}, {"path", path},
                       {"muted", before}, {"verificationScope", "owned-crash-notification-marker-only; watcher behavior unverified"},
                       {"recovery", "no-undo"}, {"outcome", value.isEmpty() ? "read" : "planned"}};
    if (program.isEmpty()) {
        QJsonArray items;
        QDirIterator it(directory, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
        while (it.hasNext()) {
            const QString item = it.next();
            if (items.size() >= 512) { fail("read_unavailable", "The mute directory exceeds the discovery limit."); return; }
            const QString error = FileOps::emptyMarkerError(env.home, item);
            items.append(QJsonObject{{"program", it.fileName()}, {"status", error.isEmpty() ? "ok" : "custom"},
                                     {"muted", error.isEmpty() ? QJsonValue(true) : QJsonValue(QJsonValue::Null)}, {"reason", error}});
        }
        result["items"] = items;
        result["complete"] = true;
        done(result); return;
    }
    if (value.isEmpty()) { done(result); return; }
    const QString changed = Compat(env).changedForKey("preferences.crash-mutes");
    if (!changed.isEmpty()) { fail("compatibility_blocked", changed + " changed since review."); return; }
    for (const auto &helper : {"omarchy", "omarchy-crash-mute", "omarchy-toggle", "omarchy-toggle-enabled"}) {
        if (env.findExecutable(helper).isEmpty()) {
            fail("unavailable", "Omarchy's crash-mute helpers are unavailable."); return;
        }
    }
    result["before"] = before; result["after"] = desired; result["changed"] = before != desired;
    result["effects"] = QJsonArray{"Changes one program's crash notification preference; it does not stop crash capture.",
                                   "Unconfirmed helper writes retain backups without automatic rollback or history undo."};
    if (dryRun) { done(result); return; }
    ChangeCoordinator::Request request;
    request.settingId = "preferences.crash-mutes@" + program; request.label = "Crash notifications: " + program;
    request.kind = "action"; request.before = before ? "true" : "false"; request.after = value;
    request.files = {path}; request.restoreFilesOnFailure = false;
    request.expectedFingerprints.insert(path, before ? FileOps::fingerprintBytes({}) : "missing");
    request.preflight = [env, path](auto ready) {
        QString error = FileOps::emptyMarkerError(env.home, path);
        if (error.isEmpty()) error = Compat(env).changedForKey("preferences.crash-mutes");
        ready(error.isEmpty(), error);
    };
    request.apply = [runner, env, path, program, desired, before](auto finished) {
        const QString error = FileOps::emptyMarkerError(env.home, path);
        if (!error.isEmpty()) { finished(false, error); return; }
        if (before == desired) { finished(true, {}); return; }
        CommandOptions options; options.timeoutMs = 5000; options.mutation = true;
        runner->run({"omarchy", "crash", "mute", "--", program, desired ? "on" : "off"}, options, runner,
                    [finished](const auto &command) { finished(command.ok(), command.describe()); });
    };
    request.verify = [env, path, desired](auto finished) {
        QString error = FileOps::emptyMarkerError(env.home, path);
        if (error.isEmpty() && QFileInfo::exists(path) != desired) error = "The requested crash mute marker was not read back.";
        finished(error.isEmpty(), error);
    };
    request.writtenFingerprints = [path, desired] { return QHash<QString, QString>{{path, desired ? FileOps::fingerprintBytes({}) : "missing"}}; };
    coordinator->submit(request, [done, result](const auto &tx) mutable {
        result["transaction"] = tx.toJson(); result["outcome"] = tx.phase;
        result["ok"] = tx.phase == "committed"; result["verified"] = tx.phase == "committed";
        if (tx.phase == "committed") result["muted"] = result["after"];
        else { result["errorCode"] = "change_not_confirmed"; result["error"] = tx.message; }
        done(result);
    });
}

QJsonObject Preferences::pins(const Env &env, const Catalog &catalog, const QString &action, const QString &id, bool dryRun) {
    const QString path = env.appConfigDir + "/preferences.json";
    auto fail = [&](const QString &code, const QString &error) { return QJsonObject{{"ok", false}, {"outcome", "blocked"}, {"errorCode", code}, {"error", error}, {"path", path}}; };
    if (!QStringList{"list", "pin", "unpin", "toggle"}.contains(action) || (action == "list" ? !id.isEmpty() || dryRun : id.isEmpty() || id.size() > 256))
        return fail("invalid_arguments", "Use preferences pins, pin <catalog-id>, or unpin <catalog-id> [--dry-run].");
    std::unique_ptr<QLockFile> lock;
    if (action != "list" && !dryRun) {
        if (!QDir().mkpath(env.appConfigDir)) return fail("write_unavailable", "The preferences directory could not be created.");
        lock = std::make_unique<QLockFile>(env.appConfigDir + "/preferences.lock");
        if (!lock->tryLock(2000)) return fail("busy", "Another Control window is changing preferences.");
    }
    QJsonObject object;
    QFileInfo info(path);
    if (info.isSymLink()) return fail("ownership_conflict", "The preferences file is linked; it was left alone.");
    if (info.exists()) {
        if (!info.isFile() || info.ownerId() != uint(geteuid()) || info.size() > 65536) return fail("read_unavailable", "The preferences file is unowned, oversized or not a regular file.");
        QByteArray bytes; QString error;
        if (!FileOps::readFile(path, &bytes, &error)) return fail("read_unavailable", error);
        QJsonParseError parsed; const auto doc = QJsonDocument::fromJson(bytes, &parsed);
        if (parsed.error != QJsonParseError::NoError || !doc.isObject()) return fail("read_unavailable", "Preferences are invalid JSON and were not overwritten.");
        object = doc.object();
    }
    const auto value = object.value("pins");
    if ((!value.isUndefined() && !value.isArray()) || value.toArray().size() > 256) return fail("read_unavailable", "The stored pins list is malformed or excessive.");
    QStringList before; QSet<QString> seen;
    for (const auto &pin : value.toArray()) {
        if (!pin.isString() || pin.toString().isEmpty() || pin.toString().size() > 256 || seen.contains(pin.toString())) return fail("read_unavailable", "The stored pins list has invalid or duplicate IDs.");
        before << pin.toString(); seen.insert(pin.toString());
    }
    QStringList after = before;
    if (action != "list") {
        const bool remove = action == "unpin" || (action == "toggle" && before.contains(id));
        if (!remove && !catalog.find(id)) return fail("unknown_id", "Choose a catalog ID from search or catalog.");
        if (remove) after.removeAll(id); else if (!after.contains(id)) after << id;
        if (after.size() > 256) return fail("invalid_value", "At most 256 entries can be pinned.");
        if (!dryRun && after != before) {
            object["pins"] = QJsonArray::fromStringList(after);
            QString error;
            const auto bytes = QJsonDocument(object).toJson();
            if (bytes.size() > 65536) return fail("invalid_value", "The updated preferences exceed their size limit.");
            if (!FileOps::atomicWrite(path, bytes, &error, 0600)) return fail("write_unavailable", error);
            QByteArray readback;
            if (!FileOps::readFile(path, &readback) || QJsonDocument::fromJson(readback).object().value("pins") != object.value("pins")) return fail("readback_unavailable", "The pin preference write could not be confirmed.");
        }
    }
    QJsonArray entries;
    for (const auto &pin : after) { const auto *entry = catalog.find(pin); entries.append(QJsonObject{{"id", pin}, {"known", entry != nullptr}}); }
    return {{"ok", true}, {"path", path}, {"pins", QJsonArray::fromStringList(after)}, {"entries", entries},
        {"before", QJsonArray::fromStringList(before)}, {"after", QJsonArray::fromStringList(after)}, {"changed", before != after},
        {"outcome", dryRun ? "planned" : action == "list" ? "read" : before == after ? "unchanged" : "committed"},
        {"verificationScope", "Control-search-pin-preference-only"}, {"recovery", "no-undo"}};
}
