#pragma once
#include <QJsonObject>
#include "env.h"

namespace DisplayPreview {
bool validToken(const QString &token);
QString runtimeBase(const Env &env, const QString &token);
bool prepare(const Env &env, QString *error);
bool save(const Env &env, const QJsonObject &receipt, QString *error = nullptr);
QJsonObject load(const Env &env, const QString &token, QString *error = nullptr);
QJsonObject current(const Env &env, QString *error = nullptr);
bool terminal(const QJsonObject &receipt);
bool sameSession(const Env &env, const QJsonObject &receipt);
QString pendingReason(const Env &env, const QString &exceptToken = {});
bool capturePersistence(const Env &env, QJsonObject *receipt, const QByteArray &module, const QByteArray &import, QString *error);
bool restorePersistence(const Env &env, const QJsonObject &receipt, QString *error);
}
