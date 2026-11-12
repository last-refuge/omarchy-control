#pragma once

#include <QJsonObject>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <optional>

#include "changecoordinator.h"
#include "env.h"

class IconThemeProvider;

// Renaming an app, giving it another icon, or hiding it from the launcher, one app at a
// time. A package's launcher is never edited: a copy in the user's applications folder
// takes its place, marked as this app's, with the source it came from and that file's
// hash, so a package update can be noticed and brought in with the same changes. The
// user's own launchers, such as web apps, are edited in place after one backup. Every
// change goes through the change coordinator, so it is journaled and can be undone.
// Launcher editing follows Iconic's (github.com/UnderStackDev/omarchy_iconic, MIT).
class LauncherEditor : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList entries READ entries NOTIFY changed)

public:
    using Finished = ChangeCoordinator::Finished;

    struct Edit {
        std::optional<QString> name;
        std::optional<QString> icon;  // A theme name, or an absolute path to an image.
        std::optional<bool> hidden;
    };

    struct Entry {
        QString id, name, comment, icon, path, source;
        QString kind;  // package, own, override (a copy of a package's), orphan (its package is gone)
        bool visible = true;
        bool omarchyHidden = false;  // On Omarchy's own list of launchers it hides.
        bool outdated = false;       // The package's launcher changed since the copy was made.
        bool modified = false;       // Changed here, so Reset has something to do.
    };

    LauncherEditor(const Env &env, ChangeCoordinator *coordinator, IconThemeProvider *icons, QObject *parent = nullptr);

    QList<Entry> scan() const;
    QVariantList entries() const { return m_entries; }
    Entry find(const QString &id) const;

    // `runId` marks the change as part of a restore holding the coordinator's lease.
    void apply(const QString &id, const Edit &edit, Finished finished, const QString &runId = QString());
    void reset(const QString &id, Finished finished);
    // Takes the package's newer launcher and makes the same changes to it again.
    void update(const QString &id, Finished finished);
    void undo(const Transaction &original, Finished finished);

    // The changes recorded in a package launcher's copy: just the fields changed here.
    Edit recordedEdit(const Entry &entry) const;
    // What the launcher has now for the name, the icon, and whether it is hidden.
    QJsonObject currentFields(const Entry &entry) const;

    // An image file copied into this app's own folder, so the launcher keeps its icon if
    // the original is moved. Returns the copy's path, or empty with `error` set.
    QString importIcon(const QString &file, QString *error) const;
    // Copies no launcher uses any more are removed.
    void cleanIcons() const;

    QString userPath(const QString &id) const;
    QString backupPath(const QString &id) const;
    QString iconFolder() const;

    // For the window.
    Q_INVOKABLE void reload();
    Q_INVOKABLE void save(const QString &id, const QVariantMap &changes);
    Q_INVOKABLE void resetEntry(const QString &id);
    Q_INVOKABLE void updateEntry(const QString &id);
    Q_INVOKABLE QString chooseIcon(const QUrl &file);
    // App icons the current pack has, matching `query`, for the icon picker.
    Q_INVOKABLE QVariantList packIcons(const QString &query, int limit = 300) const;

signals:
    void changed();
    void message(const QString &text, const QString &kind);

private:
    void submit(const QString &id, const QString &label, const QString &path, const QByteArray &contents, const QStringList &extraFiles,
                const QHash<QString, QByteArray> &extraContents, Finished finished, const QString &runId = QString());
    void finishedInWindow(const Transaction &tx);
    QString currentPack() const;

    Env m_env;
    ChangeCoordinator *m_coordinator;
    IconThemeProvider *m_icons;
    QVariantList m_entries;
};
