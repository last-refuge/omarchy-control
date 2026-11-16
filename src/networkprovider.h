#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantList>

#include "changecoordinator.h"
#include "commandrunner.h"
#include "provideroperation.h"

// Common connections. A switch that could cut the connection runs under a
// NetworkManager checkpoint, which restores the previous state by itself if the
// user does not keep the result, even when this app has died. Passwords are
// never collected here: joining a secured network goes to Omarchy's panel.
class NetworkProvider : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool wifiEnabled READ wifiEnabled NOTIFY changed)
    Q_PROPERTY(QString connectivity READ connectivity NOTIFY changed)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY changed)
    Q_PROPERTY(QVariantList networks READ networks NOTIFY changed)
    Q_PROPERTY(QVariantList saved READ saved NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QVariantMap states READ states NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // A scan is its own thing: it neither blocks nor stands for a connection change.
    Q_PROPERTY(bool scanning READ scanning NOTIFY changed)
    Q_PROPERTY(bool radioBusy READ radioBusy NOTIFY changed)
    Q_PROPERTY(QVariantMap scanResult READ scanResult NOTIFY changed)
    Q_PROPERTY(int keepSeconds READ keepSeconds NOTIFY keepChanged)
    Q_PROPERTY(bool awaitingKeep READ awaitingKeep NOTIFY keepChanged)
    Q_PROPERTY(bool canKeep READ canKeep NOTIFY keepChanged)
    Q_PROPERTY(QVariantMap ownedPreview READ ownedPreview NOTIFY keepChanged)
    Q_PROPERTY(int keepTotal READ keepTotal CONSTANT)
    // What is under way, keyed by the network's name or the device's interface, and
    // the last failure for each until it is tried again.
    Q_PROPERTY(QVariantMap pending READ pending NOTIFY changed)
    Q_PROPERTY(QVariantMap failures READ failures NOTIFY changed)

public:
    NetworkProvider(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *parent = nullptr);

    bool wifiEnabled() const { return m_wifiEnabled; }
    QString connectivity() const { return m_connectivity; }
    QVariantList devices() const { return m_devices; }
    QVariantList networks() const { return m_networks; }
    QVariantList saved() const { return m_saved; }
    QString status() const { return m_status; }
    QVariantMap states() const { return m_states; }
    QVariantMap snapshot() const;
    // A scoped CLI read avoids probing unrelated sections. refresh() requests all
    // sections for the GUI. No read triggers a scan or connectivity check.
    void refreshSnapshot(const QString &section = QString());
    void refreshScanState();
    ProviderPlan planScan(const QString &device = QString()) const;
    using ScanFinished = std::function<void(const QVariantMap &)>;
    void requestScan(const QString &device, ScanFinished finished);
    static bool isScanDeviceName(const QString &device);
    void planWifiEnabled(bool on, std::function<void(const ProviderPlan &)> finished);
    void changeWifiEnabled(const ProviderPlan &plan, bool acknowledged = false);
    bool radioBusy() const { return m_radioBusy; }
    QVariantMap radioReadback() const { return m_radioReadback; }
    bool busy() const { return m_busy || m_radioBusy || m_ownedChanging; }
    bool scanning() const { return m_scanning; }
    QVariantMap scanResult() const { return m_scanResult; }
    int keepSeconds() const { return m_keepSeconds; }
    bool awaitingKeep() const { return !m_ownedToken.isEmpty() || (!m_checkpoint.isEmpty() && m_keepSeconds > 0); }
    bool canKeep() const { return m_ownedToken.isEmpty() ? awaitingKeep() : m_ownedConfirmed && m_ownedPhase == "preview" && m_keepSeconds > 5 && !m_ownedChanging; }
    QVariantMap ownedPreview() const { return m_ownedReceipt.toVariantMap(); }
    int keepTotal() const;
    QVariantMap pending() const { return m_pending; }
    QVariantMap failures() const { return m_failures; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void rescan();
    Q_INVOKABLE void setWifiEnabled(bool on);
    Q_INVOKABLE void activate(const QString &uuid, const QString &name);
    Q_INVOKABLE void deactivate(const QString &device);
    Q_INVOKABLE void keepConnection();
    Q_INVOKABLE void revertConnection();

    static QStringList splitTerse(const QString &line);
    static QVariantList parseNetworks(const QByteArray &text);
    static QVariantList parseDevices(const QByteArray &text);
    static QVariantList parseSaved(const QByteArray &text);

signals:
    void changed();
    void refreshed();
    void keepChanged();
    void operationFinished(const Transaction &transaction);
    void message(const QString &text, bool isError);

private:
    struct ScanWork;
    void refreshSections(const QStringList &sections);
    ProviderPlan scanPlan(const QString &device, bool ownRequest) const;
    void resolveScanPaths(const std::shared_ptr<ScanWork> &work, int index, ChangeCoordinator::Done done);
    void sendScanRequests(const std::shared_ptr<ScanWork> &work, int index, ChangeCoordinator::Done done);
    void publishScan(const QVariantMap &result, const ScanFinished &finished);
    void readRadio(std::function<void(const QVariantMap &)> finished);
    void submitWifiEnabled(const ProviderPlan &plan, bool acknowledged);
    void finishWifiEnabled(const Transaction &transaction);
    bool guardLegacyConnection();
    void connected(const QString &name);
    void refreshOwnedPreview();
    void ownedCommand(const QString &action);

    CommandRunner *m_runner;
    ChangeCoordinator *m_coordinator;
    bool m_wifiEnabled = false;
    QString m_connectivity;
    QVariantList m_devices, m_networks, m_saved;
    QString m_status = "loading";
    bool m_busy = false;
    bool m_scanning = false;
    bool m_radioBusy = false;
    QVariantMap m_radioReadback;
    QVariantMap m_scanResult;
    QString m_checkpoint;
    std::unique_ptr<QLockFile> m_connectionLock;
    qint64 m_checkpointDeadline = 0;
    int m_keepSeconds = 0;
    QTimer m_keepTimer;
    QString m_ownedToken, m_ownedPhase, m_ownedQueued;
    QJsonObject m_ownedReceipt;
    bool m_ownedReading = false, m_ownedChanging = false, m_ownedConfirmed = false;
    int m_generation = 0;
    int m_readsPending = 0;
    QStringList m_requested;
    QString m_startedAt;
    QVariantMap m_states, m_values;
    QVariantMap m_pending, m_failures;
};
