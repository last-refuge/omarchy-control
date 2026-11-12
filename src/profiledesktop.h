#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <functional>

#include "capabilities.h"
#include "catalog.h"
#include "changecoordinator.h"
#include "commandrunner.h"
#include "compat.h"
#include "launchereditor.h"
#include "profilemodel.h"

// The desktop choices a profile carries beyond settings: themes installed from Git, the
// background, the dock's pins, changes to app launchers, file associations, the shortcuts
// and window rules this app owns, and terminal configuration files. Each is
// read here, compared as one canonical value, and restored through the same reviewed paths
// the window uses: one journaled, verified change at a time, under the restore's lease.
// Profiles supply data only. Nothing here runs anything a profile names, except that a
// restored shortcut runs its command when someone presses it, which is why the review
// shows every command first.
class ProfileDesktop : public QObject {
    Q_OBJECT

public:
    ProfileDesktop(const Catalog *catalog, CommandRunner *runner, Capabilities *capabilities, ChangeCoordinator *coordinator, Compat *compat,
                   LauncherEditor *launchers, QObject *parent = nullptr);

    // A theme as it is installed here.
    struct ThemeState {
        bool installed = false;  // In the user's themes folder, where Git themes go.
        bool builtin = false;    // One Omarchy ships.
        QString url;             // The https address the user's copy was cloned from.
        QString problem;         // Why the user's copy has no address a profile can carry.
    };

    // What this computer has now, in the profile's own shapes.
    struct Snapshot {
        bool dockOk = false;
        QString dockError;
        QStringList dockPins;
        QStringList defaultDockPins;                 // What Omarchy pins on a fresh installation.
        QHash<QString, QJsonObject> launchers;       // App id -> its name, icon, and hidden flag now.
        QHash<QString, QJsonObject> launcherEdits;   // App id -> just the fields changed here.
        QHash<QString, QString> associations;        // File type -> app id, for the types that were read.
        QHash<QString, QJsonObject> shortcuts;       // Record id -> a shortcut this app owns.
        QHash<QString, QJsonObject> rules;           // Record id -> a window rule this app owns.
        bool moduleEditedByHand = false;
        bool moduleImport = false;
        bool canInstallImport = false;
        bool bindsOk = false;
        QHash<QString, QString> boundKeys;           // Keys Hyprland binds now -> what they do.
        QHash<QString, QString> launcherIconFiles;   // App id -> its icon, when that is a file.
        QHash<QString, QString> launcherIconDigests; // App id -> that file's SHA-256.
        QString theme;                               // The current theme's folder name.
        // The background, as a profile records it: {theme, name}, plus sha256 for one of the
        // user's own. Empty when it is neither a theme's nor in the user's folder.
        QJsonObject background;
        QString backgroundPath;
        QHash<QString, QString> fileDigests;     // "file:<adapter>/<name>" -> its SHA-256 here, or nothing.
        QHash<QString, QString> defaultDigests;  // The same, for Omarchy's own version of the file.
        QHash<QString, ThemeState> themes;       // Every theme here, by folder name.
        // The bar's layout as shell.json has it, the user's or Omarchy's own when there is
        // none, and Omarchy's own for comparison.
        bool barOk = false;
        QString barError;
        QJsonObject barLayout, defaultBarLayout;
        QSet<QString> barWidgets;          // Omarchy's own bar widgets here.
        // Omarchy's own plugins that are not widgets, and which of them are switched off,
        // as the running shell reports them.
        bool pluginsOk = false;
        QString pluginsError;
        QSet<QString> switchablePlugins;
        QStringList disabledPlugins;
    };

    // What the checks and the restore need beyond this computer's state.
    struct Context {
        bool appsPending = false;  // The same restore installs apps first.
        QStringList themesPending; // Themes the same restore installs first, by folder.
        QString theme;             // The theme in effect when desktop choices are restored.
        QString directory;         // The profile's folder, which holds its files.
        Profile profile;
    };
    // A theme's folder name, as Omarchy makes it from the name shown, and back.
    static QString themeFolder(const QString &name);
    static QString themeName(const QString &folder);
    // Every theme installed here, by folder. Read from the files alone: nothing in a theme's
    // Git copy is run to learn where it came from.
    static QHash<QString, ThemeState> readThemes(const Env &env);
    static ThemeState themeState(const Env &env, const QString &folder);
    // An origin as a profile can carry it: https without any name or password in it, or an
    // SSH address on a well-known forge in its https form. Empty, with why, when it has none.
    static QString themeSource(const QString &origin, QString *problem = nullptr);
    static bool sameSource(const QString &a, const QString &b);
    // A layout as a profile compares it: only Omarchy's own widgets, and only the options a
    // profile carries. A profile's layout keeps just the sections it names, since a section
    // it leaves out is not managed; this computer's has every section.
    static QJsonObject projectBar(const QJsonObject &layout, bool everySection);
    // Whether `current` has what `desired` asks for, in the sections it names.
    static bool barMatches(const QString &desired, const QString &current);
    // What a layout write leaves: in each section the profile names, the saved widgets in
    // their order, each keeping options of its own the profile does not carry, then widgets
    // installed separately. Other sections stay as they are.
    static QJsonObject mergeBar(const QJsonObject &current, const QJsonObject &desired);
    // `mimes` are the file types to read besides the everyday ones.
    void read(const QStringList &mimes, std::function<void(const Snapshot &)> done);

    // The everyday file types the window lets people choose an app for.
    static QStringList commonMimes();
    static QString shortcutId(const QString &keys);
    static QString ruleId(const QJsonObject &rule);
    static QString appId(const QString &desktopId);

    // A desktop section's records, as targets: "theme:<folder>", "background", "bar",
    // "plugins", "dock", "launcher:<app>", "association:<type>", "shortcut:<id>", and
    // "rule:<id>".
    static QStringList targets(const QJsonObject &desktop);
    // The configuration files a profile carries, as targets "file:<adapter>/<name>".
    static QStringList fileTargets(const Profile &profile);
    // A target's saved value from the whole profile: its desktop section or its files.
    static QString desiredIn(const Profile &profile, const QString &target);
    // A target's saved value, and what this computer has for it, as canonical compact
    // JSON. Empty means there is none.
    static QString desired(const QJsonObject &desktop, const QString &target);
    static QString current(const Snapshot &snapshot, const QString &target);
    // Whether what this computer has is what was saved. A launcher change names only the
    // fields that were changed, so only those count.
    static bool matches(const QString &target, const QString &desired, const QString &current);
    static QString label(const QString &target, const QString &desired);
    // A value as a person reads it: the pins in order, the launcher's changes, the app.
    static QString summary(const QString &target, const QString &value);
    // What restoring a target does that someone should see first, such as the command a
    // shortcut runs.
    static QStringList effects(const QString &target, const QString &desired);

    // Why this target cannot be restored here, or empty when it can. `appsPending` says the
    // same restore installs apps first, so an app missing now may arrive in time.
    QString blockedReason(const Snapshot &snapshot, const QString &target, const QString &desired, const Context &context) const;
    // A reason it may be someone's own choice here, or empty when restoring overwrites
    // nothing anyone chose. Used when this computer has no agreed record of it.
    static QString localChoice(const Snapshot &snapshot, const QString &target, const QString &current);
    // For a shortcut whose keys already do something this app did not set up: what they do.
    // Taking them over is always a decision, whatever was agreed before.
    static QString collision(const Snapshot &snapshot, const QString &target, const QString &desired);
    // The ownership line a shortcut or rule needs in hyprland.lua, when it is not there yet.
    static QString ownershipText(const Snapshot &snapshot, const QString &target);

    // Restores one target to its saved value, as one journaled, verified change.
    void apply(const QString &target, const QString &desired, const Context &context, const QString &runId, ChangeCoordinator::Finished finished);
    QString userBackgroundsFolder(const QString &theme) const;

private:
    void readMimes(QStringList mimes, std::shared_ptr<Snapshot> snapshot, std::function<void()> done);
    // Runs commands one after another, stopping at the first that fails.
    void runSteps(QList<QStringList> steps, ChangeCoordinator::Done done);
    void submitModule(const QString &settingId, const QString &label, const QString &after, std::function<bool(class HyprModule &)> edit,
                      std::function<void(ChangeCoordinator::Done)> verify, const QString &runId, ChangeCoordinator::Finished finished);

    const Catalog *m_catalog;
    CommandRunner *m_runner;
    Capabilities *m_capabilities;
    ChangeCoordinator *m_coordinator;
    Compat *m_compat;
    LauncherEditor *m_launchers;
};
