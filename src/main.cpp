// omarchy-control: one searchable place to understand this machine, change its
// settings, and reach the right specialist tool.

#include <QCoreApplication>
#include <QGuiApplication>

#include "cli.h"
#include "cliapi.h"
#include "env.h"
#include "gui.h"
#include "version.h"

int main(int argc, char *argv[]) {
    const QString command = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    QStringList commandArguments;
    for (int i = 1; i < argc; ++i)
        commandArguments << QString::fromLocal8Bit(argv[i]);

    // A versioned call must produce a CLI refusal for unsupported operations,
    // including open, rather than unexpectedly launching a window.
    if (command == QLatin1String("open") && !CliApi::requested(commandArguments)) {
        QGuiApplication app(argc, argv);
        app.setApplicationName("omarchy-control");
        app.setApplicationVersion(APP_VERSION);
        QStringList arguments = app.arguments().mid(2);
        return runGui(Env::system(), arguments);
    }

    // Everything else runs without a display, so agents and scripts can use it.
    QCoreApplication app(argc, argv);
    app.setApplicationName("omarchy-control");
    app.setApplicationVersion(APP_VERSION);
    return Cli::run(Env::system(), app.arguments().mid(1));
}
