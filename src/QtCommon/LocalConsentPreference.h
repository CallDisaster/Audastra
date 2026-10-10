#pragma once

#include <QSettings>

namespace War3Map::QtCommon {

// 同意必须来自明确的本机勾选值；未知类型或非空字符串不能推断成同意。
inline bool ReadExplicitLocalConsent(QSettings& settings, const QString& key) {
    if (!settings.group().isEmpty()) return false;
    const auto value = settings.value(key);
    if (value.metaType().id() == QMetaType::Bool) return value.toBool();
    return value.metaType().id() == QMetaType::QString && value.toString() == QStringLiteral("true");
}

} // namespace War3Map::QtCommon
