#pragma once

#include <QObject>
#include <QJsonObject>
#include <QVariantList>

#include "changecoordinator.h"
#include "commandrunner.h"
#include "provideroperation.h"

// Shell plugins, bar widget placement, and dock pins, through Omarchy's own
// commands so clones, selected implementations, and per-widget options survive.
class PluginsProvider : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList plugins READ plugins NOTIFY changed)
    Q_PROPERTY(QVariantMap barLayout READ barLayout NOTIFY changed)
    Q_PROPERTY(QStringList dockPins READ dockPins NOTIFY changed)
    Q_PROPERTY(QVariantList dockApps READ dockApps NOTIFY changed)
    Q_PROPERTY(bool changing READ changing NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString activeBar READ activeBar NOTIFY changed)

public:
    PluginsProvider(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *parent = nullptr);

    QVariantList plugins() const { return m_plugins; }
    QVariantMap barLayout() const { return m_layout; }
    QStringList dockPins() const { return m_pins; }
    QVariantList dockApps() const { return m_dockApps; }
    // A move's positions are only good until the next one lands, so they wait their turn.
    bool changing() const { return m_changes > 0; }
    QString status() const { return m_status; }
    QString activeBar() const { return m_activeBar; }
    void inspect(const QString &scope, std::function<void(QJsonObject)> done);
    void planEnabled(const QString &id, bool enabled, std::function<void(ProviderPlan)> done);
    void planMove(const QString &selector, const QString &section, int index, std::function<void(ProviderPlan)> done);
    void planOption(const QString &selector, const QString &key, bool value, std::function<void(ProviderPlan)> done);
    void planPin(const QString &id, bool pinned, std::function<void(ProviderPlan)> done);
    void planLifecycle(const QString &id, bool clone, std::function<void(ProviderPlan)> done);
    void planRecovery(const QString &token, std::function<void(ProviderPlan)> done);
    void applyPlan(const ProviderPlan &plan);
    static bool isWidgetSelector(const QString &selector);
    static QStringList sections();

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setEnabled(const QString &id, bool enabled);
    Q_INVOKABLE void clonePlugin(const QString &id);
    Q_INVOKABLE void removePlugin(const QString &id);
    Q_INVOKABLE void moveWidget(const QString &id, const QString &section, int index, const QString &fromSection, int fromIndex);
    Q_INVOKABLE void pin(const QString &desktopId);
    Q_INVOKABLE void unpin(const QString &desktopId);

    static bool isSafeId(const QString &id);
    static QVariantList parsePlugins(const QByteArray &json);
    static QVariantList scanManifests(const Env &env);
    static QVariantMap parseLayout(const QByteArray &shellJson);
    // What a pin looks like: the name and icon its desktop entry gives, looked up in
    // `iconTheme`. Anything that cannot be found comes back empty, for the fallback.
    static QVariantMap resolveDockApp(const Env &env, const QString &desktopId, const QString &iconTheme);

signals:
    void changed();
    void message(const QString &text, bool isError);
    void operationFinished(const Transaction &transaction);

private:
    void loadConfig();
    void resolveDockApps();
    void readState(const QString &scope, std::function<void(QJsonObject)> done);
    void presentState(const QJsonObject &state);
    ProviderPlan basePlan(const QJsonObject &state, const QString &operation, const QString &target) const;
    ProviderPlan movePlan(const QJsonObject &state, const QString &selector, const QString &section, int index) const;
    void executePlan(const ProviderPlan &plan, bool alreadyChanging);
    void reject(const ProviderPlan &plan, bool alreadyChanging = false);

    CommandRunner *m_runner;
    ChangeCoordinator *m_coordinator;
    QVariantList m_plugins;
    QVariantMap m_layout;
    QStringList m_pins;
    QVariantList m_dockApps;
    QString m_iconTheme;
    int m_changes = 0;
    QString m_status = "loading";
    QString m_activeBar;
    int m_readGeneration = 0;
    QString m_viewFingerprint;
};
