#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include "env.h"

// Omarchy's launcher, dock and app grid do not look app icons up in the chosen pack. Its
// shell indexes every app icon of every installed pack and keeps the first file it finds
// per name, any SVG before any PNG, starting with ~/.icons. So this keeps one icon per
// launcher in ~/.icons/omarchy-control/apps, taken from the chosen pack, with PNGs wrapped
// in SVG so they come first too. The launcher then shows the pack without any change to
// Omarchy. The idea is Iconic's (github.com/UnderStackDev/omarchy_iconic, MIT).
//
// The layer is derived from the pack and the launchers, so building it is not a change of
// its own and is never in History. Nothing here runs anything or reaches the network.
namespace IconLayer {

struct Launcher {
    QString id;    // The desktop file id, as the launcher knows it.
    QString name;  // Name=, untranslated.
    QString icon;  // Icon=, as written.
    bool visible = true;
};

// Every application launcher, one per desktop id, the user's own first, as XDG orders them.
QList<Launcher> launchers(const Env &env);

QString folder(const Env &env);        // ~/.icons/omarchy-control
QString settingsPath(const Env &env);  // Whether the layer is wanted at all.
QString statePath(const Env &env);     // The last build's error, if it failed.

// On unless turned off: the switch in Appearance writes this.
bool enabled(const Env &env);

// Why Omarchy's launcher would not pick the layer up, or empty when it would. The layer
// relies on how the shell indexes icons, so a shell that does it differently pauses it.
QString incompatibility(const Env &env);

// A fingerprint of everything a build reads, to tell whether one is out of date.
QString stamp(const Env &env, const QString &pack);

struct Icon {
    QString name;    // Icon= as written, which is the file's name in the layer.
    QString kind;    // "pack", "own" (hicolor or pixmaps), or "missing".
    QString from;    // The pack it came from.
    QString source;  // The file it was made from.
    QStringList apps;
    bool visible = false;  // Some launcher that uses it is shown.
};

struct Build {
    bool ok = false;
    QString error;
    QList<Icon> icons;
};

// Every app and device icon `pack` can show, from itself, what it inherits, and hicolor:
// icon name to file, the nearest size first. For choosing an icon by sight.
QHash<QString, QString> appIcons(const Env &env, const QString &pack, QStringList *diagnostics = nullptr,
                               QHash<QString, QString> *origins = nullptr);

// Builds a fresh layer for `pack` beside the live one and swaps it in, so the shell never
// sees a half-built one. Not thread-safe against itself: the caller holds the layer lock.
Build build(const Env &env, const QString &pack);
// Removes the layer, and only if it is this app's.
bool remove(const Env &env, QString *error);
bool exists(const Env &env);

// What the live layer was built from and what it covers, read from its manifest.
QVariantMap status(const Env &env);

// Iconic (github.com/UnderStackDev/omarchy_iconic) keeps a layer and a theme hook of its
// own, and its hook runs after this app's, so the two would fight over the same setting.
QStringList iconicPaths(const Env &env);
// Moves Iconic's layer, hook and settings into this app's state folder, where restoring
// them puts them back exactly. Nothing is deleted.
bool retireIconic(const Env &env, QString *error);
bool restoreIconic(const Env &env, QString *error);
bool iconicRetired(const Env &env);

}  // namespace IconLayer
