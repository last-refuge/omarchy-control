#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QStringList>
#include <functional>

#include "commandrunner.h"
#include "env.h"
#include "profilemodel.h"

// The profile's history: versions saved on this computer, and the ones published to a
// private GitHub repository. Git is run with none of the user's own configuration, no hooks,
// no filters, and only https, and never checks anything out: a version is written from the
// profile's reviewed files alone, and read back blob by blob, refused whole if it holds
// anything a profile never contains. Saving never uses the network; only connecting,
// fetching, and publishing do. Publishing only ever adds to what is published.
class ProfileRepository : public QObject {
    Q_OBJECT

public:
    struct Connection {
        QString owner, name;
        QString branch = QStringLiteral("main");
        QString visibility;          // private, public, internal; empty when it could not be read
        bool publicAllowed = false;  // Publishing to a public repository was asked for by name.
        QString account;             // The GitHub account signed in when it was connected.
        QString authorEmail;         // That account's no-reply address, which versions carry.
        bool isEmpty() const { return owner.isEmpty(); }
        QString fullName() const { return owner + "/" + name; }
        QString webUrl() const { return "https://github.com/" + fullName(); }
        QJsonObject toJson() const;
        static Connection fromJson(const QJsonObject &object);
    };

    struct Version {
        QString commit, tree, message;
        qint64 time = 0;  // Seconds since the epoch.
        QStringList parents;
        bool isEmpty() const { return commit.isEmpty(); }
        QJsonObject toJson() const;
    };

    struct Status {
        bool gitAvailable = false;
        bool ghAvailable = false;
        Connection connection;
        Version saved;      // The newest version saved on this computer.
        Version published;  // The newest one published, as of the last fetch or publish.
        // nothing, unsaved-only, saved, published-only, same, ahead, behind, diverged
        QString relation;
        int ahead = 0, behind = 0;
        bool unsavedChanges = false;  // The working profile differs from the newest saved version.
        QString error;
        QJsonObject toJson() const;
    };

    struct Result {
        bool ok = false;
        QString error;
        QString message;    // What happened, as a person reads it.
        QString commit;     // The version made, published, or read.
        QString directory;  // For a version read back: where its files are.
        QStringList problems;  // For a version refused: everything wrong with it.
        QJsonObject toJson() const;
        static Result failed(const QString &error) {
            Result result;
            result.error = error;
            return result;
        }
        static Result succeeded(const QString &message = {}, const QString &commit = {}) {
            Result result;
            result.ok = true;
            result.message = message;
            result.commit = commit;
            return result;
        }
    };

    using StatusDone = std::function<void(const Status &)>;
    using Done = std::function<void(const Result &)>;

    ProfileRepository(CommandRunner *runner, const Env &env, QObject *parent = nullptr);

    // Where the profile this computer saves lives, as plain files, and its history.
    QString workingDirectory() const;
    QString repositoryPath() const;
    Connection connection() const;
    bool gitAvailable() const;

    // "https://github.com/owner/name", "github.com/owner/name", or "owner/name". Nothing
    // else: version one publishes to GitHub over https only.
    static bool parseGitHub(const QString &text, Connection *connection, QString *error);

    // Reads only this computer: what is saved, what was last seen published, and whether the
    // working profile has changes not saved yet.
    void status(StatusDone done);
    // Saves the working profile as a new version, when it differs from the newest one.
    void save(const QString &message, Done done);
    // The newest versions on this computer, or as last seen published.
    void history(bool published, int limit, std::function<void(const QList<Version> &, const QString &error)> done);
    // Reads a version back into a folder of its own, refused whole if anything in it is not
    // what a profile contains: `revision` is a commit, "saved", or "published".
    void read(const QString &revision, Done done);
    // When the working profile is empty, it starts from the newest version saved here, or
    // published, so a capture never begins again from nothing.
    void prepareWorking(Done done);

    // What one profile changes from another, record by record, as a person reads it: each
    // row has kind (setting, app, desktop, file), id, change (added, removed, changed), and
    // the values before and after.
    static QJsonArray differences(const Profile &before, const Profile &after);

    // The network, only when asked. Connecting reads the repository's visibility with GitHub
    // CLI when it is signed in; `allowPublic` is the decision to publish to a public one.
    void connectTo(const QString &repository, const QString &branch, bool allowPublic, Done done);
    // Creates a private repository with GitHub CLI and connects to it.
    void create(const QString &name, Done done);
    void fetch(Done done);
    // Fetches first, then adds this computer's versions to what is published, only when that
    // needs no rewriting. When both changed, nothing is published and both stay.
    void publish(Done done);
    // After both changed: "published" takes the published version here, keeping this
    // computer's newest under refs/omarchy-control/kept; "mine" saves this computer's
    // newest as a new version on top of the published one, so publishing it adds to history.
    void resolve(const QString &choice, Done done);

private:
    QStringList gitArgv(const QStringList &arguments, const QString &indexFile = {}) const;
    void git(const QStringList &arguments, std::function<void(const CommandResult &)> done, const QByteArray &input = {}, int timeoutMs = 15000,
             bool mutation = false, const QString &indexFile = {});
    void ensureRepository(Done done);
    void resolveRef(const QString &ref, std::function<void(const Version &)> done);
    void writeVersion(const QString &directory, const QString &parent, const QString &message, Done done);
    QString remoteUrl(const Connection &connection) const;
    bool saveConnection(const Connection &connection, QString *error) const;
    QString author(const Connection &connection, QString *email) const;

    CommandRunner *m_runner;
    Env m_env;
};
