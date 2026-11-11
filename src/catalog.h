#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

// One searchable destination: a typed setting, a helper launch, or a config file.
// Entries are compiled into the binary and reviewed. This is deliberately not a
// plugin system: nothing outside the binary can add a command to run.
struct CatalogEntry {
    QString id, page, section, label, description, type, domain, integration;
    QStringList keywords, sources, crossPages, effects, requiresCommands, requiresCaps, requiresHardware;
    QJsonObject read, write, launch, choicesFrom, confirmWhen, valueMap;
    QJsonArray choices, reset;
    double min = 0, max = 0, step = 1;
    bool hasRange = false, allowCustomValue = false, owned = false, softRequires = false, quick = false;
    QString unit, pattern, scope, recovery, restart, reloads, confirm, confirmText, confirmAlways, note, placeholder, installHint;
    QStringList files;
    // A setting that belongs to one device of several, such as one display's brightness.
    // It is addressed as "<id>@<device>", and "{targetArgs}" in its argv becomes
    // `targetArgs` with "{target}" filled in, or nothing when no device is named.
    QString target;
    QStringList targetArgs;
    // Wording for when there is more than one display, where "{target}" is the selected one.
    QString iconName, multiLabel, multiDescription;
    // A named set within a section, such as one dictation engine's settings and actions.
    QString group;
    // How an action reads when drawn as a tile: "danger" for one that ends the session.
    QString tone;
    // Where a status row's link leads: {label, page, focus}.
    QJsonObject link;
    // How each status value reads at a glance: "good", "neutral", "danger", or "muted".
    QJsonObject tones;
    // Rows of the same section kept behind this one until opened, as a strength keeps
    // its finer parts. Search and the CLI still reach them directly.
    QStringList discloses;
    // Whether a saved profile may carry this setting to another computer, and how it is
    // restored there. Reviewed per setting: being editable does not make a value portable.
    QJsonObject profile;

    bool isAction() const { return type == QLatin1String("action"); }
    bool isSetting() const { return !isAction(); }
    bool isEditable() const { return isSetting() && type != QLatin1String("status") && !write.isEmpty(); }
    // What the row's button says: what pressing it does, never how it is launched.
    QString verb() const;
    QVariantMap toVariant() const;
};

struct PageInfo {
    QString id, label, icon, description;
    QStringList sections, blocks;
    // An optional composition of the page into panels; see Catalog::panelLayout.
    QJsonArray panels;
    // The page's jump links, in order, when they are not simply its sections.
    QStringList jumps;
};

struct SearchHit {
    const CatalogEntry *entry = nullptr;
    int score = 0;
};

class Catalog {
public:
    // Loads the compiled-in data; `directory` lets tests supply their own.
    explicit Catalog(const QString &directory = QStringLiteral(":/data"));

    const QList<CatalogEntry> &entries() const { return m_entries; }
    const QList<PageInfo> &pages() const { return m_pages; }
    // Also finds "<id>@<device>" for an entry that has a target, if the device name is safe.
    const CatalogEntry *find(const QString &id) const;
    // The device in "<id>@<device>", or empty when there is none or it is not a plain name.
    static QString targetOf(const QString &id);
    const PageInfo *page(const QString &id) const;
    QList<const CatalogEntry *> entriesForPage(const QString &page) const;
    QJsonObject exclusions() const { return m_exclusions; }

    // The page's panels, as rows of columns of panels, each listing the ids of its rows
    // and, optionally, the one entry drawn as its header. Every entry of the page lands
    // in exactly one place: one a panel does not claim gets a panel of its own at the end,
    // so a new entry is never lost to an old layout. Empty when the page has no panels.
    QJsonArray panelLayout(const QString &page) const;

    // Ranked, local, and side-effect free: searching never runs anything. An
    // optional translator callback lets a translated label match as well.
    QList<SearchHit> search(const QString &query, int limit = 40) const;

    static QString normalize(const QString &text);

private:
    void load(const QString &path);

    QList<CatalogEntry> m_entries;
    QList<PageInfo> m_pages;
    QHash<QString, int> m_index;
    QJsonObject m_exclusions;
};
