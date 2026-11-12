#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include "capabilities.h"
#include "catalog.h"
#include "changecoordinator.h"
#include "commandrunner.h"

// Read-only machine facts, the installed-app list, file associations, and the
// redacted diagnostic export.
class SystemProvider : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap machine READ machine NOTIFY changed)
    Q_PROPERTY(QVariantMap battery READ battery NOTIFY changed)
    Q_PROPERTY(QVariantList storage READ storage NOTIFY changed)
    Q_PROPERTY(QVariantList apps READ apps NOTIFY appsChanged)
    Q_PROPERTY(QVariantList associations READ associations NOTIFY appsChanged)
    Q_PROPERTY(QVariantList hooks READ hooks NOTIFY changed)
    Q_PROPERTY(QVariantMap coverage READ coverage NOTIFY changed)

public:
    SystemProvider(const Catalog *catalog, CommandRunner *runner, Capabilities *capabilities, ChangeCoordinator *coordinator, QObject *parent = nullptr);

    QVariantMap machine() const { return m_machine; }
    QVariantMap battery() const { return m_battery; }
    QVariantList storage() const { return m_storage; }
    QVariantList apps() const { return m_apps; }
    QVariantList associations() const { return m_associations; }
    QVariantList hooks() const { return m_hooks; }
    QVariantMap coverage() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void refreshApps();
    Q_INVOKABLE void launchApp(const QString &desktopId);
    Q_INVOKABLE void setAssociation(const QString &mime, const QString &desktopId);
    Q_INVOKABLE QString exportDiagnostics();

    static QVariantList parseDf(const QByteArray &text);
    // The everyday file types whose app can be chosen here.
    static QStringList commonMimeTypes();
    static QVariantList readApps(const Env &env);
    static QString buildDiagnostics(const QString &debugOutput, const QVariantMap &capabilities, const QVariantMap &versions, const QList<Transaction> &history);

signals:
    void changed();
    void appsChanged();
    void message(const QString &text, bool isError);

private:
    const Catalog *m_catalog;
    CommandRunner *m_runner;
    Capabilities *m_capabilities;
    ChangeCoordinator *m_coordinator;
    QVariantMap m_machine, m_battery;
    QHash<QString, QString> m_iconUrls;
    QVariantList m_storage, m_apps, m_associations, m_hooks;
};
