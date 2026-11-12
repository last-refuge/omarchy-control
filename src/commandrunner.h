#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>
#include <functional>

#include "env.h"

class QProcess;
class QTimer;

struct CommandResult {
    enum Failure { None, NotFound, FailedToStart, Timeout, Crashed, Cancelled };
    Failure failure = None;
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
    bool truncated = false;

    bool ok() const { return failure == None && exitCode == 0; }
    QString outText() const { return QString::fromUtf8(out).trimmed(); }
    QString describe(const QString &program = {}) const;
};

struct CommandOptions {
    int timeoutMs = 8000;
    qint64 maxOutputBytes = 4 * 1024 * 1024;
    QByteArray input;
    // Reads may be coalesced and throttled; a change is never dropped or reordered.
    bool mutation = false;
};

// The only way the application starts a process. Programs get an argument
// vector, never a shell string, and every run is bounded by a timeout and an
// output limit so a stuck tool cannot stall or balloon the window.
class CommandRunner : public QObject {
    Q_OBJECT

public:
    using Callback = std::function<void(const CommandResult &)>;

    explicit CommandRunner(const Env &env, QObject *parent = nullptr);
    ~CommandRunner() override;

    const Env &env() const { return m_env; }

    void run(const QStringList &argv, const CommandOptions &options, QObject *context, Callback callback);
    void run(const QStringList &argv, QObject *context, Callback callback) { run(argv, {}, context, std::move(callback)); }

    // Runs an event loop until the command ends. Only the headless CLI and tests
    // may use this; the GUI stays asynchronous.
    CommandResult runBlocking(const QStringList &argv, const CommandOptions &options = {});

    // Starts a helper that outlives this process. Launching says nothing about
    // whether the helper then changed anything.
    bool launchDetached(const QStringList &argv, QString *error = nullptr);

    // Runs in this terminal and waits with no time limit, for a prompt only a person
    // can answer, such as sudo's. Only the headless CLI may use this. Returns the exit
    // code, or -1 with *error set if it never started.
    // `outputToStderr` sends the command's own output to this process's stderr, so a
    // caller printing JSON on stdout keeps it clean; on a terminal it looks the same.
    int runAttached(const QStringList &argv, QString *error = nullptr, bool outputToStderr = false);

    void cancelReads();
    int activeCount() const { return m_active.size(); }

private:
    struct Pending {
        QStringList argv;
        CommandOptions options;
        QObject *context = nullptr;
        bool hasContext = false;
        Callback callback;
    };
    struct Active {
        QProcess *process = nullptr;
        QTimer *timer = nullptr;
        Pending pending;
        CommandResult result;
        bool finished = false;
        bool killedForCancel = false;
    };

    void startNext();
    void start(Pending pending);
    void finish(Active *active);

    Env m_env;
    QList<Pending> m_queue;
    QList<Active *> m_active;
    static constexpr int MaxReads = 8;
};
