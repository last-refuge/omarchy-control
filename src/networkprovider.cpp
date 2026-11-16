#include "networkprovider.h"
#include "displaypreview.h"
#include "networkpreview.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace {

const QStringList Sections{"general", "devices", "saved", "wifi"};
const QStringList GuiDeviceTypes{"wifi", "ethernet", "tun", "wireguard", "vpn"};
const QStringList MachineEnvironment{"env", "LC_ALL=C.UTF-8", "LANGUAGE=C"};

// Locale and color settings are part of the read protocol, independent of the
// desktop's language or the environment from which the GUI/CLI was started.
QStringList readCommand(const QString &section) {
    const QStringList prefix = MachineEnvironment + QStringList{"nmcli", "-t", "-c", "no", "-e", "yes", "-f"};
    if (section == QLatin1String("general")) return prefix + QStringList{"WIFI,CONNECTIVITY", "general"};
    if (section == QLatin1String("devices")) return prefix + QStringList{"DEVICE,TYPE,STATE,CONNECTION", "device"};
    if (section == QLatin1String("saved")) return prefix + QStringList{"NAME,UUID,TYPE,ACTIVE", "connection", "show"};
    return prefix + QStringList{"IN-USE,SSID,SIGNAL,SECURITY,BSSID,DEVICE", "device", "wifi", "list", "--rescan", "no"};
}

struct NetworkRead {
    QVariant data;
    QString error;
};

bool terseFields(const QString &line, QStringList *fields) {
    QString field;
    for (int i = 0; i < line.size(); ++i) {
        const auto c = line[i];
        if (c == '\\') {
            if (++i >= line.size() || (line[i] != QLatin1Char('\\') && line[i] != QLatin1Char(':'))) return false;
            field += line[i];
        } else if (c == ':') {
            *fields << field;
            field.clear();
        } else {
            field += c;
        }
    }
    *fields << field;
    return true;
}

NetworkRead readRows(const QString &section, const CommandResult &result) {
    if (!result.ok()) return {{}, result.describe("nmcli")};
    if (result.truncated || result.out.size() > 1024 * 1024)
        return {{}, "NetworkManager output exceeded its read limit."};
    const QString text = QString::fromUtf8(result.out);
    if (text.toUtf8() != result.out) return {{}, "NetworkManager returned invalid UTF-8."};
    // nmcli separates access-point tables for different Wi-Fi interfaces with
    // empty lines, even in terse mode. Only nonempty records have fields.
    const auto lines = text.split('\n', Qt::SkipEmptyParts);
    if (lines.size() > 4096) return {{}, "NetworkManager returned too many rows."};
    const int count = section == QLatin1String("general") ? 2 : section == QLatin1String("wifi") ? 6 : 4;
    if (section == QLatin1String("general") && lines.size() != 1)
        return {{}, "NetworkManager did not return one general-state row."};
    QVariantList rows;
    QSet<QString> identities;
    static const QRegularExpression uuid("^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}$");
    static const QRegularExpression bssid("^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$");
    for (const auto &line : lines) {
        QStringList f;
        if (!terseFields(line, &f) || f.size() != count)
            return {{}, "NetworkManager returned a malformed or incomplete row."};
        if (section == QLatin1String("general")) {
            if (!QStringList{"enabled", "disabled"}.contains(f[0]) || !QStringList{"unknown", "none", "portal", "limited", "full"}.contains(f[1]))
                return {{}, "NetworkManager returned an unrecognized radio/connectivity state."};
            return {QVariantMap{{"wifiEnabled", f[0] == QLatin1String("enabled")}, {"connectivity", f[1]}}, {}};
        }
        QVariantMap row;
        QString identity;
        if (section == QLatin1String("devices")) {
            if (f[0].isEmpty() || f[1].isEmpty() || f[2].isEmpty())
                return {{}, "NetworkManager returned an incomplete device identity/state."};
            const bool connected = f[2] == QLatin1String("connected") || f[2].startsWith("connected (");
            const bool known = connected || QStringList{"unknown", "unmanaged", "unavailable", "disconnected", "failed"}.contains(f[2])
                || f[2].startsWith("connecting (") || f[2].startsWith("disconnecting (");
            row = {{"device", f[0]}, {"type", f[1]}, {"state", f[2]}, {"connection", f[3]},
                   {"connected", known && f[2] != QLatin1String("unknown") ? QVariant(connected) : QVariant()}, {"stateKnown", known && f[2] != QLatin1String("unknown")},
                   {"external", f[2].contains("(externally)")}, {"shownInGui", GuiDeviceTypes.contains(f[1])}};
            identity = f[0];
        } else if (section == QLatin1String("saved")) {
            if (f[0].isEmpty() || !uuid.match(f[1]).hasMatch() || f[2].isEmpty() || !QStringList{"yes", "no"}.contains(f[3]))
                return {{}, "NetworkManager returned an invalid profile identity/active state."};
            row = {{"name", f[0]}, {"uuid", f[1]}, {"type", f[2]}, {"active", f[3] == QLatin1String("yes")}};
            identity = f[1].toLower();
        } else {
            bool numeric = false;
            const int signal = f[2].toInt(&numeric);
            if (!QStringList{"", "*"}.contains(f[0].trimmed()) || !numeric || signal < 0 || signal > 100
                || !bssid.match(f[4]).hasMatch() || f[5].isEmpty())
                return {{}, "NetworkManager returned invalid access-point identity/signal data."};
            row = {{"active", f[0].trimmed() == QLatin1String("*")}, {"ssid", f[1]}, {"signal", signal}, {"security", f[3]},
                   {"secured", !f[3].isEmpty() && f[3] != QLatin1String("--")}, {"bssid", f[4].toUpper()}, {"device", f[5]}, {"ssidEmpty", f[1].isEmpty()}};
            identity = f[5] + ":" + f[4].toUpper();
        }
        if (identities.contains(identity)) return {{}, "NetworkManager returned a duplicate row identity."};
        identities.insert(identity);
        rows << row;
    }
    return {rows, {}};
}

QVariantList groupNetworks(const QVariantList &accessPoints) {
    QHash<QString, QVariantMap> best;
    QStringList order;
    for (const auto &value : accessPoints) {
        const auto row = value.toMap();
        const QString ssid = row.value("ssid").toString();
        if (ssid.isEmpty()) continue;
        const auto existing = best.constFind(ssid);
        if (existing == best.constEnd()) order << ssid;
        if (existing == best.constEnd() || row.value("active").toBool()
            || (!existing->value("active").toBool() && row.value("signal").toInt() > existing->value("signal").toInt())) best.insert(ssid, row);
    }
    QVariantList rows;
    for (const auto &ssid : order) rows << best.value(ssid);
    std::stable_sort(rows.begin(), rows.end(), [](const QVariant &a, const QVariant &b) {
        const auto left = a.toMap(), right = b.toMap();
        if (left.value("active").toBool() != right.value("active").toBool()) return left.value("active").toBool();
        return left.value("signal").toInt() > right.value("signal").toInt();
    });
    return rows;
}

}  // namespace

struct NetworkProvider::ScanWork {
    QString requested;
    ProviderPlan plan;
    QStringList devices, paths;
    QVariantList results;
    QElapsedTimer deadline;
    bool attempted = false;
    QString code = "busy", error;
};

static const QStringList ManagerCall{"busctl", "call", "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager"};
// Long enough to cover a slow connection attempt and still leave time to decide.
static constexpr int CheckpointSeconds = 60;

int NetworkProvider::keepTotal() const {
    return CheckpointSeconds;
}

NetworkProvider::NetworkProvider(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *parent)
    : QObject(parent), m_runner(runner), m_coordinator(coordinator) {
    for (const auto &section : Sections) m_states[section] = QVariantMap{{"status", "not_requested"}, {"error", QString()}};
    m_keepTimer.setInterval(1000);
    connect(&m_keepTimer, &QTimer::timeout, this, [this] {
        if (!m_ownedToken.isEmpty()) { m_keepSeconds = qMax(0, m_keepSeconds - 1); emit keepChanged(); if (!m_ownedReading) ownedCommand("status"); return; }
        m_keepSeconds = qMax<qint64>(0, (m_checkpointDeadline - QDateTime::currentMSecsSinceEpoch()) / 1000);
        if (m_keepSeconds <= 0) {
            // Expiry alone does not prove successful daemon/driver rollback.
            m_keepTimer.stop();
            m_checkpoint.clear();
            m_connectionLock.reset();
            emit message(tr("The connection preview ended without a confirmed result. Check the current connection."), true);
            refresh();
        }
        emit keepChanged();
    });
}

// nmcli's terse mode escapes ':' and '\' inside values.
QStringList NetworkProvider::splitTerse(const QString &line) {
    QStringList fields;
    QString current;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (c == '\\' && i + 1 < line.size()) {
            current += line[++i];
        } else if (c == ':') {
            fields << current;
            current.clear();
        } else {
            current += c;
        }
    }
    fields << current;
    return fields;
}

QVariantList NetworkProvider::parseNetworks(const QByteArray &text) {
    QHash<QString, QVariantMap> best;
    QStringList order;
    for (const auto &line : QString::fromUtf8(text).split('\n', Qt::SkipEmptyParts)) {
        const auto fields = splitTerse(line);
        if (fields.size() < 4 || fields[1].isEmpty())
            continue;
        QVariantMap row;
        row["active"] = fields[0].trimmed() == QLatin1String("*");
        row["ssid"] = fields[1];
        row["signal"] = fields[2].toInt();
        row["security"] = fields[3];
        row["secured"] = !fields[3].isEmpty() && fields[3] != QLatin1String("--");
        const auto existing = best.constFind(fields[1]);
        if (existing == best.constEnd())
            order << fields[1];
        // One row per name: the strongest access point, unless another is in use.
        if (existing == best.constEnd() || row["active"].toBool() || (!existing->value("active").toBool() && row["signal"].toInt() > existing->value("signal").toInt()))
            best.insert(fields[1], row);
    }
    QVariantList rows;
    for (const auto &ssid : order)
        rows << best.value(ssid);
    std::stable_sort(rows.begin(), rows.end(), [](const QVariant &a, const QVariant &b) {
        const auto ma = a.toMap(), mb = b.toMap();
        if (ma.value("active").toBool() != mb.value("active").toBool())
            return ma.value("active").toBool();
        return ma.value("signal").toInt() > mb.value("signal").toInt();
    });
    return rows;
}

QVariantList NetworkProvider::parseDevices(const QByteArray &text) {
    QVariantList rows;
    for (const auto &line : QString::fromUtf8(text).split('\n', Qt::SkipEmptyParts)) {
        const auto fields = splitTerse(line);
        if (fields.size() < 4)
            continue;
        const QString type = fields[1];
        if (type != QLatin1String("wifi") && type != QLatin1String("ethernet") && type != QLatin1String("tun") && type != QLatin1String("wireguard") && type != QLatin1String("vpn"))
            continue;
        QVariantMap row;
        row["device"] = fields[0];
        row["type"] = type;
        row["state"] = fields[2];
        row["connection"] = fields[3];
        row["connected"] = fields[2].startsWith("connected");
        // Set up by something else, such as Tailscale: shown, never driven from here.
        row["external"] = fields[2].contains("externally");
        rows << row;
    }
    return rows;
}

QVariantList NetworkProvider::parseSaved(const QByteArray &text) {
    QVariantList rows;
    for (const auto &line : QString::fromUtf8(text).split('\n', Qt::SkipEmptyParts)) {
        const auto fields = splitTerse(line);
        if (fields.size() < 4)
            continue;
        const QString type = fields[2];
        if (type == QLatin1String("loopback") || type == QLatin1String("bridge") || type == QLatin1String("tun"))
            continue;
        QVariantMap row;
        row["name"] = fields[0];
        row["uuid"] = fields[1];
        row["type"] = type;
        row["active"] = fields[3] == QLatin1String("yes");
        rows << row;
    }
    return rows;
}

void NetworkProvider::refreshOwnedPreview() {
    if (!m_checkpoint.isEmpty() || m_connectionLock || m_ownedReading) return;
    QString error; const auto receipt = NetworkPreview::localReceipt(m_runner->env(), &error);
    if (receipt.isEmpty()) return;
    const QString phase = receipt.value("phase").toString();
    const bool terminal = QStringList{"kept", "reverted", "absent", "daemon_changed", "refused"}.contains(phase);
    if (terminal && m_ownedToken.isEmpty()) return;
    if (m_ownedToken != receipt.value("token").toString()) { m_ownedConfirmed = false; m_keepSeconds = 0; }
    m_ownedToken = receipt.value("token").toString(); m_ownedPhase = phase; m_ownedReceipt = receipt;
    m_keepTimer.start(); emit keepChanged();
    ownedCommand("status");
}

void NetworkProvider::ownedCommand(const QString &action) {
    if (m_ownedToken.isEmpty()) return;
    if (action != "status") {
        if (m_ownedChanging) { emit message(tr("A connection confirmation is already running."), true); return; }
        m_ownedChanging = true; emit changed(); emit keepChanged();
    }
    if (m_ownedReading) { if (action != "status") m_ownedQueued = action; return; }
    m_ownedReading = true;
    const QString token = m_ownedToken;
    QStringList argv{m_runner->env().selfBinary, "network", "preview", action, token, "--api-version", "1"};
    if (action == "keep") argv << "--yes"; // The person's explicit Keep button.
    CommandOptions options{90000}; options.maxOutputBytes = 256 * 1024; options.mutation = action != "status";
    m_runner->run(argv, options, this, [this, token, action](const CommandResult &read) {
        m_ownedReading = false;
        const auto body = QJsonDocument::fromJson(read.out).object(), data = body.value("data").toObject();
        const auto receipt = data.value("receipt").toObject();
        const bool valid = !read.truncated && body.value("apiVersion") == 1 && receipt.value("token") == token;
        const bool ok = valid && read.ok() && body.value("ok").toBool();
        m_ownedConfirmed = ok && data.value("targetStateVerified").toBool();
        if (valid) {
            m_ownedReceipt = receipt; m_ownedPhase = receipt.value("phase").toString();
            m_keepSeconds = qBound(0, data.value("remainingSeconds").toInt(0), CheckpointSeconds);
        } else { m_ownedConfirmed = false; m_keepSeconds = 0; }
        const bool terminal = valid && QStringList{"kept", "reverted", "absent", "daemon_changed", "refused"}.contains(m_ownedPhase);
        if (terminal) { m_ownedToken.clear(); m_ownedQueued.clear(); m_keepTimer.stop(); m_ownedChanging = false; }
        if (action != "status") {
            m_ownedChanging = false;
            emit message(ok && body.value("outcome") == "kept" ? tr("Connection kept")
                : ok && body.value("outcome") == "reverted" ? tr("The previous connection was restored.")
                : tr("The connection result could not be confirmed. Inspect its current state before retrying."), !ok);
            refreshSnapshot();
        } else if (terminal && !ok) emit message(tr("The connection preview ended without a confirmed result. Check the current connection."), true);
        const QString queued = m_ownedQueued;
        m_ownedQueued.clear();
        emit keepChanged(); emit changed();
        if (!queued.isEmpty() && !terminal) { m_ownedChanging = false; ownedCommand(queued); }
    });
}

void NetworkProvider::refresh() {
    refreshSnapshot();
    refreshOwnedPreview();
}

QVariantMap NetworkProvider::snapshot() const {
    const auto general = m_values.value("general").toMap();
    return {{"status", m_status}, {"complete", m_status == QLatin1String("ok")}, {"states", m_states},
            {"requestedSections", m_requested}, {"startedAt", m_startedAt}, {"generation", m_generation},
            {"wifiEnabled", general.value("wifiEnabled")}, {"connectivity", general.value("connectivity")},
            {"connectivityKnown", general.contains("connectivity") && general.value("connectivity").toString() != QLatin1String("unknown")},
            {"devices", m_values.value("devices")}, {"saved", m_values.value("saved")}, {"accessPoints", m_values.value("wifi")},
            {"scanRequested", false}, {"accessPointAgeKnown", false}, {"connectivityCheckRequested", false}, {"atomic", false},
            {"scope", "NetworkManager-reported state and cached access points visible to this caller; no scan, connectivity probe, secrets or connection changes"}};
}

void NetworkProvider::refreshSnapshot(const QString &section) {
    refreshSections(section.isEmpty() ? Sections : QStringList{section});
}

void NetworkProvider::refreshScanState() {
    refreshSections({"general", "devices"});
}

void NetworkProvider::refreshSections(const QStringList &sections) {
    const int generation = ++m_generation;
    m_requested = sections;
    m_readsPending = m_requested.size();
    m_startedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    m_values.clear();
    m_status = "loading";
    for (const auto &key : Sections)
        m_states[key] = QVariantMap{{"status", m_requested.contains(key) ? "loading" : "not_requested"}, {"error", QString()}};
    const auto requested = m_requested;
    emit changed();
    if (generation != m_generation) return;
    for (const auto &key : requested) {
        if (generation != m_generation) return;
        if (!Sections.contains(key)) {
            m_states[key] = QVariantMap{{"status", "read_unavailable"}, {"error", "Unknown network read section."}};
            m_status = "unavailable";
            m_readsPending = 0;
            emit changed();
            if (generation == m_generation) emit refreshed();
            return;
        }
        const auto command = readCommand(key);
        CommandOptions options{4000};
        options.maxOutputBytes = 1024 * 1024;
        m_runner->run(command, options, this, [this, generation, key, command](const CommandResult &result) {
            if (generation != m_generation) return;
            const auto read = readRows(key, result);
            m_values[key] = read.data;
            m_states[key] = QVariantMap{{"status", read.error.isEmpty() ? "ok" : "read_unavailable"}, {"error", read.error},
                                       {"command", command}, {"completedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
            if (--m_readsPending) return;
            int ok = 0;
            for (const auto &requested : m_requested) ok += m_states.value(requested).toMap().value("status") == QLatin1String("ok");
            m_status = ok == m_requested.size() ? "ok" : ok ? "partial" : "unavailable";
            const auto general = m_values.value("general").toMap();
            m_wifiEnabled = general.value("wifiEnabled").toBool();
            m_connectivity = general.value("connectivity").toString();
            m_devices.clear(); m_saved.clear(); m_networks.clear();
            for (const auto &value : m_values.value("devices").toList())
                if (value.toMap().value("shownInGui").toBool()) m_devices << value;
            for (const auto &value : m_values.value("saved").toList())
                if (!QStringList{"loopback", "bridge", "tun"}.contains(value.toMap().value("type").toString())) m_saved << value;
            m_networks = groupNetworks(m_values.value("wifi").toList());
            emit changed();
            if (generation == m_generation) emit refreshed();
        });
    }
}

void NetworkProvider::rescan() {
    if (m_scanning) return;
    requestScan({}, [this](const QVariantMap &) { refresh(); });
}

bool NetworkProvider::isScanDeviceName(const QString &device) {
    if (device.isEmpty() || device.toUtf8().size() > 15 || device.startsWith('-') || device == QLatin1String(".") || device == QLatin1String("..")) return false;
    return std::none_of(device.begin(), device.end(), [](QChar c) {
        return c.isSpace() || c.category() == QChar::Other_Control || c == '/' || c == ':';
    });
}

ProviderPlan NetworkProvider::planScan(const QString &device) const {
    return scanPlan(device, false);
}

ProviderPlan NetworkProvider::scanPlan(const QString &device, bool ownRequest) const {
    ProviderPlan plan;
    plan.operation = "network.rescan";
    plan.target = device.isEmpty() ? "eligible-wifi-interfaces" : device;
    plan.after = "scan-request-accepted";
    const auto fail = [&](const QString &code, const QString &error) { plan.errorCode = code; plan.error = error; return plan; };
    const QString preview = NetworkPreview::pendingReason(m_runner->env());
    if (!preview.isEmpty()) return fail("busy", preview);
    if (!device.isEmpty() && !isScanDeviceName(device)) return fail("invalid_value", "Use a current Wi-Fi interface name from network devices.");
    if ((m_scanning && !ownRequest) || m_radioBusy || !m_coordinator->leaseRun().isEmpty() || m_coordinator->leaseHeldElsewhere())
        return fail("busy", "Finish the current scan request or profile restore before requesting another scan.");
    for (const auto &program : {"env", "nmcli", "busctl"})
        if (m_runner->env().findExecutable(program).isEmpty()) return fail("unavailable", "Network scan requests require nmcli, env and busctl.");
    if (m_states.value("general").toMap().value("status") != QLatin1String("ok") || m_states.value("devices").toMap().value("status") != QLatin1String("ok"))
        return fail("read_unavailable", "Wi-Fi radio and device state must be readable before a scan request.");
    if (!m_values.value("general").toMap().value("wifiEnabled").toBool()) return fail("unavailable", "Wi-Fi is disabled in NetworkManager.");
    QStringList devices;
    QJsonArray excluded;
    bool found = device.isEmpty();
    for (const auto &value : m_values.value("devices").toList()) {
        const auto row = value.toMap();
        const QString name = row.value("device").toString();
        if (!device.isEmpty() && name != device) continue;
        if (row.value("type") != QLatin1String("wifi")) {
            if (!device.isEmpty()) return fail("invalid_value", "That interface is not a Wi-Fi device.");
            continue;
        }
        found = true;
        const QString state = row.value("state").toString();
        const bool eligible = isScanDeviceName(name) && row.value("stateKnown").toBool() && !row.value("external").toBool()
            && state != QLatin1String("unmanaged") && state != QLatin1String("unavailable");
        if (eligible) devices << name;
        else excluded.append(QJsonObject{{"device", name}, {"reason", "The interface is not in a supported managed/available Wi-Fi state."}});
    }
    if (!found) return fail("stale_target", "That interface is no longer present in NetworkManager.");
    if (devices.isEmpty()) return fail("unavailable", "No selected Wi-Fi interface is ready for a scan request.");
    if (devices.size() > 16) return fail("unsupported_configuration", "Select one interface when more than 16 Wi-Fi adapters are eligible.");
    devices.sort();
    plan.details = {{"devices", QJsonArray::fromStringList(devices)}, {"excluded", excluded}, {"persistentSettingChange", false},
                    {"verificationScope", "scan-request-acceptance-only"}, {"scanCompletionVerified", false}, {"accessPointAgeKnown", false},
                    {"interactiveAuthorizationRequested", false}, {"requestDeadlineSeconds", 20},
                    {"identityScope", "current NetworkManager interface names; not durable hardware tokens"}};
    plan.effects << "Requests a Wi-Fi scan; the radio may briefly be busy."
                 << "Does not change connection profiles or request credentials."
                 << "Acceptance does not verify scan completion or refreshed access-point data.";
    plan.ok = true;
    return plan;
}

void NetworkProvider::resolveScanPaths(const std::shared_ptr<ScanWork> &work, int index, ChangeCoordinator::Done done) {
    if (index == work->devices.size()) { work->code = "write_unavailable"; done(true, {}); return; }
    const int remaining = 10000 - int(work->deadline.elapsed());
    if (remaining <= 0) { work->code = "read_timeout"; done(false, "Wi-Fi interface resolution exceeded its deadline."); return; }
    CommandOptions options;
    options.timeoutMs = std::min(4000, remaining);
    options.maxOutputBytes = 65536;
    const auto command = MachineEnvironment + QStringList{"nmcli", "-c", "no", "-g", "GENERAL.DBUS-PATH", "device", "show", work->devices[index]};
    m_runner->run(command, options, this, [this, work, index, done](const CommandResult &result) {
        static const QRegularExpression path("^/org/freedesktop/NetworkManager/Devices/[A-Za-z0-9_]+$");
        const QString value = result.outText();
        if (!result.ok() || result.truncated || !path.match(value).hasMatch() || path.match(value).capturedLength() != value.size()) {
            work->code = "read_unavailable";
            done(false, "NetworkManager did not return a usable object path for " + work->devices[index] + ".");
            return;
        }
        work->paths << value;
        resolveScanPaths(work, index + 1, done);
    });
}

void NetworkProvider::sendScanRequests(const std::shared_ptr<ScanWork> &work, int index, ChangeCoordinator::Done done) {
    if (index == work->devices.size()) { done(work->error.isEmpty(), work->error); return; }
    const int remaining = 20000 - int(work->deadline.elapsed());
    if (remaining <= 0) {
        for (int pending = index; pending < work->devices.size(); ++pending)
            work->results << QVariantMap{{"device", work->devices[pending]}, {"requestAttempted", false}, {"requestAccepted", false}, {"error", "Request deadline exceeded."}};
        work->error = "Some Wi-Fi scan requests were not sent before the deadline.";
        work->code = "scan_request_unconfirmed";
        done(false, work->error);
        return;
    }
    CommandOptions options;
    options.timeoutMs = std::min(15000, remaining);
    options.maxOutputBytes = 65536;
    options.mutation = true;
    const auto command = MachineEnvironment + QStringList{"busctl", "--no-pager", "--allow-interactive-authorization=no", "--timeout=10s", "call",
        "org.freedesktop.NetworkManager", work->paths[index], "org.freedesktop.NetworkManager.Device.Wireless", "RequestScan", "a{sv}", "0"};
    work->attempted = true;
    m_runner->run(command, options, this, [this, work, index, done](const CommandResult &result) {
        const bool accepted = result.ok() && !result.truncated && result.outText().isEmpty();
        const QString error = accepted ? QString() : result.describe("busctl");
        work->results << QVariantMap{{"device", work->devices[index]}, {"requestAttempted", true},
            {"requestAccepted", accepted ? QVariant(true) : QVariant()}, {"error", accepted ? QString() : error.isEmpty() ? QString("The scan request was not confirmed.") : error}};
        if (!accepted) {
            work->code = "scan_request_unconfirmed";
            work->error = "NetworkManager did not confirm every scan request; inspect per-interface results before retrying.";
        }
        sendScanRequests(work, index + 1, done);
    });
}

void NetworkProvider::publishScan(const QVariantMap &result, const ScanFinished &finished) {
    m_scanning = false;
    m_scanResult = result;
    emit changed();
    emit message(result.value("ok").toBool() ? tr("Scan requested. Results may arrive later.") : result.value("error").toString(), !result.value("ok").toBool());
    if (finished) finished(result);
}

void NetworkProvider::requestScan(const QString &device, ScanFinished finished) {
    const auto blocked = [&](const QString &code, const QString &error) {
        return QVariantMap{{"ok", false}, {"outcome", "blocked"}, {"errorCode", code}, {"error", error},
            {"requestAttempted", false}, {"requestAccepted", false}, {"verified", false}, {"scanCompletionVerified", false}, {"accessPointAgeKnown", false}};
    };
    if (m_scanning) { if (finished) finished(blocked("busy", "A scan request is already pending.")); return; }
    if (m_radioBusy) { publishScan(blocked("busy", "A Wi-Fi switch change is pending."), finished); return; }
    if (!device.isEmpty() && !isScanDeviceName(device)) { publishScan(blocked("invalid_value", "Use a Wi-Fi interface from network devices."), finished); return; }
    if (!m_coordinator->leaseRun().isEmpty() || m_coordinator->leaseHeldElsewhere()) { publishScan(blocked("busy", "A profile restore is running."), finished); return; }
    const auto work = std::make_shared<ScanWork>();
    work->requested = device;
    m_scanning = true;
    m_scanResult.clear();
    emit changed();
    ChangeCoordinator::Request request;
    request.settingId = "network.scan.request";
    request.label = tr("Wi-Fi scan request");
    request.kind = "action";
    request.after = "scan-request-acceptance";
    request.extra = {{"requestedInterface", device}, {"verificationScope", "scan-request-acceptance-only"},
                     {"persistentSettingChange", false}, {"scanCompletionVerified", false}, {"accessPointAgeKnown", false}};
    request.preflight = [this, work](ChangeCoordinator::Done done) {
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection = connect(this, &NetworkProvider::refreshed, this, [this, work, done, connection] {
            disconnect(*connection);
            work->plan = scanPlan(work->requested, true);
            if (!work->plan.ok) { work->code = work->plan.errorCode; done(false, work->plan.error); return; }
            for (const auto &device : work->plan.details.value("devices").toArray()) work->devices << device.toString();
            work->deadline.start();
            resolveScanPaths(work, 0, done);
        });
        refreshScanState();
    };
    request.apply = [this, work](ChangeCoordinator::Done done) { work->deadline.restart(); sendScanRequests(work, 0, done); };
    request.verify = [work](ChangeCoordinator::Done done) {
        const bool all = work->results.size() == work->devices.size() && std::all_of(work->results.begin(), work->results.end(), [](const QVariant &result) {
            return result.toMap().value("requestAccepted").toBool();
        });
        done(all, "Scan request acceptance was not confirmed for every selected interface.");
    };
    request.settled = [work] { return "Request accepted: " + work->devices.join(", "); };
    m_coordinator->submit(std::move(request), [this, work, finished](const Transaction &tx) {
        const bool accepted = tx.phase == QLatin1String("committed");
        const bool anyAccepted = std::any_of(work->results.begin(), work->results.end(), [](const QVariant &result) { return result.toMap().value("requestAccepted").toBool(); });
        const QString outcome = accepted ? "scan_requested" : work->attempted ? anyAccepted ? "partial" : "unknown" : "blocked";
        QVariantMap result{{"ok", accepted}, {"outcome", outcome}, {"verified", false}, {"plan", work->plan.toJson().toVariantMap()},
            {"requestAttempted", work->attempted}, {"requestAccepted", accepted ? QVariant(true) : work->attempted ? QVariant() : QVariant(false)},
            {"results", work->results}, {"transaction", tx.toJson().toVariantMap()}, {"errorCode", accepted ? QString() : work->code},
            {"error", accepted ? QString() : tx.message}, {"scanCompletionVerified", false}, {"accessPointAgeKnown", false},
            {"verificationScope", "scan-request-acceptance-only"}, {"nextRead", QStringList{"network", "wifi"}}};
        publishScan(result, finished);
    });
}

void NetworkProvider::readRadio(std::function<void(const QVariantMap &)> finished) {
    CommandOptions options{4000};
    options.maxOutputBytes = 65536;
    m_runner->run(MachineEnvironment + QStringList{"busctl", "--no-pager", "--timeout=3s", "get-property", "org.freedesktop.NetworkManager",
        "/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager", "WirelessEnabled", "WirelessHardwareEnabled"}, options, this,
        [finished](const CommandResult &result) {
            const auto lines = result.outText().split('\n');
            const auto boolean = [](const QString &line) { return line == QLatin1String("b true") || line == QLatin1String("b false"); };
            const bool known = result.ok() && !result.truncated && lines.size() == 2 && boolean(lines[0]) && boolean(lines[1]);
            finished({{"status", known ? "ok" : "read_unavailable"}, {"enabled", known ? QVariant(lines[0] == QLatin1String("b true")) : QVariant()},
                {"hardwareEnabled", known ? QVariant(lines[1] == QLatin1String("b true")) : QVariant()},
                {"error", known ? QString() : QString("NetworkManager Wi-Fi switch and hardware state could not both be read.")}});
        });
}

void NetworkProvider::planWifiEnabled(bool on, std::function<void(const ProviderPlan &)> finished) {
    ProviderPlan plan;
    plan.operation = "network.setWifiEnabled";
    plan.target = "NetworkManager-Wi-Fi-switch";
    plan.after = on ? "true" : "false";
    auto fail = [plan, finished](const QString &code, const QString &error) mutable {
        plan.errorCode = code; plan.error = error; finished(plan);
    };
    const QString preview = NetworkPreview::pendingReason(m_runner->env());
    if (!preview.isEmpty()) { fail("busy", preview); return; }
    if (m_scanning || m_busy || !m_pending.isEmpty() || !m_checkpoint.isEmpty() || !m_coordinator->leaseRun().isEmpty() || m_coordinator->leaseHeldElsewhere()) {
        fail("busy", "Finish the current scan request, connection preview or profile restore before changing Wi-Fi.");
        return;
    }
    for (const auto &program : {"env", "nmcli", "busctl"}) {
        if (m_runner->env().findExecutable(program).isEmpty()) { fail("unavailable", "Wi-Fi changes require nmcli, env and busctl."); return; }
    }
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(this, &NetworkProvider::refreshed, this, [this, plan, finished, on, connection]() mutable {
        disconnect(*connection);
        if (m_states.value("devices").toMap().value("status") != QLatin1String("ok")) {
            plan.errorCode = "read_unavailable"; plan.error = "NetworkManager device inventory could not be read."; finished(plan); return;
        }
        QStringList devices;
        for (const auto &value : m_values.value("devices").toList()) {
            const auto row = value.toMap();
            if (row.value("type") == QLatin1String("wifi")) devices << row.value("device").toString();
        }
        devices.sort();
        if (devices.isEmpty()) { plan.errorCode = "unavailable"; plan.error = "NetworkManager reports no Wi-Fi adapter."; finished(plan); return; }
        readRadio([plan, devices, finished, on](const QVariantMap &state) mutable {
            if (state.value("status") != QLatin1String("ok")) { plan.errorCode = "read_unavailable"; plan.error = state.value("error").toString(); finished(plan); return; }
            plan.before = state.value("enabled").toBool() ? "true" : "false";
            plan.details = {{"devices", QJsonArray::fromStringList(devices)}, {"hardwareEnabled", state.value("hardwareEnabled").toBool()},
                {"verificationScope", "NetworkManager-radio-switch-only"}, {"connectionVerified", false}, {"internetReachabilityVerified", false},
                {"interactiveAuthorizationRequested", false}, {"persistence", "NetworkManager owns persistence; reboot behavior is not verified"},
                {"identityScope", "current NetworkManager Wi-Fi interface names; not durable hardware tokens"}};
            plan.effects << "Changes NetworkManager's global Wi-Fi switch, including all Wi-Fi adapters; other radio types are unchanged."
                         << "NetworkManager may reconnect saved networks automatically when Wi-Fi is enabled."
                         << "No checkpoint or automatic rollback; connection usability and internet access are not verified.";
            if (!on) plan.confirmText = "Turning off Wi-Fi disconnects Wi-Fi connections and may end this remote session. No automatic rollback is available.";
            if (on && !state.value("hardwareEnabled").toBool()) {
                plan.errorCode = "hardware_blocked"; plan.error = "The Wi-Fi hardware switch is off. Enable it before changing the NetworkManager Wi-Fi switch.";
            } else plan.ok = true;
            finished(plan);
        });
    });
    refreshSnapshot("devices");
}

void NetworkProvider::finishWifiEnabled(const Transaction &tx) {
    m_radioBusy = false;
    emit changed();
    if (tx.phase != QLatin1String("committed")) emit message(tx.message, true);
    emit operationFinished(tx);
}

void NetworkProvider::changeWifiEnabled(const ProviderPlan &plan, bool acknowledged) {
    if (m_radioBusy) {
        Transaction tx; tx.phase = "failed"; tx.message = "A Wi-Fi switch change is already pending.";
        emit operationFinished(tx); return;
    }
    m_radioBusy = true;
    m_radioReadback.clear();
    emit changed();
    submitWifiEnabled(plan, acknowledged);
}

void NetworkProvider::submitWifiEnabled(const ProviderPlan &plan, bool acknowledged) {
    const bool on = plan.after == QLatin1String("true");
    if (!plan.ok || plan.operation != QLatin1String("network.setWifiEnabled") || plan.target != QLatin1String("NetworkManager-Wi-Fi-switch")
        || !QStringList{"true", "false"}.contains(plan.after) || (!on && !acknowledged)) {
        Transaction tx; tx.phase = "failed"; tx.message = !plan.ok ? plan.error : "Review the Wi-Fi plan and acknowledge turning Wi-Fi off before applying it.";
        finishWifiEnabled(tx); return;
    }
    ChangeCoordinator::Request request;
    request.settingId = "network.wifi";
    request.label = tr("Wi-Fi");
    request.before = plan.before;
    request.after = plan.after;
    request.extra = plan.details;
    request.preflight = [this, plan, on](ChangeCoordinator::Done done) {
        planWifiEnabled(on, [plan, done](const ProviderPlan &current) {
            const bool same = current.ok && current.before == plan.before && current.details.value("hardwareEnabled") == plan.details.value("hardwareEnabled")
                && current.details.value("devices") == plan.details.value("devices");
            done(same, current.ok ? QString("The Wi-Fi switch or adapter inventory changed. Review a new plan before retrying.") : current.error);
        });
    };
    request.apply = [this, on](ChangeCoordinator::Done done) {
        CommandOptions options;
        options.mutation = true;
        options.timeoutMs = 15000;
        options.maxOutputBytes = 65536;
        m_runner->run(MachineEnvironment + QStringList{"busctl", "--no-pager", "--allow-interactive-authorization=no", "--timeout=10s", "call",
            "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager", "org.freedesktop.DBus.Properties", "Set", "ssv",
            "org.freedesktop.NetworkManager", "WirelessEnabled", "b", on ? "true" : "false"}, options, this, [done](const CommandResult &result) {
                const bool ok = result.ok() && !result.truncated && result.outText().isEmpty();
                done(ok, ok ? QString() : result.describe("busctl").isEmpty() ? QString("The Wi-Fi switch write was not confirmed.") : result.describe("busctl"));
            });
    };
    request.verify = [this, on](ChangeCoordinator::Done done) {
        readRadio([this, on, done](const QVariantMap &state) {
            m_radioReadback = state;
            const bool verified = state.value("status") == QLatin1String("ok") && state.value("enabled").toBool() == on;
            done(verified, "NetworkManager did not confirm the requested Wi-Fi switch state. Connection usability is not verified.");
        });
    };
    m_coordinator->submit(std::move(request), [this](const Transaction &tx) { finishWifiEnabled(tx); });
}

void NetworkProvider::setWifiEnabled(bool on) {
    if (m_radioBusy) { emit message("A Wi-Fi switch change is already pending.", true); return; }
    m_radioBusy = true;
    m_radioReadback.clear();
    emit changed();
    // Refresh is GUI-only: the CLI must finish without queued desktop reads.
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(this, &NetworkProvider::operationFinished, this, [this, connection](const Transaction &) {
        disconnect(*connection); refresh();
    });
    planWifiEnabled(on, [this](const ProviderPlan &plan) { submitWifiEnabled(plan, true); });
}

bool NetworkProvider::guardLegacyConnection() {
    QString reason = NetworkPreview::pendingReason(m_runner->env());
    if (reason.isEmpty() && (!m_coordinator->leaseRun().isEmpty() || m_coordinator->leaseHeldElsewhere())) reason = "A profile restore is running.";
    if (reason.isEmpty() && m_connectionLock) reason = "Finish the current connection change first.";
    if (!reason.isEmpty()) { emit message(reason, true); return false; }
    auto lock = std::make_unique<QLockFile>(m_runner->env().appStateDir + "/change.lock");
    lock->setStaleLockTime(5 * 60 * 1000);
    if (!lock->tryLock(0)) { emit message("Another Control operation is changing state. Try again after it finishes.", true); return false; }
    reason = DisplayPreview::pendingReason(m_runner->env());
    if (reason.isEmpty()) reason = NetworkPreview::pendingReason(m_runner->env());
    if (!reason.isEmpty()) { emit message(reason, true); return false; }
    m_connectionLock = std::move(lock);
    return true;
}

void NetworkProvider::activate(const QString &uuid, const QString &name) {
    static const QRegularExpression valid("^[0-9a-fA-F-]{36}$");
    if (!valid.match(uuid).hasMatch()) {
        emit message(tr("That connection is no longer available."), true);
        return;
    }
    // NetworkManager refuses overlapping checkpoints, and the old one's rollback would
    // still fire underneath the new connection.
    if (m_busy || m_radioBusy || !m_checkpoint.isEmpty()) {
        emit message(tr("Finish with the current connection change first."), true);
        return;
    }
    if (!guardLegacyConnection()) return;
    m_busy = true;
    m_pending.insert(name, "connect");
    m_failures.remove(name);
    emit changed();
    auto start = [this, uuid, name] {
        CommandOptions options;
        options.mutation = true;
        options.timeoutMs = 30000;
        m_runner->run({"nmcli", "connection", "up", "uuid", uuid}, options, this, [this, uuid, name](const CommandResult &result) {
            auto fail = [this, name](const QString &text) {
                m_busy = false;
                m_pending.remove(name);
                m_failures.insert(name, text);
                emit changed();
                emit message(text, true);
                if (!m_checkpoint.isEmpty())
                    revertConnection();
                else m_connectionLock.reset();
                refresh();
            };
            if (!result.ok()) {
                fail(tr("Could not connect to %1: %2").arg(name, result.describe("nmcli")));
                return;
            }
            // nmcli returning is not the connection being up: NetworkManager has to list
            // the profile as active before this says so.
            m_runner->run({"nmcli", "-t", "-f", "NAME,UUID,TYPE,ACTIVE", "connection", "show"}, CommandOptions{4000}, this, [this, uuid, name, fail](const CommandResult &check) {
                bool active = false;
                for (const auto &row : parseSaved(check.out))
                    if (row.toMap().value("uuid").toString() == uuid)
                        active = row.toMap().value("active").toBool();
                if (!active) {
                    fail(tr("%1 did not become the active connection.").arg(name));
                    return;
                }
                m_busy = false;
                m_pending.remove(name);
                emit changed();
                connected(name);
            });
        });
    };
    // A checkpoint is protection, not a prerequisite: where polkit declines one,
    // the switch still goes ahead, as nmcli alone would.
    const qint64 created = QDateTime::currentMSecsSinceEpoch();
    m_runner->run(ManagerCall + QStringList{"CheckpointCreate", "aouu", "0", QString::number(CheckpointSeconds), "0"}, CommandOptions{5000}, this, [this, start, created](const CommandResult &result) {
        static const QRegularExpression path("\"(/org/freedesktop/NetworkManager/Checkpoint/[0-9]+)\"");
        const auto match = path.match(QString::fromUtf8(result.out));
        m_checkpoint = result.ok() && match.hasMatch() ? match.captured(1) : QString();
        m_checkpointDeadline = created + CheckpointSeconds * 1000LL;
        start();
    });
}

void NetworkProvider::connected(const QString &name) {
    if (m_checkpoint.isEmpty()) {
        m_connectionLock.reset();
        emit message(tr("Connected to %1").arg(name), false);
    } else {
        // The rollback timer started with the checkpoint, not with this line, so the
        // countdown shows what is really left.
        m_keepSeconds = qMax<qint64>(1, (m_checkpointDeadline - QDateTime::currentMSecsSinceEpoch()) / 1000);
        m_keepTimer.start();
        emit keepChanged();
    }
    refresh();
}

void NetworkProvider::keepConnection() {
    if (!m_ownedToken.isEmpty()) {
        if (!canKeep()) { emit message(tr("Only a current confirmed preview can be kept. Inspect the connection first."), true); return; }
        ownedCommand("keep"); return;
    }
    m_keepTimer.stop();
    m_keepSeconds = 0;
    const QString checkpoint = m_checkpoint;
    m_checkpoint.clear();
    emit keepChanged();
    if (checkpoint.isEmpty())
        return;
    CommandOptions options;
    options.mutation = true;
    m_runner->run(ManagerCall + QStringList{"CheckpointDestroy", "o", checkpoint}, options, this, [this](const CommandResult &result) {
        m_connectionLock.reset();
        // If this fails, the checkpoint has probably already rolled back, so saying
        // "kept" would be false.
        if (result.ok())
            emit message(tr("Connection kept"), false);
        else
            emit message(tr("The connection may already have gone back to the previous one. Check it below."), true);
        refresh();
    });
}

void NetworkProvider::revertConnection() {
    if (!m_ownedToken.isEmpty()) { ownedCommand("revert"); return; }
    m_keepTimer.stop();
    m_keepSeconds = 0;
    const QString checkpoint = m_checkpoint;
    m_checkpoint.clear();
    emit keepChanged();
    if (checkpoint.isEmpty())
        return;
    CommandOptions options;
    options.mutation = true;
    options.timeoutMs = 20000;
    m_runner->run(ManagerCall + QStringList{"CheckpointRollback", "o", checkpoint}, options, this, [this](const CommandResult &result) {
        m_connectionLock.reset();
        if (!result.ok())
            emit message(tr("Restoring the previous connection failed: %1").arg(result.describe("NetworkManager")), true);
        else
            emit message(tr("The previous connection was restored."), false);
        refresh();
    });
}

void NetworkProvider::deactivate(const QString &device) {
    if (m_radioBusy) { emit message("Finish the Wi-Fi switch change before disconnecting a device.", true); return; }
    static const QRegularExpression valid("^[A-Za-z0-9._-]+$");
    if (!valid.match(device).hasMatch())
        return;
    if (!guardLegacyConnection()) return;
    CommandOptions options;
    options.mutation = true;
    m_pending.insert(device, "disconnect");
    m_failures.remove(device);
    emit changed();
    m_runner->run({"nmcli", "device", "disconnect", device}, options, this, [this, device](const CommandResult &result) {
        auto finish = [this, device](const QString &failure) {
            m_connectionLock.reset();
            m_pending.remove(device);
            if (!failure.isEmpty()) {
                m_failures.insert(device, failure);
                emit message(failure, true);
            }
            emit changed();
            refresh();
        };
        if (!result.ok()) {
            finish(result.describe("nmcli"));
            return;
        }
        // Disconnected is what NetworkManager reports afterwards, not what the command said.
        m_runner->run({"nmcli", "-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device"}, CommandOptions{4000}, this, [this, device, finish](const CommandResult &check) {
            bool still = false;
            for (const auto &row : parseDevices(check.out))
                if (row.toMap().value("device").toString() == device)
                    still = row.toMap().value("connected").toBool();
            finish(still ? tr("%1 is still connected.").arg(device) : QString());
        });
    });
}
