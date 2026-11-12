#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include "profilecapture.h"
#include "profileplanner.h"
#include "profilerepository.h"
#include "profilerunner.h"

struct AppCore;

// My Omarchy for the window: the same capture, plan, restore, and versions the command line
// uses, as values QML can show. Nothing here reaches the network except connecting,
// fetching, and publishing, each only when asked. A restore that changes apps runs in a
// terminal, where sudo and pacman can ask, and keeps going if the window closes; this
// follows it by reading its record.
class ProfileProvider : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString busyText READ busyText NOTIFY changed)
    Q_PROPERTY(QVariantMap status READ status NOTIFY changed)
    Q_PROPERTY(QVariantList categories READ categories CONSTANT)
    Q_PROPERTY(QVariantMap review READ review NOTIFY changed)
    Q_PROPERTY(QVariantMap plan READ plan NOTIFY changed)
    Q_PROPERTY(QVariantMap run READ run NOTIFY changed)
    Q_PROPERTY(QVariantList versions READ versions NOTIFY changed)
    Q_PROPERTY(QVariantList publishedVersions READ publishedVersions NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)

public:
    ProfileProvider(AppCore *core, QObject *parent = nullptr);

    bool busy() const { return !m_busyText.isEmpty(); }
    QString busyText() const { return m_busyText; }
    QVariantMap status() const { return m_status; }
    QVariantList categories() const;
    QVariantMap review() const { return m_review; }
    QVariantMap plan() const { return m_plan; }
    QVariantMap run() const { return m_run; }
    QVariantList versions() const { return m_versions; }
    QVariantList publishedVersions() const { return m_publishedVersions; }
    QString message() const { return m_message; }
    QString error() const { return m_error; }

    // Reads this computer only: what is saved, published as last seen, and any restore.
    Q_INVOKABLE void refresh();

    // What saving would change, from a selection: categories, suggested, the desktop records
    // by name (themes, background, bar, plugins, dock, launchers, associations, shortcuts,
    // windowRules), configFiles, suggestedApps, apps, and name. Nothing is written.
    Q_INVOKABLE void startCapture(const QVariantMap &selection);
    // The same review again, with these suggested apps remembered too.
    Q_INVOKABLE void includeApps(const QStringList &apps);
    // Writes what was reviewed, saves it as a version, and publishes it when asked.
    Q_INVOKABLE void saveReview(const QString &message, bool publish);
    Q_INVOKABLE void clearReview();

    Q_INVOKABLE void connectRepository(const QString &repository, bool allowPublic);
    Q_INVOKABLE void createRepository(const QString &name);
    Q_INVOKABLE void fetch();
    Q_INVOKABLE void publish();
    Q_INVOKABLE void resolve(const QString &choice);
    Q_INVOKABLE void loadVersions();

    // A restore review of a version: "saved", "published", or a commit. Changes nothing.
    Q_INVOKABLE void planRestore(const QString &revision);
    // A decision on one item the review asks about: "use-saved", "keep-local", or "" to undo
    // it. The review is made again with it.
    Q_INVOKABLE void choose(const QString &itemId, const QString &resolution);
    Q_INVOKABLE void clearPlan();
    // Starts the reviewed restore. `partial` accepts that some items are left out.
    Q_INVOKABLE void startRestore(bool partial);
    Q_INVOKABLE void resumeRestore();
    Q_INVOKABLE void cancelRestore();
    Q_INVOKABLE void clearRun();

    // Row shapes, for tests and the window alike.
    QVariantMap captureItem(const ProfileCapture::Item &item) const;
    QVariantMap planItem(const ProfilePlanItem &item) const;
    QVariantMap runMap(const ProfileRun &run) const;

signals:
    void changed();
    void toast(const QString &text, const QString &kind);

private:
    void setBusy(const QString &text);
    void finish(const QString &message, const QString &error = {});
    void replan();
    void followTerminalRun();
    QString shownValue(const QString &id, const QString &value) const;
    ProfileCapture::Selection selectionFrom(const QVariantMap &selection) const;
    void capture(const ProfileCapture::Selection &selection);

    AppCore *m_core;
    QString m_busyText;
    QVariantMap m_status, m_review, m_plan, m_run;
    QVariantList m_versions, m_publishedVersions;
    QString m_message, m_error;
    ProfileCapture::Selection m_selection;
    ProfileCapture::Result m_capture;
    bool m_hasCapture = false;
    QString m_planRevision, m_planDirectory;
    ProfilePlanner::Options m_planOptions;
    ProfilePlan m_currentPlan;
    QTimer m_follow;  // Reads the record of a restore running in a terminal.
    QString m_dismissed;  // A stopped restore someone closed, not shown again this session.
    int m_followTicks = 0;
};
