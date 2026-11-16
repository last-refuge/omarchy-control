#include "bluetoothprovider.h"

#include <QRegularExpression>
#include <QTimer>

#include "compat.h"

BluetoothProvider::BluetoothProvider(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *parent)
    : QObject(parent), m_runner(runner), m_coordinator(coordinator) {}

bool BluetoothProvider::isValidAddress(const QString &address) {
    static const QRegularExpression valid("^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$");
    return valid.match(address).hasMatch();
}

QVariantMap BluetoothProvider::parseInfo(const QByteArray &text, const QString &address) {
    QVariantMap device;
    device["address"] = address;
    device["connected"] = false;
    device["paired"] = false;
    device["battery"] = -1;
    for (const auto &raw : QString::fromUtf8(text).split('\n')) {
        const QString line = raw.trimmed();
        const int colon = line.indexOf(':');
        if (colon < 0)
            continue;
        const QString key = line.left(colon);
        const QString value = line.mid(colon + 1).trimmed();
        if (key == QLatin1String("Name") || (key == QLatin1String("Alias") && !device.contains("name")))
            device["name"] = value;
        else if (key == QLatin1String("Connected")) {
            device["connected"] = value == QLatin1String("yes");
            device["connectedKnown"] = value == QLatin1String("yes") || value == QLatin1String("no");
        }
        else if (key == QLatin1String("Paired")) {
            device["paired"] = value == QLatin1String("yes");
            device["pairedKnown"] = value == QLatin1String("yes") || value == QLatin1String("no");
        }
        else if (key == QLatin1String("Icon"))
            device["kind"] = value;
        else if (key == QLatin1String("Battery Percentage")) {
            static const QRegularExpression percent(R"(\((\d+)\))");
            const auto match = percent.match(value);
            if (match.hasMatch())
                device["battery"] = match.captured(1).toInt();
        }
    }
    if (!device.contains("name"))
        device["name"] = address;
    return device;
}

static bool addresses(const CommandResult &result, const QString &prefix, QStringList *out) {
    if (!result.ok() || result.truncated)
        return false;
    const QRegularExpression linePattern("^" + prefix + " ([0-9A-Fa-f:]{17})(?: .*)?$");
    for (const QString &line : result.outText().split('\n', Qt::SkipEmptyParts)) {
        const auto match = linePattern.match(line.trimmed());
        if (!match.hasMatch() || !BluetoothProvider::isValidAddress(match.captured(1)))
            return false;
        const QString address = match.captured(1).toUpper();
        if (!out->contains(address))
            *out << address;
    }
    return out->size() <= 256;
}

void BluetoothProvider::readPower(std::function<void(bool, bool, bool)> done) {
    // Omarchy's is-on returns 1 both when every radio is off and when its
    // controller probes failed. Read every controller explicitly to tell them apart.
    m_runner->run({"bluetoothctl", "list"}, CommandOptions{5000}, this, [this, done](const CommandResult &list) {
        QStringList controllers;
        if (!addresses(list, "Controller", &controllers) || controllers.isEmpty() || controllers.size() > 32) {
            done(false, false, false);
            return;
        }
        auto remaining = std::make_shared<int>(controllers.size());
        auto known = std::make_shared<bool>(true);
        auto powered = std::make_shared<bool>(false);
        auto allPowered = std::make_shared<bool>(true);
        for (const auto &controller : controllers) {
            m_runner->run({"bluetoothctl", "show", controller}, CommandOptions{5000}, this,
                          [remaining, known, powered, allPowered, done](const CommandResult &r) {
                static const QRegularExpression field("(?:^|\\n)\\s*Powered: (yes|no)\\s*(?:\\n|$)");
                const auto match = field.match(r.outText());
                *known = *known && r.ok() && !r.truncated && match.hasMatch();
                *powered = *powered || match.captured(1) == QLatin1String("yes");
                *allPowered = *allPowered && match.captured(1) == QLatin1String("yes");
                if (--*remaining == 0)
                    done(*known, *powered, *allPowered);
            });
        }
    });
}

void BluetoothProvider::refresh() {
    const int generation = ++m_generation;
    m_status = "loading";
    auto fail = [this, generation] {
        if (generation != m_generation)
            return;
        m_status = "unavailable";
        m_devices.clear();
        emit changed();
        emit refreshed();
    };
    readPower([this, generation, fail](bool known, bool powered, bool allPowered) {
        if (generation != m_generation)
            return;
        if (!known) {
            fail();
            return;
        }
        m_powered = powered;
        m_allRadiosPowered = allPowered;
        m_runner->run({"bluetoothctl", "devices"}, CommandOptions{5000}, this, [this, generation, fail](const CommandResult &list) {
            if (generation != m_generation)
                return;
            QStringList devices;
            if (!addresses(list, "Device", &devices)) {
                fail();
                return;
            }
            if (devices.isEmpty()) {
                m_status = "ok";
                m_devices.clear();
                emit changed();
                emit refreshed();
                return;
            }
            auto results = std::make_shared<QHash<QString, QVariantMap>>();
            auto remaining = std::make_shared<int>(devices.size());
            auto valid = std::make_shared<bool>(true);
            for (const auto &address : devices) {
                m_runner->run({"bluetoothctl", "info", address}, CommandOptions{5000}, this,
                              [this, generation, address, devices, results, remaining, valid, fail](const CommandResult &info) {
                    const QVariantMap row = parseInfo(info.out, address);
                    *valid = *valid && info.ok() && !info.truncated && row.value("connectedKnown").toBool() && row.value("pairedKnown").toBool();
                    results->insert(address, row);
                    if (--*remaining || generation != m_generation)
                        return;
                    if (!*valid) {
                        fail();
                        return;
                    }
                    m_devices.clear();
                    for (const auto &a : devices)
                        m_devices << results->value(a);
                    for (const auto &key : m_failures.keys())
                        if (!devices.contains(key))
                            m_failures.remove(key);
                    m_status = "ok";
                    emit changed();
                    emit refreshed();
                });
            }
        });
    });
}

ProviderPlan BluetoothProvider::plan(const QString &action, const QString &target) const {
    ProviderPlan p;
    p.operation = action == QLatin1String("power") ? QStringLiteral("bluetooth.setPowered") : "bluetooth." + action + "Device";
    p.target = action == QLatin1String("power") ? QStringLiteral("all-radios") : target.toUpper();
    const auto fail = [&](const QString &code, const QString &error) { p.errorCode = code; p.error = error; return p; };
    if (!QStringList{"power", "connect", "disconnect", "forget"}.contains(action))
        return fail("invalid_arguments", "Unknown Bluetooth operation; pairing requires the Omarchy Bluetooth panel.");
    if (action == QLatin1String("power") && target != QLatin1String("true") && target != QLatin1String("false"))
        return fail("invalid_value", "Use true or false for radio power.");
    if (action != QLatin1String("power") && !isValidAddress(target))
        return fail("invalid_value", tr("That is not a valid device address."));
    if (m_status != QLatin1String("ok"))
        return fail("read_unavailable", tr("Bluetooth state could not be read. Refresh before changing it."));
    p.details = {{"action", action}, {"scope", action == QLatin1String("power") ? "system-radios" : "known-device"}};
    if (action == QLatin1String("power")) {
        p.before = m_powered ? "true" : "false";
        p.after = target;
        p.details["allRadiosPowered"] = m_allRadiosPowered;
        p.effects << "Changes every Bluetooth radio through Omarchy and remembers the rfkill choice across reboots.";
        if (target == QLatin1String("false"))
            p.confirmText = "Turning off Bluetooth disconnects Bluetooth audio and input devices, including keyboards and mice.";
    } else {
        QVariantMap device;
        for (const auto &row : m_devices)
            if (row.toMap().value("address").toString() == p.target)
                device = row.toMap();
        if (device.isEmpty())
            return fail("stale_target", tr("That Bluetooth device is no longer available."));
        if (action == QLatin1String("connect") && !device.value("paired").toBool())
            return fail("interaction_required", "Pair this device in the Omarchy Bluetooth panel before connecting it.");
        p.before = action == QLatin1String("forget") ? "known" : device.value("connected").toBool() ? "connected" : "disconnected";
        p.after = action == QLatin1String("forget") ? "forgotten" : action == QLatin1String("connect") ? "connected" : "disconnected";
        p.effects << "Changes this known Bluetooth device through Omarchy; no password or pairing PIN is collected.";
        if (action != QLatin1String("disconnect"))
            p.effects << "Omarchy may power Bluetooth on; connect also trusts the paired device.";
        if (action == QLatin1String("forget"))
            p.confirmText = "Forgetting this device removes its pairing and disconnects it. Reconnecting will require pairing again.";
    }
    const QString changed = Compat(m_runner->env()).changedForKey(action == QLatin1String("power") ? "bluetooth.power" : "bluetooth.device");
    if (!changed.isEmpty())
        return fail("compatibility_blocked", QStringLiteral("Omarchy's %1 changed since review; this operation is paused.").arg(changed));
    p.ok = true;
    return p;
}

void BluetoothProvider::change(const QString &action, const QString &target) {
    const ProviderPlan p = plan(action, target);
    if (!p.ok) {
        Transaction tx;
        tx.phase = "failed";
        tx.message = p.error;
        emit message(p.error, true);
        emit operationFinished(tx);
        return;
    }
    ChangeCoordinator::Request request;
    request.settingId = action == QLatin1String("power") ? "bluetooth.power" : "bluetooth.device";
    request.label = action == QLatin1String("power") ? tr("Bluetooth") : tr("Bluetooth device");
    request.before = p.before;
    request.after = p.after;
    request.extra = p.details;
    request.extra["target"] = p.target;
    request.apply = [this, action, target, p](ChangeCoordinator::Done done) {
        CommandOptions options;
        options.mutation = true;
        options.timeoutMs = 45000;
        const QStringList argv = action == QLatin1String("power")
            ? QStringList{"omarchy", "bluetooth", "power", target == QLatin1String("true") ? "on" : "off"}
            : QStringList{"omarchy", "bluetooth", "device", action, p.target};
        auto write = [this, argv, options, done] {
            m_runner->run(argv, options, this, [done](const CommandResult &r) { done(r.ok(), r.describe("Bluetooth")); });
        };
        if (action == QLatin1String("power")) {
            write();
        } else {
            m_runner->run({"bluetoothctl", "info", p.target}, CommandOptions{5000}, this, [this, action, p, write, done](const CommandResult &r) {
                const auto current = parseInfo(r.out, p.target);
                if (r.ok() && !r.truncated && current.value("connectedKnown").toBool() && current.value("pairedKnown").toBool()
                    && (action != QLatin1String("connect") || current.value("paired").toBool()))
                    write();
                else
                    done(false, tr("That Bluetooth device changed or could not be read; refresh before changing it."));
            });
        }
    };
    request.verify = [this, action, target, p](ChangeCoordinator::Done done) {
        if (action == QLatin1String("power")) {
            readPower([this, target, done](bool known, bool anyPowered, bool allPowered) {
                done(known && (target == QLatin1String("true") ? allPowered : !anyPowered), tr("Bluetooth did not change state."));
            });
        } else if (action == QLatin1String("forget")) {
            m_runner->run({"bluetoothctl", "devices"}, CommandOptions{5000}, this, [this, p, done](const CommandResult &r) {
                QStringList devices;
                done(addresses(r, "Device", &devices) && !devices.contains(p.target), tr("The device's removal could not be confirmed."));
            });
        } else {
            m_runner->run({"bluetoothctl", "info", p.target}, CommandOptions{5000}, this, [this, p, action, done](const CommandResult &r) {
                const QVariantMap device = parseInfo(r.out, p.target);
                done(r.ok() && !r.truncated && device.value("connectedKnown").toBool()
                         && device.value("connected").toBool() == (action == QLatin1String("connect")),
                     tr("The device did not respond. Check that it is on and in range."));
            });
        }
    };
    m_busy = true;
    m_pending.insert(p.target, action);
    m_failures.remove(p.target);
    emit changed();
    m_coordinator->submit(std::move(request), [this, p, action](const Transaction &tx) {
        m_busy = false;
        m_pending.remove(p.target);
        const bool ok = tx.phase == QLatin1String("committed");
        if (!ok)
            m_failures.insert(p.target, tx.message);
        const QString success = action == QLatin1String("connect") ? tr("Connected") : action == QLatin1String("disconnect")
            ? tr("Disconnected") : action == QLatin1String("forget") ? tr("Device forgotten") : tr("Bluetooth change confirmed");
        emit message(ok ? success : tx.message, !ok);
        emit operationFinished(tx);
        refresh();
    });
}

void BluetoothProvider::setPowered(bool on) { change("power", on ? "true" : "false"); }
void BluetoothProvider::connectDevice(const QString &address) { change("connect", address); }
void BluetoothProvider::disconnectDevice(const QString &address) { change("disconnect", address); }
void BluetoothProvider::forgetDevice(const QString &address) { change("forget", address); }
