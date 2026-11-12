#include "packageprovider.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

#include "fileops.h"
#include <QSet>
#include <memory>

PackageProvider::PackageProvider(CommandRunner *runner, QObject *parent) : QObject(parent), m_runner(runner) {}

// Parsed output stays the same whatever language the session uses.
QStringList PackageProvider::pacman(const QStringList &arguments) const {
    return QStringList{"env", "LC_ALL=C", "pacman"} + arguments;
}

QStringList PackageProvider::parseNames(const QByteArray &output) {
    QStringList names;
    for (const QString &line : QString::fromUtf8(output).split('\n', Qt::SkipEmptyParts)) {
        const QString name = line.trimmed().section(' ', 0, 0);
        if (!name.isEmpty() && !names.contains(name))
            names << name;
    }
    return names;
}

// `pacman -Sl` rows, "repository name version [installed]", for what is installed.
QHash<QString, QString> PackageProvider::parseRepositories(const QByteArray &syncList) {
    QHash<QString, QString> repositories;
    for (const QString &line : QString::fromUtf8(syncList).split('\n', Qt::SkipEmptyParts)) {
        if (!line.contains(QLatin1String("[installed")))
            continue;
        const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
        // The first repository listed wins, as it does for pacman.
        if (parts.size() >= 2 && !repositories.contains(parts[1]))
            repositories.insert(parts[1], parts[0]);
    }
    return repositories;
}

void PackageProvider::readInventory(std::function<void(const Inventory &)> done) {
    auto inventory = std::make_shared<Inventory>();
    CommandOptions options;
    options.timeoutMs = 20000;
    const auto fail = [inventory, done](const CommandResult &result) {
        inventory->error = result.describe(QStringLiteral("pacman"));
        done(*inventory);
    };
    m_runner->run(pacman({"-Q"}), options, this, [this, inventory, done, fail, options](const CommandResult &all) {
        if (!all.ok()) {
            fail(all);
            return;
        }
        for (const QString &line : QString::fromUtf8(all.out).split('\n', Qt::SkipEmptyParts)) {
            const QStringList parts = line.trimmed().split(' ', Qt::SkipEmptyParts);
            if (parts.size() >= 2)
                inventory->installed.insert(parts[0], PackageInfo{parts[0], parts[1], QString(), false, false});
        }
        m_runner->run(pacman({"-Qqe"}), options, this, [this, inventory, done, fail, options](const CommandResult &wanted) {
            if (!wanted.ok()) {
                fail(wanted);
                return;
            }
            for (const QString &name : parseNames(wanted.out))
                if (inventory->installed.contains(name))
                    inventory->installed[name].explicitly = true;
            // pacman exits 1 when nothing is foreign, which is an answer, not a failure.
            m_runner->run(pacman({"-Qqm"}), options, this, [this, inventory, done, fail, options](const CommandResult &foreign) {
                if (foreign.failure != CommandResult::None) {
                    fail(foreign);
                    return;
                }
                for (const QString &name : parseNames(foreign.out))
                    if (inventory->installed.contains(name))
                        inventory->installed[name].foreign = true;
                m_runner->run(pacman({"-Sl"}), options, this, [this, inventory, done, options](const CommandResult &sync) {
                    const QHash<QString, QString> repositories = parseRepositories(sync.out);
                    for (auto it = inventory->installed.begin(); it != inventory->installed.end(); ++it)
                        if (!it->foreign)
                            it->repository = repositories.value(it.key());
                    // Reviewed in docs/MY-OMARCHY.md: it reads Omarchy's own package lists
                    // for this platform, and changes nothing.
                    m_runner->run({"omarchy", "pkg", "defaults"}, options, this, [this, inventory, done, options](const CommandResult &defaults) {
                        if (defaults.ok()) {
                            inventory->baseline = parseNames(defaults.out);
                            inventory->hasBaseline = !inventory->baseline.isEmpty();
                        }
                        // Omarchy's own list, read as data from its package.
                        QByteArray other;
                        if (FileOps::readFile(m_runner->env().omarchyPath + "/install/omarchy-other.packages", &other)) {
                            for (QString line : QString::fromUtf8(other).split('\n')) {
                                line = line.section('#', 0, 0).trimmed();
                                if (!line.isEmpty() && !line.contains(' '))
                                    inventory->hardware << line;
                            }
                        }
                        QStringList absent;
                        for (const QString &name : inventory->baseline)
                            if (!inventory->isInstalled(name))
                                absent << name;
                        if (absent.isEmpty()) {
                            inventory->ok = true;
                            done(*inventory);
                            return;
                        }
                        // pacman -T prints the names nothing installed satisfies, provides included.
                        m_runner->run(pacman(QStringList{"-T", "--"} + absent), options, this, [inventory, done, absent](const CommandResult &test) {
                            inventory->missingDefaults = test.failure == CommandResult::None ? parseNames(test.out) : absent;
                            inventory->ok = true;
                            done(*inventory);
                        });
                    });
                });
            });
        });
    });
}

void PackageProvider::previewInstall(const QStringList &names, std::function<void(const Preview &)> done) {
    QStringList arguments{"-S", "--print", "--needed", "--print-format", "%n %v %r", "--"};
    arguments << names;
    CommandOptions options;
    options.timeoutMs = 30000;
    m_runner->run(pacman(arguments), options, this, [done](const CommandResult &result) {
        Preview preview;
        static const QRegularExpression notFound(QStringLiteral("target not found: (\\S+)"));
        auto matches = notFound.globalMatch(QString::fromUtf8(result.out) + "\n" + QString::fromUtf8(result.err));
        while (matches.hasNext())
            preview.missing << matches.next().captured(1);
        if (result.ok()) {
            preview.ok = true;
            for (const QString &line : QString::fromUtf8(result.out).split('\n', Qt::SkipEmptyParts))
                if (!line.trimmed().isEmpty())
                    preview.packages << line.trimmed();
        } else {
            preview.error = QString::fromUtf8(result.err).trimmed().section('\n', 0, 0);
            if (preview.error.isEmpty())
                preview.error = result.describe(QStringLiteral("pacman"));
        }
        done(preview);
    });
}

void PackageProvider::previewRemoval(const QStringList &names, std::function<void(const Preview &)> done) {
    QStringList arguments{"-R", "--print", "--print-format", "%n %v", "--"};
    arguments << names;
    CommandOptions options;
    options.timeoutMs = 30000;
    m_runner->run(pacman(arguments), options, this, [done](const CommandResult &result) {
        Preview preview;
        const QString err = QString::fromUtf8(result.err);
        // pacman prints the error on stderr and the broken dependencies on stdout.
        static const QRegularExpression breaks(QStringLiteral("required by (\\S+)"));
        auto matches = breaks.globalMatch(QString::fromUtf8(result.out) + "\n" + err);
        while (matches.hasNext()) {
            const QString needer = matches.next().captured(1);
            if (!preview.requiredBy.contains(needer))
                preview.requiredBy << needer;
        }
        static const QRegularExpression notFound(QStringLiteral("target not found: (\\S+)"));
        auto missing = notFound.globalMatch(err);
        while (missing.hasNext())
            preview.missing << missing.next().captured(1);
        if (result.ok()) {
            preview.ok = true;
            for (const QString &line : QString::fromUtf8(result.out).split('\n', Qt::SkipEmptyParts))
                if (!line.trimmed().isEmpty())
                    preview.packages << line.trimmed();
        } else {
            preview.error = err.trimmed().section('\n', 0, 0);
            if (preview.error.isEmpty())
                preview.error = result.describe(QStringLiteral("pacman"));
        }
        done(preview);
    });
}

QString PackageProvider::aurInfoUrl(const QStringList &names) {
    QStringList arguments;
    for (const QString &name : names)
        arguments << "arg%5B%5D=" + QString::fromLatin1(QUrl::toPercentEncoding(name));
    return "https://aur.archlinux.org/rpc/v5/info?" + arguments.join('&');
}

void PackageProvider::lookupAur(const QStringList &names, std::function<void(const AurLookup &)> done) {
    auto lookup = std::make_shared<AurLookup>();
    // A few at a time, so no request is longer than the AUR accepts.
    QList<QStringList> chunks;
    for (int i = 0; i < names.size(); i += 50)
        chunks << names.mid(i, 50);
    auto next = std::make_shared<std::function<void(int)>>();
    *next = [this, chunks, lookup, done, next](int index) {
        if (index >= chunks.size()) {
            lookup->ok = true;
            done(*lookup);
            *next = nullptr;
            return;
        }
        // HTTPS only, like the update check, and with no detail about this computer
        // beyond the names being looked up.
        const QStringList argv{"curl", "-sS", "--fail", "--proto", "=https", "--proto-redir", "=https", "--connect-timeout", "15",
                               "--max-time", "30", "-A", "omarchy-control", aurInfoUrl(chunks[index])};
        CommandOptions options;
        options.timeoutMs = 40000;
        m_runner->run(argv, options, this, [lookup, done, next, index](const CommandResult &result) {
            const QJsonObject reply = QJsonDocument::fromJson(result.out).object();
            if (!result.ok() || reply.value("type").toString() != QLatin1String("multiinfo")) {
                lookup->error = !result.ok() ? result.describe(QStringLiteral("curl"))
                                             : reply.value("error").toString(QCoreApplication::translate("PackageProvider", "The AUR gave no answer."));
                done(*lookup);
                *next = nullptr;
                return;
            }
            for (const QJsonValue &package : reply.value("results").toArray())
                lookup->found << package.toObject().value("Name").toString();
            (*next)(index + 1);
        });
    };
    (*next)(0);
}

bool PackageProvider::hasAurHelper() const {
    return !m_runner->env().findExecutable(QStringLiteral("yay")).isEmpty();
}

bool PackageProvider::isProtected(const QString &name) {
    static const QSet<QString> protectedNames{
        // The system.
        "base", "base-devel", "filesystem", "glibc", "bash", "coreutils", "util-linux", "shadow", "systemd", "systemd-libs",
        "systemd-sysvcompat", "dbus", "pacman", "pacman-mirrorlist", "archlinux-keyring", "sudo", "mkinitcpio", "linux", "linux-lts",
        "linux-zen", "linux-hardened", "linux-firmware", "intel-ucode", "amd-ucode", "limine", "grub", "efibootmgr", "btrfs-progs",
        "cryptsetup", "lvm2", "snapper",
        // The session this desktop runs in.
        "omarchy", "omarchy-settings", "omarchy-keyring", "omarchy-control", "hyprland", "uwsm", "quickshell", "sddm",
        "xdg-desktop-portal-hyprland", "xdg-desktop-portal-gtk", "networkmanager", "pipewire", "pipewire-pulse", "wireplumber",
        // What restoring relies on.
        "yay", "git"};
    return protectedNames.contains(name) || name.startsWith(QLatin1String("linux-firmware-"));
}

bool PackageProvider::isSystemPackage(const QString &name) {
    if (isProtected(name))
        return true;
    static const QRegularExpression system(QStringLiteral(
        "^(linux(-[a-z0-9]+)*|.*-firmware(-.*)?|.*-ucode|nvidia.*|mesa.*|lib32-.*|vulkan-.*|xf86-.*|.*-dkms|broadcom-wl.*|sof-.*|"
        "libva-.*|.*-drivers?(-.*)?|alsa-firmware)$"));
    return system.match(name).hasMatch();
}

bool PackageProvider::pacmanBusy() const {
    return QFileInfo::exists(m_runner->env().sys("/var/lib/pacman/db.lck"));
}

int PackageProvider::install(const QStringList &names, QString *error) {
    // --needed leaves what is already there alone. No --noconfirm: pacman shows the whole
    // transaction, replacements and conflicts included, and asks before it changes anything.
    QStringList argv{"sudo", "pacman", "-S", "--needed", "--"};
    argv << names;
    return m_runner->runAttached(argv, error, true);
}

int PackageProvider::markExplicit(const QStringList &names, QString *error) {
    QStringList argv{"sudo", "pacman", "-D", "--asexplicit", "--"};
    argv << names;
    return m_runner->runAttached(argv, error, true);
}

int PackageProvider::buildFromAur(const QString &name, QString *error) {
    // Never as root, which makepkg refuses anyway: yay asks sudo itself when it installs.
    return m_runner->runAttached({"yay", "-S", "--needed", "--aur", "--diffmenu", "--noanswerdiff", "--", name}, error, true);
}

int PackageProvider::remove(const QStringList &names, QString *error) {
    // Exactly these packages: no -s for their dependencies, no -c for what depends on them,
    // and no -n, so pacman keeps any configuration it would otherwise delete.
    QStringList argv{"sudo", "pacman", "-R", "--"};
    argv << names;
    return m_runner->runAttached(argv, error, true);
}
