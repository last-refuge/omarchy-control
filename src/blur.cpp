#include "blur.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QRegularExpression>

#include "fileops.h"

namespace Blur {

Toggles toggles(const Env &env) {
    const QString dir = env.stateHome + "/omarchy/toggles/hypr/";
    Toggles t;
    t.windows = QFileInfo::exists(dir + "window-glassy.lua");
    t.bar = QFileInfo::exists(dir + "bar-glassy.lua");
    t.noAnimations = QFileInfo::exists(dir + "no-animations.lua");
    return t;
}

QHash<QString, QString> parseGlass(const QByteArray &lua) {
    // Only plain assignments inside the blur table: numbers and booleans.
    QHash<QString, QString> values;
    const int start = lua.indexOf("blur");
    if (start < 0)
        return values;
    static const QRegularExpression assignment(QStringLiteral("^\\s*([a-z_]+)\\s*=\\s*(-?[0-9]+(?:\\.[0-9]+)?|true|false)\\s*,?\\s*(?:--.*)?$"));
    for (const QString &line : QString::fromUtf8(lua.mid(start)).split('\n')) {
        const auto match = assignment.match(line);
        if (match.hasMatch() && !values.contains(match.captured(1)))
            values.insert(match.captured(1), match.captured(2));
    }
    return values;
}

QHash<QString, QString> glassValues(const Env &env) {
    QByteArray lua;
    if (!FileOps::readFile(env.omarchyPath + "/default/hypr/glass.lua", &lua))
        return {};
    return parseGlass(lua);
}

QString status(const Toggles &toggles, bool enabled) {
    if (!enabled)
        return QStringLiteral("off");
    if (toggles.windows)
        return QStringLiteral("windows");
    if (toggles.bar)
        return QStringLiteral("bar");
    return QStringLiteral("on");
}

QString statusReason(const Toggles &toggles, bool enabled) {
    if (enabled && toggles.windows && toggles.noAnimations)
        return QCoreApplication::translate("Blur", "With Animations and blur off, Glassy windows still keep their blur.");
    if (!enabled && toggles.bar && toggles.noAnimations)
        return QCoreApplication::translate("Blur", "With Animations and blur off, the Glassy bar is not blurred.");
    if (!enabled)
        return QCoreApplication::translate("Blur", "Blur shows behind Glassy windows or a Glassy bar. Choose Glassy for Window transparency, or for Bar background on the Desktop page. Choices made here apply when it comes on.");
    if (!toggles.windows && !toggles.bar)
        return QCoreApplication::translate("Blur", "Your own Hyprland config turns blur on.");
    return {};
}

static bool sameNumber(const QString &a, const QString &b) {
    bool okA = false, okB = false;
    const double x = a.toDouble(&okA), y = b.toDouble(&okB);
    return okA && okB && qAbs(x - y) < 0.0005;
}

QString strengthName(const QJsonObject &presets, const QString &size, const QString &passes,
                     const QHash<QString, QString> &glass) {
    for (auto it = presets.begin(); it != presets.end(); ++it) {
        const QJsonObject preset = it.value().toObject();
        const QString wantSize = preset.contains("size") ? QString::number(preset.value("size").toDouble()) : glass.value("size");
        const QString wantPasses = preset.contains("passes") ? QString::number(preset.value("passes").toDouble()) : glass.value("passes");
        if (sameNumber(size, wantSize) && sameNumber(passes, wantPasses))
            return it.key();
    }
    return size + QChar(0x00D7) + passes;
}

}  // namespace Blur
