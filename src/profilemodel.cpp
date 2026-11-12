#include "profilemodel.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cstring>
#include <functional>

#include "fileops.h"

namespace {

QString tr(const char *text) {
    return QCoreApplication::translate("Profile", text);
}

const QRegularExpression &settingIdPattern() {
    static const QRegularExpression pattern(QStringLiteral("^[a-z][a-z0-9_]*(\\.[a-z0-9_]+)+$"));
    return pattern;
}

// Arch package names: letters, digits, and @._+-, never starting with - or a dot.
const QRegularExpression &packagePattern() {
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9@_+][a-z0-9@._+-]{0,127}$"));
    return pattern;
}

const QRegularExpression &desktopIdPattern() {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9._-]{1,240}\\.desktop$"));
    return pattern;
}

const QRegularExpression &digestPattern() {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern;
}

// A plain file name a profile may carry: no folders, nothing hidden.
const QRegularExpression &plainNamePattern() {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_+-][A-Za-z0-9._+-]{0,127}$"));
    return pattern;
}

bool hasControl(const QString &text, bool allowNewline = false) {
    for (const QChar c : text)
        if (c.category() == QChar::Other_Control && !(allowNewline && c == QLatin1Char('\n')))
            return true;
    return false;
}

bool isWholeNumber(const QJsonValue &value) {
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    return number == double(qint64(number));
}

// QJsonDocument keeps the last of two equal keys without a word, so a profile is
// walked once more for them: a duplicate means two writers disagree about one choice.
struct DuplicateScan {
    const QByteArray &s;
    QStringList *errors;
    int i = 0;
    bool tooDeep = false;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            ++i;
    }
    QString string() {
        const int begin = i++;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\')
                ++i;
            ++i;
        }
        ++i;
        return QJsonDocument::fromJson("[" + s.mid(begin, i - begin) + "]").array().at(0).toString();
    }
    void value(const QString &path, int depth) {
        ws();
        if (i >= s.size())
            return;
        if (depth > 32) {
            tooDeep = true;
            i = s.size();
            return;
        }
        const char c = s[i];
        if (c == '{') {
            ++i;
            QSet<QString> seen;
            ws();
            if (i < s.size() && s[i] == '}') {
                ++i;
                return;
            }
            while (i < s.size()) {
                ws();
                const QString key = string();
                const QString where = path.isEmpty() ? key : path + "." + key;
                if (seen.contains(key))
                    *errors << tr("%1 appears twice.").arg(where);
                seen.insert(key);
                ws();
                ++i;  // ':'
                value(where, depth + 1);
                ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                ++i;  // '}'
                return;
            }
        } else if (c == '[') {
            ++i;
            ws();
            if (i < s.size() && s[i] == ']') {
                ++i;
                return;
            }
            int index = 0;
            while (i < s.size()) {
                value(QStringLiteral("%1[%2]").arg(path).arg(index), depth + 1);
                ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    ++index;
                    continue;
                }
                ++i;  // ']'
                return;
            }
        } else if (c == '"') {
            string();
        } else {
            while (i < s.size() && s[i] != '\0' && !std::strchr(",]} \t\n\r", s[i]))
                ++i;
        }
    }
};

// Checks one object's keys against the ones a section allows.
bool onlyKeys(const QJsonObject &object, const QStringList &allowed, const QString &where, QStringList *errors) {
    bool ok = true;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!allowed.contains(it.key())) {
            *errors << tr("%1 has an unknown field, %2.").arg(where, it.key());
            ok = false;
        }
    }
    return ok;
}

bool text(const QJsonObject &object, const QString &key, const QString &where, int maxLength, bool required, QStringList *errors,
          bool allowNewline = false) {
    if (!object.contains(key)) {
        if (required)
            *errors << tr("%1 needs %2.").arg(where, key);
        return !required;
    }
    const QJsonValue value = object.value(key);
    if (!value.isString() || value.toString().size() > maxLength || hasControl(value.toString(), allowNewline)) {
        *errors << tr("%1.%2 must be text of at most %3 characters, with no control characters.").arg(where, key).arg(maxLength);
        return false;
    }
    return true;
}

void validateSettings(const QJsonValue &section, QStringList *errors) {
    if (!section.isObject()) {
        *errors << tr("settings must be an object.");
        return;
    }
    const QJsonObject settings = section.toObject();
    if (settings.size() > 1000)
        *errors << tr("settings holds more than 1000 entries.");
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        const QString where = "settings." + it.key();
        if (!ProfileModel::isSettingId(it.key())) {
            *errors << tr("%1 is not a setting id.").arg(where);
            continue;
        }
        if (!it.value().isObject()) {
            *errors << tr("%1 must be an object.").arg(where);
            continue;
        }
        const QJsonObject record = it.value().toObject();
        onlyKeys(record, {"mode", "value"}, where, errors);
        const QString mode = record.value("mode").toString();
        if (mode == QLatin1String("set")) {
            text(record, "value", where, 4096, true, errors, true);
        } else if (mode == QLatin1String("inherit")) {
            if (record.contains("value"))
                *errors << tr("%1 inherits its default, so it cannot also have a value.").arg(where);
        } else {
            *errors << tr("%1.mode must be \"set\" or \"inherit\".").arg(where);
        }
    }
}

void validatePackages(const QJsonValue &section, QStringList *errors) {
    if (!section.isArray()) {
        *errors << tr("packages must be a list.");
        return;
    }
    const QJsonArray packages = section.toArray();
    if (packages.size() > 2000)
        *errors << tr("packages holds more than 2000 entries.");
    QSet<QString> names;
    for (int index = 0; index < packages.size(); ++index) {
        const QString where = QStringLiteral("packages[%1]").arg(index);
        if (!packages[index].isObject()) {
            *errors << tr("%1 must be an object.").arg(where);
            continue;
        }
        const QJsonObject record = packages[index].toObject();
        onlyKeys(record, {"name", "source", "intent", "repository"}, where, errors);
        const QString name = record.value("name").toString();
        if (!packagePattern().match(name).hasMatch()) {
            *errors << tr("%1.name is not a package name.").arg(where);
            continue;
        }
        // The same package twice is either a repeat or a contradiction, such as
        // wanting it both present and absent. Either way nobody can say which wins.
        if (names.contains(name))
            *errors << tr("Package %1 appears twice.").arg(name);
        names.insert(name);
        const QString source = record.value("source").toString();
        if (source != QLatin1String("repository") && source != QLatin1String("aur") && source != QLatin1String("manual"))
            *errors << tr("%1.source must be \"repository\", \"aur\", or \"manual\".").arg(where);
        const QString intent = record.value("intent").toString();
        if (intent != QLatin1String("present") && intent != QLatin1String("absent"))
            *errors << tr("%1.intent must be \"present\" or \"absent\".").arg(where);
        if (record.contains("repository")) {
            static const QRegularExpression repository(QStringLiteral("^[a-z0-9][a-z0-9_-]{0,63}$"));
            if (!repository.match(record.value("repository").toString()).hasMatch())
                *errors << tr("%1.repository is not a repository name.").arg(where);
        }
    }
}

void validateDesktop(const QJsonValue &section, QStringList *errors) {
    if (!section.isObject()) {
        *errors << tr("desktop must be an object.");
        return;
    }
    const QJsonObject desktop = section.toObject();
    onlyKeys(desktop, {"dockPins", "launchers", "associations", "shortcuts", "windowRules"}, QStringLiteral("desktop"), errors);
    if (desktop.contains("dockPins")) {
        const QJsonValue pins = desktop.value("dockPins");
        QSet<QString> seen;
        if (!pins.isArray() || pins.toArray().size() > 100) {
            *errors << tr("desktop.dockPins must be a list of at most 100 apps.");
        } else {
            for (const QJsonValue &pin : pins.toArray()) {
                if (!desktopIdPattern().match(pin.toString()).hasMatch())
                    *errors << tr("desktop.dockPins holds something that is not an app id.");
                else if (seen.contains(pin.toString()))
                    *errors << tr("%1 is pinned twice.").arg(pin.toString());
                seen.insert(pin.toString());
            }
        }
    }
    if (desktop.contains("launchers")) {
        const QJsonValue launchers = desktop.value("launchers");
        if (!launchers.isObject()) {
            *errors << tr("desktop.launchers must be an object.");
        } else {
            const QJsonObject map = launchers.toObject();
            for (auto it = map.begin(); it != map.end(); ++it) {
                const QString where = "desktop.launchers." + it.key();
                if (!desktopIdPattern().match(it.key()).hasMatch() || !it.value().isObject()) {
                    *errors << tr("%1 is not an app's launcher change.").arg(where);
                    continue;
                }
                const QJsonObject edit = it.value().toObject();
                onlyKeys(edit, {"name", "icon", "hidden"}, where, errors);
                if (edit.isEmpty())
                    *errors << tr("%1 changes nothing.").arg(where);
                text(edit, "name", where, 200, false, errors);
                if (edit.contains("icon")) {
                    // An icon name, or a file of the profile's own by its digest.
                    static const QRegularExpression icon(QStringLiteral("^([A-Za-z0-9._+-]{1,200}|sha256:[0-9a-f]{64})$"));
                    if (!icon.match(edit.value("icon").toString()).hasMatch())
                        *errors << tr("%1.icon must be an icon name or one of the profile's files.").arg(where);
                }
                if (edit.contains("hidden") && !edit.value("hidden").isBool())
                    *errors << tr("%1.hidden must be true or false.").arg(where);
            }
        }
    }
    if (desktop.contains("associations")) {
        const QJsonValue associations = desktop.value("associations");
        static const QRegularExpression mime(QStringLiteral("^[a-z0-9-]{1,40}/[A-Za-z0-9.+_-]{1,120}$"));
        if (!associations.isObject()) {
            *errors << tr("desktop.associations must be an object.");
        } else {
            const QJsonObject map = associations.toObject();
            for (auto it = map.begin(); it != map.end(); ++it)
                if (!mime.match(it.key()).hasMatch() || !desktopIdPattern().match(it.value().toString()).hasMatch())
                    *errors << tr("desktop.associations.%1 must name a file type and an app id.").arg(it.key());
        }
    }
    static const QRegularExpression recordId(QStringLiteral("^[a-z0-9][a-z0-9-]{0,63}$"));
    if (desktop.contains("shortcuts")) {
        const QJsonValue shortcuts = desktop.value("shortcuts");
        QSet<QString> ids;
        if (!shortcuts.isArray()) {
            *errors << tr("desktop.shortcuts must be a list.");
        } else {
            int index = 0;
            for (const QJsonValue &value : shortcuts.toArray()) {
                const QString where = QStringLiteral("desktop.shortcuts[%1]").arg(index++);
                const QJsonObject shortcut = value.toObject();
                onlyKeys(shortcut, {"id", "keys", "description", "command"}, where, errors);
                if (!recordId.match(shortcut.value("id").toString()).hasMatch())
                    *errors << tr("%1.id is not a record id.").arg(where);
                else if (ids.contains(shortcut.value("id").toString()))
                    *errors << tr("Shortcut %1 appears twice.").arg(shortcut.value("id").toString());
                ids.insert(shortcut.value("id").toString());
                text(shortcut, "keys", where, 100, true, errors);
                text(shortcut, "description", where, 200, false, errors);
                text(shortcut, "command", where, 1024, true, errors);
            }
        }
    }
    if (desktop.contains("windowRules")) {
        const QJsonValue rules = desktop.value("windowRules");
        QSet<QString> ids;
        if (!rules.isArray()) {
            *errors << tr("desktop.windowRules must be a list.");
        } else {
            int index = 0;
            for (const QJsonValue &value : rules.toArray()) {
                const QString where = QStringLiteral("desktop.windowRules[%1]").arg(index++);
                const QJsonObject rule = value.toObject();
                onlyKeys(rule, {"id", "match", "action", "value"}, where, errors);
                if (!recordId.match(rule.value("id").toString()).hasMatch())
                    *errors << tr("%1.id is not a record id.").arg(where);
                else if (ids.contains(rule.value("id").toString()))
                    *errors << tr("Window rule %1 appears twice.").arg(rule.value("id").toString());
                ids.insert(rule.value("id").toString());
                text(rule, "match", where, 200, true, errors);
                static const QStringList actions{"float", "tile", "workspace", "opacity", "noblur"};
                if (!actions.contains(rule.value("action").toString()))
                    *errors << tr("%1.action is not a window rule this app makes.").arg(where);
                text(rule, "value", where, 100, false, errors);
            }
        }
    }
}

qint64 validateFiles(const QJsonValue &section, QStringList *errors) {
    qint64 total = 0;
    if (!section.isArray()) {
        *errors << tr("files must be a list.");
        return total;
    }
    QSet<QString> seen;
    int index = 0;
    for (const QJsonValue &value : section.toArray()) {
        const QString where = QStringLiteral("files[%1]").arg(index++);
        const QJsonObject file = value.toObject();
        if (!value.isObject()) {
            *errors << tr("%1 must be an object.").arg(where);
            continue;
        }
        onlyKeys(file, {"adapter", "name", "sha256", "size"}, where, errors);
        const QString adapter = file.value("adapter").toString();
        // A file only ever goes where a reviewed adapter puts it. One this version does
        // not know means the profile needs a newer Omarchy Control, so nothing starts.
        if (!ProfileModel::fileAdapters().contains(adapter))
            *errors << tr("%1 needs the %2 file adapter, which this version of Omarchy Control does not have.").arg(where, adapter.isEmpty() ? QStringLiteral("?") : adapter);
        const QString name = file.value("name").toString();
        if (!plainNamePattern().match(name).hasMatch())
            *errors << tr("%1.name must be a plain file name.").arg(where);
        if (seen.contains(adapter + "/" + name))
            *errors << tr("%1/%2 appears twice.").arg(adapter, name);
        seen.insert(adapter + "/" + name);
        if (!digestPattern().match(file.value("sha256").toString()).hasMatch())
            *errors << tr("%1.sha256 must be a SHA-256 digest.").arg(where);
        if (!isWholeNumber(file.value("size")) || file.value("size").toDouble() < 0 || file.value("size").toDouble() > ProfileModel::MaxFileBytes)
            *errors << tr("%1.size must be a size of at most 25 MiB.").arg(where);
        else
            total += qint64(file.value("size").toDouble());
    }
    return total;
}

qint64 validateAssets(const QJsonValue &section, QStringList *errors) {
    qint64 total = 0;
    if (!section.isArray()) {
        *errors << tr("assets must be a list.");
        return total;
    }
    QSet<QString> seen;
    int index = 0;
    for (const QJsonValue &value : section.toArray()) {
        const QString where = QStringLiteral("assets[%1]").arg(index++);
        if (!value.isObject()) {
            *errors << tr("%1 must be an object.").arg(where);
            continue;
        }
        const QJsonObject asset = value.toObject();
        onlyKeys(asset, {"sha256", "size", "kind", "name"}, where, errors);
        const QString digest = asset.value("sha256").toString();
        if (!digestPattern().match(digest).hasMatch())
            *errors << tr("%1.sha256 must be a SHA-256 digest.").arg(where);
        else if (seen.contains(digest))
            *errors << tr("Asset %1 appears twice.").arg(digest.left(12));
        seen.insert(digest);
        static const QStringList kinds{"background", "icon", "theme"};
        if (!kinds.contains(asset.value("kind").toString()))
            *errors << tr("%1.kind must be \"background\", \"icon\", or \"theme\".").arg(where);
        if (!plainNamePattern().match(asset.value("name").toString()).hasMatch())
            *errors << tr("%1.name must be a plain file name.").arg(where);
        if (!isWholeNumber(asset.value("size")) || asset.value("size").toDouble() < 0 || asset.value("size").toDouble() > ProfileModel::MaxFileBytes)
            *errors << tr("%1.size must be a size of at most 25 MiB.").arg(where);
        else
            total += qint64(asset.value("size").toDouble());
    }
    return total;
}

// Arrays whose order means nothing are kept sorted, so two saves of the same choices
// never differ. The dock's pins keep their order, which is the point of them.
QJsonArray sortedBy(QJsonArray array, const std::function<QString(const QJsonObject &)> &key) {
    QList<QJsonValue> values(array.begin(), array.end());
    std::stable_sort(values.begin(), values.end(), [&key](const QJsonValue &a, const QJsonValue &b) { return key(a.toObject()) < key(b.toObject()); });
    QJsonArray out;
    for (const QJsonValue &value : values)
        out.append(value);
    return out;
}

}  // namespace

QJsonObject ProfilePackage::toJson() const {
    QJsonObject object{{"name", name}, {"source", source}, {"intent", intent}};
    if (!repository.isEmpty())
        object["repository"] = repository;
    return object;
}

ProfilePackage ProfilePackage::fromJson(const QJsonObject &object) {
    return ProfilePackage{object.value("name").toString(), object.value("source").toString(), object.value("intent").toString(),
                          object.value("repository").toString()};
}

QList<ProfilePackage> Profile::packageList() const {
    QList<ProfilePackage> list;
    for (const QJsonValue &value : packages)
        list << ProfilePackage::fromJson(value.toObject());
    std::sort(list.begin(), list.end(), [](const ProfilePackage &a, const ProfilePackage &b) { return a.name < b.name; });
    return list;
}

ProfilePackage Profile::package(const QString &name) const {
    for (const QJsonValue &value : packages)
        if (value.toObject().value("name").toString() == name)
            return ProfilePackage::fromJson(value.toObject());
    return {};
}

void Profile::setPackage(const ProfilePackage &package) {
    removePackage(package.name);
    packages.append(package.toJson());
}

void Profile::removePackage(const QString &name) {
    for (int i = packages.size() - 1; i >= 0; --i)
        if (packages[i].toObject().value("name").toString() == name)
            packages.removeAt(i);
}

QJsonObject Profile::toJson() const {
    QJsonObject root;
    root["schemaVersion"] = schemaVersion;
    root["name"] = name;
    if (!source.isEmpty())
        root["source"] = source;
    QJsonObject settingsObject;
    for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
        QJsonObject record{{"mode", it.value().mode}};
        if (it.value().mode == QLatin1String("set"))
            record["value"] = it.value().value;
        settingsObject[it.key()] = record;
    }
    root["settings"] = settingsObject;
    root["packages"] = sortedBy(packages, [](const QJsonObject &o) { return o.value("name").toString(); });
    QJsonObject desktopObject = desktop;
    if (desktopObject.contains("shortcuts"))
        desktopObject["shortcuts"] = sortedBy(desktopObject.value("shortcuts").toArray(), [](const QJsonObject &o) { return o.value("id").toString(); });
    if (desktopObject.contains("windowRules"))
        desktopObject["windowRules"] = sortedBy(desktopObject.value("windowRules").toArray(), [](const QJsonObject &o) { return o.value("id").toString(); });
    root["desktop"] = desktopObject;
    root["files"] = sortedBy(files, [](const QJsonObject &o) { return o.value("adapter").toString() + "/" + o.value("name").toString(); });
    root["assets"] = sortedBy(assets, [](const QJsonObject &o) { return o.value("sha256").toString(); });
    return root;
}

QByteArray Profile::serialize() const {
    return QJsonDocument(toJson()).toJson(QJsonDocument::Indented);
}

QString Profile::digest() const {
    return QString::fromLatin1(QCryptographicHash::hash(serialize(), QCryptographicHash::Sha256).toHex());
}

bool Profile::sameChoices(const Profile &other) const {
    Profile a = *this, b = other;
    a.source = QJsonObject();
    b.source = QJsonObject();
    return a.serialize() == b.serialize();
}

bool Profile::hasUnsupportedSections() const {
    return !packages.isEmpty() || !desktop.isEmpty() || !files.isEmpty() || !assets.isEmpty();
}

ProfileTraits ProfileTraits::of(const CatalogEntry &entry) {
    ProfileTraits traits;
    const QJsonObject block = entry.profile;
    traits.portability = block.value("portability").toString();
    traits.category = block.value("category").toString();
    traits.reason = block.value("reason").toString();
    traits.inherit = block.value("inherit").toBool();
    traits.hasStock = block.contains("stock");
    traits.stock = block.value("stock").toString();
    for (const QJsonValue &value : block.value("after").toArray())
        traits.after << value.toString();
    for (const QJsonValue &value : block.value("activation").toArray())
        traits.activation << value.toString();
    for (const QJsonValue &value : block.value("overlaps").toArray())
        traits.overlaps << Overlap{value.toObject().value("setting").toString(), value.toObject().value("when").toString()};
    return traits;
}

QStringList ProfileTraits::categories() {
    return {"appearance", "windows", "input", "idle", "defaults", "language"};
}

QString ProfileTraits::describeActivation(const QString &key) {
    if (key == QLatin1String("hooks"))
        return tr("Runs any hooks you have installed for it.");
    if (key == QLatin1String("restarts-shell"))
        return tr("Restarts the Omarchy shell.");
    if (key == QLatin1String("restarts-apps"))
        return tr("Reloads open terminals and other apps.");
    if (key == QLatin1String("reloads-hyprland"))
        return tr("Reloads Hyprland's configuration.");
    if (key == QLatin1String("authentication"))
        return tr("May ask for your password.");
    if (key == QLatin1String("rearranges-workspaces"))
        return tr("Empty workspaces take the new layout; busy ones keep theirs.");
    return key;
}

QString ProfileTraits::packageFor(const CatalogEntry &entry, const QString &value, QString *source) {
    for (const QJsonValue &choice : entry.choices) {
        const QJsonObject object = choice.toObject();
        if (object.value("value").toString() != value)
            continue;
        if (source)
            *source = object.value("packageSource").toString(QStringLiteral("repository"));
        return object.value("package").toString();
    }
    return {};
}

namespace ProfileModel {

QStringList fileAdapters() {
    return {"ghostty", "alacritty"};
}

bool isSettingId(const QString &id) {
    return id.size() <= 64 && settingIdPattern().match(id).hasMatch();
}

bool isPackageName(const QString &name) {
    return packagePattern().match(name).hasMatch();
}

QString manifestPath(const QString &directory) {
    return QDir(directory).filePath(QStringLiteral("profile.json"));
}

Parsed parse(const QByteArray &json) {
    Parsed result;
    if (json.size() > MaxManifestBytes) {
        result.errors << tr("profile.json is larger than 1 MiB.");
        return result;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        result.errors << tr("profile.json is not valid JSON: %1 (at byte %2).").arg(parseError.errorString()).arg(parseError.offset);
        return result;
    }
    if (!document.isObject()) {
        result.errors << tr("profile.json must hold one object.");
        return result;
    }
    DuplicateScan scan{json, &result.errors};
    scan.value(QString(), 0);
    if (scan.tooDeep)
        result.errors << tr("profile.json is nested too deeply.");

    const QJsonObject root = document.object();
    const QJsonValue version = root.value("schemaVersion");
    if (!isWholeNumber(version) || version.toDouble() < 1) {
        result.errors << tr("schemaVersion must be a whole number from 1.");
        return result;
    }
    result.originalSchema = int(version.toDouble());
    if (result.originalSchema > Profile::SchemaVersion) {
        result.errors << tr("This profile was saved by a newer Omarchy Control (profile version %1). Update Omarchy Control to restore it.").arg(result.originalSchema);
        return result;
    }
    // Older versions migrate here, in memory, with each change in meaning noted. Version 1
    // is the first, so there is nothing to migrate yet.

    onlyKeys(root, {"schemaVersion", "name", "source", "settings", "packages", "desktop", "files", "assets"}, QStringLiteral("profile.json"), &result.errors);
    text(root, "name", QStringLiteral("profile.json"), 100, true, &result.errors);
    if (root.value("name").toString().trimmed().isEmpty() && root.value("name").isString())
        result.errors << tr("The profile needs a name.");
    if (root.contains("source")) {
        if (!root.value("source").isObject()) {
            result.errors << tr("source must be an object.");
        } else {
            const QJsonObject source = root.value("source").toObject();
            onlyKeys(source, {"control", "omarchy", "hyprland", "architecture"}, QStringLiteral("source"), &result.errors);
            for (auto it = source.begin(); it != source.end(); ++it)
                text(source, it.key(), QStringLiteral("source"), 64, false, &result.errors);
        }
    }
    if (root.contains("settings"))
        validateSettings(root.value("settings"), &result.errors);
    if (root.contains("packages"))
        validatePackages(root.value("packages"), &result.errors);
    if (root.contains("desktop"))
        validateDesktop(root.value("desktop"), &result.errors);
    qint64 total = 0;
    if (root.contains("files"))
        total += validateFiles(root.value("files"), &result.errors);
    if (root.contains("assets"))
        total += validateAssets(root.value("assets"), &result.errors);
    if (total > MaxProfileBytes)
        result.errors << tr("The profile's files add up to more than 100 MiB.");
    if (!result.errors.isEmpty())
        return result;

    Profile &profile = result.profile;
    profile.schemaVersion = Profile::SchemaVersion;
    profile.name = root.value("name").toString();
    profile.source = root.value("source").toObject();
    const QJsonObject settings = root.value("settings").toObject();
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        const QJsonObject record = it.value().toObject();
        profile.settings.insert(it.key(), ProfileSetting{record.value("mode").toString(), record.value("value").toString()});
    }
    profile.packages = root.value("packages").toArray();
    profile.desktop = root.value("desktop").toObject();
    profile.files = root.value("files").toArray();
    profile.assets = root.value("assets").toArray();
    result.ok = true;
    return result;
}

Parsed load(const QString &directory, bool *exists) {
    const QString path = manifestPath(directory);
    const QFileInfo info(path);
    if (exists)
        *exists = info.exists();
    Parsed result;
    if (!info.exists()) {
        result.errors << tr("There is no profile.json in %1.").arg(directory);
        return result;
    }
    // A link could point anywhere, and a profile is always a plain file of its own.
    if (info.isSymLink() || !info.isFile()) {
        result.errors << tr("profile.json must be a plain file, not a link.");
        return result;
    }
    if (info.size() > MaxManifestBytes) {
        result.errors << tr("profile.json is larger than 1 MiB.");
        return result;
    }
    QByteArray contents;
    QString error;
    if (!FileOps::readFile(path, &contents, &error)) {
        result.errors << error;
        return result;
    }
    return parse(contents);
}

bool save(const QString &directory, const Profile &profile, QString *error) {
    if (!QDir().mkpath(directory)) {
        if (error)
            *error = tr("%1 could not be created.").arg(directory);
        return false;
    }
    const QString path = manifestPath(directory);
    if (QFileInfo(path).isSymLink()) {
        if (error)
            *error = tr("profile.json must be a plain file, not a link.");
        return false;
    }
    // What is written must read back as exactly this profile, or it is not written.
    const QByteArray bytes = profile.serialize();
    const Parsed check = parse(bytes);
    if (!check.ok) {
        if (error)
            *error = check.errors.value(0);
        return false;
    }
    return FileOps::atomicWrite(path, bytes, error, 0644);
}

}  // namespace ProfileModel

namespace ProfileStore {

QString root(const Env &env) {
    return env.appStateDir + "/profiles";
}

QString plansDir(const Env &env) {
    return root(env) + "/plans";
}

QString runsDir(const Env &env) {
    return root(env) + "/runs";
}

QString baselinePath(const Env &env) {
    return root(env) + "/baseline.json";
}

QString newId(const QString &prefix) {
    return prefix + "-" + QUuid::createUuid().toString(QUuid::Id128).left(12);
}

bool isId(const QString &id, const QString &prefix) {
    static const QRegularExpression shape(QStringLiteral("^[a-z]+-[0-9a-f]{12}$"));
    return id.startsWith(prefix + "-") && shape.match(id).hasMatch();
}

bool writeJson(const QString &path, const QJsonObject &object, QString *error) {
    const QString directory = QFileInfo(path).absolutePath();
    QDir().mkpath(directory);
    QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return FileOps::atomicWrite(path, QJsonDocument(object).toJson(QJsonDocument::Indented), error, 0600);
}

QJsonObject readJson(const QString &path, bool *ok) {
    QByteArray contents;
    const bool read = FileOps::readFile(path, &contents);
    const QJsonDocument document = QJsonDocument::fromJson(contents);
    if (ok)
        *ok = read && document.isObject();
    return document.object();
}

QJsonObject baseline(const Env &env) {
    return readJson(baselinePath(env)).value("settings").toObject();
}

bool recordBaseline(const Env &env, const QJsonObject &entries, QString *error) {
    if (entries.isEmpty())
        return true;
    QJsonObject settings = baseline(env);
    for (auto it = entries.begin(); it != entries.end(); ++it)
        settings[it.key()] = it.value();
    return writeJson(baselinePath(env), QJsonObject{{"settings", settings}}, error);
}

}  // namespace ProfileStore
