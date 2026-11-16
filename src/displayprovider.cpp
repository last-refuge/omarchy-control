#include "displayprovider.h"
#include "displaypreview.h"
#include "networkpreview.h"
#include <QElapsedTimer>
#include <unistd.h>
#include <sys/stat.h>

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
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
// Hyprland applies a rule on a later frame than the one that answers `eval`. Reads
// 50 ms apart (a few frames at 60 Hz), twenty of them: about a second of waiting,
// plus however long each hyprctl takes to answer.
static constexpr int ObserveAttempts = 20;
static constexpr int ObserveIntervalMs = 50;

bool DisplayProvider::isSafeOutputName(const QString &name) {
    // The output name is written into Lua that Hyprland evaluates, and EDID
    // data can influence it, so only plain connector names may pass.
    static const QRegularExpression safe("^[A-Za-z0-9._-]+$");
    const auto match = safe.match(name);
    return !name.isEmpty() && name.size() <= 128 && match.hasMatch() && match.capturedLength() == name.size();
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
        row["id"] = m.value("id").toVariant();
        row["serial"] = m.value("serial").toString();
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

bool DisplayProvider::decodeMonitors(const QByteArray &bytes, QVariantList *rows, QString *error) {
    auto fail = [&](const QString &text) { if (error) *error = text; rows->clear(); return false; };
    if (bytes.size() > 1024 * 1024 || QString::fromUtf8(bytes).toUtf8() != bytes) return fail("Display data is oversized or invalid UTF-8.");
    QJsonParseError parsed; const auto doc = QJsonDocument::fromJson(bytes, &parsed);
    if (parsed.error != QJsonParseError::NoError || !doc.isArray() || doc.array().size() > 64) return fail("Hyprland did not return a bounded output array.");
    QSet<QString> names;
    auto integer = [](const QJsonValue &v, int low, int high) { const double n = v.toDouble(qQNaN()); return v.isDouble() && std::isfinite(n) && std::floor(n) == n && n >= low && n <= high; };
    for (const auto &value : doc.array()) {
        if (!value.isObject()) return fail("Hyprland returned a non-object output.");
        const auto m = value.toObject(); const auto name = m.value("name").toString();
        if (!isSafeOutputName(name) || name.size() > 128 || names.contains(name)) return fail("Hyprland returned an invalid or duplicate output identity.");
        names.insert(name);
        const bool enabled = !m.value("disabled").toBool();
        if ((!m.value("disabled").isUndefined() && !m.value("disabled").isBool())
            || !integer(m.value("width"), enabled ? 1 : 0, 32768) || !integer(m.value("height"), enabled ? 1 : 0, 32768)
            || !integer(m.value("x"), -131072, 131072) || !integer(m.value("y"), -131072, 131072)
            || !integer(m.value("transform"), 0, 7) || !m.value("scale").isDouble() || !std::isfinite(m.value("scale").toDouble())
            || (enabled && (m.value("scale").toDouble() < .25 || m.value("scale").toDouble() > 8))
            || !m.value("refreshRate").isDouble() || !std::isfinite(m.value("refreshRate").toDouble()) || m.value("refreshRate").toDouble() < 0 || m.value("refreshRate").toDouble() > 1000)
            return fail("Hyprland returned invalid or incomplete output geometry.");
        if (!m.value("availableModes").isUndefined()) {
            if (!m.value("availableModes").isArray() || m.value("availableModes").toArray().size() > 512) return fail("Hyprland returned an invalid mode list.");
            for (const auto &mode : m.value("availableModes").toArray())
                if (!mode.isString() || mode.toString().size() > 64 || !QRegularExpression("^[0-9]+x[0-9]+@[0-9]+(?:\\.[0-9]+)?(?:Hz)?$").match(mode.toString()).hasMatch()) return fail("Hyprland returned an invalid mode.");
        }
    }
    *rows = parseMonitors(bytes); return true;
}

QVariantMap DisplayProvider::snapshot() const {
    return {{"complete", m_status == "ok" && m_versionStatus == "ok"}, {"monitorsStatus", m_status}, {"monitorsError", m_monitorError},
        {"versionStatus", m_versionStatus}, {"versionError", m_versionError}, {"monitors", m_status == "ok" ? QVariant(m_monitors) : QVariant()},
        {"hyprlandVersion", m_versionStatus == "ok" ? QVariant(m_hyprlandVersion) : QVariant()}, {"recoveryCheck", lastCheck()},
        {"editingUnlocked", editingUnlocked()}, {"savedLayout", m_savedLayout}, {"generation", m_refreshGeneration},
        {"verificationScope", "Hyprland-reported-output-state"}, {"physicalAppearanceVerified", false}};
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

DisplayProvider::DisplayProvider(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *parent, bool collectHistory)
    : QObject(parent), m_runner(runner), m_coordinator(coordinator) {
    m_countdown.setInterval(1000);
    connect(&m_countdown, &QTimer::timeout, this, [this] {
        if (!m_previewToken.isEmpty()) {
            const auto receipt = previewReceipt();
            if (receipt.isEmpty()) { m_countdown.stop(); m_starting = false; m_previewing = true; emit previewChanged(); return; }
            m_receipt = receipt;
            if (DisplayPreview::terminal(receipt)) {
                m_countdown.stop(); m_starting = false; m_previewing = false; emit previewChanged();
                if (receipt.value("phase") == "reverted") endPreview("timeout"); else refresh();
                return;
            }
            m_starting = receipt.value("phase") == "arming" || receipt.value("phase") == "applying";
            m_previewing = !m_starting;
            m_previewSeconds = std::max(0, int((receipt.value("deadline").toDouble() - QDateTime::currentMSecsSinceEpoch() + 999) / 1000));
            if (receipt.value("phase") != "preview") { if (receipt.value("phase") == "restore_failed") m_countdown.stop(); emit previewChanged(); return; }
        } else --m_previewSeconds;
        emit previewChanged();
        if (m_previewSeconds > 0)
            return;
        m_countdown.stop();
        // The watchdog owns the timeout; here we only wait to hear it finish.
        waitForWatchdog(m_resultFile, "restored", m_previewToken.isEmpty() ? 80 : 400, [this](bool restored) {
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
    if (collectHistory) collectInterrupted();
}

void DisplayProvider::collectInterrupted() {
    // A preview that outlived this window: the watchdog finished it alone and left its
    // verdict behind. The plan file is gone once a watchdog is done with it.
    const QDir dir(runDirectory());
    for (const auto &info : dir.entryInfoList({"display-*.result"}, QDir::Files)) {
        if (info.fileName().startsWith("display-dp-")) continue; // Durable receipts retain their results until pruning.
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

bool DisplayProvider::previewCanKeep() const {
    const auto receipt = previewReceipt();
    return m_previewing && receipt.value("phase") == "preview" && receipt.value("deadline").toDouble() > QDateTime::currentMSecsSinceEpoch();
}

QJsonObject DisplayProvider::previewReceipt() const {
    if (m_previewToken.isEmpty()) return {};
    return DisplayPreview::load(m_runner->env(), m_previewToken);
}

bool DisplayProvider::adoptPreview(const QString &token, QString *error) {
    const auto receipt = DisplayPreview::load(m_runner->env(), token, error);
    if (receipt.isEmpty()) return false;
    if (!DisplayPreview::sameSession(m_runner->env(), receipt)) { if (error) *error = "The preview belongs to another Hyprland session."; return false; }
    if (DisplayPreview::terminal(receipt)) { if (error) *error = "The display preview has already ended."; return false; }
    m_previewToken = token; m_receipt = receipt;
    const QString base = DisplayPreview::runtimeBase(m_runner->env(), token);
    m_planPath = base + ".json"; m_keepFile = base + ".keep"; m_revertFile = base + ".revert"; m_resultFile = base + ".result";
    m_pendingLayout = receipt.value("layout").toArray().toVariantList();
    m_previewOriginal = signatureOf(receipt.value("original").toArray().toVariantList());
    m_startFingerprints.clear(); const auto fingerprints = receipt.value("fingerprints").toObject();
    for (auto it = fingerprints.begin(); it != fingerprints.end(); ++it) m_startFingerprints.insert(it.key(), it.value().toString());
    m_isCheck = false; m_starting = receipt.value("phase") == "arming" || receipt.value("phase") == "applying"; m_previewing = !m_starting;
    m_previewSeconds = std::max(0, int((receipt.value("deadline").toDouble() - QDateTime::currentMSecsSinceEpoch() + 999) / 1000));
    m_previewTotal = PreviewSeconds;
    if (receipt.value("phase") != "restore_failed") m_countdown.start();
    emit previewChanged(); return true;
}

QString DisplayProvider::recoveryCheckPath() const {
    return m_runner->env().appStateDir + "/display-recovery-check.json";
}

bool DisplayProvider::editingUnlocked() const {
    if (m_status != QLatin1String("ok") || m_versionStatus != QLatin1String("ok")) return false;
    QByteArray contents;
    if (!FileOps::readFile(recoveryCheckPath(), &contents))
        return false;
    const auto record = QJsonDocument::fromJson(contents).object();
    return record.value("passed").toBool() && !m_hyprlandVersion.isEmpty() && record.value("hyprland").toString() == m_hyprlandVersion;
}

void DisplayProvider::refresh() {
    const Env &env = m_runner->env();
    const int generation = ++m_refreshGeneration;
    m_refreshPending = 2;
    m_status = "loading"; m_versionStatus = "loading"; m_monitorError.clear(); m_versionError.clear();
    auto complete = [this, generation] {
        if (generation != m_refreshGeneration || --m_refreshPending != 0) return;
        if (!m_previewLock && m_status == "ok" && m_versionStatus == "ok") {
            const auto receipt = DisplayPreview::current(m_runner->env());
            if (!receipt.isEmpty() && !DisplayPreview::terminal(receipt)) adoptPreview(receipt.value("token").toString());
            else if (!m_previewToken.isEmpty()) { m_starting = false; m_previewing = false; m_countdown.stop(); emit previewChanged(); }
        }
        emit refreshed();
    };
    CommandOptions options{3000}; options.maxOutputBytes = 1024 * 1024;
    m_runner->run(env.hyprctl({"-j", "monitors", "all"}), options, this, [this, generation, complete](const CommandResult &result) {
        if (generation != m_refreshGeneration) return;
        QVariantList rows;
        if (!result.ok() || result.truncated || !decodeMonitors(result.out, &rows, &m_monitorError)) {
            m_status = "unavailable";
            if (m_monitorError.isEmpty()) m_monitorError = "The output read failed or exceeded its limit.";
            emit changed();
            complete();
            return;
        }
        HyprModule module(m_runner->env());
        m_savedLayout = module.load() && !module.monitors().isEmpty();
        setMonitors(rows);
        m_status = "ok";
        emit changed();
        complete();
    });
    m_runner->run(env.hyprctl({"-j", "version"}), options, this, [this, generation, complete](const CommandResult &result) {
        if (generation != m_refreshGeneration) return;
        QJsonParseError error; const auto doc = QJsonDocument::fromJson(result.out, &error);
        const QString version = doc.object().value("version").toString();
        m_versionStatus = result.ok() && !result.truncated && error.error == QJsonParseError::NoError && !version.isEmpty() && version.size() < 256 ? "ok" : "unavailable";
        if (m_versionStatus != "ok") m_versionError = "The Hyprland version could not be read.";
        const QString trusted = m_versionStatus == "ok" ? version : QString();
        if (trusted != m_hyprlandVersion) {
            m_hyprlandVersion = trusted;
            emit changed();
        }
        complete();
    });
}

QString DisplayProvider::validateLayout(const QVariantList &layout) const {
    if (layout.isEmpty() || layout.size() != m_monitors.size() || layout.size() > 64) return tr("Supply one row for every reported output.");
    QSet<QString> seen;
    bool anyEnabled = false;
    auto integer = [](const QVariant &v, int low, int high) { const auto j = QJsonValue::fromVariant(v); const double n = j.toDouble(qQNaN()); return j.isDouble() && std::isfinite(n) && std::floor(n) == n && n >= low && n <= high; };
    for (const auto &value : layout) {
        const auto row = value.toMap();
        const QString output = row.value("output").toString();
        if (!isSafeOutputName(output))
            return tr("A display has a name that cannot be used safely.");
        if (seen.contains(output)) return tr("Every output must appear exactly once.");
        seen.insert(output);
        if (row.value("enabled").metaType().id() != QMetaType::Bool || !integer(row.value("x"), -131072, 131072) || !integer(row.value("y"), -131072, 131072)) return tr("Output enablement and coordinates must be typed, bounded values.");
        const auto mirror = row.value("mirrorOf");
        if (mirror.isValid() && mirror.metaType().id() != QMetaType::QString) return tr("A mirror target must be an output name.");
        if (mirror.isValid() && !mirror.toString().isEmpty() && mirror.toString() != "none") {
            bool present = false;
            for (const auto &v : layout) if (v.toMap().value("output").toString() == mirror.toString() && v.toMap().value("enabled").toBool()) present = true;
            if (!isSafeOutputName(mirror.toString()) || mirror.toString() == output || !present) return tr("A mirror must target another enabled output.");
        }
        if (row.contains("vrr") && row.value("vrr").metaType().id() != QMetaType::Bool) return tr("Variable refresh enablement must be true or false.");
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
            if (!QJsonValue::fromVariant(row.value("scale")).isDouble() || !std::isfinite(scale) || scale < 0.25 || scale > 8)
                return tr("That scale is outside what %1 can use.").arg(output);
            const int transform = row.value("transform").toInt();
            if (!integer(row.value("transform"), 0, 7) || transform < 0 || transform > 7)
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
    if (contents.trimmed() == expected.toUtf8()) { done(true); return; }
    if (QList<QByteArray>{"restored", "kept", "restore-failed", "keep-unconfirmed", "not-started"}.contains(contents.trimmed())) { done(false); return; }
    if (attempts <= 0) {
        done(false);
        return;
    }
    QTimer::singleShot(100, this, [this, file, expected, attempts, done] { waitForWatchdog(file, expected, attempts - 1, done); });
}

void DisplayProvider::applyLayout(const QVariantList &layout, int index, std::function<void(bool, const QString &)> done) {
    if (index >= layout.size()) {
        if (m_isCheck || m_previewToken.isEmpty()) { done(true, {}); return; }
        const auto expected = signatureOf(layout);
        readMonitorsUntil(ObserveAttempts, {}, [this, expected](const QVariantList &rows) { return signatureOf(rows) == expected || m_receipt.value("deadline").toDouble() <= QDateTime::currentMSecsSinceEpoch(); },
            [expected, done](bool ok, const QVariantList &rows, bool) { done(ok && signatureOf(rows) == expected, tr("Hyprland did not confirm the complete preview layout.")); });
        return;
    }
    if (!m_isCheck && !m_previewToken.isEmpty() && (QFile::exists(m_revertFile) || m_receipt.value("deadline").toDouble() <= QDateTime::currentMSecsSinceEpoch())) { done(false, tr("The preview expired or restoration was requested.")); return; }
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
    m_previewToken.clear(); m_receipt = {}; m_mutationStarted = false;
    QString guard = DisplayPreview::pendingReason(m_runner->env());
    if (guard.isEmpty()) guard = NetworkPreview::pendingReason(m_runner->env());
    if (!guard.isEmpty() || m_coordinator->busy() || !m_coordinator->leaseRun().isEmpty() || m_coordinator->leaseHeldElsewhere()) { failStart(guard.isEmpty() ? tr("Another change or restore is running.") : guard, isCheck); return; }
    QDir().mkpath(m_runner->env().appStateDir);
    m_previewLock = std::make_unique<QLockFile>(m_runner->env().appStateDir + "/change.lock");
    if (!m_previewLock->tryLock(0)) { failStart(tr("Another Control process is changing settings."), isCheck); return; }
    guard = DisplayPreview::pendingReason(m_runner->env());
    if (guard.isEmpty()) guard = NetworkPreview::pendingReason(m_runner->env());
    if (!guard.isEmpty() || m_coordinator->leaseHeldElsewhere()) { failStart(guard.isEmpty() ? tr("A restore is running.") : guard, isCheck); return; }
    // Every output name goes into Lua below, whichever path brought it here.
    for (const auto &row : layout) {
        if (!isSafeOutputName(row.toMap().value("output").toString())) {
            failStart(tr("A display has a name that cannot be used safely."), isCheck);
            return;
        }
    }
    if (isCheck) { m_previewToken.clear(); m_receipt = {}; }
    const QString dir = runDirectory();
    QString prepareError;
    if (!DisplayPreview::prepare(m_runner->env(), &prepareError) || !m_runner->env().hasHyprlandSignature()) { failStart(prepareError.isEmpty() ? tr("A Hyprland session identity is required.") : prepareError, isCheck); return; }
    const QString token = "dp-" + QUuid::createUuid().toString(QUuid::Id128);
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
    HyprModule beforeModule(m_runner->env());
    m_startFingerprints = {{beforeModule.path(), FileOps::fingerprint(beforeModule.path())}, {beforeModule.hyprlandPath(), FileOps::fingerprint(beforeModule.hyprlandPath())}};
    if (!isCheck) {
        m_previewToken = token;
        QJsonObject fingerprints; for (auto it = m_startFingerprints.begin(); it != m_startFingerprints.end(); ++it) fingerprints[it.key()] = it.value();
        m_receipt = {{"schema", 1}, {"token", token}, {"phase", "arming"}, {"session", m_runner->env().process.value("HYPRLAND_INSTANCE_SIGNATURE")},
            {"hyprland", m_hyprlandVersion}, {"deadline", double(QDateTime::currentMSecsSinceEpoch() + seconds * 1000LL)},
            {"original", QJsonArray::fromVariantList(m_monitors)}, {"layout", QJsonArray::fromVariantList(layout)}, {"fingerprints", fingerprints},
            {"verificationScope", "reported-output-geometry-and-owned-configuration"}, {"physicalAppearanceVerified", false}};
        plan["token"] = token;
    }
    QString error;
    if ((!isCheck && !DisplayPreview::save(m_runner->env(), m_receipt, &error)) || !FileOps::atomicWrite(m_planPath, QJsonDocument(plan).toJson(), &error)) {
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
    waitForWatchdog(m_resultFile, "armed", 30, [this, layout, seconds, isCheck](bool armed) {
        if (!armed) {
            m_starting = false;
            emit previewChanged();
            failStart(tr("The display safety timer did not respond, so nothing was changed."), isCheck);
            return;
        }
        if (!isCheck) { m_receipt["phase"] = "applying"; QString error; if (!DisplayPreview::save(m_runner->env(), m_receipt, &error)) { failStart(error, isCheck); return; } }
        // Keep and Restore only appear once every row has been applied, so neither can
        // race a modeset that is still going on.
        m_mutationStarted = true;
        applyLayout(layout, 0, [this, layout, seconds, isCheck](bool ok, const QString &detail) {
            m_starting = false;
            if (!isCheck && m_receipt.value("deadline").toDouble() <= QDateTime::currentMSecsSinceEpoch()) ok = false;
            if (ok && !isCheck) { m_receipt["phase"] = "preview"; QString error; ok = DisplayPreview::save(m_runner->env(), m_receipt, &error); }
            m_previewLock.reset();
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
                    // The restore lands a frame after the watchdog reports it.
                    readMonitorsUntil(
                        ObserveAttempts, {}, [this](const QVariantList &monitors) { return signatureOf(monitors) == m_previewOriginal; },
                        [this](bool, const QVariantList &, bool) { refresh(); });
                });
                return;
            }
            m_previewing = true;
            m_previewSeconds = isCheck ? seconds : std::max(0, int((m_receipt.value("deadline").toDouble() - QDateTime::currentMSecsSinceEpoch() + 999) / 1000));
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
                // Only the nudged display at its nudged position counts, so a display
                // plugged in meanwhile is not mistaken for the move. A read that lands
                // after the check has moved on to the restore is dropped.
                const QString output = m_checkOutput;
                const int x = m_checkX;
                readMonitorsUntil(
                    ObserveAttempts, [this] { return m_isCheck && m_previewing; },
                    [output, x](const QVariantList &monitors) {
                        for (const auto &monitor : monitors) {
                            const auto m = monitor.toMap();
                            if (m.value("output").toString() == output)
                                return m.value("x").toInt() == x;
                        }
                        return false;
                    },
                    [this](bool ok, const QVariantList &monitors, bool moved) {
                        if (ok)
                            setMonitors(monitors);
                        m_checkMoved = moved;
                        emit changed();
                    });
            } else if (signatureOf(layout) == m_previewOriginal) {
                refresh();
            } else {
                // The staged edits are gone, so the page draws what Hyprland reports.
                // Read before the layout lands and it would draw the old one.
                readMonitorsUntil(
                    ObserveAttempts, {}, [this](const QVariantList &monitors) { return signatureOf(monitors) != m_previewOriginal; },
                    [this](bool, const QVariantList &, bool) { refresh(); });
            }
        });
    });
}

void DisplayProvider::failStart(const QString &text, bool isCheck) {
    if (!m_mutationStarted && !m_previewToken.isEmpty() && !m_receipt.isEmpty()) {
        m_receipt["phase"] = "not_started"; m_receipt["error"] = text;
        DisplayPreview::save(m_runner->env(), m_receipt);
    }
    m_previewLock.reset(); m_starting = false;
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
        if (!m.value("enabled").toBool()) { signature << m.value("output").toString() + " off"; continue; }
        signature << QString("%1 %2 %3x%4 %5 %6 %7").arg(m.value("output").toString(), m.value("mode").toString()).arg(m.value("x").toInt()).arg(m.value("y").toInt()).arg(m.value("scale").toDouble()).arg(m.value("transform").toInt()).arg(m.value("enabled").toBool() ? "on" : "off");
    }
    signature.sort();
    return signature;
}

bool DisplayProvider::matchesWherePresent(const QStringList &expected, const QStringList &actual) {
    // A display unplugged or plugged in meanwhile cannot be held against the
    // restore; every display present before and after must match.
    QHash<QString, QString> before;
    for (const auto &line : expected)
        before.insert(line.section(' ', 0, 0), line);
    for (const auto &line : actual) {
        const QString output = line.section(' ', 0, 0);
        if (before.contains(output) && before.value(output) != line)
            return false;
    }
    return true;
}

void DisplayProvider::readMonitorsUntil(int attempts, std::function<bool()> wanted, std::function<bool(const QVariantList &)> settled,
                                        std::function<void(bool ok, const QVariantList &monitors, bool settled)> done) {
    if (wanted && !wanted())
        return;
    m_runner->run(m_runner->env().hyprctl({"-j", "monitors", "all"}), CommandOptions{3000}, this, [this, attempts, wanted, settled, done](const CommandResult &query) {
        if (wanted && !wanted())
            return;
        QVariantList monitors;
        const bool known = query.ok() && !query.truncated && decodeMonitors(query.out, &monitors);
        const bool landed = known && settled(monitors);
        // A failed read is not a frame behind; waiting would not change it.
        if (known && !landed && attempts > 1) {
            QTimer::singleShot(ObserveIntervalMs, this, [this, attempts, wanted, settled, done] { readMonitorsUntil(attempts - 1, wanted, settled, done); });
            return;
        }
        done(known, monitors, landed);
    });
}

void DisplayProvider::verifyLayout(const QStringList &expected, std::function<void(bool same, bool known)> done) {
    // Called once the watchdog reports its restore, which Hyprland shows a frame later.
    readMonitorsUntil(
        ObserveAttempts, {}, [expected](const QVariantList &monitors) { return matchesWherePresent(expected, signatureOf(monitors)); },
        [this, done](bool ok, const QVariantList &monitors, bool same) {
            if (!ok) {
                done(false, false);
                return;
            }
            setMonitors(monitors);
            emit changed();
            done(same, true);
        });
}

void DisplayProvider::endPreview(const QString &how) {
    if (m_previewToken.isEmpty()) QFile::remove(m_resultFile);
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
    if (!m_previewing || m_previewToken.isEmpty()) return;
    const auto receipt = previewReceipt();
    if (!DisplayPreview::sameSession(m_runner->env(), receipt) || receipt.value("phase") != "preview" || receipt.value("deadline").toDouble() <= QDateTime::currentMSecsSinceEpoch()) {
        emit message(tr("The display preview has expired or belongs to another session. Inspect it before continuing."), true); return;
    }
    m_countdown.stop();
    QString error;
    if (QFileInfo(m_keepFile).isSymLink() || !FileOps::atomicWrite(m_keepFile, "user-confirmed-usable\n", &error)) { emit message(error, true); return; }
    waitForWatchdog(m_resultFile, "kept", 400, [this](bool kept) {
        const auto receipt = previewReceipt();
        const bool saved = kept && receipt.value("phase") == "kept";
        m_previewing = false; m_starting = false; emit previewChanged();
        Transaction tx = Transaction::fromJson(receipt.value("transaction").toObject());
        if (!saved && tx.phase.isEmpty()) { tx.phase = "partial"; tx.message = tr("The preview ended without a confirmed save. Inspect its receipt and current outputs."); }
        emit message(saved ? tr("Display layout saved.") : tx.message, !saved);
        emit operationFinished(tx);
        emit changed(); refresh();
    });
}

void DisplayProvider::revert() {
    if (!m_previewing && m_previewToken.isEmpty()) return;
    if (!m_previewToken.isEmpty()) {
        QString error; const auto receipt = previewReceipt();
        if (!DisplayPreview::sameSession(m_runner->env(), receipt) || DisplayPreview::terminal(receipt) || !DisplayPreview::prepare(m_runner->env(), &error)) { emit message(error.isEmpty() ? tr("The preview has ended or belongs to another session.") : error, true); return; }
        if (!QFileInfo::exists(m_planPath)) {
            QJsonArray previous; for (const auto &v : receipt.value("original").toArray()) previous.append(monitorRule(v.toObject().toVariantMap()));
            const QJsonObject plan{{"token", m_previewToken}, {"previous", previous}, {"seconds", PreviewSeconds}, {"keepFile", m_keepFile}, {"revertFile", m_revertFile}, {"resultFile", m_resultFile}};
            if (QFileInfo(m_planPath).isSymLink() || !FileOps::atomicWrite(m_planPath, QJsonDocument(plan).toJson(), &error)) { emit message(error, true); return; }
        }
        if (QFileInfo(m_revertFile).isSymLink() || !FileOps::atomicWrite(m_revertFile, "restore-requested\n", &error)) { emit message(error, true); return; }
        // An existing owner holds its own watchdog lock. A replacement exits
        // harmlessly when that owner is alive, and recovers if it has died.
        if (!m_runner->launchDetached({m_runner->env().selfBinary, "display-watchdog", m_planPath}, &error)) { emit message(error, true); return; }
    } else {
        QFile revertFile(m_revertFile);
        if (revertFile.open(QIODevice::WriteOnly)) revertFile.close();
    }
    m_countdown.stop();
    waitForWatchdog(m_resultFile, "restored", 200, [this](bool restored) {
        const auto receipt = previewReceipt();
        if (!m_previewToken.isEmpty()) restored &= receipt.value("phase") == "reverted";
        m_previewing = false; m_starting = false;
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
    m_checkOutput.clear();
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
            m_checkOutput = row.value("output").toString();
            m_checkX = row.value("x").toInt();
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
    // The restore lands a frame after the watchdog reports it, so wait for it to show.
    // A watchdog that reported no restore has failed the check already; one read says
    // what the layout is now.
    readMonitorsUntil(
        restored ? ObserveAttempts : 1, {}, [this](const QVariantList &monitors) { return signatureOf(monitors) == m_checkOriginal; },
        [this, restored](bool ok, const QVariantList &monitors, bool same) { reportCheck(ok, monitors, restored, same); });
}

void DisplayProvider::reportCheck(bool ok, const QVariantList &monitors, bool restored, bool same) {
    if (ok)
        setMonitors(monitors);
    const bool passed = restored && same && m_checkMoved;
    // Say which step failed, then what the layout is now, read back from Hyprland.
    QString reason;
    if (!ok)
        reason = tr("Hyprland could not be asked for the layout afterwards.");
    else if (!m_checkMoved)
        reason = tr("Hyprland did not report the test move, so a restore would prove nothing.");
    else if (!restored)
        reason = tr("The safety timer did not report putting the layout back.");
    else if (!same)
        reason = tr("The layout after the check is not the one from before it.");
    const QString verified = !ok ? tr("The current layout is unknown. Check your displays.")
                           : same ? tr("Hyprland reports the original layout, so nothing was left changed.")
                                  : tr("Hyprland reports a different layout than before the check. Check your displays.");
    QJsonObject record;
    record["passed"] = passed;
    record["hyprland"] = m_hyprlandVersion;
    record["at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    record["monitors"] = monitors.size();
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
}

int DisplayProvider::runWatchdog(const Env &env, const QString &planPath) {
    struct stat planInfo{};
    if (::lstat(QFile::encodeName(planPath).constData(), &planInfo) || !S_ISREG(planInfo.st_mode) || planInfo.st_uid != geteuid() || planInfo.st_nlink != 1 || planInfo.st_size > 1024 * 1024) return 1;
    QFile input(planPath);
    if (!input.open(QIODevice::ReadOnly)) return 1;
    const auto contents = input.read(1024 * 1024 + 1);
    if (input.error() != QFileDevice::NoError || contents.size() > 1024 * 1024) return 1;
    const auto plan = QJsonDocument::fromJson(contents).object();
    const QString token = plan.value("token").toString(); const bool durable = !token.isEmpty();
    QJsonObject receipt; QString error;
    std::unique_ptr<QLockFile> watchdogLock;
    if (durable) {
        receipt = DisplayPreview::load(env, token, &error);
        const QString base = DisplayPreview::runtimeBase(env, token);
        if (receipt.isEmpty() || !DisplayPreview::sameSession(env, receipt) || DisplayPreview::terminal(receipt)
            || !DisplayPreview::prepare(env, &error) || planPath != base + ".json"
            || plan.value("keepFile") != base + ".keep" || plan.value("revertFile") != base + ".revert" || plan.value("resultFile") != base + ".result"
            || QFileInfo(planPath).isSymLink() || QFileInfo(planPath).ownerId() != uint(geteuid())) return 1;
        QJsonArray previous; for (const auto &v : receipt.value("original").toArray()) previous.append(monitorRule(v.toObject().toVariantMap()));
        if (previous != plan.value("previous").toArray()) return 1;
        watchdogLock = std::make_unique<QLockFile>(base + ".watchdog.lock");
        if (!watchdogLock->tryLock(0)) return 1;
    }
    const QString keepFile = plan.value("keepFile").toString(), revertFile = plan.value("revertFile").toString(), resultFile = plan.value("resultFile").toString();
    const int seconds = qBound(0, plan.value("seconds").toInt(PreviewSeconds), 60);
    auto report = [&](const QByteArray &text) {
        struct stat info{};
        if (durable && ::lstat(QFile::encodeName(resultFile).constData(), &info) == 0 && (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1 || (info.st_mode & 0077))) return false;
        return FileOps::atomicWrite(resultFile, text + "\n");
    };
    if (!report("armed")) return 1;
    CommandRunner runner(env); QElapsedTimer restoreBudget; bool restoring = false;
    auto hyprctl = [&](const QStringList &arguments, QByteArray *out = nullptr) {
        QEventLoop loop; CommandResult result; bool finished = false;
        if (restoring && restoreBudget.elapsed() >= 20000) return false;
        CommandOptions options{restoring ? int(std::min<qint64>(3000, 20000 - restoreBudget.elapsed())) : 3000}; options.maxOutputBytes = 1024 * 1024; options.mutation = arguments.value(0) == "eval";
        runner.run(env.hyprctl(arguments), options, &loop, [&](const CommandResult &value) { result = value; finished = true; loop.quit(); });
        if (!finished) loop.exec();
        if (out) *out = result.out;
        return result.ok() && !result.truncated;
    };
    auto readMonitors = [&](QVariantList *rows) { QByteArray bytes; return hyprctl({"-j", "monitors", "all"}, &bytes) && decodeMonitors(bytes, rows); };
    auto sameHardware = [](const QVariantList &before, const QVariantList &now) {
        if (before.size() != now.size()) return false;
        for (const auto &v : before) {
            const auto row = v.toMap(); bool found = false;
            for (const auto &n : now) {
                const auto current = n.toMap(); if (current.value("output") != row.value("output")) continue;
                found = true;
                for (const auto &key : {"id", "make", "model", "serial", "description"}) if (QJsonValue::fromVariant(current.value(key)) != QJsonValue::fromVariant(row.value(key))) return false;
            }
            if (!found) return false;
        }
        return true;
    };
    const qint64 remaining = durable ? qBound<qint64>(0, qint64(receipt.value("deadline").toDouble()) - QDateTime::currentMSecsSinceEpoch(), seconds * 1000LL) : seconds * 1000LL;
    QElapsedTimer timer; timer.start(); bool kept = false;
    while (timer.elapsed() < remaining && (!durable || QDateTime::currentMSecsSinceEpoch() < receipt.value("deadline").toDouble())) {
        if (QFile::exists(revertFile)) break;
        if (QFile::exists(keepFile)) { kept = true; break; }
        QThread::msleep(50);
    }
    if (kept && durable) {
        // The detached owner performs persistence itself. Losing the caller
        // cannot leave a Keep acknowledgement between watchdog and file commit.
        receipt = DisplayPreview::load(env, token, &error);
        QVariantList rows; QByteArray version;
        const auto original = receipt.value("original").toArray().toVariantList(), layout = receipt.value("layout").toArray().toVariantList();
        kept = receipt.value("phase") == "preview" && receipt.value("deadline").toDouble() > QDateTime::currentMSecsSinceEpoch()
            && readMonitors(&rows) && sameHardware(original, rows) && signatureOf(rows) == signatureOf(layout)
            && hyprctl({"-j", "version"}, &version) && QJsonDocument::fromJson(version).object().value("version") == receipt.value("hyprland");
        if (kept) {
            receipt["phase"] = "keeping"; kept = DisplayPreview::save(env, receipt, &error);
        }
        if (kept) {
            ChangeCoordinator coordinator(env);
            ChangeCoordinator::Request request; HyprModule module(env);
            request.settingId = "displays.layout"; request.label = tr("Display layout"); request.after = "custom";
            request.files = {module.path(), module.hyprlandPath()}; request.extra = {{"displayToken", token}};
            const auto fingerprints = receipt.value("fingerprints").toObject();
            if (fingerprints.size() != 2 || !fingerprints.contains(module.path()) || !fingerprints.contains(module.hyprlandPath())) kept = false;
            for (auto it = fingerprints.begin(); it != fingerprints.end(); ++it) request.expectedFingerprints.insert(it.key(), it.value().toString());
            request.preflight = [&, original, layout](ChangeCoordinator::Done done) { QVariantList fresh; done(!QFile::exists(revertFile) && readMonitors(&fresh) && sameHardware(original, fresh) && signatureOf(fresh) == signatureOf(layout), "Outputs changed before Keep could save the layout."); };
            request.apply = [&, layout](ChangeCoordinator::Done done) {
                HyprModule target(env); QString problem;
                if (QFile::exists(revertFile)) { done(false, "Restoration was requested before Keep wrote files."); return; }
                if (!target.load(&problem)) { done(false, problem); return; }
                QJsonArray rules; for (const auto &v : layout) rules.append(monitorRule(v.toMap()));
                target.setMonitors(rules); QByteArray import;
                if (!target.plannedImport(&import, &problem) || !DisplayPreview::capturePersistence(env, &receipt, target.render(), import, &problem)) { done(false, problem); return; }
                done(target.save(&problem) && !QFile::exists(revertFile) && target.installImport(&problem), problem);
            };
            request.writtenFingerprints = [&] {
                QHash<QString, QString> written; HyprModule target(env); const auto files = receipt.value("persistence").toObject();
                if (files.contains("module")) written[target.path()] = files.value("module").toObject().value("after").toString();
                if (files.contains("import")) written[target.hyprlandPath()] = files.value("import").toObject().value("after").toString();
                return written;
            };
            request.verify = [&, layout](ChangeCoordinator::Done done) {
                HyprModule target(env); const auto files = receipt.value("persistence").toObject(); QVariantList fresh;
                done(!QFile::exists(revertFile) && FileOps::fingerprint(target.path()) == files.value("module").toObject().value("after").toString()
                    && FileOps::fingerprint(target.hyprlandPath()) == files.value("import").toObject().value("after").toString()
                    && readMonitors(&fresh) && signatureOf(fresh) == signatureOf(layout), "The owned files and saved display layout could not both be confirmed.");
            };
            if (kept) {
                QEventLoop loop; bool finished = false; Transaction transaction;
                coordinator.submit(std::move(request), [&](const Transaction &tx) { transaction = tx; finished = true; loop.quit(); });
                if (!finished) loop.exec();
                receipt["transaction"] = transaction.toJson(); kept = transaction.phase == "committed";
                if (!kept) receipt["error"] = transaction.message;
            }
        }
        if (kept) {
            receipt["phase"] = "kept";
            if (!DisplayPreview::save(env, receipt, &error)) { report("keep-unconfirmed"); return 1; }
        }
    }
    if (kept) report("kept");
    else {
        std::unique_ptr<QLockFile> changeLock; bool filesRestored = true;
        if (durable) {
            changeLock = std::make_unique<QLockFile>(env.appStateDir + "/change.lock");
            if (!changeLock->tryLock(10000)) { receipt["phase"] = "restore_failed"; receipt["error"] = "Another writer prevented restoration. Retry display preview revert."; DisplayPreview::save(env, receipt); report("restore-failed"); return 1; }
            receipt = DisplayPreview::load(env, token, &error);
            if (receipt.value("phase") == "not_started") { report("not-started"); QFile::remove(planPath); QFile::remove(keepFile); QFile::remove(revertFile); return 0; }
            filesRestored = !receipt.isEmpty() && DisplayPreview::restorePersistence(env, receipt, &error);
            if (!filesRestored) receipt["error"] = error;
            receipt["phase"] = "reverting"; DisplayPreview::save(env, receipt);
        }
        restoring = true; restoreBudget.start();
        QVariantList current; const bool known = readMonitors(&current);
        QStringList present; for (const auto &v : current) present << v.toMap().value("output").toString();
        bool anyRestoredOn = false, allOk = known;
        const auto original = receipt.value("original").toArray().toVariantList();
        const auto previous = plan.value("previous").toArray();
        for (int pass = 0; pass < 2; ++pass) for (const auto &v : previous) {
            const auto rule = v.toObject(); const bool disabled = rule.value("disabled").toBool();
            if (disabled != (pass == 1)) continue;
            const QString output = rule.value("output").toString();
            if (!known || !isSafeOutputName(output) || !present.contains(output)) continue;
            if (durable) {
                QVariantList beforeOne, nowOne;
                for (const auto &row : original) if (row.toMap().value("output") == output) beforeOne << row;
                for (const auto &row : current) if (row.toMap().value("output") == output) nowOne << row;
                if (!known || !sameHardware(beforeOne, nowOne)) { allOk = false; continue; }
            }
            if (disabled && !anyRestoredOn) continue;
            const bool ok = hyprctl({"eval", monitorLua(rule)}); allOk &= ok; if (!disabled && ok) anyRestoredOn = true;
        }
        bool lastResort = false;
        if (!anyRestoredOn && known) for (const auto &name : present) {
            if (isSafeOutputName(name) && hyprctl({"eval", QString("hl.monitor({ output = \"%1\", mode = \"preferred\", position = \"auto\", scale = 1 })").arg(name)})) { lastResort = true; break; }
        }
        bool restored = allOk && (anyRestoredOn || lastResort);
        if (durable) {
            // Command acceptance alone cannot certify restoration. Poll later
            // frames, binding the readback to the outputs that are still present.
            restored = false; QElapsedTimer observation; observation.start();
            while (allOk && observation.elapsed() < 3000) {
                QVariantList observed;
                if (!readMonitors(&observed)) break;
                bool enabled = false; for (const auto &v : observed) enabled |= v.toMap().value("enabled").toBool();
                if (enabled && matchesWherePresent(signatureOf(original), signatureOf(observed)) && !lastResort) { restored = true; break; }
                QThread::msleep(50);
            }
            restored &= filesRestored;
            receipt["phase"] = restored ? "reverted" : "restore_failed";
            receipt["restoreVerified"] = restored;
            receipt["persistenceRestored"] = filesRestored;
            receipt["restoreScope"] = "original-identity-outputs-still-present-and-captured-owned-files";
            if (lastResort) receipt["error"] = "A fallback output was enabled; the original layout could not be fully restored.";
            if (!DisplayPreview::save(env, receipt, &error)) { report("restore-failed"); return 1; }
        }
        report(restored ? "restored" : "restore-failed");
        if (durable && !restored) return 1; // Retain the plan for guarded retry.
    }
    QFile::remove(planPath); QFile::remove(keepFile); QFile::remove(revertFile);
    return 0;
}
