#pragma once

#include "changecoordinator.h"
#include "commandrunner.h"
#include "provideroperation.h"

// Shared owned-module transaction boundary for shortcuts and window rules.
// Planning/inspection never evaluates Lua or changes files.
namespace HyprChange {
ProviderPlan base(const Env &env, const QString &operation, const QString &target, const QString &collection);
QJsonArray selectable(const QJsonObject &snapshot, const QString &collection);
int selected(const ProviderPlan &plan, const QString &selector);
bool validSelector(const QString &selector);
void runtime(CommandRunner *runner, QObject *context, ProviderPlan plan, std::function<void(ProviderPlan)> done);
void submit(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *context, const ProviderPlan &plan,
            ChangeCoordinator::Step preflight, ChangeCoordinator::Step verify, ChangeCoordinator::Finished done);
}
