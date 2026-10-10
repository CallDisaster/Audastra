#pragma once

#include "AgentTypes.h"
#include <QByteArray>
#include <optional>

namespace War3Map::Agent {

struct ApplicationPreferences {
    QString gameRoot, uiDataRoot, uiFontFamily, themeId;
};
struct ApplicationSettingsImport {
    ApplicationPreferences application;
    QList<ProviderSettings> providers;
};

// 便携格式只承载显式白名单；不读写QSettings、AgentSettingsStore或凭据。
class ApplicationSettingsTransfer final {
public:
    static constexpr qsizetype MaximumBytes = 2 * 1024 * 1024;
    [[nodiscard]] static QByteArray Encode(const ApplicationPreferences& application, const AgentSettings& settings,
        QString* error = nullptr);
    [[nodiscard]] static std::optional<ApplicationSettingsImport> Decode(const QByteArray& bytes, QString* error = nullptr);
    // local应为可信本机配置；只有完全相同的提供商身份/地址/协议保留本机凭据引用。
    [[nodiscard]] static AgentSettings MergeProviders(const AgentSettings& local, const QList<ProviderSettings>& imported);
};

} // namespace War3Map::Agent
