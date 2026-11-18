#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

#include "env.h"

// The appearance browser needs previews of inactive themes and the actual
// desktop wallpaper. Omarchy UI supplies the live interface theme.
class ThemePreviewSource : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString wallpaper READ wallpaper NOTIFY changed)
    Q_PROPERTY(QString themeName READ themeName NOTIFY changed)

public:
    explicit ThemePreviewSource(const Env &env, QObject *parent = nullptr);

    QString wallpaper() const { return m_wallpaper; }
    QString themeName() const { return m_name; }

    Q_INVOKABLE QVariantMap preview(const QString &themeName) const;
    Q_INVOKABLE QString thumbnail(const QString &picture, int radius) const;
    Q_INVOKABLE QColor accentForeground(const QColor &accent) const;
    Q_INVOKABLE QColor readableColor(const QColor &color, const QColor &foreground,
                                     const QColor &canvas, const QColor &panel) const;

    static double contrast(const QString &a, const QString &b);
    static QString readable(const QString &color, const QString &foreground, const QStringList &surfaces, double ratio = 4.5);

signals:
    void changed();

private:
    static QHash<QString, QString> readColors(const QString &path);
    void reload();

    Env m_env;
    QString m_name;
    QString m_wallpaper;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
