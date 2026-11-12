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
#include <QSysInfo>
#include <algorithm>

#include "fileops.h"

SystemProvider::SystemProvider(const Catalog *catalog, CommandRunner *runner, Capabilities *capabilities, ChangeCoordinator *coordinator, QObject *parent)
    : QObject(parent), m_catalog(catalog), m_runner(runner), m_capabilities(capabilities), m_coordinator(coordinator) {}

QVariantList SystemProvider::parseDf(const QByteArray &text) {
    QVariantList rows;
    const auto lines = QString::fromUtf8(text).split('\n', Qt::SkipEmptyParts);
    static const QStringList ignored{"tmpfs", "devtmpfs", "efivarfs", "overlay", "squashfs", "proc", "sysfs", "ramfs", "fuse.portal"};
    QHash<QString, QString> firstMount;
    for (int i = 1; i < lines.size(); ++i) {
        const auto fields = lines[i].simplified().split(' ');
        if (fields.size() < 6 || ignored.contains(fields[1]))
            continue;
        QVariantMap row;
        row["device"] = fields[0];
        row["fstype"] = fields[1];
        row["size"] = fields[2].toDouble();
        row["used"] = fields[3].toDouble();
        row["available"] = fields[4].toDouble();
        row["percent"] = fields[5].chopped(1).toInt();
        row["mount"] = fields.mid(6).join(' ');
        // An app run from an image, such as an AppImage, mounts itself while it runs. It
        // is shown apart, since it is neither a disk nor space you can use.
        const QString fstype = fields[1], device = fields[0], mount = row["mount"].toString();
        row["temporary"] = (fstype.startsWith("fuse.") && (fstype.endsWith(".AppImage", Qt::CaseInsensitive) || device.endsWith(".AppImage", Qt::CaseInsensitive)))
                           || mount.startsWith("/tmp/.mount_");
        // A size of nothing is a reading that failed, not an empty disk.
        row["known"] = row["size"].toDouble() > 0;
        // Subvolumes of one disk, such as / and /home on btrfs, draw on the same space;
        // the later ones say so instead of looking like more room.
        if (!row["temporary"].toBool() && device.startsWith('/')) {
            if (firstMount.contains(device))
                row["sharedWith"] = firstMount.value(device);
            else
                firstMount.insert(device, mount);
        }
        rows << row;
    }
    return rows;
}

static QString readLine(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readLine()).trimmed() : QString();
}

void SystemProvider::refresh() {
    const Env &env = m_runner->env();
    m_machine["hostname"] = QSysInfo::machineHostName();
    m_machine["kernel"] = QSysInfo::kernelVersion();
    m_machine["architecture"] = QSysInfo::currentCpuArchitecture();
    m_machine["model"] = readLine(env.sys("/sys/class/dmi/id/product_name"));
    m_machine["vendor"] = readLine(env.sys("/sys/class/dmi/id/sys_vendor"));
    m_machine["omarchy"] = m_capabilities->versions().value("omarchy");
    m_machine["hyprland"] = m_capabilities->versions().value("hyprland");
    m_machine["session"] = env.process.value("XDG_SESSION_TYPE");

    m_battery.clear();
    const QDir power(env.sys("/sys/class/power_supply"));
    for (const auto &name : power.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (readLine(power.filePath(name) + "/type") != QLatin1String("Battery"))
            continue;
        m_battery["present"] = true;
        // An unreadable level is unknown, not empty.
        bool readable = false;
        const int percent = readLine(power.filePath(name) + "/capacity").toInt(&readable);
        m_battery["percent"] = readable ? percent : -1;
        m_battery["status"] = readLine(power.filePath(name) + "/status");
        const int cycles = readLine(power.filePath(name) + "/cycle_count").toInt();
        if (cycles > 0)
            m_battery["cycles"] = cycles;
        break;
    }

    m_hooks.clear();
    const QString hookRoot = env.configHome + "/omarchy/hooks";
    QDirIterator hooks(hookRoot, QDir::Files | QDir::Executable | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (hooks.hasNext()) {
        const QString path = hooks.next();
        // Stock samples ship inert; only a real hook runs.
        if (path.endsWith(".sample"))
            continue;
        m_hooks << QVariantMap{{"name", QDir(hookRoot).relativeFilePath(path)}, {"path", path}};
    }

    emit changed();
    m_runner->run({"df", "-B1", "--output=source,fstype,size,used,avail,pcent,target"}, CommandOptions{4000}, this, [this](const CommandResult &result) {
        m_storage = parseDf(result.out);
        emit changed();
    });
}

QVariantList SystemProvider::readApps(const Env &env) {
    QHash<QString, QVariantMap> found;
    QStringList directories{env.sys("/usr/share/applications"), env.sys("/usr/local/share/applications"),
                            env.home + "/.local/share/applications"};
    for (const auto &directory : directories) {
        for (const auto &file : QDir(directory).entryInfoList({"*.desktop"}, QDir::Files | QDir::Readable)) {
            QByteArray contents;
            FileOps::readFile(file.absoluteFilePath(), &contents);
            QString name, comment, mimes, icon;
            bool hidden = false, inEntry = false, application = false;
            for (const auto &raw : QString::fromUtf8(contents).split('\n')) {
                const QString line = raw.trimmed();
                if (line.startsWith('[')) {
                    inEntry = line == QLatin1String("[Desktop Entry]");
                    continue;
                }
                if (!inEntry)
                    continue;
                if (line.startsWith("Name=") && name.isEmpty())
                    name = line.mid(5);
                else if (line.startsWith("Comment=") && comment.isEmpty())
                    comment = line.mid(8);
                else if (line.startsWith("Icon=") && icon.isEmpty())
                    icon = line.mid(5);
                else if (line.startsWith("MimeType="))
                    mimes = line.mid(9);
                else if (line == QLatin1String("Type=Application"))
                    application = true;
                else if (line == QLatin1String("NoDisplay=true") || line == QLatin1String("Hidden=true"))
                    hidden = true;
            }
            if (!application || name.isEmpty())
                continue;
            found.insert(file.fileName(), QVariantMap{{"id", file.fileName()}, {"name", name}, {"comment", comment}, {"mimes", mimes}, {"hidden", hidden}, {"iconName", icon}});
        }
    }
    QVariantList rows;
    for (const auto &row : std::as_const(found))
        rows << row;
    std::sort(rows.begin(), rows.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("name").toString().localeAwareCompare(b.toMap().value("name").toString()) < 0;
    });
    return rows;
}

// The everyday file types people change. xdg-mime stays authoritative for the
// rest, which remain editable through mimeapps.list under Advanced.
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

void SystemProvider::refreshApps() {
    const Env &env = m_runner->env();
    m_apps = readApps(env);
    // Each app's own icon from the desktop's icon theme, looked up once per icon name.
    const QString theme = IconThemes::desktopDefault(env);
    for (auto &value : m_apps) {
        auto app = value.toMap();
        const QString name = app.value("iconName").toString();
        if (!m_iconUrls.contains(name))
            m_iconUrls.insert(name, IconThemes::appIconUrl(env, theme, {name}));
        app["icon"] = m_iconUrls.value(name);
        value = app;
    }
    emit appsChanged();
    auto results = std::make_shared<QHash<QString, QString>>();
    auto remaining = std::make_shared<int>(commonMimes().size());
    for (const auto &[mime, label] : commonMimes()) {
        m_runner->run({"xdg-mime", "query", "default", mime}, CommandOptions{3000}, this, [this, mime, results, remaining](const CommandResult &result) {
            results->insert(mime, result.outText());
            if (--*remaining > 0)
                return;
            QVariantList rows;
            for (const auto &[m, text] : commonMimes()) {
                QVariantList candidates;
                for (const auto &app : std::as_const(m_apps)) {
                    const auto map = app.toMap();
                    if (map.value("mimes").toString().split(';').contains(m))
                        candidates << QVariantMap{{"value", map.value("id")}, {"label", map.value("name")}, {"icon", map.value("icon")}};
                }
                // The current handler by its app's name and icon where it is installed, even
                // when it does not list this type; otherwise its id, exactly as configured.
                const QString current = results->value(m);
                QString currentLabel = current, currentIcon;
                for (const auto &app : std::as_const(m_apps))
                    if (app.toMap().value("id").toString() == current) {
                        currentLabel = app.toMap().value("name").toString();
                        currentIcon = app.toMap().value("icon").toString();
                    }
                rows << QVariantMap{{"mime", m}, {"label", QCoreApplication::translate("SystemProvider", text)}, {"current", current},
                                    {"currentLabel", currentLabel}, {"currentIcon", currentIcon}, {"candidates", candidates}};
            }
            m_associations = rows;
            emit appsChanged();
        });
    }
}

void SystemProvider::launchApp(const QString &desktopId) {
    if (desktopId.contains('/') || !desktopId.endsWith(".desktop"))
        return;
    QString error;
    const QString id = desktopId.chopped(8);
    const QStringList argv = m_capabilities->hasCommand("uwsm-app") ? QStringList{"uwsm-app", "--", id + ".desktop"} : QStringList{"gtk-launch", id};
    if (!m_runner->launchDetached(argv, &error))
        emit message(error, true);
}

void SystemProvider::setAssociation(const QString &mime, const QString &desktopId) {
    bool known = false;
    for (const auto &[m, label] : commonMimes())
        known = known || m == mime;
    if (!known || desktopId.contains('/') || !desktopId.endsWith(".desktop")) {
        emit message(tr("That file association is not valid."), true);
        return;
    }
    ChangeCoordinator::Request request;
    request.settingId = "apps.association." + mime;
    request.label = mime;
    request.after = desktopId;
    const QString file = m_runner->env().configHome + "/mimeapps.list";
    request.files << file;
    request.apply = [this, mime, desktopId](ChangeCoordinator::Done done) {
        CommandOptions options;
        options.mutation = true;
        m_runner->run({"xdg-mime", "default", desktopId, mime}, options, this, [done](const CommandResult &result) { done(result.ok(), result.describe("xdg-mime")); });
    };
    request.verify = [this, mime, desktopId](ChangeCoordinator::Done done) {
        m_runner->run({"xdg-mime", "query", "default", mime}, CommandOptions{3000}, this, [this, desktopId, done](const CommandResult &result) {
            done(result.outText() == desktopId, tr("The association did not change."));
        });
    };
    m_coordinator->submit(std::move(request), [this](const Transaction &tx) {
        if (tx.phase != QLatin1String("committed"))
            emit message(tx.message, true);
        refreshApps();
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

QString SystemProvider::exportDiagnostics() {
    const QString directory = m_runner->env().appStateDir + "/exports";
    QDir().mkpath(directory);
    QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QString path = directory + "/diagnostics-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") + ".txt";
    CommandOptions options;
    options.timeoutMs = 30000;
    // Local export only, and the non-sudo mode: this never uploads anything and
    // never asks for a password.
    m_runner->run({"omarchy", "debug", "--no-sudo", "--print"}, options, this, [this, path](const CommandResult &result) {
        const QString text = buildDiagnostics(QString::fromUtf8(result.out), m_capabilities->values(), m_capabilities->versions(), m_coordinator->history(20));
        QString error;
        if (FileOps::atomicWrite(path, text.toUtf8(), &error))
            emit message(tr("Saved diagnostics to %1").arg(path), false);
        else
            emit message(error, true);
    });
    return path;
}
