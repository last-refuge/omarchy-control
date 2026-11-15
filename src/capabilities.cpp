#include "capabilities.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSysInfo>

#include "fileops.h"

Capabilities::Capabilities(CommandRunner *runner, QObject *parent) : QObject(parent), m_runner(runner) {
    scanFiles();
}

bool Capabilities::hasCommand(const QString &name) const {
    return !m_runner->env().findExecutable(name).isEmpty();
}

void Capabilities::set(const QString &key, const QVariant &value) {
    if (m_values.value(key) == value && m_values.contains(key))
        return;
    m_values.insert(key, value);
    emit changed();
}

static QString firstLine(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readLine()).trimmed();
}

void Capabilities::scanFiles() {
    const Env &env = m_runner->env();

    // Hardware presence, from the kernel's own view. Absence here means
    // "No device detected", which the interface keeps distinct from unsupported.
    const QDir power(env.sys("/sys/class/power_supply"));
    bool battery = false;
    for (const auto &name : power.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (firstLine(power.filePath(name) + "/type") == QLatin1String("Battery"))
            battery = true;
    set("battery", battery);

    bool wifi = false;
    const QDir net(env.sys("/sys/class/net"));
    for (const auto &name : net.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (QFileInfo::exists(net.filePath(name) + "/wireless"))
            wifi = true;
    set("wifi", wifi);

    set("bluetooth", !QDir(env.sys("/sys/class/bluetooth")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    set("backlight", !QDir(env.sys("/sys/class/backlight")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    bool keyboardLight = false;
    for (const auto &name : QDir(env.sys("/sys/class/leds")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot))
        if (name.contains("kbd_backlight"))
            keyboardLight = true;
    set("kbdbacklight", keyboardLight);

    const int chassis = firstLine(env.sys("/sys/class/dmi/id/chassis_type")).toInt();
    set("laptop", chassis == 8 || chassis == 9 || chassis == 10 || chassis == 14 || chassis == 31 || chassis == 32);
    set("hibernation", QFileInfo::exists(env.sys("/etc/mkinitcpio.conf.d/omarchy_resume.conf")));

    QString cpu = firstLine(env.sys("/sys/firmware/devicetree/base/compatible"));
    const QString product = firstLine(env.sys("/sys/class/dmi/id/product_family"));
    set("snapdragon", cpu.contains("qcom") || product.contains("Snapdragon"));
    set("n1x", cpu.contains("nvidia,n1x"));
    set("hybridgpu", hasCommand("supergfxctl"));

    bool externalConnected = false;
    QString internalName;
    const QDir drm(env.sys("/sys/class/drm"));
    for (const auto &name : drm.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!name.contains('-') || firstLine(drm.filePath(name) + "/status") != QLatin1String("connected"))
            continue;
        if (name.contains("eDP") || name.contains("LVDS") || name.contains("DSI"))
            internalName = name.section('-', 1);  // card1-eDP-1 is eDP-1 to Hyprland.
        else
            externalConnected = true;
    }
    set("externalmonitor", externalConnected);
    set("noexternalmonitor", !externalConnected);
    set("internaldisplay", !internalName.isEmpty());
    set("internaldisplay.name", internalName);
    // Omarchy's own switches for the built-in panel, which its recovery command clears.
    const QString toggles = env.stateHome + "/omarchy/toggles/hypr/";
    set("internaloff", QFileInfo::exists(toggles + "internal-monitor-disable.lua"));
    set("internalmirror", QFileInfo::exists(toggles + "internal-monitor-mirror.lua"));

    // A kernel update removes the running kernel's modules until the next boot, the
    // same evidence omarchy-update-restart goes by.
    const QDir modules(env.sys("/usr/lib/modules"));
    set("reboot.needed", modules.exists() && !QFileInfo::exists(modules.filePath(QSysInfo::kernelVersion() + "/modules.builtin")));

    // Evidence of a custom setup, so controls can say what they cannot reach.
    QByteArray shellJson;
    FileOps::readFile(env.configHome + "/omarchy/shell.json", &shellJson);
    const auto shellConfig = QJsonDocument::fromJson(FileOps::stripJsonComments(shellJson)).object();
    const QString barId = shellConfig.value("bar").toObject().value("id").toString();
    const QJsonArray disabled = shellConfig.value("disabledPlugins").toArray();
    auto isDisabled = [&](const QString &id) {
        for (const auto &value : disabled)
            if (value.toString() == id)
                return true;
        return false;
    };
    set("custombar", !barId.isEmpty() && barId != QLatin1String("omarchy.bar"));
    set("custombar.id", barId);
    // `omarchy bar get` and `set` only reach a widget that is on the bar, so a bar without
    // Omarchy's clock (a custom one often swaps it for its own) leaves the clock formats
    // nothing to read or change. A shell.json that says nothing about the layout is the
    // stock bar, which has the clock.
    const QJsonObject barLayout = shellConfig.value("bar").toObject().value("layout").toObject();
    bool barClock = barLayout.isEmpty();
    for (auto section = barLayout.begin(); section != barLayout.end() && !barClock; ++section)
        for (const auto &widget : section.value().toArray())
            // Omarchy accepts a bare id as well as an object with options.
            if ((widget.isString() ? widget.toString() : widget.toObject().value("id").toString()) == QLatin1String("omarchy.clock"))
                barClock = true;
    set("barclock", barClock);
    set("customdock", isDisabled("omarchy.dock"));
    set("custompower", isDisabled("omarchy.power"));

    QByteArray hyprland;
    FileOps::readFile(env.configHome + "/hypr/hyprland.lua", &hyprland);
    set("hypr.luaconfig", !hyprland.isEmpty());
    set("hypr.import", hyprland.contains("require(\"hypr.omarchy-control\")"));
    set("hypr.importable", hyprland.contains("require(\"hypr.autostart\")"));

}

static QString packageVersion(const CommandResult &result) {
    // The route prints the package version; the file under OMARCHY_PATH can lag.
    return result.ok() ? result.outText() : QString();
}

void Capabilities::scanServices() {
    m_runner->run({"omarchy", "version"}, CommandOptions{3000}, this, [this](const CommandResult &result) {
        m_versions.insert("omarchy", packageVersion(result));
        emit changed();
    });
    m_runner->run({"omarchy", "shell", "shell", "ping"}, CommandOptions{3000}, this, [this](const CommandResult &result) {
        set("shell", result.ok());
    });
    m_runner->run(m_runner->env().hyprctl({"-j", "version"}), CommandOptions{2000}, this, [this](const CommandResult &result) {
        const auto object = QJsonDocument::fromJson(result.out).object();
        set("hyprland", result.ok() && !object.isEmpty());
        m_versions.insert("hyprland", object.value("version").toString());
    });
    m_runner->run(m_runner->env().hyprctl({"-j", "devices"}), CommandOptions{2000}, this, [this](const CommandResult &result) {
        const auto object = QJsonDocument::fromJson(result.out).object();
        bool touchpad = false;
        for (const auto &mouse : object.value("mice").toArray()) {
            const QString name = mouse.toObject().value("name").toString();
            if (name.contains("touchpad", Qt::CaseInsensitive) || name.contains("trackpad", Qt::CaseInsensitive))
                touchpad = true;
        }
        set("touchpad", touchpad);
        set("touchscreen", !object.value("touch").toArray().isEmpty());
    });
}

void Capabilities::scanServicesBlocking() {
    m_versions.insert("omarchy", packageVersion(m_runner->runBlocking({"omarchy", "version"}, CommandOptions{3000})));
    auto ping = m_runner->runBlocking({"omarchy", "shell", "shell", "ping"}, CommandOptions{3000});
    set("shell", ping.ok());
    auto version = m_runner->runBlocking(m_runner->env().hyprctl({"-j", "version"}), CommandOptions{2000});
    const auto object = QJsonDocument::fromJson(version.out).object();
    set("hyprland", version.ok() && !object.isEmpty());
    m_versions.insert("hyprland", object.value("version").toString());
    auto devices = m_runner->runBlocking(m_runner->env().hyprctl({"-j", "devices"}), CommandOptions{2000});
    const auto deviceObject = QJsonDocument::fromJson(devices.out).object();
    bool touchpad = false;
    for (const auto &mouse : deviceObject.value("mice").toArray()) {
        const QString name = mouse.toObject().value("name").toString();
        if (name.contains("touchpad", Qt::CaseInsensitive) || name.contains("trackpad", Qt::CaseInsensitive))
            touchpad = true;
    }
    set("touchpad", touchpad);
    set("touchscreen", !deviceObject.value("touch").toArray().isEmpty());
}

Capabilities::Availability Capabilities::availability(const CatalogEntry &entry) const {
    Availability result;
    const auto tr = [](const char *text) { return QCoreApplication::translate("Capabilities", text); };
    for (const auto &hardware : entry.requiresHardware) {
        if (!has(hardware)) {
            result.available = false;
            result.kind = "hardware";
            static const QHash<QString, const char *> names{
                {"touchpad", QT_TRANSLATE_NOOP("Capabilities", "No touchpad detected.")},
                {"touchscreen", QT_TRANSLATE_NOOP("Capabilities", "No touchscreen detected.")},
                {"battery", QT_TRANSLATE_NOOP("Capabilities", "No battery detected.")},
                {"wifi", QT_TRANSLATE_NOOP("Capabilities", "No Wi-Fi adapter detected.")},
                {"bluetooth", QT_TRANSLATE_NOOP("Capabilities", "No Bluetooth adapter detected.")},
                {"laptop", QT_TRANSLATE_NOOP("Capabilities", "This does not apply: no built-in display was detected.")},
                {"externalmonitor", QT_TRANSLATE_NOOP("Capabilities", "No external display detected.")},
                {"internaldisplay", QT_TRANSLATE_NOOP("Capabilities", "No built-in display detected.")},
                {"internaloff", QT_TRANSLATE_NOOP("Capabilities", "The built-in display is already enabled.")},
                {"noexternalmonitor", QT_TRANSLATE_NOOP("Capabilities", "An external display is connected, so use Laptop display on or off instead.")},
                {"hybridgpu", QT_TRANSLATE_NOOP("Capabilities", "No switchable graphics detected.")},
                {"snapdragon", QT_TRANSLATE_NOOP("Capabilities", "This only applies to Snapdragon laptops.")},
                {"n1x", QT_TRANSLATE_NOOP("Capabilities", "This only applies to NVIDIA N1x laptops.")},
                {"kbdbacklight", QT_TRANSLATE_NOOP("Capabilities", "No keyboard backlight detected.")},
                {"hibernation", QT_TRANSLATE_NOOP("Capabilities", "Hibernation is not set up.")},
            };
            const char *text = names.value(hardware, QT_TRANSLATE_NOOP("Capabilities", "No device detected."));
            result.reason = tr(text);
            return result;
        }
    }
    for (const auto &cap : entry.requiresCaps) {
        // A service not probed yet is not a service that is down: let the read go
        // ahead, since it reports its own failure if there is one.
        if (!m_values.contains(cap) || has(cap))
            continue;
        if (entry.softRequires)
            continue;
        result.available = false;
        result.kind = "service";
        if (cap == QLatin1String("shell"))
            result.reason = tr("The Omarchy shell is not responding, so this is unavailable right now.");
        else if (cap == QLatin1String("hyprland"))
            result.reason = tr("Hyprland is not reachable from this window.");
        else if (cap == QLatin1String("barclock"))
            result.reason = tr("Your bar has no Omarchy clock, so there is nothing to set here.");
        else
            result.reason = tr("A required service is not running.");
        return result;
    }
    for (const auto &command : entry.requiresCommands) {
        if (!hasCommand(command)) {
            if (entry.softRequires)
                continue;
            result.available = false;
            result.kind = "tool";
            result.reason = entry.installHint.isEmpty() ? tr("%1 is not installed.").arg(command) : entry.installHint;
            return result;
        }
    }
    return result;
}

