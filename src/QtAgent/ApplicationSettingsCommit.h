#pragma once

#include <QString>

#include <functional>
#include <optional>

class QSettings;

namespace War3Map::Agent {

class AgentSettingsStore;
struct AgentSettings;
struct ApplicationPreferences;

enum class ApplicationSettingsSyncStage { Preflight, Write, Rollback };
// 测试后端仍须执行实际sync/status检查；生产调用保留默认实现。
using ApplicationSettingsSync = std::function<bool(QSettings&, ApplicationSettingsSyncStage, QString*)>;

// 本机应用服务设置独立于可迁移偏好，导入/导出不包含反馈同意。
struct ApplicationServicePreferences {
    std::optional<bool> feedbackConsent;
    std::optional<QString> feedbackEndpoint;
    std::optional<bool> updateAutomatic;
    std::optional<QString> updateChannel;
    std::optional<QString> updateSkippedVersion;
    std::optional<QString> mapBackupDirectory;
    std::optional<int> mapBackupLimit;
    std::optional<QString> mapTestRuntimeRoot;
    std::optional<bool> mapTestSlk;
    std::optional<bool> recoveryEnabled;
    std::optional<int> recoveryIntervalMinutes;
    std::optional<QString> worldEffectQuality;
};

class ApplicationSettingsCommit final {
public:
    // 提交 AI JSON、应用偏好和显式本机服务设置，不持有/删除凭据。
    // 调用方串行化提交，传入根group的QSettings；取消对话框时不要调用此入口。
    // 应用偏好先sync；原AgentSettingsStore::Save为最后提交点，成功后立即true。
    // false时本入口未持久化新凭据引用；失败仅条件恢复自身偏好写入，错误说明残留。
    // 两种存储之间没有崩溃原子性；比较与写入之间也不是跨进程CAS。
    // QSettings的首次错误是sticky：失败后重试须由宿主重新构造该对象。
    [[nodiscard]] static bool Commit(QSettings& preferences, const AgentSettingsStore& store,
        const AgentSettings& settings, const ApplicationPreferences& application,
        QString* error = nullptr, const ApplicationSettingsSync& synchronize = {},
        const ApplicationServicePreferences& services = {});
};

} // namespace War3Map::Agent
