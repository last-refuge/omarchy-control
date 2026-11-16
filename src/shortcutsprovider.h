#pragma once

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QVariantList>

#include "changecoordinator.h"
#include "commandrunner.h"
#include "provideroperation.h"

// Keyboard shortcuts as Hyprland reports them, plus editing of the bindings this
// app owns. A collision is reported, never silently replaced, and the shortcut
// that opens this window's way back (the launcher) needs an explicit decision.
class ShortcutsProvider : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList binds READ binds NOTIFY changed)
    Q_PROPERTY(QVariantList ownedBinds READ ownedBinds NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)

public:
    ShortcutsProvider(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *parent = nullptr);

    QVariantList binds() const { return m_binds; }
    QVariantList ownedBinds() const;
    QString status() const { return m_status; }

    Q_INVOKABLE void refresh();
    void refresh(std::function<void()> done);
    QJsonObject snapshot() const;
    void planAdd(const QString &keys, const QString &description, const QString &command, bool replace, std::function<void(ProviderPlan)> done);
    void planRemove(const QString &selector, std::function<void(ProviderPlan)> done);
    void applyPlan(const ProviderPlan &plan);
    Q_INVOKABLE QVariantMap checkKeys(const QString &keys) const;
    Q_INVOKABLE void addBinding(const QString &keys, const QString &description, const QString &command, bool replaceExisting);
    Q_INVOKABLE void removeBinding(const QString &keys);

    static QVariantList parseBinds(const QByteArray &json);
    static QString modsToText(int modmask);
    static bool isValidKeys(const QString &keys);
    static QString canonicalKeys(const QString &keys);

signals:
    void changed();
    void message(const QString &text, bool isError);
    void operationFinished(const Transaction &transaction);

private:
    void reject(const ProviderPlan &plan);

    CommandRunner *m_runner;
    ChangeCoordinator *m_coordinator;
    QVariantList m_binds;
    QJsonArray m_reportedBinds;
    QString m_reason;
    QString m_status = "loading";
};
