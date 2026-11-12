// Usage: screens <sandbox-root|--real> <output-dir> <page[:focus[:WxH[:steps]]]>...
//
// Steps, joined by "+", set up a state before the picture: select=OUT, stage=OUT/field/value,
// focus=NAME or NAME@N (the Nth visible item of that name), click=NAME or NAME@N, text=NAME/VALUE,
// preview, check, discard, refresh, unplug=OUT, details=ID, scroll=PX, rescan, connect=UUID/NAME, apply=ID/VALUE,
// keep, goback, wait=MS (before the picture), pause=MS (between steps). They act only on the sandbox's fake
// displays: unplug drops a display from the fake Hyprland's list and refreshes.
//
// --real reads the live machine, which only ever reads: it opens pages and takes
// pictures, and clicks nothing. Those images show personal data, so they stay local.
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <functional>
#include <QRegularExpression>
#include <QTimer>

#include "backend.h"
#include "displayprovider.h"
#include "env.h"
#include "gui.h"

// Every item under `item`, depth first.
static void visit(QQuickItem *item, const std::function<void(QQuickItem *)> &each) {
    each(item);
    for (QQuickItem *child : item->childItems())
        visit(child, each);
}

static void runSteps(QQuickWindow *window, Backend &backend, const QString &home, const QString &steps, int *wait) {
    DisplayProvider *displays = backend.displays();
    for (const auto &step : steps.split('+', Qt::SkipEmptyParts)) {
        const QString name = step.section('=', 0, 0), value = step.section('=', 1);
        if (name == QLatin1String("select"))
            displays->select(value);
        else if (name == QLatin1String("stage"))
            displays->stageEdit(value.section('/', 0, 0), value.section('/', 1, 1), value.section('/', 2));
        else if (name == QLatin1String("preview"))
            displays->previewStaged();
        else if (name == QLatin1String("check"))
            displays->runRecoveryCheck();
        else if (name == QLatin1String("discard"))
            displays->discardStaged();
        else if (name == QLatin1String("refresh"))
            displays->refresh();
        else if (name == QLatin1String("wait"))
            *wait = value.toInt();
        else if (name == QLatin1String("pause")) {
            // Lets work started by an earlier step finish before the next step.
            QEventLoop loop;
            QTimer::singleShot(value.toInt(), &loop, &QEventLoop::quit);
            loop.exec();
        }
        else if (name == QLatin1String("rescan"))
            backend.network()->rescan();
        else if (name == QLatin1String("connect"))
            backend.network()->activate(value.section('/', 0, 0), value.section('/', 1));
        else if (name == QLatin1String("keep"))
            backend.network()->keepConnection();
        else if (name == QLatin1String("goback"))
            backend.network()->revertConnection();
        else if (name == QLatin1String("details"))
            visit(window->contentItem(), [&](QQuickItem *item) {
                if (item->objectName() == QLatin1String("settingRow") && item->property("entryId").toString() == value)
                    item->setProperty("showDetails", true);
            });
        // A change as if confirmed, for states only a change can reach: apply=ID/VALUE.
        else if (name == QLatin1String("apply"))
            backend.commit(value.section('/', 0, 0), value.section('/', 1));
        else if (name == QLatin1String("click")) {
            // Every visible item of that name, or only the Nth with name@N.
            const QString wanted = value.section('@', 0, 0);
            const bool one = value.contains('@');
            int skip = one ? value.section('@', 1).toInt() : 0;
            visit(window->contentItem(), [&](QQuickItem *item) {
                if (item->objectName() == wanted && item->isVisible() && (!one || skip-- == 0))
                    QMetaObject::invokeMethod(item, "click");
            });
        }
        else if (name == QLatin1String("focus")) {
            // The first visible item of that name, as Tab would reach it, or the Nth with name@N.
            // Only an active window holds keyboard focus, offscreen or not.
            window->requestActivate();
            const QString wanted = value.section('@', 0, 0);
            int skip = value.contains('@') ? value.section('@', 1).toInt() : 0;
            bool done = false;
            visit(window->contentItem(), [&](QQuickItem *item) {
                if (!done && item->objectName() == wanted && item->isVisible() && skip-- <= 0) {
                    item->forceActiveFocus(Qt::TabFocusReason);
                    done = true;
                }
            });
        } else if (name == QLatin1String("text"))
            visit(window->contentItem(), [&](QQuickItem *item) {
                if (item->objectName() == value.section('/', 0, 0) && item->isVisible())
                    item->setProperty("text", value.section('/', 1));
            });
        else if (name == QLatin1String("scroll"))
            visit(window->contentItem(), [&](QQuickItem *item) {
                if (item->objectName() == QLatin1String("pageLoader"))
                    if (auto *page = qvariant_cast<QQuickItem *>(item->property("item")))
                        if (auto *flick = qvariant_cast<QObject *>(page->property("contentItem")))
                            flick->setProperty("contentY", value.toInt());
            });
        else if (name == QLatin1String("unplug")) {
            QFile file(home + "/.sandbox-monitors.json");
            if (!file.open(QIODevice::ReadOnly))
                continue;
            QJsonArray kept;
            for (const auto &monitor : QJsonDocument::fromJson(file.readAll()).array())
                if (monitor.toObject().value("name").toString() != value)
                    kept << monitor;
            file.close();
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                file.write(QJsonDocument(kept).toJson(QJsonDocument::Compact));
            file.close();
            displays->refresh();
        }
    }
}

int main(int argc, char *argv[]) {
    // A preview launches this binary as its watchdog, inside the same sandbox.
    if (argc >= 3 && QByteArray(argv[1]) == "display-watchdog") {
        QCoreApplication core(argc, argv);
        const QString root = QFileInfo(QString::fromLocal8Bit(qgetenv("HOME"))).path();
        return DisplayProvider::runWatchdog(Env::sandbox(root, root + "/bin"), QString::fromLocal8Bit(argv[2]));
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_FORCE_STDERR_LOGGING", "1");
    QGuiApplication app(argc, argv);
    app.setApplicationName("omarchy-control");
    app.setApplicationVersion(APP_VERSION);
    const QStringList args = app.arguments();
    const QString root = args.value(1), output = args.value(2);
    const QStringList pages = args.mid(3);
    QDir().mkpath(output);
    Env env = root == QLatin1String("--real") ? Env::system() : Env::sandbox(root, root + "/bin");
    // A sandbox can ask for the compatibility checks a real session makes, to show their warning.
    if (qEnvironmentVariableIsSet("SCREENS_COMPAT"))
        env.enforceCompat = true;
    QElapsedTimer started;
    started.start();

    return runGui(env, {}, [&](QQmlApplicationEngine &engine, Backend &backend) {
        qInfo("window ready in %lld ms", started.elapsed());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        auto *index = new int(0);
        auto *timer = new QTimer(window);
        timer->setInterval(900);
        auto *wait = new int(0);
        QObject::connect(timer, &QTimer::timeout, window, [=, &backend] {
            static bool waiting = false;
            if (waiting && *wait > 0) {
                *wait -= timer->interval();
                return;
            }
            if (waiting) {
                QString name = pages[*index].section(':', 0, 0) + (pages[*index].contains(':') ? "-" + pages[*index].section(':', 1) : "");
                name.replace(QRegularExpression("[^A-Za-z0-9-]+"), "_");
                window->grabWindow().save(output + "/" + name + ".png");
                ++*index;
                waiting = false;
                if (*index >= pages.size()) {
                    QCoreApplication::quit();
                    return;
                }
            }
            const QString spec = pages[*index];
            const QString page = spec.section(':', 0, 0), focus = spec.section(':', 1, 1), size = spec.section(':', 2, 2);
            if (!size.isEmpty()) {
                window->setWidth(size.section('x', 0, 0).toInt());
                window->setHeight(size.section('x', 1, 1).toInt());
            }
            QMetaObject::invokeMethod(window, "go", Q_ARG(QVariant, page), Q_ARG(QVariant, focus));
            *wait = 0;
            runSteps(window, backend, env.home, spec.section(':', 3), wait);
            waiting = true;
        });
        timer->start();
    });
}
