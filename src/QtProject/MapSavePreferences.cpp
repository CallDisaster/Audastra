#include "MapSavePreferences.h"
#include "../QtCommon/MapBackupPaths.h"

#include <QSettings>
#include <QStandardPaths>

namespace War3Map::QtProject {

Result<InPlaceSaveOptions> ReadMapInPlaceSaveOptions() {
    const QSettings settings;
    return ReadMapInPlaceSaveOptions(settings,
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
}

Result<InPlaceSaveOptions> ReadMapInPlaceSaveOptions(
    const QSettings& settings, const QString& genericDataRoot) {
    using Output = Result<InPlaceSaveOptions>;
    if (!settings.group().isEmpty())
        return Output::Failure("MapSave.Preferences", "MAP_SAVE_PREFERENCES_UNAVAILABLE",
            "无法从设置子组读取地图保存偏好，保存已中止。");
    InPlaceSaveOptions options;
    const auto configured = settings.value(QStringLiteral("mapSave/backupDirectory")).toString();
    bool valid = false;
    const auto limit = settings.value(QStringLiteral("mapSave/backupLimit"), 10).toInt(&valid);
    if (settings.status() != QSettings::NoError)
        return Output::Failure("MapSave.Preferences", "MAP_SAVE_PREFERENCES_UNAVAILABLE",
            "无法读取地图保存偏好，保存已中止。");
    const auto directory = QtCommon::ResolveMapBackupDirectory(configured, genericDataRoot);
    if (!directory.Succeeded()) return {std::nullopt, directory.diagnostics};
    options.backupDirectory = std::filesystem::path(directory.value->toStdWString());
    options.keepBackups = valid && limit >= 0 && limit <= 1000 ? static_cast<std::size_t>(limit) : 10;
    return Output::Success(std::move(options));
}

} // namespace War3Map::QtProject
