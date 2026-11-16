#pragma once
#include <QJsonObject>
#include "env.h"

// Bounded, non-following snapshots of plugin trees, including Git metadata and
// symbolic links. Recovery preserves displaced content and refuses later edits.
namespace PluginFiles {
QJsonObject inspect(const QString &path, bool requireOwner = true, QString *error = nullptr);
bool safeRoot(const Env &env, QString *error = nullptr);
bool capture(const Env &env, const QString &token, const QString &id, QJsonObject *receipt, QString *error);
bool finish(const Env &env, QJsonObject *receipt, bool writerOk, QString *error);
QJsonObject load(const Env &env, const QString &token, QString *error);
bool save(const Env &env, const QJsonObject &receipt, QString *error);
bool canRecover(const Env &env, const QJsonObject &receipt, QString *error);
bool recover(const Env &env, QJsonObject *receipt, QString *error);
}
