#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariantList>

#include "changecoordinator.h"
#include "commandrunner.h"
#include "provideroperation.h"

// Per-app window rules, kept in the owned Lua module. Rules in the user's own files
// stay theirs; this lists and edits only what was made here.
class WindowRulesProvider : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList rules READ rules NOTIFY changed)
    Q_PROPERTY(QVariantList selectableRules READ selectableRules NOTIFY changed)

public:
    WindowRulesProvider(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *parent = nullptr);

    QVariantList rules() const;
    QJsonObject snapshot() const;
    QVariantList selectableRules() const { return snapshot().value("rules").toArray().toVariantList(); }
    static QJsonObject constraints();
    void planAdd(const QString &match, const QString &action, const QString &value, std::function<void(ProviderPlan)> done);
    void planRemove(const QString &selector, std::function<void(ProviderPlan)> done);
    void applyPlan(const ProviderPlan &plan);

    // Empty if acceptable; otherwise why not.
    Q_INVOKABLE QString validate(const QString &match, const QString &action, const QString &value) const { return check(match, action, value); }
    static QString check(const QString &match, const QString &action, const QString &value);
    Q_INVOKABLE void addRule(const QString &match, const QString &action, const QString &value);
    Q_INVOKABLE void removeRule(const QVariant &selection);

signals:
    void changed();
    void message(const QString &text, bool isError);
    void operationFinished(const Transaction &transaction);

private:
    void reject(const ProviderPlan &plan);

    CommandRunner *m_runner;
    ChangeCoordinator *m_coordinator;
};
