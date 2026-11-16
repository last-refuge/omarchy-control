#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantList>

#include "changecoordinator.h"
#include "commandrunner.h"
#include "provideroperation.h"

class AudioProvider : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY changed)
    Q_PROPERTY(QVariantList inputs READ inputs NOTIFY changed)
    Q_PROPERTY(QVariantList streams READ streams NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(int defaultOutputVolume READ defaultOutputVolume NOTIFY changed)
    // The device a default change is on its way to, until Omarchy confirms or refuses it.
    Q_PROPERTY(QString pendingDefault READ pendingDefault NOTIFY changed)

public:
    AudioProvider(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *parent = nullptr);
    ~AudioProvider() override;

    QVariantList outputs() const { return m_outputs; }
    QVariantList inputs() const { return m_inputs; }
    QVariantList streams() const { return m_streams; }
    QString status() const { return m_status; }
    int defaultOutputVolume() const;
    QString pendingDefault() const { return m_pendingDefault; }
    QString effectiveOutput() const { return m_defaultOutputSink; }
    bool outputRoutingKnown() const { return m_routingKnown; }
    ProviderPlan plan(const QString &side, const QString &action, const QString &target, const QString &value = {}, const QString &expectedIdentity = {}) const;

    Q_INVOKABLE void refresh();
    // Listens for PipeWire events only while the Sound page is on screen.
    Q_INVOKABLE void setActive(bool active);
    Q_INVOKABLE void setDefaultOutput(const QString &name);
    Q_INVOKABLE void setDefaultInput(const QString &name);
    Q_INVOKABLE void setVolume(const QString &name, int percent);
    Q_INVOKABLE void setInputVolume(const QString &name, int percent);
    Q_INVOKABLE void setMute(const QString &name, bool mute);
    Q_INVOKABLE void setInputMute(const QString &name, bool mute);
    Q_INVOKABLE void moveStream(int index, const QString &sink);

    static QVariantList parseNodes(const QByteArray &json, const QString &defaultName, bool sources);
    static QVariantList parseStreams(const QByteArray &json);
    // A short name and the line under it, from the card and port PipeWire describes,
    // falling back to the full description when there is nothing to split.
    static QVariantMap nodeNames(const QString &description, const QJsonObject &properties);
    // Which output each stream plays through, by the index the stream reports.
    static void matchStreams(QVariantList &streams, const QVariantList &outputs);

signals:
    void changed();
    void refreshed();
    void operationFinished(const Transaction &transaction);
    void message(const QString &text, bool isError);

private:
    void change(const QString &side, const QString &action, const QString &target, const QString &value = {});

    CommandRunner *m_runner;
    ChangeCoordinator *m_coordinator;
    QVariantList m_outputs, m_inputs, m_streams;
    QString m_status = "loading";
    QString m_defaultOutputSink;
    bool m_routingKnown = false;
    QString m_pendingDefault;
    QHash<QString, QString> m_appIcons;
    QProcess *m_subscribe = nullptr;
    QTimer m_debounce;
    int m_generation = 0;
};
