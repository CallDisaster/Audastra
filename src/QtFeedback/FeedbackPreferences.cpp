#include "FeedbackClient.h"
#include "FeedbackTransport.h"
#include "AudastraVersion.generated.h"
#include "../QtCommon/LocalConsentPreference.h"

#include <QSettings>

namespace War3Map::QtFeedback {

QString PublishedFeedbackEndpoint() {
    return QString::fromUtf8(BuildInfo::FeedbackEndpoint.data(), static_cast<qsizetype>(BuildInfo::FeedbackEndpoint.size()));
}

FeedbackPreferences FeedbackClient::ReadPreferences(QSettings& settings) {
    FeedbackPreferences preferences;
    // 必须传入根group。偏好只在本机，默认关闭，不读取任何AI/地图数据。
    if (!settings.group().isEmpty()) return preferences;
    preferences.consent = QtCommon::ReadExplicitLocalConsent(settings, QStringLiteral("feedback/consent"));
    const auto endpoint = settings.value(QStringLiteral("feedback/endpointOverride"));
    if (endpoint.metaType().id() == QMetaType::QString) preferences.endpointOverride = endpoint.toString();
    bool converted = false;
    const auto last = settings.value(QStringLiteral("feedback/lastSentUnixSeconds"), 0).toLongLong(&converted);
    if (converted && last >= 0 && last <= 253402300799LL) preferences.lastSentUnixSeconds = last;
    QString error;
    if (!ValidatePreferences(preferences, &error)) {
        preferences.consent = false;
        preferences.endpointOverride.clear();
    }
    return preferences;
}

bool FeedbackClient::ValidatePreferences(const FeedbackPreferences& preferences, QString* error) {
    if (preferences.lastSentUnixSeconds < 0 || preferences.lastSentUnixSeconds > 253402300799LL) {
        if (error) *error = QStringLiteral("最近上报时间无效。");
        return false;
    }
    return Detail::ValidateEndpoint(preferences.endpointOverride, false, error);
}

bool FeedbackClient::WritePreferences(QSettings& settings, const FeedbackPreferences& preferences, QString* error) {
    if (!settings.group().isEmpty() || !ValidatePreferences(preferences, error)) {
        if (error && error->isEmpty()) *error = QStringLiteral("反馈偏好必须写入本机设置根组。");
        return false;
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (error) *error = QStringLiteral("本机设置不可写，未更新反馈同意。");
        return false;
    }
    settings.setValue(QStringLiteral("feedback/consent"), preferences.consent);
    settings.setValue(QStringLiteral("feedback/endpointOverride"), preferences.endpointOverride);
    settings.setValue(QStringLiteral("feedback/lastSentUnixSeconds"), preferences.lastSentUnixSeconds);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (error) *error = QStringLiteral("反馈偏好未完整写入；宿主须按统一提交快照恢复本次写入。");
        return false;
    }
    return true;
}

} // namespace War3Map::QtFeedback
