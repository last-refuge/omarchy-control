#include "audioprovider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

#include "iconthemes.h"
#include "compat.h"

static int volumePercent(const QJsonObject &volume) {
    int highest = 0;
    bool any = false;
    for (auto it = volume.begin(); it != volume.end(); ++it) {
        bool valid = false;
        const int percent = it.value().toObject().value("value_percent").toString().remove('%').toInt(&valid);
        if (!valid)
            return -1;
        highest = qMax(highest, percent);
        any = true;
    }
    return any ? highest : -1;
}

QVariantMap AudioProvider::nodeNames(const QString &description, const QJsonObject &properties) {
    const QString card = properties.value("device.description").toString();
    QString port = properties.value("device.profile.description").toString();
    QString title = description, subtitle;
    const QString rest = card.isEmpty() || !description.startsWith(card) ? QString() : description.mid(card.size()).trimmed();
    if (rest.isEmpty()) {
        // One device, one port, as a Bluetooth headset is: its name is the name, and the
        // port says how it is being used.
        if (!port.isEmpty() && port != description)
            subtitle = port;
    } else if (!port.isEmpty()) {
        static const QRegularExpression channels(R"(\(([^()]+)\)\s*$)");
        static const QRegularExpression direction(R"(\s+(Output|Input)$)");
        static const QRegularExpression numbered(R"(\d)");
        static const QRegularExpression family(R"(^(.*\))\s+(\S.*)$)");
        port.remove(direction);
        // "Maker Chip (Series 3) HD Audio": the words after the model say what kind of
        // device it is, which a plain port like "Speaker" needs and "HDMI 1" does not.
        QString maker = card, kind;
        const auto split = family.match(card);
        if (split.hasMatch()) {
            maker = split.captured(1);
            kind = split.captured(2);
        }
        title = !kind.isEmpty() && !numbered.match(port).hasMatch() ? kind + " " + port : port;
        subtitle = maker;
        const auto layout = channels.match(rest);
        if (layout.hasMatch() && layout.captured(1) != port)
            subtitle += QString::fromUtf8(" · ") + layout.captured(1);
    }
    return {{"title", title}, {"subtitle", subtitle}};
}

static QString nodeKind(const QJsonObject &properties, const QString &description, bool source) {
    if (source)
        return QStringLiteral("mic");
    const QString hint = properties.value("device.icon_name").toString() + " " + properties.value("device.form_factor").toString();
    if (hint.contains("headphone") || hint.contains("headset"))
        return QStringLiteral("headphones");
    if (hint.contains("video-display") || description.contains("HDMI") || description.contains("DisplayPort"))
        return QStringLiteral("displays");
    if (properties.value("device.bus").toString() == QLatin1String("bluetooth"))
        return QStringLiteral("bluetooth");
    if (description.contains("Laptop", Qt::CaseInsensitive))
        return QStringLiteral("laptop");
    return QStringLiteral("speaker");
}

void AudioProvider::matchStreams(QVariantList &streams, const QVariantList &outputs) {
    for (auto &value : streams) {
        auto stream = value.toMap();
        const int sink = stream.value("sink").toInt();
        QString output;
        // pactl names a stream's sink by the same index it lists sinks with; older
        // PipeWire made that the object id, so either one counts.
        for (const auto &row : outputs)
            if (row.toMap().value("index").toInt() == sink)
                output = row.toMap().value("name").toString();
        if (output.isEmpty())
            for (const auto &row : outputs)
                if (row.toMap().value("objectId").toString() == QString::number(sink))
                    output = row.toMap().value("name").toString();
        stream["output"] = output;
        value = stream;
    }
}

QVariantList AudioProvider::parseNodes(const QByteArray &json, const QString &defaultName, bool sources) {
    QVariantList rows;
    for (const auto &value : QJsonDocument::fromJson(json).array()) {
        const auto node = value.toObject();
        const auto properties = node.value("properties").toObject();
        const QString name = node.value("name").toString();
        // Every sink has a monitor source; none of them is a microphone.
        if (sources && (properties.value("device.class").toString() == QLatin1String("monitor") || name.endsWith(".monitor") ||
                        !node.value("monitor_of_sink").toString().isEmpty()))
            continue;
        QVariantMap row;
        row["name"] = name;
        row["description"] = node.value("description").toString(name);
        row["index"] = node.value("index").toInt(-1);
        row["objectId"] = properties.value("object.id").toString();
        const auto names = nodeNames(row["description"].toString(), properties);
        row["title"] = names.value("title");
        row["subtitle"] = names.value("subtitle");
        row["kind"] = nodeKind(properties, row["description"].toString(), sources);
        row["volume"] = volumePercent(node.value("volume").toObject());
        row["mute"] = node.value("mute").toBool();
        row["muteKnown"] = node.value("mute").isBool();
        row["state"] = node.value("state").toString();
        row["isDefault"] = name == defaultName;
        rows << row;
    }
    return rows;
}

QVariantList AudioProvider::parseStreams(const QByteArray &json) {
    QVariantList rows;
    for (const auto &value : QJsonDocument::fromJson(json).array()) {
        const auto stream = value.toObject();
        const auto properties = stream.value("properties").toObject();
        // A DSP chain's own output is a sink input with no application name;
        // moving it would rewire the processing itself.
        const QString app = properties.value("application.name").toString();
        if (app.isEmpty() || app == QLatin1String("EasyEffects"))
            continue;
        QVariantMap row;
        row["index"] = stream.value("index").toInt();
        row["app"] = app;
        row["title"] = properties.value("media.name").toString();
        row["sink"] = stream.value("sink").toInt();
        row["iconName"] = properties.value("application.icon_name").toString();
        row["binary"] = properties.value("application.process.binary").toString();
        row["identity"] = properties.value("object.serial").toVariant().toString();
        row["selector"] = row.value("identity").toString().isEmpty() ? QString()
            : QString::number(row.value("index").toInt()) + "@" + row.value("identity").toString();
        row["volume"] = volumePercent(stream.value("volume").toObject());
        rows << row;
    }
    return rows;
}

AudioProvider::AudioProvider(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *parent)
    : QObject(parent), m_runner(runner), m_coordinator(coordinator) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(200);
    connect(&m_debounce, &QTimer::timeout, this, &AudioProvider::refresh);
}

AudioProvider::~AudioProvider() {
    setActive(false);
}

void AudioProvider::setActive(bool active) {
    if (active == (m_subscribe != nullptr))
        return;
    if (!active) {
        m_subscribe->disconnect();
        m_subscribe->kill();
        m_subscribe->waitForFinished(300);
        delete m_subscribe;
        m_subscribe = nullptr;
        return;
    }
    const QString pactl = m_runner->env().findExecutable("pactl");
    if (pactl.isEmpty())
        return;
    m_subscribe = new QProcess(this);
    m_subscribe->setProcessEnvironment(m_runner->env().process);
    connect(m_subscribe, &QProcess::readyReadStandardOutput, this, [this] {
        m_subscribe->readAllStandardOutput();
        m_debounce.start();
    });
    m_subscribe->start(pactl, {"subscribe"});
}

int AudioProvider::defaultOutputVolume() const {
    for (const auto &row : m_outputs)
        if (row.toMap().value("name").toString() == m_defaultOutputSink)
            return row.toMap().value("volume").toInt();
    for (const auto &row : m_outputs)
        if (row.toMap().value("isDefault").toBool())
            return row.toMap().value("volume").toInt();
    return -1;
}

void AudioProvider::refresh() {
    const int generation = ++m_generation;
    struct Pending {
        QByteArray sinks, sources, inputs;
        QString defaultSink, defaultSource;
        int remaining = 5;
        bool failed = false;
    };
    auto pending = std::make_shared<Pending>();
    auto finished = [this, pending, generation] {
        if (--pending->remaining > 0 || generation != m_generation)
            return;
        m_status = pending->failed ? "unavailable" : "ok";
        m_outputs = parseNodes(pending->sinks, pending->defaultSink, false);
        m_inputs = parseNodes(pending->sources, pending->defaultSource, true);
        m_streams = parseStreams(pending->inputs);
        matchStreams(m_streams, m_outputs);
        // The app's own icon from the desktop's icon theme, found once per app.
        const Env &env = m_runner->env();
        for (auto &value : m_streams) {
            auto stream = value.toMap();
            const QString key = stream.value("iconName").toString() + "|" + stream.value("binary").toString();
            if (!m_appIcons.contains(key)) {
                QStringList names;
                for (const QString &name : {stream.value("iconName").toString(), stream.value("binary").toString(), stream.value("app").toString().toLower()})
                    if (!name.isEmpty() && !name.contains('/'))
                        names << name;
                const QString path = names.isEmpty() ? QString() : IconThemes::findIcon(env, IconThemes::desktopDefault(env), names, 48);
                m_appIcons.insert(key, path.isEmpty() ? QString() : QUrl::fromLocalFile(path).toString());
            }
            stream["icon"] = m_appIcons.value(key);
            value = stream;
        }
        emit changed();
        // A DSP sink can front the real output, where loudness actually lives.
        m_runner->run({"omarchy", "audio", "output", "sink"}, CommandOptions{3000}, this, [this, generation](const CommandResult &result) {
            if (generation != m_generation)
                return;
            m_routingKnown = result.ok() && !result.outText().isEmpty();
            m_defaultOutputSink = m_routingKnown ? result.outText() : QString();
            emit changed();
            emit refreshed();
        });
    };
    auto fetch = [this, pending, finished](const QStringList &argv, std::function<void(const CommandResult &)> store) {
        m_runner->run(argv, CommandOptions{4000}, this, [pending, finished, store](const CommandResult &result) {
            if (!result.ok())
                pending->failed = true;
            store(result);
            finished();
        });
    };
    fetch({"pactl", "-f", "json", "list", "sinks"}, [pending](const CommandResult &r) { pending->sinks = r.out; pending->failed |= !QJsonDocument::fromJson(r.out).isArray() || r.truncated; });
    fetch({"pactl", "-f", "json", "list", "sources"}, [pending](const CommandResult &r) { pending->sources = r.out; pending->failed |= !QJsonDocument::fromJson(r.out).isArray() || r.truncated; });
    fetch({"pactl", "-f", "json", "list", "sink-inputs"}, [pending](const CommandResult &r) { pending->inputs = r.out; pending->failed |= !QJsonDocument::fromJson(r.out).isArray() || r.truncated; });
    fetch({"pactl", "get-default-sink"}, [pending](const CommandResult &r) { pending->defaultSink = r.outText(); });
    fetch({"pactl", "get-default-source"}, [pending](const CommandResult &r) { pending->defaultSource = r.outText(); });
}

static QVariantMap namedNode(const QVariantList &rows, const QString &name) {
    for (const auto &row : rows)
        if (row.toMap().value("name").toString() == name)
            return row.toMap();
    return {};
}

ProviderPlan AudioProvider::plan(const QString &side, const QString &action, const QString &target, const QString &value, const QString &expectedIdentity) const {
    ProviderPlan p;
    p.operation = "audio." + (side == QLatin1String("stream") ? QStringLiteral("moveStream")
        : action == QLatin1String("default") ? QStringLiteral("setDefault") + (side == QLatin1String("input") ? "Input" : "Output")
        : QStringLiteral("set") + (side == QLatin1String("input") ? "Input" : "") + (action == QLatin1String("volume") ? "Volume" : "Mute"));
    p.target = target;
    const auto fail = [&](const QString &code, const QString &error) { p.errorCode = code; p.error = error; return p; };
    const bool input = side == QLatin1String("input"), stream = side == QLatin1String("stream");
    if ((!input && !stream && side != QLatin1String("output")) ||
        (stream ? action != QLatin1String("move") : !QStringList{"default", "volume", "mute"}.contains(action)))
        return fail("invalid_arguments", "Unknown audio operation.");
    if (m_status != QLatin1String("ok"))
        return fail("read_unavailable", tr("Sound devices could not be read. Refresh before changing them."));
    if (action == QLatin1String("volume")) {
        bool valid = false;
        const int percent = value.toInt(&valid);
        if (!valid || percent < 0 || percent > 150)
            return fail("invalid_value", "Volume must be a whole percentage from 0 to 150.");
        p.after = QString::number(percent);
    } else if (action == QLatin1String("mute")) {
        if (value != QLatin1String("true") && value != QLatin1String("false"))
            return fail("invalid_value", "Mute must be true or false.");
        p.after = value;
    } else {
        p.after = stream ? value : target;
    }
    const bool routed = !input && !stream && action != QLatin1String("default") && namedNode(m_outputs, target).value("isDefault").toBool();
    if (routed && !m_routingKnown)
        return fail("read_unavailable", tr("The output behind the sound processing could not be confirmed."));
    const QString resolved = routed ? m_defaultOutputSink : target;
    const QVariantMap node = namedNode(input ? m_inputs : m_outputs, stream ? value : resolved);
    if (node.isEmpty() || (!stream && namedNode(input ? m_inputs : m_outputs, target).isEmpty()))
        return fail("stale_target", tr("That device is no longer available."));
    bool identityValid = false;
    node.value("objectId").toString().toUInt(&identityValid);
    if (!identityValid)
        return fail("read_unavailable", tr("The sound device's identity could not be read."));
    p.details = {{"side", side}, {"action", action}, {"effectiveTarget", stream ? value : resolved},
                 {"objectId", node.value("objectId").toString()}, {"scope", "audio-session"}, {"description", node.value("description").toString()}};
    if (routed) {
        p.details["checkRouting"] = true;
    }
    if (stream) {
        bool valid = false;
        const int index = target.toInt(&valid);
        QVariantMap row;
        for (const auto &candidate : m_streams)
            if (candidate.toMap().value("index").toInt() == index)
                row = candidate.toMap();
        if (!valid || index < 0 || row.isEmpty())
            return fail("stale_target", tr("That app stopped playing before it could move."));
        if (row.value("identity").toString().isEmpty())
            return fail("read_unavailable", tr("A stable identity for that audio stream could not be read."));
        if (!expectedIdentity.isEmpty() && row.value("identity").toString() != expectedIdentity)
            return fail("stale_target", tr("That audio stream was replaced. Read the current streams before moving it."));
        p.target = row.value("selector").toString();
        p.details["streamIdentity"] = row.value("identity").toString();
        p.before = row.value("output").toString();
        p.effects << "Moves this live application stream to the selected output.";
    } else if (action == QLatin1String("default")) {
        for (const auto &row : input ? m_inputs : m_outputs)
            if (row.toMap().value("isDefault").toBool())
                p.before = row.toMap().value("name").toString();
        p.effects << "Changes the default device and asks Omarchy to move active application streams.";
        p.details["verificationScope"] = "default-device-only";
    } else if (action == QLatin1String("volume")) {
        if (node.value("volume").toInt() < 0)
            return fail("read_unavailable", tr("The current volume could not be read."));
        p.before = QString::number(node.value("volume").toInt());
        p.effects << "Changes volume on the effective device; levels above 100% can distort.";
    } else {
        if (!node.value("muteKnown").toBool())
            return fail("read_unavailable", tr("The current mute state could not be read."));
        p.before = node.value("mute").toBool() ? "true" : "false";
        p.effects << "Changes mute on the effective sound device.";
    }
    const QString compatKey = action == QLatin1String("default") ? "audio." + side + ".default"
        : p.details.value("checkRouting").toBool() ? "audio.output.routing" : QString();
    const QString changed = Compat(m_runner->env()).changedForKey(compatKey);
    if (!changed.isEmpty())
        return fail("compatibility_blocked", QStringLiteral("Omarchy's %1 changed since review; this operation is paused.").arg(changed));
    p.ok = true;
    return p;
}

void AudioProvider::change(const QString &side, const QString &action, const QString &target, const QString &value) {
    const ProviderPlan p = plan(side, action, target, value);
    if (!p.ok) {
        Transaction tx;
        tx.phase = "failed";
        tx.message = p.error;
        emit message(p.error, true);
        emit operationFinished(tx);
        return;
    }
    const bool input = side == QLatin1String("input"), stream = side == QLatin1String("stream");
    const QString effective = p.details.value("effectiveTarget").toString();
    const QString list = input ? "sources" : "sinks";
    QStringList argv;
    if (action == QLatin1String("default"))
        argv = {"omarchy", "audio", side, "set", "default", p.details.value("objectId").toString(), target};
    else if (stream)
        argv = {"pactl", "move-sink-input", target, effective};
    else
        argv = {"pactl", "set-" + QString(input ? "source" : "sink") + "-" + action, effective,
                action == QLatin1String("volume") ? p.after + "%" : p.after == QLatin1String("true") ? "1" : "0"};
    ChangeCoordinator::Request request;
    request.settingId = action == QLatin1String("default") ? (input ? "sound.input" : "sound.output") : p.operation;
    request.label = stream ? tr("App audio output") : action == QLatin1String("default") ? (input ? tr("Microphone") : tr("Sound output"))
        : action == QLatin1String("volume") ? (input ? tr("Microphone volume") : tr("Sound output volume"))
        : (input ? tr("Microphone mute") : tr("Sound output mute"));
    request.before = p.before;
    request.after = p.after;
    request.extra = p.details;
    request.extra["target"] = p.target;
    request.apply = [this, p, argv, list, effective, stream, target](ChangeCoordinator::Done done) {
        auto run = [this, p, argv, stream, target, done] {
            auto write = [this, argv, done] {
                CommandOptions options;
                options.mutation = true;
                m_runner->run(argv, options, this, [argv, done](const CommandResult &r) { done(r.ok(), r.describe(argv.first())); });
            };
            if (stream) {
                m_runner->run({"pactl", "-f", "json", "list", "sink-inputs"}, CommandOptions{4000}, this, [this, p, target, write, done](const CommandResult &r) {
                    for (const auto &row : parseStreams(r.out))
                        if (r.ok() && !r.truncated && row.toMap().value("index").toInt() == target.toInt()
                            && row.toMap().value("identity").toString() == p.details.value("streamIdentity").toString()) {
                            write();
                            return;
                        }
                    done(false, tr("That audio stream changed or disappeared; refresh before moving it."));
                });
            } else if (p.details.value("checkRouting").toBool()) {
                m_runner->run({"omarchy", "audio", "output", "sink"}, CommandOptions{3000}, this, [p, write, done](const CommandResult &r) {
                    if (r.ok() && r.outText() == p.details.value("effectiveTarget").toString())
                        write();
                    else
                        done(false, QStringLiteral("The effective audio output changed; refresh before changing it."));
                });
            } else {
                write();
            }
        };
        m_runner->run({"pactl", "-f", "json", "list", list}, CommandOptions{4000}, this, [this, p, effective, input = (list == QLatin1String("sources")), run, done](const CommandResult &r) {
            const QVariantMap current = namedNode(parseNodes(r.out, {}, input), effective);
            if (r.ok() && !r.truncated && !current.isEmpty() && current.value("objectId").toString() == p.details.value("objectId").toString())
                run();
            else
                done(false, tr("That sound device changed or disappeared; refresh before changing it."));
        });
    };
    request.verify = [this, p, input, stream, action, target, effective, list](ChangeCoordinator::Done done) {
        if (action == QLatin1String("default")) {
            m_runner->run({"pactl", input ? "get-default-source" : "get-default-sink"}, CommandOptions{3000}, this, [this, target, done](const CommandResult &r) {
                done(r.ok() && !r.truncated && r.outText() == target, tr("The device did not become the default."));
            });
            return;
        }
        m_runner->run({"pactl", "-f", "json", "list", stream ? "sink-inputs" : list}, CommandOptions{4000}, this,
                      [this, p, input, stream, action, target, effective, done](const CommandResult &r) {
            bool verified = false;
            if (r.ok() && !r.truncated) {
                if (stream) {
                    QVariantList rows = parseStreams(r.out);
                    matchStreams(rows, m_outputs);
                    for (const auto &row : rows) {
                        const auto map = row.toMap();
                        verified |= map.value("index").toInt() == target.toInt()
                            && map.value("identity").toString() == p.details.value("streamIdentity").toString()
                            && map.value("output").toString() == effective;
                    }
                } else {
                    const auto row = namedNode(parseNodes(r.out, {}, input), effective);
                    verified = !row.isEmpty() && row.value("objectId").toString() == p.details.value("objectId").toString()
                        && (action == QLatin1String("volume") ? row.value("volume").toInt() == p.after.toInt()
                            : row.value("muteKnown").toBool() && row.value("mute").toBool() == (p.after == QLatin1String("true")));
                }
            }
            done(verified, stream ? tr("The app did not move to that output.") : tr("The sound change could not be confirmed."));
        });
    };
    if (action == QLatin1String("default")) {
        m_pendingDefault = target;
        emit changed();
    }
    m_coordinator->submit(std::move(request), [this, target, action, description = p.details.value("description").toString()](const Transaction &tx) {
        if (m_pendingDefault == target)
            m_pendingDefault.clear();
        if (tx.phase != QLatin1String("committed"))
            emit message(tx.message, true);
        else if (action == QLatin1String("default"))
            emit message(tr("Now using %1").arg(description), false);
        emit operationFinished(tx);
        refresh();
    });
}

void AudioProvider::setDefaultOutput(const QString &name) { change("output", "default", name); }
void AudioProvider::setDefaultInput(const QString &name) { change("input", "default", name); }
void AudioProvider::setVolume(const QString &name, int percent) { change("output", "volume", name, QString::number(qBound(0, percent, 150))); }
void AudioProvider::setInputVolume(const QString &name, int percent) { change("input", "volume", name, QString::number(qBound(0, percent, 150))); }
void AudioProvider::setMute(const QString &name, bool mute) { change("output", "mute", name, mute ? "true" : "false"); }
void AudioProvider::setInputMute(const QString &name, bool mute) { change("input", "mute", name, mute ? "true" : "false"); }
void AudioProvider::moveStream(int index, const QString &sink) { change("stream", "move", QString::number(index), sink); }
