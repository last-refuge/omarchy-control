#pragma once

#include <QByteArray>
#include <QJsonValue>
#include <QString>
#include <QStringList>

// Format-aware edits of files other programs own. Each edit changes only the
// bytes of the value it targets, so unrelated keys, ordering, and comments
// survive, and every write is an atomic replacement.
namespace FileOps {

bool readFile(const QString &path, QByteArray *contents, QString *error = nullptr);

// Replaces the target atomically, following a symlink so the link itself stays
// in place. Existing permissions are kept; new files get `newFileMode`. There is
// deliberately no direct-write fallback: failing is better than a torn config.
bool atomicWrite(const QString &path, const QByteArray &contents, QString *error = nullptr, int newFileMode = 0600);

// "missing" for an absent file, so that creating a file also counts as a change,
// and "unreadable" for one that exists but cannot be read.
QString fingerprint(const QString &path);
QString fingerprintBytes(const QByteArray &contents);

// Empty preference markers may be created/removed only below owned, unshared
// directories in this home, with no links or custom leaf content.
QString emptyMarkerError(const QString &home, const QString &path);

// JSON with // and /* */ comments and trailing commas, as Omarchy's menu
// extensions allow. These return false and set `error` rather than guess.
QByteArray stripJsonComments(const QByteArray &source);
bool jsonSet(QByteArray *document, const QStringList &path, const QJsonValue &value, QString *error = nullptr);
bool jsonRemove(QByteArray *document, const QStringList &path, QString *error = nullptr);
QJsonValue jsonGet(const QByteArray &document, const QStringList &path);
QByteArray jsonLiteral(const QJsonValue &value);

// Minimal TOML reading: bare [section] tables and single-line key = value pairs,
// which is all shell.toml overrides use. Omarchy's own command does the writing.
QString tomlGet(const QByteArray &document, const QString &section, const QString &key);

// One key of an INI file other tools own. Reading says whether the key is there and
// whether it is there more than once, which makes the file ambiguous. Setting replaces
// only the value's bytes and refuses to add a key or a group, or to guess between
// duplicates, so comments, spacing, line endings, and every other key stay as they are.
struct IniValue {
    bool found = false;
    bool ambiguous = false;
    QString value;
};
IniValue iniGet(const QByteArray &document, const QString &group, const QString &key);
bool iniSet(QByteArray *document, const QString &group, const QString &key, const QString &value, QString *error = nullptr);
// The same, except that a missing key is added: after the group's last line, or in a new
// group at the end. Other lines stay exactly as they were. Duplicates are still refused.
bool iniUpsert(QByteArray *document, const QString &group, const QString &key, const QString &value, QString *error = nullptr);

}  // namespace FileOps
