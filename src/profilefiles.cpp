#include "profilefiles.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include "fileops.h"

namespace {

QString tr(const char *text) {
    return QCoreApplication::translate("ProfileFiles", text);
}

const QRegularExpression &digestName() {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern;
}

}  // namespace

namespace ProfileFiles {

QString assetPath(const QString &directory, const QString &sha256) {
    return QDir(directory).filePath("assets/sha256/" + sha256);
}

bool describe(const QString &path, QString *sha256, qint64 *size, QString *error) {
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile()) {
        if (error)
            *error = tr("%1 is not a plain file.").arg(info.fileName());
        return false;
    }
    if (info.size() > ProfileModel::MaxFileBytes) {
        if (error)
            *error = tr("%1 is larger than 25 MiB, so a profile does not carry it.").arg(info.fileName());
        return false;
    }
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) {
        if (error)
            *error = tr("%1 could not be read.").arg(info.fileName());
        return false;
    }
    *sha256 = QString::fromLatin1(hash.result().toHex());
    *size = info.size();
    return true;
}

bool store(const QString &directory, const QString &source, const QString &sha256, QString *error) {
    if (!digestName().match(sha256).hasMatch()) {
        if (error)
            *error = tr("That is not a SHA-256 digest.");
        return false;
    }
    const QString target = assetPath(directory, sha256);
    QString existing;
    qint64 size = 0;
    if (QFileInfo::exists(target) && describe(target, &existing, &size) && existing == sha256)
        return true;
    QString actual;
    if (!describe(source, &actual, &size, error))
        return false;
    // The file may have changed since it was read for the capture.
    if (actual != sha256) {
        if (error)
            *error = tr("%1 changed while it was being saved. Capture again.").arg(QFileInfo(source).fileName());
        return false;
    }
    QByteArray bytes;
    if (!FileOps::readFile(source, &bytes, error))
        return false;
    return FileOps::atomicWrite(target, bytes, error, 0644);
}

bool read(const QString &directory, const QJsonObject &record, QByteArray *bytes, QString *error) {
    const QString sha256 = record.value("sha256").toString();
    if (!digestName().match(sha256).hasMatch()) {
        if (error)
            *error = tr("That is not a SHA-256 digest.");
        return false;
    }
    const QString path = assetPath(directory, sha256);
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile()) {
        if (error)
            *error = tr("The profile's copy of %1 is missing.").arg(record.value("name").toString());
        return false;
    }
    if (info.size() != qint64(record.value("size").toDouble()) || info.size() > ProfileModel::MaxFileBytes) {
        if (error)
            *error = tr("The profile's copy of %1 is not the size it was saved at.").arg(record.value("name").toString());
        return false;
    }
    QByteArray contents;
    if (!FileOps::readFile(path, &contents, error))
        return false;
    if (QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex()) != sha256) {
        if (error)
            *error = tr("The profile's copy of %1 does not match its digest.").arg(record.value("name").toString());
        return false;
    }
    *bytes = contents;
    return true;
}

void prune(const QString &directory, const Profile &profile) {
    QSet<QString> named;
    for (const QJsonValue &asset : profile.assets)
        named.insert(asset.toObject().value("sha256").toString());
    const QDir store(QDir(directory).filePath("assets/sha256"));
    for (const QFileInfo &file : store.entryInfoList(QDir::Files | QDir::Hidden | QDir::System)) {
        if (digestName().match(file.fileName()).hasMatch() && !file.isSymLink() && !named.contains(file.fileName()))
            QFile::remove(file.absoluteFilePath());
    }
}

bool configAdapter(const Env &env, const QString &id, ConfigAdapter *adapter) {
    if (id == QLatin1String("ghostty")) {
        *adapter = ConfigAdapter{id, QStringLiteral("config"), env.configHome + "/ghostty/config", env.omarchyPath + "/config/ghostty/config"};
        return true;
    }
    if (id == QLatin1String("alacritty")) {
        *adapter = ConfigAdapter{id, QStringLiteral("alacritty.toml"), env.configHome + "/alacritty/alacritty.toml",
                                 env.omarchyPath + "/config/alacritty/alacritty.toml"};
        return true;
    }
    return false;
}

QString reviewConfig(const QString &adapter, const QByteArray &contents) {
    const QString text = QString::fromUtf8(contents);
    if (text.contains(QRegularExpression(QStringLiteral("(^|[\\s\"'=])/home/"))))
        return tr("It names a path inside a home folder, which is another path on another computer.");
    if (adapter == QLatin1String("ghostty")) {
        // Ghostty's own theme include, which Omarchy writes, is the one file it may load.
        static const QString theme = QStringLiteral("~/.local/state/omarchy/current/theme/ghostty.conf");
        static const QStringList runs{"command", "initial-command", "custom-shader", "background-image", "bell-audio-path"};
        for (QString line : text.split('\n')) {
            line = line.trimmed();
            if (line.isEmpty() || line.startsWith('#') || !line.contains('='))
                continue;
            const QString key = line.section('=', 0, 0).trimmed();
            QString value = line.section('=', 1).trimmed();
            if (key == QLatin1String("config-file")) {
                value.remove(QRegularExpression(QStringLiteral("^\\?")));
                value.remove('"');
                if (value != theme)
                    return tr("It loads %1, which a profile does not carry.").arg(value);
            } else if (runs.contains(key)) {
                return tr("Its %1 setting runs a program or loads a file, which a profile does not carry yet.").arg(key);
            }
        }
        return {};
    }
    if (adapter == QLatin1String("alacritty")) {
        static const QString theme = QStringLiteral("~/.local/state/omarchy/current/theme/alacritty.toml");
        static const QRegularExpression import(QStringLiteral("^\\s*(general\\.)?import\\s*=\\s*\\[([^\\]]*)\\]"), QRegularExpression::MultilineOption);
        auto imports = import.globalMatch(text);
        while (imports.hasNext()) {
            for (QString entry : imports.next().captured(2).split(',', Qt::SkipEmptyParts)) {
                entry = entry.trimmed();
                entry.remove('"');
                entry.remove('\'');
                if (!entry.isEmpty() && entry != theme)
                    return tr("It imports %1, which a profile does not carry.").arg(entry);
            }
        }
        static const QRegularExpression runs(QStringLiteral("^\\s*\\[(terminal\\.)?shell\\]|^\\s*shell\\s*=|\\bcommand\\s*=|\\bprogram\\s*="),
                                             QRegularExpression::MultilineOption);
        if (runs.match(text).hasMatch())
            return tr("It names a program to run, which a profile does not carry yet.");
        return {};
    }
    return tr("Omarchy Control has no adapter for this file.");
}

QString configPath(const QString &directory, const QString &adapter, const QString &name) {
    return QDir(directory).filePath("files/" + adapter + "/" + name);
}

bool storeConfig(const QString &directory, const QString &adapter, const QString &name, const QByteArray &contents, QString *error) {
    if (!ProfileModel::fileAdapters().contains(adapter) || name.contains('/') || name.startsWith('.')) {
        if (error)
            *error = tr("That is not a file a profile carries.");
        return false;
    }
    return FileOps::atomicWrite(configPath(directory, adapter, name), contents, error, 0644);
}

bool readConfig(const QString &directory, const QJsonObject &record, QByteArray *contents, QString *error) {
    const QString adapter = record.value("adapter").toString(), name = record.value("name").toString();
    if (!ProfileModel::fileAdapters().contains(adapter) || name.contains('/') || name.startsWith('.')) {
        if (error)
            *error = tr("That is not a file a profile carries.");
        return false;
    }
    const QString path = configPath(directory, adapter, name);
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile()) {
        if (error)
            *error = tr("The profile's copy of %1 is missing.").arg(name);
        return false;
    }
    QByteArray bytes;
    if (info.size() != qint64(record.value("size").toDouble()) || !FileOps::readFile(path, &bytes, error)) {
        if (error && error->isEmpty())
            *error = tr("The profile's copy of %1 is not the size it was saved at.").arg(name);
        return false;
    }
    if (QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) != record.value("sha256").toString()) {
        if (error)
            *error = tr("The profile's copy of %1 does not match its digest.").arg(name);
        return false;
    }
    *contents = bytes;
    return true;
}

void pruneConfigs(const QString &directory, const Profile &profile) {
    QSet<QString> named;
    for (const QJsonValue &file : profile.files)
        named.insert(file.toObject().value("adapter").toString() + "/" + file.toObject().value("name").toString());
    for (const QString &adapter : ProfileModel::fileAdapters()) {
        const QDir folder(QDir(directory).filePath("files/" + adapter));
        for (const QFileInfo &file : folder.entryInfoList(QDir::Files | QDir::Hidden | QDir::System))
            if (!file.isSymLink() && !named.contains(adapter + "/" + file.fileName()))
                QFile::remove(file.absoluteFilePath());
    }
}

}  // namespace ProfileFiles
