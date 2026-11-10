#include "displayprovider.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <numeric>

#include "fileops.h"
#include "hypromodule.h"

static constexpr int PreviewSeconds = 15;
static constexpr int CheckSeconds = 3;
// Far enough to show up in Hyprland's answer, too small to matter for three seconds.
static constexpr int CheckNudgePixels = 8;
// A rule is applied on a later frame. One read per frame for about a second
// still finishes well inside the safety timer.
static constexpr int CheckObserveAttempts = 20;
static constexpr int CheckObserveIntervalMs = 50;

bool DisplayProvider::isSafeOutputName(const QString &name) {
    // The output name is written into Lua that Hyprland evaluates, and EDID
    // data can influence it, so only plain connector names may pass.
    static const QRegularExpression safe("^[A-Za-z0-9._-]+$");
    return safe.match(name).hasMatch();
}

double DisplayProvider::cleanScale(double requested, int width, int height) {
    // Hyprland accepts a scale only when the mode divides into whole logical
    // pixels in 1/120 steps, the same rule omarchy-hyprland-monitor-scaling uses.
    if (width <= 0 || height <= 0)
        return requested;
    const long long g = std::gcd<long long, long long>(width * 120LL, height * 120LL);
    long long k = qRound64(requested * 120);
    if (k > g)
        k = g;
    if (k < 1)
        k = 1;
    while (g % k != 0)
        ++k;
    return double(k) / 120.0;
}

QSize DisplayProvider::logicalSize(const QVariantMap &row) {
    const double scale = std::max(0.25, row.value("scale").toDouble());
    int width = row.value("width").toInt(), height = row.value("height").toInt();
    if (row.value("transform").toInt() % 2 == 1)
        std::swap(width, height);
    return QSize(qRound(width / scale), qRound(height / scale));
}

QVariantList DisplayProvider::scaleChoices(int width, int height, double current, double lowest, double highest) {
    // The usual steps, each moved to the nearest scale this mode divides into exactly,
    // so what is offered is what Hyprland will actually use.
    static const double presets[] = {1, 1.25, 1.5, 1.6, 1.75, 2, 2.5, 3, 4};
    QList<double> values;
    auto add = [&values](double value) {
        for (double existing : std::as_const(values))
            if (std::abs(existing - value) < 0.001)
                return;
        values << value;
    };
    for (double preset : presets) {
        const double value = cleanScale(preset, width, height);
        if (value >= lowest - 1e-9 && value <= highest + 1e-9)
            add(value);
    }
    if (current > 0)
        add(current);
    std::sort(values.begin(), values.end());
    QVariantList out;
    for (double value : std::as_const(values))
        out << QVariantMap{{"value", value}, {"label", QString::number(qRound(value * 100)) + "%"}};
    return out;
}

QVariantList DisplayProvider::parseMonitors(const QByteArray &json) {
    QVariantList rows;
    for (const auto &value : QJsonDocument::fromJson(json).array()) {
        const auto m = value.toObject();
        QVariantMap row;
        row["output"] = m.value("name").toString();
        row["description"] = m.value("description").toString();
        row["make"] = m.value("make").toString();
        row["model"] = m.value("model").toString();
        row["width"] = m.value("width").toInt();
        row["height"] = m.value("height").toInt();
        row["refresh"] = m.value("refreshRate").toDouble();
        row["x"] = m.value("x").toInt();
        row["y"] = m.value("y").toInt();
        row["scale"] = m.value("scale").toDouble();
        row["transform"] = m.value("transform").toInt();
        row["focused"] = m.value("focused").toBool();
        row["enabled"] = !m.value("disabled").toBool();
        row["vrr"] = m.value("vrr").toBool();
        row["mirrorOf"] = m.value("mirrorOf").toString();
        row["internal"] = m.value("name").toString().contains(QRegularExpression("^(eDP|LVDS|DSI)-"));
        QStringList modes;
        for (const auto &mode : m.value("availableModes").toArray())
            modes << mode.toString().remove("Hz");
        row["modes"] = modes;
        row["mode"] = QString("%1x%2@%3").arg(m.value("width").toInt()).arg(m.value("height").toInt()).arg(m.value("refreshRate").toDouble(), 0, 'f', 2);
        rows << row;
    }
    return rows;
}

QString DisplayProvider::monitorLua(const QJsonObject &m) {
    const QString output = m.value("output").toString();
    if (m.value("disabled").toBool())
        return QString("hl.monitor({ output = \"%1\", disabled = true })").arg(output);
    QString lua = QString("hl.monitor({ output = \"%1\", mode = \"%2\", position = \"%3\", scale = %4")
                      .arg(output, m.value("mode").toString(), m.value("position").toString(), QString::number(m.value("scale").toDouble(), 'g', 8));
    if (m.value("transform").toInt() != 0)
        lua += QString(", transform = %1").arg(m.value("transform").toInt());
    if (!m.value("mirror").toString().isEmpty())
        lua += QString(", mirror = \"%1\"").arg(m.value("mirror").toString());
    if (m.value("vrr").toBool())
        lua += ", vrr = 1";
    return lua + " })";
}

DisplayProvider::DisplayProvider(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *parent)
    : QObject(parent), m_runner(runner), m_coordinator(coordinator) {
    m_countdown.setInterval(1000);
    connect(&m_countdown, &QTimer::timeout, this, [this] {
        --m_previewSeconds;
        emit previewChanged();
        if (m_previewSeconds > 0)
            return;
        m_countdown.stop();
        // The watchdog owns the timeout; here we only wait to hear it finish.
        waitForWatchdog(m_resultFile, "restored", 80, [this](bool restored) {
            m_previewing = false;
            emit previewChanged();
            if (m_isCheck) {
                finishCheck();
                return;
            }
            emit message(restored ? tr("Kept the previous display layout.") : tr("The display layout could not be confirmed. Check your displays."), !restored);
            endPreview(restored ? "timeout" : "unconfirmed");
        });
    });
    collectInterrupted();
}

void DisplayProvider::collectInterrupted() {
    // A preview that outlived this window: the watchdog finished it alone and left its
    // verdict behind. The plan file is gone once a watchdog is done with it.
    const QDir dir(runDirectory());
    for (const auto &info : dir.entryInfoList({"display-*.result"}, QDir::Files)) {
        if (QFileInfo::exists(info.absolutePath() + "/" + info.completeBaseName() + ".json"))
            continue;
        QByteArray result;
        FileOps::readFile(info.absoluteFilePath(), &result);
        if (result.contains("restore-failed"))
            setOutcome("warning", tr("A layout preview was interrupted"),
                       tr("This window closed during a preview, and the safety timer could not confirm the previous layout. Check your displays."));
        else if (result.contains("restored"))
            setOutcome("info", tr("A layout preview was interrupted"),
                       tr("This window closed during a preview, so the safety timer put the previous layout back. Nothing was saved."));
        QFile::remove(info.absoluteFilePath());
    }
}

QVariantMap DisplayProvider::appliedRow(const QString &output) const {
    for (const auto &monitor : m_monitors)
        if (monitor.toMap().value("output").toString() == output)
            return monitor.toMap();
    return {};
}

QVariantMap DisplayProvider::stagedRow(const QString &output) const {
    for (const auto &row : m_staged)
        if (row.toMap().value("output").toString() == output)
            return row.toMap();
    return {};
}

void DisplayProvider::setOutcome(const QString &kind, const QString &title, const QString &detail, const QString &scope) {
    m_outcome = kind.isEmpty() ? QVariantMap{} : QVariantMap{{"kind", kind}, {"title", title}, {"detail", detail}, {"scope", scope}};
}

void DisplayProvider::dismissOutcome() {
    if (m_outcome.isEmpty())
        return;
    m_outcome.clear();
    emit changed();
}

QVariantMap DisplayProvider::lastCheck() const {
    QByteArray contents;
    if (!FileOps::readFile(recoveryCheckPath(), &contents))
        return {};
    const auto record = QJsonDocument::fromJson(contents).object();
    return {{"passed", record.value("passed").toBool()}, {"at", record.value("at").toString()},
            {"reason", record.value("reason").toString()},
            // A verdict for another Hyprland says nothing about this one.
            {"current", !m_hyprlandVersion.isEmpty() && record.value("hyprland").toString() == m_hyprlandVersion}};
}

static bool sameValue(const QString &field, const QVariant &a, const QVariant &b) {
    if (field == QLatin1String("scale"))
        return std::abs(a.toDouble() - b.toDouble()) < 0.0005;
    if (field == QLatin1String("enabled"))
        return a.toBool() == b.toBool();
    if (field == QLatin1String("mode"))
        return a.toString() == b.toString();
    return a.toInt() == b.toInt();
}

void DisplayProvider::rebuildStage() {
    QVariantList rows;
    bool dirty = false;
    for (const auto &monitor : std::as_const(m_monitors)) {
        QVariantMap row = monitor.toMap();
        const QString output = row.value("output").toString();
        QVariantMap edits = m_edits.value(output);
        // An edit that now matches what the display is doing is no longer a change.
        for (auto it = edits.begin(); it != edits.end();) {
            if (sameValue(it.key(), row.value(it.key()), it.value()))
                it = edits.erase(it);
            else
                ++it;
        }
        for (auto it = edits.cbegin(); it != edits.cend(); ++it)
            row[it.key()] = it.value();
        if (edits.contains("mode")) {
            const QString mode = edits.value("mode").toString();
            const auto size = mode.section('@', 0, 0).split('x');
            row["width"] = size.value(0).toInt();
            row["height"] = size.value(1).toInt();
            row["refresh"] = mode.section('@', 1).toDouble();
        }
        if (edits.isEmpty())
            m_edits.remove(output);
        else
            m_edits.insert(output, edits);
        QStringList pending = edits.keys();
        pending.sort();
        row["pending"] = pending;
        const QSize logical = logicalSize(row);
        row["logicalWidth"] = logical.width();
        row["logicalHeight"] = logical.height();
        dirty = dirty || !pending.isEmpty();
        rows << row;
    }
    m_staged = rows;
    m_dirty = dirty;
}

void DisplayProvider::setMonitors(const QVariantList &monitors) {
    m_monitors = monitors;
    QStringList present;
    QString focused, firstEnabled;
    for (const auto &monitor : monitors) {
        const auto m = monitor.toMap();
        const QString output = m.value("output").toString();
        present << output;
        if (!m.value("enabled").toBool())
            continue;
        if (m.value("focused").toBool() && focused.isEmpty())
            focused = output;
        if (firstEnabled.isEmpty())
            firstEnabled = output;
    }
    m_focused = focused;
    QStringList dropped;
    for (auto it = m_edits.begin(); it != m_edits.end();) {
        if (present.contains(it.key())) {
            ++it;
            continue;
        }
        dropped << it.key();
        it = m_edits.erase(it);
    }
    rebuildStage();
    if (!dropped.isEmpty())
        setOutcome("warning", tr("%1 was disconnected").arg(dropped.join(", ")),
                   tr("Its pending changes were dropped. Changes to the other displays are still staged."));
    // The selection is this window's own, never the desktop's focus, and only moves
    // when the selected display is gone.
    if (!present.isEmpty() && !present.contains(m_selected)) {
        const QString previous = m_selected;
        m_selected = !focused.isEmpty() ? focused : !firstEnabled.isEmpty() ? firstEnabled : present.first();
        if (!previous.isEmpty())
            emit message(tr("%1 is no longer connected, so %2 is selected.").arg(previous, m_selected), false);
        emit selectionChanged();
    }
    emit monitorsChanged();
}

void DisplayProvider::select(const QString &output) {
    if (output == m_selected || appliedRow(output).isEmpty())
        return;
    m_selected = output;
    emit changed();
    emit selectionChanged();
}

void DisplayProvider::stageEdit(const QString &output, const QString &field, const QVariant &value) {
    static const QStringList fields{"x", "y", "mode", "scale", "transform", "enabled"};
    const QVariantMap applied = appliedRow(output);
    if (applied.isEmpty() || !fields.contains(field) || busy())
        return;
    if (!editingUnlocked()) {
        emit message(tr("Run the display recovery check before arranging displays here."), true);
        return;
    }
    const QVariantMap current = stagedRow(output);
    QVariantMap edits = m_edits.value(output);
    auto sizeOf = [](const QString &mode) {
        const auto size = mode.section('@', 0, 0).split('x');
        return QSize(size.value(0).toInt(), size.value(1).toInt());
    };
    if (field == QLatin1String("mode")) {
        const QString mode = value.toString();
        edits["mode"] = mode;
        // A scale that suited the old mode may not divide the new one evenly.
        const QSize size = sizeOf(mode);
        const double scale = cleanScale(current.value("scale").toDouble(), size.width(), size.height());
        if (!sameValue("scale", scale, current.value("scale")))
            edits["scale"] = scale;
    } else if (field == QLatin1String("scale")) {
        const QSize size = sizeOf(current.value("mode").toString());
        edits["scale"] = cleanScale(value.toDouble(), size.width(), size.height());
    } else if (field == QLatin1String("enabled")) {
        edits["enabled"] = value.toBool();
        if (value.toBool() && !applied.value("enabled").toBool()) {
            // A display that was off may have no usable mode or place: give it its first
            // listed mode, and put it to the right of everything that is on.
            const QStringList modes = applied.value("modes").toStringList();
            if (!edits.contains("mode") && !modes.isEmpty() && (current.value("width").toInt() <= 0 || !modes.contains(current.value("mode").toString())))
                edits["mode"] = modes.first();
            if (current.value("scale").toDouble() <= 0)
                edits["scale"] = 1.0;
            if (!edits.contains("x")) {
                int right = 0;
                for (const auto &row : std::as_const(m_staged)) {
                    const auto r = row.toMap();
                    if (r.value("output").toString() != output && r.value("enabled").toBool())
                        right = std::max(right, r.value("x").toInt() + r.value("logicalWidth").toInt());
                }
                edits["x"] = right;
                edits["y"] = 0;
            }
        }
    } else {
        edits[field] = value.toInt();
    }
    m_edits.insert(output, edits);
    rebuildStage();
    emit changed();
}

void DisplayProvider::stagePosition(const QString &output, int x, int y) {
    if (appliedRow(output).isEmpty() || busy() || !editingUnlocked())
        return;
    QVariantMap edits = m_edits.value(output);
    edits["x"] = x;
    edits["y"] = y;
    m_edits.insert(output, edits);
    rebuildStage();
    emit changed();
}

void DisplayProvider::discardStaged() {
    m_edits.clear();
    rebuildStage();
    emit changed();
}

void DisplayProvider::previewStaged() {
    if (!m_dirty)
        return;
    preview(m_staged);
}

QString DisplayProvider::stageProblem() const {
    return m_dirty ? validateLayout(m_staged) : QString();
}

QVariantMap DisplayProvider::selectedScale() const {
    // One answer for every control that sets the selected display's scale, so the
    // arrangement and the Screen row can never disagree about what a change will do.
    const QVariantMap row = stagedRow(m_selected);
    if (row.isEmpty())
        return {};
    const QVariantMap applied = appliedRow(m_selected);
    const bool unlocked = editingUnlocked();
    const bool pending = row.value("pending").toStringList().contains("scale");
    QString route, reason;
    if (busy()) {
        route = "busy";
        reason = m_checkState == QLatin1String("running") ? tr("Wait for the recovery check to finish.") : tr("Wait for the display preview to finish.");
    } else if (!row.value("enabled").toBool()) {
        route = "off";
        reason = tr("%1 is off.").arg(m_selected);
    } else if (m_dirty) {
        // Once a layout is being staged, a scale belongs with it.
        route = unlocked ? "stage" : "locked";
    } else if (m_selected == m_focused && !m_savedLayout) {
        route = "immediate";
    } else if (unlocked) {
        route = "stage";
    } else {
        route = "locked";
        reason = m_savedLayout ? tr("Your saved display layout sets the scale. Run the recovery check under Arrangement to change it there.")
                               : tr("Only the focused display's scale changes right away. Run the recovery check under Arrangement to change %1's scale.").arg(m_selected);
    }
    if (pending && reason.isEmpty())
        reason = tr("Not applied yet: %1 stays at %2 until the layout is previewed and kept.").arg(m_selected, QString::number(qRound(applied.value("scale").toDouble() * 100)) + "%");
    const double value = row.value("scale").toDouble();
    return {{"output", m_selected}, {"value", value}, {"applied", applied.value("scale").toDouble()}, {"pending", pending},
            {"route", route}, {"reason", reason},
            {"choices", scaleChoices(row.value("width").toInt(), row.value("height").toInt(), value, 1, 4)}};
}

QString DisplayProvider::runDirectory() const {
    return m_runner->env().runtimeDir + "/omarchy-control";
}

QString DisplayProvider::recoveryCheckPath() const {
    return m_runner->env().appStateDir + "/display-recovery-check.json";
}

bool DisplayProvider::editingUnlocked() const {
    QByteArray contents;
    if (!FileOps::readFile(recoveryCheckPath(), &contents))
        return false;
    const auto record = QJsonDocument::fromJson(contents).object();
    return record.value("passed").toBool() && !m_hyprlandVersion.isEmpty() && record.value("hyprland").toString() == m_hyprlandVersion;
}

void DisplayProvider::refresh() {
    const Env &env = m_runner->env();
    m_runner->run(env.hyprctl({"-j", "monitors", "all"}), CommandOptions{3000}, this, [this](const CommandResult &result) {
        if (!result.ok()) {
            m_status = "unavailable";
            emit changed();
            return;
        }
        HyprModule module(m_runner->env());
        m_savedLayout = module.load() && !module.monitors().isEmpty();
        setMonitors(parseMonitors(result.out));
        m_status = "ok";
        emit changed();
    });
    m_runner->run(env.hyprctl({"-j", "version"}), CommandOptions{3000}, this, [this](const CommandResult &result) {
        const QString version = QJsonDocument::fromJson(result.out).object().value("version").toString();
        if (version != m_hyprlandVersion) {
            m_hyprlandVersion = version;
            emit changed();
        }
    });
}

QString DisplayProvider::validateLayout(const QVariantList &layout) const {
    bool anyEnabled = false;
    for (const auto &value : layout) {
        const auto row = value.toMap();
        const QString output = row.value("output").toString();
        if (!isSafeOutputName(output))
            return tr("A display has a name that cannot be used safely.");
        bool known = false;
        QStringList modes;
        QString current;
        for (const auto &monitor : m_monitors) {
            const auto m = monitor.toMap();
            if (m.value("output").toString() == output) {
                known = true;
                modes = m.value("modes").toStringList();
                current = m.value("mode").toString();
            }
        }
        if (!known)
            return tr("%1 is no longer connected.").arg(output);
        if (row.value("enabled").toBool()) {
            anyEnabled = true;
            // What the display is doing right now is always acceptable, listed or not:
            // nested outputs and some custom modes are not in the list.
            if (!modes.contains(row.value("mode").toString()) && row.value("mode").toString() != current)
                return tr("%1 does not offer that mode.").arg(output);
            const double scale = row.value("scale").toDouble();
            if (scale < 0.25 || scale > 8)
                return tr("That scale is outside what %1 can use.").arg(output);
            const int transform = row.value("transform").toInt();
            if (transform < 0 || transform > 7)
                return tr("That rotation is not valid.");
        }
    }
    if (!anyEnabled)
        return tr("At least one display must stay on.");
    return {};
}

static QJsonObject monitorRule(const QVariantMap &row) {
    QJsonObject rule;
    rule["output"] = row.value("output").toString();
    if (!row.value("enabled").toBool()) {
        rule["disabled"] = true;
        return rule;
    }
    const QString mode = row.value("mode").toString();
    const auto size = mode.section('@', 0, 0).split('x');
    rule["mode"] = mode;
    rule["position"] = QString("%1x%2").arg(row.value("x").toInt()).arg(row.value("y").toInt());
    rule["scale"] = DisplayProvider::cleanScale(row.value("scale").toDouble(), size.value(0).toInt(), size.value(1).toInt());
    rule["transform"] = row.value("transform").toInt();
    // A mirrored display has to come back mirrored; dropping this would break it.
    const QString mirror = row.value("mirrorOf").toString();
    if (!mirror.isEmpty() && mirror != QLatin1String("none") && DisplayProvider::isSafeOutputName(mirror))
        rule["mirror"] = mirror;
    // Same for variable refresh rate, which a rule without it would turn off.
    if (row.value("vrr").toBool())
        rule["vrr"] = true;
    return rule;
}

void DisplayProvider::waitForWatchdog(const QString &file, const QString &expected, int attempts, std::function<void(bool)> done) {
    QByteArray contents;
    FileOps::readFile(file, &contents);
    if (contents.contains(expected.toUtf8())) {
        done(true);
        return;
    }
    if (attempts <= 0) {
        done(false);
        return;
    }
    QTimer::singleShot(100, this, [this, file, expected, attempts, done] { waitForWatchdog(file, expected, attempts - 1, done); });
}

void DisplayProvider::applyLayout(const QVariantList &layout, int index, std::function<void(bool, const QString &)> done) {
    if (index >= layout.size()) {
        done(true, {});
        return;
    }
    const QString lua = monitorLua(monitorRule(layout[index].toMap()));
    CommandOptions options;
    options.mutation = true;
    options.timeoutMs = 5000;
    m_runner->run(m_runner->env().hyprctl({"eval", lua}), options, this, [this, layout, index, done](const CommandResult &result) {
        if (!result.ok()) {
            done(false, result.describe("Hyprland"));
            return;
        }
        applyLayout(layout, index + 1, done);
    });
}

void DisplayProvider::startPreview(const QVariantList &layout, int seconds, bool isCheck) {
    if (m_previewing || m_starting) {
        emit message(tr("A display preview is already running."), true);
        return;
    }
    // Every output name goes into Lua below, whichever path brought it here.
    for (const auto &row : layout) {
        if (!isSafeOutputName(row.toMap().value("output").toString())) {
            failStart(tr("A display has a name that cannot be used safely."), isCheck);
            return;
        }
    }
    const QString dir = runDirectory();
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    m_planPath = dir + "/display-" + token + ".json";
    m_keepFile = dir + "/display-" + token + ".keep";
    m_revertFile = dir + "/display-" + token + ".revert";
    m_resultFile = dir + "/display-" + token + ".result";

    QJsonArray previous;
    for (const auto &monitor : std::as_const(m_monitors)) {
        const auto row = monitor.toMap();
        previous.append(monitorRule(row));
    }
    QJsonObject plan;
    plan["previous"] = previous;
    plan["seconds"] = seconds;
    plan["keepFile"] = m_keepFile;
    plan["revertFile"] = m_revertFile;
    plan["resultFile"] = m_resultFile;
    QString error;
    if (!FileOps::atomicWrite(m_planPath, QJsonDocument(plan).toJson(), &error)) {
        failStart(error, isCheck);
        return;
    }

    // Recovery is registered before the first mutation; if it does not start,
    // nothing below ever runs.
    if (!m_runner->launchDetached({m_runner->env().selfBinary, "display-watchdog", m_planPath}, &error)) {
        failStart(tr("The display safety timer could not start, so nothing was changed. %1").arg(error), isCheck);
        return;
    }
    m_pendingLayout = layout;
    m_isCheck = isCheck;
    m_starting = true;
    m_previewOriginal = layoutSignature();
    setOutcome({}, {}, {});
    emit previewChanged();
    emit changed();
    HyprModule module(m_runner->env());
    m_startFingerprints.clear();
    m_startFingerprints.insert(module.path(), FileOps::fingerprint(module.path()));
    m_startFingerprints.insert(module.hyprlandPath(), FileOps::fingerprint(module.hyprlandPath()));
    waitForWatchdog(m_resultFile, "armed", 30, [this, layout, seconds, isCheck](bool armed) {
        if (!armed) {
            m_starting = false;
            emit previewChanged();
            failStart(tr("The display safety timer did not respond, so nothing was changed."), isCheck);
            return;
        }
        // Keep and Restore only appear once every row has been applied, so neither can
        // race a modeset that is still going on.
        applyLayout(layout, 0, [this, seconds, isCheck](bool ok, const QString &detail) {
            m_starting = false;
            if (!ok) {
                const QString problem = tr("Hyprland refused the layout: %1").arg(detail);
                // The watchdog has to be told, and it may have rows to put back.
                QFile revertFile(m_revertFile);
                if (revertFile.open(QIODevice::WriteOnly))
                    revertFile.close();
                waitForWatchdog(m_resultFile, "restored", 50, [this, problem, isCheck](bool restored) {
                    QFile::remove(m_resultFile);
                    emit previewChanged();
                    failStart(problem + " " + (restored ? tr("The previous layout was put back.") : tr("The previous layout could not be confirmed. Check your displays.")), isCheck);
                    refresh();
                });
                return;
            }
            m_previewing = true;
            m_previewSeconds = seconds;
            m_previewTotal = seconds;
            // What was staged is now on screen; Keep or the timer decides what stays.
            if (!isCheck) {
                m_edits.clear();
                rebuildStage();
            }
            emit previewChanged();
            m_countdown.start();
            if (isCheck) {
                // The nudge has to be visible in Hyprland's answer. If it is not, the
                // compositor ignored the change, and a restore would prove nothing.
                // The answer does not change in the same turn as the eval: Hyprland
                // applies the rule on a later frame, and a read that returns with the
                // eval still shows the old layout. This read is the one that waits;
                // an immediate refresh would only ever see the old layout.
                awaitCheckLayout(CheckObserveAttempts, true);
            } else {
                refresh();
            }
        });
    });
}

void DisplayProvider::failStart(const QString &text, bool isCheck) {
    if (isCheck) {
        m_checkState = "failed";
        setOutcome("error", tr("The recovery check could not run"), text, "check");
        emit changed();
        emit previewChanged();
        emit checkFinished(false, text);
    } else {
        // Staged edits are left as they were, so trying again needs nothing redone.
        setOutcome("error", tr("The layout was not previewed"), text);
        emit changed();
        emit message(text, true);
    }
}

QStringList DisplayProvider::signatureOf(const QVariantList &monitors) {
    QStringList signature;
    for (const auto &monitor : monitors) {
        const auto m = monitor.toMap();
        signature << QString("%1 %2 %3x%4 %5 %6 %7").arg(m.value("output").toString(), m.value("mode").toString()).arg(m.value("x").toInt()).arg(m.value("y").toInt()).arg(m.value("scale").toDouble()).arg(m.value("transform").toInt()).arg(m.value("enabled").toBool() ? "on" : "off");
    }
    return signature;
}

void DisplayProvider::verifyLayout(const QStringList &expected, std::function<void(bool same, bool known)> done) {
    m_runner->run(m_runner->env().hyprctl({"-j", "monitors", "all"}), CommandOptions{3000}, this, [this, expected, done](const CommandResult &query) {
        if (!query.ok()) {
            done(false, false);
            return;
        }
        setMonitors(parseMonitors(query.out));
        emit changed();
        // A display unplugged or plugged in meanwhile cannot be held against the
        // restore; every display present before and after must match.
        QHash<QString, QString> before;
        for (const auto &line : expected)
            before.insert(line.section(' ', 0, 0), line);
        bool same = true;
        for (const auto &line : layoutSignature()) {
            const QString output = line.section(' ', 0, 0);
            if (before.contains(output) && before.value(output) != line)
                same = false;
        }
        done(same, true);
    });
}

void DisplayProvider::endPreview(const QString &how) {
    QFile::remove(m_resultFile);
    emit changed();
    verifyLayout(m_previewOriginal, [this, how](bool same, bool known) {
        const QString verified = !known ? tr("Hyprland could not be asked for the current layout. Check your displays.")
                               : same ? tr("Hyprland reports the layout from before the preview.")
                                      : tr("Hyprland reports a different layout than before the preview. Check your displays.");
        if (how == QLatin1String("timeout"))
            setOutcome(same ? "info" : "warning", tr("The preview ended without Keep, so the previous layout is back"), verified);
        else if (how == QLatin1String("unconfirmed"))
            setOutcome("warning", tr("The previous layout could not be confirmed"), verified);
        else if (!same || !known)
            setOutcome("warning", tr("The restore may be incomplete"), verified);
        emit changed();
        refresh();
    });
}

void DisplayProvider::preview(const QVariantList &layout) {
    if (!editingUnlocked()) {
        emit message(tr("Run the display recovery check before arranging displays here."), true);
        return;
    }
    const QString problem = validateLayout(layout);
    if (!problem.isEmpty()) {
        emit message(problem, true);
        return;
    }
    startPreview(layout, PreviewSeconds, false);
}

void DisplayProvider::keep() {
    if (!m_previewing)
        return;
    m_countdown.stop();
    QFile keepFile(m_keepFile);
    if (keepFile.open(QIODevice::WriteOnly))
        keepFile.close();
    const QVariantList layout = m_pendingLayout;
    waitForWatchdog(m_resultFile, "kept", 50, [this, layout](bool kept) {
        m_previewing = false;
        emit previewChanged();
        if (!kept) {
            // The watchdog may already have restored; report what is true now.
            emit message(tr("The preview had already ended. Check the current layout."), true);
            endPreview("unconfirmed");
            return;
        }
        QFile::remove(m_resultFile);
        // Persist only now that the layout is confirmed to work.
        HyprModule module(m_runner->env());
        ChangeCoordinator::Request request;
        request.settingId = "displays.layout";
        request.label = tr("Display layout");
        request.after = "custom";
        request.files << module.path() << module.hyprlandPath();
        // A hand edit made while the preview ran is not ours to overwrite.
        request.expectedFingerprints = m_startFingerprints;
        request.apply = [this, layout](ChangeCoordinator::Done done) {
            HyprModule target(m_runner->env());
            QString error;
            if (!target.load(&error)) {
                done(false, error);
                return;
            }
            QJsonArray rules;
            for (const auto &row : layout)
                rules.append(monitorRule(row.toMap()));
            target.setMonitors(rules);
            done(target.save(&error) && target.installImport(&error), error);
        };
        m_coordinator->submit(std::move(request), [this](const Transaction &tx) {
            const bool saved = tx.phase == QLatin1String("committed");
            emit message(saved ? tr("Display layout saved.") : tr("The layout is active but was not saved: %1").arg(tx.message), !saved);
            if (!saved)
                setOutcome("warning", tr("The layout is active but was not saved"), tx.message);
            emit changed();
            refresh();
        });
    });
}

void DisplayProvider::revert() {
    if (!m_previewing)
        return;
    m_countdown.stop();
    QFile revertFile(m_revertFile);
    if (revertFile.open(QIODevice::WriteOnly))
        revertFile.close();
    waitForWatchdog(m_resultFile, "restored", 50, [this](bool restored) {
        m_previewing = false;
        emit previewChanged();
        emit message(restored ? tr("Restored the previous display layout.") : tr("The previous layout could not be confirmed. Check your displays."), !restored);
        endPreview(restored ? "revert" : "unconfirmed");
    });
}

void DisplayProvider::runRecoveryCheck() {
    if (m_status != QLatin1String("ok") || m_monitors.isEmpty()) {
        emit checkFinished(false, tr("No display information is available."));
        return;
    }
    if (busy()) {
        emit checkFinished(false, tr("A display preview is already running."));
        return;
    }
    m_checkState = "running";
    m_checkMoved = false;
    m_checkOriginal = layoutSignature();
    emit changed();
    emit previewChanged();
    // A real change, or the check would prove nothing: re-applying the same layout
    // looks identical whether the restore worked or not. So the first display moves
    // a few pixels, Hyprland has to report the move, and then the watchdog has to put
    // it back unprompted. A display is only nudged in space, so nothing resizes.
    QVariantList nudged;
    bool moved = false;
    for (const auto &monitor : m_monitors) {
        auto row = monitor.toMap();
        if (!moved && row.value("enabled").toBool()) {
            row["x"] = row.value("x").toInt() + CheckNudgePixels;
            moved = true;
        }
        nudged << row;
    }
    startPreview(nudged, CheckSeconds, true);
}

void DisplayProvider::finishCheck() {
    QByteArray result;
    FileOps::readFile(m_resultFile, &result);
    const bool restored = result.contains("restored") && !result.contains("restore-failed");
    QFile::remove(m_resultFile);
    awaitCheckLayout(CheckObserveAttempts, false, restored);
}

void DisplayProvider::awaitCheckLayout(int attempts, bool untilMoved, bool restored) {
    // A nudge read must not land after the check has already moved on to the restore.
    if (untilMoved && (!m_isCheck || !m_previewing))
        return;
    m_runner->run(m_runner->env().hyprctl({"-j", "monitors", "all"}), CommandOptions{3000}, this, [this, attempts, untilMoved, restored](const CommandResult &query) {
        if (untilMoved && (!m_isCheck || !m_previewing))
            return;
        if (query.ok()) {
            const auto monitors = parseMonitors(query.out);
            if (signatureOf(monitors) != layoutSignature())
                setMonitors(monitors);
        }
        const bool answered = query.ok() && (untilMoved ? layoutSignature() != m_checkOriginal : layoutSignature() == m_checkOriginal);
        if (!answered && attempts > 1) {
            QTimer::singleShot(CheckObserveIntervalMs, this, [this, attempts, untilMoved, restored] { awaitCheckLayout(attempts - 1, untilMoved, restored); });
            return;
        }
        if (untilMoved) {
            if (answered)
                m_checkMoved = true;
            emit changed();
            return;
        }
        const bool same = answered;
        const bool passed = restored && same && m_checkMoved;
        // Say which step failed, then what the layout is now, read back from Hyprland.
        QString reason;
        if (!query.ok())
            reason = tr("Hyprland could not be asked for the layout afterwards.");
        else if (!m_checkMoved)
            reason = tr("Hyprland did not report the test move, so a restore would prove nothing.");
        else if (!restored)
            reason = tr("The safety timer did not report putting the layout back.");
        else if (!same)
            reason = tr("The layout after the check is not the one from before it.");
        const QString verified = !query.ok() ? tr("The current layout is unknown. Check your displays.")
                               : same ? tr("Hyprland reports the original layout, so nothing was left changed.")
                                      : tr("Hyprland reports a different layout than before the check. Check your displays.");
        QJsonObject record;
        record["passed"] = passed;
        record["hyprland"] = m_hyprlandVersion;
        record["at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        record["monitors"] = m_monitors.size();
        if (!passed)
            record["reason"] = reason + " " + verified;
        FileOps::atomicWrite(recoveryCheckPath(), QJsonDocument(record).toJson(), nullptr, 0600);
        m_checkState = passed ? "passed" : "failed";
        if (passed)
            setOutcome({}, {}, {});
        else
            setOutcome("error", tr("The recovery check failed"), reason + " " + verified, "check");
        emit changed();
        emit previewChanged();
        emit checkFinished(passed, passed ? tr("The safety timer restored the layout correctly. Display editing is unlocked.")
                                          : tr("The recovery check failed, so display editing stays locked. %1").arg(reason));
    });
}

int DisplayProvider::runWatchdog(const Env &env, const QString &planPath) {
    QByteArray contents;
    if (!FileOps::readFile(planPath, &contents))
        return 1;
    const auto plan = QJsonDocument::fromJson(contents).object();
    const QString keepFile = plan.value("keepFile").toString();
    const QString revertFile = plan.value("revertFile").toString();
    const QString resultFile = plan.value("resultFile").toString();
    const int seconds = plan.value("seconds").toInt(PreviewSeconds);

    auto report = [&](const QByteArray &text) { FileOps::atomicWrite(resultFile, text + "\n"); };
    report("armed");

    // Plain blocking calls: this process has one job, no window to keep alive,
    // and must not depend on anything else being healthy.
    auto hyprctl = [&](const QStringList &arguments, QByteArray *out = nullptr) {
        const QStringList argv = env.hyprctl(arguments);
        QProcess process;
        process.setProcessEnvironment(env.process);
        process.start(env.findExecutable(argv.first()), argv.mid(1));
        if (!process.waitForStarted(2000) || !process.waitForFinished(5000)) {
            process.kill();
            process.waitForFinished(500);
            return false;
        }
        if (out)
            *out = process.readAllStandardOutput();
        return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    };

    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + seconds * 1000LL;
    bool kept = false;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        if (QFile::exists(keepFile)) {
            kept = true;
            break;
        }
        if (QFile::exists(revertFile))
            break;
        QThread::msleep(100);
    }
    if (kept) {
        report("kept");
    } else {
        // Hot-unplug: only outputs still present can be restored, and at least
        // one must stay on or the session would have no screen at all.
        QByteArray current;
        QStringList present;
        if (hyprctl({"-j", "monitors", "all"}, &current))
            for (const auto &value : QJsonDocument::fromJson(current).array())
                present << value.toObject().value("name").toString();

        bool anyRestoredOn = false;
        bool allOk = true;
        QJsonArray previous = plan.value("previous").toArray();
        // Enabled monitors first, so there is never a moment with no output.
        for (int pass = 0; pass < 2; ++pass) {
            for (const auto &value : previous) {
                const auto rule = value.toObject();
                const bool disabled = rule.value("disabled").toBool();
                if (disabled != (pass == 1))
                    continue;
                const QString output = rule.value("output").toString();
                if (!DisplayProvider::isSafeOutputName(output) || (!present.isEmpty() && !present.contains(output)))
                    continue;
                if (disabled && !anyRestoredOn)
                    continue;
                const bool ok = hyprctl({"eval", monitorLua(rule)});
                allOk = allOk && ok;
                if (!disabled && ok)
                    anyRestoredOn = true;
            }
        }
        bool lastResort = false;
        if (!anyRestoredOn) {
            for (const auto &name : present) {
                if (DisplayProvider::isSafeOutputName(name) && hyprctl({"eval", QString("hl.monitor({ output = \"%1\", mode = \"preferred\", position = \"auto\", scale = 1 })").arg(name)})) {
                    lastResort = true;
                    break;
                }
            }
        }
        report(allOk && (anyRestoredOn || lastResort) ? "restored" : "restore-failed");
    }
    QFile::remove(planPath);
    QFile::remove(keepFile);
    QFile::remove(revertFile);
    return 0;
}
