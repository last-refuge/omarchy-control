#include "windowrulesprovider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>

#include "hypromodule.h"

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

QString WindowRulesProvider::check(const QString &match, const QString &action, const QString &value) {
    if (match.trimmed().isEmpty() || match.size() > 120)
        return tr("Enter the app's window class, which is up to 120 characters.");
    for (const QChar c : match)
        if (c.category() == QChar::Other_Control)
            return tr("That value contains characters that are not allowed.");
    if (!QRegularExpression(match).isValid())
        return tr("That is not a valid pattern.");
    static const QRegularExpression workspace("^([0-9]{1,3}|special(:[A-Za-z0-9_-]{1,32})?)$");
    static const QRegularExpression opacity("^(0(\\.[0-9]{1,3})?|1(\\.0{1,3})?)( (0(\\.[0-9]{1,3})?|1(\\.0{1,3})?))?$");
    if (action == QLatin1String("float") || action == QLatin1String("tile") || action == QLatin1String("noblur"))
        return {};
    if (action == QLatin1String("workspace"))
        return workspace.match(value).hasMatch() ? QString() : tr("Enter a workspace number.");
    if (action == QLatin1String("opacity"))
        return opacity.match(value).hasMatch() ? QString() : tr("Enter an opacity from 0 to 1, or two values for active and inactive.");
    return tr("That rule type is not available.");
}

void WindowRulesProvider::submit(const QString &label, std::function<bool(HyprModule &, QString *)> edit, const QString &success) {
    HyprModule probe(m_runner->env());
    ChangeCoordinator::Request request;
    request.settingId = "windowrules";
    request.label = label;
    request.files << probe.path() << probe.hyprlandPath();
    auto baseline = std::make_shared<QStringList>();
    request.apply = [this, edit, baseline](ChangeCoordinator::Done done) {
        m_runner->run(m_runner->env().hyprctl({"-j", "configerrors"}), CommandOptions{3000}, this, [this, edit, baseline, done](const CommandResult &errors) {
            for (const auto &item : QJsonDocument::fromJson(errors.out).array())
                if (!item.toString().trimmed().isEmpty())
                    baseline->append(item.toString().trimmed());
            HyprModule module(m_runner->env());
            QString error;
            if (!module.load(&error) || !edit(module, &error) || !module.save(&error) || !module.installImport(&error)) {
                done(false, error);
                return;
            }
            CommandOptions options;
            options.mutation = true;
            m_runner->run(m_runner->env().hyprctl({"reload"}), options, this, [done](const CommandResult &result) { done(result.ok(), result.describe("Hyprland")); });
        });
    };
    // Hyprland has no query for a Lua window rule, so a clean reload is what can be
    // established. The message says what that does and does not prove.
    request.verify = [this, baseline](ChangeCoordinator::Done done) {
        QTimer::singleShot(150, this, [this, baseline, done] {
            m_runner->run(m_runner->env().hyprctl({"-j", "configerrors"}), CommandOptions{3000}, this, [this, baseline, done](const CommandResult &errors) {
                if (!errors.ok()) {
                    done(false, errors.describe("Hyprland"));
                    return;
                }
                for (const auto &item : QJsonDocument::fromJson(errors.out).array()) {
                    const QString text = item.toString().trimmed();
                    if (!text.isEmpty() && !baseline->contains(text)) {
                        done(false, tr("Hyprland reported a config error: %1").arg(text.left(240)));
                        return;
                    }
                }
                done(true, {});
            });
        });
    };
    request.rollback = [this](ChangeCoordinator::Done done) {
        CommandOptions options;
        options.mutation = true;
        m_runner->run(m_runner->env().hyprctl({"reload"}), options, this, [done](const CommandResult &) { done(true, {}); });
    };
    m_coordinator->submit(std::move(request), [this, success](const Transaction &tx) {
        emit message(tx.phase == QLatin1String("committed") ? success : tx.message, tx.phase != QLatin1String("committed"));
        emit changed();
    });
}

void WindowRulesProvider::addRule(const QString &match, const QString &action, const QString &value) {
    const QString problem = validate(match, action, value);
    if (!problem.isEmpty()) {
        emit message(problem, true);
        return;
    }
    const QJsonObject rule{{"match", match.trimmed()}, {"action", action}, {"value", value}};
    submit(tr("Window rule for %1").arg(match.trimmed()),
           [rule](HyprModule &module, QString *) {
               QJsonArray rules = module.rules();
               rules.append(rule);
               module.setRules(rules);
               return true;
           },
           tr("Rule saved. It applies to windows opened from now on."));
}

void WindowRulesProvider::removeRule(int index) {
    submit(tr("Window rule removed"),
           [index](HyprModule &module, QString *error) {
               QJsonArray rules = module.rules();
               if (index < 0 || index >= rules.size()) {
                   *error = tr("That rule no longer exists.");
                   return false;
               }
               rules.removeAt(index);
               module.setRules(rules);
               return true;
           },
           tr("Rule removed"));
}
