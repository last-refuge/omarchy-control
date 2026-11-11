#include "catalog.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

static QStringList toStrings(const QJsonValue &value) {
    QStringList out;
    for (const auto &item : value.toArray())
        out << item.toString();
    return out;
}

QString CatalogEntry::verb() const {
    // Listed so translators and the coverage report can see strings chosen at runtime.
    static const QHash<QString, const char *> byFirstWord{
        {"Add", QT_TRANSLATE_NOOP("HelperLabels", "Add")},
        {"Allow", QT_TRANSLATE_NOOP("HelperLabels", "Allow")},
        {"Browse", QT_TRANSLATE_NOOP("HelperLabels", "Browse")},
        {"Check", QT_TRANSLATE_NOOP("HelperLabels", "Check")},
        {"Clear", QT_TRANSLATE_NOOP("HelperLabels", "Clear")},
        {"Clone", QT_TRANSLATE_NOOP("HelperLabels", "Clone")},
        {"Create", QT_TRANSLATE_NOOP("HelperLabels", "Create")},
        {"Cycle", QT_TRANSLATE_NOOP("HelperLabels", "Cycle")},
        {"Diagnose", QT_TRANSLATE_NOOP("HelperLabels", "Diagnose")},
        {"Factory", QT_TRANSLATE_NOOP("HelperLabels", "Reset")},
        {"Forget", QT_TRANSLATE_NOOP("HelperLabels", "Forget")},
        {"List", QT_TRANSLATE_NOOP("HelperLabels", "View")},
        {"Mirror", QT_TRANSLATE_NOOP("HelperLabels", "Mirror")},
        {"Next", QT_TRANSLATE_NOOP("HelperLabels", "Next")},
        {"Pick", QT_TRANSLATE_NOOP("HelperLabels", "Pick")},
        {"Prune", QT_TRANSLATE_NOOP("HelperLabels", "Prune")},
        {"Read", QT_TRANSLATE_NOOP("HelperLabels", "Read")},
        {"Record", QT_TRANSLATE_NOOP("HelperLabels", "Record")},
        {"Recover", QT_TRANSLATE_NOOP("HelperLabels", "Recover")},
        {"Refresh", QT_TRANSLATE_NOOP("HelperLabels", "Refresh")},
        {"Rename", QT_TRANSLATE_NOOP("HelperLabels", "Rename")},
        {"Resize", QT_TRANSLATE_NOOP("HelperLabels", "Resize")},
        {"Review", QT_TRANSLATE_NOOP("HelperLabels", "Review")},
        {"Screen", QT_TRANSLATE_NOOP("HelperLabels", "Record")},
        {"Screenshot", QT_TRANSLATE_NOOP("HelperLabels", "Capture")},
        {"Send", QT_TRANSLATE_NOOP("HelperLabels", "Send")},
        {"Start", QT_TRANSLATE_NOOP("HelperLabels", "Start")},
        {"Stop", QT_TRANSLATE_NOOP("HelperLabels", "Stop")},
        {"Switch", QT_TRANSLATE_NOOP("HelperLabels", "Switch")},
        {"Sync", QT_TRANSLATE_NOOP("HelperLabels", "Sync")},
        {"Upgrade", QT_TRANSLATE_NOOP("HelperLabels", "Upgrade")},
        {"Validate", QT_TRANSLATE_NOOP("HelperLabels", "Validate")},
        {"Change", QT_TRANSLATE_NOOP("HelperLabels", "Change")},
        {"Choose", QT_TRANSLATE_NOOP("HelperLabels", "Choose")},
        {"Copy", QT_TRANSLATE_NOOP("HelperLabels", "Copy")},
        {"Edit", QT_TRANSLATE_NOOP("HelperLabels", "Edit")},
        {"Hibernate", QT_TRANSLATE_NOOP("HelperLabels", "Hibernate")},
        {"Install", QT_TRANSLATE_NOOP("HelperLabels", "Install")},
        {"Lock", QT_TRANSLATE_NOOP("HelperLabels", "Lock")},
        {"Log", QT_TRANSLATE_NOOP("HelperLabels", "Log out")},
        {"Reboot", QT_TRANSLATE_NOOP("HelperLabels", "Reboot")},
        {"Receive", QT_TRANSLATE_NOOP("HelperLabels", "Receive")},
        {"Reinstall", QT_TRANSLATE_NOOP("HelperLabels", "Reinstall")},
        {"Reload", QT_TRANSLATE_NOOP("HelperLabels", "Reload")},
        {"Remove", QT_TRANSLATE_NOOP("HelperLabels", "Remove")},
        {"Reset", QT_TRANSLATE_NOOP("HelperLabels", "Reset")},
        {"Restart", QT_TRANSLATE_NOOP("HelperLabels", "Restart")},
        {"Restore", QT_TRANSLATE_NOOP("HelperLabels", "Restore")},
        {"Screensaver", QT_TRANSLATE_NOOP("HelperLabels", "Start")},
        {"Share", QT_TRANSLATE_NOOP("HelperLabels", "Share")},
        {"Show", QT_TRANSLATE_NOOP("HelperLabels", "Show")},
        {"Shut", QT_TRANSLATE_NOOP("HelperLabels", "Shut down")},
        {"Suspend", QT_TRANSLATE_NOOP("HelperLabels", "Suspend")},
        {"Update", QT_TRANSLATE_NOOP("HelperLabels", "Update")},
        {"Use", QT_TRANSLATE_NOOP("HelperLabels", "Use")},
    };
    if (label.startsWith(QLatin1String("Sign in")))
        return QT_TRANSLATE_NOOP("HelperLabels", "Sign in");
    if (label.startsWith(QLatin1String("Set up")))
        return QT_TRANSLATE_NOOP("HelperLabels", "Set up");
    if (label.startsWith(QLatin1String("Set ")))
        return QT_TRANSLATE_NOOP("HelperLabels", "Set");
    if (label.startsWith(QLatin1String("Turn on")))
        return QT_TRANSLATE_NOOP("HelperLabels", "Turn on");
    if (label.startsWith(QLatin1String("Turn off")))
        return QT_TRANSLATE_NOOP("HelperLabels", "Turn off");
    if (const auto found = byFirstWord.constFind(label.section(' ', 0, 0)); found != byFirstWord.constEnd())
        return QString::fromLatin1(found.value());
    const QString kind = launch.value("kind").toString();
    if (kind == QLatin1String("page"))
        return QT_TRANSLATE_NOOP("HelperLabels", "View");
    if (kind == QLatin1String("config"))
        return QT_TRANSLATE_NOOP("HelperLabels", "Edit");
    if (kind == QLatin1String("terminal"))
        return QT_TRANSLATE_NOOP("HelperLabels", "Run");
    return QT_TRANSLATE_NOOP("HelperLabels", "Open");
}

QVariantMap CatalogEntry::toVariant() const {
    QVariantMap m;
    m["verb"] = verb();
    m["id"] = id;
    m["page"] = page;
    m["section"] = section;
    m["label"] = label;
    m["description"] = description;
    m["type"] = type;
    m["domain"] = domain;
    m["integration"] = integration;
    m["unit"] = unit;
    m["min"] = min;
    m["max"] = max;
    m["step"] = step;
    m["scope"] = scope;
    m["recovery"] = recovery;
    m["restart"] = restart;
    m["reloads"] = reloads;
    m["confirm"] = confirm;
    m["confirmText"] = confirmText;
    m["effects"] = effects;
    m["placeholder"] = placeholder;
    m["installHint"] = installHint;
    m["keywords"] = keywords;
    m["allowCustomValue"] = allowCustomValue;
    m["owned"] = owned;
    m["hasReset"] = !reset.isEmpty() || owned;
    m["editable"] = isEditable();
    m["crossPages"] = crossPages;
    m["launchKind"] = launch.value("kind").toString();
    m["linkPage"] = launch.value("page").toString();
    m["linkFocus"] = launch.value("focus").toString();
    // The file a config editor opens, for rows that present it as a file.
    m["file"] = launch.value("file").toString();
    // Technical details stay out of the row until someone expands them.
    QStringList details;
    if (launch.contains("command"))
        details << launch.value("command").toString();
    if (launch.contains("argv"))
        details << toStrings(launch.value("argv")).join(' ');
    if (launch.contains("file"))
        details << launch.value("file").toString();
    if (launch.contains("target"))
        details << launch.value("target").toString();
    if (write.contains("argv"))
        details << toStrings(write.value("argv")).join(' ');
    if (read.contains("file"))
        details << read.value("file").toString();
    if (write.contains("file") && write.value("file") != read.value("file"))
        details << write.value("file").toString();
    if (read.contains("option"))
        details << read.value("option").toString();
    m["details"] = details;
    m["target"] = target;
    m["icon"] = iconName;
    m["multiLabel"] = multiLabel;
    m["multiDescription"] = multiDescription;
    m["group"] = group;
    m["tone"] = tone;
    m["link"] = link.toVariantMap();
    m["tones"] = tones.toVariantMap();
    m["discloses"] = discloses;
    // A named strength's parts, so a custom pair can be placed near the nearest step.
    if (write.contains("presets"))
        m["presets"] = write.value("presets").toObject().toVariantMap();
    return m;
}

Catalog::Catalog(const QString &directory) {
    QFile pages(directory + "/pages.json");
    if (pages.open(QIODevice::ReadOnly)) {
        for (const auto &value : QJsonDocument::fromJson(pages.readAll()).array()) {
            const auto o = value.toObject();
            PageInfo page;
            page.id = o.value("id").toString();
            page.label = o.value("label").toString();
            page.icon = o.value("icon").toString();
            page.description = o.value("description").toString();
            page.sections = toStrings(o.value("sections"));
            page.blocks = toStrings(o.value("blocks"));
            page.panels = o.value("panels").toArray();
            page.jumps = toStrings(o.value("jumps"));
            m_pages << page;
        }
    }
    load(directory + "/settings.json");
    load(directory + "/workflows.json");
    QFile exclusions(directory + "/coverage-exclusions.json");
    if (exclusions.open(QIODevice::ReadOnly))
        m_exclusions = QJsonDocument::fromJson(exclusions.readAll()).object();
}

void Catalog::load(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const auto array = QJsonDocument::fromJson(file.readAll()).array();
    m_entries.reserve(m_entries.size() + array.size());
    for (const auto &value : array) {
        const auto o = value.toObject();
        CatalogEntry e;
        e.id = o.value("id").toString();
        e.page = o.value("page").toString();
        e.section = o.value("section").toString();
        e.label = o.value("label").toString();
        e.description = o.value("description").toString();
        e.type = o.value("type").toString();
        e.domain = o.value("domain").toString();
        e.integration = o.value("integration").toString();
        e.keywords = toStrings(o.value("keywords"));
        e.sources = toStrings(o.value("sources"));
        e.crossPages = toStrings(o.value("crossPages"));
        e.effects = toStrings(o.value("effects"));
        e.requiresCommands = toStrings(o.value("requiresCommands"));
        e.requiresCaps = toStrings(o.value("requiresCaps"));
        e.requiresHardware = toStrings(o.value("requiresHardware"));
        e.files = toStrings(o.value("files"));
        e.read = o.value("read").toObject();
        e.write = o.value("write").toObject();
        e.launch = o.value("launch").toObject();
        e.choices = o.value("choices").toArray();
        e.choicesFrom = o.value("choicesFrom").toObject();
        e.confirmWhen = o.value("confirmWhen").toObject();
        e.valueMap = o.value("map").toObject();
        e.reset = o.value("reset").toArray();
        e.hasRange = o.contains("min");
        e.min = o.value("min").toDouble();
        e.max = o.value("max").toDouble();
        e.step = o.value("step").toDouble(1);
        e.allowCustomValue = o.value("allowCustomValue").toBool();
        e.owned = o.value("owned").toBool();
        e.softRequires = o.value("softRequires").toBool();
        e.quick = o.value("quick").toBool();
        e.unit = o.value("unit").toString();
        e.pattern = o.value("pattern").toString();
        e.scope = o.value("scope").toString();
        e.recovery = o.value("recovery").toString(e.isAction() ? QStringLiteral("no-undo") : QStringLiteral("auto-undo"));
        e.restart = o.value("restart").toString();
        e.reloads = o.value("reloads").toString();
        e.confirm = o.value("confirm").toString();
        e.confirmText = o.value("confirmText").toString();
        e.confirmAlways = o.value("confirmAlways").toString();
        e.note = o.value("note").toString();
        e.placeholder = o.value("placeholder").toString();
        e.installHint = o.value("installHint").toString();
        e.target = o.value("target").toString();
        e.targetArgs = toStrings(o.value("targetArgs"));
        e.iconName = o.value("icon").toString();
        e.multiLabel = o.value("multiLabel").toString();
        e.multiDescription = o.value("multiDescription").toString();
        e.group = o.value("group").toString();
        e.tone = o.value("tone").toString();
        e.link = o.value("link").toObject();
        e.tones = o.value("tones").toObject();
        e.discloses = toStrings(o.value("discloses"));
        e.profile = o.value("profile").toObject();
        m_index.insert(e.id, m_entries.size());
        m_entries << e;
    }
}

const CatalogEntry *Catalog::find(const QString &id) const {
    auto it = m_index.constFind(id);
    if (it != m_index.constEnd())
        return &m_entries[it.value()];
    const int at = id.indexOf('@');
    if (at <= 0 || targetOf(id).isEmpty())
        return nullptr;
    it = m_index.constFind(id.left(at));
    if (it == m_index.constEnd() || m_entries[it.value()].target.isEmpty())
        return nullptr;
    return &m_entries[it.value()];
}

QString Catalog::targetOf(const QString &id) {
    const int at = id.indexOf('@');
    if (at < 0)
        return {};
    // The name becomes a command argument, so only a plain connector name may pass.
    static const QRegularExpression plain("^[A-Za-z0-9._-]+$");
    const QString target = id.mid(at + 1);
    return plain.match(target).hasMatch() ? target : QString();
}

const PageInfo *Catalog::page(const QString &id) const {
    for (const auto &page : m_pages)
        if (page.id == id)
            return &page;
    return nullptr;
}

QList<const CatalogEntry *> Catalog::entriesForPage(const QString &page) const {
    QList<const CatalogEntry *> out;
    for (const auto &entry : m_entries)
        if (entry.page == page)
            out << &entry;
    return out;
}

QJsonArray Catalog::panelLayout(const QString &pageId) const {
    const PageInfo *info = page(pageId);
    if (!info || info->panels.isEmpty())
        return {};
    // The rows the page shows, as Backend::sections does: a link to a block on this
    // same page is only there for search to land on.
    QList<const CatalogEntry *> entries;
    for (const auto *e : entriesForPage(pageId))
        if (!(e->launch.value("kind").toString() == QLatin1String("page") && e->launch.value("page").toString() == pageId))
            entries << e;
    // Groups that have a panel of their own leave their section's general panel.
    QHash<QString, QStringList> dedicated;
    for (const auto &row : info->panels)
        for (const auto &column : row.toObject().value("columns").toArray())
            for (const auto &spec : column.toArray()) {
                const auto o = spec.toObject();
                if (!o.value("group").toString().isEmpty())
                    dedicated[o.value("section").toString()] << o.value("group").toString();
            }
    QSet<QString> placed;
    auto fill = [&](const QJsonObject &spec) {
        const QString section = spec.value("section").toString(), group = spec.value("group").toString();
        const QString header = spec.value("header").toString();
        QJsonObject panel = spec;
        panel["title"] = spec.value("title").toString(section);
        QJsonArray rows;
        QString headerId;
        // A library claims its sections item by item, below, not as one section.
        for (const auto *e : std::as_const(entries)) {
            if (spec.contains("library"))
                break;
            if (placed.contains(e->id) || e->section != section)
                continue;
            if (group.isEmpty() ? dedicated.value(section).contains(e->group) : e->group != group)
                continue;
            placed.insert(e->id);
            if (e->id == header)
                headerId = e->id;
            else
                rows << e->id;
        }
        // A library: many sections, each its own collapsible group under a heading.
        if (spec.contains("library")) {
            QJsonArray groups;
            for (const auto &groupValue : spec.value("library").toArray()) {
                QJsonObject heading = groupValue.toObject();
                QJsonArray items;
                for (const auto &itemValue : heading.value("items").toArray()) {
                    QJsonObject item = itemValue.toObject();
                    QJsonArray ids;
                    for (const auto *e : std::as_const(entries))
                        if (!placed.contains(e->id) && e->section == item.value("section").toString()) {
                            placed.insert(e->id);
                            ids << e->id;
                            rows << e->id;
                        }
                    item["rows"] = ids;
                    items << item;
                }
                heading["items"] = items;
                groups << heading;
            }
            panel["library"] = groups;
        }
        panel["header"] = headerId;
        panel["rows"] = rows;
        return panel;
    };
    QJsonArray out;
    for (const auto &rowValue : info->panels) {
        QJsonArray columns;
        for (const auto &column : rowValue.toObject().value("columns").toArray()) {
            QJsonArray panels;
            for (const auto &spec : column.toArray())
                panels << fill(spec.toObject());
            columns << panels;
        }
        // The row keeps whatever else it says about itself, such as column weights.
        QJsonObject row = rowValue.toObject();
        row["columns"] = columns;
        out << row;
    }
    // Whatever no panel claimed, a section at a time: into the page's library if it has
    // one, so a new category of software lands with the rest, or else across the full width.
    QStringList leftover;
    for (const auto *e : std::as_const(entries))
        if (!placed.contains(e->id) && !leftover.contains(e->section))
            leftover << e->section;
    for (int r = 0; r < out.size() && !leftover.isEmpty(); ++r) {
        QJsonObject row = out[r].toObject();
        QJsonArray columns = row.value("columns").toArray();
        for (int c = 0; c < columns.size() && !leftover.isEmpty(); ++c) {
            QJsonArray panels = columns[c].toArray();
            for (int p = 0; p < panels.size() && !leftover.isEmpty(); ++p) {
                QJsonObject panel = panels[p].toObject();
                QJsonArray groups = panel.value("library").toArray();
                if (groups.isEmpty())
                    continue;
                QJsonObject last = groups.last().toObject();
                QJsonArray items = last.value("items").toArray();
                QJsonArray rows = panel.value("rows").toArray();
                for (const auto &section : std::as_const(leftover)) {
                    QJsonArray ids;
                    for (const auto *e : std::as_const(entries))
                        if (!placed.contains(e->id) && e->section == section) {
                            placed.insert(e->id);
                            ids << e->id;
                            rows << e->id;
                        }
                    items << QJsonObject{{"section", section}, {"label", section}, {"icon", "list"}, {"rows", ids}};
                }
                leftover.clear();
                last["items"] = items;
                groups[groups.size() - 1] = last;
                panel["library"] = groups;
                panel["rows"] = rows;
                panels[p] = panel;
            }
            columns[c] = panels;
        }
        row["columns"] = columns;
        out[r] = row;
    }
    for (const auto &section : std::as_const(leftover)) {
        QJsonArray rows;
        for (const auto *e : std::as_const(entries))
            if (!placed.contains(e->id) && e->section == section) {
                placed.insert(e->id);
                rows << e->id;
            }
        const QJsonObject panel{{"section", section}, {"title", section}, {"icon", "list"}, {"header", QString()}, {"rows", rows}};
        out << QJsonObject{{"columns", QJsonArray{QJsonArray{panel}}}};
    }
    return out;
}

QString Catalog::normalize(const QString &text) {
    QString out;
    out.reserve(text.size());
    for (const QChar c : text.normalized(QString::NormalizationForm_KD).toCaseFolded()) {
        if (c.isLetterOrNumber())
            out += c;
        else if (!c.isMark() && !out.endsWith(' '))
            out += ' ';
    }
    return out.trimmed();
}

// Task words people actually type. A synonym points at vocabulary already in a
// row's keywords, so one phrasing does not have to be repeated on every row.
static const QHash<QString, QString> &synonyms() {
    static const QHash<QString, QString> table{
        {"wallpaper", "background"}, {"sleep", "suspend"}, {"mic", "microphone"}, {"wifi", "wi fi"},
        {"speakers", "output"}, {"headphones", "bluetooth"}, {"colour", "color"}, {"hotkey", "shortcut"},
        {"hotkeys", "shortcuts"}, {"keybinding", "shortcut"}, {"keybindings", "shortcuts"}, {"autostart", "startup"},
        {"login", "startup"}, {"resolution", "displays"}, {"monitor", "displays"}, {"monitors", "displays"},
        {"screen", "displays"}, {"backup", "snapshot"}, {"backups", "snapshots"}, {"upgrade", "update"},
        {"password", "sign in"}, {"volume", "output"}, {"uninstall", "remove"}, {"add", "install"},
    };
    return table;
}

QList<SearchHit> Catalog::search(const QString &query, int limit) const {
    QList<SearchHit> hits;
    const QString q = normalize(query);
    if (q.isEmpty())
        return hits;
    // "Change my microphone" means "microphone": filler words say what kind of
    // sentence this is, not which setting, and must not have to match anything.
    static const QSet<QString> filler{"a", "an", "the", "my", "to", "at", "of", "on", "in", "for", "how", "do", "i", "can", "change", "set", "make", "get", "turn", "fix", "use", "want", "please", "is", "it"};
    QStringList tokens;
    for (const auto &word : q.split(' ', Qt::SkipEmptyParts))
        if (!filler.contains(word))
            tokens << word;
    if (tokens.isEmpty())
        tokens = q.split(' ', Qt::SkipEmptyParts);
    const QString meaning = tokens.join(' ');

    for (const auto &entry : m_entries) {
        // Translated text participates, so a search works in the app's language.
        const QString translated = QCoreApplication::translate("catalog", entry.label.toUtf8().constData());
        const QString label = normalize(entry.label);
        const QString local = normalize(translated);
        const QString description = normalize(entry.description);
        const QString section = normalize(entry.section);
        QString keywords;
        for (const auto &keyword : entry.keywords)
            keywords += ' ' + normalize(keyword);

        int score = 0;
        if (label == meaning || local == meaning)
            score += 1000;
        else if (label.startsWith(meaning) || local.startsWith(meaning))
            score += 700;
        else if (label.contains(meaning) || local.contains(meaning))
            score += 500;
        if (keywords.contains(q) || keywords.contains(meaning))
            score += 650;
        if (description.contains(meaning))
            score += 200;

        int matched = 0;
        for (const auto &token : tokens) {
            int best = 0;
            auto test = [&](const QString &t) {
                if (label.contains(t) || local.contains(t))
                    best = qMax(best, 120);
                if (keywords.contains(t))
                    best = qMax(best, 100);
                if (section.contains(t))
                    best = qMax(best, 40);
                if (description.contains(t))
                    best = qMax(best, 30);
            };
            test(token);
            const auto synonym = synonyms().constFind(token);
            if (synonym != synonyms().constEnd()) {
                const int before = best;
                test(synonym.value());
                if (best > before)
                    best = qMax(before, best - 15);
            }
            if (best > 0)
                ++matched;
            score += best;
        }
        // Every word has to land somewhere, or short words match half the catalog.
        if (matched < tokens.size())
            score = label.contains(meaning) || keywords.contains(q) ? score : 0;
        if (score <= 0)
            continue;
        // A setting answers "change X" better than a helper that merely mentions it.
        if (entry.isSetting())
            score += 30;
        if (entry.integration == QLatin1String("advanced-config"))
            score -= 20;
        hits << SearchHit{&entry, score};
    }
    std::stable_sort(hits.begin(), hits.end(), [](const SearchHit &a, const SearchHit &b) { return a.score > b.score; });
    if (hits.size() > limit)
        hits.resize(limit);
    return hits;
}
