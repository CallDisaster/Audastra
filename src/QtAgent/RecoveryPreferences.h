#pragma once

#include <QMetaType>
#include <QSettings>
#include <QString>
#include <QVariant>

namespace War3Map::Agent {

inline constexpr char kRecoveryEnabledSettingsKey[] = "project/autoRecoveryEnabled";
inline constexpr char kRecoveryIntervalSettingsKey[] = "project/autoRecoveryIntervalMinutes";
inline constexpr int kMinimumRecoveryIntervalMinutes = 1;
inline constexpr int kMaximumRecoveryIntervalMinutes = 120;

struct RecoveryPreferences {
    bool enabled = true;
    int intervalMinutes = 5;
};

[[nodiscard]] inline bool IsRecoveryIntervalValid(int value) {
    return value >= kMinimumRecoveryIntervalMinutes && value <= kMaximumRecoveryIntervalMinutes;
}

// 页面和主窗口使用相同的只读合同；不修正或回写来源中的非法值。
[[nodiscard]] inline RecoveryPreferences LoadRecoveryPreferences(const QSettings& settings) {
    RecoveryPreferences result;
    const auto enabled = settings.value(QString::fromLatin1(kRecoveryEnabledSettingsKey));
    const auto enabledType = enabled.metaType().id();
    if (enabledType == QMetaType::Bool) result.enabled = enabled.toBool();
    else if (enabledType == QMetaType::QString) {
        const auto text = enabled.toString().trimmed();
        if (text.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0 || text == QStringLiteral("0")) result.enabled = false;
        else if (text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 || text == QStringLiteral("1")) result.enabled = true;
    } else if (enabledType == QMetaType::Int || enabledType == QMetaType::UInt
        || enabledType == QMetaType::LongLong || enabledType == QMetaType::ULongLong) {
        bool valid = false;
        const auto value = enabled.toLongLong(&valid);
        if (valid && (value == 0 || value == 1)) result.enabled = value == 1;
    }
    const auto interval = settings.value(QString::fromLatin1(kRecoveryIntervalSettingsKey));
    const auto intervalType = interval.metaType().id();
    if (intervalType == QMetaType::QString || intervalType == QMetaType::Int || intervalType == QMetaType::UInt
        || intervalType == QMetaType::LongLong || intervalType == QMetaType::ULongLong) {
        bool valid = false;
        const auto value = interval.toLongLong(&valid);
        if (valid && value >= kMinimumRecoveryIntervalMinutes && value <= kMaximumRecoveryIntervalMinutes)
            result.intervalMinutes = static_cast<int>(value);
    }
    return result;
}

} // namespace War3Map::Agent
