#pragma once

#include <QJsonObject>
#include <QStringList>

#include "env.h"

// Per-user filesystem registration only. It neither starts an agent nor claims
// that an already-running harness has refreshed its skill cache.
namespace SkillIntegration {
struct Result {
    QJsonObject data;
    QString code;
    QString message;
    QString outcome = QStringLiteral("completed");
    bool verified = false;
};

Result run(const Env &env, const QString &action, const QStringList &targets = {}, bool dryRun = false);
}
