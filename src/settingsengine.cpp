#include "settingsengine.h"
#include "launchereditor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QTimer>
#include <cmath>
#include <memory>

#include "blur.h"
#include "fileops.h"

QVariantMap ChangePlan::toVariant() const {
    QVariantMap m;
    m["ok"] = ok;
    m["error"] = error;
    m["normalized"] = normalized;
    m["confirmText"] = confirmText;
    m["needsOwnership"] = needsOwnership;
    m["effects"] = effects;
    m["files"] = files;
    m["recovery"] = recovery;
    m["restart"] = restart;
    return m;
}

QJsonObject ChangePlan::toJson() const {
    return QJsonObject::fromVariantMap(toVariant());
}

SettingsEngine::SettingsEngine(const Catalog *catalog, CommandRunner *runner, Capabilities *capabilities,
                               ChangeCoordinator *coordinator, Compat *compat, QObject *parent)
    : QObject(parent), m_catalog(catalog), m_runner(runner), m_capabilities(capabilities), m_coordinator(coordinator), m_compat(compat) {}

QStringList SettingsEngine::expandArgv(const QStringList &argv) const {
    QStringList out;
    for (const auto &arg : argv)
        out << ((arg.startsWith("~/") || arg.contains("{config}") || arg.contains("{state}")) ? m_runner->env().expand(arg) : arg);
    return out;
}

// Which owned Lua file a Hyprland setting is kept in.
static HyprModule::Slot slotOf(const CatalogEntry &entry) {
    return entry.write.value("slot").toString() == QLatin1String("blur") ? HyprModule::Blur : HyprModule::Main;
}

static QStringList pathOf(const QJsonArray &parts) {
    QStringList path;
    for (const auto &part : parts)
        path << part.toString();
    return path;
}

// One option a Hyprland setting writes, or removes.
struct HyprEdit {
    QStringList path;
    QJsonValue value;
    bool remove = false;
};

static QJsonValue typedValue(const CatalogEntry &entry, const QString &value);

// "6×3", or "6x3" as typed, within the ranges Fine-tune offers.
static bool customStrength(const CatalogEntry &entry, const QString &value, int *size, int *passes) {
    if (entry.write.value("presets").toObject().isEmpty())
        return false;
    static const QRegularExpression pair(QStringLiteral("^([0-9]{1,3})[x\u00D7]([0-9]{1,2})$"));
    const auto match = pair.match(value);
    if (!match.hasMatch())
        return false;
    *size = match.captured(1).toInt();
    *passes = match.captured(2).toInt();
    return *size >= 1 && *size <= 20 && *passes >= 1 && *passes <= 4;
}

// Most settings write one option. Some write the same value to a second one, as Color
// boost does to both vibrancy options, and a named strength writes a pair from its
// preset, where a key the preset leaves out is removed so Omarchy's glass applies.
static QList<HyprEdit> hyprEdits(const CatalogEntry &entry, const QString &value, bool remove) {
    QList<HyprEdit> edits;
    const QJsonObject presets = entry.write.value("presets").toObject();
    if (!presets.isEmpty()) {
        const QStringList base = pathOf(entry.write.value("base").toArray());
        QJsonObject preset = presets.value(value).toObject();
        int size = 0, passes = 0;
        if (!presets.contains(value) && customStrength(entry, value, &size, &passes))
            preset = QJsonObject{{"size", size}, {"passes", passes}};
        for (const auto &key : entry.write.value("keys").toArray()) {
            HyprEdit edit;
            edit.path = base + QStringList{key.toString()};
            edit.remove = remove || !preset.contains(key.toString());
            if (!edit.remove)
                edit.value = preset.value(key.toString());
            edits << edit;
        }
        return edits;
    }
    QList<QStringList> paths{pathOf(entry.write.value("path").toArray())};
    for (const auto &also : entry.write.value("also").toArray())
        paths << pathOf(also.toArray());
    for (const auto &path : paths) {
        HyprEdit edit;
        edit.path = path;
        edit.remove = remove;
        if (!remove)
            edit.value = typedValue(entry, value);
        edits << edit;
    }
    return edits;
}

static bool anySet(const HyprModule &module, const CatalogEntry &entry) {
    for (const auto &edit : hyprEdits(entry, {}, true))
        if (module.hasOption(edit.path))
            return true;
    return false;
}

// Hyprland reports 0.750000 where glass.lua says 0.75; both read as 0.75.
static QString plainNumber(const QString &text) {
    bool ok = false;
    const double number = text.toDouble(&ok);
    if (!ok)
        return text;
    QString out = QString::number(number, 'f', 3);
    while (out.endsWith('0'))
        out.chop(1);
    if (out.endsWith('.'))
        out.chop(1);
    return out;
}

static QString mapped(const QJsonObject &map, const QString &value) {
    const auto it = map.constFind(value);
    return it == map.constEnd() ? value : it.value().toString();
}

QString SettingsEngine::parseHyprOption(const QByteArray &json, bool *ok) {
    const auto object = QJsonDocument::fromJson(json).object();
    *ok = true;
    if (object.contains("bool"))
        return object.value("bool").toBool() ? "true" : "false";
    if (object.contains("int"))
        return QString::number(object.value("int").toInt());
    if (object.contains("float")) {
        QString text = QString::number(object.value("float").toDouble(), 'f', 3);
        while (text.endsWith('0'))
            text.chop(1);
        if (text.endsWith('.'))
            text.chop(1);
        return text;
    }
    if (object.contains("str")) {
        const QString text = object.value("str").toString();
        return text == QLatin1String("[[EMPTY]]") ? QString() : text;
    }
    // Gaps are CSS-style, but Omarchy only ever sets all four sides alike.
    if (object.contains("css"))
        return object.value("css").toString().section(' ', 0, 0);
    *ok = false;
    return {};
}

QString SettingsEngine::parseOutput(const CommandResult &result, const QJsonObject &spec, bool *ok) {
    const QString parse = spec.value("parse").toString("trim");
    *ok = true;
    if (parse == QLatin1String("exitBool")) {
        if (result.failure != CommandResult::None) {
            *ok = false;
            return {};
        }
        return result.exitCode == 0 ? "true" : "false";
    }
    if (result.failure != CommandResult::None || (result.exitCode != 0 && !spec.value("allowFailure").toBool())) {
        *ok = false;
        return {};
    }
    QString value;
    if (spec.value("raw").toBool()) {
        value = QString::fromUtf8(result.out);
        while (value.endsWith('\n'))
            value.chop(1);
    } else if (parse == QLatin1String("int")) {
        static const QRegularExpression number("-?[0-9]+");
        const auto match = number.match(QString::fromUtf8(result.out));
        if (!match.hasMatch()) {
            *ok = false;
            return {};
        }
        value = match.captured();
    } else if (parse.startsWith("json:")) {
        QJsonValue field = QJsonDocument::fromJson(result.out).object();
        for (const auto &part : parse.mid(5).split('.'))
            field = field.toObject().value(part);
        if (field.isUndefined()) {
            *ok = false;
            return {};
        }
        value = field.isBool() ? (field.toBool() ? "true" : "false") : field.isDouble() ? QString::number(field.toDouble()) : field.toString();
    } else if (parse.startsWith("field:")) {
        // key<TAB>value rows, as some Omarchy commands print for the shell.
        const QString wanted = parse.mid(6);
        bool found = false;
        for (const auto &line : QString::fromUtf8(result.out).split('\n')) {
            if (line.section('\t', 0, 0) == wanted) {
                value = line.section('\t', 1).trimmed();
                found = true;
            }
        }
        if (!found)
            *ok = false;
    } else if (parse == QLatin1String("activeRow")) {
        for (const auto &line : QString::fromUtf8(result.out).split('\n')) {
            const auto parts = line.split('\t');
            if (parts.size() >= 2 && parts[1].trimmed() == QLatin1String("1"))
                value = parts[0].trimmed();
        }
        if (value.isEmpty())
            *ok = false;
    } else {
        value = result.outText();
    }
    return mapped(spec.value("map").toObject(), value);
}

void SettingsEngine::readFiles(const CatalogEntry &entry, ReadCallback callback) {
    const Env &env = m_runner->env();
    const QString kind = entry.read.value("kind").toString();
    const QString file = env.expand(entry.read.value("file").toString());
    ReadResult result;
    result.status = "ok";
    if (kind == QLatin1String("exists")) {
        const bool exists = QFileInfo::exists(file);
        result.value = exists != entry.read.value("invert").toBool() ? "true" : "false";
    } else if (kind == QLatin1String("filetext")) {
        QByteArray contents;
        if (FileOps::readFile(file, &contents))
            result.value = QString::fromUtf8(contents).section('\n', 0, 0).trimmed();
        else
            result.value = entry.read.value("default").toString();
        result.value = mapped(entry.read.value("map").toObject(), result.value);
    } else if (kind == QLatin1String("symlink")) {
        const QFileInfo info(file);
        if (info.isSymLink())
            result.value = info.canonicalFilePath();
        else {
            result.status = "unknown";
            result.reason = QCoreApplication::translate("SettingsEngine", "No current value was found.");
        }
    } else if (kind == QLatin1String("json")) {
        QStringList path;
        for (const auto &part : entry.read.value("path").toArray())
            path << part.toString();
        QByteArray contents;
        QJsonValue value(QJsonValue::Undefined);
        if (FileOps::readFile(file, &contents)) {
            value = FileOps::jsonGet(contents, path);
            result.source = file;
        }
        if (value.isUndefined() && entry.read.contains("fallbackFile")) {
            QByteArray fallback;
            if (FileOps::readFile(env.expand(entry.read.value("fallbackFile").toString()), &fallback)) {
                value = FileOps::jsonGet(fallback, path);
                result.source = QCoreApplication::translate("SettingsEngine", "Omarchy default");
            }
        }
        if (value.isUndefined()) {
            result.value = entry.read.value("default").toString();
            result.source = QCoreApplication::translate("SettingsEngine", "Omarchy default");
        } else if (value.isDouble()) {
            result.value = QString::number(value.toDouble(), 'g', 10);
            result.isOverride = result.source == file;
        } else if (value.isBool()) {
            result.value = value.toBool() ? "true" : "false";
            result.isOverride = result.source == file;
        } else {
            result.value = value.toString();
            result.isOverride = result.source == file;
        }
    } else if (kind == QLatin1String("toml")) {
        QByteArray contents;
        FileOps::readFile(file, &contents);
        result.value = FileOps::tomlGet(contents, entry.read.value("section").toString(), entry.read.value("key").toString());
        result.isOverride = !result.value.isEmpty();
        if (result.value.isEmpty() && entry.read.contains("fallbackFile")) {
            QByteArray fallback;
            FileOps::readFile(env.expand(entry.read.value("fallbackFile").toString()), &fallback);
            result.value = FileOps::tomlGet(fallback, entry.read.value("section").toString(), entry.read.value("key").toString());
        }
        if (result.value.isEmpty())
            result.value = entry.read.value("default").toString();
        if (result.value.isEmpty()) {
            result.status = "unknown";
            result.reason = QCoreApplication::translate("SettingsEngine", "No current value was found.");
        }
    }
    callback(result);
}

void SettingsEngine::read(const QString &id, ReadCallback callback) {
    const CatalogEntry *entry = m_catalog->find(id);
    if (!entry || entry->read.isEmpty()) {
        ReadResult result;
        result.reason = QCoreApplication::translate("SettingsEngine", "This setting has no way to read its value.");
        callback(result);
        return;
    }
    const auto availability = m_capabilities->availability(*entry);
    if (!availability.available) {
        ReadResult result;
        result.status = "unavailable";
        result.reason = availability.reason;
        callback(result);
        return;
    }
    if (entry->read.value("kind").toString() == QLatin1String("icons")) {
        if (!m_icons) {
            ReadResult result;
            result.status = "unavailable";
            callback(result);
            return;
        }
        m_icons->readState([callback](const IconThemeProvider::State &state) {
            ReadResult result;
            result.extra = state.toVariant();
            if (!state.known) {
                result.reason = state.error;
                callback(result);
                return;
            }
            result.status = "ok";
            result.value = state.value();
            result.reason = state.reason();
            result.isOverride = state.managed && state.fixed;
            result.source = state.effective;
            callback(result);
        });
        return;
    }
    // The launcher icon layer's switch, with what the layer covers and why it may be idle.
    if (entry->read.value("kind").toString() == QLatin1String("iconlayer")) {
        ReadResult result;
        if (!m_icons) {
            result.status = "unavailable";
            callback(result);
            return;
        }
        const QVariantMap state = m_icons->layerState();
        result.status = "ok";
        result.value = state.value("enabled").toBool() ? QStringLiteral("true") : QStringLiteral("false");
        result.reason = IconThemeProvider::layerReason(state);
        result.extra = state;
        callback(result);
        return;
    }
    const QString kind = entry->read.value("kind").toString();
    if (kind == QLatin1String("blur") || kind == QLatin1String("blurstatus")) {
        readBlur(entry, callback);
        return;
    }
    if (kind == QLatin1String("hypr")) {
        const QStringList argv = m_runner->env().hyprctl({"-j", "getoption", entry->read.value("option").toString()});
        const CatalogEntry *e = entry;
        m_runner->run(argv, CommandOptions{3000}, this, [this, e, callback](const CommandResult &command) {
            ReadResult result;
            bool ok = false;
            const QString value = command.ok() ? parseHyprOption(command.out, &ok) : QString();
            if (!ok) {
                result.reason = command.describe("Hyprland");
                callback(result);
                return;
            }
            result.status = "ok";
            result.value = value;
            HyprModule module(m_runner->env());
            QStringList path;
            for (const auto &part : e->write.value("path").toArray())
                path << part.toString();
            const auto moduleState = module.state();
            if (moduleState == HyprModule::ModifiedByUser) {
                result.status = "custom";
                result.source = QCoreApplication::translate("SettingsEngine", "Custom configuration");
                result.reason = QCoreApplication::translate("SettingsEngine", "omarchy-control.lua was edited by hand, so changes here are paused.");
            } else if (module.load() && module.hasOption(path)) {
                result.source = QCoreApplication::translate("SettingsEngine", "Set here");
                result.isOverride = true;
            } else {
                result.source = QCoreApplication::translate("SettingsEngine", "Omarchy default or your config");
            }
            // Omarchy's floating workspaces set some values for themselves while one is
            // active, as focus on click instead of on hover. There Hyprland reports
            // Omarchy's value, not the one configured, so the configured one is shown,
            // and a change is confirmed by the file written and a clean reload.
            const QString floating = e->read.value("floating").toString();
            if (floating.isEmpty() || moduleState == HyprModule::ModifiedByUser
                || m_runner->env().findExecutable("omarchy-hyprland-workspace-layout-current").isEmpty()) {
                callback(result);
                return;
            }
            m_runner->run({"omarchy-hyprland-workspace-layout-current"}, CommandOptions{3000}, this, [this, e, path, floating, result, callback](const CommandResult &layout) mutable {
                if (!layout.ok() || QString::fromUtf8(layout.out).trimmed() != QLatin1String("floating")) {
                    callback(result);
                    return;
                }
                QString source;
                const QString configured = configuredHyprValue(*e, path, &source);
                if (!configured.isEmpty()) {
                    result.value = configured;
                    result.source = source;
                }
                result.extra.insert("floatingWorkspace", true);
                result.reason = floating == QLatin1String("click")
                    ? QCoreApplication::translate("SettingsEngine", "Floating workspaces focus on click. This applies on tiled workspaces.")
                    : QCoreApplication::translate("SettingsEngine", "Floating workspaces set this themselves. This applies on tiled workspaces.");
                callback(result);
            });
        });
        return;
    }
    if (kind == QLatin1String("cmd")) {
        QStringList argv;
        for (const auto &part : entry->read.value("argv").toArray())
            argv << part.toString();
        const QString target = Catalog::targetOf(id);
        argv = targeted(argv, *entry, target);
        CommandOptions options;
        options.timeoutMs = entry->read.value("timeout").toInt(8000);
        const QJsonObject spec = entry->read;
        m_runner->run(expandArgv(argv), options, this, [callback, spec, argv, target](const CommandResult &command) {
            ReadResult result;
            bool ok = false;
            const QString value = SettingsEngine::parseOutput(command, spec, &ok);
            if (ok) {
                result.status = "ok";
                result.value = value;
            } else if (!target.isEmpty() && spec.contains("unsupported") && command.failure == CommandResult::None) {
                // The command ran and said no: this device has no such control, which is
                // a fact about the hardware rather than a failure to read it.
                result.status = "unavailable";
                result.reason = QCoreApplication::translate("catalog", spec.value("unsupported").toString().toUtf8().constData()).replace("{target}", target);
            } else {
                result.reason = command.describe(argv.value(0));
            }
            callback(result);
        });
        return;
    }
    readFiles(*entry, std::move(callback));
}

void SettingsEngine::readChoices(const QString &id, ChoicesCallback callback) {
    const CatalogEntry *entry = m_catalog->find(id);
    if (!entry) {
        callback({});
        return;
    }
    QVariantList choices;
    for (const auto &value : entry->choices) {
        const auto o = value.toObject();
        // A default-app setter installs whatever is missing, so a missing app is
        // not a choice here; installing stays a separate, explicit action.
        if (o.contains("requiresCommand") && !m_capabilities->hasCommand(o.value("requiresCommand").toString()))
            continue;
        QVariantMap choice{{"value", o.value("value").toString()}, {"label", o.value("label").toString()}};
        if (o.contains("icons"))
            choice["icons"] = o.value("icons").toVariant();
        choices << choice;
    }
    const QString kind = entry->choicesFrom.value("kind").toString();
    if (kind == QLatin1String("icons")) {
        if (!m_icons) {
            callback(choices);
            return;
        }
        m_icons->readChoices([callback, choices](const QVariantList &packs) mutable {
            callback(choices + packs);
        });
        return;
    }
    if (kind == QLatin1String("cmd")) {
        QStringList argv;
        for (const auto &part : entry->choicesFrom.value("argv").toArray())
            argv << part.toString();
        const QJsonObject labels = entry->choicesFrom.value("labels").toObject();
        m_runner->run(argv, CommandOptions{5000}, this, [callback, choices, labels](const CommandResult &result) mutable {
            if (result.ok()) {
                for (const auto &line : QString::fromUtf8(result.out).split('\n', Qt::SkipEmptyParts)) {
                    const QString value = line.trimmed();
                    choices << QVariantMap{{"value", value}, {"label", labels.value(value).toString(value)}};
                }
            }
            callback(choices);
        });
        return;
    }
    if (kind == QLatin1String("dir")) {
        const Env &env = m_runner->env();
        QByteArray nameBytes;
        FileOps::readFile(env.stateHome + "/omarchy/current/theme.name", &nameBytes);
        const QString theme = QString::fromUtf8(nameBytes).trimmed();
        QStringList filters;
        for (const auto &extension : entry->choicesFrom.value("extensions").toArray())
            filters << "*." + extension.toString();
        QStringList seen;
        for (const auto &dirValue : entry->choicesFrom.value("dirs").toArray()) {
            QString path = env.expand(dirValue.toString());
            path.replace("{theme}", theme);
            const QDir dir(path);
            for (const auto &file : dir.entryInfoList(filters, QDir::Files | QDir::Readable, QDir::Name)) {
                const QString canonical = file.canonicalFilePath();
                if (seen.contains(canonical))
                    continue;
                seen << canonical;
                choices << QVariantMap{{"value", canonical}, {"label", file.completeBaseName()}};
            }
        }
    } else if (kind == QLatin1String("locales")) {
        QByteArray contents;
        FileOps::readFile(m_runner->env().omarchyPath + "/default/i18n/locales.json", &contents);
        const auto object = QJsonDocument::fromJson(contents).object();
        for (auto it = object.begin(); it != object.end(); ++it) {
            const auto locale = it.value().toObject();
            choices << QVariantMap{{"value", it.key()}, {"label", QString("%1 (%2)").arg(locale.value("name").toString(), locale.value("englishName").toString())}};
        }
    }
    callback(choices);
}

ChangePlan SettingsEngine::plan(const QString &id, const QString &rawValue, const QVariantList &knownChoices) const {
    ChangePlan plan;
    const CatalogEntry *entry = m_catalog->find(id);
    const auto tr = [](const char *text) { return QCoreApplication::translate("SettingsEngine", text); };
    if (!entry) {
        plan.error = tr("Unknown setting.");
        return plan;
    }
    if (!entry->isEditable()) {
        plan.error = tr("This cannot be changed here.");
        return plan;
    }
    const auto availability = m_capabilities->availability(*entry);
    if (!availability.available) {
        plan.error = availability.reason;
        return plan;
    }

    // Reading is always fine. A write stays paused if the command behind it has
    // changed since it was reviewed, until someone has looked.
    if (m_compat) {
        const QString changed = m_compat->changedFor(*entry, m_capabilities->versions().value("hyprland").toString());
        if (!changed.isEmpty()) {
            plan.error = tr("Omarchy's %1 changed since this was tested, so changes here are paused. Review it under Advanced.").arg(changed);
            return plan;
        }
    }

    QString value = rawValue.trimmed();
    if (entry->type == QLatin1String("text") || value != rawValue)
        value = entry->type == QLatin1String("choice") ? rawValue : value;
    const bool iconPack = entry->write.value("kind").toString() == QLatin1String("icons");
    if (iconPack)
        value = IconThemeProvider::normalizeChoice(value);
    if (entry->type == QLatin1String("toggle")) {
        if (value != QLatin1String("true") && value != QLatin1String("false")) {
            plan.error = tr("Expected on or off.");
            return plan;
        }
    } else if (entry->type == QLatin1String("int") || entry->type == QLatin1String("slider")) {
        bool ok = false;
        const int number = value.toInt(&ok);
        if (!ok || (entry->hasRange && (number < entry->min || number > entry->max))) {
            plan.error = tr("Choose a whole number from %1 to %2.").arg(entry->min).arg(entry->max);
            return plan;
        }
    } else if (entry->type == QLatin1String("float")) {
        bool ok = false;
        const double number = value.toDouble(&ok);
        if (!ok || std::isnan(number) || (entry->hasRange && (number < entry->min || number > entry->max))) {
            plan.error = tr("Choose a number from %1 to %2.").arg(entry->min).arg(entry->max);
            return plan;
        }
    } else if (entry->type == QLatin1String("choice")) {
        bool known = false;
        for (const auto &choice : entry->choices) {
            const auto object = choice.toObject();
            if (object.value("value").toString() != value)
                continue;
            // A default-app setter opens an installer when the app is missing, so it
            // is refused here for the CLI and for an undo, not just hidden in the list.
            if (object.contains("requiresCommand") && !m_capabilities->hasCommand(object.value("requiresCommand").toString())) {
                plan.error = tr("That app is not installed. Install it first under Apps & defaults.");
                return plan;
            }
            known = true;
        }
        for (const auto &choice : knownChoices)
            if (choice.toMap().value("value").toString() == value)
                known = true;
        // A strength set through its parts reads back as "size×passes", and undo has
        // to be able to put such a pair back.
        int customSize = 0, customPasses = 0;
        if (!known && customStrength(*entry, value, &customSize, &customPasses))
            known = true;
        const bool liveList = !entry->choicesFrom.isEmpty();
        if (!known && !entry->allowCustomValue && !(liveList && knownChoices.isEmpty())) {
            plan.error = tr("That is not one of the available choices.");
            return plan;
        }
        // Omarchy's own setters run sed over these values, so a live-list value
        // whose list could not be read is refused rather than passed along.
        if (!known && liveList && knownChoices.isEmpty() && !entry->allowCustomValue) {
            plan.error = tr("The list of choices could not be read, so this was not changed.");
            return plan;
        }
    }
    if (!entry->pattern.isEmpty()) {
        const QRegularExpression pattern(entry->pattern);
        if (!pattern.match(value).hasMatch()) {
            plan.error = tr("That value is not in the expected format.");
            return plan;
        }
    }
    // Control characters are never part of a real value and could confuse the
    // text formats Omarchy's scripts edit. Stacked clock formats need newlines.
    for (const QChar c : value) {
        if (c.category() == QChar::Other_Control && !(c == QLatin1Char('\n') && id == QLatin1String("desktop.clock.vertical"))) {
            plan.error = tr("That value contains characters that are not allowed.");
            return plan;
        }
    }

    plan.normalized = value;
    plan.effects = entry->effects;
    plan.recovery = entry->recovery;
    plan.restart = entry->restart;
    for (const auto &file : entry->files)
        plan.files << m_runner->env().expand(file);
    if (iconPack && m_icons) {
        plan.files = m_icons->plannedFiles();
        plan.effects += m_icons->plannedEffects();
    }

    if (!entry->confirmAlways.isEmpty())
        plan.confirmText = entry->confirmAlways;
    else if (entry->confirmWhen.contains(value))
        plan.confirmText = entry->confirmWhen.value(value).toString();

    if (id == QLatin1String("displays.scale")) {
        // A saved layout loads after monitors.lua, where this command writes, so the
        // change would quietly vanish at the next reload.
        HyprModule module(m_runner->env());
        if (module.load() && !module.monitors().isEmpty()) {
            plan.error = tr("Your saved display layout sets the scale. Change it under Arrangement.");
            return plan;
        }
    }

    if (entry->write.value("kind").toString() == QLatin1String("hypr")) {
        HyprModule module(m_runner->env(), slotOf(*entry));
        plan.files << module.path();
        if (module.state() == HyprModule::ModifiedByUser) {
            plan.error = tr("%1 was edited by hand, so this is paused. Edit it directly under Advanced.").arg(module.fileName());
            return plan;
        }
        // Choosing Omarchy's own strength removes values rather than adding any, so
        // it needs no new line in hyprland.lua.
        bool adds = false;
        for (const auto &edit : hyprEdits(*entry, plan.normalized, plan.normalized.isEmpty()))
            adds = adds || !edit.remove;
        if (adds && !module.importInstalled()) {
            if (!module.canInstallImport()) {
                plan.error = tr("hyprland.lua does not have Omarchy's stock layout, so this can only be changed by editing it directly under Advanced.");
                return plan;
            }
            plan.needsOwnership = true;
            plan.files << module.hyprlandPath();
            plan.confirmText = module.slot() == HyprModule::Blur
                ? tr("To save this, Omarchy Control adds one marked line to hyprland.lua and keeps your blur choices in omarchy-control-blur.lua. It loads after Omarchy's toggles, because Glassy sets every blur value as it loads. Nothing else in your config changes.")
                : tr("To save this, Omarchy Control adds one marked line to hyprland.lua and keeps your choices in omarchy-control.lua. It loads after your own files and before Omarchy's toggles. Nothing else in your config changes.");
        }
    }
    plan.ok = true;
    return plan;
}

bool SettingsEngine::valuesMatch(const CatalogEntry &entry, const QString &a, const QString &b) {
    if (a == b)
        return true;
    bool okA = false, okB = false;
    const double numberA = a.toDouble(&okA), numberB = b.toDouble(&okB);
    if (!okA || !okB)
        return false;
    if (entry.type == QLatin1String("slider"))
        return std::abs(numberA - numberB) <= 2;
    return std::abs(numberA - numberB) < 0.0005;
}

QStringList SettingsEngine::targeted(const QStringList &argv, const CatalogEntry &entry, const QString &target) {
    QStringList out;
    for (const auto &part : argv) {
        if (part != QLatin1String("{targetArgs}")) {
            out << part;
            continue;
        }
        if (target.isEmpty())
            continue;
        for (QString arg : entry.targetArgs)
            out << arg.replace("{target}", target);
    }
    return out;
}

QStringList SettingsEngine::substitute(const QJsonArray &argv, const QString &value, const CatalogEntry *entry, const QString &target) const {
    // Paths expand before the value goes in, so text someone typed stays literal.
    QStringList templates;
    for (const auto &part : argv)
        templates << part.toString();
    if (entry)
        templates = targeted(templates, *entry, target);
    QStringList out = expandArgv(templates);
    for (auto &text : out)
        text.replace("{value}", value);
    return out;
}

// The value a Hyprland option is configured to, from the files that set it rather than
// the compositor: what this app wrote, else an uncommented assignment in the user's own
// files, else Omarchy's default. Read as text, never run.
void SettingsEngine::hyprOptions(const QStringList &names, std::function<void(const QHash<QString, QString> &, const QString &)> done) {
    auto values = std::make_shared<QHash<QString, QString>>();
    auto next = std::make_shared<std::function<void(int)>>();
    *next = [this, names, values, done, next](int index) {
        if (index >= names.size()) {
            done(*values, {});
            *next = nullptr;
            return;
        }
        m_runner->run(m_runner->env().hyprctl({"-j", "getoption", names[index]}), CommandOptions{3000}, this,
                      [values, names, index, done, next](const CommandResult &command) {
            bool ok = false;
            const QString value = command.ok() ? parseHyprOption(command.out, &ok) : QString();
            if (!ok) {
                done(*values, command.describe("Hyprland"));
                *next = nullptr;
                return;
            }
            values->insert(names[index], value);
            (*next)(index + 1);
        });
    };
    (*next)(0);
}

// Blur rows show what applies when blur is on, which is not always what Hyprland
// holds right now: with no Glassy toggle on, Omarchy's glass values are not loaded,
// yet they are what turning one on brings. A value chosen here is in the config
// either way, so Hyprland's own reading is shown for it.
void SettingsEngine::readBlur(const CatalogEntry *entry, ReadCallback callback) {
    const bool isStatus = entry->read.value("kind").toString() == QLatin1String("blurstatus");
    const QJsonObject presets = entry->write.value("presets").toObject();
    QStringList keys;
    if (!presets.isEmpty())
        for (const auto &key : entry->write.value("keys").toArray())
            keys << key.toString();
    else if (!isStatus)
        keys << entry->write.value("path").toArray().last().toString();
    QStringList names{QStringLiteral("decoration:blur:enabled")};
    for (const auto &key : keys)
        names << "decoration:blur:" + key;
    hyprOptions(names, [this, entry, isStatus, presets, keys, callback](const QHash<QString, QString> &live, const QString &error) {
        ReadResult result;
        if (!error.isEmpty()) {
            result.reason = error;
            callback(result);
            return;
        }
        const Env &env = m_runner->env();
        const Blur::Toggles toggles = Blur::toggles(env);
        const bool enabled = live.value("decoration:blur:enabled") == QLatin1String("true");
        if (isStatus) {
            result.status = "ok";
            result.value = Blur::status(toggles, enabled);
            result.reason = Blur::statusReason(toggles, enabled);
            result.extra.insert("enabled", enabled);
            result.extra.insert("windows", toggles.windows);
            result.extra.insert("bar", toggles.bar);
            result.extra.insert("noAnimations", toggles.noAnimations);
            callback(result);
            return;
        }
        HyprModule module(env, HyprModule::Blur);
        if (module.state() == HyprModule::ModifiedByUser) {
            result.status = "custom";
            result.value = live.value("decoration:blur:" + keys.value(0));
            result.source = QCoreApplication::translate("SettingsEngine", "Custom configuration");
            result.reason = QCoreApplication::translate("SettingsEngine", "%1 was edited by hand, so changes here are paused.").arg(module.fileName());
            callback(result);
            return;
        }
        module.load();
        const QHash<QString, QString> glass = Blur::glassValues(env);
        const bool glassApplies = toggles.windows || toggles.bar || !enabled;
        const QStringList base = presets.isEmpty() ? pathOf(entry->write.value("path").toArray()).mid(0, 2) : pathOf(entry->write.value("base").toArray());
        QHash<QString, QString> values;
        bool fromHere = false, fromGlass = false;
        for (const auto &key : keys) {
            const QString now = live.value("decoration:blur:" + key);
            if (module.hasOption(base + QStringList{key})) {
                values.insert(key, now);
                fromHere = true;
            } else if (glassApplies && glass.contains(key)) {
                values.insert(key, plainNumber(glass.value(key)));
                fromGlass = true;
            } else {
                values.insert(key, now);
            }
        }
        result.status = "ok";
        result.isOverride = fromHere || anySet(module, *entry);
        result.source = result.isOverride ? QCoreApplication::translate("SettingsEngine", "Set here")
                        : fromGlass       ? QCoreApplication::translate("SettingsEngine", "Omarchy's glass")
                                          : QCoreApplication::translate("SettingsEngine", "Omarchy default or your config");
        if (!presets.isEmpty()) {
            result.value = Blur::strengthName(presets, values.value("size"), values.value("passes"), glass);
            result.extra.insert("size", values.value("size"));
            result.extra.insert("passes", values.value("passes"));
        } else {
            result.value = values.value(keys.value(0));
        }
        result.extra.insert("blurOn", enabled);
        callback(result);
    });
}

QString SettingsEngine::configuredHyprValue(const CatalogEntry &entry, const QStringList &path, QString *source) const {
    HyprModule module(m_runner->env());
    if (module.load() && module.hasOption(path)) {
        *source = QCoreApplication::translate("SettingsEngine", "Set here");
        return module.option(path).toVariant().toString();
    }
    const QString key = path.isEmpty() ? QString() : path.last();
    const QRegularExpression assignment(QStringLiteral("^\\s*%1\\s*=\\s*(-?[0-9.]+|true|false)\\s*,?\\s*(--.*)?$").arg(QRegularExpression::escape(key)));
    auto scan = [&assignment](const QString &file) {
        QFile f(file);
        QString found;
        if (!f.open(QIODevice::ReadOnly))
            return found;
        for (const QString &line : QString::fromUtf8(f.readAll()).split('\n')) {
            const auto match = assignment.match(line);
            if (match.hasMatch())
                found = match.captured(1);
        }
        return found;
    };
    const QDir user(m_runner->env().configHome + "/hypr");
    QString value;
    for (const QString &name : user.entryList({"*.lua"}, QDir::Files, QDir::Name)) {
        if (name == QLatin1String("omarchy-control.lua"))
            continue;
        const QString found = scan(user.filePath(name));
        if (!found.isEmpty())
            value = found;
    }
    if (!value.isEmpty()) {
        *source = QCoreApplication::translate("SettingsEngine", "Your config");
        return value;
    }
    Q_UNUSED(entry);
    value = scan(m_runner->env().omarchyPath + "/default/hypr/" + path.value(0) + ".lua");
    if (!value.isEmpty())
        *source = QCoreApplication::translate("SettingsEngine", "Omarchy default");
    return value;
}

void SettingsEngine::verifyValue(const QString &id, const QString &expected, int attempts, ChangeCoordinator::Done done) {
    read(id, [this, id, expected, attempts, done](const ReadResult &result) {
        const CatalogEntry *entry = m_catalog->find(id);
        if (entry && (result.status == QLatin1String("ok") || result.status == QLatin1String("custom"))) {
            bool near = false;
            // Hyprland rounds a scale to one the mode can show, so the value it settles
            // on is the true result, as long as it is close to what was asked for.
            if (entry->write.value("verify").toString() == QLatin1String("near"))
                near = std::abs(result.value.toDouble() - expected.toDouble()) <= 0.2;
            if (near || valuesMatch(*entry, result.value, expected)) {
                done(true, {});
                return;
            }
        }
        if (attempts <= 1) {
            if (result.status != QLatin1String("ok") && !result.reason.isEmpty()) {
                done(false, QCoreApplication::translate("SettingsEngine", "The result could not be confirmed: %1").arg(result.reason));
                return;
            }
            done(false, QCoreApplication::translate("SettingsEngine", "The value did not change. Another part of your configuration may set it."));
            return;
        }
        QTimer::singleShot(250, this, [this, id, expected, attempts, done] { verifyValue(id, expected, attempts - 1, done); });
    });
}

void SettingsEngine::applyCommand(const CatalogEntry &entry, const QJsonObject &write, const QString &value, const QString &before,
                                  const QString &target, ChangeCoordinator::Done done) {
    QString mappedValue = mapped(write.value("map").toObject(), value);
    QJsonArray argv = write.value("argv").toArray();
    if (value.isEmpty() && write.contains("clear"))
        argv = write.value("clear").toArray();
    // Some Omarchy commands only toggle: run one only when it would move toward
    // the requested state, which is the read-then-act race the plan calls out.
    if (write.value("toggleOnly").toBool() && before == value) {
        done(true, {});
        return;
    }
    CommandOptions options;
    options.timeoutMs = write.value("timeout").toInt(15000);
    options.mutation = true;
    const QStringList command = substitute(argv, mappedValue, &entry, target);
    const QJsonArray after = write.value("after").toArray();
    const QString label = entry.label;
    m_runner->run(command, options, this, [this, done, after, command](const CommandResult &result) {
        if (!result.ok()) {
            done(false, result.describe(command.value(0)));
            return;
        }
        if (!after.isEmpty()) {
            CommandOptions best;
            best.mutation = true;
            best.timeoutMs = 5000;
            // Telling the shell to look again is best effort; verification decides.
            m_runner->run(substitute(after, {}), best, this, [done](const CommandResult &) { done(true, {}); });
            return;
        }
        done(true, {});
    });
}

void SettingsEngine::apply(const QString &id, const QString &value, ApplyCallback callback, const QString &kind, const QString &runId) {
    const CatalogEntry *entry = m_catalog->find(id);
    if (!entry) {
        Transaction tx;
        tx.phase = "failed";
        tx.message = QCoreApplication::translate("SettingsEngine", "Unknown setting.");
        callback(tx);
        return;
    }
    // The value is rechecked against a fresh list on every apply, including one
    // from the CLI, so a stale plan cannot pass something no longer offered.
    readChoices(id, [this, id, value, callback, kind, entry, runId](const QVariantList &choices) {
        m_lastChoices.insert(id, choices);
        const ChangePlan plan = this->plan(id, value, choices);
        if (!plan.ok) {
            Transaction tx;
            tx.settingId = id;
            tx.label = entry->label;
            tx.phase = "failed";
            tx.message = plan.error;
            callback(tx);
            return;
        }
        read(id, [this, id, entry, plan, callback, kind, runId](const ReadResult &current) {
            const bool known = current.status == QLatin1String("ok") || current.status == QLatin1String("custom");
            const QString writeKind = entry->write.value("kind").toString();
            if (writeKind == QLatin1String("icons")) {
                if (m_icons)
                    m_icons->submit(plan.normalized, kind, entry->label, callback);
                return;
            }
            if (writeKind == QLatin1String("hypr")) {
                const QString after = plan.normalized;
                const bool remove = after.isEmpty();
                submitHypr(*entry, after, current.value, !current.isOverride, remove, kind, callback, runId);
                return;
            }
            if (writeKind == QLatin1String("json")) {
                submitJson(*entry, plan.normalized, current.value, !current.isOverride, false, kind, callback, runId);
                return;
            }
            // Offering undo means promising the old value can be set again. Some cannot:
            // a custom DNS list, an app since removed, a preference never saved.
            const bool restorable = known && this->plan(id, current.value, m_lastChoices.value(id)).ok;
            ChangeCoordinator::Request request;
            request.settingId = id;
            request.label = entry->label;
            request.kind = kind;
            request.before = known ? current.value : QString();
            request.after = plan.normalized;
            request.recovery = restorable ? entry->recovery : QStringLiteral("no-undo");
            request.beforeWasDefault = !current.isOverride && !entry->reset.isEmpty();
            request.runId = runId;
            for (const auto &file : plan.files)
                request.files << file;
            const QJsonObject write = entry->write;
            const QString value = plan.normalized;
            const QString before = current.value;
            const CatalogEntry *e = entry;
            request.apply = [this, id, e, write, value, before](ChangeCoordinator::Done done) {
                applyCommand(*e, write, value, before, Catalog::targetOf(id), [this, id, e, value, before, done](bool ok, const QString &error) {
                    if (ok || valuesMatch(*e, before, value)) {
                        done(ok, error);
                        return;
                    }
                    // Exit status is not the whole story. Several Omarchy scripts end by
                    // sending a notification, so they exit 1 after doing their work when
                    // the shell is down. Only the value itself can say what happened.
                    read(id, [e, value, error, done](const ReadResult &now) {
                        done(now.status == QLatin1String("ok") && valuesMatch(*e, now.value, value), error);
                    });
                });
            };
            request.verify = [this, id, value](ChangeCoordinator::Done done) { verifyValue(id, value, 12, done); };
            if (restorable && before != value && entry->recovery != QLatin1String("no-undo")) {
                request.rollback = [this, id, e, write, before](ChangeCoordinator::Done done) {
                    // Look first: the failed attempt may not have changed anything, and
                    // a toggle-only command would then flip it the wrong way.
                    read(id, [this, id, e, write, before, done](const ReadResult &now) {
                        if (now.status == QLatin1String("ok") && valuesMatch(*e, now.value, before)) {
                            done(true, {});
                            return;
                        }
                        // Going back through the same command keeps Omarchy's own side
                        // effects consistent, and only a read-back proves it worked.
                        applyCommand(*e, write, before, now.value, Catalog::targetOf(id), [this, id, before, done](bool ok, const QString &error) {
                            // Same reasoning as above: a nonzero exit is not the last word here
                            // either, and the read-back below decides.
                            Q_UNUSED(ok);
                            verifyValue(id, before, 8, [error, done](bool restored, const QString &verifyError) {
                                done(restored, restored ? QString() : (error.isEmpty() ? verifyError : error));
                            });
                        });
                    });
                };
            }
            if (entry->write.value("verify").toString() == QLatin1String("near")) {
                auto settled = std::make_shared<QString>(plan.normalized);
                const auto check = request.verify;
                request.verify = [this, id, check, settled](ChangeCoordinator::Done done) {
                    check([this, id, settled, done](bool ok, const QString &error) {
                        if (!ok) {
                            done(false, error);
                            return;
                        }
                        read(id, [settled, done](const ReadResult &now) {
                            if (now.status == QLatin1String("ok"))
                                *settled = now.value;
                            done(true, {});
                        });
                    });
                };
                request.settled = [settled] { return *settled; };
            }
            m_coordinator->submit(std::move(request), callback);
        });
    });
}

static QJsonValue typedValue(const CatalogEntry &entry, const QString &value) {
    if (entry.type == QLatin1String("toggle"))
        return value == QLatin1String("true");
    bool ok = false;
    const qlonglong integer = value.toLongLong(&ok);
    if (ok)
        return double(integer);
    const double number = value.toDouble(&ok);
    if (ok)
        return number;
    return value;
}

void SettingsEngine::submitJson(const CatalogEntry &entry, const QString &value, const QString &before, bool beforeWasDefault, bool remove,
                                const QString &kind, ApplyCallback callback, const QString &runId) {
    const Env &env = m_runner->env();
    const QString file = env.expand(entry.write.value("file").toString());
    QStringList path;
    for (const auto &part : entry.write.value("path").toArray())
        path << part.toString();
    ChangeCoordinator::Request request;
    request.settingId = entry.id;
    request.label = entry.label;
    request.kind = kind;
    request.before = before;
    request.after = value;
    request.recovery = entry.recovery;
    request.beforeWasDefault = beforeWasDefault;
    request.runId = runId;
    request.files << file;
    const QJsonObject write = entry.write;
    const QString fallback = entry.read.value("fallbackFile").toString();
    const QString id = entry.id;
    const QString valueType = write.value("valueType").toString();
    const QJsonValue typed = valueType == QLatin1String("int")    ? QJsonValue(value.toDouble())
                             : valueType == QLatin1String("bool") ? QJsonValue(value == QLatin1String("true"))
                                                                  : QJsonValue(value);
    // Some app settings drive work of this app's own once saved, done here rather than by
    // running a copy of this app that may be another version.
    const bool iconLayer = write.value("internal").toString() == QLatin1String("icons.layer");
    request.apply = [this, file, path, typed, write, remove, fallback, iconLayer](ChangeCoordinator::Done done) {
        QByteArray document;
        const Env &env = m_runner->env();
        // A missing or empty config starts from Omarchy's defaults, exactly as
        // its own setters do, so the new key does not shadow everything else.
        if ((!FileOps::readFile(file, &document) || document.trimmed().isEmpty()) && !fallback.isEmpty())
            FileOps::readFile(env.expand(fallback), &document);
        QString error;
        const bool edited = remove ? FileOps::jsonRemove(&document, path, &error) : FileOps::jsonSet(&document, path, typed, &error);
        if (!edited || !FileOps::atomicWrite(file, document, &error)) {
            done(false, error);
            return;
        }
        if (iconLayer && m_icons)
            m_icons->syncLayer(false);
        const QJsonArray after = write.value("after").toArray();
        if (after.isEmpty()) {
            done(true, {});
            return;
        }
        CommandOptions options;
        options.mutation = true;
        options.timeoutMs = 5000;
        m_runner->run(substitute(after, {}), options, this, [done](const CommandResult &) { done(true, {}); });
    };
    auto settled = std::make_shared<QString>(value);
    if (!remove) {
        request.verify = [this, id, value](ChangeCoordinator::Done done) { verifyValue(id, value, 4, done); };
    } else {
        request.verify = [this, id, settled](ChangeCoordinator::Done done) { confirmRead(id, settled, done); };
        request.settled = [settled] { return *settled; };
    }
    const QJsonArray after = write.value("after").toArray();
    request.rollback = [this, after, iconLayer](ChangeCoordinator::Done done) {
        // The file is already back, so the layer follows it back too.
        if (iconLayer && m_icons)
            m_icons->syncLayer(false);
        if (after.isEmpty()) {
            done(true, {});
            return;
        }
        CommandOptions options;
        options.mutation = true;
        options.timeoutMs = 5000;
        m_runner->run(substitute(after, {}), options, this, [done](const CommandResult &) { done(true, {}); });
    };
    m_coordinator->submit(std::move(request), callback);
}

void SettingsEngine::confirmRead(const QString &id, std::shared_ptr<QString> settled, ChangeCoordinator::Done done) {
    // A return to default has no expected value, but it must still read back cleanly,
    // and undo needs to know what it left behind.
    read(id, [settled, done](const ReadResult &now) {
        if (now.status != QLatin1String("ok") && now.status != QLatin1String("custom")) {
            done(false, QCoreApplication::translate("SettingsEngine", "The result could not be confirmed: %1").arg(now.reason));
            return;
        }
        *settled = now.value;
        done(true, {});
    });
}

void SettingsEngine::reloadHyprland(ChangeCoordinator::Done done, const QStringList &baseline, bool checkErrors) {
    CommandOptions options;
    options.mutation = true;
    options.timeoutMs = 5000;
    m_runner->run(m_runner->env().hyprctl({"reload"}), options, this, [this, done, baseline, checkErrors](const CommandResult &reload) {
        if (!reload.ok()) {
            done(false, reload.describe("Hyprland"));
            return;
        }
        if (!checkErrors) {
            done(true, {});
            return;
        }
        // Compare against the errors that already existed, so someone with an
        // unrelated config problem can still change a setting.
        QTimer::singleShot(150, this, [this, done, baseline] {
            m_runner->run(m_runner->env().hyprctl({"-j", "configerrors"}), CommandOptions{3000}, this, [done, baseline](const CommandResult &errors) {
                QStringList introduced;
                for (const auto &value : QJsonDocument::fromJson(errors.out).array()) {
                    const QString text = value.toString().trimmed();
                    if (!text.isEmpty() && !baseline.contains(text))
                        introduced << text;
                }
                if (!introduced.isEmpty()) {
                    done(false, QCoreApplication::translate("SettingsEngine", "Hyprland reported a config error: %1").arg(introduced.first().left(240)));
                    return;
                }
                done(true, {});
            });
        });
    });
}

void SettingsEngine::submitHypr(const CatalogEntry &entry, const QString &value, const QString &before, bool beforeWasDefault, bool remove,
                                const QString &kind, ApplyCallback callback, const QString &runId) {
    const QList<HyprEdit> edits = hyprEdits(entry, value, remove);
    for (const auto &edit : edits) {
        for (const auto &part : edit.path) {
            // Keys come from the reviewed catalog, but this file is executable Lua,
            // so the check also sits at the point of generating it.
            if (!HyprModule::isSafeKey(part)) {
                Transaction tx;
                tx.phase = "failed";
                tx.message = QStringLiteral("Unsafe option key");
                callback(tx);
                return;
            }
        }
    }
    const HyprModule::Slot slot = slotOf(entry);
    HyprModule probe(m_runner->env(), slot);
    ChangeCoordinator::Request request;
    request.settingId = entry.id;
    request.label = entry.label;
    request.kind = kind;
    request.before = before;
    request.after = value;
    request.recovery = entry.recovery;
    request.beforeWasDefault = beforeWasDefault;
    request.runId = runId;
    request.files << probe.path() << probe.hyprlandPath();
    const QString id = entry.id;

    auto baseline = std::make_shared<QStringList>();
    request.apply = [this, edits, slot, baseline](ChangeCoordinator::Done done) {
        m_runner->run(m_runner->env().hyprctl({"-j", "configerrors"}), CommandOptions{3000}, this, [this, edits, slot, baseline, done](const CommandResult &errors) {
            for (const auto &item : QJsonDocument::fromJson(errors.out).array())
                if (!item.toString().trimmed().isEmpty())
                    baseline->append(item.toString().trimmed());
            HyprModule module(m_runner->env(), slot);
            QString error;
            if (!module.load(&error)) {
                done(false, error);
                return;
            }
            bool adds = false;
            for (const auto &edit : edits) {
                if (edit.remove)
                    module.removeOption(edit.path);
                else
                    module.setOption(edit.path, edit.value);
                adds = adds || !edit.remove;
            }
            // Removing a value needs no new ownership. Only setting one adds the import.
            // The blur file goes altogether once nothing is chosen in it, so Omarchy's
            // glass is back exactly as it was.
            bool saved = false;
            if (slot == HyprModule::Blur && module.isEmpty())
                saved = module.removeEntirely(&error);
            else
                saved = module.save(&error) && (!adds || module.installImport(&error));
            if (!saved) {
                done(false, error);
                return;
            }
            reloadHyprland(done, *baseline, true);
        });
    };
    if (!remove && entry.write.value("presets").toObject().value(value).toObject().isEmpty() && !entry.write.value("presets").toObject().isEmpty()) {
        // Omarchy's own strength removes values; it is confirmed by what reads back.
        auto settled = std::make_shared<QString>();
        request.verify = [this, id, settled](ChangeCoordinator::Done done) { confirmRead(id, settled, done); };
        request.settled = [settled] { return *settled; };
    } else if (!remove) {
        request.verify = [this, id, value](ChangeCoordinator::Done done) {
            verifyValue(id, value, 8, [done](bool ok, const QString &error) {
                if (ok) {
                    done(true, {});
                    return;
                }
                done(false, error.isEmpty() ? QCoreApplication::translate("SettingsEngine", "Another part of your Hyprland config sets this value.") : error);
            });
        };
    } else {
        auto settled = std::make_shared<QString>();
        request.verify = [this, id, settled](ChangeCoordinator::Done done) { confirmRead(id, settled, done); };
        request.settled = [settled] { return *settled; };
    }
    request.rollback = [this](ChangeCoordinator::Done done) {
        // The coordinator restored the files already; bring the compositor along.
        reloadHyprland(done, {}, false);
    };
    m_coordinator->submit(std::move(request), callback);
}

void SettingsEngine::reset(const QString &id, ApplyCallback callback, const QString &runId) {
    const CatalogEntry *entry = m_catalog->find(id);
    Transaction failed;
    failed.settingId = id;
    failed.phase = "failed";
    if (!entry) {
        failed.message = QCoreApplication::translate("SettingsEngine", "Unknown setting.");
        callback(failed);
        return;
    }
    failed.label = entry->label;
    const QString writeKind = entry->write.value("kind").toString();
    if (writeKind == QLatin1String("icons")) {
        apply(id, QString::fromLatin1(IconThemeProvider::FollowTheme), std::move(callback), QStringLiteral("reset"), runId);
        return;
    }
    read(id, [this, id, entry, writeKind, callback, failed, runId](const ReadResult &current) {
        if (writeKind == QLatin1String("hypr")) {
            HyprModule module(m_runner->env(), slotOf(*entry));
            if (module.state() == HyprModule::ModifiedByUser) {
                Transaction tx = failed;
                tx.message = QCoreApplication::translate("SettingsEngine", "%1 was edited by hand, so it was left alone.").arg(module.fileName());
                callback(tx);
                return;
            }
            if (!module.load() || !anySet(module, *entry)) {
                // Nothing to remove, and no reason to touch the user's config.
                Transaction tx = failed;
                tx.message = QCoreApplication::translate("SettingsEngine", "This is already using the default.");
                callback(tx);
                return;
            }
            submitHypr(*entry, {}, current.value, false, true, "reset", callback, runId);
            return;
        }
        if (writeKind == QLatin1String("json")) {
            submitJson(*entry, {}, current.value, false, true, "reset", callback, runId);
            return;
        }
        if (entry->reset.isEmpty()) {
            Transaction tx = failed;
            tx.message = QCoreApplication::translate("SettingsEngine", "This setting has no separate default to return to.");
            callback(tx);
            return;
        }
        ChangeCoordinator::Request request;
        request.settingId = id;
        request.label = entry->label;
        request.kind = "reset";
        request.before = current.value;
        request.recovery = entry->recovery;
        request.runId = runId;
        QStringList argv;
        for (const auto &part : entry->reset)
            argv << part.toString();
        request.apply = [this, argv](ChangeCoordinator::Done done) {
            CommandOptions options;
            options.mutation = true;
            options.timeoutMs = 20000;
            m_runner->run(argv, options, this, [done, argv](const CommandResult &result) { done(result.ok(), result.describe(argv.value(0))); });
        };
        auto settled = std::make_shared<QString>();
        request.verify = [this, id, settled](ChangeCoordinator::Done done) { confirmRead(id, settled, done); };
        request.settled = [settled] { return *settled; };
        m_coordinator->submit(std::move(request), callback);
    });
}

void SettingsEngine::undo(const QString &transactionId, ApplyCallback callback) {
    const Transaction original = m_coordinator->transaction(transactionId);
    Transaction failed;
    failed.settingId = original.settingId;
    failed.label = original.label;
    failed.phase = "failed";
    if (original.id.isEmpty() || !original.undoable()) {
        failed.message = QCoreApplication::translate("SettingsEngine", "This change cannot be undone automatically.");
        callback(failed);
        return;
    }
    const CatalogEntry *entry = m_catalog->find(original.settingId);
    if (!entry) {
        failed.message = QCoreApplication::translate("SettingsEngine", "This setting no longer exists.");
        callback(failed);
        return;
    }
    // A launcher change puts back the files it wrote.
    if (original.settingId == QLatin1String("apps.launcher.edit") && m_launchers) {
        m_launchers->undo(original, [this, callback, transactionId](const Transaction &result) {
            if (result.phase == QLatin1String("committed"))
                m_coordinator->markUndoneBy(transactionId, result.id);
            callback(result);
        });
        return;
    }
    // A removed icon pack comes back out of the Trash.
    if (original.settingId == QLatin1String("appearance.icons.remove") && m_icons) {
        m_icons->undoRemoval(original, [this, callback, transactionId](const Transaction &result) {
            if (result.phase == QLatin1String("committed"))
                m_coordinator->markUndoneBy(transactionId, result.id);
            callback(result);
        });
        return;
    }
    if (entry->write.value("kind").toString() == QLatin1String("icons")) {
        if (!m_icons) {
            failed.message = QCoreApplication::translate("SettingsEngine", "This change cannot be undone automatically.");
            callback(failed);
            return;
        }
        m_icons->undo(original, [this, callback, transactionId](const Transaction &result) {
            if (result.phase == QLatin1String("committed"))
                m_coordinator->markUndoneBy(transactionId, result.id);
            callback(result);
        });
        return;
    }
    // Undo only goes back from the state this change left. If anything has
    // edited the value since, that later edit is not ours to overwrite.
    read(original.settingId, [this, entry, original, callback, failed, transactionId](const ReadResult &current) {
        if (!valuesMatch(*entry, current.value, original.after)) {
            Transaction tx = failed;
            tx.message = QCoreApplication::translate("SettingsEngine", "The value changed after this, so undoing it could overwrite that edit.");
            callback(tx);
            return;
        }
        auto finish = [this, callback, transactionId](const Transaction &result) {
            if (result.phase == QLatin1String("committed"))
                m_coordinator->markUndoneBy(transactionId, result.id);
            callback(result);
        };
        if (original.beforeWasDefault && (entry->owned || entry->write.value("kind").toString() == QLatin1String("json") || !entry->reset.isEmpty()))
            reset(original.settingId, finish);
        else
            apply(original.settingId, original.before, finish, "undo");
    });
}

QString SettingsEngine::hyprOrigin(const CatalogEntry &entry) const {
    HyprModule module(m_runner->env(), slotOf(entry));
    if (module.state() == HyprModule::ModifiedByUser)
        return QStringLiteral("custom");
    if (module.load() && anySet(module, entry))
        return QStringLiteral("control");
    QString source;
    const QStringList path = pathOf(entry.write.value("path").toArray());
    // configuredHyprValue reports the user's files before Omarchy's, so "Your config"
    // means an assignment of their own exists, whatever Omarchy's default is.
    const QString value = configuredHyprValue(entry, path, &source);
    if (!value.isEmpty() && source == QCoreApplication::translate("SettingsEngine", "Your config"))
        return QStringLiteral("user");
    return QStringLiteral("omarchy");
}

QString SettingsEngine::hyprModuleValue(const CatalogEntry &entry) const {
    HyprModule module(m_runner->env(), slotOf(entry));
    const QStringList path = pathOf(entry.write.value("path").toArray());
    if (path.isEmpty() || module.state() == HyprModule::ModifiedByUser || !module.load() || !module.hasOption(path))
        return {};
    const QJsonValue value = module.option(path);
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble())
        return plainNumber(QString::number(value.toDouble(), 'f', 6));
    return value.toString();
}

QString SettingsEngine::displayValue(const CatalogEntry &entry, const QString &value, const QVariantList &choices) const {
    const auto it = entry.valueMap.constFind(value);
    if (it != entry.valueMap.constEnd())
        return QCoreApplication::translate("catalog", it.value().toString().toUtf8().constData());
    for (const auto &choice : choices) {
        const auto map = choice.toMap();
        if (map.value("value").toString() == value)
            return QCoreApplication::translate("catalog", map.value("label").toString().toUtf8().constData());
    }
    if (entry.type == QLatin1String("toggle"))
        return value == QLatin1String("true") ? QCoreApplication::translate("SettingsEngine", "On") : QCoreApplication::translate("SettingsEngine", "Off");
    if (value.isEmpty() && !entry.placeholder.isEmpty())
        return QCoreApplication::translate("catalog", entry.placeholder.toUtf8().constData());
    return entry.unit.isEmpty() ? value : value + " " + entry.unit;
}
