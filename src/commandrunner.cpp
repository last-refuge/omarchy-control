#include "commandrunner.h"

#include <unistd.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QTimer>

QString CommandResult::describe(const QString &program) const {
    // Whole sentences for an unnamed command: "%1" filled with "The command" would
    // force one grammatical gender and case on every language.
    const bool named = !program.isEmpty();
    switch (failure) {
    case NotFound:
        return named ? QCoreApplication::translate("CommandRunner", "%1 is not installed.").arg(program)
                     : QCoreApplication::translate("CommandRunner", "The command is not installed.");
    case FailedToStart:
        return named ? QCoreApplication::translate("CommandRunner", "%1 could not be started.").arg(program)
                     : QCoreApplication::translate("CommandRunner", "The command could not be started.");
    case Timeout:
        return named ? QCoreApplication::translate("CommandRunner", "%1 did not finish in time.").arg(program)
                     : QCoreApplication::translate("CommandRunner", "The command did not finish in time.");
    case Crashed:
        return named ? QCoreApplication::translate("CommandRunner", "%1 stopped unexpectedly.").arg(program)
                     : QCoreApplication::translate("CommandRunner", "The command stopped unexpectedly.");
    case Cancelled:
        return named ? QCoreApplication::translate("CommandRunner", "%1 was cancelled.").arg(program)
                     : QCoreApplication::translate("CommandRunner", "The command was cancelled.");
    case None:
        break;
    }
    if (exitCode == 0)
        return {};
    const QString detail = QString::fromUtf8(err).trimmed();
    if (!detail.isEmpty())
        return detail.left(400);
    return named ? QCoreApplication::translate("CommandRunner", "%1 failed with exit code %2.").arg(program).arg(exitCode)
                 : QCoreApplication::translate("CommandRunner", "The command failed with exit code %1.").arg(exitCode);
}

CommandRunner::CommandRunner(const Env &env, QObject *parent) : QObject(parent), m_env(env) {}

CommandRunner::~CommandRunner() {
    for (Active *active : std::as_const(m_active)) {
        active->pending.callback = nullptr;
        if (active->process) {
            active->process->disconnect();
            if (!active->pending.options.mutation) {
                active->process->kill();
                active->process->waitForFinished(500);
            } else {
                // A change in flight is allowed to finish; killing it could leave
                // an Omarchy command partway through its own multi-step write.
                active->process->waitForFinished(active->pending.options.timeoutMs);
            }
        }
        delete active;
    }
}

void CommandRunner::run(const QStringList &argv, const CommandOptions &options, QObject *context, Callback callback) {
    Pending pending{argv, options, context, context != nullptr, std::move(callback)};
    if (options.mutation) {
        start(std::move(pending));
        return;
    }
    m_queue.append(std::move(pending));
    startNext();
}

void CommandRunner::startNext() {
    int reads = 0;
    for (const Active *active : std::as_const(m_active))
        if (!active->pending.options.mutation)
            ++reads;
    while (reads < MaxReads && !m_queue.isEmpty()) {
        start(m_queue.takeFirst());
        ++reads;
    }
}

void CommandRunner::start(Pending pending) {
    auto *active = new Active;
    active->pending = std::move(pending);
    m_active.append(active);

    QPointer<QObject> context(active->pending.context);
    const QStringList &argv = active->pending.argv;
    const QString executable = argv.isEmpty() ? QString() : m_env.findExecutable(argv.first());
    if (executable.isEmpty()) {
        active->result.failure = CommandResult::NotFound;
        QTimer::singleShot(0, this, [this, active] { finish(active); });
        return;
    }

    auto *process = new QProcess(this);
    active->process = process;
    process->setProcessEnvironment(m_env.process);
    process->setProgram(executable);
    process->setArguments(argv.mid(1));
    process->setWorkingDirectory(m_env.home);

    auto *timer = new QTimer(process);
    active->timer = timer;
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, process, [active] {
        active->result.failure = CommandResult::Timeout;
        active->process->kill();
    });

    const qint64 limit = active->pending.options.maxOutputBytes;
    connect(process, &QProcess::readyReadStandardOutput, process, [active, limit] {
        const QByteArray chunk = active->process->readAllStandardOutput();
        const qint64 room = limit - active->result.out.size();
        if (room > 0)
            active->result.out.append(chunk.left(room));
        if (chunk.size() > room)
            active->result.truncated = true;
    });
    connect(process, &QProcess::readyReadStandardError, process, [active, limit] {
        const QByteArray chunk = active->process->readAllStandardError();
        const qint64 room = limit - active->result.err.size();
        if (room > 0)
            active->result.err.append(chunk.left(room));
    });
    connect(process, &QProcess::errorOccurred, process, [this, active](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            active->result.failure = CommandResult::FailedToStart;
            finish(active);
        }
    });
    connect(process, &QProcess::finished, process, [this, active](int code, QProcess::ExitStatus status) {
        active->result.out.append(active->process->readAllStandardOutput().left(
            qMax<qint64>(0, active->pending.options.maxOutputBytes - active->result.out.size())));
        active->result.err.append(active->process->readAllStandardError().left(
            qMax<qint64>(0, active->pending.options.maxOutputBytes - active->result.err.size())));
        active->result.exitCode = code;
        if (status != QProcess::NormalExit && active->result.failure == CommandResult::None)
            active->result.failure = active->killedForCancel ? CommandResult::Cancelled : CommandResult::Crashed;
        finish(active);
    });

    // A failed start can finish and delete `active` inside start(); `process` stays
    // valid because it is only ever deleted later, so nothing below may use `active`.
    const QByteArray input = active->pending.options.input;
    timer->start(active->pending.options.timeoutMs);
    process->start();
    if (process->state() == QProcess::NotRunning)
        return;
    if (!input.isEmpty())
        process->write(input);
    process->closeWriteChannel();
}

void CommandRunner::finish(Active *active) {
    if (active->finished)
        return;
    active->finished = true;
    m_active.removeOne(active);

    const CommandResult result = active->result;
    Callback callback = std::move(active->pending.callback);
    const bool hasContext = active->pending.hasContext;
    QPointer<QObject> context(active->pending.context);
    if (active->process) {
        active->timer->stop();
        active->process->deleteLater();
    }
    delete active;

    // A page closed while its read was running has no use for the answer.
    if (callback && (!hasContext || context))
        callback(result);
    startNext();
}

void CommandRunner::cancelReads() {
    m_queue.clear();
    for (Active *active : std::as_const(m_active)) {
        if (!active->pending.options.mutation && active->process) {
            active->killedForCancel = true;
            active->process->kill();
        }
    }
}

CommandResult CommandRunner::runBlocking(const QStringList &argv, const CommandOptions &options) {
    CommandResult result;
    QEventLoop loop;
    CommandOptions blocking = options;
    // Blocking callers wait for the answer, so nothing may queue it behind reads
    // whose completion is also waiting on this loop.
    blocking.mutation = true;
    run(argv, blocking, nullptr, [&](const CommandResult &r) {
        result = r;
        loop.quit();
    });
    loop.exec();
    return result;
}

int CommandRunner::runAttached(const QStringList &argv, QString *error, bool outputToStderr) {
    const QString executable = argv.isEmpty() ? QString() : m_env.findExecutable(argv.first());
    if (executable.isEmpty()) {
        if (error)
            *error = tr("%1 is not installed.").arg(argv.value(0));
        return -1;
    }
    QProcess process;
    process.setProgram(executable);
    process.setArguments(argv.mid(1));
    process.setProcessEnvironment(m_env.process);
    process.setWorkingDirectory(m_env.home);
    process.setProcessChannelMode(QProcess::ForwardedChannels);
    process.setInputChannelMode(QProcess::ForwardedInputChannel);
    // Runs in the child once its descriptors are set up, before the command starts.
    if (outputToStderr)
        process.setChildProcessModifier([] { ::dup2(STDERR_FILENO, STDOUT_FILENO); });
    process.start();
    if (!process.waitForStarted()) {
        if (error)
            *error = tr("%1 could not be started.").arg(argv.first());
        return -1;
    }
    process.waitForFinished(-1);
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : 1;
}

bool CommandRunner::launchDetached(const QStringList &argv, QString *error) {
    if (argv.isEmpty())
        return false;
    const QString executable = m_env.findExecutable(argv.first());
    if (executable.isEmpty()) {
        if (error)
            *error = tr("%1 is not installed.").arg(argv.first());
        return false;
    }
    QProcess process;
    process.setProgram(executable);
    process.setArguments(argv.mid(1));
    process.setProcessEnvironment(m_env.process);
    process.setWorkingDirectory(m_env.home);
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    qint64 pid = 0;
    if (!process.startDetached(&pid)) {
        if (error)
            *error = tr("%1 could not be started.").arg(argv.first());
        return false;
    }
    return true;
}
