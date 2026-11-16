#pragma once

#include <QStringList>

#include "env.h"

// The headless interface: `omarchy-control <command>`. Each command takes --json,
// exits 0 or 1, and preserves its legacy output. Opt-in API v1 returns errors in
// its JSON envelope. Both interfaces share the window's settings backend.
namespace Cli {

bool isCliCommand(const QString &command);
int run(const Env &env, const QStringList &arguments);

}  // namespace Cli
