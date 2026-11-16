#pragma once
#include <QJsonObject>
#include <functional>
#include "env.h"
class Catalog;
class CommandRunner;
class ChangeCoordinator;
namespace Preferences {
QJsonObject pins(const Env &env, const Catalog &catalog, const QString &action = "list", const QString &id = {}, bool dryRun = false);
void crashMutes(CommandRunner *runner, ChangeCoordinator *coordinator, const QString &program,
                const QString &value, bool dryRun, std::function<void(QJsonObject)> done);
}
