#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <functional>

#include "catalog.h"
#include "changecoordinator.h"
#include "profileplanner.h"
#include "settingsengine.h"

// One operation of a restore run, and where it got to. Each state reaches the disk before
// the next step, so a run that dies can be picked up again by reading what happened.
struct ProfileRunItem {
    QString id, kind, target, label, mode, desired;
    // What the plan saw, which an operation must still find before it changes anything.
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
    // running, completed, partial, incomplete, cancelled, interrupted
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

    ProfileRunner(const Catalog *catalog, SettingsEngine *engine, ChangeCoordinator *coordinator, const Env &env, QObject *parent = nullptr);

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
    void conclude(std::shared_ptr<ProfileRun> run, Done done);
    bool persist(const ProfileRun &run, QString *error = nullptr) const;
    void readAll(std::shared_ptr<ProfileRun> run, std::function<void(QHash<QString, ReadResult>)> done);
    ProfilePlanItem asPlanItem(const ProfileRunItem &item) const;

    const Catalog *m_catalog;
    SettingsEngine *m_engine;
    ChangeCoordinator *m_coordinator;
    Env m_env;
    bool m_cancelled = false;
    bool m_running = false;
};
