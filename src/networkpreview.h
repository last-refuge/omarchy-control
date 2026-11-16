#pragma once

#include <QJsonObject>
#include <QStringList>

#include "env.h"

// Headless, bounded NetworkManager calls over one pinned D-Bus connection.
// The daemon owns rollback; private receipts bind later calls to that daemon
// and checkpoint. GUI compatibility paths consult pendingReason before writing.
namespace NetworkPreview {
QJsonObject execute(const Env &env, const QStringList &arguments, bool dryRun, bool acknowledged);
QString pendingReason(const Env &env);
bool validToken(const QString &token);
QJsonObject localReceipt(const Env &env, QString *error = nullptr);
}
