#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include "catalog.h"
#include "env.h"

// Which commands a setting's write was reviewed against. If Omarchy later changes
// one, that setting keeps reading but stops writing until someone looks: every
// adapter here encodes what a specific script does, and a changed script may no
// longer do it. Only the settings behind the changed command pause.
class Compat {
public:
    explicit Compat(const Env &env, const QString &directory = QStringLiteral(":/data"));

    // Empty if this entry may write; otherwise what changed.
    QString changedFor(const CatalogEntry &entry, const QString &hyprlandVersion) const;
    // The same for the commands reviewed under a key that is not a setting, such as a
    // profile adapter's "profile.themes".
    QString changedForKey(const QString &key) const;
    QStringList changedCommands(const QString &hyprlandVersion) const;
    void acceptAll(const QString &hyprlandVersion);
    QString baseline() const;

private:
    QString currentHash(const QString &binary) const;
    bool isAccepted(const QString &key, const QString &value) const;
    bool hyprlandDiffers(const QString &version) const;
    void loadAccepted();

    Env m_env;
    QHash<QString, QStringList> m_settingToBinaries;
    QHash<QString, QString> m_reviewed;
    QHash<QString, QString> m_accepted;
    QString m_baselineOmarchy, m_baselineHyprland;
    mutable QHash<QString, QString> m_hashes;
};
