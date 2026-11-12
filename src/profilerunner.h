#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <functional>

#include "catalog.h"
#include "changecoordinator.h"
#include "packageprovider.h"
#include "profiledesktop.h"
#include "profileplanner.h"
#include "settingsengine.h"

// One operation of a restore run, and where it got to. Each state reaches the disk before
// the next step, so a run that dies can be picked up again by reading what happened.
struct ProfileRunItem {
    QString id, kind, target, label, mode, desired;
    QString action;            // For a package: install, mark-explicit, or remove.
    QStringList packages;      // For a package: what pacman said it would install or remove.
    QStringList needs;         // Items that must have been verified first.
    // What the plan saw, which an operation must still find before it changes anything. For
    // a package: "explicit", "dependency", or "missing".
    QString planned;
    bool plannedOverride = false;
    // pending, running, verified, failed, blocked, skipped, interrupted
    QString state = QStringLiteral("pending");
    QString transaction;  // The coordinator transaction that made the change.
    QString settled;      // What it read back as afterwards.
    QString message;

    QJsonObject toJson() const;
    static ProfileRunItem fromJson(const QJsonObject &object);
};

struct ProfileRun {
    QString id, planId, directory, profileName, profileDigest;
    // running, completed, partial, incomplete, cancelled, paused (waiting for another
    // package operation), interrupted
    QString state;
    QString startedAt, finishedAt, message;
    int leftOut = 0;  // Plan items the review left out: blocked, manual, or kept local.
    QList<ProfileRunItem> items;

    int count(const QString &itemState) const;
    bool resumable() const;
    QJsonObject toJson() const;
    static ProfileRun fromJson(const QJsonObject &object);
};

// Carries out a reviewed plan under the coordinator's restore lease, one operation at a
// time, each verified by reading it back. Nothing is replayed blindly: an operation that
// was underway when a run stopped is settled by reading its current value first.
class ProfileRunner : public QObject {
    Q_OBJECT

public:
    using Done = std::function<void(const ProfileRun &)>;

    ProfileRunner(const Catalog *catalog, SettingsEngine *engine, ChangeCoordinator *coordinator, PackageProvider *packages, const Env &env,
                  QObject *parent = nullptr);

    // Package changes run attached to a terminal, where sudo and pacman can ask. Only the CLI
    // turns this on; elsewhere they are left for a restore started from a terminal.
    void setPackageTerminal(bool attached) { m_packageTerminal = attached; }
    // How long to wait for another package operation before pausing the run.
    void setPacmanWait(int milliseconds) { m_pacmanWaitMs = milliseconds; }
    // The dock, launchers, file associations, and owned shortcuts and rules.
    void setDesktop(ProfileDesktop *desktop) { m_desktop = desktop; }

    // Refuses before any change when the plan is stale, the profile changed since it was
    // planned, or another restore holds the lease. `run.state` is empty then, and
    // `run.message` says why.
    void apply(const ProfilePlan &plan, Done done);
    void resume(const QString &runId, Done done);
    // Stops before the next operation. The one underway finishes and is verified first.
    void cancel() { m_cancelled = true; }
    bool running() const { return m_running; }

    ProfileRun load(const QString &id, QString *error = nullptr) const;
    QList<ProfileRun> runs(int limit = 20) const;

signals:
    void itemChanged(const ProfileRun &run, int index);

private:
    void start(std::shared_ptr<ProfileRun> run, bool resuming, Done done);
    void execute(std::shared_ptr<ProfileRun> run, int index, Done done);
    void executePackages(std::shared_ptr<ProfileRun> run, int index, Done done);
    void transact(std::shared_ptr<ProfileRun> run, QList<int> batch, int index, Done done);
    void conclude(std::shared_ptr<ProfileRun> run, Done done);
    void finishRun(std::shared_ptr<ProfileRun> run, Done done);
    bool persist(const ProfileRun &run, QString *error = nullptr) const;
    struct Observed {
        QHash<QString, ReadResult> settings;
        PackageProvider::Inventory packages;
        ProfileDesktop::Snapshot desktop;
    };
    void readAll(std::shared_ptr<ProfileRun> run, std::function<void(const Observed &)> done);
    ProfilePlanItem asPlanItem(const ProfileRunItem &item) const;

    const Catalog *m_catalog;
    SettingsEngine *m_engine;
    ChangeCoordinator *m_coordinator;
    PackageProvider *m_packages;
    Env m_env;
    bool m_cancelled = false;
    bool m_running = false;
    bool m_packageTerminal = false;
    ProfileDesktop::Context m_desktopContext;  // The profile being restored, and its folder.
    int m_pacmanWaitMs = 60000;
    ProfileDesktop *m_desktop = nullptr;
};
