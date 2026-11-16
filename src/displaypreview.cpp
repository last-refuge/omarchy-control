#include "displaypreview.h"
#include "fileops.h"
#include "hypromodule.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <sys/stat.h>
#include <unistd.h>

namespace {
QString directory(const Env &env) { return env.appStateDir + "/display-previews"; }
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
bool safeDirectory(const QString &path, bool privateMode, QString *error) {
    for (QString p = path; p != QDir::rootPath(); p = QFileInfo(p).absolutePath()) {
        const QFileInfo info(p);
        if (info.isSymLink() || (info.exists() && !info.isDir())) return fail(error, "A display preview directory has a linked or invalid parent.");
    }
    struct stat st{};
    if (::lstat(QFile::encodeName(path).constData(), &st) == 0 && (st.st_uid != geteuid() || (privateMode && (st.st_mode & 0077)))) return fail(error, "The display preview directory is not owned and private.");
    return true;
}
bool safeFile(const QString &path, QString *error) {
    struct stat st{};
    if (::lstat(QFile::encodeName(path).constData(), &st)) return fail(error, "The display preview receipt is unavailable.");
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 || (st.st_mode & 0077) || st.st_size > 1024 * 1024) return fail(error, "The display preview receipt is linked, excessive or not owned and private.");
    return true;
}
bool validRows(const QJsonValue &value) {
    if (!value.isArray() || value.toArray().isEmpty() || value.toArray().size() > 64) return false;
    QSet<QString> outputs; bool enabled = false;
    auto integer = [](const QJsonValue &v, int low, int high) { const double n = v.toDouble(qQNaN()); return v.isDouble() && std::isfinite(n) && n >= low && n <= high && n == std::floor(n); };
    for (const auto &v : value.toArray()) {
        const auto r = v.toObject(); const QString output = r.value("output").toString();
        if (!v.isObject() || !QRegularExpression("\\A[A-Za-z0-9._-]{1,128}\\z").match(output).hasMatch() || outputs.contains(output) || !r.value("enabled").isBool()
            || !integer(r.value("x"), -131072, 131072) || !integer(r.value("y"), -131072, 131072) || !integer(r.value("transform"), 0, 7)) return false;
        outputs.insert(output);
        if (r.value("enabled").toBool()) {
            enabled = true; const double scale = r.value("scale").toDouble(qQNaN());
            if (!std::isfinite(scale) || scale < .25 || scale > 8 || !QRegularExpression("\\A[1-9][0-9]{0,4}x[1-9][0-9]{0,4}@[0-9]{1,4}(?:\\.[0-9]{1,6})?\\z").match(r.value("mode").toString()).hasMatch()) return false;
        }
        const auto mirror = r.value("mirrorOf");
        if (!mirror.isUndefined() && (!mirror.isString() || (!mirror.toString().isEmpty() && !QRegularExpression("\\A[A-Za-z0-9._-]{1,128}\\z").match(mirror.toString()).hasMatch()))) return false;
        if (!r.value("vrr").isUndefined() && !r.value("vrr").isBool()) return false;
    }
    return enabled;
}
}
bool DisplayPreview::validToken(const QString &token) { return QRegularExpression("\\Adp-[a-f0-9]{32}\\z").match(token).hasMatch(); }
QString DisplayPreview::runtimeBase(const Env &env, const QString &token) { return validToken(token) ? env.runtimeDir + "/omarchy-control/display-" + token : QString(); }
bool DisplayPreview::prepare(const Env &env, QString *error) {
    for (const auto &path : {directory(env), env.runtimeDir + "/omarchy-control"}) {
        if (!safeDirectory(path, true, error)) return false;
        if (!QDir().mkpath(path) || ::chmod(QFile::encodeName(path).constData(), 0700)) return fail(error, "A private display preview directory could not be created.");
    }
    int retained = 0;
    const auto receipts = QDir(directory(env)).entryInfoList({"dp-*.json"}, QDir::Files | QDir::System | QDir::Hidden, QDir::Time);
    if (receipts.size() > 256) return fail(error, "There are too many display receipts to prune safely.");
    for (const auto &entry : receipts) {
        QString problem; const auto receipt = load(env, entry.completeBaseName(), &problem);
        if (receipt.isEmpty() || !terminal(receipt) || ++retained <= 100) continue;
        const QString base = runtimeBase(env, entry.completeBaseName());
        for (const auto &suffix : {".result", ".json", ".keep", ".revert"}) {
            const QString path = base + suffix;
            if (safeFile(path, nullptr)) QFile::remove(path);
        }
        for (const auto &suffix : {".before-module", ".before-import"}) {
            const QString path = directory(env) + "/" + entry.completeBaseName() + suffix;
            if (safeFile(path, nullptr)) QFile::remove(path);
        }
        QFile::remove(entry.absoluteFilePath());
    }
    return true;
}
bool DisplayPreview::save(const Env &env, const QJsonObject &r, QString *error) {
    const QString token = r.value("token").toString();
    if (!validToken(token) || r.value("schema") != 1 || !safeDirectory(directory(env), true, error)) return fail(error, "The display preview receipt identity or directory is invalid.");
    const QString path = directory(env) + "/" + token + ".json";
    if ((QFileInfo(path).exists() || QFileInfo(path).isSymLink()) && !safeFile(path, error)) return false;
    const auto bytes = QJsonDocument(r).toJson();
    if (bytes.size() > 1024 * 1024) return fail(error, "The display receipt exceeds its size limit.");
    return FileOps::atomicWrite(path, bytes, error, 0600);
}
QJsonObject DisplayPreview::load(const Env &env, const QString &token, QString *error) {
    if (!validToken(token) || !safeDirectory(directory(env), true, error)) { fail(error, "Use a display preview token returned on this computer."); return {}; }
    const QString path = directory(env) + "/" + token + ".json";
    if (!safeFile(path, error)) return {};
    QByteArray bytes; if (!FileOps::readFile(path, &bytes, error)) return {};
    const auto r = QJsonDocument::fromJson(bytes).object();
    if (r.value("schema") != 1 || r.value("token") != token || !r.value("session").isString() || r.value("session").toString().isEmpty()
        || !QStringList{"arming", "applying", "preview", "keeping", "kept", "reverting", "reverted", "not_started", "restore_failed", "unknown"}.contains(r.value("phase").toString())
        || !validRows(r.value("original")) || !validRows(r.value("layout")) || !r.value("fingerprints").isObject()
        || !r.value("deadline").isDouble() || !std::isfinite(r.value("deadline").toDouble()) || r.value("deadline").toDouble() < 0 || r.value("deadline").toDouble() > 1e15) {
        fail(error, "The display preview receipt is malformed."); return {};
    }
    return r;
}
bool DisplayPreview::terminal(const QJsonObject &r) { return r.value("phase") == "kept" || r.value("phase") == "reverted" || r.value("phase") == "not_started"; }
bool DisplayPreview::sameSession(const Env &env, const QJsonObject &r) { return !env.process.value("HYPRLAND_INSTANCE_SIGNATURE").isEmpty() && r.value("session").toString() == env.process.value("HYPRLAND_INSTANCE_SIGNATURE"); }
QJsonObject DisplayPreview::current(const Env &env, QString *error) {
    if (!safeDirectory(directory(env), true, error)) return {};
    const auto entries = QDir(directory(env)).entryInfoList({"dp-*.json"}, QDir::Files | QDir::System | QDir::Hidden, QDir::Time);
    if (entries.size() > 256) { fail(error, "There are too many display receipts; inspect and archive old terminal records."); return {}; }
    QJsonObject last;
    for (const auto &entry : entries) {
        const auto r = load(env, entry.completeBaseName(), error);
        if (r.isEmpty()) return {};
        if (!sameSession(env, r)) continue;
        if (!terminal(r)) return r;
        if (last.isEmpty()) last = r;
    }
    return last;
}
QString DisplayPreview::pendingReason(const Env &env, const QString &exceptToken) {
    QString error;
    if (!safeDirectory(directory(env), true, &error)) return error;
    const auto entries = QDir(directory(env)).entryInfoList({"dp-*.json"}, QDir::Files | QDir::Hidden | QDir::System, QDir::Time);
    if (entries.size() > 256) return "There are too many display receipts; inspect and archive old terminal records.";
    for (const auto &entry : entries) {
        const auto r = load(env, entry.completeBaseName(), &error);
        if (!error.isEmpty()) return error;
        if (!sameSession(env, r) || terminal(r) || r.value("token") == exceptToken) continue;
        return "Display preview " + r.value("token").toString() + " is unresolved. Inspect display preview status and keep or revert it first.";
    }
    return {};
}

bool DisplayPreview::capturePersistence(const Env &env, QJsonObject *r, const QByteArray &module, const QByteArray &import, QString *error) {
    if (!validToken(r->value("token").toString()) || module.size() > 256 * 1024 || import.size() > 256 * 1024) return fail(error, "The display persistence files exceed their capture limit.");
    HyprModule target(env); QJsonObject files;
    for (const auto &slot : {QString("module"), QString("import")}) {
        const QString path = slot == "module" ? target.path() : target.hyprlandPath();
        if (!safeDirectory(QFileInfo(path).absolutePath(), false, error)) return false;
        struct stat st{}; const bool exists = ::lstat(QFile::encodeName(path).constData(), &st) == 0;
        if (exists && (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 || st.st_size > 256 * 1024)) return fail(error, "A display persistence path is linked, unowned or oversized.");
        QByteArray bytes;
        if (exists) {
            QFile file(path); if (!file.open(QIODevice::ReadOnly)) return fail(error, "A display configuration file could not be captured.");
            bytes = file.read(256 * 1024 + 1); if (file.error() != QFileDevice::NoError || bytes.size() > 256 * 1024) return fail(error, "A display configuration file could not be read completely.");
        }
        const QString before = exists ? FileOps::fingerprintBytes(bytes) : "missing";
        if (before != r->value("fingerprints").toObject().value(path).toString()) return fail(error, "A display configuration file changed before capture.");
        const QString backup = directory(env) + "/" + r->value("token").toString() + ".before-" + slot;
        if (QFileInfo(backup).exists() || QFileInfo(backup).isSymLink()) return fail(error, "The display persistence backup already exists.");
        if (exists && !FileOps::atomicWrite(backup, bytes, error, 0600)) return false;
        files[slot] = QJsonObject{{"before", before}, {"after", FileOps::fingerprintBytes(slot == "module" ? module : import)}, {"mode", exists ? int(st.st_mode & 0777) : 0644}};
    }
    (*r)["persistence"] = files;
    return save(env, *r, error);
}

bool DisplayPreview::restorePersistence(const Env &env, const QJsonObject &r, QString *error) {
    if (!r.contains("persistence")) return true; // No persistence writer was started.
    if (!validToken(r.value("token").toString()) || !r.value("persistence").isObject()) return fail(error, "The persistence recovery record is malformed.");
    const auto files = r.value("persistence").toObject();
    if (files.size() != 2 || !files.contains("module") || !files.contains("import")) return fail(error, "The persistence recovery record is incomplete.");
    HyprModule target(env); QHash<QString, QByteArray> originals;
    // Validate both destinations and every needed backup before the first write.
    for (const auto &slot : {QString("module"), QString("import")}) {
        const auto file = files.value(slot).toObject(); const QString before = file.value("before").toString(), after = file.value("after").toString();
        const QString path = slot == "module" ? target.path() : target.hyprlandPath();
        const QRegularExpression hash("\\A[a-f0-9]{64}\\z");
        if ((before != "missing" && !hash.match(before).hasMatch()) || !hash.match(after).hasMatch() || file.value("mode").toInt(-1) < 0 || file.value("mode").toInt() > 0777
            || r.value("fingerprints").toObject().value(path) != before || !safeDirectory(QFileInfo(path).absolutePath(), false, error)) return fail(error, "The persistence recovery guards are malformed.");
        struct stat st{};
        if (::lstat(QFile::encodeName(path).constData(), &st) == 0 && (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1)) return fail(error, "A display persistence destination became linked or unowned.");
        const QString now = FileOps::fingerprint(path);
        if (now != before && now != after) return fail(error, "A display configuration file has later edits and was left alone.");
        if (before != "missing") {
            const QString backup = directory(env) + "/" + r.value("token").toString() + ".before-" + slot;
            QByteArray bytes;
            if (!safeFile(backup, error) || !FileOps::readFile(backup, &bytes, error) || FileOps::fingerprintBytes(bytes) != before) return fail(error, "A display persistence backup is missing or changed.");
            originals[slot] = bytes;
        }
    }
    for (const auto &slot : {QString("module"), QString("import")}) {
        const auto file = files.value(slot).toObject(); const QString path = slot == "module" ? target.path() : target.hyprlandPath();
        const QString now = FileOps::fingerprint(path), before = file.value("before").toString();
        if (now == before) continue;
        if (now != file.value("after").toString()) return fail(error, "A display configuration file changed during recovery.");
        if (before == "missing") { if (!QFile::remove(path)) return fail(error, "A newly created display module could not be removed."); }
        else if (!FileOps::atomicWrite(path, originals.value(slot), error) || ::chmod(QFile::encodeName(path).constData(), file.value("mode").toInt())) return fail(error, "An original display configuration file could not be restored.");
        if (FileOps::fingerprint(path) != before) return fail(error, "Display persistence recovery could not be verified.");
    }
    return true;
}
