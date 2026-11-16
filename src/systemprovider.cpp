#include "systemprovider.h"

#include "iconthemes.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QSet>
#include <QPointer>
#include <QSysInfo>
#include <QTemporaryFile>
#include <unistd.h>
#include <algorithm>

#include "fileops.h"
#include "compat.h"
#include <QTemporaryDir>

SystemProvider::SystemProvider(const Catalog *catalog, CommandRunner *runner, Capabilities *capabilities, ChangeCoordinator *coordinator, QObject *parent)
    : QObject(parent), m_catalog(catalog), m_runner(runner), m_capabilities(capabilities), m_coordinator(coordinator) {}

QVariantList SystemProvider::parseDf(const QByteArray &text, QString *error) {
    if (error) error->clear();
    QVariantList rows;
    const auto lines = QString::fromUtf8(text).split('\n', Qt::SkipEmptyParts);
    static const QStringList ignored{"tmpfs", "devtmpfs", "efivarfs", "overlay", "squashfs", "proc", "sysfs", "ramfs", "fuse.portal"};
    static const QRegularExpression record(R"(^\s*(.+?)\s+(\S+)\s+(-|[0-9]+)\s+(-|[0-9]+)\s+(-|-?[0-9]+)\s+(-|[0-9]+%)\s+(.+?)\s*$)");
    QHash<QString, QString> firstMount;
    bool parsed = false;
    for (int i = 1; i < lines.size(); ++i) {
        const auto match = record.match(lines[i]);
        if (!match.hasMatch()) {
            if (error) *error = "A storage row has an unsupported format.";
            continue;
        }
        parsed = true;
        const QString device = match.captured(1), fstype = match.captured(2), mount = match.captured(7);
        if (ignored.contains(fstype)) continue;
        QVariantMap row{{"device", device}, {"fstype", fstype}, {"mount", mount}};
        bool sizeKnown = false, usedKnown = false, availableKnown = false, percentKnown = false;
        const double size = match.captured(3).toDouble(&sizeKnown), used = match.captured(4).toDouble(&usedKnown), available = match.captured(5).toDouble(&availableKnown);
        QString percentText = match.captured(6); percentText.remove('%');
        const int percent = percentText.toInt(&percentKnown);
        row["size"] = sizeKnown ? QVariant(size) : QVariant();
        row["used"] = usedKnown ? QVariant(used) : QVariant();
        row["available"] = availableKnown ? QVariant(available) : QVariant();
        row["percent"] = percentKnown ? QVariant(percent) : QVariant();
        row["known"] = sizeKnown && size > 0 && usedKnown && availableKnown && percentKnown;
        row["temporary"] = (fstype.startsWith("fuse.") && (fstype.endsWith(".AppImage", Qt::CaseInsensitive) || device.endsWith(".AppImage", Qt::CaseInsensitive))) || mount.startsWith("/tmp/.mount_");
        if (!row["temporary"].toBool() && device.startsWith('/')) {
            if (firstMount.contains(device)) row["sharedWith"] = firstMount.value(device);
            else firstMount.insert(device, mount);
        }
        rows << row;
    }
    if (!parsed && error) *error = "Storage output did not contain readable rows.";
    return rows;
}

static QString readLine(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readLine(4096)).trimmed() : QString();
}

static bool traversableParents(QString path) {
    for (;;) {
        const QFileInfo parent(QFileInfo(path).absolutePath());
        if ((parent.exists() && (!parent.isDir() || !parent.isExecutable())) || (parent.isSymLink() && !parent.exists())) return false;
        if (parent.absoluteFilePath() == path) return true;
        path = parent.absoluteFilePath();
    }
}

void SystemProvider::refresh() { refreshFacts("status"); }

void SystemProvider::refreshFacts(const QString &section) {
    const int generation = ++m_factsGeneration;
    const Env &env = m_runner->env();
    const auto wants = [&](const QString &name) { return section == QLatin1String("status") || section == name; };
    auto remaining = std::make_shared<int>(1);
    const auto finished = [this, remaining, generation] {
        if (--*remaining == 0 && generation == m_factsGeneration) { emit changed(); emit factsRefreshed(); }
    };
    const auto state = [this](const QString &name, const QString &status, const QString &reason, const QString &source) {
        m_factStates[name] = QVariantMap{{"status", status}, {"reason", reason}, {"source", source}};
    };
    if (wants("machine")) {
        m_machine = {{"hostname", QSysInfo::machineHostName()}, {"kernel", QSysInfo::kernelVersion()}, {"architecture", QSysInfo::currentCpuArchitecture()},
                     {"model", readLine(env.sys("/sys/class/dmi/id/product_name"))}, {"vendor", readLine(env.sys("/sys/class/dmi/id/sys_vendor"))},
                     {"session", env.process.value("XDG_SESSION_TYPE")}};
        state("machine", "loading", {}, "kernel, DMI, session environment and service version queries");
        ++*remaining;
        const QPointer<SystemProvider> guard(this);
        m_capabilities->scanServices([this, guard, generation, state, finished] {
            if (!guard) return;
            if (generation == m_factsGeneration) {
                const auto versions = m_capabilities->versions();
                m_machine["omarchy"] = versions.value("omarchy"); m_machine["hyprland"] = versions.value("hyprland");
                bool complete = true;
                for (const QString &key : {QStringLiteral("model"), QStringLiteral("vendor"), QStringLiteral("session"), QStringLiteral("omarchy"), QStringLiteral("hyprland")}) {
                    const bool known = !m_machine.value(key).toString().isEmpty();
                    m_machine[key + "Known"] = known; complete &= known;
                }
                state("machine", complete ? "ok" : "partial", complete ? QString() : "Some platform, session or desktop-version facts are unavailable.", "kernel, DMI, session environment and service version queries");
            }
            finished();
        });
    }
    if (wants("battery")) {
        const QString path = env.sys("/sys/class/power_supply");
        const QFileInfo root(path);
        QVariantList devices;
        bool presenceKnown = root.isDir() && root.isReadable() && root.isExecutable(), complete = presenceKnown;
        if (presenceKnown) {
            for (const auto &name : QDir(path).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QString base = path + "/" + name, type = readLine(base + "/type");
                if (type.isEmpty()) { presenceKnown = false; complete = false; continue; }
                if (type != QLatin1String("Battery")) continue;
                bool known = false, cyclesKnown = false;
                const int percent = readLine(base + "/capacity").toInt(&known), cycles = readLine(base + "/cycle_count").toInt(&cyclesKnown);
                known &= percent >= 0 && percent <= 100; cyclesKnown &= cycles >= 0;
                const QString status = readLine(base + "/status");
                QVariantMap device{{"name", name}, {"path", base}, {"present", true}, {"percent", known ? percent : -1}, {"capacityKnown", known},
                                   {"status", status}, {"statusKnown", !status.isEmpty()}, {"cyclesKnown", cyclesKnown}};
                if (cyclesKnown) device["cycles"] = cycles;
                devices << device;
                complete &= known && !status.isEmpty();
            }
        }
        m_battery = devices.isEmpty() ? QVariantMap{} : devices.first().toMap();
        m_battery["devices"] = devices; m_battery["presenceKnown"] = presenceKnown || !devices.isEmpty();
        if (presenceKnown || !devices.isEmpty()) m_battery["present"] = !devices.isEmpty();
        state("battery", complete ? "ok" : !devices.isEmpty() ? "partial" : "unavailable", complete ? QString() : "Battery presence or readings could not be read completely.", path);
    }
    if (wants("hooks")) {
        const QString path = env.configHome + "/omarchy/hooks";
        const QFileInfo root(path);
        bool complete = traversableParents(path) && ((!root.exists() && !root.isSymLink()) || (root.isDir() && root.isReadable() && root.isExecutable()));
        m_hooks.clear(); m_hookInventory.clear();
        if (complete && root.exists()) {
            QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QFileInfo file(it.next());
                if (file.isDir() && !file.isSymLink()) { complete &= file.isReadable() && file.isExecutable(); continue; }
                const bool active = file.isFile() && file.isExecutable() && !file.fileName().endsWith(".sample");
                const QVariantMap row{{"name", QDir(path).relativeFilePath(file.absoluteFilePath())}, {"path", file.absoluteFilePath()},
                                      {"active", active}, {"sample", file.fileName().endsWith(".sample")}, {"executable", file.isExecutable()},
                                      {"symlink", file.isSymLink()}, {"target", file.isSymLink() ? file.symLinkTarget() : QString()}};
                m_hookInventory << row;
                if (active) m_hooks << row;
                complete &= !file.isDir() && (!file.isSymLink() || file.exists());
                if (m_hookInventory.size() >= 4096) { complete = false; break; }
            }
        }
        state("hooks", complete ? "ok" : m_hookInventory.isEmpty() ? "unavailable" : "partial", complete ? QString() : "Some hook directories or links could not be enumerated. Linked directories are not traversed.", path);
    }
    if (wants("storage")) {
        state("storage", "loading", {}, "df --output in bytes; pseudo filesystems excluded");
        ++*remaining;
        m_runner->run({"env", "LC_ALL=C", "df", "-B1", "--output=source,fstype,size,used,avail,pcent,target"}, CommandOptions{4000}, this,
                      [this, generation, state, finished](const CommandResult &result) {
            if (generation == m_factsGeneration) {
                QString error;
                m_storage = result.ok() && !result.truncated ? parseDf(result.out, &error) : QVariantList{};
                for (const auto &row : m_storage)
                    if (!row.toMap().value("known").toBool() && error.isEmpty()) error = "Some filesystem sizes or usage values are unavailable.";
                const QString status = !result.ok() || result.truncated || (!error.isEmpty() && m_storage.isEmpty()) ? "unavailable" : !error.isEmpty() ? "partial" : "ok";
                state("storage", status, status == QLatin1String("ok") ? QString() : error.isEmpty() ? "Storage command failed or returned truncated output." : error, "df --output in bytes; pseudo filesystems excluded");
            }
            finished();
        });
    }
    emit changed(); finished();
}

QJsonObject SystemProvider::facts(const QString &section) const {
    QJsonObject result, states;
    for (const QString &name : {QStringLiteral("machine"), QStringLiteral("battery"), QStringLiteral("storage"), QStringLiteral("hooks")}) {
        if (section != QLatin1String("status") && section != name) continue;
        const auto state = m_factStates.value(name).toMap();
        states[name] = QJsonObject::fromVariantMap(state);
        if (name == QLatin1String("machine")) result[name] = QJsonObject::fromVariantMap(m_machine);
        else if (name == QLatin1String("battery")) result[name] = QJsonObject::fromVariantMap(m_battery);
        else result[name] = QJsonArray::fromVariantList(name == QLatin1String("storage") ? m_storage : m_hookInventory);
    }
    result["states"] = states; result["scope"] = section;
    return result;
}

bool SystemProvider::isSafeMime(const QString &mime) {
    static const QRegularExpression safe(R"(^[a-z0-9][a-z0-9!#$&^_.+-]{0,126}/[A-Za-z0-9][A-Za-z0-9!#$&^_.+-]{0,126}$)");
    const auto match = safe.match(mime);
    return match.hasMatch() && match.capturedLength() == mime.size();
}

bool SystemProvider::isSafeDesktopId(const QString &id) {
    static const QRegularExpression safe(R"(^[A-Za-z0-9_][A-Za-z0-9_.+-]*\.desktop$)");
    const auto match = safe.match(id);
    return match.hasMatch() && match.capturedLength() == id.size();
}

static QStringList appRoots(const Env &env) {
    QStringList roots{env.dataHome + "/applications"};
    for (const QString &root : env.dataDirs)
        roots << root + "/applications";
    roots.removeDuplicates();
    return roots;
}

QVariantList SystemProvider::readApps(const Env &env, QString *error) {
    if (error) error->clear();
    QVariantList rows;
    QSet<QString> seen;
    for (const QString &root : appRoots(env)) {
        const QFileInfo directory(root);
        if (!directory.exists() && !directory.isSymLink())
            continue;
        if (!directory.isDir() || !directory.isReadable() || !directory.isExecutable()) {
            if (error) *error = "An application directory could not be read.";
            break;
        }
        QHash<QString, QStringList> paths;
        QDirIterator it(root, {"*.desktop"}, QDir::Files | QDir::System, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            paths[QDir(root).relativeFilePath(path).replace('/', '-')] << path;
        }
        QStringList ids = paths.keys();
        ids.sort();
        for (const QString &id : ids) {
            if (seen.contains(id))
                continue;
            // A higher-priority tombstone, unreadable entry or invalid application
            // still masks the same ID below it.
            seen.insert(id);
            const QString path = paths.value(id).first();
            QByteArray contents;
            const bool readable = QFileInfo(path).isFile() && QFileInfo(path).size() <= 512 * 1024 && FileOps::readFile(path, &contents);
            bool ambiguous = paths.value(id).size() != 1;
            int groups = 0;
            for (const auto &line : contents.split('\n')) groups += line.trimmed() == "[Desktop Entry]" ? 1 : 0;
            ambiguous |= groups > 1;
            auto value = [&](const QString &key) {
                const auto found = FileOps::iniGet(contents, "Desktop Entry", key);
                ambiguous |= found.ambiguous;
                return found.value;
            };
            const QString type = value("Type"), name = value("Name"), comment = value("Comment"), mimes = value("MimeType"), icon = value("Icon");
            const QString hidden = value("Hidden"), noDisplay = value("NoDisplay"), tryExec = value("TryExec");
            const bool deleted = hidden == QLatin1String("true");
            if (readable && !ambiguous && !deleted && (type != QLatin1String("Application") || name.isEmpty()))
                continue;
            QString status = !readable ? "read_unavailable" : ambiguous ? "ambiguous_target" : deleted ? "hidden"
                : type != QLatin1String("Application") || name.isEmpty() ? "unsupported_configuration" : "ok";
            if ((!hidden.isEmpty() && hidden != QLatin1String("true") && hidden != QLatin1String("false"))
                || (!noDisplay.isEmpty() && noDisplay != QLatin1String("true") && noDisplay != QLatin1String("false")))
                status = "unsupported_configuration";
            if (status == QLatin1String("ok") && !tryExec.isEmpty() && env.findExecutable(tryExec).isEmpty())
                status = "executable_unavailable";
            rows << QVariantMap{{"id", id}, {"name", name.isEmpty() ? id : name}, {"comment", comment}, {"mimes", mimes},
                                {"hidden", deleted || noDisplay == QLatin1String("true") || status != QLatin1String("ok")},
                                {"deleted", deleted}, {"noDisplay", noDisplay == QLatin1String("true")}, {"iconName", icon},
                                {"path", path}, {"source", root == env.dataHome + "/applications" ? "user" : "system"},
                                {"fingerprint", readable ? FileOps::fingerprintBytes(contents) : QStringLiteral("unreadable")},
                                {"status", status}, {"available", status == QLatin1String("ok")}, {"tryExec", tryExec}};
        }
    }
    std::sort(rows.begin(), rows.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("name").toString().localeAwareCompare(b.toMap().value("name").toString()) < 0;
    });
    return rows;
}

// The everyday file types people change. xdg-mime stays authoritative for the
// rest can be queried or changed through the CLI using the same provider.
static const QList<QPair<QString, const char *>> &commonMimes() {
    static const QList<QPair<QString, const char *>> mimes{
        {"text/plain", QT_TRANSLATE_NOOP("SystemProvider", "Text files")},
        {"application/pdf", QT_TRANSLATE_NOOP("SystemProvider", "PDF documents")},
        {"image/png", QT_TRANSLATE_NOOP("SystemProvider", "Images")},
        {"video/mp4", QT_TRANSLATE_NOOP("SystemProvider", "Videos")},
        {"audio/mpeg", QT_TRANSLATE_NOOP("SystemProvider", "Music")},
        {"inode/directory", QT_TRANSLATE_NOOP("SystemProvider", "Folders")},
        {"x-scheme-handler/mailto", QT_TRANSLATE_NOOP("SystemProvider", "Email links")},
    };
    return mimes;
}

QStringList SystemProvider::commonMimeTypes() {
    QStringList types;
    for (const auto &[mime, label] : commonMimes())
        types << mime;
    return types;
}

namespace {
QString mimeConfigHome(const Env &env) {
    const QString configured = env.process.value("XDG_CONFIG_HOME");
    return QDir::isAbsolutePath(configured) ? QDir::cleanPath(configured) : env.configHome;
}

QStringList mimeDesktops(const Env &env, QString *error = nullptr) {
    QStringList desktops;
    static const QRegularExpression safe("^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$");
    for (const QString &name : env.process.value("XDG_CURRENT_DESKTOP").split(':', Qt::SkipEmptyParts)) {
        const auto match = safe.match(name);
        if (!match.hasMatch() || match.capturedLength() != name.size() || desktops.size() >= 16) {
            if (error) *error = "The current desktop names cannot be mapped safely to MIME configuration files.";
            continue;
        }
        if (!desktops.contains(name.toLower())) desktops << name.toLower();
    }
    return desktops;
}

QStringList mimeConfigDirs(const Env &env) {
    QStringList dirs;
    for (const QString &path : env.process.value("XDG_CONFIG_DIRS").split(':', Qt::SkipEmptyParts))
        if (QDir::isAbsolutePath(path)) dirs << QDir::cleanPath(path);
    if (dirs.isEmpty()) dirs << env.sys("/etc/xdg");
    dirs.removeDuplicates();
    return dirs;
}

QStringList mimeFiles(const Env &env, bool userOnly = false) {
    QStringList roots{mimeConfigHome(env)};
    if (!userOnly) {
        roots += mimeConfigDirs(env);
        roots << env.dataHome + "/applications";
        for (const QString &root : env.dataDirs) roots << root + "/applications";
    }
    QStringList files;
    for (const QString &root : roots) {
        for (const QString &desktop : mimeDesktops(env)) files << root + "/" + desktop + "-mimeapps.list";
        files << root + "/mimeapps.list";
    }
    files.removeDuplicates();
    return files;
}

bool readMimeFile(const QString &path, QByteArray *contents, QString *error) {
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) { contents->clear(); return true; }
    if (!info.isFile() || info.size() > 1024 * 1024 || !FileOps::readFile(path, contents)) {
        *error = "A user MIME configuration file could not be read.";
        return false;
    }
    QSet<QByteArray> groups;
    bool inGroup = false;
    for (const auto &raw : contents->split('\n')) {
        const auto line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (line.startsWith('[') && line.endsWith(']')) {
            const auto group = line.mid(1, line.size() - 2).trimmed();
            if (groups.contains(group)) { *error = "A MIME configuration group appears more than once."; return false; }
            groups.insert(group); inGroup = true;
        } else if (!inGroup || line.indexOf('=') <= 0) {
            *error = "The MIME configuration has an unsupported line outside a key/value group.";
            return false;
        }
    }
    return true;
}

bool setMimeDefault(QByteArray *contents, const QString &mime, const QString &id, QString *error) {
    const auto current = FileOps::iniGet(*contents, "Default Applications", mime);
    if (current.ambiguous) { *error = "This MIME default appears more than once."; return false; }
    QStringList defaults{id};
    for (const QString &fallback : current.value.split(';', Qt::SkipEmptyParts)) {
        if (!SystemProvider::isSafeDesktopId(fallback)) { *error = "The existing MIME fallback list has an unsupported desktop ID."; return false; }
        if (!defaults.contains(fallback)) defaults << fallback;
    }
    return FileOps::iniUpsert(contents, "Default Applications", mime, defaults.join(';') + ";", error);
}

QVariantMap appById(const QVariantList &apps, const QString &id) {
    for (const auto &app : apps) if (app.toMap().value("id").toString() == id) return app.toMap();
    return {};
}

QJsonObject associationRead(const Env &env, const QString &mime, const QVariantList &apps, const QString &appError, const CommandResult &result) {
    const QString current = result.outText();
    const bool readable = result.ok() && !result.truncated && (current.isEmpty() || SystemProvider::isSafeDesktopId(current));
    QJsonArray candidates;
    for (const auto &value : apps) {
        const auto app = value.toMap();
        if (app.value("available").toBool() && app.value("mimes").toString().split(';').contains(mime))
            candidates << QJsonObject{{"value", app.value("id").toString()}, {"label", app.value("name").toString()}, {"icon", app.value("icon").toString()}};
    }
    const auto handler = appById(apps, current);
    return {{"ok", readable}, {"mime", mime}, {"status", !readable ? "read_unavailable" : current.isEmpty() ? "unresolved" : "ok"},
            {"current", readable ? current : QString()}, {"currentLabel", handler.value("name", current).toString()},
            {"currentIcon", handler.value("icon").toString()}, {"currentInstalled", !handler.isEmpty() && handler.value("available").toBool()},
            {"candidates", candidates}, {"candidatesComplete", false}, {"candidateBasis", "declared-MimeType-only"},
            {"appsStatus", appError.isEmpty() ? "ok" : "read_unavailable"}, {"lookupFiles", QJsonArray::fromStringList(mimeFiles(env))},
            {"source", "xdg-mime-query"}, {"reason", readable ? QString() : QStringLiteral("The effective MIME handler could not be read successfully.")}};
}
}

void SystemProvider::readAssociation(const QString &mime, std::function<void(QJsonObject)> done) {
    if (!isSafeMime(mime)) {
        done({{"ok", false}, {"status", "invalid_value"}, {"reason", tr("That file association is not valid.")}});
        return;
    }
    QString appError;
    const auto apps = readApps(m_runner->env(), &appError);
    m_runner->run({"xdg-mime", "query", "default", mime}, CommandOptions{5000}, this, [this, mime, apps, appError, done](const CommandResult &result) {
        done(associationRead(m_runner->env(), mime, apps, appError, result));
    });
}

void SystemProvider::refreshApps() {
    const int generation = ++m_appsGeneration;
    const Env &env = m_runner->env();
    QString appError;
    m_apps = readApps(env, &appError);
    m_appsStatus = appError.isEmpty() ? "ok" : "read_unavailable";
    const QString theme = IconThemes::desktopDefault(env);
    for (auto &value : m_apps) {
        auto app = value.toMap();
        const QString name = app.value("iconName").toString();
        if (!m_iconUrls.contains(name)) m_iconUrls.insert(name, IconThemes::appIconUrl(env, theme, {name}));
        app["icon"] = m_iconUrls.value(name);
        value = app;
    }
    emit appsChanged();
    auto results = std::make_shared<QHash<QString, QJsonObject>>();
    auto remaining = std::make_shared<int>(commonMimes().size());
    const auto apps = m_apps;
    for (const auto &[mime, label] : commonMimes()) {
        m_runner->run({"xdg-mime", "query", "default", mime}, CommandOptions{5000}, this,
                      [this, generation, mime, apps, appError, results, remaining](const CommandResult &result) {
            if (generation != m_appsGeneration) return;
            results->insert(mime, associationRead(m_runner->env(), mime, apps, appError, result));
            if (--*remaining > 0) return;
            m_associations.clear();
            for (const auto &[m, text] : commonMimes()) {
                auto row = results->value(m).toVariantMap();
                row["label"] = QCoreApplication::translate("SystemProvider", text);
                m_associations << row;
            }
            emit appsChanged();
            emit appsRefreshed();
        });
    }
}

ProviderPlan SystemProvider::planLaunch(const QString &desktopId) const {
    ProviderPlan p;
    p.operation = "apps.launch"; p.target = desktopId; p.after = "launch-requested";
    const auto fail = [&](const QString &code, const QString &error) { p.errorCode = code; p.error = error; return p; };
    if (!isSafeDesktopId(desktopId)) return fail("invalid_value", "Use a desktop ID returned by apps list.");
    QString error;
    const auto app = appById(readApps(m_runner->env(), &error), desktopId);
    if (!error.isEmpty()) return fail("read_unavailable", error);
    if (app.isEmpty()) return fail("stale_target", "That application is no longer installed.");
    if (!app.value("available").toBool()) return fail("unavailable", "That application is hidden, ambiguous, unreadable or unavailable.");
    const QString launcher = !m_runner->env().findExecutable("uwsm-app").isEmpty() ? "uwsm-app" : "gtk-launch";
    if (m_runner->env().findExecutable(launcher).isEmpty()) return fail("unavailable", "No supported desktop application launcher is installed.");
    p.details = {{"path", app.value("path").toString()}, {"fingerprint", app.value("fingerprint").toString()}, {"launcher", launcher}};
    p.effects << "Asks the desktop launcher to open this installed app. A successful handoff does not verify that a window appeared or the app stayed running.";
    p.ok = true;
    return p;
}

bool SystemProvider::launchInstalledApp(const QString &desktopId, QString *error) {
    const auto p = planLaunch(desktopId);
    if (!p.ok) { if (error) *error = p.error; return false; }
    const QString launcher = p.details.value("launcher").toString();
    const QStringList argv = launcher == QLatin1String("uwsm-app") ? QStringList{launcher, "--", desktopId} : QStringList{launcher, desktopId.chopped(8)};
    return m_runner->launchDetached(argv, error);
}

void SystemProvider::launchApp(const QString &desktopId) {
    QString error;
    if (!launchInstalledApp(desktopId, &error)) emit message(error, true);
}

void SystemProvider::planAssociation(const QString &mime, const QString &desktopId, std::function<void(ProviderPlan)> done) {
    ProviderPlan p;
    p.operation = "mime.set"; p.target = mime; p.after = desktopId;
    const auto fail = [&](const QString &code, const QString &error) { p.errorCode = code; p.error = error; done(p); };
    if (!isSafeMime(mime) || !isSafeDesktopId(desktopId)) { fail("invalid_value", tr("That file association is not valid.")); return; }
    const Env &env = m_runner->env();
    QString error;
    const auto app = appById(readApps(env, &error), desktopId);
    if (!error.isEmpty()) { fail("read_unavailable", error); return; }
    if (app.isEmpty()) { fail("stale_target", "That application is no longer installed."); return; }
    if (!app.value("available").toBool()) { fail("unavailable", "That application is hidden, ambiguous, unreadable or unavailable."); return; }
    mimeDesktops(env, &error);
    if (!error.isEmpty()) { fail("unsupported_configuration", error); return; }
    QString selected = mimeConfigHome(env) + "/mimeapps.list";
    QJsonObject fingerprints;
    for (const QString &file : mimeFiles(env, true)) {
        QByteArray bytes;
        if (!readMimeFile(file, &bytes, &error)) { fail("read_unavailable", error); return; }
        const auto current = FileOps::iniGet(bytes, "Default Applications", mime);
        if (current.ambiguous) { fail("unsupported_configuration", "This MIME default appears more than once in a user file."); return; }
        fingerprints[file] = QFileInfo(file).isSymLink() ? "symlink:" + QFileInfo(file).symLinkTarget() : FileOps::fingerprintBytes(bytes);
        if (current.found) { selected = file; break; }
    }
    if (QFileInfo(selected).isSymLink()) { fail("ownership_conflict", "The MIME user override is a link. Copy it to a regular user file before changing it here."); return; }
    const QString configCanonical = QFileInfo(mimeConfigHome(env)).canonicalFilePath();
    for (const QString &system : mimeConfigDirs(env))
        if (!configCanonical.isEmpty() && configCanonical == QFileInfo(system).canonicalFilePath()) {
            fail("ownership_conflict", "The user MIME configuration directory resolves to a system directory."); return;
        }
    QByteArray contents;
    if (!readMimeFile(selected, &contents, &error) || !setMimeDefault(&contents, mime, desktopId, &error)) {
        fail("unsupported_configuration", error); return;
    }
    p.details = {{"file", selected}, {"fileFingerprint", FileOps::fingerprint(selected)}, {"userInputs", fingerprints},
                 {"plannedDefaultList", FileOps::iniGet(contents, "Default Applications", mime).value},
                 {"appFile", app.value("path").toString()}, {"appFingerprint", app.value("fingerprint").toString()},
                 {"declaresMime", app.value("mimes").toString().split(';').contains(mime)}, {"verificationScope", "effective-default-handler"},
                 {"failureRecovery", "restore-user-file"}};
    p.effects << "Changes only this type's default in the effective user MIME override; other fallback IDs, comments, groups and package files are preserved."
              << "A readback confirms the selected desktop ID, not the app's ability to open every file of this type. There is no automatic history undo.";
    readAssociation(mime, [p, done](const QJsonObject &read) mutable {
        if (!read.value("ok").toBool()) { p.errorCode = "read_unavailable"; p.error = read.value("reason").toString(); done(p); return; }
        p.before = read.value("current").toString(); p.ok = true; done(p);
    });
}

void SystemProvider::setAssociation(const QString &mime, const QString &desktopId) {
    planAssociation(mime, desktopId, [this, mime, desktopId](const ProviderPlan &p) {
        if (!p.ok) {
            Transaction tx; tx.phase = "failed"; tx.message = p.error;
            emit message(tx.message, true); emit operationFinished(tx); return;
        }
        const QString file = p.details.value("file").toString();
        ChangeCoordinator::Request request;
        request.settingId = "apps.association." + mime; request.label = mime; request.before = p.before; request.after = desktopId;
        request.extra = p.details; request.files << file;
        request.expectedFingerprints.insert(file, p.details.value("fileFingerprint").toString());
        request.expectedFingerprints.insert(p.details.value("appFile").toString(), p.details.value("appFingerprint").toString());
        request.apply = [this, p, mime, desktopId, file](ChangeCoordinator::Done done) {
            QString error;
            const auto inputs = p.details.value("userInputs").toObject();
            for (auto it = inputs.begin(); it != inputs.end(); ++it) {
                QByteArray bytes;
                const auto info = QFileInfo(it.key());
                const QString fingerprint = info.isSymLink() ? "symlink:" + info.symLinkTarget() : FileOps::fingerprintBytes(readMimeFile(it.key(), &bytes, &error) ? bytes : QByteArray());
                if (!error.isEmpty() || fingerprint != it.value().toString()) { done(false, "The MIME overrides changed; read the current state before changing it."); return; }
            }
            readAssociation(mime, [this, p, mime, desktopId, file, done](const QJsonObject &read) {
                if (!read.value("ok").toBool() || read.value("current").toString() != p.before) { done(false, "The effective MIME handler changed or could not be read."); return; }
                QByteArray contents; QString error;
                const auto currentApp = appById(readApps(m_runner->env(), &error), desktopId);
                if (!error.isEmpty() || !currentApp.value("available").toBool()
                    || currentApp.value("path").toString() != p.details.value("appFile").toString()
                    || currentApp.value("fingerprint").toString() != p.details.value("appFingerprint").toString()
                    || QFileInfo(file).isSymLink()
                    || FileOps::fingerprint(file) != p.details.value("fileFingerprint").toString()
                    || !readMimeFile(file, &contents, &error) || !setMimeDefault(&contents, mime, desktopId, &error)) {
                    done(false, error.isEmpty() ? "The app or MIME user file changed before the write." : error); return;
                }
                done(FileOps::atomicWrite(file, contents, &error, 0644), error);
            });
        };
        request.verify = [this, mime, desktopId](ChangeCoordinator::Done done) {
            readAssociation(mime, [desktopId, done](const QJsonObject &read) {
                done(read.value("ok").toBool() && read.value("current").toString() == desktopId, tr("The association did not change."));
            });
        };
        m_coordinator->submit(std::move(request), [this](const Transaction &tx) {
            if (tx.phase != QLatin1String("committed")) emit message(tx.message, true);
            emit operationFinished(tx); refreshApps();
        });
    });
}

QVariantMap SystemProvider::coverage() const {
    QHash<QString, int> counts;
    int settings = 0, native = 0, panels = 0, wizards = 0, helpers = 0, advanced = 0, unavailable = 0;
    for (const auto &entry : m_catalog->entries()) {
        const QString integration = entry.integration;
        if (entry.isSetting())
            ++settings;
        if (integration == QLatin1String("native"))
            ++native;
        else if (integration == QLatin1String("omarchy-panel"))
            ++panels;
        else if (integration == QLatin1String("omarchy-wizard"))
            ++wizards;
        else if (integration == QLatin1String("external-helper"))
            ++helpers;
        else if (integration == QLatin1String("advanced-config"))
            ++advanced;
        if (!m_capabilities->availability(entry).available)
            ++unavailable;
        counts[entry.domain]++;
    }
    QVariantMap domains;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
        domains.insert(it.key(), it.value());
    int exclusions = m_catalog->exclusions().value("commands").toArray().size() + m_catalog->exclusions().value("menu").toArray().size();
    return QVariantMap{{"total", int(m_catalog->entries().size())}, {"settings", settings}, {"native", native}, {"panels", panels}, {"wizards", wizards},
                       {"helpers", helpers}, {"advanced", advanced}, {"unavailable", unavailable}, {"exclusions", exclusions}, {"domains", domains}};
}

QString SystemProvider::buildDiagnostics(const QString &debugOutput, const QVariantMap &capabilities, const QVariantMap &versions, const QList<Transaction> &history) {
    QString out;
    out += "Omarchy Control diagnostics\n";
    out += "Generated: " + QDateTime::currentDateTimeUtc().toString(Qt::ISODate) + "\n\n== Versions ==\n";
    for (auto it = versions.constBegin(); it != versions.constEnd(); ++it)
        out += it.key() + ": " + it.value().toString() + "\n";
    out += "\n== Capabilities ==\n";
    for (auto it = capabilities.constBegin(); it != capabilities.constEnd(); ++it)
        out += it.key() + ": " + it.value().toString() + "\n";
    out += "\n== Recent changes ==\n";
    for (const auto &tx : history)
        out += tx.at.toString(Qt::ISODate) + " " + tx.phase + " " + tx.settingId + " " + tx.message + "\n";
    out += "\n== omarchy debug --no-sudo --print ==\n" + debugOutput + "\n";
    return ChangeCoordinator::redact(out);
}

ProviderPlan SystemProvider::planDiagnostics() const {
    ProviderPlan plan;
    plan.operation = "diagnostics.export"; plan.target = m_runner->env().appStateDir + "/exports"; plan.after = "local-report";
    if (m_exporting) { plan.errorCode = "configuration_conflict"; plan.error = "A diagnostics export is already running."; return plan; }
    const Env &env = m_runner->env();
    const QString helper = env.findExecutable("omarchy-debug");
    if (helper.isEmpty() || env.findExecutable("bash").isEmpty() || env.findExecutable("env").isEmpty()) {
        plan.errorCode = "unavailable"; plan.error = "The Omarchy debug helper or its shell is not installed."; return plan;
    }
    const QString changed = Compat(env).changedForKey("diagnostics.export");
    if (!changed.isEmpty()) { plan.errorCode = "compatibility_blocked"; plan.error = "The Omarchy debug helper changed since review; private diagnostics collection is paused."; return plan; }
    QByteArray source;
    if (QFileInfo(helper).size() > 128 * 1024 || !FileOps::readFile(helper, &source)
        || source.count("LOG_FILE=\"/tmp/omarchy-debug.log\"") != 1) {
        plan.errorCode = "unsupported_configuration"; plan.error = "The debug helper's raw-log destination cannot be isolated safely."; return plan;
    }
    for (const QString &path : {m_runner->env().appStateDir, plan.target}) {
        const QFileInfo info(path);
        if (info.isSymLink() || (info.exists() && (!info.isDir() || info.ownerId() != uint(getuid())))) {
            plan.errorCode = "ownership_conflict"; plan.error = "The diagnostics state/export directory is linked, not a directory, or owned by another user."; return plan;
        }
    }
    plan.details = {{"directory", plan.target}, {"fileMode", "0600"}, {"directoryMode", "0700"}, {"redaction", "best-effort"},
                    {"sourceHelper", helper}, {"sourceFingerprint", FileOps::fingerprintBytes(source)}, {"privateRawLog", true},
                    {"debugArgv", QJsonArray{"bash", "<private-helper-copy>", "--no-sudo", "--print"}}, {"timeoutMs", 30000},
                    {"maxOutputBytes", 1024 * 1024}, {"rawLogMaxBytes", 2 * 1024 * 1024}};
    plan.effects << "Writes one private local report containing available versions, capability readings, recent history and bounded Omarchy debug output. Nothing is uploaded or run with sudo."
                 << "Runs a fingerprint-checked private copy of the helper with only its raw-log destination relocated. The shared /tmp/omarchy-debug.log is not used. Private staging is removed after collection."
                 << "Common credentials are redacted heuristically. Review a report before sharing it. A failed or truncated debug collection can still produce an explicitly incomplete report. No settings transaction or history undo is created.";
    plan.ok = true;
    return plan;
}

QString SystemProvider::collectDiagnostics(std::function<void(QJsonObject)> done) {
    const auto plan = planDiagnostics();
    if (!plan.ok) { done({{"ok", false}, {"code", plan.errorCode}, {"message", plan.error}, {"reportSaved", false}}); return {}; }
    const QString directory = plan.target;
    if (!QDir().mkpath(directory) || !QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        done({{"ok", false}, {"code", "write_unavailable"}, {"message", "The private diagnostics directory could not be prepared."}, {"reportSaved", false}}); return {};
    }
    auto staging = std::make_shared<QTemporaryDir>(directory + "/collect-XXXXXX");
    QByteArray source;
    QString stagingError;
    if (!staging->isValid() || !FileOps::readFile(plan.details.value("sourceHelper").toString(), &source)
        || FileOps::fingerprintBytes(source) != plan.details.value("sourceFingerprint").toString()) {
        done({{"ok", false}, {"code", "read_unavailable"}, {"message", "The reviewed debug helper changed or private staging could not be created."}, {"reportSaved", false}}); return {};
    }
    source.replace("LOG_FILE=\"/tmp/omarchy-debug.log\"", "LOG_FILE=\"${OMARCHY_CONTROL_DIAGNOSTICS_LOG:?}\"");
    const QString privateHelper = staging->path() + "/helper.sh";
    if (!FileOps::atomicWrite(privateHelper, source, &stagingError, 0600)) {
        done({{"ok", false}, {"code", "write_unavailable"}, {"message", stagingError}, {"reportSaved", false}}); return {};
    }
    auto file = std::make_shared<QTemporaryFile>(directory + "/diagnostics-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") + "-XXXXXX.txt");
    if (!file->open() || !file->setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
        done({{"ok", false}, {"code", "write_unavailable"}, {"message", "The private diagnostics file could not be created."}, {"reportSaved", false}}); return {};
    }
    const QString path = file->fileName();
    m_exporting = true;
    const QPointer<SystemProvider> guard(this);
    m_capabilities->scanServices([this, guard, file, path, staging, privateHelper, done] {
        if (!guard) return;
        CommandOptions options;
        options.timeoutMs = 30000; options.maxOutputBytes = 1024 * 1024;
        // The wrapper is constant shell code; paths and flags remain separate argv.
        // Its limit also bounds the helper's private raw file, and prevents core dumps.
        m_runner->run({"env", "OMARCHY_CONTROL_DIAGNOSTICS_LOG=" + staging->path() + "/raw.log", "bash", "-c",
                      "umask 077 && ulimit -c 0 && ulimit -f 2048 && exec bash \"$@\"", "omarchy-control-diagnostics", privateHelper, "--no-sudo", "--print"},
                     options, this, [this, file, path, staging, done](const CommandResult &result) {
            Q_UNUSED(staging); // Keeps both the private helper and raw log alive until completion.
            const bool complete = result.ok() && !result.truncated && !result.outText().isEmpty();
            const QString problem = result.truncated ? "The debug output exceeded the report limit." : result.ok() && result.outText().isEmpty()
                ? "The debug command returned no report text." : ChangeCoordinator::redact(result.describe("omarchy debug"));
            const QString backend = "Collection status: " + QString(complete ? "complete" : "incomplete") + "\n"
                + (complete ? QString() : problem + "\n") + "\nstdout:\n" + QString::fromUtf8(result.out) + "\nstderr:\n" + QString::fromUtf8(result.err);
            const QByteArray report = buildDiagnostics(backend, m_capabilities->values(), m_capabilities->versions(), m_coordinator->history(20)).toUtf8();
            bool saved = file->write(report) == report.size() && file->flush() && fsync(file->handle()) == 0;
            saved &= file->seek(0) && file->readAll() == report;
            saved &= QFileInfo(path).isFile() && !QFileInfo(path).isSymLink() && QFileInfo(path).ownerId() == uint(getuid())
                && FileOps::fingerprint(path) == FileOps::fingerprintBytes(report);
            const auto permissions = QFileInfo(path).permissions();
            saved &= permissions.testFlag(QFile::ReadOwner) && permissions.testFlag(QFile::WriteOwner)
                && !(permissions & (QFile::ExeOwner | QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther));
            QJsonObject response{{"ok", saved && complete}, {"reportSaved", saved}, {"complete", complete}, {"redaction", "best-effort"},
                                 {"backend", QJsonObject{{"ok", result.ok()}, {"exitCode", result.exitCode}, {"failure", int(result.failure)}, {"truncated", result.truncated}}}};
            if (saved) {
                file->setAutoRemove(false);
                response["path"] = path; response["bytes"] = report.size(); response["sha256"] = FileOps::fingerprintBytes(report); response["fileMode"] = "0600";
                response["code"] = complete ? QString() : QStringLiteral("diagnostics_incomplete");
                response["message"] = complete ? tr("Saved diagnostics to %1").arg(path)
                    : tr("Saved incomplete diagnostics to %1. %2").arg(path, problem);
            } else {
                response["code"] = "write_unavailable"; response["message"] = "The report could not be saved and verified privately.";
            }
            file->close(); m_exporting = false; done(response);
        });
    });
    return path;
}

QString SystemProvider::exportDiagnostics() {
    return collectDiagnostics([this](const QJsonObject &result) {
        emit diagnosticsFinished(result);
        emit message(result.value("message").toString(), !result.value("ok").toBool());
    });
}
