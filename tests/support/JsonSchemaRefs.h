#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#include <functional>

// 工具输入 schema 里的每个 $ref 都必须能从该 schema 根按 JSON Pointer 解析。
// 严格的模型站点（如 Gemini）遇到解析不到的引用会拒绝整次请求，宽松站点则静默放过。
inline QStringList UnresolvedSchemaRefs(const QJsonObject& root) {
    const auto resolves = [&root](const QString& ref) {
        if (ref == QStringLiteral("#")) return true;
        if (!ref.startsWith(QStringLiteral("#/"))) return false;
        QJsonValue current = root;
        for (auto part : ref.mid(2).split(QLatin1Char('/'))) {
            part.replace(QStringLiteral("~1"), QStringLiteral("/")).replace(QStringLiteral("~0"), QStringLiteral("~"));
            if (current.isObject()) {
                const auto object = current.toObject();
                if (!object.contains(part)) return false;
                current = object.value(part);
            } else if (current.isArray()) {
                bool ok = false;
                const auto index = part.toInt(&ok);
                const auto array = current.toArray();
                if (!ok || index < 0 || index >= array.size()) return false;
                current = array.at(index);
            } else {
                return false;
            }
        }
        return current.isObject() || current.isBool();
    };
    QStringList missing;
    std::function<void(const QJsonValue&, const QString&)> walk = [&](const QJsonValue& value, const QString& path) {
        if (value.isArray()) {
            const auto array = value.toArray();
            for (qsizetype i = 0; i < array.size(); ++i) walk(array.at(i), path + QLatin1Char('/') + QString::number(i));
            return;
        }
        if (!value.isObject()) return;
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (it.key() == QStringLiteral("$ref")) {
                const auto ref = it.value().toString();
                if (!it.value().isString() || !resolves(ref)) missing.append(path + QStringLiteral(" -> ") + ref);
            } else {
                walk(it.value(), path + QLatin1Char('/') + it.key());
            }
        }
    };
    walk(root, QStringLiteral("#"));
    return missing;
}
