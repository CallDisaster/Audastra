#pragma once

#include "War3MapCore/Types.h"
#include "War3MapCore/MapArchiveSession.h"

#include <QDir>
#include <QString>
#include <QStandardPaths>
#include <QSettings>

namespace War3Map::QtCommon {

// 只解析，不创建目录。使用 generic 用户数据根，避免 Qt 追加组织名。
// 显式注入的根允许测试完全留在自己的临时目录。
[[nodiscard]] inline Result<QString> ResolveMapBackupDirectory(
    const QString& configuredDirectory, const QString& genericDataRoot) {
    using Output = Result<QString>;
    if (configuredDirectory.size() > 4096)
        return Output::Failure("MapSave.Preferences", "MAP_BACKUP_DIRECTORY_INVALID",
            "备份目录不能超过 4096 个字符。");
    const auto validate = [](const QString& raw, const char* code, const char* label) {
        using Validation = Result<QString>;
        if (raw.size() > 4096)
            return Validation::Failure("MapSave.Preferences", code,
                std::string(label) + "不能超过 4096 个字符。");
        for (const auto character : raw) {
            if (character.unicode() < 0x20)
                return Validation::Failure("MapSave.Preferences", code,
                    std::string(label) + "不能包含控制字符。");
        }
        const auto directory = QDir::fromNativeSeparators(raw.trimmed());
        if (directory.isEmpty() || !QDir::isAbsolutePath(directory))
            return Validation::Failure("MapSave.Preferences", code,
                std::string(label) + "不可用：需要明确的绝对目录。");
        return Validation::Success(QDir::cleanPath(directory));
    };
    if (!configuredDirectory.trimmed().isEmpty())
        return validate(configuredDirectory, "MAP_BACKUP_DIRECTORY_INVALID", "备份目录");
    // 全空格可表示留空；其余控制字符不能被 trimmed 静默吞掉。
    for (const auto character : configuredDirectory) {
        if (character.unicode() < 0x20)
            return Output::Failure("MapSave.Preferences", "MAP_BACKUP_DIRECTORY_INVALID",
                "备份目录不能包含控制字符。");
    }
    const auto root = validate(genericDataRoot, "MAP_BACKUP_DEFAULT_UNAVAILABLE", "本地用户数据目录");
    if (!root.Succeeded()) return root;
    return validate(QDir(*root.value).filePath(QStringLiteral("Audastra/backups")),
        "MAP_BACKUP_DEFAULT_UNAVAILABLE", "默认备份目录");
}

[[nodiscard]] inline Result<QString> DefaultMapBackupDirectory(const QString& genericDataRoot) {
    return ResolveMapBackupDirectory({}, genericDataRoot);
}

[[nodiscard]] inline Result<QString> DefaultMapBackupDirectory() {
    return DefaultMapBackupDirectory(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
}

// GUI 地图、战役与独立 MCP 共同读取同一份备份偏好；这里只解析，不创建目录。
[[nodiscard]] inline Result<InPlaceSaveOptions> ReadMapInPlaceSaveOptions(
    const QSettings& settings, const QString& genericDataRoot) {
    using Output = Result<InPlaceSaveOptions>;
    if (!settings.group().isEmpty()) return Output::Failure("MapSave.Preferences", "MAP_SAVE_PREFERENCES_UNAVAILABLE",
        "无法从设置子组读取地图保存偏好，保存已中止。");
    const auto configured = settings.value(QStringLiteral("mapSave/backupDirectory")).toString();
    bool valid = false;
    const auto limit = settings.value(QStringLiteral("mapSave/backupLimit"), 10).toInt(&valid);
    if (settings.status() != QSettings::NoError) return Output::Failure("MapSave.Preferences", "MAP_SAVE_PREFERENCES_UNAVAILABLE",
        "无法读取地图保存偏好，保存已中止。");
    const auto directory = ResolveMapBackupDirectory(configured, genericDataRoot);
    if (!directory.Succeeded()) return {std::nullopt, directory.diagnostics};
    InPlaceSaveOptions options;
    options.backupDirectory = std::filesystem::path(directory.value->toStdWString());
    options.keepBackups = valid && limit >= 0 && limit <= 1000 ? static_cast<std::size_t>(limit) : 10;
    return Output::Success(std::move(options));
}

} // namespace War3Map::QtCommon
