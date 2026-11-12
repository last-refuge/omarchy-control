#include "compat.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "fileops.h"

Compat::Compat(const Env &env, const QString &directory) : m_env(env) {
    QFile file(directory + "/compat.json");
    if (file.open(QIODevice::ReadOnly)) {
        const auto root = QJsonDocument::fromJson(file.readAll()).object();
        m_baselineOmarchy = root.value("omarchy").toString();
        m_baselineHyprland = root.value("hyprland").toString();
        // Usually one command; a setting that relies on how several behave together lists
        // them. Profile adapters that are not settings are pinned the same way.
        QJsonObject settings = root.value("settings").toObject();
        const QJsonObject adapters = root.value("adapters").toObject();
        for (auto it = adapters.begin(); it != adapters.end(); ++it)
            settings.insert(it.key(), it.value());
        for (auto it = settings.begin(); it != settings.end(); ++it) {
            QStringList binaries;
            if (it.value().isArray()) {
                for (const auto &binary : it.value().toArray())
                    binaries << binary.toString();
            } else {
                binaries << it.value().toString();
            }
            m_settingToBinaries.insert(it.key(), binaries);
        }
        const auto commands = root.value("commands").toObject();
        for (auto it = commands.begin(); it != commands.end(); ++it)
            m_reviewed.insert(it.key(), it.value().toString());
    }
    loadAccepted();
}

void Compat::loadAccepted() {
    QByteArray contents;
    FileOps::readFile(m_env.appConfigDir + "/compat-accepted.json", &contents);
    const auto object = QJsonDocument::fromJson(contents).object();
    m_accepted.clear();
    for (auto it = object.begin(); it != object.end(); ++it)
        m_accepted.insert(it.key(), it.value().toString());
}

QString Compat::currentHash(const QString &binary) const {
    const auto cached = m_hashes.constFind(binary);
    if (cached != m_hashes.constEnd())
        return *cached;
    const QString path = m_env.findExecutable(binary);
    const QString hash = path.isEmpty() ? QString() : FileOps::fingerprint(path);
    m_hashes.insert(binary, hash);
    return hash;
}

bool Compat::isAccepted(const QString &key, const QString &value) const {
    return m_accepted.value(key) == value;
}

bool Compat::hyprlandDiffers(const QString &version) const {
    return !version.isEmpty() && !version.startsWith(m_baselineHyprland) && !isAccepted("hyprland", version);
}

QString Compat::changedFor(const CatalogEntry &entry, const QString &hyprlandVersion) const {
    if (!m_env.enforceCompat || !entry.isEditable())
        return {};
    if (entry.write.value("kind").toString() == QLatin1String("hypr"))
        return hyprlandDiffers(hyprlandVersion) ? QStringLiteral("Hyprland %1").arg(hyprlandVersion) : QString();
    return changedForKey(entry.id);
}

QString Compat::changedForKey(const QString &key) const {
    if (!m_env.enforceCompat)
        return {};
    for (const QString &binary : m_settingToBinaries.value(key)) {
        const QString hash = currentHash(binary);
        // A missing command is an availability problem, reported as such elsewhere.
        if (!hash.isEmpty() && hash != m_reviewed.value(binary) && !isAccepted(binary, hash))
            return binary;
    }
    return {};
}

QStringList Compat::changedCommands(const QString &hyprlandVersion) const {
    QStringList changed;
    if (!m_env.enforceCompat)
        return changed;
    if (hyprlandDiffers(hyprlandVersion))
        changed << QStringLiteral("Hyprland %1").arg(hyprlandVersion);
    for (auto it = m_reviewed.constBegin(); it != m_reviewed.constEnd(); ++it) {
        const QString hash = currentHash(it.key());
        if (!hash.isEmpty() && hash != it.value() && !isAccepted(it.key(), hash))
            changed << it.key();
    }
    changed.sort();
    return changed;
}

void Compat::acceptAll(const QString &hyprlandVersion) {
    QJsonObject object;
    for (auto it = m_accepted.constBegin(); it != m_accepted.constEnd(); ++it)
        object.insert(it.key(), it.value());
    for (auto it = m_reviewed.constBegin(); it != m_reviewed.constEnd(); ++it) {
        const QString hash = currentHash(it.key());
        if (!hash.isEmpty() && hash != it.value())
            object.insert(it.key(), hash);
    }
    if (!hyprlandVersion.isEmpty())
        object.insert("hyprland", hyprlandVersion);
    FileOps::atomicWrite(m_env.appConfigDir + "/compat-accepted.json", QJsonDocument(object).toJson(), nullptr, 0600);
    loadAccepted();
}

QString Compat::baseline() const {
    return QStringLiteral("Omarchy %1, Hyprland %2").arg(m_baselineOmarchy, m_baselineHyprland);
}
