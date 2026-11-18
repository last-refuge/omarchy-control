#include "gui.h"

#include <QGuiApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

#include "backend.h"
#include "iconprovider.h"
#include "iconthemes.h"
#include "omarchyui.h"
#include "theme.h"
#include "translator.h"
#include "trayhost.h"

// One window per session: a second launch hands its destination to the running
// one, which also rules out two instances racing to apply changes.
static QString socketName(const Env &env) {
    return env.runtimeDir + "/omarchy-control.sock";
}

static bool forwardToRunning(const Env &env, const QString &page, const QString &setting) {
    QLocalSocket socket;
    socket.connectToServer(socketName(env));
    if (!socket.waitForConnected(300))
        return false;
    socket.write((page + "\n" + setting + "\n").toUtf8());
    socket.waitForBytesWritten(300);
    return true;
}

static QString option(const QStringList &arguments, const QString &flag) {
    const int at = arguments.indexOf(flag);
    return at >= 0 && at + 1 < arguments.size() ? arguments[at + 1] : QString();
}

int runGui(const Env &env, const QStringList &arguments, std::function<void(QQmlApplicationEngine &, Backend &)> ready) {
    QCoreApplication *application = QCoreApplication::instance();
    auto *app = qobject_cast<QGuiApplication *>(application);
    if (!app)
        return 1;
    const QString page = option(arguments, "--page");
    const QString setting = option(arguments, "--setting");
    if (forwardToRunning(env, page, setting))
        return 0;

    // Matches the window to omarchy-control.desktop, so the compositor (Wayland
    // app_id) and the launcher pick up the installed icon.
    app->setDesktopFileName("omarchy-control");
    app->setWindowIcon(QIcon::fromTheme("omarchy-control"));
    QQuickStyle::setStyle("Basic");
    // The window decides when to quit: with a tray to wait in, closing only hides it.
    app->setQuitOnLastWindowClosed(false);

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator(env.configHome + "/omarchy/language", ":/i18n", env.omarchyPath + "/default/i18n/locales.json");
    app->installTranslator(&translator);

    ThemePreviewSource appearance(env);
    Backend backend(env);
    TrayHost trayHost;

    QLocalServer server;
    QLocalServer::removeServer(socketName(env));
    server.listen(socketName(env));
    QObject::connect(&server, &QLocalServer::newConnection, &backend, [&server, &backend] {
        QLocalSocket *socket = server.nextPendingConnection();
        QObject::connect(socket, &QLocalSocket::readyRead, socket, [socket, &backend] {
            const auto lines = QString::fromUtf8(socket->readAll()).split('\n');
            backend.requestRaise(lines.value(0), lines.value(1));
            socket->deleteLater();
        });
    });

    OmarchyUi::registerTypes();
    QQmlApplicationEngine engine;
    engine.addImageProvider("icons", new IconProvider);
    engine.addImageProvider("iconpacks", new IconPackImageProvider(env));
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("appearance", &appearance);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("trayHost", &trayHost);
    engine.rootContext()->setContextProperty("initialPage", page);
    engine.rootContext()->setContextProperty("initialSetting", setting);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    if (ready)
        ready(engine, backend);
    return app->exec();
}
