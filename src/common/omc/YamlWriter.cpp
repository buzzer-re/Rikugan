#include "common/omc/YamlWriter.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace rk {

namespace {

QByteArray scalar(const QJsonValue& v) {
    switch (v.type()) {
    case QJsonValue::Bool: return v.toBool() ? "true" : "false";
    case QJsonValue::Double: {
        const double d = v.toDouble();
        if (d == double(qint64(d))) return QByteArray::number(qint64(d));
        return QByteArray::number(d, 'g', 17);
    }
    case QJsonValue::String: {
        // a JSON string literal is a valid YAML double-quoted scalar
        QByteArray s = QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
        return s.mid(1, s.size() - 2);
    }
    default: return "null";
    }
}

QByteArray key(const QString& k) {
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z_][A-Za-z0-9_.-]*$"));
    return plain.match(k).hasMatch() ? k.toUtf8() : scalar(QJsonValue(k));
}

void emit(QByteArray& out, const QJsonValue& v, int indent);

void emitObject(QByteArray& out, const QJsonObject& o, int indent) {
    const QByteArray pad(indent, ' ');
    for (auto it = o.begin(); it != o.end(); ++it) {
        out += pad + key(it.key()) + ':';
        const QJsonValue& v = it.value();
        if (v.isObject() && !v.toObject().isEmpty()) {
            out += '\n';
            emitObject(out, v.toObject(), indent + 2);
        } else if (v.isArray() && !v.toArray().isEmpty()) {
            out += '\n';
            emit(out, v, indent + 2);
        } else if (v.isObject()) {
            out += " {}\n";
        } else if (v.isArray()) {
            out += " []\n";
        } else {
            out += ' ' + scalar(v) + '\n';
        }
    }
}

void emit(QByteArray& out, const QJsonValue& v, int indent) {
    const QByteArray pad(indent, ' ');
    if (v.isObject()) {
        emitObject(out, v.toObject(), indent);
        return;
    }
    for (const QJsonValue& item : v.toArray()) {
        if (item.isObject() && !item.toObject().isEmpty()) {
            // "- " then the first key on the same line, the rest aligned under it
            QByteArray sub;
            emitObject(sub, item.toObject(), indent + 2);
            sub.replace(0, indent + 2, pad + "- ");
            out += sub;
        } else if (item.isArray() && !item.toArray().isEmpty()) {
            out += pad + "-\n";
            emit(out, item, indent + 2);
        } else {
            out += pad + "- " + (item.isObject() ? QByteArray("{}") : item.isArray() ? QByteArray("[]") : scalar(item)) + '\n';
        }
    }
}

}  // namespace

QByteArray toYaml(const QJsonObject& root) {
    QByteArray out = "# written by Rikugan; changes are replaced on the next start\n";
    emitObject(out, root, 0);
    return out;
}

}  // namespace rk
