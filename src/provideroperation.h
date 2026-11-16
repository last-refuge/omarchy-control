#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

// A provider's validated operation over its latest snapshot. The GUI and CLI
// use the same plan; runtime device state remains authoritative at execution.
struct ProviderPlan {
    bool ok = false;
    QString errorCode = "invalid_value", error;
    QString operation, target, before, after;
    QString confirmText;
    QStringList effects;
    QJsonObject details;

    QJsonObject toJson() const {
        return {{"ok", ok}, {"errorCode", ok ? QString() : errorCode}, {"error", error},
                {"operation", operation}, {"target", target}, {"before", before}, {"after", after},
                {"confirmText", confirmText}, {"effects", QJsonArray::fromStringList(effects)},
                {"recovery", "no-undo"}, {"details", details}};
    }
};
