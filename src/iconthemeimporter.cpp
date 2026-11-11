#include "iconthemeimporter.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QStorageInfo>
#include <QUuid>
#include <functional>

#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fileops.h"
#include "iconarchive.h"

namespace {

// Generous for real packs, which run to tens of thousands of small files and links, and
// small enough that a wrong folder fails quickly rather than filling a disk.
constexpr qint64 MaxEntries = 400000;
constexpr qint64 MaxTotalBytes = qint64(3) * 1024 * 1024 * 1024;
constexpr qint64 MaxFileBytes = qint64(64) * 1024 * 1024;
constexpr int MaxDepth = 16;
constexpr int MaxLinkHops = 40;
const char StagingPrefix[] = ".omarchy-control-import-";
const char OwnerFile[] = ".omarchy-control-import.json";

struct Entry {
    QString path;  // Relative to the pack.
    enum Kind { Directory, File, Link } kind = File;
    qint64 size = 0;
    qint64 modified = 0;
    QString target;  // For a link.
};

struct Manifest {
    QList<Entry> entries;
    QHash<QString, QString> links;
    qint64 totalBytes = 0;
};

QByteArray local(const QString &path) {
    return QFile::encodeName(path);
}

qint64 modifiedNs(const struct stat &info) {
    return qint64(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec;
}

// Reads the tree without following a single link, so nothing outside it is even looked
// at. Returns an error, written for the person who chose the folder.
QString scan(const QString &root, const QString &relative, int depth, Manifest *manifest, const std::atomic_bool &cancel) {
    if (cancel.load())
        return IconThemeImporter::tr("Import cancelled.");
    if (depth > MaxDepth)
        return IconThemeImporter::tr("This pack nests folders too deeply to be an icon pack.");
    const QString directory = relative.isEmpty() ? root : root + "/" + relative;
    DIR *handle = ::opendir(local(directory).constData());
    if (!handle)
        return IconThemeImporter::tr("%1 could not be read.").arg(directory);
    QStringList names;
    while (const dirent *item = ::readdir(handle)) {
        const QString name = QFile::decodeName(item->d_name);
        if (name != QLatin1String(".") && name != QLatin1String(".."))
            names << name;
    }
    ::closedir(handle);
    names.sort();
    for (const QString &name : std::as_const(names)) {
        // A clone's history is not part of a pack, and can be most of its size.
        if (name == QLatin1String(".git"))
            continue;
        const QString path = relative.isEmpty() ? name : relative + "/" + name;
        struct stat info;
        if (::lstat(local(root + "/" + path).constData(), &info) != 0)
            return IconThemeImporter::tr("%1 could not be read.").arg(path);
        if (manifest->entries.size() >= MaxEntries)
            return IconThemeImporter::tr("This folder has too many files to be an icon pack.");
        Entry entry;
        entry.path = path;
        entry.modified = modifiedNs(info);
        if (S_ISLNK(info.st_mode)) {
            QByteArray target(4096, Qt::Uninitialized);
            const ssize_t length = ::readlink(local(root + "/" + path).constData(), target.data(), target.size());
            if (length < 0 || length >= target.size())
                return IconThemeImporter::tr("The link %1 could not be read.").arg(path);
            entry.kind = Entry::Link;
            entry.target = QFile::decodeName(target.left(length));
            manifest->links.insert(path, entry.target);
            manifest->entries << entry;
        } else if (S_ISDIR(info.st_mode)) {
            entry.kind = Entry::Directory;
            manifest->entries << entry;
            const QString error = scan(root, path, depth + 1, manifest, cancel);
            if (!error.isEmpty())
                return error;
        } else if (S_ISREG(info.st_mode)) {
            if (info.st_size > MaxFileBytes)
                return IconThemeImporter::tr("%1 is too large to be part of an icon pack.").arg(path);
            entry.size = info.st_size;
            manifest->totalBytes += info.st_size;
            if (manifest->totalBytes > MaxTotalBytes)
                return IconThemeImporter::tr("This folder is too large to be an icon pack.");
            manifest->entries << entry;
        } else {
            return IconThemeImporter::tr("%1 is a device or socket, which an icon pack has no use for, so nothing was imported.").arg(path);
        }
    }
    return {};
}

// Follows a link the way the filesystem would, component by component, through the pack's
// other links. Doing it on the text alone could be fooled: x/.. is somewhere else entirely
// when x is itself a link. Only a target that stays inside the pack is allowed.
QString checkLink(const QString &path, const Manifest &manifest, const QString &id, const QStringList &siblings) {
    QStringList resolved = path.split('/');
    resolved.removeLast();
    QStringList pending = manifest.links.value(path).split('/');
    if (manifest.links.value(path).startsWith('/'))
        return IconThemeImporter::tr("The link %1 points outside the pack, so nothing was imported.").arg(path);
    int hops = 0;
    while (!pending.isEmpty()) {
        const QString part = pending.takeFirst();
        if (part.isEmpty() || part == QLatin1String("."))
            continue;
        if (part == QLatin1String("..")) {
            if (resolved.isEmpty()) {
                // Up into the icons folder. Back into this pack is fine; so is a pack that
                // came in the same archive and is installed beside it, as Papirus-Dark
                // links into Papirus. That pack's own links are checked when it is imported.
                const QString into = pending.isEmpty() ? QString() : pending.first();
                if (into == id) {
                    pending.removeFirst();
                    continue;
                }
                if (into.isEmpty() || into == QLatin1String("..") || !siblings.contains(into))
                    return IconThemeImporter::tr("The link %1 points outside the pack, so nothing was imported.").arg(path);
                pending.removeFirst();
                int depth = 0;
                for (const QString &rest : std::as_const(pending)) {
                    if (rest == QLatin1String(".."))
                        --depth;
                    else if (!rest.isEmpty() && rest != QLatin1String("."))
                        ++depth;
                    if (depth < 0)
                        return IconThemeImporter::tr("The link %1 points outside the pack, so nothing was imported.").arg(path);
                }
                return {};
            }
            resolved.removeLast();
            continue;
        }
        resolved << part;
        const auto link = manifest.links.constFind(resolved.join('/'));
        if (link == manifest.links.constEnd())
            continue;
        if (++hops > MaxLinkHops)
            return IconThemeImporter::tr("The link %1 goes round in a circle, so nothing was imported.").arg(path);
        if (link->startsWith('/'))
            return IconThemeImporter::tr("The link %1 points outside the pack, so nothing was imported.").arg(path);
        resolved.removeLast();
        pending = link->split('/') + pending;
    }
    // A link to a folder holding itself would send anything that walks the pack in circles.
    const QString self = resolved.join('/');
    if (self.isEmpty() || path.startsWith(self + "/"))
        return IconThemeImporter::tr("The link %1 goes round in a circle, so nothing was imported.").arg(path);
    return {};
}

// Opens without following a link, and checks it is still the file that was scanned, so a
// swap between scanning and copying cannot bring in anything else.
QString copyFile(const QString &from, const QString &to, const Entry &entry, const std::atomic_bool &cancel, const std::function<void(qint64)> &progress) {
    const int input = ::open(local(from).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (input < 0)
        return IconThemeImporter::tr("%1 could not be read.").arg(entry.path);
    struct stat info;
    if (::fstat(input, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size != entry.size || modifiedNs(info) != entry.modified) {
        ::close(input);
        return IconThemeImporter::tr("The folder changed while it was being imported, so nothing was imported.");
    }
    // Nothing from a pack is executable here: icons do not need it, and it would make an
    // installer script that came with the pack one click from running.
    const int output = ::open(local(to).constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (output < 0) {
        ::close(input);
        return IconThemeImporter::tr("%1 could not be written.").arg(entry.path);
    }
    QByteArray buffer(256 * 1024, Qt::Uninitialized);
    QString error;
    while (error.isEmpty()) {
        const ssize_t got = ::read(input, buffer.data(), buffer.size());
        if (got == 0)
            break;
        if (got < 0) {
            error = IconThemeImporter::tr("%1 could not be read.").arg(entry.path);
            break;
        }
        qint64 written = 0;
        while (written < got) {
            const ssize_t wrote = ::write(output, buffer.constData() + written, got - written);
            if (wrote <= 0) {
                error = errno == ENOSPC ? IconThemeImporter::tr("There is not enough disk space to import this pack.") : IconThemeImporter::tr("%1 could not be written.").arg(entry.path);
                break;
            }
            written += wrote;
        }
        progress(got);
        if (cancel.load())
            error = IconThemeImporter::tr("Import cancelled.");
    }
    ::close(input);
    if (::close(output) != 0 && error.isEmpty())
        error = IconThemeImporter::tr("%1 could not be written.").arg(entry.path);
    return error;
}

bool sameTree(const Manifest &a, const Manifest &b) {
    if (a.entries.size() != b.entries.size())
        return false;
    for (qsizetype i = 0; i < a.entries.size(); ++i) {
        const Entry &x = a.entries[i], &y = b.entries[i];
        if (x.path != y.path || x.kind != y.kind || x.size != y.size || x.target != y.target || (x.kind != Entry::Directory && x.modified != y.modified))
            return false;
    }
    return true;
}

// Builds and checks the copy on its own thread. Everything it needs is copied in, and it
// reports through the callback, so it shares nothing with the window while it runs.
QString stageTree(const IconThemeImporter::Inspection &plan, const QString &staging, const QString &cacheTool, const std::atomic_bool &cancel, const std::function<void(qreal)> &report) {
    Manifest manifest;
    QString error = scan(plan.source, {}, 0, &manifest, cancel);
    if (!error.isEmpty())
        return error;
    for (const auto &entry : std::as_const(manifest.entries)) {
        if (entry.kind != Entry::Link)
            continue;
        error = checkLink(entry.path, manifest, plan.id, plan.siblings);
        if (!error.isEmpty())
            return error;
    }
    const QString parent = QFileInfo(plan.destination).absolutePath();
    QDir().mkpath(parent);
    const qint64 available = QStorageInfo(parent).bytesAvailable();
    if (available >= 0 && available < manifest.totalBytes + 16 * 1024 * 1024)
        return IconThemeImporter::tr("There is not enough disk space to import this pack.");

    if (::mkdir(local(staging).constData(), 0700) != 0)
        return IconThemeImporter::tr("%1 could not be written.").arg(staging);
    // Recorded first, so that if this stops partway a later run can tell its own
    // leftovers from anything else, and only ever remove its own.
    QJsonObject owned;
    for (const auto &entry : std::as_const(manifest.entries))
        owned.insert(entry.path, entry.size);
    QJsonObject owner;
    owner["source"] = plan.source;
    owner["destination"] = plan.destination;
    owner["paths"] = owned;
    if (!FileOps::atomicWrite(staging + "/" + OwnerFile, QJsonDocument(owner).toJson(QJsonDocument::Compact), &error, 0600))
        return error;

    qint64 copied = 0;
    const qreal total = qMax<qreal>(1, manifest.totalBytes + manifest.entries.size());
    qint64 entriesDone = 0;
    for (const auto &entry : std::as_const(manifest.entries)) {
        if (cancel.load())
            return IconThemeImporter::tr("Import cancelled.");
        const QString to = staging + "/" + entry.path;
        if (entry.kind == Entry::Directory) {
            if (::mkdir(local(to).constData(), 0755) != 0)
                return IconThemeImporter::tr("%1 could not be written.").arg(entry.path);
        } else if (entry.kind == Entry::Link) {
            if (::symlink(local(entry.target).constData(), local(to).constData()) != 0)
                return IconThemeImporter::tr("%1 could not be written.").arg(entry.path);
        } else {
            error = copyFile(plan.source + "/" + entry.path, to, entry, cancel, [&](qint64 bytes) { copied += bytes; });
            if (!error.isEmpty())
                return error;
        }
        ++entriesDone;
        if (entriesDone % 64 == 0)
            report((copied + entriesDone) / total);
    }
    // Scanned again: anything that moved while copying makes the copy untrustworthy.
    Manifest after;
    error = scan(plan.source, {}, 0, &after, cancel);
    if (!error.isEmpty())
        return error;
    if (!sameTree(manifest, after))
        return IconThemeImporter::tr("The folder changed while it was being imported, so nothing was imported.");
    QByteArray index;
    if (!FileOps::readFile(staging + "/index.theme", &index) || !IconThemes::parseIndex(index, plan.id).hasIcons())
        return IconThemeImporter::tr("The copy could not be checked, so nothing was imported.");
    if (cancel.load())
        return IconThemeImporter::tr("Import cancelled.");
    // A cache that came with the pack describes the files where it was made, and a stale
    // one hides icons. It is made afresh for this copy, or left out if it cannot be.
    QFile::remove(staging + "/icon-theme.cache");
    if (!cacheTool.isEmpty()) {
        QProcess cache;
        cache.start(cacheTool, {"-f", "-t", "-q", staging});
        // Without one, lookups are only slower, so a failure here is no reason to stop.
        if (!cache.waitForFinished(120000))
            cache.kill();
    }
    QFile::remove(staging + "/" + OwnerFile);
    ::chmod(local(staging).constData(), 0755);
    // Everything reaches the disk before the pack appears, so that a power cut cannot leave
    // a pack that looks finished with empty files in it.
    const int directory = ::open(local(staging).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0) {
        ::syncfs(directory);
        ::close(directory);
    }
    report(1.0);
    return {};
}

// The last step, and the only one that changes what is installed. It refuses to replace
// anything, so a pack that appeared in the meantime is safe.
QString publish(const QString &staging, const IconThemeImporter::Inspection &plan) {
    if (::renameat2(AT_FDCWD, local(staging).constData(), AT_FDCWD, local(plan.destination).constData(), RENAME_NOREPLACE) == 0)
        return {};
    return errno == EEXIST || errno == ENOTEMPTY ? IconThemeImporter::tr("A pack called %1 appeared in your icons folder during the import, and it was left as it is.").arg(plan.id)
                                                 : IconThemeImporter::tr("The pack could not be moved into place.");
}

}  // namespace

IconThemeImporter::IconThemeImporter(const Env &env, ChangeCoordinator *coordinator, QObject *parent)
    : QObject(parent), m_env(env), m_coordinator(coordinator) {}

IconThemeImporter::~IconThemeImporter() {
    if (m_cancel)
        m_cancel->store(true);
    if (m_worker) {
        m_worker->wait();
        delete m_worker;
    }
}

void IconThemeImporter::setState(const QString &state, const QString &message) {
    m_state = state;
    m_message = message;
    emit changed();
}

IconThemeImporter::Inspection IconThemeImporter::inspect(const QString &folder) const {
    Inspection result;
    const QFileInfo info(folder);
    if (!info.exists()) {
        result.error = tr("%1 could not be found.").arg(folder);
        return result;
    }
    if (!info.isDir()) {
        result.error = folder.endsWith(QLatin1String(".rar"), Qt::CaseInsensitive)
                           ? tr("RAR archives cannot be read here. Extract it first, then import the folder it contains.")
                           : tr("This is a file. Choose the folder that holds the pack's index.theme, or a .zip, .tar, or .7z archive.");
        return result;
    }
    result.source = info.canonicalFilePath();
    result.id = QFileInfo(result.source).fileName();
    QFile index(result.source + "/index.theme");
    if (!index.exists()) {
        // The usual download unpacks to a folder around the pack. Pointing at it is more
        // honest than guessing which pack was meant, and the id has to stay the pack's.
        QStringList nested;
        for (const QString &child : QDir(result.source).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            if (QFile::exists(result.source + "/" + child + "/index.theme"))
                nested << child;
        if (nested.size() == 1)
            result.error = tr("The pack is inside this folder, in %1. Choose that folder instead.").arg(nested.first());
        else if (nested.size() > 1)
            result.error = tr("This folder holds %n packs. Choose one of them: %1.", "", int(nested.size())).arg(nested.mid(0, 6).join(", "));
        else
            result.error = tr("This folder has no index.theme, so it is not an icon pack.");
        return result;
    }
    if (!IconThemes::isValidId(result.id)) {
        result.error = tr("A folder called %1 cannot be used as a pack's name.").arg(result.id);
        return result;
    }
    if (index.size() > 1024 * 1024 || !index.open(QIODevice::ReadOnly)) {
        result.error = tr("This pack's index.theme could not be read.");
        return result;
    }
    const IconPack pack = IconThemes::parseIndex(index.readAll(), result.id);
    if (!pack.found) {
        result.error = tr("This pack's index.theme is not valid.");
        return result;
    }
    result.name = pack.name;
    result.hidden = pack.hidden;
    if (!pack.hasIcons()) {
        result.error = tr("%1 has no icon folders. It may be a cursor theme, which goes elsewhere.").arg(pack.name);
        return result;
    }
    bool anyPresent = false;
    for (const auto &directory : pack.directories) {
        if (QFileInfo(result.source + "/" + directory.subdir).isDir()) {
            anyPresent = true;
            break;
        }
    }
    if (!anyPresent) {
        result.error = tr("None of the folders this pack's index.theme lists are here, so it would show no icons.");
        return result;
    }
    const QString root = m_env.dataHome + "/icons";
    result.destination = root + "/" + result.id;
    if (QFileInfo::exists(result.destination) || QFileInfo(result.destination).isSymLink()) {
        result.error = tr("A pack called %1 is already in your icons folder, and it was left as it is.").arg(result.id);
        return result;
    }
    for (const QString &other : IconThemes::searchRoots(m_env))
        if (other != root && QFileInfo(other + "/" + result.id).isDir())
            result.shadows << other + "/" + result.id;
    for (const QString &parent : pack.inherits)
        if (parent != QLatin1String("hicolor") && !IconThemes::load(m_env, parent).found)
            result.missingParents << parent;
    result.ok = true;
    return result;
}

void IconThemeImporter::choose(const QUrl &folder) {
    if (!folder.isLocalFile()) {
        reject(tr("Only a folder or archive on this computer can be imported."));
        return;
    }
    choosePath(folder.toLocalFile());
}

void IconThemeImporter::choosePath(const QString &folder) {
    // A second drop while one is copying must not start another.
    if (busy())
        return;
    if (QFileInfo(folder).isFile() && IconArchive::isArchive(folder)) {
        startArchive(folder);
        return;
    }
    m_inspection = inspect(folder);
    if (!m_inspection.ok) {
        setState(QStringLiteral("failed"), m_inspection.error);
        return;
    }
    if (!m_inspection.shadows.isEmpty()) {
        setState(QStringLiteral("confirm"), tr("%1 is also installed at %2. Your imported copy will be used in its place.").arg(m_inspection.name, m_inspection.shadows.first()));
        return;
    }
    start();
}

void IconThemeImporter::confirm() {
    if (m_state != QLatin1String("confirm"))
        return;
    if (m_batch.active)
        nextFromArchive();
    else
        start();
}

void IconThemeImporter::cancel() {
    if (m_cancel)
        m_cancel->store(true);
    if (m_state == QLatin1String("confirm")) {
        if (m_batch.active)
            finishArchive({}, true);
        else
            setState(QStringLiteral("idle"), {});
    }
}

void IconThemeImporter::dismiss() {
    if (!busy())
        setState(QStringLiteral("idle"), {});
}

void IconThemeImporter::reject(const QString &message) {
    if (!busy())
        setState(QStringLiteral("failed"), message);
}

void IconThemeImporter::start() {
    // Checked again now, since time may have passed waiting for a confirmation.
    const QString source = m_inspection.source;
    const QStringList siblings = m_inspection.siblings;
    m_inspection = inspect(source);
    m_inspection.siblings = siblings;
    if (!m_inspection.ok) {
        if (m_batch.active) {
            m_batch.notes << m_inspection.error;
            nextFromArchive();
            return;
        }
        setState(QStringLiteral("failed"), m_inspection.error);
        return;
    }
    if (!lockImports())
        return;
    m_progress.store(0);
    emit progressChanged();
    m_cancel = std::make_shared<std::atomic_bool>(false);
    setState(QStringLiteral("copying"), tr("Importing %1…").arg(m_inspection.name));

    const Inspection plan = m_inspection;
    const QString staging = QFileInfo(plan.destination).absolutePath() + "/" + StagingPrefix + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
    const auto cancelFlag = m_cancel;
    auto outcome = std::make_shared<QString>();
    const QString cacheTool = m_env.findExecutable(QStringLiteral("gtk-update-icon-cache"));
    m_worker = QThread::create([this, plan, staging, cacheTool, cancelFlag, outcome] {
        *outcome = stageTree(plan, staging, cacheTool, *cancelFlag, [this](qreal value) {
            m_progress.store(value);
            QMetaObject::invokeMethod(this, &IconThemeImporter::progressChanged, Qt::QueuedConnection);
        });
        // Removes only this run's own folder, and only because this run made it.
        if (!outcome->isEmpty())
            QDir(staging).removeRecursively();
    });
    connect(m_worker, &QThread::finished, this, [this, outcome, cancelFlag, staging, plan] {
        m_worker->deleteLater();
        m_worker = nullptr;
        if (!outcome->isEmpty()) {
            conclude(false, cancelFlag->load(), *outcome);
            return;
        }
        ChangeCoordinator::Request request;
        request.settingId = QStringLiteral("appearance.icons.import");
        request.label = QStringLiteral("Import an icon pack");
        request.kind = QStringLiteral("action");
        request.after = plan.id;
        request.recovery = QStringLiteral("no-undo");
        request.nothingToRestore = true;
        QJsonObject record;
        record["source"] = plan.source;
        record["destination"] = plan.destination;
        record["staging"] = staging;
        request.extra["import"] = record;
        request.apply = [staging, plan](ChangeCoordinator::Done done) {
            const QString error = publish(staging, plan);
            done(error.isEmpty(), error);
        };
        m_coordinator->submit(std::move(request), [this, staging](const Transaction &tx) {
            const bool ok = tx.phase == QLatin1String("committed");
            if (!ok)
                QDir(staging).removeRecursively();
            conclude(ok, false, tx.message);
        });
    });
    m_worker->start();
}

bool IconThemeImporter::lockImports() {
    // Copying can take a while, so it does not hold the lock other changes wait on, and a
    // theme change meanwhile still gets its turn. This lock is only for imports. It has no
    // age limit, so a long copy is never mistaken for one that stopped. An archive holds it
    // from unpacking to its last pack.
    if (m_importLock)
        return true;
    m_importLock = std::make_unique<QLockFile>(m_env.appStateDir + "/icon-import.lock");
    m_importLock->setStaleLockTime(0);
    if (m_importLock->tryLock(0))
        return true;
    m_importLock.reset();
    setState(QStringLiteral("failed"), tr("Another icon pack is being imported. Try again when it is done."));
    emit finished(false);
    return false;
}

void IconThemeImporter::startArchive(const QString &path) {
    if (!lockImports())
        return;
    m_batch = Batch{};
    m_batch.active = true;
    m_batch.name = QFileInfo(path).fileName();
    m_batch.folder = m_env.appCacheDir + "/" + StagingPrefix + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
    QDir().mkpath(m_env.appCacheDir);
    m_progress.store(0);
    emit progressChanged();
    m_cancel = std::make_shared<std::atomic_bool>(false);
    setState(QStringLiteral("extracting"), tr("Unpacking %1…").arg(m_batch.name));
    const QString folder = m_batch.folder;
    const auto cancelFlag = m_cancel;
    auto unpacked = std::make_shared<IconArchive::Result>();
    m_worker = QThread::create([this, path, folder, cancelFlag, unpacked] {
        *unpacked = IconArchive::extract(path, folder, *cancelFlag, [this](qreal value) {
            m_progress.store(value);
            QMetaObject::invokeMethod(this, &IconThemeImporter::progressChanged, Qt::QueuedConnection);
        });
    });
    connect(m_worker, &QThread::finished, this, [this, unpacked, cancelFlag] {
        m_worker->deleteLater();
        m_worker = nullptr;
        if (!unpacked->error.isEmpty() || cancelFlag->load()) {
            finishArchive(unpacked->error, cancelFlag->load());
            return;
        }
        m_batch.queue = unpacked->packs;
        m_batch.ids = unpacked->ids;
        m_batch.notes = unpacked->notes;
        // Asked once for the whole archive, before anything is installed.
        QStringList shadowed;
        for (const QString &folder : std::as_const(m_batch.queue)) {
            const Inspection look = inspect(folder);
            if (look.ok && !look.shadows.isEmpty())
                shadowed << tr("%1 is also installed at %2.").arg(look.name, look.shadows.first());
        }
        if (!shadowed.isEmpty()) {
            setState(QStringLiteral("confirm"), shadowed.join(' ') + " " + tr("The copies from this archive will be used in their place."));
            return;
        }
        nextFromArchive();
    });
    m_worker->start();
}

void IconThemeImporter::nextFromArchive() {
    while (!m_batch.queue.isEmpty()) {
        m_inspection = inspect(m_batch.queue.takeFirst());
        if (!m_inspection.ok) {
            m_batch.notes << m_inspection.error;
            continue;
        }
        m_inspection.siblings = m_batch.ids;
        start();
        return;
    }
    finishArchive({}, false);
}

void IconThemeImporter::finishArchive(const QString &error, bool cancelled) {
    QDir(m_batch.folder).removeRecursively();
    m_importLock.reset();
    const Batch batch = m_batch;
    m_batch = Batch{};
    QStringList names;
    for (const Inspection &pack : batch.imported)
        names << pack.name;
    QString message;
    if (!names.isEmpty()) {
        // "Use this pack" offers the first, which is the one the others build on.
        m_inspection = batch.imported.first();
        message = tr("Imported %1 from %2.").arg(names.join(", "), batch.name);
    }
    if (!batch.notes.isEmpty())
        message += (message.isEmpty() ? QString() : QStringLiteral(" ")) + batch.notes.join(' ');
    if (cancelled)
        setState(QStringLiteral("idle"), names.isEmpty() ? tr("Import cancelled.") : tr("Import cancelled after %1.").arg(names.join(", ")));
    else if (!names.isEmpty())
        setState(QStringLiteral("imported"), message);
    else
        setState(QStringLiteral("failed"), error.isEmpty() ? message : error);
    emit finished(!names.isEmpty());
}

void IconThemeImporter::conclude(bool ok, bool cancelled, const QString &error) {
    if (m_batch.active) {
        if (ok) {
            m_batch.imported << m_inspection;
            if (!m_inspection.missingParents.isEmpty())
                m_batch.notes << tr("%1 builds on %2, which is not installed, so some icons may be missing.").arg(m_inspection.name, m_inspection.missingParents.join(", "));
        } else if (!cancelled) {
            m_batch.notes << tr("%1 was not imported: %2").arg(m_inspection.name, error);
        }
        if (cancelled) {
            finishArchive({}, true);
            return;
        }
        nextFromArchive();
        return;
    }
    m_importLock.reset();
    if (ok) {
        QString message = tr("Imported %1.").arg(m_inspection.name);
        if (!m_inspection.shadows.isEmpty())
            message += " " + tr("It takes the place of the copy at %1.").arg(m_inspection.shadows.first());
        if (!m_inspection.missingParents.isEmpty())
            message += " " + tr("It builds on %1, which is not installed, so some icons may be missing.").arg(m_inspection.missingParents.join(", "));
        // Marked hidden by its author: there to build on, and not one to choose.
        if (m_inspection.hidden)
            message += " " + tr("Its author marked it hidden, so it is not offered as a choice.");
        setState(QStringLiteral("imported"), message);
    } else if (cancelled) {
        setState(QStringLiteral("idle"), tr("Import cancelled."));
    } else {
        setState(QStringLiteral("failed"), error);
    }
    emit finished(ok);
}

QStringList IconThemeImporter::recoverStaging() {
    QStringList messages;
    // Unpacked archives live in this app's own cache folder, which nothing else writes to.
    // One left by a run that stopped is removed, unless an import is running right now.
    const QStringList unpacked = QDir(m_env.appCacheDir).entryList({QString::fromLatin1(StagingPrefix) + "*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    if (!unpacked.isEmpty()) {
        QLockFile lock(m_env.appStateDir + "/icon-import.lock");
        lock.setStaleLockTime(0);
        if (lock.tryLock(0))
            for (const QString &name : unpacked)
                QDir(m_env.appCacheDir + "/" + name).removeRecursively();
    }
    const QString root = m_env.dataHome + "/icons";
    const QStringList leftovers = QDir(root).entryList({QString::fromLatin1(StagingPrefix) + "*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    if (leftovers.isEmpty())
        return messages;
    // Another window or the CLI may be importing right now, and its folder looks just the same.
    QLockFile lock(m_env.appStateDir + "/icon-import.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        return messages;
    for (const QString &name : leftovers) {
        const QString staging = root + "/" + name;
        QByteArray contents;
        FileOps::readFile(staging + "/" + OwnerFile, &contents);
        const QJsonObject paths = QJsonDocument::fromJson(contents).object().value("paths").toObject();
        bool unchanged = !paths.isEmpty();
        QDirIterator it(staging, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot | QDir::System, QDirIterator::Subdirectories);
        while (unchanged && it.hasNext()) {
            it.next();
            const QString relative = it.filePath().mid(staging.size() + 1);
            if (relative == QLatin1String(OwnerFile))
                continue;
            const auto owned = paths.constFind(relative);
            if (owned == paths.constEnd() || (it.fileInfo().isFile() && !it.fileInfo().isSymLink() && it.fileInfo().size() > qint64(owned->toDouble())))
                unchanged = false;
        }
        if (unchanged && QDir(staging).removeRecursively())
            messages << tr("An icon pack import was interrupted. Its unfinished copy was removed, and nothing else changed.");
        else
            messages << tr("An icon pack import was interrupted, and its unfinished copy at %1 was left for you to review.").arg(staging);
    }
    return messages;
}
