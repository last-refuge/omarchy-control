#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include "capabilities.h"
#include "catalog.h"
#include "changecoordinator.h"
#include "commandrunner.h"
#include "provideroperation.h"

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
    Q_PROPERTY(QVariantMap factStates READ factStates NOTIFY changed)

public:
    SystemProvider(const Catalog *catalog, CommandRunner *runner, Capabilities *capabilities, ChangeCoordinator *coordinator, QObject *parent = nullptr);

    QVariantMap machine() const { return m_machine; }
    QVariantMap battery() const { return m_battery; }
    QVariantList storage() const { return m_storage; }
    QVariantList apps() const { return m_apps; }
    QVariantList associations() const { return m_associations; }
    QVariantList hooks() const { return m_hooks; }
    QVariantMap coverage() const;
    QVariantMap factStates() const { return m_factStates; }
    QJsonObject facts(const QString &section = QStringLiteral("status")) const;
    void refreshFacts(const QString &section);
    ProviderPlan planDiagnostics() const;
    QString collectDiagnostics(std::function<void(QJsonObject)> done);
    QString appsStatus() const { return m_appsStatus; }
    void readAssociation(const QString &mime, std::function<void(QJsonObject)> done);
    void planAssociation(const QString &mime, const QString &desktopId, std::function<void(ProviderPlan)> done);
    ProviderPlan planLaunch(const QString &desktopId) const;
    bool launchInstalledApp(const QString &desktopId, QString *error);

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void refreshApps();
    Q_INVOKABLE void launchApp(const QString &desktopId);
    Q_INVOKABLE void setAssociation(const QString &mime, const QString &desktopId);
    Q_INVOKABLE QString exportDiagnostics();

    static QVariantList parseDf(const QByteArray &text, QString *error = nullptr);
    // The everyday file types whose app can be chosen here.
    static QStringList commonMimeTypes();
    static QVariantList readApps(const Env &env, QString *error = nullptr);
    static bool isSafeMime(const QString &mime);
    static bool isSafeDesktopId(const QString &id);
    static QString buildDiagnostics(const QString &debugOutput, const QVariantMap &capabilities, const QVariantMap &versions, const QList<Transaction> &history);

signals:
    void changed();
    void appsChanged();
    void appsRefreshed();
    void factsRefreshed();
    void diagnosticsFinished(const QJsonObject &result);
    void operationFinished(const Transaction &transaction);
    void message(const QString &text, bool isError);

private:
    const Catalog *m_catalog;
    CommandRunner *m_runner;
    Capabilities *m_capabilities;
    ChangeCoordinator *m_coordinator;
    QVariantMap m_machine, m_battery;
    QHash<QString, QString> m_iconUrls;
    QVariantList m_storage, m_apps, m_associations, m_hooks;
    QVariantList m_hookInventory;
    QVariantMap m_factStates;
    int m_factsGeneration = 0;
    bool m_exporting = false;
    QString m_appsStatus = "loading";
    int m_appsGeneration = 0;
};
