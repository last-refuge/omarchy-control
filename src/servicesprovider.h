#pragma once

#include <QObject>
#include <QVariantList>

#include "changecoordinator.h"
#include "commandrunner.h"
#include "provideroperation.h"

// Background services and login items. System-level actions go through systemctl
// so that the desktop's own polkit agent asks for authorization; the app itself
// stays unprivileged.
class ServicesProvider : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList userServices READ userServices NOTIFY changed)
    Q_PROPERTY(QVariantList systemServices READ systemServices NOTIFY changed)
    Q_PROPERTY(QVariantList startupApps READ startupApps NOTIFY changed)
    Q_PROPERTY(int failedCount READ failedCount NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)

public:
    ServicesProvider(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *parent = nullptr);

    QVariantList userServices() const { return m_user; }
    QVariantList systemServices() const { return m_system; }
    QVariantList startupApps() const { return m_startup; }
    int failedCount() const;
    QString status() const { return m_status; }
    QString scopeStatus(bool system) const { return system ? m_systemStatus : m_userStatus; }
    QString startupStatus() const { return m_startupStatus; }

    void refreshServices(bool system);
    void refreshStartup();
    void planService(const QString &unit, const QString &action, bool system, std::function<void(ProviderPlan)> done);
    ProviderPlan planStartup(const QString &id, bool enabled) const;
    void changeService(const QString &unit, const QString &action, bool system, bool interactive);

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void serviceAction(const QString &unit, const QString &action, bool system);
    Q_INVOKABLE void setStartupEnabled(const QString &id, bool enabled);

    static bool isSafeUnit(const QString &unit);
    static QVariantList mergeUnits(const QByteArray &units, const QByteArray &files);
    static QVariantList readStartup(const Env &env, QString *error = nullptr);
    static QByteArray withHidden(const QByteArray &entry, bool hidden);

signals:
    void changed();
    void refreshed();
    void operationFinished(const Transaction &transaction);
    void message(const QString &text, bool isError);

private:
    void loadScope(bool system, std::function<void()> done);
    void readService(const QString &unit, bool system, std::function<void(bool, QJsonObject)> done);
    void reject(const ProviderPlan &plan);

    CommandRunner *m_runner;
    ChangeCoordinator *m_coordinator;
    QVariantList m_user, m_system, m_startup;
    QString m_status = "loading";
    QString m_userStatus = "loading", m_systemStatus = "loading", m_startupStatus = "loading";
    int m_generation[2] = {0, 0};
};
