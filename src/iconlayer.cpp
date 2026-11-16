#include "iconlayer.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>

#include "fileops.h"
#include "iconthemes.h"

namespace {

constexpr int Version = 1;
// The launcher draws icons at 56 points and decodes them at twice that, so a lookup aims
// for 128 pixels and takes a vector wherever the pack has one.
constexpr int TargetSize = 128;
// Raster icons are embedded in the layer's SVG, so they are kept to a sensible size.
constexpr int MaxRasterSide = 256;
constexpr qint64 MaxSvgBytes = 2 * 1024 * 1024;
constexpr qint64 MaxRasterBytes = 8 * 1024 * 1024;
constexpr qint64 MaxDesktopBytes = 512 * 1024;
const char Owner[] = "omarchy-control";

QString tr(const char *text) {
    return QCoreApplication::translate("IconLayer", text);
}

QString iconsRoot(const Env &env) {
    return env.home + "/.icons";
}

QString nextFolder(const Env &env) {
    return iconsRoot(env) + "/.omarchy-control-next";
}

QString manifestPath(const QString &folder) {
    return folder + "/manifest.json";
}

QStringList applicationRoots(const Env &env) {
    QStringList roots{env.dataHome + "/applications"};
    for (const QString &directory : env.dataDirs)
        roots << directory + "/applications";
    roots.removeDuplicates();
    return roots;
}

// Only folders the launcher's own index reads: app and device icons. A pack's action icon
// of the same name, such as "zoom", is a different thing and must not stand in for an app.
bool appContext(const IconPack::Directory &directory) {
    const QString context = directory.context.toLower();
    if (!context.isEmpty())
        return context == QLatin1String("applications") || context == QLatin1String("apps") || context == QLatin1String("devices");
    const QStringList parts = directory.subdir.toLower().split('/');
    return parts.contains(QStringLiteral("apps")) || parts.contains(QStringLiteral("applications")) || parts.contains(QStringLiteral("devices"));
}

// Every app icon a pack has, name to file, nearest size first and SVG before PNG within a
// folder, as a lookup would find them. Listing each folder once keeps a whole layer cheap.
QHash<QString, QString> indexPack(const IconPack &pack, QStringList *diagnostics = nullptr, int *remaining = nullptr) {
    QHash<QString, QString> index;
    for (const IconPack::Directory &directory : IconThemes::directoriesFor(pack, TargetSize)) {
        if (!appContext(directory))
            continue;
        for (const QString &location : pack.locations) {
            const QDir folder(location + "/" + directory.subdir);
            if (remaining) {
                // Empty or missing directories also cost a visit. Count them
                // so a wide inheritance tree cannot evade the entry budget.
                if (*remaining <= 0) return index;
                --*remaining;
                const auto files = IconThemes::directoryNames(folder.path(), remaining, diagnostics);
                // Keep the same nearest-directory, location and SVG-first order
                // as the layer builder, without silently dropping unreadable files.
                for (const char *extension : {".svg", ".png"}) {
                    for (const QString &file : files) {
                        if (file.startsWith('.') || !file.endsWith(QLatin1String(extension), Qt::CaseInsensitive)) continue;
                        const QFileInfo info(folder.filePath(file));
                        if (info.isDir()) continue;
                        if (!info.isFile() || !info.isReadable()) {
                            if (diagnostics) diagnostics->append("Icon is not a readable regular file: " + info.filePath());
                            continue;
                        }
                        const QString name = file.left(file.lastIndexOf('.'));
                        if (!index.contains(name)) index.insert(name, info.filePath());
                    }
                }
                if (*remaining <= 0) return index;
                continue;
            }
            if (!folder.exists())
                continue;
            for (const char *pattern : {"*.svg", "*.png"}) {
                for (const QString &file : folder.entryList({QLatin1String(pattern)}, QDir::Files | QDir::Readable)) {
                    const QString name = file.left(file.lastIndexOf('.'));
                    if (!index.contains(name))
                        index.insert(name, folder.filePath(file));
                }
            }
        }
    }
    return index;
}

QString baseName(const QString &icon) {
    static const QStringList extensions{".svg", ".png", ".xpm"};
    for (const QString &extension : extensions)
        if (icon.endsWith(extension, Qt::CaseInsensitive))
            return icon.left(icon.size() - extension.size());
    return icon;
}

// The name as written, then the last part of a reverse-DNS name, then both in lower case:
// org.mozilla.firefox is called firefox in most packs.
QStringList candidates(const QString &icon) {
    const QString base = baseName(icon);
    QStringList names{base};
    const QString last = base.section('.', -1);
    if (base.contains('.') && !last.isEmpty())
        names << last;
    names << base.toLower();
    if (base.contains('.') && !last.isEmpty())
        names << last.toLower();
    names.removeDuplicates();
    return names;
}

struct Source {
    QString kind, from, path;
};

QByteArray wrapRaster(const QString &path) {
    if (QFileInfo(path).size() > MaxRasterBytes)
        return {};
    QImageReader reader(path);
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > 4096 || size.height() > 4096)
        return {};
    QImage image = reader.read();
    if (image.isNull())
        return {};
    if (image.width() > MaxRasterSide || image.height() > MaxRasterSide)
        image = image.scaled(MaxRasterSide, MaxRasterSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG"))
        return {};
    const QByteArray width = QByteArray::number(image.width()), height = QByteArray::number(image.height());
    // An SVG, so it is indexed before every PNG anywhere: the shell lists SVGs first.
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"" + width + "\" height=\"" + height
           + "\" viewBox=\"0 0 " + width + " " + height + "\"><image width=\"" + width + "\" height=\"" + height
           + "\" xlink:href=\"data:image/png;base64," + png.toBase64() + "\"/></svg>\n";
}

QByteArray layerBytes(const QString &path) {
    if (!path.endsWith(QLatin1String(".svg"), Qt::CaseInsensitive))
        return wrapRaster(path);
    if (QFileInfo(path).size() > MaxSvgBytes)
        return {};
    QByteArray bytes;
    if (!FileOps::readFile(path, &bytes) || !bytes.contains("<svg"))
        return {};
    return bytes;
}

bool writeBytes(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

QJsonObject readManifest(const QString &folder) {
    QByteArray contents;
    if (!FileOps::readFile(manifestPath(folder), &contents))
        return {};
    return QJsonDocument::fromJson(contents).object();
}

bool ours(const QString &folder) {
    if (!QFileInfo(folder).isDir())
        return false;
    if (readManifest(folder).value("owner").toString() == QLatin1String(Owner))
        return true;
    // An empty folder is what a build that stopped early leaves.
    return QDir(folder).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
}

// Swaps two folders in one step, so the shell sees the old layer or the new one and never
// neither. Filesystems without the exchange fall back to two renames.
bool exchange(const QString &a, const QString &b) {
    if (::renameat2(AT_FDCWD, QFile::encodeName(a).constData(), AT_FDCWD, QFile::encodeName(b).constData(), RENAME_EXCHANGE) == 0)
        return true;
    if (errno != EINVAL && errno != ENOSYS)
        return false;
    const QString old = b + ".old";
    QDir(old).removeRecursively();
    if (!QDir().rename(b, old))
        return false;
    if (!QDir().rename(a, b)) {
        QDir().rename(old, b);
        return false;
    }
    return QDir().rename(old, a);
}

struct IconicItem {
    const char *key;
    QString path;
};

QList<IconicItem> iconicItems(const Env &env) {
    return {{"layer", env.home + "/.icons/iconic-apps"},
            {"hook", env.configHome + "/omarchy/hooks/theme-set.d/iconic"},
            {"config", env.configHome + "/iconic"}};
}

QString retiredRoot(const Env &env) {
    return env.appStateDir + "/retired/iconic";
}

}  // namespace

namespace IconLayer {

QList<Launcher> launchers(const Env &env) {
    QList<Launcher> list;
    QSet<QString> seen;
    for (const QString &root : applicationRoots(env)) {
        QDirIterator it(root, {"*.desktop"}, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
        QStringList paths;
        while (it.hasNext())
            paths << it.next();
        paths.sort();
        for (const QString &path : std::as_const(paths)) {
            // Files in subfolders are known by their path with dashes, as the spec says.
            const QString id = QDir(root).relativeFilePath(path).replace('/', '-');
            if (seen.contains(id))
                continue;
            seen.insert(id);
            QByteArray contents;
            if (QFileInfo(path).size() > MaxDesktopBytes || !FileOps::readFile(path, &contents))
                continue;
            Launcher launcher;
            launcher.id = id;
            QString type;
            bool inEntry = false, hidden = false;
            for (const QByteArray &raw : contents.split('\n')) {
                const QString line = QString::fromUtf8(raw).trimmed();
                if (line.startsWith('[')) {
                    inEntry = line == QLatin1String("[Desktop Entry]");
                    continue;
                }
                if (!inEntry)
                    continue;
                const qsizetype equals = line.indexOf('=');
                if (equals <= 0)
                    continue;
                const QString key = line.left(equals).trimmed(), value = line.mid(equals + 1).trimmed();
                if (key == QLatin1String("Type"))
                    type = value;
                else if (key == QLatin1String("Name") && launcher.name.isEmpty())
                    launcher.name = value;
                else if (key == QLatin1String("Icon") && launcher.icon.isEmpty())
                    launcher.icon = value;
                else if (key == QLatin1String("NoDisplay"))
                    launcher.visible = value != QLatin1String("true");
                else if (key == QLatin1String("Hidden"))
                    hidden = value == QLatin1String("true");
            }
            // Hidden means deleted, and it hides any copy further down as well.
            if (hidden || type != QLatin1String("Application"))
                continue;
            list << launcher;
        }
    }
    return list;
}

QString folder(const Env &env) {
    return iconsRoot(env) + "/omarchy-control";
}

QString settingsPath(const Env &env) {
    return env.appConfigDir + "/icon-layer.json";
}

QString statePath(const Env &env) {
    return env.appStateDir + "/icon-layer-state.json";
}

bool enabled(const Env &env) {
    QByteArray contents;
    if (!FileOps::readFile(settingsPath(env), &contents))
        return true;
    const QJsonValue value = QJsonDocument::fromJson(contents).object().value("enabled");
    return !(value == QJsonValue(false) || value == QJsonValue(QStringLiteral("false")));
}

QString incompatibility(const Env &env) {
    QByteArray text;
    if (!FileOps::readFile(env.omarchyPath + "/shell/services/AppLibrary.qml", &text))
        return tr("Omarchy's launcher was not found, so there is nothing for these icons to change.");
    // What the layer relies on: ~/.icons is read first, only app and device folders count,
    // SVGs are listed before PNGs, and the index is asked before the themed lookup.
    const bool homeFirst = text.contains("dirs=\"$HOME/.icons $HOME/.local/share/icons\"");
    const bool appsOnly = text.contains("\"*/apps/*\"");
    const bool svgFirst = text.contains(".svg$/ { print; next }");
    const qsizetype function = text.indexOf("function iconSource(");
    const qsizetype index = function < 0 ? -1 : text.indexOf("root.iconIndex[value]", function);
    const qsizetype themed = function < 0 ? -1 : text.indexOf("Quickshell.iconPath(value, true)", function);
    if (homeFirst && appsOnly && svgFirst && index > function && themed > index)
        return {};
    return tr("Omarchy changed how its launcher finds icons, so this is paused until Omarchy Control is updated for it.");
}

QString stamp(const Env &env, const QString &pack) {
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(QByteArray::number(Version) + "\n" + pack.toUtf8() + "\n");
    const auto add = [&hash](const QString &path) {
        const QFileInfo info(path);
        hash.addData(path.toUtf8() + " " + QByteArray::number(info.exists() ? info.lastModified().toMSecsSinceEpoch() : -1) + " "
                     + QByteArray::number(info.size()) + "\n");
    };
    for (const IconPack &member : IconThemes::lookupChain(env, pack)) {
        add(member.indexPath);
        for (const QString &location : member.locations)
            add(location);
    }
    for (const QString &root : applicationRoots(env)) {
        add(root);
        QDirIterator it(root, {"*.desktop"}, QDir::Files, QDirIterator::Subdirectories);
        QStringList paths;
        while (it.hasNext())
            paths << it.next();
        paths.sort();
        for (const QString &path : std::as_const(paths))
            add(path);
    }
    for (const QString &root : QStringList{env.dataHome} + env.dataDirs)
        add(root + "/pixmaps");
    return QString::fromLatin1(hash.result().toHex());
}

QHash<QString, QString> appIcons(const Env &env, const QString &pack, QStringList *diagnostics, QHash<QString, QString> *origins) {
    QHash<QString, QString> all;
    int remaining = 100000;
    for (const IconPack &member : IconThemes::lookupChain(env, pack, diagnostics)) {
        const QHash<QString, QString> index = indexPack(member, diagnostics, &remaining);
        for (auto it = index.begin(); it != index.end(); ++it)
            if (!all.contains(it.key())) {
                all.insert(it.key(), it.value());
                if (origins) origins->insert(it.key(), member.id);
            }
        if (remaining <= 0) {
            if (diagnostics) diagnostics->append("Icon search reached its total directory-entry limit");
            break;
        }
    }
    return all;
}

Build build(const Env &env, const QString &pack) {
    Build result;
    const QString live = folder(env), next = nextFolder(env);
    if (QFileInfo::exists(live) && !ours(live)) {
        result.error = tr("%1 already exists and was not made by Omarchy Control, so it was left alone.").arg(live);
        return result;
    }
    const QList<IconPack> chain = IconThemes::lookupChain(env, pack);
    if (chain.isEmpty() || chain.first().id != pack) {
        result.error = tr("%1 is not installed.").arg(pack);
        return result;
    }

    // Which launchers use each name, so the details can say which apps a gap affects.
    QHash<QString, Icon> byName;
    QStringList order;
    for (const Launcher &launcher : launchers(env)) {
        // A path is the app's own file, which the launcher shows as it is.
        if (launcher.icon.isEmpty() || !IconThemes::isValidId(launcher.icon))
            continue;
        if (!byName.contains(launcher.icon)) {
            byName.insert(launcher.icon, Icon{launcher.icon, {}, {}, {}, {}, false});
            order << launcher.icon;
        }
        Icon &icon = byName[launcher.icon];
        // The apps people see, which is what coverage counts and the details list.
        if (launcher.visible && !launcher.name.isEmpty() && !icon.apps.contains(launcher.name))
            icon.apps << launcher.name;
        icon.visible = icon.visible || launcher.visible;
    }

    QList<QHash<QString, QString>> indexes;
    for (const IconPack &member : chain)
        indexes << indexPack(member);
    QStringList pixmaps;
    for (const QString &root : QStringList{env.dataHome} + env.dataDirs)
        pixmaps << root + "/pixmaps";
    pixmaps.removeDuplicates();

    QDir(next).removeRecursively();
    const QString building = next + "/building";
    if (!QDir().mkpath(building)) {
        result.error = tr("%1 could not be created.").arg(building);
        return result;
    }
    for (const QString &name : std::as_const(order)) {
        Icon icon = byName.value(name);
        const QStringList names = candidates(name);
        QList<Source> sources;
        for (int i = 0; i < chain.size(); ++i) {
            const bool fallback = chain[i].id == QLatin1String("hicolor");
            // The app's own icon is only ever its exact name.
            for (const QString &candidate : fallback ? QStringList{baseName(name)} : names)
                if (indexes[i].contains(candidate))
                    sources << Source{fallback ? QStringLiteral("own") : QStringLiteral("pack"), chain[i].id, indexes[i].value(candidate)};
        }
        for (const QString &root : std::as_const(pixmaps))
            for (const char *extension : {".svg", ".png"})
                if (QFileInfo::exists(root + "/" + baseName(name) + QLatin1String(extension)))
                    sources << Source{QStringLiteral("own"), QString(), root + "/" + baseName(name) + QLatin1String(extension)};
        icon.kind = QStringLiteral("missing");
        for (const Source &source : std::as_const(sources)) {
            const QByteArray bytes = layerBytes(source.path);
            if (bytes.isEmpty())
                continue;
            if (!writeBytes(building + "/" + name + ".svg", bytes)) {
                result.error = tr("%1 could not be written.").arg(building + "/" + name + ".svg");
                QDir(next).removeRecursively();
                return result;
            }
            icon.kind = source.kind;
            icon.from = source.from;
            icon.source = source.path;
            break;
        }
        result.icons << icon;
    }

    QJsonArray icons;
    for (const Icon &icon : std::as_const(result.icons))
        icons << QJsonObject{{"name", icon.name}, {"kind", icon.kind}, {"from", icon.from}, {"source", icon.source},
                             {"apps", QJsonArray::fromStringList(icon.apps)}, {"visible", icon.visible}};
    const QJsonObject manifest{{"owner", QString::fromLatin1(Owner)}, {"version", Version}, {"pack", pack}, {"stamp", stamp(env, pack)},
                               {"built", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}, {"icons", icons}};
    // Only now does the folder take a name the shell indexes.
    if (!writeBytes(manifestPath(next), QJsonDocument(manifest).toJson()) || !QDir().rename(building, next + "/apps")) {
        result.error = tr("%1 could not be written.").arg(next);
        QDir(next).removeRecursively();
        return result;
    }
    const bool published = QFileInfo::exists(live) ? exchange(next, live) : QDir().rename(next, live);
    if (!published) {
        result.error = tr("The new icons could not be moved into %1.").arg(live);
        QDir(next).removeRecursively();
        return result;
    }
    // After an exchange this holds the old layer.
    QDir(next).removeRecursively();
    result.ok = true;
    return result;
}

bool exists(const Env &env) {
    return QFileInfo(folder(env)).isDir();
}

bool remove(const Env &env, QString *error) {
    QDir(nextFolder(env)).removeRecursively();
    const QString live = folder(env);
    if (!QFileInfo::exists(live))
        return true;
    if (!ours(live)) {
        if (error)
            *error = tr("%1 was not made by Omarchy Control, so it was left alone.").arg(live);
        return false;
    }
    if (!QDir(live).removeRecursively()) {
        if (error)
            *error = tr("%1 could not be removed.").arg(live);
        return false;
    }
    return true;
}

QVariantMap status(const Env &env) {
    const QJsonObject manifest = readManifest(folder(env));
    if (manifest.value("owner").toString() != QLatin1String(Owner))
        return {{"built", false}};
    int total = 0, fromPack = 0, own = 0, missing = 0;
    QVariantList gaps;
    for (const QJsonValue &value : manifest.value("icons").toArray()) {
        const QJsonObject icon = value.toObject();
        // Coverage is about what people see: launchers that are shown.
        if (!icon.value("visible").toBool())
            continue;
        // Apps, not icon names: several apps can share one icon.
        const int apps = std::max(1, int(icon.value("apps").toArray().size()));
        total += apps;
        const QString kind = icon.value("kind").toString();
        if (kind == QLatin1String("pack")) {
            fromPack += apps;
            continue;
        }
        (kind == QLatin1String("own") ? own : missing) += apps;
        gaps << QVariantMap{{"name", icon.value("name").toString()}, {"kind", kind}, {"apps", icon.value("apps").toVariant()}};
    }
    std::sort(gaps.begin(), gaps.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("apps").toStringList().value(0).localeAwareCompare(b.toMap().value("apps").toStringList().value(0)) < 0;
    });
    return {{"built", true},
            {"pack", manifest.value("pack").toString()},
            {"stamp", manifest.value("stamp").toString()},
            {"builtAt", manifest.value("built").toString()},
            {"total", total},
            {"fromPack", fromPack},
            {"own", own},
            {"missing", missing},
            {"gaps", gaps}};
}

QStringList iconicPaths(const Env &env) {
    QStringList paths;
    for (const IconicItem &item : iconicItems(env))
        if (QFileInfo::exists(item.path) || QFileInfo(item.path).isSymLink())
            paths << item.path;
    return paths;
}

bool iconicRetired(const Env &env) {
    return QFileInfo::exists(retiredRoot(env) + "/retired.json");
}

bool retireIconic(const Env &env, QString *error) {
    const QString root = retiredRoot(env);
    if (iconicRetired(env)) {
        *error = tr("Iconic's files from an earlier hand-over are still kept. Restore them first.");
        return false;
    }
    if (!QDir().mkpath(root)) {
        *error = tr("%1 could not be created.").arg(root);
        return false;
    }
    QJsonObject moved;
    for (const IconicItem &item : iconicItems(env)) {
        if (!QFileInfo::exists(item.path) && !QFileInfo(item.path).isSymLink())
            continue;
        const QString target = root + "/" + QLatin1String(item.key);
        if (!QDir().rename(item.path, target)) {
            // Puts back what already moved, so a failed hand-over changes nothing.
            for (auto it = moved.begin(); it != moved.end(); ++it)
                QDir().rename(root + "/" + it.key(), it.value().toString());
            *error = tr("%1 could not be moved, so nothing was changed.").arg(item.path);
            return false;
        }
        moved.insert(QLatin1String(item.key), item.path);
    }
    if (!writeBytes(root + "/retired.json", QJsonDocument(moved).toJson())) {
        *error = tr("%1 could not be written.").arg(root + "/retired.json");
        return false;
    }
    return true;
}

bool restoreIconic(const Env &env, QString *error) {
    const QString root = retiredRoot(env);
    QByteArray contents;
    if (!FileOps::readFile(root + "/retired.json", &contents)) {
        *error = tr("There is nothing of Iconic's to restore.");
        return false;
    }
    const QJsonObject moved = QJsonDocument::fromJson(contents).object();
    for (auto it = moved.begin(); it != moved.end(); ++it) {
        if (QFileInfo::exists(it.value().toString())) {
            *error = tr("%1 exists again, so Iconic's copy was kept aside.").arg(it.value().toString());
            return false;
        }
    }
    for (auto it = moved.begin(); it != moved.end(); ++it) {
        const QString original = it.value().toString();
        QDir().mkpath(QFileInfo(original).absolutePath());
        if (!QDir().rename(root + "/" + it.key(), original)) {
            *error = tr("%1 could not be put back.").arg(original);
            return false;
        }
    }
    QFile::remove(root + "/retired.json");
    QDir().rmdir(root);
    return true;
}

}  // namespace IconLayer
