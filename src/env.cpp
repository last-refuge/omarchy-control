#include "env.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <unistd.h>

static QString envOr(const QProcessEnvironment &process, const char *key, const QString &fallback) {
    const QString value = process.value(QString::fromLatin1(key));
    return value.isEmpty() ? fallback : value;
}

// The base directory spec ignores relative paths, because they would mean something
// different in every working directory.
static QStringList absolutePaths(const QString &list) {
    QStringList paths;
    for (const QString &path : list.split(':', Qt::SkipEmptyParts))
        if (QDir::isAbsolutePath(path) && !paths.contains(QDir::cleanPath(path)))
            paths << QDir::cleanPath(path);
    return paths;
}

Env Env::system() {
    Env env;
    env.process = QProcessEnvironment::systemEnvironment();
    env.home = envOr(env.process, "HOME", QDir::homePath());
    env.configHome = env.home + "/.config";
    env.stateHome = env.home + "/.local/state";
    const QString xdgConfig = envOr(env.process, "XDG_CONFIG_HOME", env.configHome);
    const QString xdgState = envOr(env.process, "XDG_STATE_HOME", env.stateHome);
    const QString xdgCache = envOr(env.process, "XDG_CACHE_HOME", env.home + "/.cache");
    env.appConfigDir = xdgConfig + "/omarchy-control";
    env.appStateDir = xdgState + "/omarchy-control";
    env.appCacheDir = xdgCache + "/omarchy-control";
    env.runtimeDir = envOr(env.process, "XDG_RUNTIME_DIR", QString("/run/user/%1").arg(getuid()));
    env.omarchyPath = envOr(env.process, "OMARCHY_PATH", "/usr/share/omarchy");
    env.process.insert("OMARCHY_PATH", env.omarchyPath);
    env.sysRoot = "/";
    const QString dataHome = env.process.value("XDG_DATA_HOME");
    env.dataHome = QDir::isAbsolutePath(dataHome) ? QDir::cleanPath(dataHome) : env.home + "/.local/share";
    env.dataDirs = absolutePaths(env.process.value("XDG_DATA_DIRS"));
    if (env.dataDirs.isEmpty())
        env.dataDirs = QStringList{"/usr/local/share", "/usr/share"};
    env.selfBinary = QCoreApplication::applicationFilePath();
    env.enforceCompat = true;
    return env;
}

Env Env::sandbox(const QString &root, const QString &binDirectory) {
    Env env;
    env.home = root + "/home";
    env.configHome = env.home + "/.config";
    env.stateHome = env.home + "/.local/state";
    env.appConfigDir = env.configHome + "/omarchy-control";
    env.appStateDir = env.stateHome + "/omarchy-control";
    env.appCacheDir = env.home + "/.cache/omarchy-control";
    env.runtimeDir = root + "/run";
    env.omarchyPath = root + "/omarchy";
    env.sysRoot = root + "/sys-root";
    env.dataHome = env.home + "/.local/share";
    env.dataDirs = QStringList{env.sysRoot + "/usr/local/share", env.sysRoot + "/usr/share"};
    env.selfBinary = QCoreApplication::applicationFilePath();
    for (const QString &dir : {env.configHome, env.stateHome, env.runtimeDir, env.omarchyPath, env.sysRoot, binDirectory})
        QDir().mkpath(dir);

    // A deliberately small environment: nothing inherited can point a command
    // at the real session, and PATH resolves only the fixture commands first.
    env.process.insert("HOME", env.home);
    env.process.insert("XDG_RUNTIME_DIR", env.runtimeDir);
    env.process.insert("XDG_CONFIG_HOME", env.configHome);
    env.process.insert("XDG_STATE_HOME", env.stateHome);
    env.process.insert("XDG_DATA_HOME", env.dataHome);
    env.process.insert("XDG_DATA_DIRS", env.dataDirs.join(':'));
    env.process.insert("OMARCHY_PATH", env.omarchyPath);
    // Only plain file utilities come from the system, never anything that talks to
    // the session, so a test without a fake cannot reach the real desktop by accident.
    const QString safeBin = root + "/safe-bin";
    QDir().mkpath(safeBin);
    for (const char *tool : {"bash", "sh", "env", "cat", "sed", "awk", "grep", "head", "tail", "tr", "cut", "sort", "wc", "printf", "echo", "mkdir", "rm", "touch", "cp", "mv", "ln", "ls", "basename", "dirname", "sleep", "true", "false", "test", "setsid", "date"}) {
        const QString real = QStandardPaths::findExecutable(QString::fromLatin1(tool));
        if (!real.isEmpty())
            QFile::link(real, safeBin + "/" + QString::fromLatin1(tool));
    }
    env.process.insert("PATH", binDirectory + ":" + safeBin);
    env.process.insert("HYPRLAND_INSTANCE_SIGNATURE", "sandbox");
    env.gitHub = "file://" + root + "/github";
    env.process.insert("LC_ALL", "C");
    return env;
}

QString Env::expand(const QString &path) const {
    QString result = path;
    if (result == "~")
        return home;
    if (result.startsWith("~/"))
        result = home + result.mid(1);
    result.replace("{config}", configHome);
    result.replace("{state}", stateHome);
    result.replace("{omarchy}", omarchyPath);
    result.replace("{runtime}", runtimeDir);
    return result;
}

QString Env::findExecutable(const QString &name) const {
    if (name.contains('/'))
        return QFileInfo(name).isExecutable() ? name : QString();
    return QStandardPaths::findExecutable(name, process.value("PATH").split(':', Qt::SkipEmptyParts));
}

QString Env::sys(const QString &path) const {
    return sysRoot == "/" ? path : sysRoot + path;
}

bool Env::hasHyprlandSignature() const {
    return !process.value("HYPRLAND_INSTANCE_SIGNATURE").isEmpty();
}

QStringList Env::hyprctl(const QStringList &arguments) const {
    QStringList argv{"hyprctl"};
    if (!hasHyprlandSignature())
        argv << "--instance" << "0";
    return argv + arguments;
}
