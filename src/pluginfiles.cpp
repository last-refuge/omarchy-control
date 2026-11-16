#include "pluginfiles.h"
#include "fileops.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr qint64 Limit = 64 * 1024 * 1024;
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
QString root(const Env &env) { return env.appStateDir + "/plugin-recovery"; }
bool tokenOk(const QString &token) { return QRegularExpression("\\Apr-[a-f0-9]{32}\\z").match(token).hasMatch(); }
bool validSnapshot(const QJsonObject &snapshot, bool configuration) {
    const auto value = snapshot.value("entries");
    if (!value.isArray() || value.toArray().size() > (configuration ? 1 : 4096)) return false;
    QSet<QString> seen, directories;
    for (const auto &v : value.toArray()) {
        if (!v.isObject()) return false;
        const auto e = v.toObject(); const auto p = e.value("path"); const QString path = p.toString(), type = e.value("type").toString();
        if (!p.isString() || path.size() > 8192 || QDir::isAbsolutePath(path) || path.contains(QChar::Null) || seen.contains(path)
            || (!path.isEmpty() && (path.split('/').contains("..") || path.split('/').contains(".") || path.contains("//") || path.endsWith('/')))
            || (!path.isEmpty() && !directories.contains(path.contains('/') ? path.left(path.lastIndexOf('/')) : QString()))) return false;
        if (seen.isEmpty() && !path.isEmpty()) return false;
        if (!QStringList{"file", "directory", "link"}.contains(type) || (configuration && type != "file")) return false;
        const double mode = e.value("mode").toDouble(-1);
        if (mode < 0 || mode > 0777 || mode != int(mode)) return false;
        if (type == "directory") directories.insert(path);
        if (type == "link" && (!e.value("target").isString() || e.value("target").toString().isEmpty() || e.value("target").toString().contains(QChar::Null))) return false;
        if (type == "file" && (!QRegularExpression("\\A[a-f0-9]{64}\\z").match(e.value("hash").toString()).hasMatch() || e.value("size").toDouble(-1) < 0 || e.value("size").toDouble() > Limit)) return false;
        seen.insert(path);
    }
    return snapshot.value("fingerprint").toString() == FileOps::fingerprintBytes(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
}
bool safeParents(QString path, QString *error) {
    for (; path != QDir::rootPath(); path = QFileInfo(path).absolutePath()) {
        const QFileInfo info(path);
        if (info.isSymLink() || (info.exists() && !info.isDir())) return fail(error, "A plugin or recovery parent is linked or not a directory.");
    }
    return true;
}
bool walk(const QString &base, const QString &relative, bool owner, QJsonArray *entries, qint64 *total, int depth, QString *error) {
    if (entries->size() >= 4096 || depth > 32) return fail(error, "The plugin exceeds the 4096-entry or 32-level recovery limit.");
    const QString path = relative.isEmpty() ? base : base + "/" + relative;
    struct stat st{};
    if (::lstat(QFile::encodeName(path).constData(), &st)) {
        if (errno == ENOENT && relative.isEmpty()) return true;
        return fail(error, "The plugin tree could not be inspected completely.");
    }
    if (owner && st.st_uid != geteuid()) return fail(error, "The plugin contains content owned by another user.");
    QJsonObject entry{{"path", relative}, {"mode", int(st.st_mode & 0777)}};
    if (S_ISLNK(st.st_mode)) {
        char link[8193]; const auto size = ::readlink(QFile::encodeName(path).constData(), link, sizeof(link));
        if (size < 0 || size >= qint64(sizeof(link))) return fail(error, "A plugin link could not be captured.");
        const QByteArray bytes(link, size);
        if (QString::fromUtf8(bytes).toUtf8() != bytes) return fail(error, "A plugin link uses an unsupported filename encoding.");
        entry["type"] = "link"; entry["target"] = QString::fromUtf8(bytes);
    } else if (S_ISREG(st.st_mode)) {
        if (st.st_nlink != 1 || st.st_size < 0 || st.st_size > Limit || *total + st.st_size > Limit)
            return fail(error, "Hard-linked files or plugin trees over 64 MiB need manual recovery.");
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return fail(error, "A plugin file could not be read.");
        const auto bytes = file.read(Limit + 1);
        if (file.error() != QFileDevice::NoError || bytes.size() != st.st_size) return fail(error, "A plugin changed or could not be read completely.");
        *total += bytes.size(); entry["type"] = "file"; entry["size"] = double(bytes.size()); entry["hash"] = FileOps::fingerprintBytes(bytes);
    } else if (S_ISDIR(st.st_mode)) entry["type"] = "directory";
    else return fail(error, "Special plugin files cannot be safely captured.");
    entries->append(entry);
    if (S_ISDIR(st.st_mode)) {
        if (::access(QFile::encodeName(path).constData(), R_OK | X_OK)) return fail(error, "A plugin directory cannot be enumerated.");
        const auto children = QDir(path).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
        for (const auto &child : children) {
            const QString name = child.fileName();
            if (name.toUtf8().contains('\0') || name.contains(QChar::ReplacementCharacter)) return fail(error, "A plugin filename cannot be represented safely.");
            if (!walk(base, relative.isEmpty() ? name : relative + "/" + name, owner, entries, total, depth + 1, error)) return false;
        }
    }
    return true;
}
bool copyTree(const QString &source, const QString &destination, const QJsonObject &snapshot, bool privateFiles, QString *error) {
    if (!validSnapshot(snapshot, false)) return fail(error, "The plugin snapshot is malformed.");
    const auto entries = snapshot.value("entries").toArray();
    if (QFileInfo(destination).exists() || QFileInfo(destination).isSymLink()) return fail(error, "The recovery destination already exists.");
    for (const auto &value : entries) {
        const auto entry = value.toObject(); const QString relative = entry.value("path").toString();
        const QString to = relative.isEmpty() ? destination : destination + "/" + relative;
        const QString from = relative.isEmpty() ? source : source + "/" + relative;
        const QString type = entry.value("type").toString();
        if (type == "directory") {
            if (!QDir().mkpath(to) || ::chmod(QFile::encodeName(to).constData(), 0700)) return fail(error, "A recovery directory could not be created.");
        } else if (type == "link") {
            if (::symlink(QFile::encodeName(entry.value("target").toString()).constData(), QFile::encodeName(to).constData())) return fail(error, "A recovery link could not be created.");
        } else {
            QFile file(from);
            if (!file.open(QIODevice::ReadOnly)) return fail(error, "A recovery source could not be opened.");
            const auto bytes = file.read(Limit + 1);
            if (file.error() != QFileDevice::NoError || bytes.size() > Limit || FileOps::fingerprintBytes(bytes) != entry.value("hash").toString()
                || !FileOps::atomicWrite(to, bytes, error, privateFiles ? 0600 : entry.value("mode").toInt())) return fail(error, "A source changed during plugin capture or recovery.");
        }
    }
    // Apply permissions last so read-only directories do not stop their children.
    if (!privateFiles) for (auto it = entries.end(); it != entries.begin();) {
        const auto entry = (--it)->toObject();
        if (entry.value("type") == "link") continue;
        const QString relative = entry.value("path").toString();
        const QString path = relative.isEmpty() ? destination : destination + "/" + relative;
        if (::chmod(QFile::encodeName(path).constData(), entry.value("mode").toInt())) return fail(error, "Recovered permissions could not be set.");
    }
    return true;
}
QString config(const Env &env) { return env.configHome + "/omarchy/shell.json"; }
}

QJsonObject PluginFiles::inspect(const QString &path, bool owner, QString *error) {
    QJsonArray entries; qint64 bytes = 0;
    if (!walk(path, {}, owner, &entries, &bytes, 0, error)) return {};
    return {{"entries", entries}, {"bytes", double(bytes)}, {"fingerprint", FileOps::fingerprintBytes(QJsonDocument(entries).toJson(QJsonDocument::Compact))}};
}
bool PluginFiles::safeRoot(const Env &env, QString *error) {
    if (env.configHome != env.home + "/.config") return fail(error, "Omarchy's plugin helpers write HOME/.config; this configuration root is unsupported.");
    for (const auto &path : {env.configHome + "/omarchy/plugins", root(env)}) {
        if (!safeParents(path, error)) return false;
        const QFileInfo info(path);
        if (info.exists() && (info.ownerId() != uint(geteuid()) || !info.isReadable() || !info.isWritable() || !info.isExecutable()))
            return fail(error, "The plugin or recovery directory is not owned and accessible.");
    }
    return true;
}
bool PluginFiles::save(const Env &env, const QJsonObject &receipt, QString *error) {
    const QString token = receipt.value("token").toString();
    if (!tokenOk(token) || !safeRoot(env, error)) return false;
    if (!QDir().mkpath(root(env)) || ::chmod(QFile::encodeName(root(env)).constData(), 0700)) return fail(error, "The private recovery directory could not be created.");
    const QString path = root(env) + "/" + token + ".json";
    struct stat st{};
    if (::lstat(QFile::encodeName(path).constData(), &st) == 0 && (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 || (st.st_mode & 0077)))
        return fail(error, "The recovery receipt is linked, unowned or not private.");
    const auto bytes = QJsonDocument(receipt).toJson();
    if (bytes.size() > 4 * 1024 * 1024) return fail(error, "The recovery metadata exceeds its size limit.");
    return FileOps::atomicWrite(path, bytes, error, 0600);
}
QJsonObject PluginFiles::load(const Env &env, const QString &token, QString *error) {
    if (!tokenOk(token) || !safeRoot(env, error)) { fail(error, "Use the recovery token returned by a plugin lifecycle transaction."); return {}; }
    const QString path = root(env) + "/" + token + ".json"; const QFileInfo info(path);
    struct stat st{};
    if (info.isSymLink() || !info.isFile() || info.ownerId() != uint(geteuid()) || info.size() > 4 * 1024 * 1024
        || ::stat(QFile::encodeName(path).constData(), &st) || st.st_nlink != 1 || (st.st_mode & 0077)) { fail(error, "The recovery receipt is missing, excessive, unowned or not private."); return {}; }
    QByteArray bytes; if (!FileOps::readFile(path, &bytes, error)) return {};
    const auto object = QJsonDocument::fromJson(bytes).object(); const QString id = object.value("pluginId").toString();
    if (object.value("token") != token || object.value("schema") != 1 || !QRegularExpression("\\A[A-Za-z0-9][A-Za-z0-9._-]{0,127}\\z").match(id).hasMatch() || id.contains("..")) {
        fail(error, "The recovery receipt identity is invalid."); return {};
    }
    for (const auto &key : {QString("before"), QString("beforeConfig"), QString("after"), QString("afterConfig")}) {
        if ((object.contains(key) || key.startsWith("before")) && !validSnapshot(object.value(key).toObject(), key.endsWith("Config"))) {
            fail(error, "The recovery snapshot is malformed."); return {};
        }
    }
    return object;
}
bool PluginFiles::capture(const Env &env, const QString &token, const QString &id, QJsonObject *receipt, QString *error) {
    if (!tokenOk(token) || !QRegularExpression("\\A[A-Za-z0-9][A-Za-z0-9._-]{0,127}\\z").match(id).hasMatch() || id.contains("..") || !safeRoot(env, error)) return fail(error, "The capture identity or roots are invalid.");
    const QString target = env.configHome + "/omarchy/plugins/" + id;
    const auto before = inspect(target, true, error), beforeConfig = inspect(config(env), true, error);
    if (before.isEmpty() || !validSnapshot(beforeConfig, true)) return fail(error, "The plugin or shell configuration could not be captured safely.");
    *receipt = {{"schema", 1}, {"token", token}, {"pluginId", id}, {"phase", "capturing"}, {"before", before}, {"beforeConfig", beforeConfig}};
    if (!save(env, *receipt, error)) return false;
    const QString backup = root(env) + "/" + token;
    if (QFileInfo(backup).exists() || QFileInfo(backup).isSymLink() || !QDir().mkdir(backup) || ::chmod(QFile::encodeName(backup).constData(), 0700)) return fail(error, "The recovery backup directory already exists or could not be created.");
    if (!copyTree(target, backup + "/tree", before, true, error) || !copyTree(config(env), backup + "/config", beforeConfig, true, error)) return false;
    if (inspect(target, true, error) != before || inspect(config(env), true, error) != beforeConfig) return fail(error, "Plugin files changed during capture; no helper was run.");
    (*receipt)["phase"] = "applying";
    return save(env, *receipt, error);
}
bool PluginFiles::finish(const Env &env, QJsonObject *receipt, bool writerOk, QString *error) {
    const auto after = inspect(env.configHome + "/omarchy/plugins/" + receipt->value("pluginId").toString(), true, error);
    const auto afterConfig = inspect(config(env), true, error);
    (*receipt)["writerAcknowledged"] = writerOk; (*receipt)["phase"] = "unverified";
    if (after.isEmpty() || !validSnapshot(afterConfig, true)) { save(env, *receipt, nullptr); return fail(error, "The post-write files could not be captured safely; retain the original snapshot for review."); }
    (*receipt)["after"] = after; (*receipt)["afterConfig"] = afterConfig;
    return save(env, *receipt, error);
}
bool PluginFiles::canRecover(const Env &env, const QJsonObject &r, QString *error) {
    for (const auto &key : {QString("before"), QString("beforeConfig"), QString("after"), QString("afterConfig")})
        if (!validSnapshot(r.value(key).toObject(), key.endsWith("Config"))) return fail(error, "The recovery snapshot is malformed or incomplete.");
    if (!safeRoot(env, error) || r.value("after").toObject().isEmpty() || r.value("afterConfig").toObject().isEmpty()) return fail(error, "The interrupted helper has no confirmed post-write snapshot; retain the backup for manual review.");
    const auto tree = inspect(env.configHome + "/omarchy/plugins/" + r.value("pluginId").toString(), true, error), conf = inspect(config(env), true, error);
    const bool resumed = r.value("phase") == "recovering" || r.value("phase") == "recovered";
    if ((tree != r.value("after").toObject() && !(resumed && tree == r.value("before").toObject()))
        || (conf != r.value("afterConfig").toObject() && !(resumed && conf == r.value("beforeConfig").toObject())))
        return fail(error, "The plugin or shell configuration changed after this action; recovery would overwrite later work.");
    return true;
}
bool PluginFiles::recover(const Env &env, QJsonObject *r, QString *error) {
    if (!canRecover(env, *r, error)) return false;
    (*r)["phase"] = "recovering"; if (!save(env, *r, error)) return false;
    const QString token = r->value("token").toString(), target = env.configHome + "/omarchy/plugins/" + r->value("pluginId").toString();
    const QString backup = root(env) + "/" + token;
    // Backups keep private modes; their recorded content and link identities must
    // still match before any destination is displaced.
    auto contentMatches = [](QJsonObject stored, QJsonObject observed) {
        auto normalize = [](QJsonObject value) { QJsonArray entries; for (const auto &v : value.value("entries").toArray()) { auto e = v.toObject(); e.remove("mode"); entries.append(e); } return entries; };
        return !observed.isEmpty() && normalize(stored) == normalize(observed);
    };
    for (const auto &pair : {qMakePair(QString("tree"), QString("before")), qMakePair(QString("config"), QString("beforeConfig"))}) {
        if (!safeParents(backup, error) || !contentMatches(r->value(pair.second).toObject(), inspect(backup + "/" + pair.first, true, error))) return fail(error, "The recovery backup changed or is unavailable.");
    }
    const auto before = r->value("before").toObject();
    if (inspect(target, true, error) != before) {
        const QString stage = QFileInfo(target).absolutePath() + "/." + QFileInfo(target).fileName() + ".control-stage-" + token;
        const QString displaced = QFileInfo(target).absolutePath() + "/." + QFileInfo(target).fileName() + ".control-displaced-" + token;
        if (!copyTree(backup + "/tree", stage, before, false, error)) return false;
        if ((QFileInfo(target).exists() || QFileInfo(target).isSymLink()) && !QDir().rename(target, displaced)) return fail(error, "The current plugin could not be preserved beside its original path.");
        if (!before.value("entries").toArray().isEmpty() && !QDir().rename(stage, target)) return fail(error, "The captured plugin could not be restored; staged and displaced content were retained.");
    }
    if (inspect(config(env), true, error) != r->value("beforeConfig").toObject()) {
        const auto entries = r->value("beforeConfig").toObject().value("entries").toArray();
        if (entries.isEmpty()) { if (!QFile::remove(config(env))) return fail(error, "The original absence of shell.json could not be restored."); }
        else {
            QByteArray bytes;
            if (!FileOps::readFile(backup + "/config", &bytes, error) || !FileOps::atomicWrite(config(env), bytes, error)) return false;
            if (::chmod(QFile::encodeName(config(env)).constData(), entries.first().toObject().value("mode").toInt())) return fail(error, "The shell configuration permissions could not be restored.");
        }
    }
    if (inspect(target, true, error) != before || inspect(config(env), true, error) != r->value("beforeConfig").toObject()) return fail(error, "Recovered files could not be verified.");
    (*r)["phase"] = "recovered"; return save(env, *r, error);
}
