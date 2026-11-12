#include "profileprovider.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <memory>

#include "appcore.h"

namespace {

QString catalogText(const QString &text) {
    return QCoreApplication::translate("catalog", text.toUtf8().constData());
}

QVariantMap versionMap(const ProfileRepository::Version &version) {
    QVariantMap map = version.toJson().toVariantMap();
    map["short"] = version.commit.left(7);
    return map;
}

}  // namespace

ProfileProvider::ProfileProvider(AppCore *core, QObject *parent) : QObject(parent), m_core(core) {
    m_follow.setInterval(1000);
    connect(&m_follow, &QTimer::timeout, this, &ProfileProvider::followTerminalRun);
    // A restore running in this window reports each step as it lands.
    connect(&m_core->profileRunner, &ProfileRunner::itemChanged, this, [this](const ProfileRun &run, int) {
        m_run = runMap(run);
        emit changed();
    });
}

QVariantList ProfileProvider::categories() const {
    const QHash<QString, QString> labels{{"appearance", tr("Appearance")}, {"windows", tr("Windows")},   {"desktop", tr("Bar and dock position and style")},
                                         {"input", tr("Keyboard and mouse")}, {"idle", tr("Idle and lock")}, {"defaults", tr("Default apps")},
                                         {"language", tr("Language")}};
    QVariantList rows;
    for (const QString &id : ProfileTraits::categories())
        rows << QVariantMap{{"id", id}, {"label", labels.value(id, id)}};
    return rows;
}

void ProfileProvider::setBusy(const QString &text) {
    m_busyText = text;
    m_error.clear();
    m_message.clear();
    emit changed();
}

void ProfileProvider::finish(const QString &message, const QString &error) {
    m_busyText.clear();
    m_message = message;
    m_error = error;
    emit changed();
    if (!error.isEmpty())
        emit toast(error, QStringLiteral("error"));
    else if (!message.isEmpty())
        emit toast(message, QStringLiteral("ok"));
}

void ProfileProvider::refresh() {
    m_core->profileRepository.status([this](const ProfileRepository::Status &status) {
        QVariantMap map = status.toJson().toVariantMap();
        map["connected"] = !status.connection.isEmpty();
        if (!status.connection.isEmpty())
            map["repositoryName"] = status.connection.fullName();
        if (!status.saved.isEmpty())
            map["saved"] = versionMap(status.saved);
        if (!status.published.isEmpty())
            map["published"] = versionMap(status.published);
        const ProfileModel::Parsed parsed = ProfileModel::load(m_core->profileRepository.workingDirectory());
        map["hasProfile"] = parsed.ok;
        if (parsed.ok) {
            map["profileName"] = parsed.profile.name;
            map["settingCount"] = int(parsed.profile.settings.size());
            map["appCount"] = int(parsed.profile.packages.size());
            map["desktopCount"] = int(ProfileDesktop::targets(parsed.profile.desktop).size() + parsed.profile.files.size());
        }
        map["restoreRunning"] = m_core->coordinator.leaseHeldElsewhere();
        m_status = map;
        // A restore that is still running, or stopped and can go on, is shown on arrival,
        // including one started in a terminal before this window opened.
        if (m_run.isEmpty()) {
            const QList<ProfileRun> runs = m_core->profileRunner.runs(1);
            const bool elsewhere = m_core->coordinator.leaseHeldElsewhere();
            if (!runs.isEmpty() && runs.first().resumable() && (runs.first().id != m_dismissed || elsewhere))
                m_run = runMap(runs.first());
        }
        emit changed();
    });
}

ProfileCapture::Selection ProfileProvider::selectionFrom(const QVariantMap &selection) const {
    ProfileCapture::Selection out;
    out.categories = selection.value("categories").toStringList();
    out.suggested = selection.value("suggested").toBool();
    out.themes = selection.value("themes").toBool();
    out.background = selection.value("background").toBool();
    out.bar = selection.value("bar").toBool();
    out.plugins = selection.value("plugins").toBool();
    out.dock = selection.value("dock").toBool();
    out.launchers = selection.value("launchers").toBool();
    out.associations = selection.value("associations").toBool();
    out.shortcuts = selection.value("shortcuts").toBool();
    out.windowRules = selection.value("windowRules").toBool();
    out.configFiles = selection.value("configFiles").toStringList();
    out.suggestedApps = selection.value("suggestedApps").toBool();
    out.apps = selection.value("apps").toStringList();
    out.name = selection.value("name").toString().trimmed();
    return out;
}

QString ProfileProvider::shownValue(const QString &id, const QString &value) const {
    const CatalogEntry *entry = m_core->catalog.find(id);
    if (!entry)
        return value;
    QVariantList choices;
    for (const QJsonValue &choice : entry->choices)
        choices << QVariantMap{{"value", choice.toObject().value("value").toString()}, {"label", choice.toObject().value("label").toString()}};
    const QString shown = m_core->engine.displayValue(*entry, value, choices);
    return shown.isEmpty() ? tr("(empty)") : shown;
}

QVariantMap ProfileProvider::captureItem(const ProfileCapture::Item &item) const {
    QVariantMap row{{"kind", item.kind},     {"id", item.id},         {"category", item.category},
                    {"change", item.change}, {"reason", item.reason}, {"attention", item.attention}};
    const auto setting = [this, &item](const ProfileSetting &value) {
        return value.mode.isEmpty() ? QString() : value.mode == QLatin1String("inherit") ? tr("This computer's default") : shownValue(item.id, value.value);
    };
    if (item.kind == QLatin1String("setting")) {
        const CatalogEntry *entry = m_core->catalog.find(item.id);
        row["label"] = entry ? catalogText(entry->label) : item.id;
        row["before"] = setting(item.before);
        row["after"] = setting(item.after);
    } else if (item.kind == QLatin1String("package")) {
        const auto intent = [](const ProfilePackage &package) {
            return package.name.isEmpty() ? QString() : package.intent == QLatin1String("absent") ? tr("Removed by a restore") : tr("Installed by a restore");
        };
        row["label"] = item.label.isEmpty() ? item.id.section(':', 1) : item.label;
        row["name"] = item.id.section(':', 1);
        row["before"] = intent(item.packageBefore);
        row["after"] = intent(item.packageAfter);
    } else {
        row["label"] = item.label;
        row["before"] = item.before.value;
        row["after"] = item.after.value;
    }
    return row;
}

void ProfileProvider::startCapture(const QVariantMap &selection) {
    if (busy())
        return;
    m_selection = selectionFrom(selection);
    setBusy(tr("Reading this computer…"));
    // This computer's profile picks up where its history is, never from nothing.
    m_core->profileRepository.prepareWorking([this](const ProfileRepository::Result &prepared) {
        if (!prepared.ok) {
            finish({}, prepared.error);
            return;
        }
        capture(m_selection);
    });
}

void ProfileProvider::includeApps(const QStringList &apps) {
    if (busy() || apps.isEmpty())
        return;
    for (const QString &app : apps)
        if (!m_selection.apps.contains(app))
            m_selection.apps << app;
    setBusy(tr("Reading this computer…"));
    capture(m_selection);
}

void ProfileProvider::capture(const ProfileCapture::Selection &selection) {
    bool exists = false;
    const auto parsed = std::make_shared<ProfileModel::Parsed>(ProfileModel::load(m_core->profileRepository.workingDirectory(), &exists));
    if (exists && !parsed->ok) {
        finish({}, tr("This computer's profile cannot be used: %1").arg(parsed->errors.value(0)));
        return;
    }
    if (!exists && selection.isEmpty()) {
        finish({}, tr("Choose at least one thing to remember."));
        return;
    }
    m_core->profileCapture.capture(exists ? &parsed->profile : nullptr, selection, [this, parsed](const ProfileCapture::Result &result) {
        m_capture = result;
        m_hasCapture = result.ok;
        QVariantList items, suggested;
        QVariantMap counts;
        int unchanged = 0;
        for (const ProfileCapture::Item &item : result.items) {
            if (item.change == QLatin1String("suggested")) {
                suggested << captureItem(item);
                continue;
            }
            if (item.change == QLatin1String("unchanged")) {
                ++unchanged;
                continue;
            }
            items << captureItem(item);
            counts[item.change] = counts.value(item.change).toInt() + 1;
        }
        m_review = QVariantMap{{"ok", result.ok},     {"error", result.error}, {"changed", result.changed}, {"existed", result.existed},
                               {"items", items},      {"suggestedApps", suggested}, {"counts", counts},     {"unchanged", unchanged},
                               {"connected", !m_core->profileRepository.connection().isEmpty()}};
        finish({}, result.ok ? QString() : result.error);
    });
}

void ProfileProvider::saveReview(const QString &message, bool publish) {
    if (busy())
        return;
    if (!m_hasCapture) {
        finish({}, tr("Review what would be saved first."));
        return;
    }
    setBusy(tr("Saving…"));
    QString error;
    if (!m_core->profileCapture.save(m_core->profileRepository.workingDirectory(), m_capture, &error)) {
        finish({}, error);
        return;
    }
    m_core->profileRepository.save(message, [this, publish](const ProfileRepository::Result &saved) {
        if (!saved.ok) {
            finish({}, saved.error);
            refresh();
            return;
        }
        m_hasCapture = false;
        m_review.clear();
        if (!publish) {
            finish(saved.message);
            refresh();
            return;
        }
        setBusy(tr("Publishing…"));
        m_core->profileRepository.publish([this, saved](const ProfileRepository::Result &published) {
            finish(published.ok ? published.message : QString(), published.ok ? QString() : published.error);
            refresh();
        });
    });
}

void ProfileProvider::clearReview() {
    m_review.clear();
    m_hasCapture = false;
    emit changed();
}

void ProfileProvider::connectRepository(const QString &repository, bool allowPublic) {
    if (busy())
        return;
    setBusy(tr("Asking GitHub…"));
    m_core->profileRepository.connectTo(repository, QString(), allowPublic, [this](const ProfileRepository::Result &result) {
        finish(result.message, result.ok ? QString() : result.error);
        refresh();
    });
}

void ProfileProvider::createRepository(const QString &name) {
    if (busy())
        return;
    setBusy(tr("Creating a private repository…"));
    m_core->profileRepository.create(name, [this](const ProfileRepository::Result &result) {
        finish(result.message, result.ok ? QString() : result.error);
        refresh();
    });
}

void ProfileProvider::fetch() {
    if (busy())
        return;
    setBusy(tr("Getting the published versions…"));
    m_core->profileRepository.fetch([this](const ProfileRepository::Result &result) {
        finish(result.message, result.ok ? QString() : result.error);
        refresh();
        loadVersions();
    });
}

void ProfileProvider::publish() {
    if (busy())
        return;
    setBusy(tr("Publishing…"));
    m_core->profileRepository.publish([this](const ProfileRepository::Result &result) {
        finish(result.message, result.ok ? QString() : result.error);
        refresh();
    });
}

void ProfileProvider::resolve(const QString &choice) {
    if (busy())
        return;
    setBusy(choice == QLatin1String("published") ? tr("Taking the published version…") : tr("Keeping this computer's version…"));
    m_core->profileRepository.resolve(choice, [this](const ProfileRepository::Result &result) {
        finish(result.message, result.ok ? QString() : result.error);
        refresh();
        loadVersions();
    });
}

void ProfileProvider::loadVersions() {
    m_core->profileRepository.history(false, 50, [this](const QList<ProfileRepository::Version> &saved, const QString &) {
        m_versions.clear();
        for (const ProfileRepository::Version &version : saved)
            m_versions << versionMap(version);
        m_core->profileRepository.history(true, 50, [this](const QList<ProfileRepository::Version> &published, const QString &) {
            m_publishedVersions.clear();
            for (const ProfileRepository::Version &version : published)
                m_publishedVersions << versionMap(version);
            emit changed();
        });
    });
}

QVariantMap ProfileProvider::planItem(const ProfilePlanItem &item) const {
    QVariantMap row{{"id", item.id},
                    {"kind", item.kind},
                    {"target", item.target},
                    {"category", item.category},
                    {"status", item.status},
                    {"reason", item.reason},
                    {"resolution", item.resolution},
                    {"action", item.action},
                    {"effects", item.effects},
                    {"needs", item.needs},
                    {"confirmText", item.confirmText},
                    {"willRun", item.willRun()}};
    if (item.kind == QLatin1String("setting")) {
        row["label"] = catalogText(item.label);
        row["desired"] = item.mode == QLatin1String("inherit") ? tr("This computer's default") : shownValue(item.target, item.desired);
        row["current"] = item.current.isEmpty() ? QString() : shownValue(item.target, item.current);
    } else if (item.kind == QLatin1String("package")) {
        row["label"] = item.target;
        const QHash<QString, QString> actions{{"install", tr("Install")}, {"build", tr("Build from the AUR")}, {"mark-explicit", tr("Keep as an app of its own")},
                                              {"remove", tr("Remove")}};
        row["desired"] = actions.value(item.action, item.mode == QLatin1String("absent") ? tr("Not installed") : tr("Installed"));
        row["current"] = item.currentStatus == QLatin1String("missing") ? tr("Not installed") : tr("Installed");
        row["transaction"] = item.transaction;
    } else {
        row["label"] = item.label;
        row["desired"] = ProfileDesktop::summary(item.target, item.desired);
        row["current"] = item.current.isEmpty() ? QString() : ProfileDesktop::summary(item.target, item.current);
    }
    return row;
}

void ProfileProvider::planRestore(const QString &revision) {
    if (busy())
        return;
    m_planRevision = revision;
    m_planOptions = ProfilePlanner::Options();
    m_run.clear();
    if (revision == QLatin1String("working")) {
        m_planDirectory = m_core->profileRepository.workingDirectory();
        replan();
        return;
    }
    setBusy(tr("Reading the version…"));
    m_core->profileRepository.read(revision, [this](const ProfileRepository::Result &read) {
        if (!read.ok) {
            m_plan = QVariantMap{{"valid", false}, {"errors", QStringList{read.error} + read.problems}, {"revision", m_planRevision}};
            finish({}, read.error);
            return;
        }
        m_planDirectory = read.directory;
        replan();
    });
}

void ProfileProvider::replan() {
    setBusy(tr("Comparing with this computer…"));
    m_core->profilePlanner.plan(m_planDirectory, m_planOptions, [this](const ProfilePlan &plan) {
        m_currentPlan = plan;
        // Saved where the command line, or a terminal this starts, can apply it.
        QString saveError;
        if (plan.valid && !m_core->profilePlanner.save(plan, &saveError)) {
            finish({}, tr("The restore plan could not be saved: %1").arg(saveError));
            return;
        }
        QVariantList items;
        QVariantMap counts;
        int willRun = 0;
        bool apps = false;
        for (const ProfilePlanItem &item : plan.items) {
            items << planItem(item);
            counts[item.status] = counts.value(item.status).toInt() + 1;
            if (item.willRun())
                ++willRun;
            apps = apps || (item.kind == QLatin1String("package") && item.willRun());
        }
        int undecided = 0;
        for (const ProfilePlanItem &item : plan.items)
            undecided += item.status == QLatin1String("conflict") && item.resolution.isEmpty() ? 1 : 0;
        m_plan = QVariantMap{{"id", plan.id},         {"valid", plan.valid},         {"errors", plan.errors},     {"revision", m_planRevision},
                             {"profileName", plan.profileName}, {"items", items},   {"counts", counts},          {"willRun", willRun},
                             {"leftOut", plan.leftOut()},       {"undecided", undecided}, {"needsTerminal", apps}};
        finish({}, plan.valid ? QString() : plan.errors.value(0));
    });
}

void ProfileProvider::choose(const QString &itemId, const QString &resolution) {
    if (busy() || m_planDirectory.isEmpty())
        return;
    QString target;
    for (const ProfilePlanItem &item : m_currentPlan.items)
        if (item.id == itemId)
            target = item.target;
    if (target.isEmpty())
        return;
    // The planner reads a decision by the item's target, as the command line names it.
    for (QStringList *list : {&m_planOptions.useSaved, &m_planOptions.keepLocal}) {
        list->removeAll(target);
        list->removeAll(itemId);
    }
    if (resolution == QLatin1String("use-saved"))
        m_planOptions.useSaved << target << itemId;
    else if (resolution == QLatin1String("keep-local"))
        m_planOptions.keepLocal << target << itemId;
    replan();
}

void ProfileProvider::clearPlan() {
    m_plan.clear();
    m_currentPlan = ProfilePlan();
    m_planDirectory.clear();
    m_planOptions = ProfilePlanner::Options();
    emit changed();
}

QVariantMap ProfileProvider::runMap(const ProfileRun &run) const {
    QVariantList items;
    QVariantMap counts;
    for (const ProfileRunItem &item : run.items) {
        items << QVariantMap{{"id", item.id},
                             {"kind", item.kind},
                             {"label", item.kind == QLatin1String("setting") ? catalogText(item.label) : item.label},
                             {"state", item.state},
                             {"message", item.message}};
        counts[item.state] = counts.value(item.state).toInt() + 1;
    }
    const bool elsewhere = m_core->coordinator.leaseHeldElsewhere();
    return QVariantMap{{"id", run.id},
                       {"planId", run.planId},
                       {"state", run.state},
                       {"message", run.message},
                       {"profileName", run.profileName},
                       {"items", items},
                       {"counts", counts},
                       {"total", int(run.items.size())},
                       {"leftOut", run.leftOut},
                       {"runningElsewhere", elsewhere},
                       {"here", m_core->profileRunner.running()},
                       // A run that died while running reads as running until something
                       // settles it; with no one holding its lease, it can go on.
                       {"resumable", run.resumable() && !elsewhere && !m_core->profileRunner.running()}};
}

void ProfileProvider::startRestore(bool partial) {
    if (busy() || !m_currentPlan.valid || m_core->profileRunner.running())
        return;
    if (m_currentPlan.leftOut() > 0 && !partial) {
        finish({}, tr("Some items are left out of this restore. Confirm that you want to restore the rest."));
        return;
    }
    bool apps = false;
    for (const ProfilePlanItem &item : m_currentPlan.items)
        apps = apps || (item.kind == QLatin1String("package") && item.willRun());
    if (apps) {
        // Apps change in a terminal, where sudo and pacman ask before anything happens. The
        // restore there holds the lease and keeps going if this window closes.
        QString self = m_core->env.selfBinary;
        static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9/._+-]+$"));
        if (!safe.match(self).hasMatch())
            self = QStringLiteral("omarchy-control");
        const QString command = self + " profile apply " + m_currentPlan.id + " --yes" + (partial ? " --partial" : "");
        CommandOptions options;
        options.timeoutMs = 15000;
        m_core->runner.run({"omarchy-launch-floating-terminal-with-presentation", command}, options, this, [this](const CommandResult &result) {
            if (!result.ok() && result.failure != CommandResult::None) {
                finish({}, tr("The terminal could not be opened: %1").arg(result.describe(QStringLiteral("omarchy"))));
                return;
            }
        });
        m_run = QVariantMap{{"planId", m_currentPlan.id}, {"state", QStringLiteral("starting")}, {"terminal", true},
                            {"message", tr("Restoring in the terminal that opened. Answer sudo and pacman there; this window follows along.")}};
        m_followTicks = 0;
        m_follow.start();
        emit changed();
        return;
    }
    m_run = QVariantMap{{"planId", m_currentPlan.id}, {"state", QStringLiteral("starting")}, {"message", tr("Starting…")}};
    emit changed();
    m_core->profileRunner.apply(m_currentPlan, [this](const ProfileRun &run) {
        if (run.state.isEmpty()) {
            m_run.clear();
            finish({}, run.message);
            return;
        }
        m_run = runMap(run);
        finish(run.message);
        refresh();
    });
}

void ProfileProvider::followTerminalRun() {
    ++m_followTicks;
    const QString planId = m_run.value("planId").toString();
    for (const ProfileRun &run : m_core->profileRunner.runs(5)) {
        if (run.planId != planId)
            continue;
        m_run = runMap(run);
        m_run["terminal"] = true;
        const bool going = run.state == QLatin1String("running") && m_core->coordinator.leaseHeldElsewhere();
        if (!going) {
            m_follow.stop();
            refresh();
        }
        emit changed();
        return;
    }
    // Nothing started: the terminal was closed first, or the restore was refused before it
    // began, which the terminal shows.
    if (m_followTicks >= 600 || (m_followTicks >= 20 && !m_core->coordinator.leaseHeldElsewhere())) {
        m_follow.stop();
        m_run = QVariantMap{{"planId", planId}, {"state", QStringLiteral("not-started")}, {"terminal", true},
                            {"message", tr("The restore did not start. The terminal says why, if it is still open. Nothing was changed.")}};
        emit changed();
    }
}

void ProfileProvider::resumeRestore() {
    const QString id = m_run.value("id").toString();
    if (id.isEmpty() || busy() || m_core->profileRunner.running())
        return;
    const ProfileRun run = m_core->profileRunner.load(id);
    bool apps = false;
    for (const ProfileRunItem &item : run.items)
        apps = apps || (item.kind == QLatin1String("package") && item.state != QLatin1String("verified") && item.state != QLatin1String("blocked")
                        && item.state != QLatin1String("failed"));
    if (apps && ProfileStore::isId(id, QStringLiteral("run"))) {
        QString self = m_core->env.selfBinary;
        static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9/._+-]+$"));
        if (!safe.match(self).hasMatch())
            self = QStringLiteral("omarchy-control");
        m_core->runner.run({"omarchy-launch-floating-terminal-with-presentation", self + " profile resume " + id}, this, [](const CommandResult &) {});
        m_run["terminal"] = true;
        m_run["state"] = QStringLiteral("starting");
        m_followTicks = 0;
        m_follow.start();
        emit changed();
        return;
    }
    m_run["state"] = QStringLiteral("starting");
    emit changed();
    m_core->profileRunner.resume(id, [this](const ProfileRun &resumed) {
        if (resumed.state.isEmpty()) {
            finish({}, resumed.message);
            return;
        }
        m_run = runMap(resumed);
        finish(resumed.message);
        refresh();
    });
}

void ProfileProvider::cancelRestore() {
    if (m_core->profileRunner.running())
        m_core->profileRunner.cancel();
}

void ProfileProvider::clearRun() {
    if (m_core->profileRunner.running())
        return;
    m_follow.stop();
    m_dismissed = m_run.value("id").toString();
    m_run.clear();
    emit changed();
}
