#include "fileops.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

QString FileOps::emptyMarkerError(const QString &homePath, const QString &path) {
    const QString home = QDir::cleanPath(homePath);
    if (QDir::cleanPath(path) != path || !path.startsWith(home + '/'))
        return QStringLiteral("The preference marker is outside the user's home.");
    const QStringList parts = path.mid(home.size() + 1).split('/');
    QString current = home;
    for (int i = -1; i < parts.size(); ++i) {
        if (i >= 0) current += '/' + parts[i];
        struct stat info{};
        if (::lstat(QFile::encodeName(current).constData(), &info) != 0) {
            if (errno == ENOENT) continue;
            return QStringLiteral("The preference marker path could not be inspected.");
        }
        const bool leaf = i == parts.size() - 1;
        if (info.st_uid != getuid() || (info.st_mode & 0022) || S_ISLNK(info.st_mode)
            || (leaf ? !S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size != 0 : !S_ISDIR(info.st_mode)))
            return QStringLiteral("The preference marker or an ancestor is linked, shared, or contains custom content; it was left alone.");
    }
    return {};
}

namespace {

QString tr(const char *text) {
    return QCoreApplication::translate("FileOps", text);
}

// A value's position in the source text, found by walking the raw bytes so a
// patch never reserializes (and thereby reorders or strips) anything else.
struct Scanner {
    const QByteArray &s;
    int i = 0;
    QString error;

    void ws() {
        while (i < s.size()) {
            const char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i;
            } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n')
                    ++i;
            } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
                const int end = s.indexOf("*/", i + 2);
                i = end < 0 ? s.size() : end + 2;
            } else {
                return;
            }
        }
    }

    bool string(QString *out = nullptr) {
        if (i >= s.size() || s[i] != '"')
            return fail();
        const int begin = i++;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\')
                ++i;
            ++i;
        }
        if (i >= s.size())
            return fail();
        ++i;
        if (out) {
            const QByteArray wrapped = "[" + s.mid(begin, i - begin) + "]";
            *out = QJsonDocument::fromJson(wrapped).array().at(0).toString();
        }
        return true;
    }

    bool value() {
        ws();
        if (i >= s.size())
            return fail();
        const char c = s[i];
        if (c == '"')
            return string();
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            ++i;
            for (;;) {
                ws();
                if (i >= s.size())
                    return fail();
                if (s[i] == close) {
                    ++i;
                    return true;
                }
                if (s[i] == ',') {
                    ++i;
                    continue;
                }
                if (c == '{') {
                    if (!string())
                        return false;
                    ws();
                    if (i >= s.size() || s[i] != ':')
                        return fail();
                    ++i;
                }
                if (!value())
                    return false;
            }
        }
        const int begin = i;
        while (i < s.size() && !strchr(",}] \t\r\n/", s[i]))
            ++i;
        return i > begin || fail();
    }

    bool fail() {
        if (error.isEmpty())
            error = tr("The file is not valid JSON near byte %1.").arg(i);
        return false;
    }
};

struct Member {
    int keyBegin = -1, valueBegin = -1, valueEnd = -1;
};

// Finds `key` in the object starting at scanner.i and records member spans.
bool findMember(Scanner &scan, const QString &key, Member *found, int *closingBrace, int *lastValueEnd, bool *empty) {
    scan.ws();
    if (scan.i >= scan.s.size() || scan.s[scan.i] != '{')
        return scan.fail();
    ++scan.i;
    *empty = true;
    *lastValueEnd = scan.i;
    for (;;) {
        scan.ws();
        if (scan.i >= scan.s.size())
            return scan.fail();
        if (scan.s[scan.i] == '}') {
            *closingBrace = scan.i;
            return true;
        }
        if (scan.s[scan.i] == ',') {
            ++scan.i;
            continue;
        }
        const int keyBegin = scan.i;
        QString name;
        if (!scan.string(&name))
            return false;
        scan.ws();
        if (scan.i >= scan.s.size() || scan.s[scan.i] != ':')
            return scan.fail();
        ++scan.i;
        scan.ws();
        const int valueBegin = scan.i;
        if (!scan.value())
            return false;
        *empty = false;
        *lastValueEnd = scan.i;
        if (name == key && found->keyBegin < 0) {
            found->keyBegin = keyBegin;
            found->valueBegin = valueBegin;
            found->valueEnd = scan.i;
        }
    }
}

// The indentation the surrounding object already uses, so inserted members look
// hand-written and diffs stay one line.
QByteArray indentOf(const QByteArray &s, int position) {
    int line = s.lastIndexOf('\n', position - 1) + 1;
    int end = line;
    while (end < s.size() && (s[end] == ' ' || s[end] == '\t'))
        ++end;
    return s.mid(line, end - line);
}

}  // namespace

namespace FileOps {

bool readFile(const QString &path, QByteArray *contents, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    *contents = file.readAll();
    return true;
}

bool atomicWrite(const QString &path, const QByteArray &contents, QString *error, int newFileMode) {
    QFileInfo info(path);
    QString target = path;
    if (info.isSymLink()) {
        target = info.symLinkTarget();
        if (!QFileInfo(target).isAbsolute())
            target = info.absoluteDir().absoluteFilePath(target);
    }
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
        if (error)
            *error = tr("The folder for %1 could not be created.").arg(target);
        return false;
    }
    const bool existed = QFileInfo::exists(target);

    QSaveFile save(target);
    save.setDirectWriteFallback(false);
    if (!save.open(QIODevice::WriteOnly)) {
        if (error)
            *error = save.errorString();
        return false;
    }
    if (save.write(contents) != contents.size()) {
        if (error)
            *error = save.errorString();
        save.cancelWriting();
        return false;
    }
    if (!save.commit()) {
        if (error)
            *error = save.errorString();
        return false;
    }
    // QSaveFile keeps an existing file's mode; a new file has none to keep.
    if (!existed) {
        QFile::Permissions permissions;
        if (newFileMode & 0400) permissions |= QFile::ReadOwner;
        if (newFileMode & 0200) permissions |= QFile::WriteOwner;
        if (newFileMode & 0100) permissions |= QFile::ExeOwner;
        if (newFileMode & 0040) permissions |= QFile::ReadGroup;
        if (newFileMode & 0020) permissions |= QFile::WriteGroup;
        if (newFileMode & 0004) permissions |= QFile::ReadOther;
        QFile::setPermissions(target, permissions);
    }
    return true;
}

QString fingerprintBytes(const QByteArray &contents) {
    return QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
}

QString fingerprint(const QString &path) {
    if (!QFileInfo::exists(path))
        return QStringLiteral("missing");
    // Unreadable is not absent: treating it as absent would let a rollback delete a
    // file this app merely could not read.
    QByteArray contents;
    if (!readFile(path, &contents))
        return QStringLiteral("unreadable");
    return fingerprintBytes(contents);
}

QByteArray stripJsonComments(const QByteArray &source) {
    QByteArray out;
    out.reserve(source.size());
    int i = 0;
    while (i < source.size()) {
        const char c = source[i];
        if (c == '"') {
            const int begin = i++;
            while (i < source.size() && source[i] != '"') {
                if (source[i] == '\\')
                    ++i;
                ++i;
            }
            ++i;
            out.append(source.mid(begin, i - begin));
        } else if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            while (i < source.size() && source[i] != '\n')
                ++i;
        } else if (c == '/' && i + 1 < source.size() && source[i + 1] == '*') {
            const int end = source.indexOf("*/", i + 2);
            i = end < 0 ? source.size() : end + 2;
        } else {
            out.append(c);
            ++i;
        }
    }
    // Trailing commas are legal in JSONC and fatal to QJsonDocument. This walks the
    // text so a comma inside a string, like "a,}", is left alone.
    QByteArray cleaned;
    cleaned.reserve(out.size());
    for (int at = 0; at < out.size(); ++at) {
        const char c = out[at];
        if (c == '"') {
            const int begin = at++;
            while (at < out.size() && out[at] != '"') {
                if (out[at] == '\\')
                    ++at;
                ++at;
            }
            cleaned.append(out.mid(begin, at - begin + 1));
            continue;
        }
        if (c == ',') {
            int next = at + 1;
            while (next < out.size() && (out[next] == ' ' || out[next] == '\t' || out[next] == '\n' || out[next] == '\r'))
                ++next;
            if (next < out.size() && (out[next] == '}' || out[next] == ']'))
                continue;
        }
        cleaned.append(c);
    }
    return cleaned;
}

QByteArray jsonLiteral(const QJsonValue &value) {
    const QByteArray wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return wrapped.mid(1, wrapped.size() - 2);
}

static bool locate(const QByteArray &doc, const QStringList &path, int depthLimit, QList<Member> *members, int *containerStart,
                   Member *leaf, int *closing, int *lastEnd, bool *empty, QString *error) {
    Scanner scan{doc};
    scan.ws();
    int start = scan.i;
    for (int depth = 0; depth < depthLimit; ++depth) {
        scan.i = start;
        Member member;
        int close = -1, last = -1;
        bool none = true;
        if (!findMember(scan, path[depth], &member, &close, &last, &none)) {
            if (error)
                *error = scan.error;
            return false;
        }
        *containerStart = start;
        *closing = close;
        *lastEnd = last;
        *empty = none;
        *leaf = member;
        members->append(member);
        if (depth + 1 < depthLimit) {
            if (member.keyBegin < 0)
                return true;
            start = member.valueBegin;
        }
    }
    return true;
}

bool jsonSet(QByteArray *document, const QStringList &path, const QJsonValue &value, QString *error) {
    if (path.isEmpty())
        return false;
    QByteArray doc = document->isEmpty() ? QByteArray("{\n}\n") : *document;

    for (int depth = 1; depth <= path.size(); ++depth) {
        QList<Member> members;
        Member leaf;
        int containerStart = 0, closing = -1, lastEnd = -1;
        bool empty = true;
        if (!locate(doc, path, depth, &members, &containerStart, &leaf, &closing, &lastEnd, &empty, error))
            return false;
        if (members.size() < depth)
            continue;

        const bool isLeaf = depth == path.size();
        if (leaf.keyBegin >= 0) {
            if (isLeaf) {
                doc.replace(leaf.valueBegin, leaf.valueEnd - leaf.valueBegin, jsonLiteral(value));
                *document = doc;
                return true;
            }
            if (doc[leaf.valueBegin] != '{') {
                if (error)
                    *error = tr("%1 is not an object, so it was left alone.").arg(path.mid(0, depth).join('.'));
                return false;
            }
            continue;
        }

        // Build the missing remainder of the path as one member.
        QJsonValue inserted = value;
        for (int i = path.size() - 1; i >= depth; --i)
            inserted = QJsonObject{{path[i], inserted}};
        const QByteArray closeIndent = indentOf(doc, closing);
        const QByteArray memberIndent = empty ? closeIndent + "  " : indentOf(doc, lastEnd);
        const QByteArray literal = "\"" + path[depth - 1].toUtf8() + "\": " + jsonLiteral(inserted);
        if (empty) {
            // An "empty" object can still hold comments, as Omarchy's menu
            // extension template does; keep them and insert just before the brace.
            int lineStart = closing;
            while (lineStart > containerStart + 1 && (doc[lineStart - 1] == ' ' || doc[lineStart - 1] == '\t'))
                --lineStart;
            if (doc[lineStart - 1] == '\n')
                doc.replace(lineStart, closing - lineStart, memberIndent + literal + "\n" + closeIndent);
            else if (doc.mid(containerStart + 1, closing - containerStart - 1).trimmed().isEmpty())
                doc.replace(containerStart + 1, closing - containerStart - 1, "\n" + memberIndent + literal + "\n" + closeIndent);
            else
                doc.insert(closing, "\n" + memberIndent + literal + "\n" + closeIndent);
        } else {
            doc.insert(lastEnd, ",\n" + memberIndent + literal);
        }
        *document = doc;
        return true;
    }
    return false;
}

bool jsonRemove(QByteArray *document, const QStringList &path, QString *error) {
    if (path.isEmpty())
        return false;
    QByteArray doc = *document;
    QList<Member> members;
    Member leaf;
    int containerStart = 0, closing = -1, lastEnd = -1;
    bool empty = true;
    if (!locate(doc, path, path.size(), &members, &containerStart, &leaf, &closing, &lastEnd, &empty, error))
        return false;
    if (members.size() < path.size() || leaf.keyBegin < 0)
        return true;

    // Remove the member together with one neighbouring comma.
    int begin = leaf.keyBegin, end = leaf.valueEnd;
    Scanner after{doc, end};
    after.ws();
    if (after.i < doc.size() && doc[after.i] == ',') {
        end = after.i + 1;
    } else {
        int back = begin - 1;
        while (back > containerStart && (doc[back] == ' ' || doc[back] == '\t' || doc[back] == '\n' || doc[back] == '\r'))
            --back;
        if (doc[back] == ',')
            begin = back;
    }
    // Take the rest of the line too when nothing else shares it.
    int lineStart = doc.lastIndexOf('\n', begin - 1) + 1;
    if (doc.mid(lineStart, begin - lineStart).trimmed().isEmpty() && doc[begin] != ',')
        begin = lineStart;
    int lineEnd = doc.indexOf('\n', end);
    if (lineEnd >= 0 && doc.mid(end, lineEnd - end).trimmed().isEmpty())
        end = doc[begin] == ',' ? end : lineEnd + 1;
    doc.remove(begin, end - begin);
    *document = doc;
    return true;
}

QJsonValue jsonGet(const QByteArray &document, const QStringList &path) {
    QJsonValue current = QJsonDocument::fromJson(stripJsonComments(document)).object();
    for (const QString &key : path) {
        if (!current.isObject())
            return QJsonValue(QJsonValue::Undefined);
        current = current.toObject().value(key);
    }
    return current;
}

static QString stripTomlValue(QString value) {
    value = value.trimmed();
    if (value.startsWith('"')) {
        const int end = value.indexOf('"', 1);
        return end > 0 ? value.mid(1, end - 1) : value;
    }
    const int comment = value.indexOf('#');
    if (comment >= 0)
        value = value.left(comment).trimmed();
    return value;
}

QString tomlGet(const QByteArray &document, const QString &section, const QString &key) {
    bool inSection = section.isEmpty();
    const QStringList lines = QString::fromUtf8(document).split('\n');
    static const QRegularExpression pair(R"(^\s*([A-Za-z0-9_.-]+)\s*=\s*(.*)$)");
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith('[')) {
            inSection = trimmed.startsWith("[" + section + "]");
            continue;
        }
        if (!inSection)
            continue;
        const auto match = pair.match(line);
        if (match.hasMatch() && match.captured(1) == key)
            return stripTomlValue(match.captured(2));
    }
    return {};
}

namespace {

// Where one occurrence of the key's value sits in the document.
struct IniSpan {
    qsizetype start = 0;
    qsizetype length = 0;
};

QList<IniSpan> iniSpans(const QByteArray &document, const QByteArray &group, const QByteArray &key) {
    QList<IniSpan> spans;
    QByteArray current;
    qsizetype position = 0;
    while (position < document.size()) {
        qsizetype end = document.indexOf('\n', position);
        if (end < 0)
            end = document.size();
        const QByteArray line = document.mid(position, end - position);
        const QByteArray trimmed = line.trimmed();
        if (trimmed.startsWith('[') && trimmed.endsWith(']')) {
            current = trimmed.mid(1, trimmed.size() - 2).trimmed();
        } else if (current == group && !trimmed.startsWith('#') && !trimmed.startsWith(';')) {
            const qsizetype equals = line.indexOf('=');
            if (equals > 0 && line.left(equals).trimmed() == key) {
                // The value, without the spaces around it or a CRLF's carriage return.
                qsizetype start = equals + 1;
                qsizetype stop = line.size();
                while (start < stop && (line[start] == ' ' || line[start] == '\t'))
                    ++start;
                while (stop > start && (line[stop - 1] == ' ' || line[stop - 1] == '\t' || line[stop - 1] == '\r'))
                    --stop;
                spans << IniSpan{position + start, stop - start};
            }
        }
        position = end + 1;
    }
    return spans;
}

}  // namespace

IniValue iniGet(const QByteArray &document, const QString &group, const QString &key) {
    const QList<IniSpan> spans = iniSpans(document, group.toUtf8(), key.toUtf8());
    IniValue result;
    result.found = !spans.isEmpty();
    result.ambiguous = spans.size() > 1;
    if (!spans.isEmpty())
        result.value = QString::fromUtf8(document.mid(spans.first().start, spans.first().length));
    return result;
}

bool iniSet(QByteArray *document, const QString &group, const QString &key, const QString &value, QString *error) {
    const QList<IniSpan> spans = iniSpans(*document, group.toUtf8(), key.toUtf8());
    if (spans.size() != 1) {
        if (error)
            *error = spans.isEmpty() ? QStringLiteral("The key %1 is not in this file.").arg(key)
                                     : QStringLiteral("The key %1 appears more than once in this file.").arg(key);
        return false;
    }
    document->replace(spans.first().start, spans.first().length, value.toUtf8());
    return true;
}

bool iniUpsert(QByteArray *document, const QString &group, const QString &key, const QString &value, QString *error) {
    const QList<IniSpan> spans = iniSpans(*document, group.toUtf8(), key.toUtf8());
    if (spans.size() > 1) {
        if (error)
            *error = QStringLiteral("The key %1 appears more than once in this file.").arg(key);
        return false;
    }
    if (spans.size() == 1)
        return iniSet(document, group, key, value, error);
    // Matches the file's own line endings, so a CRLF file stays CRLF.
    const QByteArray newline = document->contains("\r\n") ? "\r\n" : "\n";
    const QByteArray line = key.toUtf8() + "=" + value.toUtf8() + newline;
    const QByteArray wanted = group.toUtf8();
    // Where the group's last non-blank line ends, so the key joins the group it belongs to.
    qsizetype insertAt = -1;
    bool inGroup = false;
    qsizetype position = 0;
    while (position < document->size()) {
        qsizetype end = document->indexOf('\n', position);
        const qsizetype next = end < 0 ? document->size() : end + 1;
        const QByteArray trimmed = document->mid(position, (end < 0 ? document->size() : end) - position).trimmed();
        if (trimmed.startsWith('[') && trimmed.endsWith(']')) {
            inGroup = trimmed.mid(1, trimmed.size() - 2).trimmed() == wanted;
            if (inGroup)
                insertAt = next;
        } else if (inGroup && !trimmed.isEmpty()) {
            insertAt = next;
        }
        position = next;
    }
    if (insertAt >= 0) {
        // A group that ends the file without a final newline needs one before the new line.
        if (insertAt == document->size() && !document->endsWith('\n')) {
            document->append(newline);
            insertAt = document->size();
        }
        document->insert(insertAt, line);
        return true;
    }
    if (!document->isEmpty() && !document->endsWith('\n'))
        document->append(newline);
    if (!document->isEmpty())
        document->append(newline);
    document->append("[" + wanted + "]" + newline + line);
    return true;
}

}  // namespace FileOps
