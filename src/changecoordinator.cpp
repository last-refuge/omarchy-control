#include "changecoordinator.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <unistd.h>

#include "fileops.h"

static constexpr int JournalLimit = 200;
static constexpr int BackupLimit = 30;

bool Transaction::finished() const {
    return phase == "committed" || phase == "rolled-back" || phase == "partial" || phase == "failed" ||
           phase == "interrupted" || phase == "not-applied";
}

bool Transaction::undoable() const {
    return phase == "committed" && undoneBy.isEmpty() && recovery != "no-undo" && kind != "action";
}

QJsonObject Transaction::toJson() const {
    QJsonObject o;
    o["id"] = id;
    o["setting"] = settingId;
    o["label"] = label;
    o["kind"] = kind;
    o["before"] = before;
    o["after"] = after;
    o["recovery"] = recovery;
    o["phase"] = phase;
    o["message"] = ChangeCoordinator::redact(message);
    o["at"] = at.toUTC().toString(Qt::ISODate);
    if (beforeWasDefault)
        o["beforeWasDefault"] = true;
    if (!undoneBy.isEmpty())
        o["undoneBy"] = undoneBy;
    if (!files.isEmpty())
        o["files"] = files;
    if (!extra.isEmpty())
        o["extra"] = extra;
    if (!runId.isEmpty())
        o["run"] = runId;
    return o;
}

Transaction Transaction::fromJson(const QJsonObject &o) {
    Transaction tx;
    tx.id = o.value("id").toString();
    tx.settingId = o.value("setting").toString();
    tx.label = o.value("label").toString();
    tx.kind = o.value("kind").toString();
    tx.before = o.value("before").toString();
    tx.after = o.value("after").toString();
    tx.recovery = o.value("recovery").toString();
    tx.phase = o.value("phase").toString();
    tx.message = o.value("message").toString();
    tx.at = QDateTime::fromString(o.value("at").toString(), Qt::ISODate);
    tx.beforeWasDefault = o.value("beforeWasDefault").toBool();
    tx.undoneBy = o.value("undoneBy").toString();
    tx.files = o.value("files").toObject();
    tx.extra = o.value("extra").toObject();
    tx.runId = o.value("run").toString();
    return tx;
}

QVariantMap Transaction::toVariant() const {
    QVariantMap m;
    m["id"] = id;
    m["settingId"] = settingId;
    m["label"] = label;
    m["kind"] = kind;
    m["before"] = before;
    m["after"] = after;
    m["recovery"] = recovery;
    m["phase"] = phase;
    m["message"] = message;
    m["at"] = at.toLocalTime();
    m["undoable"] = undoable();
    m["undoneBy"] = undoneBy;
    m["runId"] = runId;
    return m;
}

ChangeCoordinator::ChangeCoordinator(const Env &env, QObject *parent) : QObject(parent), m_env(env) {
    QDir().mkpath(m_env.appStateDir);
    QDir().mkpath(backupDirectory());
    QFile::setPermissions(m_env.appStateDir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QFile::setPermissions(backupDirectory(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

QString ChangeCoordinator::journalPath() const {
    return m_env.appStateDir + "/journal.jsonl";
}

QString ChangeCoordinator::backupDirectory() const {
    return m_env.appStateDir + "/backups";
}

QString ChangeCoordinator::leasePath() const {
    return m_env.appStateDir + "/restore.lock";
}

bool ChangeCoordinator::acquireLease(const QString &runId, QString *error) {
    if (m_lease) {
        if (m_leaseRun == runId)
            return true;
        if (error)
            *error = tr("Another restore is running.");
        return false;
    }
    auto lease = std::make_unique<QLockFile>(leasePath());
    // A restore can take as long as its slowest command, so age alone never frees the
    // lease. A holder that died does: QLockFile checks whether its process still runs.
    lease->setStaleLockTime(0);
    if (!lease->tryLock(0)) {
        if (error)
            *error = lease->error() == QLockFile::LockFailedError ? tr("Another restore is running.")
                                                                  : tr("The restore lease could not be taken in %1.").arg(m_env.appStateDir);
        return false;
    }
    m_lease = std::move(lease);
    m_leaseRun = runId;
    return true;
}

void ChangeCoordinator::releaseLease() {
    if (m_lease)
        m_lease->unlock();
    m_lease.reset();
    m_leaseRun.clear();
}

bool ChangeCoordinator::leaseHeldElsewhere() const {
    // No file, no restore. Probing creates the file for a moment, which another probe
    // could mistake for a restore, so it only happens when there is something to check.
    if (m_lease || !QFileInfo::exists(leasePath()))
        return false;
    QLockFile probe(leasePath());
    probe.setStaleLockTime(0);
    // Taking it is the only reliable look. A stale lease from a dead run is cleared by this.
    if (probe.tryLock(0)) {
        probe.unlock();
        return false;
    }
    return probe.error() == QLockFile::LockFailedError;
}

bool ChangeCoordinator::waitUntilIdle(int timeoutMs) {
    if (busy())
        return false;
    QLockFile lock(m_env.appStateDir + "/change.lock");
    lock.setStaleLockTime(5 * 60 * 1000);
    if (!lock.tryLock(timeoutMs))
        return false;
    lock.unlock();
    return true;
}

QString ChangeCoordinator::redact(const QString &text) {
    QString out = text;
    static const QList<QRegularExpression> patterns{
        QRegularExpression(R"((?i)(password|passwd|psk|secret|token|api[_-]?key|authorization)(\s*[:=]\s*|\s+)(\S+))"),
        QRegularExpression(R"((?i)(wifi-sec\.psk)\s+(\S+))"),
    };
    for (const auto &pattern : patterns)
        out.replace(pattern, "\\1\\2[redacted]");
    return out;
}

bool ChangeCoordinator::append(const Transaction &tx) {
    QFile file(journalPath());
    const bool existed = file.exists();
    // A crash can leave a partial last line. Appending onto it would corrupt this
    // record too, so it starts on a line of its own.
    bool needsNewline = false;
    if (existed && file.size() > 0 && file.open(QIODevice::ReadOnly)) {
        file.seek(file.size() - 1);
        needsNewline = file.read(1) != "\n";
        file.close();
    }
    if (!file.open(QIODevice::Append))
        return false;
    if (!existed)
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    const QByteArray line = (needsNewline ? QByteArray("\n") : QByteArray()) + QJsonDocument(tx.toJson()).toJson(QJsonDocument::Compact) + "\n";
    const bool written = file.write(line) == line.size() && file.flush();
    // Intent must reach the disk before anything changes, or a crash can leave a
    // mutation with no record to recover from.
    return written && fsync(file.handle()) == 0;
}

void ChangeCoordinator::submit(Request request, Finished finished) {
    m_queue.append(Queued{std::move(request), std::move(finished)});
    pump();
}

void ChangeCoordinator::pump() {
    if (m_running || m_queue.isEmpty())
        return;
    m_running = true;
    run(m_queue.takeFirst());
}

bool ChangeCoordinator::captureFiles(Transaction &tx, const QStringList &files, QString *error) {
    int index = 0;
    for (const auto &path : files) {
        QJsonObject entry;
        const QString before = FileOps::fingerprint(path);
        entry["before"] = before;
        if (before == QLatin1String("unreadable")) {
            *error = tr("%1 could not be read, so nothing was changed.").arg(QFileInfo(path).fileName());
            return false;
        }
        if (before != QLatin1String("missing")) {
            const QString directory = backupDirectory() + "/" + tx.id;
            QDir().mkpath(directory);
            QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            const QString backup = directory + "/" + QString::number(index);
            // No backup means no way back, so the change does not start.
            if (!QFile::copy(path, backup) || FileOps::fingerprint(backup) != before) {
                *error = tr("A backup of %1 could not be saved, so nothing was changed.").arg(QFileInfo(path).fileName());
                return false;
            }
            QFile::setPermissions(backup, QFile::ReadOwner | QFile::WriteOwner);
            entry["backup"] = backup;
        }
        tx.files.insert(path, entry);
        ++index;
    }
    return true;
}

void ChangeCoordinator::recordAfter(Transaction &tx) {
    for (auto it = tx.files.begin(); it != tx.files.end(); ++it) {
        QJsonObject entry = it.value().toObject();
        entry["after"] = FileOps::fingerprint(it.key());
        it.value() = entry;
    }
}

void ChangeCoordinator::saveReplaced(const Transaction &tx, const QString &path) {
    const QString directory = backupDirectory() + "/" + tx.id;
    QDir().mkpath(directory);
    QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QString saved = directory + "/replaced-" + QFileInfo(path).fileName();
    QFile::remove(saved);
    if (QFile::copy(path, saved))
        QFile::setPermissions(saved, QFile::ReadOwner | QFile::WriteOwner);
}

bool ChangeCoordinator::restoreFiles(const Transaction &tx, QString *error) {
    // One file someone edited is no reason to leave the rest of this change in place, so
    // every file gets its turn and the first problem is what gets reported.
    QString first;
    for (auto it = tx.files.begin(); it != tx.files.end(); ++it) {
        const auto entry = it.value().toObject();
        const QString current = FileOps::fingerprint(it.key());
        if (current == entry.value("before").toString())
            continue;
        // Once a result is on record, anything else in the file is somebody's later
        // edit, and a restore must not overwrite it.
        if (entry.contains("after") && current != entry.value("after").toString()) {
            if (first.isEmpty())
                first = tr("%1 was edited in the meantime, so it was left as it is.").arg(QFileInfo(it.key()).fileName());
            continue;
        }
        if (entry.value("before").toString() == QLatin1String("missing")) {
            saveReplaced(tx, it.key());
            QFile::remove(it.key());
            continue;
        }
        QByteArray contents;
        QString problem;
        if (!FileOps::readFile(entry.value("backup").toString(), &contents, &problem)) {
            if (first.isEmpty())
                first = problem;
            continue;
        }
        // Before a result is on record, a concurrent edit cannot be told apart from
        // this change's own work. So what gets replaced is kept, and nothing is lost.
        saveReplaced(tx, it.key());
        if (!FileOps::atomicWrite(it.key(), contents, &problem) && first.isEmpty())
            first = problem;
    }
    if (!first.isEmpty() && error)
        *error = first;
    return first.isEmpty();
}

void ChangeCoordinator::finish(Queued &item, Transaction tx) {
    append(tx);
    // Pruning rewrites shared files, so it needs the lock this run may not have got.
    if (m_lock && m_lock->isLocked())
        prune();
    m_lock.reset();
    m_running = false;
    emit historyChanged();
    if (item.finished)
        item.finished(tx);
    // Deferred so a chained change starts from a clean stack.
    QTimer::singleShot(0, this, &ChangeCoordinator::pump);
}

void ChangeCoordinator::fail(Queued item, Transaction tx, const QString &error) {
    tx.message = error;
    tx.phase = "failed";
    finish(item, tx);
}

void ChangeCoordinator::run(Queued item) {
    Transaction tx;
    tx.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
    tx.settingId = item.request.settingId;
    tx.label = item.request.label;
    tx.kind = item.request.kind;
    tx.before = item.request.before;
    tx.after = item.request.after;
    tx.recovery = item.request.recovery;
    tx.beforeWasDefault = item.request.beforeWasDefault;
    tx.extra = item.request.extra;
    tx.runId = item.request.runId;
    tx.at = QDateTime::currentDateTimeUtc();

    // A second window or the CLI must not interleave with this change.
    m_lock = std::make_unique<QLockFile>(m_env.appStateDir + "/change.lock");
    m_lock->setStaleLockTime(5 * 60 * 1000);
    if (!m_lock->tryLock(2000)) {
        fail(std::move(item), tx, tr("Another Omarchy Control window is applying a change. Try again in a moment."));
        return;
    }

    // While a restore holds the lease, only its own changes go ahead. Checked after the
    // change lock, so a restore that starts now waits for this change instead.
    if (!m_leaseRun.isEmpty() ? item.request.runId != m_leaseRun : (!item.request.runId.isEmpty() || leaseHeldElsewhere())) {
        fail(std::move(item), tx, !item.request.runId.isEmpty() && m_leaseRun.isEmpty()
                                      ? tr("This restore no longer holds its lease, so nothing was changed.")
                                      : tr("A restore is running. Changes can be made again when it finishes."));
        return;
    }

    // Reconcile: someone else changed the file since this was planned.
    for (auto it = item.request.expectedFingerprints.constBegin(); it != item.request.expectedFingerprints.constEnd(); ++it) {
        if (FileOps::fingerprint(it.key()) != it.value()) {
            fail(std::move(item), tx, tr("%1 changed outside this window. Review the current value, then try again.").arg(QFileInfo(it.key()).fileName()));
            return;
        }
    }

    QString captureError;
    if (!captureFiles(tx, item.request.files, &captureError)) {
        fail(std::move(item), tx, captureError);
        return;
    }
    tx.phase = "intent";
    if (!append(tx)) {
        fail(std::move(item), tx, tr("The change journal could not be written, so nothing was changed."));
        return;
    }

    auto shared = std::make_shared<Queued>(std::move(item));
    auto state = std::make_shared<Transaction>(tx);

    auto concludeFailure = [this, shared, state](const QString &error) {
        if (shared->request.nothingToRestore) {
            state->phase = "failed";
            state->message = error;
            finish(*shared, *state);
            return;
        }
        // Restore what can be restored, then say exactly what happened.
        QString restoreError;
        const bool restored = state->files.isEmpty() || restoreFiles(*state, &restoreError);
        auto done = [this, shared, state, error, restored, restoreError](bool rollbackOk, const QString &rollbackError) {
            const bool nothing = state->files.isEmpty() && !shared->request.rollback;
            if (nothing) {
                state->phase = "partial";
                state->message = tr("%1 The change may be partly applied.").arg(error);
            } else if (restored && rollbackOk) {
                state->phase = "rolled-back";
                state->message = tr("%1 The previous value was restored.").arg(error);
            } else {
                state->phase = "partial";
                state->message = tr("%1 Restoring the previous value also failed: %2").arg(error, restoreError.isEmpty() ? rollbackError : restoreError);
            }
            finish(*shared, *state);
        };
        if (shared->request.rollback)
            shared->request.rollback(done);
        else
            done(true, {});
    };

    state->phase = "applying";
    append(*state);
    shared->request.apply([this, shared, state, concludeFailure](bool ok, const QString &error) {
        if (!ok) {
            concludeFailure(error);
            return;
        }
        state->phase = "applied";
        recordAfter(*state);
        append(*state);
        auto verified = [this, shared, state, concludeFailure](bool good, const QString &verifyError) {
            if (!good) {
                concludeFailure(verifyError);
                return;
            }
            state->phase = "committed";
            recordAfter(*state);
            // Some results are only known now, such as the scale Hyprland settled on.
            if (shared->request.settled)
                state->after = shared->request.settled();
            finish(*shared, *state);
        };
        if (shared->request.verify)
            shared->request.verify(verified);
        else
            verified(true, {});
    });
}

QList<Transaction> ChangeCoordinator::readAll() const {
    QFile file(journalPath());
    QList<Transaction> ordered;
    if (!file.open(QIODevice::ReadOnly))
        return ordered;
    QHash<QString, int> positions;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        // A crash can truncate the final line; everything before it still counts.
        const auto document = QJsonDocument::fromJson(line);
        if (!document.isObject())
            continue;
        const Transaction tx = Transaction::fromJson(document.object());
        const auto it = positions.constFind(tx.id);
        if (it == positions.constEnd()) {
            positions.insert(tx.id, ordered.size());
            ordered << tx;
        } else {
            ordered[it.value()] = tx;
        }
    }
    return ordered;
}

QList<Transaction> ChangeCoordinator::history(int limit) const {
    QList<Transaction> all = readAll();
    std::reverse(all.begin(), all.end());
    if (all.size() > limit)
        all.resize(limit);
    return all;
}

Transaction ChangeCoordinator::transaction(const QString &id) const {
    for (const auto &tx : readAll())
        if (tx.id == id)
            return tx;
    return {};
}

void ChangeCoordinator::markUndoneBy(const QString &id, const QString &undoId) {
    Transaction tx = transaction(id);
    if (tx.id.isEmpty())
        return;
    tx.undoneBy = undoId;
    QLockFile lock(m_env.appStateDir + "/change.lock");
    lock.setStaleLockTime(5 * 60 * 1000);
    // A compaction elsewhere could otherwise drop this line. If the lock stays busy,
    // the record is still better written than not.
    lock.tryLock(2000);
    append(tx);
    emit historyChanged();
}

QStringList ChangeCoordinator::recoverInterrupted() {
    QStringList messages;
    // Another instance may be mid-change, and its transactions look just like a
    // crash from here. Only recover when nothing holds the change lock.
    QLockFile lock(m_env.appStateDir + "/change.lock");
    lock.setStaleLockTime(5 * 60 * 1000);
    if (!lock.tryLock(0))
        return messages;

    for (Transaction tx : readAll()) {
        if (tx.finished())
            continue;
        // Writes are atomic, so a file holding neither the old nor the new content is
        // not half of this change: it is someone's later edit. Recovery only ever
        // reports that. The backup stays available, and nothing is overwritten.
        bool anyDiffers = false;
        bool allAfter = !tx.files.isEmpty();
        for (auto it = tx.files.begin(); it != tx.files.end(); ++it) {
            const auto entry = it.value().toObject();
            const QString current = FileOps::fingerprint(it.key());
            if (current != entry.value("before").toString())
                anyDiffers = true;
            if (!entry.contains("after") || current != entry.value("after").toString())
                allAfter = false;
        }
        if (tx.files.isEmpty()) {
            tx.phase = "interrupted";
            tx.message = tr("The app stopped while applying this. Check the current value.");
        } else if (!anyDiffers) {
            tx.phase = "not-applied";
            tx.message = tr("The app stopped before this changed anything.");
        } else if (allAfter) {
            tx.phase = "interrupted";
            tx.message = tr("The app stopped after writing this, before it could confirm the result.");
        } else {
            tx.phase = "interrupted";
            QStringList backups;
            for (auto it = tx.files.begin(); it != tx.files.end(); ++it)
                if (it.value().toObject().contains("backup"))
                    backups << it.value().toObject().value("backup").toString();
            tx.message = backups.isEmpty() ? tr("The app stopped while applying this, and the file was left as it is.")
                                           : tr("The app stopped while applying this. The file was left as it is; the previous version is saved at %1").arg(backups.join(", "));
        }
        append(tx);
        messages << tx.label + ": " + tx.message;
    }
    if (!messages.isEmpty())
        emit historyChanged();
    prune();
    return messages;
}

void ChangeCoordinator::prune() {
    const QList<Transaction> all = readAll();

    // Retention is bounded in both directions: journal lines and file backups.
    QDir backups(backupDirectory());
    QStringList keep;
    int kept = 0;
    for (int i = all.size() - 1; i >= 0; --i) {
        // An unfinished change may still need its backup, however old it is.
        if (kept < BackupLimit || !all[i].finished())
            keep << all[i].id;
        ++kept;
    }
    for (const auto &name : backups.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (!keep.contains(name))
            QDir(backups.filePath(name)).removeRecursively();

    QFile file(journalPath());
    if (file.size() < 256 * 1024 && all.size() <= JournalLimit)
        return;
    QByteArray compacted;
    const int firstKept = qMax<int>(0, all.size() - JournalLimit);
    for (int i = 0; i < all.size(); ++i)
        if (i >= firstKept || !all[i].finished())
            compacted += QJsonDocument(all[i].toJson()).toJson(QJsonDocument::Compact) + "\n";
    FileOps::atomicWrite(journalPath(), compacted, nullptr, 0600);
}
