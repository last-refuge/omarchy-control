#pragma once

#include <QJsonObject>
#include <QObject>
#include <QSize>
#include <QTimer>
#include <QVariantList>

#include "changecoordinator.h"
#include "commandrunner.h"

// Monitor arrangement. A layout is previewed under a separate watchdog process,
// armed before the first change, which restores the previous layout unless told
// to keep it. It survives this window crashing and tolerates a monitor being
// unplugged mid-preview.
//
// Editing stays locked until a recovery check has passed against the running
// Hyprland: the check moves one display a few pixels, confirms Hyprland took the
// change, and confirms the watchdog then restores it. Passing is recorded per
// Hyprland version.
//
// The provider also holds what the page is working on: which display is selected,
// independent of where the desktop's keyboard focus is, and the edits staged for a
// preview. Both survive a refresh; an edit is only dropped when its display is gone,
// and that is said rather than done quietly.
class DisplayProvider : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList monitors READ monitors NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool editingUnlocked READ editingUnlocked NOTIFY changed)
    Q_PROPERTY(bool previewing READ previewing NOTIFY previewChanged)
    Q_PROPERTY(int previewSeconds READ previewSeconds NOTIFY previewChanged)
    Q_PROPERTY(int previewTotal READ previewTotal NOTIFY previewChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY previewChanged)
    Q_PROPERTY(QString checkState READ checkState NOTIFY changed)
    Q_PROPERTY(QVariantMap lastCheck READ lastCheck NOTIFY changed)
    Q_PROPERTY(QString selected READ selected WRITE select NOTIFY changed)
    Q_PROPERTY(QString focusedOutput READ focusedOutput NOTIFY changed)
    Q_PROPERTY(bool savedLayout READ savedLayout NOTIFY changed)
    Q_PROPERTY(QVariantList staged READ staged NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(QString stageProblem READ stageProblem NOTIFY changed)
    Q_PROPERTY(QVariantMap selectedScale READ selectedScale NOTIFY changed)
    Q_PROPERTY(QVariantMap outcome READ outcome NOTIFY changed)

public:
    DisplayProvider(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *parent = nullptr);

    QVariantList monitors() const { return m_monitors; }
    QString status() const { return m_status; }
    bool editingUnlocked() const;
    bool previewing() const { return m_previewing; }
    int previewSeconds() const { return m_previewSeconds; }
    int previewTotal() const { return m_previewTotal; }
    bool busy() const { return m_starting || m_previewing || m_checkState == QLatin1String("running"); }
    QString checkState() const { return m_checkState; }
    QVariantMap lastCheck() const;
    QString selected() const { return m_selected; }
    QString focusedOutput() const { return m_focused; }
    bool savedLayout() const { return m_savedLayout; }
    QVariantList staged() const { return m_staged; }
    bool dirty() const { return m_dirty; }
    QString stageProblem() const;
    QVariantMap selectedScale() const;
    QVariantMap outcome() const { return m_outcome; }

    Q_INVOKABLE void refresh();
    // `layout` is a list of {output, mode, x, y, scale, transform, enabled}.
    Q_INVOKABLE QString validateLayout(const QVariantList &layout) const;
    Q_INVOKABLE void preview(const QVariantList &layout);
    Q_INVOKABLE void keep();
    Q_INVOKABLE void revert();
    Q_INVOKABLE void runRecoveryCheck();

    // Selection and staging. Nothing here reaches Hyprland: previewStaged() does,
    // through the same watchdog as preview().
    Q_INVOKABLE void select(const QString &output);
    Q_INVOKABLE void stageEdit(const QString &output, const QString &field, const QVariant &value);
    Q_INVOKABLE void stagePosition(const QString &output, int x, int y);
    Q_INVOKABLE void discardStaged();
    Q_INVOKABLE void previewStaged();
    Q_INVOKABLE void dismissOutcome();
    // Scales Hyprland can show exactly for a mode, as {value, label}, with `current` kept.
    static QVariantList scaleChoices(int width, int height, double current, double lowest = 0.5, double highest = 4);

    static QVariantList parseMonitors(const QByteArray &json);
    static QString monitorLua(const QJsonObject &monitor);
    static double cleanScale(double requested, int width, int height);
    static bool isSafeOutputName(const QString &name);
    // Logical size: the mode divided by the scale, turned on its side if rotated.
    static QSize logicalSize(const QVariantMap &row);

    // The body of the hidden `display-watchdog` subcommand: restores the
    // previous layout on timeout or request, then exits. Blocks until done.
    static int runWatchdog(const Env &env, const QString &planPath);

    // Used by `omarchy-control selftest display`.
    QString recoveryCheckPath() const;

signals:
    void changed();
    void previewChanged();
    void selectionChanged();
    // Hyprland reported its displays again; staging and selection changes do not count.
    void monitorsChanged();
    void message(const QString &text, bool isError);
    void checkFinished(bool passed, const QString &detail);

private:
    void startPreview(const QVariantList &layout, int seconds, bool isCheck);
    void applyLayout(const QVariantList &layout, int index, std::function<void(bool, const QString &)> done);
    void waitForWatchdog(const QString &file, const QString &expected, int attempts, std::function<void(bool)> done);
    void finishCheck();
    // Hyprland 0.56 answers `eval` before the next frame applies the rule.
    // untilMoved waits until the layout differs from the original (the nudge);
    // otherwise it waits until the layout matches again (the restore).
    void awaitCheckLayout(int attempts, bool untilMoved, bool restored = false);
    void failStart(const QString &text, bool isCheck);
    void endPreview(const QString &result);
    void verifyLayout(const QStringList &expected, std::function<void(bool same, bool known)> done);
    void setMonitors(const QVariantList &monitors);
    void rebuildStage();
    void setOutcome(const QString &kind, const QString &title, const QString &detail, const QString &scope = {});
    void collectInterrupted();
    QVariantMap appliedRow(const QString &output) const;
    QVariantMap stagedRow(const QString &output) const;
    QString runDirectory() const;
    static QStringList signatureOf(const QVariantList &monitors);
    QStringList layoutSignature() const { return signatureOf(m_monitors); }

    CommandRunner *m_runner;
    ChangeCoordinator *m_coordinator;
    QVariantList m_monitors;
    QVariantList m_pendingLayout;
    QVariantList m_staged;
    QHash<QString, QVariantMap> m_edits;
    QString m_selected;
    QString m_focused;
    bool m_dirty = false;
    bool m_savedLayout = false;
    QVariantMap m_outcome;
    QString m_status = "loading";
    QString m_checkState = "idle";
    QString m_hyprlandVersion;
    bool m_previewing = false;
    bool m_isCheck = false;
    bool m_starting = false;
    bool m_checkMoved = false;
    QStringList m_checkOriginal;
    QStringList m_previewOriginal;
    QHash<QString, QString> m_startFingerprints;
    int m_previewSeconds = 0;
    int m_previewTotal = 0;
    QString m_planPath, m_keepFile, m_revertFile, m_resultFile;
    QTimer m_countdown;
};
