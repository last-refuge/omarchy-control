#include "profiledesktop.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

#include "fileops.h"
#include "hypromodule.h"
#include "pluginsprovider.h"
#include "profilefiles.h"
#include "shortcutsprovider.h"
#include "systemprovider.h"
#include "windowrulesprovider.h"

namespace {

QString compact(const QJsonObject &object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString compact(const QJsonArray &array) {
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QJsonObject parseObject(const QString &json) {
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

QString slug(const QString &text) {
    QString out = text.toLower();
    out.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    while (out.startsWith('-'))
        out.remove(0, 1);
    while (out.endsWith('-'))
        out.chop(1);
    return out.left(64);
}

QJsonObject shortcutRecord(const QJsonObject &bind) {
    return QJsonObject{{"keys", ShortcutsProvider::canonicalKeys(bind.value("keys").toString())},
                       {"description", bind.value("description").toString()},
                       {"command", bind.value("command").toString()}};
}

QJsonObject ruleRecord(const QJsonObject &rule) {
    return QJsonObject{{"match", rule.value("match").toString()}, {"action", rule.value("action").toString()}, {"value", rule.value("value").toString()}};
}

QString kindOf(const QString &target) {
    return target.section(':', 0, 0);
}

QString keyOf(const QString &target) {
    return target.section(':', 1);
}

QString widgetId(const QJsonValue &value) {
    // Omarchy accepts a bare id as well as an object with options.
    return value.isString() ? value.toString() : value.toObject().value("id").toString();
}

QJsonObject layoutIn(const QByteArray &shellJson, bool *ok = nullptr) {
    const QJsonDocument document = QJsonDocument::fromJson(FileOps::stripJsonComments(shellJson));
    if (ok)
        *ok = document.isObject();
    return document.object().value("bar").toObject().value("layout").toObject();
}

// Omarchy's own plugins that are not widgets, and which of them are switched off. A widget
// is on or off by being on the bar, and the bar itself is chosen apart from plugins.
bool readPluginList(const QByteArray &json, QSet<QString> *switchable, QStringList *disabled) {
    const QJsonDocument list = QJsonDocument::fromJson(json);
    if (!list.isArray())
        return false;
    for (const QJsonValue &row : list.array()) {
        const QJsonObject plugin = row.toObject();
        const QString id = plugin.value("id").toString();
        const QJsonArray kinds = plugin.value("kinds").toArray();
        if (!plugin.value("firstParty").toBool() || !ProfileModel::isOmarchyPluginId(id) || kinds.contains(QJsonValue("bar-widget"))
            || kinds.contains(QJsonValue("bar")))
            continue;
        switchable->insert(id);
        if (!plugin.value("enabled").toBool(true))
            *disabled << id;
    }
    disabled->sort();
    return true;
}

QString shortId(const QString &id) {
    return id.startsWith(QLatin1String("omarchy.")) ? id.mid(8) : id;
}

}  // namespace

ProfileDesktop::ProfileDesktop(const Catalog *catalog, CommandRunner *runner, Capabilities *capabilities, ChangeCoordinator *coordinator,
                               Compat *compat, LauncherEditor *launchers, QObject *parent)
    : QObject(parent), m_catalog(catalog), m_runner(runner), m_capabilities(capabilities), m_coordinator(coordinator), m_compat(compat),
      m_launchers(launchers) {}

QStringList ProfileDesktop::commonMimes() {
    return SystemProvider::commonMimeTypes();
}

QString ProfileDesktop::themeFolder(const QString &name) {
    // As omarchy-theme-set does: markup out, lower case, spaces to dashes.
    QString folder = name;
    folder.remove(QRegularExpression(QStringLiteral("<[^>]+>")));
    return folder.toLower().replace(' ', '-');
}

QString ProfileDesktop::themeName(const QString &folder) {
    // As omarchy-theme-list shows it: each word capitalized, dashes to spaces.
    QStringList words = folder.split('-');
    for (QString &word : words)
        if (!word.isEmpty())
            word[0] = word[0].toUpper();
    return words.join(' ');
}

ProfileDesktop::ThemeState ProfileDesktop::themeState(const Env &env, const QString &folder) {
    ThemeState state;
    // omarchy-theme-list counts a folder or a link to one alike.
    const QString path = env.configHome + "/omarchy/themes/" + folder;
    state.installed = QFileInfo(path).isDir();
    state.builtin = QFileInfo(env.omarchyPath + "/themes/" + folder).isDir();
    if (!state.installed)
        return state;
    const QFileInfo config(path + "/.git/config");
    QByteArray bytes;
    if (config.isSymLink() || !config.isFile()) {
        state.problem = tr("It was not installed from Git, so a profile cannot bring it back.");
        return state;
    }
    if (config.size() > 64 * 1024 || !FileOps::readFile(config.absoluteFilePath(), &bytes)) {
        state.problem = tr("Its Git copy could not be read.");
        return state;
    }
    // [remote "origin"] url = ..., read as text: running git in a folder someone else's
    // repository filled could run what its configuration names.
    static const QRegularExpression origin(QStringLiteral("^\\[remote\\s+\"origin\"\\]$"), QRegularExpression::CaseInsensitiveOption);
    bool inOrigin = false;
    QString url;
    for (QString line : QString::fromUtf8(bytes).split('\n')) {
        line = line.trimmed();
        if (line.startsWith('[')) {
            inOrigin = origin.match(line).hasMatch();
            continue;
        }
        if (inOrigin && url.isEmpty() && line.section('=', 0, 0).trimmed().compare(QLatin1String("url"), Qt::CaseInsensitive) == 0) {
            url = line.section('=', 1).trimmed();
            if (url.size() >= 2 && url.startsWith('"') && url.endsWith('"'))
                url = url.mid(1, url.size() - 2);
        }
    }
    if (url.isEmpty()) {
        state.problem = tr("Its Git copy does not say where it came from, so a profile cannot bring it back.");
        return state;
    }
    state.url = themeSource(url, &state.problem);
    return state;
}

QHash<QString, ProfileDesktop::ThemeState> ProfileDesktop::readThemes(const Env &env) {
    QHash<QString, ThemeState> themes;
    for (const QString &root : {env.configHome + "/omarchy/themes", env.omarchyPath + "/themes"}) {
        for (const QFileInfo &entry : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            // A name a profile could not hold is no theme a profile can name.
            if (ProfileModel::isThemeFolder(entry.fileName()) && !themes.contains(entry.fileName()))
                themes.insert(entry.fileName(), themeState(env, entry.fileName()));
        }
    }
    return themes;
}

QString ProfileDesktop::themeSource(const QString &origin, QString *problem) {
    // SSH needs keys a fresh computer does not have yet, so an SSH address is carried in its
    // https form, only where that form is known to be the same repository.
    static const QStringList forges{"github.com", "gitlab.com", "codeberg.org", "bitbucket.org", "git.sr.ht"};
    static const QRegularExpression https(QStringLiteral("^https://([^/@]*@)?(.+)$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression scp(QStringLiteral("^[A-Za-z0-9._-]+@([A-Za-z0-9.-]+):([^/].*)$"));
    static const QRegularExpression ssh(QStringLiteral("^(ssh|git\\+ssh|ssh\\+git|git)://([A-Za-z0-9._-]+@)?([A-Za-z0-9.-]+)(:[0-9]+)?/(.+)$"));
    QString url;
    QRegularExpressionMatch match;
    if ((match = https.match(origin)).hasMatch()) {
        // A name or password in the address stays on this computer.
        url = "https://" + match.captured(2);
    } else if ((match = scp.match(origin)).hasMatch() && forges.contains(match.captured(1).toLower())) {
        url = "https://" + match.captured(1).toLower() + "/" + match.captured(2);
    } else if ((match = ssh.match(origin)).hasMatch() && forges.contains(match.captured(3).toLower())) {
        url = "https://" + match.captured(3).toLower() + "/" + match.captured(5);
    } else {
        if (problem)
            *problem = tr("It came from an address that is not https, so a profile cannot bring it back.");
        return {};
    }
    if (!ProfileModel::isThemeSource(url)) {
        if (problem)
            *problem = tr("It came from an address a profile cannot carry.");
        return {};
    }
    return url;
}

bool ProfileDesktop::sameSource(const QString &a, const QString &b) {
    const auto normal = [](QString url) {
        while (url.endsWith('/'))
            url.chop(1);
        if (url.endsWith(QLatin1String(".git")))
            url.chop(4);
        const int path = url.indexOf('/', 8);
        return path < 0 ? url.toLower() : url.left(path).toLower() + url.mid(path);
    };
    return !a.isEmpty() && !b.isEmpty() && normal(a) == normal(b);
}

QJsonObject ProfileDesktop::projectBar(const QJsonObject &layout, bool everySection) {
    QJsonObject out;
    for (const QString &section : ProfileModel::barSections()) {
        if (!everySection && !layout.contains(section))
            continue;
        QJsonArray widgets;
        for (const QJsonValue &value : layout.value(section).toArray()) {
            const QString id = widgetId(value);
            if (!ProfileModel::isOmarchyPluginId(id))
                continue;
            QJsonObject entry{{"id", id}};
            const QJsonObject options = value.toObject();
            for (auto it = options.begin(); it != options.end(); ++it)
                if (it.key() != QLatin1String("id") && ProfileModel::barOptionProblem(it.key(), it.value()).isEmpty())
                    entry.insert(it.key(), it.value());
            widgets.append(entry);
        }
        out.insert(section, widgets);
    }
    return out;
}

bool ProfileDesktop::barMatches(const QString &desired, const QString &current) {
    const QJsonObject want = parseObject(desired), have = parseObject(current);
    if (have.isEmpty())
        return false;
    for (auto it = want.begin(); it != want.end(); ++it)
        if (have.value(it.key()) != it.value())
            return false;
    return true;
}

QJsonObject ProfileDesktop::mergeBar(const QJsonObject &current, const QJsonObject &desired) {
    // Each of Omarchy's widgets already on the bar, in order, so one the profile places keeps
    // whatever it has here that a profile does not carry, and moves rather than appears twice.
    struct Placed {
        QString section;
        int index = 0;
        QJsonObject entry;
    };
    QHash<QString, QList<Placed>> existing;
    for (const QString &section : ProfileModel::barSections()) {
        int index = 0;
        for (const QJsonValue &value : current.value(section).toArray()) {
            if (ProfileModel::isOmarchyPluginId(widgetId(value)))
                existing[widgetId(value)] << Placed{section, index, value.isObject() ? value.toObject() : QJsonObject{{"id", widgetId(value)}}};
            ++index;
        }
    }
    QSet<QString> moved;  // "<section>/<index>" of each widget a placement took.
    QJsonObject out = current;  // A section Omarchy adds later stays as it is.
    for (const QString &section : ProfileModel::barSections()) {
        if (!desired.contains(section))
            continue;
        QJsonArray widgets;
        for (const QJsonValue &value : desired.value(section).toArray()) {
            const QJsonObject want = value.toObject();
            QJsonObject entry;
            if (!existing.value(widgetId(want)).isEmpty()) {
                const Placed had = existing[widgetId(want)].takeFirst();
                moved.insert(had.section + "/" + QString::number(had.index));
                for (auto it = had.entry.begin(); it != had.entry.end(); ++it)
                    if (!ProfileModel::barOptionKeys().contains(it.key()))
                        entry.insert(it.key(), it.value());
            }
            for (auto it = want.begin(); it != want.end(); ++it)
                entry.insert(it.key(), it.value());
            widgets.append(entry);
        }
        // Widgets from plugins installed separately keep their section, after these.
        for (const QJsonValue &value : current.value(section).toArray())
            if (!ProfileModel::isOmarchyPluginId(widgetId(value)))
                widgets.append(value);
        out.insert(section, widgets);
    }
    // A section the profile does not name keeps everything but what moved out of it.
    for (const QString &section : ProfileModel::barSections()) {
        if (desired.contains(section) || !current.contains(section))
            continue;
        QJsonArray kept;
        int index = 0;
        for (const QJsonValue &value : current.value(section).toArray())
            if (!moved.contains(section + "/" + QString::number(index++)))
                kept.append(value);
        out.insert(section, kept);
    }
    return out;
}

QString ProfileDesktop::userBackgroundsFolder(const QString &theme) const {
    return m_runner->env().configHome + "/omarchy/backgrounds/" + theme;
}

QString ProfileDesktop::appId(const QString &desktopId) {
    return desktopId.endsWith(QLatin1String(".desktop")) ? desktopId.chopped(8) : desktopId;
}

QString ProfileDesktop::shortcutId(const QString &keys) {
    return slug(ShortcutsProvider::canonicalKeys(keys));
}

QString ProfileDesktop::ruleId(const QJsonObject &rule) {
    return slug(rule.value("action").toString() + " " + rule.value("match").toString());
}

QStringList ProfileDesktop::targets(const QJsonObject &desktop) {
    QStringList list;
    const QJsonObject themes = desktop.value("themes").toObject();
    for (auto it = themes.begin(); it != themes.end(); ++it)
        list << "theme:" + it.key();
    if (desktop.contains("background"))
        list << QStringLiteral("background");
    if (desktop.contains("bar"))
        list << QStringLiteral("bar");
    if (desktop.contains("plugins"))
        list << QStringLiteral("plugins");
    if (desktop.contains("dockPins"))
        list << QStringLiteral("dock");
    const QJsonObject launchers = desktop.value("launchers").toObject();
    for (auto it = launchers.begin(); it != launchers.end(); ++it)
        list << "launcher:" + it.key();
    const QJsonObject associations = desktop.value("associations").toObject();
    for (auto it = associations.begin(); it != associations.end(); ++it)
        list << "association:" + it.key();
    for (const QJsonValue &value : desktop.value("shortcuts").toArray())
        list << "shortcut:" + value.toObject().value("id").toString();
    for (const QJsonValue &value : desktop.value("windowRules").toArray())
        list << "rule:" + value.toObject().value("id").toString();
    return list;
}

QStringList ProfileDesktop::fileTargets(const Profile &profile) {
    QStringList list;
    for (const QJsonValue &value : profile.files)
        list << "file:" + value.toObject().value("adapter").toString() + "/" + value.toObject().value("name").toString();
    return list;
}

QString ProfileDesktop::desiredIn(const Profile &profile, const QString &target) {
    if (kindOf(target) != QLatin1String("file"))
        return desired(profile.desktop, target);
    for (const QJsonValue &value : profile.files) {
        const QJsonObject file = value.toObject();
        if ("file:" + file.value("adapter").toString() + "/" + file.value("name").toString() == target)
            return file.value("sha256").toString();
    }
    return {};
}

QString ProfileDesktop::desired(const QJsonObject &desktop, const QString &target) {
    const QString kind = kindOf(target), key = keyOf(target);
    if (kind == QLatin1String("theme"))
        return desktop.value("themes").toObject().contains(key) ? compact(desktop.value("themes").toObject().value(key).toObject()) : QString();
    if (kind == QLatin1String("background"))
        return desktop.contains("background") ? compact(desktop.value("background").toObject()) : QString();
    if (kind == QLatin1String("bar"))
        return desktop.contains("bar") ? compact(projectBar(desktop.value("bar").toObject().value("layout").toObject(), false)) : QString();
    if (kind == QLatin1String("plugins")) {
        if (!desktop.contains("plugins"))
            return {};
        QStringList ids;
        for (const QJsonValue &id : desktop.value("plugins").toObject().value("disabled").toArray())
            ids << id.toString();
        ids.sort();
        return compact(QJsonArray::fromStringList(ids));
    }
    if (kind == QLatin1String("dock")) {
        QJsonArray pins;
        for (const QJsonValue &pin : desktop.value("dockPins").toArray())
            pins.append(appId(pin.toString()));
        return compact(pins);
    }
    if (kind == QLatin1String("launcher"))
        return desktop.value("launchers").toObject().contains(key) ? compact(desktop.value("launchers").toObject().value(key).toObject()) : QString();
    if (kind == QLatin1String("association"))
        return desktop.value("associations").toObject().value(key).toString();
    if (kind == QLatin1String("shortcut")) {
        for (const QJsonValue &value : desktop.value("shortcuts").toArray())
            if (value.toObject().value("id").toString() == key)
                return compact(shortcutRecord(value.toObject()));
    }
    if (kind == QLatin1String("rule")) {
        for (const QJsonValue &value : desktop.value("windowRules").toArray())
            if (value.toObject().value("id").toString() == key)
                return compact(ruleRecord(value.toObject()));
    }
    return {};
}

QString ProfileDesktop::current(const Snapshot &snapshot, const QString &target) {
    const QString kind = kindOf(target), key = keyOf(target);
    if (kind == QLatin1String("theme")) {
        // Where the user's copy came from, or that it has none a profile can name; for a
        // theme only Omarchy ships, just that.
        const ThemeState state = snapshot.themes.value(key);
        if (state.installed)
            return state.url.isEmpty() ? QStringLiteral("local") : compact(QJsonObject{{"url", state.url}});
        return state.builtin ? QStringLiteral("builtin") : QString();
    }
    if (kind == QLatin1String("background"))
        return snapshot.background.isEmpty() ? QString() : compact(snapshot.background);
    if (kind == QLatin1String("bar"))
        return snapshot.barOk ? compact(projectBar(snapshot.barLayout, true)) : QString();
    if (kind == QLatin1String("plugins"))
        return snapshot.pluginsOk ? compact(QJsonArray::fromStringList(snapshot.disabledPlugins)) : QString();
    if (kind == QLatin1String("file"))
        return snapshot.fileDigests.value(target);
    if (kind == QLatin1String("dock")) {
        QJsonArray pins;
        for (const QString &pin : snapshot.dockPins)
            pins.append(pin);
        return snapshot.dockOk ? compact(pins) : QString();
    }
    if (kind == QLatin1String("launcher")) {
        if (!snapshot.launchers.contains(key))
            return {};
        // An icon that is a file is known by its contents, as the profile knows it.
        QJsonObject fields = snapshot.launchers.value(key);
        if (snapshot.launcherIconDigests.contains(key))
            fields["icon"] = "sha256:" + snapshot.launcherIconDigests.value(key);
        return compact(fields);
    }
    if (kind == QLatin1String("association"))
        return snapshot.associations.value(key);
    if (kind == QLatin1String("shortcut"))
        return snapshot.shortcuts.contains(key) ? compact(snapshot.shortcuts.value(key)) : QString();
    if (kind == QLatin1String("rule"))
        return snapshot.rules.contains(key) ? compact(snapshot.rules.value(key)) : QString();
    return {};
}

bool ProfileDesktop::matches(const QString &target, const QString &desired, const QString &current) {
    // A theme is there when the user's copy came from the same place, or when Omarchy ships
    // one by that name, which is what the theme setting then selects.
    if (kindOf(target) == QLatin1String("theme"))
        return current == QLatin1String("builtin") || sameSource(parseObject(desired).value("url").toString(), parseObject(current).value("url").toString());
    if (kindOf(target) == QLatin1String("bar"))
        return barMatches(desired, current);
    if (kindOf(target) != QLatin1String("launcher"))
        return desired == current;
    // A launcher change names only the fields that were changed.
    const QJsonObject want = parseObject(desired), have = parseObject(current);
    if (have.isEmpty())
        return false;
    for (auto it = want.begin(); it != want.end(); ++it)
        if (have.value(it.key()) != it.value())
            return false;
    return true;
}

QString ProfileDesktop::label(const QString &target, const QString &desired) {
    const QString kind = kindOf(target), key = keyOf(target);
    if (kind == QLatin1String("theme"))
        return tr("%1 theme").arg(themeName(key));
    if (kind == QLatin1String("background"))
        return tr("Background");
    if (kind == QLatin1String("bar"))
        return tr("Bar layout");
    if (kind == QLatin1String("plugins"))
        return tr("Omarchy plugins switched off");
    if (kind == QLatin1String("file"))
        return key.startsWith(QLatin1String("ghostty/")) ? tr("Ghostty's settings") : key.startsWith(QLatin1String("alacritty/")) ? tr("Alacritty's settings") : key;
    if (kind == QLatin1String("dock"))
        return tr("Dock");
    if (kind == QLatin1String("launcher"))
        return tr("Launcher for %1").arg(appId(key));
    if (kind == QLatin1String("association"))
        return tr("App for %1").arg(key);
    if (kind == QLatin1String("shortcut"))
        return tr("Shortcut %1").arg(parseObject(desired).value("keys").toString(key));
    if (kind == QLatin1String("rule"))
        return tr("Window rule for %1").arg(parseObject(desired).value("match").toString(key));
    return target;
}

QString ProfileDesktop::summary(const QString &target, const QString &value) {
    const QString kind = kindOf(target);
    if (kind == QLatin1String("theme")) {
        if (value.isEmpty())
            return tr("not installed");
        if (value == QLatin1String("builtin"))
            return tr("one Omarchy ships");
        if (value == QLatin1String("local"))
            return tr("installed by hand");
        return tr("from %1").arg(parseObject(value).value("url").toString());
    }
    if (value.isEmpty())
        return tr("none");
    if (kind == QLatin1String("file"))
        return tr("version %1").arg(value.left(12));
    if (kind == QLatin1String("bar")) {
        const QJsonObject layout = parseObject(value);
        const QHash<QString, QString> where{{"left", tr("left")},   {"centerBegin", tr("before the center")}, {"center", tr("center")},
                                            {"centerEnd", tr("after the center")}, {"right", tr("right")}, {"tray", tr("tray drawer")}};
        QStringList parts;
        for (const QString &section : ProfileModel::barSections()) {
            QStringList widgets;
            for (const QJsonValue &widget : layout.value(section).toArray())
                widgets << shortId(widgetId(widget));
            if (!widgets.isEmpty())
                parts << tr("%1: %2").arg(where.value(section), widgets.join(", "));
        }
        return parts.isEmpty() ? tr("no widgets") : parts.join("; ");
    }
    if (kind == QLatin1String("plugins")) {
        QStringList ids;
        for (const QJsonValue &id : QJsonDocument::fromJson(value.toUtf8()).array())
            ids << shortId(id.toString());
        return ids.isEmpty() ? tr("none") : ids.join(", ");
    }
    if (kind == QLatin1String("background")) {
        const QJsonObject background = parseObject(value);
        return background.contains("sha256") ? tr("%1, one of your own, for %2").arg(background.value("name").toString(), background.value("theme").toString())
                                             : tr("%1, from %2").arg(background.value("name").toString(), background.value("theme").toString());
    }
    if (kind == QLatin1String("dock")) {
        QStringList pins;
        for (const QJsonValue &pin : QJsonDocument::fromJson(value.toUtf8()).array())
            pins << pin.toString();
        return pins.join(", ");
    }
    if (kind == QLatin1String("association"))
        return appId(value);
    const QJsonObject record = parseObject(value);
    if (kind == QLatin1String("launcher")) {
        QStringList parts;
        if (record.contains("name"))
            parts << tr("named %1").arg(record.value("name").toString());
        if (record.contains("icon"))
            parts << tr("icon %1").arg(record.value("icon").toString());
        if (record.contains("hidden"))
            parts << (record.value("hidden").toBool() ? tr("hidden") : tr("shown"));
        return parts.join(", ");
    }
    if (kind == QLatin1String("shortcut"))
        return record.value("command").toString();
    if (kind == QLatin1String("rule"))
        return (record.value("action").toString() + " " + record.value("value").toString()).trimmed();
    return value;
}

QStringList ProfileDesktop::effects(const QString &target, const QString &desired) {
    const QJsonObject record = parseObject(desired);
    if (kindOf(target) == QLatin1String("theme"))
        return {tr("Downloads the theme from %1 into your themes. It is used only when the theme setting selects it.").arg(record.value("url").toString())};
    if (kindOf(target) == QLatin1String("bar"))
        return {tr("Widgets from plugins installed separately keep their sections, after these.")};
    if (kindOf(target) == QLatin1String("shortcut"))
        return {tr("%1 runs this command: %2").arg(record.value("keys").toString(), record.value("command").toString())};
    if (kindOf(target) == QLatin1String("rule"))
        return {tr("Windows matching %1: %2 %3").arg(record.value("match").toString(), record.value("action").toString(), record.value("value").toString()).trimmed()};
    return {};
}

QString ProfileDesktop::blockedReason(const Snapshot &snapshot, const QString &target, const QString &desired, const Context &context) const {
    const QString kind = kindOf(target), key = keyOf(target);
    const bool appsPending = context.appsPending;
    if (kind == QLatin1String("theme")) {
        const QString url = parseObject(desired).value("url").toString();
        if (!ProfileModel::isThemeFolder(key) || !ProfileModel::isThemeSource(url))
            return tr("That is not a theme this app can install.");
        // A theme already here under that name is never replaced: it may be someone's own.
        const ThemeState state = snapshot.themes.value(key);
        if (state.installed)
            return state.url.isEmpty() ? tr("A theme named %1 is already installed here, not from Git, so it was left as it is.").arg(key)
                                       : tr("A theme named %1 is already installed here from %2, so it was left as it is.").arg(key, state.url);
        if (m_runner->env().findExecutable(QStringLiteral("git")).isEmpty())
            return tr("Installing a theme needs git, which is not installed.");
        if (m_compat) {
            const QString changed = m_compat->changedForKey(QStringLiteral("profile.themes"));
            if (!changed.isEmpty())
                return tr("Omarchy's %1 changed since this was tested, so changes here are paused. Review it under Advanced.").arg(changed);
        }
        return {};
    }
    if (kind == QLatin1String("bar") || kind == QLatin1String("plugins")) {
        const bool bar = kind == QLatin1String("bar");
        if (bar && !snapshot.barOk)
            return tr("The bar's layout could not be read: %1").arg(snapshot.barError);
        if (!bar && !snapshot.pluginsOk)
            return tr("Omarchy's plugins could not be read: %1").arg(snapshot.pluginsError);
        // Only what this Omarchy has: a profile from a newer one can name a widget this one lacks.
        if (bar) {
            const QJsonObject layout = parseObject(desired);
            for (const QString &section : ProfileModel::barSections())
                for (const QJsonValue &widget : layout.value(section).toArray())
                    if (!snapshot.barWidgets.contains(widgetId(widget)))
                        return tr("This Omarchy has no %1 widget, so the bar was left as it is.").arg(widgetId(widget));
        } else {
            for (const QJsonValue &id : QJsonDocument::fromJson(desired.toUtf8()).array())
                if (!snapshot.switchablePlugins.contains(id.toString()))
                    return tr("This Omarchy has no %1 plugin to switch off.").arg(id.toString());
        }
        if (m_compat) {
            const QString changed = m_compat->changedForKey(bar ? QStringLiteral("profile.bar") : QStringLiteral("profile.plugins"));
            if (!changed.isEmpty())
                return tr("Omarchy's %1 changed since this was tested, so changes here are paused. Review it under Advanced.").arg(changed);
        }
        return {};
    }
    if (kind == QLatin1String("file")) {
        // The profile's copy has to be whole and still pass review: a hand-edited or damaged
        // profile can hold anything.
        QJsonObject record;
        for (const QJsonValue &value : context.profile.files)
            if ("file:" + value.toObject().value("adapter").toString() + "/" + value.toObject().value("name").toString() == target)
                record = value.toObject();
        ProfileFiles::ConfigAdapter adapter;
        if (!ProfileFiles::configAdapter(m_runner->env(), record.value("adapter").toString(), &adapter) || adapter.name != record.value("name").toString())
            return tr("Omarchy Control has no adapter for this file.");
        QByteArray contents;
        QString error;
        if (!ProfileFiles::readConfig(context.directory, record, &contents, &error))
            return error;
        return ProfileFiles::reviewConfig(adapter.id, contents);
    }
    if (kind == QLatin1String("background")) {
        const QJsonObject background = parseObject(desired);
        const QString theme = background.value("theme").toString(), name = background.value("name").toString();
        // A background belongs to its theme, so it waits for that theme to be the one in use.
        if (theme != context.theme)
            return tr("It belongs to %1, and this computer uses %2 when its turn comes.").arg(theme, context.theme.isEmpty() ? tr("another theme") : context.theme);
        if (m_compat && m_catalog) {
            if (const CatalogEntry *entry = m_catalog->find(QStringLiteral("appearance.background"))) {
                const QString changed = m_compat->changedFor(*entry, m_capabilities ? m_capabilities->versions().value("hyprland").toString() : QString());
                if (!changed.isEmpty())
                    return tr("Omarchy's %1 changed since this was tested, so changes here are paused. Review it under Advanced.").arg(changed);
            }
        }
        if (background.contains("sha256")) {
            QByteArray bytes;
            QString error;
            if (!ProfileFiles::read(context.directory, context.profile.asset(background.value("sha256").toString()), &bytes, &error))
                return error;
            const QString destination = userBackgroundsFolder(theme) + "/" + name;
            QString there;
            qint64 size = 0;
            if (QFileInfo::exists(destination) && (!ProfileFiles::describe(destination, &there, &size) || there != background.value("sha256").toString()))
                return tr("A different %1 is already in your backgrounds for %2, so it was left as it is.").arg(name, theme);
            return {};
        }
        // A theme this restore installs first is checked when its turn comes.
        if (context.themesPending.contains(theme))
            return {};
        const Env &env = m_runner->env();
        for (const QString &folder : {env.omarchyPath + "/themes/" + theme, env.configHome + "/omarchy/themes/" + theme})
            if (QFileInfo(folder + "/backgrounds/" + name).isFile())
                return {};
        return tr("%1 is not one of %2's backgrounds here.").arg(name, theme);
    }
    if (kind == QLatin1String("dock")) {
        if (!snapshot.dockOk)
            return tr("The dock's pins could not be read: %1").arg(snapshot.dockError);
        if (QJsonDocument::fromJson(desired.toUtf8()).array().isEmpty())
            return tr("An empty dock cannot be restored through Omarchy's dock command.");
        if (m_compat && m_catalog) {
            // The dock's own settings were reviewed against the same command.
            if (const CatalogEntry *entry = m_catalog->find(QStringLiteral("desktop.dock.position"))) {
                const QString changed = m_compat->changedFor(*entry, m_capabilities ? m_capabilities->versions().value("hyprland").toString() : QString());
                if (!changed.isEmpty())
                    return tr("Omarchy's %1 changed since this was tested, so changes here are paused. Review it under Advanced.").arg(changed);
            }
        }
        return {};
    }
    if (kind == QLatin1String("launcher")) {
        const QString icon = parseObject(desired).value("icon").toString();
        if (QDir::isAbsolutePath(icon))
            return tr("Its icon is a file outside the profile.");
        if (icon.startsWith(QLatin1String("sha256:"))) {
            QByteArray bytes;
            QString error;
            if (!ProfileFiles::read(context.directory, context.profile.asset(icon.mid(7)), &bytes, &error))
                return error;
        }
        if (!snapshot.launchers.contains(key) && !appsPending)
            return tr("Its app is not installed here.");
        return {};
    }
    if (kind == QLatin1String("association")) {
        if (!snapshot.launchers.contains(desired) && !appsPending)
            return tr("%1 is not installed here.").arg(appId(desired));
        return {};
    }
    if (kind == QLatin1String("shortcut") || kind == QLatin1String("rule")) {
        if (snapshot.moduleEditedByHand)
            return tr("omarchy-control.lua was edited by hand, so it was left alone. Edit it directly under Advanced.");
        if (!snapshot.moduleImport && !snapshot.canInstallImport)
            return tr("hyprland.lua does not have Omarchy's stock layout, so this can only be added by editing it directly under Advanced.");
        const QJsonObject record = parseObject(desired);
        if (kind == QLatin1String("shortcut")) {
            if (!ShortcutsProvider::isValidKeys(record.value("keys").toString()) || record.value("command").toString().trimmed().isEmpty())
                return tr("This is not a shortcut this app can make.");
            if (!snapshot.bindsOk)
                return tr("Hyprland's shortcuts could not be read, so nothing could be checked against them.");
        } else {
            const QString problem = WindowRulesProvider::check(record.value("match").toString(), record.value("action").toString(), record.value("value").toString());
            if (!problem.isEmpty())
                return problem;
        }
    }
    return {};
}

QString ProfileDesktop::localChoice(const Snapshot &snapshot, const QString &target, const QString &current) {
    const QString kind = kindOf(target), key = keyOf(target);
    if (kind == QLatin1String("background"))
        return snapshot.background.contains("sha256") ? tr("The background here is one of your own, which may be your choice.") : QString();
    if (kind == QLatin1String("bar"))
        return current == compact(projectBar(snapshot.defaultBarLayout, true)) ? QString() : tr("This computer has a bar layout of its own, which may be your choice.");
    if (kind == QLatin1String("plugins"))
        return current.isEmpty() || current == QLatin1String("[]") ? QString()
                                                                    : tr("This computer has plugins of its own switched off, which may be your choice.");
    if (kind == QLatin1String("file"))
        return current.isEmpty() || current == snapshot.defaultDigests.value(target) ? QString()
                                                                                    : tr("This computer has its own version of this file, which may be your choice.");
    if (kind == QLatin1String("dock")) {
        QJsonArray defaults;
        for (const QString &pin : snapshot.defaultDockPins)
            defaults.append(pin);
        return current == compact(defaults) ? QString() : tr("This computer has a dock of its own, which may be your choice.");
    }
    if (kind == QLatin1String("launcher"))
        return snapshot.launcherEdits.contains(key) ? tr("This launcher was changed on this computer.") : QString();
    if (kind == QLatin1String("association"))
        return current.isEmpty() ? QString() : tr("Nothing shows whether this computer's choice is a default or yours.");
    if (kind == QLatin1String("shortcut"))
        return current.isEmpty() ? QString() : tr("This computer has its own version of this shortcut.");
    if (kind == QLatin1String("rule"))
        return current.isEmpty() ? QString() : tr("This computer has its own version of this rule.");
    return {};
}

QString ProfileDesktop::collision(const Snapshot &snapshot, const QString &target, const QString &desired) {
    if (kindOf(target) != QLatin1String("shortcut") || snapshot.shortcuts.contains(keyOf(target)))
        return {};
    const QString keys = ShortcutsProvider::canonicalKeys(parseObject(desired).value("keys").toString());
    if (!snapshot.boundKeys.contains(keys))
        return {};
    const QString does = snapshot.boundKeys.value(keys);
    return does.isEmpty() ? tr("%1 already does something else here.").arg(keys) : tr("%1 already does something else here: %2.").arg(keys, does);
}

QString ProfileDesktop::ownershipText(const Snapshot &snapshot, const QString &target) {
    const QString kind = kindOf(target);
    if ((kind == QLatin1String("shortcut") || kind == QLatin1String("rule")) && !snapshot.moduleImport)
        return tr("To save this, Omarchy Control adds one marked line to hyprland.lua and keeps your choices in omarchy-control.lua. "
                  "It loads after your own files and before Omarchy's toggles. Nothing else in your config changes.");
    return {};
}

void ProfileDesktop::read(const QStringList &mimes, std::function<void(const Snapshot &)> done) {
    auto snapshot = std::make_shared<Snapshot>();
    const Env &env = m_runner->env();
    // The configuration files a profile can carry, as they are here and as Omarchy ships them.
    for (const QString &id : ProfileModel::fileAdapters()) {
        ProfileFiles::ConfigAdapter adapter;
        if (!ProfileFiles::configAdapter(env, id, &adapter))
            continue;
        const QString target = "file:" + id + "/" + adapter.name;
        QString sha256;
        qint64 size = 0;
        if (ProfileFiles::describe(adapter.destination, &sha256, &size))
            snapshot->fileDigests.insert(target, sha256);
        if (ProfileFiles::describe(adapter.omarchyDefault, &sha256, &size))
            snapshot->defaultDigests.insert(target, sha256);
    }
    snapshot->themes = readThemes(env);
    // The bar as shell.json has it: the user's, or Omarchy's own when there is none yet, as
    // Omarchy's own bar commands read it.
    QByteArray shell, defaults;
    FileOps::readFile(env.omarchyPath + "/config/omarchy/shell.json", &defaults);
    const bool own = FileOps::readFile(env.configHome + "/omarchy/shell.json", &shell) && !shell.trimmed().isEmpty();
    snapshot->barLayout = layoutIn(own ? shell : defaults, &snapshot->barOk);
    if (!snapshot->barOk)
        snapshot->barError = tr("shell.json is not valid JSON.");
    snapshot->defaultBarLayout = layoutIn(defaults);
    for (const QVariant &row : PluginsProvider::scanManifests(env)) {
        const QVariantMap plugin = row.toMap();
        if (plugin.value("firstParty").toBool() && plugin.value("kinds").toStringList().contains(QStringLiteral("bar-widget")))
            snapshot->barWidgets.insert(plugin.value("id").toString());
    }
    // Without Omarchy's own widgets to tell them apart, every widget would look like one
    // installed separately: a capture would save an empty bar, and a restore of one would
    // empty this computer's. Neither happens; the bar is unreadable instead.
    if (snapshot->barOk && snapshot->barWidgets.isEmpty()) {
        snapshot->barOk = false;
        snapshot->barError = tr("Omarchy's own bar widgets could not be found here.");
    }
    // The background: one of the current theme's own, or one of the user's for this theme.
    QByteArray inUse;
    FileOps::readFile(env.stateHome + "/omarchy/current/theme.name", &inUse);
    snapshot->theme = QString::fromUtf8(inUse).trimmed();
    const QFileInfo link(env.stateHome + "/omarchy/current/background");
    if (link.isSymLink()) {
        const QString path = link.canonicalFilePath();
        const QString themes = QFileInfo(env.stateHome + "/omarchy/current/theme/backgrounds").canonicalFilePath();
        const QString own = QFileInfo(userBackgroundsFolder(snapshot->theme)).canonicalFilePath();
        const QFileInfo file(path);
        if (!themes.isEmpty() && file.absolutePath() == themes) {
            snapshot->background = QJsonObject{{"theme", snapshot->theme}, {"name", file.fileName()}};
        } else if (!own.isEmpty() && file.absolutePath() == own) {
            QString sha256;
            qint64 size = 0;
            if (ProfileFiles::describe(path, &sha256, &size))
                snapshot->background = QJsonObject{{"theme", snapshot->theme}, {"name", file.fileName()}, {"sha256", sha256}};
        }
        snapshot->backgroundPath = path;
    }
    if (m_launchers) {
        for (const LauncherEditor::Entry &entry : m_launchers->scan()) {
            snapshot->launchers.insert(entry.id, m_launchers->currentFields(entry));
            const QString icon = snapshot->launchers.value(entry.id).value("icon").toString();
            QString sha256;
            qint64 size = 0;
            if (QDir::isAbsolutePath(icon) && ProfileFiles::describe(icon, &sha256, &size)) {
                snapshot->launcherIconFiles.insert(entry.id, icon);
                snapshot->launcherIconDigests.insert(entry.id, sha256);
            }
            const LauncherEditor::Edit edit = m_launchers->recordedEdit(entry);
            QJsonObject changed;
            if (edit.name)
                changed["name"] = *edit.name;
            if (edit.icon)
                changed["icon"] = *edit.icon;
            if (edit.hidden)
                changed["hidden"] = *edit.hidden;
            if (!changed.isEmpty())
                snapshot->launcherEdits.insert(entry.id, changed);
        }
    }
    HyprModule module(env);
    snapshot->moduleEditedByHand = module.state() == HyprModule::ModifiedByUser;
    snapshot->moduleImport = module.importInstalled();
    snapshot->canInstallImport = module.canInstallImport();
    if (!snapshot->moduleEditedByHand && module.load()) {
        for (const QJsonValue &value : module.binds()) {
            const QJsonObject bind = value.toObject();
            if (bind.value("unbindOnly").toBool())
                continue;
            snapshot->shortcuts.insert(shortcutId(bind.value("keys").toString()), shortcutRecord(bind));
        }
        for (const QJsonValue &value : module.rules())
            snapshot->rules.insert(ruleId(value.toObject()), ruleRecord(value.toObject()));
    }
    // Omarchy's own shell.json, read above for the bar, has the pins a fresh installation has.
    for (const QJsonValue &pin : FileOps::jsonGet(defaults, {"dock", "pinned"}).toArray())
        snapshot->defaultDockPins << appId(pin.toString());

    QStringList types = commonMimes();
    for (const QString &mime : mimes)
        if (!types.contains(mime))
            types << mime;
    m_runner->run({"omarchy", "dock", "pinned"}, CommandOptions{5000}, this, [this, snapshot, types, done](const CommandResult &dock) {
        snapshot->dockOk = dock.ok();
        if (dock.ok()) {
            for (const QString &line : QString::fromUtf8(dock.out).split('\n', Qt::SkipEmptyParts))
                if (!line.trimmed().isEmpty())
                    snapshot->dockPins << appId(line.trimmed());
        } else {
            snapshot->dockError = dock.describe(QStringLiteral("omarchy"));
        }
        m_runner->run(m_runner->env().hyprctl({"-j", "binds"}), CommandOptions{4000}, this, [this, snapshot, types, done](const CommandResult &binds) {
            snapshot->bindsOk = binds.ok();
            for (const QVariant &row : ShortcutsProvider::parseBinds(binds.out))
                snapshot->boundKeys.insert(row.toMap().value("keys").toString(), row.toMap().value("description").toString());
            m_runner->run({"omarchy", "plugin", "list", "--json"}, CommandOptions{4000}, this, [this, snapshot, types, done](const CommandResult &plugins) {
                snapshot->pluginsOk = plugins.ok() && readPluginList(plugins.out, &snapshot->switchablePlugins, &snapshot->disabledPlugins);
                if (!snapshot->pluginsOk)
                    snapshot->pluginsError = plugins.ok() ? tr("the shell's answer could not be read.") : plugins.describe(QStringLiteral("omarchy"));
                readMimes(types, snapshot, [snapshot, done] { done(*snapshot); });
            });
        });
    });
}

void ProfileDesktop::readMimes(QStringList mimes, std::shared_ptr<Snapshot> snapshot, std::function<void()> done) {
    if (mimes.isEmpty()) {
        done();
        return;
    }
    const QString mime = mimes.takeFirst();
    m_runner->run({"xdg-mime", "query", "default", mime}, CommandOptions{3000}, this, [this, mime, mimes, snapshot, done](const CommandResult &result) {
        if (result.ok())
            snapshot->associations.insert(mime, result.outText());
        readMimes(mimes, snapshot, done);
    });
}

void ProfileDesktop::submitModule(const QString &settingId, const QString &label, const QString &after, std::function<bool(HyprModule &)> edit,
                                  std::function<void(ChangeCoordinator::Done)> verify, const QString &runId, ChangeCoordinator::Finished finished) {
    HyprModule probe(m_runner->env());
    ChangeCoordinator::Request request;
    request.settingId = settingId;
    request.label = label;
    request.after = after;
    // The files come back if a step fails, but there is no undo for these records yet.
    request.recovery = QStringLiteral("no-undo");
    request.runId = runId;
    request.files << probe.path() << probe.hyprlandPath();
    auto baseline = std::make_shared<QStringList>();
    request.apply = [this, edit, baseline](ChangeCoordinator::Done done) {
        m_runner->run(m_runner->env().hyprctl({"-j", "configerrors"}), CommandOptions{3000}, this, [this, edit, baseline, done](const CommandResult &errors) {
            for (const QJsonValue &item : QJsonDocument::fromJson(errors.out).array())
                if (!item.toString().trimmed().isEmpty())
                    baseline->append(item.toString().trimmed());
            HyprModule module(m_runner->env());
            QString error;
            if (!module.load(&error) || !edit(module) || !module.save(&error) || !module.installImport(&error)) {
                done(false, error.isEmpty() ? tr("omarchy-control.lua could not be changed.") : error);
                return;
            }
            CommandOptions options;
            options.mutation = true;
            m_runner->run(m_runner->env().hyprctl({"reload"}), options, this, [done](const CommandResult &result) { done(result.ok(), result.describe("Hyprland")); });
        });
    };
    request.verify = [this, verify, baseline](ChangeCoordinator::Done done) {
        // A rule has no query of its own: a reload with no new config error is what shows.
        QTimer::singleShot(150, this, [this, verify, baseline, done] {
            m_runner->run(m_runner->env().hyprctl({"-j", "configerrors"}), CommandOptions{3000}, this, [verify, baseline, done](const CommandResult &errors) {
                for (const QJsonValue &item : QJsonDocument::fromJson(errors.out).array()) {
                    const QString text = item.toString().trimmed();
                    if (!text.isEmpty() && !baseline->contains(text)) {
                        done(false, tr("Hyprland reported a config error: %1").arg(text.left(240)));
                        return;
                    }
                }
                if (verify)
                    verify(done);
                else
                    done(true, {});
            });
        });
    };
    request.rollback = [this](ChangeCoordinator::Done done) {
        CommandOptions options;
        options.mutation = true;
        m_runner->run(m_runner->env().hyprctl({"reload"}), options, this, [done](const CommandResult &) { done(true, {}); });
    };
    m_coordinator->submit(std::move(request), finished);
}

void ProfileDesktop::runSteps(QList<QStringList> steps, ChangeCoordinator::Done done) {
    if (steps.isEmpty()) {
        done(true, {});
        return;
    }
    const QStringList argv = steps.takeFirst();
    CommandOptions options;
    options.mutation = true;
    options.timeoutMs = 15000;
    m_runner->run(argv, options, this, [this, steps, argv, done](const CommandResult &result) {
        if (!result.ok()) {
            done(false, result.describe(argv.value(0)));
            return;
        }
        runSteps(steps, done);
    });
}

void ProfileDesktop::apply(const QString &target, const QString &desired, const Context &context, const QString &runId, ChangeCoordinator::Finished finished) {
    const QString kind = kindOf(target), key = keyOf(target);
    const auto refuse = [&finished, this, &target, &desired](const QString &message) {
        Transaction tx;
        tx.label = label(target, desired);
        tx.phase = QStringLiteral("failed");
        tx.message = message;
        finished(tx);
    };
    if (kind == QLatin1String("bar")) {
        const QJsonObject want = parseObject(desired);
        const QString path = m_runner->env().configHome + "/omarchy/shell.json";
        ChangeCoordinator::Request request;
        request.settingId = QStringLiteral("desktop.bar.layout");
        request.label = tr("Bar layout");
        request.after = summary(target, desired).left(200);
        request.recovery = QStringLiteral("no-undo");
        request.runId = runId;
        request.files << path;
        request.apply = [this, path, want](ChangeCoordinator::Done done) {
            // As Omarchy's own bar commands change it: the user's file, or a copy of Omarchy's
            // when there is none yet, then the shell is asked to read it again.
            QByteArray bytes;
            if (!FileOps::readFile(path, &bytes) || bytes.trimmed().isEmpty())
                FileOps::readFile(m_runner->env().omarchyPath + "/config/omarchy/shell.json", &bytes);
            bool ok = false;
            const QJsonObject layout = layoutIn(bytes, &ok);
            QString error;
            if (!ok || !FileOps::jsonSet(&bytes, {"bar", "layout"}, mergeBar(layout, want), &error) || !FileOps::atomicWrite(path, bytes, &error, 0644)) {
                done(false, error.isEmpty() ? tr("shell.json is not valid JSON, so the bar was left as it is.") : error);
                return;
            }
            CommandOptions options;
            options.mutation = true;
            // A shell that is not running reads the file when it starts.
            m_runner->run({"omarchy", "shell", "-q", "shell", "reloadConfig"}, options, this, [done](const CommandResult &) { done(true, {}); });
        };
        request.verify = [path, desired](ChangeCoordinator::Done done) {
            QByteArray bytes;
            FileOps::readFile(path, &bytes);
            done(barMatches(desired, compact(projectBar(layoutIn(bytes), true))), tr("The bar's layout did not change."));
        };
        request.rollback = [this](ChangeCoordinator::Done done) {
            CommandOptions options;
            options.mutation = true;
            m_runner->run({"omarchy", "shell", "-q", "shell", "reloadConfig"}, options, this, [done](const CommandResult &) { done(true, {}); });
        };
        m_coordinator->submit(std::move(request), finished);
        return;
    }
    if (kind == QLatin1String("plugins")) {
        QStringList want;
        for (const QJsonValue &id : QJsonDocument::fromJson(desired.toUtf8()).array())
            want << id.toString();
        ChangeCoordinator::Request request;
        request.settingId = QStringLiteral("desktop.plugins");
        request.label = label(target, desired);
        request.after = summary(target, desired);
        request.recovery = QStringLiteral("no-undo");
        request.runId = runId;
        request.files << m_runner->env().configHome + "/omarchy/shell.json";
        request.apply = [this, want](ChangeCoordinator::Done done) {
            // Through Omarchy's own commands, which the running shell carries out.
            m_runner->run({"omarchy", "plugin", "list", "--json"}, CommandOptions{4000}, this, [this, want, done](const CommandResult &list) {
                QSet<QString> switchable;
                QStringList disabled;
                if (!list.ok() || !readPluginList(list.out, &switchable, &disabled)) {
                    done(false, tr("Omarchy's plugins could not be read: %1").arg(list.describe(QStringLiteral("omarchy"))));
                    return;
                }
                QList<QStringList> steps;
                for (const QString &id : want)
                    if (!disabled.contains(id))
                        steps << QStringList{"omarchy", "plugin", "disable", id};
                for (const QString &id : disabled)
                    if (!want.contains(id))
                        steps << QStringList{"omarchy", "plugin", "enable", id};
                runSteps(steps, done);
            });
        };
        request.verify = [this, desired](ChangeCoordinator::Done done) {
            m_runner->run({"omarchy", "plugin", "list", "--json"}, CommandOptions{4000}, this, [desired, done](const CommandResult &list) {
                QSet<QString> switchable;
                QStringList disabled;
                done(list.ok() && readPluginList(list.out, &switchable, &disabled) && compact(QJsonArray::fromStringList(disabled)) == desired,
                     tr("The plugins did not change."));
            });
        };
        m_coordinator->submit(std::move(request), finished);
        return;
    }
    if (kind == QLatin1String("theme")) {
        const QString url = parseObject(desired).value("url").toString();
        if (!ProfileModel::isThemeFolder(key) || !ProfileModel::isThemeSource(url)) {
            refuse(tr("That is not a theme this app can install."));
            return;
        }
        const QString path = m_runner->env().configHome + "/omarchy/themes/" + key;
        auto cloned = std::make_shared<bool>(false);
        ChangeCoordinator::Request request;
        request.settingId = QStringLiteral("desktop.themes");
        request.label = label(target, desired);
        request.after = url;
        // A new folder that replaces nothing. Removing the theme later is the theme menu's.
        request.recovery = QStringLiteral("no-undo");
        request.runId = runId;
        request.apply = [this, path, url, cloned](ChangeCoordinator::Done done) {
            // Checked again here, at the last moment: a theme of that name is never replaced.
            if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
                done(false, tr("A theme named %1 was installed here meanwhile, so it was left as it is.").arg(QFileInfo(path).fileName()));
                return;
            }
            QDir().mkpath(QFileInfo(path).absolutePath());
            *cloned = true;
            CommandOptions options;
            options.mutation = true;
            options.timeoutMs = 300000;
            // Cloned as omarchy-theme-install clones, but without switching to the theme:
            // the theme setting does that. git asks nothing and follows only https.
            m_runner->run({"env", "GIT_TERMINAL_PROMPT=0", "git", "-c", "protocol.allow=never", "-c", "protocol.https.allow=always", "clone", "--quiet", "--",
                           url, path},
                          options, this, [done](const CommandResult &result) { done(result.ok(), result.describe(QStringLiteral("git"))); });
        };
        request.verify = [this, key, url](ChangeCoordinator::Done done) {
            const ThemeState state = themeState(m_runner->env(), key);
            done(state.installed && sameSource(state.url, url), tr("The theme was not installed."));
        };
        // What a failed or stopped clone left behind goes, and only what this change made.
        request.rollback = [path, cloned](ChangeCoordinator::Done done) {
            if (*cloned && QFileInfo(path).isDir() && !QFileInfo(path).isSymLink())
                QDir(path).removeRecursively();
            done(true, {});
        };
        m_coordinator->submit(std::move(request), finished);
        return;
    }
    if (kind == QLatin1String("file")) {
        QJsonObject record;
        for (const QJsonValue &value : context.profile.files)
            if ("file:" + value.toObject().value("adapter").toString() + "/" + value.toObject().value("name").toString() == target)
                record = value.toObject();
        ProfileFiles::ConfigAdapter adapter;
        QByteArray contents;
        QString error;
        if (!ProfileFiles::configAdapter(m_runner->env(), record.value("adapter").toString(), &adapter) || adapter.name != record.value("name").toString()) {
            refuse(tr("Omarchy Control has no adapter for this file."));
            return;
        }
        if (!ProfileFiles::readConfig(context.directory, record, &contents, &error)) {
            refuse(error);
            return;
        }
        const QString problem = ProfileFiles::reviewConfig(adapter.id, contents);
        if (!problem.isEmpty()) {
            refuse(problem);
            return;
        }
        ChangeCoordinator::Request request;
        request.settingId = "files." + adapter.id;
        request.label = label(target, desired);
        request.after = desired.left(12);
        // The file it replaces is kept with the change, but there is no automatic undo yet.
        request.recovery = QStringLiteral("no-undo");
        request.runId = runId;
        request.files << adapter.destination;
        const QString destination = adapter.destination;
        request.apply = [destination, contents](ChangeCoordinator::Done done) {
            QString error;
            done(FileOps::atomicWrite(destination, contents, &error, 0644), error);
        };
        request.verify = [destination, desired](ChangeCoordinator::Done done) {
            QString sha256;
            qint64 size = 0;
            done(ProfileFiles::describe(destination, &sha256, &size) && sha256 == desired, tr("The file did not change."));
        };
        m_coordinator->submit(std::move(request), finished);
        return;
    }
    if (kind == QLatin1String("background")) {
        const QJsonObject background = parseObject(desired);
        const QString theme = background.value("theme").toString(), name = background.value("name").toString();
        const Env &env = m_runner->env();
        QByteArray inUse;
        FileOps::readFile(env.stateHome + "/omarchy/current/theme.name", &inUse);
        if (QString::fromUtf8(inUse).trimmed() != theme) {
            refuse(tr("It belongs to %1, which is not the theme in use, so it was left alone.").arg(theme));
            return;
        }
        QString path = env.stateHome + "/omarchy/current/theme/backgrounds/" + name;
        QByteArray bytes;
        bool install = false;
        if (!background.contains("sha256") && !QFileInfo(path).isFile()) {
            refuse(tr("%1 is not one of %2's backgrounds here.").arg(name, theme));
            return;
        }
        if (background.contains("sha256")) {
            // One of the user's own goes into their backgrounds for its theme, where the
            // theme's picker finds it too, and only from a copy that matches its digest.
            QString error;
            if (!ProfileFiles::read(context.directory, context.profile.asset(background.value("sha256").toString()), &bytes, &error)) {
                refuse(error);
                return;
            }
            path = userBackgroundsFolder(theme) + "/" + name;
            QString there;
            qint64 size = 0;
            install = !(QFileInfo::exists(path) && ProfileFiles::describe(path, &there, &size) && there == background.value("sha256").toString());
            if (QFileInfo::exists(path) && install) {
                refuse(tr("A different %1 is already in your backgrounds for %2, so it was left as it is.").arg(name, theme));
                return;
            }
        }
        ChangeCoordinator::Request request;
        request.settingId = QStringLiteral("appearance.background");
        request.label = tr("Background");
        request.after = name;
        request.recovery = QStringLiteral("no-undo");
        request.runId = runId;
        if (install)
            request.files << path;
        request.apply = [this, path, bytes, install](ChangeCoordinator::Done done) {
            QString error;
            if (install && !FileOps::atomicWrite(path, bytes, &error, 0644)) {
                done(false, error);
                return;
            }
            CommandOptions options;
            options.mutation = true;
            options.timeoutMs = 15000;
            m_runner->run({"omarchy", "theme", "bg", "set", path}, options, this, [done](const CommandResult &result) {
                done(result.ok(), result.describe(QStringLiteral("omarchy")));
            });
        };
        request.verify = [this, path](ChangeCoordinator::Done done) {
            const QFileInfo link(m_runner->env().stateHome + "/omarchy/current/background");
            done(link.isSymLink() && link.canonicalFilePath() == QFileInfo(path).canonicalFilePath(), tr("The background did not change."));
        };
        m_coordinator->submit(std::move(request), finished);
        return;
    }
    if (kind == QLatin1String("dock")) {
        QStringList pins;
        for (const QJsonValue &pin : QJsonDocument::fromJson(desired.toUtf8()).array())
            pins << appId(pin.toString());
        if (pins.isEmpty()) {
            refuse(tr("An empty dock cannot be restored through Omarchy's dock command."));
            return;
        }
        ChangeCoordinator::Request request;
        request.settingId = QStringLiteral("desktop.dock.pins");
        request.label = tr("Dock");
        request.after = pins.join(", ");
        request.recovery = QStringLiteral("no-undo");
        request.runId = runId;
        request.files << m_runner->env().configHome + "/omarchy/shell.json";
        request.apply = [this, pins](ChangeCoordinator::Done done) {
            CommandOptions options;
            options.mutation = true;
            options.timeoutMs = 15000;
            // Omarchy's own command, which keeps the order given and every other key.
            m_runner->run(QStringList{"omarchy", "dock", "pinned"} + pins, options, this, [done](const CommandResult &result) {
                done(result.ok(), result.describe(QStringLiteral("omarchy")));
            });
        };
        request.verify = [this, pins](ChangeCoordinator::Done done) {
            m_runner->run({"omarchy", "dock", "pinned"}, CommandOptions{5000}, this, [pins, done](const CommandResult &result) {
                QStringList now;
                for (const QString &line : QString::fromUtf8(result.out).split('\n', Qt::SkipEmptyParts))
                    now << appId(line.trimmed());
                done(result.ok() && now == pins, tr("The dock did not change."));
            });
        };
        m_coordinator->submit(std::move(request), finished);
        return;
    }
    if (kind == QLatin1String("launcher")) {
        if (!m_launchers) {
            refuse(tr("Launchers cannot be changed here."));
            return;
        }
        const QJsonObject record = parseObject(desired);
        LauncherEditor::Edit edit;
        if (record.contains("name"))
            edit.name = record.value("name").toString();
        if (record.contains("icon"))
            edit.icon = record.value("icon").toString();
        if (edit.icon && edit.icon->startsWith(QLatin1String("sha256:"))) {
            // The profile's icon goes through the editor's own import, which checks it is an
            // image and keeps a copy named by its contents.
            const QJsonObject asset = context.profile.asset(edit.icon->mid(7));
            QByteArray bytes;
            QString error;
            QTemporaryDir staging;
            const QString name = asset.value("name").toString();
            const QString staged = staging.path() + "/" + name;
            if (!ProfileFiles::read(context.directory, asset, &bytes, &error) || !FileOps::atomicWrite(staged, bytes, &error, 0600)) {
                refuse(error);
                return;
            }
            const QString imported = m_launchers->importIcon(staged, &error);
            if (imported.isEmpty()) {
                refuse(error);
                return;
            }
            edit.icon = imported;
        }
        if (record.contains("hidden"))
            edit.hidden = record.value("hidden").toBool();
        m_launchers->apply(key, edit, finished, runId);
        return;
    }
    if (kind == QLatin1String("association")) {
        const QString app = desired;
        static const QRegularExpression mimePattern(QStringLiteral("^[a-z0-9-]{1,40}/[A-Za-z0-9.+_-]{1,120}$"));
        if (!mimePattern.match(key).hasMatch() || app.contains('/') || !app.endsWith(QLatin1String(".desktop"))) {
            refuse(tr("That file association is not valid."));
            return;
        }
        ChangeCoordinator::Request request;
        request.settingId = "apps.association." + key;
        request.label = tr("App for %1").arg(key);
        request.after = app;
        request.recovery = QStringLiteral("no-undo");
        request.runId = runId;
        request.files << m_runner->env().configHome + "/mimeapps.list";
        request.apply = [this, key, app](ChangeCoordinator::Done done) {
            CommandOptions options;
            options.mutation = true;
            m_runner->run({"xdg-mime", "default", app, key}, options, this, [done](const CommandResult &result) { done(result.ok(), result.describe("xdg-mime")); });
        };
        request.verify = [this, key, app](ChangeCoordinator::Done done) {
            m_runner->run({"xdg-mime", "query", "default", key}, CommandOptions{3000}, this, [app, done](const CommandResult &result) {
                done(result.outText() == app, tr("The association did not change."));
            });
        };
        m_coordinator->submit(std::move(request), finished);
        return;
    }
    if (kind == QLatin1String("shortcut")) {
        const QJsonObject record = parseObject(desired);
        const QString keys = ShortcutsProvider::canonicalKeys(record.value("keys").toString());
        // Keys something else already uses are taken over only because the restore was
        // told to, and the old binding is released, never left to collide.
        m_runner->run(m_runner->env().hyprctl({"-j", "binds"}), CommandOptions{4000}, this, [this, record, keys, runId, finished, target, desired](const CommandResult &binds) {
            HyprModule module(m_runner->env());
            bool owned = false;
            if (module.load())
                for (const QJsonValue &value : module.binds())
                    owned = owned || ShortcutsProvider::canonicalKeys(value.toObject().value("keys").toString()) == keys;
            bool taken = false;
            for (const QVariant &row : ShortcutsProvider::parseBinds(binds.out))
                taken = taken || row.toMap().value("keys").toString() == keys;
            const QJsonObject bind{{"keys", keys}, {"description", record.value("description").toString()},
                                   {"command", record.value("command").toString()}, {"replaces", taken && !owned}};
            submitModule(QStringLiteral("shortcuts"), label(target, desired), keys,
                         [bind, keys](HyprModule &edited) {
                             QJsonArray kept;
                             for (const QJsonValue &value : edited.binds())
                                 if (ShortcutsProvider::canonicalKeys(value.toObject().value("keys").toString()) != keys)
                                     kept.append(value);
                             kept.append(bind);
                             edited.setBinds(kept);
                             return true;
                         },
                         [this, keys](ChangeCoordinator::Done done) {
                             m_runner->run(m_runner->env().hyprctl({"-j", "binds"}), CommandOptions{4000}, this, [keys, done](const CommandResult &after) {
                                 bool found = false;
                                 for (const QVariant &row : ShortcutsProvider::parseBinds(after.out))
                                     found = found || row.toMap().value("keys").toString() == keys;
                                 done(found, tr("Hyprland did not pick up the shortcut."));
                             });
                         },
                         runId, finished);
        });
        return;
    }
    if (kind == QLatin1String("rule")) {
        const QJsonObject rule = ruleRecord(parseObject(desired));
        const QString id = ruleId(rule);
        submitModule(QStringLiteral("windowrules"), label(target, desired), rule.value("action").toString(),
                     [rule, id](HyprModule &edited) {
                         QJsonArray kept;
                         for (const QJsonValue &value : edited.rules())
                             if (ruleId(value.toObject()) != id)
                                 kept.append(value);
                         QJsonObject stored{{"match", rule.value("match")}, {"action", rule.value("action")}, {"value", rule.value("value")}};
                         kept.append(stored);
                         edited.setRules(kept);
                         return true;
                     },
                     nullptr, runId, finished);
        return;
    }
    refuse(tr("This version of Omarchy Control does not know this desktop record."));
}
