#include "updater.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSysInfo>
#include <QVersionNumber>

#include "helperlauncher.h"

namespace {

const char LatestRelease[] = "https://api.github.com/repos/last-refuge/omarchy-control/releases/latest";
const char DownloadPrefix[] = "https://github.com/last-refuge/omarchy-control/releases/download/";

QStringList secureCurl() {
    // HTTPS only, including wherever GitHub redirects a download.
    return {"curl", "-sSL", "--proto", "=https", "--proto-redir", "=https", "--connect-timeout", "15"};
}

}  // namespace

namespace Updater {

QStringList checkCommand() {
    QStringList argv = secureCurl();
    argv << "--max-time" << "30"
         << "-H" << "Accept: application/vnd.github+json"
         << "-A" << "omarchy-control/" + currentVersion()
         << "-w" << "\n%{http_code}"
         << QString::fromLatin1(LatestRelease);
    return argv;
}

Check interpret(const CommandResult &result, const QString &architecture) {
    // Qt calls 64-bit ARM "arm64"; Arch names its packages "aarch64".
    const QString packageArchitecture = architecture == "arm64" ? QStringLiteral("aarch64") : architecture;
    Check check;
    if (result.failure == CommandResult::NotFound) {
        check.error = QCoreApplication::translate("Updater", "%1 is not installed.").arg("curl");
        return check;
    }
    if (!result.ok()) {
        check.error = QCoreApplication::translate("Updater", "Could not reach GitHub. Check your connection and try again.");
        return check;
    }
    const int split = result.out.lastIndexOf('\n');
    const int status = split < 0 ? 0 : result.out.mid(split + 1).trimmed().toInt();
    if (status == 404) {
        check.outcome = Check::NoReleases;
        return check;
    }
    if (status == 403 || status == 429) {
        check.error = QCoreApplication::translate("Updater", "GitHub is limiting requests from this network. Try again later.");
        return check;
    }
    if (status != 200) {
        check.error = QCoreApplication::translate("Updater", "GitHub answered with an error (%1). Try again later.").arg(status);
        return check;
    }
    const QJsonObject json = QJsonDocument::fromJson(result.out.left(split)).object();
    Release release;
    release.tag = json.value("tag_name").toString();
    release.version = release.tag.startsWith('v') ? release.tag.mid(1) : release.tag;
    if (!isNewer(release.version, QStringLiteral("0"))) {
        check.error = QCoreApplication::translate("Updater", "GitHub returned a release this version cannot read.");
        return check;
    }
    release.notes = plainNotes(json.value("body").toString());
    release.pageUrl = json.value("html_url").toString();

    // Only the expected file, from this project's own releases: the response decides
    // what gets downloaded, so it does not get to name anything else.
    const QRegularExpression packageName(QStringLiteral("^omarchy-control-%1-[0-9]+-%2\\.pkg\\.tar\\.zst$")
                                             .arg(QRegularExpression::escape(release.version), QRegularExpression::escape(packageArchitecture)));
    for (const auto &value : json.value("assets").toArray()) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value("name").toString();
        const QString url = asset.value("browser_download_url").toString();
        if (!url.startsWith(QLatin1String(DownloadPrefix)))
            continue;
        if (name == QLatin1String("SHA256SUMS")) {
            release.sumsUrl = url;
        } else if (packageName.match(name).hasMatch()) {
            release.packageName = name;
            release.packageUrl = url;
        }
    }
    check.outcome = Check::Found;
    check.release = release;
    return check;
}

Check fetchBlocking(CommandRunner &runner) {
    CommandOptions options;
    options.timeoutMs = 35000;
    return interpret(runner.runBlocking(checkCommand(), options), QSysInfo::currentCpuArchitecture());
}

bool isNewer(const QString &candidate, const QString &current) {
    static const QRegularExpression dotted("^[0-9]+(\\.[0-9]+)*$");
    if (!dotted.match(candidate).hasMatch() || !dotted.match(current).hasMatch())
        return false;
    return QVersionNumber::fromString(candidate) > QVersionNumber::fromString(current);
}

QString currentVersion() {
    return QCoreApplication::applicationVersion();
}

QString versionFromPacman(const QByteArray &output) {
    static const QRegularExpression line("^omarchy-control ([^-\\s]+)-[0-9.]+\\s*$");
    return line.match(QString::fromUtf8(output)).captured(1);
}

QString plainNotes(const QString &markdown) {
    static const QRegularExpression heading("^#+\\s*");
    static const QRegularExpression bullet("^[*-]\\s+");
    static const QRegularExpression attribution("\\s+by @\\S+( in https?://\\S+)?$");
    QStringList lines;
    for (QString line : markdown.split('\n')) {
        line = line.trimmed();
        if (line.startsWith(QLatin1String("**Full Changelog**")))
            continue;
        line.remove(heading).remove(attribution).replace(bullet, QStringLiteral("• "));
        if (!line.isEmpty() || (!lines.isEmpty() && !lines.last().isEmpty()))
            lines << line;
    }
    return lines.join('\n').trimmed();
}

QString checksumFor(const QByteArray &sums, const QString &fileName) {
    static const QRegularExpression entry("^([0-9a-fA-F]{64}) [ *](.+)$");
    for (const QString &row : QString::fromUtf8(sums).split('\n')) {
        const auto match = entry.match(row.trimmed());
        if (match.hasMatch() && match.captured(2) == fileName)
            return match.captured(1).toLower();
    }
    return {};
}

QStringList syncCommand() {
    return {"env", "LC_ALL=C", "pacman", "-Si", "omarchy-control"};
}

QStringList installedCommand() {
    return {"env", "LC_ALL=C", "pacman", "-Qi", "omarchy-control"};
}

// The fields of pacman's first entry, by name.
static QHash<QString, QString> pacmanFields(const QByteArray &output) {
    QHash<QString, QString> fields;
    for (const QString &line : QString::fromUtf8(output).split('\n')) {
        if (line.trimmed().isEmpty() && !fields.isEmpty())
            break;
        const qsizetype colon = line.indexOf(QLatin1String(" : "));
        if (colon > 0)
            fields.insert(line.left(colon).trimmed(), line.mid(colon + 3).trimmed());
    }
    return fields;
}

Source sourceFrom(const QByteArray &sync, const QByteArray &installed) {
    Source source;
    const auto offered = pacmanFields(sync);
    if (offered.value("Name") == QLatin1String("omarchy-control") && !offered.value("Repository").isEmpty()) {
        source.repository = offered.value("Repository");
        // "1:0.3.0-1" to "0.3.0", as installed versions are compared.
        source.version = offered.value("Version").section('-', 0, 0).section(':', -1);
    }
    // A package from GitHub is unsigned; one a repository served carries its signature.
    const auto copy = pacmanFields(installed);
    source.installedSigned = copy.value("Name") == QLatin1String("omarchy-control") && copy.value("Validated By").contains(QLatin1String("Signature"));
    return source;
}

Source sourceBlocking(CommandRunner &runner) {
    CommandOptions options;
    options.timeoutMs = 5000;
    const CommandResult sync = runner.runBlocking(syncCommand(), options);
    const CommandResult installed = runner.runBlocking(installedCommand(), options);
    return sourceFrom(sync.ok() ? sync.out : QByteArray(), installed.ok() ? installed.out : QByteArray());
}

bool install(CommandRunner &runner, const Release &release, bool assumeYes, QString *error,
             const std::function<void(const QString &)> &progress) {
    const auto fail = [&](const QString &message) {
        *error = message;
        return false;
    };
    const auto say = [&](const QString &message) {
        if (progress)
            progress(message);
    };
    if (!release.installable())
        return fail(QCoreApplication::translate("Updater", "This release has no package for %1.").arg(QSysInfo::currentCpuArchitecture()));
    const Env &env = runner.env();
    if (QFileInfo::exists(env.sys("/var/lib/pacman/db.lck")))
        return fail(QCoreApplication::translate("Updater", "Another package operation is running. Try again when it finishes."));

    const QString directory = env.appCacheDir + "/updates";
    QDir(directory).removeRecursively();
    if (!QDir().mkpath(directory))
        return fail(QCoreApplication::translate("Updater", "Could not create %1.").arg(directory));
    QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QString package = directory + "/" + release.packageName;

    say(QCoreApplication::translate("Updater", "Downloading %1").arg(release.packageName));
    CommandOptions options;
    options.timeoutMs = 300000;
    options.mutation = true;
    QStringList fetchSums = secureCurl();
    fetchSums << "--fail" << "--max-time" << "60" << release.sumsUrl;
    const CommandResult sums = runner.runBlocking(fetchSums, options);
    QStringList fetchPackage = secureCurl();
    fetchPackage << "--fail" << "--max-time" << "280" << "-o" << package << release.packageUrl;
    if (!sums.ok() || !runner.runBlocking(fetchPackage, options).ok()) {
        QDir(directory).removeRecursively();
        return fail(QCoreApplication::translate("Updater", "The download failed. Check your connection and try again."));
    }

    // Nothing reaches pacman, or root, until the bytes match what the release lists.
    const QString expected = checksumFor(sums.out, release.packageName);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QFile file(package);
    const bool readable = file.open(QIODevice::ReadOnly) && hash.addData(&file);
    file.close();
    if (expected.isEmpty() || !readable || QString::fromLatin1(hash.result().toHex()) != expected) {
        QDir(directory).removeRecursively();
        return fail(QCoreApplication::translate("Updater", "The download did not match its published checksum, so nothing was installed."));
    }
    say(QCoreApplication::translate("Updater", "Checksum verified. Installing with pacman."));

    QStringList command{"sudo", "pacman", "-U"};
    if (assumeYes)
        command << "--noconfirm";
    command << package;
    QString startError;
    const int code = runner.runAttached(command, &startError);
    QDir(directory).removeRecursively();
    if (code != 0)
        return fail(startError.isEmpty() ? QCoreApplication::translate("Updater", "pacman did not finish the update, so nothing changed.") : startError);
    return true;
}

}  // namespace Updater

UpdateProvider::UpdateProvider(CommandRunner *runner, QObject *parent) : QObject(parent), m_runner(runner) {}

QVariantMap UpdateProvider::build() const {
    return {{"qt", QString::fromLatin1(qVersion())}, {"architecture", QSysInfo::currentCpuArchitecture()}};
}

void UpdateProvider::setState(const QString &state) {
    m_state = state;
    emit changed();
}

void UpdateProvider::readSource() {
    m_runner->run(Updater::syncCommand(), CommandOptions{5000}, this, [this](const CommandResult &sync) {
        const QByteArray offered = sync.ok() ? sync.out : QByteArray();
        m_runner->run(Updater::installedCommand(), CommandOptions{5000}, this, [this, offered](const CommandResult &installed) {
            m_source = Updater::sourceFrom(offered, installed.ok() ? installed.out : QByteArray());
            if (m_source.fromRepository() && m_state != QLatin1String("restart"))
                setState(QStringLiteral("repository"));
            else
                emit changed();
        });
    });
}

void UpdateProvider::check() {
    if (m_state == QLatin1String("checking") || m_state == QLatin1String("restart"))
        return;
    // A repository's signed copy updates with the system. GitHub's is the other channel,
    // and installing it here would swap one for the other.
    if (m_source.fromRepository()) {
        readSource();
        return;
    }
    m_error.clear();
    setState(QStringLiteral("checking"));
    CommandOptions options;
    options.timeoutMs = 35000;
    // Marked as a change only so leaving this page cannot cancel it like a stale read.
    options.mutation = true;
    m_runner->run(Updater::checkCommand(), options, this, [this](const CommandResult &result) {
        const Updater::Check check = Updater::interpret(result, QSysInfo::currentCpuArchitecture());
        if (check.outcome == Updater::Check::NoReleases) {
            setState(QStringLiteral("none"));
        } else if (check.outcome == Updater::Check::Failed) {
            m_error = check.error;
            setState(QStringLiteral("error"));
        } else if (Updater::isNewer(check.release.version, currentVersion())) {
            m_release = check.release;
            m_latest = check.release.version;
            setState(QStringLiteral("available"));
        } else {
            setState(QStringLiteral("current"));
        }
    });
}

void UpdateProvider::install() {
    if (QFileInfo::exists(m_runner->env().sys("/var/lib/pacman/db.lck"))) {
        emit message(tr("Another package operation is running. Try again when it finishes."), true);
        return;
    }
    if (m_runner->env().findExecutable("omarchy-control").isEmpty()) {
        emit message(tr("%1 is not installed.").arg("omarchy-control"), true);
        return;
    }
    // Reviewed and static, like every other terminal route: the command works out what
    // to install for itself rather than being told through a shell string.
    const QStringList argv = HelperLauncher::resolve(
        QJsonObject{{"kind", "terminal"}, {"command", "omarchy-control update install"}}, m_runner->env(), false);
    QString launchError;
    if (!m_runner->launchDetached(argv, &launchError)) {
        emit message(launchError, true);
        return;
    }
    m_installStarted = true;
    emit changed();
}

void UpdateProvider::refreshInstalled() {
    m_runner->run({"pacman", "-Q", "omarchy-control"}, this, [this](const CommandResult &result) {
        if (!result.ok())
            return;
        const QString installed = Updater::versionFromPacman(result.out);
        if (!Updater::isNewer(installed, currentVersion()))
            return;
        m_latest = installed;
        m_installStarted = false;
        setState(QStringLiteral("restart"));
    });
}

bool UpdateProvider::restart() {
    // The pause lets this window close first; a second instance would only hand over to it.
    QString launchError;
    if (!m_runner->launchDetached({"bash", "-c", "sleep 1; exec omarchy-control open --page about"}, &launchError)) {
        emit message(launchError, true);
        return false;
    }
    return true;
}
