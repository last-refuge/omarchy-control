#include "profilerunner.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QThread>
#include <memory>

namespace {

QString now() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

}  // namespace

QJsonObject ProfileRunItem::toJson() const {
    QJsonObject o{{"id", id}, {"kind", kind}, {"target", target}, {"label", label}, {"mode", mode}, {"state", state}, {"planned", planned}};
    if (mode == QLatin1String("set") || kind == QLatin1String("package"))
        o["desired"] = desired;
    if (plannedOverride)
        o["plannedOverride"] = true;
    if (!action.isEmpty())
        o["action"] = action;
    if (!packages.isEmpty())
        o["packages"] = QJsonArray::fromStringList(packages);
    if (!needs.isEmpty())
        o["needs"] = QJsonArray::fromStringList(needs);
    if (!transaction.isEmpty())
        o["transaction"] = transaction;
    if (!settled.isEmpty())
        o["settled"] = settled;
    if (!message.isEmpty())
        o["message"] = message;
    return o;
}

ProfileRunItem ProfileRunItem::fromJson(const QJsonObject &o) {
    ProfileRunItem item;
    item.id = o.value("id").toString();
    item.kind = o.value("kind").toString();
    item.target = o.value("target").toString();
    item.label = o.value("label").toString();
    item.mode = o.value("mode").toString();
    item.desired = o.value("desired").toString();
    item.planned = o.value("planned").toString();
    item.plannedOverride = o.value("plannedOverride").toBool();
    item.action = o.value("action").toString();
    for (const QJsonValue &value : o.value("packages").toArray())
        item.packages << value.toString();
    for (const QJsonValue &value : o.value("needs").toArray())
        item.needs << value.toString();
    item.state = o.value("state").toString();
    item.transaction = o.value("transaction").toString();
    item.settled = o.value("settled").toString();
    item.message = o.value("message").toString();
    return item;
}

int ProfileRun::count(const QString &itemState) const {
    int n = 0;
    for (const ProfileRunItem &item : items)
        if (item.state == itemState)
            ++n;
    return n;
}

bool ProfileRun::resumable() const {
    return state == QLatin1String("running") || state == QLatin1String("cancelled") || state == QLatin1String("interrupted")
           || state == QLatin1String("paused");
}

QJsonObject ProfileRun::toJson() const {
    QJsonObject o{{"id", id},       {"plan", planId},           {"directory", directory}, {"profileName", profileName},
                  {"profileDigest", profileDigest}, {"state", state}, {"startedAt", startedAt}, {"leftOut", leftOut}};
    if (!finishedAt.isEmpty())
        o["finishedAt"] = finishedAt;
    if (!message.isEmpty())
        o["message"] = message;
    QJsonArray rows;
    for (const ProfileRunItem &item : items)
        rows.append(item.toJson());
    o["items"] = rows;
    QJsonObject summary;
    for (const char *itemState : {"pending", "running", "verified", "failed", "blocked", "skipped", "interrupted"})
        summary[QString::fromLatin1(itemState)] = count(QString::fromLatin1(itemState));
    o["summary"] = summary;
    return o;
}

ProfileRun ProfileRun::fromJson(const QJsonObject &o) {
    ProfileRun run;
    run.id = o.value("id").toString();
    run.planId = o.value("plan").toString();
    run.directory = o.value("directory").toString();
    run.profileName = o.value("profileName").toString();
    run.profileDigest = o.value("profileDigest").toString();
    run.state = o.value("state").toString();
    run.startedAt = o.value("startedAt").toString();
    run.finishedAt = o.value("finishedAt").toString();
    run.message = o.value("message").toString();
    run.leftOut = o.value("leftOut").toInt();
    for (const QJsonValue &value : o.value("items").toArray())
        run.items << ProfileRunItem::fromJson(value.toObject());
    return run;
}

ProfileRunner::ProfileRunner(const Catalog *catalog, SettingsEngine *engine, ChangeCoordinator *coordinator, PackageProvider *packages,
                             const Env &env, QObject *parent)
    : QObject(parent), m_catalog(catalog), m_engine(engine), m_coordinator(coordinator), m_packages(packages), m_env(env) {}

bool ProfileRunner::persist(const ProfileRun &run, QString *error) const {
    return ProfileStore::writeJson(ProfileStore::runsDir(m_env) + "/" + run.id + ".json", run.toJson(), error);
}

ProfileRun ProfileRunner::load(const QString &id, QString *error) const {
    if (!ProfileStore::isId(id, QStringLiteral("run"))) {
        if (error)
            *error = tr("%1 is not a restore id.").arg(id);
        return {};
    }
    bool ok = false;
    const QJsonObject object = ProfileStore::readJson(ProfileStore::runsDir(m_env) + "/" + id + ".json", &ok);
    if (!ok) {
        if (error)
            *error = tr("There is no restore %1 on this computer.").arg(id);
        return {};
    }
    return ProfileRun::fromJson(object);
}

QList<ProfileRun> ProfileRunner::runs(int limit) const {
    QList<ProfileRun> out;
    const QDir directory(ProfileStore::runsDir(m_env));
    for (const QFileInfo &file : directory.entryInfoList({"run-*.json"}, QDir::Files, QDir::Time)) {
        if (out.size() >= limit)
            break;
        const ProfileRun run = load(file.completeBaseName());
        if (!run.id.isEmpty())
            out << run;
    }
    return out;
}

ProfilePlanItem ProfileRunner::asPlanItem(const ProfileRunItem &item) const {
    ProfilePlanItem plan;
    plan.kind = item.kind;
    plan.action = item.action;
    plan.target = item.target;
    plan.mode = item.mode;
    plan.desired = item.desired;
    plan.current = item.planned;
    plan.currentOverride = item.plannedOverride;
    return plan;
}

void ProfileRunner::apply(const ProfilePlan &plan, Done done) {
    auto run = std::make_shared<ProfileRun>();
    run->planId = plan.id;
    if (m_running) {
        run->message = tr("A restore is already running here.");
        done(*run);
        return;
    }
    if (!plan.valid) {
        run->message = plan.errors.isEmpty() ? tr("This plan has nothing to restore.") : plan.errors.first();
        done(*run);
        return;
    }
    // A plan is made for one exact profile. If the profile is not that any more, what was
    // reviewed is not what would be restored.
    const ProfileModel::Parsed parsed = ProfileModel::load(plan.directory);
    if (!parsed.ok || parsed.profile.digest() != plan.profileDigest) {
        run->message = tr("The profile changed since this plan was made. Make a new plan.");
        done(*run);
        return;
    }
    run->id = ProfileStore::newId(QStringLiteral("run"));
    run->directory = plan.directory;
    run->profileName = plan.profileName;
    run->profileDigest = plan.profileDigest;
    run->leftOut = plan.leftOut();
    for (const ProfilePlanItem &item : plan.items) {
        if (!item.willRun() || (item.kind != QLatin1String("setting") && item.kind != QLatin1String("package")))
            continue;
        ProfileRunItem step;
        step.id = item.id;
        step.kind = item.kind;
        step.target = item.target;
        step.label = item.label;
        step.mode = item.mode;
        step.desired = item.desired;
        step.action = item.action;
        step.packages = item.transaction;
        step.needs = item.needs;
        // A package's state is what the plan saw of it; a setting's, its value.
        step.planned = item.kind == QLatin1String("package") ? item.currentStatus : item.current;
        step.plannedOverride = item.currentOverride;
        run->items << step;
    }
    start(run, false, done);
}

void ProfileRunner::resume(const QString &runId, Done done) {
    QString error;
    ProfileRun loaded = load(runId, &error);
    auto run = std::make_shared<ProfileRun>(loaded);
    if (loaded.id.isEmpty()) {
        run->message = error;
        done(*run);
        return;
    }
    if (m_running) {
        run->state.clear();
        run->message = tr("A restore is already running here.");
        done(*run);
        return;
    }
    if (!loaded.resumable()) {
        run->message = tr("Nothing to resume: this restore is %1.").arg(loaded.state);
        run->state.clear();
        done(*run);
        return;
    }
    const ProfileModel::Parsed parsed = ProfileModel::load(loaded.directory);
    if (!parsed.ok || parsed.profile.digest() != loaded.profileDigest) {
        run->message = tr("The profile changed since this restore began. Make a new plan.");
        run->state.clear();
        done(*run);
        return;
    }
    start(run, true, done);
}

void ProfileRunner::readAll(std::shared_ptr<ProfileRun> run, std::function<void(const Observed &)> done) {
    QStringList targets;
    bool packages = false;
    for (const ProfileRunItem &item : run->items) {
        if (item.kind == QLatin1String("package"))
            packages = true;
        else if (!targets.contains(item.target))
            targets << item.target;
    }
    auto observed = std::make_shared<Observed>();
    auto next = std::make_shared<std::function<void(int)>>();
    *next = [this, targets, packages, observed, done, next](int index) {
        if (index >= targets.size()) {
            if (packages && m_packages) {
                m_packages->readInventory([observed, done](const PackageProvider::Inventory &inventory) {
                    observed->packages = inventory;
                    done(*observed);
                });
            } else {
                done(*observed);
            }
            *next = nullptr;
            return;
        }
        m_engine->read(targets[index], [observed, targets, index, next](const ReadResult &read) {
            observed->settings.insert(targets[index], read);
            (*next)(index + 1);
        });
    };
    (*next)(0);
}

void ProfileRunner::start(std::shared_ptr<ProfileRun> run, bool resuming, Done done) {
    const auto refuse = [this, run, done](const QString &message) {
        m_coordinator->releaseLease();
        m_running = false;
        // A refusal before the first change leaves the record as it was, if there is one.
        ProfileRun refused = *run;
        refused.state.clear();
        refused.message = message;
        done(refused);
    };
    QString error;
    if (!m_coordinator->acquireLease(run->id, &error)) {
        ProfileRun refused = *run;
        refused.state.clear();
        refused.message = resuming && run->state == QLatin1String("running") ? tr("This restore is still running.") : error;
        done(refused);
        return;
    }
    m_running = true;
    m_cancelled = false;
    // Earlier changes the app died in the middle of are settled first, and a change
    // another window began just before the lease was taken is allowed to finish.
    for (const QString &message : m_coordinator->recoverInterrupted())
        run->message += (run->message.isEmpty() ? QString() : QStringLiteral("\n")) + message;
    if (!m_coordinator->waitUntilIdle(5000)) {
        refuse(tr("Another change is still being applied. Try again in a moment."));
        return;
    }
    // Everything is read again under the lease: the review was of what the plan saw.
    readAll(run, [this, run, resuming, done, refuse](const Observed &observed) {
        const QHash<QString, ReadResult> &reads = observed.settings;
        QStringList stale;
        for (ProfileRunItem &item : run->items) {
            if (item.kind == QLatin1String("package")) {
                if (item.state == QLatin1String("failed") || item.state == QLatin1String("blocked") || item.state == QLatin1String("skipped"))
                    continue;
                if (!observed.packages.ok) {
                    stale << tr("installed apps could not be read (%1)").arg(observed.packages.error);
                    continue;
                }
                const QString state = ProfilePlanner::packageState(observed.packages, item.target);
                const bool wasUnderway = item.state == QLatin1String("running") || item.state == QLatin1String("interrupted");
                if (ProfilePlanner::packageSatisfied(asPlanItem(item), state)) {
                    if (item.state != QLatin1String("verified"))
                        item.message = wasUnderway ? tr("Found already done when the restore resumed.") : tr("Already as saved.");
                    item.state = QStringLiteral("verified");
                    item.settled = state;
                } else if (item.state == QLatin1String("verified")) {
                    item.message = tr("Changed again after this restore verified it, and was left as it is.");
                } else if (state == item.planned) {
                    item.state = QStringLiteral("pending");
                    if (wasUnderway)
                        item.message = tr("Not done when the restore stopped, so it runs again.");
                } else if (!resuming) {
                    stale << item.label;
                } else {
                    item.state = wasUnderway ? QStringLiteral("interrupted") : QStringLiteral("skipped");
                    item.message = tr("Changed since the plan was made, so it was left as it is.");
                }
                continue;
            }
            const CatalogEntry *entry = m_catalog->find(item.target);
            const ReadResult read = reads.value(item.target);
            const ProfilePlanItem planned = asPlanItem(item);
            if (item.state == QLatin1String("verified")) {
                // Verified earlier in this run; still satisfied, it is simply done.
                if (entry && !ProfilePlanner::satisfied(*entry, planned, read))
                    item.message = tr("Changed again after this restore verified it, and was left as it is.");
                continue;
            }
            if (item.state == QLatin1String("failed") || item.state == QLatin1String("blocked") || item.state == QLatin1String("skipped"))
                continue;
            if (!entry) {
                item.state = QStringLiteral("blocked");
                item.message = tr("This version of Omarchy Control does not know this setting.");
                continue;
            }
            const bool wasUnderway = item.state == QLatin1String("running") || item.state == QLatin1String("interrupted");
            if (ProfilePlanner::satisfied(*entry, planned, read)) {
                item.state = QStringLiteral("verified");
                item.settled = read.value;
                item.message = wasUnderway ? tr("Found already done when the restore resumed.") : tr("Already as saved.");
                continue;
            }
            if (ProfilePlanner::unchangedSincePlan(*entry, planned, read)) {
                // Underway when the run stopped, and still as it was: nothing landed, or
                // it was put back. Either way it is safe to do again.
                item.state = QStringLiteral("pending");
                if (wasUnderway)
                    item.message = tr("Not done when the restore stopped, so it runs again.");
                continue;
            }
            if (!resuming) {
                stale << item.label;
                continue;
            }
            // Neither what the plan saw nor what it wanted: something else changed it, or
            // the stop came halfway. Either way it is not ours to overwrite.
            item.state = wasUnderway ? QStringLiteral("interrupted") : QStringLiteral("skipped");
            const QString shown = read.status == QLatin1String("ok") ? read.value : read.reason;
            item.message = wasUnderway ? tr("The restore stopped while changing this, and it now reads %1. Check it, then make a new plan if needed.").arg(shown)
                                       : tr("Changed since the plan was made, so it was left as it is: %1").arg(shown);
        }
        if (!stale.isEmpty()) {
            refuse(tr("Changed since this plan was made: %1. Make a new plan.").arg(stale.join(", ")));
            return;
        }
        if (!resuming)
            run->startedAt = now();
        run->state = QStringLiteral("running");
        QString error;
        // The record has to be on disk before the first change, or a crash would leave
        // changes no one can trace back to this restore.
        if (!persist(*run, &error)) {
            refuse(tr("The restore record could not be written, so nothing was changed: %1").arg(error));
            return;
        }
        execute(run, 0, done);
    });
}

void ProfileRunner::execute(std::shared_ptr<ProfileRun> run, int index, Done done) {
    while (index < run->items.size() && run->items[index].state != QLatin1String("pending"))
        ++index;
    if (index >= run->items.size()) {
        conclude(run, done);
        return;
    }
    if (m_cancelled) {
        run->state = QStringLiteral("cancelled");
        run->message = tr("Stopped before %1. Resume to continue.").arg(run->items[index].label);
        conclude(run, done);
        return;
    }
    // A command such as a theme change leaves work running behind it, like the icon pack's
    // theme hook, which takes the change lock for itself. The next change waits for that
    // rather than fail for being second.
    m_coordinator->waitUntilIdle(30000);
    ProfileRunItem &item = run->items[index];
    // What this depends on has to have worked first, as a default app needs its app.
    for (const QString &need : item.needs) {
        for (const ProfileRunItem &needed : run->items) {
            if (needed.id != need || needed.state == QLatin1String("verified"))
                continue;
            item.state = QStringLiteral("blocked");
            item.message = tr("Needs %1, which was not installed.").arg(needed.label);
            persist(*run);
            emit itemChanged(*run, index);
            execute(run, index + 1, done);
            return;
        }
    }
    if (item.kind == QLatin1String("package")) {
        executePackages(run, index, done);
        return;
    }
    item.state = QStringLiteral("running");
    item.message.clear();
    persist(*run);
    emit itemChanged(*run, index);
    const auto finished = [this, run, index, done](const Transaction &tx) {
        ProfileRunItem &step = run->items[index];
        step.transaction = tx.id;
        if (tx.phase == QLatin1String("committed")) {
            step.state = QStringLiteral("verified");
            step.settled = tx.after;
            step.message.clear();
            // This computer now agrees with the profile here, which later plans compare to.
            ProfileStore::recordBaseline(m_env, QJsonObject{{step.target, QJsonObject{{"mode", step.mode}, {"value", tx.after}, {"at", now()},
                                                                                       {"by", QStringLiteral("restore")}, {"run", run->id}}}});
        } else {
            step.state = QStringLiteral("failed");
            step.message = tx.message;
        }
        persist(*run);
        emit itemChanged(*run, index);
        execute(run, index + 1, done);
    };
    if (item.mode == QLatin1String("inherit"))
        m_engine->reset(item.target, finished, run->id);
    else
        m_engine->apply(item.target, item.desired, finished, QStringLiteral("set"), run->id);
}

void ProfileRunner::executePackages(std::shared_ptr<ProfileRun> run, int index, Done done) {
    // Every app with the same change goes in one transaction: one question from sudo and
    // one from pacman, which shows the whole transaction before doing anything.
    // An AUR build goes alone: one that fails or is cancelled leaves the others alone.
    const QString action = run->items[index].action;
    QList<int> batch;
    QStringList names, planned;
    for (int i = index; i < run->items.size(); ++i) {
        const ProfileRunItem &step = run->items[i];
        if (step.kind != QLatin1String("package") || step.action != action || step.state != QLatin1String("pending"))
            continue;
        if (action == QLatin1String("build") && i != index)
            continue;
        batch << i;
        names << step.target;
        for (const QString &package : step.packages)
            if (!planned.contains(package))
                planned << package;
    }
    const auto settle = [this, run, batch, index, done](const QString &state, const QString &message) {
        for (int i : batch) {
            run->items[i].state = state;
            run->items[i].message = message;
        }
        persist(*run);
        for (int i : batch)
            emit itemChanged(*run, i);
        execute(run, index + 1, done);
    };
    if (!m_packageTerminal || !m_packages) {
        settle(QStringLiteral("blocked"), tr("Apps are installed and removed in a terminal, where sudo and pacman can ask: restore from the command line."));
        return;
    }
    m_coordinator->waitUntilIdle(30000);
    // pacman's own lock is waited for, never removed. If it stays, the run pauses here and
    // resumes later from this step.
    QElapsedTimer waited;
    waited.start();
    while (m_packages->pacmanBusy() && waited.elapsed() < m_pacmanWaitMs)
        QThread::msleep(250);
    if (m_packages->pacmanBusy()) {
        run->state = QStringLiteral("paused");
        run->message = tr("Another package operation is still running. Resume this restore when it finishes.");
        conclude(run, done);
        return;
    }
    if (action == QLatin1String("mark-explicit") || action == QLatin1String("build")) {
        transact(run, batch, index, done);
        return;
    }
    // What pacman would do now has to be what was reviewed. Newer packages, or a dependency
    // that appeared or went, make it a different transaction, which needs a new plan.
    const auto compare = [this, run, batch, index, done, planned, settle](const PackageProvider::Preview &preview) {
        QStringList now = preview.packages, before = planned;
        now.sort();
        before.sort();
        if (!preview.ok)
            settle(QStringLiteral("failed"), tr("pacman refuses this now: %1").arg(preview.error));
        else if (now != before)
            settle(QStringLiteral("failed"), tr("What this would install or remove changed since the plan was made. Make a new plan."));
        else
            transact(run, batch, index, done);
    };
    if (action == QLatin1String("install"))
        m_packages->previewInstall(names, compare);
    else
        m_packages->previewRemoval(names, compare);
}

void ProfileRunner::transact(std::shared_ptr<ProfileRun> run, QList<int> batch, int index, Done done) {
    QStringList names;
    for (int i : batch)
        names << run->items[i].target;
    const QString action = run->items[batch.first()].action;
    // On disk before pacman starts, so a run that dies during it is settled by reading.
    for (int i : batch) {
        run->items[i].state = QStringLiteral("running");
        run->items[i].message.clear();
    }
    persist(*run);
    for (int i : batch)
        emit itemChanged(*run, i);

    ChangeCoordinator::Request request;
    request.settingId = QStringLiteral("packages");
    request.label = action == QLatin1String("install")   ? tr("Install %1").arg(names.join(", "))
                    : action == QLatin1String("build")  ? tr("Build %1 from the AUR").arg(names.join(", "))
                    : action == QLatin1String("remove") ? tr("Remove %1").arg(names.join(", "))
                                                        : tr("Keep %1 as wanted").arg(names.join(", "));
    request.kind = QStringLiteral("action");
    request.after = names.join(' ');
    // pacman's changes are not undone automatically, and its hooks can do more besides.
    request.recovery = QStringLiteral("no-undo");
    request.nothingToRestore = true;
    request.runId = run->id;
    request.apply = [this, names, action](ChangeCoordinator::Done finished) {
        QString error;
        const int code = action == QLatin1String("install")  ? m_packages->install(names, &error)
                         : action == QLatin1String("build")  ? m_packages->buildFromAur(names.first(), &error)
                         : action == QLatin1String("remove") ? m_packages->remove(names, &error)
                                                             : m_packages->markExplicit(names, &error);
        if (code == 0)
            finished(true, {});
        else
            finished(false, !error.isEmpty() ? error : tr("pacman did not finish (exit status %1).").arg(code));
    };
    m_coordinator->submit(std::move(request), [this, run, batch, index, done](const Transaction &tx) {
        // Each app is settled by reading it, whatever the exit status said: a refused
        // password changes nothing, and a transaction can stop partway.
        m_packages->readInventory([this, run, batch, index, done, tx](const PackageProvider::Inventory &inventory) {
            QJsonObject agreed;
            for (int i : batch) {
                ProfileRunItem &step = run->items[i];
                step.transaction = tx.id;
                if (!inventory.ok) {
                    step.state = QStringLiteral("interrupted");
                    step.message = tr("Its state could not be read afterwards: %1").arg(inventory.error);
                    continue;
                }
                const QString state = ProfilePlanner::packageState(inventory, step.target);
                step.settled = state;
                if (ProfilePlanner::packageSatisfied(asPlanItem(step), state)) {
                    step.state = QStringLiteral("verified");
                    step.message.clear();
                    agreed["package:" + step.target] = QJsonObject{{"mode", step.mode}, {"at", now()}, {"by", QStringLiteral("restore")}, {"run", run->id}};
                } else {
                    step.state = QStringLiteral("failed");
                    step.message = tx.phase == QLatin1String("committed") ? tr("pacman finished, but it is not as wanted.") : tx.message;
                }
            }
            ProfileStore::recordBaseline(m_env, agreed);
            persist(*run);
            for (int i : batch)
                emit itemChanged(*run, i);
            execute(run, index + 1, done);
        });
    });
}

void ProfileRunner::conclude(std::shared_ptr<ProfileRun> run, Done done) {
    if (run->state != QLatin1String("cancelled") && run->state != QLatin1String("paused")) {
        const int problems = run->count("failed") + run->count("interrupted") + run->count("blocked") + run->count("skipped");
        run->state = problems > 0       ? QStringLiteral("incomplete")
                     : run->leftOut > 0 ? QStringLiteral("partial")
                                        : QStringLiteral("completed");
        const int verified = run->count("verified");
        if (run->items.isEmpty())
            run->message = run->leftOut > 0 ? tr("Nothing needed to change. %1 items were left out of this restore.").arg(run->leftOut)
                                            : tr("Nothing needed to change: this computer already matches the profile.");
        else if (run->state == QLatin1String("completed"))
            run->message = tr("Restored and verified all %1 changes.").arg(verified);
        else if (run->state == QLatin1String("partial"))
            run->message = tr("Restored and verified %1 changes. %2 more were left out of this restore.").arg(verified).arg(run->leftOut);
        else
            run->message = tr("Restored and verified %1 of %2 changes. The rest need attention.").arg(verified).arg(run->items.size());
    }
    run->finishedAt = now();
    persist(*run);
    m_coordinator->releaseLease();
    m_running = false;
    done(*run);
}
