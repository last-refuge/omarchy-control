#include "backend.h"

#include <QThread>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "fileops.h"

Backend::Backend(const Env &env, QObject *parent)
    : QObject(parent),
      m_core(std::make_unique<AppCore>(env)),
      m_launcher(&m_core->runner, &m_core->capabilities),
      m_audio(&m_core->runner, &m_core->coordinator),
      m_bluetooth(&m_core->runner, &m_core->coordinator),
      m_network(&m_core->runner, &m_core->coordinator),
      m_displays(&m_core->runner, &m_core->coordinator),
      m_services(&m_core->runner, &m_core->coordinator),
      m_shortcuts(&m_core->runner, &m_core->coordinator),
      m_plugins(&m_core->runner, &m_core->coordinator),
      m_system(&m_core->catalog, &m_core->runner, &m_core->capabilities, &m_core->coordinator),
      m_windowRules(&m_core->runner, &m_core->coordinator),
      m_updates(&m_core->runner),
      m_iconImporter(env, &m_core->coordinator),
      m_profiles(m_core.get()) {
    loadPreferences();
    connect(&m_core->capabilities, &Capabilities::changed, this, &Backend::capabilitiesChanged);
    connect(&m_core->capabilities, &Capabilities::changed, this, &Backend::problemsChanged);
    connect(&m_core->coordinator, &ChangeCoordinator::historyChanged, this, &Backend::historyChanged);
    connect(&m_services, &ServicesProvider::changed, this, &Backend::problemsChanged);

    auto relay = [this](const QString &text, bool isError) { emit toast(text, isError ? "error" : "ok"); };
    connect(&m_audio, &AudioProvider::message, this, relay);
    connect(&m_bluetooth, &BluetoothProvider::message, this, relay);
    connect(&m_network, &NetworkProvider::message, this, relay);
    connect(&m_displays, &DisplayProvider::message, this, relay);
    connect(&m_displays, &DisplayProvider::checkFinished, this, [this](bool passed, const QString &detail) { emit toast(detail, passed ? "ok" : "error"); });
    connect(&m_services, &ServicesProvider::message, this, relay);
    connect(&m_shortcuts, &ShortcutsProvider::message, this, relay);
    connect(&m_plugins, &PluginsProvider::message, this, relay);
    connect(&m_system, &SystemProvider::message, this, relay);
    connect(&m_profiles, &ProfileProvider::toast, this, &Backend::toast);
    connect(&m_updates, &UpdateProvider::message, this, relay);
    connect(&m_windowRules, &WindowRulesProvider::message, this, relay);
    connect(&m_core->launchers, &LauncherEditor::message, this, [this](const QString &text, const QString &kind) { emit toast(text, kind); });
    // A launcher added, removed, or edited elsewhere shows in the list.
    connect(&m_appsWatcher, &QFileSystemWatcher::directoryChanged, &m_core->launchers, [this] {
        if (!m_core->launchers.entries().isEmpty())
            m_core->launchers.reload();
    });
    connect(&m_iconImporter, &IconThemeImporter::finished, this, [this] { refresh(QStringLiteral("appearance.icons")); });
    // The display tools describe the displays themselves, which change on their own.
    connect(&m_displays, &DisplayProvider::monitorsChanged, this, [this] {
        for (const auto *entry : m_core->catalog.entriesForPage(QStringLiteral("displays")))
            if (entry->isAction())
                refresh(entry->id);
    });
    connect(&m_displays, &DisplayProvider::selectionChanged, this, &Backend::refreshTargets);

    m_watchDebounce.setSingleShot(true);
    m_watchDebounce.setInterval(250);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_watchDebounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_watchDebounce.start(); });
    connect(&m_watchDebounce, &QTimer::timeout, this, [this] {
        watchFiles();
        m_core->capabilities.scanFiles();
        refreshPage(m_currentPage);
        // Omarchy applies a new theme's icons a moment after the theme's folder swaps, and
        // reading only at the swap would look like something else had changed them.
        if (m_currentPage == QLatin1String("appearance"))
            QTimer::singleShot(2500, this, [this] { refresh(QStringLiteral("appearance.icons")); });
    });
    watchFiles();

    m_layerDebounce.setSingleShot(true);
    m_layerDebounce.setInterval(3000);
    connect(&m_layerDebounce, &QTimer::timeout, this, &Backend::syncIconLayer);
    connect(&m_appsWatcher, &QFileSystemWatcher::directoryChanged, &m_layerDebounce, qOverload<>(&QTimer::start));
    for (const QString &root : QStringList{env.dataHome} + env.dataDirs)
        if (QFileInfo(root + "/applications").isDir())
            m_appsWatcher.addPath(root + "/applications");
    // Something may have changed while this was closed. After the window has drawn.
    QTimer::singleShot(1500, this, &Backend::syncIconLayer);

    // What a page can do depends on capabilities that arrive after it first draws.
    m_capabilityDebounce.setSingleShot(true);
    m_capabilityDebounce.setInterval(60);
    connect(&m_core->capabilities, &Capabilities::changed, &m_capabilityDebounce, qOverload<>(&QTimer::start));
    connect(&m_capabilityDebounce, &QTimer::timeout, this, [this] { refreshPage(m_currentPage); });

    // Repair anything a previous run left unfinished before showing state.
    const QStringList recovered = m_core->coordinator.recoverInterrupted();
    for (const auto &message : recovered + m_iconImporter.recoverStaging())
        QTimer::singleShot(600, this, [this, message] { emit toast(message, "warning"); });

    // Local only: where this copy came from, which decides how it updates.
    m_updates.readSource();
    m_core->capabilities.scanServices();
    m_system.refresh();
    m_services.refresh();
}

void Backend::watchFiles() {
    // Omarchy's commands replace these atomically, which drops a file watch, so
    // the directories are what is really being watched, re-armed on each event.
    const Env &env = m_core->env;
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);
    // The current theme's folders are swapped wholesale on a theme change, however it was
    // made, and everything that comes from a theme, like its backgrounds, follows from that.
    const QStringList candidates{env.stateHome + "/omarchy/current", env.stateHome + "/omarchy/current/theme",
                                 env.stateHome + "/omarchy/current/theme/backgrounds",
                                 env.configHome + "/omarchy", env.configHome + "/hypr", env.stateHome + "/omarchy/toggles",
                                 env.stateHome + "/omarchy/toggles/hypr", env.stateHome + "/omarchy/indicators",
                                 env.stateHome + "/omarchy/powerprofiles", env.stateHome + "/omarchy/defaults", env.configHome + "/autostart"};
    // Each is watched as one folder, not per pack: a pack added or removed is what matters.
    for (const auto &path : candidates + IconThemes::searchRoots(env))
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);
}

QString Backend::stateKey(const QString &id) {
    QString key = id;
    key.replace('.', "__");
    key.replace('-', "_");
    return key;
}

QVariantList Backend::pages() const {
    QVariantList list;
    for (const auto &page : m_core->catalog.pages())
        list << QVariantMap{{"id", page.id}, {"label", page.label}, {"icon", page.icon}, {"description", page.description}, {"sections", page.sections}, {"jumps", page.jumps}};
    return list;
}

QVariantMap Backend::entry(const QString &id) const {
    const CatalogEntry *e = m_core->catalog.find(id);
    return e ? e->toVariant() : QVariantMap{};
}

QVariantList Backend::sections(const QString &page) const {
    const PageInfo *info = m_core->catalog.page(page);
    QVariantList out;
    if (!info)
        return out;
    const auto entries = m_core->catalog.entriesForPage(page);
    QStringList titles = info->sections;
    for (const auto *e : entries)
        if (!titles.contains(e->section))
            titles << e->section;
    for (const auto &title : std::as_const(titles)) {
        QVariantList rows;
        for (const auto *e : entries) {
            // A row pointing at a block on its own page exists so search can land
            // there; on the page itself the block is already on screen.
            if (e->launch.value("kind").toString() == QLatin1String("page") && e->launch.value("page").toString() == page)
                continue;
            if (e->section == title)
                rows << e->toVariant();
        }
        if (!rows.isEmpty())
            out << QVariantMap{{"title", title}, {"rows", rows}};
    }
    return out;
}

QVariantList Backend::panels(const QString &page) const {
    auto entryOf = [this](const QString &id) {
        const CatalogEntry *e = m_core->catalog.find(id);
        return e ? QVariant(e->toVariant()) : QVariant();
    };
    QVariantList rows;
    for (const auto &rowValue : m_core->catalog.panelLayout(page)) {
        QVariantList columns;
        for (const auto &column : rowValue.toObject().value("columns").toArray()) {
            QVariantList panels;
            for (const auto &panelValue : column.toArray()) {
                QVariantMap panel = panelValue.toObject().toVariantMap();
                QVariantList entries;
                for (const auto &id : panelValue.toObject().value("rows").toArray())
                    entries << entryOf(id.toString());
                panel["rows"] = entries;
                const QString header = panelValue.toObject().value("header").toString();
                panel["header"] = header.isEmpty() ? QVariant() : entryOf(header);
                // A library's groups carry their rows as full entries too.
                if (panelValue.toObject().contains("library")) {
                    QVariantList groups;
                    for (const auto &groupValue : panelValue.toObject().value("library").toArray()) {
                        QVariantMap group = groupValue.toObject().toVariantMap();
                        QVariantList items;
                        for (const auto &itemValue : groupValue.toObject().value("items").toArray()) {
                            QVariantMap item = itemValue.toObject().toVariantMap();
                            QVariantList itemRows;
                            for (const auto &id : itemValue.toObject().value("rows").toArray())
                                itemRows << entryOf(id.toString());
                            item["rows"] = itemRows;
                            items << item;
                        }
                        group["items"] = items;
                        groups << group;
                    }
                    panel["library"] = groups;
                }
                panels << panel;
            }
            columns << QVariant(panels);
        }
        QVariantMap row = rowValue.toObject().toVariantMap();
        row["columns"] = columns;
        rows << row;
    }
    return rows;
}

QStringList Backend::blocks(const QString &page) const {
    const PageInfo *info = m_core->catalog.page(page);
    return info ? info->blocks : QStringList{};
}

QVariantList Backend::search(const QString &query) const {
    QVariantList results;
    for (const auto &hit : m_core->catalog.search(query, 30)) {
        QVariantMap row = hit.entry->toVariant();
        const PageInfo *page = m_core->catalog.page(hit.entry->page);
        row["pageLabel"] = page ? page->label : QString();
        const auto availability = m_core->capabilities.availability(*hit.entry);
        row["available"] = availability.available;
        row["reason"] = availability.reason;
        results << row;
    }
    return results;
}

QVariantMap Backend::availability(const QString &id) const {
    const CatalogEntry *e = m_core->catalog.find(id);
    if (!e)
        return {{"available", false}, {"reason", QString()}, {"kind", QString()}};
    const auto a = m_core->capabilities.availability(*e);
    return {{"available", a.available}, {"reason", a.reason}, {"kind", a.kind}};
}

void Backend::setState(const QString &id, const QVariantMap &state) {
    m_states.insert(stateKey(id), state);
}

QVariantMap Backend::currentState(const QString &id) const {
    return m_states.value(stateKey(id)).toMap();
}

QVariantList Backend::compatChanges() const {
    QVariantList rows;
    for (const auto &name : m_core->compat.changedCommands(m_core->capabilities.versions().value("hyprland").toString()))
        rows << name;
    return rows;
}

void Backend::acceptCompat() {
    m_core->compat.acceptAll(m_core->capabilities.versions().value("hyprland").toString());
    emit problemsChanged();
    refreshPage(m_currentPage);
    emit toast(tr("Changes resumed. If something behaves differently, check the result."), "ok");
}

QString Backend::detailFor(const CatalogEntry &entry) const {
    // What the display tools would act on, said from what Hyprland reports.
    QString internal, external;
    bool internalOn = false;
    for (const auto &monitor : m_displays.monitors()) {
        const auto m = monitor.toMap();
        if (m.value("internal").toBool() && internal.isEmpty()) {
            internal = m.value("output").toString();
            internalOn = m.value("enabled").toBool();
        } else if (!m.value("internal").toBool() && m.value("enabled").toBool() && external.isEmpty()) {
            external = m.value("output").toString();
        }
    }
    if (internal.isEmpty())
        internal = m_core->capabilities.values().value("internaldisplay.name").toString();
    if (internal.isEmpty())
        return {};
    if (entry.id == QLatin1String("trigger.hardware.laptop-display"))
        return internalOn ? tr("Built-in %1 is enabled.").arg(internal) : tr("Built-in %1 is turned off.").arg(internal);
    if (entry.id == QLatin1String("trigger.hardware.mirror-display") && !external.isEmpty())
        return m_core->capabilities.has("internalmirror") ? tr("%1 is showing the same picture as %2.").arg(external, internal)
                                                         : tr("Shows %1 on %2 as well.").arg(internal, external);
    return {};
}

void Backend::refreshTargets() {
    const QString target = m_displays.selected();
    if (target.isEmpty())
        return;
    for (const auto *entry : m_core->catalog.entriesForPage(QStringLiteral("displays")))
        if (entry->target == QLatin1String("display"))
            refresh(entry->id + "@" + target);
}

QString Backend::noteFor(const CatalogEntry &entry) const {
    const QString changed = m_core->compat.changedFor(entry, m_core->capabilities.versions().value("hyprland").toString());
    if (!changed.isEmpty())
        return tr("Omarchy's %1 changed since this was tested, so changes here are paused. Review it under Advanced.").arg(changed);
    if (entry.note == QLatin1String("custombar") && m_core->capabilities.has("custombar"))
        return tr("Your bar is a custom one (%1), which may not follow this setting.").arg(m_core->capabilities.values().value("custombar.id").toString());
    if (entry.note == QLatin1String("customdock") && m_core->capabilities.has("customdock"))
        return tr("Omarchy's dock is replaced by a custom plugin, which may not follow this setting.");
    if (entry.note == QLatin1String("custompower") && m_core->capabilities.has("custompower"))
        return tr("Omarchy's power widget is disabled, so this panel may not open.");
    return {};
}

void Backend::refresh(const QString &id) {
    const CatalogEntry *entry = m_core->catalog.find(id);
    if (!entry)
        return;
    QVariantMap state = currentState(id);
    if (entry->isAction()) {
        const auto a = m_core->capabilities.availability(*entry);
        state["status"] = a.available ? "ok" : "unavailable";
        state["reason"] = a.reason;
        state["note"] = noteFor(*entry);
        state["detail"] = a.available ? detailFor(*entry) : QString();
        setState(id, state);
        return;
    }
    // An old value is kept on screen while re-reading, but marked as unconfirmed.
    state["loading"] = true;
    if (!state.contains("status"))
        state["status"] = "loading";
    setState(id, state);

    m_core->engine.readChoices(id, [this, id](const QVariantList &listed) {
        // A choice that names an app gets that app's icon from the icon theme.
        QVariantList choices = listed;
        for (auto &value : choices) {
            auto choice = value.toMap();
            if (!choice.contains("icons"))
                continue;
            const QStringList names = choice.value("icons").toStringList();
            const QString key = names.join('|');
            if (!m_iconUrls.contains(key))
                m_iconUrls.insert(key, IconThemes::appIconUrl(m_core->env, IconThemes::desktopDefault(m_core->env), names));
            choice["icon"] = m_iconUrls.value(key);
            value = choice;
        }
        m_choices.insert(id, choices);
        QVariantMap updated = currentState(id);
        updated["choices"] = choices;
        setState(id, updated);
    });
    m_core->engine.read(id, [this, id, entry](const ReadResult &result) {
        QVariantMap updated = currentState(id);
        updated["loading"] = false;
        updated["status"] = result.status;
        updated["value"] = result.value;
        updated["source"] = result.source;
        updated["reason"] = result.reason;
        updated["isOverride"] = result.isOverride;
        updated["note"] = noteFor(*entry);
        updated["display"] = m_core->engine.displayValue(*entry, result.value, m_choices.value(id));
        updated["extra"] = result.extra;
        setState(id, updated);
    });
}

void Backend::refreshPage(const QString &page) {
    for (const auto *entry : m_core->catalog.entriesForPage(page))
        refresh(entry->id);
    if (page == QLatin1String("displays"))
        refreshTargets();
    if (page == QLatin1String("overview")) {
        for (const auto &entry : m_core->catalog.entries())
            if (entry.quick)
                refresh(entry.id);
        refresh("system.version");
    }
}

void Backend::pageShown(const QString &page) {
    m_currentPage = page;
    refreshPage(page);
    m_audio.setActive(page == QLatin1String("sound"));
    if (page == QLatin1String("sound") || page == QLatin1String("overview")) {
        m_audio.refresh();
        m_network.refresh();
    }
    // The Overview has a Bluetooth switch too, and without a read of its own it would
    // say off until the Sound page had been opened.
    if (page == QLatin1String("sound") || page == QLatin1String("overview"))
        m_bluetooth.refresh();
    // Also runs on returning to the window, which is how a finished update gets noticed.
    if (page == QLatin1String("about"))
        m_updates.refreshInstalled();
    // My Omarchy reads only this computer when shown; the network waits to be asked.
    if (page == QLatin1String("myomarchy")) {
        m_profiles.refresh();
        m_profiles.loadVersions();
    }
    if (page == QLatin1String("network"))
        m_network.refresh();
    if (page == QLatin1String("displays") || page == QLatin1String("overview"))
        m_displays.refresh();
    if (page == QLatin1String("desktop")) {
        m_plugins.refresh();
        m_shortcuts.refresh();
    }
    if (page == QLatin1String("system")) {
        m_system.refresh();
        m_services.refresh();
    }
    if (page == QLatin1String("apps"))
        m_system.refreshApps();
    if (page == QLatin1String("power") || page == QLatin1String("advanced"))
        m_system.refresh();
}

void Backend::windowActivated() {
    // A helper may have just closed, so what is on screen could be stale.
    m_core->capabilities.scanFiles();
    m_core->capabilities.scanServices();
    pageShown(m_currentPage);
}

QVariantMap Backend::planChange(const QString &id, const QString &value) {
    return m_core->engine.plan(id, value, m_choices.value(id)).toVariant();
}

void Backend::handleResult(const QString &id, const CatalogEntry *entry, const Transaction &tx) {
    QVariantMap state = currentState(id);
    state["busy"] = false;
    const bool ok = tx.phase == QLatin1String("committed");
    state["error"] = ok ? QString() : tx.message;
    setState(id, state);
    if (ok) {
        emit toast(tr("%1 changed").arg(QCoreApplication::translate("catalog", entry->label.toUtf8().constData())), "ok");
        if (!entry->restart.isEmpty() && !m_pendingRestarts.contains(entry->restart)) {
            m_pendingRestarts << entry->restart;
            emit problemsChanged();
        }
    } else {
        emit toast(tx.message, tx.phase == QLatin1String("partial") ? "warning" : "error");
    }
    refresh(id);
    m_core->capabilities.scanFiles();
    refreshRelated(id);
}

void Backend::refreshRelated(const QString &id) {
    // A display setting moves what the arrangement shows.
    if (id.startsWith(QLatin1String("displays.")))
        m_displays.refresh();
    // Blur rows share their file and their strength is made of two of them, and the
    // three settings that switch blur on and off change what each of them shows.
    static const QStringList blurSwitches{"appearance.transparency", "desktop.bar.background", "appearance.animations"};
    if (id.startsWith(QLatin1String("appearance.blur.")) || blurSwitches.contains(id))
        for (const auto &other : m_core->catalog.entries())
            if (other.id.startsWith(QLatin1String("appearance.blur.")) && other.id != id)
                refresh(other.id);
}

void Backend::commit(const QString &id, const QString &value) {
    const CatalogEntry *entry = m_core->catalog.find(id);
    if (!entry)
        return;
    QVariantMap state = currentState(id);
    state["busy"] = true;
    state["error"] = QString();
    setState(id, state);
    m_core->engine.apply(id, value, [this, id, entry](const Transaction &tx) { handleResult(id, entry, tx); });
}

void Backend::resetValue(const QString &id) {
    const CatalogEntry *entry = m_core->catalog.find(id);
    if (!entry)
        return;
    QVariantMap state = currentState(id);
    state["busy"] = true;
    setState(id, state);
    m_core->engine.reset(id, [this, id, entry](const Transaction &tx) { handleResult(id, entry, tx); });
}

void Backend::undo(const QString &transactionId) {
    m_core->engine.undo(transactionId, [this](const Transaction &tx) {
        if (tx.phase == QLatin1String("committed"))
            emit toast(tr("Undid %1").arg(QCoreApplication::translate("catalog", tx.label.toUtf8().constData())), "ok");
        else
            emit toast(tx.message, "error");
        if (!tx.settingId.isEmpty()) {
            refresh(tx.settingId);
            refreshRelated(tx.settingId);
        }
    });
}

void Backend::launch(const QString &id) {
    const CatalogEntry *entry = m_core->catalog.find(id);
    if (!entry)
        return;
    m_launcher.launch(*entry, [this](const HelperLauncher::Outcome &outcome) {
        if (!outcome.navigatePage.isEmpty()) {
            emit navigate(outcome.navigatePage, outcome.navigateFocus);
            return;
        }
        emit toast(outcome.message, outcome.opened ? "opened" : "error");
    });
}

QVariantList Backend::history() const {
    QVariantList rows;
    for (const auto &tx : m_core->coordinator.history(100))
        if (tx.finished())
            rows << tx.toVariant();
    return rows;
}

QVariantList Backend::problems() const {
    QVariantList rows;
    auto add = [&rows](const QString &text, const QString &page) { rows << QVariantMap{{"text", text}, {"page", page}}; };
    const auto &caps = m_core->capabilities;
    if (caps.values().contains("shell") && !caps.has("shell"))
        add(tr("The Omarchy shell is not responding. Panels cannot open until it restarts."), "system");
    if (caps.values().contains("hyprland") && !caps.has("hyprland"))
        add(tr("Hyprland is not reachable, so window and display settings are unavailable."), "advanced");
    if (HyprModule(m_core->env).state() == HyprModule::ModifiedByUser)
        add(tr("omarchy-control.lua was edited by hand, so Hyprland settings here are paused."), "advanced");
    if (caps.has("reboot.needed"))
        add(tr("A kernel update is waiting: restart this computer to finish it."), "power");
    const QStringList changed = m_core->compat.changedCommands(caps.versions().value("hyprland").toString());
    if (!changed.isEmpty())
        add(tr("Omarchy changed %n command(s) since this app was tested, so those settings are paused.", "", changed.size()), "advanced");
    const int failed = m_services.failedCount();
    if (failed > 0)
        add(tr("%n service(s) failed.", "", failed), "system");
    if (!compatible())
        add(tr("This Omarchy version is newer than the one this app was tested with. Changes may not behave as described."), "advanced");
    return rows;
}

// A build still running reads this app's state, so it finishes before that goes away.
Backend::~Backend() {
    if (m_layerWorker)
        m_layerWorker->wait();
}

LauncherEditor *Backend::launchers() {
    return &m_core->launchers;
}

void Backend::syncIconLayer() {
    if (m_layerRunning) {
        m_layerAgain = true;
        return;
    }
    m_layerRunning = true;
    const IconThemeProvider *icons = &m_core->icons;
    QThread *worker = QThread::create([icons] { icons->syncLayer(true); });
    m_layerWorker = worker;
    connect(worker, &QThread::finished, this, [this, worker] {
        worker->deleteLater();
        m_layerWorker = nullptr;
        m_layerRunning = false;
        refresh(QStringLiteral("appearance.icons.launcher"));
        if (m_layerAgain) {
            m_layerAgain = false;
            syncIconLayer();
        }
    });
    worker->start();
}

QVariantMap Backend::planIconRemoval(const QString &id) const {
    const IconThemeProvider::Removal plan = m_core->icons.planRemoval(id);
    return {{"ok", plan.ok}, {"reason", plan.reason}, {"name", plan.name}, {"path", plan.path}};
}

void Backend::removeIconPack(const QString &id) {
    m_core->icons.removePack(id, [this](const Transaction &tx) {
        const bool ok = tx.phase == QLatin1String("committed");
        const QString name = tx.extra.value("remove").toObject().value("name").toString();
        emit toast(ok ? tr("Moved %1 to the Trash. Undo in History brings it back.").arg(name) : tx.message, ok ? "ok" : "error");
        refresh(QStringLiteral("appearance.icons"));
        refresh(QStringLiteral("appearance.icons.launcher"));
    });
}

void Backend::retireIconic() {
    QString message;
    const int code = m_core->icons.retireIconic(&message);
    emit toast(message, code == 0 ? "ok" : "error");
    refresh(QStringLiteral("appearance.icons"));
    refresh(QStringLiteral("appearance.icons.launcher"));
}

void Backend::restoreIconic() {
    QString message;
    const int code = m_core->icons.restoreIconic(&message);
    emit toast(message, code == 0 ? "ok" : "error");
    refresh(QStringLiteral("appearance.icons"));
    refresh(QStringLiteral("appearance.icons.launcher"));
}

void Backend::clearRestartNotice() {
    m_pendingRestarts.clear();
    emit problemsChanged();
}

QString Backend::omarchyVersion() const {
    return m_core->capabilities.versions().value("omarchy").toString();
}

bool Backend::compatible() const {
    const QString version = omarchyVersion();
    // The tested baseline. Anything else keeps working but says so.
    return version.isEmpty() || version.startsWith("4.5");
}

void Backend::loadPreferences() {
    QByteArray contents;
    FileOps::readFile(m_core->env.appConfigDir + "/preferences.json", &contents);
    const auto object = QJsonDocument::fromJson(contents).object();
    m_pins.clear();
    for (const auto &value : object.value("pins").toArray())
        m_pins << value.toString();
}

void Backend::savePreferences() {
    QJsonObject object;
    object["pins"] = QJsonArray::fromStringList(m_pins);
    FileOps::atomicWrite(m_core->env.appConfigDir + "/preferences.json", QJsonDocument(object).toJson(), nullptr, 0600);
}

void Backend::togglePin(const QString &id) {
    if (m_pins.contains(id))
        m_pins.removeAll(id);
    else if (m_core->catalog.find(id))
        m_pins << id;
    savePreferences();
    emit pinsChanged();
}

QVariantList Backend::pinnedEntries() const {
    QVariantList rows;
    for (const auto &id : m_pins)
        if (const CatalogEntry *e = m_core->catalog.find(id))
            rows << e->toVariant();
    return rows;
}
