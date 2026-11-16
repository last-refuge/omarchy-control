#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "env.h"

// The Lua files this application owns in ~/.config/hypr. Each holds only values
// someone explicitly chose here and is regenerated whole from the data on its first
// line. An edit by anything else is detected rather than overwritten, and the app
// never executes the user's Lua to learn what it does.
//
// Main, omarchy-control.lua, loads after the user's override files and before
// Omarchy's toggles, so a toggle chosen later wins. Blur, omarchy-control-blur.lua,
// loads after the toggles: Omarchy's Glassy toggles set every blur value as they
// load, so a blur choice made here has to come after them to count.
class HyprModule {
public:
    enum Slot { Main, Blur };
    explicit HyprModule(const Env &env, Slot slot = Main);

    Slot slot() const { return m_slot; }
    QString path() const;
    QString fileName() const;
    QString hyprlandPath() const;

    enum State { Absent, Ours, ModifiedByUser };
    State state() const;

    QJsonObject options() const { return m_data.value("options").toObject(); }
    QJsonArray monitors() const { return m_data.value("monitors").toArray(); }
    QJsonArray binds() const { return m_data.value("binds").toArray(); }
    QJsonArray rules() const { return m_data.value("rules").toArray(); }

    bool load(QString *error = nullptr);
    // Read-only, bounded ownership inspection. A failed read is never an empty
    // collection, and the Lua is compared as text, never executed.
    QJsonObject inspect() const;
    // Nothing chosen here at all, so the file and its line have no reason to exist.
    bool isEmpty() const;
    // Takes the line out of hyprland.lua, then the file, in that order: Lua's require
    // fails outright on a missing file, and Hyprland reloads when hyprland.lua changes.
    bool removeEntirely(QString *error = nullptr);
    void setOption(const QStringList &path, const QJsonValue &value);
    void removeOption(const QStringList &path);
    bool hasOption(const QStringList &path) const;
    QJsonValue option(const QStringList &path) const;
    void setMonitors(const QJsonArray &monitors) { m_data.insert("monitors", monitors); }
    void setBinds(const QJsonArray &binds) { m_data.insert("binds", binds); }
    void setRules(const QJsonArray &rules) { m_data.insert("rules", rules); }

    // Rendered text for the current data; pure, and tested without a compositor.
    QByteArray render() const;
    bool save(QString *error = nullptr);

    // Whether the marked import line is in place, and adding or removing it. The
    // anchor is Omarchy's stock require("hypr.autostart") line for Main, and its
    // require("default.hypr.toggles") line for Blur; a config without one stays
    // untouched and the setting offers advanced access instead.
    bool importInstalled() const;
    bool canInstallImport() const;
    bool plannedImport(QByteArray *contents, QString *error = nullptr) const;
    bool installImport(QString *error = nullptr);
    bool removeImport(QString *error = nullptr);

    static QString luaString(const QString &text);
    static bool isSafeKey(const QString &key);

private:
    QByteArray requireText() const;
    QByteArray anchorText() const;
    QByteArray importLine() const;

    Env m_env;
    Slot m_slot;
    QJsonObject m_data;
};
