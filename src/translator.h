#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonValue>
#include <QLocale>
#include <QTimer>
#include <QTranslator>

// Follows the language picked under Omarchy's Setup > Language. Catalogs are
// flat JSON objects from English source text to translation, one per Omarchy
// language code (i18n/da.json, i18n/pt-BR.json), so QML keeps plain
// qsTr("Save") and C++ keeps tr("Save"). A counted string,
// qsTr("%n clips", "", count), maps to an object of plural forms keyed by
// CLDR category ({"one": "%n klip", "other": "%n klip"}).
//
// en-XA is a pseudo-language for finding layout problems before a real
// translation exists: every string comes out accented, about 40% longer, and
// bracketed, so clipped, overlapping, or untranslated text stands out. It is not
// in Omarchy's registry, so the language picker never offers it; write it to the
// language file by hand, in a sandbox or for screenshots.
class Translator : public QTranslator {
    Q_OBJECT
    Q_PROPERTY(QString language READ language NOTIFY changed)

public:
    explicit Translator(const QString &languageFile = {}, const QString &catalogDirectory = ":/i18n",
                        const QString &registryFile = {}, QObject *parent = nullptr);

    QString language() const { return m_language; }
    bool isEmpty() const override { return m_strings.isEmpty(); }
    QString translate(const char *context, const char *sourceText, const char *disambiguation = nullptr,
                      int n = -1) const override;

    int translatedCount() const { return m_strings.size(); }

    static QString readLanguage(const QString &path);
    static QString pluralCategory(const QString &language, long long count);
    static constexpr const char *PseudoLanguage = "en-XA";
    static QString pseudo(const QString &text);
    QLocale formatLocale(const QString &language) const;

signals:
    void changed();

private:
    void reload();

    QString m_languageFile;
    QString m_catalogDirectory;
    QString m_registryFile;
    QString m_language;
    QHash<QString, QJsonValue> m_strings;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
