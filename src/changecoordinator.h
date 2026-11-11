#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QLockFile>
#include <QObject>
#include <QStringList>
#include <functional>
#include <memory>

#include "env.h"

// One recorded change. Omarchy's own files stay the source of truth; the journal
// only records what this app did, so it can report honestly and recover.
struct Transaction {
    QString id;
    QString settingId;
    QString label;
    QString kind;      // set, reset, undo, action
    QString before;
    QString after;
    QString recovery;  // auto-undo, best-effort, no-undo
    QString phase;     // intent, applying, applied, committed, rolled-back, partial, failed, interrupted
    QString message;
    QDateTime at;
    bool beforeWasDefault = false;
    QString undoneBy;
    QJsonObject files;  // path -> {before, after, backup}
    QJsonObject extra;  // What a setting needs to go back that files cannot hold, such as GSettings.
    QString runId;      // The restore this change belongs to, recorded before it starts.

    bool finished() const;
    bool undoable() const;
    QVariantMap toVariant() const;
    QJsonObject toJson() const;
    static Transaction fromJson(const QJsonObject &object);
};

// The lifecycle every change goes through: capture, journal intent, apply,
// verify, then commit or restore. Changes run one at a time and never overlap
// with another running instance of the app.
class ChangeCoordinator : public QObject {
    Q_OBJECT

public:
    using Done = std::function<void(bool ok, const QString &error)>;
    using Step = std::function<void(Done)>;

    struct Request {
        QString settingId, label, kind = "set", before, after, recovery = "no-undo";
        bool beforeWasDefault = false;
        QStringList files;
        QHash<QString, QString> expectedFingerprints;  // Reconcile instead of overwrite.
        QJsonObject extra;  // Kept on the transaction, for undoing it later.
        // For an action with no earlier value, where a failure has nothing to report but itself.
        bool nothingToRestore = false;
        // The restore run making this change. While a run holds the lease, nothing else runs.
        QString runId;
        Step apply;
        Step verify;
        Step rollback;  // Beyond restoring `files`; optional.
        std::function<QString()> settled;  // The real result, when it can differ from `after`.
    };
    using Finished = std::function<void(const Transaction &)>;

    explicit ChangeCoordinator(const Env &env, QObject *parent = nullptr);

    void submit(Request request, Finished finished);

    QList<Transaction> history(int limit = 200) const;
    Transaction transaction(const QString &id) const;
    void markUndoneBy(const QString &id, const QString &undoId);

    // Finds transactions the app died in the middle of and reports what is known
    // about each. Safe to run repeatedly: each is resolved once, and no file is ever
    // overwritten, since it may hold an edit made after the crash.
    QStringList recoverInterrupted();

    // Removes secrets from any text before it can reach a log or an export.
    static QString redact(const QString &text);

    QString journalPath() const;
    QString backupDirectory() const;
    bool busy() const { return m_running || !m_queue.isEmpty(); }

    // A restore makes many changes, and none of anything else may land between them: not
    // from this window, another one, or the CLI. The run holds one lease across all of
    // them, and while it does, only changes carrying its run id go ahead. The lease is a
    // lock file, so a run that dies releases it with its process, and is then resumed.
    bool acquireLease(const QString &runId, QString *error = nullptr);
    void releaseLease();
    QString leaseRun() const { return m_leaseRun; }
    bool leaseHeldElsewhere() const;
    QString leasePath() const;
    // Waits for a change that began just before the lease was taken, here or elsewhere.
    bool waitUntilIdle(int timeoutMs);

signals:
    void historyChanged();

private:
    struct Queued {
        Request request;
        Finished finished;
    };
    void pump();
    void run(Queued item);
    void fail(Queued item, Transaction tx, const QString &error);
    bool append(const Transaction &tx);
    bool captureFiles(Transaction &tx, const QStringList &files, QString *error);
    void recordAfter(Transaction &tx);
    bool restoreFiles(const Transaction &tx, QString *error);
    void saveReplaced(const Transaction &tx, const QString &path);
    void finish(Queued &item, Transaction tx);
    void prune();
    QList<Transaction> readAll() const;

    Env m_env;
    QList<Queued> m_queue;
    bool m_running = false;
    std::unique_ptr<QLockFile> m_lock;
    std::unique_ptr<QLockFile> m_lease;
    QString m_leaseRun;
};
