#include "hyprchange.h"

#include <QFile>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>
#include <sys/stat.h>
#include <unistd.h>

#include "compat.h"
#include "fileops.h"
#include "hypromodule.h"

namespace {
QString tr(const char *text) { return QCoreApplication::translate("HyprChange", text); }

bool readBounded(const QString &path, QByteArray *out) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    *out = file.read(4 * 1024 * 1024 + 1);
    return file.error() == QFileDevice::NoError && out->size() <= 4 * 1024 * 1024;
}

bool safePaths(const Env &env) {
    // Read-only discovery follows paths, but mutation refuses linked directories
    // and files, hard links and files owned by another user.
    for (const QString &path : {env.configHome, env.configHome + "/hypr"}) {
        const QFileInfo info(path);
        if (info.isSymLink() || (info.exists() && (!info.isDir() || info.ownerId() != getuid() || !info.isWritable()))) return false;
    }
    HyprModule module(env);
    for (const QString &path : {module.path(), module.hyprlandPath()}) {
        const QFileInfo info(path);
        struct stat state {};
        if (info.isSymLink() || (info.exists() && (!info.isFile() || info.ownerId() != getuid()
            || ::stat(QFile::encodeName(path).constData(), &state) != 0 || state.st_nlink != 1))) return false;
    }
    return true;
}

bool activeImport(const QByteArray &text, const QByteArray &call) {
    int count = 0;
    for (const auto &line : text.split('\n')) {
        const auto trimmed = line.trimmed();
        if (trimmed == call || trimmed.startsWith(call + " --")) ++count;
    }
    return count == 1;
}

void errors(CommandRunner *runner, QObject *context, std::function<void(bool, QStringList)> done) {
    runner->run(runner->env().hyprctl({"-j", "configerrors"}), CommandOptions{4000}, context, [done](const CommandResult &read) {
        const auto doc = QJsonDocument::fromJson(read.out);
        bool ok = read.ok() && !read.truncated && doc.isArray();
        QStringList rows;
        for (const auto &item : doc.array()) {
            ok &= item.isString();
            if (!item.toString().trimmed().isEmpty()) rows << item.toString().trimmed();
        }
        done(ok, rows);
    });
}

QString newErrors(const QStringList &after, QStringList baseline) {
    for (const auto &text : after) {
        const int index = baseline.indexOf(text);
        if (index < 0) return tr("Hyprland reported a config error: %1").arg(text.left(240));
        baseline.removeAt(index);
    }
    return {};
}
}

namespace HyprChange {
ProviderPlan base(const Env &env, const QString &operation, const QString &target, const QString &collection) {
    ProviderPlan plan;
    plan.operation = operation; plan.target = target;
    plan.details = {{"collection", collection}, {"verificationScope", "owned-file-and-reload"},
                    {"failureRecovery", "Restore captured files and verify a recovery reload; later edits are preserved."}, {"historyUndo", false}};
    auto refuse = [&](const QString &code, const QString &reason) { plan.errorCode = code; plan.error = reason; return plan; };
    if (!safePaths(env)) return refuse("ownership_conflict", tr("Linked or unowned Hyprland paths cannot be changed here."));
    HyprModule module(env);
    const auto owned = module.inspect();
    const QString status = owned.value("status").toString();
    if (status != QLatin1String("ok") && status != QLatin1String("absent"))
        return refuse(status, owned.value("reason").toString());
    QByteArray main;
    if (!readBounded(module.hyprlandPath(), &main)) return refuse("read_unavailable", tr("hyprland.lua could not be read completely."));
    const QByteArray import = "require(\"hypr.omarchy-control\")";
    if (!activeImport(main, import) && (main.contains("hypr.omarchy-control") || !activeImport(main, "require(\"hypr.autostart\")")))
        return refuse("unsupported_configuration", tr("hyprland.lua needs one active Control import or Omarchy's stock autostart anchor."));
    plan.details["owned"] = owned;
    plan.details["entriesBefore"] = owned.value(collection);
    plan.details["files"] = QJsonArray{module.path(), module.hyprlandPath()};
    plan.details["fingerprints"] = QJsonObject{{module.path(), status == QLatin1String("absent") ? "missing" : owned.value("fingerprint").toString()},
                                               {module.hyprlandPath(), FileOps::fingerprintBytes(main)}};
    plan.effects = {tr("Write the owned Hyprland module, add its import if needed, and reload Hyprland."),
                    tr("Other owned choices are preserved. No command is launched by saving a shortcut."),
                    tr("Failed verification restores captured files. Automatic history undo is unavailable.")};
    plan.ok = true;
    return plan;
}

QJsonArray selectable(const QJsonObject &snapshot, const QString &collection) {
    auto rows = snapshot.value(collection).toArray();
    for (int i = 0; i < rows.size(); ++i) {
        auto row = rows[i].toObject();
        row["index"] = i;
        row["selector"] = QString::number(i) + '@' + snapshot.value("fingerprint").toString();
        rows[i] = row;
    }
    return rows;
}

int selected(const ProviderPlan &plan, const QString &selector) {
    static const QRegularExpression grammar("\\A(0|[1-9][0-9]*)@([a-f0-9]{64})\\z");
    const auto match = grammar.match(selector);
    bool ok = false;
    const int index = match.captured(1).toInt(&ok);
    if (!match.hasMatch() || !ok || match.captured(2) != plan.details.value("owned").toObject().value("fingerprint").toString()
        || index >= plan.details.value("entriesBefore").toArray().size()) return -1;
    return index;
}

bool validSelector(const QString &selector) {
    static const QRegularExpression grammar("\\A(0|[1-9][0-9]*)@[a-f0-9]{64}\\z");
    const auto match = grammar.match(selector);
    bool ok = false;
    match.captured(1).toInt(&ok);
    return match.hasMatch() && ok;
}

void runtime(CommandRunner *runner, QObject *context, ProviderPlan plan, std::function<void(ProviderPlan)> done) {
    if (!plan.ok) { done(plan); return; }
    runner->run(runner->env().hyprctl({"-j", "version"}), CommandOptions{4000}, context, [runner, context, plan, done](const CommandResult &read) mutable {
        const auto doc = QJsonDocument::fromJson(read.out);
        const QString version = doc.object().value("version").toString();
        if (!read.ok() || read.truncated || !doc.isObject() || !QRegularExpression("\\A[0-9]+\\.[0-9]+\\.[0-9]+[^\\r\\n]*\\z").match(version).hasMatch()) {
            plan.ok = false; plan.errorCode = "read_unavailable"; plan.error = tr("The Hyprland version could not be read."); done(plan); return;
        }
        CatalogEntry entry; entry.write = {{"kind", "hypr"}};
        const QString changed = Compat(runner->env()).changedFor(entry, version);
        if (!changed.isEmpty()) {
            plan.ok = false; plan.errorCode = "compatibility_blocked"; plan.error = tr("%1 changed since testing. Review compatibility before changing the module.").arg(changed); done(plan); return;
        }
        plan.details["hyprlandVersion"] = version;
        errors(runner, context, [plan, done](bool ok, const QStringList &rows) mutable {
            if (!ok) { plan.ok = false; plan.errorCode = "read_unavailable"; plan.error = tr("Hyprland's config errors could not be read completely."); }
            plan.details["configErrorsBefore"] = QJsonArray::fromStringList(rows);
            done(plan);
        });
    });
}

void submit(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *context, const ProviderPlan &plan,
            ChangeCoordinator::Step preflight, ChangeCoordinator::Step verify, ChangeCoordinator::Finished finished) {
    auto baseline = std::make_shared<QStringList>();
    auto wrote = std::make_shared<bool>(false);
    auto afterFiles = std::make_shared<QHash<QString, QString>>();
    ChangeCoordinator::Request request;
    request.settingId = plan.operation.startsWith("shortcuts.") ? "shortcuts" : "windowrules";
    const auto record = plan.details.value("record").toObject();
    request.label = plan.operation.startsWith("shortcuts.") ? tr("Shortcut %1").arg(record.value("keys").toString())
        : plan.operation == QLatin1String("rules.remove") ? tr("Window rule removed") : tr("Window rule for %1").arg(record.value("match").toString());
    request.before = plan.before; request.after = plan.after;
    request.extra = {{"operation", plan.operation}, {"target", plan.target}, {"verificationScope", plan.details.value("verificationScope")}};
    HyprModule probe(runner->env());
    request.files = {probe.path(), probe.hyprlandPath()};
    const auto expected = plan.details.value("fingerprints").toObject();
    for (auto it = expected.begin(); it != expected.end(); ++it) {
        request.expectedFingerprints.insert(it.key(), it.value().toString());
        afterFiles->insert(it.key(), it.value().toString());
    }
    request.writtenFingerprints = [afterFiles] { return *afterFiles; };
    request.preflight = [runner, context, plan, baseline, preflight](ChangeCoordinator::Done done) {
        if (!safePaths(runner->env())) { done(false, tr("Linked or unowned Hyprland paths cannot be changed here.")); return; }
        runtime(runner, context, plan, [plan, baseline, preflight, done](const ProviderPlan &fresh) {
            if (!fresh.ok) { done(false, fresh.error); return; }
            if (fresh.details.value("hyprlandVersion") != plan.details.value("hyprlandVersion")) { done(false, tr("The Hyprland session changed. Plan the change again.")); return; }
            for (const auto &value : fresh.details.value("configErrorsBefore").toArray()) baseline->append(value.toString());
            if (preflight) preflight(done); else done(true, {});
        });
    };
    request.apply = [runner, context, plan, wrote, afterFiles](ChangeCoordinator::Done done) {
        if (!safePaths(runner->env())) { done(false, tr("Linked or unowned Hyprland paths cannot be changed here.")); return; }
        HyprModule module(runner->env());
        QString error;
        if (!module.load(&error)) { done(false, error); return; }
        if (plan.details.value("collection").toString() == QLatin1String("binds")) module.setBinds(plan.details.value("entriesAfter").toArray());
        else module.setRules(plan.details.value("entriesAfter").toArray());
        *wrote = true;
        if (!module.save(&error)) { done(false, error); return; }
        afterFiles->insert(module.path(), FileOps::fingerprintBytes(module.render()));
        if (!module.installImport(&error)) { done(false, error); return; }
        afterFiles->insert(module.hyprlandPath(), FileOps::fingerprint(module.hyprlandPath()));
        CommandOptions options; options.mutation = true;
        runner->run(runner->env().hyprctl({"reload"}), options, context, [done](const CommandResult &read) { done(read.ok() && !read.truncated, read.describe("Hyprland")); });
    };
    request.verify = [runner, context, plan, afterFiles, baseline, verify](ChangeCoordinator::Done done) {
        QTimer::singleShot(150, context, [runner, context, plan, afterFiles, baseline, verify, done] {
            for (auto it = afterFiles->begin(); it != afterFiles->end(); ++it)
                if (FileOps::fingerprint(it.key()) != it.value()) { done(false, tr("A Hyprland file changed before verification finished.")); return; }
            const auto owned = HyprModule(runner->env()).inspect();
            QByteArray main;
            if (owned.value("status").toString() != QLatin1String("ok") || owned.value(plan.details.value("collection").toString()) != plan.details.value("entriesAfter")
                || !readBounded(HyprModule(runner->env()).hyprlandPath(), &main) || !activeImport(main, "require(\"hypr.omarchy-control\")")) {
                done(false, tr("The owned Hyprland entries or active import were not confirmed.")); return;
            }
            errors(runner, context, [baseline, verify, done](bool ok, const QStringList &rows) {
                const QString error = newErrors(rows, *baseline);
                if (!ok || !error.isEmpty()) { done(false, ok ? error : tr("Hyprland's config errors could not be read completely.")); return; }
                if (verify) verify(done); else done(true, {});
            });
        });
    };
    request.rollback = [runner, context, wrote, baseline](ChangeCoordinator::Done done) {
        if (!*wrote) { done(true, {}); return; }
        CommandOptions options; options.mutation = true;
        runner->run(runner->env().hyprctl({"reload"}), options, context, [runner, context, baseline, done](const CommandResult &read) {
            if (!read.ok() || read.truncated) { done(false, tr("The recovery reload failed.")); return; }
            QTimer::singleShot(150, context, [runner, context, baseline, done] {
                errors(runner, context, [baseline, done](bool ok, const QStringList &rows) {
                    const QString error = newErrors(rows, *baseline);
                    done(ok && error.isEmpty(), ok ? error : tr("The recovery reload could not be checked."));
                });
            });
        });
    };
    coordinator->submit(std::move(request), std::move(finished));
}
}
