#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

// Every path and process environment the application touches comes from one
// Env, so tests and the screenshot harness can run against a temporary root and
// never reach the real desktop.
struct Env {
    QString home;
    QString configHome;
    QString stateHome;      // Omarchy's own state; its commands hardcode ~/.local/state.
    QString appStateDir;    // XDG state for omarchy-control: history, backups.
    QString appConfigDir;   // XDG config for omarchy-control: preferences.
    QString appCacheDir;
    QString runtimeDir;
    QString omarchyPath;
    QString dataHome;       // Where this user's data lives, such as their icon packs.
    QStringList dataDirs;   // The shared data roots, most important first.
    QString sysRoot;        // "/" normally; a fixture tree in tests.
    QString selfBinary;     // Path of this executable, for the display watchdog.
    QProcessEnvironment process;
    bool enforceCompat = false;  // Off in sandboxes, whose commands are all fakes.
    // Where profile repositories are published. Sandboxes point it at local bare
    // repositories, so no test ever reaches GitHub.
    QString gitHub = QStringLiteral("https://github.com");

    static Env system();
    static Env sandbox(const QString &root, const QString &binDirectory);

    // Expands a leading ~/ and the {config}, {state}, {omarchy} tokens that the
    // reviewed catalog uses. Values from anywhere else are never expanded.
    QString expand(const QString &path) const;
    QString findExecutable(const QString &name) const;
    QString sys(const QString &path) const;
    bool hasHyprlandSignature() const;

    // Hyprctl needs --instance 0 when the app runs outside the session, the same
    // fallback omarchy-restart-hyprctl uses.
    QStringList hyprctl(const QStringList &arguments) const;
};
