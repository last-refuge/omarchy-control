#include "theme.h"

#include <QUrl>

#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <cmath>

#include "fileops.h"

ThemePreviewSource::ThemePreviewSource(const Env &env, QObject *parent)
    : QObject(parent), m_env(env) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &ThemePreviewSource::reload);
    reload();
}

QHash<QString, QString> ThemePreviewSource::readColors(const QString &path) {
    QHash<QString, QString> colors;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return colors;
    static const QRegularExpression expression(R"re(^\s*([A-Za-z0-9_]+)\s*=\s*["']([^"']+)["'])re",
                                                QRegularExpression::MultilineOption);
    auto matches = expression.globalMatch(QString::fromUtf8(file.readAll()));
    while (matches.hasNext()) {
        const auto match = matches.next();
        colors.insert(match.captured(1), match.captured(2));
    }
    return colors;
}

static double relativeLuminance(const QColor &c) {
    const auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    return .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
}

// Black or white, whichever reads on the accent (WCAG relative luminance).
QColor ThemePreviewSource::accentForeground(const QColor &accent) const {
    return relativeLuminance(accent) > .179 ? QColor(Qt::black) : QColor(Qt::white);
}

QColor ThemePreviewSource::readableColor(const QColor &color, const QColor &foreground,
                                        const QColor &canvas, const QColor &panel) const {
    return QColor(readable(color.name(), foreground.name(), {canvas.name(), panel.name()}));
}

double ThemePreviewSource::contrast(const QString &a, const QString &b) {
    const double first = relativeLuminance(QColor(a)), second = relativeLuminance(QColor(b));
    return (qMax(first, second) + .05) / (qMin(first, second) + .05);
}

QString ThemePreviewSource::readable(const QString &color, const QString &foreground, const QStringList &surfaces, double ratio) {
    const QColor from(color), to(foreground);
    if (!from.isValid() || !to.isValid())
        return color;
    const auto enoughOn = [&](const QString &candidate) {
        for (const auto &surface : surfaces)
            if (contrast(candidate, surface) < ratio)
                return false;
        return true;
    };
    if (enoughOn(color))
        return color;
    // Lighten or darken first, so a warning stays amber instead of drifting to gray.
    float hue, saturation, lightness;
    from.getHslF(&hue, &saturation, &lightness);
    const float target = to.lightnessF() < lightness ? 0.0f : 1.0f;
    for (int step = 1; step <= 20; ++step) {
        const QString shifted = QColor::fromHslF(hue, saturation, lightness + (target - lightness) * step / 20.0f).name();
        if (enoughOn(shifted))
            return shifted;
    }
    // A palette with no room left for that color: fall back toward its own text color.
    QString candidate = color;
    for (int step = 0; step <= 20; ++step) {
        const double t = step / 20.0;
        candidate = QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * t,
                                     from.greenF() + (to.greenF() - from.greenF()) * t,
                                     from.blueF() + (to.blueF() - from.blueF()) * t).name();
        if (enoughOn(candidate))
            break;
    }
    return candidate;
}

QString ThemePreviewSource::thumbnail(const QString &picture, int radius) const {
    if (picture.isEmpty())
        return {};
    const QString path = picture.startsWith(QLatin1String("file:")) ? QUrl(picture).toLocalFile() : picture;
    // Two themes can use the same filename in the same swapped folder, and images are
    // cached by address, so the address carries the file's age as well as its name.
    const qint64 version = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    return "image://icons/thumb/" + QString::number(radius) + "/" + QString::number(version) + "/" + QString::fromUtf8(QUrl::toPercentEncoding(path));
}

QVariantMap ThemePreviewSource::preview(const QString &themeName) const {
    QString slug = themeName.trimmed().toLower();
    slug.replace(' ', '-');
    QVariantMap result;
    if (slug.isEmpty() || slug.contains('/') || slug.startsWith('.'))
        return result;
    // A theme in the user's folder wins over one of the same name that Omarchy ships.
    for (const QString &directory : {m_env.configHome + "/omarchy/themes/" + slug, m_env.omarchyPath + "/themes/" + slug}) {
        const auto colors = readColors(directory + "/colors.toml");
        if (colors.isEmpty())
            continue;
        result["background"] = colors.value("background", "#1c1c1e");
        result["surface"] = colors.value("dark_background", colors.value("background", "#1c1c1e"));
        result["foreground"] = colors.value("foreground", "#f5f5f7");
        result["accent"] = colors.value("accent", "#FFD60A");
        result["line"] = colors.value("selection", colors.value("foreground", "#f5f5f7"));
        if (QFileInfo::exists(directory + "/preview.png")) {
            result["image"] = QUrl::fromLocalFile(directory + "/preview.png").toString();
        } else {
            // A theme with no preview still has its wallpapers, which say as much.
            const auto stills = QDir(directory + "/backgrounds").entryList({"*.png", "*.jpg", "*.jpeg", "*.webp"}, QDir::Files | QDir::Readable, QDir::Name);
            if (!stills.isEmpty())
                result["image"] = QUrl::fromLocalFile(directory + "/backgrounds/" + stills.first()).toString();
        }
        break;
    }
    return result;
}

void ThemePreviewSource::reload() {
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // Wallpaper and theme tiles are Control features. Follow swaps of current/,
    // the installed theme directory and the active wallpaper symlink.
    const QString current = m_env.stateHome + "/omarchy/current";
    QString ancestor = current;
    while (!QFileInfo::exists(ancestor) && ancestor != "/" && ancestor != ".")
        ancestor = QFileInfo(ancestor).absolutePath();

    const QString backgroundLink = current + "/background";
    const QString backgroundFile = QFileInfo(backgroundLink).canonicalFilePath();
    QStringList candidates{ancestor, QFileInfo(ancestor).absolutePath(), current, current + "/theme",
                           current + "/theme.name", backgroundFile};
    candidates.removeDuplicates();
    for (const auto &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    QByteArray nameBytes;
    FileOps::readFile(current + "/theme.name", &nameBytes);
    const QString name = QString::fromUtf8(nameBytes).trimmed();

    static const QRegularExpression still("\\.(png|jpe?g|webp)$", QRegularExpression::CaseInsensitiveOption);
    // Include the file version even when the new theme uses the same path.
    const QString wallpaper = still.match(backgroundFile).hasMatch()
                                  ? QUrl::fromLocalFile(backgroundFile).toString() + "#" + QString::number(QFileInfo(backgroundFile).lastModified().toMSecsSinceEpoch())
                                  : QString();
    if (wallpaper != m_wallpaper || name != m_name) {
        m_wallpaper = wallpaper;
        m_name = name;
        emit changed();
    }
}
