#include "windowrulesprovider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>

#include "hypromodule.h"
#include "hyprchange.h"

WindowRulesProvider::WindowRulesProvider(CommandRunner *runner, ChangeCoordinator *coordinator, QObject *parent)
    : QObject(parent), m_runner(runner), m_coordinator(coordinator) {}

QVariantList WindowRulesProvider::rules() const {
    HyprModule module(m_runner->env());
    QVariantList rows;
    if (!module.load())
        return rows;
    for (const auto &value : module.rules())
        rows << value.toObject().toVariantMap();
    return rows;
}

QJsonObject WindowRulesProvider::snapshot() const {
    auto snapshot = HyprModule(m_runner->env()).inspect();
    snapshot.remove("binds");
    if (snapshot.value("rules").isArray()) snapshot["rules"] = HyprChange::selectable(snapshot, "rules");
    snapshot["scope"] = "Owned module only; rules in other Lua files and effective window behavior are not inspected.";
    return snapshot;
}

QJsonObject WindowRulesProvider::constraints() {
    return {{"match", QJsonObject{{"type", "string"}, {"minimumLength", 1}, {"maximumLength", 120},
                                 {"normalization", "trim surrounding whitespace; control characters are rejected"},
                                 {"validation", "Qt regular-expression syntax only; compositor acceptance and app matching are not established"}}},
            {"actions", QJsonArray{
                QJsonObject{{"action", "float"}, {"valueRequired", false}, {"valuePattern", "\\A\\z"}},
                QJsonObject{{"action", "tile"}, {"valueRequired", false}, {"valuePattern", "\\A\\z"}},
                QJsonObject{{"action", "noblur"}, {"valueRequired", false}, {"valuePattern", "\\A\\z"}},
                QJsonObject{{"action", "workspace"}, {"valueRequired", true}, {"valuePattern", "\\A([0-9]{1,3}|special(:[A-Za-z0-9_-]{1,32})?)\\z"},
                            {"examples", QJsonArray{"3", "special", "special:notes"}}},
                QJsonObject{{"action", "opacity"}, {"valueRequired", true}, {"valuePattern", "\\A(0(\\.[0-9]{1,3})?|1(\\.0{1,3})?)( (0(\\.[0-9]{1,3})?|1(\\.0{1,3})?))?\\z"},
                            {"examples", QJsonArray{"0.9", "0.9 0.85"}}}}},
            {"verificationScope", "input-validation-only"},
            {"effects", QJsonArray{"No files are read or changed, and Hyprland is not contacted or reloaded."}}};
}

QString WindowRulesProvider::check(const QString &match, const QString &action, const QString &value) {
    if (match.trimmed().isEmpty() || match.size() > 120)
        return tr("Enter the app's window class, which is up to 120 characters.");
    for (const QChar c : match)
        if (c.category() == QChar::Other_Control)
            return tr("That value contains characters that are not allowed.");
    if (!QRegularExpression(match.trimmed()).isValid())
        return tr("That is not a valid pattern.");
    for (const auto &item : constraints().value("actions").toArray()) {
        const auto schema = item.toObject();
        if (schema.value("action").toString() != action) continue;
        if (QRegularExpression(schema.value("valuePattern").toString()).match(value).hasMatch()) return {};
        if (action == QLatin1String("workspace")) return tr("Enter a workspace number.");
        if (action == QLatin1String("opacity")) return tr("Enter an opacity from 0 to 1, or two values for active and inactive.");
        return tr("That rule type does not take a value.");
    }
    return tr("That rule type is not available.");
}


void WindowRulesProvider::reject(const ProviderPlan &plan) {
    Transaction tx; tx.settingId = "windowrules"; tx.phase = "failed"; tx.message = plan.error;
    emit message(plan.error, true);
    emit operationFinished(tx);
}

void WindowRulesProvider::planAdd(const QString &match, const QString &action, const QString &value, std::function<void(ProviderPlan)> done) {
    const QString error = check(match, action, value);
    if (!error.isEmpty()) { ProviderPlan plan; plan.operation = "rules.add"; plan.error = error; done(plan); return; }
    auto plan = HyprChange::base(m_runner->env(), "rules.add", match.trimmed(), "rules");
    if (!plan.ok) { done(plan); return; }
    auto rows = plan.details.value("entriesBefore").toArray();
    const QJsonObject record{{"match", match.trimmed()}, {"action", action}, {"value", value}};
    plan.details["record"] = record;
    plan.before = QString::number(rows.size()) + " owned rules";
    rows.append(record); plan.details["entriesAfter"] = rows;
    plan.after = match.trimmed() + " " + action + " " + value;
    plan.effects << tr("The rule applies to windows opened later. Reload success does not establish matching or behavior of an application window.");
    HyprChange::runtime(m_runner, this, plan, done);
}

void WindowRulesProvider::planRemove(const QString &selector, std::function<void(ProviderPlan)> done) {
    auto plan = HyprChange::base(m_runner->env(), "rules.remove", selector, "rules");
    if (!plan.ok) { done(plan); return; }
    const int index = HyprChange::selected(plan, selector);
    if (index < 0) { plan.ok = false; plan.errorCode = "stale_target"; plan.error = tr("That rule selector is invalid or stale. Read rule list again."); done(plan); return; }
    auto rows = plan.details.value("entriesBefore").toArray();
    plan.details["record"] = rows.at(index); rows.removeAt(index);
    plan.details["entriesAfter"] = rows; plan.before = selector; plan.after = "removed";
    HyprChange::runtime(m_runner, this, plan, done);
}

void WindowRulesProvider::applyPlan(const ProviderPlan &plan) {
    if (!plan.ok) { reject(plan); return; }
    HyprChange::submit(m_runner, m_coordinator, this, plan, {}, {}, [this, plan](const Transaction &tx) {
        emit message(tx.phase == QLatin1String("committed") ? (plan.operation == QLatin1String("rules.add") ? tr("Rule saved. It applies to windows opened from now on.") : tr("Rule removed")) : tx.message,
                     tx.phase != QLatin1String("committed"));
        emit operationFinished(tx);
        emit changed();
    });
}

void WindowRulesProvider::addRule(const QString &match, const QString &action, const QString &value) {
    planAdd(match, action, value, [this](const ProviderPlan &plan) { applyPlan(plan); });
}

void WindowRulesProvider::removeRule(const QVariant &selection) {
    if (selection.metaType().id() == QMetaType::QString) {
        planRemove(selection.toString(), [this](const ProviderPlan &plan) { applyPlan(plan); });
        return;
    }
    const int index = selection.toInt();
    const auto rows = HyprChange::selectable(HyprModule(m_runner->env()).inspect(), "rules");
    const QString selector = index < 0 || index >= rows.size() ? QString() : rows.at(index).toObject().value("selector").toString();
    planRemove(selector, [this](const ProviderPlan &plan) { applyPlan(plan); });
}
