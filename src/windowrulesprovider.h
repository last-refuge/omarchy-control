#pragma once

#include <QObject>
#include <QVariantList>

#include "changecoordinator.h"
#include "commandrunner.h"

// Per-app window rules, kept in the owned Lua module. Rules in the user's own files
// stay theirs; this lists and edits only what was made here.
class WindowRulesProvider : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList rules READ rules NOTIFY changed)

public:
    WindowRulesProvider(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *parent = nullptr);

    QVariantList rules() const;

    // Empty if acceptable; otherwise why not.
    Q_INVOKABLE QString validate(const QString &match, const QString &action, const QString &value) const { return check(match, action, value); }
    static QString check(const QString &match, const QString &action, const QString &value);
    Q_INVOKABLE void addRule(const QString &match, const QString &action, const QString &value);
    Q_INVOKABLE void removeRule(int index);

signals:
    void changed();
    void message(const QString &text, bool isError);

private:
    void submit(const QString &label, std::function<bool(class HyprModule &, QString *)> edit, const QString &success);

    CommandRunner *m_runner;
    ChangeCoordinator *m_coordinator;
};
