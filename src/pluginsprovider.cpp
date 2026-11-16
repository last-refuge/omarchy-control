#include "pluginsprovider.h"

#include <QDir>
#include <QCoreApplication>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QUrl>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>
#include <QSet>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>

#include "fileops.h"
#include "pluginfiles.h"
#include <QUuid>
#include <pwd.h>
#include "iconthemes.h"
#include "compat.h"
#include "systemprovider.h"


namespace {
QString tr(const char *text) { return QCoreApplication::translate("PluginsProvider", text); }
QString compact(const QJsonValue &value) {
    return QString::fromUtf8(value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact) : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
}
QJsonObject unavailable(const QString &reason) { return {{"status", "unavailable"}, {"reason", reason}}; }
bool reportedId(const QString &id) {
    if (id.isEmpty() || id.size() > 1024 || id.contains('/') || id.contains("..")) return false;
    for (const QChar c : id) if (c.category() == QChar::Other_Control) return false;
    return true;
}
bool traversable(const QString &path) {
    for (QString parent = QFileInfo(path).absolutePath();;) {
        const QFileInfo info(parent);
        if ((info.exists() || info.isSymLink()) && (!info.isDir() || !info.isReadable() || !info.isExecutable())) return false;
        const QString next = QFileInfo(parent).absolutePath();
        if (next == parent) return true;
        parent = next;
    }
}
QJsonObject configFile(const QString &path) {
    QJsonObject info{{"path", path}, {"status", "unavailable"}};
    if (!traversable(path)) { info["reason"] = tr("The configuration directory could not be inspected."); return info; }
    const QFileInfo fileInfo(path);
    if (!fileInfo.exists() && !fileInfo.isSymLink()) { info["status"] = "absent"; info["fingerprint"] = "missing"; return info; }
    QFile file(path);
    if (!fileInfo.isFile() || !file.open(QIODevice::ReadOnly)) { info["reason"] = tr("The shell configuration could not be read."); return info; }
    const QByteArray bytes = file.read(4 * 1024 * 1024 + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > 4 * 1024 * 1024) { info["reason"] = tr("The shell configuration exceeded the read limit or could not be read completely."); return info; }
    info["fingerprint"] = FileOps::fingerprintBytes(bytes);
    if (bytes.isEmpty()) { info["status"] = "empty"; return info; }
    const auto doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject() || doc.object().value("version").toDouble() != 1) { info["reason"] = tr("The shell configuration must be a JSON object with version 1."); return info; }
    info["status"] = "ok"; info["value"] = doc.object(); return info;
}
QJsonObject readFiles(const Env &env) {
    const auto user = configFile(env.configHome + "/omarchy/shell.json");
    const auto defaults = configFile(env.omarchyPath + "/config/omarchy/shell.json");
    const QString status = user.value("status").toString();
    const auto source = status == QLatin1String("absent") || status == QLatin1String("empty") ? defaults : user;
    return {{"user", user}, {"defaults", defaults}, {"source", source}};
}
bool safeWritePath(const Env &env) {
    for (const auto &path : {env.configHome, env.configHome + "/omarchy"}) {
        const QFileInfo info(path);
        if (info.isSymLink() || (info.exists() && (!info.isDir() || info.ownerId() != getuid() || !info.isWritable()))) return false;
    }
    const QString path = env.configHome + "/omarchy/shell.json";
    const QFileInfo info(path);
    struct stat state {};
    return !info.isSymLink() && (!info.exists() || (info.isFile() && info.ownerId() == getuid()
        && ::stat(QFile::encodeName(path).constData(), &state) == 0 && state.st_nlink == 1));
}
QJsonObject layoutState(const QJsonObject &config) {
    const auto bar = config.value("bar");
    if (!bar.isObject() || !bar.toObject().value("layout").isObject()) return unavailable(tr("No readable bar layout is declared in this source."));
    QJsonObject layout = bar.toObject().value("layout").toObject();
    for (const auto &section : PluginsProvider::sections()) {
        if (!layout.contains(section)) layout[section] = QJsonArray{};
        if (!layout.value(section).isArray()) return unavailable(tr("A bar section is not an array."));
        for (const auto &entry : layout.value(section).toArray()) {
            const auto id = entry.isString() ? entry : entry.toObject().value("id");
            if ((!entry.isString() && !entry.isObject()) || !id.isString() || !reportedId(id.toString())) return unavailable(tr("A bar widget has invalid identity data."));
        }
    }
    const QString active = bar.toObject().value("id").toString();
    return {{"status", "ok"}, {"layout", layout}, {"activeBar", active.isEmpty() ? "omarchy.bar" : active}, {"options", bar}};
}
QString widgetId(const QJsonValue &entry) { return entry.isString() ? entry.toString() : entry.toObject().value("id").toString(); }
QString widgetDigest(const QJsonObject &live) { return FileOps::fingerprintBytes(QJsonDocument(live.value("bar").toObject()).toJson(QJsonDocument::Compact)); }
QString widgetSelector(const QJsonObject &live, const QString &section, int index) { return section + ':' + QString::number(index) + '@' + widgetDigest(live); }
QJsonObject dockState(const QJsonObject &files) {
    const auto source = files.value("source").toObject();
    if (source.value("status").toString() != QLatin1String("ok")) return unavailable(tr("The dock's source configuration is unavailable."));
    auto pinsSource = source;
    auto pins = source.value("value").toObject().value("dock").toObject().value("pinned");
    if (pins.isUndefined() || pins.isNull()) {
        pinsSource = files.value("defaults").toObject();
        if (pinsSource.value("status").toString() != QLatin1String("ok")) return unavailable(tr("Default dock pins could not be read."));
        pins = pinsSource.value("value").toObject().value("dock").toObject().value("pinned");
        if (pins.isUndefined() || pins.isNull()) pins = QJsonArray{};
    }
    if (!pins.isArray()) return unavailable(tr("Dock pins are not an array."));
    for (const auto &pin : pins.toArray())
        if (!pin.isString() || !PluginsProvider::isSafeId(pin.toString()) || pin.toString().endsWith(".desktop")) return unavailable(tr("A dock pin has invalid identity data."));
    return {{"status", "ok"}, {"pins", pins}, {"source", pinsSource.value("path")}, {"verificationScope", "persisted-preference-only"}};
}
QJsonValue persistedEnabled(const QJsonObject &config, const QJsonObject &plugin) {
    const QString id = plugin.value("id").toString();
    const auto kinds = plugin.value("kinds").toArray();
    if (kinds.contains("bar")) {
        const QString active = config.value("bar").toObject().value("id").toString();
        return (active.isEmpty() ? QStringLiteral("omarchy.bar") : active) == id;
    }
    if (kinds.contains("bar-widget")) {
        const auto bar = layoutState(config);
        if (bar.value("status").toString() != QLatin1String("ok")) return QJsonValue::Null;
        const auto layout = bar.value("layout").toObject();
        for (const auto &section : PluginsProvider::sections())
            for (const auto &entry : layout.value(section).toArray()) if (widgetId(entry) == id) return true;
        return false;
    }
    if (plugin.value("firstParty").toBool()) return !config.value("disabledPlugins").toArray().contains(id);
    for (const auto &entry : config.value("plugins").toArray()) if (entry.toObject().value("id").toString() == id) return true;
    return false;
}
bool pluginRows(const QJsonDocument &doc) {
    if (!doc.isArray()) return false;
    QSet<QString> ids;
    for (const auto &entry : doc.array()) {
        const auto row = entry.toObject();
        const QString id = row.value("id").toString();
        if (!entry.isObject() || !reportedId(id) || ids.contains(id) || !row.value("enabled").isBool()
            || !row.value("firstParty").isBool() || !row.value("canDisable").isBool() || !row.value("kinds").isArray() || row.value("kinds").toArray().isEmpty()) return false;
        for (const auto &kind : row.value("kinds").toArray()) if (!kind.isString() || kind.toString().isEmpty()) return false;
        for (const auto &field : {"name", "clonedFrom"}) if (row.contains(field) && !row.value(field).isString()) return false;
        if (row.contains("active") && !row.value("active").isBool()) return false;
        ids.insert(id);
    }
    return true;
}
QJsonObject publicState(const QJsonObject &state, const QString &scope) {
    QJsonObject out{{"scope", scope}};
    bool complete = true;
    if (scope == QLatin1String("plugins") || scope == QLatin1String("status") || scope == QLatin1String("bar")) {
        auto plugins = state.value("plugins").toObject();
        complete &= plugins.value("status").toString() == QLatin1String("ok"); out["plugins"] = plugins;
    }
    if (scope != QLatin1String("plugins")) {
        auto files = state.value("files").toObject();
        complete &= files.value("source").toObject().value("status").toString() == QLatin1String("ok");
        for (const auto &key : {"user", "defaults", "source"}) { auto file = files.value(key).toObject(); file.remove("value"); files[key] = file; }
        out["files"] = files;
        if (scope != QLatin1String("dock")) {
            auto live = state.value("live").toObject(); live.remove("value"); out["live"] = live;
            auto bar = state.value("bar").toObject();
            complete &= bar.value("status").toString() == QLatin1String("ok");
            if (bar.value("status").toString() == QLatin1String("ok")) {
                QJsonArray widgets;
                const auto liveConfig = state.value("live").toObject().value("value").toObject();
                for (const auto &section : PluginsProvider::sections()) {
                    const auto entries = bar.value("layout").toObject().value(section).toArray();
                    for (int i = 0; i < entries.size(); ++i) {
                        QJsonObject widget{{"id", widgetId(entries[i])}, {"section", section}, {"index", i},
                            {"selector", widgetSelector(liveConfig, section, i)}, {"entry", entries[i]}};
                        bool builtinPower = false;
                        for (const auto &item : state.value("plugins").toObject().value("items").toArray()) {
                            const auto plugin = item.toObject();
                            if (plugin.value("id") == "omarchy.power") builtinPower = plugin.value("firstParty").toBool();
                        }
                        if (widgetId(entries[i]) == "omarchy.power" && builtinPower && entries[i].isObject()) {
                            const auto value = entries[i].toObject().value("showPercentage");
                            const bool known = value.isUndefined() || value.isBool();
                            widget["options"] = QJsonObject{{"showPercentage", QJsonObject{{"status", known ? "ok" : "custom"},
                                {"value", known ? QJsonValue(value.toBool(false)) : QJsonValue(QJsonValue::Null)}, {"default", false},
                                {"inherited", value.isUndefined()}, {"verificationScope", "reported-configuration; rendering unverified"}}}};
                        }
                        widgets.append(widget);
                    }
                }
                bar["widgets"] = widgets; bar["verificationScope"] = "reported-live-configuration-only";
            } else { bar["layout"] = QJsonValue::Null; bar["widgets"] = QJsonValue::Null; }
            const auto source = state.value("files").toObject().value("source").toObject();
            bar["persisted"] = source.value("status").toString() == QLatin1String("ok") ? QJsonValue(layoutState(source.value("value").toObject())) : QJsonValue(QJsonValue::Null);
            out["bar"] = bar;
        }
        if (scope == QLatin1String("dock") || scope == QLatin1String("status")) {
            auto dock = state.value("dock").toObject(); complete &= dock.value("status").toString() == QLatin1String("ok");
            if (!dock.contains("pins")) dock["pins"] = QJsonValue::Null; out["dock"] = dock;
        }
    }
    out["complete"] = complete; return out;
}
}

QStringList PluginsProvider::sections() { return {"left", "centerBegin", "center", "centerEnd", "right", "tray"}; }

PluginsProvider::PluginsProvider(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *parent)
    : QObject(parent), m_runner(runner), m_coordinator(coordinator) {}

bool PluginsProvider::isSafeId(const QString &id) {
    static const QRegularExpression safe("\\A[A-Za-z0-9][A-Za-z0-9._-]{0,254}\\z");
    return safe.match(id).hasMatch() && !id.contains("..");
}

QVariantList PluginsProvider::parsePlugins(const QByteArray &json) {
    QVariantList rows;
    for (const auto &value : QJsonDocument::fromJson(json).array())
        rows << value.toObject().toVariantMap();
    std::stable_sort(rows.begin(), rows.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("name").toString().localeAwareCompare(b.toMap().value("name").toString()) < 0;
    });
    return rows;
}

QVariantList PluginsProvider::scanManifests(const Env &env) {
    // Used when the shell is not answering. Manifests say what exists; only the
    // running shell can say what is active, so `enabled` is left unknown.
    QVariantList rows;
    for (const auto &root : {env.omarchyPath + "/shell/plugins", env.configHome + "/omarchy/plugins"}) {
        QDirIterator it(root, QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        int visited = 0;
        while (it.hasNext() && ++visited <= 8192 && rows.size() < 512) {
            const QString path = it.next();
            const QFileInfo info(path);
            if (!info.isFile() || info.isSymLink() || (info.fileName() != QLatin1String("manifest.json") && !info.fileName().endsWith(".manifest.json"))) continue;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) continue;
            const QByteArray contents = file.read(512 * 1024 + 1);
            if (file.error() != QFileDevice::NoError || contents.size() > 512 * 1024) continue;
            const auto manifest = QJsonDocument::fromJson(contents).object();
            if (manifest.value("id").toString().isEmpty())
                continue;
            QVariantMap row;
            row["id"] = manifest.value("id").toString();
            row["name"] = manifest.value("name").toString();
            row["kinds"] = manifest.value("kinds").toArray().toVariantList();
            row["firstParty"] = root == env.omarchyPath + "/shell/plugins";
            row["canDisable"] = false;
            row["unknownState"] = true;
            rows << row;
        }
    }
    return rows;
}

QVariantMap PluginsProvider::parseLayout(const QByteArray &shellJson) {
    const auto layout = QJsonDocument::fromJson(FileOps::stripJsonComments(shellJson)).object().value("bar").toObject().value("layout").toObject();
    QVariantMap sections;
    for (const char *section : {"left", "centerBegin", "center", "centerEnd", "right", "tray"}) {
        QVariantList widgets;
        for (const auto &value : layout.value(QLatin1String(section)).toArray())
            // Omarchy accepts a bare id as well as an object with options.
            widgets << (value.isString() ? value.toString() : value.toObject().value("id").toString());
        sections.insert(QLatin1String(section), widgets);
    }
    return sections;
}

QVariantMap PluginsProvider::resolveDockApp(const Env &env, const QString &desktopId, const QString &iconTheme) {
    QVariantMap app{{"id", desktopId}, {"name", QString()}, {"icon", QString()}, {"installed", false}};
    if (!IconThemes::isValidId(desktopId))
        return app;
    QByteArray entry;
    QStringList roots{env.dataHome};
    roots += env.dataDirs;
    // The first one found is the one the desktop uses, so a user's copy hides the system's.
    for (const QString &root : std::as_const(roots)) {
        if (FileOps::readFile(root + "/applications/" + desktopId + ".desktop", &entry)) {
            app["installed"] = true;
            break;
        }
    }
    if (!app.value("installed").toBool())
        return app;
    const QString group = QStringLiteral("Desktop Entry");
    const QString locale = QLocale::system().name();
    for (const QString &key : {"Name[" + locale + "]", "Name[" + locale.section('_', 0, 0) + "]", QStringLiteral("Name")}) {
        const FileOps::IniValue name = FileOps::iniGet(entry, group, key);
        if (name.found && !name.value.isEmpty()) {
            app["name"] = name.value;
            break;
        }
    }
    app["icon"] = IconThemes::appIconUrl(env, iconTheme, {FileOps::iniGet(entry, group, QStringLiteral("Icon")).value}, 96);
    return app;
}

void PluginsProvider::resolveDockApps() {
    const Env &env = m_runner->env();
    const QString theme = m_iconTheme.isEmpty() ? IconThemes::desktopDefault(env) : m_iconTheme;
    m_dockApps.clear();
    for (const QString &pin : std::as_const(m_pins))
        m_dockApps << resolveDockApp(env, pin, theme);
}

void PluginsProvider::loadConfig() {
    m_viewFingerprint = FileOps::fingerprint(m_runner->env().configHome + "/omarchy/shell.json");
    QByteArray contents;
    if (!FileOps::readFile(m_runner->env().configHome + "/omarchy/shell.json", &contents) || contents.trimmed().isEmpty())
        FileOps::readFile(m_runner->env().omarchyPath + "/config/omarchy/shell.json", &contents);
    m_layout = parseLayout(contents);
    m_activeBar = QJsonDocument::fromJson(FileOps::stripJsonComments(contents)).object().value("bar").toObject().value("id").toString();
    m_pins.clear();
    QJsonValue pins = FileOps::jsonGet(contents, {"dock", "pinned"});
    // A config without pins of its own uses the default ones, as omarchy-dock does.
    if (!pins.isArray()) {
        QByteArray defaults;
        FileOps::readFile(m_runner->env().omarchyPath + "/config/omarchy/shell.json", &defaults);
        pins = FileOps::jsonGet(defaults, {"dock", "pinned"});
    }
    for (const auto &value : pins.toArray())
        m_pins << value.toString();
    resolveDockApps();
}


void PluginsProvider::readState(const QString &scope, std::function<void(QJsonObject)> done) {
    const int generation = ++m_readGeneration;
    auto state = std::make_shared<QJsonObject>();
    (*state)["scope"] = scope;
    if (scope != QLatin1String("plugins")) {
        (*state)["files"] = readFiles(m_runner->env());
        (*state)["dock"] = dockState(state->value("files").toObject());
    }
    const bool plugins = scope == QLatin1String("plugins") || scope == QLatin1String("status") || scope == QLatin1String("bar");
    const bool live = scope == QLatin1String("bar") || scope == QLatin1String("status");
    auto pending = std::make_shared<int>(int(plugins) + int(live));
    auto finished = [this, state, generation, done] {
        if (state->contains("live")) {
            const auto liveState = state->value("live").toObject();
            (*state)["bar"] = liveState.value("status").toString() == QLatin1String("ok") ? layoutState(liveState.value("value").toObject())
                : unavailable(tr("The running shell's bar configuration is unavailable."));
        }
        if (generation == m_readGeneration) presentState(*state);
        if (done) done(*state);
    };
    if (!*pending) { finished(); return; }
    if (plugins) m_runner->run({"omarchy", "plugin", "list", "--json"}, CommandOptions{4000}, this,
        [this, state, pending, finished](const CommandResult &read) {
            const auto doc = QJsonDocument::fromJson(read.out);
            const bool ok = read.ok() && !read.truncated && pluginRows(doc);
            QJsonObject plugins{{"status", ok ? "ok" : "unavailable"}, {"source", QJsonArray{"omarchy", "plugin", "list", "--json"}},
                                {"items", ok ? QJsonValue(doc.array()) : QJsonValue(QJsonValue::Null)}};
            if (!ok) {
                plugins["reason"] = tr("The plugin registry could not be read completely as valid, unambiguous JSON.");
                plugins["manifestHints"] = QJsonArray::fromVariantList(scanManifests(m_runner->env()));
                plugins["manifestHintScope"] = "Bounded, unverified file hints only; enabled state and inventory completeness are unknown.";
            }
            (*state)["plugins"] = plugins;
            if (--*pending == 0) finished();
        });
    if (live) m_runner->run({"omarchy-shell", "shell", "listShellConfig"}, CommandOptions{4000}, this,
        [state, pending, finished](const CommandResult &read) {
            const auto doc = QJsonDocument::fromJson(read.out);
            const bool ok = read.ok() && !read.truncated && doc.isObject() && doc.object().value("version").toDouble() == 1;
            (*state)["live"] = ok ? QJsonObject{{"status", "ok"}, {"source", QJsonArray{"omarchy-shell", "shell", "listShellConfig"}}, {"value", doc.object()}}
                : unavailable(tr("The running shell configuration could not be read completely as version 1 JSON."));
            if (--*pending == 0) finished();
        });
}

void PluginsProvider::inspect(const QString &scope, std::function<void(QJsonObject)> done) {
    readState(scope, [scope, done](const QJsonObject &state) { done(publicState(state, scope)); });
}

void PluginsProvider::presentState(const QJsonObject &state) {
    if (state.contains("plugins")) {
        const auto plugins = state.value("plugins").toObject();
        m_status = plugins.value("status").toString() == QLatin1String("ok") ? "ok" : "shell-down";
        m_plugins = m_status == QLatin1String("ok") ? plugins.value("items").toArray().toVariantList() : plugins.value("manifestHints").toArray().toVariantList();
    }
    const auto bar = state.value("bar").toObject();
    if (bar.value("status").toString() == QLatin1String("ok")) {
        m_layout = parseLayout(QJsonDocument(QJsonObject{{"bar", QJsonObject{{"layout", bar.value("layout")}}}}).toJson());
        m_activeBar = bar.value("activeBar").toString();
        m_viewFingerprint = state.value("files").toObject().value("user").toObject().value("fingerprint").toString();
    }
    const auto dock = state.value("dock").toObject();
    if (dock.value("status").toString() == QLatin1String("ok")) {
        m_pins.clear(); for (const auto &pin : dock.value("pins").toArray()) m_pins << pin.toString();
        resolveDockApps();
    }
    emit changed();
}

void PluginsProvider::refresh() {
    loadConfig(); // Immediate presentation; completed API reads carry their own states.
    emit changed();
    readState("status", {});
    m_runner->run({"gsettings", "get", "org.gnome.desktop.interface", "icon-theme"}, CommandOptions{4000}, this, [this](const CommandResult &result) {
        bool ok = false;
        const QString theme = result.ok() && !result.truncated ? IconThemes::parseGVariantString(result.out, &ok) : QString();
        if (ok && theme != m_iconTheme) { m_iconTheme = theme; resolveDockApps(); emit changed(); }
    });
}

ProviderPlan PluginsProvider::basePlan(const QJsonObject &state, const QString &operation, const QString &target) const {
    ProviderPlan plan; plan.operation = operation; plan.target = target;
    auto refuse = [&](const QString &code, const QString &error) { plan.errorCode = code; plan.error = error; return plan; };
    const Env &env = m_runner->env();
    if (!safeWritePath(env)) return refuse("ownership_conflict", tr("Linked, hard-linked or unowned shell configuration paths cannot be changed here."));
    const auto files = state.value("files").toObject(), source = files.value("source").toObject();
    if (source.value("status").toString() != QLatin1String("ok")) return refuse("read_unavailable", tr("The source shell configuration is unreadable or unsupported. Nothing was changed."));
    const auto config = source.value("value").toObject();
    if (layoutState(config).value("status").toString() != QLatin1String("ok")
        || (config.contains("plugins") && !config.value("plugins").isArray())
        || (config.contains("disabledPlugins") && !config.value("disabledPlugins").isArray()))
        return refuse("unsupported_configuration", tr("The source configuration has missing or malformed layout/plugin fields; normalizing it could change unrelated choices."));
    const QString compatibility = Compat(env).changedForKey(operation);
    if (!compatibility.isEmpty()) return refuse("compatibility_blocked", tr("%1 changed since testing. Review compatibility before changing the shell configuration.").arg(compatibility));
    QJsonObject fingerprints;
    for (const auto &key : {"user", "defaults"}) {
        const auto file = files.value(key).toObject();
        if (file.contains("fingerprint")) fingerprints[file.value("path").toString()] = file.value("fingerprint");
    }
    plan.details = {{"fingerprints", fingerprints}, {"files", QJsonArray{env.configHome + "/omarchy/shell.json"}},
                    {"historyUndo", false}, {"failureRecovery", "Captured file backups are retained for review; no automatic restore of writes made by an external shell process."}};
    plan.effects = {tr("Omarchy's writer changes shell.json and may normalize its structure."),
                    tr("A failed or unverified action can leave partial changes. Captured backups are retained; there is no automatic history undo or file restoration.")};
    if (operation != QLatin1String("dock.pin")) {
        const auto live = state.value("live").toObject();
        if (live.value("status").toString() != QLatin1String("ok")) return refuse("read_unavailable", tr("The running shell configuration is unavailable."));
        if (live.value("value") != source.value("value")) return refuse("state_conflict", tr("The running shell and persisted configuration differ. Wait for them to settle and read again."));
        plan.details["liveFingerprint"] = FileOps::fingerprintBytes(QJsonDocument(live.value("value").toObject()).toJson(QJsonDocument::Compact));
        plan.details["scope"] = operation.startsWith("widgets.") ? "bar" : "status";
    } else {
        plan.details["scope"] = "dock";
    }
    plan.ok = true; return plan;
}

void PluginsProvider::planEnabled(const QString &id, bool enabled, std::function<void(ProviderPlan)> done) {
    if (!isSafeId(id)) { ProviderPlan invalid; invalid.operation = "plugins.enabled"; invalid.error = tr("Use a concrete plugin ID from plugin list."); done(invalid); return; }
    readState("status", [this, id, enabled, done](const QJsonObject &state) {
        auto plan = basePlan(state, "plugins.enabled", id);
        if (!plan.ok) { done(plan); return; }
        const auto registry = state.value("plugins").toObject();
        if (registry.value("status").toString() != QLatin1String("ok")) { plan.ok = false; plan.errorCode = "read_unavailable"; plan.error = tr("The plugin registry is unavailable."); done(plan); return; }
        QJsonObject selected;
        for (const auto &item : registry.value("items").toArray()) if (item.toObject().value("id").toString() == id) selected = item.toObject();
        if (selected.isEmpty() || (!enabled && !selected.value("canDisable").toBool())) {
            plan.ok = false; plan.errorCode = selected.isEmpty() ? "unknown_target" : "unsupported_operation";
            plan.error = selected.isEmpty() ? tr("The plugin is no longer available.") : tr("This plugin has no disable operation. Select another bar to replace the active bar."); done(plan); return;
        }
        plan.details["plugin"] = selected; plan.details["enabled"] = enabled;
        plan.details["verificationScope"] = "registry-state-and-persisted-configuration";
        plan.before = selected.value("enabled").toBool() ? "enabled" : "disabled"; plan.after = enabled ? "enabled" : "disabled";
        plan.effects << tr("Enabling a bar selects it. Clone operations can replace or restore the source implementation; widget enablement controls placement, not component loadability.");
        plan.effects << tr("Plugin code can load or unload within the shell.");
        done(plan);
    });
}

bool PluginsProvider::isWidgetSelector(const QString &selector) {
    static const QRegularExpression pattern("\\A(left|centerBegin|center|centerEnd|right|tray):(0|[1-9][0-9]*)@[a-f0-9]{64}\\z");
    bool ok = false; selector.section(':', 1).section('@', 0, 0).toInt(&ok);
    return pattern.match(selector).hasMatch() && ok;
}

ProviderPlan PluginsProvider::movePlan(const QJsonObject &state, const QString &selector, const QString &section, int index) const {
    auto plan = basePlan(state, "widgets.move", selector);
    if (!plan.ok) return plan;
    const QString sourceSection = selector.section(':', 0, 0);
    const int from = selector.section(':', 1).section('@', 0, 0).toInt();
    const auto live = state.value("live").toObject().value("value").toObject();
    const auto bar = layoutState(live);
    if (bar.value("status").toString() != QLatin1String("ok") || selector.section('@', 1) != widgetDigest(live)) {
        plan.ok = false; plan.errorCode = "stale_target"; plan.error = tr("The widget selector is stale or the layout is unavailable. Read bar status again."); return plan;
    }
    auto layout = bar.value("layout").toObject();
    auto source = layout.value(sourceSection).toArray();
    if (from < 0 || from >= source.size()) { plan.ok = false; plan.errorCode = "stale_target"; plan.error = tr("There is no widget at that source position."); return plan; }
    const auto entry = source.at(from); const QString id = widgetId(entry);
    if (section == QLatin1String("tray")) {
        const auto plugins = state.value("plugins").toObject();
        if (plugins.value("status").toString() != QLatin1String("ok")) {
            plan.ok = false; plan.errorCode = "read_unavailable"; plan.error = tr("The plugin registry is needed to check tray placement."); return plan;
        }
        bool tray = id == QLatin1String("omarchy.tray");
        for (const auto &item : plugins.value("items").toArray()) if (item.toObject().value("id").toString() == id)
            tray |= item.toObject().value("clonedFrom").toString() == QLatin1String("omarchy.tray");
        if (tray) { plan.ok = false; plan.error = tr("The tray cannot go in its own drawer."); return plan; }
    }
    source.removeAt(from); layout[sourceSection] = source;
    auto destination = layout.value(section).toArray();
    if (index < 0 || index > destination.size()) { plan.ok = false; plan.error = tr("The destination index is outside the layout after removing the source widget."); return plan; }
    destination.insert(index, entry); layout[section] = destination;
    plan.details["id"] = id; plan.details["fromSection"] = sourceSection; plan.details["fromIndex"] = from;
    plan.details["section"] = section; plan.details["index"] = index; plan.details["layoutAfter"] = layout;
    plan.details["verificationScope"] = "live-and-persisted-layout-with-instance-options";
    plan.before = sourceSection + '[' + QString::number(from) + ']'; plan.after = section + '[' + QString::number(index) + ']';
    return plan;
}

void PluginsProvider::planMove(const QString &selector, const QString &section, int index, std::function<void(ProviderPlan)> done) {
    if (!isWidgetSelector(selector) || !sections().contains(section) || index < 0) {
        ProviderPlan invalid; invalid.operation = "widgets.move"; invalid.error = tr("Use a current widget selector, a bar section and a nonnegative destination index."); done(invalid); return;
    }
    readState("bar", [this, selector, section, index, done](const QJsonObject &state) { done(movePlan(state, selector, section, index)); });
}

void PluginsProvider::planOption(const QString &selector, const QString &key, bool value, std::function<void(ProviderPlan)> done) {
    if (!isWidgetSelector(selector) || key != "showPercentage") {
        ProviderPlan invalid; invalid.operation = "widgets.option"; invalid.error = tr("Use a current Power widget selector and showPercentage with true or false."); done(invalid); return;
    }
    readState("bar", [this, selector, key, value, done](const QJsonObject &state) {
        const QString section = selector.section(':', 0, 0);
        const int index = selector.section(':', 1).section('@', 0, 0).toInt();
        // A move to the same position validates the selector and preserves its
        // entire entry. The option writer below never invokes a move.
        auto plan = movePlan(state, selector, section, index);
        plan.operation = "widgets.option";
        if (!plan.ok) { done(plan); return; }
        const QString drift = Compat(m_runner->env()).changedForKey(plan.operation);
        if (!drift.isEmpty()) { plan.ok = false; plan.errorCode = "compatibility_blocked"; plan.error = tr("%1 changed since testing. Review compatibility before changing the shell configuration.").arg(drift); done(plan); return; }
        auto layout = plan.details.value("layoutAfter").toObject(); auto entries = layout.value(section).toArray();
        const auto raw = entries[index]; auto entry = raw.toObject();
        if (state.value("plugins").toObject().value("status") != "ok") {
            plan.ok = false; plan.errorCode = "read_unavailable"; plan.error = tr("The plugin registry is unavailable."); done(plan); return;
        }
        bool builtin = false;
        for (const auto &item : state.value("plugins").toObject().value("items").toArray()) {
            const auto plugin = item.toObject();
            if (plugin.value("id") == "omarchy.power") builtin = plugin.value("firstParty").toBool() && plugin.value("enabled").toBool() && plugin.value("kinds").toArray().contains("bar-widget");
        }
        if (widgetId(raw) != "omarchy.power" || !builtin || !raw.isObject() || (entry.contains(key) && !entry.value(key).isBool())) {
            plan.ok = false; plan.errorCode = "unsupported_configuration";
            plan.error = tr("This option requires an enabled built-in Power widget with an object entry and a boolean value. Custom widgets and custom option formats are left alone."); done(plan); return;
        }
        const auto before = entry.value(key);
        entry[key] = value; entries[index] = entry; layout[section] = entries;
        auto expectedBar = state.value("live").toObject().value("value").toObject().value("bar").toObject();
        expectedBar["layout"] = layout;
        plan.details["key"] = key; plan.details["value"] = value; plan.details["valueBefore"] = before.isUndefined() ? QJsonValue(QJsonValue::Null) : before;
        plan.details["defaultWhenOmitted"] = false; plan.details["layoutAfter"] = layout;
        plan.details["barAfter"] = expectedBar;
        plan.details["verificationScope"] = "live-and-persisted-widget-option-and-preserved-layout; no rendering claim";
        plan.before = before.isUndefined() ? "inherited false" : before.toBool() ? "true" : "false";
        plan.after = value ? "true" : "false";
        done(plan);
    });
}

void PluginsProvider::planPin(const QString &input, bool pinned, std::function<void(ProviderPlan)> done) {
    const QString id = input.endsWith(".desktop") ? input.chopped(8) : input;
    if (!isSafeId(id)) { ProviderPlan invalid; invalid.operation = "dock.pin"; invalid.error = tr("Use a concrete desktop ID from apps list or dock status."); done(invalid); return; }
    readState("dock", [this, id, pinned, done](const QJsonObject &state) {
        auto plan = basePlan(state, "dock.pin", id);
        if (!plan.ok) { done(plan); return; }
        const auto dock = state.value("dock").toObject();
        if (dock.value("status").toString() != QLatin1String("ok")) { plan.ok = false; plan.errorCode = "read_unavailable"; plan.error = dock.value("reason").toString(); done(plan); return; }
        if (pinned) {
            QString error; QJsonObject app;
            for (const auto &item : SystemProvider::readApps(m_runner->env(), &error)) if (item.toMap().value("id").toString() == id + ".desktop") app = QJsonObject::fromVariantMap(item.toMap());
            if (!error.isEmpty() || app.isEmpty() || !app.value("available").toBool()) {
                plan.ok = false; plan.errorCode = "unknown_target"; plan.error = tr("That app is unavailable. Read apps list again."); done(plan); return;
            }
            plan.details["app"] = app;
        }
        QJsonArray after; bool found = false;
        for (const auto &pin : dock.value("pins").toArray()) {
            if (pin.toString() == id) { found = true; if (pinned) after.append(pin); }
            else after.append(pin);
        }
        if (pinned && !found) after.append(id);
        plan.details["pinned"] = pinned; plan.details["pinsAfter"] = after;
        plan.details["verificationScope"] = "persisted-preference-only";
        plan.before = compact(dock.value("pins")); plan.after = compact(after);
        plan.effects << tr("Write the ordered dock preference. Missing apps can be unpinned; rendering and app launching are not verified.");
        done(plan);
    });
}

void PluginsProvider::reject(const ProviderPlan &plan, bool alreadyChanging) {
    if (alreadyChanging) --m_changes;
    Transaction tx; tx.settingId = "plugins"; tx.phase = "failed"; tx.message = plan.error;
    emit message(plan.error, true); emit operationFinished(tx); emit changed();
}

void PluginsProvider::planLifecycle(const QString &id, bool clone, std::function<void(ProviderPlan)> done) {
    if (!isSafeId(id) || id.size() > 128) { ProviderPlan invalid; invalid.error = tr("Use a concrete plugin ID from plugin list."); done(invalid); return; }
    readState("status", [this, id, clone, done](const QJsonObject &state) {
        auto plan = basePlan(state, clone ? "plugins.clone" : "plugins.remove", id);
        auto refuse = [&](const QString &message) { plan.ok = false; plan.errorCode = "unsupported_target"; plan.error = message; done(plan); };
        QString error;
        if (!plan.ok) { done(plan); return; }
        if (!PluginFiles::safeRoot(m_runner->env(), &error)) { refuse(error); return; }
        const auto registry = state.value("plugins").toObject();
        if (registry.value("status") != "ok") { refuse(tr("The plugin registry is unavailable.")); return; }
        QJsonObject selected;
        for (const auto &v : registry.value("items").toArray()) if (v.toObject().value("id") == id) selected = v.toObject();
        if (clone && (!selected.value("firstParty").toBool() || !id.startsWith("omarchy."))) { refuse(tr("Only a discovered built-in plugin can be cloned.")); return; }
        if (!clone && id.startsWith("omarchy.")) { refuse(tr("Built-in plugins cannot be removed.")); return; }
        QString targetId = id;
        if (clone) {
            QString user = m_runner->env().process.value("USER");
            if (user.isEmpty()) { const auto *pw = ::getpwuid(geteuid()); if (pw) user = QString::fromLocal8Bit(pw->pw_name); }
            if (!isSafeId(user) || user == "omarchy" || user.size() > 64) { refuse(tr("The helper's username namespace is not a safe local plugin ID.")); return; }
            targetId = user + "." + id.mid(8);
        }
        const QString target = m_runner->env().configHome + "/omarchy/plugins/" + targetId;
        const auto before = PluginFiles::inspect(target, true, &error);
        if (before.isEmpty()) { refuse(error); return; }
        const auto entries = before.value("entries").toArray();
        if (clone ? !entries.isEmpty() : entries.isEmpty()) { refuse(clone ? tr("The clone destination already exists.") : tr("That plugin has no installed directory or link.")); return; }
        if (!entries.isEmpty() && entries.first().toObject().value("type") != "directory" && entries.first().toObject().value("type") != "link") { refuse(tr("The plugin target is not a directory or symbolic link.")); return; }
        plan.details["scope"] = "status"; plan.details["targetId"] = targetId; plan.details["targetPath"] = target;
        plan.details["treeBefore"] = before.value("fingerprint"); plan.details["plugin"] = selected;
        plan.details["verificationScope"] = "plugin-files-registry-and-persisted-live-configuration";
        plan.details["failureRecovery"] = "Private file/link/tree capture; explicit plugin recover refuses later edits and preserves displaced content. Interrupted writers without a post-write snapshot need manual review.";
        plan.before = clone ? "absent" : "installed"; plan.after = clone ? "cloned-and-enabled" : "removed";
        plan.confirmText = clone ? tr("The clone enables local plugin code and replaces its source implementation.") : tr("Removing this plugin can unload shell code. A Git checkout is deleted by Omarchy; a private snapshot is retained.");
        plan.effects << plan.confirmText << tr("Recovery restores captured files and reloads the shell. Omarchy's own hidden directory backups are retained.");
        if (!clone) { done(plan); return; }
        m_runner->run({"omarchy-plugin-catalog"}, CommandOptions{4000}, this, [this, plan, done](const CommandResult &read) mutable {
            auto refuse = [&](const QString &message) { plan.ok = false; plan.errorCode = "unsupported_source"; plan.error = message; done(plan); };
            const auto doc = QJsonDocument::fromJson(read.out);
            if (!read.ok() || read.truncated || !doc.isArray() || doc.array().size() > 512) { refuse(tr("The plugin source catalog is unavailable.")); return; }
            QJsonObject source; int matches = 0;
            for (const auto &value : doc.array()) if (value.toObject().value("id") == plan.target) { source = value.toObject(); ++matches; }
            const QString path = source.value("sourceDir").toString(), manifest = source.value("manifestPath").toString();
            const QString base = m_runner->env().omarchyPath + "/shell/plugins/";
            if (matches != 1 || !source.value("firstParty").toBool() || !path.startsWith(base) || QDir::cleanPath(path) != path
                || !manifest.startsWith(path + "/") || QDir::cleanPath(manifest) != manifest) { refuse(tr("The built-in source paths are ambiguous or outside the plugin tree.")); return; }
            QString error; const auto tree = PluginFiles::inspect(path, false, &error);
            if (tree.isEmpty()) { refuse(error); return; }
            for (const auto &value : tree.value("entries").toArray()) if (value.toObject().value("type") == "link") { refuse(tr("A clone source containing links needs manual review before Omarchy dereferences it.")); return; }
            for (QString parent = path; parent.startsWith(base); parent = QFileInfo(parent).absolutePath()) if (QFileInfo(parent).isSymLink()) { refuse(tr("The clone source has a linked parent.")); return; }
            QByteArray bytes;
            if (!FileOps::readFile(manifest, &bytes, &error)) { refuse(error); return; }
            const auto object = QJsonDocument::fromJson(bytes).object();
            auto safeRelative = [](const QJsonValue &v) { const QString p = v.toString(); return v.isString() && !p.isEmpty() && p.size() <= 1024 && !QDir::isAbsolutePath(p) && !p.contains("..") && !p.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")); };
            if (object.value("id") != plan.target || !object.value("entryPoints").isObject()) { refuse(tr("The clone manifest is malformed.")); return; }
            const auto entryPoints = object.value("entryPoints").toObject();
            for (auto it = entryPoints.begin(); it != entryPoints.end(); ++it) if (!safeRelative(it.value())) { refuse(tr("A clone entry point is outside the reviewed source paths.")); return; }
            const auto clonePaths = object.value("omarchy").toObject().value("clonePaths");
            if (!clonePaths.isUndefined() && !clonePaths.isArray()) { refuse(tr("The clone paths are malformed.")); return; }
            for (const auto &v : clonePaths.toArray()) if (!v.isObject() || !safeRelative(v.toObject().value("source")) || !safeRelative(v.toObject().value("target"))) { refuse(tr("A clone path is outside the reviewed source paths.")); return; }
            plan.details["sourcePath"] = path; plan.details["sourceFingerprint"] = tree.value("fingerprint"); plan.details["sourceManifest"] = manifest;
            done(plan);
        });
    });
}

void PluginsProvider::planRecovery(const QString &token, std::function<void(ProviderPlan)> done) {
    readState("status", [this, token, done](const QJsonObject &state) {
        ProviderPlan plan; plan.operation = "plugins.recover"; plan.target = token;
        const QString compatibility = Compat(m_runner->env()).changedForKey("plugins.recover");
        if (!safeWritePath(m_runner->env()) || !compatibility.isEmpty()) {
            plan.errorCode = "recovery_unavailable"; plan.error = tr("Recovery paths or the shell helper need review before writing."); done(plan); return;
        }
        // Recovery is useful precisely when persisted/live state disagrees or
        // the failed writer left invalid JSON. Its captured byte guards govern.
        QJsonObject fingerprints;
        const auto files = state.value("files").toObject();
        for (const auto &key : {"user", "defaults"}) { const auto file = files.value(key).toObject(); if (file.contains("fingerprint")) fingerprints[file.value("path").toString()] = file.value("fingerprint"); }
        plan.details = {{"fingerprints", fingerprints}, {"files", QJsonArray{m_runner->env().configHome + "/omarchy/shell.json"}}, {"historyUndo", false}, {"failureRecovery", "Captured files and any displaced content are retained; explicit recovery can resume while byte guards still match."}};
        QString error; const auto receipt = PluginFiles::load(m_runner->env(), token, &error);
        if (receipt.isEmpty() || !PluginFiles::canRecover(m_runner->env(), receipt, &error)) { plan.ok = false; plan.errorCode = "recovery_conflict"; plan.error = error; done(plan); return; }
        plan.details["scope"] = "status"; plan.details["targetId"] = receipt.value("pluginId");
        plan.details["receiptFingerprint"] = FileOps::fingerprintBytes(QJsonDocument(receipt).toJson());
        plan.details["verificationScope"] = "captured-plugin-files-and-reloaded-shell-configuration";
        plan.confirmText = tr("Restore the captured plugin and shell configuration, preserve displaced content and reload plugin code.");
        plan.before = receipt.value("phase").toString(); plan.after = "recovered"; plan.ok = true; done(plan);
    });
}

void PluginsProvider::applyPlan(const ProviderPlan &plan) { executePlan(plan, false); }

void PluginsProvider::executePlan(const ProviderPlan &plan, bool alreadyChanging) {
    if (!plan.ok) { reject(plan, alreadyChanging); return; }
    if (!alreadyChanging) ++m_changes;
    emit changed();
    const bool lifecycle = plan.operation == "plugins.clone" || plan.operation == "plugins.remove";
    const bool recovering = plan.operation == "plugins.recover";
    const QString token = recovering ? plan.target : "pr-" + QUuid::createUuid().toString(QUuid::Id128);
    auto receipt = std::make_shared<QJsonObject>();
    ChangeCoordinator::Request request;
    request.settingId = "plugins"; request.label = plan.operation.startsWith("widgets.") ? tr("Bar widget %1").arg(plan.details.value("id").toString())
        : plan.operation == QLatin1String("dock.pin") ? tr("Dock") : plan.target;
    request.before = plan.before; request.after = plan.after;
    request.files = {m_runner->env().configHome + "/omarchy/shell.json"};
    request.restoreFilesOnFailure = false;
    request.extra = {{"operation", plan.operation}, {"target", plan.target}, {"verificationScope", plan.details.value("verificationScope")}, {"failureRecovery", plan.details.value("failureRecovery")}};
    if (lifecycle || recovering) request.extra["recoveryToken"] = token;
    const auto fingerprints = plan.details.value("fingerprints").toObject();
    for (auto it = fingerprints.begin(); it != fingerprints.end(); ++it) request.expectedFingerprints.insert(it.key(), it.value().toString());
    request.preflight = [this, plan](ChangeCoordinator::Done done) {
        auto checked = [plan, done](const ProviderPlan &fresh) {
            done(fresh.ok && fresh.toJson() == plan.toJson(), fresh.ok ? tr("The shell configuration or target changed after planning. Read and plan again.") : fresh.error);
        };
        if (plan.operation == QLatin1String("plugins.enabled")) planEnabled(plan.target, plan.details.value("enabled").toBool(), checked);
        else if (plan.operation == "plugins.clone" || plan.operation == "plugins.remove") planLifecycle(plan.target, plan.operation == "plugins.clone", checked);
        else if (plan.operation == "plugins.recover") planRecovery(plan.target, checked);
        else if (plan.operation == QLatin1String("dock.pin")) planPin(plan.target, plan.details.value("pinned").toBool(), checked);
        else if (plan.operation == "widgets.option") planOption(plan.target, plan.details.value("key").toString(), plan.details.value("value").toBool(), checked);
        else planMove(plan.target, plan.details.value("section").toString(), plan.details.value("index").toInt(), checked);
    };
    request.apply = [this, plan, lifecycle, recovering, token, receipt](ChangeCoordinator::Done done) {
        if (!safeWritePath(m_runner->env())) { done(false, tr("The shell configuration path changed before writing.")); return; }
        QStringList argv;
        if (recovering) {
            QString error; *receipt = PluginFiles::load(m_runner->env(), token, &error);
            if (receipt->isEmpty() || !PluginFiles::recover(m_runner->env(), receipt.get(), &error)) { done(false, error); return; }
            CommandOptions options; options.mutation = true; options.timeoutMs = 10000;
            m_runner->run({"omarchy-shell", "shell", "rescanPlugins"}, options, this, [this, options, done](const CommandResult &rescan) {
                if (!rescan.ok() || rescan.truncated) { done(false, tr("Files recovered, but the shell did not acknowledge plugin rescan. The recovery can be retried.")); return; }
                m_runner->run({"omarchy-shell", "shell", "reloadConfig"}, options, this, [done](const CommandResult &reload) { done(reload.ok() && !reload.truncated, tr("Files recovered; shell reload was not confirmed.")); });
            });
            return;
        }
        if (lifecycle) {
            QString error;
            if (!PluginFiles::capture(m_runner->env(), token, plan.details.value("targetId").toString(), receipt.get(), &error)) { done(false, error); return; }
            if (receipt->value("before").toObject().value("fingerprint") != plan.details.value("treeBefore")
                || (plan.operation == "plugins.clone" && PluginFiles::inspect(plan.details.value("sourcePath").toString(), false, &error).value("fingerprint") != plan.details.value("sourceFingerprint"))) { done(false, tr("The plugin tree changed after planning; no helper was run.")); return; }
            argv = {"omarchy", "plugin", plan.operation == "plugins.clone" ? "clone" : "remove", plan.target};
            if (plan.operation == "plugins.remove") argv << "--yes";
        }
        else if (plan.operation == QLatin1String("plugins.enabled")) argv = {"omarchy", "plugin", plan.details.value("enabled").toBool() ? "enable" : "disable", plan.target};
        else if (plan.operation == QLatin1String("dock.pin")) argv = {"omarchy", "dock", plan.details.value("pinned").toBool() ? "pin" : "unpin", plan.target};
        else if (plan.operation == "widgets.option") argv = {"omarchy", "bar", "set", plan.details.value("id").toString(), plan.details.value("key").toString(), plan.details.value("value").toBool() ? "true" : "false", "--json",
                "--from-section", plan.details.value("fromSection").toString(), "--from-index", QString::number(plan.details.value("fromIndex").toInt())};
        else argv = {"omarchy", "bar", "move", plan.details.value("id").toString(), "--section", plan.details.value("section").toString(), "--index", QString::number(plan.details.value("index").toInt()),
                     "--from-section", plan.details.value("fromSection").toString(), "--from-index", QString::number(plan.details.value("fromIndex").toInt())};
        CommandOptions options; options.mutation = true; options.timeoutMs = 20000;
        m_runner->run(argv, options, this, [this, done, lifecycle, receipt](const CommandResult &read) {
            QString error;
            const bool writerOk = read.ok() && !read.truncated;
            const bool captured = !lifecycle || PluginFiles::finish(m_runner->env(), receipt.get(), writerOk, &error);
            done(writerOk && captured, !captured ? error : read.truncated ? tr("The shell writer's result was truncated.") : read.describe("Omarchy"));
        });
    };
    request.verify = [this, plan, lifecycle, recovering, receipt](ChangeCoordinator::Done done) {
        auto poll = std::make_shared<std::function<void(int)>>();
        // The callbacks keep polling alive only until a bounded attempt finishes.
        std::weak_ptr<std::function<void(int)>> weak = poll;
        *poll = [this, plan, done, weak, lifecycle, recovering, receipt](int remaining) {
            auto keep = weak.lock();
            readState(plan.details.value("scope").toString(), [this, plan, done, keep, remaining, lifecycle, recovering, receipt](const QJsonObject &state) {
                const auto files = state.value("files").toObject(), user = files.value("user").toObject();
                const auto live = state.value("live").toObject();
                const auto source = files.value("source").toObject();
                bool ok = user.value("status").toString() == QLatin1String("ok");
                if ((lifecycle || recovering) && user.value("status") == "absent") ok = source.value("status") == "ok";
                if (plan.operation == QLatin1String("dock.pin")) {
                    ok &= state.value("dock").toObject().value("status").toString() == QLatin1String("ok")
                        && state.value("dock").toObject().value("pins") == plan.details.value("pinsAfter");
                } else {
                    ok &= live.value("status").toString() == QLatin1String("ok") && live.value("value") == ((lifecycle || recovering) ? source.value("value") : user.value("value"));
                    if (recovering) {
                        QString error;
                        ok &= PluginFiles::inspect(m_runner->env().configHome + "/omarchy/plugins/" + receipt->value("pluginId").toString(), true, &error) == receipt->value("before").toObject()
                            && PluginFiles::inspect(m_runner->env().configHome + "/omarchy/shell.json", true, &error) == receipt->value("beforeConfig").toObject();
                    } else if (lifecycle) {
                        const auto plugins = state.value("plugins").toObject(); const QString target = plan.details.value("targetId").toString();
                        bool found = false, selected = false, sourceRestored = plan.details.value("plugin").toObject().value("clonedFrom").toString().isEmpty()
                            || !plan.details.value("plugin").toObject().value("enabled").toBool();
                        for (const auto &value : plugins.value("items").toArray()) {
                            const auto row = value.toObject();
                            if (row.value("id") == target) { found = true; selected = row.value("enabled").toBool() && persistedEnabled(user.value("value").toObject(), row).toBool(); }
                            if (row.value("id") == plan.details.value("plugin").toObject().value("clonedFrom") && row.value("enabled").toBool()) sourceRestored = true;
                        }
                        const QString path = plan.details.value("targetPath").toString();
                        ok &= plugins.value("status") == "ok";
                        if (plan.operation == "plugins.clone") {
                            QByteArray bytes; FileOps::readFile(path + "/manifest.json", &bytes);
                            const auto manifest = QJsonDocument::fromJson(bytes).object();
                            ok &= found && selected && manifest.value("id") == target && manifest.value("omarchy").toObject().value("clonedFrom") == plan.target;
                        } else ok &= !found && sourceRestored && !QFileInfo(path).exists() && !QFileInfo(path).isSymLink();
                    } else if (plan.operation == "widgets.option") ok &= live.value("value").toObject().value("bar") == plan.details.value("barAfter");
                    else if (plan.operation == "widgets.move") ok &= layoutState(live.value("value").toObject()).value("layout") == plan.details.value("layoutAfter");
                    else {
                        const auto plugins = state.value("plugins").toObject(); bool found = false;
                        for (const auto &item : plugins.value("items").toArray()) {
                            const auto row = item.toObject();
                            if (row.value("id").toString() == plan.target) found = row.value("enabled") == plan.details.value("enabled");
                        }
                        ok &= plugins.value("status").toString() == QLatin1String("ok") && found
                            && persistedEnabled(user.value("value").toObject(), plan.details.value("plugin").toObject()) == plan.details.value("enabled");
                    }
                }
                if (ok) {
                    if (lifecycle) { QString error; (*receipt)["phase"] = "committed"; if (!PluginFiles::save(m_runner->env(), *receipt, &error)) { done(false, error); return; } }
                    done(true, {}); return;
                }
                if (remaining <= 1) {
                    done(false, plan.operation == "widgets.option" ? tr("The widget option and preserved layout were not confirmed.") : plan.operation == QLatin1String("widgets.move") ? tr("The widget did not move.")
                         : plan.operation == QLatin1String("dock.pin") ? tr("The dock preference was not confirmed.") : tr("The plugin did not change state.")); return;
                }
                QTimer::singleShot(150, this, [keep, remaining] { (*keep)(remaining - 1); });
            });
        };
        (*poll)(8);
    };
    m_coordinator->submit(std::move(request), [this, plan](const Transaction &tx) {
        --m_changes;
        QString success = plan.operation == "plugins.clone" ? tr("Cloned %1 into your config").arg(plan.target)
            : plan.operation == "plugins.remove" ? tr("Removed %1").arg(plan.target) : plan.operation == "plugins.recover" ? tr("Recovered plugin files and shell configuration")
            : plan.operation == QLatin1String("widgets.move") ? tr("Moved %1").arg(plan.details.value("id").toString())
            : plan.operation == "widgets.option" ? tr("Updated %1").arg(plan.details.value("key").toString())
            : plan.operation == QLatin1String("dock.pin") ? (plan.details.value("pinned").toBool() ? tr("Pinned to the dock") : tr("Removed from the dock"))
            : plan.details.value("enabled").toBool() ? tr("%1 enabled").arg(plan.target) : tr("%1 disabled").arg(plan.target);
        emit message(tx.phase == QLatin1String("committed") ? success : tx.message, tx.phase != QLatin1String("committed"));
        emit operationFinished(tx);
        loadConfig(); emit changed();
        if (tx.phase != QLatin1String("committed")) readState(plan.details.value("scope").toString(), {});
    });
}

void PluginsProvider::setEnabled(const QString &id, bool enabled) {
    ++m_changes; emit changed();
    planEnabled(id, enabled, [this](const ProviderPlan &plan) { executePlan(plan, true); });
}

void PluginsProvider::clonePlugin(const QString &id) {
    ++m_changes; emit changed();
    planLifecycle(id, true, [this](const ProviderPlan &plan) { executePlan(plan, true); });
}

void PluginsProvider::removePlugin(const QString &id) {
    ++m_changes; emit changed();
    planLifecycle(id, false, [this](const ProviderPlan &plan) { executePlan(plan, true); });
}

void PluginsProvider::moveWidget(const QString &id, const QString &section, int index, const QString &fromSection, int fromIndex) {
    ++m_changes; emit changed();
    const QString displayed = m_viewFingerprint;
    if (!isSafeId(id) || !sections().contains(section) || !sections().contains(fromSection) || index < 0 || fromIndex < 0) {
        ProviderPlan invalid; invalid.error = tr("Use a concrete widget and valid source and destination positions."); reject(invalid, true); return;
    }
    readState("bar", [this, id, section, index, fromSection, fromIndex, displayed](const QJsonObject &state) {
        const auto live = state.value("live").toObject().value("value").toObject();
        const auto entries = layoutState(live).value("layout").toObject().value(fromSection).toArray();
        const auto entry = fromIndex < entries.size() ? entries.at(fromIndex) : QJsonValue{};
        if (widgetId(entry) != id || (!displayed.isEmpty() && state.value("files").toObject().value("user").toObject().value("fingerprint").toString() != displayed)) {
            ProviderPlan invalid; invalid.errorCode = "stale_target"; invalid.error = tr("The displayed widget changed. Read the current layout again."); reject(invalid, true); return;
        }
        executePlan(movePlan(state, widgetSelector(live, fromSection, fromIndex), section, index), true);
    });
}

void PluginsProvider::pin(const QString &desktopId) {
    ++m_changes; emit changed();
    planPin(desktopId, true, [this](const ProviderPlan &plan) { executePlan(plan, true); });
}

void PluginsProvider::unpin(const QString &desktopId) {
    ++m_changes; emit changed();
    planPin(desktopId, false, [this](const ProviderPlan &plan) { executePlan(plan, true); });
}
