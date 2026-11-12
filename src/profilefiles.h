#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include "env.h"
#include "profilemodel.h"

// The files a profile carries itself, such as a background or a launcher's icon, kept in
// the profile under their SHA-256: assets/sha256/<digest>. A file is stored only from a
// plain local file within the size limit, and read back only after its size and digest
// match what profile.json records. Nothing a profile holds is ever run or made executable,
// and each file goes only where the adapter that uses it puts it.
namespace ProfileFiles {

QString assetPath(const QString &directory, const QString &sha256);

// The digest and size of a plain file, refusing links, devices, folders, and anything over
// ProfileModel::MaxFileBytes.
bool describe(const QString &path, QString *sha256, qint64 *size, QString *error = nullptr);

// Copies a file into the profile's store under its digest. Storing the same file twice
// changes nothing.
bool store(const QString &directory, const QString &source, const QString &sha256, QString *error = nullptr);

// A stored file's bytes, once it is a plain file with the size and digest recorded.
bool read(const QString &directory, const QJsonObject &record, QByteArray *bytes, QString *error = nullptr);

// Removes stored files the profile no longer names. Only plain files named like digests
// are ever touched.
void prune(const QString &directory, const Profile &profile);

// The configuration files a profile may carry. Each adapter knows the one place its file
// lives and what in it cannot travel; a profile only ever supplies the contents.
struct ConfigAdapter {
    QString id;           // ghostty, alacritty
    QString name;         // The file's name in the profile and on disk.
    QString destination;  // Where it lives on this computer.
    QString omarchyDefault;
};
bool configAdapter(const Env &env, const QString &id, ConfigAdapter *adapter);
// Why these contents cannot travel, or empty when they can: an include other than Omarchy's
// own theme file, anything that runs a program or loads another file, or a path inside
// someone's home folder, which is another path on another computer.
QString reviewConfig(const QString &adapter, const QByteArray &contents);
QString configPath(const QString &directory, const QString &adapter, const QString &name);
bool storeConfig(const QString &directory, const QString &adapter, const QString &name, const QByteArray &contents, QString *error = nullptr);
// The profile's copy, once it has the size and digest recorded.
bool readConfig(const QString &directory, const QJsonObject &record, QByteArray *contents, QString *error = nullptr);
void pruneConfigs(const QString &directory, const Profile &profile);

}  // namespace ProfileFiles
