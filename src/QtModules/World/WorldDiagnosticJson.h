#pragma once

#include "War3MapCore/Types.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <algorithm>
#include <span>
#include <string_view>

namespace War3Map::Modules::WorldDiagnosticJson {
// 只限制诊断表示，不改变业务查询、可用性或GPU完成资格。
inline void Text(QJsonObject& object, const char* key, const QString& value, qsizetype maximum = 2048) {
    const auto name = QString::fromLatin1(key);
    const auto returned = std::clamp<qsizetype>(maximum, 0, value.size());
    object[name] = value.left(returned);
    object[name + "_length"] = QString::number(value.size());
    object[name + "_omitted"] = QString::number(value.size() - returned);
    object[name + "_truncated"] = value.size() > returned;
}
inline void Text(QJsonObject& object, const char* key, std::string_view value, std::size_t maximum = 2048) {
    const auto returned = std::min(value.size(), maximum);
    const auto name = QString::fromLatin1(key);
    object[name] = QString::fromUtf8(value.data(), static_cast<qsizetype>(returned));
    object[name + "_length"] = QString::number(value.size());
    object[name + "_omitted"] = QString::number(value.size() - returned);
    object[name + "_truncated"] = value.size() > returned;
    object[name + "_length_unit"] = "utf8-bytes";
}
inline QJsonObject Page(std::size_t total, std::size_t offset, std::size_t returned, std::size_t limit) {
    const auto begin = std::min(total, offset);
    const auto remaining = total - begin;
    return {{"total", static_cast<double>(total)}, {"offset", static_cast<double>(offset)},
        {"returned", static_cast<double>(returned)}, {"limit", static_cast<double>(limit)},
        {"omitted", static_cast<double>(total - std::min(total, returned))},
        {"remaining", static_cast<double>(remaining - std::min(remaining, returned))},
        {"counts_consistent", returned <= remaining && returned <= limit}, {"truncated", total != returned}};
}
inline QJsonArray Diagnostics(std::span<const Diagnostic> diagnostics, std::size_t limit = 64) {
    QJsonArray rows;
    for (const auto& diagnostic : diagnostics.first(std::min(diagnostics.size(), limit))) {
        QJsonObject row{{"severity", diagnostic.severity == DiagnosticSeverity::Error ? "error"
            : diagnostic.severity == DiagnosticSeverity::Warning ? "warning" : "info"}};
        Text(row, "subsystem", std::string_view(diagnostic.subsystem), 128);
        Text(row, "code", std::string_view(diagnostic.code), 256);
        Text(row, "message", std::string_view(diagnostic.message));
        if (diagnostic.origin) {
            QJsonObject origin{{"offset", QString::number(diagnostic.origin->offset)},
                {"length", QString::number(diagnostic.origin->length)}};
            Text(origin, "source", QString::fromStdWString(diagnostic.origin->source.wstring()));
            row["origin"] = origin;
        }
        rows.append(std::move(row));
    }
    return rows;
}
} // namespace War3Map::Modules::WorldDiagnosticJson
