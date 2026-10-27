#include "ApplicationSettingsCommit.h"
#include "RecoveryPreferences.h"
#include "AgentSettingsStore.h"
#include "ApplicationSettingsTransfer.h"
#include "War3MapCore/Update/SemanticVersion.h"
#include "../QtCommon/MapBackupPaths.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>
#include <QVariant>
#include <QUrl>

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace War3Map::Agent {
namespace {

constexpr qint64 kMaximumJsonBytes = 2 * 1024 * 1024;
struct JsonSnapshot { bool exists = false; QByteArray bytes; };
struct KeySnapshot { bool exists = false; QVariant value; };
struct KeyChange {
    QString key;
    KeySnapshot before;
    QVariant written;
    bool changed = false, issued = false;
};

bool Fail(QString* error, const QString& text) {
    if (error) *error = text;
    return false;
}

bool ReadJson(const QString& path, JsonSnapshot& result, QString* error) {
    const QFileInfo info(path);
    if (info.isSymLink() || (info.exists() && !info.isFile()))
        return Fail(error, QStringLiteral("AI设置来源不是可恢复的普通文件。"));
    result = {};
    if (!info.exists()) return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaximumJsonBytes)
        return Fail(error, QStringLiteral("无法捕获AI设置，或文件超过2MiB。"));
    result.exists = true;
    result.bytes = file.read(kMaximumJsonBytes + 1);
    return file.error() == QFileDevice::NoError && result.bytes.size() <= kMaximumJsonBytes
        ? true : Fail(error, QStringLiteral("AI设置快照读取失败。"));
}

bool Same(const JsonSnapshot& a, const JsonSnapshot& b) {
    return a.exists == b.exists && (!a.exists || a.bytes == b.bytes);
}
KeySnapshot ReadKey(QSettings& settings, const QString& key) {
    return {settings.contains(key), settings.value(key)};
}
bool Same(const KeySnapshot& a, const KeySnapshot& b) {
    return a.exists == b.exists && (!a.exists || (a.value.metaType() == b.value.metaType() && a.value == b.value));
}
bool HasChildren(QSettings& settings, const QString& key) {
    settings.beginGroup(key);
    const bool result = !settings.childKeys().isEmpty() || !settings.childGroups().isEmpty();
    settings.endGroup();
    return result;
}

class PrimaryStorage final {
public:
    explicit PrimaryStorage(QSettings& settings) : settings_(settings), fallback_(settings.fallbacksEnabled()) {
        // 回滚须保留主存储中“键不存在”，不能把fallback值写回主存储。
        settings_.setFallbacksEnabled(false);
    }
    ~PrimaryStorage() { settings_.setFallbacksEnabled(fallback_); }
private:
    QSettings& settings_;
    bool fallback_;
};

std::unique_ptr<QSettings> Fresh(QSettings& settings) {
    // NativeFormat在Windows的fileName是完整注册表位置；此构造同样支持INI/custom format。
    auto result = std::make_unique<QSettings>(settings.fileName(), settings.format());
    result->setFallbacksEnabled(false);
    result->setAtomicSyncRequired(settings.isAtomicSyncRequired());
    return result;
}
bool Sync(QSettings& settings, ApplicationSettingsSyncStage stage,
    const ApplicationSettingsSync& synchronize, QString* error) {
    if (synchronize) return synchronize(settings, stage, error);
    settings.sync();
    return settings.status() == QSettings::NoError
        ? true : Fail(error, QStringLiteral("应用设置同步失败（QSettings状态%1）。").arg(int(settings.status())));
}

QStringList Restore(QSettings& preferences, std::vector<KeyChange>& keys, const ApplicationSettingsSync& synchronize) {
    QStringList remaining;
    // 不使用clear/remove group，也不回写未改键。当前值已不同则保留并发新值。
    for (auto& key : keys) {
        if (!key.issued) continue;
        const auto current = ReadKey(preferences, key.key);
        if (Same(current, key.before)) continue;
        if (!Same(current, {true, key.written})) {
            remaining.push_back(key.key + QStringLiteral("：检测到并发更改，未覆盖"));
            continue;
        }
        if (key.before.exists) preferences.setValue(key.key, key.before.value);
        else if (!HasChildren(preferences, key.key)) preferences.remove(key.key);
        else remaining.push_back(key.key + QStringLiteral("：出现并发子键，未删除该键"));
    }
    const bool keysIssued = std::any_of(keys.begin(), keys.end(), [](const auto& key) { return key.issued; });
    if (keysIssued) {
        // 原对象的status不能清零，恢复持久化由相同位置的新对象确认。
        auto verification = Fresh(preferences);
        QString diagnostic;
        if (!Sync(*verification, ApplicationSettingsSyncStage::Rollback, synchronize, &diagnostic))
            remaining.push_back(QStringLiteral("应用偏好恢复未确认持久化：") + diagnostic);
        else for (const auto& key : keys) {
            if (key.issued && !Same(ReadKey(*verification, key.key), key.before)
                && !remaining.contains(key.key + QStringLiteral("：检测到并发更改，未覆盖")))
                remaining.push_back(key.key + QStringLiteral("：原存在状态/值未恢复"));
        }
    }
    return remaining;
}

} // namespace

bool ApplicationSettingsCommit::Commit(QSettings& preferences, const AgentSettingsStore& store,
    const AgentSettings& settings, const ApplicationPreferences& application, QString* error,
    const ApplicationSettingsSync& synchronize, const ApplicationServicePreferences& services,
    const MapBackupMigrationDispatch& migration) {
    if (error) error->clear();
    QString diagnostic;
    // 与对话框一致：普通AI配置不套迁移提供商URL限制，偏好仅复用便携白名单校验。
    if (!AgentSettingsStore::Validate(settings, &diagnostic)
        || ApplicationSettingsTransfer::Encode(application, AgentSettings{}, &diagnostic).isEmpty())
        return Fail(error, diagnostic);
    if (services.feedbackEndpoint && !services.feedbackEndpoint->isEmpty()) {
        const auto& endpoint = *services.feedbackEndpoint;
        const QUrl url(endpoint, QUrl::StrictMode);
        if (endpoint.size() > 2048 || !url.isValid() || url.scheme() != QStringLiteral("https")
            || url.host().isEmpty() || url.authority(QUrl::FullyEncoded).contains(QChar('@')) || url.port() == 0
            || url.hasQuery() || url.hasFragment()
            || std::any_of(endpoint.begin(), endpoint.end(), [](QChar c) { return c.isSpace() || c.unicode() < 32; }))
            return Fail(error, QStringLiteral("反馈端点必须是无凭据、查询参数或片段的 HTTPS 地址。"));
    }
    if (services.updateChannel && *services.updateChannel != QStringLiteral("stable") && *services.updateChannel != QStringLiteral("beta"))
        return Fail(error, QStringLiteral("更新渠道必须是稳定版或测试版。"));
    if (services.updateSkippedVersion && !services.updateSkippedVersion->isEmpty()
        && !Update::ParseSemanticVersion(services.updateSkippedVersion->toStdString()).Succeeded())
        return Fail(error, QStringLiteral("跳过版本必须是有效的语义版本号。"));
    if (!preferences.group().isEmpty()) return Fail(error, QStringLiteral("应用设置提交要求根group的QSettings。"));
    for (const auto* path : {&services.mapBackupDirectory, &services.mapTestRuntimeRoot}) {
        if (!*path || (*path)->isEmpty()) continue;
        if ((*path)->size() > 4096 || !QDir::isAbsolutePath(QDir::fromNativeSeparators(**path))
            || std::any_of((*path)->begin(), (*path)->end(), [](QChar c) { return c.unicode() < 32; }))
            return Fail(error, QStringLiteral("地图测试和备份目录必须是有效的绝对路径，最多4096个字符。"));
    }
    if (services.mapBackupLimit && (*services.mapBackupLimit < 0 || *services.mapBackupLimit > 1000))
        return Fail(error, QStringLiteral("备份保留数量须为0到1000，0表示不限数量。"));
    if (services.recoveryIntervalMinutes && !IsRecoveryIntervalValid(*services.recoveryIntervalMinutes))
        return Fail(error, QStringLiteral("自动恢复间隔须为1到120分钟。"));
    if (services.worldEffectQuality && *services.worldEffectQuality != QStringLiteral("complete")
        && *services.worldEffectQuality != QStringLiteral("balanced") && *services.worldEffectQuality != QStringLiteral("power-saving"))
        return Fail(error, QStringLiteral("特效显示须为完整、平衡或省电。"));
    PrimaryStorage primary(preferences);
    if (!Sync(preferences, ApplicationSettingsSyncStage::Preflight, synchronize, &diagnostic)
        || !preferences.isWritable() || !preferences.isAtomicSyncRequired())
        return Fail(error, diagnostic.isEmpty() ? QStringLiteral("应用设置不可写，或已关闭原子同步；未开始提交。") : diagnostic);

    std::optional<MapBackupMigrationRequest> moveBackups;
    if (services.mapBackupDirectory) {
        const auto dataRoot = migration.genericDataRoot.value_or(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
        const auto from = QtCommon::ResolveMapBackupDirectory(preferences.value(QStringLiteral("mapSave/backupDirectory")).toString(), dataRoot);
        const auto to = QtCommon::ResolveMapBackupDirectory(*services.mapBackupDirectory, dataRoot);
        if (!to.Succeeded()) return Fail(error, QString::fromStdString(to.diagnostics.front().message));
        if (!from.Succeeded() || *from.value != *to.value) {
            bool validLimit = false;
            const auto previousLimit = preferences.value(QStringLiteral("mapSave/backupLimit"), 10).toInt(&validLimit);
            const auto keep = services.mapBackupLimit.value_or(validLimit && previousLimit >= 0 && previousLimit <= 1000 ? previousLimit : 10);
            moveBackups = MapBackupMigrationRequest{from.value.value_or(QString{}), *to.value, static_cast<std::size_t>(keep), {}};
            if (!from.Succeeded()) moveBackups->unavailableReason = QString::fromStdString(from.diagnostics.front().message);
        }
    }

    std::vector<KeyChange> keys{
        {QStringLiteral("war3/gameRoot"), {}, application.gameRoot},
        {QStringLiteral("ui/dataRoot"), {}, application.uiDataRoot},
        {QStringLiteral("ui/fontFamily"), {}, application.uiFontFamily},
        {QStringLiteral("ui/theme"), {}, application.themeId}};
    if (services.feedbackConsent) keys.push_back({QStringLiteral("feedback/consent"), {}, *services.feedbackConsent});
    if (services.feedbackEndpoint) keys.push_back({QStringLiteral("feedback/endpointOverride"), {}, *services.feedbackEndpoint});
    if (services.updateAutomatic) keys.push_back({QStringLiteral("update/automaticEnabled"), {}, *services.updateAutomatic});
    if (services.updateChannel) keys.push_back({QStringLiteral("update/channel"), {}, *services.updateChannel});
    if (services.updateSkippedVersion) keys.push_back({QStringLiteral("update/skippedVersion"), {}, *services.updateSkippedVersion});
    if (services.mapBackupDirectory) keys.push_back({QStringLiteral("mapSave/backupDirectory"), {}, *services.mapBackupDirectory});
    if (services.mapBackupLimit) keys.push_back({QStringLiteral("mapSave/backupLimit"), {}, *services.mapBackupLimit});
    if (services.mapTestRuntimeRoot) keys.push_back({QStringLiteral("mapTest/runtimeRoot"), {}, *services.mapTestRuntimeRoot});
    if (services.mapTestSlk) keys.push_back({QStringLiteral("mapTest/slkEnabled"), {}, *services.mapTestSlk});
    if (services.recoveryEnabled) keys.push_back({QString::fromLatin1(kRecoveryEnabledSettingsKey), {}, *services.recoveryEnabled});
    if (services.recoveryIntervalMinutes) keys.push_back({QString::fromLatin1(kRecoveryIntervalSettingsKey), {}, *services.recoveryIntervalMinutes});
    if (services.worldEffectQuality) keys.push_back({QStringLiteral("world/effectQuality"), {}, *services.worldEffectQuality});
    for (auto& key : keys) {
        key.before = ReadKey(preferences, key.key);
        key.changed = key.before.exists ? !Same(key.before, {true, key.written}) : !key.written.toString().isEmpty();
        // remove(key)会删除子树，已有这类非标准结构时不要开始事务。
        if (key.changed && HasChildren(preferences, key.key))
            return Fail(error, QStringLiteral("应用偏好键包含子项，不能安全回滚：") + key.key);
    }
    const auto jsonPath = store.FilePath();
    JsonSnapshot beforeJson;
    if (!ReadJson(jsonPath, beforeJson, &diagnostic)) return Fail(error, diagnostic);

    const auto failed = [&](const QString& reason) {
        const auto remaining = Restore(preferences, keys, synchronize);
        return Fail(error, (reason.isEmpty() ? QStringLiteral("设置提交失败。") : reason) + (remaining.isEmpty() ? QStringLiteral(" 本次应用偏好更改已恢复。")
            : QStringLiteral(" 残留或未核验状态：") + remaining.join(QStringLiteral("；"))));
    };
    // 原JSON快照读取期间可能有另一个提交；写任何偏好之前再次导入并核对主存储。
    if (!Sync(preferences, ApplicationSettingsSyncStage::Preflight, synchronize, &diagnostic)) return failed(diagnostic);
    for (const auto& key : keys) if (!Same(ReadKey(preferences, key.key), key.before))
        return failed(QStringLiteral("应用偏好在准备后变化，未覆盖：") + key.key);
    for (auto& key : keys) if (key.changed) {
        key.issued = true;
        preferences.setValue(key.key, key.written);
    }
    if (std::any_of(keys.begin(), keys.end(), [](const auto& key) { return key.issued; })
        && !Sync(preferences, ApplicationSettingsSyncStage::Write, synchronize, &diagnostic)) return failed(diagnostic);
    JsonSnapshot currentJson;
    if (!ReadJson(jsonPath, currentJson, &diagnostic) || !Same(currentJson, beforeJson))
        return failed(diagnostic.isEmpty() ? QStringLiteral("AI设置在提交前发生并发更改，未覆盖该文件。") : diagnostic);

    // 原Save禁用direct-write fallback：失败不发布新JSON，Dialog可以安全清理新凭据。
    // 这是唯一最终提交点。成功之后不能再因其他存储的并发变化返回false、删除新ref。
    if (!store.Save(settings, &diagnostic)) return failed(diagnostic);
    // 设置最终提交以后才发起迁移；后台失败不能伪装成设置事务失败并清理新凭据。
    if (moveBackups) {
        try {
            if (migration.submit) migration.submit(std::move(*moveBackups));
            else if (!MapBackupMigrationService::Post(std::move(*moveBackups)))
                qWarning("Map backup migration not scheduled: application event loop unavailable");
        } catch (...) { qWarning("Map backup migration not scheduled after settings commit"); }
    }
    return true;
}

} // namespace War3Map::Agent
