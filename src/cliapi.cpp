#include "cliapi.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <algorithm>

#include "appcore.h"
#include "cli.h"
#include "audioprovider.h"
#include "bluetoothprovider.h"
#include "networkprovider.h"
#include "networkpreview.h"
#include "displayprovider.h"
#include "displaypreview.h"
#include "hypromodule.h"
#include <QDateTime>
#include "servicesprovider.h"
#include "systemprovider.h"
#include "shortcutsprovider.h"
#include "windowrulesprovider.h"
#include "hyprchange.h"
#include "pluginsprovider.h"
#include "skillintegration.h"
#include "preferences.h"

namespace {

struct Command {
    const char *name;
    const char *usage;
    int minimum;
    int maximum;
    bool mutation = false;
};

const QList<Command> &commands() {
    static const QList<Command> list{
        {"commands", "commands", 0, 0},
        {"catalog", "catalog", 0, 0},
        {"describe", "describe <setting-or-operation-id> [--live]", 1, 1},
        {"search", "search <query>", 1, 1},
        {"get", "get <setting-id>", 1, 1},
        {"plan", "plan <setting-id> <value>", 2, 2},
        {"apply", "apply <setting-id> <value> [--yes]", 2, 2, true},
        {"reset", "reset <setting-id> [--yes]", 1, 1, true},
        {"history", "history", 0, 0},
        {"undo", "undo <transaction-id> [--yes]", 1, 1, true},
        {"audio", "audio status | <output|input> default <name> | <output|input> <volume|mute> <name> <value> | stream move <index@identity> <output> [--dry-run]", 1, 4, true},
        {"bluetooth", "bluetooth status | power <true|false> | <connect|disconnect|forget|pair> <address> [--dry-run] [--yes]", 1, 2, true},
        {"network", "network <status|general|devices|saved|wifi> | rescan [interface] | radio <true|false> | activate <uuid> <interface> | disconnect <interface> | preview status [token] | preview keep|revert <token>; inspect describe for options", 1, 3, true},
        {"preferences", "preferences pins | pin|unpin <catalog-id> | crash-mutes | crash-mute <program> [true|false] [--dry-run]", 1, 3, true},
        {"profile", "profile cancel <run-id> [--dry-run] (other profile commands retain their legacy interface)", 2, 2, true},
        {"display", "display status | layout <json-file> | check | preview status [token] | preview keep|revert <token> [--dry-run] [--yes]", 1, 3, true},
        {"services", "services <user|system> status | <start|stop|restart|enable|disable> <unit> [--dry-run] [--yes]", 2, 3, true},
        {"startup", "startup list | <enable|disable> <desktop-id> [--dry-run]", 1, 2, true},
        {"apps", "apps list | launch <desktop-id> [--dry-run] (apps launcher retains its legacy API)", 1, 2, true},
        {"icons", "icons list | samples <pack-id> | search <pack-id> [query] [--limit <1..300>] (mutations retain their legacy API)", 1, 3},
        {"mime", "mime list | get <type> | set <type> <desktop-id> [--dry-run]", 1, 3, true},
        {"system", "system <status|machine|battery|storage|hooks>", 1, 1},
        {"diagnostics", "diagnostics export [--dry-run]", 1, 1, true},
        {"shortcut", "shortcut list | check <keys> | add <keys> <description> <command> [--replace] | remove <selector> [--dry-run] [--yes]", 1, 4, true},
        {"rule", "rule types | list | validate <match> <action> [value] | add <match> <action> [value] | remove <selector> [--dry-run]", 1, 4, true},
        {"plugin", "plugin list | status | <enable|disable|clone|remove> <id> | recover <token> [--dry-run] [--yes]", 1, 2, true},
        {"bar", "bar status", 1, 1},
        {"widget", "widget move <selector> <section> <index> | option <selector> showPercentage <true|false> [--dry-run]", 4, 4, true},
        {"dock", "dock status | <pin|unpin> <desktop-id> [--dry-run]", 1, 2, true},
        {"skill", "skill <status|plan|install|reconcile|remove|opt-out|opt-in> [--target <absolute-skills-directory>]... [--dry-run]", 1, 1, true},
        {"coverage", "coverage --cli", 0, 0},
        {"help", "help", 0, 0},
        {"--version", "--version", 0, 0},
    };
    return list;
}

QJsonArray strings(const QStringList &values) {
    return QJsonArray::fromStringList(values);
}

QJsonArray operations() {
    QFile file(QStringLiteral(":/data/cli-operations.json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).array();
}

CliApi::Response result(const QString &operation, const QJsonValue &data = QJsonObject{},
                        const QString &outcome = QStringLiteral("completed"), const QString &code = {},
                        const QString &message = {}, bool verified = false) {
    const bool ok = code.isEmpty();
    return {ok ? 0 : 1,
            {{"apiVersion", 1}, {"appVersion", QCoreApplication::applicationVersion()},
             {"operation", operation}, {"ok", ok}, {"outcome", outcome}, {"verified", verified},
             {"data", data}, {"error", ok ? QJsonValue(QJsonValue::Null)
                                          : QJsonValue(QJsonObject{{"code", code}, {"message", message}})}}};
}

CliApi::Response failure(const QString &operation, const QString &code, const QString &message,
                         const QJsonValue &data = QJsonObject{}) {
    return result(operation, data, QStringLiteral("blocked"), code, message);
}

struct Arguments {
    QStringList positional;
    bool yes = false;
    bool live = false;
    bool cli = false;
    bool help = false;
    bool dryRun = false;
    bool replace = false;
    bool hasLimit = false;
    int limit = 300;
    QStringList targets;
    QString error;
    QString errorCode = QStringLiteral("invalid_arguments");
};

Arguments parse(const QStringList &input) {
    Arguments args;
    QSet<QString> seen;
    bool literal = false;
    for (int i = 0; i < input.size(); ++i) {
        const QString part = input[i];
        if (!literal && part == QLatin1String("--")) {
            literal = true;
            continue;
        }
        bool number = false;
        part.toDouble(&number);
        if (literal || !part.startsWith('-') || number) {
            args.positional << part;
            continue;
        }
        const QString flag = part.startsWith(QLatin1String("--api-version=")) ? QStringLiteral("--api-version") : part;
        if (seen.contains(flag) && flag != QLatin1String("--target")) {
            args.error = QStringLiteral("Repeated option: %1").arg(flag);
            return args;
        }
        seen.insert(flag);
        if (flag == QLatin1String("--api-version")) {
            QString version;
            if (part.contains('='))
                version = part.mid(part.indexOf('=') + 1);
            else if (i + 1 < input.size() && !input[i + 1].startsWith('-'))
                version = input[++i];
            if (version.isEmpty()) {
                args.error = QStringLiteral("--api-version requires a version number.");
                return args;
            }
            if (version != QLatin1String("1")) {
                args.errorCode = QStringLiteral("unsupported_api_version");
                args.error = QStringLiteral("Supported API versions: 1.");
                return args;
            }
        } else if (flag == QLatin1String("--limit")) {
            bool ok = false;
            const QString value = i + 1 < input.size() ? input[++i] : QString();
            args.limit = value.toInt(&ok);
            if (!ok || !QRegularExpression(QStringLiteral("^[0-9]+$")).match(value).hasMatch() || args.limit < 1 || args.limit > 300) {
                args.error = QStringLiteral("--limit requires an integer from 1 to 300.");
                return args;
            }
            args.hasLimit = true;
        } else if (flag == QLatin1String("--target")) {
            if (i + 1 >= input.size() || input[i + 1].startsWith('-')) {
                args.error = QStringLiteral("--target requires an absolute skills-directory path.");
                return args;
            }
            args.targets << input[++i];
        } else if (flag == QLatin1String("--json")) {
            // This API always returns JSON, including option/grammar errors.
        } else if (flag == QLatin1String("--yes")) {
            args.yes = true;
        } else if (flag == QLatin1String("--live")) {
            args.live = true;
        } else if (flag == QLatin1String("--dry-run")) {
            args.dryRun = true;
        } else if (flag == QLatin1String("--replace")) {
            args.replace = true;
        } else if (flag == QLatin1String("--cli")) {
            args.cli = true;
        } else if (flag == QLatin1String("--help") || flag == QLatin1String("-h")) {
            args.help = true;
        } else if (flag == QLatin1String("--version") && args.positional.isEmpty()) {
            args.positional << flag;
        } else {
            args.error = QStringLiteral("Unknown option: %1").arg(part);
            return args;
        }
    }
    return args;
}

template<typename T, typename Start>
T awaitResult(Start start) {
    QEventLoop loop;
    T value;
    bool finished = false;
    start([&](const T &read) {
        value = read;
        finished = true;
        loop.quit();
    });
    if (!finished)
        loop.exec();
    return value;
}

QJsonObject readValue(AppCore &core, const CatalogEntry &entry, const QVariantList &choices, ReadResult *state) {
    *state = awaitResult<ReadResult>([&](auto done) { core.engine.read(entry.id, done); });
    QJsonObject data{{"id", entry.id}, {"status", state->status}, {"value", state->value},
                     {"display", core.engine.displayValue(entry, state->value, choices)},
                     {"source", state->source}, {"reason", state->reason}, {"isOverride", state->isOverride},
                     {"choices", QJsonArray::fromVariantList(choices)},
                     {"details", QJsonObject::fromVariantMap(state->extra)}};
    return data;
}

QJsonArray invocations(const CatalogEntry &entry) {
    QJsonArray calls;
    const auto add = [&](const QStringList &argv) { calls.append(strings(argv)); };
    if (entry.isSetting()) {
        add({"get", entry.id});
        if (entry.isEditable()) {
            add({"plan", entry.id, "<value>"});
            add({"apply", entry.id, "<value>"});
            const QString writer = entry.write.value("kind").toString();
            if (entry.owned || !entry.reset.isEmpty() || QStringList{"json", "icons"}.contains(writer))
                add({"reset", entry.id});
        }
    } else if (entry.launch.value("kind") != QLatin1String("page")) {
        // Handoffs retain their existing CLI. They are not API v1 mutations.
        add({"launch", entry.id});
    }
    return calls;
}

QJsonArray pageOperations(const CatalogEntry &entry) {
    const QHash<QString, QStringList> mapping{
        {"block.output", {"audio.inspect", "audio.setDefaultOutput", "audio.setVolume", "audio.setMute"}},
        {"block.microphone", {"audio.inspect", "audio.setDefaultInput", "audio.setInputVolume", "audio.setInputMute"}},
        {"block.appAudio", {"audio.inspect", "audio.moveStream"}}, {"block.bluetooth", {"bluetooth.inspect", "bluetooth.connectDevice", "bluetooth.disconnectDevice", "bluetooth.setPowered"}},
        {"block.wifi", {"network.refresh", "network.activate", "network.deactivate", "network.rescan", "network.setWifiEnabled"}},
        {"block.displays", {"display.inspect", "display.layout", "display.recovery-check"}},
        {"block.battery", {"system.inspect", "widgets.option"}}, {"block.bar", {"plugins.inspect", "widgets.move", "widgets.option", "dock.pin"}},
        {"block.plugins", {"plugins.inspect", "plugins.enabled", "plugins.clone", "plugins.remove", "plugins.recover"}},
        {"block.shortcuts", {"shortcuts.inspect", "shortcuts.add", "shortcuts.remove"}},
        {"block.rules", {"rules.inspect", "rules.add", "rules.remove"}},
        {"block.installedApps", {"apps.inspect", "launchers.inspect", "launchers.set", "launchers.reset", "launchers.update"}},
        {"block.associations", {"mime.set"}}, {"block.storage", {"system.inspect"}},
        {"block.startup", {"startup.enabled"}},
        {"block.services", {"services.inspect", "services.action"}}, {"block.history", {"backend.undo"}},
        {"block.diagnostics", {"diagnostics.export"}}, {"block.hooks", {"system.inspect"}},
        {"block.about", {"updates.check", "updates.install"}}, {"block.myOmarchy", {"profile.status", "profile.capture", "profile.plan", "profile.apply"}},
        {"appearance.icons", {"icons.inspect", "icons.import", "backend.icons.remove"}}
    };
    QJsonArray ids;
    for (const auto &id : mapping.value(entry.launch.value("focus").toString())) ids.append(id);
    return ids;
}

QString routeClass(const CatalogEntry &entry) {
    if (entry.isSetting()) return "typed-setting";
    const QString kind = entry.launch.value("kind").toString();
    if (kind == "page") return "native-page";
    if (kind == "config") return "advanced-config";
    if (kind == "url") return "documentation";
    if (kind == "app" || kind == "panel") return "interactive-handoff";
    return "external-workflow";
}

template<typename Provider, typename Start, typename Finished>
bool refreshProvider(Provider &provider, Start start, Finished signal) {
    QEventLoop loop;
    bool complete = false;
    QObject::connect(&provider, signal, &loop, [&] { complete = true; loop.quit(); });
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    start();
    if (!complete)
        loop.exec();
    return complete;
}

template<typename Provider, typename Start>
bool refreshProvider(Provider &provider, Start start) {
    return refreshProvider(provider, start, &Provider::refreshed);
}

template<typename Provider>
bool refreshProvider(Provider &provider) {
    return refreshProvider(provider, [&] { provider.refresh(); });
}

template<typename Provider, typename Start>
CliApi::Response changeProvider(Provider &provider, AppCore &core, const ProviderPlan &plan, bool dryRun, bool yes, Start start) {
    if (!plan.ok)
        return failure(plan.operation, plan.errorCode, plan.error, plan.toJson());
    if (dryRun)
        return result(plan.operation, plan.toJson(), "planned");
    if (!plan.confirmText.isEmpty() && !yes)
        return failure(plan.operation, "confirmation_required", plan.confirmText, plan.toJson());
    const auto recovered = core.coordinator.recoverInterrupted();
    QEventLoop loop;
    Transaction tx;
    bool finished = false;
    QObject::connect(&provider, &Provider::operationFinished, &loop, [&](const Transaction &value) {
        tx = value;
        finished = true;
        loop.quit();
    });
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    start();
    if (!finished)
        loop.exec();
    QJsonObject data{{"plan", plan.toJson()}, {"transaction", tx.toJson()}, {"recoveryMessages", strings(recovered)}};
    if (!finished)
        return result(plan.operation, data, "unknown", "operation_timeout", "The final state is unknown. Inspect history and current state before retrying.");
    return tx.phase == QLatin1String("committed") ? result(plan.operation, data, "committed", {}, {}, true)
        : result(plan.operation, data, tx.phase, "change_not_confirmed", tx.message);
}

CliApi::Response audioCommand(const Env &env, const Arguments &args) {
    const QStringList a = args.positional.mid(1);
    const bool status = a == QStringList{"status"};
    const QString side = a.value(0), action = a.value(1), target = a.value(2), value = a.value(3);
    const QString streamIndex = target.section('@', 0, 0), streamIdentity = target.section('@', 1);
    if (status ? args.dryRun || args.yes
               : (side != QLatin1String("output") && side != QLatin1String("input") && side != QLatin1String("stream"))
                    || (side == QLatin1String("stream") ? action != QLatin1String("move") || a.size() != 4
                        : action == QLatin1String("default") ? a.size() != 3
                        : !QStringList{"volume", "mute"}.contains(action) || a.size() != 4))
        return failure("audio", "invalid_arguments", "Use audio status, output/input default, output/input volume|mute, or stream move. Inspect commands for argument grammar.");
    if (!status && (action == QLatin1String("volume") || side == QLatin1String("stream"))) {
        bool valid = false;
        (side == QLatin1String("stream") ? streamIndex : value).toInt(&valid);
        if (!valid)
            return failure("audio", "invalid_value", "Volume and stream index must be whole numbers.");
    }
    if (side == QLatin1String("stream")) {
        bool valid = false;
        streamIdentity.toULongLong(&valid);
        if (!valid || target.count('@') != 1)
            return failure("audio", "invalid_value", "Use a current stream selector (index@identity) from audio status.");
    }
    if (!status && action == QLatin1String("mute") && value != QLatin1String("true") && value != QLatin1String("false"))
        return failure("audio", "invalid_value", "Mute must be true or false.");
    AppCore core(env);
    AudioProvider provider(&core.runner, &core.coordinator);
    if (!refreshProvider(provider))
        return failure("audio", "read_timeout", "Audio discovery did not finish in time.");
    QJsonObject snapshot{{"status", provider.status()}, {"outputs", QJsonArray::fromVariantList(provider.outputs())},
                         {"inputs", QJsonArray::fromVariantList(provider.inputs())}, {"streams", QJsonArray::fromVariantList(provider.streams())},
                         {"effectiveOutput", provider.effectiveOutput()}, {"outputRoutingKnown", provider.outputRoutingKnown()}};
    if (provider.status() != QLatin1String("ok"))
        return failure("audio.inspect", "read_unavailable", "Audio state could not be read completely.", snapshot);
    if (status)
        return result("audio.inspect", snapshot, "read", {}, {}, true);
    const ProviderPlan plan = provider.plan(side, action, side == QLatin1String("stream") ? streamIndex : target, value, streamIdentity);
    return changeProvider(provider, core, plan, args.dryRun, args.yes, [&] {
        if (action == QLatin1String("default")) {
            if (side == QLatin1String("input")) provider.setDefaultInput(target);
            else provider.setDefaultOutput(target);
        } else if (action == QLatin1String("volume")) {
            if (side == QLatin1String("input")) provider.setInputVolume(target, value.toInt());
            else provider.setVolume(target, value.toInt());
        } else if (action == QLatin1String("mute")) {
            if (side == QLatin1String("input")) provider.setInputMute(target, value == QLatin1String("true"));
            else provider.setMute(target, value == QLatin1String("true"));
        } else {
            provider.moveStream(streamIndex.toInt(), value);
        }
    });
}

CliApi::Response bluetoothCommand(const Env &env, const Arguments &args) {
    const QStringList a = args.positional.mid(1);
    const QString action = a.value(0), target = a.value(1);
    const bool status = a == QStringList{"status"};
    if (status ? args.dryRun || args.yes : !QStringList{"power", "connect", "disconnect", "forget", "pair"}.contains(action) || a.size() != 2)
        return failure("bluetooth", "invalid_arguments", "Use bluetooth status, power true|false, or connect|disconnect|forget <address>.");
    if (!status && (action == QLatin1String("power") ? target != QLatin1String("true") && target != QLatin1String("false") : !BluetoothProvider::isValidAddress(target)))
        return failure("bluetooth", "invalid_value", "Use true/false for power or a complete Bluetooth device address.");
    if (action == QLatin1String("pair"))
        return failure("bluetooth.pair", "interaction_required", "Pair this device through Omarchy's Bluetooth panel.",
                       QJsonObject{{"nextAction", QJsonObject{{"argv", QJsonArray{"launch", "panel.bluetooth"}}, {"apiVersions", QJsonArray{}}}}});
    AppCore core(env);
    BluetoothProvider provider(&core.runner, &core.coordinator);
    if (!refreshProvider(provider))
        return failure("bluetooth", "read_timeout", "Bluetooth discovery did not finish in time.");
    QJsonObject snapshot{{"status", provider.status()}, {"powered", provider.powered()},
                         {"allRadiosPowered", provider.allRadiosPowered()}, {"devices", QJsonArray::fromVariantList(provider.devices())}};
    if (provider.status() != QLatin1String("ok"))
        return failure("bluetooth.inspect", "read_unavailable", "Bluetooth state could not be read completely.", snapshot);
    if (status)
        return result("bluetooth.inspect", snapshot, "read", {}, {}, true);
    const ProviderPlan plan = provider.plan(action, target);
    return changeProvider(provider, core, plan, args.dryRun, args.yes, [&] {
        if (action == QLatin1String("power")) provider.setPowered(target == QLatin1String("true"));
        else if (action == QLatin1String("connect")) provider.connectDevice(target);
        else if (action == QLatin1String("disconnect")) provider.disconnectDevice(target);
        else provider.forgetDevice(target);
    });
}

CliApi::Response servicesCommand(const Env &env, const Arguments &args) {
    const QStringList a = args.positional.mid(1);
    const QString scope = a.value(0), action = a.value(1), unit = a.value(2);
    const bool status = action == QLatin1String("status");
    if (!QStringList{"user", "system"}.contains(scope)
        || (status ? a.size() != 2 || args.dryRun || args.yes
                   : a.size() != 3 || !QStringList{"start", "stop", "restart", "enable", "disable"}.contains(action)))
        return failure("services", "invalid_arguments", "Use services <user|system> status or <start|stop|restart|enable|disable> <unit>.");
    if (!status && !ServicesProvider::isSafeUnit(unit))
        return failure("services", "invalid_value", "Use a concrete .service unit from the selected scope; patterns and templates are not accepted.");
    AppCore core(env);
    ServicesProvider provider(&core.runner, &core.coordinator);
    const bool system = scope == QLatin1String("system");
    if (status) {
        if (!refreshProvider(provider, [&] { provider.refreshServices(system); }))
            return failure("services.inspect", "read_timeout", "Service discovery did not finish in time.");
        QJsonObject snapshot{{"scope", scope}, {"status", provider.scopeStatus(system)},
                             {"services", QJsonArray::fromVariantList(system ? provider.systemServices() : provider.userServices())}};
        return provider.scopeStatus(system) == QLatin1String("ok") ? result("services.inspect", snapshot, "read", {}, {}, true)
            : failure("services.inspect", "read_unavailable", "Service units and enablement could not both be read in this scope.", snapshot);
    }
    const auto plan = awaitResult<ProviderPlan>([&](auto done) { provider.planService(unit, action, system, done); });
    return changeProvider(provider, core, plan, args.dryRun, args.yes, [&] { provider.changeService(unit, action, system, false); });
}

CliApi::Response startupCommand(const Env &env, const Arguments &args) {
    const QStringList a = args.positional.mid(1);
    const QString action = a.value(0), id = a.value(1);
    const bool list = action == QLatin1String("list");
    if (list ? a.size() != 1 || args.dryRun || args.yes : a.size() != 2 || !QStringList{"enable", "disable"}.contains(action))
        return failure("startup", "invalid_arguments", "Use startup list or startup <enable|disable> <desktop-id>.");
    AppCore core(env);
    ServicesProvider provider(&core.runner, &core.coordinator);
    if (list) {
        provider.refreshStartup();
        QJsonObject snapshot{{"status", provider.startupStatus()}, {"apps", QJsonArray::fromVariantList(provider.startupApps())},
                             {"verificationScope", "startup-preference-only"}};
        return provider.startupStatus() == QLatin1String("ok") ? result("services.inspect", snapshot, "read", {}, {}, true)
            : failure("services.inspect", "read_unavailable", "Startup directories could not be read completely.", snapshot);
    }
    const bool enabled = action == QLatin1String("enable");
    const auto plan = provider.planStartup(id, enabled);
    return changeProvider(provider, core, plan, args.dryRun, args.yes, [&] { provider.setStartupEnabled(id, enabled); });
}

CliApi::Response networkCommand(const Env &env, const Arguments &args) {
    const QString section = args.positional.value(1);
    if (QStringList{"activate", "disconnect", "preview"}.contains(section)) {
        const auto preview = NetworkPreview::execute(env, args.positional.mid(1), args.dryRun, args.yes);
        const QString operation = section == "activate" ? "network.activate" : section == "disconnect" ? "network.deactivate"
            : args.positional.value(2) == "keep" ? "network.keepConnection" : args.positional.value(2) == "revert" ? "network.revertConnection" : "network.previewStatus";
        return result(operation, preview.value("data").toObject(), preview.value("outcome").toString(), preview.value("errorCode").toString(), preview.value("error").toString(), preview.value("verified").toBool());
    }
    const bool rescan = section == QLatin1String("rescan");
    const bool radio = section == QLatin1String("radio");
    const QString device = args.positional.value(2);
    if (radio ? args.positional.size() != 3 : rescan ? args.positional.size() > 3 || args.yes
        : !QStringList{"status", "general", "devices", "saved", "wifi"}.contains(section) || args.positional.size() != 2 || args.dryRun || args.yes)
        return failure("network", "invalid_arguments", "Use network status|general|devices|saved|wifi, rescan [interface] [--dry-run], or radio true|false [--dry-run] [--yes].");
    if (radio && device != QLatin1String("true") && device != QLatin1String("false"))
        return failure("network.setWifiEnabled", "invalid_value", "Wi-Fi radio state must be true or false.");
    if (rescan && args.positional.size() == 3 && !NetworkProvider::isScanDeviceName(device))
        return failure("network.rescan", "invalid_value", "Use a current Wi-Fi interface name from network devices.");
    AppCore core(env);
    NetworkProvider provider(&core.runner, &core.coordinator);
    if (radio) {
        const auto plan = awaitResult<ProviderPlan>([&](auto done) { provider.planWifiEnabled(device == QLatin1String("true"), done); });
        auto response = changeProvider(provider, core, plan, args.dryRun, args.yes, [&] { provider.changeWifiEnabled(plan, args.yes); });
        if (!args.dryRun && plan.ok && (plan.confirmText.isEmpty() || args.yes)) {
            auto data = response.body.value("data").toObject();
            data["readback"] = provider.radioReadback().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QJsonObject::fromVariantMap(provider.radioReadback()));
            data["verificationScope"] = "NetworkManager-radio-switch-only";
            data["connectionVerified"] = false;
            data["internetReachabilityVerified"] = false;
            response.body["data"] = data;
        }
        return response;
    }
    if (rescan) {
        if (args.dryRun) {
            if (!refreshProvider(provider, [&] { provider.refreshScanState(); })) return failure("network.rescan", "read_timeout", "Wi-Fi scan planning did not finish in time.");
            const auto plan = provider.planScan(device);
            return plan.ok ? result(plan.operation, plan.toJson(), "planned") : failure(plan.operation, plan.errorCode, plan.error, plan.toJson());
        }
        const auto recovery = core.coordinator.recoverInterrupted();
        const auto scan = awaitResult<QVariantMap>([&](auto done) { provider.requestScan(device, done); });
        auto data = QJsonObject::fromVariantMap(scan);
        data["recoveryMessages"] = strings(recovery);
        return result("network.rescan", data, scan.value("outcome").toString(), scan.value("errorCode").toString(), scan.value("error").toString());
    }
    if (!refreshProvider(provider, [&] { provider.refreshSnapshot(section == QLatin1String("status") ? QString() : section); }))
        return failure("network.refresh", "read_timeout", "Network discovery did not finish in time.", QJsonObject::fromVariantMap(provider.snapshot()));
    const auto data = QJsonObject::fromVariantMap(provider.snapshot());
    return data.value("complete").toBool() ? result("network.refresh", data, "read", {}, {}, true)
        : failure("network.refresh", "read_unavailable", "Some requested NetworkManager reads failed; inspect states and available sections.", data);
}

CliApi::Response iconsCommand(const Env &env, const Arguments &args) {
    const auto a = args.positional.mid(1);
    const QString action = a.value(0);
    const bool list = a == QStringList{"list"};
    const bool samples = action == QLatin1String("samples") && a.size() == 2;
    const bool search = action == QLatin1String("search") && (a.size() == 2 || a.size() == 3);
    if (!list && !samples && !search)
        return failure("icons", "invalid_arguments", "Use icons list, icons samples <pack-id>, or icons search <pack-id> [query] [--limit <1..300>]. Icon mutations retain their unversioned interface.");
    const QString operation = list ? "icons.inspect" : samples ? "icons.samples" : "launchers.icon-search";
    if (!list && !IconThemes::isValidId(a[1]))
        return failure(operation, "invalid_value", "Use an installed pack ID from icons list --api-version 1.");
    const QString query = a.value(2);
    if (query.size() > 512 || std::any_of(query.begin(), query.end(), [](QChar c) { return c.category() == QChar::Other_Control; }))
        return failure(operation, "invalid_value", "Icon queries are limited to 512 characters without control characters.");
    AppCore core(env);
    QJsonObject data;
    QStringList diagnostics;
    if (list) {
        const auto packs = core.icons.listJson(&diagnostics);
        diagnostics.removeDuplicates();
        data = {{"packs", QJsonArray::fromVariantList(packs)}, {"complete", diagnostics.isEmpty()}, {"diagnostics", strings(diagnostics.mid(0, 100))},
                {"searchRoots", strings(IconThemes::searchRoots(env))}, {"scope", "installed non-hidden packs with icon directories; cursor-only themes are excluded"}};
    } else {
        const auto pack = IconThemes::load(env, a[1], {}, &diagnostics);
        if (!pack.found)
            return failure(operation, "read_unavailable", pack.problem, QJsonObject{{"pack", a[1]}, {"complete", false}, {"diagnostics", strings(diagnostics.mid(0, 100))}});
        data = QJsonObject::fromVariantMap(samples ? core.icons.samples(a[1]) : core.launchers.iconChoices(a[1], query, args.limit));
        data["indexPath"] = pack.indexPath;
        data["locations"] = strings(pack.locations);
        // Source diagnostics found during initial pack resolution must survive
        // even if an inaccessible higher-priority copy later becomes readable.
        for (const auto &problem : data.value("diagnostics").toArray()) diagnostics << problem.toString();
        if (!pack.problem.isEmpty()) diagnostics << pack.problem;
        diagnostics.removeDuplicates();
        data["diagnostics"] = strings(diagnostics.mid(0, 100));
        data["complete"] = diagnostics.isEmpty() && data.value("complete").toBool();
    }
    const bool complete = data.value("complete").toBool();
    return complete ? result(operation, data, "read", {}, {}, true)
                    : failure(operation, "read_unavailable", "Icon discovery is incomplete; inspect diagnostics and the available rows.", data);
}

CliApi::Response appsCommand(const Env &env, const Arguments &args) {
    const QStringList a = args.positional.mid(1);
    if (a == QStringList{"list"}) {
        if (args.dryRun) return failure("apps.inspect", "invalid_arguments", "App listing does not support --dry-run.");
        QString error;
        const auto apps = SystemProvider::readApps(env, &error);
        const QJsonObject snapshot{{"apps", QJsonArray::fromVariantList(apps)}, {"status", error.isEmpty() ? "ok" : "read_unavailable"}};
        return error.isEmpty() ? result("apps.inspect", snapshot, "read", {}, {}, true) : failure("apps.inspect", "read_unavailable", error, snapshot);
    }
    if (a.value(0) == QLatin1String("launcher"))
        return failure("apps", "unsupported_api_operation", "apps launcher retains its documented unversioned interface.");
    if (a.size() != 2 || a.value(0) != QLatin1String("launch"))
        return failure("apps", "invalid_arguments", "Use apps list or apps launch <desktop-id>.");
    if (!SystemProvider::isSafeDesktopId(a[1]))
        return failure("apps.launch", "invalid_value", "Use an installed desktop ID from apps list.");
    AppCore core(env);
    SystemProvider provider(&core.catalog, &core.runner, &core.capabilities, &core.coordinator);
    const auto plan = provider.planLaunch(a[1]);
    if (!plan.ok) return failure(plan.operation, plan.errorCode, plan.error, plan.toJson());
    if (args.dryRun) return result(plan.operation, plan.toJson(), "planned");
    QString error;
    return provider.launchInstalledApp(a[1], &error) ? result(plan.operation, QJsonObject{{"plan", plan.toJson()}}, "launched")
        : failure(plan.operation, "launch_failed", error, plan.toJson());
}

CliApi::Response mimeCommand(const Env &env, const Arguments &args) {
    const QStringList a = args.positional.mid(1);
    const QString action = a.value(0), type = a.value(1), id = a.value(2);
    const bool list = a == QStringList{"list"}, get = action == QLatin1String("get") && a.size() == 2;
    if ((!list && !get && !(action == QLatin1String("set") && a.size() == 3)) || ((list || get) && args.dryRun))
        return failure("mime", "invalid_arguments", "Use mime list, mime get <type>, or mime set <type> <desktop-id> [--dry-run].");
    if (!list && (!SystemProvider::isSafeMime(type) || (!get && !SystemProvider::isSafeDesktopId(id))))
        return failure("mime", "invalid_value", "Use a concrete MIME type and an installed desktop ID from apps list.");
    AppCore core(env);
    SystemProvider provider(&core.catalog, &core.runner, &core.capabilities, &core.coordinator);
    if (list) {
        if (!refreshProvider(provider, [&] { provider.refreshApps(); }, &SystemProvider::appsRefreshed))
            return failure("apps.inspect", "read_timeout", "MIME association discovery did not finish in time.");
        bool complete = provider.appsStatus() == QLatin1String("ok");
        bool resolved = true;
        for (const auto &row : provider.associations()) {
            complete &= row.toMap().value("ok").toBool();
            resolved &= row.toMap().value("status").toString() == QLatin1String("ok");
        }
        const QJsonObject snapshot{{"associations", QJsonArray::fromVariantList(provider.associations())}, {"scope", "common-types"}};
        return complete ? result("apps.inspect", snapshot, "read", {}, {}, resolved)
            : failure("apps.inspect", "read_unavailable", "Some applications or current associations could not be read.", snapshot);
    }
    if (get) {
        const auto read = awaitResult<QJsonObject>([&](auto done) { provider.readAssociation(type, done); });
        return read.value("ok").toBool() ? result("apps.inspect", read, "read", {}, {}, read.value("status").toString() == QLatin1String("ok"))
            : failure("apps.inspect", "read_unavailable", read.value("reason").toString(), read);
    }
    const auto plan = awaitResult<ProviderPlan>([&](auto done) { provider.planAssociation(type, id, done); });
    return changeProvider(provider, core, plan, args.dryRun, args.yes, [&] { provider.setAssociation(type, id); });
}

CliApi::Response systemCommand(const Env &env, const Arguments &args) {
    const QString section = args.positional.value(1);
    if (!QStringList{"status", "machine", "battery", "storage", "hooks"}.contains(section))
        return failure("system.inspect", "invalid_arguments", "Use system status, machine, battery, storage, or hooks.");
    AppCore core(env);
    SystemProvider provider(&core.catalog, &core.runner, &core.capabilities, &core.coordinator);
    if (!refreshProvider(provider, [&] { provider.refreshFacts(section); }, &SystemProvider::factsRefreshed))
        return failure("system.inspect", "read_timeout", "System fact collection did not finish in time.");
    QJsonObject snapshot = provider.facts(section);
    bool complete = true, unavailable = false;
    const auto states = snapshot.value("states").toObject();
    for (auto it = states.begin(); it != states.end(); ++it) {
        const QString state = it.value().toObject().value("status").toString();
        complete &= state == QLatin1String("ok"); unavailable |= state == QLatin1String("unavailable");
    }
    snapshot["complete"] = complete;
    return unavailable ? failure("system.inspect", "read_unavailable", "Some requested facts could not be collected. Inspect each section's state.", snapshot)
        : result("system.inspect", snapshot, "read", {}, {}, complete);
}

CliApi::Response diagnosticsCommand(const Env &env, const Arguments &args) {
    if (args.positional.value(1) != QLatin1String("export"))
        return failure("diagnostics.export", "invalid_arguments", "Use diagnostics export [--dry-run].");
    AppCore core(env);
    SystemProvider provider(&core.catalog, &core.runner, &core.capabilities, &core.coordinator);
    const auto plan = provider.planDiagnostics();
    if (!plan.ok) return failure(plan.operation, plan.errorCode, plan.error, plan.toJson());
    if (args.dryRun) return result(plan.operation, plan.toJson(), "planned");
    const auto report = awaitResult<QJsonObject>([&](auto done) { provider.collectDiagnostics(done); });
    const bool complete = report.value("ok").toBool();
    return complete ? result(plan.operation, report, "exported", {}, {}, true)
        : result(plan.operation, report, report.value("reportSaved").toBool() ? "partial" : "blocked", report.value("code").toString(), report.value("message").toString());
}

CliApi::Response shortcutCommand(const Env &env, const Arguments &args) {
    const auto &a = args.positional;
    const bool check = a.value(1) == QLatin1String("check");
    const bool add = a.value(1) == QLatin1String("add"), remove = a.value(1) == QLatin1String("remove");
    if (add || remove) {
        if (a.size() != (add ? 5 : 3) || (args.replace && !add))
            return failure("shortcut", "invalid_arguments", "Use shortcut add <keys> <description> <command> [--replace] or shortcut remove <selector>.");
        if (remove && !HyprChange::validSelector(a[2]))
            return failure("shortcuts.remove", "invalid_value", "Use a current owned binding's selector from shortcut list, including its fingerprint.");
        AppCore core(env);
        ShortcutsProvider provider(&core.runner, &core.coordinator);
        const auto plan = awaitResult<ProviderPlan>([&](auto done) {
            if (add) provider.planAdd(a[2], a[3], a[4], args.replace, done);
            else provider.planRemove(a[2], done);
        });
        return changeProvider(provider, core, plan, args.dryRun, args.yes, [&] { provider.applyPlan(plan); });
    }
    if (args.dryRun || args.yes || args.replace)
        return failure("shortcuts.inspect", "invalid_arguments", "Shortcut inspection does not take mutation options.");
    if ((check && a.size() != 3) || (!check && (a.value(1) != QLatin1String("list") || a.size() != 2)))
        return failure("shortcuts.inspect", "invalid_arguments", "Use shortcut list or shortcut check <keys>.");
    AppCore core(env);
    ShortcutsProvider provider(&core.runner, &core.coordinator);
    if (check && !provider.checkKeys(a[2]).value("valid").toBool())
        return failure("shortcuts.inspect", "invalid_value", "Keys need unique SUPER/CTRL/ALT/SHIFT modifiers and one named key, such as SUPER + SHIFT + F.", QJsonObject::fromVariantMap(provider.checkKeys(a[2])));
    if (!refreshProvider(provider, [&] { provider.refresh(); }, &ShortcutsProvider::changed))
        return failure("shortcuts.inspect", "read_timeout", "Shortcut discovery did not finish in time.");
    auto snapshot = provider.snapshot();
    if (check) snapshot["check"] = QJsonObject::fromVariantMap(provider.checkKeys(a[2]));
    if (!snapshot.value("complete").toBool())
        return failure("shortcuts.inspect", "read_unavailable", "Some shortcut sources could not be inspected. Inspect effective and owned source states.", snapshot);
    return result("shortcuts.inspect", snapshot, "read", {}, {}, true);
}

CliApi::Response ruleCommand(const Env &env, const Arguments &args) {
    const auto &a = args.positional;
    const QString action = a.value(1);
    if (action == QLatin1String("add") || action == QLatin1String("remove")) {
        const bool add = action == QLatin1String("add");
        if (add ? a.size() < 4 || a.size() > 5 : a.size() != 3)
            return failure("rule", "invalid_arguments", "Use rule add <match> <action> [value] or rule remove <selector>.");
        if (!add && !HyprChange::validSelector(a[2]))
            return failure("rules.remove", "invalid_value", "Use a current rule selector from rule list --api-version 1, including its fingerprint.");
        AppCore core(env);
        WindowRulesProvider provider(&core.runner, &core.coordinator);
        const auto plan = awaitResult<ProviderPlan>([&](auto done) {
            if (add) provider.planAdd(a[2], a[3], a.value(4), done);
            else provider.planRemove(a[2], done);
        });
        return changeProvider(provider, core, plan, args.dryRun, args.yes, [&] { provider.applyPlan(plan); });
    }
    if (args.dryRun || args.yes)
        return failure("rules.validate", "invalid_arguments", "Rule inspection and validation do not take mutation options.");
    if (action == QLatin1String("types") && a.size() == 2)
        return result("rules.validate", WindowRulesProvider::constraints(), "described");
    if (action == QLatin1String("validate") && (a.size() == 4 || a.size() == 5)) {
        const QString problem = WindowRulesProvider::check(a[2], a[3], a.value(4));
        QJsonObject data{{"valid", problem.isEmpty()}, {"rule", QJsonObject{{"match", a[2].trimmed()}, {"action", a[3]}, {"value", a.value(4)}}},
                         {"verificationScope", "input-validation-only"}, {"changed", false}};
        return problem.isEmpty() ? result("rules.validate", data, "validated")
            : failure("rules.validate", "invalid_value", problem, data);
    }
    if (action == QLatin1String("list") && a.size() == 2) {
        AppCore core(env);
        WindowRulesProvider provider(&core.runner, &core.coordinator);
        const auto snapshot = provider.snapshot();
        const QString status = snapshot.value("status").toString();
        return status == QLatin1String("ok") || status == QLatin1String("absent") ? result("rules.inspect", snapshot, "read", {}, {}, true)
            : failure("rules.inspect", status, snapshot.value("reason").toString(), snapshot);
    }
    return failure("rules.validate", "invalid_arguments", "Use rule types, rule list, or rule validate <match> <action> [value].");
}

CliApi::Response shellCommand(const Env &env, const Arguments &args) {
    const auto &a = args.positional;
    const QString family = a.value(0), action = a.value(1);
    const bool read = action == QLatin1String("status") || (family == QLatin1String("plugin") && action == QLatin1String("list"));
    if (read) {
        if (a.size() != 2 || args.dryRun) return failure("plugins.inspect", "invalid_arguments", "Shell discovery takes no target or mutation options.");
        AppCore core(env); PluginsProvider provider(&core.runner, &core.coordinator);
        const QString scope = family == QLatin1String("plugin") ? (action == QLatin1String("list") ? "plugins" : "status") : family;
        const auto snapshot = awaitResult<QJsonObject>([&](auto done) { provider.inspect(scope, done); });
        return snapshot.value("complete").toBool() ? result("plugins.inspect", snapshot, "read", {}, {}, true)
            : failure("plugins.inspect", "read_unavailable", "Some requested shell sources are unavailable. Inspect each source state and the persisted/live distinction.", snapshot);
    }
    const bool enabled = family == QLatin1String("plugin") && QStringList{"enable", "disable"}.contains(action);
    const bool lifecycle = family == "plugin" && QStringList{"clone", "remove", "recover"}.contains(action);
    const bool pinned = family == QLatin1String("dock") && QStringList{"pin", "unpin"}.contains(action);
    const bool move = family == QLatin1String("widget") && action == QLatin1String("move");
    const bool option = family == "widget" && action == "option";
    if ((!enabled && !pinned && !move && !option && !lifecycle) || a.size() != (move || option ? 5 : 3))
        return failure(family, "invalid_arguments", "Use plugin enable|disable <id>, dock pin|unpin <desktop-id>, or widget move <selector> <section> <index>.");
    bool integer = false;
    const int index = a.value(4).toInt(&integer);
    if (move && (!PluginsProvider::isWidgetSelector(a[2]) || !PluginsProvider::sections().contains(a[3]) || !integer
                 || !QRegularExpression("\\A[0-9]+\\z").match(a[4]).hasMatch()))
        return failure("widgets.move", "invalid_value", "Use a current widget selector from bar status, a declared section and a nonnegative integer index.");
    if (option && (!PluginsProvider::isWidgetSelector(a[2]) || a[3] != "showPercentage" || (a[4] != "true" && a[4] != "false")))
        return failure("widgets.option", "invalid_value", "Use a current Power widget selector, showPercentage and true or false.");
    const QString target = a.value(2);
    const QString plainId = pinned && target.endsWith(".desktop") ? target.chopped(8) : target;
    if (!move && !option && !PluginsProvider::isSafeId(plainId)) return failure(family, "invalid_value", "Use a concrete discovered ID without control characters or path segments.");
    AppCore core(env); PluginsProvider provider(&core.runner, &core.coordinator);
    const auto plan = awaitResult<ProviderPlan>([&](auto done) {
        if (lifecycle && action == "recover") provider.planRecovery(target, done);
        else if (lifecycle) provider.planLifecycle(target, action == "clone", done);
        else if (enabled) provider.planEnabled(target, action == QLatin1String("enable"), done);
        else if (pinned) provider.planPin(target, action == QLatin1String("pin"), done);
        else if (option) provider.planOption(target, a[3], a[4] == "true", done);
        else provider.planMove(target, a[3], index, done);
    });
    return changeProvider(provider, core, plan, args.dryRun, args.yes, [&] { provider.applyPlan(plan); });
}

CliApi::Response displayCommand(const Env &env, const Arguments &args) {
    const auto &a = args.positional; const QString action = a.value(1);
    if (action == "preview") {
        const QString verb = a.value(2), token = a.value(3);
        if (!QStringList{"status", "keep", "revert"}.contains(verb) || (verb == "status" ? a.size() < 3 || a.size() > 4 : a.size() != 4)
            || (verb == "status" && (args.yes || args.dryRun)) || (verb == "revert" && args.yes)) return failure("display.preview", "invalid_arguments", "Use display preview status [token], keep <token> --yes, or revert <token>.");
        QString error; auto receipt = token.isEmpty() ? DisplayPreview::current(env, &error) : DisplayPreview::load(env, token, &error);
        if (!error.isEmpty()) return failure("display.preview", "receipt_unavailable", error);
        if (receipt.isEmpty()) return verb == "status" ? result("display.preview", QJsonObject{{"phase", "none"}}, "read") : failure("display.preview", "unknown_token", "There is no such preview.");
        QJsonObject data{{"receipt", receipt}, {"sessionMatches", DisplayPreview::sameSession(env, receipt)}, {"physicalAppearanceVerified", false},
            {"secondsRemaining", std::max(0, int((receipt.value("deadline").toDouble() - QDateTime::currentMSecsSinceEpoch() + 999) / 1000))}};
        if (verb == "status") return result("display.preview", data, "read");
        if (!DisplayPreview::sameSession(env, receipt)) return failure("display.preview", "stale_session", "This token belongs to another Hyprland session. No display request was sent.", data);
        if (DisplayPreview::terminal(receipt)) return receipt.value("phase") == (verb == "keep" ? "kept" : "reverted") ? result("display.preview", data, "already_" + receipt.value("phase").toString()) : failure("display.preview", "stale_token", "The preview ended with another outcome.", data);
        if (verb == "keep" && (receipt.value("phase") != "preview" || receipt.value("deadline").toDouble() <= QDateTime::currentMSecsSinceEpoch())) return failure("display.preview", "preview_unavailable", "Keep is only available for a current, applied preview before expiry.", data);
        if (args.dryRun) return result("display.preview." + verb, data, "planned");
        if (verb == "keep" && !args.yes) return failure("display.preview.keep", "confirmation_required", "Use --yes only after the person has confirmed the preview is visible and usable. An agent cannot establish this from reported geometry.", data);
        AppCore core(env); DisplayProvider provider(&core.runner, &core.coordinator, nullptr, false);
        if (!refreshProvider(provider, [&] { provider.refresh(); }) || !provider.adoptPreview(receipt.value("token").toString(), &error)) return failure("display.preview", "preview_unavailable", error.isEmpty() ? "Display discovery did not finish." : error, data);
        QEventLoop loop; QTimer poll; poll.setInterval(100); bool ended = false; QString problem;
        QObject::connect(&provider, &DisplayProvider::message, &loop, [&](const QString &text, bool failed) { if (failed) { problem = text; loop.quit(); } });
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
            const auto fresh = DisplayPreview::load(env, receipt.value("token").toString());
            if (DisplayPreview::terminal(fresh) || fresh.value("phase") == "restore_failed") { receipt = fresh; ended = true; loop.quit(); }
        });
        QTimer::singleShot(45000, &loop, &QEventLoop::quit);
        poll.start(); if (verb == "keep") provider.keep(); else provider.revert();
        if (problem.isEmpty()) loop.exec();
        const auto fresh = DisplayPreview::load(env, receipt.value("token").toString()); if (!fresh.isEmpty()) receipt = fresh;
        data["receipt"] = receipt;
        const QString phase = receipt.value("phase").toString();
        if (phase == (verb == "keep" ? "kept" : "reverted")) return result("display.preview." + verb, data, phase, {}, {}, true);
        return result("display.preview." + verb, data, ended ? phase : "unknown", "result_unconfirmed", problem.isEmpty() ? "Inspect the receipt, current outputs and any retained plan before retrying." : problem);
    }
    if (!QStringList{"status", "layout", "check"}.contains(action) || a.size() != (action == "layout" ? 3 : 2) || (action == "status" && (args.dryRun || args.yes))) return failure("display", "invalid_arguments", "Use display status, layout <json-file> [--dry-run] [--yes], check [--dry-run] [--yes], or preview status|keep|revert.");
    AppCore core(env); DisplayProvider provider(&core.runner, &core.coordinator, nullptr, false);
    if (!refreshProvider(provider, [&] { provider.refresh(); })) return failure("display.inspect", "read_timeout", "Display discovery did not finish.");
    auto snapshot = QJsonObject::fromVariantMap(provider.snapshot());
    if (!snapshot.value("complete").toBool()) return failure("display.inspect", "read_unavailable", "Inspect the display source states and available data.", snapshot);
    if (action == "status") return result("display.inspect", snapshot, "read", {}, {}, true);
    const QString pending = DisplayPreview::pendingReason(env);
    if (!pending.isEmpty() || provider.busy()) return failure("display.layout", "preview_pending", pending.isEmpty() ? "A display operation is running." : pending, snapshot);
    if (action == "check") {
        if (args.dryRun) return result("display.recovery-check", QJsonObject{{"seconds", 3}, {"nudgePixels", 8}, {"effect", "Briefly move one enabled output, let the independent watchdog restore it, then record reported recovery for this Hyprland version."}}, "planned");
        if (!args.yes) return failure("display.recovery-check", "confirmation_required", "The recovery check briefly moves a display. Use --yes after explaining this to the person.");
        QEventLoop loop; bool passed = false, finished = false; QString detail;
        QObject::connect(&provider, &DisplayProvider::checkFinished, &loop, [&](bool ok, const QString &text) { passed = ok; detail = text; finished = true; loop.quit(); });
        QTimer::singleShot(30000, &loop, &QEventLoop::quit); provider.runRecoveryCheck(); if (!finished) loop.exec();
        return result("display.recovery-check", QJsonObject::fromVariantMap(provider.lastCheck()), passed ? "passed" : "failed", passed ? QString() : "recovery_unconfirmed", detail, passed);
    }
    QFile file(a[2]);
    if (!file.open(QIODevice::ReadOnly)) return failure("display.layout", "input_unavailable", "The layout JSON file could not be read.");
    const auto bytes = file.read(1024 * 1024 + 1); QJsonParseError parsed; const auto doc = QJsonDocument::fromJson(bytes, &parsed);
    if (file.error() != QFileDevice::NoError || bytes.size() > 1024 * 1024 || QString::fromUtf8(bytes).toUtf8() != bytes || parsed.error != QJsonParseError::NoError || !doc.isArray() || doc.array().size() > 64) return failure("display.layout", "invalid_value", "Supply a bounded UTF-8 JSON array with one row for every output.");
    QVariantList layout;
    const QStringList writable{"output", "mode", "x", "y", "scale", "transform", "enabled", "mirrorOf", "vrr"};
    const QStringList reported{"description", "make", "model", "serial", "id", "width", "height", "refresh", "focused", "internal", "modes"};
    for (const auto &value : doc.array()) {
        if (!value.isObject()) return failure("display.layout", "invalid_value", "Every layout row must be an object.");
        const auto row = value.toObject(); QVariantMap merged;
        for (const auto &v : provider.monitors()) if (v.toMap().value("output").toString() == row.value("output").toString()) merged = v.toMap();
        for (auto it = row.begin(); it != row.end(); ++it) {
            if (!writable.contains(it.key()) && !reported.contains(it.key())) return failure("display.layout", "invalid_value", "Unknown layout field: " + it.key());
            if (QStringList{"id", "make", "model", "serial", "description"}.contains(it.key()) && QJsonValue::fromVariant(merged.value(it.key())) != it.value()) return failure("display.layout", "stale_output", "An output's reported identity changed since this layout was read.");
            if (writable.contains(it.key())) merged[it.key()] = it.value().toVariant();
        }
        layout << merged;
    }
    QString problem = provider.validateLayout(layout);
    if (!problem.isEmpty()) return failure("display.layout", "invalid_value", problem);
    for (auto &v : layout) {
        auto row = v.toMap(); if (!row.value("enabled").toBool()) continue;
        const QStringList size = row.value("mode").toString().section('@', 0, 0).split('x');
        row["scale"] = DisplayProvider::cleanScale(row.value("scale").toDouble(), size.value(0).toInt(), size.value(1).toInt());
        row["width"] = size.value(0).toInt(); row["height"] = size.value(1).toInt(); v = row;
    }
    HyprModule module(env); QString moduleError;
    if (!module.load(&moduleError) || (!module.importInstalled() && !module.canInstallImport())) return failure("display.layout", "ownership_conflict", moduleError.isEmpty() ? "The owned module import cannot be safely installed." : moduleError);
    QJsonObject data{{"layout", QJsonArray::fromVariantList(layout)}, {"seconds", 15}, {"editingUnlocked", provider.editingUnlocked()}, {"filesOnKeep", QJsonArray{module.path(), module.hyprlandPath()}},
        {"effects", QJsonArray{"Apply the full output arrangement under an independent timer; timeout restores reported original outputs that remain present.", "Keep persists the owned layout and may add its marked import; --yes on Keep acknowledges the person's confirmation of usability."}}, {"physicalAppearanceVerified", false}};
    if (args.dryRun) return result("display.layout", data, "planned");
    if (!provider.editingUnlocked()) return failure("display.layout", "recovery_check_required", "Run display check --yes for this Hyprland version before previewing.", data);
    if (!args.yes) return failure("display.layout", "confirmation_required", "The preview changes output modes, scale and positions. Use --yes after explaining the timed preview and user-confirmed Keep.", data);
    QEventLoop loop; QString error;
    QObject::connect(&provider, &DisplayProvider::message, &loop, [&](const QString &text, bool failed) { if (failed) { error = text; loop.quit(); } });
    QObject::connect(&provider, &DisplayProvider::previewChanged, &loop, [&] { if (provider.previewing()) loop.quit(); });
    QTimer::singleShot(30000, &loop, &QEventLoop::quit); provider.preview(layout); if (!provider.previewing() && error.isEmpty()) loop.exec();
    data["receipt"] = provider.previewReceipt();
    if (!error.isEmpty() || !provider.previewing()) return result("display.layout", data, "unknown", "preview_unconfirmed", error.isEmpty() ? "Inspect the returned token; the start result is unknown." : error);
    return result("display.layout", data, "preview", {}, {}, true);
}

}  // namespace

namespace CliApi {

bool requested(const QStringList &arguments) {
    // Keep the original bare install's human output. Every new skill command,
    // and install with options, uses the structured interface.
    if (arguments == QStringList{"skill", "install"}) return false;
    QString first, second;
    for (const auto &part : arguments) {
        if (part == QLatin1String("--"))
            break;
        if (part == QLatin1String("--api-version") || part.startsWith(QLatin1String("--api-version=")))
            return true;
        if (!part.startsWith('-')) {
            if (first.isEmpty()) first = part;
            else if (second.isEmpty()) second = part;
        }
    }
    if (first == QLatin1String("apps")) {
        const auto args = parse(arguments);
        return args.positional.value(1) != QLatin1String("launcher");
    }
    if (first == "profile" && second == "cancel") return true;
    if (first == "display") {
        if (QStringList{"status", "layout", "check"}.contains(second)) return true;
        const auto parsed = parse(arguments);
        if (second == "preview" && QStringList{"status", "keep", "revert"}.contains(parsed.positional.value(2))) return true;
    }
    if (first == QLatin1String("icons")) {
        const auto args = parse(arguments);
        const QString action = args.error.isEmpty() ? args.positional.value(1) : second;
        const int literal = arguments.indexOf("--");
        const auto options = literal < 0 ? arguments : arguments.mid(0, literal);
        return options.contains("--limit") || QStringList{"samples", "search"}.contains(action);
    }
    if (first == QLatin1String("shortcut") || first == QLatin1String("rule")) {
        const auto args = parse(arguments);
        const QString action = args.error.isEmpty() ? args.positional.value(1) : second;
        return args.dryRun || args.replace || (first == QLatin1String("shortcut") ? QStringList{"list", "check"}.contains(action)
            : QStringList{"types", "validate"}.contains(action));
    }
    if (first == QLatin1String("plugin") || first == QLatin1String("widget")) {
        const auto args = parse(arguments);
        const QString action = args.error.isEmpty() ? args.positional.value(1) : second;
        return args.dryRun || (first == "widget" && action == "option") || (first == QLatin1String("plugin") && QStringList{"list", "status", "recover"}.contains(action));
    }
    return QStringList{"commands", "describe", "audio", "bluetooth", "network", "preferences", "services", "startup", "mime", "system", "diagnostics", "bar", "dock", "skill"}.contains(first);
}

QJsonObject describe(const CatalogEntry &entry, const Env &env) {
    QJsonObject schema{{"type", entry.type}, {"unit", entry.unit}, {"allowCustomValue", entry.allowCustomValue},
                       {"pattern", entry.pattern}, {"choices", entry.choices},
                       {"dynamicChoices", !entry.choicesFrom.isEmpty() || entry.read.value("kind") == QLatin1String("icons")}};
    if (entry.hasRange) {
        schema["minimum"] = entry.min;
        schema["maximum"] = entry.max;
        schema["step"] = entry.step;
    }
    schema["defaultKind"] = entry.read.contains("default") ? "declared" : "inherited-or-runtime";
    if (entry.read.contains("default"))
        schema["default"] = entry.read.value("default");
    if (entry.type == QLatin1String("toggle"))
        schema["acceptedValues"] = QJsonArray{"true", "false"};
    QSet<QString> files;
    for (const auto &path : entry.files)
        files.insert(env.expand(path));
    for (const auto &spec : {entry.read, entry.write, entry.launch}) {
        for (const char *key : {"file", "fallbackFile"}) {
            const QString path = spec.value(QLatin1String(key)).toString();
            if (!path.isEmpty())
                files.insert(env.expand(path));
        }
    }
    const bool hypr = entry.write.value("kind") == QLatin1String("hypr");
    if (hypr) {
        files.insert(env.configHome + "/hypr/hyprland.lua");
        files.insert(env.configHome + (entry.write.value("slot") == QLatin1String("blur")
                                          ? "/hypr/omarchy-control-blur.lua" : "/hypr/omarchy-control.lua"));
    }
    QStringList paths = files.values();
    paths.sort();
    const bool action = entry.isAction();
    const QString kind = entry.launch.value("kind").toString();
    const QString classification = routeClass(entry);
    return {{"id", entry.id}, {"label", entry.label}, {"description", entry.description}, {"page", entry.page},
            {"section", entry.section}, {"domain", entry.domain}, {"keywords", strings(entry.keywords)},
            {"integration", entry.integration}, {"classification", classification}, {"schema", schema},
            {"readable", !entry.read.isEmpty()}, {"writable", entry.isEditable()},
            {"resetSupported", !entry.reset.isEmpty() || entry.owned || QStringList{"json", "icons"}.contains(entry.write.value("kind").toString())},
            {"recovery", entry.recovery}, {"scope", entry.scope}, {"effects", strings(entry.effects)},
            {"restart", entry.restart}, {"reloads", entry.reloads}, {"files", strings(paths)},
            {"filesComplete", false}, {"ownership", hypr ? "managed-hypr-module" : action ? "destination-only" : "reviewed-backend"},
            {"confirmation", QJsonObject{{"always", entry.confirmAlways}, {"when", entry.confirmWhen},
                                           {"message", entry.confirmText}, {"kind", entry.confirm}}},
            {"requirements", QJsonObject{{"commands", strings(entry.requiresCommands)}, {"services", strings(entry.requiresCaps)},
                                           {"hardware", strings(entry.requiresHardware)}}},
            {"availability", QJsonObject{{"status", "not_checked"}}},
            {"target", QJsonObject{{"supported", !entry.target.isEmpty()}, {"id", Catalog::targetOf(entry.id)},
                                    {"syntax", entry.target.isEmpty() ? entry.id : entry.id.section('@', 0, 0) + "@<device>"},
                                    {"enumeration", entry.target == "display" ? QJsonValue(QJsonArray{"display", "status"}) : QJsonValue(QJsonValue::Null)},
                                    {"source", entry.target == "display" ? "monitors[].output; inspect brightness availability per display" : "not applicable"}}},
            {"invocations", invocations(entry)}, {"operationIds", pageOperations(entry)},
            {"nativeSetting", entry.launch.value("nativeSetting").isString() ? entry.launch.value("nativeSetting") : QJsonValue(QJsonValue::Null)},
            {"nativeOperations", entry.launch.value("nativeOperations").toArray()},
            {"handoffReason", entry.launch.value("handoffReason")},
            {"invocationNote", "Arguments omit the binary. get/plan/apply/reset use API v1; launch retains its unversioned handoff contract."},
            {"automation", QJsonObject{{"scope", action ? kind == "page" ? "discover operationIds or the focused typed setting" : "destination only; launch does not verify effects" : "shared settings engine"},
                                        {"status", entry.launch.value("automationStatus")},
                                        {"effectVerification", !action}, {"arbitraryHelperArguments", false}}},
            {"destination", QJsonObject{{"kind", kind}, {"page", entry.launch.value("page")},
                                         {"focus", entry.launch.value("focus")}, {"file", entry.launch.value("file")}}}};
}

QJsonObject coverage(const Catalog &catalog) {
    QJsonArray rows;
    QJsonObject counts;
    const auto append = [&](QJsonObject row) {
        const QString status = row.value("classification").toString();
        counts[status] = counts.value(status).toInt() + 1;
        rows.append(row);
    };
    for (const auto &entry : catalog.entries()) {
        const QString status = routeClass(entry);
        append({{"id", entry.id}, {"surface", "catalog"}, {"classification", status}, {"invocations", invocations(entry)},
                {"operationIds", pageOperations(entry)},
                {"nativeSetting", entry.launch.value("nativeSetting").isString() ? entry.launch.value("nativeSetting") : QJsonValue(QJsonValue::Null)},
                {"nativeOperations", entry.launch.value("nativeOperations").toArray()},
                {"handoffReason", entry.launch.value("handoffReason")},
                {"reason", entry.isSetting() ? "Shared settings engine with typed input, compatibility/ownership checks, journal and readback. Live availability is machine-specific."
                                            : "Catalog destination only. Native pages expose operationIds; external helpers retain their own parameters, interaction and verification outside Control's automation contract."}});
    }
    for (const auto &operation : operations()) {
        QJsonObject row = operation.toObject();
        row["surface"] = "gui-operation";
        append(row);
    }
    return {{"complete", false}, {"inventoryComplete", true}, {"counts", counts}, {"entries", rows},
            {"note", "All GUI methods and catalog destinations are classified. External workflows expose destinations, not parameterized automation. Inventory does not establish physical desktop behavior or every harness/model's readiness."}};
}

Response execute(const Env &env, const QStringList &arguments) {
    const Arguments args = parse(arguments);
    const QString command = args.positional.value(0);
    if (!args.error.isEmpty())
        return failure(command, args.errorCode, args.error);
    const Command *spec = nullptr;
    for (const auto &candidate : commands())
        if (command == QLatin1String(candidate.name))
            spec = &candidate;
    if (!spec && !args.help && !command.isEmpty())
        return failure(command, Cli::isCliCommand(command) ? "unsupported_api_operation" : "unknown_command",
                       "This command is not available in API v1. Inspect commands for supported operations.");
    if (!args.help && command == QLatin1String("apps") && args.positional.value(1) == QLatin1String("launcher"))
        return failure(command, "unsupported_api_operation", "apps launcher retains its documented unversioned interface.");
    if (!args.help && command == QLatin1String("icons")
        && QStringList{"import", "reapply", "remove", "remove-integration", "layer", "iconic"}.contains(args.positional.value(1)))
        return failure(command, "unsupported_api_operation", "Icon mutations and integration commands retain their documented unversioned interface.");
    const int arity = args.positional.size() - 1;
    if (spec && !args.help && (arity < spec->minimum || arity > spec->maximum))
        return failure(command, "invalid_arguments", QStringLiteral("Usage: %1").arg(QLatin1String(spec->usage)));
    const bool provider = QStringList{"audio", "bluetooth", "network", "preferences", "profile", "display", "services", "startup", "apps", "mime", "diagnostics", "shortcut", "rule", "plugin", "widget", "dock", "skill"}.contains(command);
    if ((args.yes && command != QLatin1String("apply") && command != QLatin1String("reset") && command != QLatin1String("undo") && command != QLatin1String("bluetooth") && command != QLatin1String("network") && command != QLatin1String("services") && command != QLatin1String("shortcut") && command != QLatin1String("plugin") && command != QLatin1String("display")) || (args.live && command != QLatin1String("describe"))
        || (args.yes && command == "plugin" && !QStringList{"clone", "remove", "recover"}.contains(args.positional.value(1)))
        || (args.dryRun && !provider)
        || (args.replace && (command != QLatin1String("shortcut") || args.positional.value(1) != QLatin1String("add")))
        || (args.cli && command != QLatin1String("coverage")) || (!args.targets.isEmpty() && command != QLatin1String("skill")))
        return failure(command, "invalid_arguments", "An option is not supported by this command.");
    if (args.hasLimit && (command != QLatin1String("icons") || args.positional.value(1) != QLatin1String("search")))
        return failure(command, "invalid_arguments", "--limit is supported only by icons search.");

    if (command.isEmpty() || args.help || command == QLatin1String("commands") || command == QLatin1String("help")) {
        QJsonArray list;
        for (const auto &item : commands())
            list.append(QJsonObject{{"name", item.name}, {"usage", item.usage}, {"mutation", item.mutation}, {"apiVersions", QJsonArray{1}}});
        return result(command, QJsonObject{{"commands", list}, {"providerOperations", operations()},
                                          {"globalOptions", QJsonArray{"--json", "--api-version 1", "--help"}},
                                          {"literalArguments", "Use -- before values beginning with a dash."},
                                          {"coverageComplete", false}});
    }
    if (command == QLatin1String("--version"))
        return result(command, QJsonObject{{"supportedApiVersions", QJsonArray{1}}});
    if (command == QLatin1String("skill")) {
        const QString action = args.positional.value(1);
        if (!QStringList{"status", "plan", "install", "reconcile", "remove", "opt-out", "opt-in"}.contains(action)
            || (args.dryRun && QStringList{"status", "plan", "opt-out", "opt-in"}.contains(action))
            || (!args.targets.isEmpty() && QStringList{"opt-out", "opt-in"}.contains(action)))
            return failure("skill", "invalid_arguments", "Use skill status|plan|install|reconcile|remove [--target <directory>] or skill opt-out|opt-in.");
        const auto integration = SkillIntegration::run(env, action, args.targets, args.dryRun);
        return result("skill." + action, integration.data, integration.outcome, integration.code, integration.message, integration.verified);
    }
    if (command == QLatin1String("audio"))
        return audioCommand(env, args);
    if (command == QLatin1String("bluetooth"))
        return bluetoothCommand(env, args);
    if (command == QLatin1String("network"))
        return networkCommand(env, args);
    if (command == "preferences") {
        const QString action = args.positional.value(1), id = args.positional.value(2);
        if (action == "crash-mutes" || action == "crash-mute") {
            if (action == "crash-mutes" ? args.positional.size() != 2 || args.dryRun
                : args.positional.size() < 3 || args.positional.size() > 4 || id.isEmpty()
                  || (args.positional.size() == 4 && args.positional[3].isEmpty()))
                return failure("preferences.crash-mutes", "invalid_arguments", "Use preferences crash-mutes or crash-mute <program> [true|false] [--dry-run].");
            CommandRunner runner(env); ChangeCoordinator coordinator(env);
            const auto value = awaitResult<QJsonObject>([&](auto done) {
                Preferences::crashMutes(&runner, &coordinator, id, args.positional.value(3), args.dryRun, done);
            });
            return result("preferences.crash-mutes", value, value.value("outcome").toString(), value.value("errorCode").toString(),
                          value.value("error").toString(), value.value("ok").toBool() && !args.dryRun);
        }
        if (action == "pins" ? args.positional.size() != 2 || args.dryRun : !QStringList{"pin", "unpin"}.contains(action) || args.positional.size() != 3)
            return failure("preferences.pins", "invalid_arguments", "Use preferences pins or pin|unpin <catalog-id> [--dry-run].");
        Catalog catalog;
        const auto value = Preferences::pins(env, catalog, action == "pins" ? "list" : action, id, args.dryRun);
        return result("preferences.pins", value, value.value("outcome").toString(), value.value("errorCode").toString(), value.value("error").toString(), value.value("ok").toBool() && !args.dryRun);
    }
    if (command == "profile") {
        if (args.positional.value(1) != "cancel") return failure("profile", "unsupported_api_operation", "Only profile cancel currently selects this API; other profile commands retain their interface.");
        const QString id = args.positional.value(2);
        if (!ProfileStore::isId(id, "run")) return failure("profile.cancel", "invalid_value", "Use a restore run ID from profile status.");
        AppCore core(env); QString error;
        const auto run = core.profileRunner.load(id, &error);
        if (run.id != id || run.state != "running") return failure("profile.cancel", "stale_target", error.isEmpty() ? "That restore is not running." : error);
        QJsonObject data{{"runId", id}, {"state", run.state}, {"scope", "stop-between-operations"}, {"currentOperationInterrupted", false}, {"nextRead", QJsonArray{"profile", "status", "--json"}}};
        if (args.dryRun) return result("profile.cancel", data, "planned");
        if (!core.profileRunner.requestCancel(id, &error)) return failure("profile.cancel", "request_unconfirmed", error, data);
        return result("profile.cancel", data, "cancellation_requested");
    }
    if (command == "display") return displayCommand(env, args);
    if (command == QLatin1String("services"))
        return servicesCommand(env, args);
    if (command == QLatin1String("startup"))
        return startupCommand(env, args);
    if (command == QLatin1String("apps"))
        return appsCommand(env, args);
    if (command == QLatin1String("icons"))
        return iconsCommand(env, args);
    if (command == QLatin1String("mime"))
        return mimeCommand(env, args);
    if (command == QLatin1String("system"))
        return systemCommand(env, args);
    if (command == QLatin1String("diagnostics"))
        return diagnosticsCommand(env, args);
    if (command == QLatin1String("shortcut"))
        return shortcutCommand(env, args);
    if (command == QLatin1String("rule"))
        return ruleCommand(env, args);
    if (QStringList{"plugin", "bar", "widget", "dock"}.contains(command))
        return shellCommand(env, args);
    if (QStringList{"history", "reset", "undo"}.contains(command)) {
        AppCore core(env);
        if (command == "history") {
            QJsonArray rows;
            for (const auto &tx : core.coordinator.history()) rows.append(tx.toJson());
            return result(command, rows, "read");
        }
        const QString id = args.positional[1];
        const Transaction original = command == "undo" ? core.coordinator.transaction(id) : Transaction{};
        if (command == "undo" && (original.id.isEmpty() || !original.undoable()))
            return failure(command, "undo_unavailable", "This transaction cannot be undone automatically.");
        const CatalogEntry *entry = core.catalog.find(command == "undo" ? original.settingId : id);
        if (!entry || (command == "reset" && !entry->isSetting()))
            return failure(command, "unknown_id", "No such resettable setting or undo target.");
        // A reset may restore a consequential default whose effective value is
        // determined at reload. Do not assume the current value's acknowledgement
        // covers that different value.
        QString confirmation = entry->confirmAlways;
        if (command == "reset" && !entry->confirmWhen.isEmpty() && confirmation.isEmpty())
            confirmation = "Returning to the inherited default may change consequential behavior. Review describe and acknowledge with --yes.";
        if (command == "undo" && confirmation.isEmpty())
            confirmation = entry->confirmWhen.value(original.before).toString();
        if (!confirmation.isEmpty() && !args.yes)
            return failure(command, "confirmation_required", confirmation);
        core.capabilities.scanServicesBlocking();
        const QStringList recovered = core.coordinator.recoverInterrupted();
        const Transaction tx = awaitResult<Transaction>([&](auto done) {
            if (command == "reset") core.engine.reset(id, done);
            else core.engine.undo(id, done);
        });
        QJsonObject data = tx.toJson();
        data["recoveryMessages"] = strings(recovered);
        data["verificationScope"] = "shared-engine-readback; inherited defaults may differ from the previous value";
        return tx.phase == "committed" ? result(command, data, "committed", {}, {}, true)
            : result(command, data, tx.phase, "change_not_confirmed", tx.message);
    }
    Catalog catalog;
    if (command == QLatin1String("coverage")) {
        if (!args.cli)
            return failure(command, "invalid_arguments", "Use coverage --cli for the API operation inventory.");
        return result(command, coverage(catalog));
    }
    if (command == QLatin1String("catalog") || command == QLatin1String("search")) {
        QJsonArray rows;
        if (command == QLatin1String("catalog")) {
            for (const auto &entry : catalog.entries())
                rows.append(describe(entry, env));
        } else {
            for (const auto &hit : catalog.search(args.positional[1], 40)) {
                QJsonObject row = describe(*hit.entry, env);
                row["score"] = hit.score;
                rows.append(row);
            }
        }
        return result(command, rows);
    }
    const QString id = args.positional.value(1);
    if (command == QLatin1String("describe")) {
        for (const auto &operation : operations())
            if (operation.toObject().value("id").toString() == id)
                return args.live ? failure(command, "unsupported_api_operation", "Live inspection is not implemented for this provider operation.")
                                 : result(command, operation);
    }
    const CatalogEntry *entry = catalog.find(id);
    if (!entry)
        return failure(command, "unknown_id", "No such setting or operation.");
    // Catalog lookup returns the base entry for a targeted ID. Keep the target
    // throughout reading, choices, descriptions, and applying the value.
    CatalogEntry addressed = *entry;
    addressed.id = id;
    entry = &addressed;
    if (command == QLatin1String("describe") && !args.live)
        return result(command, describe(*entry, env));
    if (!entry->isSetting())
        return failure(command, "not_a_setting", "This destination is not a typed setting. Inspect describe for its operation or handoff.");
    AppCore core(env);
    core.capabilities.scanServicesBlocking();
    const QVariantList choices = awaitResult<QVariantList>([&](auto done) { core.engine.readChoices(id, done); });
    if (command == QLatin1String("describe") || command == QLatin1String("get")) {
        ReadResult state;
        QJsonObject value = readValue(core, *entry, choices, &state);
        const bool readable = state.status == QLatin1String("ok") || state.status == QLatin1String("custom");
        if (command == QLatin1String("describe")) {
            QJsonObject description = describe(*entry, env);
            description["current"] = value;
            description["liveChoices"] = QJsonArray::fromVariantList(choices);
            const auto availability = core.capabilities.availability(*entry);
            description["availability"] = QJsonObject{{"status", availability.available ? "available" : "unavailable"}, {"reason", availability.reason}};
            return result(command, description);
        }
        return readable ? result(command, value, "read", {}, {}, state.status == QLatin1String("ok"))
                        : failure(command, "read_unavailable", state.reason, value);
    }
    const ChangePlan plan = core.engine.plan(id, args.positional[2], choices);
    if (!plan.ok)
        return failure(command, plan.errorCode, plan.error, plan.toJson());
    if (command == QLatin1String("plan"))
        return result(command, plan.toJson(), "planned");
    if (!plan.confirmText.isEmpty() && !args.yes)
        return failure(command, "confirmation_required", plan.confirmText, plan.toJson());
    const QStringList recovery = core.coordinator.recoverInterrupted();
    const Transaction tx = awaitResult<Transaction>([&](auto done) { core.engine.apply(id, args.positional[2], done); });
    QJsonObject data = tx.toJson();
    data["recoveryMessages"] = strings(recovery);
    return tx.phase == QLatin1String("committed") ? result(command, data, "committed", {}, {}, true)
        : result(command, data, tx.phase, "change_not_confirmed", tx.message);
}

}  // namespace CliApi
