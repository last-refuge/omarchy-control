#include "launchereditor.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include "fileops.h"
#include "iconlayer.h"
#include "iconthemeprovider.h"
#include "iconthemes.h"

namespace {

const char Group[] = "Desktop Entry";
const char Marker[] = "X-OmarchyControl-Override";
const char SourceKey[] = "X-OmarchyControl-Source";
const char HashKey[] = "X-OmarchyControl-SourceHash";
const char ChangedKey[] = "X-OmarchyControl-Changed";
const char SettingId[] = "apps.launcher.edit";
constexpr qint64 MaxDesktopBytes = 512 * 1024;
constexpr qint64 MaxIconBytes = 4 * 1024 * 1024;

QString value(const QByteArray &document, const QString &key) {
    return FileOps::iniGet(document, QString::fromLatin1(Group), key).value;
}

bool has(const QByteArray &document, const QString &key) {
    return FileOps::iniGet(document, QString::fromLatin1(Group), key).found;
}

QString sha1(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
}

// The languages a launcher would show its name in for this session, most exact first.
QStringList languages(const Env &env) {
    for (const char *variable : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        QString locale = env.process.value(QLatin1String(variable));
        locale = locale.section('.', 0, 0).section('@', 0, 0);
        if (locale.isEmpty() || locale == QLatin1String("C") || locale == QLatin1String("POSIX"))
            continue;
        QStringList list{locale};
        if (locale.contains('_'))
            list << locale.section('_', 0, 0);
        return list;
    }
    return {};
}

QString localized(const QByteArray &document, const QString &key, const QStringList &langs) {
    for (const QString &lang : langs)
        if (has(document, key + "[" + lang + "]"))
            return value(document, key + "[" + lang + "]");
    return value(document, key);
}

QStringList applicationRoots(const Env &env) {
    QStringList roots{env.dataHome + "/applications"};
    for (const QString &directory : env.dataDirs)
        roots << directory + "/applications";
    roots.removeDuplicates();
    return roots;
}

bool setKey(QByteArray *document, const QString &key, const QString &text, QString *error) {
    return FileOps::iniUpsert(document, QString::fromLatin1(Group), key, text, error);
}

// The fields this app changed, kept in the copy so a package update can be redone.
QStringList changedFields(const QByteArray &document) {
    return value(document, QString::fromLatin1(ChangedKey)).split(';', Qt::SkipEmptyParts);
}

bool applyEdit(QByteArray *document, const LauncherEditor::Edit &edit, const QStringList &langs, QStringList *changed, QString *error) {
    if (edit.name) {
        if (!setKey(document, QStringLiteral("Name"), *edit.name, error))
            return false;
        // The name in this language too, where the launcher has one, so the new name is
        // what shows. Every other language keeps its own.
        for (const QString &lang : langs)
            if (has(*document, "Name[" + lang + "]") && !setKey(document, "Name[" + lang + "]", *edit.name, error))
                return false;
        *changed << QStringLiteral("Name");
    }
    if (edit.icon) {
        if (!setKey(document, QStringLiteral("Icon"), *edit.icon, error))
            return false;
        *changed << QStringLiteral("Icon");
    }
    if (edit.hidden) {
        if (!setKey(document, QStringLiteral("NoDisplay"), *edit.hidden ? QStringLiteral("true") : QStringLiteral("false"), error))
            return false;
        *changed << QStringLiteral("NoDisplay");
    }
    changed->removeDuplicates();
    return true;
}

QStringList omarchyHides(const Env &env) {
    QByteArray contents;
    FileOps::readFile(env.omarchyPath + "/default/omarchy/launcher.hides", &contents);
    QStringList ids;
    for (const QByteArray &line : contents.split('\n')) {
        QString id = QString::fromUtf8(line).trimmed();
        if (id.endsWith(QLatin1String(".desktop")))
            id.chop(8);
        if (!id.isEmpty())
            ids << id;
    }
    return ids;
}

}  // namespace

LauncherEditor::LauncherEditor(const Env &env, ChangeCoordinator *coordinator, IconThemeProvider *icons, QObject *parent)
    : QObject(parent), m_env(env), m_coordinator(coordinator), m_icons(icons) {}

QString LauncherEditor::userPath(const QString &id) const {
    return m_env.dataHome + "/applications/" + id;
}

QString LauncherEditor::backupPath(const QString &id) const {
    return m_env.dataHome + "/omarchy-control/launchers/" + id;
}

QString LauncherEditor::iconFolder() const {
    return m_env.dataHome + "/omarchy-control/launcher-icons";
}

QList<LauncherEditor::Entry> LauncherEditor::scan() const {
    QList<Entry> list;
    QSet<QString> seen;
    const QStringList langs = languages(m_env);
    const QStringList hides = omarchyHides(m_env);
    const QString userRoot = m_env.dataHome + "/applications";
    for (const QString &root : applicationRoots(m_env)) {
        QDirIterator it(root, {"*.desktop"}, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
        QStringList paths;
        while (it.hasNext())
            paths << it.next();
        paths.sort();
        for (const QString &path : std::as_const(paths)) {
            const QString id = QDir(root).relativeFilePath(path).replace('/', '-');
            if (seen.contains(id))
                continue;
            seen.insert(id);
            QByteArray contents;
            if (QFileInfo(path).size() > MaxDesktopBytes || !FileOps::readFile(path, &contents))
                continue;
            // Hidden means deleted, and it hides the same launcher further down too.
            if (value(contents, "Type") != QLatin1String("Application") || value(contents, "Hidden") == QLatin1String("true"))
                continue;
            Entry entry;
            entry.id = id;
            entry.path = path;
            entry.name = localized(contents, "Name", langs);
            entry.comment = localized(contents, "Comment", langs);
            entry.icon = value(contents, "Icon");
            entry.visible = value(contents, "NoDisplay") != QLatin1String("true");
            entry.omarchyHidden = hides.contains(id.chopped(id.endsWith(QLatin1String(".desktop")) ? 8 : 0));
            if (root != userRoot) {
                entry.kind = QStringLiteral("package");
            } else if (value(contents, QString::fromLatin1(Marker)) == QLatin1String("true")) {
                entry.source = value(contents, QString::fromLatin1(SourceKey));
                entry.modified = true;
                QByteArray source;
                if (entry.source.isEmpty() || !FileOps::readFile(entry.source, &source)) {
                    entry.kind = QStringLiteral("orphan");
                } else {
                    entry.kind = QStringLiteral("override");
                    entry.outdated = sha1(source) != value(contents, QString::fromLatin1(HashKey));
                }
            } else {
                entry.kind = QStringLiteral("own");
                entry.modified = QFileInfo::exists(backupPath(id));
            }
            if (!entry.name.isEmpty())
                list << entry;
        }
    }
    std::sort(list.begin(), list.end(), [](const Entry &a, const Entry &b) { return a.name.localeAwareCompare(b.name) < 0; });
    return list;
}

LauncherEditor::Entry LauncherEditor::find(const QString &id) const {
    const QString wanted = id.endsWith(QLatin1String(".desktop")) ? id : id + ".desktop";
    for (const Entry &entry : scan())
        if (entry.id == wanted)
            return entry;
    return {};
}

// One change over the launcher's file, and any file that goes with it. A missing value
// removes the file. The coordinator keeps what was there, for rollback and undo.
void LauncherEditor::submit(const QString &id, const QString &label, const QString &path, const QByteArray &contents, const QStringList &extraFiles,
                            const QHash<QString, QByteArray> &extraContents, Finished finished, const QString &runId) {
    QByteArray current;
    FileOps::readFile(path, &current);
    const QStringList langs = languages(m_env);
    // History shows the app by name: what it was called, and what it is called now.
    const auto shown = [&langs, &id](const QByteArray &document) {
        const QString name = localized(document, QStringLiteral("Name"), langs);
        return name.isEmpty() ? id : name;
    };
    // The package's launcher under the user's copy: what shows when there is no copy.
    QByteArray below;
    for (const QString &root : applicationRoots(m_env).mid(1)) {
        QString file = root + "/" + id;
        for (int dash = 0; !QFileInfo::exists(file) && (dash = id.indexOf('-', dash)) >= 0; ++dash)
            file = root + "/" + QString(id).replace(dash, 1, '/');
        if (FileOps::readFile(file, &below))
            break;
    }
    const QByteArray now = contents.isNull() ? below : contents;
    ChangeCoordinator::Request request;
    request.settingId = QString::fromLatin1(SettingId);
    request.label = label;
    request.kind = QStringLiteral("apply");
    request.before = shown(current.isEmpty() ? below : current);
    request.after = shown(now);
    request.recovery = QStringLiteral("auto-undo");
    request.files = QStringList{path} + extraFiles;
    for (const QString &file : std::as_const(request.files))
        request.expectedFingerprints.insert(file, FileOps::fingerprint(file));
    request.extra["launcher"] = QJsonObject{{"id", id}};
    request.runId = runId;
    const bool remove = contents.isNull();
    request.apply = [path, contents, remove, extraFiles, extraContents](ChangeCoordinator::Done done) {
        QString error;
        for (const QString &file : extraFiles) {
            const QByteArray bytes = extraContents.value(file);
            if (bytes.isNull()) {
                if (!QFile::remove(file) && QFileInfo::exists(file)) {
                    done(false, LauncherEditor::tr("%1 could not be removed.").arg(file));
                    return;
                }
            } else if (!FileOps::atomicWrite(file, bytes, &error, 0644)) {
                done(false, error);
                return;
            }
        }
        if (remove) {
            if (!QFile::remove(path) && QFileInfo::exists(path)) {
                done(false, LauncherEditor::tr("%1 could not be removed.").arg(path));
                return;
            }
        } else if (!FileOps::atomicWrite(path, contents, &error, 0644)) {
            done(false, error);
            return;
        }
        done(true, {});
    };
    request.verify = [path, contents, remove](ChangeCoordinator::Done done) {
        QByteArray now;
        const bool ok = remove ? !QFileInfo::exists(path) : (FileOps::readFile(path, &now) && now == contents);
        done(ok, ok ? QString() : LauncherEditor::tr("The launcher did not change."));
    };
    // Only launchers that open files are in that database, before or after the change.
    const bool mime = contents.contains("MimeType=") || current.contains("MimeType=");
    m_coordinator->submit(std::move(request), [this, mime, finished](const Transaction &tx) {
        if (tx.phase == QLatin1String("committed")) {
            const QString tool = m_env.findExecutable(QStringLiteral("update-desktop-database"));
            if (!tool.isEmpty() && mime)
                QProcess::startDetached(tool, {"-q", m_env.dataHome + "/applications"});
            if (m_icons)
                m_icons->syncLayer(true);
        }
        finished(tx);
    });
}

static Transaction failed(const QString &label, const QString &message) {
    Transaction tx;
    tx.settingId = QString::fromLatin1(SettingId);
    tx.label = label;
    tx.phase = QStringLiteral("failed");
    tx.message = message;
    return tx;
}

LauncherEditor::Edit LauncherEditor::recordedEdit(const Entry &entry) const {
    Edit edit;
    QByteArray copy;
    if (entry.kind != QLatin1String("override") || !FileOps::readFile(entry.path, &copy))
        return edit;
    const QStringList changed = changedFields(copy);
    if (changed.contains(QStringLiteral("Name")))
        edit.name = value(copy, "Name");
    if (changed.contains(QStringLiteral("Icon")))
        edit.icon = value(copy, "Icon");
    if (changed.contains(QStringLiteral("NoDisplay")))
        edit.hidden = value(copy, "NoDisplay") == QLatin1String("true");
    return edit;
}

QJsonObject LauncherEditor::currentFields(const Entry &entry) const {
    QByteArray document;
    if (entry.id.isEmpty() || !FileOps::readFile(entry.path, &document))
        return {};
    return QJsonObject{{"name", value(document, "Name")}, {"icon", value(document, "Icon")}, {"hidden", value(document, "NoDisplay") == QLatin1String("true")}};
}

void LauncherEditor::apply(const QString &id, const Edit &edit, Finished finished, const QString &runId) {
    const QString label = QString::fromUtf8(QT_TRANSLATE_NOOP("catalog", "Change an app's launcher"));
    const Entry entry = find(id);
    if (entry.id.isEmpty()) {
        finished(failed(label, tr("No launcher called %1 was found.").arg(id)));
        return;
    }
    Edit clean = edit;
    if (clean.name) {
        QString name = clean.name->simplified();
        if (name.isEmpty() || name.size() > 200) {
            finished(failed(label, tr("An app's name needs to be between 1 and 200 characters.")));
            return;
        }
        clean.name = name;
    }
    if (clean.icon) {
        const QString icon = clean.icon->trimmed();
        const bool file = QDir::isAbsolutePath(icon);
        if (icon.isEmpty() || (file ? !QFileInfo(icon).isFile() : !IconThemes::isValidId(icon))) {
            finished(failed(label, tr("%1 is not an icon that can be used.").arg(icon)));
            return;
        }
        clean.icon = icon;
    }
    const QString path = userPath(entry.id);
    QByteArray document;
    QStringList extraFiles;
    QHash<QString, QByteArray> extraContents;
    QStringList changed;
    QString error;
    if (!FileOps::readFile(entry.path, &document, &error)) {
        finished(failed(label, error));
        return;
    }
    if (entry.kind == QLatin1String("package")) {
        // A copy of the package's file, so the package's own stays exactly as installed.
        const QString hash = sha1(document);
        if (!setKey(&document, QString::fromLatin1(Marker), QStringLiteral("true"), &error) || !setKey(&document, QString::fromLatin1(SourceKey), entry.path, &error)
            || !setKey(&document, QString::fromLatin1(HashKey), hash, &error)) {
            finished(failed(label, error));
            return;
        }
    } else if (entry.kind == QLatin1String("own") && !QFileInfo::exists(backupPath(entry.id))) {
        // The user's own launcher, kept once as it was, so Reset can always go back to it.
        extraFiles << backupPath(entry.id);
        extraContents.insert(backupPath(entry.id), document);
    }
    changed = changedFields(document);
    if (!applyEdit(&document, clean, languages(m_env), &changed, &error)) {
        finished(failed(label, error));
        return;
    }
    if (entry.kind != QLatin1String("own") && !setKey(&document, QString::fromLatin1(ChangedKey), changed.join(';') + ";", &error)) {
        finished(failed(label, error));
        return;
    }
    submit(entry.id, label, path, document, extraFiles, extraContents, finished, runId);
}

void LauncherEditor::reset(const QString &id, Finished finished) {
    const QString label = QString::fromUtf8(QT_TRANSLATE_NOOP("catalog", "Reset an app's launcher"));
    const Entry entry = find(id);
    if (entry.id.isEmpty() || !entry.modified) {
        finished(failed(label, tr("This launcher has no changes to reset.")));
        return;
    }
    if (entry.kind == QLatin1String("own")) {
        QByteArray original;
        if (!FileOps::readFile(backupPath(entry.id), &original)) {
            finished(failed(label, tr("The launcher's backup could not be read, so it was left as it is.")));
            return;
        }
        submit(entry.id, label, entry.path, original, {backupPath(entry.id)}, {{backupPath(entry.id), QByteArray()}}, finished);
        return;
    }
    // Removing the copy brings the package's launcher back, as it is now.
    submit(entry.id, label, entry.path, QByteArray(), {}, {}, finished);
}

void LauncherEditor::update(const QString &id, Finished finished) {
    const QString label = QString::fromUtf8(QT_TRANSLATE_NOOP("catalog", "Update an app's launcher"));
    const Entry entry = find(id);
    QByteArray copy, source;
    if (entry.kind != QLatin1String("override") || !entry.outdated || !FileOps::readFile(entry.path, &copy) || !FileOps::readFile(entry.source, &source)) {
        finished(failed(label, tr("This launcher is already up to date.")));
        return;
    }
    // The package's new launcher, with the same changes made to it again.
    QByteArray document = source;
    const QStringList changed = changedFields(copy);
    QString error;
    Edit edit;
    if (changed.contains(QStringLiteral("Name")))
        edit.name = value(copy, "Name");
    if (changed.contains(QStringLiteral("Icon")))
        edit.icon = value(copy, "Icon");
    if (changed.contains(QStringLiteral("NoDisplay")))
        edit.hidden = value(copy, "NoDisplay") == QLatin1String("true");
    QStringList again;
    if (!setKey(&document, QString::fromLatin1(Marker), QStringLiteral("true"), &error) || !setKey(&document, QString::fromLatin1(SourceKey), entry.source, &error)
        || !setKey(&document, QString::fromLatin1(HashKey), sha1(source), &error) || !applyEdit(&document, edit, languages(m_env), &again, &error)
        || !setKey(&document, QString::fromLatin1(ChangedKey), again.join(';') + ";", &error)) {
        finished(failed(label, error));
        return;
    }
    submit(entry.id, label, entry.path, document, {}, {}, finished);
}

void LauncherEditor::undo(const Transaction &original, Finished finished) {
    const QString changedSince = tr("The launcher changed after this, so undoing it could overwrite that edit.");
    QStringList paths;
    QHash<QString, QByteArray> restores;
    for (auto it = original.files.begin(); it != original.files.end(); ++it) {
        const QJsonObject file = it.value().toObject();
        if (FileOps::fingerprint(it.key()) != file.value("after").toString()) {
            finished(failed(original.label, changedSince));
            return;
        }
        paths << it.key();
        QByteArray contents;
        if (file.value("before").toString() != QLatin1String("missing") && !FileOps::readFile(file.value("backup").toString(), &contents)) {
            finished(failed(original.label, tr("The saved copy of %1 is gone, so this cannot be undone.").arg(QFileInfo(it.key()).fileName())));
            return;
        }
        // A null value is a file that was not there before, removed again.
        restores.insert(it.key(), file.value("before").toString() == QLatin1String("missing") ? QByteArray() : (contents.isNull() ? QByteArray("") : contents));
    }
    if (paths.isEmpty()) {
        finished(failed(original.label, tr("This change cannot be undone automatically.")));
        return;
    }
    const QString main = paths.first();
    QStringList rest = paths.mid(1);
    QHash<QString, QByteArray> restContents;
    for (const QString &file : std::as_const(rest))
        restContents.insert(file, restores.value(file));
    // Every file goes back to what it was, through the same steps as any change.
    ChangeCoordinator::Request request;
    request.settingId = original.settingId;
    request.label = original.label;
    request.kind = QStringLiteral("undo");
    request.before = original.after;
    request.after = original.before;
    request.recovery = QStringLiteral("auto-undo");
    request.files = paths;
    for (const QString &file : std::as_const(paths))
        request.expectedFingerprints.insert(file, FileOps::fingerprint(file));
    request.extra = original.extra;
    const QByteArray mainContents = restores.value(main);
    request.apply = [main, mainContents, rest, restContents](ChangeCoordinator::Done done) {
        QString error;
        for (const QString &file : rest + QStringList{main}) {
            const QByteArray bytes = file == main ? mainContents : restContents.value(file);
            if (bytes.isNull()) {
                if (!QFile::remove(file) && QFileInfo::exists(file)) {
                    done(false, LauncherEditor::tr("%1 could not be removed.").arg(file));
                    return;
                }
            } else if (!FileOps::atomicWrite(file, bytes, &error, 0644)) {
                done(false, error);
                return;
            }
        }
        done(true, {});
    };
    request.verify = [main, mainContents](ChangeCoordinator::Done done) {
        QByteArray now;
        done(mainContents.isNull() ? !QFileInfo::exists(main) : (FileOps::readFile(main, &now) && now == mainContents), {});
    };
    m_coordinator->submit(std::move(request), [this, finished](const Transaction &tx) {
        if (tx.phase == QLatin1String("committed") && m_icons)
            m_icons->syncLayer(true);
        finished(tx);
    });
}

QString LauncherEditor::importIcon(const QString &file, QString *error) const {
    const QFileInfo info(file);
    const QString extension = info.suffix().toLower();
    if (!info.isFile() || info.size() > MaxIconBytes || !QStringList{"png", "svg", "xpm"}.contains(extension)) {
        *error = tr("Choose a PNG, SVG, or XPM image under 4 MB.");
        return {};
    }
    QByteArray bytes;
    if (!FileOps::readFile(file, &bytes) || (extension == QLatin1String("svg") ? !bytes.contains("<svg") : QImageReader(file).read().isNull())) {
        *error = tr("%1 could not be read as an image.").arg(info.fileName());
        return {};
    }
    QString base = info.completeBaseName();
    base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    // Named by its contents, so the same image chosen twice is kept once.
    const QString target = iconFolder() + "/" + base.left(60) + "-" + sha1(bytes).left(10) + "." + extension;
    if (!QFileInfo::exists(target) && !FileOps::atomicWrite(target, bytes, error, 0644))
        return {};
    return target;
}

void LauncherEditor::cleanIcons() const {
    // A copy stays while any launcher, or any backup History could restore, still names it.
    QSet<QString> used;
    QStringList scanned{m_env.dataHome + "/applications", m_env.dataHome + "/omarchy-control/launchers", m_coordinator->backupDirectory()};
    for (const QString &root : std::as_const(scanned)) {
        QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QByteArray contents;
            const QString path = it.next();
            if (QFileInfo(path).size() <= MaxDesktopBytes && FileOps::readFile(path, &contents) && contents.contains(iconFolder().toUtf8()))
                for (const QByteArray &line : contents.split('\n'))
                    if (line.startsWith("Icon="))
                        used.insert(QString::fromUtf8(line.mid(5)).trimmed());
        }
    }
    for (const QFileInfo &icon : QDir(iconFolder()).entryInfoList(QDir::Files))
        if (!used.contains(icon.absoluteFilePath()))
            QFile::remove(icon.absoluteFilePath());
}

QString LauncherEditor::currentPack() const {
    const QString pack = m_icons ? m_icons->layerPack() : QString();
    return pack.isEmpty() ? IconThemes::desktopDefault(m_env) : pack;
}

void LauncherEditor::reload() {
    const QString pack = currentPack();
    QHash<QString, QString> urls;
    QVariantList rows;
    for (const Entry &entry : scan()) {
        if (!urls.contains(entry.icon))
            urls.insert(entry.icon, IconThemes::appIconUrl(m_env, pack, {entry.icon}));
        rows << QVariantMap{{"id", entry.id}, {"name", entry.name}, {"comment", entry.comment}, {"icon", entry.icon}, {"iconUrl", urls.value(entry.icon)},
                            {"kind", entry.kind}, {"visible", entry.visible}, {"omarchyHidden", entry.omarchyHidden}, {"outdated", entry.outdated},
                            {"modified", entry.modified}};
    }
    m_entries = rows;
    emit changed();
}

void LauncherEditor::finishedInWindow(const Transaction &tx) {
    const bool ok = tx.phase == QLatin1String("committed");
    emit message(ok ? tr("Saved. The launcher shows it the next time it opens.") : tx.message, ok ? QStringLiteral("ok") : QStringLiteral("error"));
    cleanIcons();
    reload();
}

void LauncherEditor::save(const QString &id, const QVariantMap &changes) {
    Edit edit;
    if (changes.contains("name"))
        edit.name = changes.value("name").toString();
    if (changes.contains("icon"))
        edit.icon = changes.value("icon").toString();
    if (changes.contains("hidden"))
        edit.hidden = changes.value("hidden").toBool();
    apply(id, edit, [this](const Transaction &tx) { finishedInWindow(tx); });
}

void LauncherEditor::resetEntry(const QString &id) {
    reset(id, [this](const Transaction &tx) { finishedInWindow(tx); });
}

void LauncherEditor::updateEntry(const QString &id) {
    update(id, [this](const Transaction &tx) { finishedInWindow(tx); });
}

QString LauncherEditor::chooseIcon(const QUrl &file) {
    QString error;
    const QString path = file.isLocalFile() ? importIcon(file.toLocalFile(), &error) : QString();
    if (path.isEmpty())
        emit message(error.isEmpty() ? tr("Only an image on this computer can be used.") : error, QStringLiteral("error"));
    return path;
}

QVariantList LauncherEditor::packIcons(const QString &query, int limit) const {
    const QHash<QString, QString> icons = IconLayer::appIcons(m_env, currentPack());
    QStringList names;
    const QString wanted = query.trimmed().toLower();
    for (auto it = icons.begin(); it != icons.end(); ++it)
        if (wanted.isEmpty() || it.key().toLower().contains(wanted))
            names << it.key();
    names.sort(Qt::CaseInsensitive);
    QVariantList rows;
    for (const QString &name : names.mid(0, limit))
        rows << QVariantMap{{"name", name}, {"url", QUrl::fromLocalFile(icons.value(name)).toString()}};
    return rows;
}
