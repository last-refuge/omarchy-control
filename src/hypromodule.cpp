#include "hypromodule.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>

#include "fileops.h"

static const char Header[] = "-- Managed by Omarchy Control. Edits here are detected and never overwritten;\n"
                             "-- remove this file's require line from hyprland.lua to take over by hand.\n";
static const char DataPrefix[] = "-- omarchy-control-data: ";

HyprModule::HyprModule(const Env &env, Slot slot) : m_env(env), m_slot(slot) {
    m_data = QJsonObject{{"options", QJsonObject{}}, {"monitors", QJsonArray{}}, {"binds", QJsonArray{}}};
}

QString HyprModule::fileName() const {
    return m_slot == Blur ? QStringLiteral("omarchy-control-blur.lua") : QStringLiteral("omarchy-control.lua");
}

QString HyprModule::path() const {
    return m_env.configHome + "/hypr/" + fileName();
}

// The whole require call, quotes and parenthesis included, so neither file's line is
// ever taken for the other's.
QByteArray HyprModule::requireText() const {
    return m_slot == Blur ? QByteArray("require(\"hypr.omarchy-control-blur\")") : QByteArray("require(\"hypr.omarchy-control\")");
}

QByteArray HyprModule::anchorText() const {
    return m_slot == Blur ? QByteArray("require(\"default.hypr.toggles\")") : QByteArray("require(\"hypr.autostart\")");
}

QByteArray HyprModule::importLine() const {
    return requireText() + (m_slot == Blur ? QByteArray(" -- Managed by Omarchy Control; your blur choices, after Omarchy's toggles.")
                                           : QByteArray(" -- Managed by Omarchy Control; loads your choices made there."));
}

QString HyprModule::hyprlandPath() const {
    return m_env.configHome + "/hypr/hyprland.lua";
}

bool HyprModule::isSafeKey(const QString &key) {
    static const QRegularExpression safe("^[A-Za-z0-9_-]+$");
    return safe.match(key).hasMatch();
}

QString HyprModule::luaString(const QString &text) {
    QString out = "\"";
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            // No control character has any business in a setting value, and an
            // unescaped one could end the Lua string early.
            if (c.unicode() >= 0x20)
                out += c;
        }
    }
    return out + "\"";
}

static QString luaKey(const QString &key) {
    static const QRegularExpression identifier("^[A-Za-z_][A-Za-z0-9_]*$");
    return identifier.match(key).hasMatch() ? key : "[" + HyprModule::luaString(key) + "]";
}

static QString luaValue(const QJsonValue &value, int indent) {
    const QString pad(indent * 2, ' ');
    if (value.isBool())
        return value.toBool() ? "true" : "false";
    if (value.isDouble()) {
        const double number = value.toDouble();
        return number == qint64(number) ? QString::number(qint64(number)) : QString::number(number, 'g', 8);
    }
    if (value.isString())
        return HyprModule::luaString(value.toString());
    if (value.isObject()) {
        QString out = "{\n";
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            out += pad + "  " + luaKey(it.key()) + " = " + luaValue(it.value(), indent + 1) + ",\n";
        return out + pad + "}";
    }
    return "nil";
}

QByteArray HyprModule::render() const {
    QString out = QString::fromLatin1(Header);
    out += QString::fromLatin1(DataPrefix) + QString::fromUtf8(QJsonDocument(m_data).toJson(QJsonDocument::Compact)) + "\n\n";

    const auto options = this->options();
    if (!options.isEmpty())
        out += "hl.config(" + luaValue(options, 0) + ")\n\n";

    for (const auto &value : monitors()) {
        const auto monitor = value.toObject();
        out += "hl.monitor({ output = " + luaString(monitor.value("output").toString());
        if (monitor.value("disabled").toBool()) {
            out += ", disabled = true })\n";
            continue;
        }
        out += ", mode = " + luaString(monitor.value("mode").toString());
        out += ", position = " + luaString(monitor.value("position").toString());
        out += ", scale = " + luaValue(monitor.value("scale"), 0);
        if (monitor.value("transform").toInt() != 0)
            out += ", transform = " + QString::number(monitor.value("transform").toInt());
        if (!monitor.value("mirror").toString().isEmpty())
            out += ", mirror = " + luaString(monitor.value("mirror").toString());
        if (monitor.value("vrr").toBool())
            out += ", vrr = 1";
        out += " })\n";
    }
    if (!monitors().isEmpty())
        out += "\n";

    for (const auto &value : rules()) {
        const auto rule = value.toObject();
        const QString action = rule.value("action").toString();
        QString props;
        if (action == QLatin1String("float") || action == QLatin1String("tile"))
            props = action + " = true";
        else if (action == QLatin1String("noblur"))
            props = "no_blur = true";
        else if (action == QLatin1String("workspace"))
            props = "workspace = " + luaString(rule.value("value").toString());
        else if (action == QLatin1String("opacity"))
            props = "opacity = " + luaString(rule.value("value").toString());
        if (!props.isEmpty())
            out += "o.window(" + luaString(rule.value("match").toString()) + ", { " + props + " })\n";
    }
    if (!rules().isEmpty())
        out += "\n";

    for (const auto &value : binds()) {
        const auto bind = value.toObject();
        const QString keys = luaString(bind.value("keys").toString());
        if (bind.value("replaces").toBool())
            out += "hl.unbind(" + keys + ")\n";
        if (bind.value("unbindOnly").toBool())
            continue;
        out += "o.bind(" + keys + ", " + luaString(bind.value("description").toString()) + ", " +
               luaString(bind.value("command").toString()) + ")\n";
    }
    return out.toUtf8();
}

HyprModule::State HyprModule::state() const {
    QByteArray contents;
    if (!QFileInfo::exists(path()) || !FileOps::readFile(path(), &contents))
        return Absent;
    const int begin = contents.indexOf(DataPrefix);
    if (begin < 0)
        return ModifiedByUser;
    const int start = begin + int(strlen(DataPrefix));
    const int end = contents.indexOf('\n', start);
    const auto document = QJsonDocument::fromJson(contents.mid(start, end - start));
    if (!document.isObject())
        return ModifiedByUser;
    HyprModule copy(m_env, m_slot);
    copy.m_data = document.object();
    return copy.render() == contents ? Ours : ModifiedByUser;
}

bool HyprModule::load(QString *error) {
    const State current = state();
    if (current == Absent)
        return true;
    if (current == ModifiedByUser) {
        if (error)
            *error = QObject::tr("%1 was edited outside Omarchy Control, so it was left alone.").arg(fileName());
        return false;
    }
    QByteArray contents;
    FileOps::readFile(path(), &contents);
    const int start = contents.indexOf(DataPrefix) + int(strlen(DataPrefix));
    m_data = QJsonDocument::fromJson(contents.mid(start, contents.indexOf('\n', start) - start)).object();
    return true;
}

QJsonObject HyprModule::inspect() const {
    QJsonObject snapshot{{"path", path()}, {"hyprlandPath", hyprlandPath()},
                         {"binds", QJsonValue::Null}, {"rules", QJsonValue::Null}};
    // Missing children of an inaccessible directory cannot establish absence.
    for (QString parent = QFileInfo(path()).absolutePath();;) {
        const QFileInfo info(parent);
        if ((info.exists() || info.isSymLink()) && (!info.isDir() || !info.isReadable() || !info.isExecutable())) {
            snapshot["status"] = "unavailable";
            snapshot["reason"] = QObject::tr("The owned module's directory could not be inspected.");
            return snapshot;
        }
        const QString next = QFileInfo(parent).absolutePath();
        if (next == parent) break;
        parent = next;
    }
    const QFileInfo info(path());
    if (!info.exists() && !info.isSymLink()) {
        snapshot["status"] = "absent";
        snapshot["binds"] = QJsonArray{};
        snapshot["rules"] = QJsonArray{};
        return snapshot;
    }
    QFile file(path());
    if (!info.isFile() || !file.open(QIODevice::ReadOnly)) {
        snapshot["status"] = "unavailable";
        snapshot["reason"] = QObject::tr("The owned module could not be read.");
        return snapshot;
    }
    const QByteArray contents = file.read(4 * 1024 * 1024 + 1);
    if (file.error() != QFileDevice::NoError || contents.size() > 4 * 1024 * 1024) {
        snapshot["status"] = "unavailable";
        snapshot["reason"] = QObject::tr("The owned module could not be read completely within the size limit.");
        return snapshot;
    }
    const int begin = contents.indexOf(DataPrefix);
    const int start = begin + int(strlen(DataPrefix));
    const auto document = begin < 0 ? QJsonDocument{} : QJsonDocument::fromJson(contents.mid(start, contents.indexOf('\n', start) - start));
    HyprModule copy(m_env, m_slot);
    copy.m_data = document.object();
    if (!document.isObject() || copy.render() != contents) {
        snapshot["status"] = "ownership_conflict";
        snapshot["reason"] = QObject::tr("The module was edited outside Omarchy Control; its contents were left alone.");
        return snapshot;
    }
    snapshot["status"] = "ok";
    snapshot["fingerprint"] = FileOps::fingerprintBytes(contents);
    snapshot["binds"] = copy.binds();
    snapshot["rules"] = copy.rules();
    return snapshot;
}

static QJsonObject withOption(QJsonObject object, const QStringList &path, int index, const QJsonValue &value, bool remove) {
    const QString &key = path[index];
    if (index == path.size() - 1) {
        if (remove)
            object.remove(key);
        else
            object.insert(key, value);
        return object;
    }
    QJsonObject child = withOption(object.value(key).toObject(), path, index + 1, value, remove);
    if (child.isEmpty())
        object.remove(key);
    else
        object.insert(key, child);
    return object;
}

void HyprModule::setOption(const QStringList &path, const QJsonValue &value) {
    m_data.insert("options", withOption(options(), path, 0, value, false));
}

void HyprModule::removeOption(const QStringList &path) {
    m_data.insert("options", withOption(options(), path, 0, {}, true));
}

QJsonValue HyprModule::option(const QStringList &path) const {
    QJsonValue current = options();
    for (const auto &key : path) {
        if (!current.isObject())
            return QJsonValue(QJsonValue::Undefined);
        current = current.toObject().value(key);
    }
    return current;
}

bool HyprModule::hasOption(const QStringList &path) const {
    return !option(path).isUndefined();
}

bool HyprModule::isEmpty() const {
    return options().isEmpty() && monitors().isEmpty() && binds().isEmpty() && rules().isEmpty();
}

bool HyprModule::removeEntirely(QString *error) {
    if (importInstalled() && !removeImport(error))
        return false;
    if (QFileInfo::exists(path()) && !QFile::remove(path())) {
        if (error)
            *error = QObject::tr("%1 could not be removed.").arg(fileName());
        return false;
    }
    return true;
}

bool HyprModule::save(QString *error) {
    return FileOps::atomicWrite(path(), render(), error, 0644);
}

bool HyprModule::importInstalled() const {
    QByteArray contents;
    return FileOps::readFile(hyprlandPath(), &contents) && contents.contains(requireText());
}

bool HyprModule::canInstallImport() const {
    QByteArray contents;
    return FileOps::readFile(hyprlandPath(), &contents) && contents.contains(anchorText());
}

bool HyprModule::plannedImport(QByteArray *planned, QString *error) const {
    QByteArray contents;
    if (!FileOps::readFile(hyprlandPath(), &contents, error))
        return false;
    if (contents.contains(requireText())) { *planned = contents; return true; }
    const QByteArray anchor = anchorText();
    const int at = contents.indexOf(anchor);
    if (at < 0) {
        if (error)
            *error = QObject::tr("hyprland.lua does not have Omarchy's stock layout, so no line was added.");
        return false;
    }
    const int lineEnd = contents.indexOf('\n', at);
    contents.insert(lineEnd < 0 ? contents.size() : lineEnd, QByteArray("\n") + importLine());
    *planned = contents;
    return true;
}

bool HyprModule::installImport(QString *error) {
    QByteArray contents;
    if (!plannedImport(&contents, error)) return false;
    // Create the module first: Lua's require fails outright on a missing file,
    // and Hyprland reloads as soon as hyprland.lua changes.
    if (!QFileInfo::exists(path()) && !save(error)) return false;
    if (FileOps::fingerprint(hyprlandPath()) == FileOps::fingerprintBytes(contents)) return true;
    return FileOps::atomicWrite(hyprlandPath(), contents, error, 0644);
}

bool HyprModule::removeImport(QString *error) {
    QByteArray contents;
    if (!FileOps::readFile(hyprlandPath(), &contents, error))
        return false;
    QList<QByteArray> kept;
    for (const QByteArray &line : contents.split('\n'))
        if (!line.contains(requireText()))
            kept << line;
    return FileOps::atomicWrite(hyprlandPath(), kept.join('\n'), error, 0644);
}
