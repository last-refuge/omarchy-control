#include "networkpreview.h"
#include "displaypreview.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <time.h>
#include <unistd.h>

#include "changecoordinator.h"
#include "networkprovider.h"

namespace {
const QString Manager = "org.freedesktop.NetworkManager";
const QString Root = "/org/freedesktop/NetworkManager";
const QString Properties = "org.freedesktop.DBus.Properties";
constexpr int Timeout = 60;
using Settings = QMap<QString, QVariantMap>;
using Rollback = QMap<QString, uint>;
struct Failure { QString code, message; };
void require(bool ok, const QString &code, const QString &message) { if (!ok) throw Failure{code, message}; }
bool matches(const QString &value, const QString &pattern) {
    const auto match = QRegularExpression(pattern).match(value);
    return match.hasMatch() && match.capturedLength() == value.size();
}
bool objectPath(const QString &path, const QString &kind) { return matches(path, "^" + Root + "/" + kind + "/[A-Za-z0-9_]+$"); }
bool uuid(const QString &value) { return matches(value, "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"); }
qint64 bootMs() {
#ifdef Q_OS_LINUX
    timespec value{};
    require(clock_gettime(CLOCK_BOOTTIME, &value) == 0, "read_unavailable", "The Linux boot clock could not be read.");
    return qint64(value.tv_sec) * 1000 + value.tv_nsec / 1000000;
#else
    throw Failure{"unavailable", "Network previews require the Linux boot clock."};
#endif
}
bool terminal(const QString &phase) { return QStringList{"kept", "reverted", "absent", "daemon_changed", "refused"}.contains(phase); }
QString directory(const Env &env) { return env.appStateDir + "/network-previews"; }
void privateFile(const QString &path, bool dir) {
    QFileInfo info(path);
    require(!info.isSymLink() && (dir ? info.isDir() : info.isFile()) && info.ownerId() == uint(geteuid())
        && !(info.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther)),
        "state_conflict", "The network preview state has unsafe ownership, permissions or links: " + path);
}
QJsonObject readJson(const QString &path) {
    privateFile(path, false);
    QFile file(path);
    require(file.size() <= 65536 && file.open(QIODevice::ReadOnly), "state_conflict", "The network preview receipt could not be read.");
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    require(error.error == QJsonParseError::NoError && doc.isObject(), "state_conflict", "The network preview receipt is invalid JSON.");
    return doc.object();
}
void writeJson(const QString &path, const QJsonObject &value) {
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) privateFile(path, false);
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    require(file.open(QIODevice::WriteOnly) && file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        && file.write(bytes) == bytes.size() && file.commit(), "write_unavailable", "The private network preview receipt could not be saved.");
}
struct Store {
    const Env &env;
    QString path() const { return directory(env); }
    void prepare() {
        if (!QFileInfo::exists(path())) {
            require(!QFileInfo(path()).isSymLink() && QDir().mkpath(path()) && QFile::setPermissions(path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
                "write_unavailable", "The private network preview directory could not be created.");
        }
        privateFile(path(), true);
    }
    QString current() const {
        if (!QFileInfo::exists(path()) && !QFileInfo(path()).isSymLink()) return {};
        privateFile(path(), true);
        const QString index = path() + "/current.json";
        if (!QFileInfo::exists(index) && !QFileInfo(index).isSymLink()) return {};
        const auto value = readJson(index);
        require(value.size() == 1 && NetworkPreview::validToken(value.value("token").toString()), "state_conflict", "The network preview index is invalid.");
        return value.value("token").toString();
    }
    QJsonObject read(const QString &token) const {
        require(NetworkPreview::validToken(token), "invalid_value", "Use a preview token returned by network activate/disconnect.");
        privateFile(path(), true);
        const QString file = path() + "/" + token + ".json";
        require(QFileInfo::exists(file) || QFileInfo(file).isSymLink(), "stale_token", "That preview token has no local receipt.");
        auto value = readJson(file);
        const QStringList keys{"version", "token", "phase", "action", "interface", "uuid", "device", "udi", "type", "beforeUuid", "beforeState", "busId", "owner", "requestedBootMs", "checkpoint", "createdBootMs", "expiresBootMs", "active", "message"};
        for (const auto &key : value.keys()) require(keys.contains(key), "state_conflict", "The preview receipt has unrecognized fields.");
        require(value.value("version").toInt() == 1 && value.value("token") == token
            && QStringList{"creating", "checkpoint", "applying", "applied", "preview", "keeping", "reverting", "unknown", "kept", "reverted", "absent", "daemon_changed", "refused"}.contains(value.value("phase").toString())
            && QStringList{"activate", "disconnect"}.contains(value.value("action").toString())
            && NetworkProvider::isScanDeviceName(value.value("interface").toString()) && objectPath(value.value("device").toString(), "Devices")
            && !value.value("udi").toString().isEmpty() && value.value("udi").toString().size() <= 4096
            && (value.value("type").toInt() == 1 || value.value("type").toInt() == 2)
            && (value.value("beforeState").toInt() == 30 || value.value("beforeState").toInt() == 100)
            && (value.value("beforeState").toInt() == 30 ? value.value("beforeUuid").toString().isEmpty() : uuid(value.value("beforeUuid").toString()))
            && (value.value("action") == "disconnect" ? value.value("uuid").toString().isEmpty() : uuid(value.value("uuid").toString()))
            && matches(value.value("busId").toString(), "^[a-fA-F0-9]{32}$") && matches(value.value("owner").toString(), "^:[0-9]+\\.[0-9]+$")
            && matches(value.value("requestedBootMs").toString(), "^[0-9]{1,16}$")
            && (value.value("checkpoint").toString().isEmpty() || objectPath(value.value("checkpoint").toString(), "Checkpoint"))
            && (value.value("active").toString().isEmpty() || objectPath(value.value("active").toString(), "ActiveConnection"))
            && (value.value("createdBootMs").toString().isEmpty() || (matches(value.value("createdBootMs").toString(), "^[0-9]{1,16}$")
                && value.value("expiresBootMs").toString().toLongLong() == value.value("createdBootMs").toString().toLongLong() + Timeout * 1000)),
            "state_conflict", "The network preview receipt is malformed; no network action was sent.");
        return value;
    }
    void save(const QJsonObject &value) { writeJson(path() + "/" + value.value("token").toString() + ".json", value); }
    void prune() {
        const auto indexed = current();
        QDirIterator entries(path(), QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
        QList<QFileInfo> finished;
        int count = 0;
        while (entries.hasNext()) {
            entries.next();
            require(++count <= 256, "state_conflict", "Too many preview receipt entries; review the private receipt directory before creating another preview.");
            const auto info = entries.fileInfo();
            if (info.fileName() == "current.json") continue;
            require(info.suffix() == "json" && NetworkPreview::validToken(info.completeBaseName()), "state_conflict", "An unrecognized preview receipt entry was preserved.");
            const auto value = read(info.completeBaseName());
            if (info.completeBaseName() != indexed && terminal(value.value("phase").toString())) finished << info;
        }
        std::sort(finished.begin(), finished.end(), [](const QFileInfo &a, const QFileInfo &b) { return a.lastModified() < b.lastModified(); });
        // The indexed terminal receipt becomes an older receipt when the next
        // token is published, so leave room for it in the 100-entry retention.
        while (finished.size() > 99) {
            const auto oldest = finished.takeFirst();
            require(QFile::remove(oldest.filePath()), "write_unavailable", "An old owned preview receipt could not be pruned.");
        }
    }
};

class Client {
    QString name;
    QDBusConnection bus;
    QElapsedTimer deadline;
public:
    QString busId, owner;
    explicit Client(const Env &env) : name("omarchy-control-preview-" + QUuid::createUuid().toString(QUuid::WithoutBraces)),
        bus(QDBusConnection::connectToBus(env.process.value("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/run/dbus/system_bus_socket"), name)) {
        try {
            deadline.start();
            require(bus.isConnected(), "unavailable", "The system D-Bus is unavailable.");
            busId = rpc("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetId", {}, "s").arguments().first().toString();
            owner = rpc("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner", {Manager}, "s").arguments().first().toString();
            require(matches(busId, "^[a-fA-F0-9]{32}$") && matches(owner, "^:[0-9]+\\.[0-9]+$"), "read_unavailable", "D-Bus did not identify NetworkManager.");
            qDBusRegisterMetaType<Settings>(); qDBusRegisterMetaType<Rollback>();
        } catch (...) { QDBusConnection::disconnectFromBus(name); throw; }
    }
    ~Client() { QDBusConnection::disconnectFromBus(name); }
    void resetBudget() { deadline.restart(); }
    QDBusMessage rpc(const QString &service, const QString &path, const QString &iface, const QString &method, const QVariantList &args, const QString &signature, bool mutation = false) {
        const int remaining = 25000 - int(deadline.elapsed());
        require(remaining > 0, "operation_timeout", "Network preview processing exceeded its deadline; inspect its token before retrying.");
        auto request = QDBusMessage::createMethodCall(service, path, iface, method);
        request.setInteractiveAuthorizationAllowed(false);
        request.setArguments(args);
        const auto reply = bus.call(request, QDBus::Block, std::min(remaining, mutation ? 5000 : 3000));
        const bool denied = reply.type() == QDBusMessage::ErrorMessage && QStringList{"org.freedesktop.DBus.Error.AccessDenied", "org.freedesktop.NetworkManager.PermissionDenied", "org.freedesktop.NetworkManager.NotAuthorized"}.contains(reply.errorName());
        const bool missingMethod = reply.type() == QDBusMessage::ErrorMessage && reply.errorName() == "org.freedesktop.DBus.Error.UnknownMethod";
        require(reply.type() == QDBusMessage::ReplyMessage, mutation ? denied ? "authorization_denied" : missingMethod ? "unsupported_configuration" : "request_unconfirmed" : "read_unavailable",
            "NetworkManager " + method + " was not confirmed: " + ChangeCoordinator::redact(reply.errorName()).left(200));
        require(reply.signature() == signature, mutation ? "request_unconfirmed" : "read_unavailable", "NetworkManager returned an unexpected " + method + " reply.");
        return reply;
    }
    QDBusMessage call(const QString &path, const QString &iface, const QString &method, const QVariantList &args, const QString &signature, bool mutation = false) {
        return rpc(owner, path, iface, method, args, signature, mutation);
    }
    QVariant get(const QString &path, const QString &iface, const QString &property) {
        const auto reply = call(path, Properties, "Get", {iface, property}, "v");
        return qvariant_cast<QDBusVariant>(reply.arguments().first()).variant();
    }
    QString string(const QString &path, const QString &iface, const QString &property) {
        const auto value = get(path, iface, property);
        require(value.metaType().id() == QMetaType::QString && value.toString().size() <= 4096, "read_unavailable", "NetworkManager returned an invalid " + property + ".");
        return value.toString();
    }
    uint number(const QString &path, const QString &iface, const QString &property) {
        const auto value = get(path, iface, property);
        require(value.metaType().id() == QMetaType::UInt, "read_unavailable", "NetworkManager returned an invalid " + property + ".");
        return value.toUInt();
    }
    bool boolean(const QString &path, const QString &iface, const QString &property) {
        const auto value = get(path, iface, property);
        require(value.metaType().id() == QMetaType::Bool, "read_unavailable", "NetworkManager returned an invalid " + property + ".");
        return value.toBool();
    }
    QString path(const QString &node, const QString &iface, const QString &property) {
        const auto value = get(node, iface, property);
        require(value.metaType() == QMetaType::fromType<QDBusObjectPath>(), "read_unavailable", "NetworkManager returned an invalid " + property + ".");
        return qvariant_cast<QDBusObjectPath>(value).path();
    }
    QStringList paths(const QString &node, const QString &iface, const QString &property) {
        const auto value = get(node, iface, property);
        require(value.metaType() == QMetaType::fromType<QDBusArgument>() && qvariant_cast<QDBusArgument>(value).currentSignature() == "ao",
            "read_unavailable", "NetworkManager returned an invalid " + property + " list.");
        const auto values = qdbus_cast<QList<QDBusObjectPath>>(value);
        require(values.size() <= 4096, "read_unavailable", "NetworkManager returned too many objects.");
        QStringList result;
        for (const auto &entry : values) { require(!result.contains(entry.path()), "read_unavailable", "NetworkManager returned duplicate objects."); result << entry.path(); }
        return result;
    }
    QStringList checkpoints() {
        const auto values = paths(Root, Manager, "Checkpoints");
        for (const auto &value : values) require(objectPath(value, "Checkpoint"), "read_unavailable", "NetworkManager returned an invalid checkpoint.");
        return values;
    }
    QJsonObject device(const QString &path) {
        require(objectPath(path, "Devices"), "stale_target", "The selected device path is invalid.");
        const QString iface = Manager + ".Device";
        const QString name = string(path, iface, "Interface"), udi = string(path, iface, "Udi");
        const uint type = number(path, iface, "DeviceType"), state = number(path, iface, "State");
        require(NetworkProvider::isScanDeviceName(name) && !udi.isEmpty() && (type == 1 || type == 2), "unsupported_configuration", "Only managed Ethernet and Wi-Fi device previews are supported.");
        require(boolean(path, iface, "Managed"), "unsupported_configuration", "That device is not managed by NetworkManager.");
        const QString active = this->path(path, iface, "ActiveConnection");
        QJsonObject result{{"device", path}, {"interface", name}, {"udi", udi}, {"type", int(type)}, {"state", int(state)}, {"active", active}, {"uuid", ""}, {"activeState", 0}};
        if (active != QLatin1String("/")) {
            require(objectPath(active, "ActiveConnection"), "read_unavailable", "NetworkManager returned an invalid active connection.");
            const auto aiface = Manager + ".Connection.Active";
            const auto devices = paths(active, aiface, "Devices");
            require(devices == QStringList{path}, "unsupported_configuration", "Composite and multi-device connections are not supported by this preview.");
            const QString id = string(active, aiface, "Uuid");
            require(uuid(id), "read_unavailable", "NetworkManager returned an invalid active UUID.");
            const QString profile = this->path(active, aiface, "Connection");
            require(objectPath(profile, "Settings") && !(number(profile, Manager + ".Settings.Connection", "Flags") & 14),
                "unsupported_configuration", "Externally generated or volatile connections are not driven by Control previews.");
            result["uuid"] = id.toLower();
            result["activeState"] = int(number(active, aiface, "State"));
        }
        return result;
    }
    void identity(const QJsonObject &receipt) {
        require(busId == receipt.value("busId").toString() && owner == receipt.value("owner").toString(), "stale_daemon", "The D-Bus or NetworkManager instance changed; the old token cannot control this daemon.");
    }
    void checkpoint(const QJsonObject &receipt) {
        identity(receipt);
        const QString cp = receipt.value("checkpoint").toString();
        require(!cp.isEmpty() && !receipt.value("createdBootMs").toString().isEmpty() && checkpoints().contains(cp), "preview_unconfirmed", "The owned checkpoint is absent or its identity was not durably recorded.");
        const auto iface = Manager + ".Checkpoint";
        const auto created = get(cp, iface, "Created");
        require(created.metaType().id() == QMetaType::LongLong && QString::number(created.toLongLong()) == receipt.value("createdBootMs").toString()
            && number(cp, iface, "RollbackTimeout") == Timeout && paths(cp, iface, "Devices") == QStringList{receipt.value("device").toString()},
            "stale_checkpoint", "Checkpoint identity or rollback policy changed; no keep/revert was sent.");
    }
};

bool sameDevice(const QJsonObject &a, const QJsonObject &b) {
    for (const auto &key : {"device", "interface", "udi", "type"}) if (a.value(key) != b.value(key)) return false;
    return true;
}
bool target(const QJsonObject &receipt, const QJsonObject &state) {
    if (!sameDevice(receipt, state)) return false;
    if (receipt.value("action") == "disconnect") return state.value("state").toInt() == 30 && state.value("active") == "/";
    return state.value("state").toInt() == 100 && state.value("activeState").toInt() == 2 && state.value("uuid") == receipt.value("uuid")
        && !receipt.value("active").toString().isEmpty() && state.value("active") == receipt.value("active");
}
bool baseline(const QJsonObject &receipt, const QJsonObject &state) {
    return sameDevice(receipt, state) && state.value("state") == receipt.value("beforeState") && state.value("uuid") == receipt.value("beforeUuid")
        && (state.value("state").toInt() == 30 ? state.value("active") == "/" : state.value("activeState").toInt() == 2);
}
QJsonObject response(bool ok, const QString &outcome, QJsonObject data = {}, const QString &code = {}, const QString &message = {}, bool verified = false) {
    data["verificationScope"] = "NetworkManager-reported-device-state";
    data["internetReachabilityVerified"] = false;
    return {{"ok", ok}, {"outcome", outcome}, {"data", data}, {"errorCode", code}, {"error", message}, {"verified", verified}};
}
QJsonObject observe(Client &client, const QJsonObject &receipt) {
    const auto state = client.device(receipt.value("device").toString());
    require(sameDevice(receipt, state), "stale_target", "The device identity changed; no action was sent to the replacement.");
    return state;
}
QJsonObject poll(Client &client, const QJsonObject &receipt, bool restoring) {
    QElapsedTimer timer; timer.start();
    QJsonObject state;
    do {
        state = observe(client, receipt);
        if (restoring ? baseline(receipt, state) : target(receipt, state)) return state;
        QThread::msleep(100);
    } while (timer.elapsed() < 15000);
    throw Failure{"state_unconfirmed", "NetworkManager did not report the expected device state before the readback deadline."};
}
QJsonObject plan(Client &client, const QString &action, const QString &id, const QString &interface, const QString &ownedCheckpoint = {}) {
    const auto checkpoints = client.checkpoints();
    require(ownedCheckpoint.isEmpty() ? checkpoints.isEmpty() : checkpoints == QStringList{ownedCheckpoint}, "busy", "NetworkManager already has another checkpoint; finish it before starting another preview.");
    const auto deviceReply = client.call(Root, Manager, "GetDeviceByIpIface", {interface}, "o");
    const QString path = qvariant_cast<QDBusObjectPath>(deviceReply.arguments().first()).path();
    auto current = client.device(path);
    require(current.value("interface") == interface, "stale_target", "The interface changed while its device identity was being resolved.");
    const int state = current.value("state").toInt();
    require((state == 30 && current.value("active") == "/" && current.value("uuid").toString().isEmpty())
        || (state == 100 && current.value("activeState").toInt() == 2), "unavailable", "The selected device is not in a stable disconnected/activated state.");
    QJsonObject details = current;
    details["busId"] = client.busId; details["owner"] = client.owner;
    if (action == QLatin1String("activate")) {
        const auto reply = client.call(Root + "/Settings", Manager + ".Settings", "GetConnectionByUuid", {id}, "o");
        const QString profile = qvariant_cast<QDBusObjectPath>(reply.arguments().first()).path();
        require(objectPath(profile, "Settings") && !(client.number(profile, Manager + ".Settings.Connection", "Flags") & 14), "unsupported_configuration", "Only ordinary saved profiles are supported by this preview.");
        const auto settingsReply = client.call(profile, Manager + ".Settings.Connection", "GetSettings", {}, "a{sa{sv}}");
        const auto settings = qdbus_cast<Settings>(settingsReply.arguments().first());
        require(settings.size() <= 32 && !settings.contains("802-11-wireless-security") && !settings.contains("802-1x"),
            "interaction_required", "Secured Wi-Fi and 802.1X activation stay in the owning network panel; this CLI does not request secrets.");
        const auto connection = settings.value("connection");
        const QString type = connection.value("type").toString();
        require(connection.value("uuid").toString().compare(id, Qt::CaseInsensitive) == 0
            && (current.value("type").toInt() == 1 ? type == QLatin1String("802-3-ethernet") : type == QLatin1String("802-11-wireless"))
            && connection.value("master").toString().isEmpty() && connection.value("controller").toString().isEmpty()
            && connection.value("slave-type").toString().isEmpty() && connection.value("port-type").toString().isEmpty()
            && connection.value("secondaries").toStringList().isEmpty()
            && (connection.value("interface-name").toString().isEmpty() || connection.value("interface-name").toString() == interface),
            "unsupported_configuration", "The profile is incompatible with this simple single-device preview.");
        require(client.paths(path, Manager + ".Device", "AvailableConnections").contains(profile), "stale_target", "That profile is not currently available on the selected interface.");
        require(current.value("uuid").toString().compare(id, Qt::CaseInsensitive) != 0, "unchanged", "That profile is already active on the selected interface.");
        details["profile"] = profile;
    } else require(state == 100, "unchanged", "That interface is already disconnected.");
    return {{"ok", true}, {"operation", action == "activate" ? "network.activate" : "network.deactivate"}, {"target", interface},
        {"before", current.value("uuid")}, {"after", action == "activate" ? id : "disconnected"}, {"details", details},
        {"confirmText", "This preview can interrupt connectivity and end a remote session. NetworkManager will attempt rollback after 60 seconds unless you explicitly keep it."},
        {"recovery", "NetworkManager-checkpoint"}, {"effects", QJsonArray{"No unprotected fallback. No profile/secret writes. NetworkManager owns timed rollback; usability and internet access are unverified."}}};
}
}

bool NetworkPreview::validToken(const QString &token) { return matches(token, "^[a-f0-9]{32}$"); }
QJsonObject NetworkPreview::localReceipt(const Env &env, QString *error) {
    try { Store store{env}; const auto token = store.current(); return token.isEmpty() ? QJsonObject{} : store.read(token); }
    catch (const Failure &failure) { if (error) *error = failure.message; return {}; }
}
QString NetworkPreview::pendingReason(const Env &env) {
    try {
        Store store{env}; const auto token = store.current();
        if (token.isEmpty()) return {};
        const auto receipt = store.read(token);
        return terminal(receipt.value("phase").toString()) ? QString() : QString("A network preview is unresolved. Run network preview status before changing network state or restoring a profile.");
    } catch (const Failure &error) { return error.message; }
}

QJsonObject NetworkPreview::execute(const Env &env, const QStringList &args, bool dryRun, bool acknowledged) {
    const QString action = args.value(0);
    const bool starting = action == "activate" || action == "disconnect";
    const QString sub = args.value(1);
    Store store{env}; QJsonObject receipt;
    bool effect = false;
    try {
        require(starting || (action == "preview" && QStringList{"status", "keep", "revert"}.contains(sub)), "invalid_arguments", "Use network activate <uuid> <interface>, disconnect <interface>, or preview status|keep|revert [token].");
        if (starting) {
            require(args.size() == (action == "activate" ? 3 : 2), "invalid_arguments", "Activation requires a UUID and interface; disconnection requires an interface.");
            require((action != "activate" || uuid(args[1])) && NetworkProvider::isScanDeviceName(args.last()), "invalid_value", "Use a complete saved profile UUID and a current interface name.");
        } else {
            require((sub == "status" ? args.size() == 2 || args.size() == 3 : args.size() == 3) && !dryRun && (sub == "keep" || !acknowledged),
                "invalid_arguments", "Use preview status [token], keep <token> --yes, or revert <token>.");
            if (args.size() == 3) require(validToken(args[2]), "invalid_value", "Use the preview token returned by Control.");
        }
        ChangeCoordinator coordinator(env);
        QLockFile lock(env.appStateDir + "/change.lock"); lock.setStaleLockTime(5 * 60 * 1000);
        require(lock.tryLock(2000), "busy", "Another Control operation holds the change lock.");
        if (starting) require(DisplayPreview::pendingReason(env).isEmpty(), "display_preview_pending", DisplayPreview::pendingReason(env));
        require(coordinator.leaseRun().isEmpty() && !coordinator.leaseHeldElsewhere(), "busy", "A profile restore is running.");
        const auto current = store.current();
        if (starting) {
            require(pendingReason(env).isEmpty(), "busy", pendingReason(env));
            Client client(env);
            const QString id = action == "activate" ? args[1].toLower() : QString();
            const auto reviewed = plan(client, action, id, args.last());
            if (dryRun) return response(true, "planned", reviewed);
            require(acknowledged, "confirmation_required", reviewed.value("confirmText").toString());
            store.prepare(); store.prune();
            const auto device = reviewed.value("details").toObject();
            const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
            receipt = {{"version", 1}, {"token", token}, {"phase", "creating"}, {"action", action}, {"interface", args.last()}, {"uuid", id},
                {"device", device.value("device")}, {"udi", device.value("udi")}, {"type", device.value("type")}, {"beforeUuid", device.value("uuid")},
                {"beforeState", device.value("state")}, {"busId", client.busId}, {"owner", client.owner}, {"requestedBootMs", QString::number(bootMs())},
                {"checkpoint", ""}, {"createdBootMs", ""}, {"expiresBootMs", ""}, {"active", ""}, {"message", ""}};
            store.save(receipt); writeJson(store.path() + "/current.json", {{"token", token}});
            effect = true;
            const auto reply = client.call(Root, Manager, "CheckpointCreate", {QVariant::fromValue(QList<QDBusObjectPath>{QDBusObjectPath(device.value("device").toString())}), uint(Timeout), uint(0)}, "o", true);
            const QString cp = qvariant_cast<QDBusObjectPath>(reply.arguments().first()).path();
            require(objectPath(cp, "Checkpoint"), "request_unconfirmed", "Checkpoint creation returned an invalid path.");
            receipt["checkpoint"] = cp; store.save(receipt);
            const auto created = client.get(cp, Manager + ".Checkpoint", "Created");
            require(created.metaType().id() == QMetaType::LongLong && created.toLongLong() >= receipt.value("requestedBootMs").toString().toLongLong() - 1000
                && created.toLongLong() <= bootMs(), "request_unconfirmed", "Checkpoint creation time was not confirmed.");
            receipt["createdBootMs"] = QString::number(created.toLongLong()); receipt["expiresBootMs"] = QString::number(created.toLongLong() + Timeout * 1000);
            client.checkpoint(receipt);
            receipt["phase"] = "checkpoint"; store.save(receipt);
            require(baseline(receipt, observe(client, receipt)), "stale_target", "The device changed while its checkpoint was being prepared; no connection action was sent.");
            const auto fresh = plan(client, action, id, args.last(), cp).value("details").toObject();
            require(sameDevice(device, fresh) && device.value("uuid") == fresh.value("uuid") && device.value("active") == fresh.value("active")
                && device.value("profile") == fresh.value("profile"), "stale_target", "The selected connection changed while its checkpoint was being prepared.");
            client.checkpoint(receipt);
            require(bootMs() + 25000 < receipt.value("expiresBootMs").toString().toLongLong(), "preview_expiring", "Too little checkpoint time remains to begin a connection change safely.");
            receipt["phase"] = "applying"; store.save(receipt); client.resetBudget();
            if (action == "activate") {
                const auto activated = client.call(Root, Manager, "ActivateConnection", {QVariant::fromValue(QDBusObjectPath(device.value("profile").toString())), QVariant::fromValue(QDBusObjectPath(device.value("device").toString())), QVariant::fromValue(QDBusObjectPath("/"))}, "o", true);
                const QString active = qvariant_cast<QDBusObjectPath>(activated.arguments().first()).path();
                require(objectPath(active, "ActiveConnection"), "request_unconfirmed", "Activation returned an invalid active connection path.");
                receipt["active"] = active;
            } else client.call(Root, Manager, "DeactivateConnection", {QVariant::fromValue(QDBusObjectPath(device.value("active").toString()))}, "", true);
            receipt["phase"] = "applied"; store.save(receipt);
            const auto observed = poll(client, receipt, false);
            client.checkpoint(receipt);
            require(bootMs() < receipt.value("expiresBootMs").toString().toLongLong(), "preview_unconfirmed", "The preview expired before its result was confirmed.");
            receipt["phase"] = "preview"; store.save(receipt);
            return response(true, "preview", {{"receipt", receipt}, {"state", observed}, {"targetStateVerified", true}, {"requiresUserUsabilityConfirmation", true},
                {"remainingSeconds", qMax<qint64>(0, (receipt.value("expiresBootMs").toString().toLongLong() - bootMs()) / 1000)},
                {"nextStatus", QJsonArray{"network", "preview", "status", token}}, {"nextRevert", QJsonArray{"network", "preview", "revert", token}}});
        }
        const QString token = args.size() == 3 ? args[2] : current;
        if (token.isEmpty()) return response(true, "read", {{"phase", "none"}});
        receipt = store.read(token);
        QString phase = receipt.value("phase").toString();
        if (terminal(phase)) {
            const QJsonObject data{{"receipt", receipt}, {"historicalReceipt", true}};
            const bool confirmed = phase == "kept" || phase == "reverted";
            if (sub == "status") return response(confirmed, confirmed ? phase : "unknown", data, confirmed ? QString() : QString("preview_unconfirmed"), confirmed ? QString() : QString("This receipt does not confirm a keep or rollback outcome."));
            require((sub == "keep" && phase == "kept") || (sub == "revert" && phase == "reverted"), "state_conflict", "The preview is already terminal; no new network action was sent.");
            return response(true, phase, data);
        }
        require(current == token, "stale_token", "That token is not the current owned preview.");
        Client client(env);
        if (client.busId != receipt.value("busId").toString() || client.owner != receipt.value("owner").toString()) {
            if (sub == "status") { receipt["phase"] = "daemon_changed"; store.save(receipt); }
            return response(false, "unknown", {{"receipt", receipt}}, "stale_daemon", "The daemon changed; no keep/revert was sent. Current connectivity must be inspected.");
        }
        const auto checkpoints = client.checkpoints();
        const QString cp = receipt.value("checkpoint").toString();
        if (cp.isEmpty() || !checkpoints.contains(cp)) {
            // Retire metadata only after the full checkpoint window and a
            // fresh, pinned-daemon read proving no checkpoint remains. This
            // does not infer activation, rollback or usable connectivity.
            const bool expired = !receipt.value("expiresBootMs").toString().isEmpty() && bootMs() >= receipt.value("expiresBootMs").toString().toLongLong() + 5000;
            const bool creationExpired = phase == "creating" && receipt.value("expiresBootMs").toString().isEmpty()
                && bootMs() >= receipt.value("requestedBootMs").toString().toLongLong() + (Timeout + 5) * 1000LL;
            const bool retired = sub == "status" && (expired || creationExpired) && checkpoints.isEmpty();
            if (retired) { receipt["phase"] = "absent"; store.save(receipt); }
            QJsonObject data{{"receipt", receipt}, {"checkpointPresent", cp.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(false)},
                {"daemonHasCheckpoints", !checkpoints.isEmpty()}, {"localMetadataRetired", retired}, {"state", QJsonValue(QJsonValue::Null)}};
            try {
                const auto state = observe(client, receipt);
                data["state"] = state; data["baselineStateObserved"] = baseline(receipt, state); data["targetStateVerified"] = target(receipt, state);
            } catch (const Failure &error) { data["observationError"] = error.message; }
            return response(false, "unknown", data, "preview_unconfirmed", "The checkpoint is absent or its creation was interrupted. No rollback, keep or usability result is inferred.");
        }
        client.checkpoint(receipt);
        const auto observed = observe(client, receipt);
        if (sub == "status") {
            if (phase == "applied" && target(receipt, observed) && bootMs() + 5000 < receipt.value("expiresBootMs").toString().toLongLong()) {
                phase = "preview"; receipt["phase"] = phase; store.save(receipt);
            }
            return response(true, phase, {{"receipt", receipt}, {"state", observed}, {"checkpointPresent", true}, {"targetStateVerified", target(receipt, observed)},
                {"remainingSeconds", qMax<qint64>(0, (receipt.value("expiresBootMs").toString().toLongLong() - bootMs()) / 1000)}});
        }
        if (sub == "keep") {
            require(acknowledged, "confirmation_required", "Only keep after the user confirms this connection is usable. --yes records that confirmation; it does not test usability.");
            require(phase == "preview" && target(receipt, observed), "state_conflict", "Only a confirmed, unchanged preview can be kept. Inspect status or revert it.");
            require(bootMs() + 5000 < receipt.value("expiresBootMs").toString().toLongLong(), "preview_expiring", "Too little rollback time remains to safely keep this preview.");
            receipt["phase"] = "keeping"; store.save(receipt); effect = true;
            client.call(Root, Manager, "CheckpointDestroy", {QVariant::fromValue(QDBusObjectPath(cp))}, "", true);
            require(!client.checkpoints().contains(cp) && target(receipt, observe(client, receipt)), "state_unconfirmed", "Keep completion or the resulting device state could not be confirmed.");
            receipt["phase"] = "kept"; store.save(receipt);
            return response(true, "kept", {{"receipt", receipt}, {"userUsabilityAcknowledged", true}}, {}, {}, true);
        }
        require(target(receipt, observed) || baseline(receipt, observed) || (phase != "preview" && observed.value("uuid") == receipt.value("uuid")
            && (receipt.value("active").toString().isEmpty() || observed.value("active") == receipt.value("active"))),
            "state_conflict", "The device now has an unrelated connection. No explicit rollback was sent; NetworkManager's timer remains its owner.");
        receipt["phase"] = "reverting"; store.save(receipt); effect = true; client.resetBudget();
        const auto reply = client.call(Root, Manager, "CheckpointRollback", {QVariant::fromValue(QDBusObjectPath(cp))}, "a{su}", true);
        const auto results = qdbus_cast<Rollback>(reply.arguments().first());
        require(results.size() == 1 && results.contains(receipt.value("device").toString()) && results.value(receipt.value("device").toString()) == 0,
            "rollback_unconfirmed", "NetworkManager reported an incomplete or failed per-device rollback. Its checkpoint was not destroyed by Control.");
        const auto restored = poll(client, receipt, true);
        if (client.checkpoints().contains(cp)) { client.checkpoint(receipt); client.call(Root, Manager, "CheckpointDestroy", {QVariant::fromValue(QDBusObjectPath(cp))}, "", true); }
        require(!client.checkpoints().contains(cp) && baseline(receipt, observe(client, receipt)), "rollback_unconfirmed", "Rollback finalization could not be confirmed.");
        receipt["phase"] = "reverted"; store.save(receipt);
        return response(true, "reverted", {{"receipt", receipt}, {"state", restored}}, {}, {}, true);
    } catch (const Failure &error) {
        if (receipt.value("phase") == "creating" && receipt.value("checkpoint").toString().isEmpty()
            && (error.code == "authorization_denied" || error.code == "unsupported_configuration" || error.code == "operation_timeout")) {
            receipt["phase"] = "refused";
            receipt["message"] = error.message;
            effect = false;
            try { store.save(receipt); } catch (const Failure &) {}
        }
        if (effect && !receipt.isEmpty()) {
            receipt["message"] = error.message;
            // Do not invent a rollback when a writer's result is uncertain.
            try { store.save(receipt); } catch (const Failure &) {}
        }
        QJsonObject data;
        if (!receipt.isEmpty()) data = {{"receipt", receipt}, {"nextStatus", QJsonArray{"network", "preview", "status", receipt.value("token")}}, {"automaticRetry", false}};
        if (error.code == "interaction_required" && receipt.isEmpty()) data["nextAction"] = QJsonObject{{"argv", QJsonArray{"launch", "panel.network"}}, {"apiVersions", QJsonArray{}}};
        return response(false, effect ? "unknown" : "blocked", data, error.code, error.message);
    }
}
