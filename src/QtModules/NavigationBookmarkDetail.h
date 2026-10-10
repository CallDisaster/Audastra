#pragma once

#include <QJsonObject>
#include <QString>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace War3Map::Modules::NavigationBookmarkDetail {

inline bool Header(const QJsonObject& value, const char* kind) {
    return value.value("schema").isDouble() && value.value("schema").toDouble() == 1
        && value.value("kind").isString() && value.value("kind").toString() == QLatin1String(kind);
}
inline std::optional<QString> Text(const QJsonObject& value, const char* key, qsizetype maximum = 2048) {
    const auto raw = value.value(QLatin1String(key));
    if (!raw.isString() || raw.toString().contains(QChar::Null)) return {};
    const auto bytes = raw.toString().toUtf8();
    if (bytes.size() > maximum || QString::fromUtf8(bytes) != raw.toString()) return {};
    return raw.toString();
}
inline std::optional<int> Scroll(const QJsonObject& value, const char* key) {
    const auto raw = value.value(QLatin1String(key));
    if (!raw.isDouble()) return {};
    const double number = raw.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number || number < 0
        || number > static_cast<double>((std::numeric_limits<int>::max)())) return {};
    return static_cast<int>(number);
}
inline std::optional<std::uint64_t> Unsigned(const QJsonObject& value, const char* key) {
    const auto text = Text(value, key, 20);
    if (!text || text->isEmpty()) return {};
    bool valid = false;
    const auto number = text->toULongLong(&valid, 10);
    if (!valid || QString::number(number) != *text) return {};
    return static_cast<std::uint64_t>(number);
}

} // namespace War3Map::Modules::NavigationBookmarkDetail
