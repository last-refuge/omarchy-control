#pragma once

#include <QHash>
#include <QObject>
#include <QVariantMap>

#include "catalog.h"
#include "commandrunner.h"

// What this machine has. Detection only ever reads: it inspects PATH, /sys, and
// config files, and asks running services through their status interfaces.
class Capabilities : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap values READ values NOTIFY changed)

public:
    Capabilities(CommandRunner *runner, QObject *parent = nullptr);

    QVariantMap values() const { return m_values; }
    bool has(const QString &name) const { return m_values.value(name).toBool(); }
    bool hasCommand(const QString &name) const;

    struct Availability {
        bool available = true;
        QString kind;    // "", "hardware", "tool", "service"
        QString reason;
    };
    Availability availability(const CatalogEntry &entry) const;

    void scanFiles();                // Cheap and synchronous: /sys, config files, PATH.
    void scanServices();             // Asynchronous: shell ping, Hyprland reachability.
    void scanServices(std::function<void()> done);
    void scanServicesBlocking();     // For the headless CLI.
    QVariantMap versions() const { return m_versions; }

signals:
    void changed();

private:
    void set(const QString &key, const QVariant &value);

    CommandRunner *m_runner;
    QVariantMap m_values;
    QVariantMap m_versions;
};
