#pragma once

#include <QJsonObject>
#include <QStringList>

#include "env.h"

struct CatalogEntry;
class Catalog;

// Explicitly versioned automation, alongside the existing command line. Results
// are values so validation and failure responses cannot bypass JSON formatting.
namespace CliApi {

struct Response {
    int exitCode = 0;
    QJsonObject body;
};

bool requested(const QStringList &arguments);
Response execute(const Env &env, const QStringList &arguments);
QJsonObject describe(const CatalogEntry &entry, const Env &env);
QJsonObject coverage(const Catalog &catalog);

}  // namespace CliApi
