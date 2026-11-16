#include "skillintegration.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

#include "fileops.h"

namespace {
constexpr qint64 Limit = 1024 * 1024;
constexpr int MaxTargets = 32;

QJsonObject missing() { return {{"type", "missing"}}; }
QJsonObject linkTo(const QString &target) { return {{"type", "link"}, {"target", target}}; }
bool within(const QString &path, const QString &root) { return path.startsWith(root + '/'); }
bool plain(const QString &path) {
    return QDir::isAbsolutePath(path) && QDir::cleanPath(path) == path
        && !path.contains(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")));
}

// Do not follow the last component. Raw link text is part of ownership, including
// relative and dangling links. Regular files must not share an inode elsewhere.
QJsonObject describe(const QString &path) {
    struct stat st{};
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0)
        return errno == ENOENT ? missing() : QJsonObject{{"type", "unreadable"}};
    if (st.st_uid != ::getuid()) return {{"type", "unowned"}};
    if (S_ISLNK(st.st_mode)) {
        QByteArray target(65536, '\0');
        const ssize_t size = ::readlink(QFile::encodeName(path).constData(), target.data(), target.size());
        if (size < 0 || size == target.size()) return {{"type", "unreadable"}};
        target.resize(size);
        return linkTo(QFile::decodeName(target));
    }
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size > Limit) return {{"type", "conflict"}};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {{"type", "unreadable"}};
    const QByteArray bytes = file.read(Limit + 1);
    if (file.error() != QFile::NoError || bytes.size() > Limit) return {{"type", "unreadable"}};
    return {{"type", "file"}, {"sha256", FileOps::fingerprintBytes(bytes)}};
}

QByteArray resource(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QJsonObject fileState(const QByteArray &bytes) {
    return {{"type", "file"}, {"sha256", FileOps::fingerprintBytes(bytes)}};
}

class Registration {
public:
    Registration(const Env &e, const QStringList &requested) : env(e), explicitTargets(requested) {
        home = QDir::cleanPath(env.home);
        dataRoot = env.dataHome + "/omarchy-control/agent-skills";
        stateDir = env.appStateDir + "/skill-integration";
        manifestPath = stateDir + "/manifest.json";
        stock = env.omarchyPath + "/default/agents/skills/omarchy";
        payloads.insert(dataRoot + "/omarchy-control/SKILL.md", resource(":/data/skill.md"));
        payloads.insert(dataRoot + "/omarchy/SKILL.md", resource(":/data/skill-bridge.md"));
    }

    SkillIntegration::Result run(const QString &action, bool dryRun);

private:
    Env env;
    QStringList explicitTargets, roots;
    QString home, dataRoot, stateDir, manifestPath, stock, error;
    QJsonObject manifest, entries, manifestState;
    QMap<QString, QByteArray> payloads;
    QJsonArray warnings;
    bool stockAvailable = false, upstreamAware = false, changed = false;

    bool safe(const QString &path, bool includeLast = false) const;
    bool mkdir(const QString &path);
    QString resolveRoot(const QString &path) const;
    bool load();
    bool targets();
    bool save();
    bool validEntry(const QString &path, const QJsonObject &entry) const;
    bool owned(const QJsonObject &actual, const QJsonObject &entry) const;
    bool stockLink(const QString &path, const QJsonObject &actual) const;
    bool canWrite(const QString &path, const QString &role) const;
    bool replace(const QString &path, const QJsonObject &before, const QJsonObject &after, const QByteArray &bytes = {});
    bool put(const QString &path, const QString &role, const QJsonObject &after, const QByteArray &bytes = {});
    bool erase(const QString &path);
    QJsonArray rows(bool removing = false) const;
    QJsonObject report(const QString &action, bool removing = false) const;
    SkillIntegration::Result fail(const QString &code, const QString &message, const QString &action);
};

bool Registration::safe(const QString &path, bool includeLast) const {
    if (!plain(path) || !(path == home || within(path, home))) return false;
    const QString parent = includeLast ? path : QFileInfo(path).absolutePath();
    if (!(parent == home || within(parent, home))) return false;
    QString current = home;
    QStringList parts{QString()};
    if (parent != home) parts += parent.mid(home.size() + 1).split('/');
    bool absent = false;
    for (const auto &part : parts) {
        if (!part.isEmpty()) current += '/' + part;
        if (absent) continue;
        struct stat st{};
        if (::lstat(QFile::encodeName(current).constData(), &st) != 0) {
            if (errno != ENOENT) return false;
            absent = true;
        } else if (!S_ISDIR(st.st_mode) || st.st_uid != ::getuid() || (st.st_mode & 0022)) {
            return false;
        }
    }
    return true;
}

bool Registration::mkdir(const QString &path) {
    if (!safe(path, true)) { error = "Unsafe, linked, shared or unowned directory: " + path; return false; }
    if (QFileInfo::exists(path)) return true;
    if (!mkdir(QFileInfo(path).absolutePath())) return false;
    if (::mkdir(QFile::encodeName(path).constData(), 0700) != 0 && errno != EEXIST) {
        error = "Could not create directory: " + path;
        return false;
    }
    return safe(path, true);
}

// Discovery directories may intentionally alias another user-owned directory.
// Resolve only that directory, then validate every resolved parent before use.
QString Registration::resolveRoot(const QString &path) const {
    if (!plain(path) || !within(path, home)) return {};
    QString existing = path;
    QStringList suffix;
    while (!QFileInfo::exists(existing) && !QFileInfo(existing).isSymbolicLink()) {
        suffix.prepend(QFileInfo(existing).fileName());
        existing = QFileInfo(existing).absolutePath();
        if (existing == "/") return {};
    }
    const QString canonical = QFileInfo(existing).canonicalFilePath();
    if (canonical.isEmpty()) return {};
    const QString root = QDir::cleanPath(canonical + (suffix.isEmpty() ? QString() : '/' + suffix.join('/')));
    return safe(root, true) ? root : QString();
}

bool Registration::validEntry(const QString &path, const QJsonObject &entry) const {
    const QString role = entry.value("role").toString();
    if (!safe(path)) return false;
    const QString type = entry.value("original").toObject().value("type").toString();
    if (role == QLatin1String("registration")) {
        if (!within(path, home) || (QFileInfo(path).fileName() != QLatin1String("omarchy")
                                  && QFileInfo(path).fileName() != QLatin1String("omarchy-control"))) return false;
        if (type != QLatin1String("missing") && type != QLatin1String("link")) return false;
    } else if (role == QLatin1String("payload")) {
        if (!payloads.contains(path) || type != QLatin1String("missing")) return false;
    } else if (role == QLatin1String("upstream")) {
        if ((path != dataRoot + "/omarchy/.upstream" && path != dataRoot + "/omarchy/upstream")
            || type != QLatin1String("missing")) return false;
    } else return false;
    for (const QString &key : {QStringLiteral("current"), QStringLiteral("pending")}) {
        if (!entry.contains(key)) continue;
        const QJsonObject state = entry.value(key).toObject();
        const QString stateType = state.value("type").toString();
        if (stateType == QLatin1String("missing")) continue;
        if (role == QLatin1String("payload")) {
            if (stateType != QLatin1String("file") || !QRegularExpression("^[0-9a-f]{64}$").match(state.value("sha256").toString()).hasMatch()) return false;
        } else if (stateType != QLatin1String("link")) return false;
        else if (role == QLatin1String("registration") && state.value("target").toString() != dataRoot + '/' + QFileInfo(path).fileName()
                 && !(key == QLatin1String("pending") && state == entry.value("original").toObject())) return false;
    }
    return entry.contains("current");
}

bool Registration::load() {
    for (const auto &bytes : payloads)
        if (bytes.isEmpty()) { error = "A bundled skill resource is missing."; return false; }
    if (!plain(stock) || !safe(manifestPath) || !safe(dataRoot, true)) {
        error = "Integration data/state paths must be private, unlinked directories inside the user's home.";
        return false;
    }
    manifestState = describe(manifestPath);
    if (manifestState == missing()) {
        manifest = {{"version", 1}, {"apiVersion", 1}, {"home", home}, {"dataRoot", dataRoot}, {"optedOut", false}, {"targets", QJsonArray{}}};
    } else {
        if (manifestState.value("type") != QLatin1String("file")) { error = "The integration manifest is not a safe readable file."; return false; }
        QFile file(manifestPath);
        if (!file.open(QIODevice::ReadOnly)) return false;
        QJsonParseError parse;
        const QJsonDocument doc = QJsonDocument::fromJson(file.read(Limit + 1), &parse);
        manifest = doc.object();
        if (parse.error != QJsonParseError::NoError || !doc.isObject() || manifest.value("version") != 1
            || manifest.value("apiVersion") != 1 || manifest.value("home") != home || manifest.value("dataRoot") != dataRoot
            || !manifest.value("optedOut").isBool() || !manifest.value("entries").isObject() || !manifest.value("targets").isArray()
            || manifest.value("targets").toArray().size() > MaxTargets) {
            error = "The integration manifest is malformed, incompatible, or belongs to different data/home paths.";
            return false;
        }
        entries = manifest.value("entries").toObject();
        for (const auto &target : manifest.value("targets").toArray())
            if (!target.isString() || resolveRoot(target.toString()) != target.toString()) {
                error = "A configured skill directory is unsafe or has changed identity.";
                return false;
            }
        // During guide migration both the old and hidden references are
        // journaled until cleanup completes, including at the 32-target limit.
        if (entries.size() > MaxTargets * 2 + 4) { error = "The integration manifest exceeds the registration limit."; return false; }
        for (auto it = entries.begin(); it != entries.end(); ++it)
            if (!it.value().isObject() || !validEntry(it.key(), it.value().toObject())) {
                error = "The integration manifest contains an unsafe or unsupported entry.";
                return false;
            }
    }
    QFile upstream(stock + "/SKILL.md");
    if (upstream.open(QIODevice::ReadOnly)) {
        const QByteArray text = upstream.read(Limit + 1);
        stockAvailable = text.size() <= Limit && upstream.error() == QFile::NoError
            && QRegularExpression("(?m)^name: *omarchy\\s*$").match(QString::fromUtf8(text)).hasMatch()
            && !QFileInfo(stock).canonicalFilePath().startsWith(dataRoot + '/');
        upstreamAware = stockAvailable && text.contains("omarchy-control");
    }
    if (!stockAvailable) warnings.append("Stock Omarchy guidance is unavailable; only the companion can be registered. Check OMARCHY_PATH.");
    if (upstreamAware) warnings.append("The stock skill already mentions Control; forwarding is skipped. Its routing behavior has not been verified.");
    return targets();
}

bool Registration::targets() {
    QStringList candidates = explicitTargets;
    if (candidates.isEmpty()) {
        // Preserve the original entry point unless the harness was moved. The
        // caller can select unverified/custom discovery roots explicitly.
        if (env.process.value("CLAUDE_CONFIG_DIR").isEmpty()) candidates << home + "/.claude/skills";
        for (const QString relative : {".agents/skills", ".codex/skills", ".pi/agent/skills", ".gemini/config/skills", ".hermes/skills"}) {
            if (relative == QLatin1String(".codex/skills") && !env.process.value("CODEX_HOME").isEmpty()) continue;
            const QString path = home + '/' + relative;
            if (QFileInfo::exists(path) || QFileInfo(path).isSymbolicLink()) candidates << path;
        }
        const QDir profiles(home + "/.hermes/profiles");
        if (profiles.exists()) {
            const auto names = profiles.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            if (names.size() > MaxTargets) { error = "Too many Hermes profiles; select explicit --target directories."; return false; }
            for (const auto &profile : names) candidates << profile.absoluteFilePath() + "/skills";
        }
        for (auto it = entries.begin(); it != entries.end(); ++it)
            if (it.value().toObject().value("role") == QLatin1String("registration")) candidates << QFileInfo(it.key()).absolutePath();
        for (const auto &target : manifest.value("targets").toArray()) candidates << target.toString();
        if (!env.process.value("CODEX_HOME").isEmpty() || !env.process.value("CLAUDE_CONFIG_DIR").isEmpty())
            warnings.append("Agent custom-home overrides are not automatically registered. Select their discovery directories explicitly with --target.");
    }
    QSet<QString> seen;
    for (const auto &candidate : candidates) {
        const QString root = resolveRoot(candidate);
        if (root.isEmpty() || root == dataRoot || within(root, dataRoot) || within(dataRoot, root)) {
            error = "Unsafe, external, or recursive skill target: " + candidate;
            return false;
        }
        if (!seen.contains(root)) { roots << root; seen.insert(root); }
    }
    for (auto it = entries.begin(); it != entries.end(); ++it)
        if (it.value().toObject().value("role") == QLatin1String("registration")) seen.insert(QFileInfo(it.key()).absolutePath());
    for (const auto &target : manifest.value("targets").toArray()) seen.insert(target.toString());
    if (seen.size() > MaxTargets) { error = "At most 32 distinct skill target directories can be managed."; return false; }
    return true;
}

bool Registration::save() {
    if (!safe(manifestPath) || describe(manifestPath) != manifestState) {
        error = "The integration manifest changed during registration.";
        return false;
    }
    manifest["entries"] = entries;
    const QByteArray bytes = QJsonDocument(manifest).toJson();
    if (bytes.size() > Limit) { error = "The integration manifest is too large."; return false; }
    if (!replace(manifestPath, manifestState, fileState(bytes), bytes)) return false;
    manifestState = describe(manifestPath);
    return true;
}

bool Registration::owned(const QJsonObject &actual, const QJsonObject &entry) const {
    return !entry.isEmpty() && (actual == entry.value("current").toObject()
        || (entry.contains("pending") && actual == entry.value("pending").toObject()));
}

bool Registration::stockLink(const QString &path, const QJsonObject &actual) const {
    if (!stockAvailable || actual.value("type") != QLatin1String("link")) return false;
    const QString target = actual.value("target").toString();
    const QString absolute = QDir::isAbsolutePath(target) ? target : QFileInfo(path).absolutePath() + '/' + target;
    return QFileInfo(absolute).canonicalFilePath() == QFileInfo(stock).canonicalFilePath();
}

bool Registration::canWrite(const QString &path, const QString &role) const {
    if (!safe(path)) return false;
    const auto actual = describe(path);
    return actual == missing() || owned(actual, entries.value(path).toObject())
        || (role == QLatin1String("registration") && QFileInfo(path).fileName() == QLatin1String("omarchy") && stockLink(path, actual));
}

bool Registration::replace(const QString &path, const QJsonObject &before, const QJsonObject &after, const QByteArray &bytes) {
    if (!safe(path) || describe(path) != before) { error = "The registration path changed: " + path; return false; }
    const QString type = after.value("type").toString();
    if (type == QLatin1String("missing")) {
        if (before == missing()) return true;
        changed = true;
        if (::unlink(QFile::encodeName(path).constData()) != 0) { error = "Could not remove owned entry: " + path; return false; }
    } else if (type == QLatin1String("link")) {
        const QString temp = QFileInfo(path).absolutePath() + "/.oc-skill-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (::symlink(QFile::encodeName(after.value("target").toString()).constData(), QFile::encodeName(temp).constData()) != 0) {
            error = "Could not stage skill link: " + path;
            return false;
        }
        bool ok = safe(path) && describe(path) == before;
        if (ok) {
            changed = true;
            ok = ::rename(QFile::encodeName(temp).constData(), QFile::encodeName(path).constData()) == 0;
        }
        ::unlink(QFile::encodeName(temp).constData());
        if (!ok) { error = "Could not replace skill link, or its source changed: " + path; return false; }
    } else {
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
            || file.write(bytes) != bytes.size() || !safe(path) || describe(path) != before) {
            error = "Could not stage integration file: " + path;
            return false;
        }
        changed = true;
        if (!file.commit()) {
            error = "Could not atomically write integration file: " + path;
            return false;
        }
    }
    if (describe(path) != after) { error = "Integration readback did not match: " + path; return false; }
    return true;
}

bool Registration::put(const QString &path, const QString &role, const QJsonObject &after, const QByteArray &bytes) {
    if (!canWrite(path, role) || !mkdir(QFileInfo(path).absolutePath())) {
        if (error.isEmpty()) error = "Custom or modified skill entry was preserved: " + path;
        return false;
    }
    const auto before = describe(path);
    QJsonObject entry = entries.value(path).toObject();
    if (entry.isEmpty()) entry = {{"role", role}, {"original", before}, {"current", missing()}};
    else if (role == QLatin1String("registration") && stockLink(path, before)) entry["original"] = before;
    if (before == after && entry.value("current") == after && !entry.contains("pending")) return true;
    // Persist intent before replacing anything. A stopped process leaves both
    // allowable states recorded; reconcile/remove can finish without guessing.
    entry["pending"] = after;
    entries[path] = entry;
    if (!save() || !replace(path, before, after, bytes)) return false;
    changed = true;
    entry["current"] = after;
    entry.remove("pending");
    entries[path] = entry;
    return save();
}

bool Registration::erase(const QString &path) {
    QJsonObject entry = entries.value(path).toObject();
    if (entry.isEmpty()) return true;
    const auto before = describe(path);
    const auto original = entry.value("original").toObject();
    // A missing entry or an already-restored original needs no replacement.
    if (before != missing() && before != original && !owned(before, entry)) {
        error = "Modified registration or generated content was preserved: " + path;
        return false;
    }
    if (before != original && before != missing()) {
        entry["pending"] = original;
        entries[path] = entry;
        if (!save() || !replace(path, before, original)) return false;
        changed = true;
    }
    entries.remove(path);
    return save();
}

QJsonArray Registration::rows(bool removing) const {
    QJsonArray rows;
    for (const auto &root : roots) {
        for (const QString &name : {QStringLiteral("omarchy-control"), QStringLiteral("omarchy")}) {
            const QString path = root + '/' + name;
            const auto entry = entries.value(path).toObject();
            const auto actual = describe(path);
            const auto desired = linkTo(dataRoot + '/' + name);
            QString state, operation;
            if (removing) {
                if (entry.isEmpty()) { state = "unmanaged"; operation = "preserve"; }
                else if (actual == missing() || actual == entry.value("original").toObject() || owned(actual, entry)) {
                    state = "owned"; operation = "restore_original";
                } else { state = "conflict"; operation = "preserve"; }
            } else if (manifest.value("optedOut").toBool()) { state = "opted_out"; operation = "none"; }
            else if (name == QLatin1String("omarchy") && (!stockAvailable || upstreamAware)) {
                state = !stockAvailable ? "upstream_unavailable" : "upstream_reference_detected"; operation = "preserve";
            } else if (actual == desired && owned(actual, entry) && !entry.contains("pending")) { state = "registered"; operation = "none"; }
            else if (canWrite(path, "registration")) { state = entry.isEmpty() ? "unregistered" : "stale"; operation = "register"; }
            else { state = "conflict"; operation = "preserve"; }
            rows.append(QJsonObject{{"path", path}, {"name", name}, {"state", state}, {"action", operation},
                                    {"current", actual}, {"desired", desired}, {"owned", owned(actual, entry)}});
        }
    }
    return rows;
}

QJsonObject Registration::report(const QString &action, bool removing) const {
    QJsonArray artifacts;
    bool contentCurrent = true;
    for (auto it = payloads.begin(); it != payloads.end(); ++it) {
        if (!stockAvailable && it.key().contains("/omarchy/SKILL.md")) continue;
        artifacts.append(QJsonObject{{"path", it.key()}, {"state", describe(it.key())},
                                     {"writable", canWrite(it.key(), "payload")}});
        contentCurrent &= describe(it.key()) == fileState(it.value());
    }
    const QString upstreamPath = dataRoot + "/omarchy/.upstream";
    if ((stockAvailable && !upstreamAware) || entries.contains(upstreamPath))
        artifacts.append(QJsonObject{{"path", upstreamPath}, {"state", describe(upstreamPath)},
                                     {"desired", linkTo(stock)}, {"writable", canWrite(upstreamPath, "upstream")}});
    const QString legacyUpstream = dataRoot + "/omarchy/upstream";
    const auto legacyState = describe(legacyUpstream);
    if (legacyState != missing() || entries.contains(legacyUpstream))
        artifacts.append(QJsonObject{{"path", legacyUpstream}, {"state", legacyState}, {"desired", missing()},
                                     {"action", "remove_owned_visible_reference"},
                                     {"writable", legacyState == missing() || owned(legacyState, entries.value(legacyUpstream).toObject())}});
    const QJsonArray registrations = rows(removing);
    bool hasConflicts = false;
    for (const auto &value : artifacts) hasConflicts |= !value.toObject().value("writable").toBool();
    for (const auto &value : registrations) hasConflicts |= value.toObject().value("state") == QLatin1String("conflict");
    bool registrationComplete = stockAvailable && !upstreamAware && contentCurrent
        && describe(dataRoot + "/omarchy/.upstream") == linkTo(stock) && legacyState == missing();
    for (const auto &value : registrations) registrationComplete &= value.toObject().value("state") == QLatin1String("registered");
    bool packagedActivation = true;
    for (const auto &relative : {"/usr/lib/omarchy-control/agent-integration/user-recovery", "/usr/lib/omarchy-control/agent-integration/session-notify",
                                 "/usr/lib/systemd/user/omarchy-control-agent-reconcile.service", "/usr/share/libalpm/hooks/90-omarchy-control-agent-install.hook"}) {
        struct stat st{}; const QString path = env.sys(relative);
        packagedActivation &= ::lstat(QFile::encodeName(path).constData(), &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 && st.st_nlink == 1 && !(st.st_mode & 0022);
    }
    return {{"action", action}, {"registrations", registrations}, {"artifacts", artifacts},
            {"manifestPath", manifestPath}, {"dataRoot", dataRoot}, {"stockPath", stock},
            {"stockAvailable", stockAvailable}, {"upstreamReferenceDetected", upstreamAware},
            {"optedOut", manifest.value("optedOut").toBool()}, {"warnings", warnings},
            {"registrationComplete", registrationComplete},
            {"hasConflicts", hasConflicts},
            {"registrationScope", "per-user filesystem links and generated content"},
            {"provisioningBaseline", "Omarchy 4.5.0"}, {"harnessDiscoveryVerified", false},
            {"automaticActivation", packagedActivation}, {"automaticActivationScope", "root-owned package activation files present; service completion and harness loading are separate"}, {"refreshRequired", true},
            {"nextStep", "Start a fresh harness session or use its documented refresh. Inspect user-service/package-hook results separately; filesystem registration does not prove harness loading."}};
}

SkillIntegration::Result Registration::fail(const QString &code, const QString &message, const QString &action) {
    auto data = report(action, action == QLatin1String("remove") || action == QLatin1String("opt-out"));
    data["nextStep"] = "Inspect skill status/plan and the reported paths. Preserve custom content; resolve the reported conflict or state error before reconcile/remove. Pending manifest entries retain interrupted ownership intent.";
    return {data, code, message,
            changed ? QStringLiteral("partial") : QStringLiteral("blocked"), false};
}

SkillIntegration::Result Registration::run(const QString &action, bool dryRun) {
    const bool removing = action == QLatin1String("remove") || action == QLatin1String("opt-out");
    const bool readOnly = dryRun || action == QLatin1String("status") || action == QLatin1String("plan");
    if (!readOnly && (::getuid() == 0 || ::geteuid() != ::getuid()))
        return fail("user_context_required", "Run skill registration as the desktop user, without sudo.", action);
    if (!load()) return fail("integration_state_unavailable", error, action);
    if (readOnly) return {report(action, removing), {}, {}, action == QLatin1String("status") ? QStringLiteral("completed") : QStringLiteral("planned"), false};
    const QString lockType = describe(stateDir + "/registration.lock").value("type").toString();
    if (!mkdir(stateDir) || !safe(stateDir + "/registration.lock") || (lockType != QLatin1String("missing") && lockType != QLatin1String("file")))
        return fail("ownership_conflict", error.isEmpty() ? QStringLiteral("Unsafe integration lock path.") : error, action);
    QLockFile lock(stateDir + "/registration.lock");
    if (!lock.tryLock(0)) return fail("integration_busy", "Another skill registration is running, or its lock needs inspection.", action);
    // Re-read under the lock, including user edits and the opt-out preference.
    entries = {};
    roots.clear();
    warnings = {};
    if (!load()) return fail("integration_state_unavailable", error, action);
    if (action == QLatin1String("opt-out") || action == QLatin1String("opt-in")) {
        manifest["optedOut"] = action == QLatin1String("opt-out");
        if (!save()) return fail("integration_write_failed", error, action);
        changed = true;
    }
    if (!removing && manifest.value("optedOut").toBool()) return {report(action), {}, {}, "opted_out", false};
    if (!removing && roots.isEmpty()) return fail("target_required", "Select a discovery directory with --target; no default target was resolved.", action);
    if (!removing) {
        QSet<QString> configured;
        for (const auto &target : manifest.value("targets").toArray()) configured.insert(target.toString());
        for (const auto &root : roots) configured.insert(root);
        QStringList ordered = configured.values();
        ordered.sort();
        const auto selected = QJsonArray::fromStringList(ordered);
        if (manifest.value("targets") != selected) {
            manifest["targets"] = selected;
            if (!save()) return fail("integration_write_failed", error, action);
            changed = true;
        }
    }
    bool conflict = false;
    if (removing) {
        for (const auto &root : roots)
            for (const QString &name : {QStringLiteral("omarchy"), QStringLiteral("omarchy-control")})
                if (!erase(root + '/' + name)) conflict = true;
        if (action == QLatin1String("remove")) {
            QJsonArray retained;
            for (const auto &target : manifest.value("targets").toArray()) {
                const QString root = target.toString();
                if (!roots.contains(root) || entries.contains(root + "/omarchy") || entries.contains(root + "/omarchy-control")) retained.append(root);
            }
            if (manifest.value("targets") != retained) {
                manifest["targets"] = retained;
                if (!save()) return fail("integration_write_failed", error, action);
                changed = true;
            }
        }
        bool registered = false;
        for (auto it = entries.begin(); it != entries.end(); ++it)
            registered |= it.value().toObject().value("role") == QLatin1String("registration");
        // Keep fallback guidance reachable if a modified registration remains.
        if (!registered) {
            const QStringList paths = entries.keys();
            bool modified = false;
            for (const auto &path : paths) {
                const auto actual = describe(path);
                modified |= actual != missing() && !owned(actual, entries.value(path).toObject());
            }
            if (modified) { error = "Modified generated guidance was preserved together with its references."; conflict = true; }
            else for (const auto &path : paths) if (!erase(path)) conflict = true;
        }
    } else {
        for (auto it = payloads.begin(); it != payloads.end(); ++it) {
            if (it.key().contains("/omarchy/SKILL.md") && (!stockAvailable || upstreamAware)) continue;
            if (!canWrite(it.key(), "payload")) return fail("ownership_conflict", "Modified generated skill was preserved: " + it.key(), action);
        }
        const QString upstreamPath = dataRoot + "/omarchy/.upstream";
        if (stockAvailable && !upstreamAware && !canWrite(upstreamPath, "upstream"))
            return fail("ownership_conflict", "Modified upstream reference was preserved: " + upstreamPath, action);
        const QString legacyUpstream = dataRoot + "/omarchy/upstream";
        const auto legacyState = describe(legacyUpstream);
        if (legacyState != missing() && !owned(legacyState, entries.value(legacyUpstream).toObject()))
            return fail("ownership_conflict", "A custom visible upstream reference was preserved: " + legacyUpstream, action);
        // Install the hidden guide reference before publishing instructions that
        // use it. Visible nested skills are recursively discovered by Codex.
        if (stockAvailable && !upstreamAware && !put(upstreamPath, "upstream", linkTo(stock))) return fail("integration_write_failed", error, action);
        for (auto it = payloads.begin(); it != payloads.end(); ++it) {
            if (it.key().contains("/omarchy/SKILL.md") && (!stockAvailable || upstreamAware)) continue;
            if (!put(it.key(), "payload", fileState(it.value()), it.value())) return fail("integration_write_failed", error, action);
        }
        if (stockAvailable && !upstreamAware && entries.contains(legacyUpstream) && !erase(legacyUpstream))
            return fail("integration_write_failed", error, action);
        for (const auto &root : roots) {
            for (const QString &name : {QStringLiteral("omarchy-control"), QStringLiteral("omarchy")}) {
                if (name == QLatin1String("omarchy") && (!stockAvailable || upstreamAware)) continue;
                if (!put(root + '/' + name, "registration", linkTo(dataRoot + '/' + name))) conflict = true;
            }
        }
    }
    if (conflict) return fail("registration_conflict", error, action);
    if (!removing) {
        for (auto it = payloads.begin(); it != payloads.end(); ++it) {
            if (it.key().contains("/omarchy/SKILL.md") && (!stockAvailable || upstreamAware)) continue;
            if (!safe(it.key()) || describe(it.key()) != fileState(it.value()))
                return fail("verification_failed", "Generated guidance changed before final verification.", action);
        }
        for (const auto &value : rows()) {
            const auto row = value.toObject();
            if (row.value("name") == QLatin1String("omarchy") && (!stockAvailable || upstreamAware)) continue;
            if (row.value("state") != QLatin1String("registered"))
                return fail("verification_failed", "A registration changed before final verification.", action);
        }
        if (stockAvailable && !upstreamAware && describe(dataRoot + "/omarchy/.upstream") != linkTo(stock))
            return fail("verification_failed", "The upstream reference changed before final verification.", action);
    }
    return {report(action, removing), {}, {}, removing ? QStringLiteral("removed") : QStringLiteral("registered"), true};
}
}

SkillIntegration::Result SkillIntegration::run(const Env &env, const QString &action, const QStringList &targets, bool dryRun) {
    return Registration(env, targets).run(action, dryRun);
}
