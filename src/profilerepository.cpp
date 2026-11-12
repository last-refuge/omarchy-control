#include "profilerepository.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimeZone>
#include <memory>

#include "fileops.h"
#include "profiledesktop.h"
#include "profilefiles.h"
#include "profilemodel.h"

namespace {

QString tr(const char *text) {
    return QCoreApplication::translate("ProfileRepository", text);
}

QString connectionPath(const Env &env) {
    return env.appConfigDir + "/profile-repository.json";
}

const QRegularExpression &commitPattern() {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{40}([0-9a-f]{24})?$"));
    return pattern;
}

// The only paths a version may hold. Anything else, however harmless it looks, means the
// repository holds something a profile never contains, and the version is not used.
bool allowedFile(const QString &path) {
    static const QRegularExpression asset(QStringLiteral("^assets/sha256/[0-9a-f]{64}$"));
    static const QRegularExpression plainName(QStringLiteral("^[A-Za-z0-9_+-][A-Za-z0-9._+-]{0,127}$"));
    if (path == QLatin1String("profile.json") || asset.match(path).hasMatch())
        return true;
    const QStringList parts = path.split('/');
    return parts.size() == 3 && parts[0] == QLatin1String("files") && ProfileModel::fileAdapters().contains(parts[1]) && plainName.match(parts[2]).hasMatch();
}

bool allowedFolder(const QString &path) {
    if (path == QLatin1String("assets") || path == QLatin1String("assets/sha256") || path == QLatin1String("files"))
        return true;
    return path.startsWith(QLatin1String("files/")) && ProfileModel::fileAdapters().contains(path.mid(6));
}

// The files of a profile folder that a version holds: the manifest, the assets it names, and
// its configuration files, each checked against the digest the manifest records.
QStringList profileFiles(const QString &directory, QString *error) {
    bool exists = false;
    const ProfileModel::Parsed parsed = ProfileModel::load(directory, &exists);
    if (!exists) {
        *error = tr("There is no profile here yet. Capture one first.");
        return {};
    }
    if (!parsed.ok) {
        *error = tr("The profile is not valid: %1").arg(parsed.errors.value(0));
        return {};
    }
    QStringList files{QStringLiteral("profile.json")};
    QStringList digests = parsed.profile.referencedAssets().values();
    digests.sort();
    for (const QString &sha256 : digests) {
        QByteArray bytes;
        if (!ProfileFiles::read(directory, parsed.profile.asset(sha256), &bytes, error))
            return {};
        files << "assets/sha256/" + sha256;
    }
    for (const QJsonValue &value : parsed.profile.files) {
        QByteArray contents;
        if (!ProfileFiles::readConfig(directory, value.toObject(), &contents, error))
            return {};
        files << "files/" + value.toObject().value("adapter").toString() + "/" + value.toObject().value("name").toString();
    }
    return files;
}

ProfileRepository::Version parseVersion(const QString &line) {
    // %H %T %ct %P %s, separated by unit separators.
    const QStringList fields = line.split(QChar(0x1f));
    ProfileRepository::Version version;
    if (fields.size() < 5 || !commitPattern().match(fields[0]).hasMatch())
        return version;
    version.commit = fields[0];
    version.tree = fields[1];
    version.time = fields[2].toLongLong();
    version.parents = fields[3].split(' ', Qt::SkipEmptyParts);
    version.message = fields.mid(4).join(QChar(0x1f));
    return version;
}

const char LogFormat[] = "--format=%H%x1f%T%x1f%ct%x1f%P%x1f%s";

// GitHub CLI answers over the network, so it gets longer than a local read.
CommandOptions gitHubOptions() {
    CommandOptions options;
    options.timeoutMs = 20000;
    return options;
}

bool removeProfileFiles(const QString &directory) {
    // Only what a profile holds, so anything else someone keeps there stays.
    bool ok = QFile::remove(directory + "/profile.json") || !QFileInfo::exists(directory + "/profile.json");
    for (const QString &folder : {QStringLiteral("assets"), QStringLiteral("files")})
        ok = QDir(directory + "/" + folder).removeRecursively() && ok;
    return ok;
}

bool copyProfile(const QString &from, const QString &to, QString *error) {
    QString ignored;
    const QStringList files = profileFiles(from, error);
    if (files.isEmpty())
        return false;
    removeProfileFiles(to);
    for (const QString &file : files) {
        QByteArray bytes;
        if (!FileOps::readFile(from + "/" + file, &bytes, error) || !FileOps::atomicWrite(to + "/" + file, bytes, error, 0644))
            return false;
    }
    return true;
}

}  // namespace

QJsonObject ProfileRepository::Connection::toJson() const {
    QJsonObject object{{"owner", owner}, {"name", name}, {"branch", branch}};
    if (!visibility.isEmpty())
        object["visibility"] = visibility;
    if (publicAllowed)
        object["publicAllowed"] = true;
    if (!account.isEmpty())
        object["account"] = account;
    if (!authorEmail.isEmpty())
        object["authorEmail"] = authorEmail;
    return object;
}

ProfileRepository::Connection ProfileRepository::Connection::fromJson(const QJsonObject &object) {
    Connection connection;
    connection.owner = object.value("owner").toString();
    connection.name = object.value("name").toString();
    connection.branch = object.value("branch").toString(QStringLiteral("main"));
    connection.visibility = object.value("visibility").toString();
    connection.publicAllowed = object.value("publicAllowed").toBool();
    connection.account = object.value("account").toString();
    connection.authorEmail = object.value("authorEmail").toString();
    return connection;
}

QJsonObject ProfileRepository::Version::toJson() const {
    return QJsonObject{{"commit", commit},
                       {"message", message},
                       {"time", QDateTime::fromSecsSinceEpoch(time, QTimeZone::UTC).toString(Qt::ISODate)},
                       {"parents", QJsonArray::fromStringList(parents)}};
}

QJsonObject ProfileRepository::Status::toJson() const {
    QJsonObject object{{"git", gitAvailable}, {"githubCli", ghAvailable}, {"relation", relation}, {"unsavedChanges", unsavedChanges}};
    if (!connection.isEmpty()) {
        QJsonObject remote = connection.toJson();
        remote["url"] = connection.webUrl();
        object["repository"] = remote;
    }
    if (!saved.isEmpty())
        object["saved"] = saved.toJson();
    if (!published.isEmpty())
        object["published"] = published.toJson();
    if (ahead)
        object["ahead"] = ahead;
    if (behind)
        object["behind"] = behind;
    if (!error.isEmpty())
        object["error"] = error;
    return object;
}

QJsonObject ProfileRepository::Result::toJson() const {
    QJsonObject object{{"ok", ok}};
    if (!error.isEmpty())
        object["error"] = error;
    if (!message.isEmpty())
        object["message"] = message;
    if (!commit.isEmpty())
        object["commit"] = commit;
    if (!directory.isEmpty())
        object["directory"] = directory;
    if (!problems.isEmpty())
        object["problems"] = QJsonArray::fromStringList(problems);
    return object;
}

ProfileRepository::ProfileRepository(CommandRunner *runner, const Env &env, QObject *parent) : QObject(parent), m_runner(runner), m_env(env) {}

QString ProfileRepository::workingDirectory() const {
    return m_env.dataHome + "/omarchy-control/profiles/my-omarchy";
}

QString ProfileRepository::repositoryPath() const {
    return m_env.dataHome + "/omarchy-control/profiles/my-omarchy.git";
}

bool ProfileRepository::gitAvailable() const {
    return !m_env.findExecutable(QStringLiteral("git")).isEmpty();
}

ProfileRepository::Connection ProfileRepository::connection() const {
    bool ok = false;
    const QJsonObject object = ProfileStore::readJson(connectionPath(m_env), &ok);
    return ok ? Connection::fromJson(object) : Connection();
}

bool ProfileRepository::saveConnection(const Connection &connection, QString *error) const {
    QDir().mkpath(m_env.appConfigDir);
    return FileOps::atomicWrite(connectionPath(m_env), QJsonDocument(connection.toJson()).toJson(), error, 0600);
}

bool ProfileRepository::parseGitHub(const QString &text, Connection *connection, QString *error) {
    // owner/name, with the forms GitHub shows for the same repository. Nothing that names a
    // user and password, another host, or another protocol.
    static const QRegularExpression form(QStringLiteral("^(?:(?:https://)?(?:www\\.)?github\\.com/)?([A-Za-z0-9](?:[A-Za-z0-9-]{0,38}))/([A-Za-z0-9._-]{1,100}?)(?:\\.git)?/?$"));
    const QRegularExpressionMatch match = form.match(text.trimmed());
    if (!match.hasMatch() || match.captured(2) == QLatin1String(".") || match.captured(2) == QLatin1String("..")) {
        if (error)
            *error = tr("%1 is not a GitHub repository. Give it as https://github.com/owner/name or owner/name.").arg(text.left(200));
        return false;
    }
    connection->owner = match.captured(1);
    connection->name = match.captured(2);
    return true;
}

QString ProfileRepository::remoteUrl(const Connection &connection) const {
    return m_env.gitHub + "/" + connection.fullName() + ".git";
}

QString ProfileRepository::author(const Connection &connection, QString *email) const {
    // A version carries no one's real address: the account's no-reply one once connected,
    // otherwise a placeholder.
    if (!connection.account.isEmpty() && !connection.authorEmail.isEmpty()) {
        *email = connection.authorEmail;
        return connection.account;
    }
    *email = QStringLiteral("my-omarchy@localhost");
    return QStringLiteral("My Omarchy");
}

QStringList ProfileRepository::gitArgv(const QStringList &arguments, const QString &indexFile) const {
    // None of the user's own configuration, so nothing in it can run a program, rewrite an
    // address, or change what is written. Nothing from a repository can either: no hooks, no
    // attributes, no filters, and objects are checked as they arrive.
    QString email;
    const QString name = author(connection(), &email);
    QStringList argv{"env",
                     "GIT_CONFIG_NOSYSTEM=1",
                     "GIT_CONFIG_GLOBAL=/dev/null",
                     "GIT_ATTR_NOSYSTEM=1",
                     "GIT_TERMINAL_PROMPT=0",
                     "GIT_ASKPASS=",
                     "SSH_ASKPASS=",
                     "GIT_NO_REPLACE_OBJECTS=1",
                     "GIT_OPTIONAL_LOCKS=0",
                     "GIT_AUTHOR_NAME=" + name,
                     "GIT_AUTHOR_EMAIL=" + email,
                     "GIT_COMMITTER_NAME=" + name,
                     "GIT_COMMITTER_EMAIL=" + email};
    if (!indexFile.isEmpty())
        argv << "GIT_INDEX_FILE=" + indexFile;
    argv << "git" << "-c" << "core.hooksPath=/dev/null" << "-c" << "core.fsmonitor=false" << "-c" << "core.attributesFile=/dev/null" << "-c"
         << "protocol.allow=never" << "-c" << "protocol.https.allow=always" << "-c" << "transfer.fsckObjects=true" << "-c" << "fetch.recurseSubmodules=false"
         << "-c" << "submodule.recurse=false" << "-c" << "gc.auto=0" << "-c" << "commit.gpgSign=false" << "-c" << "credential.helper=";
    if (m_env.gitHub.startsWith(QLatin1String("file://")))
        argv << "-c" << "protocol.file.allow=always";
    // Signing in stays with GitHub CLI, as `gh auth setup-git` would set it up, but only for
    // this repository's commands.
    const QString gh = m_env.findExecutable(QStringLiteral("gh"));
    if (!gh.isEmpty() && !gh.contains('"') && !gh.contains('\\') && !gh.contains('$') && !gh.contains('`'))
        argv << "-c" << "credential.https://github.com.helper=!\"" + gh + "\" auth git-credential";
    return argv + arguments;
}

void ProfileRepository::git(const QStringList &arguments, std::function<void(const CommandResult &)> done, const QByteArray &input, int timeoutMs, bool mutation,
                            const QString &indexFile) {
    CommandOptions options;
    options.timeoutMs = timeoutMs;
    options.input = input;
    options.mutation = mutation;
    // A version is at most 100 MiB, read back in one batch.
    options.maxOutputBytes = ProfileModel::MaxProfileBytes + 16 * 1024 * 1024;
    m_runner->run(gitArgv(QStringList{"--git-dir=" + repositoryPath()} + arguments, indexFile), options, this, std::move(done));
}

void ProfileRepository::ensureRepository(Done done) {
    if (!gitAvailable()) {
        done(Result::failed(tr("Saving versions needs git, which is not installed.")));
        return;
    }
    if (QFileInfo(repositoryPath() + "/HEAD").isFile()) {
        done(Result::succeeded());
        return;
    }
    QDir().mkpath(QFileInfo(repositoryPath()).absolutePath());
    QFile::setPermissions(QFileInfo(repositoryPath()).absolutePath(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    CommandOptions options;
    options.mutation = true;
    // No template, so no sample hooks or anything else is copied in.
    m_runner->run(gitArgv({"init", "--bare", "--template=", "--quiet", repositoryPath()}), options, this, [this, done](const CommandResult &result) {
        if (!result.ok()) {
            done(Result::failed(tr("The profile's history could not be started: %1").arg(result.describe(QStringLiteral("git")))));
            return;
        }
        git({"symbolic-ref", "HEAD", "refs/heads/" + connection().branch}, [done](const CommandResult &) { done(Result::succeeded()); }, {}, 15000, true);
    });
}

void ProfileRepository::resolveRef(const QString &ref, std::function<void(const Version &)> done) {
    if (!QFileInfo(repositoryPath() + "/HEAD").isFile()) {
        done(Version());
        return;
    }
    git({"log", "-1", LogFormat, ref, "--"}, [done](const CommandResult &result) {
        done(result.ok() ? parseVersion(result.outText()) : Version());
    });
}

void ProfileRepository::status(StatusDone done) {
    auto status = std::make_shared<Status>();
    status->gitAvailable = gitAvailable();
    status->ghAvailable = !m_env.findExecutable(QStringLiteral("gh")).isEmpty();
    status->connection = connection();
    const bool working = QFileInfo::exists(ProfileModel::manifestPath(workingDirectory()));
    if (!status->gitAvailable) {
        status->relation = working ? QStringLiteral("unsaved-only") : QStringLiteral("nothing");
        status->unsavedChanges = working;
        done(*status);
        return;
    }
    const QString branch = status->connection.branch;
    resolveRef("refs/heads/" + branch, [this, status, branch, working, done](const Version &saved) {
        status->saved = saved;
        resolveRef("refs/remotes/origin/" + branch, [this, status, working, done](const Version &published) {
            status->published = published;
            const auto finish = [this, status, working, done] {
                // Unsaved changes: the working profile's files against the newest version's.
                if (!working) {
                    done(*status);
                    return;
                }
                if (status->saved.isEmpty()) {
                    status->unsavedChanges = true;
                    done(*status);
                    return;
                }
                QString error;
                const QStringList files = profileFiles(workingDirectory(), &error);
                if (files.isEmpty()) {
                    status->unsavedChanges = true;
                    status->error = error;
                    done(*status);
                    return;
                }
                QByteArray paths;
                for (const QString &file : files)
                    paths += (workingDirectory() + "/" + file).toUtf8() + "\n";
                git({"hash-object", "--no-filters", "--stdin-paths"}, [this, status, files, done](const CommandResult &hashed) {
                    const QStringList shas = hashed.outText().split('\n', Qt::SkipEmptyParts);
                    git({"ls-tree", "-r", "-z", "--full-tree", status->saved.commit}, [status, files, shas, done](const CommandResult &tree) {
                        QHash<QString, QString> savedFiles;
                        for (const QByteArray &entry : tree.out.split('\0')) {
                            const int tab = entry.indexOf('\t');
                            if (tab > 0)
                                savedFiles.insert(QString::fromUtf8(entry.mid(tab + 1)), QString::fromUtf8(entry.left(tab)).section(' ', 2, 2));
                        }
                        bool differs = shas.size() != files.size() || savedFiles.size() != files.size();
                        for (int i = 0; !differs && i < files.size(); ++i)
                            differs = savedFiles.value(files[i]) != shas[i];
                        status->unsavedChanges = differs;
                        done(*status);
                    });
                }, paths);
            };
            if (status->saved.isEmpty() && published.isEmpty()) {
                status->relation = working ? QStringLiteral("unsaved-only") : QStringLiteral("nothing");
                finish();
            } else if (status->saved.isEmpty()) {
                status->relation = QStringLiteral("published-only");
                finish();
            } else if (published.isEmpty()) {
                status->relation = status->connection.isEmpty() ? QStringLiteral("saved") : QStringLiteral("ahead");
                git({"rev-list", "--count", status->saved.commit}, [status, finish](const CommandResult &count) {
                    status->ahead = status->connection.isEmpty() ? 0 : count.outText().toInt();
                    finish();
                });
            } else if (status->saved.commit == published.commit) {
                status->relation = QStringLiteral("same");
                finish();
            } else {
                git({"rev-list", "--left-right", "--count", status->saved.commit + "..." + published.commit}, [status, finish](const CommandResult &count) {
                    status->ahead = count.outText().section('\t', 0, 0).toInt();
                    status->behind = count.outText().section('\t', 1, 1).toInt();
                    status->relation = status->ahead > 0 && status->behind > 0 ? QStringLiteral("diverged")
                                       : status->ahead > 0                     ? QStringLiteral("ahead")
                                                                               : QStringLiteral("behind");
                    finish();
                });
            }
        });
    });
}

void ProfileRepository::writeVersion(const QString &directory, const QString &parent, const QString &message, Done done) {
    QString error;
    const QStringList files = profileFiles(directory, &error);
    if (files.isEmpty()) {
        done(Result::failed(error));
        return;
    }
    QByteArray paths;
    for (const QString &file : files)
        paths += (directory + "/" + file).toUtf8() + "\n";
    // The version is built from these files alone, in an index of its own: nothing is checked
    // out, and nothing runs while it is made.
    git({"hash-object", "-w", "--no-filters", "--stdin-paths"}, [this, files, parent, message, done](const CommandResult &hashed) {
        const QStringList shas = hashed.outText().split('\n', Qt::SkipEmptyParts);
        if (!hashed.ok() || shas.size() != files.size()) {
            done(Result::failed(tr("The profile's files could not be stored: %1").arg(hashed.describe(QStringLiteral("git")))));
            return;
        }
        auto scratch = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/omarchy-control-index-XXXXXX");
        const QString index = scratch->path() + "/index";
        QByteArray entries;
        for (int i = 0; i < files.size(); ++i)
            entries += "100644 " + shas[i].toLatin1() + "\t" + files[i].toUtf8() + "\n";
        git({"update-index", "--add", "--index-info"}, [this, scratch, index, parent, message, done](const CommandResult &indexed) {
            if (!indexed.ok()) {
                done(Result::failed(tr("The version could not be put together: %1").arg(indexed.describe(QStringLiteral("git")))));
                return;
            }
            git({"write-tree"}, [this, scratch, parent, message, done](const CommandResult &written) {
                const QString tree = written.outText();
                if (!written.ok() || tree.isEmpty()) {
                    done(Result::failed(tr("The version could not be put together: %1").arg(written.describe(QStringLiteral("git")))));
                    return;
                }
                const auto commit = [this, tree, parent, message, done](const QString &parentTree) {
                    if (!parentTree.isEmpty() && parentTree == tree) {
                        Result same = Result::succeeded();
                        same.commit = parent;
                        same.message = tr("Nothing changed since the last version, so nothing new was saved.");
                        done(same);
                        return;
                    }
                    QStringList arguments{"commit-tree", tree};
                    for (const QString &each : parent.split(' ', Qt::SkipEmptyParts))
                        arguments << "-p" << each;
                    arguments << "-m" << message;
                    git(arguments, [done](const CommandResult &made) {
                        Result result;
                        result.ok = made.ok() && commitPattern().match(made.outText()).hasMatch();
                        if (result.ok)
                            result.commit = made.outText();
                        else
                            result.error = tr("The version could not be saved: %1").arg(made.describe(QStringLiteral("git")));
                        done(result);
                    }, {}, 15000, true);
                };
                // Only the first parent counts for "nothing changed": a version that keeps one
                // side of two is new even when its files match one of them.
                if (parent.isEmpty() || parent.contains(' '))
                    commit(QString());
                else
                    resolveRef(parent, [commit](const Version &previous) { commit(previous.tree); });
            }, {}, 15000, true, index);
        }, entries, 15000, true, index);
    }, paths, 60000, true);
}

void ProfileRepository::save(const QString &message, Done done) {
    ensureRepository([this, message, done](const Result &ready) {
        if (!ready.ok) {
            done(ready);
            return;
        }
        const QString ref = "refs/heads/" + connection().branch;
        resolveRef(ref, [this, ref, message, done](const Version &saved) {
            const QString text = message.trimmed().isEmpty() ? tr("Saved with Omarchy Control") : message.trimmed().left(500);
            writeVersion(workingDirectory(), saved.commit, text, [this, ref, saved, done](const Result &written) {
                if (!written.ok || written.commit == saved.commit) {
                    done(written);
                    return;
                }
                // Moved only from what was read, so two saves at once cannot lose either.
                git({"update-ref", "-m", "save", ref, written.commit, saved.commit}, [written, done](const CommandResult &moved) {
                    Result result = written;
                    result.ok = moved.ok();
                    if (result.ok)
                        result.message = tr("Saved version %1 on this computer.").arg(written.commit.left(7));
                    else
                        result.error = tr("Another save happened at the same time. Try again: %1").arg(moved.describe(QStringLiteral("git")));
                    done(result);
                }, {}, 15000, true);
            });
        });
    });
}

void ProfileRepository::history(bool published, int limit, std::function<void(const QList<Version> &, const QString &)> done) {
    if (!QFileInfo(repositoryPath() + "/HEAD").isFile()) {
        done({}, QString());
        return;
    }
    const QString ref = (published ? "refs/remotes/origin/" : "refs/heads/") + connection().branch;
    git({"log", "-n", QString::number(qBound(1, limit, 1000)), LogFormat, ref, "--"}, [done](const CommandResult &result) {
        QList<Version> versions;
        for (const QString &line : result.outText().split('\n', Qt::SkipEmptyParts)) {
            const Version version = parseVersion(line);
            if (!version.isEmpty())
                versions << version;
        }
        // A branch with nothing on it yet is no history, not an error.
        done(versions, QString());
    });
}

void ProfileRepository::read(const QString &revision, Done done) {
    if (!gitAvailable() || !QFileInfo(repositoryPath() + "/HEAD").isFile()) {
        done(Result::failed(gitAvailable() ? tr("Nothing has been saved or fetched yet.") : tr("Reading versions needs git, which is not installed.")));
        return;
    }
    static const QRegularExpression shortCommit(QStringLiteral("^[0-9a-f]{7,64}$"));
    QString ref;
    if (revision == QLatin1String("saved"))
        ref = "refs/heads/" + connection().branch;
    else if (revision == QLatin1String("published"))
        ref = "refs/remotes/origin/" + connection().branch;
    else if (shortCommit.match(revision).hasMatch())
        ref = revision;
    else {
        done(Result::failed(tr("%1 is not a version. Name one from the history, or saved, or published.").arg(revision.left(80))));
        return;
    }
    git({"rev-parse", "--verify", "--quiet", "--end-of-options", ref + "^{commit}"}, [this, revision, done](const CommandResult &resolved) {
        const QString commit = resolved.outText();
        if (!resolved.ok() || !commitPattern().match(commit).hasMatch()) {
            done(Result::failed(revision == QLatin1String("published") ? tr("Nothing has been fetched from GitHub yet.")
                               : revision == QLatin1String("saved")   ? tr("Nothing has been saved on this computer yet.")
                                                                      : tr("There is no version %1 here.").arg(revision)));
            return;
        }
        git({"ls-tree", "-r", "-t", "-z", "-l", "--full-tree", commit}, [this, commit, done](const CommandResult &listed) {
            if (!listed.ok()) {
                done(Result::failed(tr("Version %1 could not be read: %2").arg(commit.left(7), listed.describe(QStringLiteral("git")))));
                return;
            }
            // Everything is checked before anything is read: every path, kind, and size.
            QStringList problems, paths, blobs;
            qint64 total = 0;
            for (const QByteArray &record : listed.out.split('\0')) {
                const int tab = record.indexOf('\t');
                if (tab < 0)
                    continue;
                const QString path = QString::fromUtf8(record.mid(tab + 1));
                const QStringList fields = QString::fromLatin1(record.left(tab)).split(' ', Qt::SkipEmptyParts);
                const QString mode = fields.value(0), type = fields.value(1), object = fields.value(2);
                const qint64 size = fields.value(3).toLongLong();
                if (type == QLatin1String("tree")) {
                    if (!allowedFolder(path))
                        problems << tr("It holds a folder %1, which a profile never contains.").arg(path.left(200));
                    continue;
                }
                if (type == QLatin1String("commit")) {
                    problems << tr("It holds a submodule, %1, which a profile never contains.").arg(path.left(200));
                    continue;
                }
                if (!allowedFile(path)) {
                    problems << tr("It holds %1, which a profile never contains.").arg(path.left(200));
                    continue;
                }
                if (mode != QLatin1String("100644")) {
                    problems << (mode == QLatin1String("120000") ? tr("%1 is a link, which a profile never contains.").arg(path)
                                                                 : tr("%1 is marked as a program, which a profile never contains.").arg(path));
                    continue;
                }
                const qint64 limit = path == QLatin1String("profile.json") ? ProfileModel::MaxManifestBytes : ProfileModel::MaxFileBytes;
                if (size > limit) {
                    problems << tr("%1 is larger than a profile allows.").arg(path);
                    continue;
                }
                total += size;
                paths << path;
                blobs << object;
            }
            if (!paths.contains(QStringLiteral("profile.json")))
                problems << tr("It has no profile.json.");
            if (total > ProfileModel::MaxProfileBytes)
                problems << tr("Its files come to more than the 100 MiB a profile allows.");
            if (!problems.isEmpty()) {
                Result refused = Result::failed(tr("Version %1 cannot be used: it is not only a profile.").arg(commit.left(7)));
                refused.problems = problems;
                refused.commit = commit;
                done(refused);
                return;
            }
            git({"cat-file", "--batch"}, [this, commit, paths, blobs, done](const CommandResult &contents) {
                if (!contents.ok() || contents.truncated) {
                    done(Result::failed(tr("Version %1 could not be read: %2").arg(commit.left(7), contents.describe(QStringLiteral("git")))));
                    return;
                }
                // Written to a folder of its own, private to this computer, and checked the
                // way any profile is before it can be planned.
                const QString root = ProfileStore::root(m_env) + "/revisions";
                const QString directory = root + "/" + commit;
                QDir(directory).removeRecursively();
                QString error;
                int at = 0;
                for (int i = 0; i < paths.size(); ++i) {
                    const int newline = contents.out.indexOf('\n', at);
                    const QList<QByteArray> header = contents.out.mid(at, newline - at).split(' ');
                    const qint64 size = header.value(2).toLongLong();
                    if (newline < 0 || header.value(0) != blobs[i].toLatin1() || header.value(1) != "blob" || newline + 1 + size > contents.out.size()) {
                        QDir(directory).removeRecursively();
                        done(Result::failed(tr("Version %1 could not be read whole.").arg(commit.left(7))));
                        return;
                    }
                    if (!FileOps::atomicWrite(directory + "/" + paths[i], contents.out.mid(newline + 1, size), &error, 0600)) {
                        QDir(directory).removeRecursively();
                        done(Result::failed(error));
                        return;
                    }
                    at = newline + 1 + size + 1;
                }
                QFile::setPermissions(root, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
                profileFiles(directory, &error);
                if (!error.isEmpty()) {
                    QDir(directory).removeRecursively();
                    Result refused = Result::failed(tr("Version %1 cannot be used: %2").arg(commit.left(7), error));
                    refused.commit = commit;
                    refused.problems << error;
                    done(refused);
                    return;
                }
                Result result = Result::succeeded();
                result.commit = commit;
                result.directory = directory;
                done(result);
            }, blobs.join('\n').toLatin1() + "\n", 120000);
        });
    });
}

void ProfileRepository::prepareWorking(Done done) {
    if (QFileInfo::exists(ProfileModel::manifestPath(workingDirectory())) || !gitAvailable() || !QFileInfo(repositoryPath() + "/HEAD").isFile()) {
        done(Result::succeeded());
        return;
    }
    const QString branch = connection().branch;
    resolveRef("refs/heads/" + branch, [this, branch, done](const Version &saved) {
        resolveRef("refs/remotes/origin/" + branch, [this, branch, saved, done](const Version &published) {
            if (saved.isEmpty() && published.isEmpty()) {
                done(Result::succeeded());
                return;
            }
            read(saved.isEmpty() ? QStringLiteral("published") : QStringLiteral("saved"), [this, branch, saved, published, done](const Result &version) {
                if (!version.ok) {
                    done(version);
                    return;
                }
                QString error;
                if (!copyProfile(version.directory, workingDirectory(), &error)) {
                    done(Result::failed(error));
                    return;
                }
                if (!saved.isEmpty()) {
                    done(Result::succeeded(tr("Started from the newest version saved here.")));
                    return;
                }
                // A computer with nothing saved yet takes up the published history, so its
                // first save adds to it.
                git({"update-ref", "-m", "start from published", "refs/heads/" + branch, published.commit, ""}, [published, done](const CommandResult &moved) {
                    done(moved.ok() ? Result::succeeded(QCoreApplication::translate("ProfileRepository", "Started from the published version %1.").arg(published.commit.left(7)))
                                    : Result::failed(moved.describe(QStringLiteral("git"))));
                }, {}, 15000, true);
            });
        });
    });
}

QJsonArray ProfileRepository::differences(const Profile &before, const Profile &after) {
    QJsonArray rows;
    const auto row = [&rows](const QString &kind, const QString &id, const QString &was, const QString &now) {
        if (was == now)
            return;
        QJsonObject change{{"kind", kind}, {"id", id}, {"change", was.isEmpty() ? "added" : now.isEmpty() ? "removed" : "changed"}};
        if (!was.isEmpty())
            change["before"] = was;
        if (!now.isEmpty())
            change["after"] = now;
        rows.append(change);
    };
    const auto setting = [](const ProfileSetting &value) {
        return value.mode.isEmpty() ? QString() : value.mode == QLatin1String("inherit") ? QStringLiteral("this computer's default") : value.value;
    };
    QStringList ids = before.settings.keys() + after.settings.keys();
    ids.removeDuplicates();
    ids.sort();
    for (const QString &id : ids)
        row(QStringLiteral("setting"), id, setting(before.settings.value(id)), setting(after.settings.value(id)));
    QStringList names;
    for (const ProfilePackage &package : before.packageList() + after.packageList())
        names << package.name;
    names.removeDuplicates();
    names.sort();
    const auto app = [](const ProfilePackage &package) {
        return package.name.isEmpty() ? QString() : package.intent == QLatin1String("absent") ? QStringLiteral("unwanted") : QStringLiteral("wanted, from %1").arg(package.source);
    };
    for (const QString &name : names)
        row(QStringLiteral("app"), name, app(before.package(name)), app(after.package(name)));
    QStringList targets = ProfileDesktop::targets(before.desktop) + ProfileDesktop::targets(after.desktop) + ProfileDesktop::fileTargets(before)
                          + ProfileDesktop::fileTargets(after);
    targets.removeDuplicates();
    for (const QString &target : targets) {
        // Compared as saved, and shown as summaries: two values can read alike and differ.
        const QString was = ProfileDesktop::desiredIn(before, target), now = ProfileDesktop::desiredIn(after, target);
        if (was == now)
            continue;
        QJsonObject change{{"kind", target.startsWith(QLatin1String("file:")) ? "file" : "desktop"},
                           {"id", target},
                           {"change", was.isEmpty() ? "added" : now.isEmpty() ? "removed" : "changed"}};
        if (!was.isEmpty())
            change["before"] = ProfileDesktop::summary(target, was);
        if (!now.isEmpty())
            change["after"] = ProfileDesktop::summary(target, now);
        rows.append(change);
    }
    if (before.name != after.name)
        row(QStringLiteral("name"), QStringLiteral("name"), before.name, after.name);
    return rows;
}

void ProfileRepository::connectTo(const QString &repository, const QString &branch, bool allowPublic, Done done) {
    Connection connection;
    QString error;
    if (!parseGitHub(repository, &connection, &error)) {
        done(Result::failed(error));
        return;
    }
    static const QRegularExpression branchPattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$"));
    connection.branch = branch.isEmpty() ? QStringLiteral("main") : branch;
    if (!branchPattern.match(connection.branch).hasMatch() || connection.branch.contains(QLatin1String("..")) || connection.branch.endsWith(QLatin1String(".lock"))) {
        done(Result::failed(tr("%1 is not a branch name this app uses.").arg(connection.branch.left(100))));
        return;
    }
    connection.publicAllowed = allowPublic;
    const auto finish = [this, done](const Connection &connection) {
        if (connection.visibility == QLatin1String("public") && !connection.publicAllowed) {
            done(Result::failed(tr("%1 is public, so anyone could read your profile there. Connect with --allow-public if that is what you want.").arg(connection.fullName())));
            return;
        }
        QString error;
        const QString previous = this->connection().branch;
        if (!saveConnection(connection, &error)) {
            done(Result::failed(error));
            return;
        }
        const auto report = [this, connection, done] {
            const QString shown = connection.visibility.isEmpty() ? tr("its visibility could not be checked") : connection.visibility;
            QString message = tr("Connected to %1 (%2), branch %3. Nothing is sent until you publish.").arg(connection.fullName(), shown, connection.branch);
            if (m_env.findExecutable(QStringLiteral("gh")).isEmpty())
                message += " " + tr("GitHub CLI is not installed, so publishing cannot sign in. Install github-cli, then run gh auth login.");
            else if (connection.account.isEmpty())
                message += " " + tr("GitHub CLI is not signed in. Run gh auth login before publishing.");
            done(Result::succeeded(message));
        };
        // Versions saved before connecting are on the branch used until now. A new branch with
        // nothing on it starts from them, so they are not left behind.
        if (previous == connection.branch || !QFileInfo(repositoryPath() + "/HEAD").isFile()) {
            report();
            return;
        }
        git({"rev-parse", "--verify", "--quiet", "refs/heads/" + previous}, [this, connection, report](const CommandResult &old) {
            if (!old.ok() || !commitPattern().match(old.outText()).hasMatch()) {
                report();
                return;
            }
            git({"update-ref", "refs/heads/" + connection.branch, old.outText(), ""}, [report](const CommandResult &) { report(); }, {}, 15000, true);
        });
    };
    if (m_env.findExecutable(QStringLiteral("gh")).isEmpty()) {
        finish(connection);
        return;
    }
    // Who is signed in, for versions' no-reply address, and whether the repository is private.
    m_runner->run({"gh", "api", "user", "--jq", ".login + \" \" + (.id | tostring)"}, gitHubOptions(), this, [this, connection, finish, done](const CommandResult &user) mutable {
        static const QRegularExpression login(QStringLiteral("^([A-Za-z0-9-]{1,39}) ([0-9]{1,20})$"));
        const QRegularExpressionMatch who = login.match(user.outText());
        if (who.hasMatch()) {
            connection.account = who.captured(1);
            connection.authorEmail = who.captured(2) + "+" + who.captured(1) + "@users.noreply.github.com";
        }
        m_runner->run({"gh", "repo", "view", connection.fullName(), "--json", "visibility"}, gitHubOptions(), this, [connection, finish, done](const CommandResult &view) mutable {
            const QString visibility = QJsonDocument::fromJson(view.out).object().value("visibility").toString().toLower();
            if (view.ok() && !visibility.isEmpty()) {
                connection.visibility = visibility;
            } else if (!connection.account.isEmpty()) {
                // Signed in, and still not found: it does not exist, or this account cannot see it.
                const QString detail = QString::fromUtf8(view.err);
                const bool missing = detail.contains(QLatin1String("Could not resolve")) || detail.contains(QLatin1String("Not Found"));
                done(Result::failed(missing ? tr("GitHub has no repository %1 that %2 can see.").arg(connection.fullName(), connection.account)
                                            : tr("GitHub could not be asked about %1: %2").arg(connection.fullName(), view.describe(QStringLiteral("gh")))));
                return;
            }
            finish(connection);
        });
    });
}

void ProfileRepository::create(const QString &name, Done done) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9._-]{1,100}$"));
    if (!pattern.match(name).hasMatch() || name == QLatin1String(".") || name == QLatin1String("..")) {
        done(Result::failed(tr("%1 is not a repository name GitHub accepts.").arg(name.left(100))));
        return;
    }
    if (m_env.findExecutable(QStringLiteral("gh")).isEmpty()) {
        done(Result::failed(tr("Creating a repository needs GitHub CLI: install github-cli, then run gh auth login.")));
        return;
    }
    CommandOptions options = gitHubOptions();
    options.timeoutMs = 60000;
    options.mutation = true;
    m_runner->run({"gh", "repo", "create", name, "--private", "--description", "My Omarchy profile, saved by Omarchy Control"}, options, this,
                  [this, name, done](const CommandResult &made) {
                      if (!made.ok()) {
                          done(Result::failed(tr("GitHub did not create %1: %2").arg(name, made.describe(QStringLiteral("gh")))));
                          return;
                      }
                      // gh answers with the new repository's address.
                      const QString url = made.outText().split('\n').last().trimmed();
                      connectTo(url, QStringLiteral("main"), false, done);
                  });
}

void ProfileRepository::fetch(Done done) {
    const Connection connection = this->connection();
    if (connection.isEmpty()) {
        done(Result::failed(tr("No GitHub repository is connected. Connect one first.")));
        return;
    }
    ensureRepository([this, connection, done](const Result &ready) {
        if (!ready.ok) {
            done(ready);
            return;
        }
        const QString tracking = "refs/remotes/origin/" + connection.branch;
        git({"fetch", "--no-tags", "--no-write-fetch-head", "--quiet", remoteUrl(connection), "+refs/heads/" + connection.branch + ":" + tracking},
            [this, tracking, done](const CommandResult &fetched) {
                const QString detail = QString::fromUtf8(fetched.err);
                if (!fetched.ok() && detail.contains(QLatin1String("couldn't find remote ref"))) {
                    // Connected, with nothing published on that branch yet.
                    git({"update-ref", "-d", tracking}, [done](const CommandResult &) {
                        done(Result::succeeded(QCoreApplication::translate("ProfileRepository", "Nothing is published there yet.")));
                    }, {}, 15000, true);
                    return;
                }
                if (!fetched.ok()) {
                    const bool auth = detail.contains(QLatin1String("Authentication")) || detail.contains(QLatin1String("could not read Username"))
                                      || detail.contains(QLatin1String("403")) || detail.contains(QLatin1String("not found"));
                    done(Result::failed(auth ? QCoreApplication::translate("ProfileRepository", "GitHub refused the request. Sign in with gh auth login, and check that this account can see the repository.")
                                            : QCoreApplication::translate("ProfileRepository", "Fetching failed: %1").arg(fetched.describe(QStringLiteral("git")))));
                    return;
                }
                done(Result::succeeded(QCoreApplication::translate("ProfileRepository", "Fetched the published versions.")));
            },
            {}, 300000);
    });
}

void ProfileRepository::publish(Done done) {
    const Connection connection = this->connection();
    if (connection.isEmpty()) {
        done(Result::failed(tr("No GitHub repository is connected. Connect one first.")));
        return;
    }
    if (connection.visibility == QLatin1String("public") && !connection.publicAllowed) {
        done(Result::failed(tr("%1 is public. Connect with --allow-public to publish there.").arg(connection.fullName())));
        return;
    }
    fetch([this, connection, done](const Result &fetched) {
        if (!fetched.ok) {
            done(Result::failed(tr("Nothing was published: %1 Your versions stay saved on this computer.").arg(fetched.error)));
            return;
        }
        status([this, connection, done](const Status &status) {
            if (status.saved.isEmpty()) {
                done(Result::failed(tr("Nothing is saved on this computer yet. Save a version first.")));
                return;
            }
            if (status.relation == QLatin1String("same")) {
                done(Result::succeeded(tr("Already published: GitHub has version %1.").arg(status.saved.commit.left(7)), status.saved.commit));
                return;
            }
            if (status.relation == QLatin1String("behind")) {
                done(Result::failed(tr("GitHub has newer versions than this computer, and nothing here is new. Use the published version here, or save new changes on top of it.")));
                return;
            }
            if (status.relation == QLatin1String("diverged")) {
                // Both sides changed: nothing is rewritten, merged, or forced, and both stay.
                done(Result::failed(tr("Both this computer and GitHub have versions the other does not, so nothing was published. Choose which to keep: profile resolve --use-published or --keep-mine.")));
                return;
            }
            const QString tracking = "refs/remotes/origin/" + connection.branch;
            git({"push", "--quiet", "--no-verify", remoteUrl(connection), status.saved.commit + ":refs/heads/" + connection.branch},
                [this, tracking, status, done](const CommandResult &pushed) {
                    if (!pushed.ok()) {
                        const QString detail = QString::fromUtf8(pushed.err);
                        done(Result::failed(detail.contains(QLatin1String("rejected")) || detail.contains(QLatin1String("fetch first"))
                                               ? QCoreApplication::translate("ProfileRepository", "GitHub changed while publishing, so nothing was published. Fetch, then try again.")
                                               : QCoreApplication::translate("ProfileRepository", "Publishing failed, and your versions stay saved on this computer: %1").arg(pushed.describe(QStringLiteral("git")))));
                        return;
                    }
                    git({"update-ref", tracking, status.saved.commit}, [status, done](const CommandResult &) {
                        Result result = Result::succeeded();
                        result.commit = status.saved.commit;
                        result.message = QCoreApplication::translate("ProfileRepository", "Published version %1.").arg(status.saved.commit.left(7));
                        done(result);
                    }, {}, 15000, true);
                },
                {}, 300000);
        });
    });
}

void ProfileRepository::resolve(const QString &choice, Done done) {
    if (choice != QLatin1String("published") && choice != QLatin1String("mine")) {
        done(Result::failed(tr("Choose published or mine.")));
        return;
    }
    status([this, choice, done](const Status &status) {
        const QString branch = status.connection.branch;
        const QString ref = "refs/heads/" + branch;
        if (status.published.isEmpty()) {
            done(Result::failed(tr("Nothing has been fetched from GitHub yet.")));
            return;
        }
        if (choice == QLatin1String("mine")) {
            if (status.saved.isEmpty() || status.relation != QLatin1String("diverged")) {
                done(Result::failed(tr("There is nothing to keep: this computer's versions do not differ from GitHub's that way.")));
                return;
            }
            if (status.unsavedChanges) {
                done(Result::failed(tr("Save your changes first, so they are part of what you keep.")));
                return;
            }
            // A new version with both histories behind it and this computer's files, so
            // publishing it only adds to what is published.
            writeVersion(workingDirectory(), status.published.commit + " " + status.saved.commit, tr("Kept this computer's version over the published one"),
                         [this, ref, status, done](const Result &written) {
                             if (!written.ok) {
                                 done(written);
                                 return;
                             }
                             git({"update-ref", "-m", "keep mine", ref, written.commit, status.saved.commit}, [written, done](const CommandResult &moved) {
                                 Result result = written;
                                 result.ok = moved.ok();
                                 result.message = moved.ok() ? QCoreApplication::translate("ProfileRepository", "Saved version %1, which keeps this computer's choices. Publish it to add it to GitHub.").arg(written.commit.left(7))
                                                             : QString();
                                 result.error = moved.ok() ? QString() : moved.describe(QStringLiteral("git"));
                                 done(result);
                             }, {}, 15000, true);
                         });
            return;
        }
        // Taking the published version: whatever this computer had, saved or not, is kept as
        // a version of its own first, never thrown away.
        const QString kept = "refs/omarchy-control/kept/" + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        const auto take = [this, ref, status, done](const QString &keptCommit) {
            git({"update-ref", "-m", "use published", ref, status.published.commit, status.saved.commit}, [this, status, keptCommit, done](const CommandResult &moved) {
                if (!moved.ok()) {
                    done(Result::failed(moved.describe(QStringLiteral("git"))));
                    return;
                }
                read(status.published.commit, [this, keptCommit, done](const Result &version) {
                    QString error;
                    if (!version.ok || !copyProfile(version.directory, workingDirectory(), &error)) {
                        done(Result::failed(version.ok ? error : version.error));
                        return;
                    }
                    Result result = Result::succeeded();
                    result.commit = version.commit;
                    result.message = keptCommit.isEmpty() ? QCoreApplication::translate("ProfileRepository", "Now using the published version %1.").arg(version.commit.left(7))
                                                          : QCoreApplication::translate("ProfileRepository", "Now using the published version %1. This computer's was kept as %2.")
                                                                .arg(version.commit.left(7), keptCommit.left(7));
                    done(result);
                });
            }, {}, 15000, true);
        };
        const bool working = QFileInfo::exists(ProfileModel::manifestPath(workingDirectory()));
        if (!status.unsavedChanges || !working) {
            if (status.saved.isEmpty() || status.relation == QLatin1String("behind") || status.relation == QLatin1String("same")) {
                take(QString());
                return;
            }
            git({"update-ref", kept, status.saved.commit, ""}, [take, status](const CommandResult &) { take(status.saved.commit); }, {}, 15000, true);
            return;
        }
        writeVersion(workingDirectory(), status.saved.commit, tr("Kept when the published version was taken"), [this, kept, take, done](const Result &written) {
            if (!written.ok) {
                done(written);
                return;
            }
            git({"update-ref", kept, written.commit, ""}, [take, written](const CommandResult &) { take(written.commit); }, {}, 15000, true);
        });
    });
}
